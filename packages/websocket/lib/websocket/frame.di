# RFC 6455 section 5.2 wire framing -- opcode constants plus raw
# frame encode/decode. Everything here operates on plain Diamond
# `String`s (already a raw, binary-safe byte buffer -- docs/local-io.md)
# and the new Int bitwise operators (`&`, `|`, `^`, `>>`) that landed on
# `main` specifically to make this practical to write in pure Diamond
# rather than needing a native VM implementation (see
# tests/cases/bitwise_operators.di).

# Plain top-level `NAME = value` assignments aren't visible from inside
# a `def` body anywhere else in this codebase (confirmed directly --
# packages/http/lib/http/status.di's own http_max_body_size is a
# zero-argument function for exactly this reason, not a bare constant),
# so every fixed value used across more than one function here is one
# too.
def websocket_opcode_continuation() = 0
def websocket_opcode_text() = 1
def websocket_opcode_binary() = 2
def websocket_opcode_close() = 8
def websocket_opcode_ping() = 9
def websocket_opcode_pong() = 10

# A frame whose declared payload length is larger than this is rejected
# (IOError) before ever attempting to read that many bytes off the
# connection. RFC 6455's 64-bit extended-length field lets a hostile or
# just-broken peer claim an arbitrary, effectively unbounded length --
# reading that many bytes into one String would be an easy way to
# exhaust memory long before any higher-level message-size limit gets a
# chance to reject it. packages/http's own http_max_body_size is the
# precedent for capping a length field before ever trusting it.
def websocket_max_frame_size() = 16 * 1024 * 1024

# XOR-cycles `key` (always exactly 4 bytes) over `data`, byte by byte --
# RFC 6455 section 5.3's masking algorithm. Its own inverse: masking and
# unmasking are the identical operation.
def websocket_mask(data, key)
  sb = StringBuilder.new()
  index = 0
  while index < data.length()
    key_byte = key[index % 4].ord()
    sb.append(chr(data[index].ord() ^ key_byte))
    index = index + 1
  end
  sb.to_s()
end

# Builds one frame's raw bytes. `fin` defaults true (a complete,
# unfragmented frame) since WebSocketConnection itself never sends a
# fragmented message -- every outgoing message is already fully built in
# memory before it's sent -- but is a real parameter, not hardcoded,
# so this package's own test suite can build a deliberately-fragmented
# two-frame message (fin: false, then a websocket_opcode_continuation()
# frame with fin: true) to exercise WebSocketConnection#receive's
# reassembly path.
#
# `masked` is only ever passed `true` from this package's own test
# suite: a real *server* frame must never be masked (RFC 6455 section
# 5.1) -- WebSocketConnection never passes anything but the default.
# Setting it true draws a fresh random 4-byte key from SecureRandom and
# masks the payload, letting the test suite build a spec-compliant
# masked client frame to exercise websocket_read_frame's read side
# without a second, real WebSocket implementation to talk to.
def websocket_encode_frame(opcode, payload, masked = false, fin = true)
  fin_bit = if fin then 128 else 0 end
  byte0 = fin_bit | opcode
  length = payload.length()
  sb = StringBuilder.new()
  sb.append(chr(byte0))
  mask_bit = if masked then 128 else 0 end
  if length <= 125
    sb.append(chr(mask_bit | length))
  elsif length <= 65535
    sb.append(chr(mask_bit | 126))
    sb.append(chr((length >> 8) & 255))
    sb.append(chr(length & 255))
  else
    sb.append(chr(mask_bit | 127))
    shift = 56
    while shift >= 0
      sb.append(chr((length >> shift) & 255))
      shift = shift - 8
    end
  end
  if masked
    key = SecureRandom.bytes(4)
    sb.append(key)
    sb.append(websocket_mask(payload, key))
  else
    sb.append(payload)
  end
  sb.to_s()
end

# NonblockingConnection#read(n) promises "up to n bytes, fewer only at
# EOF" (the same contract as File#read) -- reads less than `n` only once
# the peer is genuinely gone, never spuriously. Every fixed-size piece
# of a frame (the 2-byte base header, an extended-length field, the
# 4-byte mask key, the payload once its length is known) needs exactly
# that many bytes or a clean "connection is gone" signal, so a short
# read here always means the latter.
def websocket_read_exact(conn, n)
  if n == 0
    return ""
  end
  data = conn.read(n)
  if data.length() != n
    raise IOError.new("WebSocket connection closed mid-frame")
  end
  data
end

# Reads exactly one frame's raw header + payload off `conn`, applying
# the mask if the frame declared one -- but, unlike websocket_read_frame
# below, does not require a mask to be present. Split out from
# websocket_read_frame so this package's own test suite can decode a
# *server*-written (therefore unmasked) frame too, such as the Pong this
# package writes back in reply to a Ping -- something no code inside
# this package ever legitimately needs to do outside of testing, since a
# real server only ever reads client frames. Blocks (via `conn`'s own
# Fiber-yield, never the OS thread) until the full frame has arrived or
# the connection goes away mid-frame (IOError, from websocket_read_exact
# above).
def websocket_read_frame_bytes(conn)
  header = websocket_read_exact(conn, 2)
  byte0 = header[0].ord()
  byte1 = header[1].ord()
  fin = (byte0 & 128) != 0
  opcode = byte0 & 15
  masked = (byte1 & 128) != 0
  length7 = byte1 & 127
  length = if length7 == 126
    extended = websocket_read_exact(conn, 2)
    (extended[0].ord() << 8) | extended[1].ord()
  elsif length7 == 127
    extended = websocket_read_exact(conn, 8)
    value = 0
    index = 0
    while index < 8
      value = (value << 8) | extended[index].ord()
      index = index + 1
    end
    value
  else
    length7
  end
  if length > websocket_max_frame_size()
    raise IOError.new("WebSocket frame payload of #{length} bytes exceeds maximum of #{websocket_max_frame_size()}")
  end
  payload = if masked
    key = websocket_read_exact(conn, 4)
    raw_payload = websocket_read_exact(conn, length)
    websocket_mask(raw_payload, key)
  else
    websocket_read_exact(conn, length)
  end
  {"fin": fin, "opcode": opcode, "payload": payload, "masked": masked}
end

# The one server-side callers outside this package's own tests actually
# use: same as websocket_read_frame_bytes, but additionally enforces RFC
# 6455 section 5.1 -- a server MUST close the connection upon receiving
# an unmasked frame from a client. This is the one genuinely required
# (not merely lenient-vs-strict) protocol check on the read side; every
# conforming client always masks, so skipping it would let a malformed
# or hostile client silently desync the framing (an unmasked
# length/payload read as if it were masked).
def websocket_read_frame(conn)
  frame = websocket_read_frame_bytes(conn)
  unless frame["masked"]
    raise IOError.new("WebSocket client frame was not masked (RFC 6455 section 5.1)")
  end
  {"fin": frame["fin"], "opcode": frame["opcode"], "payload": frame["payload"]}
end
