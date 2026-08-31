# packages/gremlin

A small, fiber-based, Puma-like concurrent HTTP server for
[Diamond](https://gitlab.com/dmn9180/diamond) -- same Rack-style handler
contract as [`packages/http`](../http/README.md)'s `http_serve`
(`Callable[1]` taking a request `Hash`, returning `[status, headers,
body]`), but every connection is handled concurrently instead of one at a
time. A slow client reading its response a byte at a time, or one that
opens a connection and never sends anything, never blocks any other
connection's own progress.

## Install

Same story as `packages/http` -- either copy this directory straight into
another project as `cuts/gremlin/` (alongside a `cuts/http/` and a
`cuts/logger/` for the `require "../../http/lib/http"`/
`require "../../logger/lib/logger"` this package makes internally to
still exist), or give all three their own git remotes and depend on
them via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## Usage

```ruby
require "/path/to/gremlin/lib/gremlin"

def run()
  def handler(request, context)
    path = request["path"]
    [200, {"Content-Type": "text/plain"}, "hello, #{path}"]
  end
  gremlin_serve(8080, handler)
end
run()
```

Almost identical to `http_serve`'s own usage -- same request `Hash`, same
`[status, headers, body]` response convention -- plus one addition:
`handler` takes a second argument, `context`, a plain `Hash` that starts
empty and is this *worker's* own to keep mutating across requests (see
"Per-worker context" below). The other difference is what happens under
load: `gremlin_serve` keeps accepting and progressing every other
connection while any one of them is slow.

Unhandled request-handler failures are emitted as newline-delimited JSON with
the event name and error stored in separate fields. Gremlin diagnostics can
therefore share an application's structured log stream without adding an
unparseable text line.

Pass `threads: N` to actually use more than one core:

```ruby
gremlin_serve(8080, handler, threads: 4)
```

Each of the `N` threads runs its own fully independent copy of the same
fiber-driven event loop (see "How it works" below), on its own OS thread
(Diamond's [`Thread`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/threads.md)
primitive), with its own listener bound to the same port -- no shared
state between them, so no new locking or synchronization to reason about.
`threads` defaults to `1`, exactly today's single-thread behavior with no
`Thread` involved at all -- existing `gremlin_serve(port, handler)` call
sites are unaffected.

`handler` must be a zero-capture `Callable` (an ordinary top-level `def`,
like the example above, or a closure literal that captures nothing) when
`threads` is more than 1 -- the same restriction `Thread.new` itself
imposes on any value crossing into a spawned thread. `context` is how a
handler keeps state across requests despite that restriction -- see
below.

### Per-worker context

`handler`'s second argument is a `Hash`, created empty once per worker
(not once per server -- see below) before that worker's own accept loop
starts, and handed back on every subsequent request that same worker
happens to handle. It exists because closing over a mutable local the
ordinary way doesn't survive `threads > 1`: `Thread.new` hard-rejects any
`Callable` that captures local state, and Diamond has no class-variable
or other static-storage mechanism a zero-capture function could reach by
name instead. Without `context`, a handler running under `threads > 1`
would have no way to keep anything across requests at all -- no
session cache, no counter, nothing.

`context` is **per worker, not shared or synchronized across workers**:
`threads: N` gives a handler N independent Hashes, one per worker, each
only ever touched by that one worker's own single OS thread (fibers
within a worker are cooperative, never preemptive, so ordinary
non-atomic reads/writes on `context` are safe there too -- no locking
needed within a worker either). A counter incremented in `context` is a
per-worker counter, not a global one; a cache built up in `context` is
warm only for requests that happen to land on that same worker. That's
the same tradeoff every other piece of per-worker state here already
makes (own heap, own listener, own connections list) -- `context` isn't
special, it's just the one piece of that state `handler` actually gets
to see.

### Graceful shutdown

`gremlin_serve` traps `SIGTERM`/`SIGINT` and drains cleanly: it stops
accepting new connections, lets every already-in-flight request finish
and send its real response, then exits `0` -- no `kill`-then-hope, no
cut-off responses. Nothing to opt into; it's on unconditionally. A
10-second grace period bounds how long shutdown can take (a stuck or
slow client can't wedge it forever) -- if connections are still open
when the deadline passes, the process exits anyway, logging
`server.shutdown_complete` with `"forced": true` and however many
connections were still open, rather than hanging indefinitely. A
*second* `SIGTERM`/`SIGINT` -- an impatient double Ctrl+C, or an
operator who doesn't want to wait out the rest of the grace period --
skips the drain/deadline logic entirely and exits right away, logging
`server.shutdown_forced_by_signal`.

**Verified for `threads: 1` (the default) only.** `Signal.trap`'s own
pending-signal state is process-wide, not per-VM (`src/vm.c`'s own
comment on this: "signal delivery is a process-wide OS concept, not a
per-VM-instance one"), while each `threads: N` worker is a fully
independent VM/OS thread with no shared state at all otherwise (see
above). Which worker's own registered handler actually runs when the
process receives one `SIGTERM`, and whether several independent
workers' own shutdown sequences interleave safely, isn't something
this has been tested against -- if you run `threads > 1` today, treat
shutdown as untested there specifically (everything else about
`threads > 1` is unaffected).

## How it works

No OS-level scheduler *within* one worker thread, no changes to
`packages/http`'s own `http_parse_request`/`http_write_response` (used
here completely unmodified). Three pieces, all new:

- **Non-blocking sockets** (`TCPServer.listen_nonblocking`, the `Socket`
  object `.accept()` returns from one): `.read(n)`/`.write(value)` raise
  `WouldBlockError` instead of blocking when nothing's ready, rather than
  the ordinary blocking `TCPServer`/`TCPSocket`/`File` primitives
  `http_serve` itself still uses.
- **`IO.poll(readables, writables, timeout_ms)`**: asks the OS which of a
  set of listeners/sockets are actually ready, so gremlin's own loop
  never has to guess or busy-spin.
- **One `Fiber` per connection**: `NonblockingConnection` wraps a
  `Socket`, and its `#gets`/`#read`/`#write` catch `WouldBlockError` and
  call `yield` instead of propagating it -- suspending that connection's
  own fiber, in place, at whatever call depth it happened to be at
  (inside `http_parse_request`, inside `http_write_response`, wherever).
  `gremlin_worker`'s own loop -- run once per thread, `threads` times over
  -- is the only thing that ever calls `IO.poll` or `.resume`, deciding
  which of *that thread's own* suspended connections have become ready
  again and waking exactly those.

See `gremlin.di`'s own top-of-file comment and
[`docs/io.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/io.md)/[`docs/fibers.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/fibers.md)
for the full design.

## Related

[`packages/rack`](../rack/README.md) layers composable middleware
(logging, auth, etc.) on top of `handler` here -- including a documented
pattern for `threads > 1` specifically.

## What's deliberately out of scope

- **Keep-alive, request pipelining**: same scope cut `http_serve` already
  makes -- one request per connection, then closed.
- **Per-connection timeouts**: a client that opens a connection and never
  sends anything sits in the server's own connection list until it
  disconnects or the process exits. Nothing evicts it.
- **A real reactor/epoll**: `IO.poll` is `poll(2)` under the hood, called
  fresh every iteration -- fine at the connection counts this is meant
  for, not tuned for thousands of concurrent idle connections.
- Everything `packages/http`'s own README already lists as out of scope
  (chunked transfer encoding, a routing layer, HTTPS/TLS) applies here
  too, since request parsing/response writing is the exact same code.
