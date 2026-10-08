"""仮想DS4の通常入力と読取り専用メモリで、標準キャラ選択のボス列を検証する。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
import random
import shutil
import subprocess
import time
from collections import deque
from run_training_character import device_guid, ROOT
from run_training_corner import protected, digest

BOSSES = [16,32,53,58,59,72,73,85]
CELLS = [45,46,47,48,50,51,52,53]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect',action='store_true')
    parser.add_argument('--disabled',action='store_true')
    parser.add_argument('--offline',action='store_true')
    parser.add_argument('--loading-inspect',action='store_true',help='ロード画面で入力を止め、目視確認用の時間を確保する')
    parser.add_argument('--characters',type=int,nargs='+',choices=BOSSES,default=BOSSES)
    parser.add_argument('--stage-inspect',action='store_true',help='ステージ決定前にcontinueを待つ')
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
    out=ROOT/'test/logs'/time.strftime('boss_select_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    result=dict(errors=[],cases=[],runtime=str(runtime))
    before=protected(runtime)
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
    def wait(condition,reason,seconds=20):
        deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            if condition():return
            if proc and proc.poll() is not None:raise RuntimeError('起動元終了: '+reason)
            time.sleep(.03)
        raise RuntimeError(reason)
    buttons=dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE,b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,start=vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT)
    directions=dict(left=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,right=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
                    up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,down=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH)
    def press(key):
        pad.reset()
        if key in buttons:pad.press_button(buttons[key])
        else:pad.directional_pad(directions[key])
        pad.update();time.sleep(.09);pad.reset();pad.update();time.sleep(.15)
    def navigate(target):
        grid=read(0x77181c)
        ids=[read(grid+i*24+8) for i in range(63)]
        start=read(0x74d8f8)
        queue=deque([(start,[])])
        seen={start}
        while queue:
            cell,path=queue.popleft()
            if cell==target:
                for direction in path:press(direction)
                if read(0x74d8f8)!=target:raise RuntimeError(f'カーソル不一致: {start}->{target}, {path}, actual={read(0x74d8f8)}')
                return
            for key,delta in [('down',9),('up',-9),('right',1),('left',-1)]:
                nxt=cell
                for _ in range(63):
                    nxt=(nxt+delta)%63 if abs(delta)==9 else (cell//9)*9+(nxt+delta)%9
                    if ids[nxt]!=0xffffffff:break
                if nxt not in seen:seen.add(nxt);queue.append((nxt,path+[key]))
        raise RuntimeError('到達できないセル')
    def return_select():
        press('start');wait(lambda:read(0x74d7fc)!=0,'Trainingメニューなし');time.sleep(.4)
        menu=read(read(read(0x74d7fc)+0x10));begin,end=read(menu+0x4c),read(menu+0x50)
        keys=[]
        for p in range(begin,end,4):
            s=read(p)+0x3c;address=s+4 if read(s+0x18)<16 else read(s+4)
            keys.append(raw(address,read(s+0x14)).decode('ascii',errors='replace'))
        target=keys.index('CHARACTER_SELECT')
        for _ in range(len(keys)):
            if read(menu+0x40)==target:break
            press('down')
        press('a');time.sleep(.4);press('up');press('a')
        wait(lambda:read(0x54eee8)==20,'キャラ選択へ復帰しない');time.sleep(3)
    try:
        with (out/'deploy.log').open('w') as stream:
            subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'deploy.ps1'),'-TestRoot',str(runtime)],check=True,stdout=stream,stderr=subprocess.STDOUT)
        pad=Pad();found=[]
        wait(lambda:bool(found.append(device_guid((product<<16)|0x054c)) or found[-1]),'仮想パッドなし')
        (caster/'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={found[-1]}\nDelay=2\n',encoding='utf-8')
        (caster/'Wireless Controller.ini').write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
        result['binaries']={name:digest(caster/name) for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')}
        if any(digest(ROOT/'build/bin'/name)!=sha for name,sha in result['binaries'].items()):raise RuntimeError('配置SHA不一致')
        if log.exists():shutil.copy2(log,out/'previous_game.log');log.unlink()
        env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
        env.update(CCCASTER_TRAINING_TRACE='1',CCCASTER_INPUT_DIAGNOSTIC='1')
        with (out/'launcher.log').open('w') as stream:
            proc=subprocess.Popen([str(caster/'CCCaster_B.exe'),'--offline' if args.offline else '--training',
                '--no-boss-characters' if args.disabled else '--boss-characters'],cwd=caster,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        wait(lambda:'[FastBoot] ★ CharaSelect reached!' in logs(),'キャラ選択未到達',40)
        pid=subprocess.check_output(['pwsh','-NoProfile','-Command',f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"],text=True).strip()
        handle=k.OpenProcess(0x10,False,int(pid));time.sleep(3.5)
        grid=read(0x77181c);row=[[read(grid+i*24+j*4) for j in range(6)] for i in CELLS]
        result['row']=row
        allowed=not args.disabled and not args.offline
        if [c[2] for c in row]!=(BOSSES if allowed else [0xffffffff]*8):raise RuntimeError('ボス列の有効条件不一致')
        if allowed:
            for i,character in enumerate(args.characters):
                navigate(CELLS[BOSSES.index(character)]);time.sleep(.5)
                if read(0x74d8fc)!=character:raise RuntimeError('キャラ番号不一致')
                if args.inspect and i in (0,1):
                    print(f'INSPECT character={character}; continueで再開',flush=True)
                    input()
                press('a');wait(lambda:read(0x74d8ec)==2,'ボスのカラー選択に未到達')
                moon=0 if character==32 else 8 if character==53 else 9
                if read(0x74d900)!=moon:raise RuntimeError('専用ムーン不一致')
                deadline=time.monotonic()+35
                loading={}
                inspected=False
                stage_inspected=False
                while read(0x54eee8)!=1 or read(0x55d20b,1) or read(0x55d203,1):
                    if time.monotonic()>deadline:raise RuntimeError('戦闘未到達')
                    if args.stage_inspect and not stage_inspected and read(0x54eee8)==20 and read(0x74d8ec)>=4 and read(0x74d910)>=4:
                        stage_inspected=True;print('STAGE_READY; continueで決定',flush=True);input();deadline=time.monotonic()+35
                    if read(0x54eee8) in (8,13):
                        for key,address in dict(cut=0x74fa44,portrait=0x74fbf0,flash=0x74fbe4,name=0x74fbd4,
                                                subtitle=0x74fac0,color=0x74fad8,command=0x74facc).items():
                            loading[key]=max(loading.get(key,0),read(address))
                        if args.loading_inspect and not inspected:
                            inspected=True;print(f'LOADING character={character}',flush=True);time.sleep(5)
                    press('a')
                time.sleep(.5)
                data=dict(character=read(0x74d840),moon=read(0x74d84c),actor=read(0x555135,1),
                    assets=[read(0x557d30+i*4) for i in range(3)],images=[read(0x5642c8+i*4) for i in range(7)])
                data['loading']=loading
                result['cases'].append(data);print(json.dumps(data),flush=True)
                if data['character']!=character or data['moon']!=moon or data['actor']!=character or not data['assets'][0]:raise RuntimeError('戦闘キャラまたは資産が不一致')
                if len(loading)!=7 or not all(loading.values()):raise RuntimeError('ロード画面の7画像が未取得')
                if i+1<len(args.characters):return_select()
        if '[Exception]' in logs() or '[InputGate] FAILED' in logs():raise RuntimeError('実ゲーム例外')
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
        result['protected_unchanged']=before==protected(runtime)
        result['passed']=not result['errors'] and result['protected_unchanged']
        (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(dict(passed=result['passed'],errors=result['errors'],logs=str(out)),ensure_ascii=False),flush=True)
    return 0 if result['passed'] else 1

if __name__=='__main__':raise SystemExit(main())
