# arel

Build SQL queries from an immutable expression tree with dialect-specific rendering.

## Installation

Install the cut at `cuts/arel/` and load it with `require_cut "arel"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

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

## Notes

`to_sql(visitor)` returns SQL and ordered bind values. The default visitor targets SQLite; pass `PostgreSQLVisitor`, `MariaDBVisitor`, or `MySQLVisitor` for those dialects. See [VISITORS.md](VISITORS.md) for the renderer protocol.
