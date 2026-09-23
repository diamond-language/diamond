"""Privileged staging drill: run only in a disposable local QEMU guest."""
import hashlib
import http.client
import json
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]


def run(*args, **kwargs):
    result = subprocess.run(list(map(str, args)), capture_output=True, text=True, timeout=60, **kwargs)
    if result.returncode:
        raise AssertionError(f'{Path(str(args[0])).name} failed: {result.stderr}')
    return result.stdout


if os.geteuid() != 0 or os.environ.get('REGISTRY_QEMU_STAGING') != '1':
    raise SystemExit('Run as root with REGISTRY_QEMU_STAGING=1 inside the local staging VM')

with tempfile.TemporaryDirectory(prefix='diamond-registry-systemd-', dir='/opt') as temporary:
    work = Path(temporary)
    work.chmod(0o755)
    name = 'dreg-' + work.name.rsplit('-', 1)[1]
    unit_name = name + '.service'
    unit = Path('/run/systemd/system') / unit_name
    state = Path('/var/lib') / name
    environment = work / 'registry.env'
    account_created = False
    unit_created = False
    try:
        assert not unit.exists() and not state.exists()
        run('useradd', '--system', '--no-create-home', '--shell', '/usr/sbin/nologin', name)
        account_created = True
        account = pwd.getpwnam(name)
        app = work / 'app'
        app.mkdir(mode=0o755)
        run(ROOT / 'tools/install_local_cuts.sh', app)
        for filename in ('app.di', 'credentials.di', 'backup.py', 'catalog.di', 'catalog.html', 'catalog.js'):
            shutil.copy(ROOT / 'applications/registry' / filename, app / filename)
        for executable in ('diamond', 'facet'):
            shutil.copy(ROOT / 'build' / executable, work / executable)
        # Root-owned code is readable, but not writable, by the service account.
        for directory, _, files in os.walk(app):
            Path(directory).chmod(0o755)
            for filename in files:
                (Path(directory) / filename).chmod(0o644)
        state.mkdir(mode=0o700)
        os.chown(state, account.pw_uid, account.pw_gid)
        for directory in ('blobs', 'staging'):
            (state / directory).mkdir(mode=0o700)
            os.chown(state / directory, account.pw_uid, account.pw_gid)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]

        def write_environment(data):
            environment.write_text(f'REGISTRY_ROOT={data}\nREGISTRY_FACET={work}/facet\n'
                                   f'REGISTRY_PORT={port}\nREGISTRY_BASE=/registry\nDIAMOND_NO_CACHE=1\n')
            environment.chmod(0o600)

        write_environment(state)
        template = (ROOT / 'applications/registry/deploy/registry.service').read_text()
        for old, new in (
            ('User=diamond-registry', 'User=' + name),
            ('Group=diamond-registry', 'Group=' + name),
            ('StateDirectory=diamond-registry', 'StateDirectory=' + name),
            ('ReadWritePaths=/var/lib/diamond-registry', 'ReadWritePaths=' + str(state)),
            ('WorkingDirectory=/opt/diamond/applications/registry', 'WorkingDirectory=' + str(app)),
            ('EnvironmentFile=/etc/diamond-registry.env', 'EnvironmentFile=' + str(environment)),
            ('ExecStart=/opt/diamond/build/diamond app.di', 'ExecStart=' + str(work / 'diamond') + ' app.di'),
        ):
            assert old in template
            template = template.replace(old, new)
        with unit.open('x') as output:
            unit_created = True
            output.write(template)
        run('systemd-analyze', 'verify', unit)
        run('systemctl', 'daemon-reload')

        def property_value(key):
            return run('systemctl', 'show', unit_name, '--property=' + key, '--value').strip()

        def request(path='/health', method='GET', body=None, headers=None):
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=2)
            try:
                conn.request(method, '/registry' + path, body, headers or {})
                reply = conn.getresponse()
                return reply.status, reply.read()
            finally:
                conn.close()

        def healthy(previous=None):
            deadline = time.monotonic() + 30
            while time.monotonic() < deadline:
                try:
                    pid = int(property_value('MainPID'))
                    if pid and pid != previous and request()[0] == 200:
                        return pid
                except (OSError, http.client.HTTPException):
                    pass
                time.sleep(.2)
            raise AssertionError('service did not become healthy')

        run('systemctl', 'start', unit_name)
        pid = healthy()
        assert Path('/proc/' + str(pid)).stat().st_uid == account.pw_uid
        assert property_value('ProtectSystem') == 'strict'
        assert property_value('NoNewPrivileges') == 'yes'
        assert (state / 'registry.db').stat().st_uid == account.pw_uid
        assert (state / 'registry.db').stat().st_mode & 0o077 == 0
        print('systemd startup, service identity, sandbox settings, and private database passed', flush=True)
        credential_env = dict(os.environ, REGISTRY_ROOT=str(state), REGISTRY_OPERATOR='systemd-drill',
                              DIAMOND_NO_CACHE='1')
        token = json.loads(run('runuser', '-u', name, '--', work / 'diamond', 'credentials.di',
                              'issue', 'canary-owner', '3600', 'publish:canary', 'systemd drill',
                              cwd=app, env=credential_env))['token']
        cut = work / 'canary'
        (cut / 'lib').mkdir(parents=True)
        (cut / 'diamond.cut').write_text(json.dumps(dict(name='canary', version='1.0.0',
            summary='systemd canary', license='MIT', maintainers=[dict(name='Test', contact='test@example.com')], dependencies={})))
        (cut / 'README.md').write_text('Systemd staging canary\n')
        (cut / 'LICENSE').write_text('MIT\n')
        (cut / 'lib/canary.di').write_text('def canary() = "restored"\n')
        archive = work / 'canary.tar'
        run(work / 'facet', 'pack', cut, archive)
        payload = archive.read_bytes()
        digest = hashlib.sha256(payload).hexdigest()
        headers = {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/octet-stream'}
        assert request('/v1/cuts/canary/versions', 'POST', payload, headers)[0] == 201
        run('systemctl', 'restart', unit_name)
        pid = healthy(pid)
        assert request('/v1/blobs/sha256/' + digest) == (200, payload)
        run('systemctl', 'kill', '--kill-whom=main', '--signal=SIGKILL', unit_name)
        pid = healthy(pid)
        assert int(property_value('NRestarts')) >= 1
        assert request('/v1/blobs/sha256/' + digest) == (200, payload)
        print('explicit restart and automatic crash recovery preserved published data', flush=True)
        snapshot = work / 'snapshot'
        run('python3', app / 'backup.py', 'backup', state, snapshot)
        run('systemctl', 'stop', unit_name)
        assert property_value('ActiveState') == 'inactive'
        recovered = state / 'recovered'
        run('python3', app / 'backup.py', 'restore', snapshot, recovered)
        for directory, _, files in os.walk(recovered):
            os.chown(directory, account.pw_uid, account.pw_gid)
            for filename in files:
                os.chown(Path(directory) / filename, account.pw_uid, account.pw_gid)
        write_environment(recovered)
        run('systemctl', 'start', unit_name)
        healthy()
        assert request('/v1/blobs/sha256/' + digest) == (200, payload)
        assert request('/v1/cuts/canary/versions', 'POST', payload, headers)[0] == 200
        assert request('/v1/cuts/canary/versions', 'POST', payload)[0] == 401
        manifest = json.loads((cut / 'diamond.cut').read_text())
        manifest['version'] = '1.0.1'
        (cut / 'diamond.cut').write_text(json.dumps(manifest))
        run(work / 'facet', 'pack', cut, work / 'new-release.tar')
        new_payload = (work / 'new-release.tar').read_bytes()
        assert request('/v1/cuts/canary/versions', 'POST', new_payload, headers)[0] == 201
        new_digest = hashlib.sha256(new_payload).hexdigest()
        assert request('/v1/blobs/sha256/' + new_digest) == (200, new_payload)

        journal = run('journalctl', '-u', unit_name, '--no-pager', '-o', 'cat')
        assert 'request.completed' in journal and token not in journal
        run('systemctl', 'stop', unit_name)
        assert property_value('ActiveState') == 'inactive'
        print('restored systemd service passed blob, credential, write, journal, and clean-stop checks', flush=True)
    except BaseException:
        print(run('journalctl', '-u', unit_name, '--no-pager', '-n', '80'))
        raise
    finally:
        if unit_created:
            subprocess.run(['systemctl', 'stop', unit_name], check=False, capture_output=True)
            unit.unlink()
            subprocess.run(['systemctl', 'daemon-reload'], check=False, capture_output=True)
            subprocess.run(['systemctl', 'reset-failed', unit_name], check=False, capture_output=True)
        if account_created:
            shutil.rmtree(state, ignore_errors=True)
            run('userdel', name)
