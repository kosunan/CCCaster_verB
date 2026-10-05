"""独立コピー・仮想DS4の通常入力でTrainingのキャラ/ムーン切替を検証。メモリは読取りのみ。"""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
import os
import random
import re
from pathlib import Path
import shutil
import subprocess
import time
import uuid
from run_training_corner import protected

ROOT = Path(__file__).resolve().parents[3]
CHARACTERS = [22,7,51,15,28,8,2,0,30,11,9,31,4,3,1,19,12,13,14,29,17,18,33,23,10,25,35,5,20,6,34,
              16,32,53,58,59,72,73,85]
COLUMNS = 10


def device_guid(product):
    """DirectInputの列挙だけを行い、今回作った仮想個体を通常INIへ明示する。"""
    class Instance(C.Structure):
        _fields_ = [('size', W.DWORD), ('instance', C.c_ubyte*16), ('product', C.c_ubyte*16),
                    ('type', W.DWORD), ('name', W.WCHAR*260), ('product_name', W.WCHAR*260),
                    ('ff', C.c_ubyte*16), ('usage_page', W.WORD), ('usage', W.WORD)]
    callback_type = C.WINFUNCTYPE(W.BOOL, C.POINTER(Instance), C.c_void_p)
    found = []
    @callback_type
    def callback(pointer, context):
        info = pointer.contents
        if int.from_bytes(bytes(info.product)[:4], 'little') == product:
            found.append(str(uuid.UUID(bytes_le=bytes(info.instance))).upper())
        return True
    library = C.WinDLL('dinput8')
    create = library.DirectInput8Create
    create.argtypes = [W.HINSTANCE, W.DWORD, C.c_void_p, C.POINTER(C.c_void_p), C.c_void_p]
    create.restype = C.c_long
    kernel = C.WinDLL('kernel32')
    kernel.GetModuleHandleW.restype = W.HMODULE
    interface = C.c_void_p()
    iid = C.create_string_buffer(uuid.UUID('BF798031-483A-4DA2-AA99-5D64ED369700').bytes_le)
    status = create(kernel.GetModuleHandleW(None),0x800,iid,C.byref(interface),None)
    if status < 0: raise RuntimeError(f'DirectInput列挙初期化失敗: {status}')
    table = C.cast(interface,C.POINTER(C.POINTER(C.c_void_p))).contents
    try:
        enum = C.WINFUNCTYPE(C.c_long,C.c_void_p,W.DWORD,callback_type,C.c_void_p,W.DWORD)(table[4])
        status = enum(interface,4,callback,None,1)
        if status < 0: raise RuntimeError(f'DirectInput列挙失敗: {status}')
    finally:
        C.WINFUNCTYPE(W.ULONG,C.c_void_p)(table[2])(interface)
    return found[0] if len(found) == 1 else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inspect-seconds', type=int, default=0)
    parser.add_argument('--interactive', action='store_true')
    parser.add_argument('--hidden', action='store_true',help='隠し8体とイクリプスを検査')
    args = parser.parse_args()
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    product = random.SystemRandom().randrange(0x8000,0xFFFF)
    class TestPad(vg.VDS4Gamepad):
        def target_alloc(self):
            target = vc.vigem_target_ds4_alloc()
            vc.vigem_target_set_vid(target,0x054C)
            vc.vigem_target_set_pid(target,product)
            return target
    out = ROOT / 'test/logs' / time.strftime('training_character_%Y%m%d_%H%M%S')
    runtime = ROOT / 'test/runtime/training_character'
    out.mkdir(parents=True)
    sources = [ROOT / 'test/runtime' / f'MBAACC_{i}' for i in range(1, 4)]
    before = {str(p): protected(p) for p in sources}
    result = dict(errors=[], samples=[], runtime=str(runtime))
    proc = handle = pad = None
    copies_before = None
    caster = runtime / 'MBAACC_1/cccaster_B'
    log = caster / 'cccaster_hook_log.txt'
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; k.OpenProcess.restype = W.HANDLE
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]
    buttons = dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE, b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
                   start=vg.DS4_BUTTONS.DS4_BUTTON_TRIGGER_RIGHT,
                   save=vg.DS4_BUTTONS.DS4_BUTTON_SHARE, reset=vg.DS4_BUTTONS.DS4_BUTTON_OPTIONS)
    directions = dict(left=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_WEST,
                      right=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_EAST,
                      up=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH,
                      down=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_SOUTH)

    def text():
        return log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''

    def wait(predicate, seconds=12, reason='条件未達'):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if predicate(): return
            if proc and proc.poll() is not None: raise RuntimeError('ゲーム起動元が終了: ' + reason)
            time.sleep(.03)
        raise RuntimeError(reason)

    def read_bytes(addr, size):
        data = C.create_string_buffer(size)
        if not k.ReadProcessMemory(handle, addr, data, size, None): raise C.WinError(C.get_last_error())
        return data.raw

    def read(addr, size=4):
        return int.from_bytes(read_bytes(addr,size), 'little')

    def sample(label):
        value = dict(label=label, mode=read(0x54EEE8), stage=read(0x74FD98),
                     p1=[read(0x74D840), read(0x74D84C)], p2=[read(0x74D86C), read(0x74D878)],
                     sim=read(0x55D1CC), pause=read(0x55D203,1), training_pause=read(0x562A64),
                     intro=read(0x55D20B,1), p1x=read(0x555238), p2x=read(0x555D34),
                     p1pattern=read(0x555140), p2pattern=read(0x555C3C),
                     assets=[read(0x557D30 + i*12) for i in range(4)],
                     actors=[dict(exists=read(0x555130+i*0xAFC), character=read(0x555135+i*0xAFC,1),
                                  moon=read(0x55513C+i*0xAFC,2)) for i in range(4)],
                     enemy_status=read(0x77C1E8),
                     bgm_thread=read(0x76E844), bgm_patch=read(0x472C6D,2))
        result['samples'].append(value)
        print(json.dumps(value), flush=True)
        return value

    def press(command):
        pad.reset()
        if command in buttons: pad.press_button(buttons[command])
        elif command in directions: pad.directional_pad(directions[command])
        else: raise ValueError(command)
        pad.update(); time.sleep(.12)
        pad.reset(); pad.update(); time.sleep(.16)

    def open_menu():
        press('start')
        wait(lambda: read(0x74D7FC) != 0, reason='Trainingメニューが開かない')
        time.sleep(.4)

    def open_picker():
        old = text().count('[TrainingCharacter] OPEN')
        open_menu()
        press('a')
        wait(lambda: text().count('[TrainingCharacter] OPEN') > old, reason='最上段のキャラ選択が開かない')
        wait(lambda: '[TrainingCharacter] IMAGE name=csel_style00' in text(), reason='ムーン画像が未ロード')

    def resumed():
        wait(lambda: read(0x55D20B,1) == 0 and read(0x55D203,1) == 0 and read(0x562A64) == 0,
             reason='切替後に練習へ復帰しない')

    def choose(side, character, moon, label, already_open=False):
        before_change = sample(label+'_before')
        old = text().count('[TrainingCharacter] LOAD end')
        if not already_open: open_picker()
        # P1通常入力で開いた選択を上端からP1/P2欄へ移し、指定側へ切替える。
        opening_side = read(0x55DF0F,1) % 2
        current = before_change['p'+str(opening_side+1)]
        for _ in range(CHARACTERS.index(current[0])//COLUMNS+1): press('up')
        # 同じ側でも一度往復し、上移動中の仮選択を元の確定キャラへ戻す。
        for _ in range(1 if side != opening_side else 2): press('right')
        press('a')
        current = before_change['p'+str(side+1)]
        delta = (CHARACTERS.index(character)-CHARACTERS.index(current[0])) % len(CHARACTERS)
        direction, steps = ('left',len(CHARACTERS)-delta) if delta > len(CHARACTERS)//2 else ('right',delta)
        # キャラ移動で、収録されないムーンは最初の収録スタイルへ補正される。
        styles = {int(c):int(m) for c,m in re.findall(r'OPTION char=(\d+) styles=(\d+)',text())}
        slot = min(current[1],3)
        index = CHARACTERS.index(current[0])
        for _ in range(steps):
            press(direction)
            index = (index + (1 if direction == 'right' else -1)) % len(CHARACTERS)
            if not styles[CHARACTERS[index]] & (1<<slot):
                slot = next(s for s in range(4) if styles[CHARACTERS[index]] & (1<<s))
        press('a')
        target_slot = min(moon,3)
        if not styles[character] & (1<<target_slot): raise RuntimeError(label+': 要求スタイル未収録')
        while slot != target_slot:
            press('right')
            slot = (slot+1)%4
            while not styles[character] & (1<<slot): slot = (slot+1)%4
        press('a'); press('a')
        wait(lambda: text().count('[TrainingCharacter] LOAD end') == old+1, reason=label+': 読込み未完了')
        resumed()
        after_change = sample(label)
        if after_change['p'+str(side+1)] != [character,moon]: raise RuntimeError(label+': 選択不一致')
        other = 'p'+str(2-side)
        if after_change[other] != before_change[other]: raise RuntimeError(label+': 相手の選択が変更された')
        if after_change['actors'][side]['moon'] != moon: raise RuntimeError(label+': 実体のムーンが不一致')
        for key in ('mode','stage','bgm_thread','bgm_patch','enemy_status'):
            if after_change[key] != before_change[key]: raise RuntimeError(label+': '+key+'が変化')
        time.sleep(.25)
        if read(0x55D1CC) <= after_change['sim']: raise RuntimeError(label+': 更新停止')
        return after_change

    try:
        print('Logs:', out, flush=True)
        for source in sources:
            target = runtime / source.name
            if not target.exists():
                shutil.copytree(source, target, ignore=shutil.ignore_patterns('cccaster_hook_log.txt', 'broadcast'))
        with (out / 'deploy.log').open('w') as stream:
            subprocess.run(['pwsh', '-NoProfile', '-File', str(ROOT / 'deploy.ps1'), '-TestRoot', str(runtime)],
                           cwd=ROOT, check=True, stdout=stream, stderr=subprocess.STDOUT)
        pad = TestPad()
        guid = None
        deadline = time.monotonic()+10
        while guid is None and time.monotonic() < deadline:
            guid = device_guid((product<<16)|0x054C)
            if guid is None: time.sleep(.1)
        if guid is None: raise RuntimeError('試験用仮想パッドの個体を識別できない')
        result['controller_guid'] = guid
        (caster / 'cccaster.ini').write_text(f'[Settings]\nP1Device=Wireless Controller\nP1DeviceGuid={guid}\nDelay=2\n', encoding='utf-8')
        (caster / 'Wireless Controller.ini').write_text(
            '[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\n'
            'A=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n', encoding='utf-8')
        copies_before = protected(runtime)
        if log.exists():
            shutil.copy2(log, out / 'previous_game.log')
            log.unlink()
        env = {key:value for key,value in os.environ.items() if not key.startswith('CCCASTER_')}
        env.update(CCCASTER_TRAINING_TRACE='1', CCCASTER_INPUT_DIAGNOSTIC='1')
        with (out / 'launcher.log').open('w') as stream:
            proc = subprocess.Popen([str(caster / 'CCCaster_B.exe'), '--training'], cwd=caster, env=env,
                                    stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        deadline = time.monotonic() + 40
        while '[TrainingAdvantage] valid=1' not in text():
            if time.monotonic() > deadline: raise RuntimeError('戦闘に未到達')
            if '[FastBoot] ★ CharaSelect reached!' in text(): press('a')
            else: time.sleep(.2)
        pid = subprocess.check_output(['pwsh','-NoProfile','-Command',
            f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"], text=True).strip()
        handle = k.OpenProcess(0x10,False,int(pid))
        table = read(0x55DF18)
        kind, base, stride, count = [read(table+i*4) for i in range(4)]
        if not 0 < count <= 256: raise RuntimeError('キャラ定義数が不正')
        catalogue = []
        for character in range(count):
            address = base+stride*character if kind == 0 else read(base+4*character)
            if not address: continue
            data = read_bytes(address,max(stride,0x74))
            string = lambda offset: data[offset:offset+32].split(b'\0')[0].decode('cp932',errors='replace')
            catalogue.append(dict(character=character,name=string(4),first=string(0x34),partner=string(0x54),
                                  portrait=read(0x5519F8+character*4),raw=data.hex()))
        (out/'catalogue.json').write_text(json.dumps(dict(stride=stride,characters=catalogue),ensure_ascii=False,indent=2),encoding='utf-8')
        initial = sample('initial')
        press('save')
        wait(lambda: '[TrainingState] event=1 ' in text(), reason='保存できない')
        open_picker()
        if args.inspect_seconds:
            print('INSPECT picker', flush=True); time.sleep(args.inspect_seconds)
        if args.interactive:
            print('Commands: a b start save reset left right up down sample quit',flush=True)
            for line in iter(input, 'quit'):
                if line == 'sample': sample('interactive')
                elif line.startswith('choose '):
                    side,character,moon=map(int,line.split()[1:])
                    choose(side,character,moon,'interactive_choice',already_open=True)
                else: press(line)
            result['interactive_only'] = True
        elif args.hidden:
            # ムーン9を持つ通常キャラと姫アルク、および追加した全8体。
            cases=[(0,30,9),(1,51,9),(0,16,9),(1,32,0),(0,53,8),(1,58,9),
                   (0,59,9),(1,72,9),(0,73,9),(1,85,9),(0,0,0),(1,11,0),(0,53,8)]
            for i,(side,character,moon) in enumerate(cases):
                changed=choose(side,character,moon,f'hidden_{i}_{character}_{moon}',already_open=i==0)
                if character==85 and not changed['actors'][3]['exists']: raise RuntimeError('ボス子キャラ未生成')
                # 変身・攻撃を含む通常入力後にもメニューへ戻れることを次ケースで確認する。
                press('a'); time.sleep(.5)
            # 元のキャラ選択画面への復帰を通常メニュー入力で検査。
            open_menu()
            menu = read(read(read(0x74D7FC)+0x10))
            begin,end = read(menu+0x4c),read(menu+0x50)
            keys=[]
            for p in range(begin,end,4):
                string=read(p)+0x3c
                address=string+4 if read(string+0x18)<16 else read(string+4)
                keys.append(read_bytes(address,read(string+0x14)).decode('ascii',errors='replace'))
            target=keys.index('CHARACTER_SELECT')
            for _ in range(len(keys)):
                if read(menu+0x40)==target: break
                press('down')
            if read(menu+0x40)!=target: raise RuntimeError('標準キャラ選択への項目に到達できない')
            press('a')
            # 元ゲームのYes/NoダイアログはNOから始まる（0x4D8900）。
            time.sleep(.4); press('up'); press('a')
            wait(lambda: read(0x54EEE8)==20,reason='標準キャラ選択へ復帰しない')
            time.sleep(.5)
            result['return_selectors']=[[read(0x74D8FC+i*36),read(0x74D900+i*36)] for i in range(2)]
            if any(c not in CHARACTERS[:31] or m>=3 for c,m in result['return_selectors']):
                raise RuntimeError('標準キャラ選択に隠しカーソル値が残留')
            deadline=time.monotonic()+15
            while read(0x54EEE8)!=1 and time.monotonic()<deadline: press('a')
            if read(0x54EEE8)!=1: raise RuntimeError('標準キャラ選択から戦闘へ戻らない')
            resumed()
            sample('returned_from_standard_select')
            result['hidden_cases']=len(cases)
        else:
            # 仮選択を変更してキャンセル。双方の確定キャラは不変。
            press('right'); press('b')
            cancelled = sample('cancelled')
            if cancelled['p1'] != initial['p1'] or cancelled['p2'] != initial['p2']:
                raise RuntimeError('キャンセルで確定選択が変更された')
            press('a')
            changed = choose(0,30,1,'changed_p1',already_open=True)
            loads = text().count('[TrainingState] event=2 ')
            press('reset'); time.sleep(.4)
            reset = sample('reset_new_character')
            if reset['p1'] != changed['p1'] or text().count('[TrainingState] event=2 ') != loads:
                raise RuntimeError('別キャラの旧保存状態を復元した')
            # 切替後の入力と新しい保存／復元が有効。
            resumed(); press('save')
            saved_x = read(0x555238)
            press('right'); press('right')
            if read(0x555238) == saved_x: raise RuntimeError('切替後の移動入力が無効')
            press('reset')
            wait(lambda: text().count('[TrainingState] event=2 ') > loads, reason='新キャラで保存復元できない')
            resumed()
            if read(0x555238) != saved_x: raise RuntimeError('新キャラの保存位置に復帰しない')
            pair = choose(1,4,2,'changed_p2_pair')
            if not pair['actors'][3]['exists']: raise RuntimeError('P2子キャラが未生成')
            single = choose(1,11,0,'changed_p2_single')
            if single['actors'][3]['exists']: raise RuntimeError('P2子キャラが残留')
            choose(0,30,2,'changed_moon_only')
            # 同じ選択の決定はロードせずメニューに戻る。
            old = text().count('[TrainingCharacter] LOAD end')
            open_picker(); press('a'); press('a'); press('a'); press('b'); resumed()
            if text().count('[TrainingCharacter] LOAD end') != old: raise RuntimeError('同一選択を再ロードした')
            # 既存のENEMY設定を通常の入力でDUMMYへ変更して切替。
            open_menu(); press('down'); press('down')
            for _ in range(5): press('right')
            press('b'); resumed()
            if read(0x77C1E8) != 5: raise RuntimeError('既存のENEMY設定を操作できない')
            choose(0,0,0,'changed_during_dummy')
            if '[Exception]' in text() or '[InputGate] FAILED' in text(): raise RuntimeError('実ゲームログに失敗')
    except Exception as exc:
        result['errors'].append(str(exc))
    finally:
        if pad: pad.reset(); pad.update()
        if handle: k.CloseHandle(handle)
        if proc:
            target = str(runtime / 'MBAACC_1/MBAA.exe').replace("'", "''")
            subprocess.run(['pwsh','-NoProfile','-Command',
                f"Get-CimInstance Win32_Process | Where-Object {{ $_.ParentProcessId -eq {proc.pid} -and $_.ExecutablePath -eq '{target}' }} | ForEach-Object {{ Stop-Process -Id $_.ProcessId -Force }}"],capture_output=True)
            if proc.poll() is None: proc.terminate(); proc.wait(timeout=5)
        if log.exists(): shutil.copy2(log,out / 'game.log')
        result['protected_unchanged'] = before == {str(p): protected(p) for p in sources}
        result['test_ini_unchanged'] = copies_before is not None and copies_before == protected(runtime)
        result['passed'] = not result['errors'] and result['protected_unchanged'] and result['test_ini_unchanged'] and not args.interactive
        (out / 'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(result,ensure_ascii=False,indent=2),flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
