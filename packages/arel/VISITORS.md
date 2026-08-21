# Arel visitor protocol

A visitor turns immutable Arel statements into `[sql, bind_params]`. Callers
normally use `statement.to_sql(visitor)` or `Arel.render(statement, visitor)`;
omitting the visitor selects `ArelSQLiteVisitor`.

`ArelVisitor` is the reusable base. It owns AST traversal, relation-scope
validation, nested query context, statement dispatch, capability diagnostics,
and the portable SQL baseline. A concrete dialect supplies at least
`visitor_name()`, `supports_extension?(name)`, `quote_identifier(name)`, and
`render_pagination(limit, offset, params, bind_values)`. The pagination method
owns placeholder collection for limits and offsets. `ArelSQLiteVisitor`
supplies those policies for the default renderer.

Diamond uses method-shape conventions rather than interfaces. A visitor used
by every current statement manager provides:

- `render(query) -> Array` for an `ArelQuery`;
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
unless its SQL adds or removes corresponding placeholders.

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
- `integer bitwise operators`.

`ArelSQLiteVisitor` supports all of them. A visitor may support any subset.
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

`tests/cases/arel_dialects.di` defines an `ArelVisitor` subclass that rejects
every extension without inheriting SQLite capability or quoting policy.
Its fixtures are the current portable baseline for SELECT, INSERT, UPDATE,
DELETE, compound queries, and CTEs. SQLite-specific rendering and execution
remain covered by the other focused Arel suites.

A future database visitor should begin by running the portable fixtures against
its renderer, then add separate fixtures for each capability it chooses to
support. It should not claim a capability merely because the target dialect has
similarly named syntax; bind ordering and node semantics must also match.

`ArelPostgreSQLVisitor` (`lib/arel.di`) is the second such visitor, and the
first to actually follow that guidance end to end: every capability it
claims (`supports_extension?` returns `true` for all of them, same as
`ArelSQLiteVisitor`) is backed by a real execution-based fixture in
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
