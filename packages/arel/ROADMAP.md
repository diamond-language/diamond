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

## Dialect grammar seams

`ArelVisitor` provides shared traversal, relation-scope validation, query
context, statement dispatch, and diagnostics without inheriting SQLite
capability or quoting policy. Pagination and literal spelling are independently
overridable, and nested compound branch grouping is an explicit visitor
seam. Write `RETURNING` rendering is an explicit visitor seam as well. The
CTE prefix spelling is an explicit visitor seam as well. The portable SQL
baseline also delegates join-clause spelling to the visitor.

**Done**: `ArelPostgreSQLVisitor` (`lib/arel.di`) is the second dialect this
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
  `LIMIT` clause at all, so `ArelPostgreSQLVisitor#render_pagination` skips
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

## Deferred expression decisions

These need another dialect or a real query before they justify nodes:

- per-column DEFAULT, which SQLite does not accept wherever other dialects do;
- named-constraint conflict targets;
- parameterized or dialect-specific CAST type declarations;
- additional operators beyond the measured SQLite use cases.

Do not emulate unsupported SQLite syntax merely for API symmetry.

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
