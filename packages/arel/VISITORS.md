# Arel visitor protocol

A visitor turns immutable Arel statements into `[sql, bind_params]`. Callers
normally use `statement.to_sql(visitor)` or `Arel.render(statement, visitor)`;
omitting the visitor selects `Arel::SQLiteVisitor`.

`Arel::Visitor` is the reusable base. It owns AST traversal, relation-scope
validation, nested query context, statement dispatch, capability diagnostics,
and the portable SQL baseline. A concrete dialect supplies at least
`visitor_name()`, `supports_extension?(name)`, `quote_identifier(name)`, and
`render_pagination(limit, offset, params, bind_values)`. The pagination method
owns placeholder collection for limits and offsets. `Arel::SQLiteVisitor`
supplies those policies for the default renderer.

Diamond uses method-shape conventions rather than interfaces. A visitor used
by every current statement manager provides:

- `render(query) -> Array` for an `Arel::Query`;
- `render_compound(query) -> Array` for compound statements;
- `render_insert(statement) -> Array`, `render_update(statement) -> Array`, and
  `render_delete(statement) -> Array` for visitor-owned write rendering;
- `render_expression(expression, params) -> String` for projections,
  predicates, assignments, ordering, and RETURNING;
- `render_literal(value) -> String` for dialect-specific literal spelling;
- `render_pagination(limit, offset, params, bind_values) -> String` for SELECT
  and compound pagination;
- `render_ctes(statement, params) -> String` for read and write managers;
- `quote_identifier(name) -> String` for every relation, column, alias, CTE,
  collation, conflict target, and assignment identifier;
- `visitor_name() -> String` for diagnostics;
- `supports_extension?(name) -> Bool` and `require_extension(name)` for
  dialect-specific nodes.

Query context is restored even when rendering raises, so a visitor instance
may be reused safely after validation or unsupported-capability errors.
The base literal renderer spells booleans as `TRUE`/`FALSE`; dialects may
override it. Pagination is deliberately concrete-dialect policy because
offset-only syntax and placeholder support differ.
Query builders reject negative limits and offsets before rendering, so dialect
pagination methods receive only `nil` or non-negative integer values.
When a paginated query is nested, its visitor appends predicate and pagination
binds while rendering that subtree. Enclosing predicates append afterward;
dialects must not reorder the returned bind array independently of their SQL
placeholders.

Compound and all three write managers enter the selected visitor first. The
SQLite visitor delegates to each statement's `render_default(visitor)` fallback
for its current grammar; another visitor may replace the complete statement or
wrap that fallback. A wrapper must return the fallback's bind array unchanged
unless its SQL adds or removes corresponding placeholders. `Arel::MariaDBVisitor`
is the first to actually replace the complete statement rather than wrap the
fallback: its `render_insert`/`render_update`/`render_delete` reimplement all
three from scratch (via each statement's `structure()` accessor) rather than
calling `render_default` at all, since its upsert/RETURNING/DEFAULT VALUES
grammar differs too much from the shared fallback to wrap.

Visitors may override `quote_identifier` independently of expression
rendering. Statement managers never call SQLite's quoting helper directly, so
an override applies consistently to SELECT and all write families. The method
receives a validated identifier component, not a dotted SQL fragment.

## Extension capabilities

The current named capabilities are:

- `excluded-row attributes`;
- `conflict-target predicates`;
- `upsert conflict actions`;
- `insert default values`;
- `returning clauses`;
- `explicit NULL ordering`;
- `write CTEs`;
- `recursive CTEs`;
- `integer bitwise operators`;
- `per-column default values`;
- `named-constraint conflict targets`;
- `RETURNING on UPDATE` -- private to `Arel::MariaDBVisitor`'s own
  `render_update` override (see below); no shared, portable code path ever
  checks this name, unlike every other capability in this list.

`Arel::SQLiteVisitor` supports every capability except `per-column default
values` and `named-constraint conflict targets`, which are genuinely
PostgreSQL-only: SQLite has no per-column `DEFAULT` placeholder in a
multi-row `VALUES` list and no named-constraint `ON CONFLICT ON CONSTRAINT`
form, only column-list conflict targets. A visitor may support any subset.
Unsupported use raises an `ArgumentError` naming both the visitor and the
capability before SQL is returned.

SQLite's `quote_identifier` rejects empty components with
`SQL identifier cannot be empty` and doubles embedded quote characters. The
same check therefore covers relations, attributes, aliases, CTEs, write
columns, INSERT…SELECT columns, and conflict targets. Other dialect visitors
own their corresponding identifier validity rules.

Quoting preserves the caller's identifier spelling, while SQLite-oriented
query validation compares relation scopes case-insensitively. This applies to
base relations, join aliases, explicit correlations, CTE declarations, and
recursive CTE self-references. A future dialect with different name-resolution
rules will need to make this validation policy dialect-aware as well as
overriding `quote_identifier`.

Extension nodes expose `extension_name()` where the capability belongs to a
specific node. Statement managers check clause-level capabilities such as
RETURNING, upsert actions, and write CTEs. The visitor checks expression-level
capabilities such as explicit NULL ordering and SQLite's integer operators at
the point where it renders them.

## Conformance

`tests/cases/arel_dialects.di` defines an `Arel::Visitor` subclass that rejects
every extension without inheriting SQLite capability or quoting policy.
Its fixtures are the current portable baseline for SELECT, INSERT, UPDATE,
DELETE, compound queries, and CTEs. SQLite-specific rendering and execution
remain covered by the other focused Arel suites.

A future database visitor should begin by running the portable fixtures against
its renderer, then add separate fixtures for each capability it chooses to
support. It should not claim a capability merely because the target dialect has
similarly named syntax; bind ordering and node semantics must also match.

`Arel::PostgreSQLVisitor` (`lib/arel.di`) is the second such visitor, and the
first to actually follow that guidance end to end: every capability it
claims (`supports_extension?` returns `true` for all of them, a strict
superset of `Arel::SQLiteVisitor`'s) is backed by a real execution-based fixture in
`test_postgres_dialect.di`, run against a live PostgreSQL server via
`test_postgres_dialect.sh` -- not just plausible-looking rendered SQL. That
suite is deliberately kept out of `tests/cases/`: everything there is
self-contained (in-memory SQLite), while this needs an already-running
external server Diamond can't spin up itself, so it's opt-in
(`test_postgres_dialect.sh` manages its own throwaway container) rather than
part of `make test`/CI. See `ROADMAP.md`'s "pick the next dialect" entry for
what this pass found: nearly everything Arel models turned out to be
identical between the two dialects (both were modeled on Postgres's own SQL
to begin with), with pagination as the one real grammar seam.

`Arel::MariaDBVisitor` (`lib/arel.di`) is the third, verified the same
way against a live MariaDB server via `test_mariadb_dialect.di`/`.sh`.
Named "MariaDB" rather than "MySQL" since real MySQL 8.x lacks `RETURNING`
entirely (a MariaDB-only feature this visitor does support) -- calling it
"MySQL" would have overclaimed. Unlike PostgreSQLVisitor, this dialect
diverges enough from the shared portable model that `render_insert`/
`render_update`/`render_delete` are overridden entirely rather than
relying on `render_default` plus a capability flag: no `ON CONFLICT`
syntax at all (`INSERT IGNORE`/`ON DUPLICATE KEY UPDATE col = VALUES(col)`
instead, with no explicit conflict target MariaDB can express), `RETURNING`
only on `INSERT`/`DELETE` not `UPDATE`, no bare `DEFAULT VALUES`, its own
pagination sentinel, and no `NULLS FIRST`/`LAST` or write CTEs at all. See
`ROADMAP.md`'s "pick the next dialect" entry for the full inventory,
including the semantic gap this pass deliberately accepted (`INSERT
IGNORE` suppresses a broader class of errors than `ON CONFLICT ... DO
NOTHING` does) and the affected-rows convention difference (a no-op
upsert reports `0`, not `1`; a value-changing `ON DUPLICATE KEY UPDATE`
reports `2`, not `1`) worth knowing before writing further fixtures
against it.

`Arel::MySQLVisitor` (`lib/arel.di`) is the fourth, verified against a
live MySQL 8 server via `test_mysql_dialect.di`/`.sh`. It does *not*
support `RETURNING on UPDATE` -- unlike MariaDBVisitor, it rejects
`RETURNING` unconditionally through the single shared `returning clauses`
capability, since MySQL 8 has no `RETURNING` on any statement kind at all,
so the private `RETURNING on UPDATE` capability above is specific to
MariaDBVisitor's own narrower gap and doesn't apply here. Otherwise nearly
identical to MariaDBVisitor (verified live, not assumed) -- same
`INSERT IGNORE`, pagination sentinel, `NULLS FIRST`/`LAST` and write-CTE
rejection, and `0`/`2` affected-rows convention. The one other real
difference is upsert rendering: MySQL 8's `ON DUPLICATE KEY UPDATE col =
VALUES(col)` is deprecated in favor of a row-alias form
(`... VALUES (...) AS new_row ... col = new_row.col`), which this visitor
emits for a `VALUES(...)`-list insert, falling back to the older
`VALUES(col)` form for an `INSERT ... SELECT` source -- verified live that
the row-alias form has no working syntax at all for that shape. See
`ROADMAP.md`'s own entry for the full comparison.
