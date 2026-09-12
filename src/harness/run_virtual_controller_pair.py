"""ViGEm DS4 -> DirectInput -> 入力時計を通す実対戦自動試験。既存ViGEmBusとvgamepad==0.1.0が必要。"""
import argparse
import gc
import hashlib
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=105)
    parser.add_argument('--port', type=int, default=17870)
    parser.add_argument('--scenario', type=int, choices=(0, 1, 2), default=0)
    parser.add_argument('--network', default='15,25,5')
    parser.add_argument('--idle-select', type=float, default=18,
                        help='選択前の無操作秒数。ランチャーの旧15秒終了の回帰確認')
    parser.add_argument('--hotplug-delay', type=float, default=0,
                        help='ゲーム起動後、この秒数待ってから仮想パッドを接続する')
    parser.add_argument('--test-root', default='_TEST_MBAACC',
                        help='2つのMBAACCテストコピーを含むディレクトリ')
    args = parser.parse_args()
    if args.seconds < 70:
        parser.error('--seconds は70以上が必要')
    if args.idle_select < 0 or args.idle_select > args.seconds - 60:
        parser.error('--idle-select は0以上、試験時間から60秒を引いた値以下が必要')
    if args.hotplug_delay < 0 or args.hotplug_delay > 20:
        parser.error('--hotplug-delay は0以上20以下が必要')
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('0.0.0.0', args.port))
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc

    class TestPad(vg.VDS4Gamepad):
        def __init__(self, product):
            self.product = product
            super().__init__()

        def target_alloc(self):
            target = vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target, 0x054C)
            vc.vigem_target_set_pid(target, self.product)
            return target

    out = ROOT / 'build_logs' / time.strftime('virtual_pad_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    test_root = (ROOT / args.test_root).resolve()
    ini_before = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in test_root.rglob('*.ini')}
    result = dict(scenario=args.scenario, input_source='ViGEm DS4 -> DirectInput -> InputTimeline',
                  scripted_input=False, probes=[], errors=[])
    result['network'] = args.network
    result['seconds'] = args.seconds
    result['binaries'] = {name: hashlib.sha256((ROOT / 'build/bin' / name).read_bytes()).hexdigest()
                          for name in ('CCCaster_v10.exe', 'libcccaster_hook.dll')}
    pads, process = [], None
    files = [test_root / f'MBAACC_{s}/cccaster/cccaster_hook_log.txt' for s in (1, 2)]
    offsets, pending = [0, 0], [b'', b'']
    phases, phase_since, values, seen = [0, 0], [0., 0.], [None, None], [[], []]
    detected, detection_seconds = [False, False], [None, None]
    pads_connected_at = None

    def read_logs():
        for side, file in enumerate(files):
            if not file.exists():
                continue
            with file.open('rb') as stream:
                stream.seek(offsets[side])
                data = pending[side] + stream.read()
                offsets[side] = stream.tell()
            lines = data.split(b'\n')
            pending[side] = lines.pop()
            for raw in lines:
                line = raw.decode('utf-8', errors='replace')
                match = re.search(r'\[SceneRunner\] Phase change: \d+ -> (\d+)', line)
                if match:
                    phases[side], phase_since[side] = int(match[1]), time.monotonic()
                match = re.search(r'\[VirtualPad\] product=([0-9A-F]+) matches=(\d+) joy=(-?\d+) value=(\d+)', line)
                if match:
                    matches = int(match[2])
                    if matches == 1 and not detected[side]:
                        detected[side] = True
                        if pads_connected_at is not None:
                            detection_seconds[side] = time.monotonic() - pads_connected_at
                    elif matches != 1 and detected[side]:
                        raise RuntimeError(f'仮想パッドの識別失敗: side={side+1} {line}')
                    values[side] = int(match[4])
                    seen[side].append(values[side])

    def wait(duration):
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            read_logs()
            if process.poll() is not None:
                raise RuntimeError('操作完了前にランナーが終了')
            time.sleep(.025)

    def report(side, action):
        pad = pads[side]
        pad.reset()
        kind, value = action
        if kind == 'axis':
            pad.left_joystick(*value)
        elif kind == 'hat':
            pad.directional_pad(value)
        elif kind == 'button':
            pad.press_button(value)
        pad.update()

    neutral = ('neutral', 0)
    confirm = ('button', vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
    probes = [('axis_up', ('axis', (128, 0)), 8 << 16),
              ('axis_down', ('axis', (128, 255)), 2 << 16),
              ('axis_left', ('axis', (0, 128)), 4 << 16),
              ('axis_right', ('axis', (255, 128)), 6 << 16),
              ('axis_up_left', ('axis', (0, 0)), 7 << 16),
              ('hat_up', ('hat', vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH), 8 << 16),
              ('hat_down', ('hat', vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH), 2 << 16),
              ('hat_left', ('hat', vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST), 4 << 16),
              ('hat_right', ('hat', vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST), 6 << 16),
              ('A', confirm, 0x410),
              ('B', ('button', vg.DS4_BUTTONS.DS4_BUTTON_CROSS), 0x820),
              ('C', ('button', vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE), 8),
              ('D', ('button', vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE), 4)]

    def connect_pads():
        nonlocal pads_connected_at
        for product in (0x05C4, 0x09CC):
            pads.append(TestPad(product))
        pads_connected_at = time.monotonic()
        result['devices'] = [dict(vid=p.get_vid(), pid=p.get_pid(), bus_index=p.get_index()) for p in pads]

    try:
        if args.hotplug_delay == 0:
            connect_pads()
            time.sleep(2)
        env = os.environ.copy()
        for key in list(env):
            if key.startswith('CCCASTER_TEST_') or key == 'CCCASTER_SCRIPT_INPUT':
                del env[key]
        env.update(CCCASTER_SCRIPT_INPUT='0', CCCASTER_INPUT_TRACE='1', CCCASTER_TEST_RETRY_QUICK='1')
        command = ['pwsh.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                   str(ROOT / 'src/harness/run_bounded_real_pair.ps1'), '-Seconds', str(args.seconds),
                   '-Port', str(args.port), '-VirtualController', '-OutputDirectory', str(out),
                   '-TestRoot', str(test_root)]
        if args.network:
            command += ['-Network', args.network]
        with (out / 'runner.log').open('w', encoding='utf-8') as runner_log:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=runner_log,
                                       stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            # 既存ログの退避と新しい起動ログの作成を待つ。
            time.sleep(5)
            if args.hotplug_delay > 0:
                wait(args.hotplug_delay)
                connect_pads()
            deadline = time.monotonic() + 30
            while phases != [2, 2] or not all(detected):
                if time.monotonic() > deadline:
                    raise RuntimeError('キャラ選択または仮想パッド取得が開始しない')
                wait(.1)
            result['hotplug_delay'] = args.hotplug_delay
            result['hotplug_detection_seconds'] = detection_seconds
            wait(args.idle_select)
            result['idle_select_seconds'] = args.idle_select
            for side in (0, 1):
                for name, action, expected in probes:
                    starts = [len(s) for s in seen]
                    report(side, action)
                    wait(.2)
                    report(side, neutral)
                    wait(.2)
                    got, other = seen[side][starts[side]:], seen[1-side][starts[1-side]:]
                    passed = expected in got and values[side] == 0 and not any(other)
                    result['probes'].append(dict(side=side+1, name=name, expected=expected,
                                                observed=got, other=other, passed=passed))
                    if not passed:
                        raise RuntimeError(f'入力取得不一致: {result["probes"][-1]}')
            print(f'方向・ボタン・左右分離: {len(result["probes"])}件成功。実対戦継続: {out}', flush=True)
            while process.poll() is None:
                read_logs()
                now = time.monotonic()
                for side in (0, 1):
                    elapsed = now - phase_since[side]
                    action = neutral
                    if phases[side] == 2 and elapsed % .7 < .2:
                        action = confirm
                    elif phases[side] == 4:
                        cycle = int(elapsed / .2 + side * 3) % 12
                        action = probes[cycle % 9][1] if cycle < 9 else ('button',
                            vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE if side else vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
                    elif phases[side] == 5:
                        if args.scenario == 0:
                            if elapsed > (2 if side == 0 else 7) and elapsed % .8 < .2:
                                action = confirm
                        elif side == args.scenario - 1:
                            if 2 < elapsed < 2.3:
                                action = confirm
                            elif 3 < elapsed < 3.3:
                                action = probes[6][1]
                            elif elapsed > 4 and elapsed % .8 < .2:
                                action = confirm
                    report(side, action)
                time.sleep(.025)
            result['runner_exit'] = process.returncode
    except Exception as exc:
        result['errors'].append(str(exc))
    finally:
        for pad in pads:
            pad.reset()
            pad.update()
        # 起動したゲームの終了・ログ回収はPowerShellのfinallyで行う。
        if process is not None and process.poll() is None:
            process.wait(timeout=args.seconds + 40)
        pads.clear()
        if 'pad' in locals():
            del pad
        gc.collect()
        result['ini_unchanged'] = all(Path(p).exists() and hashlib.sha256(Path(p).read_bytes()).hexdigest() == h
                                      for p, h in ini_before.items())
        (out / 'ini_before.json').write_text(json.dumps(ini_before, indent=2), encoding='utf-8')
    for script, extra in [('compare_rollback_pair.py', []), ('verify_native_retry_pair.py', [str(args.scenario)])]:
        completed = subprocess.run([sys.executable, str(ROOT / 'src/harness' / script), str(out), *extra],
                                   capture_output=True, text=True, encoding='utf-8', errors='replace')
        (out / (script + '.txt')).write_text(completed.stdout + completed.stderr, encoding='utf-8')
        result[script] = completed.returncode == 0
    logs = [(out / f'game_{s}.log').read_text(encoding='utf-8', errors='replace')
            if (out / f'game_{s}.log').exists() else '' for s in (1, 2)]
    loaded = [re.findall(r'\[Select\] LOADED (.*)', log) for log in logs]
    result['loaded_selections'] = loaded
    result['same_loaded_selection'] = bool(loaded[0] and loaded[0] == loaded[1])
    result['random_stages'] = list(map(int, re.findall(r'\[Select\] RANDOM resolved=(\d+)', logs[0])))
    result['random_stage_exercised'] = bool(result['random_stages'] and all(result['random_stages']))
    result['passed'] = bool(not result['errors'] and len(result['probes']) == 26 and
                            all(p['passed'] for p in result['probes']) and result['ini_unchanged'] and
                            result.get('runner_exit') == 0 and result['compare_rollback_pair.py'] and
                            result['verify_native_retry_pair.py'] and result['same_loaded_selection'] and
                            result['random_stage_exercised'])
    (out / 'virtual_controller_result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(logs=str(out), passed=result['passed'], errors=result['errors']), ensure_ascii=False), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
