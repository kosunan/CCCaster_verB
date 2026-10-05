"""独立3コピー・通常DS4入力で左右FN2と保存保持を検証する。メモリは読取り専用。"""
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


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def protected(root):
    return {str(p): digest(p) for p in root.rglob('*')
            if p.is_file() and (p.suffix.lower() == '.ini' or p.name.lower() == 'mbaa.exe')}


def main():
    import vgamepad as vg
    stamp = time.strftime('%Y%m%d_%H%M%S')
    out = ROOT / 'test/logs' / ('training_corner_' + stamp)
    runtime = ROOT / 'test/runtime' / ('training_corner_' + stamp)
    out.mkdir(parents=True)
    sources = [ROOT / 'test/runtime' / f'MBAACC_{i}' for i in range(1, 4)]
    before = {str(p): protected(p) for p in sources}
    result = dict(errors=[], samples=[], runtime=str(runtime))
    proc = None
    handle = None
    game_pid = None
    pad = None
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
    k.OpenProcess.restype = W.HANDLE
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]
    caster = runtime / 'MBAACC_1/cccaster_B'
    log = caster / 'cccaster_hook_log.txt'

    def text():
        return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''

    def count(event):
        return text().count(f'[TrainingState] event={event} ')

    def wait_until(predicate, seconds=8):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(.03)
        raise RuntimeError('条件未達: ' + getattr(predicate, '__name__', 'wait'))

    def read(addr, size=4, signed=False):
        data = C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle, addr, data, size, None):
            raise C.WinError(C.get_last_error())
        return int.from_bytes(data.raw, 'little', signed=signed)

    def sample(label):
        value = dict(label=label, p1x=read(0x555238, signed=True), p2x=read(0x555D34, signed=True),
                     camera=read(0x564B14, signed=True), p1face=read(0x555444, 1),
                     p2face=read(0x555F40, 1), sim=read(0x55D1CC),
                     bgm_patch=read(0x472C6D, 2), bgm_thread=read(0x76E844))
        result['samples'].append(value)
        return value

    def release():
        pad.reset(); pad.update(); time.sleep(.15)

    def press(button, seconds=.12):
        pad.press_button(button); pad.update(); time.sleep(seconds); release()

    try:
        print('Logs:', out, flush=True)
        for source in sources:
            shutil.copytree(source, runtime / source.name,
                            ignore=shutil.ignore_patterns('cccaster_hook_log.txt', 'broadcast'))
        subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'deploy.ps1'),
                        '-TestRoot', str(runtime)], cwd=ROOT, check=True,
                       stdout=(out / 'deploy.log').open('w'), stderr=subprocess.STDOUT)
        (caster / 'cccaster.ini').write_text(
            '[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid=\nDelay=2\n', encoding='utf-8')
        (caster / 'Wireless Controller.ini').write_text(
            '[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\n'
            'A=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n', encoding='utf-8')
        pad = vg.VDS4Gamepad()
        time.sleep(1)
        env = {key: value for key, value in os.environ.items() if not key.startswith('CCCASTER_')}
        env.update(CCCASTER_TRAINING_TRACE='1', CCCASTER_INPUT_DIAGNOSTIC='1')
        with (out / 'launcher.log').open('w') as stream:
            proc = subprocess.Popen([str(caster / 'CCCaster_B.exe'), '--training'], cwd=caster,
                                    env=env, stdout=stream, stderr=subprocess.STDOUT,
                                    creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic() + 40
        while '[TrainingAdvantage] valid=1' not in text():
            if time.monotonic() > deadline:
                raise RuntimeError('戦闘に未到達')
            if '[FastBoot] ★ CharaSelect reached!' in text():
                press(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
            else:
                time.sleep(.2)
        pid = subprocess.check_output(['pwsh', '-NoProfile', '-Command',
            f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | "
            f"Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"], text=True).strip()
        game_pid = int(pid)
        handle = k.OpenProcess(0x10, False, game_pid)
        time.sleep(.3)
        SAVE, RESET = vg.DS4_BUTTONS.DS4_BUTTON_SHARE, vg.DS4_BUTTONS.DS4_BUTTON_OPTIONS
        initial = sample('initial')

        def corner(direction, saved):
            corners, loads = count(10), count(2)
            pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST if direction < 0
                                else vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST)
            pad.press_button(RESET); pad.update()
            wait_until(lambda: count(10) == corners + 1)
            # 方向だけ離し、FN2は保持。追加リセットされず配置が残ることを確認。
            pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NONE); pad.update()
            time.sleep(.25)
            actual = sample(f'corner_{direction}_saved_{saved}')
            if actual['p1x'] != direction * 45056 or actual['p2x'] != direction * 61440:
                raise RuntimeError('壁際配置不一致: ' + str(actual))
            if (actual['p1face'], actual['p2face']) != ((1, 0) if direction < 0 else (0, 1)):
                raise RuntimeError('向き不一致')
            time.sleep(.3)
            held = sample('held_FN2')
            if held['sim'] <= actual['sim'] or count(10) != corners + 1 or count(2) != loads:
                raise RuntimeError('FN2保持中の再発火または保存の誤ロード')
            release()

        for direction in (-1, 1):
            corner(direction, False)
        press(RESET); time.sleep(.6)
        neutral = sample('neutral_no_save')
        if (neutral['p1x'], neutral['p2x']) != (initial['p1x'], initial['p2x']):
            raise RuntimeError('保存なし通常リセット不一致')
        saved_count = count(1)
        pad.press_button(SAVE); pad.update()
        wait_until(lambda: count(1) == saved_count + 1)
        saved = sample('saved')
        time.sleep(.3)
        held = sample('save_held')
        if (saved['p1x'], saved['p2x']) != (held['p1x'], held['p2x']):
            raise RuntimeError('FN1停止中の移動')
        release()
        for direction in (-1, 1):
            corner(direction, True)
            loads = count(2)
            press(RESET)
            wait_until(lambda: count(2) == loads + 1)
            loaded = sample('neutral_load')
            if (loaded['p1x'], loaded['p2x']) != (saved['p1x'], saved['p2x']):
                raise RuntimeError('方向指定後の保存保持不一致')
        if any(s['bgm_patch'] != 0x5EB or s['bgm_thread'] != initial['bgm_thread'] for s in result['samples']):
            raise RuntimeError('BGM維持命令・スレッド不一致')
    except Exception as exc:
        result['errors'].append(str(exc))
    finally:
        if pad:
            pad.reset(); pad.update()
        if handle:
            k.CloseHandle(handle)
        # 今回起動した子PIDかつ対象の絶対パスに一致するゲームだけ終了する。
        if proc:
            target = str(runtime / 'MBAACC_1/MBAA.exe').replace("'", "''")
            subprocess.run(['pwsh', '-NoProfile', '-Command',
                f"Get-CimInstance Win32_Process | Where-Object {{ $_.ParentProcessId -eq {proc.pid} "
                f"-and $_.ExecutablePath -eq '{target}' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"],
                capture_output=True)
            if proc.poll() is None:
                proc.terminate(); proc.wait(timeout=5)
        if log.exists():
            shutil.copy2(log, out / 'game.log')
        result['protected_unchanged'] = before == {str(p): protected(p) for p in sources}
        result['binaries'] = {p.name: digest(p) for p in (ROOT / 'build/bin').glob('*')
                              if p.name in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')}
        result['passed'] = not result['errors'] and result['protected_unchanged']
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps({k: v for k, v in result.items() if k != 'samples'}, ensure_ascii=False), flush=True)
    return int(not result['passed'])


if __name__ == '__main__':
    raise SystemExit(main())
