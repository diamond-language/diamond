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
require "./lib/http"
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
require "./lib/http"
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
require "./lib/http"
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
require "./lib/http"
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
require "./lib/http"
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

# Client: https:// is a real, supported scheme now (TLSSocket.connect's
# own options Hash gives the client everything it needs -- a custom
# trust store here, for a hermetic self-signed test server instead of
# the real system trust store). A plain TCPServer standing in for a
# real https:// server would need its own TLS layer reimplemented, so
# this uses TLSServer.listen directly rather than http_serve (which has
# no TLS mode of its own).
tls_dir="$(mktemp -d)"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tls_dir/key.pem" \
    -out "$tls_dir/cert.pem" -days 1 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1
https_port=18749
https_server_out="$(mktemp)"
"$diamond" -e "$(printf 'listener = TLSServer.listen(%d, "%s", "%s")
puts("ready")
conn = listener.accept()
conn.gets()
loop do
  line = conn.gets()
  if line == nil || line == ""
    break
  end
end
conn.write("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nhttps: hi!")
conn.close()
listener.close()
0' "$https_port" "$tls_dir/cert.pem" "$tls_dir/key.pem")" >"$https_server_out" 2>&1 &
https_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$https_server_out" && break
    sleep 0.05
done
https_response="$(timeout 10 "$diamond" -e "$(printf 'require "./lib/http"
r = http_get("https://localhost:%d/", {}, {"ca_file": "%s"})
puts(r["status"])
puts(r["body"])' "$https_port" "$tls_dir/cert.pem")")"
wait "$https_server_pid"
[[ "$https_response" == $'200\nhttps: hi!\nnil' ]]
rm -f "$https_server_out"
rm -rf "$tls_dir"

# An unsupported scheme is still rejected with a clear, rescuable error
# rather than silently doing something unexpected.
error_response="$(timeout 10 "$diamond" -e '
require "./lib/http"
begin
  http_get("ftp://example.com/")
rescue error: ArgumentError
  puts(error.message())
end
')"
[[ "$error_response" == $'unsupported URL scheme \'ftp\' (only http/https are supported)\nnil' ]]

# Server: a request declaring a Content-Length far past
# http_max_body_size (packages/http/http.di) is dropped -- connection
# closed, no attempt to read that many bytes -- rather than crashing
# the whole accept loop or trying to accumulate gigabytes of memory.
# The server must stay alive afterward: a second, ordinary connection
# right after proves the accept loop itself survived.
http_port=18747
http_out="$(mktemp)"
http_src="$(cat <<'HTTPEOF'
require "./lib/http"
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
require \"./lib/http\"
begin
  http_get(\"http://127.0.0.1:$oversized_body_port/\")
rescue error: IOError
  puts(error.message())
end
")"
wait "$fake_server_pid" 2>/dev/null || true
rm -f "$fake_server_out"
[[ "$oversized_body_response" == $'response Content-Length 99999999999 exceeds maximum of 26214400\nnil' ]]

# Client: all the verb helpers, options["basic_auth"]/["bearer_token"]
# (Authorization header building), and options["json"] (auto-encoded
# body, auto-set Content-Type) against one real server.
verbs_port=19310
verbs_out="$(mktemp)"
verbs_src="$(cat <<'HTTPEOF'
require "./lib/http"
def run()
  def handler(request)
    method = request["method"]
    path = request["path"]
    headers = request["headers"]
    body = request["body"]
    if path == "/auth"
      [200, {"Content-Type": "text/plain"}, headers["authorization"]]
    elsif path == "/echo"
      [200, {"Content-Type": "text/plain"}, "#{method}:#{headers["content-type"]}:#{body}"]
    elsif method == "PUT"
      [200, {"Content-Type": "text/plain"}, "put:#{body}"]
    elsif method == "PATCH"
      [200, {"Content-Type": "text/plain"}, "patch:#{body}"]
    elsif method == "DELETE"
      [204, {}, ""]
    elsif method == "HEAD"
      [200, {"Content-Type": "text/plain", "Content-Length": "5"}, ""]
    elsif method == "OPTIONS"
      [200, {"Allow": "GET, POST"}, ""]
    else
      [404, {}, "nf"]
    end
  end
  http_serve(VERBS_PORT, handler)
end
run()
HTTPEOF
)"
verbs_src="${verbs_src/VERBS_PORT/$verbs_port}"
timeout 10 "$diamond" -e "$verbs_src" >"$verbs_out" 2>&1 &
verbs_pid=$!
wait_for_port "$verbs_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
verbs_response="$(timeout 10 "$diamond" -e "$(cat <<CLIENTEOF
require "./lib/http"
puts(http_put("http://127.0.0.1:$verbs_port/x", "putbody")["body"])
puts(http_patch("http://127.0.0.1:$verbs_port/x", "patchbody")["body"])
puts(http_delete("http://127.0.0.1:$verbs_port/x")["status"])
head_response = http_head("http://127.0.0.1:$verbs_port/x")
puts(head_response["status"])
puts(head_response["body"] == "")
puts(http_options("http://127.0.0.1:$verbs_port/x")["headers"]["allow"])
puts(http_get("http://127.0.0.1:$verbs_port/auth", {}, {"basic_auth": {"user": "alice", "password": "secret"}})["body"])
puts(http_get("http://127.0.0.1:$verbs_port/auth", {}, {"bearer_token": "tok123"})["body"])
puts(http_post("http://127.0.0.1:$verbs_port/echo", "", {}, {"json": {"a": 1}})["body"])
CLIENTEOF
)")"
kill "$verbs_pid" 2>/dev/null || true
wait "$verbs_pid" 2>/dev/null || true
expected_verbs=$'put:putbody\npatch:patchbody\n204\n200\ntrue\nGET, POST\nBasic YWxpY2U6c2VjcmV0\nBearer tok123\nPOST:application/json:{"a":1}\nnil'
[[ "$verbs_response" == "$expected_verbs" ]]
rm -f "$verbs_out"

# Client: multipart file upload, encoded by http_post's own
# options["multipart_fields"]/["multipart_files"] and decoded by
# packages/multipart's multipart_parse on the server side -- a real
# round trip between this package's encoder and the other package's
# parser, not just a wire-format assumption.
upload_port=19311
upload_out="$(mktemp)"
upload_src="$(cat <<'HTTPEOF'
require "./lib/http"
require "../multipart/lib/multipart"
def run()
  def handler(request)
    parsed = multipart_parse(request)
    if parsed == nil
      [400, {}, "bad multipart"]
    else
      file = parsed["files"]["doc"]
      [200, {"Content-Type": "text/plain"},
        "name=#{parsed["fields"]["name"]} file=#{file["filename"]}:#{file["content_type"]}:#{file["data"]}"]
    end
  end
  http_serve(UPLOAD_PORT, handler)
end
run()
HTTPEOF
)"
upload_src="${upload_src/UPLOAD_PORT/$upload_port}"
timeout 10 "$diamond" -e "$upload_src" >"$upload_out" 2>&1 &
upload_pid=$!
wait_for_port "$upload_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
upload_response="$(timeout 10 "$diamond" -e "$(cat <<CLIENTEOF
require "./lib/http"
r = http_post("http://127.0.0.1:$upload_port/upload", "", {}, {
  "multipart_fields": {"name": "alice"},
  "multipart_files": {"doc": {"filename": "hello.txt", "content_type": "text/plain", "data": "file contents"}}
})
puts(r["status"])
puts(r["body"])
CLIENTEOF
)")"
kill "$upload_pid" 2>/dev/null || true
wait "$upload_pid" 2>/dev/null || true
[[ "$upload_response" == $'200\nname=alice file=hello.txt:text/plain:file contents\nnil' ]]
rm -f "$upload_out"

# Client: options["gzip"] (the default) transparently decompresses a
# Content-Encoding: gzip response -- Gzip.compress produces the body,
# http_serve treats it as any other opaque String body (already
# binary-safe), so this is a real gzip payload over real HTTP, not a
# simulated one.
gzip_port=19312
gzip_out="$(mktemp)"
gzip_src="$(cat <<'HTTPEOF'
require "./lib/http"
def run()
  def handler(request)
    plain = "hello gzip world "
    i = 0
    while i < 20
      plain = plain + "hello gzip world "
      i = i + 1
    end
    [200, {"Content-Type": "text/plain", "Content-Encoding": "gzip"}, Gzip.compress(plain)]
  end
  http_serve(GZIP_PORT, handler)
end
run()
HTTPEOF
)"
gzip_src="${gzip_src/GZIP_PORT/$gzip_port}"
timeout 10 "$diamond" -e "$gzip_src" >"$gzip_out" 2>&1 &
gzip_pid=$!
wait_for_port "$gzip_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
gzip_response="$(timeout 10 "$diamond" -e "$(cat <<CLIENTEOF
require "./lib/http"
r = http_get("http://127.0.0.1:$gzip_port/")
puts(r["headers"]["content-encoding"])
puts(r["body"].start_with?("hello gzip world hello gzip world "))
CLIENTEOF
)")"
kill "$gzip_pid" 2>/dev/null || true
wait "$gzip_pid" 2>/dev/null || true
[[ "$gzip_response" == $'gzip\ntrue\nnil' ]]
rm -f "$gzip_out"

# Client: Transfer-Encoding: chunked decoding -- http_serve has no
# chunked mode of its own (see this file's own top-of-file comment), so
# this uses a plain TCPServer to hand-write a chunked response, the same
# "prove the wire format directly" approach the oversized-Content-Length
# test above uses.
chunked_port=19313
chunked_out="$(mktemp)"
"$diamond" -e "$(printf 'server = TCPServer.listen(%d)
conn = server.accept()
conn.gets()
loop do
  line = conn.gets()
  if line == nil || line == ""
    break
  end
end
conn.write("HTTP/1.1 200 OK\\r\\nTransfer-Encoding: chunked\\r\\n\\r\\n")
conn.write("5\\r\\nhello\\r\\n")
conn.write("7\\r\\n world!\\r\\n")
conn.write("0\\r\\n\\r\\n")
conn.close()
0' "$chunked_port")" >"$chunked_out" 2>&1 &
chunked_pid=$!
sleep 0.3
chunked_response="$(timeout 10 "$diamond" -e "$(printf 'require "./lib/http"
r = http_get("http://127.0.0.1:%d/")
puts(r["status"])
puts(r["body"])' "$chunked_port")")"
wait "$chunked_pid" 2>/dev/null || true
[[ "$chunked_response" == $'200\nhello world!\nnil' ]]
rm -f "$chunked_out"

# Client: options["follow_redirects"] -- off by default (a plain 302
# comes back as-is), on follows it, downgrading a non-GET/HEAD method to
# a bodyless GET for 301/302/303 while 307/308 preserve method and body.
redirect_port=19314
redirect_out="$(mktemp)"
redirect_src="$(cat <<'HTTPEOF'
require "./lib/http"
def run()
  def handler(request)
    path = request["path"]
    method = request["method"]
    if path == "/start"
      [302, {"Location": "/final"}, ""]
    elsif path == "/final" && method == "GET"
      [200, {"Content-Type": "text/plain"}, "redirected"]
    elsif path == "/preserve" && method == "PUT"
      [307, {"Location": "/preserve2"}, ""]
    elsif path == "/preserve2" && method == "PUT"
      [200, {"Content-Type": "text/plain"}, "preserved:#{request["body"]}"]
    else
      [404, {}, "nf"]
    end
  end
  http_serve(REDIRECT_PORT, handler)
end
run()
HTTPEOF
)"
redirect_src="${redirect_src/REDIRECT_PORT/$redirect_port}"
timeout 10 "$diamond" -e "$redirect_src" >"$redirect_out" 2>&1 &
redirect_pid=$!
wait_for_port "$redirect_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
redirect_response="$(timeout 10 "$diamond" -e "$(cat <<CLIENTEOF
require "./lib/http"
not_followed = http_get("http://127.0.0.1:$redirect_port/start")
puts(not_followed["status"])
followed = http_post("http://127.0.0.1:$redirect_port/start", "ignored", {}, {"follow_redirects": true})
puts(followed["status"])
puts(followed["body"])
preserved = http_put("http://127.0.0.1:$redirect_port/preserve", "keepme", {}, {"follow_redirects": true})
puts(preserved["body"])
CLIENTEOF
)")"
kill "$redirect_pid" 2>/dev/null || true
wait "$redirect_pid" 2>/dev/null || true
[[ "$redirect_response" == $'302\n200\nredirected\npreserved:keepme\nnil' ]]
rm -f "$redirect_out"

# Client: HttpSession's cookie jar (a Set-Cookie value comes back on
# every later request to the same host) and connection reuse (a second
# call over the cached connection after the server itself has already
# closed its own end must transparently reconnect rather than raising).
session_port=19315
session_out="$(mktemp)"
session_src="$(cat <<'HTTPEOF'
require "./lib/http"
def run()
  def handler(request)
    if request["path"] == "/login"
      [200, {"Content-Type": "text/plain", "Set-Cookie": "sid=abc123; Path=/"}, "logged in"]
    else
      [200, {"Content-Type": "text/plain"}, "cookie:#{request["headers"]["cookie"]}"]
    end
  end
  http_serve(SESSION_PORT, handler)
end
run()
HTTPEOF
)"
session_src="${session_src/SESSION_PORT/$session_port}"
timeout 10 "$diamond" -e "$session_src" >"$session_out" 2>&1 &
session_pid=$!
wait_for_port "$session_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
session_response="$(timeout 10 "$diamond" -e "$(cat <<CLIENTEOF
require "./lib/http"
session = HttpSession.new()
puts(session.get("http://127.0.0.1:$session_port/login")["body"])
puts(session.get("http://127.0.0.1:$session_port/whoami")["body"])
session.close()
CLIENTEOF
)")"
kill "$session_pid" 2>/dev/null || true
wait "$session_pid" 2>/dev/null || true
[[ "$session_response" == $'logged in\ncookie:sid=abc123\n{}' ]]
rm -f "$session_out"

reuse_port=19316
reuse_out="$(mktemp)"
"$diamond" -e "$(printf 'server = TCPServer.listen(%d)
i = 0
while i < 2
  conn = server.accept()
  conn.gets()
  loop do
    line = conn.gets()
    if line == nil || line == ""
      break
    end
  end
  conn.write("HTTP/1.1 200 OK\\r\\nContent-Type: text/plain\\r\\nContent-Length: 5\\r\\n\\r\\nresp#{i}")
  conn.close()
  i = i + 1
end
0' "$reuse_port")" >"$reuse_out" 2>&1 &
reuse_pid=$!
sleep 0.3
reuse_response="$(timeout 10 "$diamond" -e "$(printf 'require "./lib/http"
session = HttpSession.new()
puts(session.get("http://127.0.0.1:%d/")["body"])
puts(session.get("http://127.0.0.1:%d/")["body"])
session.close()' "$reuse_port" "$reuse_port")")"
wait "$reuse_pid" 2>/dev/null || true
[[ "$reuse_response" == $'resp0\nresp1\n{}' ]]
rm -f "$reuse_out"

echo "16 http package tests passed"
