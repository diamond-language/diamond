#!/usr/bin/env python3
"""Sample Arel guard costs using perf, restricted to the benchmark loop."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile

import audit_arel_query_parameters as audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path,
                        help='optimized binary with debug symbols for source reports')
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--output-dir', required=True, type=Path)
    parser.add_argument('--iterations', type=int, default=1000000)
    parser.add_argument('--frequency', type=int, default=1999)
    parser.add_argument('--cpu', type=int)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not os.access(binary, os.X_OK) or min(args.iterations, args.frequency) < 1:
        parser.error('executable binary and positive iterations/frequency required')
    if not shutil.which('perf') or not hasattr(os, 'sched_getaffinity'):
        parser.error('Linux perf and CPU affinity support are required')
    available = os.sched_getaffinity(0)
    cpu = min(available) if args.cpu is None else args.cpu
    if cpu not in available:
        parser.error('CPU outside available affinity mask')
    os.sched_setaffinity(0, {cpu})
    root = args.output_dir.resolve()
    # Keep profiles and their inputs together; avoid overwriting earlier evidence.
    root.mkdir(parents=True, exist_ok=False)
    commit = subprocess.check_output(
        ['git', 'rev-parse', '--verify', '--end-of-options',
         f'{args.baseline_ref}^{{commit}}'], cwd=audit.REPO, text=True).strip()
    archive = subprocess.check_output(
        ['git', 'archive', commit, '--', 'packages/arel'], cwd=audit.REPO)
    driver = audit.timed_driver('arel_builder_chain').replace(
        'while i < 200000', f'while i < {args.iterations}')
    driver = driver.replace('started = Time.monotonic()',
                            'started = Time.monotonic()\nputs("loop_started=#{started}")')
    driver = driver.replace('puts("loop_seconds=#{Time.monotonic() - started}")',
                            'finished = Time.monotonic()\n'
                            'puts("loop_finished=#{finished}")\n'
                            'puts("loop_seconds=#{finished - started}")')
    metadata = {'commit': commit, 'binary': str(binary),
                'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
                'cpu': cpu, 'iterations': args.iterations, 'frequency': args.frequency,
                'clock': 'CLOCK_MONOTONIC', 'event': 'cycles:u',
                'control': 'Query constructor/copy annotations removed; other annotations retained',
                'timing': 'loop-only samples; profiled elapsed times are not benchmark evidence',
                'profiles': {}}
    for variant in ('untyped', 'typed'):
        directory = root / variant
        directory.mkdir()
        with tarfile.open(fileobj=io.BytesIO(archive)) as files:
            files.extractall(directory, filter='data')
        query = directory / 'packages/arel/lib/arel/query.di'
        source = query.read_text()
        if variant == 'untyped':
            source = audit.remove_known_annotations(source, 'initialize', audit.CONSTRUCTOR_TYPES)
            source = audit.remove_known_annotations(source, 'copy', audit.COPY_TYPES)
        query.write_text(source)
        (directory / 'bench').mkdir()
        (directory / 'bench/arel_builder_chain.di').write_text(driver)
        data = root / f'{variant}.perf.data'
        result = subprocess.run(
            ['perf', 'record', '-q', '-k', 'mono', '-F', str(args.frequency),
             '-e', 'cycles:u', '--call-graph', 'dwarf,4096', '-o', str(data), '--',
             str(binary), 'bench/arel_builder_chain.di'],
            cwd=directory, env=audit.environment(False), capture_output=True,
            text=True, check=True, timeout=300)
        (root / f'{variant}.stdout').write_text(result.stdout)
        (root / f'{variant}.record.log').write_text(result.stderr)
        if result.stdout.splitlines()[-2:] != [str(args.iterations), 'nil']:
            raise RuntimeError(f'{variant}: builder output changed')
        bounds = []
        for name in ('started', 'finished'):
            match = re.search(rf'^loop_{name}=([0-9]+\.[0-9]+)$', result.stdout, re.MULTILINE)
            if match is None:
                raise RuntimeError(f'{variant}: missing loop {name}')
            bounds.append(match.group(1))
        if float(bounds[1]) <= float(bounds[0]):
            raise RuntimeError('loop clock did not advance')
        for name, sort in (('symbols', 'symbol'), ('lines', 'srcline')):
            report = subprocess.run(
                ['perf', 'report', '--stdio', '--no-children', '--call-graph', 'none',
                 '--sort', sort, '--percent-limit', '0.3', '--time', ','.join(bounds),
                 '-i', str(data)], capture_output=True, text=True, check=True)
            (root / f'{variant}.{name}.txt').write_text(report.stdout)
            (root / f'{variant}.{name}.log').write_text(report.stderr)
        # Record matching workload diagnostics separately from perf sampling.
        traced = subprocess.run([str(binary), 'bench/arel_builder_chain.di'],
                                cwd=directory, env=audit.environment(False, True),
                                capture_output=True, text=True, check=True, timeout=300)
        if traced.stdout.splitlines()[-2:] != [str(args.iterations), 'nil']:
            raise RuntimeError('tracing changed builder output')
        (root / f'{variant}.trace.log').write_text(traced.stderr)
        metadata['profiles'][variant] = {
            'loop_bounds': bounds, 'diagnostic_counts': audit.summarize_diagnostics(traced.stderr)}
        (root / 'metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
        print(f'{variant}: reports saved in {root}', flush=True)


if __name__ == '__main__':
    main()
