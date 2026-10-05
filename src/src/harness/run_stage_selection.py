"""既存ViGEm DS4の通常入力でステージ一覧を一周し、指定ステージのロードを確認する。"""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import threading
import time

from bench_legacy_real import close, open_process, path_of, processes, read32, rpm, terminate, wait

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stage', type=int, choices=range(100), default=55, help='0はランダム')
    parser.add_argument('--baseline', action='store_true', help='修正前の一覧採取のみ')
    parser.add_argument('--hold', type=int, default=0, help='一覧と戦闘の画面確認用待機秒数')
    parser.add_argument('--netplay', action='store_true', help='現行P2Pの対戦2窓で同じ一覧とロードを検証')
    args = parser.parse_args()
    import vgamepad as vg

    game_dir = ROOT / 'test/runtime/MBAACC_1'
    runtime = game_dir / 'cccaster_B'
    out = ROOT / 'test/logs' / time.strftime('stage_selection_%Y%m%d_%H%M%S')
    out.mkdir()
    result = dict(baseline=args.baseline, netplay=args.netplay, stage=args.stage,
                  input='ViGEm DS4 -> DirectInput -> GameMem', passed=False)
    protected = [p for side in (1, 2, 3) for p in
                 [*(game_dir.parent / f'MBAACC_{side}').rglob('*.ini'),
                  game_dir.parent / f'MBAACC_{side}/MBAA.exe']]
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    before = {str(p): sha(p) for p in protected}
    result['binaries'] = {name: sha(runtime / name) for name in
                          ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')}
    for name, digest in result['binaries'].items():
        if sha(ROOT / 'build/bin' / name) != digest:
            raise RuntimeError('最新版3点をdeploy.batで配置してから実行してください')
    # 仮想入力が別の実ゲームへ届くことを防ぐ。対象の起動済みプロセスはdeployで終了する。
    if any(name.lower() == 'mbaa.exe' for _, _, name in processes()):
        raise RuntimeError('MBAAが起動中です。対象へのdeploy完了と対象外ゲームの終了を確認してください')
    log = runtime / 'cccaster_hook_log.txt'
    if log.exists():
        log.replace(out / 'before_game.log')
    pad, peer_pad, process, handle, peer_handle, service = None, None, None, None, None, None
    try:
        pad = vg.VDS4Gamepad()
        if args.netplay:
            from vgamepad.win import vigem_client as vc
            from test_p2p_service import Service, free_port

            class PeerPad(vg.VDS4Gamepad):
                def target_alloc(self):
                    target = vc.vigem_target_ds4_alloc()
                    vc.vigem_target_set_vid(target, 0x054c)
                    vc.vigem_target_set_pid(target, 0x09cc)
                    return target

            peer_pad = PeerPad()
            service = Service()
            threading.Thread(target=service.serve_forever, daemon=True).start()
        time.sleep(.5)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('CCCASTER_', 'CCBENCH_'))}
        env['CCCASTER_TEST_VIRTUAL_PRODUCT'] = '05C4054C'
        command = [str(runtime / 'CCCaster_B.exe'), '--training']
        if args.netplay:
            env['CCCASTER_NTFY_SERVER'] = f'http://127.0.0.1:{service.server_port}'
            command = [shutil.which('pwsh'), '-NoProfile', '-File',
                       str(ROOT / 'src/src/harness/run_bounded_real_pair.ps1'),
                       '-Seconds', str(95 + args.hold * 2), '-Port', str(free_port()), '-Network', '15,25,5',
                       '-VirtualController', '-UseConnectionCode', '-CloseSide', '1',
                       '-OutputDirectory', str(out / 'pair')]
        with (out / 'launcher.log').open('wb') as stream:
            process = subprocess.Popen(command, cwd=runtime,
                                       env=env, stdout=stream, stderr=subprocess.STDOUT,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic() + 35
            while time.monotonic() < deadline:
                for pid, parent, name in processes():
                    if (not args.netplay and parent != process.pid) or name.lower() != 'mbaa.exe':
                        continue
                    candidate = open_process(0x1000 | 0x10 | 0x100000 | 1, False, pid)
                    if not candidate:
                        continue
                    candidate_path = path_of(candidate)
                    if not handle and candidate_path == (game_dir / 'MBAA.exe').resolve():
                        handle = candidate
                        result['game_pid'] = pid
                    elif args.netplay and not peer_handle and candidate_path == (game_dir.parent / 'MBAACC_2/MBAA.exe').resolve():
                        peer_handle = candidate
                        result['peer_pid'] = pid
                    else:
                        close(candidate)
                if handle and (not args.netplay or peer_handle):
                    break
                time.sleep(.1)
            if not handle or (args.netplay and not peer_handle):
                raise RuntimeError('ゲーム起動タイムアウト')

            def state(target=None):
                target = target or handle
                return dict(mode=read32(target, 0x54eee8), p1=read32(target, 0x74d8ec),
                            p2=read32(target, 0x74d910), stage=read32(target, 0x74fd98))

            def pulse(up=False, target=None):
                target = target or pad
                target.reset()
                if up:
                    target.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH)
                else:
                    target.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
                target.update()
                time.sleep(.06)
                target.reset()
                target.update()
                time.sleep(.18)

            while time.monotonic() < deadline and state()['mode'] != 20:
                time.sleep(.1)
            if state()['mode'] != 20:
                raise RuntimeError('キャラ選択到達タイムアウト')
            result['tables'] = {}
            result['available_stages'] = [i for i in range(100) if read32(handle, 0x74fc08 + i * 4)]
            for address, size in [(0x54cebc, 20), (0x54cf68, 84), (0x7695dc, 40)]:
                buf = (C.c_ubyte * size)()
                count = C.c_size_t()
                if not rpm(handle, address, buf, size, C.byref(count)) or count.value != size:
                    raise RuntimeError('テーブル読取り失敗')
                result['tables'][hex(address)] = bytes(buf).hex(' ')
            deadline = time.monotonic() + 25
            while time.monotonic() < deadline:
                if state()['p1'] >= 4 and state()['p2'] >= 4:
                    break
                if not args.netplay or state()['p1'] < 4:
                    pulse()
                elif state(peer_handle)['p2'] < 4:
                    pulse(target=peer_pad)
                else:
                    time.sleep(.1)
            else:
                raise RuntimeError('キャラ確定タイムアウト')
            time.sleep(.6)
            stages = [state()['stage']]
            for _ in range(105):
                pulse(up=True)
                value = state()['stage']
                if value == stages[0]:
                    break
                if value in stages:
                    raise RuntimeError(f'一覧の途中で循環: {stages}, {value}')
                stages.append(value)
            else:
                raise RuntimeError('一覧が一周しない')
            result['stages'] = stages
            print(f'一覧 {len(stages)}項目: {stages}', flush=True)
            if not args.baseline:
                if not {0, 55, 57, 58}.issubset(stages) or set(stages) != set(result['available_stages']):
                    raise RuntimeError('存在する背景の一覧と選択可能なステージが不一致')
                for _ in range(len(stages)):
                    if state()['stage'] == args.stage:
                        break
                    pulse(up=True)
                else:
                    raise RuntimeError('指定ステージが見つからない')
                print(f'選択中 stage={args.stage}, PID={result["game_pid"]}', flush=True)
                time.sleep(args.hold)
                pulse()
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    if (state()['mode'] == 1 and (read32(handle, 0x55d20b) & 255) == 0
                            and (not args.netplay or state(peer_handle)['mode'] == 1)):
                        break
                    time.sleep(.1)
                else:
                    raise RuntimeError('戦闘開始タイムアウト')
                result['loaded'] = state()
                if (args.stage and result['loaded']['stage'] != args.stage) or not result['loaded']['stage']:
                    raise RuntimeError('選択とロードのステージが不一致')
                if args.netplay:
                    result['peer_loaded'] = state(peer_handle)
                    if result['peer_loaded']['stage'] != result['loaded']['stage']:
                        raise RuntimeError('対戦相手のステージが不一致')
                timer = read32(handle, 0x55d1d4)
                time.sleep(max(2, args.hold))
                result['advanced_frames'] = read32(handle, 0x55d1d4) - timer
                if result['advanced_frames'] < 60:
                    raise RuntimeError('戦闘が進行していない')
                if args.netplay:
                    # 両側の入力を実DirectInput経路で変化させ、通信劣化ありの同期比較用ログを採る。
                    for _ in range(40):
                        if process.poll() is not None:
                            raise RuntimeError('対戦が予定より早く終了した')
                        pulse()
                        pulse(target=peer_pad)
                    process.wait(timeout=115 + args.hold * 2)
                    if process.returncode:
                        raise RuntimeError('実対戦ランナーが失敗した')
            result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        if pad:
            pad.reset()
            pad.update()
        if peer_pad:
            peer_pad.reset()
            peer_pad.update()
        if handle:
            terminate(handle, 0)
            wait(handle, 5000)
            close(handle)
        if peer_handle:
            terminate(peer_handle, 0)
            wait(peer_handle, 5000)
            close(peer_handle)
        if process:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        if log.exists():
            shutil.copy2(log, out / 'game.log')
        if service:
            service.running = False
            with service.cv:
                service.cv.notify_all()
            service.shutdown()
            service.server_close()
        after = {str(p): sha(p) for p in protected}
        result['protected_unchanged'] = before == after
        result['protected_files'] = len(before)
        result['passed'] &= result['protected_unchanged']
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(f'Logs: {out}', flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
