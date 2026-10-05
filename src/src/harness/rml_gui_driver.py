"""明示起動したRmlUi GUIの実コンテキストを操作する開発用ドライバー。"""
import json
import pathlib
import threading
import time


class Gui:
    def __init__(self, directory):
        self.directory = pathlib.Path(directory)
        self.sequence = int(time.time() * 1000)

    def call(self, action='inspect', **values):
        self.sequence += 1
        request = dict(action=action, sequence=self.sequence, **values)
        pending = self.directory/'request.tmp'
        pending.write_text(json.dumps(request, ensure_ascii=False), encoding='utf-8')
        pending.replace(self.directory/'request.json')
        end = time.monotonic() + 15
        while time.monotonic() < end:
            try:
                response = json.loads((self.directory/'response.json').read_text(encoding='utf-8'))
                if response.get('sequence') == self.sequence:
                    if not response['ok']:
                        raise RuntimeError(response.get('error', 'UI operation failed'))
                    self.commands = response.get('commands', [])
                    return response['result']
            except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                pass
            time.sleep(.05)
        raise TimeoutError(f'GUI response: {request}')

    def click(self, element):
        return self.call('click', id=element)

    def settings(self):
        self.click('nav-matching')
        self.click('matching-tab')
        return self.click('profile-settings')

    def value(self, element, value):
        return self.call('value', id=element, value=str(value))

    def type(self, element, value):
        return self.call('type', id=element, value=value)

    def wait(self, predicate, seconds=35):
        end = time.monotonic()+seconds
        while time.monotonic() < end:
            value = self.call()
            if predicate(value):
                return value
            time.sleep(.15)
        raise TimeoutError('GUI condition: '+json.dumps(value.get('state', {}), ensure_ascii=False)[:3000])

    def checked(self, element, enabled):
        if self.call()['elements'][element]['checked'] != enabled:
            self.click(element)

    def select_player(self, code):
        self.click('matching-tab')
        data = self.wait(lambda d: any(p['code'] == code for p in d['state']['matching']['people']))
        index = next(i for i, p in enumerate(data['state']['matching']['people']) if p['code'] == code)
        page = int(data['elements']['page-count']['text'].split('/')[0]) - 1
        while page != index // 20:
            self.click('next' if page < index // 20 else 'prev')
            page += 1 if page < index // 20 else -1
        self.click('player-'+str(index))


def check_emblems(guis, targets, report):
    import base64
    import struct
    presets = pathlib.Path(__file__).resolve().parents[1]/'gui_launcher/assets/emblems'

    def expected(data):
        assert len(data) == 918 and data[:2] == b'BM'
        assert struct.unpack_from('<iiHHI', data, 18) == (24, 12, 1, 24, 0)
        return b''.join(data[54+(y*24+x)*3:54+(y*24+x)*3+3]+b'\xff' for y in reversed(range(12)) for x in range(24))

    a, b, _ = guis
    for gui in [a, b]:
        gui.wait(lambda d: (d.get('state') or {}).get('protocol') == 1)
        gui.settings();gui.click('emblem-preset-toggle')
    buttons = [key for key in a.call()['elements'] if key.startswith('emblem-preset-') and key != 'emblem-preset-toggle' and key != 'emblem-preset-details']
    assert len(buttons) == 24
    for button in buttons:
        code = button.rsplit('-', 1)[1]
        a.click(button)
        data = a.wait(lambda d: not d['state']['error'] and bool(d['state']['profile']['emblemId']))
        bmp = (presets/(code+'.bmp')).read_bytes()
        assert base64.b64decode(data['state']['profile']['pixels']) == expected(bmp), code
        assert (targets[0]/'cccaster_B/player-emblem.bmp').read_bytes() == bmp, code
    report['checks'].append('国旗24種類の選択・288画素一致・24bit BMP自動保存')
    for scale in [1., 1.5]:
        a.call('dpi', value=scale)
        data = a.call();rect=data['elements']['emblem']['rect'];content=data['elements']['content']
        assert abs((rect[2]-2*scale)/(rect[3]-2*scale)-2) < .05
        assert content['scrollWidth'] <= content['clientWidth']+1
    a.call('dpi', value=1.)
    report['checks'].append('国旗一覧展開時の100%／150%レイアウトと2:1プレビュー')
    a.click('emblem-remove')
    a.wait(lambda d: d['state']['profile']['emblemId'] == 0)
    assert not (targets[0]/'cccaster_B/player-emblem.bmp').exists()
    report['checks'].append('エンブレム削除とBMP消去')
    for gui, code in [(a, 'jp'), (b, 'fr')]:
        gui.click('emblem-preset-'+code)
        state = gui.call()['state']['profile']
        report.setdefault('emblems', []).append({'code':code,'id':state['emblemId']})
        gui.call('renderer', software=True)
        assert gui.call()['state']['profile'] == state
        gui.click('settings-back')
        check_matching_profile(gui, report)
    report['checks'].append('描画再構築後も選択画像を維持')


def check_matching_profile(gui, report):
    gui.settings()
    assert gui.call()['elements']['settings-back']['visible']
    gui.click('settings-back')
    data = gui.call();elements = data['elements']
    assert data['page'] == 'matching' and elements['matching-panel']['visible']
    image_id = data['state']['profile']['emblemId']
    assert elements['matching-emblem']['visible'] == bool(image_id)
    assert elements['matching-emblem-placeholder']['visible'] == (not image_id)
    assert f'src="emblem:{image_id}"' in elements['matching-profile-image']['text']
    if image_id:
        image = elements['matching-emblem']['rect'];name = elements['matching-name']['rect']
        assert image[2] > 0 and abs(image[2]/image[3]-2) < .01
        assert image[0]+image[2] < name[0]
    report['checks'].append('設定からマッチングへ戻る・名前の左に登録画像／未登録時の代替表示')


def check_public_layout(gui, report):
    gui.click('nav-matching');gui.click('matching-tab')
    for scale in [1.0, 1.5]:
        gui.call('dpi', value=scale)
        data = gui.call();elements = data['elements']
        cells = [elements['player-'+str(i)]['rect'] for i in range(20)]
        panel = elements['people-window']['rect']
        rows = {}
        for cell in cells:
            rows.setdefault(round(cell[1], 1), []).append(cell)
        assert len(cells) == 20 and all(r[2] >= 196*scale-1 for r in cells)
        for row in rows.values():
            assert all(left[0]+left[2] <= right[0]+1 for left, right in zip(row, row[1:]))
        assert all(r[2] > 0 and abs(r[3]-32*scale) <= 1 and r[0] >= panel[0] and r[1] >= panel[1] and
                   r[0]+r[2] <= panel[0]+panel[2]+1 and r[1]+r[3] <= panel[1]+panel[3]+1 for r in cells)
        name = elements['matching-name']['rect'];button = elements['profile-settings']['rect']
        assert name[0]+name[2] <= button[0]+1 and button[1] < name[1]+name[3]
        assert 'nav-settings' not in elements
        for i in range(20):
            name = elements[f'player-name-{i}']['rect'];age = elements[f'player-age-{i}']['rect']
            assert name[2] > 0 and age[0] >= name[0]+name[2]-1
            assert age[0]+age[2] <= cells[i][0]+cells[i][2]+1
        content = elements['content']
        assert content['scrollWidth'] <= content['clientWidth']+1
        directory = elements['public-directory']['rect'];own = elements['registration-card']['rect']
        if scale == 1.0:
            assert directory[0]+directory[2] <= own[0]+1 and abs(directory[1]-own[1]) <= 1
        report.setdefault('layouts', []).append({'scale':scale,'directory':directory,'own':own,'cells':cells})
        report['checks'].append(f'DPI {scale} 名前10文字分の幅・20名の折り返し・経過時間・名前右の設定ボタン・横幅')
        gui.click('profile-settings')
        assert gui.call()['page'] == 'settings'
        gui.click('settings-back')
        assert gui.call()['page'] == 'matching'
    gui.call('dpi', value=1.0)
    report['checks'].append('左の一覧・右の待機欄とプロフィールからの設定画面遷移')


def check_public_listing_times(gui, report):
    base = gui.call()['state']
    fixture = json.loads(json.dumps(base))
    people = fixture['matching']['people'];now = int(time.time())
    for i, person in enumerate(people):
        person['listedAt'] = now - 7200 - 60*i
    people[0]['listedAt'] = now-5105
    people[0]['name'] = 'LONG_PLAYER_NAME_123456789012345'
    people[1]['listedAt'] = now-65
    people[2]['listedAt'] = now+60
    people[-1].pop('listedAt', None)
    selected = people[0]
    try:
        gui.call('fixture', state=fixture)
        data = gui.call()
        expected = sorted(people, key=lambda p: (-p.get('listedAt', 0), p['id']))
        assert [p['id'] for p in data['state']['matching']['people']] == [p['id'] for p in expected]
        assert [data['elements'][f'player-age-{i}']['text'] for i in range(3)] == ['0:00', '0:01', '1:25']
        gui.click('player-name-2')
        assert gui.call()['elements']['selected-player']['text'] == selected['name']
        selected['listedAt'] = now+120
        gui.call('fixture', state=fixture)
        assert gui.call()['state']['matching']['people'][0]['id'] == selected['id']
        gui.click('invite-selected')
        assert gui.commands[-1] == {'type':'matching_invite', 'code':selected['code']}
        gui.click('next')
        assert gui.call()['elements'][f'player-age-{len(people)-1}']['text'] == '--:--'
        gui.click('prev')
        # 新しい受信データがなくても、募集時刻から1分に達した表示だけが進む。
        for person in people:
            person['listedAt'] = int(time.time())-3600
        selected['listedAt'] = int(time.time())-58
        gui.call('fixture', state=fixture)
        assert gui.call()['elements']['player-age-0']['text'] == '0:00'
        gui.wait(lambda d: d['elements']['player-age-0']['text'] == '0:01', seconds=5)
        report['checks'].append('新着順・再掲載順・時分表示・不明と未来時刻・経過時間の自動更新・選択相手の維持')
    finally:
        gui.call('fixture', state={})


def before_game(guis, ports, report):
    a, b, watch = guis
    def check(name, condition=True):
        assert condition, name
        report['checks'].append(name)

    for gui in guis:
        initial = gui.wait(lambda d: (d.get('state') or {}).get('protocol') == 1)
        if initial['state']['language'] != 'ja':
            gui.click('language')
            gui.wait(lambda d: d['state']['language'] == 'ja')
    check('WARPのCPU描画', b.call()['state']['display']['software'])
    # 旧応答を開いて置換を一時的に拒否し、操作の応答を取りこぼさないことを確認する。
    with (a.directory/'response.json').open('r', encoding='utf-8') as held:
        release = threading.Timer(.4, held.close)
        release.start()
        try:
            a.call()
        finally:
            release.join()
    check('試験応答ファイルの一時ロックから復帰')
    check('初期表示は公開一覧と待機開始', a.call()['elements']['public-directory']['visible'] and '公開マッチング待機開始' in a.call()['elements']['start-matching']['text'])
    saved_visibility = a.call()['state']['settings']['public']
    a.click('direct-tab')
    check('公開と直接接続の2タブ・コード欄は1つ', a.call()['elements']['direct-code']['visible'] and 'private-tab' not in a.call()['elements'] and 'match-code' not in a.call()['elements'])
    a.click('matching-tab')
    check('タブ閲覧だけでは公開設定を変更しない', a.call()['state']['settings']['public'] == saved_visibility)
    a.click('language')
    a.wait(lambda d: d['state']['language'] == 'en')
    check('英語切替', 'Public matching' in a.call()['elements']['matching-tab']['text'] and 'Start public standby' in a.call()['elements']['start-matching']['text'])
    a.click('language');a.wait(lambda d: d['state']['language'] == 'ja')
    a.click('direct-tab');a.value('direct-port', 0);a.click('start-direct')
    a.wait(lambda d: bool(d['state']['error']) and not d['state']['session']['running'])
    check('不正ポートではゲームを起動しない')
    a.value('direct-port', ports[2]);a.value('direct-code', '!invalid!');a.click('start-direct')
    a.wait(lambda d: bool(d['state']['error']) and not d['state']['session']['running'])
    check('不正な接続コードを待受と誤認しない')
    a.value('direct-code', '')
    a.value('direct-port', ports[2]);a.click('start-direct')
    a.wait(lambda d: len(d['state']['session']['code']) == 6)
    a.click('cancel-session');a.wait(lambda d: not d['state']['session']['running'])
    check('直接募集の取消', a.call()['state']['session']['status'] == '接続をキャンセルしました。')
    a.click('matching-tab')
    for gui in guis:
        gui.settings();gui.value('connection-preference', 0)
        gui.checked('allow-spectators', True);gui.click('nav-matching')
    for scale in [1.0, 1.5]:
        a.call('dpi', value=scale)
        for page in ['matching', 'settings', 'guide', 'spectate', 'training', 'replay']:
            a.settings() if page == 'settings' else a.click('nav-'+page)
            data = a.call();main = data['elements']['content']['rect']
            # 主要操作の幅とページ表示を、実際にレイアウトした座標で確認する。
            check(f'DPI {scale} {page} ページ表示', data['elements']['page-'+page]['visible'] and main[2] > 200)
            content = data['elements']['content']
            check(f'DPI {scale} {page} 横はみ出しなし', content['scrollWidth'] <= content['clientWidth'] + 1)
        a.click('nav-matching')
        for tab in ['direct-tab', 'matching-tab']:
            a.click(tab)
            content = a.call()['elements']['content']
            check(f'DPI {scale} {tab} 横はみ出しなし', content['scrollWidth'] <= content['clientWidth'] + 1)
    a.call('dpi', value=1.0);a.click('nav-matching')
    a.wait(lambda d: len(d['state']['matching']['people']) >= 23)
    check('一覧1ページ20名', a.call()['elements']['people']['text'].count('data-command="select-person"') == 20)
    check_public_layout(a, report)
    check_public_listing_times(a, report)
    check_matching_profile(a, report)
    check('公開一覧は名前と経過時間', 'GUI pagination fixture' not in a.call()['elements']['people']['text'] and 'comment' not in a.call()['elements'])
    check('未選択では一覧操作を無効化', a.call()['elements']['invite-selected']['disabled'] and a.call()['elements']['watch-selected']['disabled'])
    a.click('player-0')
    check('名前選択で申し込み操作を有効化', not a.call()['elements']['invite-selected']['disabled'])
    a.click('next');check('一覧2ページ3名', a.call()['elements']['people']['text'].count('data-command="select-person"') == 3)
    check('ページ切替で選択を解除', a.call()['elements']['invite-selected']['disabled'])
    a.click('prev')
    fixture = json.loads(json.dumps(a.call()['state']))
    fixture['matching']['people'][0]['name'] = '<img src=x> & PLAYER'
    fixture['matching']['people'][0]['comment'] = 'COMMENT_MUST_NOT_APPEAR'
    a.call('fixture', state=fixture)
    markup = a.call()['elements']['people']['text']
    check('名前をマークアップとして扱わずコメント非表示', '&lt;img' in markup and '<img' not in markup and 'COMMENT_MUST_NOT_APPEAR' not in markup)
    a.click('player-0');a.click('invite-selected')
    check('選択相手のコードに申し込む', a.commands[-1] == {'type':'matching_invite', 'code':fixture['matching']['people'][0]['code']})
    fixture['matching']['people'].pop(0)
    a.call('fixture', state=fixture)
    check('選択相手の掲載終了で別人に移らない', a.call()['elements']['invite-selected']['disabled'])
    a.call('fixture', state={})
    a.settings();a.checked('sound', False);a.checked('software-rendering', True)
    a.wait(lambda d: not d['state']['settings']['Sound'] and d['state']['settings']['SoftwareRendering'])
    check('設定を保存')
    a.click('nav-matching')
    a.click('player-0')
    selected_name = a.call()['elements']['selected-player']['text']
    a.call('renderer', software=True)
    check('描画切替後も名前選択を維持', a.call()['elements']['selected-player']['text'] == selected_name and not a.call()['elements']['invite-selected']['disabled'])
    # ポートは通常折りたたみ。実表示してから不正値と公開開始を検証する。
    a.click('matching-options-toggle')
    a.value('matching-port', 0);a.click('start-matching')
    a.wait(lambda d: bool(d['state']['error']) and not d['state']['matching']['registered'])
    check('公開待機の不正ポートを拒否')
    a.value('matching-port', ports[0]);a.click('start-matching')
    host = a.wait(lambda d: d['state']['matching']['registered'] and d['state']['matching']['public'])['state']['matching']['code']
    check('公開待機開始で公開登録', a.call()['state']['matching']['public'])
    a.wait(lambda d: any(p['code'] == host and p.get('self') for p in d['state']['matching']['people']))
    a.select_player(host)
    own = a.call()['elements']
    check('自分の公開募集も一覧に表示', '自分' in own['people']['text'] and '自分の募集' in own['selected-player']['text'])
    check('自分への申し込み・観戦は無効', own['invite-selected']['disabled'] and own['watch-selected']['disabled'])
    check('公開掲載中は再掲載ボタンを表示しない', not a.call()['elements']['switch-visibility']['visible'])
    a.click('direct-tab')
    check('待機中のタブ切替では公開を維持', a.call()['state']['matching']['public'] and a.call()['elements']['direct-code']['visible'])
    a.call('renderer', software=False)
    check('描画再構築後も直接接続タブと公開状態を維持', a.call()['elements']['direct-panel']['visible'] and a.call()['state']['matching']['public'])
    a.click('matching-tab')
    b.wait(lambda d: any(p['code'] == host for p in d['state']['matching']['people']))
    host_person = next(p for p in b.call()['state']['matching']['people'] if p['code'] == host)
    check('公開待機にコメントを付けない', host_person['comment'] == '')
    b.click('direct-tab');b.value('direct-port', ports[1]);b.value('direct-code', host);b.click('start-direct')
    a.wait(lambda d: len(d['state']['matching']['incoming']) == 1)
    guest = b.wait(lambda d: d['state']['matching']['registered'] and bool(d['state']['matching']['outgoing']['id']))['state']['matching']['code']
    check('統合コード欄から公開マッチングへ申し込み', not b.call()['state']['session']['running'] and b.call()['elements']['outgoing']['visible'])
    a.click('reject-0');a.wait(lambda d: not d['state']['matching']['incoming'])
    b.wait(lambda d: not d['state']['matching']['outgoing']['id']);check('申し込みを断る')
    # 一覧に載っていない既存形式の個人コードも同じ欄から解決する。
    a.click('direct-tab');a.value('direct-code', guest);a.click('start-direct')
    b.wait(lambda d: len(d['state']['matching']['incoming']) == 1)
    check('統合コード欄から未掲載の個人コードへ申し込み', b.call()['elements']['requests-card']['visible'])
    b.click('reject-0');b.wait(lambda d: not d['state']['matching']['incoming'])
    a.wait(lambda d: not d['state']['matching']['outgoing']['id']);a.click('matching-tab')
    b.click('matching-tab');b.click('switch-visibility')
    b.wait(lambda d: d['state']['matching']['public'])
    check('同じ個人コードで公開待機に移行', b.call()['state']['matching']['code'] == guest)
    b.click('direct-tab');b.click('start-direct');a.wait(lambda d: len(d['state']['matching']['incoming']) == 1)
    b.wait(lambda d: bool(d['state']['matching']['outgoing']['id']) and d['elements']['cancel-request']['visible'])
    b.click('cancel-request');a.wait(lambda d: not d['state']['matching']['incoming']);check('申し込み取消')
    watch.select_player(host);watch.click('watch-selected')
    watch.wait(lambda d: d['state']['session']['running'] and '待機中' in d['state']['session']['status'])
    check('観戦開始前の待機')
    b.select_player(host);b.click('invite-selected')
    a.wait(lambda d: len(d['state']['matching']['incoming']) == 1)
    check('公開一覧から対戦申し込み')
    a.click('accept-0')
    return host, guest


def after_game(guis, codes, report):
    a, b, watch = guis
    for gui, code in zip([a, b], codes):
        data = gui.wait(lambda d: d['state']['matching']['state'] == 'waiting' and not d['state']['session']['running'])
        assert data['state']['matching']['code'] == code
    report['checks'].append('同じ個人コードで待受へ復帰')
    watch.wait(lambda d: not d['state']['session']['running'])
    report['checks'].append('一組の終了で観戦も停止')
    a.click('stop-matching');b.click('stop-matching')
    for gui in [a, b]:
        gui.wait(lambda d: not d['state']['matching']['registered'])
    report['passed'] = True


def before_direct_game(guis, ports, report):
    a, b, watch = guis
    for gui in guis:
        initial = gui.wait(lambda d: (d.get('state') or {}).get('protocol') == 1)
        if initial['state']['language'] != 'ja':
            gui.click('language');gui.wait(lambda d: d['state']['language'] == 'ja')
        gui.settings();gui.value('connection-preference', 0)
        gui.checked('allow-spectators', True);gui.checked('sound', False)
        gui.click('nav-matching');gui.click('direct-tab')
    a.value('direct-port', ports[0]);a.value('direct-code', ' \t ');a.click('start-direct')
    code = a.wait(lambda d: d['state']['session']['running'] and len(d['state']['session']['code']) == 6)['state']['session']['code']
    report['checks'].append('空白のみの接続コードで指定ポートの待受を開始')
    watch.click('nav-spectate');watch.value('spectator-code', code);watch.click('watch')
    watch.wait(lambda d: d['state']['session']['running'])
    b.value('direct-port', ports[1]);b.type('direct-code', ' '+code+' ')
    b.call('renderer', software=True)
    assert b.call()['elements']['direct-code']['value'] == ' '+code+' '
    assert b.call()['elements']['direct-panel']['visible']
    report['checks'].append('描画再構築後も直接接続のコード入力を維持')
    b.click('start-direct')
    b.wait(lambda d: d['state']['session']['running'])
    report['checks'].append('コード入力から接続を開始（前後の空白を除去）')
    return [code]


def after_direct_game(guis, codes, report):
    for gui in guis:
        gui.wait(lambda d: not d['state']['session']['running'], seconds=45)
    report['checks'].append('直接対戦の終了後に対戦・観戦の全GUIが復帰')
    report['passed'] = True
