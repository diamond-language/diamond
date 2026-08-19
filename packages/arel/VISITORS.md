# Arel visitor protocol

A visitor turns immutable Arel statements into `[sql, bind_params]`. Callers
normally use `statement.to_sql(visitor)` or `Arel.render(statement, visitor)`;
omitting the visitor selects `ArelSQLiteVisitor`.

Diamond uses method-shape conventions rather than interfaces. A visitor used
by every current statement manager provides:

- `render(query) -> Array` for an `ArelQuery`;
- `render_expression(expression, params) -> String` for projections,
  predicates, assignments, ordering, and RETURNING;
- `render_ctes(statement, params) -> String` for read and write managers;
- `visitor_name() -> String` for diagnostics;
- `supports_extension?(name) -> Bool` and `require_extension(name)` for
  dialect-specific nodes.

Compound queries render their branches through each branch's `render_with`
method and use `render_expression` for result ordering. INSERT, UPDATE, and
DELETE managers render their own statement skeletons, delegating all embedded
queries, CTE bodies, and expressions to the selected visitor.

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
- `SQLite integer operators`.

`ArelSQLiteVisitor` supports all of them. A visitor may support any subset.
Unsupported use raises an `ArgumentError` naming both the visitor and the
capability before SQL is returned.

Extension nodes expose `extension_name()` where the capability belongs to a
specific node. Statement managers check clause-level capabilities such as
RETURNING, upsert actions, and write CTEs. The visitor checks expression-level
capabilities such as explicit NULL ordering and SQLite's integer operators at
the point where it renders them.

## Conformance

`tests/cases/arel_dialects.di` defines a visitor that rejects every extension.
Its fixtures are the current portable baseline for SELECT, INSERT, UPDATE,
DELETE, compound queries, and CTEs. SQLite-specific rendering and execution
remain covered by the other focused Arel suites.

A future database visitor should begin by running the portable fixtures against
its renderer, then add separate fixtures for each capability it chooses to
support. It should not claim a capability merely because the target dialect has
similarly named syntax; bind ordering and node semantics must also match.
