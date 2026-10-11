#!/usr/bin/env python3
"""Isolate Query constructor/copy guards from other Arel annotation changes."""

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import re
import statistics
import subprocess
import tarfile
import tempfile

REPO = Path(__file__).resolve().parent.parent
CONSTRUCTOR_TYPES = {
    'table_name': 'String', 'predicates': 'Array', 'orderings': 'Array',
    'limit_value': 'Int | Nil', 'offset_value': 'Int | Nil',
    'projections': 'Array', 'quoted_identifiers': 'Bool', 'bind_limits': 'Bool',
    'table_alias': 'String | Nil', 'distinct_value': 'Bool', 'groups': 'Array',
    'havings': 'Array', 'joins': 'Array', 'correlations': 'Array', 'ctes': 'Array',
}
COPY_TYPES = {name: CONSTRUCTOR_TYPES[name] for name in
              ('predicates', 'orderings', 'limit_value', 'offset_value', 'projections')}


def annotate(source, method, types):
    start = source.index(f'    def {method}(')
    end = source.index(')', start) + 1
    signature = source[start:end]
    for name, kind in types.items():
        signature, count = re.subn(r'\b' + name + r'(?=,| =|\))',
                                   f'{name}: {kind}', signature)
        if count != 1:
            raise RuntimeError(f'{method}/{name}: expected one untyped parameter')
    return source[:start] + signature + source[end:]


def environment(jit, tracing=False):
    env = {k: v for k, v in os.environ.items() if not k.startswith('DIAMOND_')}
    env['DIAMOND_NO_CACHE'] = '1'
    if jit:
        env['DIAMOND_JIT'] = '1'
    if tracing:
        for name in ('IC', 'FIELDS', 'OPCODES', 'JIT', 'IC_REWRITES'):
            env[f'DIAMOND_TRACE_{name}'] = '1'
    return env


def timed_driver(name):
    source = (REPO / f'bench/{name}.di').read_text()
    loop, output = (('j = 0\nwhile j < iterations', 'puts(sql)') if name == 'arel_render'
                    else ('while i < 200000', 'puts(count)'))
    if source.count(loop) != 1 or source.count(output) != 1:
        raise RuntimeError(f'{name}: timing insertion points changed')
    source = source.replace(loop, 'started = Time.monotonic()\n' + loop)
    return source.replace(output, 'puts("loop_seconds=#{Time.monotonic() - started}")\n' + output)


def run(binary, directory, bench, jit, tracing=False):
    result = subprocess.run([str(binary), f'bench/{bench}.di'], cwd=directory,
                            env=environment(jit, tracing), capture_output=True,
                            text=True, check=True, timeout=120)
    timing, _, output = result.stdout.partition('\n')
    if not timing.startswith('loop_seconds='):
        raise RuntimeError(f'missing loop timing: {result.stdout}')
    return float(timing.split('=', 1)[1]), output, result.stderr


def probe_default_allocations(binary, directory):
    probes = {}
    for kind, default in (('Array', '[]'), ('Hash', '{}')):
        source = (f'class DefaultProbe\n'
                  f'  def initialize(values: {kind} = {default})\n'
                  '    @values = values\n  end\nend\n'
                  'i = 0\nwhile i < 100\n'
                  '  probe = DefaultProbe.new()\n  i += 1\nend\nputs(i)\n')
        path = directory / f'{kind.lower()}_default_probe.di'
        path.write_text(source)
        env = environment(True)
        env.update(DIAMOND_JIT_THRESHOLD='1', DIAMOND_TRACE_JIT='1')
        result = subprocess.run([str(binary), str(path)], env=env, capture_output=True,
                                text=True, check=True, timeout=120)
        if result.stdout != '100\nnil\n':
            raise RuntimeError(f'{kind} default probe output changed')
        probes[kind] = {'source': source, 'diagnostic': result.stderr}
    return probes


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', default='HEAD')
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--cpu', type=int)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve()
    if args.runs < 1 or not os.access(binary, os.X_OK):
        parser.error('positive runs and an executable binary are required')
    cpu = args.cpu
    if hasattr(os, 'sched_getaffinity'):
        available = os.sched_getaffinity(0)
        cpu = min(available) if cpu is None else cpu
        if cpu not in available:
            parser.error('CPU outside available affinity mask')
        os.sched_setaffinity(0, {cpu})
    elif cpu is not None:
        parser.error('--cpu requires affinity support')
    commit = subprocess.check_output(['git', 'rev-parse', '--verify', '--end-of-options',
                                      f'{args.baseline_ref}^{{commit}}'], cwd=REPO).decode().strip()
    archive = subprocess.check_output(['git', 'archive', commit, '--', 'packages/arel'], cwd=REPO)
    results = {'baseline_commit': commit, 'binary': str(binary),
               'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(), 'cpu': cpu,
               'runs': args.runs, 'timing': 'benchmark loop only',
               'constructor_parameter_types': CONSTRUCTOR_TYPES,
               'copy_parameter_types': COPY_TYPES, 'benchmarks': {}}
    variants = ('baseline', 'constructor', 'copy', 'both')
    with tempfile.TemporaryDirectory(prefix='diamond-query-guards-') as temporary:
        directories = {}
        for variant in variants:
            directory = Path(temporary) / variant
            directory.mkdir()
            with tarfile.open(fileobj=io.BytesIO(archive)) as files:
                files.extractall(directory, filter='data')
            query = directory / 'packages/arel/lib/arel/query.di'
            source = query.read_text()
            if variant in ('constructor', 'both'):
                source = annotate(source, 'initialize', CONSTRUCTOR_TYPES)
            if variant in ('copy', 'both'):
                source = annotate(source, 'copy', COPY_TYPES)
            query.write_text(source)
            (directory / 'bench').mkdir()
            for bench in ('arel_render', 'arel_builder_chain'):
                (directory / f'bench/{bench}.di').write_text(timed_driver(bench))
            directories[variant] = directory
        for bench in ('arel_render', 'arel_builder_chain'):
            expected = None
            results['benchmarks'][bench] = {}
            for jit in (False, True):
                samples = {variant: [] for variant in variants}
                # Rotate positions, then reverse alternate rounds to limit ordering bias.
                for round_number in range(args.runs):
                    shift = round_number % len(variants)
                    order = variants[shift:] + variants[:shift]
                    if round_number % 2:
                        order = order[::-1]
                    for variant in order:
                        elapsed, output, _ = run(binary, directories[variant], bench, jit)
                        if expected is None:
                            expected = output
                        if output != expected:
                            raise RuntimeError(f'{bench}/{variant}: output changed')
                        samples[variant].append(elapsed)
                medians = {v: statistics.median(s) for v, s in samples.items()}
                diagnostics = {}
                for variant in variants:
                    _, output, diagnostic = run(binary, directories[variant], bench, jit, True)
                    if output != expected:
                        raise RuntimeError('tracing changed benchmark output')
                    diagnostics[variant] = diagnostic
                mode = 'jit' if jit else 'interpreter'
                summary = {'samples': samples, 'medians': medians,
                           'change_percent': {v: 100 * (m / medians['baseline'] - 1)
                                              for v, m in medians.items()},
                           'diagnostics': diagnostics}
                results['benchmarks'][bench][mode] = summary
                args.output.write_text(json.dumps(results, indent=2) + '\n')
                print(bench, mode, json.dumps({k: summary[k] for k in
                                              ('medians', 'change_percent')}), flush=True)
        results['default_allocation_probes'] = probe_default_allocations(binary, Path(temporary))
    args.output.write_text(json.dumps(results, indent=2) + '\n')


if __name__ == '__main__':
    main()
