"""高速／実ゲーム回帰チェックを一括実行し、失敗を終了コードとJSONに残す。"""
import argparse
import hashlib
import json
import os
import socket
import shutil
import struct
import subprocess
import sys
import time
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from regression_checks import resumed_frames, baseline_failures
from analyze_update_cadence import analyze_text as analyze_cadence

ROOT = Path(__file__).resolve().parents[3]
HARNESS = ROOT / 'src/src/harness'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def available_port():
    # 子プロセスへの引渡しまでの競合は完全には排除できない。bind失敗は試験失敗となる。
    for port in range(17900, 18000, 2):
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as a, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as b:
            try:
                a.bind(('0.0.0.0', port))
                b.bind(('0.0.0.0', port+1))
                return port
            except OSError:
                pass
    raise RuntimeError('試験用UDPポートが空いていない')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=['quick', 'real', 'full'], default='real')
    parser.add_argument('--repeat', type=int, default=1)
    parser.add_argument('--case', action='append', choices=['combat', 'stress', 'gauge', 'gauge_control', 'driver_locks', 'retry_0', 'retry_1', 'retry_2'],
                        help='指定した実ゲームケースだけ再実行。複数指定可能')
    parser.add_argument('--baseline', type=Path, help='同条件で成功したsummary.json。実ゲーム再計算p99を比較')
    parser.add_argument('--max-slowdown', type=float, default=1.5)
    parser.add_argument('--gauge-max-work-us', type=float, help='ゲージ試験の更新・描画時間上限。根治の判定用')
    parser.add_argument('--spike-states', action='store_true', help='診断専用: 全スパイクで既存リングの直前10Fを書出す。通常性能とは別条件')
    parser.add_argument('--spin-probe', action='store_true', help='診断専用: スピン内・後処理の内訳を追加')
    parser.add_argument('--inline-deadline-trace', action='store_true', help='比較専用: 締切後WAITログを旧位置で整形')
    args = parser.parse_args()
    if args.profile == 'quick' and (args.spike_states or args.spin_probe or args.inline_deadline_trace):
        parser.error('診断オプションにはreal/fullが必要')
    if args.spin_probe and args.inline_deadline_trace:
        parser.error('SpinProbeはWAITログを抑止するため旧WAIT整形との同時比較は不可')
    if args.repeat < 1 or args.max_slowdown < 1:
        parser.error('repeat/slowdown は1以上')
    if args.gauge_max_work_us is not None and (not 0 < args.gauge_max_work_us < float('inf')):
        parser.error('gauge-max-work-us は有限の正数')
    if args.case and (args.profile == 'quick' or
                     (args.profile == 'real' and any(c.startswith('retry') for c in args.case))):
        parser.error('--case は選んだprofileに含まれるケースを指定する')
    out = ROOT / 'test/logs' / ('regression_' + datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    out.mkdir(parents=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
    env['PATH'] = 'C:/msys64/mingw32/bin;C:/msys64/usr/bin;' + env.get('PATH', '')
    env['PYTHONUTF8'] = '1'
    # pwshからPython経由でWindows PowerShellを起動する場合のモジュール混在を防ぐ。
    env['PSModulePath'] = os.path.join(env.get('SystemRoot', 'C:/Windows'),
                                     'System32/WindowsPowerShell/v1.0/Modules')
    result = dict(schema=1, profile=args.profile, selected_cases=args.case, repeat=args.repeat, started_utc=datetime.now(timezone.utc).isoformat(),
                  source={}, binaries={}, steps=[], scenarios=[], passed=False)
    result['cadence_failures'] = []
    result['performance_scope'] = '実ゲーム自動入力・同期ログあり。通常時性能の認定ではない'
    result['cadence_checked'] = False
    started = time.monotonic()
    summary = out / 'summary.json'

    def save():
        result['elapsed_seconds'] = round(time.monotonic()-started, 3)
        summary.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')

    def run(name, command, extra=None, timeout=600):
        print(f'[{name}] 開始', flush=True)
        begin = time.monotonic()
        path = out / (name + '.log')
        with path.open('w', encoding='utf-8') as log:
            process = subprocess.Popen(list(map(str, command)), cwd=ROOT, env=dict(env, **(extra or {})),
                                       stdout=log, stderr=subprocess.STDOUT)
            try:
                code = process.wait(timeout=timeout)
            except (subprocess.TimeoutExpired, KeyboardInterrupt):
                # subprocess.runの親だけのkillではゲームが孤立する。今回の子ツリーだけを終了。
                if process.poll() is None:
                    subprocess.run(['taskkill', '/PID', str(process.pid), '/T', '/F'],
                                   stdout=log, stderr=subprocess.STDOUT, timeout=15)
                    process.wait(timeout=15)
                raise
        result['steps'].append(dict(name=name, command=list(map(str, command)), exit=code,
                                    seconds=round(time.monotonic()-begin, 3), log=str(path)))
        save()
        if code:
            raise RuntimeError(f'{name} 失敗: {path}')
        print(f'[{name}] 成功 ({time.monotonic()-begin:.1f}秒)', flush=True)

    def ps(script, *options):
        return ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', HARNESS/script, *options]

    try:
        if os.name != 'nt':
            raise RuntimeError('この入口はWindows 32bit実装用。Linux harnessとは別条件')
        files = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
        result['source'] = {p: sha(ROOT/p) for p in sorted(set(files)) if p and (ROOT/p).is_file()
                            and '__pycache__' not in Path(p).parts
                            and (p.startswith(('src/', 'server/')) or p.endswith('CMakeLists.txt')
                                 or p == 'InputInjector.hpp')}
        head = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=ROOT, text=True, capture_output=True)
        result['head'] = head.stdout.strip() if head.returncode == 0 else None
        run('build', ['cmake', '--build', 'build', '-j8'])
        artifacts = out/'artifacts'
        artifacts.mkdir()
        for name in ('CCCaster_B.exe', 'libcccaster_hook.dll', 'harness.exe'):
            binary = ROOT/'build/bin'/name
            data = binary.read_bytes()
            pe = struct.unpack_from('<I', data, 0x3c)[0]
            if data[pe:pe+4] != b'PE\0\0' or struct.unpack_from('<H', data, pe+4)[0] != 0x14c:
                raise RuntimeError(f'32bit PEではない: {binary}')
            result['binaries'][name] = sha(binary)
            shutil.copy2(binary, artifacts/name)
            if sha(artifacts/name) != result['binaries'][name]:
                raise RuntimeError('採取用バイナリの保全不一致')
        if args.spike_states:
            # 将来のレイアウト照合・逆アセンブルに、hashだけでなく生成物と元ソースも残す。
            with zipfile.ZipFile(artifacts/'sources.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
                for path, digest in result['source'].items():
                    data = (ROOT/path).read_bytes()
                    if hashlib.sha256(data).hexdigest() != digest:
                        raise RuntimeError('ビルド中にソースが変化した: ' + path)
                    archive.writestr(path, data)
        run('ctest', ['ctest', '--test-dir', 'build', '--output-on-failure', '--no-tests=error', '--timeout', '30'])
        run('python_tests', [sys.executable, '-m', 'unittest', 'discover', '-s', HARNESS, '-p', 'test_*.py'])
        for iteration in range(args.repeat):
            name = f'harness_{iteration}'
            run(name, ps('run_bounded_pair.ps1', '-Scale', '4', '-Rounds', '1', '-Port', available_port(),
                         '-Name', f'{out.name}/{name}'), timeout=180)
        if args.profile != 'quick':
            cases = [('combat', 45, '15,25,5', {}, None),
                     ('stress', 45, '60,96,5', {'CCCASTER_COMBAT_STRESS': '2'}, None),
                     ('gauge',45,'',{'CCCASTER_GAUGE_STRESS':'1','CCCASTER_PACE_TRACE':'1'},None),
                     ('gauge_control',45,'',{'CCCASTER_GAUGE_STRESS':'2','CCCASTER_PACE_TRACE':'1'},None)]
            if args.case and 'driver_locks' in args.case:
                cases.append(('driver_locks',45,'',{'CCCASTER_GAUGE_STRESS':'1',
                    'CCCASTER_PACE_TRACE':'1','CCCASTER_RENDER_PROBE':'1','CCCASTER_DRIVER_LOCK_PROBE':'1'},None))
            if args.profile == 'full':
                cases += [(f'retry_{s}', 85, '60,96,5', {'CCCASTER_TEST_NATIVE_RETRY': '1',
                           'CCCASTER_TEST_RETRY_QUICK': '1', 'CCCASTER_TEST_REMATCH': str(s),
                           'CCCASTER_TEST_RANDOM_STAGE': '1', 'CCCASTER_PACE_TRACE': '1'}, s) for s in range(3)]
            if args.case:
                cases = [case for case in cases if case[0] in args.case]
            baseline = json.loads(args.baseline.read_text(encoding='utf-8')) if args.baseline else None
            if baseline and (not baseline.get('passed') or baseline.get('profile') != args.profile):
                raise RuntimeError('基準は同じprofileで合格したsummary.jsonを指定する')
            for iteration in range(args.repeat):
                for case, seconds, network, extra, retry in cases:
                    name = f'{case}_{iteration}'
                    folder = out/name
                    extra = dict(extra, CCCASTER_UPDATE_CADENCE='1')
                    if args.spin_probe:
                        extra['CCCASTER_SPIN_PROBE'] = '1'
                    if args.inline_deadline_trace:
                        extra['CCCASTER_INLINE_DEADLINE_TRACE'] = '1'
                    if args.spike_states:
                        (folder/'states').mkdir(parents=True)
                        extra['CCCASTER_SPIKE_STATE_DIR'] = str(folder/'states')
                    ini = {p: sha(p) for p in (ROOT/'_TEST_MBAACC').rglob('*.ini')}
                    run(name, ps('run_bounded_real_pair.ps1', '-Seconds', seconds, '-Port', available_port(),
                                 '-Network', network, '-OutputDirectory', folder), extra, timeout=seconds+90)
                    after = {p: sha(p) for p in (ROOT/'_TEST_MBAACC').rglob('*.ini')}
                    if not ini or ini != after:
                        raise RuntimeError(f'{name}: INIの追加・削除・変更を検出')
                    run(name+'_compare', [sys.executable, HARNESS/'compare_rollback_pair.py', folder]+
                        ([] if network else ['--allow-no-rollback']))
                    comparison = json.loads((folder/'rollback_comparison.json').read_text(encoding='utf-8'))
                    logs = [(folder/f'game_{s}.log').read_text(encoding='utf-8') for s in (1, 2)]
                    if any('[Clock] WASAPI active' not in log or 'QPC fallback' in log for log in logs):
                        raise RuntimeError(f'{name}: WASAPI未使用またはQPC切替を検出')
                    if case == 'stress' and any('[CombatStressInstall] mode=2' not in log or '[CombatStress] ' not in log for log in logs):
                        raise RuntimeError('負荷チート未適用')
                    if case.startswith('gauge'):
                        run(name+'_gauge',[sys.executable,HARNESS/'analyze_gauge_stress.py',folder,'--check']+
                            (['--control'] if case=='gauge_control' else [])+
                            (['--maximum-work-us',str(args.gauge_max_work_us)] if args.gauge_max_work_us else []))
                    if case=='driver_locks':
                        run(name+'_locks',[sys.executable,HARNESS/'analyze_driver_locks.py',folder,'--check'])
                    if retry is not None:
                        run(name+'_score', [sys.executable, HARNESS/'verify_session_score_pair.py', folder])
                        run(name+'_retry', [sys.executable, HARNESS/'verify_native_retry_pair.py', folder, retry])
                        run(name+'_selection', [sys.executable, HARNESS/'verify_selection_pair.py', folder, '--random-stage'])
                        retry_result = json.loads((folder/'native_retry_comparison.json').read_text(encoding='utf-8'))
                        if resumed_frames(comparison, retry_result) < 120:
                            raise RuntimeError('再戦後の戦闘確認が不足')
                    scenario = dict(name=name, case=case, seconds=seconds, network=network, cheats=extra,
                                    ini_unchanged=True, comparison=comparison, logs=str(folder))
                    result['scenarios'].append(scenario)
                    cadence = {str(s): analyze_cadence(logs[s-1], evidence_dir=folder/'states') for s in (1, 2)}
                    (folder/'update_cadence.json').write_text(json.dumps(cadence, ensure_ascii=False, indent=2), encoding='utf-8')
                    scenario['update_cadence'] = {s: {k: v for k, v in r.items() if k not in ('spikes', 'evidence')} for s, r in cadence.items()}
                    result['cadence_checked'] = True
                    if not all(r['passed'] for r in cadence.values()):
                        result['cadence_failures'].append(name)
                    if baseline:
                        matches = [s for s in baseline['scenarios'] if s['name'] == name]
                        if len(matches) != 1:
                            raise RuntimeError(f'{name}: 基準の試験条件が不一致')
                        failures = baseline_failures(scenario, matches[0], args.max_slowdown)
                        if failures:
                            raise RuntimeError(f'{name}: {failures}')
                    save()
        if result['cadence_failures']:
            raise RuntimeError('通常更新周期が未達: ' + ', '.join(result['cadence_failures']))
        result['passed'] = True
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as exc:
        result['error'] = str(exc)
        print(str(exc), file=sys.stderr, flush=True)
    finally:
        save()
        print(f'結果: {summary}', flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
