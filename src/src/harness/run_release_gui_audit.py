"""独立した新規配置で初期値・日英画面・設定保存を検証。公開サービスへ投稿しない。"""
import argparse
import ctypes
from ctypes import wintypes
import datetime
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time

from rml_gui_driver import Gui
from test_p2p_service import ROOT, Service


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect', action='store_true', help='目視確認用にGUIを残し、finishファイルで終了')
    args = parser.parse_args()
    out = ROOT / 'test/logs' / datetime.datetime.now().strftime('release_gui_audit_%Y%m%d_%H%M%S')
    out.mkdir()
    target = out / 'fresh' / 'cccaster_B'
    target.mkdir(parents=True)
    for name in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll'):
        shutil.copyfile(ROOT / 'build/bin' / name, target / name)
    ui = out / 'ui'
    ui.mkdir()
    service = Service()
    threading.Thread(target=service.serve_forever, daemon=True).start()
    env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
    env['CCCASTER_NTFY_SERVER'] = f'http://127.0.0.1:{service.server_port}'
    gui = Gui(ui)
    process = None
    result = dict(passed=False, checks=[])

    def start():
        return subprocess.Popen([str(target / 'CCCaster_B_GUI.exe'), '--ui-test-dir', str(ui)],
                                cwd=target, env=env)

    def close():
        if process and process.poll() is None:
            process.terminate()
            process.wait(timeout=15)

    def save(name, value):
        (out / name).write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding='utf-8')

    def check_connection(snapshot, language, choice):
        labels = (('自動（LAN／IPv6／IPv4）', 'IPv4優先', 'IPv6優先') if language == 'ja'
                  else ('Automatic (LAN / IPv6 / IPv4)', 'Prefer IPv4', 'Prefer IPv6'))
        # 状態通知の直後は次の描画更新前の場合があるため、表示への反映も待つ。
        if snapshot['elements']['connection-preference']['selectedText'] != labels[choice]:
            snapshot = gui.wait(lambda s: s['elements']['connection-preference']['selectedText'] == labels[choice])
        element = snapshot['elements']['connection-preference']
        assert element['options'] == [dict(value=str(i), label=text) for i, text in enumerate(labels)], element
        assert element['value'] == str(choice) and element['selectedText'] == labels[choice], element
        assert snapshot['state']['settings']['ConnectionPreference'] == choice
        assert 'If the screen is blank' not in snapshot['elements']['page-settings']['text']
        assert '画面が映らないときは' not in snapshot['elements']['page-settings']['text']
        check_display_menu(language)
        result['checks'].append(f'{language}: 接続優先{choice}の候補・選択表示・設定値・表示メニュー')
        return snapshot

    def check_display_menu(language):
        user = ctypes.windll.user32
        user.GetMenu.argtypes = [wintypes.HWND]; user.GetMenu.restype = wintypes.HMENU
        user.GetSubMenu.argtypes = [wintypes.HMENU, ctypes.c_int]; user.GetSubMenu.restype = wintypes.HMENU
        user.GetMenuStringW.argtypes = [wintypes.HMENU, wintypes.UINT, wintypes.LPWSTR, ctypes.c_int, wintypes.UINT]
        user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        windows = []
        callback_type = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def collect(window, _):
            pid = wintypes.DWORD()
            user.GetWindowThreadProcessId(window, ctypes.byref(pid))
            if pid.value == process.pid and user.GetMenu(window): windows.append(window)
            return True
        user.EnumWindows(callback_type(collect), 0)
        assert len(windows) == 1, windows
        menu = user.GetMenu(windows[0]); render = user.GetSubMenu(menu, 0)
        actual = []
        for target, index in ((menu, 0), (render, 0), (render, 1)):
            buffer = ctypes.create_unicode_buffer(128)
            assert user.GetMenuStringW(target, index, buffer, len(buffer), 0x400)
            actual.append(buffer.value)
        expected_menu = (['表示', '自動描画で再読み込み', 'CPU描画で再読み込み（F8）'] if language == 'ja'
                         else ['Display', 'Reload with automatic rendering', 'Reload with CPU rendering (F8)'])
        assert actual == expected_menu, actual

    try:
        process = start()
        initial = gui.wait(lambda s: not s['elements']['loading']['visible'])
        save('initial.json', initial)
        expected = dict(Sound=True, FlashTaskbar=True, DesktopPopup=True, ConnectionPreference=0,
                        AllowSpectators=True, ShowOpponentExtraColors=True, BossCharacters=False,
                        TrainingStandby=False, SoftwareRendering=False, NtfyServer='https://ntfy.sh',
                        public=False, port=7500, delay=2, rollback=7)
        assert initial['state']['settings'] == expected, initial['state']['settings']
        assert initial['state']['language'] == 'ja'
        assert initial['state']['profile']['name'] == '' and initial['state']['profile']['emblemId'] == 0
        assert not initial['state']['matching']['registered'] and not initial['state']['session']['running']
        assert not (target / 'cccaster.ini').exists()
        result['checks'].append('新INI・旧INIなしの初期値、未登録・未接続、起動だけではINIを作らない')
        for language in ('ja', 'en'):
            if gui.call()['state']['language'] != language:
                gui.click('language')
                gui.wait(lambda s: s['state']['language'] == language)
            for page in ('matching', 'spectate', 'controller', 'settings', 'guide'):
                if page == 'settings':
                    gui.settings()
                else:
                    gui.click('nav-' + page)
                snapshot = gui.wait(lambda s: s['page'] == page)
                assert not snapshot['overflow'], (language, page, snapshot['overflow'])
                assert not snapshot['state']['error'], snapshot['state']['error']
                save(f'{language}_{page}.json', snapshot)
                if page == 'settings':
                    for choice in range(3):
                        gui.value('connection-preference', choice)
                        snapshot = gui.wait(lambda s: s['state']['settings']['ConnectionPreference'] == choice)
                        snapshot = check_connection(snapshot, language, choice)
                        # 値を維持したまま切り替え、候補だけでなく閉じた選択欄も確認する。
                        for switched in ('en' if language == 'ja' else 'ja', language):
                            gui.click('language')
                            snapshot = gui.wait(lambda s: s['state']['language'] == switched)
                            snapshot = check_connection(snapshot, switched, choice)
                        save(f'{language}_connection_{choice}.json', snapshot)
                    gui.value('connection-preference', 0)
            gui.click('nav-matching')
            direct = gui.click('direct-tab')
            assert not direct['overflow']
            save(f'{language}_direct.json', direct)
            result['checks'].append(f'{language}: 5ページと直接接続の表示・横はみ出しなし')
        gui.settings()
        gui.value('connection-preference', 2)
        gui.wait(lambda s: s['state']['settings']['ConnectionPreference'] == 2)
        gui.type('player-name', 'RELEASE_TEST')
        gui.click('settings-back')
        gui.wait(lambda s: s['state']['profile']['name'] == 'RELEASE_TEST')
        close()
        process = start()
        persisted = gui.wait(lambda s: not s['elements']['loading']['visible'])
        assert persisted['state']['profile']['name'] == 'RELEASE_TEST'
        assert persisted['state']['language'] == 'en'
        assert persisted['state']['settings'] == dict(expected, ConnectionPreference=2)
        assert not persisted['state']['matching']['registered']
        save('persisted.json', persisted)
        result['checks'].append('名前・言語を保存し再起動で復元、他の初期値と未掲載状態を保持')
        gui.settings()
        check_connection(gui.call(), 'en', 2)
        for software in (True, False):
            gui.call('renderer', software=software)
            snapshot = gui.wait(lambda s: s['state']['display']['software'] == software)
            snapshot = check_connection(snapshot, 'en', 2)
            save(f'en_renderer_{software}.json', snapshot)
        gui.click('language')
        check_connection(gui.wait(lambda s: s['state']['language'] == 'ja'), 'ja', 2)
        result['passed'] = True
        print(f'Audit ready: {out}', flush=True)
        save('audit-ready.json', result)
        if args.inspect:
            deadline = time.monotonic() + 900
            while not (out / 'finish').exists() and process.poll() is None:
                if time.monotonic() >= deadline:
                    raise TimeoutError('目視確認の終了待ち上限')
                time.sleep(.2)
    except Exception as exc:
        result.update(passed=False, error=str(exc))
        raise
    finally:
        close()
        service.running = False
        with service.cv:
            service.cv.notify_all()
        service.shutdown()
        service.server_close()
        result['posts'] = sum(request[0] == 'POST' for request in service.requests)
        result['passed'] &= result['posts'] == 0
        save('result.json', result)
        print(json.dumps(dict(passed=result['passed'], logs=str(out)), ensure_ascii=False), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
