#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-gremlin-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
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

server_src() {
    local port="$1"
    local threads="${2:-}"
    local serve_call="gremlin_serve($port, handler)"
    if [[ -n "$threads" ]]; then
        serve_call="gremlin_serve($port, handler, threads: $threads)"
    fi
    cat <<SRCEOF
require "$(pwd)/lib/gremlin"
def run()
  def handler(request, context)
    if request["method"] == "GET"
      [200, {"Content-Type": "text/plain"}, "hello, #{request["path"]}"]
    else
      [201, {"Content-Type": "text/plain"}, "posted: #{request["body"]}"]
    end
  end
  $serve_call
end
run()
SRCEOF
}

# Basic GET/POST round trip -- proves gremlin_serve's Rack-style handler
# contract and packages/http's request-parsing/response-writing work
# unmodified against the non-blocking connection wrapper.
port=19410
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /world HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nhello, /world' ]]

exec 3<>"/dev/tcp/127.0.0.1/$port"
body='{"n":1}'
printf 'POST /items HTTP/1.1\r\nHost: localhost\r\nContent-Length: %d\r\n\r\n%s' \
    "${#body}" "$body" >&3
response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\nContent-Length: 15\r\n\r\nposted: {"n":1}' ]]
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$out"

# The actual point of this package: a connection that opens and sends
# nothing at all must not block any other connection's own progress.
# http_serve's single blocking accept loop would wedge on this forever --
# gremlin_serve's whole reason to exist is that it doesn't.
port=19411
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

# Connection A: connect, send nothing, leave it open and idle.
exec 4<>"/dev/tcp/127.0.0.1/$port"

# Connection B: opened *after* A is already sitting idle, sends a real
# request. If gremlin_serve were secretly serialized on A, B would never
# get a response inside the timeout below.
exec 5<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /concurrent HTTP/1.1\r\nHost: localhost\r\n\r\n' >&5
response="$(timeout 3 cat <&5)"
{ exec 4<&- 4>&-; } 2>/dev/null || true
{ exec 5<&- 5>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 18\r\n\r\nhello, /concurrent' ]]

# A third connection after both prior ones (one closed, one that hung up
# without ever sending a request) confirms the server's own bookkeeping
# stays correct across a full accept/resume/prune cycle, not just a
# single round.
exec 6<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /third HTTP/1.1\r\nHost: localhost\r\n\r\n' >&6
response="$(timeout 3 cat <&6)"
{ exec 6<&- 6>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nhello, /third' ]]

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$out"

# threads: 3 -- each worker opens its own listener on the same port via
# reuse_port: true (see gremlin_worker); if that ever silently fell back
# to a single worker (or SO_REUSEPORT wasn't actually reaching the
# socket), the server would still pass every test above unchanged, so
# this specifically proves multi-listener startup on one port actually
# works end to end. Several sequential requests can't deterministically
# prove which thread handled which (kernel-hashed), but a wrong or
# crashed additional listener would make *some* of these time out or
# fail outright.
port=19412
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port" 3)" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

for i in 1 2 3 4 5; do
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    printf 'GET /mt%d HTTP/1.1\r\nHost: localhost\r\n\r\n' "$i" >&3
    response="$(timeout 3 cat <&3)"
    { exec 3<&- 3>&-; } 2>/dev/null || true
    expected_length=$(( 10 + ${#i} ))
    [[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: '"$expected_length"$'\r\n\r\nhello, /mt'"$i" ]]
done

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$out"

# Graceful shutdown, idle case: SIGTERM with zero in-flight connections
# exits promptly (the fast path -- see GremlinShutdown.requested?() &&
# connections.length() == 0 in server.di) with a clean exit code, not a
# bare kill. `timeout` forwards a signal it receives to the process it's
# monitoring and (when the child exits on its own well within the
# timeout) reports that child's own exit status, so `$pid` here still
# refers to the right thing for both `kill -TERM` and `wait`.
port=19413
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

kill -TERM "$pid"
wait "$pid"
status=$?
[[ "$status" == "0" ]]
grep -q '"message":"server.shutdown_complete"' "$out"
grep -q '"forced":false' "$out"
rm -f "$out"

# Graceful shutdown, in-flight case: SIGTERM arriving while a request is
# genuinely mid-flight (a connection sitting in gremlin_worker's own
# `connections`, not yet resumed to completion) must not cut it off --
# the process should stay alive until that request's own response is
# fully sent, only then exit cleanly. Deliberately sends an incomplete
# request line/headers with no terminating blank line first, so
# http_parse_request is left waiting for more data (WouldBlockError,
# fiber suspended) at the moment SIGTERM arrives -- a real in-flight
# connection, not one that already finished before the signal.
port=19414
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /inflight HTTP/1.1\r\nHost: localhost\r\n' >&3
sleep 0.2

kill -TERM "$pid"
sleep 0.2
kill -0 "$pid" 2>/dev/null   # still alive -- draining, hasn't been cut off

printf '\r\n' >&3
response="$(timeout 3 cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 16\r\n\r\nhello, /inflight' ]]

wait "$pid"
status=$?
[[ "$status" == "0" ]]
grep -q '"message":"server.shutdown_complete"' "$out"
grep -q '"forced":false' "$out"
rm -f "$out"

# Graceful shutdown, impatient double-signal case: a second SIGTERM/
# SIGINT arriving *after* the first has already been processed (draining
# started, listener already closed -- not two signals racing to arrive
# before Diamond's dispatch loop ever runs, which the OS can coalesce
# into a single delivery) must exit immediately, cutting off whatever's
# still in flight, rather than waiting out the rest of the 10s grace
# period. The 0.3s gap after the first `kill` is deliberately generous
# for this reason -- it's there to guarantee the first signal has
# already been handled, not just a throttle.
#
# Deliberately NOT wrapped in `timeout` (unlike every other server
# invocation in this file): confirmed directly (a standalone trap-based
# script, not Diamond) that GNU `timeout` only forwards the *first*
# signal it receives to the process it's monitoring -- a second `kill`
# sent to `timeout`'s own PID is silently swallowed, never reaching the
# real child at all. Using it here would make this test unable to ever
# observe the behavior it's testing, not because of anything wrong with
# gremlin_serve. The bounded poll loop below (instead of a bare `wait`)
# is what keeps a genuine bug (a hang) from blocking the whole suite in
# `timeout`'s place.
port=19415
out="$(mktemp)"
"$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /stuck HTTP/1.1\r\nHost: localhost\r\n' >&3
sleep 0.2

kill -TERM "$pid"
sleep 0.3
kill -0 "$pid" 2>/dev/null   # still alive -- draining after the first signal

kill -TERM "$pid"
exited=1
for _ in $(seq 1 100); do
    if ! kill -0 "$pid" 2>/dev/null; then
        exited=0
        break
    fi
    sleep 0.05
done
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$exited" == "0" ]]
wait "$pid"
status=$?
[[ "$status" == "0" ]]
grep -q '"message":"server.shutdown_forced_by_signal"' "$out"
rm -f "$out"

# context["gremlin_connection"] escape hatch: a handler that writes its
# own response directly to the raw connection and returns nil must have
# that response reach the client verbatim, with handle_connection's own
# http_write_response never firing a second, conflicting response behind
# it (the whole point of the nil convention -- see gremlin.di's own
# "Escape hatch" doc comment). A deliberately non-HTTP-shaped body (no
# Content-Length, a status line gremlin_serve itself would never
# generate) is proof the response came from the handler's own write, not
# from http_write_response reinterpreting it.
port=19416
out="$(mktemp)"
raw_handler_src() {
    local port="$1"
    cat <<SRCEOF
require "$(pwd)/lib/gremlin"
def run()
  def handler(request, context)
    conn = context["gremlin_connection"]
    conn.write("RAW 200 direct\r\n\r\nhandled-directly")
    nil
  end
  gremlin_serve($port, handler)
end
run()
SRCEOF
}
timeout 10 "$diamond" -e "$(raw_handler_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /ws HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
response="$(timeout 3 cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'RAW 200 direct\r\n\r\nhandled-directly' ]]

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$out"

echo "13 gremlin tests passed"
