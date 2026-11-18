# graphql

Define GraphQL schemas and execute queries in d6.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/d6/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no Cutfile yet
facet add graphql
facet install
```

This installs the cut into `cuts/graphql/`; load it with `require_cut "graphql"`.

## Usage

Types are built from plain values, resolvers are callables, and `schema.execute` takes the
query text, variables, and a context Hash. This program serves a schema at `POST /graphql`
using `gremlin`; the "database" is an in-memory Hash so it runs as is:

```ruby
# app.d6
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
diamond app.d6
curl -d '{"query":"query($id: ID!) { author(id: $id) { name books { title } } }","variables":{"id":"1"}}' \
  localhost:8080/graphql
# {"data":{"author":{"name":"Ada Lovelace","books":[{"title":"Notes"},{"title":"Sketch"}]}}}
```

The schema is built once per server worker and kept in the worker's `context`, because each
`gremlin_serve` worker thread has its own VM. Replace `AUTHORS` with your database connection in
the context Hash; every resolver receives that Hash as its third argument. To resolve straight
from SQL tables, see [`graphsql`](https://github.com/diamond-language/d6/tree/main/packages/graphsql).

## Defining a schema

Types are plain values built with chained calls; there are no class-body macros.

### Scalars and wrappers

`GraphQL::ScalarType.string()`, `.int()`, `.float()`, `.boolean()`, `.id()`. Each call returns a new instance.

The built-in scalars are strict. On input, a String takes a String, an Int an Int, a Float an Int or Float, a Boolean true or false and an ID a String or Int; anything else (a list or object where a number is expected, say) is a request error. On output, a resolver may return a String, Int, Float, Boolean or Symbol; a list, a Hash or a model object for a scalar field is an error on that field rather than a serialised object.

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

<!-- doc-check: skip -->
```ruby
result = schema.execute(query_string, variables = {}, context = {}, root_value = nil, operation_name = nil)
```

The result is a Hash:

- `{"data": ...}` on success.
- `{"data": ..., "errors": [...]}` when one or more fields errored. Fields that did resolve are still returned.
- `{"errors": [...]}`, with no `"data"` key, for a request-level failure: an unparseable document, an unknown operation, or a variable or argument that doesn't coerce.

The `context` Hash is yours. Whatever you pass is handed to every resolver, which is the usual place for the database connection and the signed-in user. Two entries are set by the executor for each request: `context["lookahead"]` and `context["dataloader"]`.

### Query limits

Fragments let a small document expand into an enormous one: `fragment F0 on Query { a { ...F1 } b { ...F1 } }` chained 40 deep selects 2^40 fields. The parser caps how deeply a document nests syntactically, but not what its fragments expand to. Before a document is validated or executed, the schema measures it with its fragments expanded (in time proportional to the document, not the expansion) and rejects it when the deepest field nesting exceeds `schema.max_query_depth()` (default 20) or it selects more than `schema.max_query_nodes()` fields (default 10,000). Set them with `schema.max_query_depth(30)` and `schema.max_query_nodes(50000)`.

### Errors

Raise `GraphQL::ExecutionError.new("message")` from a resolver to return a specific, client-visible message for that field. Any other `StandardError` is reported as a generic internal error, so an implementation detail doesn't leak to the client. The real message is written to stderr; `schema.mask_errors(false)` sends it to the client instead, for development.

```ruby
def sign_in(object, args, context)
  raise GraphQL::ExecutionError.new("invalid email or password") unless ok
  # ...
end
```

Pass a Hash as the second argument to attach `extensions` to the error, the spec's place for machine-readable detail such as a code:

```ruby
raise GraphQL::ExecutionError.new("invalid email or password", {"code": "UNAUTHENTICATED"})
# => {"message": "invalid email or password", "path": ["signIn"], "extensions": {"code": "UNAUTHENTICATED"}}
```

An error without extensions has no `"extensions"` key.

`GraphQL::Fail` is shorthand for errors with a code, so a client can branch on the code and never
on the message text:

```ruby
GraphQL::Fail.signal("NOT_FOUND", "no such board")   # raises the ExecutionError above, with that code
GraphQL::Fail.code(error)                            # its code, or "INTERNAL"
GraphQL::Fail.annotate(schema.execute(query))        # gives every error a code
```

`annotate` leaves an error that already has extensions alone. Any other field error gets
`"INTERNAL"`, and an error that rejected the whole request (no `"data"` in the result) gets
`"BAD_REQUEST"`.

### Lookahead

Immediately before each field resolves, the executor sets `context["lookahead"]` to a `GraphQL::Execution::Lookahead` describing that field's sub-selections. A resolver can use it to skip work the query didn't ask for. [`graphsql`](https://github.com/diamond-language/d6/blob/main/packages/graphsql/README.md) builds on this to select only the requested SQL columns and to preload only the requested associations.

Lookahead honours `@include`/`@skip` and expands fragments, but it does not filter by a fragment's type condition. A selection that appears only under `... on OtherType` on a polymorphic field will look "selected" even if the runtime type doesn't match. It is an over-approximation, never an under-approximation.

### Batching (N+1)

`context["dataloader"]` coalesces repeated loads of the same kind among a list's items or a selection's sibling fields:

```ruby
context["dataloader"].with("author_by_id", BatchModule.batch).load(key)
```

Batching is local to a group of siblings (one list's items, or one selection set's fields), not global across the whole query tree. That covers the usual shape: N parents each resolving the same child.

## SDL

`schema.to_sdl()` returns the schema as GraphQL SDL text, for client codegen and review. Types are in name order,
the built-in scalars are omitted, and descriptions are printed as block strings. Resolvers and
`resolve_type` callables are not part of SDL. A server can serve it directly:

```ruby
[200, {"Content-Type": "text/plain; charset=utf-8"}, schema.to_sdl()]
```

`GraphQL::Schema.from_sdl(text)` goes the other way. It builds a schema from SDL: object, interface, union, enum,
input and scalar types, a `schema { query: ... mutation: ... }` block, descriptions, directive definitions, and
directives applied to field definitions:

```ruby
schema = GraphQL::Schema.from_sdl(File.read("schema.graphql"))
schema.query_type().field_named("books").directive("auth").arguments()   # {"as": "ANY"}
```

```graphql
directive @auth(as: Principal!) on FIELD_DEFINITION

enum Principal {
  ANY
  EDITOR
}

type Query {
  books(first: Int = 10): [Book!]! @auth(as: ANY)
}

type Book {
  id: ID!
  title: String!
}
```

The fields have no resolvers, so each resolves to the parent object's public method of the same name. A custom
`scalar` passes values through unchanged. An applied directive's `arguments()` include the definition's defaults.
Directives do nothing by themselves; code that reads `field.directives()` decides what they mean.

`from_sdl` raises `GraphQL::SchemaError` with the problem and its line, for example
`unknown type Bok in Query.books (line 9)`. It also reports what the type system can't represent:
- `extend` and subscriptions
- an interface implementing another interface
- a directive anywhere other than a field definition
- a type that no root type, field or directive leads to (it would vanish from introspection)

A schema printed by `to_sdl()` reads back into one that prints the same text.

## Apps: SDL plus resolvers bound by name

`GraphQL::App` builds a schema from SDL and binds resolvers to it by name. A resolvers object is
bound to an object type, and its method named after a field resolves that field. Only fields that
need work need a method:

```ruby
class QueryResolvers
  def board(_, args, ctx) = Boards.find(ctx.repo(), args["slug"])
end

class BoardResolvers
  def top_scores(board, args, ctx) = Scores.top(ctx.repo(), board, args["limit"])
end

schema = GraphQL::App.new(File.read("schema.graphql"))
  .resolvers("Query", QueryResolvers.new())
  .resolvers("Board", BoardResolvers.new())
  .int_booleans()
  .build()
```

A field gets its value from the first of these that exists:

1. the bound resolvers' method with the field's snake_case name (`topScores` -> `top_scores`),
   called as `(object, args, context)`
2. the parent object's zero-argument method of that name, such as a `struct` field
3. a key on a parent Hash, snake_case first and then the field's own name

`Query` and `Mutation` fields, and any field that takes arguments, need a resolver method. A value
for an `ID` that is an Int becomes a String. With `int_booleans`, a `Boolean` field also turns 0 and
1 into `false` and `true`, for SQLite rows.

An interface or union finds a value's concrete type from its class name (the last `::` segment), or
from a Hash's `"__typename"` key. `.resolve_type("Result", callable)` overrides that: `callable` gets
`(value, context)` and returns a type name.

`build()` returns the `Schema`. If anything is missing, it raises one `SchemaError` that lists every
problem:

```text
the GraphQL app is not ready:
- resolvers are bound to Nope, which the schema does not define
- Board.topScores takes arguments, so it needs top_scores(object, args, context) on BoardResolvers
- Query.board has no resolver: define board(object, args, context) on QueryResolvers
```

The executor stores `"dataloader"` and `"lookahead"` in the context. A Hash context works as is. To
pass your own object, `include GraphQL::Context` in its class:

```ruby
class RequestContext
  include GraphQL::Context
  def initialize(repo)
    @repo = repo
  end
  def repo() = @repo
end

schema.execute(query, variables, RequestContext.new(repo))
```

### Directives as middleware

A directive applied to fields in SDL does whatever function `.directive(name, function)`
registers for it. The function wraps each such field's resolver and is called as
`(directive_args, object, args, context, resolve)`:
- `directive_args` holds the directive's arguments, with the defaults filled in.
- `resolve(object, args, context)` runs the field.

A guard raises to deny, or returns `resolve(...)`:

```graphql
directive @auth(as: Role = USER) on FIELD_DEFINITION

type Query {
  me: Account @auth
  auditLog: [Entry!]! @auth(as: ADMIN)
}
```

```ruby
def auth(directive, object, args, context, resolve)
  user = context["user"]
  if user == nil then GraphQL::Fail.signal("UNAUTHENTICATED", "sign in first") end
  if directive["as"] == "ADMIN" && !user.admin?() then GraphQL::Fail.signal("FORBIDDEN", "admins only") end
  resolve(object, args, context)
end

schema = GraphQL::App.new(File.read("schema.graphql"))
  .resolvers("Query", QueryResolvers.new())
  .directive("auth", auth)
  .build()
```

A function can also change the result: `def upper(d, o, a, c, resolve) = resolve(o, a, c).upcase()`.
On a field with several directives, the first one written runs outermost. Directives wrap plain
fields too, not only fields with resolver methods. `build()` reports any directive used on a field
with no function registered, and any function registered for a directive the schema doesn't
define. Directives on interface fields don't run, because interface fields are never resolved
directly; put them on the implementing types' fields.

## Introspection

Standard introspection queries (`__schema`, `__type`) are supported, so GraphiQL-style tooling works against a `Schema`.
Fragments may name the meta types (`fragment F on __Type { name kind }`, `... on __Type { ... }`), as introspection tools write them.

## Validation

Queries are validated against the schema before execution. An invalid document returns a request-level `{"errors": [...]}`.
A fragment that spreads itself, directly or through other fragments, is rejected (the check is iterative, so a long chain of
fragments cannot exhaust the stack).

The parser refuses a document nested more than `GraphQL::Language::Parser.max_nesting()` (20) levels deep, counting selection sets,
list and object literals and list types, with a parse error. Parsing, validation and execution recurse once per level and a call
stack is only about 90 frames deep (a request handler runs on a fiber), so an unbounded document would otherwise crash the server.
