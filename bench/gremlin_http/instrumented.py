#!/usr/bin/env python3
"""Instrumented gremlin runs for docs/internal/gremlin-scaling-investigation.md.

Like sweep.py, but each measured window also samples what the machine is
doing: CPU frequency of the cores the server may use, user/system CPU split of
the server process, context switches, and per-thread CPU balance. Server and
load generator can be pinned to disjoint CPU sets so they stop competing.

Usage: instrumented.py --threads 2,6,12 [--server-cpus 0-9] [--ab-cpus 10,11]
                       [--workload mixed|hello|cpu] [--rounds 2] [--measure 8]
"""
import argparse
import os
import statistics
import subprocess
import tempfile
import time
from pathlib import Path

import sweep

WORKLOADS = {
    "mixed": None,  # sweep.SERVER_SOURCE
    "hello": """\
require_cut "gremlin"

def handler(request, context)
  [200, {{"Content-Type": "text/plain"}}, "hello"]
end

gremlin_serve({port}, handler, threads: {threads})
""",
    "cpu": """\
require_cut "gremlin"

def handler(request, context)
  sum = 0
  i = 0
  while i < 20000
    sum = sum + (i * i) % 7
    i = i + 1
  end
  [200, {{"Content-Type": "text/plain"}}, "#{{sum}}"]
end

gremlin_serve({port}, handler, threads: {threads})
""",
}


def parse_cpus(spec):
    cpus = []
    for part in spec.split(","):
        if "-" in part:
            low, high = part.split("-")
            cpus.extend(range(int(low), int(high) + 1))
        else:
            cpus.append(int(part))
    return cpus


def read_freqs(cpus):
    out = []
    for cpu in cpus:
        try:
            out.append(int(Path(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_cur_freq").read_text()) / 1000)
        except OSError:
            pass
    return out


def task_stats(pid):
    """Per-thread (utime, stime) in clock ticks, summed context switches."""
    threads = {}
    voluntary = involuntary = 0
    for task in Path(f"/proc/{pid}/task").iterdir():
        try:
            stat = (task / "stat").read_text().rsplit(")", 1)[1].split()
            threads[task.name] = (int(stat[11]), int(stat[12]))
            for line in (task / "status").read_text().splitlines():
                if line.startswith("voluntary_ctxt_switches"):
                    voluntary += int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches"):
                    involuntary += int(line.split()[1])
        except (OSError, IndexError):
            pass
    return threads, voluntary, involuntary


def rss_mb(pid):
    for line in Path(f"/proc/{pid}/status").read_text().splitlines():
        if line.startswith("VmRSS"):
            return int(line.split()[1]) / 1024
    return 0.0


def measure(diamond, project, threads, port, args):
    source = (WORKLOADS[args.workload] or sweep.SERVER_SOURCE).format(port=port, threads=threads)
    command = [diamond, "-e", source]
    if args.server_cpus:
        command = ["taskset", "-c", args.server_cpus] + command
    server = subprocess.Popen(command, cwd=project, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not sweep.wait_for_port(port):
            raise RuntimeError("server failed to start")
        # taskset execs diamond, so server.pid is the diamond process.
        pid = server.pid
        ab_prefix = ["taskset", "-c", args.ab_cpus] if args.ab_cpus else []
        original = sweep.run_ab

        def pinned(port_, seconds, concurrency):
            import re
            result = subprocess.run(
                ab_prefix + ["ab", "-q", "-t", str(seconds), "-n", "100000000", "-c", str(concurrency),
                             f"http://127.0.0.1:{port_}/bench"], capture_output=True, text=True, timeout=seconds + 60)
            rps = re.search(r"Requests per second:\s+([\d.]+)", result.stdout)
            failed = re.search(r"Failed requests:\s+(\d+)", result.stdout)
            p99 = re.search(r"^\s*99%\s+(\d+)", result.stdout, re.M)
            return float(rps.group(1)), int(failed.group(1)) if failed else 0, float(p99.group(1)) if p99 else float("nan")

        sweep.run_ab = pinned
        try:
            sweep.load(port, args.warmup, args.clients, args.concurrency)
            cpus = parse_cpus(args.server_cpus) if args.server_cpus else list(range(os.cpu_count()))
            before, vol0, invol0 = task_stats(pid)
            rss0 = rss_mb(pid)
            freqs = []
            started = time.monotonic()

            import threading
            result = {}
            worker = threading.Thread(target=lambda: result.update(
                r=sweep.load(port, args.measure, args.clients, args.concurrency)))
            worker.start()
            while worker.is_alive():
                time.sleep(1)
                top = sorted(read_freqs(cpus), reverse=True)[:min(threads, len(cpus))]
                freqs.append(statistics.mean(top))
            worker.join()
            elapsed = time.monotonic() - started
            after, vol1, invol1 = task_stats(pid)
            rss1 = rss_mb(pid)
        finally:
            sweep.run_ab = original
        ticks = os.sysconf("SC_CLK_TCK")
        user = sum(after[t][0] - before.get(t, (0, 0))[0] for t in after) / ticks
        system = sum(after[t][1] - before.get(t, (0, 0))[1] for t in after) / ticks
        per_thread = sorted(
            ((after[t][0] + after[t][1]) - sum(before.get(t, (0, 0)))) / ticks for t in after)
        busy = [x for x in per_thread if x > 0.05 * elapsed]
        rps, failed, p99 = result["r"]
        return {
            "rps": rps, "p99": p99, "failed": failed,
            "user_cpu_pct": 100 * user / elapsed, "sys_cpu_pct": 100 * system / elapsed,
            "ctx_vol_per_req": (vol1 - vol0) / (rps * elapsed), "ctx_invol_per_req": (invol1 - invol0) / (rps * elapsed),
            "mean_mhz": statistics.mean(freqs) if freqs else float("nan"),
            "busy_threads": len(busy),
            "thread_cpu_spread": (min(busy) / max(busy)) if busy else float("nan"),
            "rss_mb": rss1, "rss_growth_mb": rss1 - rss0,
            "cpu_ms_per_req": 1000 * (user + system) / (rps * elapsed),
        }
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--threads", default="2,6,12")
    parser.add_argument("--rounds", type=int, default=2)
    parser.add_argument("--warmup", type=int, default=4)
    parser.add_argument("--measure", type=int, default=8)
    parser.add_argument("--clients", type=int, default=3)
    parser.add_argument("--concurrency", type=int, default=32)
    parser.add_argument("--server-cpus", default="")
    parser.add_argument("--ab-cpus", default="")
    parser.add_argument("--workload", default="mixed", choices=sorted(WORKLOADS))
    args = parser.parse_args()
    diamond = os.environ.get("DIAMOND_BIN", str((sweep.HERE / "../../build/diamond").resolve()))
    project = Path(tempfile.mkdtemp(prefix="gremlin-instr-"))
    sweep.make_project(project)
    counts = [int(n) for n in args.threads.split(",")]
    print(f"workload={args.workload} server-cpus={args.server_cpus or 'any'} ab-cpus={args.ab_cpus or 'any'} "
          f"clients={args.clients}x{args.concurrency}", flush=True)
    rows = {n: [] for n in counts}
    port = 19800
    for round_number in range(args.rounds):
        shift = round_number % len(counts)
        for threads in counts[shift:] + counts[:shift]:
            rows[threads].append(measure(diamond, project, threads, port, args))
            port += 1
    keys = ["rps", "p99", "cpu_ms_per_req", "user_cpu_pct", "sys_cpu_pct", "mean_mhz",
            "ctx_vol_per_req", "ctx_invol_per_req", "busy_threads", "thread_cpu_spread",
            "rss_mb", "rss_growth_mb", "failed"]
    print("threads | " + " | ".join(f"{k:>17}" for k in keys))
    for threads in counts:
        means = {k: statistics.mean(r[k] for r in rows[threads]) for k in keys}
        print(f"{threads:>7} | " + " | ".join(f"{means[k]:>17.2f}" for k in keys))


if __name__ == "__main__":
    main()
