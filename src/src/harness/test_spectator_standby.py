"""6文字観戦は対戦枠を消費せず、開始・取消・拒否・募集終了を識別する。"""
import pathlib
import re
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
from test_p2p_service import PROBE, Service, free_port


@unittest.skipUnless(PROBE.exists(), "32bit p2p_probeを先にビルド")
class SpectatorStandbyTest(unittest.TestCase):
    def scenario(self, action):
        with tempfile.TemporaryDirectory(prefix="cccaster-watch-") as temp:
            folder = pathlib.Path(temp)
            service = Service()
            threading.Thread(target=service.serve_forever, daemon=True).start()
            server = f"http://127.0.0.1:{service.server_port}"
            processes, files = [], []
            tcp = None
            try:
                port = free_port()
                def launch(name, role, own_port, permission="allow"):
                    output = open(folder / (name + ".log"), "w", encoding="utf-8")
                    files.append(output)
                    process = subprocess.Popen([str(PROBE), role, str(own_port), server, "local",
                                                str(folder / (name + ".control")), permission],
                                               stdout=output, stderr=subprocess.STDOUT)
                    processes.append(process)
                    return process

                def log(name):
                    return (folder / (name + ".log")).read_text(encoding="utf-8", errors="replace")

                def wait_for(name, needle, timeout=12):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        text = log(name)
                        if needle in text:
                            return text
                        time.sleep(.05)
                    self.fail(name + " did not reach " + needle + "\n" + log(name))

                host = launch("host", "host", port, "no-spectators" if action == "disabled" else "allow")
                text = wait_for("host", "[P2P_CODE]")
                code = re.search(r"\[P2P_CODE\] (\S+)", text)[1]
                posts_before = sum(kind == "POST" for kind, _ in service.requests)
                viewer = launch("viewer", "watch:" + code.lower(), 0)
                if action == "disabled":
                    wait_for("viewer", "[WATCH_STATUS] disabled")
                else:
                    wait_for("viewer", "[WATCH_STATUS] standby")
                    self.assertIsNone(viewer.poll())
                    self.assertEqual(posts_before, sum(kind == "POST" for kind, _ in service.requests))
                    self.assertNotIn("[P2P_EXCHANGE]", log("host"))
                    if action == "cancel":
                        (folder / "viewer.control").write_text("cancel")
                        wait_for("viewer", "[WATCH_STATUS] cancelled")
                    elif action == "closed":
                        (folder / "host.control").write_text("cancel")
                        host.wait(timeout=5)
                        wait_for("viewer", "[WATCH_STATUS] closed")
                    else:
                        tcp = socket.socket()
                        tcp.bind(("0.0.0.0", port))
                        tcp.listen(8)
                        tcp.settimeout(5)
                        def greet():
                            try:
                                while True:
                                    conn, _ = tcp.accept()
                                    with conn:
                                        conn.settimeout(3)
                                        hello = b""
                                        while len(hello) < 16:
                                            part = conn.recv(16 - len(hello))
                                            if not part: break
                                            hello += part
                                        if hello == struct.pack("<4I", 0x53504343, 4, 0x01071400, 5):
                                            conn.sendall(hello)
                            except (OSError, TimeoutError):
                                pass
                        threading.Thread(target=greet, daemon=True).start()
                        guest = launch("guest", code, free_port())
                        wait_for("viewer", "WATCH_READY")
                        self.assertIn("[WATCH_STATUS] connecting", log("viewer"))
                        guest.wait(timeout=12)
                        host.wait(timeout=12)
                        self.assertEqual(guest.returncode, 0, log("guest"))
                        self.assertEqual(host.returncode, 0, log("host"))
                viewer.wait(timeout=5)
                self.assertEqual(viewer.returncode, 0 if action == "start" else 1, log("viewer"))
                self.assertNotIn("WATCH_READY", log("viewer")) if action != "start" else None
            finally:
                for process in processes:
                    if process.poll() is None: process.terminate()
                    process.wait(timeout=5)
                for file in files: file.close()
                if tcp: tcp.close()
                service.running = False
                with service.cv: service.cv.notify_all()
                service.shutdown()
                service.server_close()

    def test_standby_then_start_without_join_request(self): self.scenario("start")
    def test_standby_cancel(self): self.scenario("cancel")
    def test_host_closes_before_match(self): self.scenario("closed")
    def test_host_denies_spectators(self): self.scenario("disabled")


if __name__ == "__main__":
    unittest.main()
