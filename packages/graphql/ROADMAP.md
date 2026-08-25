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
- **No dynamic dispatch by a runtime name.** No `send`/`public_send`.
  `method_missing` exists but only fires when a call site's own
  *literal* method name (written in Diamond source) fails to resolve --
  it can't be used to invoke "the method named by this string I have in
  a variable," which is exactly what graphql-ruby's default field-
  resolution (`obj.public_send(field.method_sym)`) depends on. A GraphQL
  query's field names only exist as runtime strings (parsed from the
  query document), so there's no way to auto-wire a field to a
  same-named resolver method.
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

- **`dataloader`** (N+1 batching via Fiber-based lazy resolution).
  Execution here is plain synchronous Diamond calls; adding batching
  would mean reworking the whole executor around a lazy/deferred value
  concept graphql-ruby's own `execution/interpreter/runtime.rb` (1012
  lines) exists to solve. Revisit if a real consumer hits N+1 query
  problems in practice (`packages/active_record`'s own `has_many` is
  the likely trigger).
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

## Diamond-level findings worth remembering

Not bugs in this package, but real, previously-undocumented Diamond
compiler behavior this package's own build surfaced -- recorded here so
a future session (in this package or any other) doesn't have to
rediscover them:

- **A `module_function` module method can't call itself recursively**,
  even spelled `ModuleName.method(...)` (the usual fix for the
  sibling-call gotcha) -- fails at runtime with a plain "type error."
  Confirmed with a throwaway factorial fixture. `execution/coercion.di`,
  `execution/executor.di`, and `validation/validator.di` are all
  written as classes with `self.` methods instead, since several of
  their own methods genuinely recurse (nested list/non-null unwrapping,
  nested input objects, nested selection sets). A class's own
  `self.method(...)` recursing into itself via `self.` works fine, just
  not via `ClassName.method(...)`.
- **A closure `def` nested inside a `self.` method fails Callable
  arity-checking when handed to `Array#map`** ("expected Callable[1],
  got Callable"), even though the identical nested `def` works fine
  inside a plain top-level function. A real, previously-undiscovered
  compiler bug (distinct from the already-fixed self-capturing-
  instance-closure arity bug from earlier this session) -- worked
  around throughout this package with explicit index loops instead of
  `.map(nested_def)`, not chased down further.
- **Chained double-calls don't parse**: `type.coerce_input()(value)`
  ("expected newline after expression") -- store the intermediate
  `Callable` in a local first (`coercer = type.coerce_input();
  coercer(value)`).

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
