#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-websocket-package` from the repo root, which sets this up
# already). The live-server section below also requires `node` on
# PATH -- Node's own global `WebSocket` client (no install needed, as
# of Node 22+) is the one piece of real, standards-compliant, non-
# Diamond WebSocket tooling available here, so it's what proves actual
# RFC 6455 interop rather than just this package's own encoder agreeing
# with its own decoder.
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

# Part 1: pure protocol-level checks -- no sockets, no gremlin_serve,
# just frame.di/handshake.di/connection.di exercised directly against a
# small in-memory fake connection. Anything that doesn't need a real
# byte stream lives here rather than in the slower, flakier live-server
# section below.
unit_out="$(mktemp)"
if ! "$diamond" -e "$(cat <<'DIEOF'
require "./lib/websocket"

def check(condition, message)
  unless condition
    raise StandardError.new(message)
  end
end

def repeat(ch, n)
  sb = StringBuilder.new()
  i = 0
  while i < n
    sb.append(ch)
    i = i + 1
  end
  sb.to_s()
end

# A fake connection that serves #read(n) from a fixed input buffer and
# collects every #write(value) into its own buffer -- enough of
# NonblockingConnection's own contract for both directions of
# WebSocketConnection to run against without a real socket. #read never
# blocks or signals EOF the way the real thing does (there is no fiber
# to yield to here) -- it just returns whatever is left, which is
# exactly what makes "ran out of input" show up as
# websocket_read_exact's own short-read IOError, itself a useful signal
# in a couple of the checks below.
class FakeConn
  def initialize(input)
    @input = input
    @written = StringBuilder.new()
    @closed = false
  end
  def read(n)
    took = if n > @input.length() then @input.length() else n end
    result = @input.slice(0, took)
    @input = @input.slice(took, @input.length())
    result
  end
  def write(value)
    @written.append(value)
  end
  def written() = @written.to_s()
  def close()
    @closed = true
  end
  def closed?() = @closed
end

# RFC 6455 section 1.3's own worked handshake example.
check(
  websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
  "accept key does not match RFC 6455's own worked example"
)

req_ok = {"headers": {"upgrade": "websocket", "connection": "Upgrade", "sec-websocket-key": "dGhlIHNhbXBsZSBub25jZQ=="}}
req_bad_connection = {"headers": {"upgrade": "websocket", "connection": "keep-alive", "sec-websocket-key": "dGhlIHNhbXBsZSBub25jZQ=="}}
req_bad_upgrade = {"headers": {"upgrade": "h2c", "connection": "Upgrade", "sec-websocket-key": "dGhlIHNhbXBsZSBub25jZQ=="}}
req_plain = {"headers": {}}
check(websocket_upgrade_request?(req_ok), "expected true for a real upgrade request")
check(!websocket_upgrade_request?(req_bad_connection), "expected false without an 'upgrade' Connection token")
check(!websocket_upgrade_request?(req_bad_upgrade), "expected false for a non-websocket Upgrade header")
check(!websocket_upgrade_request?(req_plain), "expected false for a plain request with no relevant headers")

# websocket_accept writes the real 101 response bytes and rejects a
# non-upgrade request outright.
handshake_conn = FakeConn.new("")
websocket_accept(handshake_conn, req_ok)
check(handshake_conn.written().start_with?("HTTP/1.1 101 Switching Protocols\r\n"), "expected a 101 response")
check(
  handshake_conn.written().index_of("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != nil,
  "expected the correctly-computed Accept header"
)
rejected_non_upgrade = false
begin
  websocket_accept(FakeConn.new(""), req_plain)
rescue error: ArgumentError
  rejected_non_upgrade = true
end
check(rejected_non_upgrade, "expected websocket_accept to reject a non-upgrade request")

# Frame round trip at several sizes, crossing both extended-length
# boundaries (a 126-byte payload selects the 16-bit length form, a
# 65536-byte payload the 64-bit form).
def check_round_trip(text)
  encoded = websocket_encode_frame(websocket_opcode_text(), text, true)
  frame = websocket_read_frame(FakeConn.new(encoded))
  check(frame["opcode"] == websocket_opcode_text(), "round trip: wrong opcode")
  check(frame["fin"] == true, "round trip: expected fin")
  check(frame["payload"] == text, "round trip: payload mismatch (#{frame["payload"].length()} vs #{text.length()} bytes)")
end
check_round_trip("")
check_round_trip("hello")
check_round_trip(repeat("x", 125))
check_round_trip(repeat("x", 126))
check_round_trip(repeat("x", 70000))

# An unmasked "client" frame must be rejected (RFC 6455 section 5.1).
unmasked_frame = websocket_encode_frame(websocket_opcode_text(), "hi", false)
unmasked_rejected = false
begin
  websocket_read_frame(FakeConn.new(unmasked_frame))
rescue error: IOError
  unmasked_rejected = true
end
check(unmasked_rejected, "expected an unmasked frame to be rejected")

# A frame claiming a payload length past websocket_max_frame_size()
# must be rejected before ever trying to read that many bytes -- built
# by hand (just the 10-byte header) rather than via
# websocket_encode_frame, since the whole point is never allocating a
# payload this large in the first place.
oversized_header = StringBuilder.new()
oversized_header.append(chr(128 | websocket_opcode_text()))
oversized_header.append(chr(128 | 127))
big_length = 32 * 1024 * 1024
shift = 56
while shift >= 0
  oversized_header.append(chr((big_length >> shift) & 255))
  shift = shift - 8
end
oversized_rejected = false
begin
  websocket_read_frame(FakeConn.new(oversized_header.to_s()))
rescue error: IOError
  oversized_rejected = true
end
check(oversized_rejected, "expected an oversized frame length to be rejected")

# Ping is answered with Pong transparently, with the same payload,
# never surfaced from #receive itself.
conn = FakeConn.new(websocket_encode_frame(websocket_opcode_ping(), "ping-payload", true))
ws = WebSocketConnection.new(conn)
ping_answered = false
begin
  ws.receive()
rescue error: IOError
  # #receive loops back for the *next* frame after answering the Ping,
  # and the fake input is now exhausted -- exactly the short-read
  # IOError websocket_read_exact raises, not a bug in the Ping handling
  # itself. What matters here is what got written before this point.
  ping_answered = true
end
check(ping_answered, "expected #receive to keep looping (and hit end of fake input) after answering a Ping")
pong_frame = websocket_read_frame_bytes(FakeConn.new(conn.written()))
check(pong_frame["opcode"] == websocket_opcode_pong(), "expected a Pong reply to a Ping")
check(pong_frame["masked"] == false, "a server-sent frame must never be masked")
check(pong_frame["payload"] == "ping-payload", "Pong payload should echo the Ping's own payload")

# A fragmented message (fin: false, then a continuation frame with
# fin: true) is reassembled into one message, keeping the original
# TEXT/BINARY opcode from the first fragment.
frag1 = websocket_encode_frame(websocket_opcode_text(), "Hello, ", true, false)
frag2 = websocket_encode_frame(websocket_opcode_continuation(), "World!", true, true)
conn = FakeConn.new(frag1 + frag2)
ws = WebSocketConnection.new(conn)
message = ws.receive()
check(message["opcode"] == websocket_opcode_text(), "reassembled message should keep the first fragment's TEXT opcode")
check(message["data"] == "Hello, World!", "fragments were not reassembled correctly")

# A continuation frame with no message in progress is a protocol error.
stray_continuation = websocket_encode_frame(websocket_opcode_continuation(), "stray", true)
ws = WebSocketConnection.new(FakeConn.new(stray_continuation))
stray_rejected = false
begin
  ws.receive()
rescue error: IOError
  stray_rejected = true
end
check(stray_rejected, "expected a stray continuation frame (no message in progress) to be rejected")

# Peer-initiated close (arrives via #receive): answered with this
# side's own Close frame, #receive returns nil, and the underlying
# connection is closed.
close_payload = "#{chr(3)}#{chr(232)}bye"
conn = FakeConn.new(websocket_encode_frame(websocket_opcode_close(), close_payload, true))
ws = WebSocketConnection.new(conn)
result = ws.receive()
check(result == nil, "expected #receive to return nil once the peer initiated a close")
check(conn.closed?(), "expected the underlying connection to be closed after a peer-initiated close")
close_reply = websocket_read_frame_bytes(FakeConn.new(conn.written()))
check(close_reply["opcode"] == websocket_opcode_close(), "expected this side to reply with its own Close frame")

# This-side-initiated close (#close): sends a Close frame, waits for
# the peer's own Close frame back, then closes the underlying
# connection.
peer_close_reply = websocket_encode_frame(websocket_opcode_close(), "", true)
conn = FakeConn.new(peer_close_reply)
ws = WebSocketConnection.new(conn)
ws.close()
check(conn.closed?(), "expected the underlying connection to be closed after the close handshake completes")
sent_close = websocket_read_frame_bytes(FakeConn.new(conn.written()))
check(sent_close["opcode"] == websocket_opcode_close(), "expected this side to have sent its own Close frame first")
check(sent_close["masked"] == false, "a server-sent Close frame must never be masked")

puts("all unit checks passed")
DIEOF
)" >"$unit_out" 2>&1; then
    cat "$unit_out"
    rm -f "$unit_out"
    exit 1
fi
grep -q "all unit checks passed" "$unit_out"
rm -f "$unit_out"
echo "websocket unit checks passed"

# Part 2: a live gremlin_serve echo server, talked to over a real TCP
# socket by Node's own standards-compliant global WebSocket client --
# the one thing part 1 above can't prove, since every check there is
# this package talking to itself.
if ! command -v node >/dev/null 2>&1; then
    echo "node not found on PATH -- skipping live interop test" >&2
    exit 0
fi

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

port=19420
out="$(mktemp)"
cat >"$out.di" <<DIEOF
require "$(pwd)/../gremlin/lib/gremlin"
require "$(pwd)/lib/websocket"
def run()
  def handler(request, context)
    if request["path"] == "/echo" && websocket_upgrade_request?(request)
      ws = websocket_accept(context["gremlin_connection"], request)
      loop do
        message = ws.receive()
        if message == nil
          break
        end
        if message["opcode"] == websocket_opcode_text()
          ws.send_text(message["data"])
        else
          ws.send_binary(message["data"])
        end
      end
      nil
    else
      [404, {"Content-Type": "text/plain"}, "not found"]
    end
  end
  gremlin_serve($port, handler)
end
run()
DIEOF
timeout 10 "$diamond" "$out.di" >"$out" 2>&1 &
pid=$!
wait_for_port "$port"
{ exec 3<&- 3>&-; } 2>/dev/null || true

node_out="$(mktemp)"
node -e "
const ws = new WebSocket('ws://127.0.0.1:$port/echo');
const timer = setTimeout(() => { console.error('timed out'); process.exit(1); }, 5000);
ws.addEventListener('open', () => { ws.send('hello from node'); });
ws.addEventListener('message', (event) => {
  if (event.data !== 'hello from node') {
    console.error('unexpected echo: ' + event.data);
    process.exit(1);
  }
  ws.close(1000, 'done');
});
ws.addEventListener('close', (event) => {
  clearTimeout(timer);
  if (event.code !== 1000) {
    console.error('unexpected close code: ' + event.code);
    process.exit(1);
  }
  console.log('interop round trip OK');
  process.exit(0);
});
ws.addEventListener('error', (event) => {
  console.error('websocket error: ' + event.message);
  process.exit(1);
});
" >"$node_out" 2>&1
node_status=$?
cat "$node_out"
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
rm -f "$out" "$out.di" "$node_out"
if [[ "$node_status" != "0" ]]; then
    exit 1
fi

echo "websocket live interop test passed"
