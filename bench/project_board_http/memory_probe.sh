#!/usr/bin/env bash
# Ad-hoc measurement, not yet a committed benchmark: samples the
# server's own VmRSS (whole process, every worker thread's independent
# heap combined -- see docs/threads.md's "Isolated-heap design") at
# idle (right after every gremlin_serve worker thread has started and
# is listening, before any request) and again after a short, FIXED
# real load run (same total request volume regardless of server
# thread count, so the delta isolates "more workers" from "more total
# requests"), across a sweep of thread counts. Answers "what does
# adding one more worker thread cost in RSS, and is that cost
# consistent" -- each worker clones the whole program's function/class
# tables (docs/threads.md), a fixed cost independent of request
# volume, on top of whatever live request-scoped data it accumulates.
set -euo pipefail

diamond="$(cd "$(dirname "$0")/../.." && pwd)/build/diamond"
bench_dir="$(cd "$(dirname "$0")" && pwd)"
app_dir="$(cd "$bench_dir/../../examples/project_board" && pwd)"
port=19621
fixed_load_threads=6
fixed_iterations=20

rss_kb() {
    grep VmRSS "/proc/$1/status" | awk '{print $2}'
}

wait_for_port() {
    for _ in $(seq 1 200); do
        if exec 3<>"/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            exec 3<&- 3>&-
            return 0
        fi
        sleep 0.05
    done
    return 1
}

for threads in 1 2 4 6; do
    (cd "$app_dir" && DIAMOND_ENV=test "$diamond" "$bench_dir/server.di" "$threads" "$port") >/tmp/mp_server.log 2>&1 &
    server_pid=$!
    if ! wait_for_port; then
        echo "threads=$threads server failed to start" >&2
        cat /tmp/mp_server.log >&2
        kill "$server_pid" 2>/dev/null || true
        exit 1
    fi
    sleep 0.3
    idle_1=$(rss_kb "$server_pid")
    sleep 0.5
    idle_2=$(rss_kb "$server_pid")
    sleep 0.5
    idle_3=$(rss_kb "$server_pid")

    (cd "$bench_dir" && "$diamond" load.di "$fixed_load_threads" "$fixed_iterations" "http://127.0.0.1:$port") >/tmp/mp_load.log 2>&1

    post_1=$(rss_kb "$server_pid")
    sleep 0.3
    post_2=$(rss_kb "$server_pid")
    sleep 0.3
    post_3=$(rss_kb "$server_pid")

    echo "threads=$threads idle_rss_kb=$idle_1,$idle_2,$idle_3 post_load_rss_kb=$post_1,$post_2,$post_3"

    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    sleep 0.3
done
