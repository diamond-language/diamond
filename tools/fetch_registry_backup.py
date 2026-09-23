#!/usr/bin/env python3
"""Manually download and verify a registry snapshot into a new local directory."""
import argparse
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sqlite3
import sys
import tarfile

sys.dont_write_bytecode = True

ROOT = Path(__file__).resolve().parents[1]


def fetch(destination, host, app, data, port=22):
    if not 1 <= port <= 65535:
        raise ValueError("SSH port must be between 1 and 65535")
    if host.startswith('-') or any(char.isspace() for char in host):
        raise ValueError('invalid SSH host')
    if not app.startswith('/') or not data.startswith('/'):
        raise ValueError('remote application and data paths must be absolute')
    destination.mkdir(mode=0o700)
    try:
        archive = destination / '.transfer.tar'
        command = ("set -eu; work=$(mktemp -d /tmp/diamond-registry-backup.XXXXXXXX); "
                   "trap 'rm -rf -- \"$work\"' EXIT; python3 "
                   + shlex.quote(app.rstrip('/') + '/backup.py') + ' backup '
                   + shlex.quote(data) + ' "$work/snapshot" >/dev/null; '
                   + 'tar -C "$work" -cf - snapshot')
        with archive.open('xb') as output:
            subprocess.run(['ssh', '-F', '/dev/null', '-p', str(port), '-o', 'BatchMode=yes', '-o',
                            'StrictHostKeyChecking=yes', '-o', 'ConnectTimeout=10',
                            host, command], stdout=output, check=True)
        with tarfile.open(archive) as snapshot:
            if any(member.name != 'snapshot' and not member.name.startswith('snapshot/') for member in snapshot.getmembers()):
                raise ValueError('unexpected snapshot archive contents')
            snapshot.extractall(destination, filter='data')
        specification = importlib.util.spec_from_file_location('registry_backup', ROOT / 'applications/registry/backup.py')
        backup = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(backup)
        backup.transfer('restore', destination / 'snapshot', destination / '.restore-check')
        shutil.rmtree(destination / '.restore-check')
        archive.unlink()
    except BaseException:
        shutil.rmtree(destination)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path, help='new directory on your laptop')
    parser.add_argument('--port', type=int, default=22)
    parser.add_argument('--host', required=True, help='verified SSH destination, such as root@your-host')
    parser.add_argument('--app', required=True, help='remote directory containing backup.py')
    parser.add_argument('--data', required=True, help='remote REGISTRY_ROOT')
    args = parser.parse_args()
    os.umask(0o077)
    try:
        fetch(args.destination, args.host, args.app, args.data, args.port)
    except (OSError, ValueError, subprocess.CalledProcessError, tarfile.TarError, sqlite3.Error) as error:
        parser.exit(1, f'backup download failed: {error}\n')
    print(f'Verified snapshot: {args.destination / "snapshot"}')
