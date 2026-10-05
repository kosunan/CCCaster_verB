"""実装済みprobeを使うローカル通知サーバー統合試験。公開ntfyへは送信しない。"""
import contextlib
import http.server
import json
import pathlib
import re
import socket
import subprocess
import tempfile
import threading
import time
import unittest
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[3]
PROBE = ROOT / "build/bin/p2p_probe.exe"


class Service(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.messages = {}
        self.cv = threading.Condition()
        self.requests = []
        self.running = True


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError):
            pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"])).decode()
        topic = self.path.strip("/")
        with self.server.cv:
            self.server.requests.append(("POST", topic))
            event = {"event": "message", "id": str(len(self.server.requests)), "topic": topic,
                     "time": int(time.time()), "message": body}
            self.server.messages.setdefault(topic, []).append(event)
            self.server.cv.notify_all()
        self.send_response(200)
        self.send_header("Content-Length", "2")
        self.end_headers()
        self.wfile.write(b"{}")

    def chunk(self, event):
        line = (json.dumps(event) + "\n").encode()
        self.wfile.write(f"{len(line):x}\r\n".encode() + line + b"\r\n")
        self.wfile.flush()

    def do_GET(self):
        parsed = urllib.parse.urlsplit(self.path)
        topic = parsed.path.split("/")[1]
        query = urllib.parse.parse_qs(parsed.query)
        poll = "poll" in query
        with self.server.cv:
            self.server.requests.append(("POLL" if poll else "STREAM", topic))
            cached = self.server.messages.get(topic, [])[:]
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        try:
            if poll:
                for event in cached[-1:]:
                    self.chunk(event)
                self.wfile.write(b"0\r\n\r\n")
                return
            self.chunk({"event": "open"})
            index = 0
            since = query.get("since", [""])[0]
            if since.isdigit():
                index = next((i + 1 for i, e in enumerate(cached) if e["id"] == since), 0)
            while self.server.running:
                with self.server.cv:
                    entries = self.server.messages.get(topic, [])[index:]
                    index += len(entries)
                for event in entries:
                    self.chunk(event)
                with self.server.cv:
                    self.server.cv.wait(.2)
            self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
        udp.bind(("0.0.0.0", 0))
        return udp.getsockname()[1]


@unittest.skipUnless(PROBE.exists(), "32bit p2p_probeを先にビルド")
class P2PServiceTest(unittest.TestCase):
    def run_pair(self, mode):
        with tempfile.TemporaryDirectory(prefix="cccaster-p2p-") as temp:
            folder = pathlib.Path(temp)
            service = Service()
            server_thread = threading.Thread(target=service.serve_forever, daemon=True)
            server_thread.start()
            processes = []
            files = []
            server = f"http://127.0.0.1:{service.server_port}"
            try:
                host_log = folder / "host.log"
                peer_file = folder / "peer.txt"
                output = open(host_log, "w", encoding="utf-8")
                files.append(output)
                host = subprocess.Popen([str(PROBE), "host", str(free_port()), server,
                                         "offline" if mode != "service" else "local", str(peer_file)],
                                        stdout=output, stderr=subprocess.STDOUT)
                processes.append(host)
                deadline = time.monotonic() + 10
                code = None
                while time.monotonic() < deadline:
                    text = host_log.read_text(encoding="utf-8", errors="replace")
                    prefix = "P2P_MANUAL" if mode == "manual" else "P2P_CODE"
                    match = re.search(r"\[" + prefix + r"\] (\S+)", text)
                    if match:
                        code = match.group(1)
                        break
                    if host.poll() is not None:
                        break
                    time.sleep(.05)
                self.assertTrue(code, text)
                guest_log = folder / "guest.log"
                output = open(guest_log, "w", encoding="utf-8")
                files.append(output)
                guest = subprocess.Popen([str(PROBE), code, str(free_port()), server,
                                          "offline" if mode != "service" else "local"],
                                         stdout=output, stderr=subprocess.STDOUT)
                processes.append(guest)
                if mode == "manual":
                    deadline = time.monotonic() + 10
                    while time.monotonic() < deadline:
                        text = guest_log.read_text(encoding="utf-8", errors="replace")
                        match = re.search(r"\[P2P_MANUAL\] (\S+)", text)
                        if match:
                            peer_file.write_text(match.group(1), encoding="ascii")
                            break
                        time.sleep(.05)
                for process in processes:
                    try:
                        process.wait(timeout=25)
                    except subprocess.TimeoutExpired:
                        self.fail("timeout\n" + host_log.read_text(encoding="utf-8", errors="replace") + "\nGUEST\n" + guest_log.read_text(encoding="utf-8", errors="replace") + "\n" + repr(service.requests))
                for process, path in zip(processes, (host_log, guest_log)):
                    text = path.read_text(encoding="utf-8", errors="replace")
                    self.assertEqual(process.returncode, 0, text)
                    self.assertIn("data=1", text)
                    self.assertEqual(text.count("[INCOMING_REQUEST] "),
                                     1 if path == host_log else 0, text)
                if mode == "service":
                    kinds = [kind for kind, _ in service.requests]
                    self.assertEqual(kinds.count("POLL"), 3)
                    self.assertEqual(kinds.count("STREAM"), 2)
                    self.assertLessEqual(kinds.count("POST"), 6)
                    self.assertGreaterEqual(kinds.count("POST"), 5)
                else:
                    self.assertEqual(service.requests, [])
            finally:
                for process in processes:
                    if process.poll() is None:
                        process.terminate()
                    process.wait(timeout=5)
                for file in files:
                    file.close()
                service.running = False
                with service.cv:
                    service.cv.notify_all()
                service.shutdown()
                service.server_close()

    def test_encrypted_service_exchange(self):
        self.run_pair("service")

    def test_manual_exchange(self):
        self.run_pair("manual")

    def test_offline_lan_discovery(self):
        self.run_pair("lan")


if __name__ == "__main__":
    unittest.main()
