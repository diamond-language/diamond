#!/usr/bin/env python3
"""Publish reviewed archive bytes over verified HTTPS using a private credential file."""
import argparse
import hashlib
import http.client
import json
from pathlib import Path
import re
import ssl
import time
from urllib.parse import urlsplit


def publish(base, seed, credentials):
    url = urlsplit(base)
    if url.scheme != 'https' or not url.hostname or url.username or url.password or url.query or url.fragment:
        raise ValueError('expected an HTTPS registry base URL without credentials/query/fragment')
    if credentials.stat().st_mode & 0o077:
        raise ValueError('credential file must be private (mode 0600)')
    token = json.loads(credentials.read_text())['token']
    if not isinstance(token, str) or not token or any(ord(char) < 33 or ord(char) > 126 for char in token):
        raise ValueError('invalid credential')
    inventory = json.loads((seed / 'inventory.json').read_text())
    if inventory.get('format') != 1 or not inventory.get('cuts'):
        raise ValueError('invalid inventory')
    names = set()
    for cut in inventory['cuts']:
        name = cut['name']
        if not re.fullmatch('[a-z][a-z0-9_]*', name) or name in names or cut['archive'] != name + '.tar':
            raise ValueError('invalid or duplicate cut identity')
        names.add(name)
        data = (seed / cut['archive']).read_bytes()
        if len(data) != cut['size'] or hashlib.sha256(data).hexdigest() != cut['sha256']:
            raise ValueError('archive differs from reviewed inventory: ' + name)
    # Verify every local archive before the first network write.
    for cut in inventory['cuts']:
        data = (seed / cut['archive']).read_bytes()
        if len(data) != cut['size'] or hashlib.sha256(data).hexdigest() != cut['sha256']:
            raise ValueError('archive changed while publishing: ' + cut['name'])
        for attempt in range(6):
            connection = http.client.HTTPSConnection(url.hostname, url.port or 443,
                context=ssl.create_default_context(), timeout=60)
            try:
                connection.request('POST', url.path.rstrip('/') + '/v1/cuts/' + cut['name'] + '/versions',
                    data, {'Authorization': 'Bearer ' + token, 'Content-Type': 'application/octet-stream'})
                response = connection.getresponse()
                status = response.status
                body = response.read(65537)
            finally:
                connection.close()
            if status != 429:
                break
            time.sleep(10.1)
        if status not in (200, 201) or len(body) > 65536:
            raise ValueError(f"publish failed for {cut['name']}: HTTP {status}")
        published = json.loads(body)
        if any(published.get(key) != cut[key] for key in ('name', 'version', 'sha256', 'size')):
            raise ValueError('published identity mismatch: ' + cut['name'])
        print(f"verified published {cut['name']} {cut['version']} {cut['sha256']}", flush=True)
        time.sleep(10.1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('registry')
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--credentials', type=Path, required=True)
    args = parser.parse_args()
    try:
        publish(args.registry, args.seed, args.credentials)
    except (OSError, ValueError, KeyError, http.client.HTTPException) as error:
        parser.exit(1, f'seed publication failed: {error}\n')
