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

## Next milestone: compound-query composition

Round out the structural set-operation layer:

- explicit grouping when nested compound branches require parentheses;
- ordering, limits, and offsets applied to the compound result rather than an
  individual branch;
- compound queries as CTE bodies and derived-table sources in execution tests;
- clearer diagnostics when wildcard projections make branch shape unknowable;
- optional structural type compatibility checks if query expressions acquire
  reliable result-type metadata.

Completion means every feature composes with existing predicates, preserves
bind ordering, quotes identifiers correctly, and executes against SQLite in
the package tests.

## Following milestone: recursive common table expressions

Extend the existing non-recursive `WITH` representation:

- `WITH RECURSIVE` rendering without making every CTE recursive;
- self-reference through an explicit CTE relation object;
- anchor/recursive branch helpers built from ordinary `UNION ALL` nodes;
- validation for duplicate names and invalid self-reference;
- SQLite execution coverage for a bounded recursive sequence or tree walk.

CTEs should remain query nodes visited by the renderer, never interpolated SQL
strings. SQLite execution tests should cover multiple CTE references and bind
parameters in both CTE bodies and the consuming query.

## Milestone 3: data-changing statements

Add separate immutable managers for:

- `INSERT`, including multi-row inserts and `INSERT ... SELECT`;
- `UPDATE` with structured assignments and predicates;
- `DELETE` with predicates;
- SQLite conflict handling and `RETURNING` through dialect-specific nodes.

Write statements return `[sql, bind_params]` like SELECT statements. Executing
them remains the responsibility of the caller or a higher repository layer.

## Milestone 4: visitor and adapter boundary

Prove that the AST is not accidentally SQLite-specific:

- define the visitor protocol expected by query managers;
- make visitor selection explicit rather than hard-coded by `to_sql`;
- separate portable nodes from dialect extension nodes;
- add a second visitor when Diamond gains another database adapter;
- maintain shared conformance fixtures for portable SQL and dialect-specific
  fixtures for quoting, placeholders, and extensions.

Do not design hypothetical dialect abstractions before a second adapter exists.
SQLite behavior should stay direct and readable until a concrete difference
needs an abstraction.

## Milestone 5: ergonomics and diagnostics

Once the algebra is stable:

- concise construction helpers that respect Diamond's lack of user-defined
  `[]` and variadic arguments;
- inspectable node output for debugging;
- actionable errors for unsupported nodes and invalid query shapes;
- deterministic structural equality for nodes, useful in tests and rewriting;
- optional query-rewrite passes only where they eliminate real duplication or
  enable an adapter feature.

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
