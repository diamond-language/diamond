#!/usr/bin/env python3
"""Build a deterministic registry seed in a new directory; never publish it."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def facet_json(facet, *args):
    result = subprocess.run([str(facet), *map(str, args)], capture_output=True, text=True, timeout=60)
    if result.returncode:
        raise ValueError(result.stderr.strip() or 'facet command failed')
    return result.stdout


def prepare(destination, selection, facet, expected=None):
    selected = json.loads(selection.read_text())
    if not isinstance(selected, dict) or not selected:
        raise ValueError('selection must map cut names to exact versions')
    for name, version in selected.items():
        if not re.fullmatch('[a-z][a-z0-9_]*', name) or not isinstance(version, str):
            raise ValueError('invalid selected name or version')
    destination.mkdir(mode=0o700)  # Existing directories are never overwritten.
    try:
        entries = {}
        for name in sorted(selected):
            archive = destination / (name + '.tar')
            facet_json(facet, 'pack', ROOT / 'packages' / name, archive)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            metadata = json.loads(facet_json(facet, 'verify', archive, '--sha256', digest, '--json'))
            if metadata['name'] != name or metadata['version'] != selected[name]:
                raise ValueError('selected identity changed: ' + name)
            entries[name] = {key: metadata[key] for key in ('name', 'version', 'dependencies', 'sha256', 'size')}
            entries[name]['archive'] = archive.name
        ordered = []
        visiting = set()
        visited = set()

        def visit(name):
            if name not in entries:
                raise ValueError('dependency missing from seed: ' + name)
            if name in visiting:
                raise ValueError('dependency cycle at: ' + name)
            if name in visited:
                return
            visiting.add(name)
            for dependency in sorted(entries[name]['dependencies']):
                visit(dependency)
            visiting.remove(name)
            visited.add(name)
            ordered.append(entries[name])

        for name in sorted(entries):
            visit(name)
        inventory = {'format': 1, 'cuts': ordered}
        if expected is not None and inventory != json.loads(expected.read_text()):
            raise ValueError('seed differs from reviewed inventory; review changes before updating it')
        (destination / 'inventory.json').write_text(json.dumps(inventory, indent=2, sort_keys=True) + '\n')
        return inventory
    except BaseException:
        shutil.rmtree(destination)
        raise


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--selection', type=Path, default=ROOT / 'applications/registry/launch-cuts.json')
    parser.add_argument('--facet', type=Path, default=ROOT / 'build/facet')
    parser.add_argument('--expect', type=Path, help='require an exact match to a reviewed inventory')
    args = parser.parse_args()
    try:
        inventory = prepare(args.destination, args.selection, args.facet, args.expect)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f'seed preparation failed: {error}\n')
    print(f"prepared {len(inventory['cuts'])} verified cuts in {args.destination}")
