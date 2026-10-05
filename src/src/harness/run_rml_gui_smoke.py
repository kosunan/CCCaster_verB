"""RmlUi GUI 3窓から承諾・実対戦・観戦・復帰を検証。通知先はローカルに限定。"""
import argparse
import datetime
import hashlib
import json
import os
import pathlib
import shutil
import subprocess
import tempfile
import threading
import time
import psutil
from test_p2p_service import ROOT, Service, free_port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seconds', type=int, default=40)
    parser.add_argument('--direct', action='store_true', help='空欄待受とコード入力接続の実ゲーム試験')
    parser.add_argument('--emblems', action='store_true', help='BMP国旗プリセットと対戦・観戦への画像共有を確認')
    args = parser.parse_args()
    folder = ROOT/'test/logs'/(('rml_gui_direct_' if args.direct else 'rml_gui_')+datetime.datetime.now().strftime('%Y%m%d_%H%M%S'))
    folder.mkdir()
    targets = [ROOT/f'test/runtime/MBAACC_{i}' for i in (1,2,3)]
    def targets_running():
        result=[]
        for process in psutil.process_iter(['exe']):
            try:
                # PID再利用後のprocess_iterキャッシュで別EXEと誤認しない。
                current=psutil.Process(process.pid)
                exe=pathlib.Path(current.exe() or '').resolve()
                if any(exe.is_relative_to(target.resolve()) for target in targets):result.append(current)
            except (psutil.NoSuchProcess,psutil.AccessDenied):pass
        return result
    if targets_running():raise RuntimeError('先にdeploy.batでテスト環境のプロセスを終了してください')
    for target in targets:
        for name in ('CCCaster_B.exe','CCCaster_B_GUI.exe','libcccaster_hook.dll'):
            assert hashlib.sha256((target/'cccaster_B'/name).read_bytes()).digest()==hashlib.sha256((ROOT/'build/bin'/name).read_bytes()).digest()
    saved = {p:p.read_bytes() for target in targets for p in target.rglob('*.ini*') if p.is_file()}
    emblem_paths = [target/'cccaster_B'/name for target in targets for name in ('player-emblem.bmp','player-emblem.bin')]
    saved_emblems = {p:p.read_bytes() if p.exists() else None for p in emblem_paths}
    service=Service();threading.Thread(target=service.serve_forever,daemon=True).start()
    env={k:v for k,v in os.environ.items() if not k.startswith('CCCASTER_')}
    env.update(CCCASTER_NTFY_SERVER=f'http://127.0.0.1:{service.server_port}',CCCASTER_SCRIPT_INPUT='1',
               CCCASTER_INPUT_TRACE='1',CCCASTER_MEM_TRACE='1',CCCASTER_TIME_SCALE='1',CCCASTER_TEST_NETWORK='15,25,5')
    fixture={'gamePorts':[free_port(),free_port(),free_port()]}
    from rml_gui_driver import Gui, before_game, after_game
    if args.direct:
        from rml_gui_driver import before_direct_game as before_game, after_direct_game as after_game
    guis=[];report={'checks':[], 'passed':False}
    (folder/'fixture.json').write_text(json.dumps(fixture),encoding='utf-8')
    processes=[];outputs=[];result={'passed':False}
    try:
        subprocess.run([str(ROOT/'build/bin/matching_probe.exe'),env['CCCASTER_NTFY_SERVER'],'seed'],check=True,stdout=subprocess.DEVNULL,creationflags=subprocess.CREATE_NO_WINDOW)
        for side,target in enumerate(targets,1):
            old=target/'cccaster_B/cccaster_hook_log.txt'
            if old.exists():old.replace(folder/f'before_game_{side}.log')
            out=open(folder/f'gui_{side}.log','w',encoding='utf-8');outputs.append(out)
            directory=folder/f'ui_{side}';directory.mkdir();guis.append(Gui(directory))
            cmd=[str(target/'cccaster_B/CCCaster_B_GUI.exe'),'--ui-test-dir',str(directory)]
            if side==2:cmd.append('--software-rendering')
            processes.append(subprocess.Popen(cmd,cwd=target/'cccaster_B',env=env,stdout=out,stderr=subprocess.STDOUT))
        (folder/'processes.json').write_text(json.dumps([p.pid for p in processes]),encoding='utf-8')
        print(f'GUI試験開始: {folder}',flush=True)
        if args.emblems:
            from rml_gui_driver import check_emblems
            check_emblems(guis, targets, report)
        codes=before_game(guis,fixture['gamePorts'],report)
        (folder/'playing-request.json').write_text(json.dumps(codes))
        deadline=time.monotonic()+160
        while time.monotonic()<deadline:
            if any(p.poll() is not None for p in processes):raise RuntimeError('GUI terminated')
            logs=[]
            for p in processes[:3]:
                path=pathlib.Path(tempfile.gettempdir())/f'CCCaster_B_GUI_{p.pid}.log'
                logs.append(path.read_text(encoding='utf-8',errors='replace') if path.exists() else '')
            if (folder/'playing-request.json').exists() and all('[ IN GAME ]' in log for log in logs):break
            time.sleep(.2)
        else:raise RuntimeError('GUIからの対戦・観戦起動が完了しませんでした')
        print(f'実ゲーム3窓: {args.seconds}秒、15〜25ms／損失5%',flush=True)
        # この待機中、GUIの非表示時描画停止もネイティブログへ記録される。
        time.sleep(args.seconds)
        if args.emblems:
            first, second = [entry['id'] for entry in report['emblems']]
            texts = [(target/'cccaster_B/cccaster_hook_log.txt').read_text(encoding='utf-8', errors='replace') for target in targets]
            assert f'[Emblem] peer received id={second:08x}' in texts[0]
            assert f'[Emblem] peer received id={first:08x}' in texts[1]
            for side, image_id in enumerate((first, second), 1):
                assert f'[Emblem] spectator player={side} id={image_id:08x}' in texts[2]
            assert all(gui.call()['elements']['emblem-preset-jp']['disabled'] for gui in guis)
            report['checks'].append('対戦双方と観戦で日本・フランスの画像ID一致、接続中の変更禁止')
        closing=[process for process in targets_running() if pathlib.Path(process.exe()).resolve()==(targets[1]/'MBAA.exe').resolve()]
        if len(closing)!=1:raise RuntimeError(f'終了対象ゲームを一意に特定できません: {len(closing)}件')
        closing[0].terminate();closing[0].wait(timeout=10)
        (folder/'game-close.json').write_text(json.dumps({'pid':closing[0].pid,'path':str(targets[1]/'MBAA.exe'),'exited':True}),encoding='utf-8')
        (folder/'game-ended').write_text('1')
        after_game(guis,codes,report)
        result={'passed':True,'gui_to_game':True,'direct':args.direct,'seconds':args.seconds,'network':'15,25,5'}
        print('RmlUi操作と待受復帰に成功。同期ログを保存します。',flush=True)
    except Exception as exc:
        result['error']=str(exc)
        report['error']=str(exc)
        result['gui_exit_codes']=[p.poll() for p in processes[:3]]
        raise
    finally:
        cleanup=targets_running()
        for process in cleanup:
            try:process.terminate()
            except psutil.NoSuchProcess:pass
        psutil.wait_procs(cleanup,timeout=10)
        for p in processes:
            try:
                if p.poll() is None:p.terminate()
                p.wait(timeout=10)
            except (subprocess.TimeoutExpired,ProcessLookupError):pass
        for out in outputs:out.close()
        for side,(p,target) in enumerate(zip(processes[:3],targets),1):
            log=pathlib.Path(tempfile.gettempdir())/f'CCCaster_B_GUI_{p.pid}.log'
            if log.exists():shutil.copyfile(log,folder/f'worker_{side}.log')
            display=pathlib.Path(tempfile.gettempdir())/f'CCCaster_B_RmlGui_{p.pid}.log'
            if display.exists():shutil.copyfile(display,folder/f'display_{side}.log')
            log=target/'cccaster_B/cccaster_hook_log.txt'
            if log.exists():shutil.copyfile(log,folder/f'game_{side}.log')
        for path,contents in saved.items():path.write_bytes(contents)
        for path,contents in saved_emblems.items():
            if contents is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(contents)
        for target in targets:
            for path in target.rglob('*.ini*'):
                if path.is_file() and path not in saved:
                    # この試験が設定保存で新規作成したINI/バックアップだけを除去。
                    assert path.resolve().is_relative_to(target.resolve())
                    path.unlink()
        (folder/'ui-result.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf-8')
        result['ini_preserved']=all(path.read_bytes()==contents for path,contents in saved.items())
        result['emblems_preserved']=all((path.read_bytes() if path.exists() else None)==contents for path,contents in saved_emblems.items())
        service.running=False
        with service.cv:service.cv.notify_all()
        service.shutdown();service.server_close()
        (folder/'service_requests.json').write_text(json.dumps(service.requests),encoding='utf-8')
        (folder/'result.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(f'Logs: {folder}',flush=True)
    return 0


if __name__=='__main__':raise SystemExit(main())
