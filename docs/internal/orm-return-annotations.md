# ORM return annotation audit

The working branch is `orm-type-annotations`. Keep Arel and later ActiveRecord
annotation batches on this branch, with separate verified commits; open one
consolidated PR when the work is ready.

## Arel fixed contracts

The first batch adds declared returns for:

- Query and compound pagination (`Int | Nil`), table aliases and ordering NULL
  placement (`String | Nil`).
- Query counts and Insert/Update/Delete execution counts (`Int`), and statement
  row queries (`Array`). PostgreSQL integer OIDs and MariaDB integer field types
  decode to Diamond integers, as SQLite does; execution counts are integers in
  all three native drivers. UncachedStatement forwards the same contracts.
- Compound operator, orderings, and projection metadata; fixed join/CTE metadata;
  write CTE and structure accessors; conflict columns and constraint names.
- Concrete factories, render/inspection/traversal forwarding methods, visitor
  names, PostgreSQL capability checks, and the Array-normalization helper.

Nullable contracts retain both the unset and populated cases, including zero
limits. No getter changes its implementation or coerces its result. Connections
and decorators must return Arrays for row queries and integers for execution;
these annotations enforce that boundary.

## Returns that remain dynamic

- `Query#source_query`, compound left/right branches, CTE queries, scalar and
  EXISTS subqueries: a Query/CompoundQuery-only union would close the currently
  duck-typed rendering surface. A shared query protocol needs separate design.
- Expression children, predicates, membership operands, bound/literal values,
  and assignment values: their types intentionally vary with caller input.
- Inspector reconstruction and simplification (and their module forwarders):
  extension nodes may return their own classes, and reporting changes the result
  shape. Avoid a closed union of the built-in node classes.
- `PreparedStatements.for`: native Statements, uncached forwarding, and custom
  connection/statement decorators share a protocol, not one concrete class.
- Constructors and raise-only/validation helpers are not part of this batch.

## ActiveRecord follow-up

Audit `Relation#count`, repository write counts, and Model forwarding first.
`Relation#first`, `Repository#find`, and model lookup results depend on the row
mapper; do not assume a Hash or one Model class. Preserve nullable and callback
contracts before proposing types.

## Validation and measurements

The nullable-accessor regression covers Query, CompoundQuery, Table, and Ordering.
A compile-error regression rejects a String-returning wrapper around Query#count.
The existing Arel cases cover dialect rendering, custom visitors, SQLite writes
and RETURNING, compound queries, traversal, and inspection. Run `make test` before
every commit, and compare benchmark output as well as timing.

Release render benchmark on this checkout (`make release`, GCC, `-O3`, native
architecture, LTO; `bench/arel_render.di`, 20,000 renders, cache disabled):
five before/after runs per mode, alternating order, with identical SQL and
parameter-count output. Both variants used the same binary and isolated copies
of the package; the baseline package came from `55c363ed`.

| Mode | Baseline median | Annotated median | Change |
| --- | ---: | ---: | ---: |
| Interpreter | 1.450 s | 1.581 s | 9.1% slower |
| `DIAMOND_JIT=1` | 1.436 s | 1.561 s | 8.7% slower |

These are whole-process microbenchmark timings, including compilation, not
application request measurements. Removing only the Query nullable annotations,
Table annotations, or Array-normalization annotation in three-run exploratory
comparisons did not establish a single cause. The first batch adds checked
contracts, not a demonstrated speedup. Investigate the regression before merging
the consolidated annotation work; do not extend the prior batches' performance
claims to this one.
