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
# diamond.cut, in your project
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
reaches any host `TCPSocket.connect`/`TLSSocket.connect` can, `http://`
and `https://` alike. A response is a `Hash` too, but shaped for reading
rather than building positionally: `{"status": ..., "headers": ...,
"body": ...}`.

Every client function takes a final `options` Hash (always optional):

```ruby
# TLS: custom trust store, client certificate, timeouts
http_get("https://internal.example.com/", {}, {
  "ca_file": "internal-ca.pem", "connect_timeout_ms": 5000
})

# Basic/Bearer auth
http_get("http://example.com/private", {}, {"bearer_token": "abc123"})

# A JSON request body -- Content-Type set automatically
http_post("http://example.com/items", "", {}, {"json": {"name": "widget"}})

# A file upload -- Content-Type (with boundary) set automatically
http_post("http://example.com/upload", "", {}, {
  "multipart_fields": {"name": "alice"},
  "multipart_files": {"doc": {"filename": "a.txt", "content_type": "text/plain", "data": "..."}}
})

# Follow redirects (off by default)
http_get("http://example.com/old-path", {}, {"follow_redirects": true})
```

`gzip`/`deflate` response decompression and Transfer-Encoding: chunked
response decoding both happen automatically, with no option needed
(`gzip: false` opts out of the former if the raw wire bytes are wanted).
See `docs/syntax.md`'s `TLSSocket.connect` and `Gzip`/`Base64` sections,
and this file's own "Functions" section below, for the full `options`
reference.

For several calls against the same server, `HttpSession` adds a cookie
jar (a `Set-Cookie` from one response is sent back automatically on
later requests to the same host) and per-host connection reuse:

```ruby
session = HttpSession.new({"follow_redirects": true})
session.get("http://example.com/login")   # Set-Cookie remembered
session.get("http://example.com/account") # cookie sent automatically
session.close()
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
- `http_request(method, url, headers = {}, body = "", options = {})` —
  client-side: connects via `TCPSocket.connect`/`TLSSocket.connect`
  (scheme-dependent), writes the request, and parses the status
  line/headers/body back into a `Hash`, decoding chunked
  Transfer-Encoding and decompressing a gzip/deflate Content-Encoding
  along the way. `http_get(url, headers = {}, options = {})` /
  `http_post(url, body = "", headers = {}, options = {})` /
  `http_put` / `http_patch` / `http_delete(url, headers = {}, options =
  {})` / `http_head(url, headers = {}, options = {})` /
  `http_options(url, headers = {}, options = {})` are thin wrappers.
  `options` (every key optional):
  - `connect_timeout_ms` / `read_timeout_ms` / `write_timeout_ms` —
    forwarded to `TCPSocket.connect`/`TLSSocket.connect`.
  - `ca_file` / `ca_path` / `cert` / `key` — forwarded to
    `TLSSocket.connect` for an `https://` URL (custom trust store, or a
    client certificate for mutual TLS); ignored for `http://`.
  - `gzip` — `true` by default: sends `Accept-Encoding: gzip, deflate`
    and decompresses a matching response; `false` leaves the wire bytes
    as-is.
  - `basic_auth: {"user":, "password":}` / `bearer_token: "..."` — sets
    the `Authorization` header. Overridden by an explicit `Authorization`
    in `headers`.
  - `json: <value>` — the body becomes `JSON.stringify(value)` and
    `Content-Type` becomes `application/json` (unless `headers` already
    set one).
  - `multipart_fields: {name: value}` / `multipart_files: {name:
    {"filename":, "content_type":, "data":}}` — the body becomes a
    `multipart/form-data` encoding of both (`http_encode_multipart`);
    `Content-Type` (with boundary) is set the same way `json`'s is.
  - `follow_redirects` — `false` by default. `true` follows a `3xx`
    response's own `Location`, up to `max_redirects` (default `5`) hops.
    307/308 preserve the original method and body; 301/302/303 downgrade
    to a bodyless `GET` unless the original request was already
    `GET`/`HEAD` (matching `curl -L`/every browser, not a strict RFC
    2616 reading). `Authorization` is dropped when the redirect target's
    host differs from the original request's.
- `http_encode_multipart(fields, files)` — builds a `multipart/form-data`
  body and boundary from a fields `Hash` (name -> `String`) and a files
  `Hash` (name -> `{"filename":, "content_type":, "data":}`) — the exact
  shapes `packages/multipart`'s own `multipart_parse` produces on the
  server side. Returns `{"content_type": ..., "body": ...}`.
- `HttpSession` — a cookie jar plus one reused connection per
  `scheme://host:port`, for several calls against the same server.
  `HttpSession.new(options = {})` takes the same `options` every
  function above does, applied to every request the session makes.
  `.get(url, headers = {})` / `.post` / `.put` / `.patch` /
  `.delete(url, headers = {})` mirror the top-level functions;
  `.request(method, url, headers = {}, body = "")` is the general form.
  `.close()` closes every cached connection. Cookie scope is
  deliberately simplified: a `Set-Cookie` name/value pair is stored per
  exact request host and sent back to that exact host — no
  Domain-attribute subdomain matching, no Path scoping, no
  Expires/Max-Age eviction. Connection reuse retries once against a
  fresh connection on any failure, rather than tracking whether the
  server actually intends to keep a connection alive.
- `http_parse_url(url)` — splits a `http://`/`https://`
  `scheme://host[:port][/path]` URL into `{"scheme": ..., "host": ...,
  "port": ..., "path": ..., "default_port": ...}`; raises
  `ArgumentError` for a missing or unsupported scheme.

A connected socket from Diamond's `TCPSocket.connect`/`TCPServer.accept`
already *is* a `File` under the hood (a `TLSSocket` shares the same
`.gets()`/`.read(n)`/`.write(value)` surface), so this library is plain
Diamond throughout — no native VM code of its own, just calls into
existing language builtins (`TLSSocket`/`Gzip`/`Base64`/`SecureRandom`)
for everything below the wire-protocol logic itself.

## What's deliberately out of scope

The client side has grown well past "minimal" (HTTPS, chunked decoding,
gzip/deflate, redirects, auth, JSON/multipart, a session with cookies and
connection reuse); the server side (`http_serve`) is still deliberately
basic:

- **Chunked transfer encoding**: the *server* only reads
  `Content-Length`-declared request bodies, and never emits a chunked
  *response* — a chunked request body will not be parsed correctly. (The
  *client* does decode a chunked response.)
- **Keep-alive**: `http_serve` closes every connection after one
  request/response, regardless of what the client sent in its
  `Connection` header. (The client's own `HttpSession` does reuse
  connections — see above.)
- **Malformed-request robustness**: a connection that closes before
  sending anything is closed and skipped, not fed into the handler, so a
  bare port scanner or health check doesn't take the server down. Past
  that: a client that sends a garbled request line is still not handled
  gracefully — parsing it will raise an ordinary uncaught-method error
  rather than a clean HTTP 400.
- **A real routing layer**: `http_serve` calls one handler for every
  request; building a router (matching `path`/`method` against declared
  routes) is left to the handler itself, or a future library on top.
- **HTTPS/TLS on the server side**: `http_serve` is plain TCP only.
  `TLSServer.listen` exists (see `docs/io.md`) for a caller that wants a
  TLS server, but nothing in this package wraps it into an
  `https_serve`-style helper yet.
- **`br`/`zstd`/other Content-Encoding schemes**: the client only
  decompresses `gzip`/`deflate`; anything else is left exactly as
  received.

Each of these is a plausible next slice, sized independently rather than
attempted together.

## Related

[`packages/rack`](../rack/README.md) layers composable middleware
(logging, auth, etc.) on top of `handler` here.
