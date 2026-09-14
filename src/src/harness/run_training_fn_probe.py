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
    parser.add_argument('--inspect-seconds', type=int, default=0)
    parser.add_argument('--menu-inspect', type=int, default=0)
    parser.add_argument('--menu-down', type=int, default=9)
    parser.add_argument('--menu-only', action='store_true')
    parser.add_argument('--confirm-inspect', type=int, default=0)
    args = parser.parse_args()
    if args.baseline_dll and not args.baseline_dll.is_file():
        parser.error('比較用DLLが存在しない')
    baseline = args.baseline_dll is not None
    import vgamepad as vg
    out = ROOT / 'test/logs' / time.strftime('training_fn_%Y%m%d_%H%M%S')
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
    result = dict(baseline=baseline,  runtime=str(game), samples=[], errors=[])
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
                     true_frame=read(0x562A40), sim=read(0x55D1CC), pause=read(0x55D203,1), freeze=read(0x562A48), p2x=read(0x555D34), p1seq=read(0x555140), p1seqstate=read(0x555144), p2seqstate=read(0x555C40), p2seq=read(0x555C3C), bgm_thread=read(0x76E844), bgm_playing=read(0x76E838),
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
        SAVE = vg.DS4_BUTTONS.DS4_BUTTON_SHARE
        RESET = vg.DS4_BUTTONS.DS4_BUTTON_OPTIONS
        def count(event):
            return text().count(f'[TrainingState] event={event} ')
        def move(direction, seconds=.6):
            pad.directional_pad(direction); pad.update(); time.sleep(seconds)
            pad.reset(); pad.update(); time.sleep(.3)
        def hold_save(label):
            previous = count(1)
            pad.press_button(SAVE); pad.update(); time.sleep(.15)
            frozen = [sample(label + '_hold_' + str(i)) for i in range(1)]
            for i in range(12):
                time.sleep(.05); frozen.append(sample(label + '_hold_' + str(i+1)))
            keys = ('x','p2x','p1seq','p2seq','p1seqstate','p2seqstate')
            if any(any(s[key] != frozen[0][key] for key in keys) for s in frozen):
                result['errors'].append(label + ': 両キャラの停止を維持できない')
            if count(1) != previous+1:
                result['errors'].append(label + ': 1押下1保存にならない')
            if not any(s['freeze'] != 0 for s in frozen) or any(s['pause'] != 0 for s in frozen):
                result['errors'].append(label + ': 保存押下中の停止フラグなし')
            pad.reset(); pad.update(); time.sleep(.25)
            if read(0x55D203,1) != 0:
                result['errors'].append(label + ': 離した後の停止解除に失敗')
            return frozen[0]
        if args.menu_only:
            hold_save('menu')
        else:
            initial = sample('initial_no_save')
            move(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST)
            button(RESET); time.sleep(.8)
            no_save = sample('reset_without_save')
            if no_save['x'] != initial['x'] or count(2):
                result['errors'].append('保存なしのFN2が通常リセットにならない')
            move(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST)
            saved = hold_save('normal')
            # FN1旧機能や旧TrainingLoad割当がロードを発火しない。
            previous=count(2)
            button(vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_LEFT)
            move(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,.3)
            if count(2) != previous:
                result['errors'].append('FN2以外でロードが発火')
            for i in range(3):
                previous=count(2)
                pad.press_button(RESET); pad.update()
                for j in range(40):
                    sample(f'reset_{i}_{j}'); time.sleep(.025)
                pad.reset(); pad.update(); time.sleep(.2)
                loaded=sample(f'loaded_{i}')
                if count(2) != previous+1 or loaded['x'] != saved['x']:
                    result['errors'].append(f'FN2リセット後の1回だけの復元失敗: {i}')
            move(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,2.5)
            pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE); pad.update()
            deadline=time.monotonic()+1
            while time.monotonic()<deadline:
                counters=[[read(0x5552A2+s*0xAFC,1),read(0x5552D4+s*0xAFC,1)] for s in range(2)]
                if all(any(v) for v in counters):
                    result['hitstop_counters_at_press']=counters
                    break
                time.sleep(.001)
            else:
                raise RuntimeError('双方のヒットストップに到達しない')
            held_hitstop=hold_save('hitstop')
            for i in range(3):
                offset=len(text()); button(RESET); time.sleep(.6)
                if 'restoredStopped=1' not in text()[offset:]:
                    result['errors'].append('FN2後にヒットストップを復元できない')
            if args.inspect_seconds:
                pad.press_button(SAVE); pad.update()
                print('INSPECT: holding FN1 for UI verification',flush=True)
                time.sleep(min(args.inspect_seconds,180))
                pad.reset(); pad.update(); time.sleep(.3)
            # 通常メニューのCHARACTER SELECTへ移動。試験設定のStart=B7(R2)。
        previous=count(7)
        button(vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT)
        for i in range(args.menu_down):
            move(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH,.07)
        if args.menu_inspect:
            print('INSPECT: character-select menu selection',flush=True)
            time.sleep(min(args.menu_inspect,60))
        button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
        if args.confirm_inspect:
            print('INSPECT: after CHARACTER SELECT confirmation',flush=True)
            time.sleep(min(args.confirm_inspect,60))
        time.sleep(1)
        if count(7) == previous and read(0x54EEE8)==1:
            # 実画面で確認したReturn to character selection?は既定NO。上のYESを選ぶ。
            move(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,.07)
            button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
        deadline=time.monotonic()+8
        while count(7)==previous and time.monotonic()<deadline:
            time.sleep(.1)
        if count(7) != previous+1:
            result['errors'].append('キャラセレクト移動時の保存消去に未到達')
        else:
            previous=count(2)
            deadline=time.monotonic()+25
            while time.monotonic()<deadline:
                if read(0x54EEE8)==1 and read(0x55D20B,1)==0:
                    break
                button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
            else:
                raise RuntimeError('キャラセレクトから再度戦闘に入れない')
            button(RESET); time.sleep(.8)
            if count(2) != previous:
                result['errors'].append('キャラセレクト後に古い保存を読み込んだ')
            sample('reset_after_character_select')
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
