"""キャラ選択の表示設定8項目とディレイ非保存を通常入力・再起動で検査する。"""
import argparse
import configparser
import ctypes as C
from ctypes import wintypes as W
import json
import os
import random
import re
import shutil
import subprocess
import time
from run_training_character import ROOT, device_guid
from run_training_corner import protected, digest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect',action='store_true')
    parser.add_argument('--inspect-at',action='append',default=[],help='実画面確認で停止するラベル。省略時は全箇所。')
    args=parser.parse_args()
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    product=random.SystemRandom().randrange(0x8000,0xffff)
    class Pad(vg.VDS4Gamepad):
        def target_alloc(self):
            target=vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target,0x054c);vc.vigem_target_set_pid(target,product)
            return target
    runtime=ROOT/'test/runtime/training_character'
    caster=runtime/'MBAACC_1/cccaster_B'
    out=ROOT/'test/logs'/time.strftime('selection_preferences_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    result=dict(errors=[],checks={})
    def preserved():
        return protected(runtime) | {str(p):digest(p) for p in runtime.rglob('*.rep')}
    before=preserved()
    backups={p:p.read_bytes() if p.exists() else None for p in tuple(caster/name for name in ('cccaster.ini','Wireless Controller.ini','display.ini','display.ini.bak','display.ini.tmp'))}
    proc=handle=pad=None
    log=caster/'cccaster_hook_log.txt'
    k=C.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
    k.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
    k.CloseHandle.argtypes=[W.HANDLE]
    def raw(addr,size):
        data=C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle,addr,data,size,None):raise C.WinError(C.get_last_error())
        return data.raw
    def read(addr,size=4):return int.from_bytes(raw(addr,size),'little')
    def logs():return log.read_text(encoding='utf-8',errors='replace') if log.exists() else ''
    def check(name,value):
        result['checks'][name]=bool(value)
        if not value:raise RuntimeError(name)
    def wait(condition,reason,seconds=20):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if condition():return
            if proc and proc.poll() is not None:raise RuntimeError('起動元終了: '+reason)
            time.sleep(.03)
        raise RuntimeError(reason)
    buttons=dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE,b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
        c=vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE,d=vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE,
        start=vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT,save=vg.DS4_BUTTONS.DS4_BUTTON_SHARE,reset=vg.DS4_BUTTONS.DS4_BUTTON_OPTIONS)
    directions=dict(left=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,right=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
        up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,down=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH,
        dr=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTHEAST,dl=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTHWEST)
    def press(key,hold=.09,release=.17):
        pad.reset()
        for part in key.split('+'):
            if part in buttons:pad.press_button(buttons[part])
            else:pad.directional_pad(directions[part])
        pad.update();time.sleep(hold);pad.reset();pad.update();time.sleep(release)
    def inspect(label):
        if args.inspect and (not args.inspect_at or label in args.inspect_at):
            print('INSPECT '+label+'; continueで再開',flush=True)
            input()
    try:
        with (out/'deploy.log').open('w') as stream:
            subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'deploy.ps1'),'-TestRoot',str(runtime)],check=True,stdout=stream,stderr=subprocess.STDOUT)
        pad=Pad();found=[]
        wait(lambda:bool(found.append(device_guid((product<<16)|0x054c)) or found[-1]),'仮想パッドなし')
        (caster/'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={found[-1]}\n[Netplay]\nDefaultDelay=2\n',encoding='utf-8')
        (caster/'Wireless Controller.ini').write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
        result['binaries']={name:digest(caster/name) for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')}
        check('最新3バイナリ',all(digest(ROOT/'build/bin'/name)==sha for name,sha in result['binaries'].items()))
        for name in ('display.ini','display.ini.bak','display.ini.tmp'):
            (caster/name).unlink(missing_ok=True)
        configured=(caster/'cccaster.ini').read_bytes()
        env={key:value for key,value in os.environ.items() if not key.startswith('CCCASTER_')}
        env['CCCASTER_TEST_SELECTION_OPTIONS']='1' # HUDの値を観測。SCRIPT_INPUTは有効にしない。
        run=0
        def stop():
            nonlocal proc,handle
            if handle:k.CloseHandle(handle);handle=None
            if proc:
                target=str(runtime/'MBAACC_1/MBAA.exe').replace("'","''")
                subprocess.run(['pwsh','-NoProfile','-Command',f"Get-CimInstance Win32_Process | Where-Object {{ $_.ParentProcessId -eq {proc.pid} -and $_.ExecutablePath -eq '{target}' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"],capture_output=True)
                if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
                proc=None
            if log.exists():shutil.copy2(log,out/f'game_{run}.log')
        def start(mode='--training'):
            nonlocal proc,handle,run
            run+=1
            if log.exists():shutil.copy2(log,out/f'previous_{run}.log');log.unlink()
            with (out/f'launcher_{run}.log').open('w') as stream:
                proc=subprocess.Popen([str(caster/'CCCaster_B.exe'),mode,'--no-boss-characters'],cwd=caster,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
            wait(lambda:'[SelectionPreferences] RESTORED' in logs(),'初回復元未完了',40)
            pid=subprocess.check_output(['pwsh','-NoProfile','-Command',f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"],text=True).strip()
            handle=k.OpenProcess(0x10,False,int(pid));time.sleep(1)
            initial_delay = 0 if mode == '--offline' else 2
            check(f'起動{run}はモード既定のD{initial_delay}',f'delay={initial_delay} maxRB=' in logs())
            check(f'起動{run}でキャラ選択到達',read(0x54eee8)==20)
        def saved():
            ini=configparser.ConfigParser();ini.optionxform=str
            ini.read(caster/'display.ini',encoding='utf-8')
            return {key:int(value) for key,value in ini['Display'].items()}
        def state():
            native=read(0x554140)
            value={'StageAnimation':1-read(native+0x164),'RenderWidth':read(0x54d048),'RenderHeight':read(0x54d04c)}
            for key,offset in [('CharacterFilter',0x160),('ScreenFilter',0x174),('AspectRatio',0x178),('ViewFps',0x168)]:value[key]=read(native+offset)
            return value
        def verify_restore(expected):
            actual=state();check(f'再起動{run}の実設定一致',all(actual[key]==value for key,value in expected.items() if key in actual))
            line=re.findall(r'\[SelectionPreferences\] RESTORED (.*)',logs())[-1]
            check(f'再起動{run}のHUD保持',f"hud={['NORMAL','DETAILED','HIDDEN'][expected['HudMode']]}" in line)
            check(f'再起動{run}の全画面保持',f"fullscreen={expected['Fullscreen']}" in line)
            check(f'再起動{run}で保存ファイル不変',saved()==expected)
            check(f'再起動{run}でメイン設定不変',(caster/'cccaster.ini').read_bytes()==configured)
        start()
        check('既定起動で表示設定ファイルを新規作成しない',not (caster/'display.ini').exists())
        press('start');wait(lambda:'[SelectionOptions] OPEN' in logs(),'メニュー未開放')
        press('right');wait(lambda:'[TrainingDelay] ACTIVE D=3' in logs(),'D3未適用')
        check('ディレイだけの変更では保存しない',not (caster/'display.ini').exists() and (caster/'cccaster.ini').read_bytes()==configured)
        press('down');press('right');press('left') # 背景OFFを明示
        press('down');press('right') # HUD詳細
        press('down');press('left') # 描画解像度
        wait(lambda:bool(re.search(r'\[NativeResolution\] COMPLETE applied=1',logs())),'解像度変更未完了')
        press('down');press('right') # 全画面ON
        for _ in range(4):press('down');press('right')
        expected=saved();result['first_saved']=expected
        check('8項目9値を保存',set(expected)=={'StageAnimation','HudMode','RenderWidth','RenderHeight','Fullscreen','CharacterFilter','ScreenFilter','AspectRatio','ViewFps'})
        check('OFFと詳細と全画面ONを保存',expected['StageAnimation']==0 and expected['HudMode']==1 and expected['Fullscreen']==1)
        check('ディレイ値を保存しない','Delay' not in (caster/'display.ini').read_text() and (caster/'cccaster.ini').read_bytes()==configured)
        check('保存エラーなし','SAVE success=0' not in logs())
        inspect('SAVED_FIRST')
        press('b');stop()
        start();verify_restore(expected)
        press('start');inspect('RESTORED_FULLSCREEN_DETAIL_DELAY2')
        press('down');press('right') # 背景ON
        press('down');press('right') # HUD非表示
        press('down');press('down');press('left') # 全画面OFF
        expected=saved();result['second_saved']=expected
        check('OFFへの上書きも保存',expected['StageAnimation']==1 and expected['HudMode']==2 and expected['Fullscreen']==0)
        press('b');stop()
        start('--offline');verify_restore(expected)
        press('start');inspect('RESTORED_WINDOWED_HIDDEN_OFFLINE_DELAY0')
        press('down');press('down');press('right') # 通常HUDへ戻す変更も保存
        check('HUD通常の0を保存',saved()['HudMode']==0)
        check('メイン設定は最後まで不変',(caster/'cccaster.ini').read_bytes()==configured)
        check('例外なし','[Exception]' not in logs() and '[InputGate] FAILED' not in logs())
        stop()
    except Exception as exc:result['errors'].append(str(exc))
    finally:
        if pad:pad.reset();pad.update()
        if handle:k.CloseHandle(handle)
        if proc:
            target=str(runtime/'MBAACC_1/MBAA.exe').replace("'","''")
            subprocess.run(['pwsh','-NoProfile','-Command',f"Get-CimInstance Win32_Process | Where-Object {{ $_.ParentProcessId -eq {proc.pid} -and $_.ExecutablePath -eq '{target}' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"],capture_output=True)
            if proc.poll() is None:proc.terminate();proc.wait(timeout=5)
        if log.exists():shutil.copy2(log,out/'game.log')
        for path,data in backups.items():
            if data is None:path.unlink(missing_ok=True)
            else:path.write_bytes(data)
        result['protected_unchanged']=before==preserved()
        result['passed']=not result['errors'] and result['protected_unchanged']
        (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(dict(passed=result['passed'],errors=result['errors'],logs=str(out)),ensure_ascii=False),flush=True)
    return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
