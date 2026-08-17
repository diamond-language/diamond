# packages/rack

A small, composable, Rack-style middleware layer for
[Diamond](https://gitlab.com/dmn9180/diamond). Wraps around
[`packages/http`](../http/README.md)'s `http_serve` and
[`packages/gremlin`](../gremlin/README.md)'s `gremlin_serve` -- both
already share a request `Hash`/`[status, headers, body]` response
convention -- with a standard way to layer logging, auth, and anything
else around a handler, instead of every app hand-rolling its own
wrapping.

This package itself has no dependency on either server: no `Fiber`, no
`Thread`, no socket types, just the request/response convention both
already speak. It's built to be genuinely server-agnostic, not just
gremlin-flavored -- see "Using it with `http_serve`" below.

## Install

Same story as `packages/http`/`packages/gremlin` -- copy this directory
into another project as `diamond_packages/rack/`, or give it its own git
remote and depend on it via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## The contract

A **middleware** is a `Callable[3]`: `(request, context, forward)`.
`forward` is itself a `Callable[2]`: `(request, context) -> [status,
headers, body]`, standing for "the rest of the chain." Call it to
continue on to the next middleware (or the app, at the end); don't call
it to short-circuit -- an auth check returning `[401, ..., ...]` directly
instead of reaching the real app, say.

The terminal **application handler** is an ordinary `Callable[2]`
`(request, context)` -- the exact shape `gremlin_serve`'s own `handler`
already is -- so an existing handler needs no changes to become the tail
of a chain.

```ruby
require "/path/to/rack"

def logging_middleware(request, context, forward)
  response = forward(request, context)
  puts("#{request["method"]} #{request["path"]} -> #{response[0]}")
  response
end

def auth_middleware(request, context, forward)
  if request["headers"]["authorization"] == "Bearer secret"
    forward(request, context)
  else
    [401, {"Content-Type": "text/plain"}, "unauthorized"]
  end
end

def app_handler(request, context)
  [200, {"Content-Type": "text/plain"}, "hello, #{request["path"]}"]
end

chain = rack_compose([logging_middleware, auth_middleware], app_handler)
response = rack_run_chain(chain, 0, some_request, {})
```

`rack_compose(middlewares, app)` returns a plain `Array` -- the composed
chain -- not a callable you invoke directly; `rack_run_chain(chain, 0,
request, context)` is what actually runs it, starting from index 0.
That split matters once threading enters the picture -- see below.

## Using it with `gremlin_serve`

At `threads: 1` (the default), wiring is exactly the snippet above:
build `chain` once, and drive it from a top-level handler:

```ruby
require "/path/to/gremlin"
require "/path/to/rack"

chain = rack_compose([logging_middleware, auth_middleware], app_handler)

def rack_app(request, context)
  rack_run_chain(chain, 0, request, context)
end

gremlin_serve(8080, rack_app)
```

### `threads > 1`: why `RackChain` exists

`gremlin_serve(..., threads: N)` spawns each worker via `Thread.new`,
and `Thread.new` categorically rejects any `Callable` that captures
local state (a captured value is live GC state tied to one heap, and
`Thread` heaps are fully isolated -- see
[`docs/threads.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/threads.md)).
The `rack_app` above captures `chain` -- fine for `threads: 1` (nothing
ever crosses a thread boundary there), but handing that same closure to
`gremlin_serve` as `handler` once `threads > 1` would be rejected on the
spot.

`RackChain` sidesteps this using Diamond's class variables (`@@cvar`):
each `Thread`-spawned worker already gets its own fully independent
`DiamondVm`, so memoizing the composed chain in a class variable gives
every worker its own independently-built copy, built the first time that
worker actually handles a request -- and the only thing that ever
crosses the `Thread.new` boundary is an ordinary zero-capture top-level
`def`, exactly what `gremlin_serve`'s `handler` contract already
requires today.

```ruby
def build_chain()
  rack_compose([logging_middleware, auth_middleware], app_handler)
end

def rack_app(request, context)
  rack_run_chain(RackChain.get(build_chain), 0, request, context)
end

gremlin_serve(8080, rack_app, threads: 4)
```

`rack_app` itself captures nothing -- it looks the chain up through
`RackChain.get`, a class-variable read, not a lexical capture -- so it
crosses `Thread.new` exactly like any plain top-level handler does. This
pattern works identically at `threads: 1` too, so it's the one worth
reaching for by default if there's any chance a service grows into
`threads > 1` later.

`RackChain` memoizes **one** chain per VM (the common case: one app per
process). A program that genuinely needs several independent chains at
once should write its own small memoizing class following the same
two-line pattern, keyed however it needs.

## Using it with `http_serve`

`http_serve` is single-threaded -- no `Thread.new` involved at all, so
none of the capture restriction above applies. The plain closure-based
form works directly, no `RackChain` needed:

```ruby
require "/path/to/http"
require "/path/to/rack"

chain = rack_compose([logging_middleware], app_handler)

def rack_app(request)
  rack_run_chain(chain, 0, request, {})
end

http_serve(8080, rack_app)
```

(`http_serve`'s handler is `Callable[1]` -- request only, no `context` --
so `rack_app` here takes one argument and passes an empty `Hash` through
to the chain in its place.) This is the same core (`rack_compose`/
`rack_run_chain`) as the `gremlin_serve` example above, unmodified --
proof this package doesn't actually know or care which server is
driving it.

## What's deliberately out of scope

- **Routing.** This composes middleware around one handler; it doesn't
  dispatch by path/method to different handlers. Put a router in front
  of (or as) your terminal `app_handler` if you need one.
- Everything `packages/http`'s and `packages/gremlin`'s own READMEs
  already list as out of scope (chunked transfer encoding, keep-alive,
  HTTPS/TLS) applies here too -- this package sits entirely above
  request parsing/response writing, which it never touches.
