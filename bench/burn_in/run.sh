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

# ab's own `-t` flag silently implies `-n 50000` internally (see `man
# ab`) regardless of any larger `-n` given explicitly alongside it --
# confirmed empirically: `-n 100000000 -t 180` still stops dead at
# exactly 50000 requests, in ~6s, not 180s. Relying on `-t` here would
# mean each "batch" is really a few seconds of real load followed by the
# server sitting idle for the rest of batch_seconds while this script's
# own elapsed counter keeps ticking -- exactly the kind of thing this
# harness exists to catch, just aimed at itself. So: no `-t` anywhere
# below. Instead, calibrate actual throughput with a small warmup batch,
# size every real batch's `-n` to target roughly batch_seconds of
# genuine load, and let each batch run to natural completion -- then
# track total elapsed time from ab's own reported "Time taken for
# tests", not an assumed constant.
calibration_out="$(mktemp)"
ab -q -l -n 2000 -c "$concurrency" "http://127.0.0.1:$port/calibrate" \
    >"$calibration_out" 2>&1 || true
calibrated_rps="$(awk '/Requests per second/ {print $4}' "$calibration_out")"
rm -f "$calibration_out"
# Integer floor via awk; 200 req/s floor keeps a batch from ballooning
# to an absurd request count if calibration ever reads oddly low.
requests_per_batch="$(awk -v r="${calibrated_rps:-200}" -v s="$batch_seconds" \
    'BEGIN { n = r * s; if (n < 1000) n = 1000; printf "%d", n }')"

echo "burn-in: pid=$server_pid port=$port duration=${duration_seconds}s concurrency=$concurrency"
echo "burn-in: calibrated ~${calibrated_rps:-?} req/s -> ${requests_per_batch} requests/batch (target ~${batch_seconds}s/batch)"
printf '%-6s %10s %14s %10s %10s %10s %8s %10s\n' \
    batch rss_kb req_per_sec mean_ms p95_ms p99_ms failed batch_s

batch=0
elapsed=0
start_rss="$(rss_kb "$server_pid")"
declare -a rss_series=()
declare -a p99_series=()

while (( elapsed < duration_seconds )); do
    batch=$((batch + 1))
    ab_out="$(mktemp)"
    # -l (ignore response-length variation) because the session-cache
    # handler's response body legitimately grows (hits=/sessions_live=)
    # request to request -- without it ab flags nearly every response as
    # "failed" for a length mismatch that isn't a real error.
    ab -q -l -n "$requests_per_batch" -c "$concurrency" \
        "http://127.0.0.1:$port/burn/$batch" >"$ab_out" 2>&1 || true

    req_per_sec="$(awk '/Requests per second/ {print $4}' "$ab_out")"
    mean_ms="$(awk '/Time per request/ && /mean\)$/ {print $4}' "$ab_out" | head -1)"
    p95_ms="$(awk '/ 95%/ {print $2}' "$ab_out")"
    p99_ms="$(awk '/ 99%/ {print $2}' "$ab_out")"
    failed="$(awk '/Failed requests/ {print $3}' "$ab_out")"
    batch_seconds_actual="$(awk '/Time taken for tests/ {print $5}' "$ab_out")"
    current_rss="$(rss_kb "$server_pid")"
    rss_series+=("$current_rss")
    p99_series+=("${p99_ms:-0}")

    printf '%-6d %10s %14s %10s %10s %10s %8s %10s\n' \
        "$batch" "$current_rss" "${req_per_sec:-?}" "${mean_ms:-?}" \
        "${p95_ms:-?}" "${p99_ms:-?}" "${failed:-?}" "${batch_seconds_actual:-?}"

    rm -f "$ab_out"
    # Truncate to whole seconds for the loop condition -- fine-grained
    # enough given batches are on the order of several seconds each.
    batch_elapsed_int="$(awk -v t="${batch_seconds_actual:-$batch_seconds}" 'BEGIN { printf "%d", t }')"
    if (( batch_elapsed_int < 1 )); then batch_elapsed_int=1; fi
    elapsed=$((elapsed + batch_elapsed_int))
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
