#!/usr/bin/env bash
set -euo pipefail

# HTTP throughput/latency benchmark for examples/library -- the full
# stack, not a synthetic handler like bench/gremlin_http/run.sh's own
# hello/cpu workloads: real Dials::Router dispatch, real ActiveRecord
# queries against SQLite3, real Div template rendering, over gremlin_serve's
# `threads: N` worker parallelism. Read-only routes only (GET) -- a write
# workload would add SQLite lock-contention as a second variable this
# benchmark isn't trying to isolate; see "Caveats" in RESULTS.md.
#
# Mirrors bench/gremlin_http/run.sh's own shape (server_src/run_trial/
# results-table structure) as closely as possible, parameterized by route
# instead of by synthetic workload. Requires: `make release` build and
# Apache Bench (`ab`, `dnf install httpd-tools` on Fedora).
diamond="${DIAMOND_BIN:-$(cd "$(dirname "$0")/../.." && pwd)/build/diamond}"
lib_dir="$(cd "$(dirname "$0")/../../examples/library" && pwd)"
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

# Same require list and Author/Book.configure setup as examples/library/
# app.di itself -- just port/threads parameterized (app.di hardcodes
# both) and started from lib_dir so Database.get's relative "library.db"
# path resolves to the same seeded database examples/library already
# uses (bash setup_db.di once below, before any trial runs).
server_src() {
    local port="$1" threads="$2"
    cat <<SRCEOF
require "$(cd ../../packages/active_record && pwd)/lib/active_record"
require "$(cd ../../packages/gremlin && pwd)/lib/gremlin"
require "$(cd ../../packages/rack && pwd)/lib/rack"
require "$(cd ../../packages/div && pwd)/lib/div/runtime"
require "$(cd ../../packages/dials && pwd)/lib/dials"

require "$lib_dir/views/.cache/author_books_table.html"
require "$lib_dir/views/.cache/author_show.html"
require "$lib_dir/views/.cache/authors_table.html"
require "$lib_dir/views/.cache/books_table.html"
require "$lib_dir/views/.cache/book_show.html"
require "$lib_dir/views/.cache/author_form.html"
require "$lib_dir/views/.cache/book_form.html"
require "$lib_dir/views/.cache/home.html"
require "$lib_dir/views/.cache/layout.html"

require "$lib_dir/lib/database"
require "$lib_dir/lib/author"
require "$lib_dir/lib/book"

require "$lib_dir/lib/authors_controller"
require "$lib_dir/lib/books_controller"
require "$lib_dir/lib/routes"
require "$lib_dir/lib/middleware"

gremlin_serve($port, app, threads: $threads)
SRCEOF
}

run_trial() {
    local route_name="$1" path="$2" threads="$3" port="$4" requests="$5" concurrency="$6"
    local out
    out="$(mktemp)"
    (cd "$lib_dir" && "$diamond" -e "$(server_src "$port" "$threads")") >/dev/null 2>&1 &
    local pid=$!
    if ! wait_for_port "$port"; then
        echo "  ($route_name, threads=$threads) FAILED TO START"
        kill "$pid" 2>/dev/null || true
        return
    fi
    { exec 3<&- 3>&-; } 2>/dev/null || true
    ab -q -n "$requests" -c "$concurrency" "http://127.0.0.1:$port$path" >"$out" 2>&1 || true
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true

    local rps mean p99 failed
    rps=$(grep "Requests per second" "$out" | awk '{print $4}')
    mean=$(grep "Time per request" "$out" | head -1 | awk '{print $4}')
    p99=$(grep " 99%" "$out" | awk '{print $2}')
    failed=$(grep "Failed requests" "$out" | awk '{print $3}')
    printf "%-15s | %7s | %10s | %12s | %10s | %8s\n" \
        "$route_name" "$threads" "${rps:-?}" "${mean:-?}ms" "${p99:-?}ms" "${failed:-?}"
    rm -f "$out"
}

echo "seeding library.db (examples/library/setup_db.di)..."
(cd "$lib_dir" && rm -f library.db && "$diamond" setup_db.di >/dev/null)

echo "route           | threads | req/sec   | mean latency | p99 latency | failed"
echo "----------------|---------|-----------|--------------|-------------|-------"
port=19600
for route_name_path in "home:/" "authors_index:/authors" "authors_show:/authors/1" \
                       "books_index:/books" "books_available:/books/available"; do
    route_name="${route_name_path%%:*}"
    path="${route_name_path#*:}"
    for threads in 1 4; do
        run_trial "$route_name" "$path" "$threads" "$port" 5000 50
        port=$((port + 1))
    done
done
