"""ローカル通知サービス・実GUI・実ゲームでTraining待受から対戦を確認。

decline/acceptのstage.jsonを出したら、Computer Useで対象ゲームのF6/F5を押す。
ネット対戦は既存脚本入力と1000確定F・損失ありの比較で条件達成時に終了する。
"""
import datetime
import argparse
import hashlib
import json
import os
import pathlib
import random
import shutil
import subprocess
import tempfile
import threading
import time
import psutil
from test_p2p_service import ROOT, Service
from rml_gui_driver import Gui
from run_stage_rematch import free_match_port
from real_game_checkpoint import clean_environment, evaluate, protected_hashes


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--battle',action='store_true',help='専用の仮想DS4でTraining戦闘へ進んでから通知を検証する')
    args=parser.parse_args()
    runtime = ROOT/'test/runtime'
    folder = ROOT/'test/logs'/('training_standby_'+datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    folder.mkdir()
    targets = [runtime/f'MBAACC_{side}' for side in (1, 2)]
    def processes():
        found = []
        for entry in psutil.process_iter():
            try:
                p = psutil.Process(entry.pid)
                exe = pathlib.Path(p.exe()).resolve()
                if any(exe.is_relative_to(target.resolve()) for target in targets): found.append(p)
            except (psutil.NoSuchProcess, psutil.AccessDenied): pass
        return found
    def game(side):
        found = [p for p in processes() if pathlib.Path(p.exe()).resolve() == (targets[side]/'MBAA.exe').resolve()]
        assert len(found) <= 1, '旧ゲームと新ゲームの同時起動'
        return found[0] if found else None
    if processes(): raise RuntimeError('先にdeploy.batで対象テスト環境を更新してください')
    for target in targets:
        for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll'):
            assert (target/'cccaster_B'/name).read_bytes() == (ROOT/'build/bin'/name).read_bytes()
    before = protected_hashes(runtime)
    saved = {p:p.read_bytes() for target in targets for p in target.rglob('*.ini*') if p.is_file()}
    service = Service(); threading.Thread(target=service.serve_forever,daemon=True).start()
    env = clean_environment()
    env.update(CCCASTER_NTFY_SERVER=f'http://127.0.0.1:{service.server_port}',
               CCCASTER_SCRIPT_INPUT='1',CCCASTER_INPUT_TRACE='1',CCCASTER_MEM_TRACE='1',
               CCCASTER_TIME_SCALE='1',CCCASTER_TEST_NETWORK='15,25,5')
    guis, children, outputs = [], [], []
    pad = None
    result = dict(passed=False, checks=[], environment={k:v for k,v in env.items() if k.startswith('CCCASTER_')})
    def wait(predicate, timeout=35):
        end = time.monotonic()+timeout
        while time.monotonic()<end:
            value = predicate()
            if value: return value
            time.sleep(.1)
        raise TimeoutError('Training待受試験の条件待ち: '+str(folder))
    def log(side=0):
        path=targets[side]/'cccaster_B/cccaster_hook_log.txt'
        return path.read_text(encoding='utf-8',errors='replace') if path.exists() else ''
    def stage(name, request):
        (folder/'stage.json').write_text(json.dumps(dict(stage=name,request=request,game_pid=initial.pid)),encoding='utf-8')
        print(f'Computer Use: {name} ({folder})',flush=True)
    def check(label): result['checks'].append(label)
    try:
        if args.battle:
            import vgamepad as vg
            from vgamepad.win import vigem_client as vc
            product=random.SystemRandom().randrange(0x8000,0xFFFF)
            class TestPad(vg.VDS4Gamepad):
                def target_alloc(self):
                    target=vc.vigem_target_ds4_alloc()
                    vc.vigem_target_set_vid(target,0x054C);vc.vigem_target_set_pid(target,product)
                    return target
            pad=TestPad()
            env['CCCASTER_TEST_VIRTUAL_PRODUCT']=f'{(product<<16)|0x054C:08X}'
            env['CCCASTER_TRAINING_TRACE']='1'
            result['environment'].update({k:v for k,v in env.items() if k.startswith('CCCASTER_')})
        for side,target in enumerate(targets,1):
            old=target/'cccaster_B/cccaster_hook_log.txt'
            if old.exists(): old.replace(folder/f'before_{side}.log')
            ui=folder/f'ui_{side}';ui.mkdir();guis.append(Gui(ui))
            out=open(folder/f'gui_{side}.log','w',encoding='utf-8');outputs.append(out)
            children.append(subprocess.Popen([str(target/'cccaster_B/CCCaster_B_GUI.exe'),'--ui-test-dir',str(ui)],
                cwd=target/'cccaster_B',env=env,stdout=out,stderr=subprocess.STDOUT))
        a,b=guis
        for gui,name in zip(guis,('TRAINING HOST','WAITING OPPONENT')):
            gui.wait(lambda d: (d.get('state') or {}).get('protocol')==1)
            gui.settings();gui.call('focus',id='player-name');gui.value('player-name',name);gui.click('settings-back')
            gui.checked('training-standby',False)
            gui.click('matching-options-toggle');gui.value('matching-port',free_match_port())
        a.checked('training-standby',True)
        assert a.call()['state']['settings']['TrainingStandby']
        a.click('start-matching')
        state=a.wait(lambda d: d['state']['matching']['trainingStandby'] and d['state']['matching']['registered'])['state']
        code=state['matching']['code']
        initial=wait(lambda: game(0))
        worker=pathlib.Path(tempfile.gettempdir())/f'CCCaster_B_GUI_{children[0].pid}.log'
        wait(lambda: '[ TRAINING READY ]' in worker.read_text(encoding='utf-8',errors='replace'))
        assert not a.call()['state']['matching']['otherMode']
        check('チェックONで公開募集とTraining起動、受付を休止しない')
        if args.battle:
            deadline=time.monotonic()+35
            while '[TrainingAdvantage] valid=1' not in log():
                if time.monotonic()>deadline:raise TimeoutError('Training戦闘へ未到達')
                pad.press_button(vg.DS4_BUTTONS.DS4_BUTTON_SQUARE);pad.update();time.sleep(.12)
                pad.reset();pad.update();time.sleep(.16)
            check('実際のTraining戦闘中に通知を確認')
        def invite():
            b.select_player(code);b.click('invite-selected')
            data=a.wait(lambda d: bool(d['state']['matching']['incoming']))
            request=data['state']['matching']['incoming'][0]['id']
            wait(lambda: f'SHOW request={request}' in log())
            return request
        request=invite();b.click('cancel-request')
        a.wait(lambda d: not d['state']['matching']['incoming'])
        wait(lambda: f'HIDE request={request}' in log())
        assert game(0).pid==initial.pid
        check('申込取消でゲーム内通知を消し、同じTrainingを保持')
        request=invite();stage('decline',request)
        wait(lambda: f'REPLY request={request} decision=DECLINE' in log(),120)
        a.wait(lambda d: not d['state']['matching']['incoming'])
        b.wait(lambda d: not d['state']['matching']['outgoing']['id'])
        assert game(0).pid==initial.pid
        check('ゲーム内F6で拒否し、TrainingのPIDを保持')
        request=invite();stage('accept',request)
        wait(lambda: not psutil.pid_exists(initial.pid),120)
        new=wait(lambda: game(0))
        assert new.pid!=initial.pid
        result['training_pid']=initial.pid;result['match_pid']=new.pid
        check('ゲーム内F5承諾後にTrainingを終了、新しいPIDで対戦起動')
        for gui in guis: gui.wait(lambda d: d['state']['matching']['state']=='playing',90)
        marker='[InitThread] Starting hook initialization...'
        # 元ログは全て保全し、比較器へは2番目の起動（対戦）だけを渡す。
        def snapshots():
            for side in (0,1):
                text=log(side);parts=text.split(marker)
                index=2 if side==0 else 1
                assert len(parts)>index
                (folder/f'game_{side+1}.log').write_text(marker+parts[index],encoding='utf-8')
        started=time.monotonic();last={}
        while time.monotonic()-started<55:
            snapshots();last=evaluate(folder,{})
            if last['passed']: break
            time.sleep(1)
        (folder/'checkpoint.json').write_text(json.dumps(last,ensure_ascii=False,indent=2),encoding='utf-8')
        assert last.get('passed'), '同期比較が条件未達'
        check('片道15〜25ms・損失5%、1000以上の確定F一致・ロールバックあり')
        game(1).terminate()
        a.wait(lambda d: d['state']['matching']['trainingStandby'] and d['state']['matching']['state']=='waiting',40)
        resumed=wait(lambda: game(0))
        assert resumed.pid not in (initial.pid,new.pid)
        assert a.call()['state']['matching']['code']==code
        snapshots();result.update(evaluate(folder,{}))
        check('対戦終了後は同じ募集コードでTraining待受へ復帰')
        result['resumed_training_pid']=resumed.pid
        b.wait(lambda d: not d['state']['session']['running'])
        guest_worker=pathlib.Path(tempfile.gettempdir())/f'CCCaster_B_GUI_{children[1].pid}.log'
        close_log=guest_worker.read_text(encoding='utf-8',errors='replace')
        result['exit_notice_ack']='[ CLOSE ACK ]' in close_log
        assert result['exit_notice_ack'], '終了通知の受領確認なし'
        a.click('stop-matching');a.wait(lambda d: not d['state']['matching']['registered'])
        assert game(0).pid==resumed.pid
        b.click('stop-matching');b.wait(lambda d: not d['state']['matching']['registered'])
        check('募集終了後は通知を解除し、ローカルTrainingを継続')
        result['passed']=True
    except Exception as exc:
        result.update(passed=False,error=repr(exc))
        raise
    finally:
        if pad:pad.reset();pad.update()
        for side,p in enumerate(children,1):
            for prefix,label in (('CCCaster_B_GUI_','worker'),('CCCaster_B_RmlGui_','display')):
                source=pathlib.Path(tempfile.gettempdir())/f'{prefix}{p.pid}.log'
                if source.exists():shutil.copyfile(source,folder/f'{label}_{side}.log')
        for side in (0,1):
            (folder/f'all_game_{side+1}.log').write_text(log(side),encoding='utf-8')
        cleanup=processes()
        for p in cleanup:
            try: p.terminate()
            except psutil.NoSuchProcess: pass
        psutil.wait_procs(cleanup,timeout=10)
        for out in outputs: out.close()
        for path,data in saved.items():path.write_bytes(data)
        for target in targets:
            for path in target.rglob('*.ini*'):
                if path.is_file() and path not in saved:
                    assert path.resolve().is_relative_to(target.resolve());path.unlink()
        result['protected_unchanged']=before==protected_hashes(runtime)
        result['protected_count']=len(before)
        result['passed'] &= result['protected_unchanged']
        (folder/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        service.running=False
        with service.cv:service.cv.notify_all()
        service.shutdown();service.server_close()
        print(f'Logs: {folder}',flush=True)
    return 0 if result['passed'] else 1


if __name__=='__main__':raise SystemExit(main())
