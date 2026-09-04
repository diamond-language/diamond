# Server-side RFC 6455 WebSocket support for Diamond, built entirely on
# top of an ordinary read(n)/write(value)/close() connection -- no new
# C/VM opcodes were needed for the protocol logic itself. What made this
# practical to write in pure Diamond rather than needing a native
# implementation was Int gaining bitwise operators (`&`, `|`, `^`, `>>`)
# on `main` first: frame masking and the extended-length header fields
# are pure bit manipulation (see tests/cases/bitwise_operators.di).
#
# The one integration point outside this package: an app handler has to
# opt out of gremlin_serve's own Rack-style [status, headers, body]
# response contract to get at the raw connection at all. See
# packages/gremlin/lib/gremlin.di's own "Escape hatch: taking over the
# raw connection" doc comment -- context["gremlin_connection"] plus a
# `nil` handler return is exactly the mechanism websocket_accept below
# relies on. This package itself has no dependency on packages/gremlin
# or any other transport: anything exposing #read(n)/#write(value)/
# #close() (a plain socket wrapper, gremlin's own NonblockingConnection,
# a test double) works.
#
#   def handler(request, context)
#     if websocket_upgrade_request?(request)
#       ws = websocket_accept(context["gremlin_connection"], request)
#       while (message = ws.receive()) != nil
#         ws.send_text(message["data"])   # echo
#       end
#       nil
#     else
#       [200, {"Content-Type": "text/plain"}, "not a websocket request"]
#     end
#   end
#
# One file per class/concern (matching every other package's own
# convention): `frame` first (opcode constants + single-frame encode/
# decode, no dependency on anything else here), then `handshake` (pure
# HTTP/crypto, also no dependency on the others), then `connection`
# (WebSocketConnection, depends on both) -- a class/module-name
# reference resolves regardless of require order via the compiler's own
# declaration-discovery pass (see packages/arel/lib/arel.di's own
# comment), so this ordering is for natural reading, not correctness.
require "./websocket/frame"
require "./websocket/handshake"
require "./websocket/connection"
require "./websocket/broadcast"
