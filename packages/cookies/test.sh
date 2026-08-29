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

# --- CookieSession middleware, end to end against a real gremlin_serve
# --- instance: a first request gets a fresh session and a Set-Cookie;
# --- replaying that cookie on a second request restores the mutated
# --- session; a third request with a hand-corrupted cookie value falls
# --- back to a fresh session instead of erroring; an app that already
# --- sets its own Set-Cookie keeps both (packages/http's own
# --- multi-Set-Cookie support, exercised here for real).
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
  count = request["session"]["count"]
  count = if count == nil then 0 else count end
  request["session"]["count"] = count + 1
  [200, {"Content-Type": "text/plain"}, "count=#{count}"]
end

def rack_app(request, context)
  CookieSession.call(request, context, app_handler)
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

kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true

echo "$count cookies tests passed"
