# packages/dials

The controller layer for [Diamond](https://gitlab.com/dmn9180/diamond):
declarative routing, request-param parsing, and generic
`[status, headers, body]` responses -- sitting on top of
[`packages/rack`](../rack/README.md)'s request/response convention,
alongside [`packages/div`](../div/README.md) (views) and
[`packages/active_record`](../active_record/README.md) (models). "Dials"
because Diamond has Dials.

Both [`packages/http`](../http/README.md)'s and `packages/rack`'s own
READMEs explicitly leave routing to "the handler itself, or a future
library on top" -- this is that library.

## Install

Same story as every other package here -- copy this directory into
another project as `cuts/dials/`, or give it its own git remote and
depend on it via `facet` (see
[`docs/packages.md`](../../docs/packages.md)).

## Routing

```ruby
require "path/to/dials/lib/dials"

def authors_index(request, context, params) = AuthorsController.index(request, context, params)
def authors_show(request, context, params) = AuthorsController.show(request, context, params)
def authors_create(request, context, params) = AuthorsController.create(request, context, params)

router = Dials::Router.new()
router.get("/authors", authors_index)
router.get("/authors/:id", authors_show)
router.post("/authors", authors_create)

response = router.dispatch(request, context)
```

`Router.new()` builds an empty route table; `.get(pattern, handler)`/
`.post(pattern, handler)` register a route against it, in the order you
call them -- the first one whose verb and path both match wins.
`handler` is an ordinary `Callable[3]`: `(request, context, params)`.
A pattern segment starting with `:` (`/authors/:id`) matches any path
segment and captures it into `params["id"]`; every other segment must
match literally, and segment *count* must match exactly (no wildcard/
catch-all segment -- see [`ROADMAP.md`](ROADMAP.md)). No route matches
-> `Dials::Response.not_found(path)`.

`params` is always a `Hash`, whether or not the route captured anything:
query-string params for a `GET`, form-body params for anything else
(`Dials::Params.parse`, the same request shape `packages/http`'s own
README documents), with any path-captured `:name` values merged in
*after* -- a path param always wins over a same-named query/body one.

## Why controllers still need one small shim function per action

A class-owned `self.` method is not a referenceable value in
Diamond -- `AuthorsController.show` (no call) is a parse error, only a
bare *top-level* `def`'s name is. So a route's `handler` can never be a
controller's `self.` method directly; it needs exactly one bare
top-level function per action forwarding into it, as in the routing
example above. This is not a limitation `dials` introduces -- it's a
real, confirmed Diamond constraint (`ClassName.compile_method` can't
help either: a synthesized method body can't name an external class it
was never told about), and it's the same shim `packages/rack`'s own
examples already need for their own terminal app handler.

A controller itself is an ordinary class with `self.` action methods,
now taking a third `params` argument alongside the usual `request`/
`context`:

```ruby
class AuthorsController
  def self.index(request, context, params)
    # ...
  end
  def self.show(request, context, params)
    id = params["id"]
    # ...
  end
end
```

No `Dials::Controller` base class or mixin -- there's no shared state or
template-method behavior that would benefit from real inheritance here
(`self.` dispatch already resolves dynamically per class without one),
so this stays a documented convention, not a type you inherit from.

## Wiring into `rack`/`gremlin`

A `Dials::Router` instance is mutable (built once via `.get`/`.post`,
read on every request) -- exactly the shape `packages/rack`'s own
`RackChain` exists for: `gremlin_serve(..., threads: N)` spawns real OS
threads via `Thread.new`, which categorically rejects a *capturing*
closure, so the only safe way for a bare top-level dispatch shim to
reach a stateful `Router` is a class-variable-memoized per-worker
singleton, not a closure capturing it. `Dials::RouterHolder.get(builder)`
mirrors `RackChain.get` exactly:

```ruby
require "path/to/rack/lib/rack"
require "path/to/dials/lib/dials"

def build_router()
  router = Dials::Router.new()
  router.get("/authors", authors_index)
  router.get("/authors/:id", authors_show)
  router
end

def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

def app(request, context)
  chain = rack_compose([logging_middleware], route)
  rack_run_chain(chain, 0, request, context)
end

gremlin_serve(18080, app)
```

`build_router()` stays a zero-capture top-level function (it only
references other bare top-level functions, never a local), the same
constraint every other Thread.new-safe handler in this codebase already
works within.

## `Dials::Response`

```ruby
Dials::Response.text(200, "hello")           # -> [200, {"Content-Type": "text/plain"}, "hello"]
Dials::Response.redirect("/authors", "moved") # -> [302, {"Location": "/authors"}, "moved"]
Dials::Response.not_found(request["path"])    # -> [404, {"Content-Type": "text/plain"}, "not found: ..."]
```

Protocol-generic only -- an HTML response still goes through
`Div.html_response` (`packages/div`) directly at the controller call
site; `dials` stays `div`-agnostic, the same way it stays
`active_record`-agnostic.

## Tests

```
make test-dials-package
```
