"""Training初回と戦闘後のキャラ選択BGMを通常入力・読取り専用メモリで比較する。"""
import argparse
import ctypes as C
import json
from pathlib import Path
import random
import shutil
import subprocess
import time
import traceback
from run_exit_guard import Game, ROOT, processes
from run_training_character import device_guid
from run_training_corner import digest
from bench_startup import protected_files
from real_game_checkpoint import clean_environment
from verify_selection_bgm import verify


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', default='probe')
    args = parser.parse_args()
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    out = ROOT/'test/logs'/time.strftime('selection_bgm_%Y%m%d_%H%M%S')
    out.mkdir()
    print('Logs:', out, flush=True)
    runtime = ROOT/'test/runtime'
    sides = [runtime/f'MBAACC_{i}' for i in (1, 2, 3)]
    caster = sides[0]/'cccaster_B'
    before = protected_files(sides)
    backups = {Path(p):Path(p).read_bytes() for p in before if Path(p).suffix.lower()=='.ini'}
    result = dict(label=args.label,errors=[],checks={},binaries={})
    samples = []
    stage = 'startup'
    game = proc = pad = stream = None
    log = caster/'cccaster_hook_log.txt'
    started = time.monotonic()

    def sample():
        if game is None: return
        obj = game.read(0x76dff8)
        row = dict(t=time.monotonic()-started,stage=stage,mode=game.read(0x54eee8),
                   bgm=game.read(0x76c0b0),object=obj,playing=game.read(0x76e838))
        if obj:
            row.update(kind=game.read(obj+0x54),thread=game.read(obj+0x78),
                       part=game.read(obj+0x4c),blocks=game.read(obj+0x44))
            if row['kind']==2:
                decoder=game.read(obj+0x38)
                row.update(decoder=decoder,decoded=game.read(decoder+0x324))
        samples.append(row)

    def delay(seconds):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            sample()
            time.sleep(.01)

    def wait(predicate, reason, seconds=30):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            if predicate(): return
            delay(.03)
        raise RuntimeError(reason)

    buttons = dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE,b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
                   start=vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT)
    directions = dict(right=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
                      left=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,
                      up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,
                      down=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH)
    def press(key):
        pad.reset()
        if key in buttons: pad.press_button(buttons[key])
        else: pad.directional_pad(directions[key])
        pad.update(); delay(.12); pad.reset(); pad.update(); delay(.25)

    def selection(label):
        nonlocal stage
        stage=label+'_idle'; delay(3)
        for key in ('right','left','a','b','right','left'):
            stage=label+'_'+key; press(key); delay(1)
        stage=label+'_tail'; delay(3)
        assert game.read(0x54eee8)==20

    try:
        assert not processes(sides[0]/'MBAA.exe'), '対象ゲーム起動中'
        for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll'):
            result['binaries'][name]=digest(caster/name)
            assert result['binaries'][name]==digest(ROOT/'build/bin'/name)
        product=random.SystemRandom().randrange(0x8000,0xffff)
        class Pad(vg.VDS4Gamepad):
            def target_alloc(self):
                target=vc.vigem_target_ds4_alloc()
                vc.vigem_target_set_vid(target,0x054c);vc.vigem_target_set_pid(target,product)
                return target
        pad=Pad()
        found=[]
        wait(lambda:bool(found.append(device_guid((product<<16)|0x054c)) or found[-1]),'仮想パッドなし')
        config=caster/'cccaster.ini'
        mapping=caster/f'Wireless Controller__{found[-1]}.ini'
        for path in (config,mapping): backups.setdefault(path,path.read_bytes() if path.exists() else None)
        (out/'config_backups.json').write_text(json.dumps({str(p):b.hex() if b is not None else None for p,b in backups.items()}),encoding='utf-8')
        config.write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={found[-1]}\n',encoding='utf-8')
        mapping.write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n',encoding='utf-8')
        if log.exists(): log.replace(out/'previous_game.log')
        env=clean_environment();env['CCCASTER_STARTUP_TRACE']='1'
        env['CCCASTER_SELECTION_SOUND_CAPTURE']=str(out)
        stream=(out/'launcher.log').open('w',encoding='utf-8')
        proc=subprocess.Popen([str(caster/'CCCaster_B.exe'),'--training'],cwd=caster,env=env,
                              stdout=stream,stderr=subprocess.STDOUT,creationflags=subprocess.CREATE_NO_WINDOW)
        game=Game(1)
        wait(lambda:game.read(0x54eee8)==20,'初回キャラ選択なし')
        assert game.read(0x41e0e2,5)==int.from_bytes(bytes.fromhex('e829f90b00'),'little'), '共通SEの元の準備処理が無効'
        selection('initial')
        stage='enter_battle'
        end=time.monotonic()+40
        while not game.battle():
            if time.monotonic()>end: raise RuntimeError('戦闘未到達')
            press('a')
        delay(2)
        stage='return_to_selection'
        press('start');wait(lambda:game.read(0x74d7fc)!=0,'Trainingメニューなし');delay(.7)
        menu=game.read(game.read(0x74d7fc)+0x10)
        menu=game.read(menu)
        begin,end=game.read(menu+0x4c),game.read(menu+0x50)
        keys=[]
        for p in range(begin,end,4):
            string=game.read(p)+0x3c
            address=string+4 if game.read(string+0x18)<16 else game.read(string+4)
            keys.append(bytes(game.read(address+i,1) for i in range(game.read(string+0x14))).decode('ascii'))
        result['menu_keys']=keys
        target=keys.index('CHARACTER_SELECT')
        for _ in range(len(keys)):
            if game.read(menu+0x40)==target: break
            press('down')
        assert game.read(menu+0x40)==target
        press('a');delay(.4);press('up');press('a')
        wait(lambda:game.read(0x54eee8)==20,'キャラ選択復帰なし')
        selection('returned')
        result['checks']['both_selection_phases']=True
        stage='exit'
        game.key(27);game.process.wait(timeout=10);proc.wait(timeout=10)
        result['waveform']=verify(out)
        assert result['waveform']['passed'], '初回と復帰後のBGM波形が不一致'
        result['checks']['initial_bgm_matches_returned']=True
    except Exception:
        result['errors'].append(traceback.format_exc())
    finally:
        if pad: pad.reset();pad.update()
        if game: game.close_handle()
        for p in processes(sides[0]/'MBAA.exe'):
            p.terminate();p.wait(timeout=10)
        if proc and proc.poll() is None: proc.terminate();proc.wait(timeout=10)
        if stream: stream.close()
        if log.exists(): shutil.copy2(log,out/'game.log')
        for path,data in backups.items():
            if data is None: path.unlink(missing_ok=True)
            else: path.write_bytes(data)
        result['protected_unchanged']=before==protected_files(sides)
        result['sample_count']=len(samples)
        result['passed']=not result['errors'] and result['protected_unchanged']
        (out/'samples.json').write_text(json.dumps(samples),encoding='utf-8')
        (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(result,ensure_ascii=False),flush=True)
    return 0 if result['passed'] else 1

if __name__=='__main__': raise SystemExit(main())
