# graphql

Define GraphQL schemas and execute queries in Diamond.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add graphql --registry https://cuts.dilang.tech --version "^0.1.3"
facet update
```

This installs the cut into `cuts/graphql/`; load it with `require_cut "graphql"`.

## Usage

Types are built from plain values, resolvers are callables, and `schema.execute` takes the
query text, variables, and a context Hash. This program serves a schema at `POST /graphql`
using `gremlin`; the "database" is an in-memory Hash so it runs as is:

```ruby
# app.di
require_cut "gremlin"
require_cut "graphql"

AUTHORS = {
  "1": {"id": "1", "name": "Ada Lovelace", "books": [{"title": "Notes"}, {"title": "Sketch"}]},
  "2": {"id": "2", "name": "Alan Turing", "books": []}
}

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
  def author(object, args, context) = context["db"]["#{args["id"]}"]
end

def build_schema()
  book = GraphQL::ObjectType.new("Book")
  book.field("title", GraphQL::ScalarType.string().non_null(), BookResolvers.title)

  author = GraphQL::ObjectType.new("Author")
  author.field("id", GraphQL::ScalarType.id().non_null(), AuthorResolvers.id)
  author.field("name", GraphQL::ScalarType.string().non_null(), AuthorResolvers.name)
  author.field("books", GraphQL::ListType.of(book), AuthorResolvers.books)

  query = GraphQL::ObjectType.new("Query")
  query.field("author", author, QueryResolvers.author,
    [GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())])

  schema = GraphQL::Schema.new()
  schema.query(query)
  schema
end

class Endpoint
  def self.handle(request, context)
    if request["method"] != "POST" || request["path"] != "/graphql"
      return [404, {"Content-Type": "text/plain"}, "not found"]
    end
    payload = JSON.parse(request["body"])
    schema = context["schema"]
    if schema == nil
      schema = build_schema()
      context["schema"] = schema
    end
    result = schema.execute(payload["query"], payload["variables"] || {}, {"db": AUTHORS})
    [200, {"Content-Type": "application/json"}, JSON.stringify(result)]
  end
end

gremlin_serve(8080, Endpoint.handle)
```

```sh
diamond app.di
curl -d '{"query":"query($id: ID!) { author(id: $id) { name books { title } } }","variables":{"id":"1"}}' \
  localhost:8080/graphql
# {"data":{"author":{"name":"Ada Lovelace","books":[{"title":"Notes"},{"title":"Sketch"}]}}}
```

The schema is built once per server worker and kept in the worker's `context`, because each
`gremlin_serve` worker thread has its own VM. Replace `AUTHORS` with your database connection in
the context Hash; every resolver receives that Hash as its third argument. To resolve straight
from SQL tables, see [`graphsql`](https://github.com/diamond-language/diamond/tree/main/packages/graphsql).

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

Variables follow the spec's argument-coercion rules. A variable the request declared but did not supply is treated as an **omitted** argument, so the argument's schema default applies (or, for a required argument, validation rejects the document). An explicit `null` is a *provided* value and overrides the default.

```ruby
# page(limit: Int = 20): both documents are valid
schema.execute("query($l: Int) { page(limit: $l) }", {})              # limit == 20
schema.execute("query($l: Int) { page(limit: $l) }", {"l": nil})      # limit == nil
```

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

Immediately before each field resolves, the executor sets `context["lookahead"]` to a `GraphQL::Execution::Lookahead` describing that field's sub-selections. A resolver can use it to skip work the query didn't ask for. [`graphsql`](https://github.com/diamond-language/diamond/blob/main/packages/graphsql/README.md) builds on this to select only the requested SQL columns and to preload only the requested associations.

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
