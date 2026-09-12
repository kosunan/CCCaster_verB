"""旧版プロトコルのローカル実ソケット試験。外部接続は--public指定時のみ。"""
import argparse
import contextlib
import json
import os
import pathlib
import selectors
import shutil
import socket
import struct
import subprocess
import threading
import time


class Relay:
    def __init__(self, fragmented=True, corrupt=False):
        self.tcp = socket.socket()
        self.tcp.bind(('127.0.0.1', 0))
        self.port = self.tcp.getsockname()[1]
        self.tcp.listen()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(('127.0.0.1', self.port))
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.tcp, selectors.EVENT_READ, 'accept')
        self.selector.register(self.udp, selectors.EVENT_READ, 'udp')
        self.hosts, self.matches, self.clients = {}, {}, []
        self.next_id, self.udp_packets = 1, 0
        self.fragmented = fragmented
        self.corrupt = corrupt
        self.pending = []
        self.drop_hosts = threading.Event()
        self.registrations = 0
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def send(self, peer, packet):
        if self.fragmented:
            peer.sendall(packet[:3])
            self.pending.append((time.monotonic() + .03, peer, packet[3:]))
        else:
            peer.sendall(packet)

    def run(self):
        while not self.stop:
            if self.drop_hosts.is_set():
                for peer in set(self.hosts.values()):
                    with contextlib.suppress(KeyError): self.selector.unregister(peer)
                    peer.close()
                self.hosts.clear()
                self.drop_hosts.clear()
            for when, peer, packet in self.pending[:]:
                if time.monotonic() >= when:
                    with contextlib.suppress(OSError): peer.sendall(packet)
                    self.pending.remove((when, peer, packet))
            for key, _ in self.selector.select(.01):
                try:
                    if key.data == 'accept':
                        peer, _ = self.tcp.accept()
                        self.clients.append(peer)
                        self.selector.register(peer, selectors.EVENT_READ, 'register')
                    elif key.data == 'udp':
                        packet, address = self.udp.recvfrom(1024)
                        assert len(packet) == 5, 'Relay received something other than rendezvous data'
                        self.udp_packets += 1
                        side, match_id = struct.unpack('<BI', packet)
                        pair = self.matches.get(match_id)
                        if pair and side in (0, 1) and pair[side]:
                            # MatchInfoの途中へ別メッセージを割り込ませない。
                            if self.pending: continue
                            data = b'TunInfo' + struct.pack('<I', match_id) + f'{address[0]}:{address[1]}'.encode() + b'\0'
                            self.send(pair[side], data)
                            pair[side] = None
                    else:
                        data = key.fileobj.recv(1024)
                        if not data:
                            self.selector.unregister(key.fileobj)
                            continue
                        if self.corrupt:
                            self.send(key.fileobj, b'MatchInfo' + b'\0'*4)
                            continue
                        if len(data) == 3 and data[0] == ord('U'):
                            port = struct.unpack('<H', data[1:])[0]
                            self.hosts[f'U127.0.0.1:{port}'.encode()] = key.fileobj
                            self.registrations += 1
                        elif data in self.hosts:
                            match_id = self.next_id
                            self.next_id += 1
                            pair = [key.fileobj, self.hosts[data]]
                            self.matches[match_id] = pair
                            for peer in pair:
                                self.send(peer, b'MatchInfo' + struct.pack('<I', match_id))
                        else:
                            self.selector.unregister(key.fileobj)
                            key.fileobj.close()
                except OSError:
                    pass

    def close(self):
        self.stop = True
        self.thread.join(2)
        for peer in self.clients: peer.close()
        self.tcp.close()
        self.udp.close()
        self.selector.close()


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def launch(exe, out, name, role, ip, port, seconds, relays, force=True):
    env = os.environ.copy()
    env['CCCASTER_RELAY_SERVERS'] = relays
    if force: env['CCCASTER_TEST_FORCE_RELAY'] = '1'
    else: env.pop('CCCASTER_TEST_FORCE_RELAY', None)
    log = open(out / f'{name}.log', 'wb')
    proc = subprocess.Popen([str(exe), role, ip, str(port), str(seconds)], stdout=log, stderr=subprocess.STDOUT,
                            env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    return proc, log


def finish(proc, log, timeout=20):
    try: return proc.wait(timeout)
    finally:
        if proc.poll() is None: proc.kill(); proc.wait()
        log.close()


def local(exe, out):
    relay = Relay()
    corrupt = Relay(corrupt=True)
    results = {}
    try:
        port = free_port()
        # 1番目が死んでいても、2番目が不正でも、3番目で接続できる。
        relays = f'127.0.0.1:1;127.0.0.1:{corrupt.port};127.0.0.1:{relay.port}'
        host, hlog = launch(exe, out, 'host_delayed', 'host', '', port, 25, relays)
        try:
            time.sleep(.5)
            # UDPを送らない参加者の8秒試行を終了しても、募集は生存する。
            with socket.create_connection(('127.0.0.1', relay.port), 2) as silent:
                silent.sendall(f'U127.0.0.1:{port}'.encode())
                time.sleep(9)
            assert host.poll() is None, 'Host incorrectly expired during bulletin-board waiting'
            client, clog = launch(exe, out, 'client_punch', 'join', '127.0.0.1', port, 16, relays)
            assert finish(client, clog) == 0
            assert finish(host, hlog) == 0
        finally:
            if host.poll() is None: host.kill(); host.wait(); hlog.close()
        for name in ('host_delayed', 'client_punch'):
            assert '[CONNECT_ROUTE] hole_punch' in (out/f'{name}.log').read_text(errors='replace')
        results['delayed_join_fragmented_messages_relay_fallback'] = True
        assert '[CONNECT_STAGE] attempt_expired' in (out/'host_delayed.log').read_text(errors='replace')
        results['failed_participant_does_not_end_hosting'] = True
        results['malformed_match_rejected'] = True
        # 接続支援が止まっていても、直接接続は通る。
        port = free_port()
        host, hlog = launch(exe, out, 'host_direct', 'host', '', port, 10, '127.0.0.1:1', False)
        time.sleep(.3)
        client, clog = launch(exe, out, 'client_direct', 'join', '127.0.0.1', port, 8, '127.0.0.1:1', False)
        assert finish(client, clog) == 0
        assert finish(host, hlog) == 0
        results['direct_without_relay'] = True
        # 参加者は無応答でも12秒で終了する。募集待ちは終了しない。
        client, clog = launch(exe, out, 'client_timeout', 'join', '127.0.0.1', free_port(), 16, relays)
        assert finish(client, clog) == 1
        timeout_log = (out/'client_timeout.log').read_text(errors='replace')
        assert '[CONNECT_STAGE] timeout' in timeout_log
        results['client_deadline'] = True
        host, hlog = launch(exe, out, 'host_cancel', 'host', '', free_port(), 2, relays)
        assert finish(host, hlog) == 1
        results['cancel'] = True
        # サーバーとの切断後も同じ募集ポートで再登録し、後から参加できる。
        port = free_port()
        reconnect_relay = Relay()
        host, hlog = launch(exe, out, 'host_reconnect', 'host', '', port, 42, f'127.0.0.1:{reconnect_relay.port}')
        try:
            deadline = time.monotonic()+3
            while reconnect_relay.registrations < 1 and time.monotonic()<deadline: time.sleep(.05)
            assert reconnect_relay.registrations == 1
            reconnect_relay.drop_hosts.set()
            deadline = time.monotonic()+33
            while reconnect_relay.registrations < 2 and time.monotonic()<deadline: time.sleep(.1)
            assert reconnect_relay.registrations == 2 and host.poll() is None
            client, clog = launch(exe, out, 'client_reconnect', 'join', '127.0.0.1', port, 8, f'127.0.0.1:{reconnect_relay.port}')
            assert finish(client, clog) == 0
            assert finish(host, hlog) == 0
            results['host_reregisters_after_relay_disconnect'] = True
        finally:
            if host.poll() is None: host.kill(); host.wait(); hlog.close()
            reconnect_relay.close()
        results['rendezvous_udp_packets'] = relay.udp_packets
    finally: relay.close(); corrupt.close()
    return results


def public(server):
    """自分の2ソケットだけを登録し、既存公開サーバーのMatchInfo/TunInfoを検証。"""
    host_name, server_port = server.rsplit(':', 1)
    address = (socket.gethostbyname(host_name), int(server_port))
    with contextlib.ExitStack() as stack:
        uh = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
        uc = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
        uh.bind(('', 0)); uc.bind(('', 0)); uh.settimeout(3)
        transaction = os.urandom(12)
        uh.sendto(struct.pack('!HHI', 1, 0, 0x2112a442)+transaction, ('stun.l.google.com', 19302))
        packet, _ = uh.recvfrom(1024)
        assert packet[8:20] == transaction
        ip = None
        at = 20
        while at+4 <= len(packet):
            kind, length = struct.unpack('!HH', packet[at:at+4])
            if kind == 0x20 and length == 8:
                ip = socket.inet_ntoa(bytes(a ^ b for a,b in zip(packet[at+8:at+12], b'\x21\x12\xa4\x42')))
            at += 4 + (length+3)//4*4
        assert ip
        th = stack.enter_context(socket.create_connection(address, 3)); th.settimeout(3)
        th.sendall(b'U'+struct.pack('<H', uh.getsockname()[1]))
        time.sleep(.3)
        tc = stack.enter_context(socket.create_connection(address, 3)); tc.settimeout(3)
        tc.sendall(f'U{ip}:{uh.getsockname()[1]}'.encode())
        def read_exact(s, n):
            result = b''
            while len(result)<n:
                data=s.recv(n-len(result))
                if not data: raise RuntimeError('server closed before reply')
                result+=data
            return result
        mh, mc = read_exact(th, 13), read_exact(tc, 13)
        assert mh[:9] == b'MatchInfo' and mh == mc
        match_id = struct.unpack('<I', mh[9:])[0]
        uh.sendto(struct.pack('<BI', 0, match_id), address)
        uc.sendto(struct.pack('<BI', 1, match_id), address)
        def tun(s):
            header = read_exact(s, 11)
            assert header[:7] == b'TunInfo' and struct.unpack('<I', header[7:])[0] == match_id
            endpoint = b''
            for _ in range(22):
                c = read_exact(s, 1)
                if c == b'\0': return bool(endpoint)
                endpoint += c
            raise RuntimeError('invalid endpoint')
        return dict(server=server, match_info=True, host_tun_info=tun(th), client_tun_info=tun(tc), wan_peer_connection_tested=False)


if __name__ == '__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--exe', type=pathlib.Path, default=pathlib.Path('build/bin/relay_negotiation_probe.exe'))
    parser.add_argument('--out', type=pathlib.Path, default=pathlib.Path('build_logs/legacy_relay_20260913'))
    parser.add_argument('--public', dest='server')
    parser.add_argument('--real', action='store_true')
    args=parser.parse_args(); args.out.mkdir(parents=True, exist_ok=True)
    if args.real:
        relay=Relay()
        try:
            env=os.environ.copy();env['CCCASTER_RELAY_SERVERS']=f'127.0.0.1:{relay.port}';env['CCCASTER_TEST_FORCE_RELAY']='1'
            result=subprocess.run([shutil.which('pwsh') or 'powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',
                'src/harness/run_bounded_real_pair.ps1','-Seconds','40','-Port',str(free_port()),'-Network','15,25,5',
                '-OutputDirectory',str((args.out/'real_pair').resolve())],env=env,capture_output=True,
                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
            (args.out/'real_pair_runner.log').write_bytes(result.stdout+result.stderr)
            if result.returncode: raise RuntimeError('real pair failed; see real_pair_runner.log')
            for side in (1,2):
                assert '[CONNECT_ROUTE] hole_punch' in (args.out/'real_pair'/f'launcher_{side}.log').read_text(errors='replace')
            result={'real_pair_hole_punch_route':True,'rendezvous_udp_packets':relay.udp_packets}
        finally: relay.close()
    else:
        result=public(args.server) if args.server else local(args.exe.resolve(),args.out)
    print(json.dumps(result,ensure_ascii=False,indent=2))
    (args.out/('public_'+args.server.split(':')[0]+'.json' if args.server else 'real_pair_route.json' if args.real else 'integration.json')).write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
