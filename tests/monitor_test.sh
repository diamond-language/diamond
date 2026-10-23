#!/usr/bin/env bash
# applications/registry/monitor.d6 against a local TLS server: a healthy registry passes, and one that trickles its response
# body a byte at a time (just inside every socket timeout) is cut off by the overall deadline instead of holding the probe open.
set -euo pipefail
cd "$(dirname "$0")/.."
diamond="${DIAMOND_BIN:-./build/diamond}"
work="$(mktemp -d)"
server_pid=""
cleanup() { [[ -n "$server_pid" ]] && kill "$server_pid" 2>/dev/null || true; rm -rf "$work"; }
trap cleanup EXIT
export DIAMOND_NO_CACHE=1

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$work/server.key" -out "$work/server.pem" -days 2 \
    -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1

cat > "$work/server.d6" <<'DIEOF'
mode = ARGV[0]
port = ARGV[1].to_i()
listener = TLSServer.listen(port, ARGV[2], ARGV[3], {})
File.write(ARGV[4], "ready")
conn = listener.accept()
loop do
  line = conn.gets()
  break if line == nil || line.strip() == ""
end
if mode == "ok"
  body = "{\"protocol\": 1, \"status\": \"ok\"}"
  conn.write("HTTP/1.1 200 OK\r\nContent-Length: #{body.length()}\r\nConnection: close\r\n\r\n#{body}")
else
  conn.write("HTTP/1.1 200 OK\r\nContent-Length: 4096\r\nConnection: close\r\n\r\n")
  sent = 0
  while sent < 120
    conn.write("x")
    IO.poll([], [], 300)
    sent += 1
  end
end
conn.close()
DIEOF

run_server() { # mode port
    rm -f "$work/ready"
    "$diamond" "$work/server.d6" "$1" "$2" "$work/server.pem" "$work/server.key" "$work/ready" >"$work/server.log" 2>&1 &
    server_pid=$!
    for _ in $(seq 1 100); do [[ -f "$work/ready" ]] && return 0; sleep 0.1; done
    echo "monitor_test: the $1 server did not start" >&2; cat "$work/server.log" >&2; exit 1
}

probe() { # port -> prints "<exit status>|<record>|<seconds>"
    local started status=0 record
    started=$(date +%s)
    record="$("$diamond" applications/registry/monitor.d6 "https://localhost:$1" --ca-file "$work/server.pem" --timeout 1)" || status=$?
    printf '%s|%s|%s' "$status" "$record" "$(( $(date +%s) - started ))"
}

run_server ok 18321
result="$(probe 18321)"
kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; server_pid=""
[[ "$result" == 0\|'{"ok":true,"duration_ms":'* ]] || { echo "monitor_test: healthy probe: $result" >&2; exit 1; }

run_server trickle 18322
result="$(probe 18322)"
kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; server_pid=""
seconds="${result##*|}"
[[ "$result" == 1\|'{"ok":false,"error":"Timeout",'* ]] || { echo "monitor_test: trickle probe: $result" >&2; exit 1; }
(( seconds <= 12 )) || { echo "monitor_test: the trickling server held the probe for ${seconds}s" >&2; exit 1; }

echo "monitor tests passed"
