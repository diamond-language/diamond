#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-network-safety-package` from the repo root, which sets this up
# already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/network_safety\"
$script"
}

assert_eq() {
    local actual="$1" expected="$2"
    if [[ "$actual" != "$expected" ]]; then
        echo "expected '$expected', got '$actual'" >&2
        exit 1
    fi
    count=$((count + 1))
}

# --- a real public HTTP(S) URL parses through untouched ---
actual="$(run_case '
parsed = resolve_public_hostname("https://api.webamp.org/graphql")
"#{parsed["scheme"]} #{parsed["host"]} #{parsed["port"]} #{parsed["path"]}"
')"
assert_eq "$actual" "https api.webamp.org 443 /graphql"

actual="$(run_case '
parsed = resolve_public_hostname("http://example.com:8080/download")
"#{parsed["host"]} #{parsed["port"]}"
')"
assert_eq "$actual" "example.com 8080"

# --- an unsupported/missing scheme is rejected (http_parse_url's own
# --- check, surfaced through resolve_public_hostname unchanged) ---
actual="$(run_case '
begin
  resolve_public_hostname("ftp://example.com/x")
  "no error"
rescue error: ArgumentError
  "rejected"
end
')"
assert_eq "$actual" "rejected"

# --- embedded userinfo is rejected, and doesn't get a chance to
# --- silently corrupt the parsed host first ---
actual="$(run_case '
begin
  resolve_public_hostname("http://user:pass@evil.com/x")
  "no error"
rescue error: ArgumentError
  "rejected"
end
')"
assert_eq "$actual" "rejected"

# --- a literal IPv6 host is rejected outright, not misparsed ---
actual="$(run_case '
begin
  resolve_public_hostname("http://[::1]/x")
  "no error"
rescue error: ArgumentError
  "rejected"
end
')"
assert_eq "$actual" "rejected"

# --- every documented private/reserved/loopback/link-local IPv4
# --- literal is rejected -- one representative host per blocked
# --- network, plus one address from just outside each range's
# --- boundary that must NOT be rejected ---
blocked_hosts=("0.1.2.3" "10.5.5.5" "100.64.0.1" "127.0.0.1" "169.254.1.1"
  "172.16.5.5" "192.0.0.5" "192.0.2.5" "192.168.1.1" "198.18.0.5"
  "198.51.100.5" "203.0.113.5" "224.0.0.1" "240.0.0.1")
for host in "${blocked_hosts[@]}"; do
    actual="$(run_case "
begin
  resolve_public_hostname(\"http://$host/x\")
  \"no error\"
rescue error: ArgumentError
  \"rejected\"
end
")"
    assert_eq "$actual" "rejected"
done

allowed_hosts=("8.8.8.8" "1.1.1.1" "93.184.216.34")
for host in "${allowed_hosts[@]}"; do
    actual="$(run_case "resolve_public_hostname(\"http://$host/x\")[\"host\"]")"
    assert_eq "$actual" "$host"
done

# --- "localhost" and a .local/.internal suffix are rejected by name,
# --- not just by IP literal ---
actual="$(run_case '
begin
  resolve_public_hostname("http://localhost/x")
  "no error"
rescue error: ArgumentError
  "rejected"
end
')"
assert_eq "$actual" "rejected"

actual="$(run_case '
begin
  resolve_public_hostname("http://printer.local/x")
  "no error"
rescue error: ArgumentError
  "rejected"
end
')"
assert_eq "$actual" "rejected"

actual="$(run_case '
begin
  resolve_public_hostname("http://db.internal/x")
  "no error"
rescue error: ArgumentError
  "rejected"
end
')"
assert_eq "$actual" "rejected"

# --- network_safety_ipv4_to_int: rejects non-canonical/garbage forms
# --- (leading zero, out-of-range octet, wrong segment count, non-
# --- numeric) as well as accepting a real address ---
actual="$(run_case '"#{network_safety_ipv4_to_int("1.2.3.4")}|#{network_safety_ipv4_to_int("01.2.3.4")}|#{network_safety_ipv4_to_int("1.2.3.256")}|#{network_safety_ipv4_to_int("1.2.3")}|#{network_safety_ipv4_to_int("a.b.c.d")}"')"
assert_eq "$actual" "16909060|nil|nil|nil|nil"

echo "$count network_safety tests passed"
