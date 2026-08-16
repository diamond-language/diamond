#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-http-package` from the repo root, which sets this up already).
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

http_port=18743
http_out="$(mktemp)"
http_src="$(cat <<'HTTPEOF'
require "http"
def run()
  def handler(request)
    path = request["path"]
    [200, {"Content-Type": "text/plain"}, "hello, #{path}"]
  end
  http_serve(HTTP_PORT, handler)
end
run()
HTTPEOF
)"
http_src="${http_src/HTTP_PORT/$http_port}"
timeout 10 "$diamond" -e "$http_src" >"$http_out" 2>&1 &
http_pid=$!
wait_for_port "$http_port"
printf 'GET /world HTTP/1.1\r\nHost: localhost\r\n\r\n' >&3
http_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
kill "$http_pid" 2>/dev/null || true
wait "$http_pid" 2>/dev/null || true
[[ "$http_response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\nhello, /world' ]]
rm -f "$http_out"

http_port=18744
http_out="$(mktemp)"
http_src="$(cat <<'HTTPEOF'
require "http"
def run()
  def handler(request)
    method = request["method"]
    body = request["body"]
    [201, {"Content-Type": "text/plain"}, "#{method}: #{body}"]
  end
  http_serve(HTTP_PORT, handler)
end
run()
HTTPEOF
)"
http_src="${http_src/HTTP_PORT/$http_port}"
timeout 10 "$diamond" -e "$http_src" >"$http_out" 2>&1 &
http_pid=$!
wait_for_port "$http_port"
http_body='{"name":"diamond"}'
printf 'POST /items HTTP/1.1\r\nHost: localhost\r\nContent-Length: %d\r\n\r\n%s' \
    "${#http_body}" "$http_body" >&3
http_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
kill "$http_pid" 2>/dev/null || true
wait "$http_pid" 2>/dev/null || true
[[ "$http_response" == $'HTTP/1.1 201 Created\r\nContent-Type: text/plain\r\nContent-Length: 24\r\n\r\nPOST: {"name":"diamond"}' ]]
rm -f "$http_out"

# Lowercase request header name still parses (case-insensitive lookup).
http_port=18745
http_out="$(mktemp)"
http_src="$(cat <<'HTTPEOF'
require "http"
def run()
  def handler(request)
    body = request["body"]
    [200, {"Content-Type": "text/plain"}, "got: #{body}"]
  end
  http_serve(HTTP_PORT, handler)
end
run()
HTTPEOF
)"
http_src="${http_src/HTTP_PORT/$http_port}"
timeout 10 "$diamond" -e "$http_src" >"$http_out" 2>&1 &
http_pid=$!
wait_for_port "$http_port"
printf 'POST /items HTTP/1.1\r\nHost: localhost\r\ncontent-length: 5\r\n\r\nhello' >&3
http_response="$(cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
kill "$http_pid" 2>/dev/null || true
wait "$http_pid" 2>/dev/null || true
[[ "$http_response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\ngot: hello' ]]
rm -f "$http_out"

# Client: http_get/http_post against a real diamond-http server (not
# bash's own /dev/tcp trick, which only proves the wire format -- this
# proves http_request's own request-writing/response-parsing is
# correct end to end).
http_port=18746
http_out="$(mktemp)"
http_src="$(cat <<'HTTPEOF'
require "http"
def run()
  def handler(request)
    if request["method"] == "GET"
      [200, {"Content-Type": "text/plain"}, "hello, #{request["path"]}"]
    else
      [201, {"Content-Type": "text/plain"}, "posted: #{request["body"]}"]
    end
  end
  http_serve(HTTP_PORT, handler)
end
run()
HTTPEOF
)"
http_src="${http_src/HTTP_PORT/$http_port}"
timeout 10 "$diamond" -e "$http_src" >"$http_out" 2>&1 &
http_pid=$!
wait_for_port "$http_port"
# wait_for_port's own probe connection is left open on fd 3 -- unlike
# the earlier blocks, nothing here reuses it, so it must be closed or
# the single-threaded server blocks forever reading a request from it
# instead of accepting the real client connection below.
{ exec 3<&- 3>&-; } 2>/dev/null || true

client_src="$(cat <<CLIENTEOF
require "http"
response = http_get("http://127.0.0.1:$http_port/world")
puts(response["status"])
puts(response["headers"]["content-type"])
puts(response["body"])
post_response = http_post("http://127.0.0.1:$http_port/items", "hi there")
puts(post_response["status"])
puts(post_response["body"])
CLIENTEOF
)"
client_response="$(timeout 10 "$diamond" -e "$client_src")"
kill "$http_pid" 2>/dev/null || true
wait "$http_pid" 2>/dev/null || true
[[ "$client_response" == $'200\ntext/plain\nhello, /world\n201\nposted: hi there\nnil' ]]
rm -f "$http_out"

# Client: an https:// URL is rejected with a clear, rescuable error
# rather than silently connecting in the clear on port 443 -- no
# server needed, this never gets as far as opening a connection.
error_response="$(timeout 10 "$diamond" -e '
require "http"
begin
  http_get("https://example.com/")
rescue error: ArgumentError
  puts(error.message())
end
')"
[[ "$error_response" == $'unsupported URL scheme \'https\' (only http is supported)\nnil' ]]

# Server: a request declaring a Content-Length far past
# http_max_body_size (packages/http/http.di) is dropped -- connection
# closed, no attempt to read that many bytes -- rather than crashing
# the whole accept loop or trying to accumulate gigabytes of memory.
# The server must stay alive afterward: a second, ordinary connection
# right after proves the accept loop itself survived.
http_port=18747
http_out="$(mktemp)"
http_src="$(cat <<'HTTPEOF'
require "http"
def run()
  def handler(request)
    [200, {"Content-Type": "text/plain"}, "still alive"]
  end
  http_serve(HTTP_PORT, handler)
end
run()
HTTPEOF
)"
http_src="${http_src/HTTP_PORT/$http_port}"
timeout 10 "$diamond" -e "$http_src" >"$http_out" 2>&1 &
http_pid=$!
wait_for_port "$http_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
exec 3<>"/dev/tcp/127.0.0.1/$http_port"
printf 'POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999999\r\n\r\n' >&3
oversized_response="$(timeout 5 cat <&3)"
{ exec 3<&- 3>&-; } 2>/dev/null || true
[[ -z "$oversized_response" ]]
exec 4<>"/dev/tcp/127.0.0.1/$http_port"
printf 'GET / HTTP/1.1\r\nHost: localhost\r\n\r\n' >&4
recovery_response="$(timeout 5 cat <&4)"
{ exec 4<&- 4>&-; } 2>/dev/null || true
kill "$http_pid" 2>/dev/null || true
wait "$http_pid" 2>/dev/null || true
[[ "$recovery_response" == $'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 11\r\n\r\nstill alive' ]]
rm -f "$http_out"

# Client: a response declaring a Content-Length far past
# http_max_body_size raises rather than accumulating gigabytes of
# memory reading a malicious/misbehaving server's reply. The "server"
# here is a plain TCPServer (not http_serve), just enough to hand back
# one deliberately-malicious response.
oversized_body_port=18748
fake_server_out="$(mktemp)"
fake_server_src="$(cat <<'FAKEEOF'
server = TCPServer.listen(FAKE_PORT)
conn = server.accept()
conn.gets()
loop do
  line = conn.gets()
  if line == nil || line == ""
    break
  end
end
conn.write("HTTP/1.1 200 OK\r\nContent-Length: 99999999999\r\n\r\n")
conn.write("only a little data")
conn.close()
FAKEEOF
)"
fake_server_src="${fake_server_src/FAKE_PORT/$oversized_body_port}"
timeout 10 "$diamond" -e "$fake_server_src" >"$fake_server_out" 2>&1 &
fake_server_pid=$!
# Not wait_for_port: its probe opens a real connection that this
# script's own single accept() would consume (it handles exactly one
# connection and exits, unlike http_serve's loop above, which is why
# the other blocks in this file can use wait_for_port safely) --
# leaving no accept() left for the real client below. Listen+bind on
# the same binary that's already running is fast enough that a short
# fixed wait is reliable here.
sleep 0.3
oversized_body_response="$(timeout 10 "$diamond" -e "
require \"http\"
begin
  http_get(\"http://127.0.0.1:$oversized_body_port/\")
rescue error: IOError
  puts(error.message())
end
")"
wait "$fake_server_pid" 2>/dev/null || true
rm -f "$fake_server_out"
[[ "$oversized_body_response" == $'response Content-Length 99999999999 exceeds maximum of 10485760\nnil' ]]

echo "7 http package tests passed"
