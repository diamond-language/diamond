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

## Next milestone: remaining expression decisions

Visitors now expose named dialect capabilities, SQLite-only forms are checked
before rendering, and portable conformance fixtures cover every statement
manager. The remaining write forms need concrete semantic decisions:

- whether per-column DEFAULT warrants a node when SQLite cannot use it in the
  same places as other dialects;
- named-constraint conflict targets for dialects that support them;
- string concatenation and bitwise nodes only if repository queries need them;
- explicit cast nodes and their portability boundary;
- whether functions need a public generic constructor beyond `Arel.sql`.

Completion means each accepted expression has defined bind, grouping, and
dialect behavior rather than existing solely for API symmetry.

## Following milestone: ergonomics and diagnostics

Once the algebra is stable:

- concise construction helpers that respect Diamond's lack of user-defined
  `[]` and variadic arguments;
- inspectable node output for debugging;
- deterministic structural equality for nodes, useful in tests and rewriting;
- optional query-rewrite passes only where they eliminate real duplication or
  enable an adapter feature.

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
