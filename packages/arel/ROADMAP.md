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

## Milestone 1: complete SELECT composition

Build the common single-table query surface on the existing AST:

- aliases for tables, attributes, and projected expressions;
- `DISTINCT` and expression aliases;
- `IN`/`NOT IN`, `BETWEEN`, and `LIKE` predicates;
- ordering with explicit null placement where the dialect supports it;
- grouping and `HAVING`;
- aggregate and scalar function nodes, beginning with `COUNT`, `SUM`, `MIN`,
  `MAX`, `AVG`, `LOWER`, and `UPPER`;
- a first-class SQL literal node for deliberate fragments that cannot yet be
  represented structurally.

Completion means every feature composes with existing predicates, preserves
bind ordering, quotes identifiers correctly, and executes against SQLite in
the package tests.

## Milestone 2: joins and query aliases

Add relations involving more than one source:

- inner and left outer joins;
- structured join predicates;
- multiple joins in a deterministic order;
- table aliases and self-joins;
- qualified wildcard projections;
- clear errors for attributes used outside the query's relation set.

Joins should be nodes visited by the renderer, never interpolated SQL strings.
SQLite execution tests should cover ambiguous column names, aliases, and bind
parameters inside join conditions.

## Milestone 3: subqueries and set operations

Make a SELECT query usable anywhere a relation or expression is valid:

- subqueries in `FROM` with a required alias;
- scalar subqueries in projections and predicates;
- `EXISTS`/`NOT EXISTS`;
- `IN (subquery)`;
- common table expressions (`WITH`), including recursive CTEs if the node model
  does not need special treatment;
- `UNION`, `UNION ALL`, `INTERSECT`, and `EXCEPT`.

This milestone is the point where the library becomes a relational algebra
rather than a fluent SELECT builder.

## Milestone 4: data-changing statements

Add separate immutable managers for:

- `INSERT`, including multi-row inserts and `INSERT ... SELECT`;
- `UPDATE` with structured assignments and predicates;
- `DELETE` with predicates;
- SQLite conflict handling and `RETURNING` through dialect-specific nodes.

Write statements return `[sql, bind_params]` like SELECT statements. Executing
them remains the responsibility of the caller or a higher repository layer.

## Milestone 5: visitor and adapter boundary

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

## Milestone 6: ergonomics and diagnostics

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
