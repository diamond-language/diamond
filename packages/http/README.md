# http

Serve HTTP/1.1 requests and make outbound HTTP requests.

## Installation

Install the cut at `cuts/http/` and load it with `require_cut "http"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

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
