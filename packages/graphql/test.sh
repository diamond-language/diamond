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

# --- an unknown field is a field-level error, not a crash ---
actual="$(run_case '
t = GraphQL::ObjectType.new("Query")
schema = GraphQL::Schema.new()
schema.query(t)
puts(schema.execute("{ nope }"))
')"
assert_contains "$actual" 'field "nope" not found on type "Query"'
count=$((count + 1))

echo "$count graphql tests passed"
