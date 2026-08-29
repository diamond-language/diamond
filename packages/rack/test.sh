#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-rack-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/rack\"
$script"
}

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
require "$(pwd)/../gremlin/lib/gremlin"
require "$(pwd)/lib/rack"

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
count=$((count + 1))

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
count=$((count + 1))

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
count=$((count + 1))

# --- SecurityHeaders: safe defaults (SAMEORIGIN, nosniff, a referrer
# --- policy), no CSP/HSTS unless configured, and the app's own headers
# --- survive alongside the security ones ---
actual="$(run_case '
def app(request, context)
  [200, {"Content-Type": "text/plain"}, "hi"]
end
[status, headers, body] = SecurityHeaders.call({}, {}, app)
"#{headers["X-Frame-Options"]}|#{headers["X-Content-Type-Options"]}|#{headers["Referrer-Policy"]}|#{headers["Content-Security-Policy"]}|#{headers["Strict-Transport-Security"]}|#{headers["Content-Type"]}"
')"
[[ "$actual" == 'SAMEORIGIN|nosniff|strict-origin-when-cross-origin|nil|nil|text/plain' ]]
count=$((count + 1))

# --- SecurityHeaders.configure: frame_options: false omits the header
# --- entirely, an explicit CSP is passed through, and hsts: true expands
# --- to a real default value while a literal hsts string passes through
# --- unchanged ---
actual="$(run_case '
def app(request, context)
  [200, {}, "hi"]
end
SecurityHeaders.configure({"frame_options": false, "content_security_policy": "default-src '"'"'self'"'"'", "hsts": true})
[s1, h1, b1] = SecurityHeaders.call({}, {}, app)
SecurityHeaders.configure({"hsts": "max-age=60"})
[s2, h2, b2] = SecurityHeaders.call({}, {}, app)
"#{h1["X-Frame-Options"]}|#{h1["Content-Security-Policy"]}|#{h1["Strict-Transport-Security"]}|#{h2["Strict-Transport-Security"]}"
')"
[[ "$actual" == "nil|default-src 'self'|max-age=31536000; includeSubDomains|max-age=60" ]]
count=$((count + 1))

# --- SecurityHeaders wired into a real gremlin_serve chain: the
# --- security headers show up on an actual over-the-wire response,
# --- alongside app_handler's own Content-Type ---
port=19423
timeout 10 "$diamond" -e "$(cat <<SRCEOF
require "$(pwd)/../gremlin/lib/gremlin"
require "$(pwd)/lib/rack"

def app_handler(request, context)
  [200, {"Content-Type": "text/plain"}, "secured"]
end

def build_chain()
  rack_compose([SecurityHeaders.call], app_handler)
end

def rack_app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve($port, rack_app)
SRCEOF
)" >/dev/null 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ "$response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nX-Frame-Options: SAMEORIGIN\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: strict-origin-when-cross-origin\r\nContent-Length: 7\r\n\r\nsecured' ]]
count=$((count + 1))

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# --- RateLimit: allows up to the configured limit per key, rejects with
# --- 429 + Retry-After past it, and tracks separate keys independently ---
actual="$(run_case '
def app(request, context)
  [200, {}, "ok"]
end
def key_by_ip(request)
  request["headers"]["x-real-ip"]
end
RateLimit.configure({"limit": 2, "window": 60, "key": key_by_ip})
def try_it(ip)
  [status, headers, body] = RateLimit.call({"headers": {"x-real-ip": ip}}, {}, app)
  "#{status}|#{headers["Retry-After"]}"
end
"#{try_it("1.1.1.1")}|#{try_it("1.1.1.1")}|#{try_it("1.1.1.1")}|#{try_it("2.2.2.2")}"
')"
[[ "$actual" == "200|nil|200|nil|429|60|200|nil" ]]
count=$((count + 1))

# --- RateLimit: a fixed window resets after it elapses ---
actual="$(run_case '
def app(request, context)
  [200, {}, "ok"]
end
def key_by_ip(request)
  request["headers"]["x-real-ip"]
end
RateLimit.configure({"limit": 1, "window": 1, "key": key_by_ip})
def try_it()
  [status, headers, body] = RateLimit.call({"headers": {"x-real-ip": "1.1.1.1"}}, {}, app)
  status
end
first = try_it()
second = try_it()
Process.run(["sleep", "1.2"])
third = try_it()
"#{first}|#{second}|#{third}"
')"
[[ "$actual" == "200|429|200" ]]
count=$((count + 1))

# --- RateLimit wired into a real gremlin_serve chain: the 4th request
# --- from the same key within the window gets a real over-the-wire 429 ---
port=19424
timeout 10 "$diamond" -e "$(cat <<SRCEOF
require "$(pwd)/../gremlin/lib/gremlin"
require "$(pwd)/lib/rack"

def app_handler(request, context)
  [200, {"Content-Type": "text/plain"}, "ok"]
end

def key_by_ip(request)
  request["headers"]["x-real-ip"]
end

def build_chain()
  RateLimit.configure({"limit": 3, "window": 60, "key": key_by_ip})
  rack_compose([RateLimit.call], app_handler)
end

def rack_app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve($port, rack_app)
SRCEOF
)" >/dev/null 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

for i in 1 2 3; do
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    printf 'GET / HTTP/1.1\r\nHost: localhost\r\nX-Real-Ip: 9.9.9.9\r\n\r\n' >&3
    response="$(cat <&3)"
    { exec 3<&- 3>&-; } 2>/dev/null || true
    assert_contains "$response" "HTTP/1.1 200 OK"
done
count=$((count + 1))

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\nX-Real-Ip: 9.9.9.9\r\n\r\n' >&3
limited_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$limited_response" "HTTP/1.1 429"
count=$((count + 1))

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

# --- Cors: wildcard default grants any Origin, no Origin header at all
# --- (same-origin) gets no CORS headers, and a preflight (OPTIONS with
# --- its own Access-Control-Request-Method) is answered directly with
# --- 204 and never reaches the app, echoing the requested headers back ---
actual="$(run_case '
def app(request, context)
  [200, {}, "ok"]
end
[s1, h1, b1] = Cors.call({"method": "GET", "headers": {"origin": "https://evil.example"}}, {}, app)
[s2, h2, b2] = Cors.call({"method": "GET", "headers": {}}, {}, app)
[s3, h3, b3] = Cors.call({"method": "OPTIONS", "headers": {"origin": "https://app.example", "access-control-request-method": "PUT", "access-control-request-headers": "X-Custom"}}, {}, app)
"#{h1["Access-Control-Allow-Origin"]}|#{h2["Access-Control-Allow-Origin"]}|#{s3}|#{h3["Access-Control-Allow-Origin"]}|#{h3["Access-Control-Allow-Headers"]}|#{b3}"
')"
[[ "$actual" == "*|nil|204|*|X-Custom|" ]]
count=$((count + 1))

# --- Cors: an ordinary (non-preflight) OPTIONS request -- no
# --- Access-Control-Request-Method header -- still reaches the app ---
actual="$(run_case '
def app(request, context)
  [200, {}, "handled"]
end
[status, headers, body] = Cors.call({"method": "OPTIONS", "headers": {}}, {}, app)
"#{status}|#{body}"
')"
[[ "$actual" == "200|handled" ]]
count=$((count + 1))

# --- Cors.configure: an explicit allow-list reflects a listed origin
# --- exactly (never "*", forced by credentials: true) and grants
# --- nothing to an unlisted one -- whose request still succeeds, since
# --- CORS is a browser-side grant, not a server-side rejection ---
actual="$(run_case '
def app(request, context)
  [200, {}, "ok"]
end
Cors.configure({"origins": ["https://app.example"], "credentials": true, "max_age": 600})
[s1, h1, b1] = Cors.call({"method": "GET", "headers": {"origin": "https://app.example"}}, {}, app)
[s2, h2, b2] = Cors.call({"method": "GET", "headers": {"origin": "https://evil.example"}}, {}, app)
[s3, h3, b3] = Cors.call({"method": "OPTIONS", "headers": {"origin": "https://app.example", "access-control-request-method": "POST"}}, {}, app)
"#{h1["Access-Control-Allow-Origin"]}|#{h1["Access-Control-Allow-Credentials"]}|#{s2}|#{h2["Access-Control-Allow-Origin"]}|#{h3["Access-Control-Max-Age"]}"
')"
[[ "$actual" == "https://app.example|true|200|nil|600" ]]
count=$((count + 1))

# --- Cors wired into a real gremlin_serve chain: a real preflight over
# --- the wire gets 204 with the CORS headers and never reaches the app,
# --- and a real cross-origin GET gets the app's response plus the
# --- Access-Control-Allow-Origin header ---
port=19425
timeout 10 "$diamond" -e "$(cat <<SRCEOF
require "$(pwd)/../gremlin/lib/gremlin"
require "$(pwd)/lib/rack"

def app_handler(request, context)
  [200, {"Content-Type": "text/plain"}, "ok"]
end

def build_chain()
  Cors.configure({"origins": ["https://app.example"]})
  rack_compose([Cors.call], app_handler)
end

def rack_app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve($port, rack_app)
SRCEOF
)" >/dev/null 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'OPTIONS / HTTP/1.1\r\nHost: localhost\r\nOrigin: https://app.example\r\nAccess-Control-Request-Method: POST\r\n\r\n' >&3
preflight_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$preflight_response" "HTTP/1.1 204"
count=$((count + 1))
assert_contains "$preflight_response" "Access-Control-Allow-Origin: https://app.example"
count=$((count + 1))

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\nOrigin: https://app.example\r\n\r\n' >&3
cors_get_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$cors_get_response" "HTTP/1.1 200 OK"
count=$((count + 1))
assert_contains "$cors_get_response" "Access-Control-Allow-Origin: https://app.example"
count=$((count + 1))

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo "$count rack tests passed"
