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

- **Route-level middleware/filters.** A real authentication consumer,
  `examples/project_board`, made global middleware inspect a growing list
  of methods and path strings to decide which routes were protected. Routes
  now accept an ordered array of before filters: each receives the action's
  `(request, context, params)`, returns `nil` to continue, or returns a
  response to short-circuit. This keeps authorization policy beside the
  route without synthesizing capturing `forward` closures in a router used
  by threaded Gremlin workers. Whole-app around concerns remain Rack's job.

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

- **Named routes / path helpers.** `skindicate.dia` hit exactly the
  "real, repeated pain point" this section used to wait for -- the same
  route shapes (`/skins/:id`, `/users/:username/followers`, `/admin*`)
  hand-interpolated 3-10+ times each across its own controllers and view
  templates. `.get`/`.post` now take an optional `name:` (`lib/dials/
  router.di`), `Router#routes()` exposes the built table, and
  `Dials::PathHelpers.generate_source` (`lib/dials/path_helpers.di`)
  turns it into real `#{name}_path(...)` functions -- see README's
  "Named routes and path helpers". Not built: a `_url` host-aware
  variant (every consumer so far is same-origin-relative-path-only) and
  no `resources`-style macro tying route registration to name derivation
  automatically (still fully explicit, per this file's own "Deliberately
  out of scope for v1" above) -- naming stays a per-route opt-in.

## Open questions

Nothing open right now.
