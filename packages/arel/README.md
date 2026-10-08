# arel

Build SQL queries from an immutable expression tree with dialect-specific rendering.

## Installation

From your project directory (see the [package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md) for `facet`):

```sh
facet init myapp          # once, if the project has no diamond.cut yet
facet add arel --registry https://cuts.dilang.tech --version "^0.34.4"
facet update
```

This installs the cut into `cuts/arel/`; load it with `require_cut "arel"`.

## Usage

```ruby
require_cut "arel"

people = Arel.table("people")
query = Arel.from(people).project([
  people.column("name"),
  people.column("age")
])
active = people.column("active").eq(true)
adult = people.column("age").gteq(18)
query = query.where(active.and_also(adult))
query = query.order(people.column("name").asc()).take(20).skip(5)

sql, params = query.to_sql()
# SELECT "people"."name", "people"."age" FROM "people"
# WHERE ("people"."active" = ? AND "people"."age" >= ?)
# ORDER BY "people"."name" ASC LIMIT ? OFFSET ?
# params: [true, 18, 20, 5]
```

## Running queries

Queries execute against an open connection. This program creates a table, inserts, selects,
updates, and deletes through Arel:

```ruby
require_cut "arel"

db = SQLite3.open(":memory:")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER, active INTEGER)")

people = Arel.table("people")
Arel.insert_into(people).values({"name": "Ada", "age": 36, "active": true}).execute(db)

adults = Arel.from(people).where(people.column("age").gteq(18)).order(people.column("name").asc())
adults.to_a(db)
# => [{"id": 1, "name": "Ada", "age": 36, "active": 1}]

Arel.update(people).set({"age": 37}).where(people.column("name").eq("Ada")).execute(db)
Arel.delete_from(people).where(people.column("age").lt(18)).execute(db)
```

Most applications put a repository on top of this; see [`active_record`](https://github.com/diamond-language/diamond/tree/main/packages/active_record).

## Notes

`to_sql(visitor)` returns SQL and ordered bind values. The default visitor targets SQLite; pass `PostgreSQLVisitor`, `MariaDBVisitor`, or `MySQLVisitor` for those dialects. See [VISITORS.md](https://github.com/diamond-language/diamond/blob/main/packages/arel/VISITORS.md) for the renderer protocol.
