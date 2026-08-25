# packages/graphql

A from-scratch, spec-inspired GraphQL query engine for
[Diamond](https://gitlab.com/dmn9180/diamond) -- not a line-by-line
port of the Ruby [`graphql`](https://github.com/rmosolgo/graphql-ruby)
gem this package is modeled on. That gem's whole public API is built on
Ruby metaprogramming Diamond doesn't have (class bodies that execute
arbitrary macro calls, dispatch-by-runtime-string-name) -- see
[`ROADMAP.md`](ROADMAP.md) for the full story on what that means for
this package's own shape, and why schemas here are built with a fluent
instance builder (`packages/dials`'s own `Router#get`/`#post` is the
template) instead of Ruby-style `field :name, String` class-body
macros.

## Install

Same story as every other package here -- copy this directory into
another project as `cuts/graphql/`, or give it its own git remote and
depend on it via `facet` (see
[`docs/packages.md`](../../docs/packages.md)).

## Building a schema

```ruby
require "path/to/graphql/lib/graphql"

module BookResolvers
  module_function
  def title(object, args, context) = object["title"]
end

module AuthorResolvers
  module_function
  def id(object, args, context) = object["id"]
  def name(object, args, context) = object["name"]
  def books(object, args, context) = object["books"]
end

module QueryResolvers
  module_function
  def author(object, args, context) = context["db"][args["id"]]
end

BookType = GraphQL::ObjectType.new("Book")
BookType.field("title", GraphQL::ScalarType.string().non_null(), BookResolvers.title)

AuthorType = GraphQL::ObjectType.new("Author")
AuthorType.field("id", GraphQL::ScalarType.id().non_null(), AuthorResolvers.id)
AuthorType.field("name", GraphQL::ScalarType.string().non_null(), AuthorResolvers.name)
AuthorType.field("books", GraphQL::ListType.of(BookType), AuthorResolvers.books)

QueryType = GraphQL::ObjectType.new("Query")
QueryType.field("author", AuthorType, QueryResolvers.author,
  [GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())])

Schema = GraphQL::Schema.new()
Schema.query(QueryType)
```

`ObjectType.new(name)` builds an empty type; `.field(name, type,
resolve, arguments = [], description = nil)` registers a field against
it and returns `self`, so repeated calls read like a declaration list
even though each is an ordinary method call (Diamond has no
class-body-macro mechanism to make this a single expression the way
graphql-ruby's own `field :name, String, null: false` is -- see
`ROADMAP.md`). `resolve` is always required and always an explicit
`Callable` -- there's no auto-wiring a field to a same-named resolver
method the way graphql-ruby's own default resolution does, since
Diamond has no dispatch-by-runtime-string-name at all. A bare `self.`
singleton method reference (`AuthorResolvers.id`, no call --
`docs/syntax.md`'s "Bare singleton method references") is the usual
shape; an inline closure works too.

`GraphQL::Type` is the common base every concrete kind
(`ScalarType`/`ObjectType`/`InterfaceType`/`UnionType`/`EnumType`/
`InputObjectType`, plus the two wrapper kinds) subclasses, purely for
`.non_null()` (wraps in `NonNullType`) and `.list()`/`ListType.of(type)`
(wraps in `ListType`) -- these compose (`STRING.non_null().list()` is
`[String!]`).

Built-in scalars: `GraphQL::ScalarType.string()`/`.int()`/`.float()`/
`.boolean()`/`.id()` -- a fresh instance each call (Diamond has no
top-level constant binding, so these are factory methods, not
constants), fine since they're identity-less immutable values.

`InterfaceType`/`UnionType` need `.resolve_type(callable)` (a
`(object, context) -> String` `Callable` returning the concrete
implementing/member type's name) before any interface- or union-typed
field can actually be resolved -- there's no way to guess a concrete
type from a plain Diamond value otherwise, same "explicit Callable, no
magic" stance as `Field#resolve`. `ObjectType#implements(interface)`
registers the relationship in both directions, so
`Schema#type_map`/`#execute` can find an object type that's only
reachable by implementing an interface (see `ROADMAP.md`'s "Resolved"
section for why that matters).

## Executing queries

```ruby
result = Schema.execute(query_string, variables, context, root_value, operation_name)
# => {"data": {...}}
# or {"data": {...}, "errors": [{"message": ..., "path": [...]}]}
# or {"errors": [{"message": ...}]}   -- no "data" key: a request-level
#                                          failure (parse error, unknown
#                                          operation, failed validation,
#                                          a variable that doesn't coerce)
```

All four trailing arguments are optional (default `{}`/`{}`/`nil`/
`nil`) and positional -- `Schema#execute` is an ordinary method call, so
Diamond's keyword-call syntax doesn't reach it either (`docs/syntax.md`'s
"Method calls... stay positional-only"). `operation_name` is only
required when `query_string` defines more than one operation.

Execution is synchronous and spec-shaped (`ExecuteRequest` ->
`ExecuteSelectionSet` -> `ExecuteField` -> `CompleteValue`): fragment
spreads and inline fragments (typed and untyped) are expanded,
`@include`/`@skip` are honored, and a `null` result for a `!`-typed
field bubbles up to the nearest nullable ancestor per spec (recording
exactly one error at the point of origin, not once per level it passes
through on the way up).

A resolver raising `GraphQL::ExecutionError` surfaces its own message
as that field's error; any other exception surfaces its own
`.message()` the same way (still just that one field's result going
`null`, siblings unaffected, unless the field itself is non-null).

## Lookahead

`context["lookahead"]` is set immediately before every resolver call --
a `GraphQL::Execution::Lookahead` scoped to that field's own
sub-selections, for deciding whether to do expensive work (an eager
load, say) *before* doing it, not after:

```ruby
module AuthorResolvers
  module_function
  def books(object, args, context)
    if context["lookahead"].selects?("title")
      # the query actually asked for book titles -- eager-load them
    else
      # skip it, the query didn't ask
    end
    object.books()
  end
end
```

A resolver one level up can peek into a *not-yet-resolved* child
field's own selections the same way, via `.selection(field_name)`
(returns another `Lookahead`, empty -- so every `.selects?` under it is
`false` -- if `field_name` wasn't selected at all):

```ruby
def author(object, args, context)
  if context["lookahead"].selection("books").selects?("title")
    # decide up front whether resolving `author` should also prefetch
    # book titles in the same query, before `books`' own resolver runs
  end
  object
end
```

`.selections()` returns every field name selected at this level, for a
resolver that wants to see everything at once. Read `context["lookahead"]`
synchronously, during your own resolver call -- it's the same shared
`context` Hash mutated in place for every field, not a fresh copy per
field (see `execution/executor.di`'s own header comment on why that's
safe here). Deliberately simpler than graphql-ruby's own Lookahead: a
selection nested only under `... on OtherType` on a polymorphic field
still counts as "selected" here regardless of the runtime type (see
`ROADMAP.md`).

## Validation

Every `Schema#execute` call validates the parsed document before
running any resolver -- a request that fails validation returns *only*
`{"errors": [...]}`, no partial `"data"` at all. This is a deliberate
*subset* of the spec's own ~30 rules; see `ROADMAP.md` for exactly
what's covered and what isn't.

## Introspection

`__schema`, `__type(name: "...")`, and `__typename` all work out of the
box on every `Schema` -- no separate opt-in, built the same way a
user's own types are (`packages/graphql/lib/graphql/introspection.di`).
Good enough for GraphiQL/Apollo-style tooling to introspect a server;
see `ROADMAP.md` for what's deliberately not tracked yet (type-level
descriptions, deprecation, a real directive registry).

## Tests

```
make test-graphql-package
```
