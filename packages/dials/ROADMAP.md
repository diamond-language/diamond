# packages/dials roadmap

This document is the same kind of running log
[`packages/arel/ROADMAP.md`](../arel/ROADMAP.md) keeps -- future
directions and open decisions specific to this package, with completed
work described in [`README.md`](README.md) rather than duplicated here.

## Deliberately out of scope for v1

Each of these was considered and explicitly deferred, not overlooked:

- **`resources`-style route macros.** Confirmed with the user before
  building this package: fully explicit `.get`/`.post` calls per route
  for v1, matching this repo's own repeated "no macro until a concrete
  need surfaces" pattern (`has_many`/`belongs_to`, `delegate`). A
  `resources("authors", ...)` convenience generating the usual seven
  routes would also have to assume a fixed shim-function naming
  convention that doesn't exist anywhere in this codebase yet.
- **Verbs beyond `GET`/`POST`.** The only two `examples/library` (or any
  other consumer) has ever needed -- HTML forms can't natively send
  `PUT`/`PATCH`/`DELETE` anyway, so the established convention here (and
  in `examples/library` before this package existed) is a literal
  `/:id/delete`-style path segment under `POST`, not a real HTTP verb.
  Add real verb support if a real JSON-API-style consumer needs it.
- **Wildcard/catch-all route segments** (`/files/*path`). Every
  registered pattern's segment *count* must match a request path's
  exactly; `Router.match_segments` (`lib/dials/router.di`) has no
  "match the rest" concept. Revisit if a real consumer needs one.
- **A `Dials::Controller` base class or mixin.** A controller is an
  ordinary class with `self.` action methods -- there's no shared state
  or template-method behavior `self.` dispatch (already dynamic per
  class) would benefit from inheriting, so this stays a documented
  convention (README's "Controllers"), not a type to subclass.

## Resolved

- **The one-shim-per-action requirement -- resolved in Diamond itself,
  not worked around here.** This section used to say a class-owned
  `self.` method could never be referenced as a bare value in Diamond,
  confirmed directly at the time (`ClassName.compile_method` couldn't
  synthesize a trampoline around one either, since a synthesized method
  body can't name an external class it was never told about), and
  concluded "revisit only if the language itself grows a way to
  reference a `self.` method as a value." It did: `ClassName.method`/
  `ModuleName.method` with no call is now a `Callable` value (see
  `docs/design.md`'s "Bare singleton method references" and `docs/
  syntax.md`'s section of the same name) -- turned out buildable with no
  VM/opcode changes at all, since `ClassName.method(...)` was already
  fully resolved at compile time, never runtime-dispatched, so a
  synthesized wrapper reusing that same compiled shape had nothing
  dynamic to get wrong. Routes here now pass `AuthorsController.show`
  directly; see README's "Controllers".

## Open questions

- **Named routes / URL generation.** Nothing here builds a path back
  *from* a route name (`authors_path(id: 5)`-style) -- every response
  that needs a URL builds it by hand (`"/authors/#{id}"`, as
  `examples/library`'s own controllers already do). Worth adding once
  building URLs by hand becomes a real, repeated pain point, not
  preemptively.
- **Route-level middleware/filters** (an auth check scoped to one route
  or group, say). `packages/rack`'s own middleware chain already covers
  cross-cutting concerns for an entire app; nothing here composes with
  it at a *per-route* granularity yet.
