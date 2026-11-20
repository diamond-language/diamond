# http

Serve HTTP/1.1 requests and make outbound HTTP requests.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/d6/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no Cutfile yet
facet add http
facet install
```

This installs the cut into `cuts/http/`; load it with `require_cut "http"`.

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

## Serving from a thread

`http_serve(port, handler, host, ready, stop)` takes three optional arguments after the
handler. `host` is the address to listen on, `ready` is a Channel that gets the port once the
server listens (pass port 0 for any free one), and closing the `stop` Channel makes it
return. Together they let a program, such as a test, run a server on its own thread:

```ruby
require_cut "http"

def hello(request) = [200, {"Content-Type": "text/plain"}, "hello #{request["path"]}"]

ready = Channel.new(1)
stop = Channel.new(1)
server = Thread.new(http_serve, 0, hello, "127.0.0.1", ready, stop)
port = ready.receive()
http_get("http://127.0.0.1:#{port}/x")["body"]   # => "hello /x"
stop.close()
server.join()
```

The handler must be a top-level function, as every thread entry point is.

## Making requests

`http_get`, `http_post`, `http_put`, `http_patch`, `http_delete`, `http_head`, and
`http_options` return a Hash with `status`, `headers` (lower-case names), and `body`. Against
the server above:

<!-- doc-check: skip -->
```ruby
require_cut "http"

response = http_get("http://127.0.0.1:8080/ping", {"Accept": "application/json"})
response["status"]                      # => 200
JSON.parse(response["body"])

posted = http_post("http://127.0.0.1:8080/items", "name=widget",
  {"Content-Type": "application/x-www-form-urlencoded"})
```

`http_request(method, url, headers = {}, body = "", options = {})` is the general form.
Its `options` include timeouts, `follow_redirects`, and `max_body_bytes`, the largest
response body it will read (25 MiB by default; a longer one raises `IOError`).
`https://` URLs are supported. To fetch a URL a user supplied, check it first with the
[`network_safety`](https://github.com/diamond-language/d6/tree/main/packages/network_safety) cut.

## Notes

`http_serve` accepts a request handler returning `[status, headers, body]`. The response to a `HEAD` request leaves the body out; `http_write_response(conn, response, false)` writes a response that way. `http_get` and `http_post` return hashes with `status`, `headers`, and `body`.

`http_parse_request(conn, limits)` optionally enables bounded parsing using
`line_bytes`, `header_bytes`, `header_count`, and `body_bytes` from a limits Hash.
It requires CRLF, validates request framing, rejects duplicate headers and
transfer encoding, checks body length before reading, and verifies complete
receipt. It supports `Expect: 100-continue` after length validation. Failures
raise `HttpRequestError` with status 400, 413, 417, or 431; Gremlin's optional
limits policy turns them into JSON responses. The original one-argument parser
retains its existing behavior. Deadlines belong to the server connection loop.
