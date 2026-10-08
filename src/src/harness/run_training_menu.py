"""TrainingのInformationとメニュー開閉を仮想DS4入力・読取り専用メモリで検査する。HUDは--inspectで実画面確認。"""
import argparse
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
    out=ROOT/'test/logs'/time.strftime('training_menu_hud_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    result=dict(errors=[],checks={})
    def preserved():
        return protected(runtime) | {str(p):digest(p) for p in runtime.rglob('*.rep')}
    before=preserved()
    backups={p:p.read_bytes() if p.exists() else None for p in (caster/'cccaster.ini',caster/'Wireless Controller.ini')}
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
    def menu_items():
        menu=read(read(read(0x74d7fc)+0x10));begin,end=read(menu+0x4c),read(menu+0x50)
        keys=[]
        for p in range(begin,end,4):
            s=read(p)+0x3c;address=s+4 if read(s+0x18)<16 else read(s+4)
            keys.append(raw(address,read(s+0x14)).decode('ascii',errors='replace'))
        return menu,keys
    def latest_mask():
        values=re.findall(r'\[Hitbox\] (?:OPTION .*|OPEN) mask=(\d+)',logs())
        return int(values[-1]) if values else None
    def inspect(label):
        if args.inspect and (not args.inspect_at or label in args.inspect_at):
            print('INSPECT '+label+'; continueで再開',flush=True)
            input()
    try:
        with (out/'deploy.log').open('w') as stream:
            subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'deploy.ps1'),'-TestRoot',str(runtime)],check=True,stdout=stream,stderr=subprocess.STDOUT)
        pad=Pad();found=[]
        wait(lambda:bool(found.append(device_guid((product<<16)|0x054c)) or found[-1]),'仮想パッドなし')
        (caster/'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={found[-1]}\nDelay=2\n',encoding='utf-8')
        (caster/'Wireless Controller.ini').write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
        result['binaries']={name:digest(caster/name) for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')}
        check('最新3バイナリ',all(digest(ROOT/'build/bin'/name)==sha for name,sha in result['binaries'].items()))
        if log.exists():shutil.copy2(log,out/'previous_game.log');log.unlink()
        env={key:value for key,value in os.environ.items() if not key.startswith('CCCASTER_')}
        env['CCCASTER_HITBOX_TRACE']='1'
        with (out/'launcher.log').open('w') as stream:
            proc=subprocess.Popen([str(caster/'CCCaster_B.exe'),'--training','--no-boss-characters'],cwd=caster,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        wait(lambda:'[FastBoot] ★ CharaSelect reached!' in logs(),'キャラ選択未到達',40)
        pid=subprocess.check_output(['pwsh','-NoProfile','-Command',f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"],text=True).strip()
        handle=k.OpenProcess(0x10,False,int(pid));time.sleep(3.5)
        deadline=time.monotonic()+35
        while read(0x54eee8)!=1 or read(0x55d20b,1) or read(0x55d203,1):
            if time.monotonic()>deadline:raise RuntimeError('戦闘未到達')
            press('a')
        def native_string(address):
            buffer=address+4 if read(address+0x18)<16 else read(address+4)
            return raw(buffer,read(address+0x14)).decode('ascii',errors='replace')
        def descriptions():
            information=read(read(0x74d7fc)+0xdc)
            begin,end=read(information+0x90),read(information+0x94)
            return [(native_string(p),native_string(p+0x1c)) for p in range(begin,end,0x38)]
        expected={
            'CC_CHARACTER':'Change the character and Moon style for P1 or P2.',
            'CC_PALETTE':'Edit character colors and save or load palettes.',
            'CC_HITBOX':'Show or hide hitboxes and other collision boxes.',
        }
        inspect('COMPACT_BATTLE')
        press('start');wait(lambda:read(0x74d7fc)!=0,'Trainingメニューなし');time.sleep(.4)
        menu,keys=menu_items();result['menu_keys']=keys
        entries=descriptions();result['information']=dict(entries)
        check('3項目の説明を標準Informationへ登録',all(entries.count((key,value))==1 for key,value in expected.items()))
        check('標準項目の説明を維持',bool(dict(entries).get('PLAYER_SETTING')))
        count=len(entries)
        for index,key in enumerate(expected):
            check(key+'選択',keys[read(menu+0x40)]==key)
            inspect(key+'_INFORMATION')
            press('a');time.sleep(.5)
            # 各カスタム画面と戻りを実画面でも確認する。
            inspect(key+'_DIALOG')
            check(key+'の開閉中は本体選択を保持',read(menu+0x40)==index and read(0x74d7fc)!=0)
            if key=='CC_HITBOX':
                press('down');press('right');check('喰らい判定ON',latest_mask()==2)
            press('b');time.sleep(.3)
            if index<2:press('down')
        press('down');check('標準BATTLE SETTINGSへ移動',keys[read(menu+0x40)]=='PLAYER_SETTING')
        inspect('STANDARD_INFORMATION')
        press('a');time.sleep(.5);inspect('STANDARD_SUBMENU');press('b');time.sleep(.6)
        press('b');wait(lambda:read(0x74d7fc)==0,'練習へ戻らない');time.sleep(.5)
        inspect('COMPACT_RESTORED_WITH_BOXES')
        # 既存説明の保全、二重追加、ネイティブ文字列の所有権を開き直しで確認。
        for cycle in range(10):
            press('start');wait(lambda:read(0x74d7fc)!=0,'再開メニューなし');time.sleep(.6)
            entries=descriptions()
            check(f'再開{cycle+1}で説明数と本文を保持',len(entries)==count and all(entries.count((key,value))==1 for key,value in expected.items()))
            press('b');wait(lambda:read(0x74d7fc)==0,'再開後に練習へ戻らない')
        inspect('F1_DETAIL_BATTLE')
        press('start');wait(lambda:read(0x74d7fc)!=0,'詳細表示メニューなし');time.sleep(.3)
        inspect('DETAIL_MENU_WITH_BOXES_HIDDEN')
        press('b');wait(lambda:read(0x74d7fc)==0,'詳細表示復帰なし')
        inspect('DETAIL_RESTORED')
        inspect('F1_HIDDEN_BATTLE')
        press('start');wait(lambda:read(0x74d7fc)!=0,'HUD非表示メニューなし');time.sleep(.3)
        inspect('HIDDEN_MODE_MENU')
        press('b');wait(lambda:read(0x74d7fc)==0,'HUD非表示復帰なし')
        inspect('HIDDEN_MODE_RESTORED')
        check('例外なし','[Exception]' not in logs() and '[InputGate] FAILED' not in logs())
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
