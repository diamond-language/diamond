#!/usr/bin/env python3
"""Consistent local registry snapshots. Requires Python 3 and trusted directories."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import sqlite3


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()


def connect(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('database must be a regular file')
    return sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True)


def inventory(db):
    if db.execute('PRAGMA integrity_check').fetchall() != [('ok',)]:
        raise ValueError('database integrity check failed')
    if db.execute('PRAGMA foreign_key_check').fetchall():
        raise ValueError('database foreign key check failed')
    blobs = {}
    for sha, size in db.execute('SELECT sha256, size FROM releases ORDER BY sha256'):
        if not isinstance(sha, str) or not re.fullmatch('[0-9a-f]{64}', sha):
            raise ValueError('invalid blob digest')
        if type(size) is not int or size <= 0:
            raise ValueError('invalid blob size')
        if sha in blobs and blobs[sha] != size:
            raise ValueError('inconsistent blob size')
        blobs[sha] = size
    return blobs


def sync(path):
    with path.open('rb') as stream:
        os.fsync(stream.fileno())


def copy_blob(source, target, sha, size):
    if source.is_symlink() or not source.is_file():
        raise ValueError('blob must be a regular file: ' + sha)
    shutil.copyfile(source, target)
    if target.stat().st_size != size or digest(target) != sha:
        raise ValueError('blob integrity check failed: ' + sha)
    sync(target)


def sync_directory(path):
    fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def transfer(action, source, target):
    # mkdir is exclusive: an existing destination is never changed or removed.
    source = Path(source).resolve()
    target = Path(target).absolute()
    target.mkdir(mode=0o700)
    try:
        (target / 'blobs').mkdir(mode=0o700)
        if action == 'backup':
            original = connect(source / 'registry.db')
            snapshot = sqlite3.connect(target / 'registry.db')
            try:
                original.backup(snapshot)
                blobs = inventory(snapshot)
            finally:
                snapshot.close()
                original.close()
        else:
            manifest = json.loads((source / 'manifest.json').read_text())
            if manifest.get('format') != 1 or digest(source / 'registry.db') != manifest.get('database_sha256'):
                raise ValueError('invalid backup manifest or database digest')
            shutil.copyfile(source / 'registry.db', target / 'registry.db')
            db = connect(target / 'registry.db')
            try:
                blobs = inventory(db)
            finally:
                db.close()
            if blobs != manifest.get('blobs'):
                raise ValueError('backup inventory does not match database')
            (target / 'staging').mkdir(mode=0o700)
            sync_directory(target / 'staging')
        for sha, size in blobs.items():
            copy_blob(source / 'blobs' / sha, target / 'blobs' / sha, sha, size)
        sync(target / 'registry.db')
        sync_directory(target / 'blobs')
        if action == 'backup':
            # Completion marker is written only after every referenced blob is durable.
            manifest = {'format': 1, 'database_sha256': digest(target / 'registry.db'), 'blobs': blobs}
            (target / 'manifest.json').write_text(json.dumps(manifest, sort_keys=True) + '\n')
            sync(target / 'manifest.json')
        sync_directory(target)
        sync_directory(target.parent)
    except BaseException:
        shutil.rmtree(target)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('backup', 'restore'))
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    os.umask(0o077)
    try:
        transfer(args.action, args.source, args.destination)
    except (OSError, ValueError, sqlite3.Error) as error:
        parser.exit(1, f'{args.action} failed: {error}\n')
    print(f'{args.action} complete: {args.destination}')
