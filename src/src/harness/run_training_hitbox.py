"""Training HITBOXを仮想DS4入力と読取り専用メモリで検査する。"""
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
import traceback
from run_training_character import ROOT, device_guid
from run_training_corner import protected, digest


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect',action='store_true')
    parser.add_argument('--inspect-at',action='append',default=[])
    parser.add_argument('--afterimages',action='store_true',help='標準メニューのImmediately MAXで残像除外と飛び道具を確認')
    parser.add_argument('--geometry',type=int,nargs=3,metavar=('WIDTH','HEIGHT','ASPECT'),help='保存済み表示設定から起動し中央・左右端を確認')
    parser.add_argument('--fullscreen',action='store_true',help='--geometryの解像度をボーダーレス表示')
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
    out=ROOT/'test/logs'/time.strftime('training_hitbox_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    result=dict(errors=[],checks={})
    before=protected(runtime)
    backups={p:p.read_bytes() if p.exists() else None for p in (caster/name for name in
        ('cccaster.ini','Wireless Controller.ini','display.ini','display.ini.bak','display.ini.tmp'))}
    proc=handle=pad=None
    log=caster/'cccaster_hook_log.txt'
    k=C.WinDLL('kernel32',use_last_error=True)
    k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
    k.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
    k.CloseHandle.argtypes=[W.HANDLE]
    def raw(addr,size):
        data=C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle,addr,data,size,None):
            raise RuntimeError(f'メモリ読取り失敗: address=0x{addr:08x} size={size}: {C.WinError(C.get_last_error())}')
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
    def menu_items(window=None):
        menu=read(read((window or read(0x74d7fc))+0x10));begin,end=read(menu+0x4c),read(menu+0x50)
        keys=[]
        for p in range(begin,end,4):
            s=read(p)+0x3c;address=s+4 if read(s+0x18)<16 else read(s+4)
            keys.append(raw(address,read(s+0x14)).decode('ascii',errors='replace'))
        return menu,keys
    def open_hitbox():
        press('start');wait(lambda:read(0x74d7fc)!=0,'Trainingメニューなし');time.sleep(.4)
        menu,keys=menu_items();result['menu_keys']=keys
        target=keys.index('CC_HITBOX')
        for _ in range(len(keys)):
            if read(menu+0x40)==target:break
            press('down')
        offset=len(logs());press('a');wait(lambda:'[Hitbox] OPEN' in logs()[offset:],'HITBOX未開放')
        return menu,target
    def latest_mask():
        values=re.findall(r'\[Hitbox\] (?:OPTION .*|OPEN) mask=(\d+)',logs())
        return int(values[-1]) if values else None
    def inspect(label):
        if args.inspect and (not args.inspect_at or label in args.inspect_at):
            print('INSPECT '+label+'; continueで再開',flush=True);input()
    def set_circuit(setting):
        press('start');wait(lambda:read(0x74d7fc)!=0,'Trainingメニューなし');time.sleep(.4)
        menu,keys=menu_items()
        for _ in range(len(keys)):
            if keys[read(menu+0x40)]=='PLAYER_SETTING':break
            press('down')
        press('a');wait(lambda:read(read(0x74d7fc)+0xc8)!=0,'BATTLE SETTINGSなし');time.sleep(.5)
        menu,keys=menu_items(read(read(0x74d7fc)+0xc8))
        for _ in range(len(keys)):
            if keys[read(menu+0x40)]=='MAGIC_CIRCUIT':break
            press('down')
        check('MAGIC CIRCUIT項目を選択',keys[read(menu+0x40)]=='MAGIC_CIRCUIT')
        item=read(read(menu+0x4c)+keys.index('MAGIC_CIRCUIT')*4)
        # 0x42F8F0のSelectElement+0x58は選択値。通常入力のみで変更する。
        for _ in range(4):
            if read(item+0x58)==setting:break
            press('right')
        check('MAGIC CIRCUITの選択値',read(item+0x58)==setting)
        press('b');time.sleep(.5);press('b');wait(lambda:read(0x74d7fc)==0,'設定から戻らない')
        check('標準ゲーム設定に反映',read(0x77c1fc)==setting)
    try:
        with (out/'deploy.log').open('w') as stream:
            subprocess.run(['pwsh','-NoProfile','-File',str(ROOT/'deploy.ps1'),'-TestRoot',str(runtime)],check=True,stdout=stream,stderr=subprocess.STDOUT)
        pad=Pad();found=[]
        wait(lambda:bool(found.append(device_guid((product<<16)|0x054c)) or found[-1]),'仮想パッドなし')
        (caster/'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={found[-1]}\nDelay=2\n',encoding='utf-8')
        (caster/'Wireless Controller.ini').write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
        if args.geometry:
            width,height,aspect=args.geometry
            (caster/'display.ini').write_text(f'[Display]\nRenderWidth={width}\nRenderHeight={height}\nAspectRatio={aspect}\nFullscreen={int(args.fullscreen)}\n',encoding='utf-8')
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
        if args.geometry:
            width,height,aspect=args.geometry
            check('指定解像度で起動',read(0x54d048)==width and read(0x54d04c)==height)
            check('指定アスペクト比で起動',read(read(0x554140)+0x178)==aspect)
        deadline=time.monotonic()+35
        while read(0x54eee8)!=1 or read(0x55d20b,1) or read(0x55d203,1):
            if time.monotonic()>deadline:raise RuntimeError('戦闘未到達')
            press('a')
        menu,target=open_hitbox();check('初期値は全OFF',latest_mask()==0)
        mark=len(logs());press('a',.8)
        check('A保持で1回だけ切替',len(re.findall(r'\[Hitbox\] OPTION',logs()[mark:]))==1 and latest_mask()==1)
        press('a');check('AでOFFへ戻る',latest_mask()==0)
        for i in range(6):
            press('right');check(f'独立ON項目{i}',latest_mask()==(1<<(i+1))-1)
            if i<5:press('down')
        check('サブメニュー入力を本体へ渡さない',read(menu+0x40)==target and read(0x74d7fc)!=0)
        inspect('ALL_ON_MENU')
        press('b');press('b');wait(lambda:read(0x74d7fc)==0,'練習へ戻らない')
        time.sleep(.6);inspect('STANDING_BOXES')
        if args.geometry:
            result['geometry']=dict(width=width,height=height,aspect=aspect,fullscreen=args.fullscreen,samples=[])
            for label,direction in [('CENTER',None),('LEFT_EDGE','left'),('RIGHT_EDGE','right')]:
                if direction:press(direction+'+reset',.12,.7)
                positions=[int.from_bytes(raw(addr,4),'little',signed=True) for addr in (0x555238,0x555d34)]
                result['geometry']['samples'].append(dict(label=label,positions=positions,frame=read(0x55d1cc)))
                if direction:
                    sign=-1 if direction=='left' else 1
                    check(label+'の壁際に到達',positions==[sign*45056,sign*61440])
                inspect(label)
            press('reset',.12,.7)
        if args.afterimages:
            set_circuit(2)
            # Immediately MAXは299.9%へ補充する。標準の攻撃でMAXへ移行させる。
            press('right',.65);press('c',.1,.8)
            wait(lambda:read(0x555218,1) in (1,2,3),'MAX状態なし')
            result['max_circuit_state']=read(0x555218,1)
            time.sleep(.5);inspect('MAX_AFTERIMAGES')
        press('save');press('left',.65);press('right',.25)
        press('down',.5);press('up',.08,.7)
        for key in ('a','b','c','d','down+a','down+b','down+c','down+d','up+c'):
            press(key,.3,.4)
        # 236A、214A。ゲームの通常入力だけで飛び道具等のデータを観測する。
        for diagonal,forward in [('dr','right'),('dl','left')]:
            press('down',.04,.02);press(diagonal,.04,.02);press(forward+'+a',.06,.7)
        if args.afterimages:
            # MAX中も既存の本体と飛び道具の判定を残す。236B/236Cも通常入力で試す。
            for button in ('b','c'):
                press('down',.04,.02);press('dr',.04,.02);press('right+'+button,.06,1.2)
        press('reset');time.sleep(.5)
        check('FN復元後も表示が続く',latest_mask()==63)
        open_hitbox();check('開き直しで保持',latest_mask()==63)
        # 前回の選択行5から逆順にOFF。
        expected=63
        for i in range(5,-1,-1):
            press('left');expected&=~(1<<i);check(f'独立OFF項目{i}',latest_mask()==expected)
            if i:press('up')
        inspect('ALL_OFF_MENU')
        press('b');press('b');wait(lambda:read(0x74d7fc)==0,'OFF後に練習へ戻らない')
        mark=len(logs());time.sleep(.5);check('全OFFで収集と描画を停止','[Hitbox] FRAME' not in logs()[mark:])
        lines=re.findall(r'\[Hitbox\] FRAME f=(\d+) mask=(\d+) found=([\d,]+) drawn=(\d+) checked=(\d+) mismatch=(\d+) zoom=([\d.]+)',logs())
        check('座標比較標本あり',len(lines)>60)
        check('全標本がネイティブ座標変換と一致',all(int(x[5])==0 for x in lines))
        result['native_boxes_checked']=sum(int(x[4]) for x in lines)
        result['max_found']=[max(int(x[2].split(',')[i]) for x in lines) for i in range(6)]
        check('攻撃・喰らい・押し合いの実矩形あり',all(n>0 for n in result['max_found'][:3]))
        check('ONの種類だけ描画',all(int(x[3])==sum(int(n) for i,n in enumerate(x[2].split(',')) if int(x[1])&(1<<i)) for x in lines))
        if args.geometry:
            from fractions import Fraction
            ratio={0:Fraction(4,3),1:Fraction(width,height),2:Fraction(4,3),3:Fraction(16,9),4:Fraction(16,10),5:Fraction(5,4),6:Fraction(15,9)}[aspect]
            w=width if ratio<=Fraction(4,3) else int(width*Fraction(4,3)/ratio)
            h=height if ratio>=Fraction(4,3) else int(height*ratio/Fraction(4,3))
            x,y=(width-w)//2,(height-h)//2
            expected=f'{x},{y},{x+w},{y+h}'
            actual=re.findall(r'surface=(\d+)x(\d+) aspect=(\d+) image=([\d,]+)',logs())
            result['geometry']['expected_image']=expected
            check('全標本で黒帯を除いた合成先と一致',len(actual)>60 and all(item==(str(width),str(height),str(aspect),expected) for item in actual))
        if args.afterimages:
            effects=re.findall(r'afterimages=(\d+) effect_boxes=(\d+) afterimage_boxes=(\d+)',logs())
            result['afterimage_samples']=sum(int(x[0])>0 for x in effects)
            result['max_afterimages']=max(int(x[0]) for x in effects)
            result['max_effect_boxes']=max(int(x[1]) for x in effects)
            check('実際の残像を60標本以上観測',result['afterimage_samples']>=60)
            check('残像のHITBOXは全標本0',all(int(x[2])==0 for x in effects))
            check('飛び道具等の実オブジェクト矩形を維持',result['max_effect_boxes']>0)
            check('MAX残像中も本体の矩形あり',any(int(x[1])==63 and int(x[3])>0 and int(x[4])>0 for x in lines))
        check('例外なし','[Exception]' not in logs() and '[InputGate] FAILED' not in logs())
    except Exception as exc:
        result['errors'].append(str(exc))
        result['traceback']=traceback.format_exc()
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
