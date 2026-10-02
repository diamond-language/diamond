#!/usr/bin/env python3
"""Thread-count sweep for gremlin_serve under a small, mixed-work endpoint.

For each worker-thread count the script starts a fresh server, warms it up,
then measures steady-state throughput. Rounds are interleaved (every thread
count once per round, in rotating order) so slow drift on the host (thermal
state, other processes) spreads across all configurations instead of
favouring whichever ran first. Results are averaged over the rounds.

The load generator is several concurrent Apache Bench processes: one `ab` is
single-threaded and would cap out before the server does. gremlin has no
keep-alive, so `ab`'s default of one connection per request matches how the
server is actually used. The generator shares the machine with the server, so
at high thread counts they compete for cores; that is part of what this
measures and is called out in the output.

Usage: sweep.py [--threads 2,4,6,8,10,12] [--rounds 5] [--warmup 5]
                [--measure 10] [--clients 3] [--concurrency 32]
Environment: DIAMOND_BIN (default: ../../build/diamond, a `make release` build).
"""
import argparse
import os
import re
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
PACKAGES = (HERE / "../../packages").resolve()

# A little bit of work per request, deliberately not a pure busy loop: some
# integer arithmetic, string building, hash construction, and JSON encoding,
# i.e. the shape of a small API handler. Roughly a few hundred microseconds.
SERVER_SOURCE = """\
require_cut "gremlin"

def handler(request, context)
  sum = 0
  i = 0
  while i < 2000
    sum = sum + (i * i) % 7
    i = i + 1
  end
  items = []
  j = 0
  while j < 8
    items.push({{"id": j, "label": "item-#{{j}}", "score": sum + j}})
    j = j + 1
  end
  body = JSON.stringify({{"path": request["path"], "total": sum, "items": items}})
  [200, {{"Content-Type": "application/json"}}, body]
end

gremlin_serve({port}, handler, threads: {threads})
"""


def wait_for_port(port, timeout=10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        with socket.socket() as sock:
            sock.settimeout(0.2)
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.05)
    return False


def run_ab(port, seconds, concurrency):
    """One ab process for `seconds`; returns (requests/sec, failed, p99 ms)."""
    result = subprocess.run(
        ["ab", "-q", "-t", str(seconds), "-n", "100000000", "-c", str(concurrency),
         f"http://127.0.0.1:{port}/bench"],
        capture_output=True, text=True, timeout=seconds + 60)
    text = result.stdout
    rps = re.search(r"Requests per second:\s+([\d.]+)", text)
    failed = re.search(r"Failed requests:\s+(\d+)", text)
    p99 = re.search(r"^\s*99%\s+(\d+)", text, re.M)
    if not rps:
        raise RuntimeError("ab produced no result:\n" + text + result.stderr)
    return float(rps.group(1)), int(failed.group(1)) if failed else 0, float(p99.group(1)) if p99 else float("nan")


def load(port, seconds, clients, concurrency):
    with ThreadPoolExecutor(max_workers=clients) as pool:
        results = list(pool.map(lambda _: run_ab(port, seconds, concurrency), range(clients)))
    return (sum(r[0] for r in results), sum(r[1] for r in results), max(r[2] for r in results))


def make_project(root):
    """A throwaway project whose cuts/ links the in-repo gremlin, http and logger."""
    (root / "cuts").mkdir()
    for name in ("gremlin", "http", "logger"):
        (root / "cuts" / name).symlink_to(PACKAGES / name)


def trial(diamond, project, threads, port, args):
    source = SERVER_SOURCE.format(port=port, threads=threads)
    server = subprocess.Popen([diamond, "-e", source], cwd=project, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    try:
        if not wait_for_port(port):
            raise RuntimeError(f"server with {threads} threads failed to start")
        load(port, args.warmup, args.clients, args.concurrency)  # discarded
        return load(port, args.measure, args.clients, args.concurrency)
    finally:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--threads", default="2,4,6,8,10,12")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--warmup", type=int, default=5)
    parser.add_argument("--measure", type=int, default=10)
    parser.add_argument("--clients", type=int, default=3, help="concurrent ab processes")
    parser.add_argument("--concurrency", type=int, default=32, help="connections per ab process")
    args = parser.parse_args()
    diamond = os.environ.get("DIAMOND_BIN", str((HERE / "../../build/diamond").resolve()))
    counts = [int(n) for n in args.threads.split(",")]
    samples = {n: [] for n in counts}
    failures = {n: 0 for n in counts}
    p99s = {n: [] for n in counts}

    version = subprocess.run([diamond, "--version"], capture_output=True, text=True).stdout.strip()
    print(f"{version}; {os.cpu_count()} logical CPUs; ab x{args.clients} at -c {args.concurrency} "
          f"({args.clients * args.concurrency} connections total)")
    print(f"{args.rounds} rounds x {len(counts)} thread counts, {args.warmup}s warmup + "
          f"{args.measure}s measured each\n", flush=True)

    project = Path(tempfile.mkdtemp(prefix="gremlin-sweep-"))
    make_project(project)
    port = 19700
    for round_number in range(args.rounds):
        # Rotate the starting point so no thread count is always first/last.
        order = counts[round_number % len(counts):] + counts[:round_number % len(counts)]
        for threads in order:
            rps, failed, p99 = trial(diamond, project, threads, port, args)
            port += 1
            samples[threads].append(rps)
            p99s[threads].append(p99)
            failures[threads] += failed
            print(f"round {round_number + 1} threads={threads:>2}: {rps:9.0f} req/s "
                  f"(p99 {p99:.0f} ms, {failed} failed)", flush=True)

    print("\nthreads |  mean req/s |  stdev | stdev% |    min |    max | mean p99 ms | failed")
    print("--------|-------------|--------|--------|--------|--------|-------------|-------")
    best = max(counts, key=lambda n: statistics.mean(samples[n]))
    for threads in counts:
        values = samples[threads]
        mean = statistics.mean(values)
        stdev = statistics.stdev(values) if len(values) > 1 else 0.0
        marker = "  <- peak" if threads == best else ""
        print(f"{threads:>7} | {mean:11.0f} | {stdev:6.0f} | {100 * stdev / mean:5.1f}% | "
              f"{min(values):6.0f} | {max(values):6.0f} | {statistics.mean(p99s[threads]):11.0f} | "
              f"{failures[threads]:>6}{marker}")


if __name__ == "__main__":
    sys.exit(main())
