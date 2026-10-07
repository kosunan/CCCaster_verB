"""独立コピーで入力遅延を観測。仮想DS4を操作し、CPUの生入力→Present要求を記録。"""
import argparse
import atexit
import ctypes as C
from ctypes import wintypes as W
import csv
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
import uuid
import psutil
from run_training_character import device_guid
from run_training_corner import protected

ROOT=Path(__file__).resolve().parents[3]

def audit_process(pid,game_path,out):
    """標準入力関数の機械語一致、待機パッチ、対戦条件を読み取り専用で保存。"""
    kernel=C.WinDLL('kernel32',use_last_error=True)
    kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];kernel.OpenProcess.restype=W.HANDLE
    kernel.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
    kernel.CloseHandle.argtypes=[W.HANDLE]
    handle=kernel.OpenProcess(0x1010,False,pid)
    if not handle: raise C.WinError(C.get_last_error())
    def read(address,size):
        buffer=C.create_string_buffer(size)
        if not kernel.ReadProcessMemory(handle,address,buffer,size,None): raise C.WinError(C.get_last_error())
        return buffer.raw
    try:
        binary=game_path.read_bytes()
        pe=struct.unpack_from('<I',binary,0x3c)[0]
        count=struct.unpack_from('<H',binary,pe+6)[0]
        table=pe+24+struct.unpack_from('<H',binary,pe+20)[0]
        sections=[struct.unpack_from('<4I',binary,table+i*40+8) for i in range(count)]
        def original(address,size):
            rva=address-0x400000
            for virtual_size,virtual_address,raw_size,raw_address in sections:
                if virtual_address<=rva and rva+size<=virtual_address+raw_size:
                    offset=raw_address+rva-virtual_address
                    return binary[offset:offset+size]
            raise ValueError(hex(address))
        functions=[(0x410870,181),(0x4106d0,411),(0x49f9e0,232),(0x49fad0,117),
                   (0x49fca0,365),(0x4a0090,26),(0x4a00d0,96),(0x41f080,61),(0x4a0230,199)]
        data=dict(functions={hex(a):dict(stock_code_equal=read(a,n)==original(a,n),
                        sha256=hashlib.sha256(read(a,n)).hexdigest()) for a,n in functions})
        data['code']={hex(a):read(a,n).hex() for a,n in [(0x41f098,2),(0x41f0a0,3),(0x4a024e,2),
            (0x4a027f,3),(0x4a0291,3),(0x4a02a2,3),(0x4a02b4,3),(0x4a02e9,2),(0x4a02f2,3),
            (0x41fdfb,16),(0x42007e,6),(0x4333f7,6),(0x54d2c0,20)]}
        data['game']={name:int.from_bytes(read(a,4),'little') for name,a in
            [('p1character',0x74d8fc),('p2character',0x74d920),('stage',0x74fd98),('mode',0x54eee8),('world',0x55d1d4)]}
        (out/'runtime_audit.json').write_text(json.dumps(data,indent=2),encoding='utf-8')
        return data
    finally: kernel.CloseHandle(handle)

def native_pad_number(guid):
    class Instance(C.Structure):
        _fields_=[('size',W.DWORD),('instance',C.c_ubyte*16),('product',C.c_ubyte*16),('type',W.DWORD),
                  ('name',W.WCHAR*260),('product_name',W.WCHAR*260),('ff',C.c_ubyte*16),('usage_page',W.WORD),('usage',W.WORD)]
    callback_type=C.WINFUNCTYPE(W.BOOL,C.POINTER(Instance),C.c_void_p)
    devices=[]
    @callback_type
    def callback(pointer,context):
        d=pointer.contents
        devices.append((str(uuid.UUID(bytes_le=bytes(d.instance))).upper(),d.name))
        return True
    create=C.WinDLL('dinput8').DirectInput8Create
    create.argtypes=[W.HINSTANCE,W.DWORD,C.c_void_p,C.POINTER(C.c_void_p),C.c_void_p]
    create.restype=C.c_long
    kernel=C.WinDLL('kernel32');kernel.GetModuleHandleW.restype=W.HMODULE
    interface=C.c_void_p()
    iid=C.create_string_buffer(uuid.UUID('BF798031-483A-4DA2-AA99-5D64ED369700').bytes_le)
    assert create(kernel.GetModuleHandleW(None),0x800,iid,C.byref(interface),None)>=0
    table=C.cast(interface,C.POINTER(C.POINTER(C.c_void_p))).contents
    try:
        enum=C.WINFUNCTYPE(C.c_long,C.c_void_p,W.DWORD,callback_type,C.c_void_p,W.DWORD)(table[4])
        assert enum(interface,4,callback,None,1)>=0
    finally: C.WINFUNCTYPE(W.ULONG,C.c_void_p)(table[2])(interface)
    return next((i+1,name) for i,(instance,name) in enumerate(devices) if instance==guid)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode',choices=['native','tool','tool-native-input'],default='native')
    parser.add_argument('--delay',type=int,default=0)
    parser.add_argument('--stage',type=int,default=1,help='tool系は通常の方向入力でこのステージを選ぶ')
    parser.add_argument('--binaries',type=Path,default=ROOT/'build/bin',help='比較中に固定するverBの製品3点')
    args=parser.parse_args()
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    runtime=ROOT/'test/runtime/input_latency/MBAACC_1'
    game_path=(runtime/'MBAA.exe').resolve()
    if any(p.info['exe'] and Path(p.info['exe']).resolve()==game_path for p in psutil.process_iter(['exe'])):
        raise RuntimeError('同じ独立コピーのゲームが起動中。先の測定終了後に実行すること')
    out=ROOT/'test/logs'/time.strftime(f'input_latency_{args.mode}_d{args.delay}_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    product=random.SystemRandom().randrange(0x8000,0xffff)
    class Pad(vg.VDS4Gamepad):
        def target_alloc(self):
            target=vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target,0x054c)
            vc.vigem_target_set_pid(target,product)
            return target
    pad=Pad()
    guid=None
    for _ in range(100):
        guid=device_guid((product<<16)|0x054c)
        if guid: break
        time.sleep(.05)
    if not guid: raise RuntimeError('仮想DS4の列挙失敗')
    original_protected=protected(runtime)
    native_ini=runtime/'System/_KeyConfig.ini'
    native_original=native_ini.read_bytes()
    atexit.register(native_ini.write_bytes,native_original)
    # 対照側も同じ標準デバイス設定。toolでは9命令のNOPによりゲーム入力へは混ざらない。
    number,name=native_pad_number(guid)
    values=[17,18,19,20,8,0,1,2,3,4,5,6]+[0]*12
    native_ini.write_text('[KeyConfig]\nNoKeyboard=1\n'+''.join(
        f'P{side}_Num={number if side==0 else 0}\nP{side}_Name={name if side==0 else ""}\n'+
        ''.join(f'P{side}_Val{i:02}={value if side==0 else 0}\n' for i,value in enumerate(values))
        for side in range(4)),encoding='ascii')
    caster=runtime/'cccaster_B'
    if args.mode!='native':
        caster.mkdir(exist_ok=True)
        for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll'):
            shutil.copy2(args.binaries/name,caster/name)
        (caster/'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={guid}\n[Netplay]\nDefaultDelay={args.delay}\n',encoding='utf-8')
        (caster/f'Wireless Controller__{guid}.ini').write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
    before=protected(runtime)
    (out/'protected_before.json').write_text(json.dumps(before,indent=2),encoding='utf-8')
    env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
    env['CCCASTER_INPUT_DIAGNOSTIC']='1'
    if args.mode=='native': env['CCCASTER_LATENCY_NATIVE']='1'
    if args.mode=='tool-native-input': env['CCCASTER_LATENCY_NATIVE_INPUT']='1'
    artifacts=[runtime/'MBAA.exe',ROOT/'test/tools/frame_observer/input_latency_probe.dll',
               ROOT/'src/src/harness/input_latency_probe.cpp',ROOT/'src/src/harness/run_input_latency.py']
    if args.mode!='native': artifacts += [caster/name for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')]
    (out/'manifest.json').write_text(json.dumps(dict(mode=args.mode,requested_delay=args.delay,requested_stage=args.stage,device_guid=guid,
        native_pad_number=number,sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}),indent=2),encoding='utf-8')
    for source in (runtime/'input_latency_boot.log',caster/'cccaster_hook_log.txt'):
        if source.exists(): source.replace(out/('previous_'+source.name))
    stream=(out/'launcher.log').open('w')
    if args.mode=='native':
        startup=subprocess.STARTUPINFO()
        startup.dwFlags=subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow=1
        proc=subprocess.Popen([str(runtime/'MBAA.exe')],cwd=runtime,env=env,stdout=stream,stderr=subprocess.STDOUT,startupinfo=startup)
    else:
        proc=subprocess.Popen([str(caster/'CCCaster_B.exe'),'--training'],cwd=caster,env=env,stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
    def close_launcher():
        if proc.poll() is None:
            proc.terminate();proc.wait(10)
    atexit.register(close_launcher)
    game=None
    for _ in range(300):
        for p in psutil.process_iter(['exe']):
            if p.info['exe'] and Path(p.info['exe']).resolve()==game_path and (args.mode=='native' and p.pid==proc.pid or args.mode!='native' and p.ppid()==proc.pid): game=p
        if game: break
        time.sleep(.1)
    if not game: raise RuntimeError('ゲーム起動失敗')
    def close_game():
        if game.is_running(): game.terminate();game.wait(10)
    atexit.register(close_game)
    # 製品の起動パッチを先に完了させる。nativeはダイアログ待ち中でも注入できる。
    if args.mode!='native': time.sleep(5)
    subprocess.run([str(ROOT/'test/tools/frame_observer/legacy_benchmark_inject.exe'),str(game.pid),
                    str(ROOT/'test/tools/frame_observer/input_latency_probe.dll'),str(runtime/'MBAA.exe')],check=True)
    memory=mmap.mmap(-1,4096+262144*56,tagname=f'Local\\CCCasterInputLatency_{game.pid}',access=mmap.ACCESS_READ)
    k=C.WinDLL('kernel32')
    k.QueryPerformanceCounter.argtypes=[C.POINTER(C.c_int64)]
    k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
    k.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
    k.CloseHandle.argtypes=[W.HANDLE]
    read_handle=k.OpenProcess(0x1010,False,game.pid)
    def game_value(address):
        value=C.c_uint32()
        if not k.ReadProcessMemory(read_handle,address,C.byref(value),4,None): raise C.WinError()
        return value.value
    def qpc():
        t=C.c_int64(); k.QueryPerformanceCounter(C.byref(t)); return t.value
    def status():
        magic,version,count,capacity,freq,state,size=struct.unpack_from('<4Iq2I',memory)
        last=struct.unpack_from('<q12I',memory,4096+(count-1)*56) if count else None
        return dict(count=count,frequency=freq,status=state,last=last)
    commands=out/'cmd.json'
    buttons=dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE,b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,c=vg.DS4_BUTTONS.DS4_BUTTON_CIRCLE,
                 d=vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE,start=vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT)
    dirs=dict(left=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,right=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
              up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,down=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH)
    events=[]
    def set_pad(name):
        pad.reset()
        if name in buttons: pad.press_button(buttons[name])
        elif name in dirs: pad.directional_pad(dirs[name])
        start=qpc(); pad.update(); end=qpc()
        events.append(dict(command=name,start=start,end=end,observation=status()))
    def press(name):
        set_pad(name);time.sleep(.12);set_pad('neutral');time.sleep(.18)
    print(json.dumps(dict(out=str(out),pid=game.pid,guid=guid,status=status())),flush=True)
    def save():
        state=status()
        with (out/'frames.csv').open('w',newline='') as f:
            writer=csv.writer(f); writer.writerow(['qpc','phase','loop','world','mode','intro','rawDirection','rawButtons','converted','actorDirection','x','y','pattern'])
            for n in range(state['count']): writer.writerow(struct.unpack_from('<q12I',memory,4096+n*56))
        (out/'events.json').write_text(json.dumps(events,indent=2),encoding='utf-8')
        (out/'status.json').write_text(json.dumps(state,indent=2),encoding='utf-8')
        for source in (runtime/'input_latency_boot.log',caster/'cccaster_hook_log.txt'):
            if source.exists(): shutil.copy2(source,out/source.name)
    try:
        while game.is_running():
            if commands.exists():
                try: data=json.loads(commands.read_text())
                except (json.JSONDecodeError, PermissionError):
                    time.sleep(.04)
                    continue
                commands.unlink()
                cmd=data['command']
                if cmd in buttons or cmd in dirs:
                    for _ in range(data.get('repeat',1)):
                        set_pad(cmd); time.sleep(data.get('duration',.12)); set_pad('neutral'); time.sleep(.18)
                elif cmd in ('prepare','characters'):
                    if args.mode=='native': raise RuntimeError('標準の高速起動は自動で対戦まで進む')
                    if status()['last'][4]!=20: raise RuntimeError('キャラ選択中でない')
                    press('start')
                    for _ in range(abs(args.delay-2)): press('left' if args.delay<2 else 'right')
                    press('b')
                    for _ in range(24):
                        if game_value(0x74d8ec)==5 and game_value(0x74d910)==5: break
                        press('a')
                    else: raise RuntimeError('両キャラ確定に未到達')
                    if cmd=='prepare':
                        for _ in range(70):
                            if game_value(0x74fd98)==args.stage: break
                            press('down')
                        else: raise RuntimeError('固定ステージ選択失敗')
                        press('a')
                        for _ in range(12):
                            if status()['last'][4]==1: break
                            time.sleep(.5);press('a')
                elif cmd=='measure':
                    deadline=time.monotonic()+45
                    while not (status()['last'] and status()['last'][4:6]==(1,0)):
                        if time.monotonic()>deadline: raise RuntimeError('操作可能な対戦画面に未到達')
                        time.sleep(.05)
                    for n in range(30):
                        set_pad('left' if n%2==0 else 'right'); time.sleep(.16)
                        set_pad('neutral'); time.sleep(.22+(n%5)*.007)
                    audit_process(game.pid,game_path,out)
                    save()
                elif cmd=='save': save()
                elif cmd=='exit': break
                print(json.dumps(dict(command=cmd,status=status())),flush=True)
            time.sleep(.02)
    finally:
        save(); pad.reset(); pad.update(); memory.close(); k.CloseHandle(read_handle)
        if game.is_running(): game.terminate(); game.wait(10)
        try: proc.wait(10)
        except subprocess.TimeoutExpired: proc.terminate(); proc.wait(10)
        stream.close()
        after=protected(runtime)
        (out/'protected_after.json').write_text(json.dumps(after,indent=2),encoding='utf-8')
        native_ini.write_bytes(native_original)
        final=protected(runtime)
        baseline=original_protected if args.mode=='native' else {**before,str(native_ini):original_protected[str(native_ini)]}
        result=dict(protected_unchanged=baseline==final,game_saved_ini=before!=after,
                    mode=args.mode,delay=args.delay,pid=game.pid,out=str(out))
        (out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps(result),flush=True)

if __name__=='__main__': main()
