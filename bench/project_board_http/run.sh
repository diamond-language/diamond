#!/usr/bin/env bash
set -euo pipefail

diamond="${DIAMOND_BIN:-$(cd "$(dirname "$0")/../.." && pwd)/build/diamond}"
bench_dir="$(cd "$(dirname "$0")" && pwd)"
app_dir="$(cd "$bench_dir/../../examples/project_board" && pwd)"
server_threads="${SERVER_THREADS:-6}"
load_threads="${LOAD_THREADS:-6}"
iterations="${ITERATIONS:-100}"
port=19620

wait_for_port() {
    { for _ in $(seq 1 200); do
        if exec 3<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            exec 3<&- 3>&-
            return 0
        fi
        sleep 0.05
    done } 2>/dev/null
    return 1
}

server_log="$(mktemp)"
cleanup() {
    if [[ -n "${server_pid:-}" ]]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
    rm -f "$server_log"
}
trap cleanup EXIT

(cd "$app_dir" && "$diamond" setup_db.di >/dev/null)
(cd "$app_dir" && "$diamond" "$bench_dir/server.di" "$server_threads" "$port") >"$server_log" 2>&1 &
server_pid=$!
if ! wait_for_port; then
    echo "project board benchmark server failed to start" >&2
    cat "$server_log" >&2
    exit 1
fi

(cd "$bench_dir" && "$diamond" load.di "$load_threads" "$iterations" "http://127.0.0.1:$port")

if grep -q '^{"' "$server_log"; then
    echo "logging was not disabled" >&2
    head -n 5 "$server_log" >&2
    exit 1
fi
