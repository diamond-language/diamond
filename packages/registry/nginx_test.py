"""Real nginx/HTTPS staging drill. Run inside a disposable local QEMU VM."""
import collections
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import tempfile
import time

source = Path(__file__).resolve().parents[2]
diamond = str(source / 'build/diamond')
facet = str(source / 'build/facet')
nginx = shutil.which('nginx') or '/usr/sbin/nginx'


def run(*args, **kwargs):
    result = subprocess.run(args, capture_output=True, text=True, timeout=90, **kwargs)
    if result.returncode:
        # Do not echo argv: publishing arguments contain a temporary credential.
        raise AssertionError(f'{Path(args[0]).name} failed: {result.stderr}')
    return result


def port():
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        return probe.getsockname()[1]


def stop(process):
    if process is not None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


with tempfile.TemporaryDirectory(prefix='diamond-registry-nginx-') as temporary:
    work = Path(temporary)
    backend_port, proxy_port = port(), port()
    assert backend_port != proxy_port
    run(str(source / 'tools/install_local_cuts.sh'), str(work))
    for name in ('app.di', 'credentials.di'):
        shutil.copy(source / 'applications/registry' / name, work / name)
    (work / 'data/blobs').mkdir(parents=True)
    (work / 'data/staging').mkdir()
    env = dict(os.environ, REGISTRY_ROOT=str(work / 'data'), REGISTRY_FACET=facet,
               REGISTRY_PORT=str(backend_port), REGISTRY_BASE='/registry',
               REGISTRY_OPERATOR='staging-test', DIAMOND_NO_CACHE='1')
    run('openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
        '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost',
        '-keyout', str(work / 'key.pem'), '-out', str(work / 'cert.pem'))
    template = (source / 'applications/registry/deploy/nginx.conf.example').read_text()
    # Only host-specific settings change; rate, size and buffering policy is exact.
    for old, new in (
        ('listen 443 ssl;', f'listen 127.0.0.1:{proxy_port} ssl;'),
        ('cuts.example.com', 'localhost'),
        ('/etc/ssl/registry/fullchain.pem', str(work / 'cert.pem')),
        ('/etc/ssl/registry/privkey.pem', str(work / 'key.pem')),
        ('/var/log/nginx/registry-access.log', str(work / 'access.log')),
        ('/var/log/nginx/registry-error.log', str(work / 'error.log')),
        ('127.0.0.1:18120', f'127.0.0.1:{backend_port}'),
    ):
        assert old in template
        template = template.replace(old, new)
    config = work / 'nginx.conf'
    config.write_text('daemon off;\nworker_processes 1;\npid ' + str(work / 'nginx.pid') + ';\n'
                      'error_log ' + str(work / 'startup.log') + ';\n'
                      'events { worker_connections 256; }\nhttp {\n'
                      'client_body_temp_path ' + str(work / 'body') + ';\n'
                      'proxy_temp_path ' + str(work / 'proxy') + ';\n' + template + '\n}\n')
    run(nginx, '-t', '-p', str(work), '-c', str(config))
    trust = ssl.create_default_context(cafile=str(work / 'cert.pem'))
    env['CURL_CA_BUNDLE'] = str(work / 'cert.pem')
    env['NO_PROXY'] = 'localhost,127.0.0.1'
    url = f'https://localhost:{proxy_port}/registry'

    def request(path='/health', method='GET', body=None, headers=None):
        connection = http.client.HTTPSConnection('localhost', proxy_port, context=trust, timeout=10)
        try:
            connection.request(method, '/registry' + path, body, headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    proxy = server = None
    with (work / 'process.log').open('w+') as log:
        try:
            server = subprocess.Popen([diamond, 'app.di'], cwd=work, env=env, stdout=log, stderr=log)
            for _ in range(100):
                try:
                    conn = http.client.HTTPConnection('127.0.0.1', backend_port, timeout=1)
                    conn.request('GET', '/registry/health')
                    response = conn.getresponse()
                    ready = response.status == 200
                    response.read()
                    conn.close()
                    if ready:
                        break
                except OSError:
                    pass
                time.sleep(.1)
            else:
                raise AssertionError('registry did not start')
            proxy = subprocess.Popen([nginx, '-p', str(work), '-c', str(config)], stdout=log, stderr=log)
            for _ in range(100):
                try:
                    status, headers, body = request()
                    if status == 200:
                        break
                except OSError:
                    pass
                time.sleep(.05)
            else:
                raise AssertionError('nginx did not start')
            assert headers.get('X-Request-ID')
            assert json.loads(body) == {'protocol': 1, 'status': 'ok'}
            token = json.loads(run(diamond, 'credentials.di', 'issue', 'staging', '3600',
                                   'publish:canary', 'nginx drill', cwd=work, env=env).stdout)['token']
            package = work / 'canary'
            (package / 'lib').mkdir(parents=True)
            (package / 'diamond.cut').write_text(json.dumps(dict(name='canary', version='1.0.0',
                summary='staging canary', license='MIT', dependencies={})))
            (package / 'README.md').write_text('Staging canary\n')
            (package / 'LICENSE').write_text('MIT\n')
            (package / 'lib/canary.di').write_text('def canary() = "nginx staging passed"\n')
            run(facet, 'publish', str(package), '--registry', url, '--token', token, env=env)
            consumer = work / 'consumer'
            consumer.mkdir()
            run(facet, 'init', 'consumer', cwd=consumer, env=env)
            run(facet, 'add', 'canary', '--registry', url, '--version', '^1.0.0', cwd=consumer, env=env)
            run(facet, 'update', cwd=consumer, env=env)
            assert run(diamond, '-e', 'require_cut "canary"\ncanary()', cwd=consumer,
                       env=env).stdout.strip() == 'nginx staging passed'
            # Declared oversized uploads are rejected before the client sends a body.
            status, _, _ = request('/v1/cuts/canary/versions', 'POST', b'',
                                   {'Content-Length': str(26214401), 'Expect': '100-continue'})
            assert status == 413
            assert request('/v1/cuts/canary/versions', 'POST', b'invalid')[0] == 401
            marker = 'staging-sensitive-marker'
            writes = [request('/v1/cuts/canary/versions?secret=' + marker, 'POST', b'invalid',
                              {'Authorization': 'Bearer ' + marker})[0] for _ in range(8)]
            # Query-bearing route is not a publish route; use an exact route for auth.
            assert 429 in writes and set(writes) <= {404, 429}
            assert request()[0] == 200  # write throttling does not block reads
            reads = [request()[0] for _ in range(160)]
            assert 200 in reads and 429 in reads and set(reads) <= {200, 429}
            # Allow the full read burst debt to drain; write debt lasts much longer.
            time.sleep(3)
            assert request()[0] == 200
            stop(proxy)
            proxy = None
            records = [json.loads(line) for line in (work / 'access.log').read_text().splitlines()]
            assert any(row['status'] == 429 and row['limit'] == 'REJECTED' for row in records)
            assert any(row['status'] == 200 and row['request_id'] for row in records)
            access = (work / 'access.log').read_text()
            assert token not in access and marker not in access and '/registry' not in access
            print('nginx configuration, HTTPS facet publish/install, 413, write/read 429, recovery, and log checks passed')
            print('read statuses:', dict(collections.Counter(reads)))
        except BaseException:
            log.flush()
            log.seek(0)
            print(log.read())
            for name in ('startup.log', 'error.log'):
                if (work / name).exists():
                    print((work / name).read_text())
            raise
        finally:
            stop(proxy)
            stop(server)
