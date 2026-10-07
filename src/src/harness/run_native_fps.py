"""仮想DS4の連続入力で標準FPS表示と通常更新速度を実測する。ゲームメモリは読取りのみ。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import random
import re
import shutil
import statistics
import subprocess
import time
import psutil
from run_training_character import device_guid
from run_training_corner import protected, digest
from verify_native_input_writes import verify_local, verify_local_disabled
from verify_frame_pipeline import verify as verify_pipeline

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['training', 'offline'], default='training')
    parser.add_argument('--expect-inflation', action='store_true')
    parser.add_argument('--expect-local-rollback', action='store_true', help='旧方式の実験を明示的に有効化する')
    parser.add_argument('--pacing-trace', action='store_true',
                        help='通常更新の締切・待機後準備・再計算の区間を記録する')
    parser.add_argument('--prepare-spike-us', type=int, choices=range(0,8001), default=0,
                        help='60Fごとに処理開始後の準備へ指定µsの実負荷を注入する')
    parser.add_argument('--monitor-trace', action='store_true', help='待機中の再提示を含むPresent所要時間を記録する')
    parser.add_argument('--binaries', type=Path, default=ROOT / 'build/bin')
    parser.add_argument('--source-game', type=Path, default=ROOT / 'test/runtime/MBAACC_1',
                        help='読取り専用のコピー元。別のゲームが使用中なら停止済みコピーを指定する')
    args = parser.parse_args()
    if args.prepare_spike_us and not args.pacing_trace:
        parser.error('--prepare-spike-us requires --pacing-trace')
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    stamp = time.strftime('%Y%m%d_%H%M%S')
    out = ROOT / 'test/logs' / f'native_fps_{args.mode}_{stamp}'
    runtime = ROOT / 'test/runtime' / out.name
    out.mkdir(parents=True)
    source = args.source_game.resolve()
    before = protected(source)
    shutil.copytree(source, runtime, ignore=shutil.ignore_patterns('cccaster_hook_log.txt', 'broadcast'))
    caster = runtime / 'cccaster_B'
    names = ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')
    for name in names:
        shutil.copy2(args.binaries / name, caster / name)
        assert digest(caster / name) == digest(args.binaries / name)
    product = random.SystemRandom().randrange(0x8000, 0xFFFF)
    class Pad(vg.VDS4Gamepad):
        def target_alloc(self):
            target = vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target, 0x054C)
            vc.vigem_target_set_pid(target, product)
            return target
    pad = Pad()
    time.sleep(.6)
    guid = device_guid((product << 16) | 0x054C)
    assert guid, '仮想パッドのGUIDを特定できない'
    # Offline uses this same pad for both sides so every transition exercises both inputs.
    (caster / 'cccaster.ini').write_text('[Settings]\n' + ''.join(
        f'P{p}Device=Wireless Controller\nP{p}DeviceGuid={guid}\n' for p in (1, 2)) +
        '[Netplay]\nDefaultDelay=0\n', encoding='utf-8')
    (caster / f'Wireless Controller__{guid}.ini').write_text(
        '[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nStart=B7\n', encoding='utf-8')
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; k.OpenProcess.restype = W.HANDLE
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]
    handle = proc = game = None
    result = dict(mode=args.mode, expect_inflation=args.expect_inflation,
                  pacing_trace=args.pacing_trace, errors=[], phases={}, samples=[])
    log = caster / 'cccaster_hook_log.txt'
    def text(): return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
    def read(address, size=4):
        data = C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle, address, data, size, None): raise C.WinError(C.get_last_error())
        return int.from_bytes(data.raw, 'little')
    def press():
        pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE); pad.update(); time.sleep(.12)
        pad.reset(); pad.update(); time.sleep(.2)
    try:
        env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
        env.update(CCCASTER_INPUT_TRACE='1', CCCASTER_FRAME_TIMING_TRACE='1')
        if args.expect_local_rollback:
            env['CCCASTER_TEST_LOCAL_INPUT_ROLLBACK'] = '1'
        if args.pacing_trace:
            env.update(CCCASTER_UPDATE_CADENCE='1', CCCASTER_OFFLINE_PACING_TRACE='1',
                       CCCASTER_SPIN_PROBE='1')
        if args.prepare_spike_us:
            env['CCCASTER_TEST_FRAME_PREPARE_US'] = str(args.prepare_spike_us)
        if args.monitor_trace:
            env['CCCASTER_MONITOR_PRESENT_TRACE'] = '1'
        result['diagnostics'] = {k: v for k, v in env.items() if k.startswith('CCCASTER_')}
        with (out / 'launcher.log').open('w') as stream:
            proc = subprocess.Popen([str(caster / 'CCCaster_B.exe'), '--' + args.mode], cwd=caster,
                env=env, stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        print('Logs:', out, flush=True)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            games = [p for p in psutil.process_iter(['exe']) if p.info['exe'] and
                     Path(p.info['exe']).resolve() == (runtime / 'MBAA.exe').resolve()]
            if games:
                game = games[0]; handle = k.OpenProcess(0x10, False, game.pid); break
            time.sleep(.1)
        if not handle: raise RuntimeError('ゲームを起動できない')
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            if read(0x54EEE8) == 1 and read(0x55D20B, 1) == 0: break
            press()
        else: raise RuntimeError('戦闘に到達しない')
        # Allow two stock reporting windows to replace the loading/intro sample.
        pad.reset(); pad.update(); time.sleep(2.2)
        for label, seconds, active in [('idle', 4, False), ('rapid', 8, True), ('recovered', 3, False)]:
            start = time.perf_counter(); initial_world = read(0x55D1D4)
            samples = []; index = 0
            while time.perf_counter() - start < seconds:
                pad.reset()
                if active:
                    pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST if index % 2 else
                                        vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST)
                pad.update(); index += 1
                now = time.perf_counter()
                samples.append(dict(phase=label, elapsed=now-start, fps=read(0x774A70), world=read(0x55D1D4),
                                    directions=[read(0x55541B,1),read(0x555F17,1)]))
                time.sleep(.025)
            final_world = read(0x55D1D4)
            rate = (final_world - initial_world) / (time.perf_counter() - start)
            settled = [s['fps'] for s in samples if s['elapsed'] >= 1.5]
            result['samples'] += samples
            result['phases'][label] = dict(world_hz=rate, fps_min=min(settled),
                fps_max=max(settled), fps_median=statistics.median(settled), samples=len(settled),
                initial_world=initial_world, final_world=final_world)
            assert 58 <= rate <= 62, f'{label}: 通常更新が60Hzから外れた: {rate}'
            if label == 'rapid' and args.expect_inflation:
                assert statistics.median(settled) > 100, '修正前のFPS上昇を再現できない'
            else:
                assert 57 <= min(settled) <= max(settled) <= 63, f'{label}: 標準FPSが不安定: {settled}'
        players = (1,) if args.mode == 'training' else (1,2)
        result['directions_seen'] = {str(p): sorted({s['directions'][p-1] for s in result['samples']
            if s['phase']=='rapid'}) for p in players}
        assert all({4,6}.issubset(result['directions_seen'][str(p)]) for p in players), '両方向の入力反映がない'
        result['native_input_rollback'] = (verify_local(text(), 1 if args.mode == 'training' else 5,
            players=players) if args.expect_local_rollback or args.expect_inflation else verify_local_disabled(text()))
        assert result['native_input_rollback']['passed'], '指定した入力再計算の有効／無効と一致しない'
        assert not re.search(r'\[Exception\]|\[SceneRunner\] FAILED|SYNC TIMEOUT', text())
        if not args.expect_inflation:
            assert '[NativeFpsCounter] installed=1' in text(), 'FPS修正フックを確認できない'
    except Exception as exc:
        result['errors'].append(str(exc))
    finally:
        pad.reset(); pad.update()
        if handle: k.CloseHandle(handle)
        if game and game.is_running(): game.terminate(); game.wait(timeout=10)
        if proc:
            try: proc.wait(timeout=4)
            except subprocess.TimeoutExpired: proc.terminate(); proc.wait(timeout=4)
        if log.exists(): shutil.copy2(log, out / 'game.log')
        if args.pacing_trace and result['phases']:
            result['frame_pipeline'] = {name: verify_pipeline(text(),
                first=phase['initial_world'], last=phase['final_world'],
                minimum=120, spike_us=args.prepare_spike_us)
                for name, phase in result['phases'].items()}
            if not all(p['passed'] for p in result['frame_pipeline'].values()):
                result['errors'].append('入力準備前のフレーム処理開始を確認できない')
        result['binaries'] = {n: digest(caster/n) for n in names}
        after = protected(source)
        result['source_game'] = str(source)
        result['source_unchanged'] = after == before
        result['source_changes'] = {p: dict(before=before.get(p), after=after.get(p))
                                   for p in before.keys() | after.keys() if before.get(p) != after.get(p)}
        result['passed'] = not result['errors'] and result['source_unchanged']
        (out/'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k:v for k,v in result.items() if k!='samples'}, ensure_ascii=False), flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
