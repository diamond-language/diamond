# packages/http

Minimal Rack-style HTTP/1.1 support for [Diamond](https://gitlab.com/dmn9180/diamond),
structured as a real, `facet`-installable package (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md))
rather than bundled into the runtime — kept in this same repo, under
`packages/`, since it has no dependents outside it to keep a separate git
history in sync with.

## Install

Either copy this directory straight into another project as
`cuts/http/`, or give it its own git remote and depend on
that:

```ruby
# cut.cut, in your project
{"name": "myapp", "dependencies": {"http": {"git": "<url-of-a-remote-for-this-directory>", "tag": "v0.1.0"}}}
```

```
$ facet install
```

## Usage

```ruby
require_cut "http"

def run()
  def handler(request)
    path = request["path"]
    [200, {"Content-Type": "text/plain"}, "hello, #{path}"]
  end
  http_serve(8080, handler)
end
run()
```

### Client

```ruby
require_cut "http"

response = http_get("http://example.com/status")
puts(response["status"])
puts(response["headers"]["content-type"])
puts(response["body"])

posted = http_post("http://example.com/items", "hi there")
puts(posted["status"])
```

Unlike `http_serve`, the client isn't limited to loopback/localhost — it
reaches any host `TCPSocket.connect` can. A response is a `Hash` too, but
shaped for reading rather than building positionally:
`{"status": ..., "headers": ..., "body": ...}`. Plain `http://` only —
an `https://` URL raises `ArgumentError` rather than silently connecting
in the clear on port 443.

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
directly, matching Diamond's "familiar syntax and object conventions"
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
- `http_get(url, headers = {})` / `http_post(url, body, headers = {})` /
  `http_request(method, url, headers = {}, body = "")` — client-side:
  connects via `TCPSocket.connect`, writes the request, and parses the
  status line/headers/body back into a `Hash`. `http_get`/`http_post` are
  thin wrappers over `http_request`.
- `http_parse_url(url)` — splits a `http://host[:port][/path]` URL into
  `{"host": ..., "port": ..., "path": ...}`; raises `ArgumentError` for a
  missing or non-`http` scheme.

A connected socket from Diamond's `TCPSocket.connect`/`TCPServer.accept`
already *is* a `File` under the hood, so this library is just ordinary
`.gets()`/`.read(n)`/`.write(value)` calls on it — no native VM code of
its own, plain Diamond throughout.

## What's deliberately out of scope

- **Chunked transfer encoding**: only `Content-Length`-declared bodies are
  read; a chunked request/response body will not be parsed correctly.
- **Keep-alive**: every connection is closed after one request/response,
  regardless of what the client sent (or the server's own request sends)
  in its `Connection` header.
- **Malformed-request robustness**: a connection that closes before
  sending anything is closed and skipped, not fed into the handler, so a
  bare port scanner or health check doesn't take the server down. Past
  that: a client that sends a garbled request line is still not handled
  gracefully — parsing it will raise an ordinary uncaught-method error
  rather than a clean HTTP 400.
- **Redirect-following**: the client returns a `3xx` response as-is,
  `Location` header and all; chasing it is left to the caller.
- **A real routing layer**: `http_serve` calls one handler for every
  request; building a router (matching `path`/`method` against declared
  routes) is left to the handler itself, or a future library on top.
- **HTTPS/TLS**: plain TCP only on both client and server, matching the
  underlying socket layer's own scope. The client rejects an `https://`
  URL outright rather than connecting in the clear on port 443.

Each of these is a plausible next slice, sized independently rather than
attempted together.

## Related

[`packages/rack`](../rack/README.md) layers composable middleware
(logging, auth, etc.) on top of `handler` here.
