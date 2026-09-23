"""Live facet -> HTTPS proxy -> Diamond registry integration; no external network."""
import hashlib
import http.client
import http.server
import json
import os
from pathlib import Path
import secrets
import shutil
import socket
import sqlite3
import ssl
import subprocess
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
    shutil.copy(source / 'applications/registry/app.di', work / 'app.di')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    env = dict(os.environ, REGISTRY_ROOT=str(data), REGISTRY_FACET=str(facet),
               REGISTRY_PORT=str(port), REGISTRY_BASE='/registry', DIAMOND_NO_CACHE='1')
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

        token = secrets.token_hex(32)
        db = sqlite3.connect(data / 'registry.db')
        db.execute('INSERT INTO credentials (token_digest, subject, scopes, created_at) VALUES (?, ?, ?, ?)',
                   (hashlib.sha256(token.encode()).hexdigest(), 'test-owner',
                    json.dumps(['publish:greeter', 'publish:helper']), 0))
        db.commit()
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

        def request(path, method='GET', body=None, headers=None):
            connection = http.client.HTTPSConnection('localhost', public_port, context=trust, timeout=30)
            connection.request(method, '/registry' + path, body, headers or {})
            response = connection.getresponse()
            payload = response.read()
            status = response.status
            content_type = response.getheader('Content-Type')
            connection.close()
            if content_type == 'application/json':
                payload = json.loads(payload)
            return status, payload

        def package(name, version, body, dependencies=None):
            path = work / name
            (path / 'lib').mkdir(parents=True, exist_ok=True)
            (path / 'diamond.cut').write_text(json.dumps(dict(name=name, version=version, summary='test cut',
                license='MIT', dependencies=dependencies or {})))
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

        consumer = work / 'consumer'
        consumer.mkdir()
        run(str(facet), 'init', 'consumer', cwd=consumer, env=env)
        run(str(facet), 'add', 'greeter', '--registry', url, '--version', '^1.0.0', cwd=consumer, env=env)
        run(str(facet), 'update', cwd=consumer, env=env)
        assert run(str(diamond), '-e', 'require_cut "greeter"\ngreet()', cwd=consumer, env=env).stdout.strip() == 'installed from registry'
        assert (consumer / 'cuts/helper/lib/helper.di').exists()
        db.execute('UPDATE releases SET yanked = 1 WHERE version = ?', ('1.0.0',))
        db.commit()
        shutil.rmtree(consumer / 'cuts')
        run(str(facet), 'install', cwd=consumer, env=env)
        assert (consumer / 'cuts/greeter/lib/greeter.di').exists()
        db.execute("UPDATE releases SET takedown_reason = 'test removal' WHERE cut_id = (SELECT id FROM cuts WHERE name = 'greeter')")
        db.commit()
        assert request('/v1/cuts/greeter/versions/1.0.0')[0] == 404
        assert request(release['archive']['path'])[0] == 404
        db.close()
        print('registry HTTPS publish, transitive resolve, locked install, and takedown tests passed')
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
