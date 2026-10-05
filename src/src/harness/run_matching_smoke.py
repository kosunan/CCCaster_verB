"""ローカル通知サーバーでマッチング→実対戦→観戦→同じ登録へ復帰を確認する。"""
import argparse
import datetime
import json
import os
import pathlib
import re
import shutil
import subprocess
import threading
import time
import tempfile
import psutil
from test_matching_service import Peer, PROBE, DIRECTORY
from test_p2p_service import ROOT, Service, free_port


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gui", action="store_true", help="GUIを手操作し、invite.txtへそのコードを書く")
    parser.add_argument("--seconds", type=int, default=40)
    args = parser.parse_args()
    folder = ROOT / "test/logs" / ("matching_" + ("gui_" if args.gui else "real_") + datetime.datetime.now().strftime("%Y%m%d_%H%M%S"))
    folder.mkdir()
    service = Service()
    threading.Thread(target=service.serve_forever, daemon=True).start()
    server = f"http://127.0.0.1:{service.server_port}"
    env = os.environ.copy()
    env.update(CCCASTER_NTFY_SERVER=server, CCCASTER_SCRIPT_INPUT="1", CCCASTER_INPUT_TRACE="1",
               CCCASTER_MEM_TRACE="1", CCCASTER_TIME_SCALE="1", CCCASTER_TEST_NETWORK="15,25,5")
    peers, workers, outputs, gui_logs = [], [], [], {}
    runtime = ROOT / "test/runtime"
    for side in (1, 2, 3):
        dest = runtime / f"MBAACC_{side}/cccaster_B"
        for name in ("CCCaster_B.exe", "CCCaster_B_GUI.exe", "libcccaster_hook.dll"):
            import hashlib
            assert hashlib.sha256((dest/name).read_bytes()).digest() == hashlib.sha256((ROOT/"build/bin"/name).read_bytes()).digest(), "先にdeploy.ps1で最新版を配置"

    def launch(side, arguments, gui=False):
        target = runtime / f"MBAACC_{side}/cccaster_B"
        old_log=(target/"cccaster_hook_log.txt").resolve()
        archive=(folder/f"before_{side}.log").resolve()
        assert old_log.is_relative_to(runtime.resolve()) and archive.is_relative_to((ROOT/"test/logs").resolve())
        if old_log.exists():
            old_log.replace(archive)
        output = open(folder/f"launcher_{side}.log", "w", encoding="utf-8")
        outputs.append(output)
        startup=None
        if not gui:
            # 既存の実ゲームharnessと同じくコンソールを隠して起動する。
            # CREATE_NO_WINDOWではこの環境の3窓目が初期化途中で停止した。
            startup=subprocess.STARTUPINFO()
            startup.dwFlags|=subprocess.STARTF_USESHOWWINDOW
            startup.wShowWindow=0
        process = subprocess.Popen([str(target/("CCCaster_B_GUI.exe" if gui else "CCCaster_B.exe")), *arguments],
                                   env=env, cwd=target, stdout=output, stderr=subprocess.STDOUT,
                                   startupinfo=startup,creationflags=0 if gui else subprocess.CREATE_NEW_CONSOLE)
        workers.append(process)
        if gui:
            gui_logs[side]=pathlib.Path(tempfile.gettempdir())/f"CCCaster_B_GUI_{process.pid}.log"
        return process

    def text(side):
        return (folder/f"launcher_{side}.log").read_text(encoding="utf-8", errors="replace")

    def wait_text(side, needle, timeout=35):
        deadline = time.monotonic()+timeout
        while time.monotonic()<deadline:
            value=text(side)
            if needle in value:
                return value
            time.sleep(.1)
        raise RuntimeError(f"side {side}: {needle}\n{text(side)}")

    def game_processes():
        paths = {str(runtime/f"MBAACC_{side}/MBAA.exe").lower() for side in (1,2,3)}
        found=[]
        for process in psutil.process_iter(["exe"]):
            try:
                if (process.info["exe"] or "").lower() in paths:
                    found.append(process)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return found

    try:
        if game_processes():
            raise RuntimeError("テスト環境に実行中のゲームがあります。deploy.ps1を先に実行")
        b=Peer(server); peers.append(b)
        b.send("start", name="MATCHING TEST PEER", comment="Manual approval test", public=True, spectators=True)
        b.wait(lambda s:s.get("registered") and s.get("public"))
        initial_b=b.state["code"]
        if args.gui:
            launch(1, [], gui=True)
            subprocess.run([str(PROBE), server, "seed"], check=True, stdout=subprocess.DEVNULL)
            (folder/"ready.json").write_text(json.dumps(dict(server=server, peer=initial_b, folder=str(folder))), encoding="utf-8")
            print(f"GUI ready. Write GUI code to {folder/'invite.txt'}", flush=True)
            deadline=time.monotonic()+600
            while not (folder/"invite.txt").exists() and time.monotonic()<deadline:
                time.sleep(.2)
            code=(folder/"invite.txt").read_text(encoding="utf-8").strip()
            b.send("invite", code=code)
            event=b.event(timeout=180)
            assert not event["host"], event
            launch(2, ["join",event["code"],"--port",str(free_port()),"--allow-spectators"])
            wait_text(2,"[P2P_STATUS] connected")
            b.send("playing",match=event["match"])
            match=event["match"]
        else:
            a=Peer(server);peers.append(a)
            a.send("start", name="PRIVATE TEST PEER", comment="", public=False, spectators=True)
            a.wait(lambda s:s.get("registered"))
            initial_a=a.state["code"]
            # 申し込み側の個人コードで、対戦前から観戦待機する。
            launch(3,["spectate",initial_a])
            wait_text(3,"[WATCH_STATUS] standby")
            a.send("invite",code=initial_b)
            request=b.wait(lambda s:len(s.get("incoming",[]))==1)["incoming"][0]["id"]
            assert not a.events and not b.events
            b.send("accept",request=request)
            event=b.event();assert event["host"]
            match=event["match"]
            launch(1,["host","--port",str(free_port()),"--allow-spectators"])
            code=re.search(r"\[P2P_CODE\] (\S+)",wait_text(1,"[P2P_CODE]"))[1]
            b.send("host_ready",match=match,code=code)
            guest=a.event();assert not guest["host"] and guest["match"]==match
            launch(2,["join",guest["code"],"--port",str(free_port()),"--allow-spectators"])
            for side,peer in ((1,b),(2,a)):
                wait_text(side,"[P2P_STATUS] connected")
                peer.send("playing",match=match)
            wait_text(3,"[WATCH_STATUS] ready")
            wait_text(3,"[ IN GAME ]",timeout=35)
            # 承諾側の個人コードからも、同じ対戦の実TCP待受へ到達する。
            host_watch=subprocess.run([str(ROOT/"build/bin/p2p_probe.exe"), "watch:"+initial_b, "0", server,
                                       "local", str(folder/"host_watch.control"), "allow"],
                                      capture_output=True,text=True,encoding="utf-8",timeout=30,
                                      creationflags=subprocess.CREATE_NO_WINDOW)
            (folder/"host_watch.log").write_text(host_watch.stdout+host_watch.stderr,encoding="utf-8")
            assert host_watch.returncode==0 and "WATCH_READY" in host_watch.stdout, host_watch.stdout
        before=sum(kind=="POST" and topic==DIRECTORY for kind,topic in service.requests)
        wait_text(2,"[ IN GAME ]")
        print(f"Playing for {args.seconds}s. Logs: {folder}",flush=True)
        time.sleep(args.seconds)
        for game in game_processes():
            if pathlib.Path(game.exe()).parent.name=="MBAACC_2":
                game.terminate(); game.wait(timeout=5)
        for worker in workers:
            if not args.gui or worker is not workers[0]:
                worker.wait(timeout=15)
                assert worker.returncode==0, f"launcher {worker.pid} exited with {worker.returncode}"
        for peer in peers:
            peer.send("finished",match=match)
            peer.wait(lambda s:s["state"]=="waiting")
        assert b.state["code"]==initial_b
        if not args.gui:
            assert a.state["code"]==initial_a
        assert before==sum(kind=="POST" and topic==DIRECTORY for kind,topic in service.requests)
        (folder/"result.json").write_text(json.dumps(dict(passed=True, same_code=True, public_posts_during_game=0),indent=2),encoding="utf-8")
        if args.gui:
            print("GUI returned to waiting. Finish inspection within 60s.",flush=True)
            time.sleep(60)
        return 0
    finally:
        for game in game_processes():
            try: game.terminate();game.wait(timeout=5)
            except psutil.NoSuchProcess: pass
        for worker in workers:
            if worker.poll() is None:
                worker.terminate()
            worker.wait(timeout=10)
        for peer in peers:
            (folder/f"matching_{peers.index(peer)}.log").write_text("".join(peer.lines),encoding="utf-8")
            peer.close()
        for output in outputs:output.close()
        for side,source in gui_logs.items():
            if source.exists():shutil.copyfile(source,folder/f"gui_worker_{side}.log")
        for side in (1,2,3):
            source=runtime/f"MBAACC_{side}/cccaster_B/cccaster_hook_log.txt"
            if source.exists():shutil.copyfile(source,folder/f"game_{side}.log")
        service.running=False
        with service.cv:service.cv.notify_all()
        service.shutdown();service.server_close()
        (folder/"service_requests.json").write_text(json.dumps(service.requests,indent=2),encoding="utf-8")
        print(f"Logs: {folder}",flush=True)


if __name__=="__main__":raise SystemExit(main())
