# HTTP

`lib/http.di` is minimal Rack-style HTTP/1.1 support. Unlike `lib/core.di`,
it is **not** auto-embedded in every program — `require "lib/http"` to opt
in, so programs that never touch HTTP don't spend any of the 64-entry
function-table budget on it.

```ruby
require "lib/http"

def run()
  def handler(request)
    path = request["path"]
    [200, {"Content-Type": "text/plain"}, "hello, #{path}"]
  end
  http_serve(8080, handler)
end
run()
```

## Convention

A request is a `Hash`: `{"method": ..., "path": ..., "headers": ..., "body": ...}`.
`headers` is itself a `Hash` of header name to value, with names
normalized to lowercase (`content-type`, not `Content-Type`) — HTTP
header names are case-insensitive per spec, and a raw exact-case `Hash`
lookup would silently miss a legal lowercase header from a client or
proxy that doesn't send canonical casing. Look up request headers using
lowercase names; a handler's own response headers are written to the
wire exactly as given, with no normalization. A handler is a
`Callable[1]` taking a request and returning a 3-element response `Array`:
`[status, headers, body]` — `status` an `Int`, `headers` a `Hash`, `body` a
`String`. This mirrors Rack's own `call(env) => [status, headers, body]`
directly, matching this project's "familiar syntax and object conventions"
stance without claiming actual Rack compatibility.

Only nested `def`s produce a referenceable closure value (an established
Diamond limitation, not specific to this library), so a handler always
needs the same wrapping shown above — a top-level `def handler(...)` cannot
be passed to `http_serve` directly.

## Functions

- `http_serve(port, handler)` — `TCPServer.listen(port)`, then loops
  forever: accept a connection, parse a request, call `handler(request)`,
  write the response, close the connection. One connection per request —
  no keep-alive.
- `http_parse_request(conn)` — reads the request line and headers via
  `conn.gets()`, then, if a `Content-Length` header is present, reads
  exactly that many bytes as the body via `conn.read(n)` (not `conn.read()`,
  which reads to EOF and would hang waiting for a client that isn't going
  to close the connection).
- `http_write_response(conn, response)` — writes the status line, headers,
  a `Content-Length` computed from the body's own `.length()`, and the
  body.
- `http_status_text(status)` — a small fixed lookup (200, 201, 204, 301,
  302, 400, 401, 403, 404, 405, 500 → `"Unknown"` otherwise) used to build
  the status line.

## What made this possible with almost no new VM code

A connected socket — from `TCPSocket.connect` or from `.accept()` on a
`TCPServer` — already *is* a `File` under the hood (see `docs/io.md`), so
`http_parse_request`/`http_write_response` are just ordinary `.gets()`/
`.read(n)`/`.write(value)` calls on it; no new I/O plumbing was needed for
this library at all.

What genuinely was missing, and got added as this library's real
prerequisite: Diamond had **no string manipulation beyond concatenation,
interpolation, and `.length()`** before this — no indexing, no substring,
no numeric parsing. Three new native `String` methods closed that gap:

- `.index_of(needle)` — first match position as an `Int`, or `nil` if not
  found (matching `Hash` lookup's nil-for-missing convention, not `-1`).
- `.slice(start, length)` — a substring, clamping `length` to what's
  actually available but still bounds-checking `start` (a rescuable
  `IndexError`, matching `Array`/`Hash`).
- `.to_i()` — lenient decimal parsing (leading `+`/`-`, stops at the first
  non-digit, `0` for no leading digits at all, matching Ruby's own `to_i`),
  using checked arithmetic and raising a rescuable `RangeError` on
  overflow rather than wrapping, matching this project's existing
  integer-overflow discipline elsewhere in the VM.

`File#read(n)` (length-limited read; `.read()` with no arguments still
reads to EOF, unchanged) was the other prerequisite, needed specifically
for reading a request body of known `Content-Length`.

## What's deliberately out of scope

- **Chunked transfer encoding**: only `Content-Length`-declared bodies are
  read; a chunked request body will not be parsed correctly.
- **Keep-alive**: every connection is closed after one request/response,
  regardless of what the client sent in its `Connection` header.
- **Malformed-request robustness**: a client that sends a garbled request
  line, no request line at all, or closes early is not handled gracefully
  — `conn.gets()` returning `nil` and being fed into `.index_of` will raise
  an ordinary uncaught-method error rather than a clean HTTP 400.
- **A real routing layer**: `http_serve` calls one handler for every
  request; building a router (matching `path`/`method` against declared
  routes) is left to the handler itself, or a future library on top.
- **HTTPS/TLS**: plain TCP only, matching the underlying socket layer's
  own scope (`docs/io.md`).

Each of these is a plausible next slice, sized independently rather than
attempted together.
