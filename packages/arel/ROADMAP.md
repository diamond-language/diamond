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

## Next milestone: finish recursive-query ergonomics

Recursive CTEs now compose with every read and write manager. The remaining
work is to make recursive intent easier to express and validate:

- provide an explicit named CTE relation helper for self-reference;
- add an anchor/recursive-branch helper built from ordinary `UNION ALL` nodes;
- reject invalid recursive declarations before SQLite sees them;
- cover multiple references to one recursive CTE and mixed recursive/nonrecursive
  declarations.

Completion means recursive queries can be constructed without manually keeping
the CTE name, table reference, and compound body synchronized.

## Following milestone: deepen write expressions

Round out SQLite's data-changing expression forms:

- structural values and expressions in INSERT rows;
- conflict-target predicates if they can be represented without a
  SQLite-specific leak into portable nodes;
- reusable excluded-row attributes for conflict updates;
- DEFAULT VALUES and per-column DEFAULT where SQLite permits them.

Values should remain bound by default; expression insertion must require an
explicit wrapper just as UPDATE assignments do today.

## Milestone 3: visitor and adapter boundary

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

## Milestone 4: ergonomics and diagnostics

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
