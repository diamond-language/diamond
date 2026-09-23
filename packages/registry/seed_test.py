"""Seed reproducibility and failure checks; no network or credentials."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[2]
tool = root / 'tools/prepare_registry_seed.py'
expected = root / 'docs/registry-launch-inventory.json'


def prepare(destination, *args, success=True):
    result = subprocess.run([sys.executable, str(tool), str(destination), *map(str, args)],
                            capture_output=True, text=True, timeout=60)
    assert (result.returncode == 0) == success, result.stderr
    return result


with tempfile.TemporaryDirectory(prefix='diamond-seed-test-') as temporary:
    work = Path(temporary)
    first, second = work / 'first', work / 'second'
    prepare(first, '--expect', expected)
    prepare(second, '--expect', expected)
    assert {p.name: p.read_bytes() for p in first.iterdir()} == {p.name: p.read_bytes() for p in second.iterdir()}
    saved = (first / 'inventory.json').read_bytes()
    prepare(first, success=False)
    assert (first / 'inventory.json').read_bytes() == saved
    changed = json.loads(saved)
    changed['cuts'][0]['sha256'] = '0' * 64
    (work / 'changed.json').write_text(json.dumps(changed))
    failed = work / 'failed'
    prepare(failed, '--expect', work / 'changed.json', success=False)
    assert not failed.exists()
    selection = work / 'selection.json'
    registry_version = json.loads((root / 'applications/registry/launch-cuts.json').read_text())['registry']
    selection.write_text(json.dumps({'registry': registry_version}))
    assert 'dependency missing' in prepare(failed, '--selection', selection, success=False).stderr
    assert not failed.exists()
    selection.write_text(json.dumps({'registry': '99.0.0'}))
    assert 'identity changed' in prepare(failed, '--selection', selection, success=False).stderr
    assert not failed.exists()
print('seed reproducibility, reviewed digest, destination preservation, and dependency checks passed')
