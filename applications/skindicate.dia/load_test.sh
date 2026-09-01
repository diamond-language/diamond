#!/usr/bin/env bash
# Load-tests a running Skindicate deployment. Run this from your OWN
# machine (or any box that ISN'T the target droplet) -- running it on
# the droplet itself means the load generator competes with Diamond
# and Caddy for the same limited CPU/RAM, confounding whatever numbers
# you get.
#
# Prefers `wrk` (richer -t/-c/-d model, keep-alive) if installed,
# falls back to `ab` (Apache Bench, more commonly preinstalled)
# otherwise. Hits a small mix of real Skindicate routes -- not just
# "/" -- at a ramping sequence of concurrency levels, so you can see
# *where* things start to degrade rather than getting one data point.
#
# Usage: TARGET=https://modartist.app bash load_test.sh
#   TARGET      base URL (default: https://modartist.app)
#   DURATION    seconds per wrk run (default: 15) -- ab uses a fixed
#               request count instead (see REQUESTS below), since ab
#               has no native duration mode
#   REQUESTS    total requests per ab run (default: 2000)
#   LEVELS      space-separated concurrency levels to ramp through
#               (default: "10 50 100")

set -euo pipefail

TARGET="${TARGET:-https://modartist.app}"
DURATION="${DURATION:-15}"
REQUESTS="${REQUESTS:-2000}"
LEVELS="${LEVELS:-10 50 100}"
OUT_DIR="$(dirname "$0")/load_test_results_$(date +%Y%m%d_%H%M%S)"

ENDPOINTS=(
    "/"
    "/skins/1"
    "/login"
)

mkdir -p "$OUT_DIR"

echo "== Baseline check =="
if ! curl -sS -o /dev/null -w "  %{http_code} %{time_total}s  ${TARGET}/\n" -m 10 "${TARGET}/"; then
    echo "Baseline request to ${TARGET}/ failed -- check the server's actually up before load testing it." >&2
    exit 1
fi

if command -v wrk >/dev/null 2>&1; then
    TOOL="wrk"
elif command -v ab >/dev/null 2>&1; then
    TOOL="ab"
else
    echo "Neither wrk nor ab is installed." >&2
    echo "  wrk: https://github.com/wg/wrk (build from source, or your OS package manager)" >&2
    echo "  ab:  apt install apache2-utils / dnf install httpd-tools" >&2
    exit 1
fi

echo "== Using $TOOL, results saved under $OUT_DIR/ =="
echo

run_wrk() {
    local url="$1" concurrency="$2" out="$3"
    local threads=4
    if [[ "$concurrency" -lt "$threads" ]]; then
        threads="$concurrency"
    fi
    wrk -t"$threads" -c"$concurrency" -d"${DURATION}s" --latency "$url" | tee "$out"
}

run_ab() {
    local url="$1" concurrency="$2" out="$3"
    # -k: reuse connections (HTTP keep-alive). Without it, ab pays a full
    # new TLS handshake on *every single request* -- over any real
    # network distance that dominates the measured latency completely,
    # swamping whatever the server itself actually takes to respond
    # (confirmed directly: ~500-900ms/request at concurrency 2 without
    # -k against this exact target, almost entirely handshake cost, not
    # server processing time). wrk already reuses connections by
    # default, so this only matters for the ab path.
    ab -k -n "$REQUESTS" -c "$concurrency" -g "${out%.txt}.tsv" "$url" | tee "$out"
}

# Pulls the headline numbers back out of one run's raw output so the
# final table doesn't make you go re-open 9+ separate files by hand.
# ab and wrk report these in totally different formats, hence the
# TOOL branch -- there's no shared summary line to grep for.
SUMMARY_ROWS=()

record_summary() {
    local endpoint="$1" concurrency="$2" out="$3"
    local rps="?" latency_ms="?" failed="0"
    if [[ "$TOOL" == "wrk" ]]; then
        rps="$(grep -m1 '^Requests/sec:' "$out" | awk '{print $2}')"
        latency_ms="$(grep -m1 '^ *Latency' "$out" | awk '{print $2}')"
        failed="$(grep -m1 'Non-2xx or 3xx responses' "$out" | awk '{print $NF}')"
        [[ -z "$failed" ]] && failed=0
    else
        rps="$(grep -m1 '^Requests per second:' "$out" | awk '{print $4}')"
        latency_ms="$(grep -m1 '^Time per request:.*(mean)$' "$out" | awk '{print $4}')"
        failed="$(grep -m1 '^Failed requests:' "$out" | awk '{print $3}')"
    fi
    SUMMARY_ROWS+=("$(printf '%-10s %6s %12s %14s %8s' "$endpoint" "$concurrency" "${rps:-?}" "${latency_ms:-?}" "${failed:-?}")")
}

for endpoint in "${ENDPOINTS[@]}"; do
    url="${TARGET}${endpoint}"
    safe_name="$(echo "$endpoint" | tr -c 'a-zA-Z0-9' '_')"
    [[ -z "$safe_name" || "$safe_name" == "_" ]] && safe_name="root"

    for concurrency in $LEVELS; do
        echo "== $endpoint  concurrency=$concurrency =="
        out_file="$OUT_DIR/${safe_name}_c${concurrency}.txt"
        if [[ "$TOOL" == "wrk" ]]; then
            run_wrk "$url" "$concurrency" "$out_file"
        else
            run_ab "$url" "$concurrency" "$out_file"
        fi
        record_summary "$endpoint" "$concurrency" "$out_file"
        echo
        sleep 2   # let the server settle between runs rather than back-to-back slamming
    done
done

{
    printf '%-10s %6s %12s %14s %8s\n' "endpoint" "conc" "req/s" "latency(ms)" "failed"
    for row in "${SUMMARY_ROWS[@]}"; do
        echo "$row"
    done
} | tee "$OUT_DIR/summary.txt"

echo
echo "== Done. Raw output in $OUT_DIR/, summary above also saved to $OUT_DIR/summary.txt =="
echo "While a run's in flight, on the droplet in a separate SSH session:"
echo "  watch -n1 free -h"
echo "  journalctl -u skindicate -f"
