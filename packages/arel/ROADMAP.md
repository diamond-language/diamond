# Arel roadmap

This roadmap describes where the query-construction library is going. It is
ordered by dependency and usefulness, not by resemblance to the Ruby Arel API.
SQLite is the proving dialect; the node model should remain usable by future
database visitors.

## Direction

Arel should provide a small relational algebra that produces a SQL statement
and its ordered bind parameters. Query construction stays immutable and does
not open connections, manage transactions, map rows to domain objects, or
track changes.

The central boundary is:

```text
query nodes -> dialect visitor -> [sql, bind_params]
```

Raw SQL remains an explicit escape hatch, not the representation used by new
features. Values are bind parameters by default. Identifiers are represented
as nodes and quoted by the active visitor.

## File layout

**Done**: `lib/arel.di` (was one 3357-line file, ~45 classes/interfaces/
functions, all wrapped in one `module Arel ... end`) is now split one file
per class under `lib/arel/`, with a handful of shared files for the smaller
AST-leaf node groups (`boolean_nodes.di` for `Not`/`Logical`/`Predicate`/
`Membership`/`Between`, `ordering_nodes.di` for `Collation`/`Ordering`/
`Alias`, `expression_nodes.di` for `Function`/`BinaryExpression`/`Literal`/
`Cast`, `relation_nodes.di` for `Attribute`/`QualifiedStar`/`Table`/
`RawSql`/`ExcludedAttribute`/`ConflictAttribute`, `join_nodes.di` for
`Join`/`Exists`/`ScalarSubquery`/`Cte`), each of the larger classes in its
own file (`visitor.di`, `sqlite_visitor.di`, `postgresql_visitor.di`,
`mariadb_visitor.di`, `mysql_visitor.di`, `query.di`, `compound_query.di`,
`cte_relation.di`, `insert.di`, `update.di`, `delete.di`, `inspector.di`),
`write_support.di` for the small write-statement helper classes
(`AssignmentValue`, `ConflictTarget`, `ConflictConstraintTarget`,
`DefaultValues`, `ColumnDefault`) plus the `Arel.append_cte`/
`Arel.render_insert_conflict` module methods they're used by, and
`module_functions.di` for the rest of `Arel`'s own `self.x` convenience
methods (`Arel.table`, `Arel.from`, ...). `lib/arel.di` is now a thin entry
point, one `require` per file in the original top-to-bottom order --
readable, and also the order that happens to already satisfy the two real
constraints that exist here (unlike active_record's split, which had none):
`sqlite_visitor`/`postgresql_visitor`/`mariadb_visitor`/`mysql_visitor` each
inherit from `Visitor` and so need `visitor.di` required first, and
`cte_relation.di`'s `CteRelation < Table` needs `relation_nodes.di` (where
`Table` lives) required first -- reopening and the declaration-discovery
pass make class/module cross-references order-independent, but not real
inheritance. `support.di` (the four `Arel*Node`/`ArelInspectable` interfaces
and the `arel_array`/`arel_quote_identifier`/`arel_quote_identifier_backtick`/
`arel_cte_name` free functions, the only content that sits outside
`module Arel` itself) is required before everything else for a different
reason: those are bare top-level function calls, which -- unlike class
references -- Diamond still only resolves in source order, not forward
(see the [compiler overview](../../docs/runtime-reference.md#no-ast)).

One incidental fix made in passing: a large comment block documenting
`Arel::MariaDBVisitor`'s design (backtick quoting, its upsert grammar,
`RETURNING` only on `INSERT`/`DELETE`, ...) was sitting directly above
`class Query` in the old single file rather than above `class
MariaDBVisitor` itself, evidently orphaned from its class by an earlier
edit. Moved to sit above `MariaDBVisitor` in `mariadb_visitor.di`, where it
actually belongs -- verified every class/interface/`self.` method name in
the original file appears exactly once across the new files (no drops, no
duplicates) and the full `tests/cases` corpus (1048 cases) still passes
before treating the split as done.

## Dialect grammar seams

`Arel::Visitor` provides shared traversal, relation-scope validation, query
context, statement dispatch, and diagnostics without inheriting SQLite
capability or quoting policy. Pagination and literal spelling are independently
overridable, and nested compound branch grouping is an explicit visitor
seam. Write `RETURNING` rendering is an explicit visitor seam as well. The
CTE prefix spelling is an explicit visitor seam as well. The portable SQL
baseline also delegates join-clause spelling to the visitor.

**Done**: `Arel::PostgreSQLVisitor` (`lib/arel.di`) is the second dialect this
called for, verified against a live PostgreSQL server
(`test_postgres_dialect.di`/`.sh` -- see `VISITORS.md`'s Conformance
section for why that lives outside `tests/cases/`). The actual inventory,
for whoever investigates a *third* dialect next:

- almost everything Arel currently models is identical syntax and semantics
  between SQLite and PostgreSQL, because SQLite's own CTE/`RETURNING`/
  `ON CONFLICT` support was deliberately modeled on Postgres's to begin
  with -- identifier quoting, `ON CONFLICT ... DO NOTHING`/`DO UPDATE`,
  `DEFAULT VALUES`, `RETURNING`, `WITH`/`WITH RECURSIVE`,
  `NULLS FIRST`/`NULLS LAST`, and the integer bitwise operators (`&`/`|`/
  `<<`/`>>`, renamed from the SQLite-specific-sounding "SQLite integer
  operators" to "integer bitwise operators" as part of this work, since a
  non-SQLite visitor now claims it too) all needed zero new hooks;
- pagination was the one real grammar seam found: SQLite's grammar requires
  `LIMIT` before `OFFSET`, forcing the `LIMIT -1 OFFSET ?` sentinel for an
  offset with no limit; Postgres's grammar accepts a bare `OFFSET n` with no
  `LIMIT` clause at all, so `Arel::PostgreSQLVisitor#render_pagination` skips
  that sentinel entirely;
- this means the "avoid speculative abstraction" caution below was
  justified -- most of the grammar-seam machinery this section used to list
  as open questions turned out not to need dialect-specific hooks once a
  real second dialect existed to check against.

For a third dialect: choose it and inventory its concrete differences before
adding another hook, the same way this pass did, rather than assuming the
SQLite/Postgres split above generalizes -- add narrowly named visitor methods
only for differences an actual conformance fixture proves exist, keep bind
collection in the shared traversal whenever placeholder order is identical
across dialects, and avoid speculative abstraction when the new dialect uses
the same syntax and semantics as the existing two.

**Done**: `Arel::MariaDBVisitor` (`lib/arel.di`) is the third dialect,
verified against a live MariaDB 11 server
(`test_mariadb_dialect.di`/`.sh`). Named "MariaDB" rather than "MySQL"
deliberately: real MySQL 8.x has no `RETURNING` at all, which this visitor
does support (a MariaDB-only feature since 10.5) -- calling it "MySQL"
would have overclaimed for anyone actually running real MySQL. Unlike
Postgres (where pagination was the *only* real seam), this dialect
diverges enough that `Visitor#render_insert`/`#render_update`/
`#render_delete` (the extension point the "Compound and all three write
managers enter the selected visitor first" paragraph above already
describes) needed full per-statement overrides rather than a shared
`render_default` plus a capability flag:

- MariaDB has no `ON CONFLICT` syntax at all: `INSERT IGNORE` (do-nothing)
  and `... ON DUPLICATE KEY UPDATE col = VALUES(col)` (do-update, the
  `excluded.col` equivalent) instead, neither taking an explicit conflict
  target -- MariaDB always resolves against whatever unique/primary key it
  hits. A plain column-list `ConflictTarget` is accepted (and ignored,
  since there's nothing to render); a real target predicate or
  `ConflictConstraintTarget` -- both requesting something more specific
  than MariaDB can express -- are rejected via `conflict-target
  predicates`/`named-constraint conflict targets`, same as SQLite already
  rejects them. `INSERT IGNORE` is honestly a broader mechanism than `ON
  CONFLICT ... DO NOTHING` (it suppresses errors for *any* constraint
  violation on the statement, not just ones matching a specific target) --
  a deliberately accepted semantic gap, not a hidden one;
- `RETURNING` genuinely only works on `INSERT`/`DELETE`, not `UPDATE` (a
  real MariaDB syntax error) -- caught directly, not assumed from the name
  alone. Gated by its own `RETURNING on UPDATE` capability, checked before
  ever calling the shared `render_returning` (which only knows the single
  `returning clauses` capability, true here for `INSERT`/`DELETE`'s sake);
- bare `INSERT ... DEFAULT VALUES` isn't valid MariaDB syntax --
  `INSERT INTO t () VALUES ()` is the equivalent rendered instead, under
  the same `insert default values` capability name so callers don't need
  to know the two dialects spell it differently;
- pagination needed its own sentinel again, like SQLite, but a different
  one: MariaDB's grammar requires `LIMIT` before `OFFSET` same as SQLite,
  but has no negative-limit convention -- `LIMIT 18446744073709551615`
  (2^64-1, MariaDB/MySQL's own documented "unlimited" idiom) stands in for
  SQLite's `LIMIT -1`;
- `explicit NULL ordering` (`NULLS FIRST`/`LAST`) has no MariaDB syntax at
  all, unlike both SQLite and Postgres -- rejected outright, not
  approximated;
- `write CTEs` (`WITH ... INSERT/UPDATE/DELETE`) aren't supported either --
  only `WITH ... SELECT`, read-side recursive CTEs included, so
  `recursive CTEs` stays supported (write statements are already blocked
  earlier by the `write CTEs` rejection, so that combination never reaches
  the recursive check);
- quoting (backticks, not double quotes), per-column `DEFAULT` in a
  multi-row `VALUES` list, and the integer bitwise operators all matched
  SQLite/Postgres exactly, needing no changes.

Also worth knowing for anyone writing further MariaDB conformance fixtures:
its own `#execute`/affected-rows convention differs from Postgres/SQLite's
for upsert specifically -- a duplicate `INSERT IGNORE` or a `ON DUPLICATE
KEY UPDATE` that writes back the same value both report `0` rows affected
(not `1`), while an actual value change reports `2` (not `1`) for the
update case. Verified directly against a live server before writing any
assertion depending on it, not assumed from the SQLite/Postgres pattern.

**Done**: `Arel::MySQLVisitor` (`lib/arel.di`) is the fourth dialect,
verified against a live MySQL 8 server (`test_mysql_dialect.di`/`.sh`).
Correcting an earlier assumption in this doc and `docs/roadmap.md`: a
fourth dialect does *not* need new native connectivity. Diamond's
"MariaDB" native support was never MariaDB-branded at the native layer at
all -- the object kind, handle struct, and stdlib class are literally
named `MySQL` (`src/object.h`, `docs/io.md`), backed by MariaDB
Connector/C, which already speaks the real MySQL wire protocol. This
dialect is purely a new visitor, no native code.

Checked every dialect-specific behavior `Arel::MariaDBVisitor` implements
against a live MySQL 8.4.11 server rather than assuming MariaDB's own
findings still applied -- almost all of it is identical shared MariaDB/
MySQL syntax: backtick quoting, the `LIMIT 18446744073709551615` pagination
sentinel, `INSERT IGNORE`, bare `INSERT INTO t () VALUES ()`, rejecting
`NULLS FIRST`/`LAST` and write CTEs, and the `0`/`2` upsert affected-rows
convention all matched exactly. Two real differences:

- `RETURNING` doesn't exist in MySQL 8 on *any* statement kind (MariaDB has
  had it on `INSERT`/`DELETE` since 10.5) -- rejected unconditionally
  through the single shared `returning clauses` capability
  (`Visitor#render_returning`), simpler than MariaDB's own two-part gate
  (a separate, narrower `RETURNING on UPDATE` rejection was only needed
  because MariaDB *does* support it elsewhere);
- `ON DUPLICATE KEY UPDATE col = VALUES(col)` still works on MySQL 8 but is
  deprecated (warning 1287, verified live) in favor of a row-alias form:
  `INSERT ... VALUES (...) AS new_row ON DUPLICATE KEY UPDATE col =
  new_row.col`. `Arel::MySQLVisitor` emits this modern form for a
  `VALUES(...)`-list insert (confirmed live that the alias can always be
  present, even unreferenced, with no warning or error) -- **except** for
  an `INSERT ... SELECT` source, where the row-alias form has no working
  syntax at all: every placement tried against a live server (after the
  table name, after the column list, after the SELECT) is either a syntax
  error or silently aliases the wrong table (the `FROM` source, not the
  inserted row). That shape keeps rendering the older `VALUES(col)` form,
  the only thing that actually works there -- a genuine, live-discovered
  asymmetry MySQL's own docs don't advertise, tracked by a
  `@mysql_insert_uses_row_alias` instance flag `MySQLVisitor` sets per
  render (`Visitor` already has this kind of mutable per-render state
  precedent in `@query`).

Also worth knowing: real MySQL 8 is stricter than MariaDB about DDL, not
Arel-rendered SQL -- a bare `TEXT`/`BLOB` column can't have a `DEFAULT` or
appear in a `UNIQUE` key without an explicit prefix length (MariaDB allows
both). This only affects `test_mysql_dialect.di`'s own fixture schemas
(`VARCHAR(50)` where the MariaDB fixture uses bare `TEXT`), not anything
Arel generates, since Arel never renders `CREATE TABLE`.

## Deferred expression decisions

Resolved once the PostgreSQL dialect existed to check each against:

- per-column DEFAULT: genuinely PostgreSQL-only (SQLite's multi-row `VALUES`
  grammar has no per-column `DEFAULT` placeholder). Modeled as `ColumnDefault`
  (`Arel.column_default()`), gated behind the `per-column default values`
  capability -- `Arel::SQLiteVisitor` rejects it, `Arel::PostgreSQLVisitor`
  accepts it;
- named-constraint conflict targets: also genuinely PostgreSQL-only (SQLite's
  `ON CONFLICT` only ever takes a column list, never `ON CONSTRAINT name`).
  Modeled as `ConflictConstraintTarget`
  (`Arel.conflict_target_on_constraint(name)`), gated behind the
  `named-constraint conflict targets` capability;
- parameterized CAST type declarations (e.g. `NUMERIC(10, 2)`): turned out
  portable -- verified directly against both dialects that they accept
  identical syntax here, so this needed a widened regex on `Cast`'s type
  validation, not a capability gate or a new node.

**Closed, deliberately not pursued**: additional operators beyond the
measured SQLite use cases. Unlike the three items above, nothing ever
resolved this one against a second dialect or a real consuming need --
checked the full history of this exact line (unchanged since it was
first written) and it was explicitly left open twice before, each time
for the same stated reason: "no concrete need has surfaced yet." That's
still true. No file in this package, no test, and no prior commit names
a single candidate operator (no `LIKE` variant beyond plain `LIKE`, no
`IS DISTINCT FROM`, no regex match, no JSON/array operators, no unary
negation) -- there is no "measured" use case driving this, so adding
operators now would be exactly the speculative API-symmetry expansion
this line already warns against. Revisit only when a real consuming
need actually surfaces (a repository method that needs one, a dialect
that renders one differently), not preemptively.

Worth knowing for anyone who does revisit this: a handful of existing
operator methods already aren't exercised by any test today --
`lteq`, `not_in_subquery`, `subtract_expression`, `multiply_expression`,
`divide_expression`, `concat_expression`, `modulo_expression` (the
`_expression` variants of `Attribute`'s own arithmetic operators, taking
an AST node instead of a bind value on the right side). These were
added as symmetric pairs alongside their tested counterparts rather than
against a measured need, the same pattern this section's own guidance
warns future additions against -- not removed here since deleting public
API surface is its own separate decision, but a real candidate for
either a conformance fixture (if they're worth keeping) or removal (if
they're not) the next time this area gets touched.

## Quality bar

Every milestone should include:

- rendering tests for SQL and exact bind order;
- execution tests against an in-memory SQLite database;
- immutability tests proving branches do not mutate their source query;
- identifier-quoting and injection-safety cases;
- nested-composition cases rather than isolated method-only tests;
- documentation examples that are valid Diamond syntax.

The compatibility string API should remain supported while it has users, but
new capabilities do not need parallel raw-string convenience methods.

## Outside this library

These belong in layers built on Arel rather than in Arel itself:

- connection pools and connection lifecycle;
- transactions and retry policy;
- schema definition and migrations;
- model classes, identity maps, dirty tracking, validations, and callbacks;
- repositories, associations, eager loading, and row-to-object mapping;
- automatic query execution or implicit global database connections.

Those layers may consume Arel nodes and `[sql, bind_params]`; they should not
become responsibilities of the query algebra.
