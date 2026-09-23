#!/usr/bin/env python3
"""Probe registry HTTPS health and optionally a pinned archive; emit one JSON record."""
import argparse
import hashlib
import http.client
import json
import re
import ssl
import time
from urllib.parse import urlsplit


def probe(base, ca_file=None, timeout=10, archive=None):
    url = urlsplit(base)
    if (url.scheme != 'https' or not url.hostname or url.username is not None
            or url.password is not None or url.query or url.fragment):
        raise ValueError('expected an HTTPS base URL without credentials, query or fragment')
    context = ssl.create_default_context(cafile=ca_file)
    connection = http.client.HTTPSConnection(url.hostname, url.port or 443,
                                           context=context, timeout=timeout)
    prefix = url.path.rstrip('/')
    try:
        connection.request('GET', prefix + '/health')
        response = connection.getresponse()
        body = response.read(4097)
        if response.status != 200 or len(body) > 4096:
            raise ValueError('health response failed')
        health = json.loads(body)
        if not isinstance(health, dict) or health.get('protocol') != 1 or health.get('status') != 'ok':
            raise ValueError('unexpected health response')
        connection.close()
        if archive:
            connection.request('GET', prefix + '/v1/blobs/sha256/' + archive)
            response = connection.getresponse()
            if response.status != 200:
                raise ValueError('archive response failed')
            digest = hashlib.sha256()
            total = 0
            started = time.monotonic()
            while True:
                block = response.read(65536)
                if not block:
                    break
                total += len(block)
                if total > 58720256 or time.monotonic() - started > timeout:
                    raise ValueError('archive probe limit exceeded')
                digest.update(block)
            if digest.hexdigest() != archive:
                raise ValueError('archive digest mismatch')
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('base_url')
    parser.add_argument('--ca-file')
    parser.add_argument('--timeout', type=int, default=10, choices=range(1, 301), metavar='SECONDS')
    parser.add_argument('--archive-sha256', type=lambda value: value if re.fullmatch('[0-9a-f]{64}', value)
                        else parser.error('archive digest must be 64 lowercase hex characters'))
    args = parser.parse_args()
    started = time.monotonic()
    try:
        probe(args.base_url, args.ca_file, args.timeout, args.archive_sha256)
        result = {'ok': True}
    except (OSError, ValueError, http.client.HTTPException) as error:
        # Avoid exception text: it can contain URLs or remote-controlled contents.
        result = {'ok': False, 'error': type(error).__name__}
    result['duration_ms'] = round((time.monotonic() - started) * 1000)
    print(json.dumps(result))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
