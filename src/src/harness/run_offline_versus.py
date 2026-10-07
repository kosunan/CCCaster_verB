"""独立コピーと2台の仮想DS4でオフライン対戦を検証。ゲームメモリは読取り専用。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import time
import traceback
import psutil
from run_training_character import device_guid
from run_training_corner import protected, digest
from rml_gui_driver import Gui

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect-seconds', type=int, default=0)
    parser.add_argument('--inputs-only', action='store_true', help='入力・D設定・ポーズを確認したら終了し、KO/再戦は重ねない')
    parser.add_argument('--timing-trace', action='store_true', help='補助時計と通常更新の診断を記録する')
    args = parser.parse_args()
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    out = ROOT / 'test/logs' / time.strftime('offline_versus_%Y%m%d_%H%M%S')
    runtime = ROOT / 'test/runtime/offline_versus'
    out.mkdir(parents=True)
    sources = [ROOT / 'test/runtime' / f'MBAACC_{i}' for i in (1, 2, 3)]
    before = {str(p): protected(p) for p in sources}
    caster = runtime / 'MBAACC_1/cccaster_B'
    game_path = (caster.parent / 'MBAA.exe').resolve()
    log = caster / 'cccaster_hook_log.txt'
    report = dict(checks=[], samples=[], errors=[], runtime=str(runtime))
    procs, pads, handle, copy_before = [], [], None, None
    config_backups = {}
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]

    def text():
        return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''

    def wait(predicate, reason, seconds=30):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            if predicate():
                return
            time.sleep(.04)
        raise RuntimeError(reason)

    def read(address, size=4, signed=False):
        value = C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle, address, value, size, None):
            raise C.WinError(C.get_last_error())
        return int.from_bytes(value.raw, 'little', signed=signed)

    def sample(label):
        value = dict(label=label, mode=read(0x54EEE8), intro=read(0x55D20B, 1),
                     world=read(0x55D1D4), sim=read(0x55D1CC), kind=read(0x562A74),
                     versus=read(0x77BF2C), pause=read(0x55D203, 1),
                     p1x=read(0x555238, signed=True), p2x=read(0x555D34, signed=True),
                     hp=[read(0x5551EC), read(0x555CE8)], timer=read(0x562A3C),
                     selections=[read(0x74D8EC), read(0x74D910)],
                     cursors=[read(0x74D8F8), read(0x74D91C)],
                     characters=[read(0x74D8FC), read(0x74D920)], stage=read(0x74FD98))
        report['samples'].append(value)
        print(json.dumps(value), flush=True)
        return value

    buttons = dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE, b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
                   c=vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE, start=vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT)
    directions = dict(left=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,
                      right=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
                      up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,
                      down=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH)

    def press(side, command, duration=.12):
        pad = pads[side]
        pad.reset()
        if command in buttons:
            pad.press_button(buttons[command])
        else:
            pad.directional_pad(directions[command])
        pad.update()
        time.sleep(duration)
        pad.reset()
        pad.update()
        time.sleep(.13)

    def games():
        matches = []
        for p in psutil.process_iter(['exe']):
            try:
                if Path(psutil.Process(p.pid).exe()).resolve() == game_path:
                    matches.append(psutil.Process(p.pid))
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return matches

    def attach():
        nonlocal handle
        wait(lambda: len(games()) == 1, 'ゲーム未起動')
        game = games()[0]
        handle = k.OpenProcess(0x10, False, game.pid)
        if not handle:
            raise C.WinError(C.get_last_error())
        wait(lambda: '[FastBoot] ★ CharaSelect reached!' in text(), 'キャラ選択未到達')
        wait(lambda: read(0x54EEE8) == 20, 'キャラ選択mode不一致')
        first = read(0x55D1D4)
        wait(lambda: read(0x55D1D4) >= first + 90, 'キャラ選択の登場演出が進まない')
        assert not game.net_connections(kind='inet'), 'オフラインゲームに通信ソケットがある'
        assert 'mode=5 host=1 delay=0' in text()
        return game

    def close_game():
        nonlocal handle
        if handle:
            k.CloseHandle(handle)
            handle = None
        for game in games():
            game.terminate()
            game.wait(timeout=10)

    def battle():
        deadline = time.monotonic() + 40
        while time.monotonic() < deadline:
            if read(0x54EEE8) == 1 and read(0x55D20B, 1) == 0:
                return
            press(0, 'a')
            press(1, 'a')
        raise RuntimeError('両者を確定しても戦闘に未到達')

    def win_match():
        deadline = time.monotonic() + 240
        # 通常入力で接近・攻撃し、元ゲームのKO／ラウンド終了を通す。
        while time.monotonic() < deadline:
            if read(0x54EEE8) == 5:
                return
            if read(0x54EEE8) == 1 and read(0x55D20B, 1) == 0:
                if abs(read(0x555D34, signed=True)-read(0x555238, signed=True)) > 10000:
                    press(0, 'right', .2)
                press(0, 'c')
                press(0, 'a')
            else:
                press(0, 'a')
        raise RuntimeError('通常攻撃から試合結果に未到達')

    def result_menu():
        first = read(0x55D1D4)
        wait(lambda: read(0x55D1D4) >= first + 45, '結果表示が進まない')
        press(0, 'a')
        wait(lambda: read(0x767440) > 0, '標準の結果メニューが開かない')
        first = read(0x55D1D4)
        wait(lambda: read(0x55D1D4) >= first + 20, '結果メニューの登場演出が進まない')

    try:
        print('Logs:', out, flush=True)
        for source in sources:
            target = runtime / source.name
            if not target.exists():
                shutil.copytree(source, target, ignore=shutil.ignore_patterns('cccaster_hook_log.txt', 'broadcast'))
        with (out / 'deploy.log').open('w') as stream:
            subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'deploy.ps1'), '-TestRoot', str(runtime)],
                           cwd=ROOT, check=True, stdout=stream, stderr=subprocess.STDOUT)
        guids = []
        for product in random.SystemRandom().sample(range(0x8000, 0xFFFF), 2):
            class TestPad(vg.VDS4Gamepad):
                def target_alloc(self):
                    target = vc.vigem_target_ds4_alloc()
                    vc.vigem_target_set_vid(target, 0x054C)
                    vc.vigem_target_set_pid(target, product)
                    return target
            pads.append(TestPad())
            wait(lambda: device_guid((product << 16) | 0x054C), '仮想パッド識別失敗')
            guids.append(device_guid((product << 16) | 0x054C))
        copy_before = protected(runtime)
        for p in [caster / 'cccaster.ini', *[caster / f'Wireless Controller__{guid}.ini' for guid in guids]]:
            config_backups[p] = p.read_bytes() if p.exists() else None
        (caster / 'cccaster.ini').write_text(
            '[Settings]\n' + ''.join(f'P{i}Device=Wireless Controller\nP{i}DeviceGuid={guid}\n' for i, guid in enumerate(guids, 1)) +
            '[GUI]\nLanguage=ja\n[Netplay]\nDefaultDelay=8\n', encoding='utf-8')
        mapping = '[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n'
        for guid in guids:
            (caster / f'Wireless Controller__{guid}.ini').write_text(mapping, encoding='utf-8')
        env = {key: value for key, value in os.environ.items() if not key.startswith('CCCASTER_')}
        env.update(CCCASTER_INPUT_DIAGNOSTIC='1', CCCASTER_INPUT_TRACE='1', CCCASTER_STARTUP_TRACE='1', CCCASTER_NTFY_SERVER='http://127.0.0.1:1')
        if args.timing_trace:
            env.update(CCCASTER_UPDATE_CADENCE='1', CCCASTER_FRAME_TIMING_TRACE='1')
        if log.exists():
            log.replace(out / 'previous_game.log')
        with (out / 'cli.log').open('w') as stream:
            proc = subprocess.Popen([str(caster / 'CCCaster_B.exe'), '--offline'], cwd=caster, env=env,
                                    stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            procs.append(proc)
            attach()
            sample('cli_selection')
            close_game()
            proc.wait(timeout=10)
        shutil.copy2(log, out / 'cli_game.log')
        log.unlink()
        assert '[ OFFLINE READY ]' in (out / 'cli.log').read_text(encoding='utf-8', errors='replace')
        report['checks'].append('CLI --offline: 通信なし・キャラ選択到達・オンラインD8設定でもD0開始')
        ui = out / 'ui'
        ui.mkdir()
        with (out / 'gui.log').open('w') as stream:
            proc = subprocess.Popen([str(caster / 'CCCaster_B_GUI.exe'), '--ui-test-dir', str(ui)],
                                    cwd=caster, env=env, stdout=stream, stderr=subprocess.STDOUT)
        procs.append(proc)
        gui = Gui(ui)
        gui.wait(lambda d: (d.get('state') or {}).get('protocol') == 1)
        gui.click('nav-offline')
        for scale in (1., 1.5):
            data = gui.call('dpi', value=scale)
            assert data['page'] == 'offline' and data['elements']['offline']['visible']
            content = data['elements']['content']
            assert content['scrollWidth'] <= content['clientWidth'] + 1
        gui.call('dpi', value=1.)
        (out / 'gui_page.json').write_text(json.dumps(gui.call(), ensure_ascii=False, indent=2), encoding='utf-8')
        print('INSPECT GUI', flush=True)
        time.sleep(args.inspect_seconds)
        gui.click('offline')
        game = attach()
        assert gui.call()['elements']['offline']['disabled']
        report['checks'].append('GUIの起動・100%/150%の横はみ出しなし・起動中の二重起動防止')
        print('INSPECT BEFORE INPUT', flush=True)
        time.sleep(args.inspect_seconds)
        initial = sample('gui_selection')
        press(0, 'right')
        press(0, 'a')
        p1 = sample('p1_select')
        assert p1['selections'][0] == 1 and p1['selections'][1] == initial['selections'][1], '1Pだけキャラを決定できない'
        press(1, 'left')
        press(1, 'a')
        p2 = sample('p2_select')
        assert p2['selections'][1] == 1 and p2['selections'][0] == p1['selections'][0], '2Pだけキャラを決定できない'
        press(0, 'start')
        wait(lambda: '[SelectionOptions] OPEN' in text(), '設定メニュー未表示')
        press(1, 'right')
        wait(lambda: '[OfflineDelay] ACTIVE D=1 R=0 mode=20' in text(), 'ローカルD変更失敗')
        press(0, 'left')
        wait(lambda: '[OfflineDelay] ACTIVE D=0 R=0 mode=20' in text(), 'D0復帰失敗')
        assert sample('settings')['selections'] == p2['selections'], '設定中の選択漏れ'
        press(0, 'right')
        press(0, 'right')
        wait(lambda: '[OfflineDelay] ACTIVE D=2 R=0 mode=20' in text(), 'D2変更失敗')
        press(0, 'b')
        print('INSPECT SELECTION', flush=True)
        time.sleep(args.inspect_seconds)
        battle()
        start = sample('battle')
        press(0, 'left', .4)
        moved1 = sample('p1_move')
        press(1, 'right', .4)
        moved2 = sample('p2_move')
        assert moved1['p1x'] < start['p1x'] and moved1['p2x'] == start['p2x']
        assert moved2['p2x'] > moved1['p2x'] and moved2['p1x'] == moved1['p1x']
        assert re.search(r'mode=1 configuring=0 raw1=00040000 raw2=00000000 output1=00000000', text()), '戦闘のD2が入力に反映されない'
        report['checks'].append('2台のGUID別パッドによるキャラ選択・両者の独立移動・設定中の入力遮断・D0/1/2変更と戦闘中D2')
        press(1, 'start')
        paused = sample('pause')
        time.sleep(.3)
        still = sample('paused')
        assert paused['timer'] == still['timer'], 'ポーズでタイマーが止まらない'
        press(1, 'b')
        wait(lambda: read(0x562A3C) < still['timer'], 'ポーズ解除失敗')
        report['checks'].append('2PのSTARTでポーズ・Bで再開')
        print('INSPECT BATTLE', flush=True)
        time.sleep(args.inspect_seconds)
        if not args.inputs_only:
            win_match()
            sample('result')
            assert '[OfflineScore] p1=1 p2=0 unresolved=0' in text(), '1試合目の勝数不一致'
            print('INSPECT RESULT', flush=True)
            time.sleep(args.inspect_seconds)
            result_menu()
            press(0, 'a')
            battle()
            sample('rematch')
            report['checks'].append('通常の攻撃・KO・ラウンドを経た試合終了とONCE再戦')
            win_match()
            assert text().count('[OfflineScore]') == 2 and '[OfflineScore] p1=2 p2=0 unresolved=0' in text(), '再戦後の勝数が重複または不一致'
            result_menu()
            press(0, 'down')
            press(0, 'a')
            wait(lambda: read(0x54EEE8) == 20, '結果メニューからキャラ選択へ戻れない')
            sample('character_return')
            report['checks'].append('結果メニューからキャラ選択へ復帰・試合勝数を各1回加算')
        assert not game.net_connections(kind='inet')
        assert not re.search(r'\[Exception\]|\[SceneRunner\] FAILED|SYNC TIMEOUT', text())
        close_game()
        gui.wait(lambda d: not d['elements']['offline']['disabled'])
        report['checks'].append('終了後にGUIの起動ボタンが復帰・ゲーム通信ソケット0')
    except Exception as exc:
        report['errors'].append(traceback.format_exc())
    finally:
        for pad in pads:
            pad.reset()
            pad.update()
        close_game()
        for proc in procs:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
        if log.exists():
            shutil.copy2(log, out / 'game.log')
        report['binaries'] = {name: digest(caster / name) for name in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll') if (caster / name).exists()}
        report['protected_unchanged'] = before == {str(p): protected(p) for p in sources}
        for p, original in config_backups.items():
            if original is None:
                if p.exists(): p.unlink()
            else: p.write_bytes(original)
        report['test_ini_unchanged'] = copy_before is not None and copy_before == protected(runtime)
        from verify_native_input_writes import verify_local_disabled
        report['native_input_rollback'] = verify_local_disabled(text())
        if not report['native_input_rollback']['passed']:
            report['errors'].append('通常オフライン対戦で自入力の再計算が残っているか、初期化がない')
        report['passed'] = not report['errors'] and report['protected_unchanged'] and report['test_ini_unchanged']
        (out / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
