# Minimal Rack-style HTTP/1.1 support for Diamond. Install via facet
# (see diamond.cut), then `require_cut "http"` to bring in http_serve
# (server) and http_get/http_post/http_request/HttpSession (client).
#
# A server request is a Hash: {"method": ..., "path": ..., "headers": ...,
# "body": ...}. A handler is a Callable[1] taking a request and
# returning a 3-element response Array: [status, headers, body]. A
# client response is a Hash too, but shaped for reading rather than
# building positionally: {"status": ..., "headers": ..., "body": ...}.
#
# The server side stays deliberately basic: no chunked transfer
# encoding, no keep-alive (every connection/request is handled once then
# closed), and a malformed request/response is not caught -- it
# propagates like any other error. The one exception: a connection that
# closes before sending a request line (a port scanner, a health check,
# anything that connects and disconnects without writing) is closed and
# skipped rather than crashing the whole server -- http_serve is a
# single blocking accept loop, so an unhandled error from one connection
# would otherwise take every subsequent connection down with it. The
# client side (http_request and friends, HttpSession) has grown well
# past "minimal" -- see http/client.di's own top-of-file comment and
# README.md's "Client" section for the full options/HttpSession surface
# (HTTPS, chunked decoding, gzip/deflate, redirects, auth, JSON/
# multipart, a cookie jar, connection reuse).

# One file per logical grouping rather than per class -- `status` and
# `server` have exactly one class-worth of state to speak of (none:
# every function in them is a plain top-level def); `client` does now
# define one real class, HttpSession, but stays in one file with its own
# plain top-level functions rather than splitting further, matching
# active_record/arel's own "one file per class" convention loosely
# rather than by a hard rule this package never needed until now.
# `status` first: both `server` and `client` call http_max_body_size,
# and `server`'s own http_write_response calls http_status_text --
# Diamond's bare top-level function calls only resolve source/require
# order, not forward (see packages/arel/lib/arel.di's own comment), so
# the shared helpers have to load before either side that uses them.
require "./http/status"
require "./http/server"
require "./http/client"
