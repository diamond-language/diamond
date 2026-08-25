#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-graphql-package` from the repo root, which sets this up already).
#
# Phase 1 (language: lexer/nodes/parser) + Phase 2 (type system/schema
# builder) -- see ROADMAP.md and this package's own plan for what's
# still to come (execution/validation/introspection). Covers parsing
# and schema-building, not execution -- nothing here runs a query
# against a schema and resolvers yet.
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

assert_contains() {
    local haystack="$1" needle="$2"
    [[ "$haystack" == *"$needle"* ]]
}

# Diamond string literals cap out at 255 bytes (DIAMOND_MAX_STRING_LENGTH,
# src/vm.h) -- any query text longer than that has to be assembled as an
# Array of lines joined with "\n", not a single literal. run_file below
# (source written to a temp .di file, not passed via -e) is what most
# cases here use for exactly this reason once a query gets past a
# one-liner.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/graphql\"
$script"
}

run_file() {
    local script="$1"
    local f="$work/case.di"
    {
        echo "require \"$(pwd)/lib/graphql\""
        echo "$script"
    } >"$f"
    "$diamond" "$f"
}

# --- simple query, shorthand (anonymous) form ---
actual="$(run_case '
doc = GraphQL::Language::Parser.parse("{ hello }")
op = doc.definitions()[0]
puts(op.operation())
puts(op.name())
puts(op.selection_set()[0].name())
')"
assert_contains "$actual" "query"
assert_contains "$actual" "hello"
count=$((count + 1))

# --- named query with a variable (default value + [String!]! type), aliases, nested selections ---
actual="$(run_file '
lines = [
  "query GetUsers($ids: [String!]! = [\"a\", \"b\"]) {",
  "  first: user(id: $ids) {",
  "    name",
  "    address { city }",
  "  }",
  "}"
]
doc = GraphQL::Language::Parser.parse(lines.join("\n"))
op = doc.definitions()[0]
puts(op.name())
vardef = op.variable_definitions()[0]
puts(vardef.name())
puts(vardef.type())
inner = vardef.type().of_type().of_type().of_type()
puts(inner.name())
default_list = vardef.default_value()
puts(default_list.values()[0])
puts(default_list.values()[1])
field = op.selection_set()[0]
puts(field.alias_name())
puts(field.name())
arg_value = field.arguments()[0].value()
puts(arg_value)
puts(arg_value.name())
nested = field.selection_set()[1]
puts(nested.name())
puts(nested.selection_set()[0].name())
')"
assert_contains "$actual" "GetUsers"
assert_contains "$actual" "ids"
assert_contains "$actual" "NonNullType"
assert_contains "$actual" "String"
assert_contains "$actual" $'a\nb'
assert_contains "$actual" "first"
assert_contains "$actual" "user"
assert_contains "$actual" "Variable"
assert_contains "$actual" "ids"
assert_contains "$actual" "address"
assert_contains "$actual" "city"
count=$((count + 1))

# --- mutation ---
actual="$(run_case '
doc = GraphQL::Language::Parser.parse("mutation CreateThing { create(input: {name: \"x\"}) { id } }")
puts(doc.definitions()[0].operation())
')"
assert_contains "$actual" "mutation"
count=$((count + 1))

# --- fragment definition + spread, typed and untyped inline fragments, directives with arguments ---
actual="$(run_file '
lines = [
  "query Q {",
  "  a: hero(episode: EMPIRE) @include(if: true) {",
  "    name",
  "    ...HeroFields",
  "    ... on Droid { primaryFunction }",
  "    ... @skip(if: false) { untyped }",
  "  }",
  "}",
  "fragment HeroFields on Character {",
  "  friends(filter: {names: [\"Luke\", \"Leia\"], limit: 5})",
  "}"
]
doc = GraphQL::Language::Parser.parse(lines.join("\n"))
op = doc.definitions()[0]
hero = op.selection_set()[0]
puts(hero.alias_name())
puts(hero.name())
directive = hero.directives()[0]
puts(directive.name())
puts(directive.arguments()[0].value())
sels = hero.selection_set()
puts(sels.length())
puts(sels[1].name())
puts(sels[2].type_condition())
puts(sels[3].type_condition())
frag = doc.definitions()[1]
puts(frag.name())
puts(frag.type_condition())
friends = frag.selection_set()[0]
obj = friends.arguments()[0].value()
puts(obj.fields()[0].name())
list = obj.fields()[0].value()
puts(list.values()[0])
puts(list.values()[1])
puts(obj.fields()[1].value())
')"
assert_contains "$actual" "a"
assert_contains "$actual" "hero"
assert_contains "$actual" "include"
assert_contains "$actual" "true"
assert_contains "$actual" "4"
assert_contains "$actual" "HeroFields"
assert_contains "$actual" "Droid"
assert_contains "$actual" $'nil\nHeroFields\nCharacter'
assert_contains "$actual" "names"
assert_contains "$actual" $'Luke\nLeia'
assert_contains "$actual" "5"
count=$((count + 1))

# --- block string argument value, with indentation stripped ---
actual="$(run_file '
lines = [
  "query Q {",
  "  note(text: \"\"\"",
  "    hello",
  "      world",
  "    \"\"\")",
  "}"
]
doc = GraphQL::Language::Parser.parse(lines.join("\n"))
field = doc.definitions()[0].selection_set()[0]
puts(field.arguments()[0].value())
')"
assert_contains "$actual" $'hello\n  world'
count=$((count + 1))

# --- null literal, enum literal, boolean literals as argument values ---
actual="$(run_case '
doc = GraphQL::Language::Parser.parse("{ f(a: null, b: RED, c: true, d: false) }")
args = doc.definitions()[0].selection_set()[0].arguments()
puts(args[0].value())
puts(args[1].value())
puts(args[1].value().name())
puts(args[2].value())
puts(args[3].value())
')"
assert_contains "$actual" "NullValue"
assert_contains "$actual" "EnumValue"
assert_contains "$actual" "RED"
assert_contains "$actual" $'true\nfalse'
count=$((count + 1))

# --- int/float literals, including exponent and negative forms ---
actual="$(run_case '
doc = GraphQL::Language::Parser.parse("{ f(a: 42, b: -3, c: 3.14, d: -2.5e-3) }")
args = doc.definitions()[0].selection_set()[0].arguments()
puts(args[0].value())
puts(args[1].value())
puts(args[2].value())
puts(args[3].value())
')"
assert_contains "$actual" $'42\n-3\n3.14'
count=$((count + 1))

# --- a deliberate syntax error surfaces a sensible message ---
actual="$(run_case '
begin
  GraphQL::Language::Parser.parse("query { a(")
rescue e: GraphQL::Language::ParseError
  puts(e.message())
end
')"
assert_contains "$actual" "expected"
count=$((count + 1))

# --- scalar type shapes + wrapping ---
actual="$(run_case '
s = GraphQL::ScalarType.string()
puts(s.name())
puts(s.kind())
puts(s.non_null().name())
puts(s.list().name())
puts(s.list().non_null().name())
puts(s.non_null().list().non_null().name())
')"
assert_contains "$actual" "String"
assert_contains "$actual" "SCALAR"
assert_contains "$actual" "String!"
assert_contains "$actual" "[String]"
assert_contains "$actual" "[String]!"
assert_contains "$actual" "[String!]!"
count=$((count + 1))

# --- object type with several fields, argument on a field ---
actual="$(run_case '
def noop(object, args, context) = nil
t = GraphQL::ObjectType.new("Author")
t.field("id", GraphQL::ScalarType.id().non_null(), noop)
t.field("name", GraphQL::ScalarType.string().non_null(), noop, [GraphQL::Argument.new("locale", GraphQL::ScalarType.string(), "en", true)])
puts(t.name())
puts(t.kind())
puts(t.fields().length())
puts(t.field_named("name").arguments()[0].name())
puts(t.field_named("name").arguments()[0].has_default?())
puts(t.field_named("name").arguments()[0].default_value())
puts(t.field_named("missing") == nil)
')"
assert_contains "$actual" "Author"
assert_contains "$actual" "OBJECT"
assert_contains "$actual" "2"
assert_contains "$actual" "locale"
assert_contains "$actual" "en"
count=$((count + 1))

# --- enum type ---
actual="$(run_case '
e = GraphQL::EnumType.new("Status")
e.value("ACTIVE")
e.value("INACTIVE", "not active")
puts(e.kind())
puts(e.values().length())
puts(e.value_named?("ACTIVE"))
puts(e.value_named?("NOPE"))
')"
assert_contains "$actual" "ENUM"
assert_contains "$actual" "true"
assert_contains "$actual" "false"
count=$((count + 1))

# --- interface + implementing object type ---
actual="$(run_case '
node = GraphQL::InterfaceType.new("Node")
node.field("id", GraphQL::ScalarType.id().non_null())
t = GraphQL::ObjectType.new("Book")
t.field("id", GraphQL::ScalarType.id().non_null(), nil)
t.implements(node)
puts(node.kind())
puts(t.interfaces()[0].name())
')"
assert_contains "$actual" "INTERFACE"
assert_contains "$actual" "Node"
count=$((count + 1))

# --- union type ---
actual="$(run_case '
def noop(object, args, context) = nil
a = GraphQL::ObjectType.new("Author")
b = GraphQL::ObjectType.new("Book")
u = GraphQL::UnionType.new("SearchResult")
u.possible_type(a)
u.possible_type(b)
puts(u.kind())
puts(u.includes?("Author"))
puts(u.includes?("Nope"))
')"
assert_contains "$actual" "UNION"
assert_contains "$actual" "true"
assert_contains "$actual" "false"
count=$((count + 1))

# --- input object type ---
actual="$(run_case '
i = GraphQL::InputObjectType.new("AuthorFilter")
i.argument("country", GraphQL::ScalarType.string(), nil, false, "ISO country code")
puts(i.kind())
puts(i.argument_named("country").description())
')"
assert_contains "$actual" "INPUT_OBJECT"
assert_contains "$actual" "ISO country code"
count=$((count + 1))

# --- schema type_map walk, cyclic graph terminates ---
actual="$(run_case '
def noop(object, args, context) = nil
author = GraphQL::ObjectType.new("Author")
book = GraphQL::ObjectType.new("Book")
author.field("id", GraphQL::ScalarType.id().non_null(), noop)
author.field("books", GraphQL::ListType.of(book), noop)
book.field("id", GraphQL::ScalarType.id().non_null(), noop)
book.field("author", author, noop)
query = GraphQL::ObjectType.new("Query")
query.field("author", author, noop, [GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())])
schema = GraphQL::Schema.new()
schema.query(query)
map = schema.type_map()
puts(map.length())
puts(map["Author"].name())
puts(map["Book"].name())
puts(map["Query"].name())
puts(map["ID"].name())
')"
assert_contains "$actual" "4"
assert_contains "$actual" "Author"
assert_contains "$actual" "Book"
assert_contains "$actual" "Query"
assert_contains "$actual" "ID"
count=$((count + 1))

# --- end-to-end: variables, arguments, nested object + list, aliases ---
actual="$(run_file '
module R
  module_function
  def author_id(o, a, c) = o["id"]
  def author_name(o, a, c) = o["name"]
  def author_books(o, a, c) = o["books"]
  def book_title(o, a, c) = o["title"]
  def query_author(o, a, c) = c["db"][a["id"]]
end
book_type = GraphQL::ObjectType.new("Book")
book_type.field("title", GraphQL::ScalarType.string().non_null(), R.book_title)
author_type = GraphQL::ObjectType.new("Author")
author_type.field("id", GraphQL::ScalarType.id().non_null(), R.author_id)
author_type.field("name", GraphQL::ScalarType.string().non_null(), R.author_name)
author_type.field("books", GraphQL::ListType.of(book_type), R.author_books)
query_type = GraphQL::ObjectType.new("Query")
query_type.field("author", author_type, R.query_author, [GraphQL::Argument.new("id", GraphQL::ScalarType.id().non_null())])
schema = GraphQL::Schema.new()
schema.query(query_type)
db = {"1": {"id": "1", "name": "Ada Lovelace", "books": [{"title": "Notes"}]}}
lines = [
  "query Q($id: ID!) {",
  "  a: author(id: $id) { name books { title } }",
  "}"
]
puts(schema.execute(lines.join("\n"), {"id": "1"}, {"db": db}))
')"
assert_contains "$actual" "Ada Lovelace"
assert_contains "$actual" "Notes"
count=$((count + 1))

# --- null propagation: non-null field errors and nulls the response;
# nullable field just nulls that one key, siblings unaffected ---
actual="$(run_case '
module R
  module_function
  def bad(o, a, c) = nil
  def ok(o, a, c) = "fine"
end
t = GraphQL::ObjectType.new("Query")
t.field("required", GraphQL::ScalarType.string().non_null(), R.bad)
t.field("optional", GraphQL::ScalarType.string(), R.bad)
t.field("sibling", GraphQL::ScalarType.string(), R.ok)
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ required sibling }"))
puts(schema.execute("{ optional sibling }"))
')"
assert_contains "$actual" $'{data: nil, errors: [{message: cannot return null for a non-null field, path: [required]}]}\n{data: {optional: nil, sibling: fine}}'
count=$((count + 1))

# --- a resolver-raised GraphQL::ExecutionError surfaces its own
# message as a field error; a missing required argument is a
# request-level GraphQL::RequestError instead ---
actual="$(run_file '
module R
  module_function
  def boom(o, a, c)
    raise GraphQL::ExecutionError.new("custom failure")
  end
  def echo(o, a, c) = a["x"]
end
t = GraphQL::ObjectType.new("Query")
t.field("boom", GraphQL::ScalarType.string(), R.boom)
t.field("echo", GraphQL::ScalarType.string().non_null(), R.echo, [GraphQL::Argument.new("x", GraphQL::ScalarType.string().non_null())])
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ boom }"))
puts(schema.execute("{ echo }"))
')"
assert_contains "$actual" "custom failure"
assert_contains "$actual" 'missing required argument "x"'
count=$((count + 1))

# --- union type: __typename + resolve_type + typed inline fragments ---
actual="$(run_file '
module R
  module_function
  def name(o, a, c) = o["name"]
  def title(o, a, c) = o["title"]
  def resolve_search_type(o, c)
    if o.keys().include?("title") then "Book" else "Author" end
  end
  def search(o, a, c) = [{"name": "Ada"}, {"title": "Notes"}]
end
author_type = GraphQL::ObjectType.new("Author")
author_type.field("name", GraphQL::ScalarType.string(), R.name)
book_type = GraphQL::ObjectType.new("Book")
book_type.field("title", GraphQL::ScalarType.string(), R.title)
result_type = GraphQL::UnionType.new("SearchResult")
result_type.possible_type(author_type)
result_type.possible_type(book_type)
result_type.resolve_type(R.resolve_search_type)
t = GraphQL::ObjectType.new("Query")
t.field("search", result_type.list(), R.search)
schema = GraphQL::Schema.new()
schema.query(t)
lines = [
  "{ search { __typename ... on Author { name } ... on Book { title } } }"
]
puts(schema.execute(lines.join("\n")))
')"
assert_contains "$actual" "Author"
assert_contains "$actual" "Ada"
assert_contains "$actual" "Book"
assert_contains "$actual" "Notes"
count=$((count + 1))

# --- interface type: an object reachable only via implementing an
# interface must still show up in Schema#type_map's own reachability
# walk (a real bug this exact case caught) ---
actual="$(run_file '
module R
  module_function
  def id(o, a, c) = o["id"]
  def resolve_node_type(o, c) = "Thing"
  def node(o, a, c) = {"id": "1"}
end
node_iface = GraphQL::InterfaceType.new("Node")
node_iface.field("id", GraphQL::ScalarType.id().non_null())
node_iface.resolve_type(R.resolve_node_type)
thing_type = GraphQL::ObjectType.new("Thing")
thing_type.field("id", GraphQL::ScalarType.id().non_null(), R.id)
thing_type.implements(node_iface)
t = GraphQL::ObjectType.new("Query")
t.field("node", node_iface, R.node)
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.type_map().keys().include?("Thing"))
puts(schema.execute("{ node { __typename id } }"))
')"
assert_contains "$actual" $'true\n{data: {node: {__typename: Thing, id: 1}}}'
count=$((count + 1))

# --- enum type: a resolver returning an unknown value is a field
# error, not a silent pass-through ---
actual="$(run_case '
module R
  module_function
  def status(o, a, c) = "ACTIVE"
  def bogus(o, a, c) = "NOPE"
end
status_type = GraphQL::EnumType.new("Status")
status_type.value("ACTIVE")
status_type.value("INACTIVE")
t = GraphQL::ObjectType.new("Query")
t.field("status", status_type, R.status)
t.field("bogus", status_type, R.bogus)
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ status }"))
puts(schema.execute("{ bogus }"))
')"
assert_contains "$actual" "ACTIVE"
assert_contains "$actual" 'is not a valid value for enum "Status"'
count=$((count + 1))

# --- named fragment spread + @include/@skip directives (with a
# variable-driven @skip) ---
actual="$(run_file '
module R
  module_function
  def id(o, a, c) = o["id"]
  def name(o, a, c) = o["name"]
  def query_author(o, a, c) = {"id": "1", "name": "Ada"}
end
author_type = GraphQL::ObjectType.new("Author")
author_type.field("id", GraphQL::ScalarType.id().non_null(), R.id)
author_type.field("name", GraphQL::ScalarType.string(), R.name)
t = GraphQL::ObjectType.new("Query")
t.field("author", author_type, R.query_author)
schema = GraphQL::Schema.new()
schema.query(t)
lines = [
  "query($skipName: Boolean!) {",
  "  a: author { ...Fields }",
  "  b: author { id name @skip(if: $skipName) }",
  "}",
  "fragment Fields on Author { id name @include(if: false) }"
]
puts(schema.execute(lines.join("\n"), {"skipName": true}))
')"
assert_contains "$actual" '{data: {a: {id: 1}, b: {id: 1}}}'
count=$((count + 1))

# --- an unknown field is a request-level validation error (rejected
# before any resolver runs, no "data" key at all), not a crash ---
actual="$(run_case '
t = GraphQL::ObjectType.new("Query")
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ nope }"))
')"
assert_contains "$actual" 'field "nope" does not exist on type "Query"'
count=$((count + 1))

# --- validation: leaf-vs-composite selection-set mismatch, unknown
# argument, undefined variable, and a variable/argument type
# incompatibility -- all rejected before any resolver runs ---
actual="$(run_file '
def noop(o, a, c) = "x"
t = GraphQL::ObjectType.new("Query")
t.field("name", GraphQL::ScalarType.string(), noop)
t.field("author", t, noop)
t.field("greet", GraphQL::ScalarType.string(), noop, [GraphQL::Argument.new("who", GraphQL::ScalarType.string().non_null())])
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ name { x } }"))
puts(schema.execute("{ author }"))
puts(schema.execute("{ greet(who: \"x\", bogus: 1) }"))
puts(schema.execute("{ greet(who: $x) }"))
puts(schema.execute("query($x: Int!) { greet(who: $x) }", {"x": 5}))
')"
assert_contains "$actual" 'field "name" is a leaf type and cannot have a sub-selection'
assert_contains "$actual" 'field "author" of composite type "Query" must have a sub-selection'
assert_contains "$actual" 'unknown argument "bogus" on "greet"'
assert_contains "$actual" 'undefined variable "$x"'
assert_contains "$actual" 'variable "$x" of type "Int!" is not compatible with expected type "String!"'
count=$((count + 1))

# --- validation: duplicate operation names, and more than one
# operation when one of them is anonymous ---
actual="$(run_case '
def noop(o, a, c) = "x"
t = GraphQL::ObjectType.new("Query")
t.field("name", GraphQL::ScalarType.string(), noop)
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("query A { name } query A { name }", {}, {}, nil, "A"))
puts(schema.execute("{ name } query A { name }"))
')"
assert_contains "$actual" 'duplicate operation name "A"'
assert_contains "$actual" "must have only one operation when it includes an anonymous operation"
count=$((count + 1))

# --- introspection: __schema (queryType, the full reachable types
# list) and __type(name:), including nested field/type-wrapper
# traversal (NON_NULL/ofType) ---
actual="$(run_file '
def noop(o, a, c) = "x"
author_type = GraphQL::ObjectType.new("Author")
author_type.field("id", GraphQL::ScalarType.id().non_null(), noop)
author_type.field("name", GraphQL::ScalarType.string(), noop)
t = GraphQL::ObjectType.new("Query")
t.field("author", author_type, noop)
schema = GraphQL::Schema.new()
schema.query(t)
lines = [
  "{",
  "  __schema { queryType { name } types { name kind } }",
  "  __type(name: \"Author\") { name kind fields { name type { kind ofType { name } } } }",
  "}"
]
puts(schema.execute(lines.join("\n")))
')"
assert_contains "$actual" "queryType: {name: Query}"
assert_contains "$actual" "{name: Author, kind: OBJECT}"
assert_contains "$actual" "{kind: NON_NULL, ofType: {name: ID}}"
count=$((count + 1))

# --- introspection: enumValues (with description), possibleTypes via
# an interface implementor, interfaces, and a default argument value
# printed back as a GraphQL literal String; an unknown type name is
# just nil, not an error ---
actual="$(run_file '
def noop(o, a, c) = "x"
status_type = GraphQL::EnumType.new("Status")
status_type.value("ACTIVE", "is active")
node_iface = GraphQL::InterfaceType.new("Node")
node_iface.field("id", GraphQL::ScalarType.id().non_null())
thing_type = GraphQL::ObjectType.new("Thing")
thing_type.field("id", GraphQL::ScalarType.id().non_null(), noop)
thing_type.field("status", status_type, noop, [GraphQL::Argument.new("locale", GraphQL::ScalarType.string(), "en", true)])
thing_type.implements(node_iface)
t = GraphQL::ObjectType.new("Query")
t.field("thing", thing_type, noop)
schema = GraphQL::Schema.new()
schema.query(t)
lines = [
  "{",
  "  a: __type(name: \"Status\") { enumValues { name description } }",
  "  b: __type(name: \"Node\") { possibleTypes { name } }",
  "  c: __type(name: \"Thing\") { interfaces { name } fields { name args { defaultValue } } }",
  "  d: __type(name: \"Nope\") { name }",
  "}"
]
puts(schema.execute(lines.join("\n")))
')"
assert_contains "$actual" "{name: ACTIVE, description: is active}"
assert_contains "$actual" "possibleTypes: [{name: Thing}]"
assert_contains "$actual" "interfaces: [{name: Node}]"
assert_contains "$actual" 'defaultValue: "en"'
assert_contains "$actual" "d: nil"
count=$((count + 1))

# --- lookahead: a parent resolver can peek at whether a not-yet-
# resolved child field will itself select a given sub-field (the
# eager-load-avoidance use case), via context["lookahead"] ---
actual="$(run_file '
module AuthorResolvers
  module_function
  def name(o, a, c) = o["name"]
  def books(o, a, c)
    if c["lookahead"].selects?("title")
      c["log"].push("eager-loading titles")
    else
      c["log"].push("skipping title load")
    end
    o["books"]
  end
end
module BookResolvers
  module_function
  def title(o, a, c) = o["title"]
end
module QueryResolvers
  module_function
  def author(o, a, c)
    la = c["lookahead"]
    c["log"].push(la.selections())
    c["log"].push(la.selection("books").selects?("title"))
    o
  end
end
book_type = GraphQL::ObjectType.new("Book")
book_type.field("title", GraphQL::ScalarType.string().non_null(), BookResolvers.title)
author_type = GraphQL::ObjectType.new("Author")
author_type.field("name", GraphQL::ScalarType.string(), AuthorResolvers.name)
author_type.field("books", GraphQL::ListType.of(book_type), AuthorResolvers.books)
t = GraphQL::ObjectType.new("Query")
t.field("author", author_type, QueryResolvers.author)
schema = GraphQL::Schema.new()
schema.query(t)
data = {"name": "Ada", "books": [{"title": "Notes"}]}
log = []
puts(schema.execute("{ author { name books { title } } }", {}, {"log": log}, data))
puts(log)
log2 = []
puts(schema.execute("{ author { name } }", {}, {"log": log2}, data))
puts(log2)
')"
assert_contains "$actual" "{data: {author: {name: Ada, books: [{title: Notes}]}}}"
assert_contains "$actual" "[[name, books], true, eager-loading titles]"
assert_contains "$actual" "{data: {author: {name: Ada}}}"
assert_contains "$actual" "[[name], false]"
count=$((count + 1))

# --- dataloader: the core proof -- 3 authors each independently
# resolving books via context["dataloader"].with(name, batch).load(id)
# coalesce into exactly ONE batch dispatch, not 3 ---
actual="$(run_file '
module BookLoader
  module_function
  def batch(author_ids, context)
    counter = context["batch_calls"]
    counter[0] = counter[0] + 1
    db = context["db"]
    result = {}
    index = 0
    while index < author_ids.length()
      aid = author_ids[index]
      result[aid] = db[aid]
      index += 1
    end
    result
  end
end
module BookResolvers
  module_function
  def title(o, a, c) = o["title"]
end
module AuthorResolvers
  module_function
  def name(o, a, c) = o["name"]
  def books(o, a, c)
    loader = c["dataloader"].with("books_by_author", BookLoader.batch)
    loader.load(o["id"])
  end
end
module QueryResolvers
  module_function
  def authors(o, a, c) = c["all_authors"]
end
book_type = GraphQL::ObjectType.new("Book")
book_type.field("title", GraphQL::ScalarType.string().non_null(), BookResolvers.title)
author_type = GraphQL::ObjectType.new("Author")
author_type.field("name", GraphQL::ScalarType.string(), AuthorResolvers.name)
author_type.field("books", GraphQL::ListType.of(book_type), AuthorResolvers.books)
t = GraphQL::ObjectType.new("Query")
t.field("authors", GraphQL::ListType.of(author_type), QueryResolvers.authors)
schema = GraphQL::Schema.new()
schema.query(t)
db = {"1": [{"title": "Book A"}], "2": [{"title": "Book B"}], "3": [{"title": "Book C"}]}
all_authors = [{"id": "1", "name": "Ada"}, {"id": "2", "name": "Bob"}, {"id": "3", "name": "Cid"}]
context = {"db": db, "all_authors": all_authors, "batch_calls": [0]}
result = schema.execute("{ authors { name books { title } } }", {}, context)
puts(result)
puts(context["batch_calls"])
')"
assert_contains "$actual" "{name: Ada, books: [{title: Book A}]}"
assert_contains "$actual" "{name: Bob, books: [{title: Book B}]}"
assert_contains "$actual" "{name: Cid, books: [{title: Book C}]}"
assert_contains "$actual" "[1]"
count=$((count + 1))

# --- dataloader: a batch function that raises surfaces as a normal
# per-field error for every item waiting on it -- no hang, no crash ---
actual="$(run_case '
module BadLoader
  module_function
  def batch(ids, context)
    raise GraphQL::ExecutionError.new("db unavailable")
  end
end
module AuthorResolvers
  module_function
  def books(o, a, c)
    loader = c["dataloader"].with("bad", BadLoader.batch)
    loader.load(o["id"])
  end
end
module QueryResolvers
  module_function
  def authors(o, a, c) = c["all_authors"]
end
def noop(o, a, c) = "x"
book_type = GraphQL::ObjectType.new("Book")
book_type.field("title", GraphQL::ScalarType.string(), noop)
author_type = GraphQL::ObjectType.new("Author")
author_type.field("books", GraphQL::ListType.of(book_type), AuthorResolvers.books)
t = GraphQL::ObjectType.new("Query")
t.field("authors", GraphQL::ListType.of(author_type), QueryResolvers.authors)
schema = GraphQL::Schema.new()
schema.query(t)
context = {"all_authors": [{"id": "1"}, {"id": "2"}]}
puts(schema.execute("{ authors { books { title } } }", {}, context))
')"
assert_contains "$actual" "{data: {authors: [{books: nil}, {books: nil}]}"
assert_contains "$actual" "{message: db unavailable, path: [authors, 0, books]}"
assert_contains "$actual" "{message: db unavailable, path: [authors, 1, books]}"
count=$((count + 1))

# --- dataloader: null propagation through a fiber-driven list item
# still works correctly (a NON_NULL item field failing nulls the whole
# list, exactly one error recorded) ---
actual="$(run_case '
def bad(o, a, c) = nil
def ok(o, a, c) = "fine"
def items_resolver(o, a, c) = [{}, {}]
item_type = GraphQL::ObjectType.new("Item")
item_type.field("value", GraphQL::ScalarType.string().non_null(), bad)
t = GraphQL::ObjectType.new("Query")
t.field("items", GraphQL::ListType.of(item_type.non_null()), items_resolver)
t.field("sibling", GraphQL::ScalarType.string(), ok)
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ items { value } sibling }"))
')"
assert_contains "$actual" "{data: {items: nil, sibling: fine}, errors: [{message: cannot return null for a non-null field, path: [items, 0, value]}]}"
count=$((count + 1))

echo "$count graphql tests passed"
