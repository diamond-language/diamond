# gremlin

Serve concurrent HTTP requests with fiber-based connection handling.

## Installation

Install the cut at `cuts/gremlin/` and load it with `require_cut "gremlin"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `http`, `logger`.

## Usage

```ruby
require_cut "gremlin"

def run()
  def handler(request, context)
    path = request["path"]
    [200, {"Content-Type": "text/plain"}, "hello, #{path}"]
  end
  gremlin_serve(8080, handler)
end
run()
```

## Notes

The handler receives `(request, context)` and returns `[status, headers, body]`. The context is private to its worker. Call `gremlin_serve(port, handler, threads: 4)` for multiple workers.
