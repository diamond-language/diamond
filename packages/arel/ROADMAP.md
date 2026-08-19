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

## Next milestone: explicit dialect capabilities

The node model and rendering entry point are already visitor-oriented, but
SQLite knowledge still leaks into which builders callers may safely use. Make
the boundary explicit before implementing another database renderer:

- define a small dialect capability contract for syntax that is not portable,
  beginning with conflict handling, RETURNING, NULL ordering, and write CTEs;
- let visitors reject unsupported node combinations with useful errors before
  emitting partial SQL;
- keep SQLite as the sole production dialect while exercising the contract
  with focused visitor fixtures;
- document which builders are relationally portable and which represent
  deliberate SQLite extensions;
- preserve the current `[sql, bind_params]` result and exact bind ordering so a
  later dialect visitor does not change the execution boundary.

Completion means a second dialect can be added by implementing and declaring
visitor behavior, without changing existing query nodes or teaching callers to
inspect visitor classes.

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
