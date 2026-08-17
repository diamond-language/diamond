#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-rack-package` from the repo root, which sets this up already).
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

# Wires a two-middleware chain (logging wraps auth wraps the app) up to
# a real gremlin_serve instance via the documented RackChain pattern --
# the same one the README recommends for threads > 1, exercised here
# even at threads: 1 so every subtest goes through the identical code
# path a multi-threaded deployment would use.
server_src() {
    local port="$1"
    local threads="${2:-}"
    local serve_call="gremlin_serve($port, rack_app)"
    if [[ -n "$threads" ]]; then
        serve_call="gremlin_serve($port, rack_app, threads: $threads)"
    fi
    cat <<SRCEOF
require "$(pwd)/../gremlin/gremlin"
require "$(pwd)/rack"

def logging_middleware(request, context, forward)
  response = forward(request, context)
  puts("LOG #{request["method"]} #{request["path"]} -> #{response[0]}")
  response
end

def auth_middleware(request, context, forward)
  if request["headers"]["authorization"] == "Bearer secret"
    forward(request, context)
  else
    [401, {"Content-Type": "text/plain"}, "unauthorized"]
  end
end

def app_handler(request, context)
  [200, {"Content-Type": "text/plain"}, "hello, #{request["path"]}"]
end

def build_chain()
  rack_compose([logging_middleware, auth_middleware], app_handler)
end

def rack_app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

$serve_call
SRCEOF
}

# A request carrying the right bearer token reaches app_handler through
# both middlewares, and the logging middleware's own wrapping (it calls
# forward, then puts's afterward) shows up in the server's stdout --
# proving both composition and ordering, not just that a response came
# back at all.
port=19420
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /hi HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer secret\r\n\r\n' >&3
response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nhello, /hi' ]]

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
grep -q "LOG GET /hi -> 200" "$out"
rm -f "$out"

# The auth middleware short-circuits without ever reaching app_handler
# -- a missing/wrong token gets 401 with the auth middleware's own body,
# not app_handler's "hello, ..." response. The logging middleware still
# wraps it (it always calls forward and always logs afterward,
# regardless of what came back), proving the chain composes both ways:
# a middleware can run code after a downstream short-circuit too.
port=19421
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port")" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /secret HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\nContent-Length: 12\r\n\r\nunauthorized' ]]

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
grep -q "LOG GET /secret -> 401" "$out"
rm -f "$out"

# threads: 3 -- each worker is its own Thread-spawned, fully independent
# DiamondVm (see docs/threads.md), so RackChain's @@instance memoization
# inside build_chain is independently nil per worker the first time that
# worker handles a request. If that ever broke (a shared/uninitialized
# chain, or a crash building it more than once somewhere), some or all
# of these requests would fail or hang instead of cleanly returning 200
# -- kernel-hashed request-to-worker assignment means several requests
# in a row are needed to have a real chance of hitting more than one
# worker.
port=19422
out="$(mktemp)"
timeout 10 "$diamond" -e "$(server_src "$port" 3)" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

for i in 1 2 3 4 5 6; do
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    printf 'GET /mt%d HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer secret\r\n\r\n' "$i" >&3
    response="$(timeout 3 cat <&3)"
    { exec 3<&- 3>&-; } 2>/dev/null || true
    expected_length=$(( 10 + ${#i} ))
    [[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: '"$expected_length"$'\r\n\r\nhello, /mt'"$i" ]]
done

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$out"

echo "3 rack tests passed"
