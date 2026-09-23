"""Test archive download verification with a controlled SSH transport."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import sqlite3
import sys
import tarfile
import tempfile
from unittest.mock import patch

sys.dont_write_bytecode = True

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('download', root / 'tools/fetch_registry_backup.py')
download = importlib.util.module_from_spec(spec)
spec.loader.exec_module(download)
with tempfile.TemporaryDirectory(prefix='registry-download-') as temporary:
    work = Path(temporary)
    snapshot = work / 'snapshot'
    (snapshot / 'blobs').mkdir(parents=True)
    data = b'verified backup blob'
    digest = hashlib.sha256(data).hexdigest()
    (snapshot / 'blobs' / digest).write_bytes(data)
    with sqlite3.connect(snapshot / 'registry.db') as db:
        db.execute('CREATE TABLE releases (sha256 TEXT, size INTEGER)')
        db.execute('INSERT INTO releases VALUES (?, ?)', (digest, len(data)))
    (snapshot / 'manifest.json').write_text(json.dumps(dict(format=1, blobs={digest: len(data)},
        database_sha256=hashlib.sha256((snapshot / 'registry.db').read_bytes()).hexdigest())))

    def transport(argv, stdout, check):
        assert argv[:4] == ['ssh', '-F', '/dev/null', '-p']
        assert 'StrictHostKeyChecking=yes' in argv
        with tarfile.open(fileobj=stdout, mode='w') as output:
            output.add(snapshot, arcname='snapshot')

    destination = work / 'downloaded'
    with patch.object(download.subprocess, 'run', transport):
        download.fetch(destination, 'operator@example.invalid', '/app', '/data')
        assert (destination / 'snapshot/blobs' / digest).read_bytes() == data
        assert not (destination / '.transfer.tar').exists()
        try:
            download.fetch(destination, 'operator@example.invalid', '/app', '/data')
            raise AssertionError('existing directory accepted')
        except FileExistsError:
            pass
        (snapshot / 'blobs' / digest).write_bytes(b'corrupt')
        try:
            download.fetch(work / 'bad', 'operator@example.invalid', '/app', '/data')
            raise AssertionError('corrupt snapshot accepted')
        except ValueError:
            assert not (work / 'bad').exists()

    def malicious(argv, stdout, check):
        with tarfile.open(fileobj=stdout, mode='w') as output:
            member = tarfile.TarInfo('../escaped')
            member.size = 1
            output.addfile(member, io.BytesIO(b'x'))

    with patch.object(download.subprocess, 'run', malicious):
        try:
            download.fetch(work / 'bad', 'operator@example.invalid', '/app', '/data')
            raise AssertionError('unexpected archive accepted')
        except ValueError:
            assert not (work / 'bad').exists() and not (work / 'escaped').exists()
print('manual backup download: verified restore, corruption, traversal, and destination preservation passed')
