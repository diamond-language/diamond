# graphql

Define GraphQL schemas and execute queries in Diamond.

## Installation

Install the cut at `cuts/graphql/` and load it with `require_cut "graphql"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## Usage

```ruby
require_cut "graphql"

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

result = Schema.execute("{ author(id: \"1\") { name } }", {}, {"db": db})
```

## Defining a schema

Types are plain values built with chained calls; there are no class-body macros.

### Scalars and wrappers

`GraphQL::ScalarType.string()`, `.int()`, `.float()`, `.boolean()`, `.id()`. Each call returns a new instance.

`type.non_null()` wraps a type as `T!`. `type.list()` or `GraphQL::ListType.of(type)` wraps it as `[T]`. They compose, so `[T!]!` is `GraphQL::ListType.of(t.non_null()).non_null()`.

A custom scalar is `GraphQL::ScalarType.new(name, coerce_input, coerce_result)`. Both are required `Callable[1]`s. `coerce_input` turns a literal or variable into the value resolvers receive, and `coerce_result` turns a resolver's return value into the value placed in the response.

### Object types

```ruby
PlayerType = GraphQL::ObjectType.new("Player")
PlayerType.field("handle", GraphQL::ScalarType.string().non_null())
PlayerType.field("scores", GraphQL::ListType.of(ScoreType.non_null()).non_null(),
  PlayerResolvers.scores, [
    GraphQL::Argument.new("limit", GraphQL::ScalarType.int(), 20, true)
  ])
```

`field(name, type, resolve = nil, arguments = [], description = nil)` returns the type, so calls chain.

A resolver is a `Callable[3]` called as `(object, args, context)`. If `resolve` is omitted, the executor calls a public zero-argument method with the field's own name on the runtime object. That is convenient for model objects with matching accessors. Use an explicit resolver when you need arguments, the context, a name translation (a camelCase field over a snake_case method), or a Hash lookup.

`implements(interface_type)` on an object type registers it with an interface.

### Arguments

`GraphQL::Argument.new(name, type, default_value = nil, has_default = false, description = nil)`

`has_default` is a separate flag from `default_value` because GraphQL distinguishes "default is `null`" from "no default". To give an argument a default, pass both: `Argument.new("limit", int, 20, true)`. A resolver reads values from the `args` Hash, keyed by the GraphQL argument name (`args["limit"]`, `args["leaderboardId"]`).

An `ID` argument reaches the resolver exactly as the client sent it: an `Int` for `id: 1`, a `String` for `id: "1"`. Normalise it (`id = id.to_i() if id is String`) before using it as a database key.

### Other type kinds

- `GraphQL::EnumType.new(name).value("NAME", description = nil)`
- `GraphQL::InputObjectType.new(name).argument(name, type, default = nil, has_default = false, description = nil)`
- `GraphQL::InterfaceType.new(name).field(...)` with `resolve_type(callable)` to pick the concrete type
- `GraphQL::UnionType.new(name).possible_type(object_type)` with `resolve_type(callable)`

### Schema

```ruby
schema = GraphQL::Schema.new().query(QueryType).mutation(MutationType)
```

`mutation` is optional. `type_map()` returns every type reachable from the roots, which introspection uses.

## Executing

```ruby
result = schema.execute(query_string, variables = {}, context = {}, root_value = nil, operation_name = nil)
```

The result is a Hash:

- `{"data": ...}` on success.
- `{"data": ..., "errors": [...]}` when one or more fields errored. Fields that did resolve are still returned.
- `{"errors": [...]}`, with no `"data"` key, for a request-level failure: an unparseable document, an unknown operation, or a variable or argument that doesn't coerce.

The `context` Hash is yours. Whatever you pass is handed to every resolver, which is the usual place for the database connection and the signed-in user. Two entries are set by the executor for each request: `context["lookahead"]` and `context["dataloader"]`.

### Errors

Raise `GraphQL::ExecutionError.new("message")` from a resolver to return a specific, client-visible message for that field. Any other `StandardError` is reported as a generic internal error, so an implementation detail doesn't leak to the client.

```ruby
def sign_in(object, args, context)
  raise GraphQL::ExecutionError.new("invalid email or password") unless ok
  # ...
end
```

### Lookahead

Immediately before each field resolves, the executor sets `context["lookahead"]` to a `GraphQL::Execution::Lookahead` describing that field's sub-selections. A resolver can use it to skip work the query didn't ask for. [`graphsql`](../graphsql/README.md) builds on this to select only the requested SQL columns and to preload only the requested associations.

Lookahead honours `@include`/`@skip` and expands fragments, but it does not filter by a fragment's type condition. A selection that appears only under `... on OtherType` on a polymorphic field will look "selected" even if the runtime type doesn't match. It is an over-approximation, never an under-approximation.

### Batching (N+1)

`context["dataloader"]` coalesces repeated loads of the same kind among a list's items or a selection's sibling fields:

```ruby
context["dataloader"].with("author_by_id", BatchModule.batch).load(key)
```

Batching is local to a group of siblings (one list's items, or one selection set's fields), not global across the whole query tree. That covers the usual shape: N parents each resolving the same child.

## Introspection

Standard introspection queries (`__schema`, `__type`) are supported, so GraphiQL-style tooling works against a `Schema`.

## Validation

Queries are validated against the schema before execution. An invalid document returns a request-level `{"errors": [...]}`.
