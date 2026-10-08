"""実通信probeと実ゲームのP2P確認。ログをtest/logsへ保存する。"""
import argparse
import datetime
import json
import pathlib
import re
import subprocess
import shutil
import sys
import threading
import time
from test_p2p_service import ROOT, PROBE, Service, free_port
from real_game_checkpoint import clean_environment, evaluate, protected_hashes


def run_real(args, output):
    from run_stage_rematch import free_match_port
    shell = shutil.which('pwsh')
    if not shell:
        raise RuntimeError('PowerShell 7 (pwsh)が必要です')
    env = clean_environment()
    env['CCCASTER_NTFY_SERVER'] = args.server
    if args.extra_color: env['CCCASTER_TEST_EXTRA_COLOR']='1'
    if args.input_runahead:
        env['CCCASTER_TEST_INPUT_RUNAHEAD_SIDES'] = args.input_runahead
    if args.present_rollback is not None:
        env['CCCASTER_TEST_PRESENT_ROLLBACK_SIDES'] = args.present_rollback
    if args.monitor_timing:
        env.update(CCCASTER_MONITOR_PRESENT_TRACE='1', CCCASTER_FRAME_TIMING_TRACE='1',
                   CCCASTER_UPDATE_CADENCE='1')
    if args.input_handoff_trace:
        env.update(CCCASTER_INPUT_SEND_TRACE='1', CCCASTER_UPDATE_CADENCE='1',
                   CCCASTER_MONITOR_PRESENT_TRACE='1')
    if args.legacy_input_handoff:
        env.update(CCCASTER_TEST_INPUT_PHASE_US='3000')
    if args.monitor_hz:
        env['CCCASTER_TEST_MONITOR_HZ'] = str(args.monitor_hz)
    config = dict(spectator=args.standby_spectator and not args.no_spectators, input_runahead=args.input_runahead)
    config['native_input_writes'] = args.present_rollback == '12'
    config['present_rollback'] = dict(lead=1 if args.present_rollback == '12' else 0, delay=args.delay)
    if args.selection_options:
        env.update(CCCASTER_TEST_SELECTION_OPTIONS='1', CCCASTER_TEST_FIXED_STAGE='59')
        config['selection_options'] = True
    config_path = output / 'checkpoint_config.json'
    config_path.write_text(json.dumps(config), encoding='utf-8')
    command = [shell, '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
               str(ROOT / 'src/src/harness/run_bounded_real_pair.ps1'),
               '-Seconds', str(args.seconds), '-Port', str(free_match_port()),
               '-Network', '65,85,5' if args.selection_options else '15,25,5',
               '-UseConnectionCode', '-CloseSide', '1', '-OutputDirectory', str(output)]
    if not args.fixed_duration:
        command += ['-CheckpointConfig', str(config_path), '-Python', sys.executable]
    if args.standby_spectator:
        command.append('-StandbySpectator')
    if args.no_spectators:
        command.append('-NoSpectators')
    runtime = args.test_root.resolve()
    command += ['-TestRoot', str(runtime)]
    before = protected_hashes(runtime)
    settings = {runtime / f'MBAACC_{side}/cccaster_B/cccaster.ini': None for side in (1,2)} if args.delay is not None else {}
    for path in settings: settings[path] = path.read_bytes() if path.exists() else None
    (output / 'protected_before.json').write_text(json.dumps(before, indent=2), encoding='utf-8')
    result = dict(passed=False, command=command,
                  environment={k: v for k, v in env.items() if k.startswith('CCCASTER_')})
    started = time.monotonic()
    try:
        for path, saved in settings.items():
            data=saved or b''
            section=re.search(rb'(?ims)^\[Netplay\]\r?\n(.*?)(?=^\[|\Z)',data)
            entry=f'DefaultDelay={args.delay}\r\n'.encode('ascii')
            if section:
                body=re.sub(rb'(?im)^DefaultDelay\s*=.*\r?\n?',b'',section[1])
                data=data[:section.start(1)]+entry+body+data[section.end(1):]
            else: data+=b'\r\n[Netplay]\r\n'+entry
            path.write_bytes(data)
        run = subprocess.run(command, env=env)
        result['exit_code'] = run.returncode
        result.update(evaluate(output, config))
        if args.extra_color:
            color_logs={role:(output/f'game_{side}.log').read_text(encoding='utf-8',errors='replace') for side,role in ((1,'host'),(2,'client'))}
            result['extra_colors']={role:bool(re.search(r'\[ExtraColor\] LOAD slot=0 character=0 .*matched=1 applied=1',data)) for role,data in color_logs.items()}
            result['passed'] &= all(result['extra_colors'].values())
        result['passed'] &= run.returncode == 0
    except Exception as exc:
        result.update(passed=False, error=str(exc))
    finally:
        for path, saved in settings.items():
            if saved is None: path.unlink(missing_ok=True)
            else: path.write_bytes(saved)
        result['protected_unchanged'] = before == protected_hashes(runtime)
        result['protected_count'] = len(before)
        result['passed'] &= result['protected_unchanged']
        result['elapsed_seconds'] = round(time.monotonic() - started, 3)
        (output / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps(dict(passed=result['passed'], seconds=result['elapsed_seconds'], logs=str(output))), flush=True)
    return 0 if result['passed'] else 1


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", help="省略時はローカルの通知サーバー")
    parser.add_argument("--real-game", action="store_true")
    parser.add_argument('--input-runahead', choices=['1', '2', '12'], help='指定した端だけで1F先行表示を検証')
    parser.add_argument('--present-rollback', choices=['0','1','2','12'], help='対応ビットを送る端。片側だけなら双方の入力前倒しが無効になることを検査')
    parser.add_argument('--delay',type=int,choices=range(9),help='試験中だけ両者の初期Dを指定し、元のINIを復元する')
    parser.add_argument("--standby-spectator", action="store_true", help="対戦参加前に6文字コードで観戦待機")
    parser.add_argument("--no-spectators", action="store_true", help="観戦拒否と実ゲームのTCP待受停止を確認")
    parser.add_argument('--seconds', type=int, default=40, help='試験上限秒。条件達成で早期終了')
    parser.add_argument('--fixed-duration', action='store_true', help='指定秒まで継続する比較・耐久用')
    parser.add_argument('--selection-options', action='store_true', help='キャラ選択の設定メニューと背景ON/OFF混在を検査')
    parser.add_argument('--extra-color',action='store_true',help='保存済みホストEXTRA 6の転送・両側適用を検査')
    parser.add_argument('--monitor-timing', action='store_true', help='実Presentと60Hz更新を別々に採取する')
    parser.add_argument('--input-handoff-trace', action='store_true', help='採取・公開からゲーム注入までを実QPCで記録する')
    parser.add_argument('--legacy-input-handoff', action='store_true', help='比較専用：旧3msの入力公開位相')
    parser.add_argument('--monitor-hz', type=int, choices=range(20, 1001), metavar='20..1000',
                        help='検証専用の表示要求Hz。実モニター設定は変更しない')
    parser.add_argument('--test-root', type=pathlib.Path, default=ROOT / 'test/runtime', help='独立したMBAACC_1〜3の親フォルダー')
    args = parser.parse_args()
    if args.seconds < 1:
        parser.error('--secondsは1以上')
    output = ROOT / "test/logs" / ("p2p_real_" if args.real_game else "p2p_smoke_")
    output = output.with_name(output.name + datetime.datetime.now().strftime("%Y%m%d_%H%M%S_%f"))
    output.mkdir()
    service = None
    if not args.server:
        service = Service()
        threading.Thread(target=service.serve_forever, daemon=True).start()
        args.server = f"http://127.0.0.1:{service.server_port}"
    processes, files = [], []
    try:
        if args.real_game:
            return run_real(args, output)
        for role in ("host", "guest"):
            code = "host"
            if role == "guest":
                deadline = time.monotonic() + 30
                while time.monotonic() < deadline:
                    text = (output / "host.log").read_text(encoding="utf-8", errors="replace")
                    match = re.search(r"\[P2P_CODE\] (\S+)", text)
                    if match:
                        code = match.group(1)
                        break
                    if processes[0].poll() is not None:
                        raise RuntimeError("host exited before issuing code")
                    time.sleep(.1)
                if code == "host":
                    raise RuntimeError("host code timeout")
            file = open(output / f"{role}.log", "w", encoding="utf-8")
            files.append(file)
            process = subprocess.Popen([str(PROBE), code, str(free_port()), args.server, "local"],
                                       stdout=file, stderr=subprocess.STDOUT)
            processes.append(process)
        for process in processes:
            process.wait(timeout=55)
        return 0 if all(p.returncode == 0 for p in processes) else 1
    finally:
        for process in processes:
            if process.poll() is None:
                process.terminate()
            process.wait(timeout=5)
        for file in files:
            file.close()
        if service:
            service.running = False
            with service.cv:
                service.cv.notify_all()
            service.shutdown()
            service.server_close()
            (output / "service_requests.txt").write_text(repr(service.requests), encoding="utf-8")
        print(f"Logs: {output}", flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
