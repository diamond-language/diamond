#!/usr/bin/env bash
# Runs the metrics collector: offline aggregation against golden output, then
# a live UDP server (built with `diamond build`) that is fed datagrams,
# queried, reset, and shut down both by SIGTERM (whose trapped handler prints
# the final totals and exits 0 while the server is blocked in receive) and by
# a !stop datagram. Client calls are still wrapped in `timeout` as a
# backstop, though the client's own IO.poll timeout should fire first.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond, and
# METRICS_PORT to pick the port.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
port="${METRICS_PORT:-18100}"
work="$(mktemp -d)"
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2> /dev/null || true; rm -rf "$work"; }
trap cleanup EXIT

"$diamond" build metrics.di -o "$work/metrics" > "$work/build.log"
m="$work/metrics"
client() { timeout 10 "$m" "$@"; }
status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }

# Offline aggregation, interpreted and built.
"$diamond" metrics.di aggregate testdata/lines.txt | cmp testdata/lines.expected -
"$m" aggregate testdata/lines.txt | cmp testdata/lines.expected -
[[ "$("$m" aggregate testdata/bad_lines.txt 2>&1 >/dev/null)" == "testdata/bad_lines.txt:2: bad value 'x'" ]]
[[ "$(status_of "$m" aggregate testdata/bad_lines.txt)" == 65 ]]

start() {
  # bash starts background jobs with SIGINT ignored; reset it (docs/networking.md).
  (trap - INT TERM; exec "$m" serve "$port") > "$work/server.out" 2>&1 &
  server_pid=$!
  for _ in $(seq 50); do
    [[ "$(client send "$port" '!ping' 2> /dev/null || true)" == "pong" ]] && return 0
    sleep 0.1
  done
  echo "server didn't start" >&2; cat "$work/server.out" >&2; exit 1
}

start
[[ "$(client send "$port" 'requests:1|c' 'requests:2|c' | tr '\n' '|')" == "ok 1|ok 1|" ]]
# A batch is one datagram with several lines; a bad line rejects all of it.
[[ "$(client send "$port" $'queue_depth:9|g\nlatency:10|ms\nlatency:30|ms')" == "ok 3" ]]
[[ "$(client send "$port" $'errors:1|c\nerrors:oops|c')" == "error: bad value 'oops'" ]]
[[ "$(client send "$port" 'nonsense')" == "error: expected name:value|type in 'nonsense'" ]]
[[ "$(status_of client send "$port" 'nonsense')" == 1 ]]
[[ "$(client report "$port" | head -2 | tr '\n' '|')" == "counters|  requests           3.00|" ]]
[[ "$(client report "$port" --json)" == '{"counters":{"requests":3.0},"gauges":{"queue_depth":9.0},"timers":{"lat'* ]]

# A second server on the same port fails cleanly instead of hanging.
[[ "$(status_of timeout 10 "$m" serve "$port")" == 66 ]]

client send "$port" '!reset' > /dev/null
[[ "$(client report "$port")" == "(nothing recorded)" ]]

# SIGTERM while blocked in receive: the trapped handler prints and exits 0.
client send "$port" 'requests:5|c' 'latency:4|ms' > /dev/null
kill -TERM "$server_pid"
status=0; wait "$server_pid" || status=$?; server_pid=""
[[ "$status" == 0 ]]
grep -qx "signal received; final totals:" "$work/server.out"
grep -qx "  requests           5.00" "$work/server.out"

# A restart, then a graceful stop by datagram.
start
client send "$port" 'hits:2|c' > /dev/null
[[ "$(client send "$port" '!stop')" == "stopping" ]]
status=0; wait "$server_pid" || status=$?; server_pid=""
[[ "$status" == 0 ]]
grep -qx "stopped by !stop; final totals:" "$work/server.out"
grep -qx "  hits               2.00" "$work/server.out"

# With no server listening, the client gives up after its own 2 s timeout.
[[ "$(client send "$port" '!ping' 2>&1 || true)" == "no reply from the server within 2 seconds" ]]
[[ "$(status_of client send "$port" '!ping')" == 66 ]]

[[ "$(status_of "$m")" == 64 ]]
[[ "$(status_of "$m" serve notaport)" == 64 ]]
[[ "$(status_of "$m" send 99999 x)" == 64 ]]
[[ "$(status_of "$m" aggregate testdata/missing.txt)" == 66 ]]

echo "metrics smoke test passed"
