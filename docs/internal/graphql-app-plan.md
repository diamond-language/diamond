# Plan: schema-first GraphQL apps (`GraphQL::App`)

Status: in progress on branch `graphql-app` (filed 2026-11-17). Steps 1-3 are written
(graphql 0.3.6): the SDL parser, `GraphQL::App`, and directive middleware with `Fail`. Step 4
ports Pheint. The roadmap entry is "Schema-first GraphQL apps" under "Grow packages from
application needs".

## The problem

The `graphql` cut builds a schema in code: one `ObjectType.new` per type, one `.field` call per
field, and one resolver per field that needs anything besides a same-name lookup. Pheint is a
real GraphQL API on this cut, and its `lib/api.di` (1,166 lines) holds four kinds of repetition.
Measured at Pheint 1c867db:

| Repetition | Count | Why it is redundant |
|---|---|---|
| Schema-building lines (`.field`, `.value`, `GraphQL::*Type.new`) | 332 | They restate `frontend/schema.graphql` (444 lines), which Pheint already commits and checks for drift |
| Resolvers that only read a property (`object.key_name()`, `== 1`, `.to_s()`) | 104 of 226 | They exist only to map camelCase to snake_case or to convert a value |
| Inline access checks (`Auth.player!(context)` and friends) | 57 | The same handful of rules, repeated |
| `context["repo"], context["schemas"]` argument pairs | 107 | The context is an untyped Hash |

The remaining resolvers hold real application logic, and they stay.

## The design

The `.graphql` file becomes the source of truth. The app binds a resolver class to each type
that needs one, and everything else resolves by convention.

```graphql
directive @auth(as: Principal!) on FIELD_DEFINITION

type Leaderboard {
  id: ID!
  slug: String!
  trustedOnly: Boolean!          # no resolver: reads trusted_only, 0/1 becomes a Boolean
  entries(limit: Int = 25, offset: Int = 0, scope: LeaderboardScope = ALL): [LeaderboardEntry!]! @auth(as: ANY)
}

type Mutation {
  createGame(slug: String!, name: String!, description: String): Game! @auth(as: DEVELOPER)
}
```

```ruby
class LeaderboardResolvers
  def entries(board, args, ctx) = Leaderboards.page(ctx.repo, board, args["limit"], args["offset"], ctx.scope_ids(args["scope"]))
end

class MutationResolvers
  def create_game(_, args, ctx) = Games.create(ctx.repo, ctx.account, args)
end

schema = GraphQL::App.new(File.read("schema.graphql"))
  .resolvers("Mutation", MutationResolvers.new())
  .resolvers("Leaderboard", LeaderboardResolvers.new())
  .directive("auth", Guards.auth)   # wraps every field marked @auth; raises to deny
  .build()
```

### How a field gets its value

The executor tries three sources, in order:

1. A method on the resolver class bound to the field's type. The field name goes from
   camelCase to snake_case, so `entries` stays `entries` and `trustedOnly` becomes `trusted_only`.
2. Otherwise, a method of that name on the object (`object.trusted_only()`).
3. Otherwise, a key on a Hash object: snake_case first, then camelCase.

The value is then coerced to the declared type: an Int becomes an `ID` string, a String becomes an
enum value, and 0/1 becomes a `Boolean`. The 0/1 rule is opt-in per schema, for SQLite rows.

### Other parts

- **Directives as middleware.** `.directive(name, fn)` wraps every field that carries the
  directive. The function receives the directive's arguments, the object, the field's arguments
  and the context, and raises to deny. `@auth`, page-size limits and rate limits each become one
  function in place of inline checks.
- **Any context object.** The app passes its own class (`ctx.repo`, `ctx.account`), not a Hash.
- **`Fail.signal(code, message)`** moves from Pheint into the cut as the standard error with a
  code in `extensions`.
- **Validation in `build()`.** It raises once and lists every problem: a Query or Mutation field
  with no resolver method, a resolver class bound to an unknown type, and a directive with no
  registered function.

### Language support this relies on

Binding and the field fallback use `respond_to?(name)` and `public_send(name, ...)` on class
instances, which both work, and both take a Symbol or a String. Resolvers are classes, not
modules: a module answers `respond_to?`, but `public_send` on a module does not compile, because a
module is not a value.

## Steps

Ship it as one branch with one draft PR, pushed after each step. It is a minor release of the
`graphql` cut, not a new cut: the parser and executor already exist.

1. **SDL parser (written).** `Schema.from_sdl` parses type-system definitions into the cut's existing
   schema objects, keeping field directives and type descriptions. Pheint's 444-line `schema.graphql`
   reads back through `to_sdl()` byte for byte (checked by hand; the package tests use their own
   schema, since Pheint is a private repository).
2. **`GraphQL::App` (written).** Binding by name, the three-source field lookup, the value
   coercions and `build()` validation. Interfaces and unions default to the value's class name, or a
   Hash's `"__typename"`, and `resolve_type` overrides that. The executor writes `"dataloader"` and
   `"lookahead"` into the context, so a custom context class includes `GraphQL::Context`.
3. **Directive middleware, and `Fail` in the cut (written).** A registered function is called as
   `(directive_args, object, args, context, resolve)` and wraps the field's resolver; the first
   directive written runs outermost. `GraphQL::Fail` has `signal`, `code` and `annotate`; Pheint's
   HTTP `status` mapping stays in Pheint, since the codes are its own.
4. **Port Pheint.** Its `compat_test` and `docs/api-baseline.graphql` already pin the public API.
   The expected result is `api.di` going from about 1,170 lines to about 450.

## Rejected: a shorter code-first syntax

A terser d6 syntax for declaring the schema would keep two descriptions of the same API. The
Pheint frontend's codegen already reads the `.graphql` file, so the file stays the source of truth.
