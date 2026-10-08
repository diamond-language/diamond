# dials

Route requests to controllers and construct HTTP responses.

## Installation

`dials` is not published to the registry yet. Until it is, copy `packages/dials` from a checkout of the [Diamond repository](https://github.com/diamond-language/diamond) into your project as `cuts/dials/`, then load it with `require_cut "dials"`. `facet update` leaves hand-copied cuts in place.

## Usage

A route handler is any callable taking `(request, context, params)` and returning
`[status, headers, body]`. `params` merges the query string or form body with the
`:name` captures from the path (captures win on a name clash). Grouping handlers as
class methods keeps a project readable:

```ruby
# app.di
require_cut "gremlin"
require_cut "dials"

class AuthorsController
  def self.index(request, context, params)
    Dials::Response.text(200, "authors")
  end

  def self.show(request, context, params)
    Dials::Response.text(200, "author #{params["id"]}")
  end

  def self.create(request, context, params)
    Dials::Response.redirect("/authors/#{params["name"]}", "created")
  end
end

def build_router()
  router = Dials::Router.new()
  router.get("/authors", AuthorsController.index)
  router.get("/authors/:id", AuthorsController.show)
  router.post("/authors", AuthorsController.create)
  router
end

def app(request, context)
  router = Dials::RouterHolder.get(build_router)
  router.dispatch(request, context)
end

gremlin_serve(8080, app)
```

```sh
diamond app.di
curl localhost:8080/authors/7        # author 7
curl -d name=ada localhost:8080/authors   # 302 to /authors/ada
curl localhost:8080/nope             # 404 "not found: /nope"
```

`RouterHolder.get` builds the router once per server worker and reuses it, which is
why the dispatch function is a top-level `def` rather than a closure over a router
built in the main program: each `gremlin_serve` worker thread runs in its own VM.
With the `http` cut's `http_serve`, whose handler takes only `request`, call `router.dispatch(request, {})` yourself.

## Filters and responses

`router.get(pattern, handler, filters, name)` and `router.post(...)` accept an array
of filters, callables with the same `(request, context, params)` signature. Filters
run in order before the handler; the first one that returns a response (anything but
`nil`) short-circuits the request, which makes them the place for authentication
checks.

`Dials::Response` builds the three common triples: `text(status, body)`,
`redirect(location, body)` (status 302), and `not_found(path)`. A handler can return
any `[status, headers, body]` array, such as `Div.html_response(200, html)` from the
[`div`](https://github.com/diamond-language/diamond/tree/main/packages/div) cut.

## Notes

Routes are tested in registration order; the first match for the verb and path wins.
Segment counts must match exactly, and there is no wildcard segment. The query string
is ignored when matching. A request that matches no route gets the `not_found`
response. Pass a `name` to a route to generate `<name>_path(...)` helper functions
with `Dials::PathHelpers.generate_source(router.routes())`.
