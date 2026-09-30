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

## Returning to an application owner on shutdown

The optional seventh argument, `return_after_shutdown: true`, makes a
single-worker server return after draining (or closing at the shutdown deadline)
instead of exiting the process. This lets the caller's `ensure` cancel and join
other owned work. Pass the preceding optional arguments explicitly:

```ruby
begin
  gremlin_serve(8080, handler, 1, nil, nil, nil, true)
ensure
  # Close application-owned resources here.
end
```

This mode requires `threads = 1`; multi-worker shutdown coordination is not
implemented. Default behavior is unchanged. A second shutdown signal still
forces process exit and bypasses cleanup. Signal handlers are not restored when
the server returns; this mode is intended for shutdown of the owning service.
