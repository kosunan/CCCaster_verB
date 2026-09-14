"""独立コピー上で旧CCCaster/v10を同じ外部観測器で計測する（Windows）。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import mmap
import os
from pathlib import Path
import statistics
import struct
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]
K = C.WinDLL('kernel32', use_last_error=True)
U = C.WinDLL('user32', use_last_error=True)
U.GetForegroundWindow.restype = W.HWND
U.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]


def bind(name, result, *args):
    fn = getattr(K, name)
    fn.restype, fn.argtypes = result, args
    return fn


class Entry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD),
                ('heap', C.c_size_t), ('module', W.DWORD), ('threads', W.DWORD),
                ('parent', W.DWORD), ('priority', W.LONG), ('flags', W.DWORD),
                ('exe', W.WCHAR * 260)]


snap = bind('CreateToolhelp32Snapshot', W.HANDLE, W.DWORD, W.DWORD)
first = bind('Process32FirstW', W.BOOL, W.HANDLE, C.POINTER(Entry))
next_entry = bind('Process32NextW', W.BOOL, W.HANDLE, C.POINTER(Entry))
close = bind('CloseHandle', W.BOOL, W.HANDLE)
open_process = bind('OpenProcess', W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
query = bind('QueryFullProcessImageNameW', W.BOOL, W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD))
rpm = bind('ReadProcessMemory', W.BOOL, W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t))
terminate = bind('TerminateProcess', W.BOOL, W.HANDLE, W.UINT)
wait = bind('WaitForSingleObject', W.DWORD, W.HANDLE, W.DWORD)
qpc = bind('QueryPerformanceCounter', W.BOOL, C.POINTER(C.c_longlong))
qpf = bind('QueryPerformanceFrequency', W.BOOL, C.POINTER(C.c_longlong))
frequency = C.c_longlong()
qpf(C.byref(frequency))


def ticks():
    t = C.c_longlong()
    qpc(C.byref(t))
    return t.value


def processes():
    handle = snap(2, 0)
    if handle == C.c_void_p(-1).value:
        raise C.WinError(C.get_last_error())
    row = Entry()
    row.size = C.sizeof(row)
    try:
        ok = first(handle, C.byref(row))
        while ok:
            yield row.pid, row.parent, row.exe
            ok = next_entry(handle, C.byref(row))
    finally:
        close(handle)


def path_of(handle):
    value = C.create_unicode_buffer(32768)
    size = W.DWORD(len(value))
    if not query(handle, 0, value, C.byref(size)):
        raise C.WinError(C.get_last_error())
    return Path(value.value).resolve()


def read32(handle, address):
    result = C.c_uint32()
    count = C.c_size_t()
    if not rpm(handle, address, C.byref(result), 4, C.byref(count)) or count.value != 4:
        raise C.WinError(C.get_last_error())
    return result.value


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def dist(values):
    if not values:
        return None
    values = sorted(values)
    def percentile(p):
        position = (len(values) - 1) * p
        lower = int(position)
        upper = min(lower + 1, len(values) - 1)
        return values[lower] + (values[upper] - values[lower]) * (position - lower)
    return dict(count=len(values), min=values[0], median=statistics.median(values),
                mean=statistics.mean(values), p95=percentile(.95), p99=percentile(.99), max=values[-1])


RECORD = struct.Struct('<qq8I')
HEADER = struct.Struct('<4Iq2I')
MAP_SIZE = 4096 + 262144 * RECORD.size


def select_training(handle, pad, vg):
    """比較コピーの通常DS4入力でシオン/C/01対Vシオン/C/01、stage50を選ぶ。"""
    events = []
    def state():
        return dict(mode=read32(handle, 0x54eee8), p1=read32(handle, 0x74d8ec),
                    p2=read32(handle, 0x74d910), stage=read32(handle, 0x74fd98))
    def pulse(up=False):
        pad.reset()
        if up:
            pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH)
        else:
            pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
        pad.update(); time.sleep(.05)
        pad.reset(); pad.update(); time.sleep(.4)
        events.append(state())
    deadline = time.monotonic()+35
    while time.monotonic() < deadline:
        s = state()
        if s['mode'] != 20:
            raise RuntimeError(f'選択中の予期しない遷移: {s}')
        if s['p1'] >= 4 and s['p2'] >= 4:
            break
        pulse()
    else:
        raise RuntimeError('通常パッド経路のキャラクター決定タイムアウト')
    time.sleep(.5)
    for _ in range(70):
        if state()['stage'] == 50:
            break
        pulse(up=True)
    else:
        raise RuntimeError('ステージ50を選択できない')
    pulse()
    deadline = time.monotonic()+40
    while time.monotonic() < deadline:
        if read32(handle, 0x54eee8) == 1 and (read32(handle, 0x55d20b)&255) == 0:
            return events
        time.sleep(.05)
    raise RuntimeError('戦闘開始タイムアウト')


def analyze(raw):
    magic, version, count, capacity, freq, status, size = HEADER.unpack_from(raw)
    if (magic != 0x42434343 or version != 1 or size != RECORD.size or status != 1
            or capacity != 262144 or count > capacity or freq <= 0
            or len(raw) < 4096 + count * size):
        raise RuntimeError(f'観測器ヘッダー異常: {HEADER.unpack_from(raw)}')
    records = list(RECORD.iter_unpack(raw[4096:4096 + count * size]))
    result = dict(samples=count, frequency=freq, status=status, phases={})
    result['probe_cost_us'] = dist([(r[1] - r[0]) * 1e6 / freq for r in records])
    for mode, name in [(20, 'character_select'), (1, 'battle')]:
        selected = []
        for previous, current in zip(records, records[1:]):
            if (previous[3] == current[3] == mode and current[2] == previous[2] + 1
                    and not previous[6] and not current[6]
                    and (mode != 1 or previous[7] == current[7] == 0)):
                selected.append((current[0] - previous[0]) * 1e6 / freq)
        errors = [abs(value - 1e6 / 60) for value in selected]
        result['phases'][name] = dict(interval_us=dist(selected), abs_error_us=dist(errors),
            over_3us=sum(e > 3 for e in errors), over_100us=sum(e > 100 for e in errors),
            over_1ms=sum(e > 1000 for e in errors))
    result['mode_counts'] = {str(m): sum(r[3] == m for r in records) for m in sorted({r[3] for r in records})}
    # 操作・ロード・イントロを除いた最初の連続戦闘区間を使用。
    # 最良区間の探索はせず、開始120Fを捨てて次の1500間隔を固定採用する。
    runs, run = [], []
    for row in records:
        valid = row[3] == 1 and row[6] == 0 and row[7] == 0
        if run and (not valid or row[2] != run[-1][2] + 1):
            runs.append(run)
            run = []
        if valid:
            run.append(row)
    if run:
        runs.append(run)
    eligible = next((r for r in runs if len(r) >= 1621), None)
    if eligible:
        window = eligible[120:1621]
        intervals = [(b[0]-a[0])*1e6/freq for a,b in zip(window,window[1:])]
        errors = [abs(v-1e6/60) for v in intervals]
        phase = [(r[0]-window[0][0])*1e6/freq-i*1e6/60 for i,r in enumerate(window)]
        result['fixed_battle'] = dict(warmup_frames=120, intervals=1500,
            first_ordinal=window[0][8], last_ordinal=window[-1][8],
            first_tick=window[0][0], last_tick=window[-1][0],
            interval_us=dist(intervals), abs_error_us=dist(errors),
            phase_end_us=phase[-1], phase_min_us=min(phase), phase_max_us=max(phase),
            over_3us=sum(e>3 for e in errors), over_100us=sum(e>100 for e in errors),
            over_1ms=sum(e>1000 for e in errors))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--variant', choices=['old', 'new'], required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--observe-seconds', type=float, default=0)
    parser.add_argument('--auto-training', action='store_true', help='既存ViGEm DS4と通常設定を使ってstage50を選択')
    parser.add_argument('--environment', action='append', default=[], help='明示するCCCASTER_診断設定 KEY=VALUE')
    args = parser.parse_args()
    if args.auto_training and (args.variant != 'new' or args.observe_seconds < 30):
        parser.error('自動選択はnew、観測30秒以上が必要')
    game_dir = (ROOT / '_TEST_MBAACC/LegacyBenchmark' / args.variant / 'MBAACC_1').resolve()
    game = game_dir / 'MBAA.exe'
    exe = game_dir / ('cccaster.v3.1.exe' if args.variant == 'old' else 'cccaster_B/CCCaster_B.exe')
    dll = game_dir / 'cccaster' / ('hook.dll' if args.variant == 'old' else 'libcccaster_hook.dll')
    if not all(p.is_file() for p in [game, exe, dll]):
        raise RuntimeError('独立コピーの成果物不足')
    existing = set()
    for pid, _, name in processes():
        if args.auto_training and name.lower() == 'mbaa.exe':
            raise RuntimeError('仮想入力が既存ゲームへ届かないよう、自動試験は既存MBAAなしで実行する')
        if name.lower() in ['mbaa.exe', exe.name.lower(), 'launcher.exe']:
            handle = open_process(0x1000, False, pid)
            if not handle:
                raise RuntimeError(f'既存対象名PIDの照合不可: {pid}')
            try:
                if path_of(handle).is_relative_to(game_dir):
                    raise RuntimeError(f'対象コピー使用中: {pid}')
            finally:
                close(handle)
        existing.add(pid)
    args.out.mkdir(parents=True, exist_ok=False)
    runtime_dir = 'cccaster' if args.variant == 'old' else 'cccaster_B'
    log_paths = [game_dir/f'{runtime_dir}/cccaster_hook_log.txt', game_dir/'cccaster.log',
                 game_dir/f'{runtime_dir}/dll.log', game_dir/f'{runtime_dir}/debug.log']
    for log in log_paths:
        if log.exists():
            log.replace(args.out / ('before_' + log.name))
    protected = {str(p): sha(p) for p in game_dir.rglob('*.ini')}
    protected[str(game)] = sha(game)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('CCCASTER_', 'CCBENCH_'))}
    for entry in args.environment:
        key, separator, value = entry.partition('=')
        if not separator or not key.startswith('CCCASTER_'):
            parser.error('診断設定はCCCASTER_名=値で指定する')
        env[key] = value
    command = [str(exe), '-ot', '-n'] if args.variant == 'old' else [str(exe), '--training']
    cwd = game_dir if args.variant == 'old' else exe.parent
    result = dict(variant=args.variant, command=command, game_dir=str(game_dir),
                  files={str(p): sha(p) for p in [game, exe, dll]}, frequency=frequency.value)
    result['environment'] = dict(entry.split('=',1) for entry in args.environment)
    handle = None
    proc = None
    observed = None
    owned = {}
    pad = None
    try:
        if args.auto_training:
            import vgamepad as vg
            pad = vg.VDS4Gamepad()
            time.sleep(.5)
        with (args.out / 'launcher.log').open('wb') as stream:
            result['start_tick'] = ticks()
            if args.variant == 'old':
                # 旧版は -n でもコンソールのフォントAPIを初期化する。
                # 標準出力をファイルへ置換せず、非表示の専用コンソールを付ける。
                startup = subprocess.STARTUPINFO()
                startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
                startup.wShowWindow = 0
                proc = subprocess.Popen(command, cwd=cwd, env=env, startupinfo=startup,
                                        creationflags=subprocess.CREATE_NEW_CONSOLE)
            else:
                proc = subprocess.Popen(command, cwd=cwd, env=env, stdout=stream,
                                        stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
            result['launcher_pid'] = proc.pid
            deadline = time.monotonic() + 45
            while time.monotonic() < deadline and handle is None:
                for pid, parent, name in processes():
                    if pid in existing or name.lower() not in ['mbaa.exe', 'launcher.exe']:
                        continue
                    candidate = open_process(0x1000 | 0x10 | 0x100000 | 1, False, pid)
                    if not candidate:
                        continue
                    candidate_path = path_of(candidate)
                    if not candidate_path.is_relative_to(game_dir):
                        close(candidate)
                        continue
                    owned[pid] = dict(path=str(candidate_path), parent=parent)
                    if candidate_path == game:
                        handle = candidate
                        result['game_pid'] = pid
                        result['discovered_tick'] = ticks()
                        break
                    close(candidate)
                if handle is None:
                    if proc.poll() is not None:
                        raise RuntimeError(f'ゲーム検出前にランチャー終了: {proc.returncode}')
                    time.sleep(.01)
            if handle is None:
                raise RuntimeError('対象ゲーム未検出')
            previous_tick = ticks()
            previous_world = None
            first_character_tick = None
            while time.monotonic() < deadline:
                mode = read32(handle, 0x54eee8)
                world = read32(handle, 0x55d1d4)
                current_tick = ticks()
                if mode == 20:
                    if first_character_tick is None:
                        first_character_tick = current_tick
                        result['first_character_select_tick'] = current_tick
                    if previous_world is not None and world != previous_world:
                        result.update(ready_tick=current_tick, ready_previous_poll_tick=previous_tick,
                                      ready_world=world, ready_mode=mode,
                                      startup_seconds=(current_tick-result['start_tick'])/frequency.value,
                                      poll_bracket_us=(current_tick-previous_tick)*1e6/frequency.value)
                        break
                    previous_world = world
                else:
                    previous_world = None
                previous_tick = current_tick
                time.sleep(.0005)
            else:
                raise RuntimeError('キャラクター選択のフレーム進行を検出できない')
            print(json.dumps({k: result[k] for k in ['variant','game_pid','startup_seconds','poll_bracket_us']}), flush=True)
            if args.observe_seconds:
                tools_dir = ROOT / 'build_logs/legacy_benchmark_20260913/tools'
                subprocess.run([str(tools_dir / 'legacy_benchmark_inject.exe'), str(result['game_pid']),
                    str(tools_dir / 'legacy_benchmark_probe.dll'), str(game)], check=True,
                    stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
                time.sleep(.25)
                observed = mmap.mmap(-1, MAP_SIZE, tagname=f"Local\\CCCasterLegacyBench_{result['game_pid']}")
                header = HEADER.unpack_from(observed)
                if header[0] != 0x42434343 or header[5] != 1:
                    raise RuntimeError(f'観測開始失敗: {header}')
                if args.auto_training:
                    # 前面化はComputer Useで行う。前面条件が揃うまで通常入力を開始しない。
                    foreground_deadline = time.monotonic() + 90
                    print('ゲームの前面化を確認してから自動選択を開始', flush=True)
                    while True:
                        foreground_pid = W.DWORD()
                        U.GetWindowThreadProcessId(U.GetForegroundWindow(), C.byref(foreground_pid))
                        if foreground_pid.value == result['game_pid']:
                            break
                        if time.monotonic() >= foreground_deadline:
                            raise RuntimeError('前面条件を確認できないため比較を開始しない')
                        time.sleep(.2)
                    result['automatic_selection'] = select_training(handle, pad, vg)
                    print('通常DS4経路で同一キャラ・stage50の戦闘に到達', flush=True)
                print(f"共通フレーム観測を{args.observe_seconds}秒実行中", flush=True)
                end = time.monotonic() + args.observe_seconds
                result['foreground_samples'] = []
                while time.monotonic() < end:
                    if wait(handle, 0) == 0:
                        raise RuntimeError('計測中にゲーム終了')
                    foreground_pid = W.DWORD()
                    U.GetWindowThreadProcessId(U.GetForegroundWindow(), C.byref(foreground_pid))
                    result['foreground_samples'].append(dict(tick=ticks(), game=foreground_pid.value == result['game_pid']))
                    time.sleep(.2)
                raw = observed[:]
                (args.out / 'frames.bin').write_bytes(raw)
                result['cadence'] = analyze(raw)
                result['scene'] = {name: read32(handle, address) for name,address in dict(
                    mode=0x54eee8, stage=0x74fd98, p1=0x74d8fc, p2=0x74d920,
                    p1moon=0x74d900, p2moon=0x74d924, p1color=0x74d904, p2color=0x74d928).items()}
                if args.auto_training:
                    fixed = result['cadence'].get('fixed_battle')
                    if not fixed:
                        raise RuntimeError('固定1500間隔を採取できない')
                    foreground = [s for s in result['foreground_samples'] if fixed['first_tick'] <= s['tick'] <= fixed['last_tick']]
                    if not foreground or not all(s['game'] for s in foreground):
                        raise RuntimeError('固定測定区間で前面条件が変化したため比較対象外')
            else:
                time.sleep(.3)
            result['passed'] = True
    except Exception as exc:
        result.update(passed=False, error=str(exc))
    finally:
        if handle:
            # 取得時に実パスを照合した同じHANDLE。終了途中のパス再照会は失敗し得る。
            if wait(handle, 0) != 0:
                terminate(handle, 0)
                wait(handle, 3000)
            close(handle)
        if proc:
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.terminate()
                proc.wait(timeout=3)
        if observed:
            observed.close()
        if pad:
            pad.reset(); pad.update()
        result['owned_processes'] = owned
        result['protected_unchanged'] = all(Path(p).exists() and sha(Path(p)) == h for p,h in protected.items())
        result['passed'] = result.get('passed', False) and result['protected_unchanged']
        result['protected_before'] = protected
        for log in log_paths:
            if log.exists():
                (args.out / log.name).write_bytes(log.read_bytes())
        (args.out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps({k: result[k] for k in ['variant','passed','protected_unchanged']}),flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
