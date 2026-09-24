"""End-to-end check against a real server: two WebSocket clients, the
supervised bot, a crash and restart, and a clean SIGTERM shutdown.

Run from this directory after `facet install`: python3 smoke_test.py
Set DIAMOND_BIN to use a diamond other than ../../build/diamond."""
import base64
import json
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import time

here = Path(__file__).resolve().parent
diamond = os.environ.get('DIAMOND_BIN', str(here.parents[1] / 'build/diamond'))


class Client:
    """Just enough RFC 6455 for text frames: masked sends, unmasked receives."""

    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f'GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\n'
                           f'Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n'
                           'Sec-WebSocket-Version: 13\r\n\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            head += self.sock.recv(1)
        assert head.startswith(b'HTTP/1.1 101'), head

    def send(self, event):
        payload = json.dumps(event).encode()
        mask = os.urandom(4)
        header = bytes([0x81])
        if len(payload) < 126:
            header += bytes([0x80 | len(payload)])
        else:
            header += bytes([0x80 | 126]) + struct.pack('>H', len(payload))
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def read_exact(self, n):
        data = b''
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError('closed')
            data += chunk
        return data

    def receive(self):
        first, second = self.read_exact(2)
        length = second & 0x7F
        if length == 126:
            length = struct.unpack('>H', self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack('>Q', self.read_exact(8))[0]
        payload = self.read_exact(length)
        assert first & 0x0F == 1, f'unexpected opcode {first & 0x0F}'
        return json.loads(payload)

    def expect(self, predicate, what):
        deadline = time.monotonic() + 10
        seen = []
        while time.monotonic() < deadline:
            event = self.receive()
            seen.append(event)
            if predicate(event):
                return event
        raise AssertionError(f'never saw {what}; got {seen}')

    def close(self):
        self.sock.close()


with socket.socket() as probe:
    probe.bind(('127.0.0.1', 0))
    port = probe.getsockname()[1]
env = dict(os.environ, CHAT_PORT=str(port), DIAMOND_NO_CACHE='1')
server = subprocess.Popen([diamond, 'app.di'], cwd=here, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
try:
    for _ in range(200):
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1):
                break
        except OSError:
            time.sleep(0.05)
    else:
        raise AssertionError('server did not start')

    ada = Client(port)
    ada.send({'type': 'say', 'text': 'too early'})
    assert 'Join first' in ada.receive()['text']
    ada.send({'type': 'join', 'name': '  Ada  '})
    welcome = ada.receive()
    assert welcome['type'] == 'welcome' and welcome['name'] == 'Ada', welcome
    ada.expect(lambda e: e['type'] == 'joined' and e['members'] == ['Ada'], 'Ada joined')

    dup = Client(port)
    dup.send({'type': 'join', 'name': 'Ada'})
    assert 'already here' in dup.receive()['text']
    dup.close()

    bob = Client(port)
    bob.send({'type': 'join', 'name': 'Bob'})
    assert bob.receive()['type'] == 'welcome'
    ada.expect(lambda e: e['type'] == 'joined' and e['name'] == 'Bob', 'Bob joined')

    ada.send({'type': 'say', 'text': 'hello from Ada'})
    bob.expect(lambda e: e.get('text') == 'hello from Ada' and e['name'] == 'Ada', 'Ada message')

    bob.send({'type': 'say', 'text': '/roll 20'})
    roll = ada.expect(lambda e: e['type'] == 'bot' and 'rolled' in e['text'], 'bot roll')
    value = int(roll['text'].split('rolled ')[1].split(' ')[0])
    assert 1 <= value <= 20 and roll['text'].startswith('Bob rolled'), roll

    ada.send({'type': 'say', 'text': '/crash'})
    restart = bob.expect(lambda e: e['type'] == 'bot' and 'restarted' in e['text'], 'restart notice')
    assert 'RuntimeError' in restart['text'], restart
    ada.send({'type': 'say', 'text': '/time'})
    ada.expect(lambda e: e['type'] == 'bot' and e['text'].startswith('Server time'), 'bot after restart')
    ada.send({'type': 'say', 'text': '/stats'})
    stats = ada.expect(lambda e: e['type'] == 'bot' and 'online' in e['text'], 'stats')
    assert stats['text'] == '2 online; bot restarts: 1', stats

    carol = Client(port)
    carol.send({'type': 'join', 'name': 'Carol'})
    history = carol.receive()['history']
    assert any(e.get('text') == 'hello from Ada' for e in history), history

    bob.close()
    ada.expect(lambda e: e['type'] == 'left' and e['name'] == 'Bob', 'Bob left')
    ada.close()
    carol.close()

    server.send_signal(signal.SIGTERM)
    server.wait(timeout=15)
finally:
    if server.poll() is None:
        server.kill()
        output = server.communicate()[0]
        raise AssertionError(f'server did not exit on SIGTERM:\n{output}')
print('chat smoke test passed')
