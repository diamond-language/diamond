#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-cookies-package` from the repo root, which sets this up already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/cookies\"
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

# --- cookie_parse: basic split, "; " and bare ";" both accepted, ---
# --- last-write-wins on a duplicate name, nil/empty header -> {} ---
actual="$(run_case '
h = cookie_parse("a=1; b=2;c=3; a=4")
"#{h["a"]}|#{h["b"]}|#{h["c"]}|#{h.length()}"
')"
[[ "$actual" == "4|2|3|3" ]]
count=$((count + 1))

actual="$(run_case 'cookie_parse(nil).length()')"
[[ "$actual" == "0" ]]
count=$((count + 1))

actual="$(run_case 'cookie_parse("").length()')"
[[ "$actual" == "0" ]]
count=$((count + 1))

# --- cookie_serialize: defaults (HttpOnly, SameSite=Lax, no Secure) ---
actual="$(run_case 'cookie_serialize("name", "value")')"
[[ "$actual" == "name=value; HttpOnly; SameSite=Lax" ]]
count=$((count + 1))

# --- cookie_serialize: every option, http_only explicitly off ---
actual="$(run_case 'cookie_serialize("name", "value", {"path": "/app", "domain": "example.com", "max_age": 3600, "secure": true, "same_site": "Strict", "http_only": false})')"
[[ "$actual" == "name=value; Path=/app; Domain=example.com; Max-Age=3600; Secure; SameSite=Strict" ]]
count=$((count + 1))

# --- base64url: round-trips arbitrary bytes, including embedded NULs ---
# --- and 0xff, using only the URL-safe alphabet (no '+', '/', '=') ---
actual="$(run_case '
data = "a" + 0.chr() + "b" + 255.chr()
encoded = base64url_encode(data)
"#{encoded}|#{base64url_decode(encoded) == data}"
')"
assert_contains "$actual" "|true"
count=$((count + 1))
[[ "$actual" != *"+"* && "$actual" != *"/"* && "$actual" != *"="* ]]
count=$((count + 1))

# --- base64url_decode: malformed input is nil, not a crash ---
actual="$(run_case 'base64url_decode("not valid base64url!!!")')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- hex_decode: a real digest decodes to exactly 32 bytes (AES-256 key
# --- size); bad hex (wrong chars, odd length) is nil ---
actual="$(run_case 'hex_decode(Digest.sha256("x")).length()')"
[[ "$actual" == "32" ]]
count=$((count + 1))

actual="$(run_case 'hex_decode("zz")')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

actual="$(run_case 'hex_decode("abc")')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- SignedCookies: sign/verify round-trip, wrong secret and tampered
# --- value both rejected as nil rather than raising ---
actual="$(run_case '
signed = SignedCookies.sign("user:42", "topsecret")
verified = SignedCookies.verify(signed, "topsecret")
wrong_secret = SignedCookies.verify(signed, "wrong")
"#{verified}|#{wrong_secret}"
')"
[[ "$actual" == "user:42|nil" ]]
count=$((count + 1))

actual="$(run_case 'SignedCookies.verify("garbage-no-dot", "topsecret")')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- EncryptedCookies: encrypt/decrypt round-trip, confidential (the
# --- cookie value never contains the plaintext), wrong secret and a
# --- hand-corrupted value both rejected as nil ---
actual="$(run_case '
enc = EncryptedCookies.encrypt("sensitive-session-data", "topsecret")
"#{enc.index_of("sensitive")}|#{EncryptedCookies.decrypt(enc, "topsecret")}"
')"
[[ "$actual" == "nil|sensitive-session-data" ]]
count=$((count + 1))

actual="$(run_case '
enc = EncryptedCookies.encrypt("sensitive-session-data", "topsecret")
EncryptedCookies.decrypt(enc, "wrong secret")
')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

actual="$(run_case 'EncryptedCookies.decrypt("hand-corrupted-not-real", "topsecret")')"
[[ "$actual" == "nil" ]]
count=$((count + 1))

# --- constant_time_equal: equal, different-content, and different-
# --- length pairs all compare correctly ---
actual="$(run_case '"#{constant_time_equal("abc", "abc")}|#{constant_time_equal("abc", "abd")}|#{constant_time_equal("abc", "ab")}"')"
[[ "$actual" == "true|false|false" ]]
count=$((count + 1))

# --- Csrf: .token mints once and is stable across calls on the same
# --- request/session; .valid? accepts the right token and rejects a
# --- missing session token, a missing submitted token, and a wrong one
actual="$(run_case '
request = {"session": {}}
first = Csrf.token(request)
second = Csrf.token(request)
"#{first == second}|#{first.length()}"
')"
[[ "$actual" == "true|64" ]]
count=$((count + 1))

actual="$(run_case '
request = {"session": {}}
token = Csrf.token(request)
"#{Csrf.valid?(request, token)}|#{Csrf.valid?(request, "wrong")}|#{Csrf.valid?(request, nil)}"
')"
[[ "$actual" == "true|false|false" ]]
count=$((count + 1))

actual="$(run_case 'Csrf.valid?({"session": {}}, "anything")')"
[[ "$actual" == "false" ]]
count=$((count + 1))

# --- Csrf.call: GET always passes through and mints a token; POST
# --- without/with-wrong/with-correct header is rejected/rejected/passed
actual="$(run_case '
def app(request, context)
  "handled:#{request["session"]["csrf_token"] != nil}"
end
request = {"method": "GET", "session": {}}
Csrf.call(request, {}, app)
')"
[[ "$actual" == "handled:true" ]]
count=$((count + 1))

actual="$(run_case '
def app(request, context)
  [200, {"Content-Type": "text/plain"}, "handled"]
end
session = {}
request = {"method": "GET", "headers": {}, "session": session}
Csrf.call(request, {}, app)
post_no_header = {"method": "POST", "headers": {}, "session": session}
[status, headers, body] = Csrf.call(post_no_header, {}, app)
post_wrong = {"method": "POST", "headers": {"x-csrf-token": "wrong"}, "session": session}
[status_wrong, h2, b2] = Csrf.call(post_wrong, {}, app)
post_right = {"method": "POST", "headers": {"x-csrf-token": session["csrf_token"]}, "session": session}
[status_right, h3, body_right] = Csrf.call(post_right, {}, app)
"#{status}|#{status_wrong}|#{status_right}|#{body_right}"
')"
[[ "$actual" == "403|403|200|handled" ]]
count=$((count + 1))

# --- Flash: a message set on one request is readable on the next one
# --- (via CookieSession's own rotation), then gone on the one after
# --- that; reading it back within the *same* request that set it stays
# --- nil, matching Rails' own flash[]= (not flash.now) behavior.
actual="$(run_case '
CookieSession.configure(secret: "flash-test-secret")

def set_and_read_same_request(request, context)
  Flash.set(request, "notice", "created!")
  same_request = Flash.get(request, "notice")
  [200, {}, "same_request=#{same_request}"]
end
def read_notice(request, context)
  [200, {}, "msg=#{Flash.get(request, "notice")}"]
end

req1 = {"headers": {}}
[status1, headers1, body1] = CookieSession.call(req1, {}, set_and_read_same_request)
cookie_pair = headers1["Set-Cookie"][0].split(";")[0]

req2 = {"headers": {"cookie": cookie_pair}}
[status2, headers2, body2] = CookieSession.call(req2, {}, read_notice)
cookie_pair2 = headers2["Set-Cookie"][0].split(";")[0]

req3 = {"headers": {"cookie": cookie_pair2}}
[status3, headers3, body3] = CookieSession.call(req3, {}, read_notice)

"#{body1}|#{body2}|#{body3}"
')"
[[ "$actual" == "same_request=nil|msg=created!|msg=nil" ]]
count=$((count + 1))

# --- Flash.all: every key set on the previous request, as a plain Hash
actual="$(run_case '
CookieSession.configure(secret: "flash-all-secret")

def set_two(request, context)
  Flash.set(request, "notice", "a")
  Flash.set(request, "error", "b")
  [200, {}, "ok"]
end
def read_all(request, context)
  all = Flash.all(request)
  [200, {}, "#{all["notice"]}|#{all["error"]}|#{all["missing"]}"]
end

req1 = {"headers": {}}
[status1, headers1, body1] = CookieSession.call(req1, {}, set_two)
cookie_pair = headers1["Set-Cookie"][0].split(";")[0]
req2 = {"headers": {"cookie": cookie_pair}}
[status2, headers2, body2] = CookieSession.call(req2, {}, read_all)
body2
')"
[[ "$actual" == "a|b|nil" ]]
count=$((count + 1))

# --- CookieSession/Csrf middleware, end to end against a real
# --- gremlin_serve instance: a first request gets a fresh session and a
# --- Set-Cookie; replaying that cookie on a second request restores the
# --- mutated session; a third request with a hand-corrupted cookie
# --- value falls back to a fresh session instead of erroring; an app
# --- that already sets its own Set-Cookie keeps both (packages/http's
# --- own multi-Set-Cookie support, exercised here for real); a POST
# --- without/with the right X-CSRF-Token header is rejected/accepted.
server_src() {
    local port="$1"
    cat <<SRCEOF
require "$(pwd)/../gremlin/lib/gremlin"
require "$(pwd)/lib/cookies"

CookieSession.configure(secret: "test-secret")

def app_handler(request, context)
  if request["path"] == "/flash"
    return [200, {"Set-Cookie": "flash=hi; Path=/"}, "ok"]
  end
  if request["path"] == "/csrf-token"
    return [200, {"Content-Type": "text/plain"}, "token=#{Csrf.token(request)}"]
  end
  if request["path"] == "/csrf-post"
    return [200, {"Content-Type": "text/plain"}, "posted"]
  end
  count = request["session"]["count"]
  count = if count == nil then 0 else count end
  request["session"]["count"] = count + 1
  [200, {"Content-Type": "text/plain"}, "count=#{count}"]
end

def csrf_protected(request, context)
  Csrf.call(request, context, app_handler)
end

def rack_app(request, context)
  CookieSession.call(request, context, csrf_protected)
end

gremlin_serve($port, rack_app)
SRCEOF
}

port=19430
timeout 10 "$diamond" -e "$(server_src "$port")" >/dev/null 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
response1="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$response1" "count=0"
count=$((count + 1))
assert_contains "$response1" $'\r\nSet-Cookie: _session='
count=$((count + 1))

set_cookie_line="$(grep -o 'Set-Cookie: _session=[^;]*' <<<"$response1")"
cookie_pair="${set_cookie_line#Set-Cookie: }"

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\nCookie: %s\r\n\r\n' "$cookie_pair" >&3
response2="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$response2" "count=1"
count=$((count + 1))

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\nCookie: _session=hand-corrupted\r\n\r\n' >&3
response3="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$response3" "count=0"
count=$((count + 1))

# An app handler that already sets its own Set-Cookie keeps both --
# packages/http's multi-Set-Cookie-lines support (Array header value),
# exercised for real over the wire here, not just at the middleware level.
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /flash HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
response4="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$response4" $'\r\nSet-Cookie: flash=hi; Path=/\r\n'
count=$((count + 1))
assert_contains "$response4" $'\r\nSet-Cookie: _session='
count=$((count + 1))

# Csrf.call, wired into the same chain, protects POST: a GET first
# fetches a session cookie and the token minted for it; a POST replaying
# the cookie but no/wrong X-CSRF-Token header is rejected, and one with
# the right header (read back out of the GET response body, the way a
# real page would embed it) is accepted.
exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'GET /csrf-token HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
csrf_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
csrf_set_cookie_line="$(grep -o 'Set-Cookie: _session=[^;]*' <<<"$csrf_response")"
csrf_cookie_pair="${csrf_set_cookie_line#Set-Cookie: }"
csrf_token="${csrf_response##*token=}"

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'POST /csrf-post HTTP/1.1\r\nHost: localhost\r\nCookie: %s\r\nContent-Length: 0\r\n\r\n' "$csrf_cookie_pair" >&3
response5="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$response5" "403"
count=$((count + 1))

exec 3<>"/dev/tcp/127.0.0.1/$port"
printf 'POST /csrf-post HTTP/1.1\r\nHost: localhost\r\nCookie: %s\r\nX-CSRF-Token: %s\r\nContent-Length: 0\r\n\r\n' "$csrf_cookie_pair" "$csrf_token" >&3
response6="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
assert_contains "$response6" "posted"
count=$((count + 1))

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo "$count cookies tests passed"
