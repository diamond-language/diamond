#!/usr/bin/env bash
set -euo pipefail

# HTTP throughput/latency benchmark for gremlin_serve's `threads: N`
# parallelism (see packages/gremlin/gremlin.di) -- one SO_REUSEPORT
# listener per worker thread, each its own independent DiamondVm/heap.
# Two workloads:
#   - hello: a fixed tiny response, no per-request compute -- measures
#     raw request-handling overhead (HTTP parse/write, fiber scheduling,
#     syscalls), not application logic.
#   - cpu: a ~3ms-per-request busy loop -- CPU-bound work fibers can't
#     help with (only I/O-bound waits yield), so this is the workload
#     that actually shows whether `threads: N` buys real multi-core
#     parallelism versus a single worker.
# Requires: `make release` build (this script rebuilds it) and Apache
# Bench (`ab`, `dnf install httpd-tools` on Fedora) -- no wrk/hey
# available in this environment.

diamond="${DIAMOND_BIN:-$(cd "$(dirname "$0")/../.." && pwd)/build/diamond}"
cd "$(dirname "$0")"

wait_for_port() {
    local port="$1"
    { for _ in $(seq 1 100); do
        if exec 3<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done } 2>/dev/null
    return 1
}

hello_src() {
    local port="$1" threads="$2"
    cat <<SRCEOF
require "$(cd ../../packages/gremlin && pwd)/gremlin"

def handler(request, context)
  [200, {"Content-Type": "text/plain"}, "hello"]
end

gremlin_serve($port, handler, threads: $threads)
SRCEOF
}

cpu_src() {
    local port="$1" threads="$2"
    cat <<SRCEOF
require "$(cd ../../packages/gremlin && pwd)/gremlin"

def handler(request, context)
  sum = 0
  i = 0
  while i < 50000
    sum = sum + i
    i = i + 1
  end
  [200, {"Content-Type": "text/plain"}, "#{sum}"]
end

gremlin_serve($port, handler, threads: $threads)
SRCEOF
}

# Runs one (workload, thread-count) trial: start the server, hammer it
# with ab, parse out requests/sec and mean/p99 latency, tear it down.
run_trial() {
    local workload="$1" threads="$2" port="$3" requests="$4" concurrency="$5"
    local src_fn="${workload}_src"
    local out
    out="$(mktemp)"
    "$diamond" -e "$($src_fn "$port" "$threads")" >/dev/null 2>&1 &
    local pid=$!
    if ! wait_for_port "$port"; then
        echo "  ($workload, threads=$threads) FAILED TO START"
        kill "$pid" 2>/dev/null || true
        return
    fi
    { exec 3<&- 3>&-; } 2>/dev/null || true
    ab -q -n "$requests" -c "$concurrency" "http://127.0.0.1:$port/" >"$out" 2>&1 || true
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    local rps mean p99 failed
    rps=$(grep "Requests per second" "$out" | awk '{print $4}')
    mean=$(grep "Time per request" "$out" | head -1 | awk '{print $4}')
    p99=$(grep " 99%" "$out" | awk '{print $2}')
    failed=$(grep "Failed requests" "$out" | awk '{print $3}')
    printf "%-6s | %7s | %10s | %12s | %10s | %8s\n" \
        "$workload" "$threads" "${rps:-?}" "${mean:-?}ms" "${p99:-?}ms" "${failed:-?}"
    rm -f "$out"
}

echo "workload | threads | req/sec   | mean latency | p99 latency | failed"
echo "---------|---------|-----------|--------------|-------------|-------"
port=19500
for threads in 1 2 4 6 12; do
    run_trial hello "$threads" "$port" 20000 100
    port=$((port + 1))
done
for threads in 1 2 4 6 12; do
    run_trial cpu "$threads" "$port" 3000 50
    port=$((port + 1))
done
