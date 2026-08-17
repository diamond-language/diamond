#!/usr/bin/env bash
# Sustained-load burn-in for gremlin_serve, multi-threaded (server.di runs
# threads: 4). Establishes the baseline docs/roadmap.md's "Generational or
# incremental GC" entry calls for before any collector redesign: does RSS
# and tail latency stay flat under a long, steady request churn, or do
# they visibly climb as the live heap grows? See README.md in this
# directory for how to read the output.
#
# Usage: bash bench/burn_in/run.sh [duration_seconds] [concurrency]
#   duration_seconds: total sustained-load time, default 300 (5 min)
#   concurrency:       ab -c value, default 20
#
# Requires: `ab` (apache2-utils) on PATH, and build/diamond already built
# (release build recommended -- `make release`).
set -euo pipefail
cd "$(dirname "$0")/../.."

diamond="${DIAMOND_BIN:-./build/diamond}"
if [[ ! -x "$diamond" ]]; then
    echo "diamond binary not found at $diamond -- run 'make release' first" >&2
    exit 1
fi
if ! command -v ab >/dev/null 2>&1; then
    echo "'ab' (apache2-utils) not found on PATH" >&2
    exit 1
fi

duration_seconds="${1:-300}"
concurrency="${2:-20}"
batch_seconds=15
port=19420

wait_for_port() {
    local port="$1"
    # Bash's own /dev/tcp connect-refused message can leak to stderr
    # past a single command's own redirect (see packages/gremlin/test.sh's
    # identical wrapper) -- the outer brace group is belt-and-suspenders.
    { for _ in $(seq 1 100); do
        if exec 3<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            { exec 3<&- 3>&-; } 2>/dev/null || true
            return 0
        fi
        sleep 0.05
    done } 2>/dev/null
    return 1
}

rss_kb() {
    # VmRSS covers every thread in the process, so this is the whole
    # server's memory (all `threads: 4` workers' heaps combined), not
    # just the main thread's.
    awk '/VmRSS/ {print $2}' "/proc/$1/status" 2>/dev/null || echo "?"
}

server_log="$(mktemp)"
"$diamond" "bench/burn_in/server.di" >"$server_log" 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true' EXIT

if ! wait_for_port "$port"; then
    echo "server never came up -- log:" >&2
    cat "$server_log" >&2
    exit 1
fi

echo "burn-in: pid=$server_pid port=$port duration=${duration_seconds}s concurrency=$concurrency batch=${batch_seconds}s"
printf '%-6s %10s %14s %10s %10s %10s\n' \
    batch rss_kb req_per_sec mean_ms p95_ms p99_ms

batch=0
elapsed=0
start_rss="$(rss_kb "$server_pid")"
declare -a rss_series=()
declare -a p99_series=()

while (( elapsed < duration_seconds )); do
    batch=$((batch + 1))
    ab_out="$(mktemp)"
    # -n large, -t caps by wall time -- the actual duration knob.
    ab -q -n 100000000 -c "$concurrency" -t "$batch_seconds" \
        "http://127.0.0.1:$port/burn/$batch" >"$ab_out" 2>&1 || true

    req_per_sec="$(awk '/Requests per second/ {print $4}' "$ab_out")"
    mean_ms="$(awk '/Time per request/ && /mean\)$/ {print $4}' "$ab_out" | head -1)"
    p95_ms="$(awk '/ 95%/ {print $2}' "$ab_out")"
    p99_ms="$(awk '/ 99%/ {print $2}' "$ab_out")"
    current_rss="$(rss_kb "$server_pid")"
    rss_series+=("$current_rss")
    p99_series+=("${p99_ms:-0}")

    printf '%-6d %10s %14s %10s %10s %10s\n' \
        "$batch" "$current_rss" "${req_per_sec:-?}" "${mean_ms:-?}" \
        "${p95_ms:-?}" "${p99_ms:-?}"

    rm -f "$ab_out"
    elapsed=$((elapsed + batch_seconds))
done

end_rss="$(rss_kb "$server_pid")"
kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
trap - EXIT
rm -f "$server_log"

echo
echo "== summary =="
echo "RSS: start=${start_rss}KB end=${end_rss}KB over $batch batches"

# Trend check: compare the first and last few batches so a slow climb is
# visible even though this is a coarse, unscientific signal -- exact
# interpretation belongs in README.md, not baked into this script.
count=${#rss_series[@]}
if (( count >= 6 )); then
    window=3
    first_sum=0
    last_sum=0
    for ((i = 0; i < window; i++)); do first_sum=$((first_sum + rss_series[i])); done
    for ((i = count - window; i < count; i++)); do last_sum=$((last_sum + rss_series[i])); done
    first_avg=$((first_sum / window))
    last_avg=$((last_sum / window))
    echo "RSS: first ${window}-batch avg=${first_avg}KB, last ${window}-batch avg=${last_avg}KB"
fi
