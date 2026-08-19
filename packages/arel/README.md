# packages/arel

See [ROADMAP.md](ROADMAP.md) for the forward-looking development plan.

A small, immutable SQL AST and chainable query builder for
[Diamond](https://gitlab.com/dmn9180/diamond) -- the first slice toward
a DataMapper-style persistence layer. Builds and renders `SELECT`
statements through a SQLite visitor; `INSERT`/`UPDATE`/`DELETE` belong to a mapper/
repository layer built on top of this, not here -- matches real
[Arel](https://github.com/rails/rails/tree/main/activerecord)'s own
historical scope in Rails.

The initial renderer is `ArelSQLiteVisitor`, but execution remains loosely
coupled: `#to_a`/`#count` only call `db.query(sql, params)`, the exact
method shape the SQLite3 driver exposes (see
[`docs/io.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/io.md)'s
"SQLite3" section). Any future adapter exposing the same
`#query(sql, params)` contract is a drop-in target, the same way
[`packages/rack`](../rack/README.md) stayed server-agnostic by
depending on a shared method convention rather than a concrete type.

## Install

Same story as the other packages here -- copy this directory into
another project as `diamond_packages/arel/`, or give it its own git
remote and depend on it via `facet` (see
[`docs/packages.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/packages.md)).

## Usage

The AST API quotes identifiers and binds values:

```ruby
require "/path/to/arel"

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

Diamond does not currently support user-defined `[]`, so attributes use
`table.column("name")` rather than Ruby Arel's `table[:name]`. Likewise,
`and_also`/`or_else`/`not_` avoid Diamond's reserved boolean keywords.

Supported attribute predicates are `eq`, `not_eq`, `lt`, `lteq`, `gt`, and
`gteq`. `eq(nil)` and `not_eq(nil)` render as `IS NULL` and `IS NOT NULL`.
Predicates compose with `and_also`, `or_else`, and `not_`; explicit grouping is
preserved in the rendered SQL. Attributes also provide `asc()` and `desc()`.

The original string-oriented API remains available for compatibility:

```ruby
require "/path/to/arel"

base = Arel.from("people").where({"active": true})
adults = base.where("age >= ?", [18])
minors = base.where("age < ?", [18])

sql, params = adults.to_sql()
# => "SELECT * FROM people WHERE active = ? AND age >= ?", [true, 18]

adults.to_a(db)    # runs it, returns the rows
adults.count(db)   # runs a COUNT(*) wrapping the same query
```

`ArelQuery` is immutable: every chain method returns a *new*
`ArelQuery` rather than mutating the receiver, so a base query is safe
to reuse as a starting point for several different queries -- `adults`/
`minors` above each see only their own added where-clause, not each
other's, and `base` itself is never touched by either.

### Compatibility `where`

Takes either a `Hash` (ANDed equality shorthand) or a raw SQL fragment
`String` paired with its own `params` `Array`:

```ruby
Arel.from("people").where({"active": true, "role": "admin"})
# => WHERE active = ? AND role = ?

Arel.from("people").where("age > ?", [21])
# => WHERE age > ?
```

Multiple legacy `.where()` calls -- and multiple keys within one `Hash` call --
all AND together. Use AST predicates for explicit `OR`/`NOT` composition.

### `project`, `order`, `take`, `skip`

The AST names are `project`, `order`, `take`, and `skip`; `select`, `limit`,
and `offset` remain aliases. A projection accepts one expression or an Array
of expressions because Diamond does not have variadic arguments.

The legacy API continues to accept raw Strings:

```ruby
Arel.from("people").select(["name", "age"]).order("age DESC").limit(10).offset(20)
```

`select` replaces the column list (default `["*"]`); `order` appends to
whatever ordering earlier `.order()` calls already contributed, and
takes either a single column `String` or an `Array` of them.

### `to_sql`, `to_a`, `count`

`to_sql()` returns `[sql, params]` -- the same positional-`Array`-return
shape used elsewhere in this codebase (parsed URLs, HTTP responses) --
and can be unpacked directly with
[multi-value destructuring](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/syntax.md#multiple-assignment):

```ruby
sql, params = query.to_sql()
```

`to_a(db)` calls `db.query(sql, params)` and returns the rows. `count(db)`
wraps the *entire* rendered query (including any `LIMIT`/`OFFSET`) as a
subquery -- `SELECT COUNT(*) FROM (...)` -- so it reflects whatever rows
that exact query would actually return, including one already `LIMIT`ed
below the true total. Both require an already-open connection exposing
`#query(sql, params)`; see the file comment at the top of `arel.di` for
why nothing here names `SQLite3` directly.

## What's deliberately out of scope

- **`INSERT`/`UPDATE`/`DELETE`.** This builds and reads `SELECT`
  statements only -- writes belong to a mapper/repository layer above
  this one.
- **Joins, grouping, aggregates, aliases, and subqueries as first-class
  values.** These are the next relational-algebra layers, not hidden raw SQL
  shortcuts in the initial AST.
- **Visitors for other adapters.** Nodes contain no SQLite rendering logic;
  `ArelSQLiteVisitor` is deliberately separate so later dialect visitors can
  render the same query tree.
