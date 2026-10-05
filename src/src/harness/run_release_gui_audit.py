"""独立した新規配置で初期値・日英画面・設定保存を検証。公開サービスへ投稿しない。"""
import argparse
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

    try:
        process = start()
        initial = gui.wait(lambda s: not s['elements']['loading']['visible'])
        save('initial.json', initial)
        expected = dict(Sound=True, FlashTaskbar=True, DesktopPopup=True, ConnectionPreference=0,
                        AllowSpectators=True, SoftwareRendering=False, NtfyServer='https://ntfy.sh',
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
            for page in ('matching', 'spectate', 'training', 'replay', 'settings', 'guide'):
                if page == 'settings':
                    gui.settings()
                else:
                    gui.click('nav-' + page)
                snapshot = gui.wait(lambda s: s['page'] == page)
                assert not snapshot['overflow'], (language, page, snapshot['overflow'])
                assert not snapshot['state']['error'], snapshot['state']['error']
                save(f'{language}_{page}.json', snapshot)
            gui.click('nav-matching')
            direct = gui.click('direct-tab')
            assert not direct['overflow']
            save(f'{language}_direct.json', direct)
            result['checks'].append(f'{language}: 6ページと直接接続の表示・横はみ出しなし')
        gui.settings()
        gui.type('player-name', 'RELEASE_TEST')
        gui.click('settings-back')
        gui.wait(lambda s: s['state']['profile']['name'] == 'RELEASE_TEST')
        close()
        process = start()
        persisted = gui.wait(lambda s: not s['elements']['loading']['visible'])
        assert persisted['state']['profile']['name'] == 'RELEASE_TEST'
        assert persisted['state']['language'] == 'en'
        assert persisted['state']['settings'] == expected
        assert not persisted['state']['matching']['registered']
        save('persisted.json', persisted)
        result['checks'].append('名前・言語を保存し再起動で復元、他の初期値と未掲載状態を保持')
        gui.click('language')
        gui.wait(lambda s: s['state']['language'] == 'ja')
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
