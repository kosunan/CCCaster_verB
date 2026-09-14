"""IPv4/IPv6の実ソケット・選択合意・旧接続支援のローカル回帰。外部接続なし。"""
import argparse
import heapq
import json
import os
import pathlib
import re
import selectors
import socket
import subprocess
import threading
import time
from test_legacy_relay import Relay, free_port


class Proxy:
    def __init__(self, host_port, delay4=0, delay6=0, drop_accept=False, drop6=False):
        self.sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM), socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)]
        self.sockets[1].setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        self.sockets[0].bind(('127.0.0.1', 0))
        self.port = self.sockets[0].getsockname()[1]
        self.sockets[1].bind(('::1', self.port))
        self.hosts = [('127.0.0.1', host_port), ('::1', host_port)]
        self.clients = [None, None]
        self.delay = [delay4, delay6]
        self.drop_accept, self.drop6 = drop_accept, drop6
        self.queue = []
        self.serial = 0
        self.stop = False
        self.selector = selectors.DefaultSelector()
        for f, sock in enumerate(self.sockets): self.selector.register(sock, selectors.EVENT_READ, f)
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop:
            now = time.monotonic()
            while self.queue and self.queue[0][0] <= now:
                _, _, sock, data, address = heapq.heappop(self.queue)
                sock.sendto(data, address)
            for key, _ in self.selector.select(.001):
                sock, family = key.fileobj, key.data
                data, source = sock.recvfrom(2048)
                if family and self.drop6: continue
                if data[:8] == b'CCBRTE\0\1' and self.drop_accept and data[8] == 4: continue
                if source[:2] == self.hosts[family]: target = self.clients[family]
                else:
                    self.clients[family] = source
                    target = self.hosts[family]
                if target:
                    self.serial += 1
                    heapq.heappush(self.queue, (time.monotonic()+self.delay[family], self.serial, sock, data, target))

    def close(self):
        self.stop = True
        self.thread.join(2)
        for sock in self.sockets: sock.close()
        self.selector.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--exe', type=pathlib.Path, required=True)
    parser.add_argument('--out', type=pathlib.Path, required=True)
    parser.add_argument('--edges-only', action='store_true')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    results = {}

    def pair(name, preference, expected, v4='127.0.0.1', v6='::1', proxy_settings=None, punch=False, advertise=True):
        host_port = free_port()
        proxy = Proxy(host_port, **proxy_settings) if proxy_settings is not None else None
        relay = Relay() if punch else None
        env = os.environ.copy()
        env['CCCASTER_RELAY_SERVERS'] = f'127.0.0.1:{relay.port}' if relay else ''
        if punch: env['CCCASTER_TEST_FORCE_RELAY'] = '1'
        else: env.pop('CCCASTER_TEST_FORCE_RELAY', None)
        processes, files = [], []
        try:
            for role in ('host', 'join'):
                path = args.out/f'{name}_{role}.log'
                log = open(path, 'wb'); files.append(log)
                port = host_port if role == 'host' or not proxy else proxy.port
                local6 = '::1' if advertise and v6 != '-' else '-'
                command = [str(args.exe.resolve()), role, str(port), v4, v6, str(preference), local6, '15000', '12345']
                processes.append(subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)))
                if role == 'host': time.sleep(.25)
            for proc in processes: assert proc.wait(20) == 0, name
            for log in files: log.flush()
            texts = [(args.out/f'{name}_{role}.log').read_text(errors='replace') for role in ('host','join')]
            for text in texts:
                assert f'RESULT success=1 ipv6={expected}' in text, (name, text)
                if punch: assert '[CONNECT_ROUTE] hole_punch' in text, name
            if relay: assert relay.udp_packets > 0
            peer_ports = [int(re.search(r'RESULT.*peerPort=(\d+)', text)[1]) for text in texts]
            local_ports = [int(re.search(r'RESULT.*local=(\d+)', text)[1]) for text in texts]
            if not proxy: assert peer_ports == list(reversed(local_ports)), name
            results[name] = {'ok': True, 'ipv6': expected, 'relay_udp': relay.udp_packets if relay else 0}
            print(name, 'OK', flush=True)
        finally:
            for proc in processes:
                if proc.poll() is None: proc.kill(); proc.wait()
            for log in files: log.close()
            if proxy: proxy.close()
            if relay: relay.close()

    if not args.edges_only:
        pair('prefer_v4', 1, 0)
        pair('prefer_v6', 2, 1)
        pair('v4_only_fallback', 2, 0, v6='-')
        pair('v6_only_fallback', 1, 1, v4='-')
        pair('auto_v4_faster', 0, 0, proxy_settings={'delay6': .035}, advertise=False)
        pair('auto_v6_faster', 0, 1, proxy_settings={'delay4': .035}, advertise=False)
        pair('v6_unresponsive_fallback', 2, 0, proxy_settings={'drop6': True}, advertise=False)
        pair('selection_ack_loss', 2, 1, proxy_settings={'drop_accept': True}, advertise=False)
        pair('v4_hole_punch', 1, 0, punch=True)
        pair('v6_hole_punch', 2, 1, punch=True)

    processes, files = [], []
    env = os.environ.copy(); env['CCCASTER_RELAY_SERVERS'] = ''
    env.pop('CCCASTER_TEST_FORCE_RELAY', None)

    def launch(name, role, port, timeout=6000, token=12345, old=False):
        log = open(args.out/f'{name}.log', 'wb'); files.append(log)
        command = [str(args.exe.resolve()), role, str(port), '127.0.0.1', '::1', '0', '::1', str(timeout), str(token)]
        if old: command = [str(args.exe.resolve().with_name('relay_negotiation_probe.exe')), role, '127.0.0.1', str(port), '10']
        proc = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, env=env,
                                creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0))
        processes.append(proc); return proc

    def read(name):
        for file in files: file.flush()
        return (args.out/f'{name}.log').read_text(errors='replace')

    try:
        for old_role in ('host', 'join'):
            port = free_port()
            host = launch(f'legacy_{old_role}_host','host',port,old=old_role=='host')
            time.sleep(.25)
            client = launch(f'legacy_{old_role}_join','join',port,old=old_role=='join')
            assert client.wait(12)==0 and host.wait(12)==0, old_role
            results[f'legacy_{old_role}']={'ok':True}
            print('legacy', old_role, 'OK', flush=True)
        port = free_port()
        client = launch('cancel','join',port,timeout=250)
        assert client.wait(2)==1 and 'cancelled' in read('cancel')
        results['cancel']={'ok':True}
        client = launch('both_unreachable','join',port,timeout=18000)
        assert client.wait(19)==1
        text = read('both_unreachable')
        assert '[CONNECT_STAGE] timeout' in text and 'punch_unavailable' in text and 'punch_failed' in text
        results['both_unreachable']={'ok':True}
        print('cancel / both unreachable OK', flush=True)
        port = free_port()
        host = launch('wrong_token_host','host',port,timeout=10000)
        time.sleep(.25)
        client = launch('wrong_token_join','join',port,timeout=2500,token=98765)
        assert client.wait(4)==1 and host.poll() is None
        client = launch('retry_join','join',port)
        assert client.wait(10)==0 and host.wait(10)==0
        results['wrong_token_then_retry']={'ok':True}
        print('wrong token / retry OK', flush=True)
    finally:
        for proc in processes:
            if proc.poll() is None: proc.kill(); proc.wait()
        for log in files: log.close()
    (args.out/'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')


if __name__ == '__main__': main()
