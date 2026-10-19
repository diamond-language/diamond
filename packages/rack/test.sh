#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-rack-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"
package_root="$(pwd)"
test_project="$(mktemp -d)"
trap 'rm -rf "$test_project"' EXIT
mkdir -p "$test_project/cuts"
ln -s "$package_root/../http" "$test_project/cuts/http"
ln -s "$package_root/../logger" "$test_project/cuts/logger"
cd "$test_project"

count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

run_case() {
    local script="$1"
    "$diamond" -e "require \"$package_root/lib/rack\"
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
require "$package_root/../gremlin/lib/gremlin"
require "$package_root/lib/rack"

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

# --- SecurityHeaders passes a nil response straight through untouched
# --- instead of crashing on it -- nil is gremlin_serve's own signal
# --- that a handler already fully took over the connection itself (a
# --- WebSocket upgrade being the motivating case, packages/websocket),
# --- so there's no [status, headers, body] to add headers onto ---
actual="$(run_case '
def app(request, context)
  nil
end
result = SecurityHeaders.call({}, {}, app)
"#{result == nil}"
')"
[[ "$actual" == "true" ]]
count=$((count + 1))

# --- SecurityHeaders wired into a real gremlin_serve chain: the
# --- security headers show up on an actual over-the-wire response,
# --- alongside app_handler's own Content-Type ---
port=19423
timeout 10 "$diamond" -e "$(cat <<SRCEOF
require "$package_root/../gremlin/lib/gremlin"
require "$package_root/lib/rack"

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
require "$package_root/../gremlin/lib/gremlin"
require "$package_root/lib/rack"

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

# --- Cors passes a nil response straight through untouched for an
# --- ordinary (non-preflight) request, instead of crashing on it -- the
# --- same gremlin_serve "already fully handled" signal SecurityHeaders
# --- was just checked for above ---
actual="$(run_case '
def app(request, context)
  nil
end
result = Cors.call({"method": "GET", "headers": {"origin": "https://app.example"}}, {}, app)
"#{result == nil}"
')"
[[ "$actual" == "true" ]]
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
require "$package_root/../gremlin/lib/gremlin"
require "$package_root/lib/rack"

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

# --- StaticFiles: serves a real file with the right Content-Type, falls
# --- through (to the app) for a missing file, a directory, a non-GET
# --- request, and a blocked path-traversal attempt, and respects an
# --- explicit "prefix" ---
static_root="$(mktemp -d)"
mkdir -p "$static_root/css"
printf 'body { color: red; }' > "$static_root/css/app.css"
printf "console.log('hi');" > "$static_root/app.js"
printf 'secret' > "$(dirname "$static_root")/$(basename "$static_root")-outside.txt"

actual="$(run_case "
def app(request, context)
  [404, {}, \"app-fallback\"]
end
StaticFiles.configure({\"root\": \"$static_root\"})
[s1, h1, b1] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/app.js\"}, {}, app)
[s2, h2, b2] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/css/app.css\"}, {}, app)
[s3, h3, b3] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/missing.js\"}, {}, app)
[s4, h4, b4] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/../$(basename "$static_root")-outside.txt\"}, {}, app)
[s5, h5, b5] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/css\"}, {}, app)
[s6, h6, b6] = StaticFiles.call({\"method\": \"POST\", \"path\": \"/app.js\"}, {}, app)
\"#{s1}|#{h1[\"Content-Type\"]}|#{b1}|#{s2}|#{h2[\"Content-Type\"]}|#{s3}|#{b3}|#{s4}|#{s5}|#{s6}\"
")"
[[ "$actual" == "200|text/javascript|console.log('hi');|200|text/css|404|app-fallback|404|404|404" ]]
count=$((count + 1))

actual="$(run_case "
def app(request, context)
  [404, {}, \"app-fallback\"]
end
StaticFiles.configure({\"root\": \"$static_root\", \"prefix\": \"/assets\"})
[s1, h1, b1] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/assets/app.js\"}, {}, app)
[s2, h2, b2] = StaticFiles.call({\"method\": \"GET\", \"path\": \"/app.js\"}, {}, app)
\"#{s1}|#{s2}\"
")"
[[ "$actual" == "200|404" ]]
count=$((count + 1))

# --- StaticFiles wired into a real gremlin_serve chain: a real GET over
# --- the wire serves the file with its Content-Type, and a missing path
# --- falls through to the app instead of erroring ---
port=19426
timeout 10 "$diamond" -e "$(cat <<SRCEOF
require "$package_root/../gremlin/lib/gremlin"
require "$package_root/lib/rack"

def app_handler(request, context)
  [404, {"Content-Type": "text/plain"}, "app-fallback"]
end

def build_chain()
  StaticFiles.configure({"root": "$static_root"})
  rack_compose([StaticFiles.call], app_handler)
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
printf 'GET /app.js HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
static_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$static_response" "HTTP/1.1 200 OK"
count=$((count + 1))
assert_contains "$static_response" "Content-Type: text/javascript"
count=$((count + 1))
assert_contains "$static_response" "console.log('hi');"
count=$((count + 1))

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /missing.js HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
missing_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$missing_response" "app-fallback"
count=$((count + 1))

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

rm -rf "$static_root" "$(dirname "$static_root")/$(basename "$static_root")-outside.txt"

# --- ContentNegotiation: format_from_path ---
actual="$(run_case "
\"#{ContentNegotiation.format_from_path(\"/skins/1.json\")}|#{ContentNegotiation.format_from_path(\"/skins/1.HTML\")}|#{ContentNegotiation.format_from_path(\"/skins/1\")}|#{ContentNegotiation.format_from_path(\"/.json\")}|#{ContentNegotiation.format_from_path(\"/skins/1.\")}|#{ContentNegotiation.format_from_path(\"/\")}\"
")"
[[ "$actual" == "json|html|nil|nil|nil|nil" ]]
count=$((count + 1))

# --- ContentNegotiation: negotiate (Accept-header parsing) ---
actual="$(run_case "
exact = ContentNegotiation.negotiate(\"application/json\", [\"html\", \"json\"], \"html\")
q_preference = ContentNegotiation.negotiate(\"text/html;q=0.5, application/json;q=0.9\", [\"html\", \"json\"], \"html\")
wildcard_any = ContentNegotiation.negotiate(\"*/*\", [\"html\", \"json\"], \"html\")
wildcard_type = ContentNegotiation.negotiate(\"application/*\", [\"html\", \"json\"], \"html\")
no_header = ContentNegotiation.negotiate(nil, [\"html\", \"json\"], \"html\")
no_match = ContentNegotiation.negotiate(\"application/xml\", [\"html\", \"json\"], \"html\")
\"#{exact}|#{q_preference}|#{wildcard_any}|#{wildcard_type}|#{no_header}|#{no_match}\"
")"
[[ "$actual" == "json|json|html|json|html|html" ]]
count=$((count + 1))

# --- ContentNegotiation: respond_to -- path suffix wins over Accept,
# --- Accept alone picks when there's no suffix, an unavailable
# --- explicit format 406s instead of silently falling back, and
# --- extra_headers merge in on top of Content-Type. Laziness (only
# --- the *chosen* representation's Callable is ever invoked) is
# --- proven by giving the unchosen side a builder that raises --
# --- the whole script would abort before producing any output at all
# --- if respond_to ever called it by mistake, so reaching the
# --- expected \$actual string already proves it didn't.
actual="$(run_case "
def build_json()
  \"{}\"
end
def build_boom()
  raise RuntimeError.new(\"should not have been called\")
end

[s1, h1, b1] = ContentNegotiation.respond_to({\"path\": \"/skins/1.json\", \"headers\": {\"accept\": \"text/html\"}},
  status: 200, representations: {\"html\": build_boom, \"json\": build_json}, default_format: \"html\")
[s2, h2, b2] = ContentNegotiation.respond_to({\"path\": \"/skins/1\", \"headers\": {\"accept\": \"application/json\"}},
  status: 200, representations: {\"html\": build_boom, \"json\": build_json}, default_format: \"html\")
[s3, h3, b3] = ContentNegotiation.respond_to({\"path\": \"/skins/1\", \"headers\": {\"accept\": \"application/atom+xml\"}},
  status: 200, representations: {\"xml\": build_boom}, default_format: \"json\")
[s4, h4, b4] = ContentNegotiation.respond_to({\"path\": \"/skins/1\", \"headers\": {}},
  status: 200, representations: {\"json\": build_json}, default_format: \"json\", extra_headers: {\"X-Extra\": \"yes\"})

\"#{s1}|#{h1[\"Content-Type\"]}|#{b1}|#{s2}|#{h2[\"Content-Type\"]}|#{b2}|#{s3}|#{s4}|#{h4[\"Content-Type\"]}|#{h4[\"X-Extra\"]}\"
")"
[[ "$actual" == "200|application/json|{}|200|application/json|{}|406|200|application/json|yes" ]]
count=$((count + 1))


# --- Cors: credentials with the wildcard origin is refused at configuration (it would answer every site's request with that site's own
# --- origin plus Allow-Credentials); an explicit list adds Vary: Origin to every response, wildcard-without-credentials does not, and a
# --- preflight varies by Origin and the requested headers ---
actual="$(run_case '
def app(request, context)
  [200, {"Vary": "Accept-Encoding"}, "ok"]
end
refused = []
[{"credentials": true}, {"credentials": true, "origins": "*"}].each() do |options|
  begin
    Cors.configure(options)
    refused.push("accepted")
  rescue error: ArgumentError
    refused.push("refused")
  end
end
Cors.configure({"origins": ["https://app.example"], "credentials": true})
[s1, listed, b1] = Cors.call({"method": "GET", "headers": {"origin": "https://app.example"}}, {}, app)
[s2, unlisted, b2] = Cors.call({"method": "GET", "headers": {"origin": "https://evil.example"}}, {}, app)
[s3, preflight, b3] = Cors.call({"method": "OPTIONS", "headers": {"origin": "https://app.example", "access-control-request-method": "POST", "access-control-request-headers": "X-A"}}, {}, app)
Cors.configure({})
[s4, wildcard, b4] = Cors.call({"method": "GET", "headers": {"origin": "https://any.example"}}, {}, app)
"#{refused.join(",")}|#{listed["Vary"]}|#{unlisted["Vary"]}|#{preflight["Vary"]}|#{wildcard["Vary"]}"
')"
[[ "$actual" == "refused,refused|Accept-Encoding, Origin|Accept-Encoding, Origin|Origin, Access-Control-Request-Headers|Accept-Encoding" ]]
count=$((count + 1))

# --- SecurityHeaders: a header the handler set (in any spelling) is kept; the defaults fill in only what is missing ---
actual="$(run_case '
def app(request, context)
  [200, {"x-frame-options": "DENY", "Content-Security-Policy": "default-src none"}, "ok"]
end
SecurityHeaders.configure({"content_security_policy": "default-src self"})
[status, headers, body] = SecurityHeaders.call({"method": "GET", "headers": {}}, {}, app)
frame_keys = headers.keys().select() do |key| "#{key}".downcase() == "x-frame-options" end
"#{frame_keys.join(",")}|#{headers["x-frame-options"]}|#{headers["Content-Security-Policy"]}|#{headers["X-Content-Type-Options"]}"
')"
[[ "$actual" == "x-frame-options|DENY|default-src none|nosniff" ]]
count=$((count + 1))

# --- StaticFiles: the query string is not part of the file name, hidden files and directories are never served, the
# --- prefix ends at a path segment, and a NUL byte names nothing ---
static_root="$(mktemp -d)"
mkdir -p "$static_root/css" "$static_root/.git"
printf 'body { color: red; }' > "$static_root/css/app.css"
printf "console.log('hi');" > "$static_root/app.js"
printf 'TOKEN=1' > "$static_root/.env"
printf '[core]' > "$static_root/.git/config"
actual="$(run_case "
def app(request, context)
  [404, {}, \"app-fallback\"]
end
StaticFiles.configure({\"root\": \"$static_root\"})
results = []
[\"/app.js?v=3\", \"/css/app.css?x=1&y=2\", \"/.env\", \"/.git/config\", \"/css/../.env\", \"/app.js\" + chr(0) + \"x\"].each() do |path|
  results.push(StaticFiles.call({\"method\": \"GET\", \"path\": path}, {}, app)[0])
end
StaticFiles.configure({\"root\": \"$static_root\", \"prefix\": \"/assets\"})
[\"/assets/app.js\", \"/assetsX/app.js\", \"/assets\"].each() do |path|
  results.push(StaticFiles.call({\"method\": \"GET\", \"path\": path}, {}, app)[0])
end
results.join(\",\")
")"
[[ "$actual" == "200,200,404,404,404,404,200,404,404" ]]
count=$((count + 1))
rm -rf "$static_root"

# --- ContentNegotiation: q=0 refuses a format instead of ranking it lowest, and the response varies by Accept ---
actual="$(run_case '
def build_json()
  "{}"
end
def build_html()
  "<p></p>"
end
available = ["json", "html"]
refuse_json = ContentNegotiation.negotiate("application/json;q=0, text/html", available, "json")
refuse_only = ContentNegotiation.negotiate("text/html;q=0", ["html"], "html")
ranked = ContentNegotiation.negotiate("text/html;q=0.5, application/json;q=0.9", available, "html")
[status, headers, body] = ContentNegotiation.respond_to({"path": "/x", "headers": {"accept": "application/json;q=0, text/html"}},
  status: 200, representations: {"json": build_json, "html": build_html}, default_format: "json")
[refused_status, refused_headers, refused_body] = ContentNegotiation.respond_to({"path": "/x", "headers": {"accept": "text/html;q=0"}},
  status: 200, representations: {"html": build_html}, default_format: "html")
"#{refuse_json}|#{refuse_only}|#{ranked}|#{status}|#{headers["Vary"]}|#{refused_status}"
')"
[[ "$actual" == "html|nil|json|200|Accept|406" ]]
count=$((count + 1))

# --- RateLimit: once more keys than "max_keys" are held, buckets of finished windows are dropped ---
actual="$(run_case '
def app(request, context)
  [200, {}, "ok"]
end
def identify(request) = request["who"]
RateLimit.configure({"limit": 5, "window": 0.05, "key": identify, "max_keys": 3})
["a", "b", "c", "d", "e"].each() do |who|
  RateLimit.call({"who": who}, {}, app)
end
held = RateLimit.tracked_keys()
IO.poll([], [], 250)
RateLimit.call({"who": "z"}, {}, app)
"#{held}|#{RateLimit.tracked_keys()}"
')"
[[ "$actual" == "5|1" ]]
count=$((count + 1))

echo "$count rack tests passed"
