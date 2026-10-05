"""指定テストコピーの標準リプレイを1回再生し、フレーム末尾とPresentMonを併記する。

既存ViGEm DS4環境を使用。READY後にComputer Useで前面・表示モードを確認し、
出力先へgoファイルを作成する。ゲーム状態へ書き込まず、通常の決定入力だけを送る。
"""
import argparse
import csv
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import mmap
import os
from pathlib import Path
import subprocess
import time

from bench_legacy_real import (processes, open_process, close, path_of, read32,
                               ticks, frequency, terminate, wait, HEADER, MAP_SIZE, RECORD, U)

ROOT = Path(__file__).resolve().parents[3]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--game-dir', type=Path, default=ROOT/'test/runtime/MBAACC_1')
    parser.add_argument('--timeout', type=int, default=130)
    parser.add_argument('--present-interval', choices=('original', 'one'), default='original')
    parser.add_argument('--track-dwm', action='store_true', help='合成後の表示時刻をDWMの提示と照合する')
    args = parser.parse_args()
    out, game_dir = args.out.resolve(), args.game_dir.resolve()
    out.mkdir(parents=True, exist_ok=False)
    game = game_dir/'MBAA.exe'
    caster = game_dir/'cccaster_B'
    replay_files = list((game_dir/'ReplayVS').glob('*.rep'))
    if len(replay_files) != 1:
        raise RuntimeError('対象フォルダーのREPは1本にしてください')
    for pid, _, name in processes():
        if name.lower() == 'mbaa.exe':
            raise RuntimeError(f'既存ゲームへの仮想入力混入を避けるため先に終了が必要: {pid}')
    protected = {p: sha(p) for p in [*game_dir.rglob('*.ini'), *replay_files, game]}
    result = dict(replay=str(replay_files[0]), replay_sha256=sha(replay_files[0]),
                  frequency=frequency.value, foreground=[], events=[], binaries={})
    for filename in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll'):
        result['binaries'][filename] = sha(caster/filename)
        if result['binaries'][filename] != sha(ROOT/'build/bin'/filename):
            raise RuntimeError(f'最新版と不一致: {filename}')
    log_path = caster/'cccaster_hook_log.txt'
    log_offset = log_path.stat().st_size if log_path.exists() else 0
    game_handle = launcher = monitor = observed = pad = None
    session = 'CCCasterReplay_' + str(os.getpid())
    pm = ROOT/'test/tools/PresentMon/PresentMon-2.5.1-x64.exe'
    try:
        import vgamepad as vg
        pad = vg.VDS4Gamepad()
        time.sleep(1)
        env = {k: v for k, v in os.environ.items() if not k.startswith(('CCCASTER_', 'CCBENCH_'))}
        env['CCCASTER_TEST_VIRTUAL_PRODUCT'] = '05C4054C'
        result['environment'] = {'CCCASTER_TEST_VIRTUAL_PRODUCT': '05C4054C'}
        if args.present_interval != 'original':
            env['CCCASTER_TEST_PRESENT_INTERVAL'] = args.present_interval
            result['environment']['CCCASTER_TEST_PRESENT_INTERVAL'] = args.present_interval
        with (out/'launcher.log').open('wb') as log, (out/'presentmon.log').open('wb') as pm_log:
            launcher = subprocess.Popen([str(caster/'CCCaster_B.exe'), '--headless', '--replay'],
                cwd=caster, env=env, stdout=log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            deadline = time.monotonic()+35
            while time.monotonic() < deadline and not game_handle:
                for pid, parent, name in processes():
                    if name.lower() != 'mbaa.exe' or parent != launcher.pid:
                        continue
                    candidate = open_process(0x1010 | 0x100000 | 1, False, pid)
                    if not candidate:
                        continue
                    if path_of(candidate) == game:
                        game_handle = candidate
                        result['game_pid'] = pid
                        break
                    close(candidate)
                time.sleep(.05)
            if not game_handle:
                raise RuntimeError('対象ゲームを検出できません')
            while read32(game_handle, 0x54eee8) != 26:
                if time.monotonic() > deadline:
                    raise RuntimeError('リプレイ一覧へ到達しません')
                time.sleep(.1)
            observer = ROOT/'test/tools/frame_observer'
            subprocess.run([str(observer/'legacy_benchmark_inject.exe'), str(result['game_pid']),
                str(observer/'legacy_benchmark_probe.dll'), str(game)], check=True,
                stdout=log, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(.3)
            observed = mmap.mmap(-1, MAP_SIZE, tagname=f"Local\\CCCasterLegacyBench_{result['game_pid']}")
            header = HEADER.unpack_from(observed)
            if header[0] != 0x42434343 or header[5] != 1:
                raise RuntimeError(f'フレーム観測器の初期化失敗: {header}')
            print(f"READY pid={result['game_pid']} go={out/'go'}", flush=True)
            deadline = time.monotonic()+300
            while not (out/'go').exists():
                if time.monotonic() > deadline or wait(game_handle, 0) == 0:
                    raise RuntimeError('開始待ちが終了しました')
                time.sleep(.1)
            foreground = W.DWORD()
            U.GetWindowThreadProcessId(U.GetForegroundWindow(), C.byref(foreground))
            if foreground.value != result['game_pid']:
                raise RuntimeError('ゲームが前面ではないため計測を開始しません')
            command = [str(pm), '--process_id', str(result['game_pid']), '--session_name', session,
                '--output_file', str(out/'presents.csv'), '--v1_metrics', '--qpc_time',
                '--no_console_stats', '--no_track_input', '--timed', str(args.timeout+5),
                '--terminate_after_timed']
            if args.track_dwm:
                dwm_pids = [pid for pid, _, name in processes() if name.lower() == 'dwm.exe']
                if len(dwm_pids) != 1:
                    raise RuntimeError(f'DWMの対象が一意ではありません: {dwm_pids}')
                # DWMは通常権限では名前が不明。PID指定と名前指定の併用では
                # ETW側のPIDフィルターでゲームが消えるため同時採取後に2PIDへ絞る。
                result['dwm_pid'] = dwm_pids[0]
                del command[1:3]
            result['presentmon_command'] = command
            monitor = subprocess.Popen(command, stdout=pm_log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            time.sleep(1.5)
            if monitor.poll() is not None:
                raise RuntimeError('PresentMonを開始できません')
            result['start_tick'] = ticks()
            pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE); pad.update()
            time.sleep(.18)
            pad.reset(); pad.update()
            deadline = time.monotonic()+args.timeout
            previous = None
            playing = False
            finished = False
            while time.monotonic() < deadline and wait(game_handle, 0) != 0:
                mode = read32(game_handle, 0x54eee8)
                world = read32(game_handle, 0x55d1d4)
                round_no = read32(game_handle, 0x77bfa4)
                now = ticks()
                if (mode, round_no) != previous:
                    event = dict(tick=now, mode=mode, round=round_no, world=world)
                    result['events'].append(event)
                    print('SCENE', json.dumps(event), flush=True)
                    previous = mode, round_no
                U.GetWindowThreadProcessId(U.GetForegroundWindow(), C.byref(foreground))
                result['foreground'].append(dict(tick=now, game=foreground.value == result['game_pid']))
                if mode == 1:
                    playing = True
                if playing and mode in (5, 26):
                    finished = True
                    break
                time.sleep(.1)
            result['end_tick'] = ticks()
            result['terminal_mode'] = mode
            result['reached_replay_end'] = finished
            # 書き込み途中の末尾を含めず、公開されたcountだけを保存する。
            raw_header = observed[:4096]
            header = HEADER.unpack_from(raw_header)
            (out/'frames.bin').write_bytes(raw_header+observed[4096:4096+header[2]*RECORD.size])
            result['observer_header'] = header
            result['capture_complete'] = finished and header[5] == 1
            print('CAPTURE', result['capture_complete'], header, flush=True)
    except Exception as exc:
        result['error'] = str(exc)
        result['capture_complete'] = False
        print('ERROR', exc, flush=True)
    finally:
        if monitor and monitor.poll() is None:
            subprocess.run([str(pm), '--session_name', session, '--terminate_existing_session'],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                creationflags=subprocess.CREATE_NO_WINDOW)
            try:
                monitor.wait(timeout=8)
            except subprocess.TimeoutExpired:
                monitor.terminate(); monitor.wait(timeout=3)
        if monitor:
            result['presentmon_exit'] = monitor.returncode
        if pad:
            pad.reset(); pad.update()
        if game_handle:
            if wait(game_handle, 0) != 0:
                terminate(game_handle, 0); wait(game_handle, 3000)
            close(game_handle)
        if launcher:
            try:
                launcher.wait(timeout=5)
            except subprocess.TimeoutExpired:
                launcher.terminate(); launcher.wait(timeout=3)
        if observed:
            observed.close()
        if log_path.exists():
            with log_path.open('rb') as log:
                log.seek(log_offset)
                (out/'game.log').write_bytes(log.read())
        # 子プロセスを回収してからCSVを検証する。ETW採取失敗をドロップ0としない。
        try:
            csv_path = out/'presents.csv'
            with csv_path.open(encoding='utf-8-sig', newline='') as file:
                reader = csv.DictReader(file)
                columns, rows = reader.fieldnames, list(reader)
            result['game_present_rows'] = sum(int(r['ProcessID']) == result['game_pid'] for r in rows)
            if result.get('dwm_pid'):
                selected = [r for r in rows if int(r['ProcessID']) in (result['game_pid'], result['dwm_pid'])]
                result['other_process_rows_discarded'] = len(rows)-len(selected)
                result['dwm_present_rows'] = sum(int(r['ProcessID']) == result['dwm_pid'] for r in selected)
                with csv_path.open('w', encoding='utf-8-sig', newline='') as file:
                    writer = csv.DictWriter(file, fieldnames=columns)
                    writer.writeheader(); writer.writerows(selected)
                if not result['dwm_present_rows']:
                    raise RuntimeError('DWMのPresentを採取できませんでした')
            if not result['game_present_rows']:
                raise RuntimeError('ゲームのPresentを採取できませんでした')
        except Exception as exc:
            result['capture_complete'] = False
            result.setdefault('error', str(exc))
        result['protected_changes'] = [str(p) for p, digest in protected.items() if not p.exists() or sha(p) != digest]
        (out/'capture.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    return 0 if result.get('capture_complete') and not result['protected_changes'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
