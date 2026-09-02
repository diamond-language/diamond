# packages/websocket

Server-side [RFC 6455](https://datatracker.ietf.org/doc/html/rfc6455)
WebSocket support for [Diamond](https://gitlab.com/dmn9180/diamond),
structured as a real, `facet`-installable package (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).
Pure Diamond, no new VM opcodes: the handshake is HTTP header checks
plus SHA1/Base64 (both already native), and framing is Int bitwise
manipulation (`&`, `|`, `^`, `>>`) over an already-binary-safe `String` --
[`packages/gremlin`](../gremlin/README.md)'s own `NonblockingConnection`
already provides everything else (non-blocking `#read(n)`/`#write(value)`).

## Install

Same story as every other package here — copy this directory into
another project as `cuts/websocket/`, or give it its own git remote and
depend on it via `facet`.

## Use

A WebSocket upgrade is an ordinary HTTP request with a few extra
headers — nothing about parsing it is special — but *answering* one
needs the raw connection, not a `[status, headers, body]` value, since
a WebSocket keeps that connection open for framed messages afterward
instead of ending it after one response. `gremlin_serve` supports this
via an escape hatch made for exactly this case: `context["gremlin_connection"]`
is this request's own live connection, and a handler that writes its
own response directly to it returns `nil` instead of a real response
value to tell `gremlin_serve` not to write a second, conflicting one of
its own (see [`packages/gremlin`](../gremlin/README.md)'s own "Escape
hatch: taking over the raw connection" doc comment).

```ruby
require "/path/to/gremlin/lib/gremlin"
require "/path/to/websocket/lib/websocket"

def handler(request, context)
  if request["path"] == "/echo" && websocket_upgrade_request?(request)
    ws = websocket_accept(context["gremlin_connection"], request)
    loop do
      message = ws.receive()
      if message == nil
        break   # peer closed the connection
      end
      ws.send_text(message["data"])   # echo it back
    end
    nil
  else
    [404, {"Content-Type": "text/plain"}, "not found"]
  end
end

gremlin_serve(8080, handler)
```

`websocket_upgrade_request?(request)` checks for a real WebSocket
upgrade (`Upgrade: websocket`, an `Upgrade` token in `Connection`, and a
`Sec-WebSocket-Key`) — false for an ordinary request, so a route can
serve both WebSocket and plain HTTP traffic depending on what showed up.
`websocket_accept(conn, request)` computes and writes the 101 handshake
response, then returns a `WebSocketConnection` for everything after
that.

`ws.receive()` returns the next fully-reassembled message as
`{"opcode": WEBSOCKET_OPCODE_TEXT | WEBSOCKET_OPCODE_BINARY, "data": String}`,
or `nil` once the close handshake has completed (either side can
initiate it — see below). Ping/Pong frames and multi-frame
(fragmented) messages are handled transparently; `#receive` never
surfaces a Ping/Pong to the caller, and a fragmented message comes back
as one already-reassembled `data` String. `ws.send_text(text)` /
`ws.send_binary(data)` each send one complete, unfragmented frame.
`ws.close(code = 1000, reason = "")` sends a Close frame and waits for
the peer's own Close frame back before closing the underlying
connection — the same graceful, let-the-peer-finish spirit as
`GremlinShutdown`'s own SIGTERM handling.

## What's deliberately out of scope

- **A WebSocket client.** Everything here is the server side of the
  handshake and framing (masked frames in, unmasked frames out) — there
  is no `websocket_connect(url)` to talk to someone else's server.
- **Message fragmentation on send.** `#send_text`/`#send_binary` always
  send one complete frame; nothing in this package ever needs to split
  an outgoing message across several frames, since every outgoing
  message is already fully built in memory before it's sent.
  (`websocket_encode_frame` itself does support building a `fin: false`
  frame — used by this package's own test suite to build a
  deliberately-fragmented message and exercise the reassembly path on
  the read side.)
- **Compression (`permessage-deflate`).** Not negotiated or supported.
- **A per-connection idle/ping timeout.** Matches
  [`packages/gremlin`](../gremlin/README.md)'s own "no per-connection
  timeout" scope cut — an idle WebSocket connection sits open until it
  disconnects or the process exits.

## Testing

`test.sh` has two parts: pure protocol-level checks against a small
in-memory fake connection (the RFC 6455 handshake worked example,
frame round-tripping at both extended-length boundaries, an unmasked or
oversized frame being rejected, Ping/Pong, fragmentation, and both
directions of the close handshake), then a live `gremlin_serve` echo
server talked to over a real TCP socket by Node's own global
`WebSocket` client (no install needed, Node 22+) — real interop with a
standards-compliant client, not just this package's own encoder
agreeing with its own decoder. The live section is skipped (not failed)
if `node` isn't on `PATH`.
