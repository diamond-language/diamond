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

router = Dials::Router.new()
router.get("/authors", AuthorsController.index)
router.get("/authors/:id", AuthorsController.show)
router.post("/authors", AuthorsController.create)

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

### Route filters

An optional third argument to `.get`/`.post` is an ordered `Array` of
route filters. Each filter is a `Callable[3]` receiving the same
`(request, context, params)` values as the action. Return `nil` to allow
dispatch to continue, or return a Rack-style response to stop immediately;
later filters and the action are not called after a short circuit.

```ruby
def require_user(request, context, params)
  if context["current_user"] == nil
    Dials::Response.redirect("/login", "authentication required")
  else
    nil
  end
end

router.get("/authors", AuthorsController.index)
router.get("/authors/new", AuthorsController.new_form, [require_user])
router.post("/authors", AuthorsController.create, [require_user])
```

Filters belong on the routes whose policy they enforce. Whole-application
concerns such as request logging and loading a session into `context` still
fit Rack middleware better. Filters are deliberately before-action hooks,
not around middleware: they need no capturing `forward` closure, keeping a
memoized router straightforward under `gremlin_serve(..., threads: N)`.

## Controllers

`AuthorsController.show` above, with no call, is a bare reference to a
`self.` singleton method -- a `Callable` value Diamond synthesizes a
small zero-capture wrapper for, matching the method's own parameter list
exactly (see `docs/syntax.md`'s "Bare singleton method references").
Registering a route no longer needs a hand-written shim function per
action; pass the controller method directly.

(Earlier versions of this package needed exactly one top-level shim
function per action, since a `self.` method wasn't a referenceable value
at all before this Diamond feature existed. A *variadic* or *generic*
`self.` method still can't be referenced this way -- both are rejected
with a clear compile error, not silently narrowed -- but no ordinary
controller action is either.)

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
  router.get("/authors", AuthorsController.index)
  router.get("/authors/:id", AuthorsController.show)
  router
end

def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

def app(request, context)
  chain = rack_compose([logging_middleware], route)
  rack_run_chain(chain, 0, request, context)
end

gremlin_serve(18080, app)
```

`build_router()` stays a zero-capture top-level function -- true of a
bare singleton method reference too (`AuthorsController.index` bakes its
target class as a compile-time constant, capturing nothing), the same
constraint every other Thread.new-safe handler in this codebase already
works within.

## Named routes and path helpers

An optional fourth positional argument to `.get`/`.post` names the route:

```ruby
router.get("/authors/:id", AuthorsController.show, [], name: "author")
router.post("/authors/:id", AuthorsController.update, [require_user], name: "author")
```

Two routes can share one name -- the GET show and POST update above both
resolve to one `author_path` helper, the same way Rails' own `resources`
gives you one `author_path` covering show/update/destroy on the same path.

Diamond has no way to synthesize a real top-level function
(`author_path(id)`) at runtime: `ClassName.compile_method`/`.define_method`
only attach **instance** methods (`docs/internal/design.md`'s own scoping note), and
there's no `self.method_missing`. So path helpers are generated *offline*
into a real, checked-in `.di` file -- `Dials::PathHelpers.generate_source
(router.routes())` turns every named route into a function definition:

```ruby
require "path/to/dials/lib/dials"

router = build_router() # your own app's function, as above
File.open("lib/routes/path_helpers.di", "w").write(
  Dials::PathHelpers.generate_source(router.routes())
).close()
```

For `router.get("/authors/:id", ..., name: "author")` this generates:

```ruby
def author_path(id, query: Hash = {}) = "/authors/#{id}" + Dials::PathHelpers.query_suffix(query)
```

Every generated helper takes an optional trailing `query:` Hash for a
percent-encoded `?key=value&...` suffix: `author_path(5, query: {"tab":
"books"})` -> `"/authors/5?tab=books"`.

**Commit the generated file** -- it isn't a gitignored build artifact like
a `packages/div` template's compiled `.cache/*.di` output. Generating it
needs your app's `build_router()` actually running, which needs your whole
controller/model graph loaded, i.e. the generator itself needs `require
"./boot"` (or equivalent) to succeed -- if your own boot chain in turn
unconditionally required this generated file, a fresh clone could never
bootstrap far enough to run the generator that creates it in the first
place. Regenerate and re-commit whenever your route table changes, the
same "generate, review the diff, commit" workflow as any other checked-in
generated code. See `skindicate.dia`'s own `generate_route_helpers.di` +
`compile_routes.sh` for a complete driver-script example.

`Dials::PathHelpers.query_suffix`/the generated helpers' own `query:` kwarg
both go through `url_encode` (`lib/dials/url_encoding.di`), a small
percent-encoder this package also exposes as a bare top-level function
(not `Dials`-scoped, so it reads the same whether you're inside this
package or calling it from your own view templates).

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
