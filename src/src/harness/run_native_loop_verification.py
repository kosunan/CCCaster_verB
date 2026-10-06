"""ネイティブループの候補到達確認／役割を交代できる通常経路の時間比較。"""
import argparse
import csv
import datetime
import hashlib
import json
import re
import random
import shutil
import socket
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

from test_p2p_service import ROOT, Service
from real_game_checkpoint import clean_environment, evaluate, protected_hashes


def free_match_port():
    # Windowsの動的割当範囲と予約TCP範囲の重複を避け、連続2ポートを確認。
    for port in random.SystemRandom().sample(range(18000, 40000), 200):
        sockets = []
        try:
            for value in (port, port + 1):
                for kind in (socket.SOCK_DGRAM, socket.SOCK_STREAM):
                    sock = socket.socket(socket.AF_INET, kind)
                    sockets.append(sock)
                    sock.bind(('0.0.0.0', value))
            return port
        except OSError:
            pass
        finally:
            for sock in sockets:
                sock.close()
    raise RuntimeError('対戦UDP・観戦TCPの連続2ポートを確保できません')


def read_samples(text):
    values = {}
    for line in text.splitlines():
        match = re.search(r'\[Pace\] f=(\d+).*? work=(\d+) play=1', line)
        if match:
            frame, work = map(int, match.groups())
            # 初回ロード・イントロ・時計立上げを除く。範囲は両側で共通にする。
            if frame // 65536 >= 2 and frame % 65536 >= 480 and work > 0:
                values[frame] = work
    return values


def summarize(texts, masks, coverage):
    result = dict(masks=masks)
    installed = [re.findall(r'\[NativeLoopMask\] requested=(\d+) applied=(\d+)', t) for t in texts]
    result['installed'] = installed
    result['installation_passed'] = all(rows == [(str(mask), str(mask))] for rows, mask in zip(installed, masks))
    if coverage:
        rows = [re.findall(r'\[NativeVerify\] sample=(\d+) entries=(\d+) players=(\d+) objects=(\d+) playerHits=(\d+) objectHits=(\d+) live=(\d+) maxLive=(\d+)', t) for t in texts]
        result['coverage'] = [dict(zip(('sample', 'entries', 'players', 'objects', 'player_hits', 'object_hits', 'live', 'max_live'), map(int, r[-1]))) if r else {} for r in rows]
        result['candidate_coverage_passed'] = all(r.get('objects', 0) > 0 and r.get('object_hits', 0) > 0 for r in result['coverage'])
        result['passed'] = result['installation_passed'] and result['candidate_coverage_passed']
        return result, []
    samples = [read_samples(t) for t in texts]
    frames = sorted(samples[0].keys() & samples[1].keys())
    rows = [(f, samples[0][f], samples[1][f]) for f in frames]
    result['common_samples'] = len(frames)
    result['work_us'] = [dict(median=statistics.median(v), mean=statistics.mean(v), p95=sorted(v)[int((len(v)-1)*.95)]) if v else {} for v in [[row[s+1] for row in rows] for s in (0, 1)]]
    if rows:
        result['median_side2_minus_side1_us'] = statistics.median(b-a for _, a, b in rows)
    result['passed'] = result['installation_passed'] and len(rows) >= 500
    return result, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-root', type=Path, required=True)
    parser.add_argument('--coverage', action='store_true')
    parser.add_argument('--projectiles', action='store_true')
    parser.add_argument('--mask1', type=int, choices=range(32), default=0)
    parser.add_argument('--mask2', type=int, choices=range(32), default=31)
    parser.add_argument('--seconds', type=int, default=55)
    parser.add_argument('--build-manifest', type=Path, help='配置直後のbuild/binの3点のSHA-256。反復中に同一ビルドを保つ')
    parser.add_argument('--cpu1', type=int, choices=range(32))
    parser.add_argument('--cpu2', type=int, choices=range(32))
    parser.add_argument('--combat-stress', type=int, choices=(1, 2))
    args = parser.parse_args()
    output = ROOT / 'test/logs' / ('native_verify_' + datetime.datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    output.mkdir()
    runtime = args.test_root.resolve()
    before = protected_hashes(runtime)
    (output / 'protected_before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    service = Service()
    threading.Thread(target=service.serve_forever, daemon=True).start()
    env = clean_environment()
    env.update(CCCASTER_NTFY_SERVER=f'http://127.0.0.1:{service.server_port}', CCCASTER_PACE_TRACE='1',
               CCCASTER_TEST_FIXED_STAGE='59', CCCASTER_TEST_NATIVE_MASK_1=str(args.mask1), CCCASTER_TEST_NATIVE_MASK_2=str(args.mask2))
    if args.coverage:
        env['CCCASTER_NATIVE_LOOP_VERIFY'] = '1'
    if args.projectiles:
        env['CCCASTER_TEST_PROJECTILE_INPUT'] = '1'
    if args.combat_stress:
        env['CCCASTER_COMBAT_STRESS'] = str(args.combat_stress)
    for side, cpu in ((1, args.cpu1), (2, args.cpu2)):
        if cpu is not None:
            env[f'CCCASTER_TEST_CPU_PIN_{side}'] = str(cpu)
    command = [shutil.which('pwsh'), '-NoProfile', '-File', str(ROOT / 'src/src/harness/run_bounded_real_pair.ps1'),
               '-Seconds', str(args.seconds), '-Port', str(free_match_port()), '-Network', '15,25,5',
               '-UseConnectionCode', '-CloseSide', '1', '-OutputDirectory', str(output), '-TestRoot', str(runtime)]
    if args.build_manifest:
        command += ['-BuildManifest', str(args.build_manifest.resolve())]
    # 比較測定は同じ長さで採る。通常同期判定の1000F閾値は変更しない。
    started = time.monotonic()
    result = dict(passed=False, command=command, environment={k:v for k,v in env.items() if k.startswith('CCCASTER_')})
    result['binaries'] = {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in (runtime / 'MBAACC_1/cccaster_B').iterdir() if f.name in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')}
    try:
        run = subprocess.run(command, env=env)
        result['exit_code'] = run.returncode
        result.update(evaluate(output, {}))
        texts = [(output / f'game_{side}.log').read_text(encoding='utf-8') for side in (1, 2)]
        analysis, rows = summarize(texts, [args.mask1, args.mask2], args.coverage)
        analysis['cpu_pins'] = [re.findall(r'\[GameCpuPin\].*? cpu=(-?\d+).*? enabled=(\d+)', t) for t in texts]
        for side, cpu in enumerate((args.cpu1, args.cpu2)):
            if cpu is not None:
                analysis['passed'] &= analysis['cpu_pins'][side] == [(str(cpu), '1')]
        result['native'] = analysis
        result['passed'] &= analysis['passed'] and run.returncode == 0
        if rows:
            with (output / 'work.csv').open('w', newline='', encoding='utf-8') as stream:
                writer = csv.writer(stream)
                writer.writerow(('frame', 'side1_work_us', 'side2_work_us'))
                writer.writerows(rows)
    except Exception as exc:
        result.update(passed=False, error=str(exc))
    finally:
        result['protected_unchanged'] = before == protected_hashes(runtime)
        result['protected_count'] = len(before)
        result['passed'] &= result['protected_unchanged']
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        service.running = False
        with service.cv:
            service.cv.notify_all()
        service.shutdown()
        service.server_close()
    print(json.dumps(dict(passed=result['passed'], logs=str(output), native=result.get('native')), ensure_ascii=False), flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
