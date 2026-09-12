"""仮想DS4の実DirectInputでトレーニングの有利不利表示を検証する。"""
import gc
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    import vgamepad as vg
    game_dir = ROOT / '_TEST_MBAACC/MBAACC_1'
    caster = game_dir / 'cccaster'
    out = ROOT / 'build_logs' / time.strftime('training_utility_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    existing = subprocess.check_output(['powershell', '-NoProfile', '-Command',
        "@(Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\").Count"], text=True).strip()
    if existing != '0':
        raise RuntimeError('既存ゲームが起動中。試験を開始しない')
    ini = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in game_dir.rglob('*.ini')}
    for name in ('CCCaster_v10.exe', 'libcccaster_hook.dll'):
        shutil.copy2(caster / name, out / ('before_' + name))
        shutil.copy2(ROOT / 'build/bin' / name, caster / name)
    log = caster / 'cccaster_hook_log.txt'
    if log.exists():
        shutil.move(log, out / 'before.log')
    env = {k: v for k, v in os.environ.items() if not k.startswith('CCCASTER_')}
    env.update(CCCASTER_TRAINING_TRACE='1', CCCASTER_TEST_VIRTUAL_PRODUCT='05C4054C', CCCASTER_FRAME_TIMING_TRACE='1')
    pad = vg.VDS4Gamepad()
    process = None
    errors = []
    try:
        time.sleep(1)
        with (out / 'launcher.log').open('w') as stream:
            process = subprocess.Popen([str(caster / 'CCCaster_v10.exe'), '--training'], cwd=caster,
                env=env, stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        print(f'Logs: {out}', flush=True)

        def contents():
            return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''

        def button(value, seconds=.1):
            pad.press_button(value); pad.update(); time.sleep(seconds)
            pad.reset(); pad.update(); time.sleep(.2)

        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            text = contents()
            if '[TrainingAdvantage] valid=1' in text:
                break
            if '[FastBoot] ★ CharaSelect reached!' in text:
                button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
            else:
                time.sleep(.2)
            if process.poll() is not None:
                raise RuntimeError('起動・選択中に終了')
        else:
            raise RuntimeError('トレーニング戦闘へ未到達')
        print('Training ready; approaching dummy', flush=True)
        pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST)
        pad.update(); time.sleep(1.8); pad.reset(); pad.update(); time.sleep(.4)
        button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
        time.sleep(1)
        print('Attack sequence done; keeping final result visible for 30 seconds', flush=True)
        time.sleep(30)
        text = contents()
        if 'state=1' not in text or 'state=2' not in text:
            errors.append('計測中→確定へ未到達')
        if not re.search(r'\[VirtualPad\].*matches=1.*value=1040', text):
            errors.append('仮想パッドのA入力を未確認')
        samples = re.findall(r'\[TrainingAdvantage\] valid=(\d+) true=(\d+) sim=(\d+) round=(\d+) active=(-?\d+)/(-?\d+) busy=(-?\d+)/(-?\d+) stopped=(\d+) state=(\d+) p1=(-?\d+)', text)
        finals = []
        last_state = 0
        for row in samples:
            values = list(map(int, row))
            if values[9] == 2 and last_state != 2:
                finals.append(values)
            last_state = values[9]
        print(f'Final results: {finals}', flush=True)
    except Exception as exc:
        errors.append(str(exc))
        finals = []
    finally:
        pad.reset(); pad.update()
        if process is not None:
            # 今回のランチャーの子だけを終了。既存のゲームは対象にしない。
            subprocess.run(['powershell', '-NoProfile', '-Command',
                f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {process.pid} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"], check=True)
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.terminate(); process.wait(timeout=3)
        del pad
        gc.collect()
        if log.exists():
            shutil.copy2(log, out / 'game.log')
        after = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in game_dir.rglob('*.ini')}
        if ini != after:
            errors.append('INI変化を検出')
        result = dict(passed=not errors, errors=errors, finals=finals, ini_unchanged=ini == after,
            dll_sha256=hashlib.sha256((caster / 'libcccaster_hook.dll').read_bytes()).hexdigest())
        (out / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False), flush=True)
    return int(bool(errors))


if __name__ == '__main__':
    raise SystemExit(main())
