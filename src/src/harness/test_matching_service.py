"""公開／非公開と手動承諾を実Clientプロセス＋ローカルntfy互換サービスで検証。"""
import base64
import json
import subprocess
import tempfile
import threading
import time
import unittest
from unittest import mock
import urllib.request
from pathlib import Path
import psutil
from test_p2p_service import ROOT, Service, Handler

PROBE = ROOT / "build/bin/matching_probe.exe"
DIRECTORY = (subprocess.run([str(PROBE), "--directory-topic"], check=True, capture_output=True,
                           text=True, encoding="utf-8", timeout=5).stdout.strip() if PROBE.exists() else "")


def directory_records(events):
    result = subprocess.run([str(PROBE), "--open-directory"],
                            input="".join(e["message"] + "\n" for e in events),
                            check=True, capture_output=True, text=True, encoding="utf-8", timeout=5)
    return [json.loads(line) for line in result.stdout.splitlines()]


def seal_directory(record):
    return subprocess.run([str(PROBE), "--seal-directory"], input=json.dumps(record) + "\n",
                          check=True, capture_output=True, text=True, encoding="utf-8", timeout=5).stdout.strip()


class Peer:
    def __init__(self, server, seconds=60, cleanup_directory=None):
        args = [str(PROBE), server, str(seconds)]
        if cleanup_directory is not None:
            args.append(str(cleanup_directory))
        self.process = subprocess.Popen(args, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                        text=True, encoding="utf-8", bufsize=1)
        self.cv = threading.Condition()
        self.state, self.events, self.lines = {}, [], []
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for line in self.process.stdout:
            with self.cv:
                self.lines.append(line)
                try:
                    value = json.loads(line)
                    if value["kind"] == "snapshot":
                        self.state = value
                    else:
                        self.events.append(value)
                except (ValueError, KeyError):
                    pass
                self.cv.notify_all()

    def send(self, kind, **values):
        self.process.stdin.write(json.dumps(dict(type=kind, **values)) + "\n")
        self.process.stdin.flush()

    def wait(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        with self.cv:
            while time.monotonic() < deadline:
                if predicate(self.state):
                    return dict(self.state)
                self.cv.wait(.05)
        raise AssertionError("Matching timeout: " + "".join(self.lines[-15:]))

    def event(self, kind="launch", timeout=15):
        deadline = time.monotonic() + timeout
        with self.cv:
            while time.monotonic() < deadline:
                for event in self.events:
                    if event["type"] == kind:
                        self.events.remove(event)
                        return event
                self.cv.wait(.05)
        raise AssertionError("Event timeout: " + "".join(self.lines[-15:]))

    def close(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            self.process.wait(timeout=5)
        self.reader.join(timeout=3)
        self.process.stdout.close()


@unittest.skipUnless(PROBE.exists(), "matching_probeの32bitビルドが必要")
class MatchingServiceTest(unittest.TestCase):
    def setUp(self):
        self.service = Service()
        threading.Thread(target=self.service.serve_forever, daemon=True).start()
        self.server = f"http://127.0.0.1:{self.service.server_port}"
        self.peers = []

    def tearDown(self):
        for peer in self.peers:
            peer.close()
        self.service.running = False
        with self.service.cv:
            self.service.cv.notify_all()
        self.service.shutdown()
        self.service.server_close()

    def peer(self, name, public=False, seconds=60, spectators=True, cleanup_directory=None):
        peer = Peer(self.server, seconds, cleanup_directory)
        self.peers.append(peer)
        peer.send("start", name=name, comment="test", public=public, spectators=spectators)
        peer.wait(lambda s: s.get("registered") and s.get("public") == public)
        return peer

    def public_posts(self):
        return sum(kind == "POST" and topic == DIRECTORY for kind, topic in self.service.requests)

    def recovery_folder(self):
        temporary = tempfile.TemporaryDirectory(prefix='cccaster-cancellation-')
        self.addCleanup(temporary.cleanup)
        return Path(temporary.name)

    def recovery_peer(self, folder, server=None):
        peer = Peer(server or self.server, cleanup_directory=folder)
        self.peers.append(peer)
        return peer

    def crash_all(self, peer):
        children = psutil.Process(peer.process.pid).children()
        helpers = [p for p in children if Path(p.exe()).resolve() == PROBE.resolve()
                   and '--matching-cleanup' in p.cmdline()]
        self.assertEqual(len(helpers), 1)
        # この試験が起動した取消用の子だけ先に止め、終了通知の送信を不能にする。
        helpers[0].kill();helpers[0].wait(timeout=5)
        peer.process.kill();peer.process.wait(timeout=5)

    def test_recovery_preserves_live_registration_and_protects_saved_data(self):
        folder = self.recovery_folder()
        owner = self.peer('DURABLE OWNER', True, cleanup_directory=folder)
        files = list(folder.glob('*.pending'))
        self.assertEqual(len(files), 1)
        for value in ('DURABLE OWNER', owner.state['code'], self.server, 'records', DIRECTORY):
            self.assertNotIn(value.encode(), files[0].read_bytes())
        posts = self.public_posts()
        another = self.recovery_peer(folder)
        another.wait(lambda s: s.get('service') == 'online')
        another.send('cleanup')
        time.sleep(.25)
        self.assertEqual(self.public_posts(), posts, '動作中の別ウィンドウの募集は取り消さない')
        self.assertTrue(files[0].exists())
        owner.send('stop');owner.wait(lambda s: not s['registered'])
        self.assertFalse(list(folder.glob('*.pending')), '通常取消成功時は保存データも削除')

    def test_recovery_after_parent_and_helper_crash_cancels_once(self):
        folder = self.recovery_folder()
        observer = self.peer('OBSERVER')
        owner = self.peer('LOST OWNER', True, cleanup_directory=folder)
        observer.wait(lambda s: len(s['people']) == 1)
        code = owner.state['code'];posts = self.public_posts()
        self.crash_all(owner)
        recovered = self.recovery_peer(folder)
        recovered.wait(lambda s: s.get('notice') == 'cleanup_finished')
        observer.wait(lambda s: not s['people'])
        self.assertEqual(self.public_posts(), posts + 1)
        self.assertFalse(list(folder.glob('*.pending')))
        observer.send('invite', code=code)
        observer.wait(lambda s: s.get('notice') == 'closed')
        recovered.send('cleanup');time.sleep(.25)
        self.assertEqual(self.public_posts(), posts + 1, '取消成功後の再操作では重複投稿しない')
        late = self.recovery_peer(folder)
        late.wait(lambda s: s.get('service') == 'online')
        self.assertFalse(late.state['people'], '履歴を再取得しても募集が復活しない')

    def test_recovery_failed_startup_can_retry_manually(self):
        folder = self.recovery_folder()
        owner = self.peer('RETRY LOST OWNER', True, cleanup_directory=folder)
        self.crash_all(owner)
        original = Handler.do_POST
        def unavailable(handler):
            if handler.server is self.service:
                handler.rfile.read(int(handler.headers['Content-Length']))
                handler.send_response(503);handler.send_header('Content-Length', '0');handler.end_headers()
            else:
                original(handler)
        with mock.patch.object(Handler, 'do_POST', unavailable):
            recovered = self.recovery_peer(folder)
            recovered.wait(lambda s: s.get('cleanupPending') == 1 and s.get('error') == 'cleanup_pending')
            self.assertEqual(len(list(folder.glob('*.pending'))), 1)
        recovered.send('cleanup')
        recovered.wait(lambda s: s.get('cleanupPending') == 0 and s.get('notice') == 'cleanup_finished')
        self.assertFalse(list(folder.glob('*.pending')))

    def test_recovery_does_not_send_to_another_server_or_discard_corrupt_data(self):
        folder = self.recovery_folder()
        owner = self.peer('SERVER OWNER', True, cleanup_directory=folder)
        self.crash_all(owner)
        original_file = next(folder.glob('*.pending'))
        other = Service();threading.Thread(target=other.serve_forever, daemon=True).start()
        try:
            client = self.recovery_peer(folder, f'http://127.0.0.1:{other.server_port}')
            client.wait(lambda s: s.get('service') == 'online')
            self.assertFalse(any(kind == 'POST' for kind, _ in other.requests))
            self.assertTrue(original_file.exists())
            client.close();self.peers.remove(client)
        finally:
            other.running = False
            with other.cv: other.cv.notify_all()
            other.shutdown();other.server_close()
        corrupt = folder / ('f'*32 + '.pending');corrupt.write_bytes(b'broken cancellation')
        recovered = self.recovery_peer(folder)
        recovered.wait(lambda s: s.get('cleanupPending') == 1 and not s['people'])
        self.assertFalse(original_file.exists())
        self.assertEqual(corrupt.read_bytes(), b'broken cancellation')

    def test_recovery_rate_limit_survives_restart(self):
        folder = self.recovery_folder()
        owner = self.peer('RATE LIMITED OWNER', True, cleanup_directory=folder)
        self.crash_all(owner)
        original = Handler.do_POST;attempts = []
        def limited(handler):
            if handler.server is self.service:
                handler.rfile.read(int(handler.headers['Content-Length']));attempts.append(time.monotonic())
                handler.send_response(429);handler.send_header('Retry-After', '90')
                handler.send_header('Content-Length', '0');handler.end_headers()
            else:
                original(handler)
        with mock.patch.object(Handler, 'do_POST', limited):
            first = self.recovery_peer(folder)
            first.wait(lambda s: s.get('cleanupPending') == 1 and s.get('error') == 'rate_limited')
            first.close();self.peers.remove(first)
            second = self.recovery_peer(folder)
            second.wait(lambda s: s.get('cleanupPending') == 1 and s.get('error') == 'rate_limited')
            second.send('cleanup');time.sleep(.25)
        self.assertEqual(len(attempts), 1, '再起動と手動再操作でもRetry-Afterを無視しない')
        self.assertEqual(len(list(folder.glob('*.pending'))), 1)

    def test_registration_requires_durable_cancellation_storage(self):
        folder = self.recovery_folder();blocked = folder / 'not-a-directory';blocked.write_text('preserve')
        owner = self.recovery_peer(blocked)
        owner.send('start', name='UNSAVED OWNER', public=True)
        owner.wait(lambda s: s.get('error') == 'cleanup_unavailable' and not s['registered'])
        self.assertFalse(any(kind == 'POST' for kind, _ in self.service.requests))
        self.assertEqual(blocked.read_text(), 'preserve')

    def test_process_exit_cancels_without_explicit_stop(self):
        observer = self.peer("OBSERVER")
        owner = self.peer("EXIT OWNER", True)
        observer.wait(lambda s: len(s["people"]) == 1)
        posts = self.public_posts()
        owner.close()
        self.peers.remove(owner)
        observer.wait(lambda s: not s["people"])
        self.assertEqual(self.public_posts(), posts + 1, "通常終了の取消を監視側から重複送信しない")
        observer.send("invite", code=owner.state["code"])
        observer.wait(lambda s: s.get("notice") == "closed")

    def test_abnormal_exit_cancels_latest_public_listing(self):
        observer = self.peer("OBSERVER")
        owner = self.peer("CRASH OWNER", True)
        observer.wait(lambda s: len(s["people"]) == 1)
        owner.send("visibility", public=False)
        observer.wait(lambda s: not s["people"])
        owner.wait(lambda s: not s["public"])
        owner.send("visibility", public=True)
        owner.wait(lambda s: s["public"])
        observer.wait(lambda s: len(s["people"]) == 1)
        posts = self.public_posts()
        owner.process.kill()  # デストラクタを通らないプロセス単体の強制終了。
        owner.process.wait(timeout=5)
        observer.wait(lambda s: not s["people"])
        self.assertEqual(self.public_posts(), posts + 1)
        records = directory_records(self.service.messages[DIRECTORY])
        self.assertEqual(records[-1]["action"], "remove")
        self.assertGreater(records[-1]["revision"], records[-2]["revision"])
        observer.send("invite", code=owner.state["code"])
        observer.wait(lambda s: s.get("notice") == "closed")
        late = self.peer("AFTER CRASH")
        self.assertFalse(late.state["people"], "後から履歴を読んでも終了済みの募集が復活しない")

    def test_abnormal_private_exit_does_not_post_public_cancellation(self):
        observer = self.peer("OBSERVER")
        owner = self.peer("PRIVATE CRASH")
        posts = self.public_posts()
        before = sum(len(events) for events in self.service.messages.values())
        owner.process.kill()
        owner.process.wait(timeout=5)
        deadline = time.monotonic() + 10
        with self.service.cv:
            while sum(len(events) for events in self.service.messages.values()) == before and time.monotonic() < deadline:
                self.service.cv.wait(.1)
        observer.send("invite", code=owner.state["code"])
        observer.wait(lambda s: s.get("notice") == "closed")
        self.assertEqual(self.public_posts(), posts)

    def test_exit_cancellation_retries_after_temporary_service_failure(self):
        observer = self.peer("OBSERVER")
        owner = self.peer("RETRY OWNER", True)
        observer.wait(lambda s: len(s["people"]) == 1)
        original = Handler.do_POST
        failed = []
        def flaky(handler):
            if handler.server is self.service and handler.path.strip('/') == DIRECTORY and len(failed) < 2:
                handler.rfile.read(int(handler.headers['Content-Length']))
                failed.append(time.monotonic())
                handler.send_response(503)
                handler.send_header('Content-Length', '0')
                handler.end_headers()
            else:
                original(handler)
        posts = self.public_posts()
        with mock.patch.object(Handler, 'do_POST', flaky):
            owner.close()
            self.peers.remove(owner)
            observer.wait(lambda s: not s["people"])
        self.assertEqual(len(failed), 2, "通常終了と補助プロセスの最初の取消を失敗させる")
        self.assertEqual(self.public_posts(), posts + 1)
        observer.send("invite", code=owner.state["code"])
        observer.wait(lambda s: s.get("notice") == "closed")

    def test_manual_approval_decline_cancel_and_reuse(self):
        a, b = self.peer("PUBLIC", True), self.peer("PRIVATE")
        a.wait(lambda s: len(s["people"]) == 1)
        b.wait(lambda s: len(s["people"]) == 1)
        code_a, code_b = a.state["code"], b.state["code"]
        posts = self.public_posts()
        b.send("invite", code=code_a)
        request = a.wait(lambda s: len(s.get("incoming", [])) == 1)["incoming"][0]["id"]
        self.assertFalse(a.events)
        self.assertFalse(b.events)
        a.send("reject", request=request)
        b.wait(lambda s: s.get("notice") == "declined" and not s["outgoing"])
        b.send("invite", code=code_a)
        a.wait(lambda s: len(s.get("incoming", [])) == 1)
        b.send("cancel")
        a.wait(lambda s: not s["incoming"])
        b.wait(lambda s: not s["outgoing"])
        a.send("invite", code=code_b)
        request = b.wait(lambda s: len(s.get("incoming", [])) == 1)["incoming"][0]["id"]
        b.send("accept", request=request)
        host = b.event()
        self.assertTrue(host["host"])
        self.assertFalse(a.events, "承諾側の接続準備前に参加側を起動しない")
        b.send("host_ready", match=host["match"], code="XYZ234")
        guest = a.event()
        self.assertFalse(guest["host"])
        self.assertEqual(host["match"], guest["match"])
        self.assertEqual(guest["code"], "XYZ234")
        for peer in (a, b):
            peer.send("playing", match=host["match"])
            peer.wait(lambda s: s["state"] == "playing")
        self.assertEqual(self.public_posts(), posts)
        for peer in (a, b):
            peer.send("finished", match=host["match"])
            peer.wait(lambda s: s["state"] == "waiting")
        self.assertEqual((a.state["code"], b.state["code"]), (code_a, code_b))
        self.assertEqual(self.public_posts(), posts)
        a.send("stop")
        a.wait(lambda s: not s["registered"])
        b.wait(lambda s: not s["people"])
        self.assertEqual(self.public_posts(), posts + 1)
        self.assertRegex(DIRECTORY, r"^[0-9a-f]{32}$")
        for _, topic in self.service.requests:
            self.assertRegex(topic, r"^[0-9a-f]{32}$", "全通信先にツール名・用途を含めない")
        for events in self.service.messages.values():
            for event in events:
                body = event["message"]
                self.assertLessEqual(len(body), 4096)
                self.assertGreaterEqual(len(base64.b64decode(body, validate=True)), 28)
                self.assertNotIn('"name"', body)
                self.assertNotIn('"action"', body)
                with self.assertRaises(json.JSONDecodeError):
                    json.loads(body)

    def test_mutual_request_busy_and_both_spectator_permissions(self):
        a, b, c = self.peer("A", True), self.peer("B", True, spectators=False), self.peer("C")
        a.send("invite", code=b.state["code"])
        b.send("invite", code=a.state["code"])
        deadline = time.monotonic() + 15
        host_peer = None
        while time.monotonic() < deadline:
            if a.events or b.events:
                host_peer = a if a.events else b
                break
            time.sleep(.05)
        self.assertIsNotNone(host_peer)
        other = b if host_peer is a else a
        host = host_peer.event()
        self.assertTrue(host["host"])
        self.assertFalse(host["spectators"])
        host_peer.send("host_ready", match=host["match"], code="XYZ234")
        guest = other.event()
        self.assertEqual(host["match"], guest["match"])
        self.assertFalse(guest["spectators"])
        c.send("invite", code=a.state["code"])
        c.wait(lambda s: s.get("notice") == "busy")
        self.assertFalse(c.events)
        self.assertFalse(a.events)
        self.assertFalse(b.events)

    def test_timeout_and_old_accept_do_not_start_game(self):
        a, b = self.peer("A"), self.peer("B", seconds=3)
        b.send("invite", code=a.state["code"])
        request = a.wait(lambda s: len(s.get("incoming", [])) == 1)["incoming"][0]["id"]
        b.wait(lambda s: s.get("notice") == "no_response")
        a.wait(lambda s: not s["incoming"])
        a.send("accept", request=request)
        time.sleep(.3)
        self.assertFalse(a.events)
        self.assertFalse(b.events)
        self.assertEqual(a.state["state"], "waiting")

    def test_visibility_and_pause_keep_private_registration(self):
        a, b = self.peer("A"), self.peer("B")
        code = a.state["code"]
        self.assertEqual(self.public_posts(), 0)
        a.send("visibility", public=True)
        b.wait(lambda s: len(s["people"]) == 1)
        a.send("pause", paused=True)
        b.send("invite", code=code)
        b.wait(lambda s: s.get("notice") == "paused")
        self.assertFalse(a.events)
        a.send("visibility", public=False)
        b.wait(lambda s: not s["people"])
        self.assertEqual(a.state["code"], code)
        self.assertEqual(self.public_posts(), 2)
        a.send("pause", paused=False)
        b.send("invite", code=code)
        a.wait(lambda s: len(s["incoming"]) == 1)

    def test_cancel_preserves_start_cleanup_without_extra_posts(self):
        a, b = self.peer("PUBLIC", True), self.peer("PRIVATE")
        code = b.state["code"]
        subprocess.run([str(PROBE), self.server, "seed-cleanup"], check=True, timeout=15)
        initial = b.wait(lambda s: len(s["people"]) == 3)
        self.assertEqual({p["name"] for p in initial["people"]}, {"PUBLIC", "FRESH", "RELISTED"},
                         "開始時の掃除境界より古い投稿は後から届いても復活しない")
        records = directory_records(self.service.messages[DIRECTORY])
        published = next(r for r in records if r["name"] == "PUBLIC")
        self.assertGreaterEqual(published["listed_at"], published["created"])
        posts = self.public_posts()
        a.send("stop")
        a.wait(lambda s: not s["registered"])
        remaining = b.wait(lambda s: len(s["people"]) == 2)
        self.assertEqual({p["name"] for p in remaining["people"]}, {"FRESH", "RELISTED"})
        self.assertEqual(self.public_posts(), posts + 1, "取消と掃除は同じ1投稿")
        cancellation = directory_records(self.service.messages[DIRECTORY][-1:])[0]
        self.assertEqual(cancellation["action"], "remove")
        self.assertLessEqual(abs(cancellation["cleanup_before"] - (int(time.time()) - 6*60*60)), 5)
        self.assertTrue(b.state["registered"])
        self.assertEqual(b.state["code"], code)
        c = self.peer("LATE READER")
        replayed = c.wait(lambda s: len(s["people"]) == 2)
        self.assertEqual({p["name"] for p in replayed["people"]}, {"FRESH", "RELISTED"})
        b.send("stop")
        b.wait(lambda s: not s["registered"])
        self.assertEqual(self.public_posts(), posts + 1, "非公開登録の取消には公開投稿を足さない")

    def test_start_and_relisting_clean_without_extra_posts_and_ignore_garbage(self):
        observer = self.peer("PRIVATE")
        subprocess.run([str(PROBE), self.server, "seed-cleanup"], check=True, timeout=15)
        observer.wait(lambda s: len(s["people"]) == 4)
        posts = self.public_posts()
        newcomer = self.peer("NEW PUBLIC", True)
        remaining = observer.wait(lambda s: len(s["people"]) == 3)
        self.assertEqual({p["name"] for p in remaining["people"]}, {"FRESH", "RELISTED", "NEW PUBLIC"})
        self.assertEqual(self.public_posts(), posts + 1, "開始と掃除は同じ1投稿")
        record = directory_records(self.service.messages[DIRECTORY][-1:])[0]
        self.assertEqual(record["action"], "register")
        self.assertEqual(record["cleanup_before"], record["listed_at"] - 6*60*60)
        code = newcomer.state["code"]
        late = self.peer("LATE READER")
        late.wait(lambda s: len(s["people"]) == 3)

        # 不正な本文や他人の署名を改変した投稿に、正規掲載の削除・上書きを許さない。
        forged = dict(record, name="FORGED", cleanup_before=int(time.time()))
        corrupted = bytearray(base64.b64decode(self.service.messages[DIRECTORY][-1]["message"]))
        corrupted[-1] ^= 1
        for body in ("garbage", "[]", json.dumps(record), seal_directory(forged), base64.b64encode(corrupted).decode()):
            request = urllib.request.Request(self.server + "/" + DIRECTORY, data=body.encode(), method="POST")
            with urllib.request.urlopen(request, timeout=5) as response:
                self.assertEqual(response.status, 200)
        posts = self.public_posts()
        another = self.peer("ANOTHER", True)
        state = observer.wait(lambda s: len(s["people"]) == 4)
        self.assertEqual({p["name"] for p in state["people"]}, {"FRESH", "RELISTED", "NEW PUBLIC", "ANOTHER"})
        self.assertEqual(self.public_posts(), posts + 1, "不正投稿への削除通知を送信しない")
        self.assertNotEqual(newcomer.state["id"], another.state["id"])

        newcomer.send("visibility", public=False)
        observer.wait(lambda s: len(s["people"]) == 3)
        subprocess.run([str(PROBE), self.server, "seed-cleanup"], check=True, timeout=15)
        observer.wait(lambda s: len(s["people"]) == 5)
        posts = self.public_posts()
        newcomer.send("visibility", public=True)
        state = observer.wait(lambda s: len(s["people"]) == 6)
        self.assertFalse(any(p["name"] in {"EXPIRED", "LEGACY", "FORGED"} for p in state["people"]))
        self.assertEqual(self.public_posts(), posts + 1, "再掲載にも掃除を同梱")
        self.assertEqual(newcomer.state["code"], code)
        self.assertTrue(observer.state["registered"])

    def test_choose_one_request_and_stop_registration_during_game(self):
        a, b, c = self.peer("A", True), self.peer("B"), self.peer("C")
        code = a.state["code"]
        b.send("invite", code=code)
        c.send("invite", code=code)
        incoming = a.wait(lambda s: len(s["incoming"]) == 2)["incoming"]
        request = next(r["id"] for r in incoming if r["name"] == "C")
        a.send("accept", request=request)
        host = a.event()
        b.wait(lambda s: s.get("notice") == "busy" and not s["outgoing"])
        self.assertFalse(b.events)
        a.send("host_ready", match=host["match"], code="XYZ234")
        guest = c.event()
        self.assertEqual(host["match"], guest["match"])
        for peer in (a, c):
            peer.send("playing", match=host["match"])
            peer.wait(lambda s: s["state"] == "playing")
        a.send("stop")
        a.wait(lambda s: not s["registered"])
        b.wait(lambda s: not s["people"])
        self.assertEqual(a.state["state"], "playing")
        self.assertEqual(c.state["state"], "playing")
        self.assertFalse(a.events)
        b.send("invite", code=code)
        b.wait(lambda s: s.get("notice") == "closed")
        for peer, state in ((a, "idle"), (c, "waiting")):
            peer.send("finished", match=host["match"])
            peer.wait(lambda s: s["state"] == state)

    def test_cancel_connection_reserves_slot_until_worker_finishes(self):
        a, b, c = self.peer("A"), self.peer("B"), self.peer("C")
        code = b.state["code"]
        a.send("invite", code=code)
        request = b.wait(lambda s: len(s["incoming"]) == 1)["incoming"][0]["id"]
        b.send("accept", request=request)
        host = b.event()
        b.send("cancel_match")
        self.assertEqual(b.event("abort")["match"], host["match"])
        a.wait(lambda s: s["state"] == "waiting" and not s["outgoing"])
        b.wait(lambda s: s["state"] == "ending")
        c.send("invite", code=code)
        c.wait(lambda s: s.get("notice") == "busy")
        self.assertFalse(a.events)
        b.send("finished", match=host["match"])
        b.wait(lambda s: s["state"] == "waiting")
        self.assertEqual(b.state["code"], code)


if __name__ == "__main__":
    unittest.main()
