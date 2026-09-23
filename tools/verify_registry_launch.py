#!/usr/bin/env python3
"""Verify the reviewed launch inventory through public facet resolution and reinstall."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd):
    result = subprocess.run(list(map(str, args)), cwd=cwd, capture_output=True, text=True,
                            env=dict(os.environ, DIAMOND_NO_CACHE='1'), timeout=120)
    if result.returncode:
        raise ValueError(result.stderr or 'command failed')
    return result.stdout


def verify(registry, inventory):
    if not registry.startswith('https://'):
        raise ValueError('public verification requires HTTPS')
    cuts = json.loads(inventory.read_text())['cuts']
    dependencies = {name for cut in cuts for name in cut['dependencies']}
    with tempfile.TemporaryDirectory(prefix='diamond-public-install-') as temporary:
        work = Path(temporary)
        run(ROOT / 'build/facet', 'init', 'launch-verification', cwd=work)
        manifest = json.loads((work / 'diamond.cut').read_text())
        manifest['dependencies'] = {cut['name']: {'registry': registry, 'version': cut['version']}
                                    for cut in cuts if cut['name'] not in dependencies}
        (work / 'diamond.cut').write_text(json.dumps(manifest))
        run(ROOT / 'build/facet', 'update', cwd=work)
        lock_bytes = (work / 'facet.lock').read_bytes()
        lock = json.loads(re.sub(rb',\s*}\s*$', b'}', lock_bytes))
        if set(lock) != {cut['name'] for cut in cuts}:
            raise ValueError('resolved cut set differs from inventory')
        for cut in cuts:
            locked = lock[cut['name']]
            if locked['source'] != 'registry' or locked['registry'] != registry:
                raise ValueError('registry source mismatch: ' + cut['name'])
            if any(locked[key] != cut[key] for key in ('version', 'sha256', 'size')):
                raise ValueError('locked archive differs from inventory: ' + cut['name'])
            run(ROOT / 'build/diamond', '-e', 'require_cut "' + cut['name'] + '"\n"loaded"', cwd=work)
        shutil.rmtree(work / 'cuts')
        run(ROOT / 'build/facet', 'install', cwd=work)
        if (work / 'facet.lock').read_bytes() != lock_bytes:
            raise ValueError('locked reinstall changed the lockfile')
        print(f'{len(cuts)} public cuts resolved, digest-checked, loaded, and reinstalled from unchanged lock')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('registry')
    parser.add_argument('--inventory', type=Path, default=ROOT / 'docs/registry-launch-inventory.json')
    args = parser.parse_args()
    try:
        verify(args.registry, args.inventory)
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'public verification failed: {error}\n')
