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

echo "$count graphql tests passed"
