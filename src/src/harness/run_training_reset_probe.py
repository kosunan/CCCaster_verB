"""独立コピーと仮想DS4の通常設定経路で保存→FN2→読込を確認する。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-dll', type=Path, help='比較用の修正前DLL（元ファイルは保全）')
    parser.add_argument('--hitstop', action='store_true', help='接近して打撃のヒットストップ中にも保存する')
    args = parser.parse_args()
    if args.baseline_dll and not args.baseline_dll.is_file():
        parser.error('比較用DLLが存在しない')
    baseline = args.baseline_dll is not None
    import vgamepad as vg
    out = ROOT / 'test/logs' / time.strftime('training_reset_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    game = ROOT / 'test/runtime' / ('TrainingReset_' + out.name)
    source = ROOT / 'test/runtime/MBAACC_1'
    # 共有リンクは作らず、元の設定・バイナリ・ログには触れない。
    shutil.copytree(source, game, ignore=shutil.ignore_patterns('cccaster_hook_log.txt', 'broadcast'))
    caster = game / 'cccaster_B'
    for name in ('CCCaster_B.exe', 'libcccaster_hook.dll'):
        shutil.copy2(ROOT / 'build/bin' / name, caster / name)
    if baseline:
        shutil.copy2(args.baseline_dll, caster / 'libcccaster_hook.dll')
    (caster / 'cccaster.ini').write_text('[Settings]\nP1Device = Wireless Controller\nP1DeviceGuid =\n', encoding='utf-8')
    (caster / 'Wireless Controller.ini').write_text(
        '[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\n'
        'A=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n'
        'TrainingSave=B5\nTrainingLoad=B6\n', encoding='utf-8')
    original_ini = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source.rglob('*.ini')}
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; k.OpenProcess.restype = W.HANDLE
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]
    handle = None
    proc = None
    result = dict(baseline=baseline, hitstop=args.hitstop, runtime=str(game), samples=[], errors=[])
    pad = vg.VDS4Gamepad()
    log = caster / 'cccaster_hook_log.txt'

    def text():
        return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''

    def button(b):
        pad.press_button(b); pad.update(); time.sleep(.12)
        pad.reset(); pad.update(); time.sleep(.3)

    def read(addr, size=4):
        data = C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle, addr, data, size, None):
            raise C.WinError(C.get_last_error())
        return int.from_bytes(data.raw, 'little')

    def sample(label):
        value = dict(label=label, mode=read(0x54EEE8), intro=read(0x55D20B, 1),
                     round=read(0x5550E0), x=read(0x555238),
                     bgm_thread=read(0x76E844), bgm_playing=read(0x76E838),
                     bgm_marker=read(0x76E004), patch=hex(read(0x472C6D, 2)))
        result['samples'].append(value)
        return value

    try:
        time.sleep(1)
        env = {key: value for key, value in os.environ.items() if not key.startswith('CCCASTER_')}
        env.update(CCCASTER_TRAINING_TRACE='1', CCCASTER_INPUT_DIAGNOSTIC='1')
        with (out / 'launcher.log').open('w') as stream:
            proc = subprocess.Popen([str(caster / 'CCCaster_B.exe'), '--training'], cwd=caster,
                                    env=env, stdout=stream, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        print('Logs:', out, flush=True)
        deadline = time.monotonic() + 35
        while time.monotonic() < deadline:
            if '[TrainingAdvantage] valid=1' in text():
                break
            if '[FastBoot] ★ CharaSelect reached!' in text():
                button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
            else:
                time.sleep(.2)
        else:
            raise RuntimeError('戦闘開始に到達しない')
        pid = subprocess.check_output(['powershell', '-NoProfile', '-Command',
            f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"], text=True).strip()
        handle = k.OpenProcess(0x10, False, int(pid))
        time.sleep(1)
        pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST)
        pad.update(); time.sleep(.65); pad.reset(); pad.update(); time.sleep(.5)
        saved = sample('before_save')
        button(vg.DS4_BUTTONS.DS4_BUTTON_SHOULDER_RIGHT)
        if '[TrainingState] event=1 saved=1' not in text():
            raise RuntimeError('保存に失敗')
        for index in range(3):
            before = sample(f'before_reset_{index}')
            button(vg.DS4_BUTTONS.DS4_BUTTON_OPTIONS)
            for tick in range(35):
                sample(f'reset_{index}_{tick}')
                time.sleep(.05)
            reset = sample(f'after_reset_{index}')
            if reset['x'] == saved['x']:
                result['errors'].append('FN2による位置リセットを確認できない')
            button(vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_LEFT)
            loaded = sample(f'after_load_{index}')
            if loaded['x'] != saved['x']:
                result['errors'].append(f'保存位置を復元できない: {index}')
            if loaded['round'] != saved['round']:
                result['errors'].append('ラウンド番号不一致')
        if args.hitstop:
            pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST)
            pad.update(); time.sleep(2.5); pad.reset(); pad.update(); time.sleep(.5)
            event_offset = len(text())
            pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE); pad.update()
            deadline = time.monotonic() + 1
            while time.monotonic() < deadline:
                # ReadTrainingFrameと同じ本体／被弾ヒットストップBYTE。
                counters = [[read(0x5552A2 + side * 0xAFC, 1),
                             read(0x5552D4 + side * 0xAFC, 1)] for side in range(2)]
                if all(any(values) for values in counters):
                    result['hitstop_counters_at_press'] = counters
                    sample('hitstop_before_save')
                    pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SHOULDER_RIGHT); pad.update()
                    time.sleep(.04)
                    pad.reset(); pad.update()
                    break
                time.sleep(.001)
            else:
                raise RuntimeError('双方のヒットストップに到達しない')
            time.sleep(.6)
            new_events = text()[event_offset:]
            if '[TrainingState] event=1 saved=1' not in new_events:
                raise RuntimeError('ヒットストップ中の保存を受け付けない')
            if not baseline and 'stopped=1 paused=0 players=1/1' not in new_events:
                raise RuntimeError('保存時点で双方がヒットストップ中か確認できない')
            for index in range(3):
                event_offset = len(text())
                button(vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_LEFT)
                sample(f'hitstop_after_load_{index}')
                if not baseline and 'restoredStopped=1' not in text()[event_offset:]:
                    result['errors'].append('ヒットストップの復元を確認できない')
                time.sleep(.5)
        result['state_events'] = [line for line in text().splitlines() if '[TrainingState]' in line]
    except Exception as exc:
        result['errors'].append(str(exc))
    finally:
        pad.reset(); pad.update()
        if handle:
            k.CloseHandle(handle)
        if proc:
            subprocess.run(['powershell', '-NoProfile', '-Command',
                f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"], check=True)
            try:
                proc.wait(timeout=4)
            except subprocess.TimeoutExpired:
                proc.terminate(); proc.wait(timeout=4)
        if log.exists():
            shutil.copy2(log, out / 'game.log')
        result['original_ini_unchanged'] = original_ini == {
            str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in source.rglob('*.ini')}
        result['dll_sha256'] = hashlib.sha256((caster / 'libcccaster_hook.dll').read_bytes()).hexdigest()
        # 命令の実メモリ照合。音声の聴感や再生カーソルの連続性とは区別する。
        result['music_patch_verified'] = bool(result['samples']) and all(
            s['patch'] == '0x5eb' for s in result['samples'])
        if not baseline and not result['music_patch_verified']:
            result['errors'].append('BGM維持パッチを確認できない')
        result['passed'] = not result['errors'] and result['original_ini_unchanged']
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: v for k, v in result.items() if k != 'samples'}, ensure_ascii=False), flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
