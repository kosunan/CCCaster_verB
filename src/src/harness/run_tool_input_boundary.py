"""標準側の既存結果へ対応させ、製品D0のDI受取り→実描画読出しを補完測定。"""
import csv
import argparse
import ctypes as C
import hashlib
import json
import mmap
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import time
import psutil
from run_training_character import device_guid
from run_training_corner import protected
from run_input_latency import audit_process
from run_stock_input import ProcessReader, FIELDS, FORMAT, ROOT


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',choices=['boundary','controller'],default='boundary')
    parser.add_argument('--delay',type=int,choices=range(9),default=0)
    parser.add_argument('--label',default='measure')
    parser.add_argument('--runahead',action='store_true',help='保持予測1Fの先行表示を検証する')
    parser.add_argument('--late-depth',type=int,choices=(0,1,2),help='D0固定で描画後の実入力差替えを比較。0=なし/1=現在更新/2=直前更新')
    parser.add_argument('--late-neutral',action='store_true',help='再計算に元の入力を使い、復元だけによる差を調べる対照')
    args=parser.parse_args()
    if args.late_depth is not None and (args.delay or args.runahead or args.source!='controller'):
        parser.error('--late-depthは--source controller --delay 0、先行表示なしで使う')
    if args.late_neutral and args.late_depth not in (1,2):
        parser.error('--late-neutralは--late-depth 1/2と使う')
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    runtime=ROOT/'test/runtime/input_latency/MBAACC_1'
    game_path=(runtime/'MBAA.exe').resolve();caster=runtime/'cccaster_B'
    exact_paths={game_path,*[(caster/n).resolve() for n in ['CCCaster_B.exe','CCCaster_B_GUI.exe']]}
    for p in psutil.process_iter(['exe']):
        if p.info['exe'] and Path(p.info['exe']).resolve() in exact_paths:
            p.terminate();p.wait(10)
    out=ROOT/'test/logs'/time.strftime(f'tool_input_{args.source}_d{args.delay}_{args.label}_%Y%m%d_%H%M%S');out.mkdir(parents=True)
    product=random.SystemRandom().randrange(0x8000,0xffff)
    class Pad(vg.VDS4Gamepad):
        def target_alloc(self):
            t=vc.vigem_target_ds4_alloc();vc.vigem_target_set_vid(t,0x054c);vc.vigem_target_set_pid(t,product);return t
    pad=Pad()
    for _ in range(100):
        guid=device_guid((product<<16)|0x054c)
        if guid: break
        time.sleep(.05)
    if not guid: raise RuntimeError('仮想DS4列挙失敗')
    before=protected(runtime);(out/'protected_before.json').write_text(json.dumps(before,indent=2),encoding='utf-8')
    configs={caster/'cccaster.ini':None,caster/f'Wireless Controller__{guid}.ini':None}
    for p in configs:
        if p.exists(): configs[p]=p.read_bytes()
    memory=late=reader=proc=game=stream=None;result=dict(errors=[],out=str(out),source=args.source,delay=args.delay,runahead=args.runahead,late_depth=args.late_depth,late_neutral=args.late_neutral)
    events=[]
    corrections=[]
    late_fields='magic version command status depth trial frame worldBefore worldAfter rawBefore rawLatest actorBefore actorAfter dirBefore dirAfter queuedBefore queuedAfter motionBefore motionAfter pastPixelsDifferent pastImageEqual replayCount width height errors neutral stateBytesDifferent stateFirstDifference'.split()
    def late_status(): return dict(zip(late_fields,struct.unpack_from('<28I',late)))
    counter=C.WinDLL('kernel32').QueryPerformanceCounter
    counter.argtypes=[C.POINTER(C.c_int64)]
    def qpc():
        value=C.c_int64();counter(C.byref(value));return value.value
    def status():
        return dict(zip(['magic','version','count','capacity','frequency','status','recordSize','command','completed','bootDone','reserved'],struct.unpack_from('<4Iq6I',memory)))
    def press(name):
        pad.reset()
        if name in ('left','down'):
            pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST if name=='left' else vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH)
        else: pad.press_button({'a':vg.DS4_BUTTONS.DS4_BUTTON_SQUARE,'b':vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
                               'start':vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT}[name])
        pad.update();time.sleep(.12);pad.reset();pad.update();time.sleep(.18)
    def wait(predicate,seconds,reason):
        deadline=time.monotonic()+seconds
        while not predicate():
            if proc.poll() is not None: raise RuntimeError('起動元終了: '+reason)
            if memory is not None and status()['status'] not in (0,1): raise RuntimeError(str(status()))
            if time.monotonic()>deadline: raise RuntimeError(reason)
            time.sleep(.05)
    try:
        # verBの現行3点を一括配置。対象コピーの旧名バイナリを残さない。
        for p in caster.iterdir():
            if p.is_file() and p.suffix.lower() in ('.exe','.dll') and ('cccaster' in p.name.lower()):
                if p.name not in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll'): p.unlink()
        for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll'):
            shutil.copy2(ROOT/'build/bin'/name,caster/name)
            assert hashlib.sha256((caster/name).read_bytes()).digest()==hashlib.sha256((ROOT/'build/bin'/name).read_bytes()).digest()
        (caster/'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={guid}\n[Netplay]\nDefaultDelay=0\n',encoding='utf-8')
        (caster/f'Wireless Controller__{guid}.ini').write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
        for p in [runtime/'stock_input_boot.log',caster/'cccaster_hook_log.txt']:
            if p.exists(): p.replace(out/('previous_'+p.name))
        probe=ROOT/'test/tools/frame_observer/stock_input_probe.dll'
        artifacts=[game_path,probe,Path(__file__),Path(__file__).with_name('stock_input_probe.cpp'),Path(__file__).with_name('build_input_latency.ps1')]
        artifacts += [caster/n for n in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')]
        for p in artifacts[1:]: shutil.copy2(p,out/p.name)
        (out/'manifest.json').write_text(json.dumps(dict(mode='tool',delay=args.delay,source=args.source,device_guid=guid,runahead=args.runahead,late_depth=args.late_depth,late_neutral=args.late_neutral,
            sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}),indent=2),encoding='utf-8')
        env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
        env.update(CCCASTER_STOCK_PROBE_TOOL='1',CCCASTER_STOCK_PROBE_GUID=guid)
        env['CCCASTER_TEST_INPUT_RUNAHEAD']='1' if args.runahead else '0'
        if args.late_depth is not None:
            env['CCCASTER_TEST_LATE_INPUT_ROLLBACK']=str(args.late_depth)
            env['CCCASTER_TEST_LATE_INPUT_DIRECTORY']=str(out)
            env['CCCASTER_TEST_LATE_INPUT_NEUTRAL']='1' if args.late_neutral else '0'
        stream=(out/'launcher.log').open('w')
        proc=subprocess.Popen([str(caster/'CCCaster_B.exe'),'--training'],cwd=caster,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        def locate():
            nonlocal game
            for p in psutil.process_iter(['exe']):
                if p.info['exe'] and Path(p.info['exe']).resolve()==game_path and p.ppid()==proc.pid: game=p;return True
            return False
        wait(locate,25,'ゲーム起動失敗');reader=ProcessReader(game.pid)
        wait(lambda:reader.value(0x54eee8)==20,35,'キャラ選択へ未到達')
        subprocess.run([str(ROOT/'test/tools/frame_observer/legacy_benchmark_inject.exe'),str(game.pid),str(probe),str(game_path)],check=True,timeout=15)
        memory=mmap.mmap(-1,4096+262144*FORMAT.size,tagname=f'Local\\CCCasterStockInput_{game.pid}',access=mmap.ACCESS_WRITE)
        print(json.dumps(dict(out=str(out),pid=game.pid)),flush=True)
        wait(lambda:status()['status']==1,5,'観測器初期化失敗')
        if args.late_depth is not None:
            late=mmap.mmap(-1,4096,tagname=f'Local\\CCCasterLateInput_{game.pid}',access=mmap.ACCESS_WRITE)
            wait(lambda:late_status()['magic']==0x4c495242,5,'描画前差替え検証器が未初期化')
            assert late_status()['version']==2,'描画前差替え検証器の版が不一致'
        press('start')
        for _ in range(abs(args.delay-2)):
            if args.delay<2: press('left')
            else:
                pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST);pad.update();time.sleep(.12);pad.reset();pad.update();time.sleep(.18)
        press('b')
        for _ in range(24):
            if reader.value(0x74d8ec)==5 and reader.value(0x74d910)==5: break
            press('a')
        else: raise RuntimeError('キャラ確定失敗')
        for _ in range(70):
            if reader.value(0x74fd98)==1: break
            press('down')
        else: raise RuntimeError('ステージ確定失敗')
        press('a')
        wait(lambda:status()['bootDone'] and reader.value(0x54eee8)==1 and reader.read(0x55d20b,1)==b'\0',45,'対戦操作可能状態へ未到達')
        if args.runahead:
            wait(lambda:'[InputRunahead] restored=' in (caster/'cccaster_hook_log.txt').read_text(encoding='utf-8',errors='replace'),5,'先行表示が始まらない')
        audit=audit_process(game.pid,game_path,out)
        nop_sites={0x41f098:2,0x41f0a0:3,0x4a024e:2,0x4a027f:3,0x4a0291:3,0x4a02a2:3,0x4a02b4:3,0x4a02e9:2,0x4a02f2:3}
        assert all(reader.read(a,n)==b'\x90'*n for a,n in nop_sites.items()),'製品の標準入力抑止が不一致'
        log=(caster/'cccaster_hook_log.txt').read_text(encoding='utf-8',errors='replace')
        assert ('[InputRunahead] restored=' in log)==args.runahead,'先行表示設定の適用証拠が不一致'
        assert f'[TrainingDelay] ACTIVE D={args.delay}' in log or args.delay==2,'ディレイの適用証拠がない'
        assert audit['game']['p1character']==0 and audit['game']['p2character']==11 and audit['game']['stage']==1
        print(json.dumps(dict(battle=audit['game'],delay=args.delay,input_optimizations_retained=True)),flush=True)
        if args.source=='boundary':
            struct.pack_into('<I',memory,32,1)
            wait(lambda:status()['completed']==30,65,'測定標本未完了')
        else:
            struct.pack_into('<I',memory,32,3)
            time.sleep(1)
            for n in range(30):
                wait(lambda:reader.value(0x555140)==0 and reader.value(0x55541c)==0,3,'入力解放・待機動作へ戻らない')
                time.sleep(.25+(n%11)*.0017)
                if late is not None:
                    struct.pack_into('<I',late,8,1)
                    wait(lambda:late_status()['status']==2,3,'Present前ゲート未到達')
                kind=n%3+1
                pad.reset()
                if kind==3: pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
                else: pad.directional_pad(vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST if kind==1 else vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST)
                begin=qpc();pad.update();end=qpc()
                events.append(dict(sample=n+1,stimulus=kind,start=begin,end=end))
                if late is not None:
                    struct.pack_into('<I',late,8,2)
                    wait(lambda:late_status()['status'] in (4,5),3,'入力差替え未完了')
                    correction=late_status();corrections.append(correction)
                    if correction['status']!=4 or correction['errors']: raise RuntimeError('入力差替え失敗: '+str(correction))
                time.sleep(.09);pad.reset();pad.update();time.sleep(.45)
            wait(lambda:status()['completed']==30,5,'外部入力30標本未完了')
        result.update(completed=30,pid=game.pid,input_optimizations_retained=True)
    except Exception as e: result['errors'].append(f'{type(e).__name__}: {e}')
    finally:
        if late is not None: late.close()
        (out/'corrections.json').write_text(json.dumps(corrections,indent=2),encoding='utf-8')
        if memory:
            state=status()
            with (out/'frames.csv').open('w',newline='') as f:
                w=csv.writer(f);w.writerow(FIELDS)
                for n in range(state['count']): w.writerow(FORMAT.unpack_from(memory,4096+n*FORMAT.size))
            (out/'status.json').write_text(json.dumps(state,indent=2),encoding='utf-8');memory.close()
        if reader: reader.close()
        if game and game.is_running(): game.terminate();game.wait(10)
        if proc and proc.poll() is None: proc.terminate();proc.wait(10)
        if stream: stream.close()
        for p,original in configs.items():
            if original is None:
                if p.exists(): p.unlink()
            else: p.write_bytes(original)
        for p in [runtime/'stock_input_boot.log',caster/'stock_input_boot.log',caster/'cccaster_hook_log.txt']:
            if p.exists(): shutil.copy2(p,out/p.name)
        pad.reset();pad.update();after=protected(runtime)
        result['protected_unchanged']=before==after
        if not result['protected_unchanged']: result['errors'].append('保全対象の変更')
        (out/'protected_after.json').write_text(json.dumps(after,indent=2),encoding='utf-8')
        (out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8');print(json.dumps(result),flush=True)
        (out/'events.json').write_text(json.dumps(events,indent=2),encoding='utf-8')
    return bool(result['errors'])


if __name__=='__main__': raise SystemExit(main())
