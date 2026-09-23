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

## Optional request limits

Pass a sixth `limits` argument (after `threads`, `tick_interval`, and `on_tick`)
to enable bounded HTTP parsing and per-worker connection controls:

```diamond
limits = {"line_bytes": 8192, "header_bytes": 32768, "header_count": 100,
  "body_bytes": 26214400, "connections": 8, "timeout_seconds": 30}
gremlin_serve(8080, handler, 1, nil, nil, limits)
```

All six values must be positive integers. Omitting limits preserves existing
behavior. Each limited connection receives a generated `request_id` in its
request hash. Parser failures return JSON errors and `X-Request-ID`; deadline
and capacity failures close the socket and log an event. Accept batches are
bounded so busy listeners yield back to existing connections. Deadline polling
continues even with no socket activity. These are I/O deadlines from accept,
not preemption of synchronous handlers; stalled response writes also expire.
Long-lived protocols such as WebSockets should not use this policy without
accounting for the connection lifetime limit.
