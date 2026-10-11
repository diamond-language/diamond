#!/usr/bin/env python3
"""Compare two binaries against baseline and annotated Arel package snapshots."""

import argparse
import io
import json
import os
from pathlib import Path
import statistics
import subprocess
import tarfile
import tempfile
import time


REPO = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.check_output(["git", *args], cwd=REPO)


def snapshot(ref, destination, benchmark):
    commit = git("rev-parse", "--verify", "--end-of-options", f"{ref}^{{commit}}")
    commit = commit.decode().strip()
    archive = git("archive", commit, "--", "packages/arel")
    destination.mkdir()
    with tarfile.open(fileobj=io.BytesIO(archive)) as files:
        files.extractall(destination, filter="data")
    (destination / "bench").mkdir()
    (destination / "bench/arel_render_timed.di").write_text(benchmark)
    return commit


def environment(jit):
    # Do not inherit repeat, tracing, threshold, or quickening experiments.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith("DIAMOND_")}
    env["DIAMOND_NO_CACHE"] = "1"
    # DIAMOND_JIT is presence-based: even "0" enables the native backend.
    if jit == "1":
        env["DIAMOND_JIT"] = "1"
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-ref", required=True)
    parser.add_argument("--annotated-ref", default="HEAD")
    parser.add_argument("--baseline-binary", required=True, type=Path)
    parser.add_argument("--candidate-binary", default=REPO / "build/diamond", type=Path)
    parser.add_argument("--runs", default=5, type=int)
    parser.add_argument("--cpu", type=int, help="default: first available CPU on Linux")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    binaries = {"baseline": args.baseline_binary.resolve(),
                "candidate": args.candidate_binary.resolve()}
    for binary in binaries.values():
        if not os.access(binary, os.X_OK):
            parser.error(f"binary is not executable: {binary}")
    cpu = args.cpu
    if hasattr(os, "sched_getaffinity"):
        available = os.sched_getaffinity(0)
        cpu = min(available) if cpu is None else cpu
        if cpu not in available:
            parser.error(f"CPU {cpu} is outside the available affinity mask")
        os.sched_setaffinity(0, {cpu})
    elif cpu is not None:
        parser.error("--cpu requires CPU affinity support")

    source = (REPO / "bench/arel_render.di").read_text()
    loop = "j = 0\nwhile j < iterations"
    output = "puts(sql)"
    if source.count(loop) != 1 or source.count(output) != 1:
        raise RuntimeError("arel_render.di changed; update the timing insertion points")
    source = source.replace(loop, "started = Time.monotonic()\n" + loop)
    source = source.replace(output,
                            'puts("render_seconds=#{Time.monotonic() - started}")\n' + output)
    result = {"cpu": cpu, "binaries": {key: str(value) for key, value in binaries.items()},
              "runs": args.runs, "commits": {}, "modes": {}, "diagnostics": {}}
    expected_output = None
    with tempfile.TemporaryDirectory(prefix="diamond-arel-compare-") as temporary:
        packages = {name: Path(temporary) / name for name in ("baseline", "annotated")}
        for name, ref in (("baseline", args.baseline_ref), ("annotated", args.annotated_ref)):
            result["commits"][name] = snapshot(ref, packages[name], source)
        cases = [(binary, package) for binary in binaries for package in packages]
        for jit in ("0", "1"):
            samples = {f"{binary}/{package}": [] for binary, package in cases}
            for round_number in range(args.runs):
                order = cases if round_number % 2 == 0 else cases[::-1]
                for binary, package in order:
                    started = time.perf_counter()
                    run = subprocess.run([binaries[binary], "bench/arel_render_timed.di"],
                                         cwd=packages[package], env=environment(jit),
                                         capture_output=True, text=True, check=True, timeout=120)
                    wall = time.perf_counter() - started
                    timing, separator, sql_output = run.stdout.partition("\n")
                    if not separator or not timing.startswith("render_seconds="):
                        raise RuntimeError(f"missing render timing: {run.stdout}")
                    if expected_output is None:
                        expected_output = sql_output
                    if sql_output != expected_output:
                        raise RuntimeError(f"SQL/parameter output differs for {binary}/{package}")
                    samples[f"{binary}/{package}"].append(
                        {"wall": wall, "render": float(timing.split("=", 1)[1])})
            medians = {key: {metric: statistics.median(sample[metric] for sample in values)
                             for metric in ("wall", "render")}
                       for key, values in samples.items()}
            result["modes"][jit] = {"jit_enabled": jit == "1",
                                    "samples": samples, "medians": medians}
            setting = "unset" if jit == "0" else "1"
            print(f"DIAMOND_JIT {setting}: {json.dumps(medians)}", flush=True)
        # Trace separately: instrumentation can change timings and heap layout.
        for binary, package in cases:
            env = environment("0")
            env.update(DIAMOND_TRACE_IC="1", DIAMOND_TRACE_FIELDS="1", DIAMOND_TRACE_JIT="1",
                       DIAMOND_TRACE_IC_REWRITES="1", DIAMOND_TRACE_OPCODES="1")
            run = subprocess.run([binaries[binary], "bench/arel_render_timed.di"],
                                 cwd=packages[package], env=env, capture_output=True,
                                 text=True, check=True, timeout=120)
            if run.stdout.partition("\n")[2] != expected_output:
                raise RuntimeError(f"trace output differs for {binary}/{package}")
            result["diagnostics"][f"{binary}/{package}"] = run.stderr
    result["sql_output"] = expected_output
    args.output.write_text(json.dumps(result, indent=2) + "\n")


if __name__ == "__main__":
    main()
