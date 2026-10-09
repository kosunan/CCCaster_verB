"""実ゲームの場面別終了操作。仮想DS4とWin32メッセージを使い、ゲームメモリは読取りのみ。"""
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess
import sys
import threading
import time
import traceback
import psutil
from run_training_character import device_guid
from run_training_corner import digest
from bench_startup import protected_files
from real_game_checkpoint import clean_environment
from run_stage_rematch import free_match_port
from test_p2p_service import Service

ROOT = Path(__file__).resolve().parents[3]
RUNTIME = ROOT / 'test/runtime'
U = C.WinDLL('user32', use_last_error=True)
K = C.WinDLL('kernel32', use_last_error=True)
K.OpenProcess.argtypes = [W.DWORD, W.BOOL, W.DWORD]
K.OpenProcess.restype = W.HANDLE
K.ReadProcessMemory.argtypes = [W.HANDLE, C.c_void_p, C.c_void_p, C.c_size_t, C.c_void_p]
K.CloseHandle.argtypes = [W.HANDLE]
U.PostMessageW.argtypes = [W.HWND, W.UINT, W.WPARAM, W.LPARAM]
U.GetWindowThreadProcessId.argtypes = [W.HWND, C.POINTER(W.DWORD)]
U.IsWindowVisible.argtypes = [W.HWND]
U.SetForegroundWindow.argtypes = [W.HWND]
U.GetForegroundWindow.restype = W.HWND
ENUM = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
U.EnumWindows.argtypes = [ENUM, W.LPARAM]


def wait(predicate, label, seconds=30):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        value = predicate()
        if value:
            return value
        time.sleep(.03)
    raise RuntimeError('時間切れ: ' + label)


def processes(path):
    matches = []
    for p in psutil.process_iter(['exe']):
        try:
            if p.info['exe'] and Path(p.info['exe']).resolve() == path.resolve():
                matches.append(p)
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
    return matches


class Game:
    def __init__(self, side):
        self.process = wait(lambda: processes(RUNTIME / f'MBAACC_{side}/MBAA.exe'), 'ゲーム起動')[0]
        self.handle = K.OpenProcess(0x1010, False, self.process.pid)
        if not self.handle:
            raise C.WinError(C.get_last_error())
        def window():
            found = []
            @ENUM
            def visit(hwnd, _):
                pid = W.DWORD()
                U.GetWindowThreadProcessId(hwnd, C.byref(pid))
                if pid.value == self.process.pid and U.IsWindowVisible(hwnd):
                    found.append(hwnd)
                return True
            U.EnumWindows(visit, 0)
            return found
        self.hwnd = wait(window, 'ゲーム窓')[0]

    def read(self, address, size=4):
        data = C.create_string_buffer(size)
        if not K.ReadProcessMemory(self.handle, address, data, size, None):
            raise C.WinError(C.get_last_error())
        return int.from_bytes(data.raw, 'little')

    def post(self, message, w=0, l=0):
        if not U.PostMessageW(self.hwnd, message, w, l):
            raise C.WinError(C.get_last_error())

    def key(self, key):
        self.post(0x100, key)
        time.sleep(.08)
        # Escによる終了直後はキーアップの配送失敗を許容する。
        U.PostMessageW(self.hwnd, 0x101, key, 0xc0000000)

    def battle(self):
        return self.read(0x54eee8) == 1 and self.read(0x55d20b, 1) == 0

    def blocked(self):
        rows = []
        # 直接WM_CLOSEは入力待機中のPeekMessage経路も通り得る。
        for name, messages in (
            ('escape', [(0x100, 27, 0), (0x100, 27, 1 << 30), (0x102, 27, 0), (0x101, 27, 0xc0000000)]),
            ('sys_escape', [(0x104, 27, 0), (0x105, 27, 0xc0000000)]),
            ('caption_close', [(0xa1, 20, 0), (0xa2, 20, 0), (0xa3, 20, 0)]),
            ('system_close', [(0x112, 0xf063, 0)]),
            ('queued_close', [(0x10, 0, 0)] * 8)):
            first = self.read(0x55d1d4)
            for message in messages:
                self.post(*message)
            wait(lambda: self.read(0x55d1d4) >= first + 20, name + '後の進行', 3)
            assert self.battle(), name + 'で戦闘を離れた'
            rows.append(dict(operation=name, advanced=self.read(0x55d1d4)-first))
        # OSのキー状態を更新して、WndProc以外のGetAsyncKeyState監視も検査する。
        U.SetForegroundWindow(self.hwnd)
        wait(lambda: U.GetForegroundWindow() == self.hwnd, '前面化', 3)
        first = self.read(0x55d1d4)
        U.keybd_event(27, 1, 0, 0)
        try:
            time.sleep(.25)
        finally:
            U.keybd_event(27, 1, 2, 0)
        wait(lambda: self.read(0x55d1d4) >= first + 20, 'OS Esc後の進行', 3)
        assert self.battle()
        rows.append(dict(operation='async_escape', advanced=self.read(0x55d1d4)-first))
        return rows

    def close_handle(self):
        K.CloseHandle(self.handle)


def main():
    import vgamepad as vg
    from vgamepad.win import vigem_client as vc
    out = ROOT / 'test/logs' / time.strftime('exit_guard_%Y%m%d_%H%M%S')
    out.mkdir()
    print('Logs:', out, flush=True)
    sides = [RUNTIME / f'MBAACC_{n}' for n in (1, 2, 3)]
    before = protected_files(sides)
    (out/'protected_before.json').write_text(json.dumps(before,indent=2),encoding='utf-8')
    result = dict(checks={}, errors=[], binaries={})
    backups, pads, streams, launchers, games = {}, [], [], [], []
    env = clean_environment()
    env['CCCASTER_STARTUP_TRACE'] = '1'
    service = Service()
    threading.Thread(target=service.serve_forever, daemon=True).start()
    env['CCCASTER_NTFY_SERVER'] = f'http://127.0.0.1:{service.server_port}'

    def launch(side, args, label):
        caster = sides[side-1] / 'cccaster_B'
        log = caster / 'cccaster_hook_log.txt'
        if log.exists():
            log.replace(out / (label + '_previous.log'))
        stream = (out / (label + '_launcher.log')).open('w', encoding='utf-8')
        streams.append(stream)
        proc = subprocess.Popen([str(caster/'CCCaster_B.exe'), *args], cwd=caster, env=env,
                                stdout=stream, stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NO_WINDOW)
        launchers.append(proc)
        return proc

    def attach(side):
        game = Game(side)
        games.append(game)
        wait(lambda: game.read(0x54eee8) == 20, 'キャラ選択')
        log = sides[side-1] / 'cccaster_B/cccaster_hook_log.txt'
        wait(lambda: '[InputHook] SetWindowLongPtr SUCCEEDED' in log.read_text(encoding='utf-8', errors='replace'), 'WndProc導入')
        return game

    def preserve_logs(label, side=1):
        shutil.copy2(sides[side-1]/'cccaster_B/cccaster_hook_log.txt', out/(label+'_game.log'))

    def press_a():
        for pad in pads:
            pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE)
            pad.update()
        time.sleep(.1)
        for pad in pads:
            pad.reset()
            pad.update()
        time.sleep(.15)

    try:
        for side in sides:
            assert not processes(side/'MBAA.exe'), '対象ゲームが起動中'
            for name in ('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll'):
                path = side/'cccaster_B'/name
                result['binaries'][str(path)] = digest(path)
                assert digest(path) == digest(ROOT/'build/bin'/name)
        # 通常終了時にゲーム自身が保存する窓設定も含めて戻す。
        backups.update({Path(p):Path(p).read_bytes() for p in before if Path(p).suffix.lower()=='.ini'})
        guids = []
        for _ in range(2):
            product = random.SystemRandom().randrange(0x8000, 0xffff)
            class Pad(vg.VDS4Gamepad):
                def target_alloc(self):
                    target = vc.vigem_target_ds4_alloc()
                    vc.vigem_target_set_vid(target, 0x054c)
                    vc.vigem_target_set_pid(target, product)
                    return target
            pads.append(Pad())
            guids.append(wait(lambda: device_guid((product << 16) | 0x054c), '仮想パッド'))
        caster = sides[0]/'cccaster_B'
        paths = [caster/'cccaster.ini', *[caster/f'Wireless Controller__{guid}.ini' for guid in guids]]
        for path in paths:
            if path not in backups:
                backups[path] = path.read_bytes() if path.exists() else None
        # 後処理の予期しない失敗からも復旧できるよう、変更前のバイト列を保存する。
        (out/'config_backups.json').write_text(json.dumps({str(p):b.hex() if b is not None else None
                                                        for p,b in backups.items()},indent=2),encoding='utf-8')
        paths[0].write_text('[Settings]\n'+''.join(f'P{n}Device=Wireless Controller\nP{n}DeviceGuid={guid}\n' for n,guid in enumerate(guids,1)), encoding='utf-8')
        for path in paths[1:]:
            path.write_text('[Mapping]\nUp=H0_8\nDown=H0_2\nLeft=H0_4\nRight=H0_6\nA=B0\nB=B1\nC=B2\nD=B3\nE=B4\nStart=B7\nFN1=B8\nFN2=B9\n', encoding='utf-8')
        for mode, battle, operation in [('training',False,'escape'), ('offline',False,'close'),
                                        ('offline',False,'escape'), ('training',True,'escape'),
                                        ('training',True,'close'), ('offline',True,'guard')]:
            label = f'{mode}_{"battle" if battle else "selection"}_{operation}'
            print(label, flush=True)
            proc = launch(1, ['--'+mode], label)
            game = attach(1)
            if battle:
                end = time.monotonic()+40
                while not game.battle():
                    if time.monotonic() > end:
                        raise RuntimeError('戦闘に未到達')
                    press_a()
            if operation == 'guard':
                result['checks'][label] = game.blocked()
                # 試験終了だけは明示的なプロセス終了。抑止判定とは区別する。
                game.process.terminate()
            elif operation == 'escape':
                game.key(27)
            else:
                game.post(0x112, 0xf060)
            game.process.wait(timeout=5)
            proc.wait(timeout=10)
            preserve_logs(label)
            result['checks'].setdefault(label, True)
        # ネット対戦のキャラ選択ではEscを相手へ通知して両ゲームを終了する。
        port = free_match_port()
        host = launch(1, ['--headless','--host','--port',str(port)], 'net_selection_host')
        def code():
            text = (out/'net_selection_host_launcher.log').read_text(encoding='utf-8', errors='replace')
            match = re.search(r'\[HEADLESS HOST\] Hash: ([A-Za-z0-9]+)', text)
            return match[1] if match else None
        connection = wait(code, '接続コード')
        peer = launch(2, ['--headless','--hash',connection,'--port',str(port+1)], 'net_selection_peer')
        host_game, peer_game = attach(1), attach(2)
        host_game.key(27)
        host_game.process.wait(timeout=5)
        peer_game.process.wait(timeout=5)
        host.wait(timeout=10)
        peer.wait(timeout=10)
        preserve_logs('net_selection_host',1)
        preserve_logs('net_selection_peer',2)
        peer_log = (out/'net_selection_peer_launcher.log').read_text(encoding='utf-8', errors='replace')
        assert '[ PEER CLOSED ] reason=2' in peer_log
        result['checks']['net_selection_escape_and_notice'] = True
        # 通常の1000確定F・遅延・損失・観戦検証の中で終了抑止も確認する。
        stream = (out/'p2p.log').open('w', encoding='utf-8')
        streams.append(stream)
        proc = subprocess.Popen([sys.executable,'-X','utf8',str(ROOT/'src/src/harness/run_p2p_smoke.py'),
                                 '--real-game','--standby-spectator'], stdout=stream, stderr=subprocess.STDOUT,
                                env=clean_environment(), creationflags=subprocess.CREATE_NO_WINDOW)
        launchers.append(proc)
        network = [Game(side) for side in (1,2)]
        games.extend(network)
        for side, game in enumerate(network,1):
            wait(game.battle, 'ネット対戦開始')
            result['checks'][f'net_battle_guard_{side}'] = game.blocked()
        assert proc.wait(timeout=90) == 0, 'P2P回帰失敗'
        p2p_text = (out/'p2p.log').read_text(encoding='utf-8', errors='replace')
        folders = re.findall(r'I:[^\r\n]*p2p_real_\d+_\d+_\d+', p2p_text)
        assert folders, 'P2P結果パスなし'
        folder = Path(folders[-1].strip())
        p2p_result = json.loads((folder/'result.json').read_text(encoding='utf-8'))
        assert p2p_result['passed'] and p2p_result['protected_unchanged']
        result['p2p_result'] = str(folder/'result.json')
    except Exception:
        result['errors'].append(traceback.format_exc())
    finally:
        # プロセス列挙と終了の間に自然終了しても、設定復元と結果保存を続ける。
        for game in games:
            game.close_handle()
        for side in sides:
            for game in processes(side/'MBAA.exe'):
                try:
                    game.terminate()
                    game.wait(timeout=10)
                except psutil.NoSuchProcess:
                    pass
        for proc in launchers:
            if proc.poll() is None:
                proc.terminate()
                proc.wait(timeout=10)
        for stream in streams:
            stream.close()
        for path, saved in backups.items():
            if saved is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(saved)
        for pad in pads:
            pad.reset()
            pad.update()
        service.shutdown()
        service.server_close()
        result['protected_unchanged'] = before == protected_files(sides)
        result['passed'] = not result['errors'] and result['protected_unchanged']
        (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(result,ensure_ascii=False,indent=2),flush=True)
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
