#!/usr/bin/env bash
# A modest, actively-safety-capped push past run.sh's own baseline (see
# server_hard.di: max_sessions=30000/worker vs server.di's 20000). Unlike
# run.sh, this carries its own RSS watchdog polling every 1s independent
# of ab's batch boundaries, and kill -9's the server the instant RSS
# crosses RSS_HARD_CAP_KB -- run.sh's own per-batch RSS check only looks
# once per ~15s batch, which isn't tight enough to be a real safety net
# on a single-machine setup with no spare hardware. See this project's
# "Bound resource experiments" lesson: a previous, unsupervised run of
# this same idea (60000 sessions/worker, no active watchdog) consumed
# all RAM and most of swap and crashed the machine before anyone could
# react to a bad number. This script is built specifically so that
# can't happen again: the cap below is a hard ceiling chosen with real
# margin under currently-free memory (checked via `free` immediately
# before picking it), not a "try big, back off if it looks bad" guess.
#
# Usage: bash bench/burn_in/run_hard.sh [duration_seconds] [concurrency]
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
server_script="${3:-bench/burn_in/server_hard.di}"
batch_seconds=15
port="${BURN_IN_PORT:-19421}"
# Hard ceiling, checked every watchdog tick, independent of batch
# boundaries. Chosen with real margin under `free`'s currently-free
# total at launch time (see the check right below) -- not a guess.
rss_hard_cap_kb="${BURN_IN_RSS_CAP_KB:-5500000}"
watchdog_interval_s=1

free_kb="$(awk '/^Mem:/ {print $4}' <(free))"
if (( free_kb < rss_hard_cap_kb + 2000000 )); then
    echo "aborting: only ${free_kb}KB free, need >$((rss_hard_cap_kb + 2000000))KB headroom for a ${rss_hard_cap_kb}KB cap to be safe" >&2
    exit 1
fi
echo "burn-in (hard): free=${free_kb}KB at launch, rss_hard_cap=${rss_hard_cap_kb}KB, watchdog every ${watchdog_interval_s}s"

wait_for_port() {
    local port="$1"
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
    awk '/VmRSS/ {print $2}' "/proc/$1/status" 2>/dev/null || echo 0
}

server_log="$(mktemp)"
"$diamond" "$server_script" >"$server_log" 2>&1 &
server_pid=$!

watchdog_tripped_file="$(mktemp)"
echo "0" > "$watchdog_tripped_file"
(
    while kill -0 "$server_pid" 2>/dev/null; do
        current="$(rss_kb "$server_pid")"
        if (( current > rss_hard_cap_kb )); then
            echo "1" > "$watchdog_tripped_file"
            kill -9 "$server_pid" 2>/dev/null || true
            break
        fi
        sleep "$watchdog_interval_s"
    done
) &
watchdog_pid=$!

cleanup() {
    kill "$watchdog_pid" 2>/dev/null || true
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    wait "$watchdog_pid" 2>/dev/null || true
}
trap cleanup EXIT

check_watchdog() {
    if [[ "$(cat "$watchdog_tripped_file" 2>/dev/null || echo 0)" == "1" ]]; then
        echo >&2
        echo "ABORTED: server RSS crossed ${rss_hard_cap_kb}KB -- watchdog killed it." >&2
        echo "This IS a data point: the workload's memory growth is not safely" >&2
        echo "bounded at this live-set size on this machine. Do not re-run at a" >&2
        echo "larger size without redesigning the cap/workload first." >&2
        rm -f "$watchdog_tripped_file" "$server_log"
        exit 1
    fi
}

if ! wait_for_port "$port"; then
    check_watchdog
    echo "server never came up -- log:" >&2
    cat "$server_log" >&2
    exit 1
fi

calibration_out="$(mktemp)"
ab -q -l -n 2000 -c "$concurrency" "http://127.0.0.1:$port/calibrate" \
    >"$calibration_out" 2>&1 || true
check_watchdog
calibrated_rps="$(awk '/Requests per second/ {print $4}' "$calibration_out")"
rm -f "$calibration_out"
requests_per_batch="$(awk -v r="${calibrated_rps:-200}" -v s="$batch_seconds" \
    'BEGIN { n = r * s; if (n < 1000) n = 1000; printf "%d", n }')"

echo "burn-in (hard): pid=$server_pid port=$port duration=${duration_seconds}s concurrency=$concurrency"
echo "burn-in (hard): calibrated ~${calibrated_rps:-?} req/s -> ${requests_per_batch} requests/batch (target ~${batch_seconds}s/batch)"
printf '%-6s %10s %14s %10s %10s %10s %8s %10s\n' \
    batch rss_kb req_per_sec mean_ms p95_ms p99_ms failed batch_s

batch=0
elapsed=0
start_rss="$(rss_kb "$server_pid")"
declare -a rss_series=()

while (( elapsed < duration_seconds )); do
    check_watchdog
    batch=$((batch + 1))
    ab_out="$(mktemp)"
    ab -q -l -n "$requests_per_batch" -c "$concurrency" \
        "http://127.0.0.1:$port/burn/$batch" >"$ab_out" 2>&1 || true
    check_watchdog

    req_per_sec="$(awk '/Requests per second/ {print $4}' "$ab_out")"
    mean_ms="$(awk '/Time per request/ && /mean\)$/ {print $4}' "$ab_out" | head -1)"
    p95_ms="$(awk '/ 95%/ {print $2}' "$ab_out")"
    p99_ms="$(awk '/ 99%/ {print $2}' "$ab_out")"
    failed="$(awk '/Failed requests/ {print $3}' "$ab_out")"
    batch_seconds_actual="$(awk '/Time taken for tests/ {print $5}' "$ab_out")"
    current_rss="$(rss_kb "$server_pid")"
    rss_series+=("$current_rss")

    printf '%-6d %10s %14s %10s %10s %10s %8s %10s\n' \
        "$batch" "$current_rss" "${req_per_sec:-?}" "${mean_ms:-?}" \
        "${p95_ms:-?}" "${p99_ms:-?}" "${failed:-?}" "${batch_seconds_actual:-?}"

    rm -f "$ab_out"
    batch_elapsed_int="$(awk -v t="${batch_seconds_actual:-$batch_seconds}" 'BEGIN { printf "%d", t }')"
    if (( batch_elapsed_int < 1 )); then batch_elapsed_int=1; fi
    elapsed=$((elapsed + batch_elapsed_int))
done

check_watchdog
end_rss="$(rss_kb "$server_pid")"
rm -f "$watchdog_tripped_file" "$server_log"

echo
echo "== summary =="
echo "RSS: start=${start_rss}KB end=${end_rss}KB over $batch batches (hard cap ${rss_hard_cap_kb}KB, never tripped)"

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
