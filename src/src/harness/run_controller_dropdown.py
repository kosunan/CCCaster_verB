"""実RmlUiの内部候補数を検査し、再描画によるデバイス候補の蓄積を防ぐ。"""
import argparse
import copy
import datetime
import hashlib
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import psutil
from rml_gui_driver import Gui
from test_p2p_service import ROOT, Service


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--test-root',type=Path,required=True)
    parser.add_argument('--visual',action='store_true')
    args=parser.parse_args()
    target=args.test_root.resolve()/'MBAACC_1'
    assert target.is_relative_to((ROOT/'test/runtime').resolve())
    binary=target/'cccaster_B'
    for entry in psutil.process_iter():
        try:
            if Path(psutil.Process(entry.pid).exe()).resolve().is_relative_to(target):
                raise RuntimeError('検証対象が起動中です。先に専用環境へdeployしてください')
        except (psutil.NoSuchProcess,psutil.AccessDenied):pass
    folder=ROOT/'test/logs'/('controller_dropdown_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    folder.mkdir();directory=folder/'ui';directory.mkdir()
    saved={p:p.read_bytes() for p in target.rglob('*.ini*') if p.is_file()}
    game_hash=hashlib.sha256((target/'MBAA.exe').read_bytes()).hexdigest()
    binaries={name:hashlib.sha256((binary/name).read_bytes()).hexdigest() for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll')}
    assert all(h==hashlib.sha256((ROOT/'build/bin'/name).read_bytes()).hexdigest() for name,h in binaries.items())
    result={'passed':False,'checks':[],'binaries':binaries}
    gui=Gui(directory);process=None
    service=Service();threading.Thread(target=service.serve_forever,daemon=True).start()
    env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
    env['CCCASTER_NTFY_SERVER']=f'http://127.0.0.1:{service.server_port}'

    def check(name,data=None):
        data=data or gui.call()
        controller=data['state']['controller'];element=data['elements']['controller-device']
        expected=[{'value':'','label':'割当なし' if data['state']['language']=='ja' else 'Unassigned'}]
        expected += [{'value':d['id'],'label':d['label']} for d in controller['devices']]
        missing=controller['assigned'] and controller['selected'] not in [d['id'] for d in controller['devices']]
        if missing:
            expected.append({'value':'missing','label':'保存済みの機器（未接続／再検出が必要）' if data['state']['language']=='ja' else 'Saved device (disconnected / refresh needed)'})
        assert element['options']==expected,(name,element['options'],expected)
        assert len({o['value'] for o in element['options']})==len(expected),name
        assert element['value']==('missing' if missing else controller['selected']),name
        result['checks'].append({'name':name,'options':len(expected)})
        return data

    try:
        process=subprocess.Popen([str(binary/'CCCaster_B_GUI.exe'),'--ui-test-dir',str(directory)],cwd=binary,env=env)
        print(f'GUI: {folder}',flush=True)
        gui.wait(lambda d:(d.get('state') or {}).get('protocol')==1)
        gui.click('nav-controller');gui.wait(lambda d:len(d['state']['controller']['devices'])>=1)
        check('初回に仮表示--が残らない')
        # 実機の再列挙・画面再入場・P1/P2・言語切替を反復する。
        for cycle in range(12):
            gui.click('controller-refresh');check(f'{cycle}: 再検出')
            for player in (1,0):
                gui.click(f'controller-p{player+1}');gui.wait(lambda d:d['state']['controller']['player']==player)
                check(f'{cycle}: P{player+1}')
            language='en' if gui.call()['state']['language']=='ja' else 'ja'
            gui.click('language');gui.wait(lambda d:d['state']['language']==language)
            check(f'{cycle}: {language}')
            gui.click('nav-guide');gui.click('nav-controller')
            gui.wait(lambda d:len(d['state']['controller']['devices'])>=1)
            check(f'{cycle}: 再入場')
        base=gui.call()['state']
        fixture=copy.deepcopy(base)
        devices=[{'id':'keyboard','label':'Keyboard'},{'id':'pad-one','label':'Same model (00000001)'},{'id':'pad-two','label':'Same model (00000002)'}]
        try:
            for cycle in range(24):
                controller=fixture['controller'];mode=cycle%4
                controller.update(devices=devices if mode!=2 else devices[:1],selected=('pad-one','pad-two','pad-one','')[mode],assigned=mode!=3,connected=mode not in (2,3))
                fixture['language']='ja' if cycle%2 else 'en'
                gui.call('fixture',state=fixture)
                check(f'差分反映{cycle}: 同名2台／保存機器の切断／未割当')
        finally:gui.call('fixture',state={})
        for software in (True,False):
            gui.call('renderer',software=software)
            gui.wait(lambda d:d['page']=='controller' and len(d['state']['controller']['devices'])>=1)
            check('CPU描画の再構築' if software else 'GPU描画の再構築')
        # 最後は現在接続されている候補だけを表示して目視する。
        final=check('最終候補')
        (folder/'inspect.json').write_text(json.dumps(final,ensure_ascii=False,indent=2),encoding='utf-8')
        if args.visual:
            (folder/'visual-ready').write_text(str(process.pid))
            print(f'VISUAL READY: {folder}',flush=True)
            deadline=time.monotonic()+180
            while not (folder/'continue').exists():
                if time.monotonic()>deadline:raise TimeoutError('目視確認待ち')
                time.sleep(.2)
        result['passed']=True
    except Exception as exc:
        result['error']=repr(exc)
        raise
    finally:
        if process and process.poll() is None:process.terminate();process.wait(10)
        for path,content in saved.items():path.write_bytes(content)
        for path in target.rglob('*.ini*'):
            if path.is_file() and path not in saved:
                assert path.resolve().is_relative_to(target)
                path.unlink()
        result['protected_unchanged']=all(p.read_bytes()==v for p,v in saved.items()) and hashlib.sha256((target/'MBAA.exe').read_bytes()).hexdigest()==game_hash
        service.running=False
        with service.cv:service.cv.notify_all()
        service.shutdown();service.server_close()
        (folder/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        print(f'Logs: {folder}',flush=True)
    return 0 if result['passed'] and result['protected_unchanged'] else 1


if __name__=='__main__':raise SystemExit(main())
