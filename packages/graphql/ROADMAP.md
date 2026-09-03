# packages/graphql roadmap

Same kind of running log [`packages/dials/ROADMAP.md`](../dials/ROADMAP.md)
keeps -- future directions and open decisions specific to this package,
with completed work described in [`README.md`](README.md) rather than
duplicated here.

## Why this isn't a line-by-line port

Confirmed directly before designing anything (not assumed), by reading
`packages/active_record`'s own `Model` class first -- this codebase's
closest precedent for a metaprogrammed DSL -- and the relevant sections
of `docs/syntax.md`/`docs/design.md`:

- **Diamond class bodies can't execute arbitrary statements.** They're
  a fixed, static declaration list (`def`, `attr_accessor`/`reader`/
  `writer`, `alias_method`, `delegate`) -- there's no mechanism for a
  `field :name, String, null: false`-style line to run as code inside a
  class body and accumulate into shared state, the way every single
  graphql-ruby type definition works.
- **Runtime-name dispatch was unavailable when v1 was designed.** Diamond now
  provides visibility-safe `public_send`, so a future schema API could opt
  into same-named resolver methods. The current explicit `Callable` resolver
  contract remains intentional and unchanged.
- **Method-call sites can't use keyword-argument syntax.** `obj.foo(x:
  1)` only resolves for a direct call to a top-level `def`, not a
  method call, a call through a `Callable` value, or a constructor --
  every builder method in this package takes positional arguments
  (required first, then optional-with-defaults), never `name: value`.

Consequence: the schema is built with an instance-level fluent builder
(`packages/dials`'s `Router#get`/`#post` is the template), and every
field's resolver is an explicit, required `Callable` -- see `README.md`.

## Deliberately out of scope for v1

Each of these was considered and explicitly deferred, not overlooked:

- **Global (cross-branch) dataloader coalescing.** `dataloader` itself
  shipped (see "Resolved" below) -- what's still out of scope is
  graphql-ruby's own *global* batching, which coalesces every pending
  `.load()` call across the entire in-flight query tree regardless of
  nesting depth or branch. This package only batches within one list's
  own items. See `execution/dataloader.di`'s own header comment and the
  "Resolved" entry below for the full reasoning.
- **Lazy/deferred field values as a general concept** (a resolver
  returning "a value that will be ready later" for reasons other than
  dataloader batching, e.g. an async I/O call). `dataloader`'s own
  batching is the one specific mechanism this package has; there's no
  general laziness framework underneath it.
- **`subscriptions`.** Needs a pubsub/transport layer this package has
  no opinion on yet.
- **`pagination`/`relay`** (cursor-based connections). A real, common
  need, but a separate, sizable piece of API surface (`Connection`/
  `Edge` types, cursor encoding) -- add once a consumer actually needs
  paginated lists, not preemptively.
- **`tracing`/`analysis`** (query complexity cost, depth limiting,
  monitoring hooks). No consumer need yet.
- **`dashboard`** (a bundled web UI). Would need `packages/dials`/
  `packages/div` wiring and has no clear owner in this package.
- **`backtrace`** (pretty resolver-exception backtraces). A resolver
  exception's own `.message()` surfaces as the field error today;
  fancier formatting can wait.
- **`testing` helpers, `rake_task`, `rubocop`.** Ruby/Rails-ecosystem
  tooling with no Diamond equivalent -- `rake_task`/`rubocop`
  specifically will likely never be ported at all.
- **`railtie`.** No Rails.
- **SDL text parsing** (`build_from_definition`, loading a schema from
  a `.graphql` file or an introspection JSON dump) **and SDL printing**
  (`schema/printer.rb`). Schemas here are Diamond code, not text --
  `GraphQL::Language::Parser` only parses *query* documents, never
  schema definition language.
- **Authorization hooks** (`authorized?`). No auth story in this
  package at all yet.
- **Lazy/async resolution** (`GraphQL::Execution::Lazy`). Same
  reasoning as `dataloader` -- the whole point of that mechanism is
  batching/deferring, out of scope alongside it.
- **The full spec validation rule set.** Ported: field existence,
  known/required arguments (plus unique argument names),
  leaf-vs-composite selection-set correctness, fragment type conditions
  reference a real composite type, unique operation/fragment names, at
  most one anonymous operation, undefined-variable usage, and a
  simplified variable/argument type-compatibility check. Not ported:
  overlapping-fields-can-be-merged, fragment cycle detection,
  directive-valid-location checking, and the spec's own "a nullable
  variable is still allowed where non-null is expected if the location
  has a non-null default" exception to type-compatibility (this
  package's own compatibility check always requires the variable itself
  to be declared non-null). See `validation/validator.di`'s own header
  comment.
- **Type-level `description`, deprecation, and a real directive
  registry** in introspection. `Field`/`Argument` already carry a
  `description`; `ObjectType`/`InterfaceType`/`UnionType`/`EnumType`/
  `InputObjectType`/`ScalarType` don't track their own yet, so
  `__Type.description` always returns `nil`. `isDeprecated`/
  `deprecationReason` are always `false`/`nil` -- no deprecation-marking
  mechanism exists anywhere in this package. `__Schema.directives` is
  always empty -- `@include`/`@skip` are handled structurally by the
  executor but were never modeled as introspectable `GraphQL::Directive`
  values.
- **Validating *inside* a `__schema`/`__type` selection.**
  `validation/validator.di` recognizes the two root introspection
  meta-fields themselves, but doesn't recurse into their own
  sub-selections (would need building a second copy of
  `introspection.di`'s meta-types just to validate against). A typo'd
  nested introspection field still gets rejected correctly, just as an
  execution-time field error via the executor's own defensive fallback
  rather than a validation-time one.
- **`Lookahead`'s own fragment-type-condition filtering.** `.selects?`/
  `.selection` consider every selection reachable through a fragment
  spread or inline fragment unconditionally, regardless of its own
  `... on SomeType` condition -- a selection nested only under a type
  condition that wouldn't actually apply to the eventual runtime type
  still counts as "selected." A real over-approximation, acceptable for
  the primary use case (deciding whether to eager-load an association,
  almost always against a single concrete object type with no type
  condition in play at all); revisit if a polymorphic-field lookahead
  need shows up in practice. See `execution/lookahead.di`'s own header
  comment.

## Resolved

- **An object type reachable only by implementing an interface used to
  be invisible to `Schema#type_map`'s own reachability walk.**
  `InterfaceType#fields` declares a contract, not which concrete types
  satisfy it, so nothing about walking an interface's own fields ever
  turned up an implementor -- a real bug caught by testing an
  interface-typed field end to end. Fixed: `ObjectType#implements` now
  also registers itself back onto the interface
  (`InterfaceType#register_implementor`), and `Schema.visit`'s own
  `INTERFACE` branch walks known implementors the same way it already
  walked a union's own possible types.
- **Variable type references only resolved against the schema's own
  reachable types.** `$flag: Boolean!` failed to resolve if nothing in
  the schema happened to use `Boolean` anywhere reachable from the
  query/mutation roots. Fixed: `Coercion.resolve_type_reference` always
  recognizes the five built-in scalars regardless of schema reachability.
- **No `Lookahead` support at all.** A real gap in the original v1
  scoping conversation, not a deliberate exclusion -- surfaced when the
  user asked for it directly, needed to avoid over-fetching in a
  resolver mapping GraphQL queries onto minimal ActiveRecord queries.
  Added: `execution/lookahead.di`'s `GraphQL::Execution::Lookahead`,
  set on `context["lookahead"]` immediately before every resolver call
  -- see `README.md`'s own "Lookahead" section for the API and the
  `... on Type`-filtering simplification recorded above.
- **No N+1 batching at all.** Added: `execution/dataloader.di`'s
  `GraphQL::Execution::Dataloader`/`Loader`, set on
  `context["dataloader"]` once per request -- see `README.md`'s own
  "Dataloader" section for the API. Ships as **local** batching (a
  list's own items, via `#complete_list_value`) not graphql-ruby's own
  **global** cross-branch coalescing -- a deliberate scope decision made
  *before* writing any code (Diamond's bare `Fiber` primitive has no
  scheduler/promise-type/fiber-pool, so fully general coalescing would
  need every "wait for my children" point in the executor to bubble its
  own "still stuck" state up through however many levels of Fiber
  nesting sit above it, a project-sized undertaking on its own).
  **A real design mistake was caught and fixed while building this**,
  worth recording in detail: the first implementation *also*
  fiber-wrapped `#execute_selection_set`'s own per-field loop (reasoning
  it'd be "free" sibling-field batching on top of the list-item
  batching) -- this actively broke list-item batching entirely, proven
  by a test where 3 authors each loading `books` fired the batch
  function 3 times instead of 1. Cause: a field's own fiber
  (`booksFiber`) is a genuinely new Fiber; when its resolver's
  `loader.load(...)` yields, that suspends `booksFiber` itself, not the
  enclosing list-item's own fiber `#complete_list_value` is tracking --
  so the `Dataloader#run` call spawned *inside* `#execute_selection_set`
  saw its own tiny local group "stuck" and dispatched immediately, with
  only that one item's key ever having registered, before
  `#complete_list_value`'s own outer loop ever got a chance to resume
  the next sibling author. Fixed by reverting
  `#execute_selection_set` to a plain sequential loop (no fiber-
  wrapping at that level at all) -- with no intervening fiber boundary,
  `yield` inside `loader.load()` correctly propagates up through
  however many *ordinary* (non-fiber) nested calls sit above it to
  suspend the list-item's own outer fiber instead, which is what makes
  cross-item coalescing work. This is also *why* sibling-field batching
  isn't supported: it would need its own version of the same
  bubbling-up machinery explicitly scoped out above.

## Diamond-level findings worth remembering

Not bugs in this package, but real Diamond compiler behavior this
package's own build surfaced -- recorded here so a future session (in
this package or any other) doesn't have to rediscover it:

- **`def` vs `closure` for a nested function called directly --
  two things this package originally misdiagnosed as compiler bugs
  turned out to be exactly this, not bugs at all.** `docs/syntax.md`'s
  own "closure name() ... end" section documents a plain nested `def`
  as built for exactly one job -- "a detached patch, meant to be handed
  to `define_method`/`redefine_method`"; called directly instead
  (passed to `Array#map`, `Fiber.new`, or just invoked in place), its
  own `self`/`@ivar` references "don't mean anything," and (this
  package additionally found) its `Callable` arity can come out wrong
  too when handed to something like `Array#map`. `closure name() ...
  end` is the documented, correct form for a nested function that's
  called immediately and needs `self` -- legal anywhere `self` already
  exists (an instance method, or a class-owned `def self.x`, one level
  deep). Confirmed directly, twice: swapping `def` for `closure` in the
  exact same shape fixed both an `Array#map` arity mismatch ("expected
  Callable[1], got Callable") *and* a `self` reference reading back
  `nil` instead of the intended receiver, with no other code change
  either time. `execution/executor.di`'s own `#complete_list_value`
  uses `closure body() ... end` for its per-item fiber bodies for
  exactly this reason (an earlier draft used plain `def` plus a manual
  `executor = self` capture workaround -- unnecessary, removed once
  this was understood).
- **~~A `module_function` module method can't call itself recursively~~
  -- fixed at the compiler level 2026-08-25.** Qualified self-recursion
  (`ModuleName.method(...)` calling itself) used to fail at compile
  time ("undefined module singleton function") -- the exported
  descriptor only got registered after the whole body compiled,
  confirmed with a throwaway factorial fixture. Fixed in `src/compiler.c`
  by registering it as soon as the parameter list is known, before the
  body compiles (see `tests/cases/module_function_self_recursion.di`).
  Still NOT fixed, deliberately out of scope for that change: `self.
  foo(...)` recursion (a separate runtime-dispatch mechanism), and
  mutual recursion between two *different* module_function siblings
  where the callee is defined later in the same module. This package's
  own `execution/coercion.di`, `execution/executor.di`, and
  `validation/validator.di` are still written as classes with `self.`
  methods rather than `module_function` -- not reverted, since the
  class-based style works unconditionally regardless of definition
  order and isn't worth churning now that the narrower bug is fixed.
- **~~Chained double-calls/double-subscripts don't parse~~ -- fixed at
  the compiler level, no longer a workaround-only issue.**
  `type.coerce_input()(value)` and `hash["a"]["b"] = value` both used
  to fail with "expected newline after expression". Both were real,
  previously-undiscovered gaps (not documented cuts) fixed directly in
  `src/compiler.c`: `parse_precedence`'s own postfix-chaining loop only
  ever checked for a following `.`/`[` after an expression, never a
  bare `(` (fixed by reusing `parse_closure_call_arguments`, the
  existing "call whatever Callable is in this register" helper already
  used for a local/`@ivar`/`@@cvar` holding one); `index_assignment_ahead`
  only ever scanned a single `[...]` group before checking for `=`, so
  any chain longer than one level fell through to plain expression
  parsing instead (fixed by scanning through consecutive `[...]` groups,
  and having `compile_index_assignment` emit an ordinary read for every
  group but the last). Both ship on `main` with their own new
  `tests/cases/` coverage (`chained_call_expression.di`,
  `chained_indexed_assignment.di`). This package's own workarounds (an
  intermediate local before a second call, in `execution/coercion.di`'s
  `coerce_literal`/`coerce_runtime_value` and `execution/executor.di`'s
  `complete_value`/`#execute_field`) were cleaned up in a later
  language-limitation sweep -- `type.coerce_input()(value)` and
  `schema_field.resolve()(object_value, args, context)` directly, no
  intermediate `coercer`/`resolve` local.

## Open questions

- **Named types' own `description`/deprecation support.** Worth adding
  once introspection needs to describe more than field/argument-level
  documentation (see "out of scope" above).
- **`dataloader`-style batching.** The most likely next real need if
  this package gets wired into a real app backed by
  `packages/active_record` -- watch for actual N+1 pain before building
  it.
- **Wiring into `examples/library` (or a new example) via
  `packages/dials`.** Not started -- this package has landed
  standalone, matching how `packages/div`/`packages/dials` themselves
  shipped before `examples/library` was updated to use them.
