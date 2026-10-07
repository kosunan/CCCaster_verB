"""製品DLLなしの標準ゲームで、DI受取りバッファへの入力→実描画読出しを測定する。"""
import csv
import ctypes as C
from ctypes import wintypes as W
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
from run_input_latency import native_pad_number

ROOT=Path(__file__).resolve().parents[3]
FIELDS=('qpc phase loop world sample stimulus mode intro deviceIndex result buffer pov buttons '
        'rawDirection rawButtons converted actorDirection sequence state animation x y actor '
        'renderX renderY renderAnimation actorButtons').split()
FORMAT=struct.Struct('<q26I')
HOOKS=[0x40e390,0x41f0c0,0x4bdd03,0x4108ef,0x46d90e,0x41af48,0x41a390,0x4618c0,0x46ddf0]


class ProcessReader:
    def __init__(self,pid):
        self.kernel=C.WinDLL('kernel32',use_last_error=True)
        self.kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD]
        self.kernel.OpenProcess.restype=W.HANDLE
        self.kernel.ReadProcessMemory.argtypes=[W.HANDLE,C.c_void_p,C.c_void_p,C.c_size_t,C.c_void_p]
        self.kernel.CloseHandle.argtypes=[W.HANDLE]
        self.handle=self.kernel.OpenProcess(0x1010,False,pid)
        if not self.handle: raise C.WinError(C.get_last_error())
    def read(self,address,size):
        data=C.create_string_buffer(size)
        if not self.kernel.ReadProcessMemory(self.handle,address,data,size,None):
            raise C.WinError(C.get_last_error())
        return data.raw
    def value(self,address): return int.from_bytes(self.read(address,4),'little')
    def close(self): self.kernel.CloseHandle(self.handle)


def audit(reader,game_path,pid):
    """.text全域を標準EXEと照合。例外はこの観測器の7箇所のJMP各5bytesだけ。"""
    binary=game_path.read_bytes()
    pe=struct.unpack_from('<I',binary,0x3c)[0]
    count=struct.unpack_from('<H',binary,pe+6)[0]
    table=pe+24+struct.unpack_from('<H',binary,pe+20)[0]
    sections=[]
    for i in range(count):
        offset=table+i*40
        name=binary[offset:offset+8].rstrip(b'\0').decode('ascii')
        if name!='.text': continue
        virtual_size,rva,raw_size,raw_offset=struct.unpack_from('<4I',binary,offset+8)
        size=min(virtual_size,raw_size)
        original=binary[raw_offset:raw_offset+size]
        actual=reader.read(0x400000+rva,size)
        allowed={site+j for site in HOOKS for j in range(5)}
        differences=[0x400000+rva+j for j,(a,b) in enumerate(zip(original,actual)) if a!=b]
        unexpected=[hex(a) for a in differences if a not in allowed]
        normalized=bytearray(actual)
        for site in HOOKS:
            index=site-(0x400000+rva)
            normalized[index:index+5]=original[index:index+5]
        sections.append(dict(name=name,bytes=size,unexpected_differences=unexpected,
                             stock_sha256=hashlib.sha256(original).hexdigest(),
                             normalized_sha256=hashlib.sha256(normalized).hexdigest()))
    modules=sorted({entry.path for entry in psutil.Process(pid).memory_maps() if entry.path})
    hooks={hex(site):reader.read(site,5).hex() for site in HOOKS}
    game={name:reader.value(address) for name,address in [
        ('mode',0x54eee8),('world',0x55d1d4),('p1character',0x74d8fc),('p2character',0x74d920),
        ('stage',0x74fd98),('p1device',0x74da18)]}
    return dict(sections=sections,hooks=hooks,modules=modules,game=game,
        passed=bool(sections) and all(s['stock_sha256']==s['normalized_sha256'] for s in sections)
        and all(code.startswith('e9') for code in hooks.values())
        and not any('libcccaster_hook.dll' in path.lower() for path in modules))


def main():
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    runtime=ROOT/'test/runtime/input_latency/MBAACC_1'
    game_path=(runtime/'MBAA.exe').resolve()
    if any(p.info['exe'] and Path(p.info['exe']).resolve()==game_path for p in psutil.process_iter(['exe'])):
        raise RuntimeError('同じ独立コピーが起動中')
    out=ROOT/'test/logs'/time.strftime('stock_input_boundary_%Y%m%d_%H%M%S')
    out.mkdir(parents=True)
    product=random.SystemRandom().randrange(0x8000,0xffff)
    class Pad(vg.VDS4Gamepad):
        def target_alloc(self):
            target=vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target,0x054c);vc.vigem_target_set_pid(target,product)
            return target
    pad=Pad()
    for _ in range(100):
        guid=device_guid((product<<16)|0x054c)
        if guid: break
        time.sleep(.05)
    if not guid: raise RuntimeError('仮想DS4の列挙失敗')
    number,name=native_pad_number(guid)
    native_ini=runtime/'System/_KeyConfig.ini'
    original_ini=native_ini.read_bytes()
    original_protected=protected(runtime)
    (out/'protected_before.json').write_text(json.dumps(original_protected,indent=2),encoding='utf-8')
    proc=memory=reader=stream=None
    result=dict(errors=[],out=str(out))
    def status():
        values=struct.unpack_from('<4Iq6I',memory)
        return dict(zip(['magic','version','count','capacity','frequency','status','recordSize',
                         'command','completed','bootDone','reserved'],values))
    def save():
        if memory is None: return
        current=status()
        if current['recordSize']==FORMAT.size:
            with (out/'frames.csv').open('w',newline='') as f:
                writer=csv.writer(f);writer.writerow(FIELDS)
                for n in range(current['count']): writer.writerow(FORMAT.unpack_from(memory,4096+n*FORMAT.size))
        (out/'status.json').write_text(json.dumps(current,indent=2),encoding='utf-8')
    try:
        # 標準のValは1起算、0は未割当。Val06=D,07=C,08=A,09=B。
        # DI button0をAだけへ割当（旧予備試行のbutton0はDだった）。
        values=[17,18,19,20,8,0,4,3,1,2,0,0]+[0]*12
        native_ini.write_text('[KeyConfig]\nNoKeyboard=1\n'+''.join(
            f'P{side}_Num={number if side==0 else 0}\nP{side}_Name={name if side==0 else ""}\n'+
            ''.join(f'P{side}_Val{i:02}={value if side==0 else 0}\n' for i,value in enumerate(values))
            for side in range(4)),encoding='ascii')
        probe=ROOT/'test/tools/frame_observer/stock_input_probe.dll'
        artifacts=[game_path,probe,Path(__file__),Path(__file__).with_name('stock_input_probe.cpp'),
                   Path(__file__).with_name('build_input_latency.ps1')]
        for source in artifacts[1:]: shutil.copy2(source,out/source.name)
        (out/'manifest.json').write_text(json.dumps(dict(device_guid=guid,native_pad_number=number,
            sha256={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts}),indent=2),encoding='utf-8')
        boot_log=runtime/'stock_input_boot.log'
        if boot_log.exists(): boot_log.replace(out/'previous_stock_input_boot.log')
        env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
        stream=(out/'launcher.log').open('w')
        startup=subprocess.STARTUPINFO();startup.dwFlags=subprocess.STARTF_USESHOWWINDOW;startup.wShowWindow=1
        proc=subprocess.Popen([str(game_path)],cwd=runtime,env=env,stdout=stream,stderr=subprocess.STDOUT,startupinfo=startup)
        result['pid']=proc.pid
        subprocess.run([str(ROOT/'test/tools/frame_observer/legacy_benchmark_inject.exe'),str(proc.pid),
                        str(probe),str(game_path)],check=True,timeout=15)
        memory=mmap.mmap(-1,4096+262144*FORMAT.size,tagname=f'Local\\CCCasterStockInput_{proc.pid}',access=mmap.ACCESS_WRITE)
        reader=ProcessReader(proc.pid)
        print(json.dumps(dict(out=str(out),pid=proc.pid,guid=guid)),flush=True)
        deadline=time.monotonic()+65
        while True:
            state=status()
            if proc.poll() is not None: raise RuntimeError(f'ゲーム終了: {proc.returncode}')
            if state['status'] not in (0,1): raise RuntimeError(f'観測器異常: {state}')
            if state['bootDone'] and reader.value(0x54eee8)==1 and reader.read(0x55d20b,1)==b'\0': break
            if time.monotonic()>deadline: raise RuntimeError(f'対戦画面への到達失敗: {state}')
            time.sleep(.05)
        before=audit(reader,game_path,proc.pid)
        (out/'runtime_audit_before.json').write_text(json.dumps(before,indent=2),encoding='utf-8')
        if not before['passed']: raise RuntimeError('標準実行コードの照合失敗')
        print(json.dumps(dict(battle=before['game'],stock_code_verified=True)),flush=True)
        struct.pack_into('<I',memory,32,1)
        deadline=time.monotonic()+65
        while status()['completed']<30:
            if proc.poll() is not None: raise RuntimeError(f'測定中にゲーム終了: {proc.returncode}')
            if status()['status']!=1: raise RuntimeError(f'測定中の観測器異常: {status()}')
            if time.monotonic()>deadline: raise RuntimeError(f'入力試験の完了失敗: {status()}')
            time.sleep(.05)
        after=audit(reader,game_path,proc.pid)
        (out/'runtime_audit_after.json').write_text(json.dumps(after,indent=2),encoding='utf-8')
        if not after['passed']: raise RuntimeError('測定後の標準実行コード照合失敗')
        result['completed']=status()['completed']
        result['stock_code_verified']=True
    except Exception as e:
        result['errors'].append(f'{type(e).__name__}: {e}')
    finally:
        try: save()
        finally:
            if reader: reader.close()
            if memory: memory.close()
            if proc and proc.poll() is None: proc.terminate();proc.wait(10)
            if stream: stream.close()
            native_ini.write_bytes(original_ini)
            pad.reset();pad.update()
        if (runtime/'stock_input_boot.log').exists(): shutil.copy2(runtime/'stock_input_boot.log',out/'stock_input_boot.log')
        final=protected(runtime)
        result['protected_unchanged']=final==original_protected
        if not result['protected_unchanged']: result['errors'].append('保全対象の変更を検出')
        (out/'protected_after.json').write_text(json.dumps(final,indent=2),encoding='utf-8')
        (out/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(json.dumps(result),flush=True)
    return bool(result['errors'])


if __name__=='__main__': raise SystemExit(main())
