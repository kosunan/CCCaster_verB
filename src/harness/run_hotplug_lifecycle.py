"""実DirectInputの後挿し・切断時ニュートラル・相手パッド維持・再接続を検査する。"""
import gc
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, default=17943)
    parser.add_argument('--test-root', type=Path, default=ROOT/'_TEST_MBAACC')
    args = parser.parse_args()
    test_root = args.test_root.resolve(strict=True)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(('0.0.0.0', args.port))
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc

    class Pad(vg.VDS4Gamepad):
        def __init__(self, product):
            self.product = product
            super().__init__()

        def target_alloc(self):
            target = vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target, 0x054C)
            vc.vigem_target_set_pid(target, self.product)
            return target

    out = ROOT/'build_logs'/time.strftime('hotplug_lifecycle_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
    env.update(CCCASTER_DEVICE_TRACE='1', CCCASTER_FRAME_TIMING_TRACE='1')
    pads, process = [None, None], None
    result = {'passed': False, 'steps': [], 'input_source': 'ViGEm DS4 -> DirectInput -> InputTimeline'}
    logs = [test_root/f'MBAACC_{s}/cccaster_B/cccaster_hook_log.txt' for s in (1, 2)]

    def samples(side):
        text = logs[side].read_text(encoding='utf-8', errors='replace') if logs[side].exists() else ''
        return [(int(m), int(v)) for m, v in re.findall(
            r'\[VirtualPad\] product=\w+ matches=(\d+) joy=-?\d+ value=(\d+)', text)]

    def until(label, predicate, timeout=5):
        start = time.monotonic()
        while not predicate():
            if process.poll() is not None or time.monotonic()-start > timeout:
                raise RuntimeError(label+' timeout')
            time.sleep(.025)
        result['steps'].append({'name': label, 'seconds': time.monotonic()-start})

    def latest(side, expected):
        values = samples(side)
        return bool(values and values[-1] == expected)

    def hold(side):
        pads[side].press_button(vg.DS4_BUTTONS.DS4_BUTTON_CROSS)
        pads[side].update()

    try:
        with (out/'runner.log').open('w', encoding='utf-8') as log:
            process = subprocess.Popen(['pwsh', '-NoProfile', '-File',
                str(ROOT/'src/harness/run_bounded_real_pair.ps1'), '-Seconds', '55',
                '-Port', str(args.port), '-VirtualController', '-TestRoot', str(test_root),
                '-OutputDirectory', str(out)],
                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                creationflags=subprocess.CREATE_NO_WINDOW)
            # ランナーによる旧ログ退避とキャラ選択への到達を待つ。
            time.sleep(5)
            if process.poll() is not None:
                raise RuntimeError('ランナーがゲーム起動前に終了: runner.logを確認')
            until('initial_neutral', lambda: latest(0, (0, 0)) and latest(1, (0, 0)), 30)
            pads[0], pads[1] = Pad(0x05C4), Pad(0x09CC)
            until('attached', lambda: latest(0, (1, 0)) and latest(1, (1, 0)))
            hold(0)
            hold(1)
            # CrossはB/取消。未選択時はゲームへ流さず、採取値だけを検査する。
            until('both_held', lambda: latest(0, (1, 2080)) and latest(1, (1, 2080)))
            other_begin = len(samples(1))
            pads[0] = None
            gc.collect()
            until('removed_neutral', lambda: latest(0, (0, 0)))
            pads[0] = Pad(0x05C4)
            until('reattached_neutral', lambda: latest(0, (1, 0)))
            hold(0)
            until('reattached_input', lambda: latest(0, (1, 2080)))
            assert latest(1, (1, 2080))
            assert all(value == (1, 2080) for value in samples(1)[other_begin:])
            result['other_pad_input_preserved'] = True
            result['steps'].append({'name': 'other_pad_held_through_replug'})
            for pad in pads:
                pad.reset()
                pad.update()
            until('released_neutral', lambda: latest(0, (1, 0)) and latest(1, (1, 0)))
            print('後挿し・切断・再接続・相手入力維持: 成功。'+str(out), flush=True)
            result['runner_exit'] = process.wait(timeout=75)
            result['passed'] = result['runner_exit'] == 0
    except Exception as error:
        result['error'] = str(error)
    finally:
        if process is not None and process.poll() is None:
            subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                           capture_output=True, timeout=15)
        pads.clear()
        if 'pad' in locals():
            del pad
        gc.collect()
        (out/'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'logs': str(out), **result}, ensure_ascii=False), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
