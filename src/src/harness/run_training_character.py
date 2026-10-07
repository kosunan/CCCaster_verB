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
import sys
import time
import uuid
from run_training_corner import protected, digest

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
    parser.add_argument('--palette', action='store_true', help='マウスの画面操作と読取り検査を組み合わせてパレット／鉛筆を確認')
    parser.add_argument('--extra', action='store_true', help='ACT取込み・永続保存・再ロードを画面操作と読取りで確認')
    parser.add_argument('--extra-restore', action='store_true', help='既存の保存ファイルを別プロセスで復元して検査')
    parser.add_argument('--extra-select', action='store_true', help='既存カラー一覧の7ページ目から42番(EXTRA 6)を選び、実データ適用を検査')
    parser.add_argument('--color-page-preview', action='store_true', help='extra-selectの42番で画面確認用に待機。continueで再開')
    parser.add_argument('--color-page-fixtures', action='store_true', help='独立コピーの37〜41番に識別用の5色を一時配置し、終了時に元へ戻す')
    parser.add_argument('--hidden', action='store_true',help='隠し8体とイクリプスを検査')
    args = parser.parse_args()
    if args.extra_restore: args.extra=True
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
    color_backups = {}
    copies_before = None
    caster = runtime / 'MBAACC_1/cccaster_B'
    log = caster / 'cccaster_hook_log.txt'
    k = C.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]; k.OpenProcess.restype = W.HANDLE
    k.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
    k.CloseHandle.argtypes = [W.HANDLE]
    buttons = dict(a=vg.DS4_BUTTONS.DS4_BUTTON_SQUARE, b=vg.DS4_BUTTONS.DS4_BUTTON_CROSS,
                   d=vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE,
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
        for _ in range(3):
            if read(0x74D7FC): break
            press('start'); time.sleep(.4)
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

    def menu_item(key):
        menu = read(read(read(0x74D7FC)+0x10))
        begin,end = read(menu+0x4c),read(menu+0x50)
        keys=[]
        for p in range(begin,end,4):
            string=read(p)+0x3c
            address=string+4 if read(string+0x18)<16 else read(string+4)
            keys.append(read_bytes(address,read(string+0x14)).decode('ascii',errors='replace'))
        target=keys.index(key)
        for _ in range(len(keys)):
            if read(menu+0x40)==target: return
            press('down')
        raise RuntimeError('メニュー項目に未到達: '+key)

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
        binary_names=('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')
        result['tested_binaries']={name:digest(caster/name) for name in binary_names}
        if result['tested_binaries']!={name:digest(ROOT/'build/bin'/name) for name in binary_names}:
            raise RuntimeError('配置バイナリがbuild/binと不一致')
        if args.color_page_fixtures:
            source=(caster/'extra_colors/character_0/extra_6.cccolor').read_bytes()
            for index,rgb in enumerate((0x40d040,0xe06030,0x20d0e0,0xd040c0,0xd0d030),1):
                path=caster/f'extra_colors/character_0/extra_{index}.cccolor'
                color_backups[path]=path.read_bytes() if path.exists() else None
                data=bytearray(source)
                for color in range(1,256): data[24+color*4:28+color*4]=rgb.to_bytes(4,'little')
                checksum=2166136261
                for byte in data[:-4]: checksum=((checksum^byte)*16777619)&0xffffffff
                data[-4:]=(checksum or 1).to_bytes(4,'little');path.write_bytes(data)
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
            if '[FastBoot] ★ CharaSelect reached!' in text():
                if args.extra_select and handle is None:
                    pid = subprocess.check_output(['pwsh','-NoProfile','-Command',
                        f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"], text=True).strip()
                    handle = k.OpenProcess(0x10,False,int(pid))
                if args.extra_select and read(0x74D8EC)==2 and '[ExtraColor] CHOOSE side=0 character=0 extra=6' not in text():
                    time.sleep(.6)
                    press('left')
                    wait(lambda: f'[ExtraColor] PAGE side=0 color=37 saved={int(args.color_page_fixtures)}' in text(),reason='7ページ目に移れない')
                    if not args.color_page_fixtures:
                        press('a')
                        if read(0x74D8EC)!=2: raise RuntimeError('未登録37番を決定できてしまう')
                    press('right')
                    if read(0x74D904)!=0: raise RuntimeError('7ページ目から標準1番へ戻れない')
                    press('left');press('left')
                    if read(0x74D904)!=30: raise RuntimeError('7ページ目から標準31番へ戻れない')
                    press('right')
                    if args.color_page_fixtures:
                        for index,rgb in enumerate((0x40d040,0xe06030,0x20d0e0,0xd040c0,0xd0d030)):
                            cg=read(read(read(0x74D808)+0x1b0)+4)
                            if read(cg+16+17*4)!=rgb: raise RuntimeError(f'{37+index}番とプレビューの色が不一致')
                            press('down')
                        press('down');press('down') # RANDOM経由で37番へ戻る。
                    press('up')
                    if read(read(0x74D808)+0x38)!=1: raise RuntimeError('7ページ目からRANDOMへ移れない')
                    press('up')
                    wait(lambda: '[ExtraColor] PAGE side=0 color=42 saved=1' in text(),reason='RANDOMから42番へ戻れない')
                    result['native_color_page']=dict(page_count=7,rows=6,empty_rejected=not args.color_page_fixtures,
                        wrap=True,random_return=True,distinct_previews=args.color_page_fixtures)
                    if args.color_page_preview:
                        print('COLOR_PAGE_PREVIEW: 42 / continueで再開',flush=True)
                        for line in sys.stdin:
                            command=line.strip()
                            if command=='continue': break
                            press(command)
                        deadline=time.monotonic()+40
                    press('a')
                    wait(lambda: '[ExtraColor] CHOOSE side=0 character=0 extra=6' in text(),reason='42番を決定できない')
                else: press('a')
            else: time.sleep(.2)
        pid = subprocess.check_output(['pwsh','-NoProfile','-Command',
            f"Get-CimInstance Win32_Process -Filter \"Name='MBAA.exe'\" | Where-Object ParentProcessId -eq {proc.pid} | Select-Object -ExpandProperty ProcessId"], text=True).strip()
        if handle is None: handle = k.OpenProcess(0x10,False,int(pid))
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
        if args.extra_select:
            data=(caster/'extra_colors/character_0/extra_6.cccolor').read_bytes()
            if read_bytes(read(0x557D34)+0x10,1024)!=b'\0'*4+data[28:1048]:
                raise RuntimeError('Trainingキャラ選択のエクストラパレットが不一致')
            if not re.search(r'\[ExtraColor\] LOAD slot=0 character=0 .*matched=1 applied=1',text()):
                raise RuntimeError('Trainingキャラ選択でエクストラ未適用')
            if not re.search(r'\[TrainingPalette\] APPLY slot=0 pages=\d+ pixels=13 uploaded=1',text()):
                raise RuntimeError('Trainingキャラ選択で鉛筆を復元できない')
            result['extra_selection']=True
            loads=text().count('[TrainingState] event=2 ')
            press('right');press('reset')
            wait(lambda: text().count('[TrainingState] event=2 ')>loads,reason='エクストラ選択後のFN保存復元が失敗')
            resumed();sample('extra_selected')
        elif args.palette or args.extra:
            original_palettes = [read_bytes(read(0x557D34+i*12)+0x10,1024) for i in range(2)]
            last_palettes = original_palettes
            last_apply_count = 0
            result['palette_checks'] = []
            if args.extra_restore: shutil.copy2(caster/'extra_colors/character_0/extra_6.cccolor',out/'saved_extra_6.cccolor')
            open_menu(); menu_item('CC_PALETTE'); press('a')
            print('PALETTE ready. Commands: palette_applied / pixel_applied / cancelled / p2_applied / restored / reopen / done; normal pad commands also available',flush=True)
            for line in iter(input, 'done'):
                if line == 'reopen':
                    if not read(0x74D7FC): open_menu()
                    menu_item('CC_PALETTE'); press('a')
                    continue
                if line in buttons or line in directions:
                    press(line); continue
                if args.extra and line == 'reload':
                    press('b'); resumed();choose(0,7,0,'extra_other_character');choose(0,0,0,'extra_reload_original')
                    open_menu(); menu_item('CC_PALETTE'); press('a');continue
                if args.extra and line == 'extra_loaded':
                    data=(out/'saved_extra_6.cccolor').read_bytes()
                    expected=b'\0'*4+data[28:1048]
                    wait(lambda: read_bytes(read(0x557D34)+0x10,1024)==expected,reason='再ロードしたエクストラのパレットが不一致')
                current = [read_bytes(read(0x557D34+i*12)+0x10,1024) for i in range(2)]
                applies = re.findall(r'\[TrainingPalette\] APPLY slot=(\d+) pages=(\d+) pixels=(\d+) uploaded=(\d+)',text())
                if args.extra and line == 'act_applied':
                    source=Path('C:/Users/junna/Downloads/Crimson Cacophony.act').read_bytes()
                    expected=b''.join(source[i:i+3]+b'\0' for i in range(0,768,3))
                    expected=b'\0'*4+expected[4:]
                    if current[0] != expected: raise RuntimeError('ACTの256色が実メモリと不一致')
                elif args.extra and line == 'extra_saved':
                    saved=caster/'extra_colors/character_0/extra_6.cccolor'
                    data=saved.read_bytes()
                    if data[:8] != b'CCL1\0\0\0\0': raise RuntimeError('キャラ別EXTRA 6の形式が不正')
                    shutil.copy2(saved,out/'saved_extra_6.cccolor')
                elif args.extra and line == 'extra_loaded':
                    data=(out/'saved_extra_6.cccolor').read_bytes()
                    expected=b'\0'*4+data[28:1048]
                    if current[0] != expected: raise RuntimeError('再ロードしたエクストラのパレットが不一致')
                    if not applies or int(applies[-1][2])==0: raise RuntimeError('鉛筆が復元されていない')
                elif line == 'palette_applied':
                    if current[0]==original_palettes[0] or current[1]!=original_palettes[1]: raise RuntimeError('P1パレット変更の範囲が不正')
                    if not applies or applies[-1][0]!='0' or applies[-1][3]!='1': raise RuntimeError('P1適用未成功')
                elif line == 'pixel_applied':
                    if current!=last_palettes: raise RuntimeError('ピクセル編集でパレットを変更した')
                    if len(applies)!=last_apply_count+1 or int(applies[-1][2])<=0 or applies[-1][3]!='1': raise RuntimeError('ピクセルの適用未成功')
                elif line == 'cancelled':
                    if current!=last_palettes: raise RuntimeError('取消が実パレットを変更した')
                    if len(applies)!=last_apply_count: raise RuntimeError('取消がピクセルを適用した')
                elif line == 'p2_applied':
                    if current[0]!=last_palettes[0] or current[1]==last_palettes[1]: raise RuntimeError('P2パレット変更の範囲が不正')
                    if not applies or applies[-1][0]!='1' or applies[-1][3]!='1': raise RuntimeError('P2適用未成功')
                elif line == 'restored':
                    if current!=original_palettes: raise RuntimeError('元のパレットに戻らない')
                else: raise RuntimeError('不明な検査コマンド: '+line)
                result['palette_checks'].append(dict(check=line,applies=applies,
                    changed_entries=[sum(a[j:j+4]!=b[j:j+4] for j in range(0,1024,4)) for a,b in zip(original_palettes,current)]))
                last_palettes=current
                last_apply_count=len(applies)
                print('PASS '+line,flush=True)
            required={'extra_loaded'} if args.extra_restore else {'act_applied','extra_saved','extra_loaded'} if args.extra else {'palette_applied','pixel_applied','cancelled','p2_applied','restored'}
            if required-{item['check'] for item in result['palette_checks']}: raise RuntimeError('必要な画面操作が未確認')
            press('b'); resumed(); initial_world=read(0x55D1CC)
            time.sleep(.3)
            if read(0x55D1CC)<=initial_world: raise RuntimeError('編集後に戦闘が進行しない')
            press('save'); loads=text().count('[TrainingState] event=2 ')
            press('right'); press('reset')
            wait(lambda: text().count('[TrainingState] event=2 ')>loads,reason='編集後のFN保存復元が失敗')
            resumed(); sample('palette_finished')
            if '[Exception]' in text(): raise RuntimeError('例外を検出')
        else:
            open_picker()
        if args.inspect_seconds:
            print('INSPECT picker', flush=True); time.sleep(args.inspect_seconds)
        if args.palette or args.extra or args.extra_select:
            pass
        elif args.interactive:
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
            open_menu(); menu_item('ENEMY_STATUS')
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
        for path,data in color_backups.items():
            if data is None: path.unlink(missing_ok=True)
            else: path.write_bytes(data)
        result['protected_unchanged'] = before == {str(p): protected(p) for p in sources}
        result['test_ini_unchanged'] = copies_before is not None and copies_before == protected(runtime)
        if 'tested_binaries' in result and result['tested_binaries']!={name:digest(ROOT/'build/bin'/name) for name in result['tested_binaries']}:
            result['errors'].append('試験中にbuild/binが変わった')
        result['passed'] = not result['errors'] and result['protected_unchanged'] and result['test_ini_unchanged'] and not args.interactive
        (out / 'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(json.dumps(result,ensure_ascii=False,indent=2),flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
