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

## Next milestone: correlated subqueries

Allow an inner query to refer deliberately to relations owned by its enclosing
query:

- an explicit correlation API rather than disabling relation-scope checks;
- correlated `EXISTS`, scalar, and `IN` subqueries;
- validation that distinguishes permitted outer references from accidental
  unrelated attributes;
- nested correlation across more than one query level;
- execution tests covering the same column name at inner and outer levels.

Completion means every feature composes with existing predicates, preserves
bind ordering, quotes identifiers correctly, and executes against SQLite in
the package tests.

## Following milestone: common table expressions

Add named query sources shared within a statement:

- non-recursive `WITH` entries;
- multiple CTEs with deterministic declaration and bind order;
- references to CTE output through the ordinary table/attribute surface;
- recursive CTEs after the non-recursive representation is stable;
- clear duplicate-name and out-of-scope diagnostics.

CTEs should remain query nodes visited by the renderer, never interpolated SQL
strings. SQLite execution tests should cover multiple CTE references and bind
parameters in both CTE bodies and the consuming query.

## Milestone 3: set operations

Combine compatible SELECT queries:

- `UNION`, `UNION ALL`, `INTERSECT`, and `EXCEPT`;
- explicit grouping when a compound query is nested or ordered;
- bind ordering across every branch;
- validation of incompatible projection counts where that information is
  structurally available.

This milestone completes the compound-query portion of the relational algebra.

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
