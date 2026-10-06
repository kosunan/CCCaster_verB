"""GUIの実キー・仮想DS4設定と、Training側の設定読込みを検証する。

keyboard-ready.json後にComputer UseでAへKを登録する。visual-ready.json後は
画面を確認してcontinueファイルを作成する。既存のゲームとINIを保全する。
"""
import datetime
import argparse
import configparser
import gc
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time
import psutil
from rml_gui_driver import Gui
from test_p2p_service import ROOT, Service


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-root',type=Path,default=ROOT/'test/runtime')
    args=parser.parse_args()
    folder = ROOT/'test/logs'/('controller_gui_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    folder.mkdir()
    target = args.test_root.resolve()/'MBAACC_1'
    assert target.resolve().is_relative_to((ROOT/'test/runtime').resolve())
    binary = target/'cccaster_B'
    def processes():
        found=[]
        for entry in psutil.process_iter():
            try:
                p=psutil.Process(entry.pid)
                if Path(p.exe()).resolve().is_relative_to(target.resolve()):found.append(p)
            except (psutil.NoSuchProcess,psutil.AccessDenied):pass
        return found
    assert not processes(), '先にdeploy.batを実行してください'
    names=('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')
    hashes={name:hashlib.sha256((binary/name).read_bytes()).hexdigest() for name in names}
    assert all(hashes[n]==hashlib.sha256((ROOT/'build/bin'/n).read_bytes()).hexdigest() for n in names)
    saved={p:p.read_bytes() for p in target.rglob('*.ini*') if p.is_file()}
    game_hash=hashlib.sha256((target/'MBAA.exe').read_bytes()).hexdigest()
    report={'passed':False,'checks':[],'binaries':hashes}
    service=Service();threading.Thread(target=service.serve_forever,daemon=True).start()
    env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
    env.update(CCCASTER_NTFY_SERVER=f'http://127.0.0.1:{service.server_port}',CCCASTER_INPUT_DIAGNOSTIC='1')
    sys.path.insert(0,str(ROOT/'build/virtual-pad-venv/Lib/site-packages'))
    import vgamepad as vg
    pads=[]
    def check(name,condition=True):
        assert condition,name
        report['checks'].append(name)
        print(name,flush=True)
    def write(name,data):
        (folder/name).write_text(json.dumps(data,ensure_ascii=False,indent=2),encoding='utf-8')
    def wait_file(name,seconds=600):
        until=time.monotonic()+seconds
        while not (folder/name).exists():
            if time.monotonic()>until:raise TimeoutError(name)
            time.sleep(.2)
    try:
        # 試験用割当のみ変更し、finallyでINI・バックアップをバイト単位で戻す。
        with (binary/'cccaster.ini').open('a',encoding='utf-8') as out:
            out.write('\n[Settings]\nP1Device=Keyboard\nP1DeviceGuid=\nP2Device=\nP2DeviceGuid=\n[GUI]\nLanguage=ja\n')
        (binary/'Keyboard.ini').write_text('[Mapping]\nA=S\nB=D\nUp_Alt=UpArrow\nTrainingSave=Q\n[Keep]\nValue=kept\n',encoding='utf-8')
        old=binary/'cccaster_hook_log.txt'
        if old.exists():shutil.copyfile(old,folder/'before-game.log')
        directory=folder/'ui';directory.mkdir();gui=Gui(directory)
        process=subprocess.Popen([str(binary/'CCCaster_B_GUI.exe'),'--ui-test-dir',str(directory)],cwd=binary,env=env)
        gui.wait(lambda d:(d.get('state') or {}).get('protocol')==1)
        gui.click('nav-controller')
        state=gui.wait(lambda d:d['state']['controller']['selected']=='keyboard')
        check('コントローラ設定タブ・ゲーム内F4の小型案内',state['elements']['controller-game-hint']['visible'] and 'F4' in state['elements']['controller-game-hint']['text'])
        write('keyboard-ready.json',{'pid':process.pid,'folder':str(folder)})
        print(f'KEYBOARD READY: {folder}',flush=True)
        gui.wait(lambda d:d['state']['controller']['bindings'][4]=='K',seconds=600)
        check('実ウィンドウのキー入力でAへKを登録・重複した下入力を解除',gui.call()['state']['controller']['bindings'][1]=='--')
        profile=configparser.ConfigParser(interpolation=None)
        profile.read(binary/'Keyboard.ini',encoding='utf-8')
        check('補助方向・TrainingSave・未知の設定を保持',profile['Mapping']['Up_Alt']=='UpArrow' and profile['Mapping']['TrainingSave']=='Q' and profile['Keep']['Value']=='kept')
        gui.click('controller-p2');gui.value('controller-device','keyboard')
        gui.wait(lambda d:bool(d['state']['controller']['notice']))
        data=gui.call()
        check('1P/2Pへの同一機器重複を拒否して表示も戻す',data['state']['controller']['selected']=='' and data['elements']['controller-device']['value']=='')
        gui.click('controller-p1');gui.click('controller-clear-4')
        gui.wait(lambda d:d['state']['controller']['bindings'][4]=='--')
        gui.click('nav-guide');gui.click('nav-controller')
        check('解除・画面再入場時の保存読込み',gui.call()['state']['controller']['bindings'][4]=='--')
        gui.click('controller-bind-4');gui.wait(lambda d:d['state']['controller']['capture']==4)
        gui.click('controller-cancel');gui.wait(lambda d:d['state']['controller']['capture']==-1)
        check('入力中止',gui.call()['state']['controller']['capture']==-1)
        for language in ['en','ja']:
            gui.click('language');gui.wait(lambda d:d['state']['language']==language)
            for scale in [1.,1.5]:
                gui.call('dpi',value=scale);data=gui.call()
                check(f'{language} DPI {scale} 横はみ出しなし',not data['overflow'])
        gui.call('dpi',value=1.)
        # 描画を再作成しても選択画面・割当を保つ。
        gui.call('renderer',software=True)
        gui.wait(lambda d:d['page']=='controller' and d['state']['controller']['selected']=='keyboard')
        check('CPU描画へ切替後の設定タブ・保存内容維持')
        before={d['id'] for d in gui.call()['state']['controller']['devices']}
        pads=[vg.VDS4Gamepad(),vg.VDS4Gamepad()]
        time.sleep(1)
        gui.click('controller-refresh')
        data=gui.wait(lambda d:len([v for v in d['state']['controller']['devices'] if v['id'] not in before])==2)
        devices=[d for d in data['state']['controller']['devices'] if d['id'] not in before]
        check('同名DS4の2個体を別々のGUIDで検出',devices[0]['id']!=devices[1]['id'])
        write('pad-ready.json',{'devices':devices})
        print(f'PAD READY: {folder}',flush=True)
        wait_file('pad-continue')
        gui.value('controller-device',devices[0]['id'])
        gui.wait(lambda d:d['state']['controller']['selected']==devices[0]['id'])
        # 列挙順ではなく、実入力を出した個体を対応付ける。
        def capture(row,operate,expected):
            gui.click(f'controller-bind-{row}');gui.wait(lambda d:d['state']['controller']['capture']==row);time.sleep(.3)
            # 他作業で前面が変わった時間は入力を採用しない。前面に戻るまで再入力する。
            deadline=time.monotonic()+60
            while time.monotonic()<deadline:
                operate();time.sleep(.3)
                for p in pads:p.reset();p.update()
                time.sleep(.3)
                data=gui.call()
                write('capture.json',data['state']['controller'])
                if data['state']['controller']['capture']==-1:break
            else:raise TimeoutError('foreground controller capture')
            check(f'仮想DS4 {row} = {expected}',gui.call()['state']['controller']['bindings'][row]==expected)
        def button():
            for p in pads:p.press_button(button=vg.DS4_BUTTONS.DS4_BUTTON_TRIANGLE);p.update()
        capture(4,button,'Button 4')
        check('同じパッドボタンの以前の割当を解除',gui.call()['state']['controller']['bindings'][7]=='--')
        def hat():
            for p in pads:p.directional_pad(direction=vg.DS4_DPAD_DIRECTIONS.DS4_BUTTON_DPAD_NORTH);p.update()
        capture(0,hat,'D-pad UP')
        def axis():
            for p in pads:p.left_joystick(x_value=255,y_value=128);p.update()
        capture(3,axis,'A0+')
        gui.click('controller-p2');gui.value('controller-device',devices[1]['id'])
        gui.wait(lambda d:d['state']['controller']['selected']==devices[1]['id'])
        check('同名2台を1P/2Pへ個別保存')
        gui.click('controller-p1');gui.wait(lambda d:d['state']['controller']['player']==0)
        gui.click('controller-bind-5');gui.wait(lambda d:d['state']['controller']['capture']==5)
        pads.clear();gc.collect()
        gui.wait(lambda d:d['state']['controller']['capture']==-1 and not d['state']['controller']['connected'])
        check('入力待ち中の切断で中止・保存済み設定を保持')
        gui.click('controller-refresh')
        data=gui.call()
        check('切断した個体を別の同名機器へ付替えない',data['state']['controller']['assigned'] and not data['state']['controller']['connected'])
        # ゲーム読込みはKeyboardのA未割当・補助設定保持を照合する。
        gui.value('controller-device','keyboard');gui.wait(lambda d:d['state']['controller']['selected']=='keyboard')
        write('visual-ready.json',gui.call())
        print(f'VISUAL READY: {folder}',flush=True)
        wait_file('continue')
        gui.click('nav-training');gui.click('training')
        gui.wait(lambda d:d['state']['session']['game'],seconds=45)
        gui.click('nav-controller')
        gui.wait(lambda d:d['state']['controller']['locked'])
        check('ゲーム起動中のGUI編集禁止',gui.call()['elements']['controller-device']['disabled'])
        deadline=time.monotonic()+30
        while time.monotonic()<deadline:
            log=(binary/'cccaster_hook_log.txt').read_text(encoding='utf-8',errors='replace')
            if '[InputConfig] P1 A=' in log and '[InputConfig] P1 device=Keyboard' in log:break
            time.sleep(.2)
        else:raise TimeoutError('game input config')
        check('実TrainingがGUIの保存したKeyboard割当を読込み','[InputConfig] P1 A=\n' in log)
        games=[p for p in processes() if Path(p.exe()).name.lower()=='mbaa.exe']
        assert len(games)==1
        games[0].terminate();games[0].wait(10)
        gui.wait(lambda d:not d['state']['session']['running'],seconds=30)
        gui.wait(lambda d:not d['state']['controller']['locked'] and d['state']['controller']['connected'])
        check('ゲーム終了後のGUI編集再開')
        report['passed']=True
    except Exception as exc:
        report['error']=repr(exc)
        if 'process' in locals():report['gui_exit_code']=process.poll()
        raise
    finally:
        for p in processes():
            try:p.terminate();p.wait(10)
            except psutil.NoSuchProcess:pass
        pads.clear();gc.collect()
        log=binary/'cccaster_hook_log.txt'
        if log.exists():shutil.copyfile(log,folder/'game.log')
        for path,content in saved.items():path.write_bytes(content)
        for path in target.rglob('*.ini*'):
            if path.is_file() and path not in saved:
                assert path.resolve().is_relative_to(target.resolve())
                path.unlink()
        report['protected_unchanged']=all(p.read_bytes()==v for p,v in saved.items()) and hashlib.sha256((target/'MBAA.exe').read_bytes()).hexdigest()==game_hash
        service.running=False
        with service.cv:service.cv.notify_all()
        service.shutdown();service.server_close()
        write('result.json',report)
        print(f'Logs: {folder}',flush=True)
    return 0 if report['passed'] and report['protected_unchanged'] else 1


if __name__=='__main__':raise SystemExit(main())
