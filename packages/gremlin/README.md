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
another project as `diamond_packages/gremlin/` (alongside a
`diamond_packages/http/` for the `require "../http/http"` this package
makes internally to still exist), or give both their own git remotes and
depend on them via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## Usage

```ruby
require "/path/to/gremlin"

def run()
  def handler(request)
    path = request["path"]
    [200, {"Content-Type": "text/plain"}, "hello, #{path}"]
  end
  gremlin_serve(8080, handler)
end
run()
```

Identical to `http_serve`'s own usage -- same handler shape, same request
`Hash`, same response convention. The only difference is what happens
under load: `gremlin_serve` keeps accepting and progressing every other
connection while any one of them is slow.

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
imposes on any value crossing into a spawned thread.

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
