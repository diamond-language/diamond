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
