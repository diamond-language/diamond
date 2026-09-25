"""Live facet -> HTTPS proxy -> Diamond registry integration; no external network."""
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import ssl
import subprocess
import sys
import tempfile
import threading
import time

source = Path(__file__).resolve().parents[2]
diamond = Path(os.environ.get('DIAMOND_BIN', source / 'build/diamond')).resolve()
facet = source / 'build/facet'


def run(*args, **kwargs):
    result = subprocess.run(args, capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise AssertionError(f"{Path(args[0]).name} failed ({result.returncode}): {result.stderr}")
    return result


with tempfile.TemporaryDirectory(prefix='diamond-registry-http-') as temporary:
    work = Path(temporary)
    run(str(source / 'tools/install_local_cuts.sh'), str(work))
    data = work / 'data'
    (data / 'blobs').mkdir(parents=True)
    (data / 'staging').mkdir()
    for name in ('app.di', 'catalog.di', 'catalog.html', 'catalog.js', 'catalog.css', 'cut.html', 'cut.js'):
        shutil.copy(source / 'applications/registry' / name, work / name)
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, REGISTRY_ROOT=str(data), REGISTRY_FACET=str(facet),
               REGISTRY_PORT=str(port), REGISTRY_BASE='/registry', DIAMOND_NO_CACHE='1',
               REGISTRY_TIMEOUT_SECONDS='2', REGISTRY_MAX_CONNECTIONS='4',
               REGISTRY_MAX_BODY_BYTES='1048576')
    log = open(work / 'server.log', 'w+')
    server = subprocess.Popen([str(diamond), 'app.di'], cwd=work, env=env, stdout=log, stderr=log)
    proxy = None
    try:
        for _ in range(100):
            if server.poll() is not None:
                log.seek(0)
                raise AssertionError(log.read())
            try:
                conn = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
                conn.request('GET', '/registry/health')
                response = conn.getresponse()
                ready = response.status == 200
                response.read()
                conn.close()
                if ready:
                    break
            except OSError:
                pass
            time.sleep(.05)
        else:
            raise AssertionError('registry failed to start')

        shutil.copy(source / 'applications/registry/credentials.di', work / 'credentials.di')
        env['REGISTRY_OPERATOR'] = 'test-operator'

        def credential(*args):
            return json.loads(run(str(diamond), 'credentials.di', *args, cwd=work, env=env).stdout)

        issued = credential('issue', 'test-owner', '3600',
                            'publish:greeter,publish:helper,manage:greeter', 'test setup')
        token = issued['token']
        def raw_request(data, half_close=False):
            with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
                client.sendall(data)
                if half_close:
                    client.shutdown(socket.SHUT_WR)
                chunks = []
                while True:
                    try:
                        chunk = client.recv(65536)
                    except ConnectionResetError:
                        break
                    if not chunk:
                        break
                    chunks.append(chunk)
                return b''.join(chunks)

        for extra, expected in [
            (b'Content-Length: 1048577', 413),
            (b'Content-Length: -1', 400),
            (b'Content-Length: 2junk', 400),
            (b'Content-Length: 0\r\nContent-Length: 0', 400),
            (b'Transfer-Encoding: chunked', 400),
            (b'X-Large: ' + b'a' * 8200, 431),
            (b'\r\n'.join(f'X-{i}: a'.encode() for i in range(101)), 431),
            (b'\r\n'.join(f'X-{i}: '.encode() + b'a' * 7000 for i in range(5)), 431),
        ]:
            response = raw_request(b'POST /registry/unknown HTTP/1.1\r\nHost: localhost\r\n' + extra + b'\r\n\r\n')
            assert response.startswith(f'HTTP/1.1 {expected} '.encode()), response[:150]
            head, body = response.split(b'\r\n\r\n', 1)
            payload = json.loads(body)
            assert payload['protocol'] == 1 and payload['request_id'].encode() in head
        response = raw_request(b'POST /registry/unknown HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nx', half_close=True)
        assert response.startswith(b'HTTP/1.1 400 ')
        response = raw_request(b'GET /registry/\x00 HTTP/1.1\r\nHost: localhost\r\n\r\n')
        assert response.startswith(b'HTTP/1.1 400 ')
        # Supported Expect handshake occurs only after length validation.
        with socket.create_connection(('127.0.0.1', port), timeout=5) as client:
            client.sendall(b'POST /registry/unknown HTTP/1.1\r\nHost: localhost\r\nExpect: 100-continue\r\nContent-Length: 1\r\n\r\n')
            assert client.recv(4096) == b'HTTP/1.1 100 Continue\r\n\r\n'
            client.sendall(b'x')
            assert client.recv(4096).startswith(b'HTTP/1.1 404 ')
        for partial in (b'', b'GET /registry/health HTTP/1.1\r\nHost:',
                        b'POST /registry/unknown HTTP/1.1\r\nHost: localhost\r\nContent-Length: 5\r\n\r\nx'):
            start = time.monotonic()
            assert raw_request(partial) == b''
            assert time.monotonic() - start < 5
        idle = [socket.create_connection(('127.0.0.1', port), timeout=5) for _ in range(4)]
        try:
            time.sleep(.15)
            started = time.monotonic()
            assert raw_request(b'') == b''
            assert time.monotonic() - started < 1.5
        finally:
            for client in idle:
                client.close()
        for _ in range(100):
            response = raw_request(b'GET /registry/health HTTP/1.1\r\nHost: localhost\r\n\r\n')
            if response.startswith(b'HTTP/1.1 200 '):
                break
            time.sleep(.02)
        else:
            raise AssertionError('capacity did not recover after clients disconnected')
        # A request containing secret-looking data must not copy it into logs.
        marker = b'registry-test-sensitive-marker'
        response = raw_request(b'POST /registry/unknown?secret=' + marker + b' HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer ' + marker + b'\r\nContent-Length: ' + str(len(marker)).encode() + b'\r\n\r\n' + marker)
        assert response.startswith(b'HTTP/1.1 404 ')

        db = sqlite3.connect(data / 'registry.db')
        run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
            '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
            '-keyout', str(work / 'key.pem'), '-out', str(work / 'cert.pem'))

        class Proxy(http.server.BaseHTTPRequestHandler):
            def forward(self):
                body = self.rfile.read(int(self.headers.get('Content-Length', '0')))
                backend = http.client.HTTPConnection('127.0.0.1', port, timeout=30)
                try:
                    backend.request(self.command, self.path, body, dict(self.headers))
                    reply = backend.getresponse()
                    payload = reply.read()
                    self.send_response(reply.status)
                    for name, value in reply.getheaders():
                        if name.lower() not in ('connection', 'transfer-encoding', 'content-length'):
                            self.send_header(name, value)
                    self.send_header('Content-Length', str(len(payload)))
                    self.end_headers()
                    self.wfile.write(payload)
                finally:
                    backend.close()

            do_GET = forward
            do_POST = forward

            def log_message(self, *_):
                pass

        proxy = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Proxy)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(work / 'cert.pem', work / 'key.pem')
        proxy.socket = tls.wrap_socket(proxy.socket, server_side=True)
        threading.Thread(target=proxy.serve_forever, daemon=True).start()
        public_port = proxy.server_port
        url = f'https://localhost:{public_port}/registry'
        env['CURL_CA_BUNDLE'] = str(work / 'cert.pem')
        env['NO_PROXY'] = 'localhost,127.0.0.1'
        trust = ssl.create_default_context(cafile=str(work / 'cert.pem'))

        request_ids = []

        def request(path, method='GET', body=None, headers=None):
            connection = http.client.HTTPSConnection('localhost', public_port, context=trust, timeout=30)
            connection.request(method, '/registry' + path, body, headers or {})
            response = connection.getresponse()
            payload = response.read()
            status = response.status
            content_type = response.getheader('Content-Type')
            request_id = response.getheader('X-Request-ID')
            assert request_id
            request_ids.append(request_id)
            connection.close()
            if content_type == 'application/json':
                payload = json.loads(payload)
                if status >= 400:
                    assert payload['request_id'] == request_id
            return status, payload

        def package(name, version, body, dependencies=None):
            path = work / name
            (path / 'lib').mkdir(parents=True, exist_ok=True)
            (path / 'diamond.cut').write_text(json.dumps(dict(name=name, version=version, summary='test cut',
                license='MIT', maintainers=[dict(name='Test', contact='test@example.com')], dependencies=dependencies or {})))
            (path / 'README.md').write_text('test\n')
            (path / 'LICENSE').write_text('MIT\n')
            (path / 'lib' / f'{name}.di').write_text(body)
            return path

        helper = package('helper', '1.0.0', 'def helper_value() = "installed from registry"\n')
        greeter = package('greeter', '1.0.0', 'require_cut "helper"\ndef greet() = helper_value()\n', {'helper': '^1.0.0'})
        for path in (helper, greeter):
            run(str(facet), 'publish', str(path), '--registry', url, '--token', token, env=env)
        # Publish deliberately out of order, including numeric prerelease IDs.
        for version in ('1.10.0', '2.0.0-beta.10', '2.0.0-beta.2', '2.0.0', '1.2.0'):
            package('helper', version, 'def helper_value() = "installed from registry"\n')
            run(str(facet), 'publish', str(helper), '--registry', url, '--token', token, env=env)
        status, helper_index = request('/v1/cuts/helper/versions')
        assert status == 200
        assert [v['version'] for v in helper_index['versions']] == [
            '1.0.0', '1.2.0', '1.10.0', '2.0.0-beta.2', '2.0.0-beta.10', '2.0.0']
        run(str(facet), 'pack', str(greeter), str(work / 'greeter.tar'))
        archive = (work / 'greeter.tar').read_bytes()
        headers = {'Authorization': f'Bearer {token}', 'Content-Type': 'application/octet-stream'}
        status, repeated = request('/v1/cuts/greeter/versions', 'POST', archive, headers)
        assert status == 200 and repeated['version'] == '1.0.0'
        assert 'created' not in repeated
        assert request('/v1/cuts/greeter/versions', 'POST', archive)[0] == 401
        assert request('/v1/cuts/greeter/versions', 'POST', b'bad', headers)[0] == 422
        assert request('/v1/cuts/unknown/versions', 'POST', archive, headers)[0] == 403
        assert request('/v1/cuts/greeter/versions', 'POST', archive,
                       {'Authorization': f'Bearer {token}', 'Content-Type': 'text/plain'})[0] == 400
        assert request('/v1/blobs/sha256/not-a-digest')[0] == 400
        (greeter / 'README.md').write_text('different bytes for same version\n')
        run(str(facet), 'pack', str(greeter), str(work / 'changed.tar'))
        status, conflict = request('/v1/cuts/greeter/versions', 'POST', (work / 'changed.tar').read_bytes(), headers)
        assert status == 409 and conflict['error'] == 'release_exists' and conflict['request_id']
        # A release packed before manifests declared maintainers: blank the field
        # in place so the ustar header and size stay canonical.
        legacy_root = work / 'legacy' / 'greeter'
        shutil.copytree(greeter, legacy_root)
        manifest = json.loads((legacy_root / 'diamond.cut').read_text())
        manifest['version'] = '0.9.0'
        (legacy_root / 'diamond.cut').write_text(json.dumps(manifest))
        run(str(facet), 'pack', str(legacy_root), str(work / 'legacy.tar'))
        legacy = (work / 'legacy.tar').read_bytes()
        field = b', "maintainers": ' + json.dumps(manifest['maintainers']).encode()
        assert legacy.count(field) == 1
        legacy = legacy.replace(field, b' ' * len(field))
        status, rejected = request('/v1/cuts/greeter/versions', 'POST', legacy, headers)
        assert status == 422 and rejected['error'] == 'invalid_archive'
        # Releases that predate the requirement stay idempotent on retry.
        cut_id = db.execute("SELECT id FROM cuts WHERE name = 'greeter'").fetchone()[0]
        db.execute("INSERT INTO releases (cut_id, version, dependencies, sha256, size, created_at) VALUES (?, '0.9.0', ?, ?, ?, 0)",
                   (cut_id, json.dumps(manifest['dependencies']), hashlib.sha256(legacy).hexdigest(), len(legacy)))
        db.commit()
        status, retried = request('/v1/cuts/greeter/versions', 'POST', legacy, headers)
        assert status == 200 and retried['version'] == '0.9.0'
        stored = dict(db.execute("SELECT version, maintainers FROM releases WHERE cut_id = ?", (cut_id,)).fetchall())
        assert stored['0.9.0'] is None
        assert json.loads(stored['1.0.0']) == [{'name': 'Test', 'contact': 'test@example.com'}]
        status, catalog = request('/catalog.json')
        listed = {(row['name'], row['version']): row['maintainers'] for row in catalog['releases']}
        # The catalog lists only each cut's newest release.
        assert status == 200 and ('greeter', '0.9.0') not in listed
        assert json.loads(listed[('greeter', '1.0.0')])[0]['contact'] == 'test@example.com'
        db.execute("DELETE FROM releases WHERE cut_id = ? AND version = '0.9.0'", (cut_id,))
        db.commit()
        status, index = request('/v1/cuts/greeter/versions')
        assert status == 200 and index['versions'] == [{'version': '1.0.0', 'yanked': False}]
        status, release = request('/v1/cuts/greeter/versions/1.0.0')
        assert status == 200 and release['dependencies'] == {'helper': '^1.0.0'}
        status, blob = request(release['archive']['path'])
        assert status == 200 and blob == archive
        assert hashlib.sha256(blob).hexdigest() == release['archive']['sha256']
        stored = data / 'blobs' / release['archive']['sha256']
        stored.write_bytes(b'corrupt')
        status, failure = request(release['archive']['path'])
        assert status == 500 and failure['error'] == 'internal_error'
        stored.write_bytes(archive)
        orphan = b'not a committed release'
        digest = hashlib.sha256(orphan).hexdigest()
        (data / 'blobs' / digest).write_bytes(orphan)
        assert request('/v1/blobs/sha256/' + digest)[0] == 404

        monitor = str(source / 'applications/registry/monitor.py')
        monitor_args = [sys.executable, monitor, url, '--ca-file', str(work / 'cert.pem')]
        result = run(*monitor_args, '--archive-sha256', release['archive']['sha256'])
        assert json.loads(result.stdout)['ok'] is True
        for arguments in (
                [sys.executable, monitor, url],  # untrusted TLS certificate
                [sys.executable, monitor, url + '/missing', '--ca-file', str(work / 'cert.pem')],
                monitor_args + ['--archive-sha256', '0' * 64],
                [sys.executable, monitor, 'http://localhost:' + str(public_port)],
        ):
            result = subprocess.run(arguments, capture_output=True, text=True, timeout=15)
            assert result.returncode == 1 and json.loads(result.stdout)['ok'] is False
            assert url not in result.stdout

        assert request('/')[0] == 200
        assert request('/catalog.js')[0] == 200
        status, catalog = request('/catalog.json')
        assert status == 200 and any(row['name'] == 'greeter' for row in catalog['releases'])
        for cursor in ('bad', '', '-1', '01', '1&after=2', '999999999999999999999999'):
            assert request('/catalog.json?after=' + cursor)[0] == 400, cursor
        # Show pages: every served version newest first, plus the latest
        # release's summary and README read from its verified archive.
        assert request('/catalog.css')[0] == 200 and request('/cut.js')[0] == 200
        status, page = request('/cuts/helper')
        assert status == 200 and b'cut.js' in page
        assert request('/cuts/Not-A-Name')[0] == 404
        status, shown = request('/catalog/helper.json')
        assert status == 200 and shown['name'] == 'helper' and shown['latest'] == '2.0.0'
        assert [v['version'] for v in shown['versions']] == [
            '2.0.0', '2.0.0-beta.10', '2.0.0-beta.2', '1.10.0', '1.2.0', '1.0.0']
        assert shown['summary'] == 'test cut' and shown['license'] == 'MIT' and shown['readme'] == 'test\n'
        assert shown['versions'][0]['maintainers'] == [{'name': 'Test', 'contact': 'test@example.com'}]
        assert request('/catalog/missing.json')[0] == 404
        assert request('/catalog/..%2Fx.json')[0] == 404
        consumer = work / 'consumer'
        consumer.mkdir()
        run(str(facet), 'init', 'consumer', cwd=consumer, env=env)
        run(str(facet), 'add', 'greeter', '--registry', url, '--version', '^1.0.0', cwd=consumer, env=env)
        run(str(facet), 'update', cwd=consumer, env=env)
        assert run(str(diamond), '-e', 'require_cut "greeter"\ngreet()', cwd=consumer, env=env).stdout.strip() == 'installed from registry'
        assert (consumer / 'cuts/helper/lib/helper.di').exists()
        def manage(action, credential_token, reason='test state change'):
            return request('/v1/cuts/greeter/versions/1.0.0/' + action, 'POST',
                           json.dumps({'reason': reason}).encode(),
                           {'Authorization': 'Bearer ' + credential_token, 'Content-Type': 'application/json'})

        outsider = credential('issue', 'other-owner', '3600', 'manage:greeter', 'wrong owner test')
        publisher_only = credential('issue', 'test-owner', '3600', 'publish:greeter', 'scope test')
        assert manage('yank', outsider['token'])[0] == 403
        assert manage('yank', publisher_only['token'])[0] == 403
        def owners(action=None, actor=token, owner='other-owner', reason='ownership test'):
            path = '/v1/cuts/greeter/owners'
            headers = {'Authorization': 'Bearer ' + actor, 'Content-Type': 'application/json'}
            if action is None:
                return request(path, headers=headers)
            return request(path + '/' + action, 'POST', json.dumps({'owner': owner, 'reason': reason}).encode(), headers)

        assert owners(actor=outsider['token'])[0] == 403
        assert owners(actor=publisher_only['token'])[0] == 403
        assert request('/v1/cuts/greeter/owners')[0] == 401
        status, original = owners()
        assert status == 200 and [row['owner'] for row in original['owners']] == ['test-owner']
        status, failure = owners('remove', owner='test-owner')
        assert status == 409 and failure['error'] == 'last_owner'
        assert owners('add', owner='   ')[0] == 400
        db.execute("CREATE TRIGGER fail_owner_audit BEFORE INSERT ON audit_events WHEN NEW.action = 'owner_add' BEGIN SELECT RAISE(ABORT, 'owner audit failure'); END")
        db.commit()
        assert owners('add')[0] == 500
        assert owners()[1] == original
        db.execute('DROP TRIGGER fail_owner_audit')
        db.commit()
        status, added = owners('add')
        assert status == 200 and [row['owner'] for row in added['owners']] == ['other-owner', 'test-owner']
        assert owners('add')[0] == 200
        assert db.execute("SELECT count(*) FROM audit_events WHERE action = 'owner_add'").fetchone()[0] == 1
        assert owners(actor=outsider['token'])[0] == 200
        # Removal revokes management and publishing despite unchanged scopes.
        assert owners('remove', owner='test-owner', actor=outsider['token'])[0] == 200
        assert owners()[0] == 403
        assert request('/v1/cuts/greeter/versions', 'POST', archive, headers)[0] == 403
        assert owners('add', owner='test-owner', actor=outsider['token'])[0] == 200
        assert owners('remove')[0] == 200
        count = db.execute("SELECT count(*) FROM audit_events WHERE action = 'owner_remove'").fetchone()[0]
        assert owners('remove')[0] == 200
        assert db.execute("SELECT count(*) FROM audit_events WHERE action = 'owner_remove'").fetchone()[0] == count
        assert owners(actor=outsider['token'])[0] == 403
        event = db.execute("SELECT target_owner, reason, credential_id FROM audit_events WHERE action = 'owner_add' ORDER BY id LIMIT 1").fetchone()
        assert event == ('other-owner', 'ownership test', issued['id'])

        assert manage('takedown', token)[0] == 403
        assert manage('yank', 'invalid')[0] == 401
        assert manage('yank', token, '   ')[0] == 400
        assert manage('yank', token, 'x' * 1025)[0] == 400
        assert request('/v1/cuts/greeter/versions/1.0.0/yank', 'POST', b'[]',
                       {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/json'})[0] == 400
        # An audit failure must roll back the state update.
        db.execute("CREATE TRIGGER fail_yank_audit BEFORE INSERT ON audit_events WHEN NEW.action = 'yank' BEGIN SELECT RAISE(ABORT, 'test failure'); END")
        db.commit()
        assert manage('yank', token)[0] == 500
        assert request('/v1/cuts/greeter/versions/1.0.0')[1]['yanked'] is False
        db.execute('DROP TRIGGER fail_yank_audit')
        db.commit()
        status, yanked = manage('yank', token)
        assert status == 200 and yanked['yanked'] is True and yanked['archive'] == release['archive']
        assert manage('yank', token, 'repeat')[0] == 200
        assert db.execute("SELECT count(*) FROM audit_events WHERE action = 'yank'").fetchone()[0] == 1
        shutil.rmtree(consumer / 'cuts')
        run(str(facet), 'install', cwd=consumer, env=env)
        assert (consumer / 'cuts/greeter/lib/greeter.di').exists()
        status, unyanked = manage('unyank', token)
        assert status == 200 and unyanked['yanked'] is False
        assert request('/v1/cuts/greeter/versions')[1]['versions'][0]['yanked'] is False
        admin = credential('issue', 'operator', '3600', 'admin', 'takedown test')
        # Failure to audit the replacement must not revoke the old credential.
        before = db.execute('SELECT count(*) FROM credentials').fetchone()[0]
        db.execute("CREATE TRIGGER fail_issue_audit BEFORE INSERT ON audit_events WHEN NEW.action = 'credential_issue' BEGIN SELECT RAISE(ABORT, 'test failure'); END")
        db.commit()
        failed = subprocess.run([str(diamond), 'credentials.di', 'rotate', str(admin['id']), '3600', 'failed rotation'],
                                cwd=work, env=env, capture_output=True, text=True, timeout=30)
        assert failed.returncode != 0
        assert db.execute('SELECT count(*) FROM credentials').fetchone()[0] == before
        assert db.execute('SELECT revoked_at FROM credentials WHERE id = ?', (admin['id'],)).fetchone()[0] is None
        db.execute('DROP TRIGGER fail_issue_audit')
        db.commit()
        for scope, ttl in [('publish:../../invalid', '3600'), ('admin', '3600junk')]:
            failed = subprocess.run([str(diamond), 'credentials.di', 'issue', 'operator', ttl, scope, 'invalid input'],
                                    cwd=work, env=env, capture_output=True, text=True, timeout=30)
            assert failed.returncode != 0
        assert db.execute('SELECT count(*) FROM credentials').fetchone()[0] == before
        rotated = credential('rotate', str(admin['id']), '3600', 'rotation test')
        assert manage('takedown', admin['token'])[0] == 401
        status, taken = manage('takedown', rotated['token'], 'test removal')
        assert status == 200 and taken['taken_down'] is True
        assert manage('takedown', rotated['token'], 'repeat')[0] == 200
        assert db.execute("SELECT count(*) FROM audit_events WHERE action = 'takedown'").fetchone()[0] == 1
        assert request('/v1/cuts/greeter/versions/1.0.0')[0] == 404
        assert not any(row['name'] == 'greeter' for row in request('/catalog.json')[1]['releases'])
        assert request(release['archive']['path'])[0] == 404
        assert manage('unyank', token)[0] == 404
        audit_headers = {'Authorization': 'Bearer ' + rotated['token']}
        assert request('/v1/audit')[0] == 401
        assert request('/v1/audit', headers={'Authorization': 'Bearer ' + token})[0] == 403
        assert owners(actor=rotated['token'])[0] == 200
        assert owners('remove', owner='test-owner', actor=rotated['token'])[0] == 409
        for query in ('limit=101', 'limit=0', 'after=-1', 'after=abc', 'limit=2&limit=3', 'unknown=1'):
            assert request('/v1/audit?' + query, headers=audit_headers)[0] == 400
        after = 0
        events = []
        while True:
            status, page = request(f'/v1/audit?after={after}&limit=2', headers=audit_headers)
            assert status == 200 and len(page['events']) <= 2
            events.extend(page['events'])
            if page['next_after'] is None:
                break
            assert page['next_after'] > after
            after = page['next_after']
        expected_ids = [row[0] for row in db.execute('SELECT id FROM audit_events ORDER BY id')]
        assert [event['id'] for event in events] == expected_ids
        assert all('token_digest' not in event and 'token' not in event for event in events)
        assert any(event['target_owner'] == 'other-owner' for event in events)
        assert any(event['action'] == 'takedown' and event['credential_scopes'] == ['admin'] for event in events)
        revoked = credential('revoke', str(rotated['id']), 'revocation test')
        assert revoked['revoked'] is True
        assert manage('takedown', rotated['token'])[0] == 401
        assert request('/v1/audit', headers=audit_headers)[0] == 401
        credential('revoke', str(rotated['id']), 'repeat revocation')
        assert db.execute("SELECT count(*) FROM audit_events WHERE action = 'credential_revoke' AND credential_id = ?", (rotated['id'],)).fetchone()[0] == 1
        audit = db.execute("SELECT reason, credential_id, sha256 FROM audit_events WHERE action = 'takedown'").fetchone()
        assert audit == ('test removal', rotated['id'], release['archive']['sha256'])
        inventory = credential('list')
        assert any(item['id'] == rotated['id'] and item['revoked_at'] is not None for item in inventory)
        assert all('token' not in item and 'token_digest' not in item for item in inventory)
        # Exercise bounded, one-row-per-cut catalog pagination without creating
        # public artifacts: SemVer order picks 1.0.10 over 1.0.9, an unyanked
        # release beats a newer yanked one, and an all-yanked cut still appears.
        temporary_cuts = []
        for index in range(105):
            cut_id = db.execute("INSERT INTO cuts (name, created_at) VALUES (?, 1)", (f'catalog_page_{index}',)).lastrowid
            temporary_cuts.append(cut_id)
            for version, yanked in (('1.0.9', 0), ('1.0.10', 1 if index == 0 else 0), ('1.1.0', 1)):
                db.execute('INSERT INTO releases (cut_id, version, dependencies, sha256, size, yanked, created_at) VALUES (?, ?, ?, ?, 1, ?, 1)',
                           (cut_id, version, '{}', hashlib.sha256(f'catalog-{index}-{version}'.encode()).hexdigest(), yanked))
        db.commit()
        seen = []
        after = 0
        while True:
            status, page = request('/catalog.json?after=' + str(after))
            assert status == 200 and len(page['releases']) <= 100
            seen.extend(page['releases'])
            if page['next_after'] is None:
                break
            assert page['next_after'] > after
            after = page['next_after']
        listed_cuts = [row['id'] for row in seen]
        assert listed_cuts == sorted(set(listed_cuts))
        expected_cuts = [row[0] for row in db.execute('SELECT DISTINCT cut_id FROM releases WHERE takedown_reason IS NULL ORDER BY cut_id')]
        assert listed_cuts == expected_cuts
        latest = {row['name']: (row['version'], row['yanked']) for row in seen}
        assert latest['catalog_page_0'] == ('1.0.9', 0)
        assert all(latest[f'catalog_page_{index}'] == ('1.0.10', 0) for index in range(1, 105))
        db.execute('UPDATE releases SET yanked = 1 WHERE cut_id = ?', (temporary_cuts[1],))
        db.commit()
        assert {row['name']: row['version'] for row in request('/catalog.json')[1]['releases']}['catalog_page_1'] == '1.1.0'
        for cut_id in temporary_cuts:
            db.execute('DELETE FROM releases WHERE cut_id = ?', (cut_id,))
            db.execute('DELETE FROM cuts WHERE id = ?', (cut_id,))
        db.commit()
        dump = '\n'.join(db.iterdump())
        for raw in (token, outsider['token'], publisher_only['token'], admin['token'], rotated['token']):
            assert raw not in dump
        db.close()
        log.flush()
        log.seek(0)
        log_text = log.read()
        assert marker.decode() not in log_text
        for raw in (token, outsider['token'], publisher_only['token'], admin['token'], rotated['token']):
            assert raw not in log_text
        records = [json.loads(line) for line in log_text.splitlines() if line.strip()]
        assert any(row['message'] == 'request.timeout' for row in records)
        assert any(row['message'] == 'connection.rejected' for row in records)
        assert any(row['message'] == 'request.rejected' and row['status'] == 413 for row in records)
        completed = [row for row in records if row['message'] == 'request.completed']
        assert set(request_ids).issubset({row['request_id'] for row in completed})
        assert completed and all(row['request_id'] and row['duration_ms'] >= 0 for row in completed)
        assert all('path' not in row and 'headers' not in row and 'body' not in row for row in completed)
        backup_tool = str(source / 'applications/registry/backup.py')
        backup = work / 'backup'
        restored = work / 'restored'
        run(sys.executable, backup_tool, 'backup', str(data), str(backup))
        run(sys.executable, backup_tool, 'restore', str(backup), str(restored))
        with sqlite3.connect(restored / 'registry.db') as recovered:
            assert '\n'.join(recovered.iterdump()) == dump
        assert not list((restored / 'staging').iterdir())
        assert not (restored / 'blobs' / digest).exists()  # orphan excluded
        result = subprocess.run([sys.executable, backup_tool, 'restore', str(backup), str(restored)],
                                capture_output=True, timeout=10)
        assert result.returncode != 0 and (restored / 'registry.db').exists()
        blob = next((backup / 'blobs').iterdir())
        original_bytes = blob.read_bytes()
        blob.write_bytes(b'corrupt')
        failed = work / 'failed-restore'
        result = subprocess.run([sys.executable, backup_tool, 'restore', str(backup), str(failed)],
                                capture_output=True, timeout=10)
        assert result.returncode != 0 and not failed.exists()
        blob.write_bytes(original_bytes)
        saved_database = (backup / 'registry.db').read_bytes()
        (backup / 'registry.db').write_bytes(b'corrupt database')
        result = subprocess.run([sys.executable, backup_tool, 'restore', str(backup), str(failed)],
                                capture_output=True, timeout=10)
        assert result.returncode != 0 and not failed.exists()
        (backup / 'registry.db').write_bytes(saved_database)
        manifest = backup / 'manifest.json'
        manifest.unlink()
        result = subprocess.run([sys.executable, backup_tool, 'restore', str(backup), str(failed)],
                                capture_output=True, timeout=10)
        assert result.returncode != 0 and not failed.exists()
        server.terminate()
        server.wait(timeout=5)
        env['REGISTRY_ROOT'] = str(restored)
        server = subprocess.Popen([str(diamond), 'app.di'], cwd=work, env=env, stdout=log, stderr=log)
        for _ in range(100):
            try:
                conn = http.client.HTTPConnection('127.0.0.1', port, timeout=1)
                conn.request('GET', '/registry/health')
                response = conn.getresponse()
                ready = response.status == 200
                response.read()
                conn.close()
                if ready:
                    break
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(.05)
        else:
            raise AssertionError('restored registry failed to start')
        assert request('/v1/cuts/greeter/versions/1.0.0')[0] == 404
        assert not any(row['name'] == 'greeter' for row in request('/catalog.json')[1]['releases'])
        assert request('/v1/audit', headers=audit_headers)[0] == 401
        recovery_consumer = work / 'recovery-consumer'
        recovery_consumer.mkdir()
        run(str(facet), 'init', 'recovery-consumer', cwd=recovery_consumer, env=env)
        run(str(facet), 'add', 'helper', '--registry', url, '--version', '^1.0.0', cwd=recovery_consumer, env=env)
        run(str(facet), 'update', cwd=recovery_consumer, env=env)
        assert run(str(diamond), '-e', 'require_cut "helper"\nhelper_value()',
                   cwd=recovery_consumer, env=env).stdout.strip() == 'installed from registry'
        print('registry HTTPS publish/install, monitoring, administration, and restore tests passed')
    finally:
        if proxy:
            proxy.shutdown()
            proxy.server_close()
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()
        log.close()
