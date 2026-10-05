"""公開／非公開と手動承諾を実Clientプロセス＋ローカルntfy互換サービスで検証。"""
import json
import subprocess
import threading
import time
import unittest
import urllib.request
from test_p2p_service import ROOT, Service

PROBE = ROOT / "build/bin/matching_probe.exe"
DIRECTORY = (subprocess.run([str(PROBE), "--directory-topic"], check=True, capture_output=True,
                           text=True, encoding="utf-8", timeout=5).stdout.strip() if PROBE.exists() else "")


class Peer:
    def __init__(self, server, seconds=60):
        self.process = subprocess.Popen([str(PROBE), server, str(seconds)], stdin=subprocess.PIPE,
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

    def peer(self, name, public=False, seconds=60, spectators=True):
        peer = Peer(self.server, seconds)
        self.peers.append(peer)
        peer.send("start", name=name, comment="test", public=public, spectators=spectators)
        peer.wait(lambda s: s.get("registered") and s.get("public") == public)
        return peer

    def public_posts(self):
        return sum(kind == "POST" and topic == DIRECTORY for kind, topic in self.service.requests)

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
        records = [json.loads(e["message"]) for e in self.service.messages[DIRECTORY]]
        published = next(r for r in records if r["name"] == "PUBLIC")
        self.assertGreaterEqual(published["listed_at"], published["created"])
        posts = self.public_posts()
        a.send("stop")
        a.wait(lambda s: not s["registered"])
        remaining = b.wait(lambda s: len(s["people"]) == 2)
        self.assertEqual({p["name"] for p in remaining["people"]}, {"FRESH", "RELISTED"})
        self.assertEqual(self.public_posts(), posts + 1, "取消と掃除は同じ1投稿")
        cancellation = json.loads(self.service.messages[DIRECTORY][-1]["message"])
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
        record = json.loads(self.service.messages[DIRECTORY][-1]["message"])
        self.assertEqual(record["action"], "register")
        self.assertEqual(record["cleanup_before"], record["listed_at"] - 6*60*60)
        code = newcomer.state["code"]
        late = self.peer("LATE READER")
        late.wait(lambda s: len(s["people"]) == 3)

        # 不正な本文や他人の署名を改変した投稿に、正規掲載の削除・上書きを許さない。
        forged = dict(record, name="FORGED", cleanup_before=int(time.time()))
        for body in ("garbage", "[]", json.dumps(forged)):
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
