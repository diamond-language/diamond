# Minimal Rack-style HTTP/1.1 support for Diamond. Install via facet
# (see diamond.cut), then `require_cut "http"` to bring in http_serve
# (server) and http_get/http_post/http_request (client).
#
# A server request is a Hash: {"method": ..., "path": ..., "headers": ...,
# "body": ...}. A handler is a Callable[1] taking a request and
# returning a 3-element response Array: [status, headers, body]. A
# client response is a Hash too, but shaped for reading rather than
# building positionally: {"status": ..., "headers": ..., "body": ...}.
#
# Deliberately basic on both sides: no chunked transfer encoding, no
# keep-alive (every connection/request is handled/sent once then
# closed), no redirect-following on the client side, and a malformed
# request/response is not caught -- it propagates like any other
# error. The one exception: a connection that closes before sending a
# request line (a port scanner, a health check, anything that connects
# and disconnects without writing) is closed and skipped rather than
# crashing the whole server -- http_serve is a single blocking accept
# loop, so an unhandled error from one connection would otherwise take
# every subsequent connection down with it. See http_request's own
# comment for what's specific to the client half.

# One file per logical grouping rather than per class -- this package
# has exactly one class-worth of state to speak of (none: every
# function here is a plain top-level def, no instances involved).
# `status` first: both `server` and `client` call http_max_body_size,
# and `server`'s own http_write_response calls http_status_text --
# Diamond's bare top-level function calls only resolve source/require
# order, not forward (see packages/arel/lib/arel.di's own comment), so
# the shared helpers have to load before either side that uses them.
require "./http/status"
require "./http/server"
require "./http/client"
