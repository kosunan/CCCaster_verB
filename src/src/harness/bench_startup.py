"""専用ゲームコピーの起動計測。既存プロセス・環境・配布物は変更しない。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import time


def readiness(content, mode):
    """表示・入力経路・選択モードを独立して確認する。"""
    expected = dict(target=1, mode=20, kind=4112, versus=0) if mode == 'training' else dict(
        target=0, mode=20, kind=1, versus=1)
    records = [dict(zip(('target', 'mode', 'kind', 'versus'), map(int, match)))
               for match in re.findall(r'\[StartupMode\]\s+target=(\d+)\s+mode=(\d+)\s+kind=(\d+)\s+versus=(\d+)', content)]
    present = bool(re.search(r'\[Startup\]\s+event=chara_present\b', content))
    input_ready = bool(re.search(r'\[Startup\]\s+event=chara_input\b', content))
    return {'charaPresent': present, 'charaInput': input_ready, 'expectedMode': expected,
            'observedModes': records, 'modeMatches': bool(records) and all(r == expected for r in records),
            'ready': present and input_ready and bool(records) and all(r == expected for r in records)}


def measure_idle_cpu(pid, seconds, out):
    """読取り専用。全論理CPUを100%とするプロセスCPU時間率。"""
    import psutil
    process = psutil.Process(pid)
    kernel = C.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    kernel.OpenProcess.restype = W.HANDLE
    kernel.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, W.LPVOID, C.c_size_t, W.LPVOID]
    kernel.CloseHandle.argtypes = [W.HANDLE]
    handle = kernel.OpenProcess(0x1010, False, pid)
    if not handle:
        raise C.WinError(C.get_last_error())
    def read(address):
        value = W.DWORD()
        if not kernel.ReadProcessMemory(handle, address, C.byref(value), 4, None):
            raise C.WinError(C.get_last_error())
        return value.value
    def sample():
        cpu = process.cpu_times()
        return dict(time=time.perf_counter(), user=cpu.user, kernel=cpu.system,
                    mode=read(0x54EEE8), frame=read(0x55D1D4),
                    rss=process.memory_info().rss,
                    threads={str(t.id): t.user_time+t.system_time for t in process.threads()})
    try:
        print(f'CPU待機測定 PID={pid}: 10秒安定化 → {seconds}秒測定', flush=True)
        time.sleep(10)
        rows = [sample()]
        for _ in range(seconds):
            time.sleep(1)
            rows.append(sample())
        first, last = rows[0], rows[-1]
        elapsed = last['time']-first['time']
        cpus = psutil.cpu_count()
        user, system = last['user']-first['user'], last['kernel']-first['kernel']
        result = dict(pid=pid, logicalCpus=cpus, elapsedSeconds=elapsed,
                      cpuPercent=(user+system)/elapsed/cpus*100,
                      userPercent=user/elapsed/cpus*100, kernelPercent=system/elapsed/cpus*100,
                      frameHz=((last['frame']-first['frame']) & 0xffffffff)/elapsed,
                      allCharacterSelect=all(r['mode']==20 for r in rows),
                      rows=rows)
        (out/'idle_cpu.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        if not result['allCharacterSelect']:
            raise RuntimeError('測定中にキャラセレを離れました')
        return result
    finally:
        kernel.CloseHandle(handle)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['training', 'versus'], required=True)
    parser.add_argument('--variant', choices=['baseline', 'fast'], required=True)
    parser.add_argument('--comparison', choices=['all', 'system-info', 'assets', 'first'], default='all',
                        help='他の高速化を維持し、system-info は情報収集省略、assets は素材キャッシュだけを比較する')
    parser.add_argument('--empty-cache', action='store_true',
                        help='試行出力内の未作成・各側独立キャッシュを指定する。既存キャッシュは変更しない')
    parser.add_argument('--label', required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--root', type=Path, required=True,
                        help='MBAACC_1 と MBAACC_2 を含む専用テストディレクトリ')
    parser.add_argument('--port', type=int, default=18940)
    parser.add_argument('--caster-dir', default='cccaster')
    parser.add_argument('--ready-hold-seconds', type=float, default=2,
                        help='キャラセレ到達後の保持秒数（画面確認用）')
    parser.add_argument('--idle-cpu-seconds', type=int, default=0,
                        help='Trainingキャラセレ到達後10秒安定化し、指定秒のCPU時間とモードを採取')
    parser.add_argument('--instances', type=int, choices=[1, 2],
                        help='未指定は training=1、versus=2。versus は2のみ')
    args = parser.parse_args()
    if not 2 <= args.ready_hold_seconds <= 120:
        parser.error('ready-hold-secondsは2..120')
    if args.idle_cpu_seconds and (args.mode != 'training' or args.idle_cpu_seconds < 1):
        parser.error('CPU待機測定はTrainingかつ正の秒数のみ')
    args.instances = args.instances if args.instances is not None else (1 if args.mode == 'training' else 2)
    if args.idle_cpu_seconds and args.instances != 1:
        parser.error('CPU待機測定は他窓の干渉を避けるため --instances 1 のみ')
    if args.mode == 'versus' and args.instances != 2:
        parser.error('versus の比較には --instances 2 が必要です')
    if os.name != 'nt':
        parser.error('Windows 専用です')
    if not 1024 <= args.port <= 65534:
        parser.error('port は 1024..65534')
    kernel = C.WinDLL('kernel32', use_last_error=True)
    class Entry(C.Structure):
        _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD),
                    ('heap', C.c_size_t), ('module', W.DWORD), ('threads', W.DWORD),
                    ('parent', W.DWORD), ('priority', W.LONG), ('flags', W.DWORD),
                    ('exe', W.WCHAR * 260)]
    def bind(name, restype, *argtypes):
        fn = getattr(kernel, name)
        fn.restype, fn.argtypes = restype, argtypes
        return fn
    snapshot = bind('CreateToolhelp32Snapshot', W.HANDLE, W.DWORD, W.DWORD)
    first = bind('Process32FirstW', W.BOOL, W.HANDLE, C.POINTER(Entry))
    next_entry = bind('Process32NextW', W.BOOL, W.HANDLE, C.POINTER(Entry))
    close = bind('CloseHandle', W.BOOL, W.HANDLE)
    open_process = bind('OpenProcess', W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
    query = bind('QueryFullProcessImageNameW', W.BOOL, W.HANDLE, W.DWORD,
                 W.LPWSTR, C.POINTER(W.DWORD))
    wait = bind('WaitForSingleObject', W.DWORD, W.HANDLE, W.DWORD)
    terminate = bind('TerminateProcess', W.BOOL, W.HANDLE, W.UINT)
    exit_code = bind('GetExitCodeProcess', W.BOOL, W.HANDLE, C.POINTER(W.DWORD))
    event = bind('CreateEventW', W.HANDLE, W.LPVOID, W.BOOL, W.BOOL, W.LPCWSTR)
    set_event = bind('SetEvent', W.BOOL, W.HANDLE)
    counter = bind('QueryPerformanceCounter', W.BOOL, C.POINTER(C.c_longlong))
    frequency = bind('QueryPerformanceFrequency', W.BOOL, C.POINTER(C.c_longlong))
    freq = C.c_longlong()
    if not frequency(C.byref(freq)):
        raise C.WinError(C.get_last_error())
    def qpc_us():
        value = C.c_longlong()
        counter(C.byref(value))
        return value.value * 1000000 // freq.value
    def processes():
        handle = snapshot(2, 0)
        if handle == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        try:
            row = Entry()
            row.size = C.sizeof(row)
            ok = first(handle, C.byref(row))
            while ok:
                yield row.pid, row.parent, row.exe
                ok = next_entry(handle, C.byref(row))
        finally:
            close(handle)
    def image_path(handle):
        text = C.create_unicode_buffer(32768)
        size = W.DWORD(len(text))
        if not query(handle, 0, text, C.byref(size)):
            raise C.WinError(C.get_last_error())
        return Path(text.value).resolve()
    root, out = args.root.resolve(), args.out.resolve()
    sides = [root / f'MBAACC_{side}' for side in range(1, args.instances + 1)]
    exe_name = 'CCCaster_B.exe'
    if args.mode == 'training':
        exe_name = 'CCCaster_startup_worker.exe'
        if not all((side / args.caster_dir / exe_name).is_file() for side in sides):
            exe_name = 'CCCaster_B_GUI.exe'
    game_paths = {(side / 'MBAA.exe').resolve() for side in sides}
    if len(game_paths) != args.instances:
        raise RuntimeError('指定数の独立したゲームコピーが必要です')
    watched_paths = game_paths | {(side / args.caster_dir / exe_name).resolve() for side in sides}
    for side in sides:
        if not side.resolve().is_relative_to(root):
            raise RuntimeError('テストコピーが root 外を参照しています')
        for path in [side / 'MBAA.exe', side / args.caster_dir / exe_name,
                     side / args.caster_dir / 'libcccaster_hook.dll']:
            if not path.is_file() or not path.resolve().is_relative_to(root):
                raise RuntimeError(f'専用コピー内の必要ファイルを確認してください: {path}')
    for pid, _, name in processes():
        if name.lower() not in {'mbaa.exe', exe_name.lower()}:
            continue
        handle = open_process(0x1000, False, pid)
        if not handle:
            raise RuntimeError(f'既存対象名プロセスを確認できません: {pid}')
        try:
            if image_path(handle) in watched_paths:
                raise RuntimeError(f'対象コピーは既に使用中です。停止しません: {pid}')
        finally:
            close(handle)
    out.mkdir(parents=True, exist_ok=False)
    logs = [side / args.caster_dir / 'cccaster_hook_log.txt' for side in sides]
    for index, log in enumerate(logs, 1):
        if log.exists():
            shutil.move(str(log), str(out / f'before_{index}.log'))
    env = os.environ.copy()
    env['CCCASTER_STARTUP_TRACE'] = '1'
    env.pop('CCCASTER_STARTUP_BASELINE', None)
    env.pop('CCCASTER_STARTUP_SECONDS_BASELINE', None)
    env.pop('CCCASTER_STARTUP_ASSETS_BASELINE', None)
    env.pop('CCCASTER_STARTUP_FIRST_BASELINE', None)
    if args.variant == 'baseline':
        env[{'system-info': 'CCCASTER_STARTUP_SECONDS_BASELINE',
             'assets': 'CCCASTER_STARTUP_ASSETS_BASELINE',
             'first': 'CCCASTER_STARTUP_FIRST_BASELINE',
             'all': 'CCCASTER_STARTUP_BASELINE'}[args.comparison]] = '1'
    binaries = [{'side': i, 'files': {
        str(p): hashlib.sha256(p.read_bytes()).hexdigest()
        for p in (side / 'MBAA.exe', side / args.caster_dir / exe_name,
                  side / args.caster_dir / 'libcccaster_hook.dll')}}
        for i, side in enumerate(sides, 1)]
    workers, events, files, games = [], [], [], {}
    starts, ready_at = [], [None] * args.instances
    game_exits = {}
    idle_cpu = None
    status = 'timeout'
    error = None
    def discover():
        parents = {p.pid for p in workers}
        for pid, parent, name in processes():
            # PIDは再利用される。起動前の無関係なPID一覧で除外しない。
            # 既存の対象パスは起動前検査で拒否済み。自分のworkerの子＋実パスで限定する。
            if parent not in parents or pid in games or name.lower() != 'mbaa.exe':
                continue
            handle = open_process(0x1000 | 0x100000 | 1, False, pid)
            if not handle:
                continue
            try:
                if image_path(handle) in game_paths:
                    games[pid] = handle
                    handle = None
            finally:
                if handle:
                    close(handle)
    try:
        deadline = time.monotonic() + 30 + args.ready_hold_seconds
        for index, side in enumerate(sides, 1):
            launcher_log = out / f'launcher_{index}.log'
            if args.mode == 'training':
                name = f'Local\\CCCasterStartup_{os.getpid()}_{index}'
                cancel = event(None, True, False, name)
                if not cancel:
                    raise C.WinError(C.get_last_error())
                events.append(cancel)
                command = [str(side / args.caster_dir / exe_name), '--worker', name,
                           str(launcher_log), 'training', '0']
                stdout = subprocess.DEVNULL
            else:
                command = [str(side / args.caster_dir / exe_name), '--headless']
                command += ['--host'] if index == 1 else ['--ip', '127.0.0.1']
                command += ['--port', str(args.port)]
                stdout = launcher_log.open('wb')
                files.append(stdout)
            stderr = (out / f'launcher_{index}.err').open('wb')
            files.append(stderr)
            starts.append(qpc_us())
            side_env = env.copy()
            if args.empty_cache:
                cache = out / f'cache_{index}'
                if cache.exists():
                    raise RuntimeError('初回測定のキャッシュは未作成でなければなりません')
                side_env['CCCASTER_STARTUP_CACHE_DIR'] = str(cache)
            workers.append(subprocess.Popen(command, cwd=side / args.caster_dir, env=side_env,
                                            stdout=stdout, stderr=stderr,
                                            creationflags=subprocess.CREATE_NO_WINDOW))
        while time.monotonic() < deadline:
            discover()
            for pid, handle in games.items():
                if wait(handle, 0) == 0:
                    code = W.DWORD()
                    game_exits[pid] = code.value if exit_code(handle, C.byref(code)) else None
            if game_exits:
                status = 'game_failed' if any(code != 0 for code in game_exits.values()) else 'game_exited'
                break
            if any(p.poll() is not None for p in workers):
                status = 'worker_exited'
                break
            for index, log in enumerate(logs):
                if ready_at[index] is None and log.exists():
                    content = log.read_text(encoding='utf-8', errors='replace')
                    if readiness(content, args.mode)['ready']:
                        ready_at[index] = time.monotonic()
            if len(games) == args.instances and all(t is not None for t in ready_at) and time.monotonic() >= max(ready_at) + args.ready_hold_seconds:
                if args.idle_cpu_seconds:
                    idle_cpu = measure_idle_cpu(next(iter(games)), args.idle_cpu_seconds, out)
                status = 'ready'
                break
            time.sleep(0.05)
    except BaseException as exc:
        error = repr(exc)
        status = 'error'
    finally:
        # 生成元を止めてから子を再走査。既存 PID と無関係なプロセスは対象外。
        cleanup_errors = []
        for cancel in events:
            set_event(cancel)
        for proc in workers:
            try:
                if proc.poll() is None:
                    proc.terminate()
                proc.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired) as exc:
                cleanup_errors.append(f'worker {proc.pid}: {exc}')
        try:
            discover()
        except OSError as exc:
            cleanup_errors.append(f'子プロセスの最終確認: {exc}')
        game_results = []
        for pid, handle in games.items():
            stopped_by_harness = False
            if wait(handle, 0) == 258:
                stopped_by_harness = True
                if not terminate(handle, 0) or wait(handle, 2000) == 258:
                    cleanup_errors.append('テストゲームの終了を確認できません')
            code = W.DWORD()
            got_code = bool(exit_code(handle, C.byref(code)))
            game_results.append({'pid': pid, 'stoppedByHarness': stopped_by_harness,
                                 'exitCode': code.value if got_code else None})
            if not stopped_by_harness:
                game_exits[pid] = code.value if got_code else None
            close(handle)
        if game_exits:
            status = 'game_failed' if any(code != 0 for code in game_exits.values()) else 'game_exited'
        for cancel in events:
            close(cancel)
        for stream in files:
            stream.close()
        samples = []
        for index, log in enumerate(logs, 1):
            if log.exists():
                shutil.copy2(log, out / f'game_{index}.log')
            found = []
            for path in [out / f'game_{index}.log', out / f'launcher_{index}.err',
                         out / f'launcher_{index}.log']:
                if path.exists():
                    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
                        match = re.search(r'\[Startup\]\s+event=(\S+).*?\bqpcUs=(\d+)', line)
                        if match:
                            stamp = int(match[2])
                            found.append({'event': match[1], 'qpcUs': stamp,
                                          'fromLaunchUs': stamp - starts[index - 1] if index <= len(starts) else None,
                                          'source': path.name, 'line': line})
            content = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
            samples.append({'side': index, 'events': found, 'validation': readiness(content, args.mode)})
        if status == 'ready' and not all(sample['validation']['ready'] for sample in samples):
            status = 'validation_failed'
        result = {'label': args.label, 'mode': args.mode, 'variant': args.variant,
                  'comparison': args.comparison, 'binaries': binaries, 'idleCpu': idle_cpu,
                  'casterDir': args.caster_dir,
                  'readyHoldSeconds': args.ready_hold_seconds,
                  'diagnosticEnvironment': {key: value for key, value in env.items()
                                            if key.startswith('CCCASTER_')},
                  'emptyCache': args.empty_cache,
                  'instances': args.instances,
                  'status': status, 'error': error, 'root': str(root), 'samples': samples,
                  'cleanupErrors': cleanup_errors,
                  'launchQpcUs': starts, 'workerPids': [p.pid for p in workers],
                  'gamePids': list(games), 'network': env.get('CCCASTER_TEST_NETWORK'),
                  'gameProcesses': game_results,
                  'note': '表示・通常入力経路・モード照合後2秒を維持。idleCpu有効時はさらに10秒安定化後に採取。CPU率は全論理CPUを100%とするCPU時間率。物理表示・手入力応答の確認ではありません。'}
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if status == 'ready' and not cleanup_errors else 1


if __name__ == '__main__':
    raise SystemExit(main())
