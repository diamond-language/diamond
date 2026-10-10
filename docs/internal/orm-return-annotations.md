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

## Regression investigation

The green correctness checks do not resolve the performance regression. A second
comparison pinned both variants to CPU 0 and timed only the render loop with
`Time.monotonic()`. Five interleaved runs per variant used the same release binary,
baseline package, and 20,000-render workload described above.

| Mode | Baseline render median | Annotated render median | Change |
| --- | ---: | ---: | ---: |
| Interpreter | 1.330 s | 1.431 s | 7.6% slower |
| `DIAMOND_JIT=1` | 1.337 s | 1.473 s | 10.2% slower |

Compilation stayed around 70–72 ms, with approximately 1 ms difference between
variants. SQL and parameter-count output matched. The slowdown therefore occurs
primarily during execution, rather than package loading or compilation.

Separate diagnostic runs with `DIAMOND_TRACE_OPCODES=1`, `DIAMOND_TRACE_IC=1`,
and `DIAMOND_TRACE_GC=1` produced these counts (trace runs are not timing samples):

| Counter | Baseline | Annotated |
| --- | ---: | ---: |
| Method-cache hits | 4,799,971 | 4,459,983 |
| Method-cache misses | 420,038 | 760,026 |
| `INVOKE` executions | 9,400,187 | 9,700,166 |
| `INVOKE_MONO` executions | 4,739,941 | 4,439,962 |
| `CHECK_TYPE` executions | 1,320,018 | 1,460,019 |
| Major collections | 13,338 | 13,338 |

Every other opcode count matched. Normalized `--dump-bytecode` output showed
32 changed function bodies, each differing only by added `CHECK_TYPE`
instructions. The annotation batch introduces both extra guard work and a
substantial loss of monomorphic method dispatch.

The method, extension, and field caches use 64 direct-mapped slots selected by
`((uintptr_t)site >> 2) % DIAMOND_INLINE_CACHE_COUNT` (see `src/vm.c`,
`src/vm_internal.h`, and `src/vm.h`). Added bytecode changes call-site addresses
and therefore collisions. An `INVOKE_MONO` whose slot belongs to another site
falls back to `INVOKE`; this explains why unchanged call instructions can execute
differently after adding return guards.

A temporary C diagnostic harness relocated function code buffers without
changing their bytecode. Three interleaved interpreter samples gave:

| Code-buffer layout | Baseline median | Annotated median | Baseline / annotated method-cache misses |
| --- | ---: | ---: | ---: |
| Original | 1.426 s | 1.588 s | 420,038 / 760,026 |
| Deterministic relocation, seed 1 | 1.439 s | 1.542 s | 420,038 / 420,038 |
| Deterministic relocation, seed 2 | 1.467 s | 1.546 s | 580,033 / 620,031 |

Changing only addresses reduced the original gap and changed cache misses. This
establishes cache-layout sensitivity as a contributor, not a complete attribution:
even equal method-cache miss counts left a gap, and extension/field collisions
were not controlled by that experiment. In a separate diagnostic, replacing five
new hot accessor checks with equal-length self-moves preserved method-cache
counts and reduced annotated runtime from 1.588 s to 1.562 s. That experiment
confirms guard cost but retains instruction-dispatch overhead and does not remove
every added check. The guard bypass was temporary and is not a proposed fix.

For reproduction, use isolated package snapshots from `55c363ed` and `d53ab581`
with one release binary, `DIAMOND_NO_CACHE=1`, and alternating execution order.
Add `Time.monotonic()` immediately before the existing render loop in
`bench/arel_render.di` and print the elapsed time immediately after it. Collect
trace counters separately from timings; trace flags and allocation layout can
change collision counts. Compare complete SQL and parameter output in every run.

Next, benchmark a cache-collision mitigation against both package snapshots and
other runtime workloads, preserving all return checks. Keep that VM change
separate from this annotation audit. Do not remove contracts or claim an
annotation speedup based on the current evidence.
