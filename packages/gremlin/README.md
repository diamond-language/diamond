# gremlin

Serve concurrent HTTP requests with fiber-based connection handling.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add gremlin --registry https://cuts.dilang.tech --version "^0.4.1"
facet update
```

This installs the cut into `cuts/gremlin/`; load it with `require_cut "gremlin"`. `http` and `logger` are installed with it.

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

## Token-managed shutdown

Gremlin 0.3.0 accepts optional eighth and ninth arguments, `shutdown_token` and
`shutdown_timeout` (seconds, default `10.0`, finite and nonnegative):

```ruby
source = Cancellation::Source.new()
begin
  gremlin_serve(8080, handler, 1, nil, nil, nil, false, source.token(), 0.5)
ensure
  source.cancel()
  # Join other owned work and close application resources.
end
```

Token mode needs the [`cancellation`](https://github.com/diamond-language/diamond/tree/main/packages/cancellation) cut, which is not yet on the
registry (copy `packages/cancellation` from a Diamond checkout into `cuts/`), and a runtime
with cancellable `IO.poll` (cancellation 0.3.0). Gremlin itself keeps its existing
HTTP/logger dependencies; ordinary callers need no cancellation package.

Token mode supports multiple workers and always returns to its caller, regardless of
`return_after_shutdown`. The application owns signal handling: Gremlin does not
install or replace `Signal.trap` handlers in this mode. A handler can call
`source.cancel()`, as the [job service](https://github.com/diamond-language/diamond/blob/main/examples/job_service/README.md) does.
Cancellation from another thread, a parent source, or the token's deadline
closes each worker's listener and starts draining. `server.shutdown_started` is logged
after the listener closes. The grace deadline is anchored once and does not
restart on another cancellation. All workers share the first observer's absolute
drain deadline, including workers that notice cancellation later. The caller
joins every spawned worker before returning. A worker setup or loop failure
also stops its siblings; joins finish before the error is re-raised. A
pre-cancelled token accepts no requests.

Use `threads: 4` with the same token to enable this in Gremlin 0.4.0. Handlers
and tick callbacks must be zero-capture callables, as with ordinary multi-worker
servers. Class variables and request contexts remain private to each VM.

Active connections can finish until the grace deadline. Once it expires,
remaining sockets close and handlers suspended in socket I/O resume once so
their `ensure` cleanup can run. `server.shutdown_complete` reports `forced` and,
on forced closure, the remaining connection count. During drain, Gremlin stops
watching the already-cancelled token and waits on live sockets/deadlines instead.

Handlers must cooperate: synchronous CPU/native work cannot be preempted, and
code that catches a closed-socket error and yields again is not guaranteed to
finish. A grace period bounds I/O waiting, not arbitrary application execution.
Without a token, custom grace periods still require one worker; the existing
signal-driven shutdown and second-signal forced exit remain in place.
