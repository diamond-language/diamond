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
    cat <<SRCEOF
require "$(pwd)/gremlin"
def run()
  def handler(request)
    if request["method"] == "GET"
      [200, {"Content-Type": "text/plain"}, "hello, #{request["path"]}"]
    else
      [201, {"Content-Type": "text/plain"}, "posted: #{request["body"]}"]
    end
  end
  gremlin_serve($port, handler)
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

echo "4 gremlin tests passed"
