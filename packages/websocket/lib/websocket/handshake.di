# Server-side RFC 6455 handshake: recognizing a genuine WebSocket
# upgrade request (already parsed the ordinary way, by packages/http's
# own http_parse_request -- an upgrade request is a perfectly normal GET
# with a few extra headers, nothing about parsing it is special) and
# answering it with the raw 101 response bytes, written directly to the
# connection rather than through http_write_response -- see
# packages/gremlin/lib/gremlin.di's own "Escape hatch: taking over the
# raw connection" doc comment for why a handler needs to do that at all.

# RFC 6455 section 1.3's fixed GUID, concatenated onto the client's own
# Sec-WebSocket-Key before hashing. Not a secret -- a fixed magic value
# the spec uses so the computed Sec-WebSocket-Accept could only have
# come from a server that actually understands the WebSocket protocol,
# not a cache or proxy blindly echoing the request back.
def websocket_guid() = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# Digest.sha1 returns a 40-character lowercase hex string (docs/
# collections.md), not the 20 raw bytes the handshake's own
# Base64.encode step needs -- and String#to_i is base-10 only, so
# there's no native radix parse to lean on. Small enough to hand-roll
# rather than pull in a general base-N parser for one call site --
# packages/http/lib/http/client.di's http_parse_hex hand-rolls the same
# underlying hex-digit-value gap for chunked-encoding sizes.
def websocket_hex_digit_value(ch)
  code = ch.ord()
  if code >= 48 && code <= 57
    code - 48
  elsif code >= 97 && code <= 102
    code - 97 + 10
  elsif code >= 65 && code <= 70
    code - 65 + 10
  else
    raise IOError.new("invalid hex digit in WebSocket handshake key")
  end
end

def websocket_hex_to_bytes(hex)
  sb = StringBuilder.new()
  index = 0
  while index < hex.length()
    high = websocket_hex_digit_value(hex[index])
    low = websocket_hex_digit_value(hex[index + 1])
    sb.append(chr(high * 16 + low))
    index = index + 2
  end
  sb.to_s()
end

# RFC 6455 section 4.2.2, step 5.2: Base64(SHA1(key + GUID)).
def websocket_accept_key(client_key)
  digest_hex = Digest.sha1(client_key + websocket_guid())
  Base64.encode(websocket_hex_to_bytes(digest_hex))
end

# The header check every real client sends (RFC 6455 sections 4.1/4.2.1):
# `Upgrade: websocket` and a `Connection` header naming "upgrade" as one
# of (possibly several, comma-separated) tokens, both matched case-
# insensitively, plus a present Sec-WebSocket-Key -- its actual random
# bytes don't need validating here, only that the client sent one at
# all, since websocket_accept_key below fails loudly (a raised error,
# not a wrong-but-silent Accept value) on anything that isn't a real
# hex-digest-shaped key. Deliberately does not require
# Sec-WebSocket-Version == "13": a version mismatch is a real client
# bug worth surfacing, but rejecting on a missing/malformed Upgrade or
# Connection header (indicating this isn't a WebSocket request at all)
# is the check that actually matters for routing -- a handler decides
# whether to even attempt the handshake based on this function alone.
def websocket_upgrade_request?(request)
  headers = request["headers"]
  upgrade = headers["upgrade"]
  connection = headers["connection"]
  key = headers["sec-websocket-key"]
  if upgrade == nil || connection == nil || key == nil
    return false
  end
  if upgrade.downcase() != "websocket"
    return false
  end
  if connection.downcase().index_of("upgrade") == nil
    return false
  end
  true
end

def websocket_handshake_response_bytes(request)
  accept_key = websocket_accept_key(request["headers"]["sec-websocket-key"])
  "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: #{accept_key}\r\n\r\n"
end

# The one call an application handler needs: given
# context["gremlin_connection"] and the already-parsed request, writes
# the 101 response directly to `conn` and returns a fresh
# WebSocketConnection wrapping it for the framed read/write lifetime of
# this connection from here on.
#
# Raises ArgumentError if `request` isn't really a WebSocket upgrade --
# a programmer error, not a client's fault: a real caller is expected to
# have already checked websocket_upgrade_request?(request) before ever
# reaching this point (matching e.g. packages/multipart's own
# multipart_save_file "caller already checked" contracts), and route on
# the result the same way an ordinary [status, headers, body] response
# would be chosen instead.
def websocket_accept(conn, request)
  unless websocket_upgrade_request?(request)
    raise ArgumentError.new("websocket_accept called on a request that is not a WebSocket upgrade")
  end
  conn.write(websocket_handshake_response_bytes(request))
  WebSocketConnection.new(conn)
end
