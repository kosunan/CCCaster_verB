"""遅延・損失を注入せず、通常対戦の締切診断ログを一定時間採取する。"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time

from compare_rollback_pair import compare
from real_game_checkpoint import clean_environment, protected_hashes
from run_stage_rematch import free_match_port
from test_p2p_service import Service


def run(output, seconds, detailed=False, cpu_policy='pinned', fixed_stage=None):
    if cpu_policy not in ('pinned', 'unpinned'):
        raise ValueError('CPU配置はpinned/unpinnedのみ')
    root = Path(__file__).resolve().parents[3]
    runtime = root / 'test/runtime'
    output.mkdir(parents=True, exist_ok=False)
    protected = protected_hashes(runtime)
    binaries = {name: hashlib.sha256((root / 'build/bin' / name).read_bytes()).hexdigest()
                for name in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')}
    service = Service()
    threading.Thread(target=service.serve_forever, daemon=True).start()
    env = clean_environment()
    env.update(CCCASTER_NTFY_SERVER=f'http://127.0.0.1:{service.server_port}',
               CCCASTER_FRAME_TIMING_TRACE='1', CCCASTER_UPDATE_CADENCE='1',
               CCCASTER_PACE_TRACE='1', CCCASTER_MONITOR_PRESENT_TRACE='1')
    if detailed:
        env.update(CCCASTER_SPIN_PROBE='1', CCCASTER_RENDER_PROBE='1', CCCASTER_SOUND_PROBE='1')
    if cpu_policy == 'unpinned':
        env['CCCASTER_DISABLE_GAME_CPU_PIN'] = '1'
    if fixed_stage is not None:
        env['CCCASTER_TEST_FIXED_STAGE'] = str(fixed_stage)
    command = [shutil.which('pwsh'), '-NoProfile', '-File',
               str(root / 'src/src/harness/run_bounded_real_pair.ps1'),
               '-Seconds', str(seconds), '-Port', str(free_match_port()),
               '-UseConnectionCode', '-CloseSide', '1', '-TestRoot', str(runtime),
               '-OutputDirectory', str(output)]
    result = dict(passed=False, injected_network=None, time_scale=1,
                  cpu_policy=cpu_policy, fixed_stage=fixed_stage,
                  command=command, binaries=binaries,
                  environment={k: v for k, v in env.items() if k.startswith('CCCASTER_')})
    (output / 'protected_before.json').write_text(json.dumps(protected, indent=2), encoding='utf-8')
    started = time.monotonic()
    try:
        result['exit_code'] = subprocess.run(command, env=env).returncode
        result['sync'] = compare(output, require_rollback=False)
        result['passed'] = result['exit_code'] == 0 and result['sync']['passed']
    except Exception as exc:
        result['error'] = str(exc)
    finally:
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        result['protected_unchanged'] = protected == protected_hashes(runtime)
        result['protected_count'] = len(protected)
        result['binaries_unchanged'] = all(
            hashlib.sha256((root / 'build/bin' / name).read_bytes()).hexdigest() == digest
            for name, digest in binaries.items())
        result['passed'] &= result['protected_unchanged'] and result['binaries_unchanged']
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        service.running = False
        with service.cv:
            service.cv.notify_all()
        service.shutdown()
        service.server_close()
    print(json.dumps(dict(passed=result['passed'], logs=str(output)), ensure_ascii=False), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=75, choices=range(45, 121), metavar='45..120')
    parser.add_argument('--detailed', action='store_true', help='スピン・描画・音声の追加プローブも記録')
    parser.add_argument('--cpu-policy', choices=('pinned', 'unpinned'), default='pinned')
    parser.add_argument('--fixed-stage', type=int)
    args = parser.parse_args()
    raise SystemExit(run(args.output.resolve(), args.seconds, args.detailed, args.cpu_policy, args.fixed_stage))
