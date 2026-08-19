# packages/arel

See [ROADMAP.md](ROADMAP.md) for the forward-looking development plan and
[VISITORS.md](VISITORS.md) for the renderer protocol and extension capabilities.

A small, immutable SQL AST and chainable query builder for
[Diamond](https://gitlab.com/dmn9180/diamond) -- the first slice toward
a DataMapper-style persistence layer. Builds and renders SELECT and data-changing
statements through a SQLite visitor while leaving models, row mapping, and
change tracking to a repository layer -- similar to real
[Arel](https://github.com/rails/rails/tree/main/activerecord)'s own
historical scope in Rails.

The initial renderer is `ArelSQLiteVisitor`, but execution remains loosely
coupled: reads call `db.query(sql, params)` and writes call
`db.execute(sql, params)`, the method shapes the SQLite3 driver exposes (see
[`docs/io.md`](https://gitlab.com/dmn9180/diamond/-/blob/main/docs/io.md)'s
"SQLite3" section). Any future adapter exposing the same
`#query(sql, params)` contract is a drop-in target, the same way
[`packages/rack`](../rack/README.md) stayed server-agnostic by
depending on a shared method convention rather than a concrete type.

Every query and write manager accepts an optional visitor in `to_sql(visitor)`;
`Arel.render(statement, visitor)` is the common entry point when code should not
care which manager it has. The no-argument form continues to select
`ArelSQLiteVisitor`. Explicit visitors propagate through derived tables,
subqueries, compound branches, CTE bodies, expressions, conflict clauses, and
`RETURNING`. Execution methods accept the visitor after the database argument,
for example `query.to_a(db, visitor)` and `insert.execute(db, visitor)`.
Visitors declare dialect support through named extension capabilities.
Excluded-row attributes, partial conflict targets, upserts, DEFAULT VALUES,
RETURNING, explicit NULL ordering, write and recursive CTEs, and SQLite integer
operators fail early with a visitor-specific diagnostic when unsupported.
Ordinary SELECTs, compounds, non-recursive read CTEs, and basic INSERT, UPDATE,
and DELETE statements form the portable fixture baseline. See
[VISITORS.md](VISITORS.md) for the complete protocol and capability names.
Visitors also own identifier quoting through `quote_identifier(name)`. One
override therefore applies consistently to relations, attributes, aliases,
CTEs, conflict targets, assignments, and RETURNING expressions across both
read and write statements.

Compound queries and each write manager enter `render_compound`,
`render_insert`, `render_update`, or `render_delete` on the active visitor.
Visitors can replace a complete statement or call `super(statement)` to wrap
SQLite's default rendering while retaining its ordered bind array.

Dialect visitors can inherit `ArelVisitor` for shared AST traversal,
relation-scope checks, nested query context, dispatch, and diagnostics without
inheriting SQLite's capability or identifier-quoting policy. They provide a
visitor name, capability predicate, and identifier quoting method; the default
`ArelSQLiteVisitor` remains selected when no visitor is passed.
Pagination and literal spelling are narrow grammar seams: dialects implement
`render_pagination` and may override `render_literal` without replacing query
traversal. SQLite binds limits and offsets, and renders an offset without an
explicit limit as `LIMIT -1 OFFSET ?`, which is valid SQLite syntax.

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

Supported attribute predicates are `eq`, `not_eq`, `lt`, `lteq`, `gt`,
`gteq`, `in_list`, `not_in`, `between`, `not_between`, `like`, and `not_like`.
`eq(nil)` and `not_eq(nil)` render as `IS NULL` and `IS NOT NULL`.
Predicates compose with `and_also`, `or_else`, and `not_`; explicit grouping is
preserved in the rendered SQL. Attributes also provide `asc()` and `desc()`.

Queries support table/projection aliases, `DISTINCT`, grouping, `HAVING`, and
structured inner/left-outer/cross joins. Duplicate relation aliases and
attributes outside the query's relation set are rejected. `Arel.count`/`sum`/`min`/`max`/`avg` and
`Arel.lower`/`upper` construct function nodes. `Arel.sql(fragment, params)` is
the explicit escape hatch for an expression the AST cannot represent yet.

`Arel.from_subquery(query, "name")` uses a query as an aliased derived table.
`Arel.exists(query)`, `attribute.in_subquery(query)`, and
`Arel.scalar(query)` place subqueries in predicate and scalar-expression
positions while retaining their bind parameters. `Arel.as`, `Arel.asc`, and
`Arel.desc` apply aliases or ordering to arbitrary expressions; orderings can
select `nulls_first()` or `nulls_last()`.

Inner queries opt into outer references with `correlate(table)` or
`correlate_all(tables)`, retaining relation-scope validation at every nesting
level. `Arel.cte(name)` creates a named CTE relation whose columns can be used
like table columns. Its `recursive_body(anchor, branch)` helper constructs and
validates the usual `UNION ALL` body, while `with` and `with_recursive` accept
either that relation or a name. `Arel.union`,
`union_all`, `intersect`, and `except` build structural compound queries and
reject branches with different projection counts. Compound results can be
ordered, paginated, nested as derived sources, or used as CTE bodies.

Immutable write managers use the same `[sql, params]` contract:

```ruby
items = Arel.table("items")

insert = Arel.insert_into(items).values({"name": "pens", "qty": 3})
bulk_insert = Arel.insert_into(items).values_many([
  {"name": "paper", "qty": 5},
  {"name": "cards", "qty": 2}
])
update = Arel.update(items).set({"qty": 4})
update = update.where(items.column("name").eq("pens"))
delete = Arel.delete_from(items).where(items.column("qty").lt(1))

insert.execute(db)
rows = update.returning(items.column("qty")).to_a(db)
delete.execute(db)
```

`from_query(columns, query)` builds `INSERT ... SELECT` and validates that the
target-column and projection counts match. `on_conflict_do_nothing(columns)`
and `on_conflict_do_update(columns, assignments)` expose SQLite's conflict
actions. Wrap AST or raw-SQL assignment expressions with `Arel.expression`;
ordinary assignment values remain binds. INSERT statements can also prepend a
query CTE with `with(name, query)`.

INSERT row values accept `Arel.expression(node)` when a SQL expression is
intentional; unwrapped values remain binds, including in bulk inserts.
`Arel.excluded(column)` structurally references SQLite's excluded row in a
conflict update. `default_values()` emits `INSERT ... DEFAULT VALUES` and
composes with `RETURNING`.

Attributes, excluded-row attributes, and arithmetic expressions provide
`add`, `subtract`, `multiply`, and `divide`; their ordinary arguments remain
bind parameters. The corresponding `*_expression` methods accept another AST
expression. Arithmetic is explicitly parenthesized so chaining preserves the
constructed tree.

`concat`/`concat_expression` build the SQL `||` operator, and `modulo` builds
`%`. `Arel.integer_operator(expression, operator, value)` accepts the validated
operators `&`, `|`, `<<`, and `>>` without multiplying convenience methods
across every node class. All ordinary right operands remain binds.

`Arel.function(name, arguments)` constructs a generic function after validating
that its name is a single identifier. `Arel.cast(expression, type_name)` does
the same for simple CAST type names. Parameterized or dialect-specific type
fragments still require an explicit dialect extension or `Arel.sql`.

`Arel.inspect(node)` returns a deterministic structural description rather than
executable SQL. It covers expressions, predicates, decorators, relations,
subqueries, joins, CTE declarations, compounds, and a concise query summary.
`Arel.same?(left, right)` performs structural comparison for those expression
families and for queries with grouping/HAVING, joins, derived sources, CTEs,
and compound branches as well as projections, predicates, ordering,
distinctness, and pagination. Membership and raw-SQL bind arrays compare by
value rather than array identity. INSERT, UPDATE, and DELETE inspection and
comparison include assignments, predicates, source queries, conflict state,
RETURNING expressions, and CTEs. Unknown third-party objects are reported as
`ArelNode(unknown)` instead of being confused with a supported built-in node.

`Arel.children(node)` exposes each built-in node's immediate children in a
stable semantic order. `Arel.walk(node, visitor)` performs an iterative
depth-first preorder walk, calls `visitor.visit(node)` when a visitor is
provided, and returns the visited nodes for simple analysis without a visitor.
Bound scalar values are not nodes and therefore do not appear in traversal.
`Arel.simplify(node)` performs the deliberately conservative rewrites
`NOT NOT predicate -> predicate` and empty membership normalization recursively
throughout supported read and write trees. It returns immutable nodes and
leaves bind order unchanged. An ordered array of `[pattern, replacement]`
pairs adds declarative application policy; children are rewritten before their
parents, and later rules can consume the result of earlier rules without a
second tree walk. Passing `true` as the third argument returns
`[rewritten_node, changed]`.

```diamond
active = people.column("active").eq(true)
verified = people.column("verified").eq(true)
query, changed = Arel.simplify(
  Arel.from(people).where(active),
  [[active, verified]],
  true
)
```

Rules use `Arel.same?` structural matching rather than object identity. Each
rule must contain exactly one pattern and replacement; malformed rules raise
`ArgumentError`. Replacements are not recursively revisited during that pass.

`Arel.with_children(node, replacements)` rebuilds decorators,
expressions, predicates, joins, CTEs, compounds, SELECT queries, and all three
write managers according to the same order returned by `Arel.children`;
mismatched shapes fail early. Write replacement includes expression-valued
rows/assignments, INSERT sources and conflicts, predicates, RETURNING, and CTEs
while ordinary bound values remain untouched.

Third-party nodes can structurally opt into the tooling protocols by providing
`arel_children()`, `arel_with_children(replacements)`, `arel_inspect()`, and
`arel_same?(other)`. This keeps extension nodes outside the central built-in
type chain while allowing them to participate in traversal, transformations,
diagnostics, and equality.

`Arel.conflict_target(columns)` builds an immutable SQLite conflict target.
Use `target.column(name)` with `where(predicate)` for partial unique indexes;
`Arel.literal(value)` supplies the literal integer or boolean SQLite requires
when matching an index predicate. Strings are deliberately rejected as
structural literals and remain binds everywhere else.

UPDATE and DELETE require a predicate unless the caller explicitly opts into a
whole-table operation with `all()`. SQLite `RETURNING` is available on all three
write managers, including multi-row INSERTs. Every write manager accepts
`with(name, query)` and `with_recursive(name, query)`; duplicate CTE names are
rejected.

Compound branches require explicit projections so their shapes can be checked.
Nested compound trees retain their grouping, including branch-local ordering
and pagination, when rendered for SQLite.

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

`to_sql()` (or `to_sql(visitor)`) returns `[sql, params]` -- the same positional-`Array`-return
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

- **Visitors for other adapters.** Nodes contain no SQLite rendering logic;
  `ArelSQLiteVisitor` is deliberately separate so later dialect visitors can
  render the same query tree.
