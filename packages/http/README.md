# http

Serve HTTP/1.1 requests and make outbound HTTP requests.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add http --registry https://cuts.dilang.tech --version "^0.1.1"
facet update
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

## Making requests

`http_get`, `http_post`, `http_put`, `http_patch`, `http_delete`, `http_head`, and
`http_options` return a Hash with `status`, `headers` (lower-case names), and `body`. Against
the server above:

```ruby
require_cut "http"

response = http_get("http://127.0.0.1:8080/ping", {"Accept": "application/json"})
response["status"]                      # => 200
JSON.parse(response["body"])

posted = http_post("http://127.0.0.1:8080/items", "name=widget",
  {"Content-Type": "application/x-www-form-urlencoded"})
```

`http_request(method, url, headers = {}, body = "", options = {})` is the general form.
`https://` URLs are supported. To fetch a URL a user supplied, check it first with the
[`network_safety`](https://github.com/diamond-language/diamond/tree/main/packages/network_safety) cut.

## Notes

`http_serve` accepts a request handler returning `[status, headers, body]`. `http_get` and `http_post` return hashes with `status`, `headers`, and `body`.

`http_parse_request(conn, limits)` optionally enables bounded parsing using
`line_bytes`, `header_bytes`, `header_count`, and `body_bytes` from a limits Hash.
It requires CRLF, validates request framing, rejects duplicate headers and
transfer encoding, checks body length before reading, and verifies complete
receipt. It supports `Expect: 100-continue` after length validation. Failures
raise `HttpRequestError` with status 400, 413, 417, or 431; Gremlin's optional
limits policy turns them into JSON responses. The original one-argument parser
retains its existing behavior. Deadlines belong to the server connection loop.
