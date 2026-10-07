"""キャラ選択メニューの実入力・D合意・背景ローカル設定を検査する。"""
import re


def verify(texts):
    errors = []
    for side, text in enumerate(texts, 1):
        events = re.findall(r'\[SelectionOptions\] (.*)', text)
        requests = re.findall(r'DELAY request=(\d+) accepted=1', '\n'.join(events))
        expected = ['1', '2'] if side == 1 else ['3']
        if requests != expected:
            errors.append(f'P{side}: D操作が不足または不正: {requests}')
        if events.count('OPEN') != 1 or events.count('CLOSE') != 1:
            errors.append(f'P{side}: メニュー開閉が不正')
        expected_bg = ['OFF', 'ON'] if side == 1 else ['OFF']
        bg = re.findall(r'BACKGROUND animation=(ON|OFF) mode=20', '\n'.join(events))
        # 元からOFFの端末では左OFFは無変更であり、書込みログは出ない。
        # ホストのOFF→ON実変更と両側の戦闘時設定は引き続き必須。
        if bg != expected_bg and not (side == 2 and not bg):
            errors.append(f'P{side}: 背景切替が不足または不正: {bg}')
        hud = re.findall(r'HUD mode=(\w+)', '\n'.join(events))
        expected_hud = ['DETAILED', 'HIDDEN', 'DETAILED'] if side == 1 else ['DETAILED', 'HIDDEN']
        if hud != expected_hud:
            errors.append(f'P{side}: HUD切替が不足または不正: {hud}')
        battle_hud = re.findall(r'BATTLE animation=\d+ delay=\d+ hud=(\w+)', '\n'.join(events))
        if not battle_hud or any(mode != expected_hud[-1] for mode in battle_hud):
            errors.append(f'P{side}: 戦闘開始後のHUDが不正: {battle_hud}')
        display = re.findall(r'DISPLAY action=(\w+) applied=(\d+) size=(\d+)x(\d+) fullscreen=(\d+)', '\n'.join(events))
        if len(display) != 2 or [(d[0], d[1], d[4]) for d in display] != [
                ('fullscreen', '1', '1'), ('fullscreen', '1', '0')]:
            errors.append(f'P{side}: 全画面切替が不正: {display}')
        requests = re.findall(r'RESOLUTION accepted=(\d+)', '\n'.join(events))
        resolutions = re.findall(r'\[NativeResolution\] COMPLETE applied=(\d+) requested=(\d+)x(\d+) backbuffer=(\d+)x(\d+) fullscreen=(\d+) window=(\d+)x(\d+) hr=(\w+)',text)
        if requests != ['1']*4 or len(resolutions) != 4:
            errors.append(f'P{side}: 描画解像度の要求／完了数が不正: {requests} / {resolutions}')
        elif any(r[0] != '1' or r[1:3] != r[3:5] or r[1:3] != r[6:8] or int(r[8],16) != 0 for r in resolutions):
            errors.append(f'P{side}: Reset失敗／実バックバッファ寸法／復帰寸法が不一致: {resolutions}')
        elif ([r[5] for r in resolutions] != ['0','0','1','1'] or resolutions[0][1:3] == resolutions[1][1:3] or
              resolutions[0][1:3] != resolutions[2][1:3] or resolutions[1][1:3] != resolutions[3][1:3]):
            errors.append(f'P{side}: 通常窓・全画面中の解像度往復が不正: {resolutions}')
        elif len(display) == 2 and any(d[2:4] != resolutions[-1][1:3] for d in display):
            errors.append(f'P{side}: 全画面切替時の窓寸法が不正: {display}')
        for name, count in [('CHARACTER_FILTER', 4), ('SCREEN_FILTER', 2), ('ASPECT_RATIO', 7), ('VIEW_FPS', 2)]:
            values = [int(v) for v in re.findall(r'NATIVE option=' + name + r' value=(\d+)', '\n'.join(events))]
            if len(values) != 2 or any(v >= count for v in values) or values[0] != (values[-1]+1) % count:
                errors.append(f'P{side}: {name}の往復変更が不正: {values}')
        battle = re.findall(r'BATTLE animation=(\d+) delay=(\d+)', '\n'.join(events))
        if not battle or any(pair != ('1' if side == 1 else '0', '3') for pair in battle):
            errors.append(f'P{side}: 戦闘開始後の背景設定またはD合意が不正: {battle}')
        close = text.find('[SelectionOptions] CLOSE')
        selected = text.find('[Select] LOCAL ')
        if close < 0 or selected < close:
            errors.append(f'P{side}: メニューを閉じる前にキャラが確定、または未確定')
    return dict(passed=not errors, errors=errors)
