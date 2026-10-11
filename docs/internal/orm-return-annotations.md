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
five before/after runs per batch, alternating order, with identical SQL and
parameter-count output. Both variants used the same binary and isolated copies
of the package; the baseline package came from `55c363ed`.

| Mode | Baseline median | Annotated median | Change |
| --- | ---: | ---: | ---: |
| JIT enabled, batch A | 1.450 s | 1.581 s | 9.1% slower |
| JIT enabled, batch B | 1.436 s | 1.561 s | 8.7% slower |

Correction: the initial harness set `DIAMOND_JIT=0` for its intended interpreter
mode, but Diamond enables JIT whenever the variable is present. Both batches
above enabled JIT. The corrected comparison below unsets the variable for
interpreter runs.

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
| JIT enabled, batch A | 1.330 s | 1.431 s | 7.6% slower |
| JIT enabled, batch B | 1.337 s | 1.473 s | 10.2% slower |

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

At the time of the investigation, the method, extension, and field caches used
64 direct-mapped slots selected by
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

The mitigation must be measured against both package snapshots and other runtime
workloads, preserving all return checks. Do not remove contracts or claim an
annotation speedup based on the current evidence.

## Cache mitigation

The follow-up increases the three shared site tables from 64 to 256 slots and
moves them to the end of `DiamondVm`. GC state, dispatch counters, opcode
counters, quickening/JIT flags, and instruction-budget state are no longer
separated by the tables. Lookup, receiver/shape guards, replacement, invalidation,
and return checks retain their existing behavior.

Capacity alone was insufficient: 128 slots improved annotated rendering only
about 1–2% in exploratory JIT-enabled runs, while 256 slots in the original
struct position improved rendering but slowed several dispatch controls by
4–5% and struct access by about 9%. Those candidates were not retained. The
combined capacity/layout change below avoids those large control regressions.

Corrected measurements use `bench/compare_arel_annotations.py`, CPU 0, five
interleaved samples per binary/package combination, identical SQL and parameter
output, no package cache, and the same 20,000-render loop. The original binary
has the VM source from `6ceaf2a4`; both binaries are GCC release builds with LTO.
Package snapshots are `55c363ed` and `d53ab581`. Interpreter runs **unset**
`DIAMOND_JIT`; enabled runs set it to `1`. The script removes inherited
`DIAMOND_*` experiment settings and records samples and separate trace counters.

| Render-only median | Original VM, baseline package | Original VM, annotated package | New VM, baseline package | New VM, annotated package |
| --- | ---: | ---: | ---: | ---: |
| Interpreter | 1.454 s | 1.552 s | 1.415 s | 1.428 s |
| `DIAMOND_JIT=1` | 1.351 s | 1.474 s | 1.323 s | 1.331 s |

Annotated rendering improves **8.0%** in the interpreter and **9.7%** with JIT
enabled. Within the new VM, the annotation gaps are **1.0%** and **0.6%**,
respectively, compared with **6.7%** and **9.1%** on the original VM in this
corrected comparison. These small residual gaps should not be interpreted as
precise guard-cost estimates or guarantees for other queries.

The new trace driver changes some original-binary collision counts relative to
the earlier traces; compare counters within this run, rather than combining
them across drivers. Separate interpreter traces show:

| Annotated-package counter | Original VM | New VM |
| --- | ---: | ---: |
| Method-cache misses | 740,027 | 260,045 |
| Field-cache misses | 740,080 | 160,076 |
| Direct dispatch rewrites | 160,015 | 60,037 |
| `CHECK_TYPE` executions | 1,460,019 | 1,460,019 |

The return checks remain active; the improvement comes with reduced cache
interference and the new VM layout. The three tables grow from 18 KiB to 72 KiB
on this 64-bit build, adding **54 KiB per VM** (`sizeof(DiamondVm)` grows from
25,800 to 81,096 bytes). They remain direct-mapped and address-sensitive, so this
mitigates the measured collision cliff rather than eliminating collisions.

Fourteen other controls were measured in three interleaved pairs per mode,
pinned to CPU 0, with in-process `DIAMOND_REPEAT` execution. The percentages
below compare median whole-process time divided by the repeat count; negative
means faster. Output matched between binaries in every run.

| Workload | Repeats | Interpreter change | JIT-enabled change |
| --- | ---: | ---: | ---: |
| `dispatch_monomorphic` | 4 | +1.5% | -1.5% |
| `dispatch_polymorphic_no_index` | 4 | +0.6% | -0.9% |
| `dispatch_megamorphic` | 2 | -0.2% | -0.4% |
| `dispatch_reassign_control` | 4 | +0.0% | +0.1% |
| `typed_dispatch` | 2 | -0.3% | -5.6% |
| `int_arithmetic` | 3 | +2.5% | +2.6% |
| `array_ops` | 6 | -1.0% | +0.9% |
| `closures` | 4 | +1.5% | +2.9% |
| `struct_field_access` | 6 | -2.9% | -0.6% |
| `object_hydration` | 12 | -1.6% | -0.1% |
| `hash_ivar_construct` | 16 | +0.1% | -1.3% |
| `iterator_blocks` | 2 | -2.0% | -1.8% |
| `jit_native_collection_reads` | 4 | -2.0% | -2.4% |
| `arel_builder_chain` | 1 | -2.0% | -2.5% |

A longer JIT-enabled recheck (five interleaved pairs, eight repeats for
`int_arithmetic`, twelve for `closures`) found arithmetic **3.5% slower**
and closures **0.9% faster**. Arithmetic is a confirmed small
tradeoff of the retained layout on this build, not evidence of universal runtime
improvement. Hoisting dispatch flags to the front of the VM was also tried and
rejected: it increased the arithmetic slowdown to about 8%. Given the Arel
priority, retain the compact 256-slot mitigation on the shared branch for review,
with both the arithmetic cost and increased per-VM storage visible before merge.

The retained change passed a clean debug build and `make -j6 test` (1,785 tests).
All 22 Arel case files also passed with `DIAMOND_JIT=1`,
`DIAMOND_JIT_THRESHOLD=1`, and `DIAMOND_NO_CACHE=1`, including the negative
String-return-contract case.

To reproduce, save an original release binary before rebuilding this checkout,
then run:

```sh
python3 bench/compare_arel_annotations.py \
  --baseline-ref 55c363ed --annotated-ref d53ab581 \
  --baseline-binary /tmp/diamond-cache64 \
  --candidate-binary build/diamond --output /tmp/arel-cache-comparison.json
```

Keep this comparison in the validation of later Arel annotation batches; a green
correctness suite alone does not establish performance safety.

## Arel write-statement parameter batch

The next retained batch checks `Insert`, `Update`, and `Delete` constructor
collections as `Array`, assignment maps as `Hash | Nil`, and control flags as
`Bool`. Default `nil` assignments, empty arrays, explicit `false`, and false
bound values remain valid. Insert source queries and conflict targets remain
dynamic: conflict targets include arrays and dedicated target nodes, while
source queries and visitors support extension protocols. Collection element
values also remain dynamic.

The initial broader experiment annotated Query construction/copy state,
nullable table/ordering metadata, compound pagination, and function/CTE flags.
Five interleaved samples with the same release binary found about 19% slower
interpreter builder chains and 18% slower JIT-enabled chains. Removing only
Query constructor checks left a 15–17% rendering regression and a 2–6% builder
regression. A further reduced batch still shifted rendering by about 5% and
builders by 3–4%, despite adding no checks inside the render loop. These
experiments were discarded; the hot Query, table, ordering, binary-expression,
and remaining compound/helper parameters are deferred. This result warrants
further investigation of dispatch/cache sensitivity before broadening them.

Retained measurements compare the package at `2be1dcfa` with this batch using
`/tmp/diamond-cache256-compact-release` for both snapshots. Five alternating
samples per package and mode use a single CPU, fresh compilation, no inherited
`DIAMOND_*` settings, `DIAMOND_NO_CACHE=1`, and either an unset `DIAMOND_JIT` or
`DIAMOND_JIT=1`. SQL and parameter output match in every run. Timings include
process startup and compilation; these read-query benchmarks do not quantify
the added checks in write-construction workloads.

| Benchmark | Mode | Baseline median | Annotated median | Change |
| --- | --- | ---: | ---: | ---: |
| Arel render | Interpreter | 1.5631 s | 1.5699 s | +0.4% |
| Arel render | JIT enabled | 1.4784 s | 1.4580 s | −1.4% |
| Arel builder chain | Interpreter | 1.6239 s | 1.6322 s | +0.5% |
| Arel builder chain | JIT enabled | 1.4760 s | 1.4842 s | +0.6% |

The retained read-path differences are small; they do not establish a speedup.
`arel_parameter_contracts.di` checks constructor rejection through dynamic
callers and preservation of defaults and false bind values. The separate
`arel_invalid_flag_parameter.di` checks a direct invalid flag call and its
runtime diagnostic. Existing dialect, write, conflict-target, and extension
cases cover the preserved protocols.

Next: audit the remaining Arel visitor parameters, then investigate the hot
constructor/check and cache sensitivity before attempting another broad batch.

Validation: `make -j6 test` passed all 1,787 tests (1,634 corpus cases;
39 cases skipped without an expectation or `run!` marker). All 24 Arel case
files passed separately with forced JIT (`DIAMOND_JIT=1`,
`DIAMOND_JIT_THRESHOLD=1`, `DIAMOND_NO_CACHE=1`), including both negative
contract fixtures.

## Built-in visitor pagination parameters

SQLite, PostgreSQL, MariaDB, and MySQL pagination renderers now declare both
pagination values as `Int | Nil` and `bind_values` as `Bool`. Their bind array
already had an `Array` annotation. These are the same values supplied by the
query pagination accessors and flags; no node, expression, statement, visitor,
or connection protocol is narrowed.

A direct-call fixture covers all four dialects: omitted pagination, zero limit
and offset, binding enabled by default, explicit unbound rendering, and each
offset-only sentinel. Invalid limit, offset, and binding types received through
a dynamic forwarding function raise `TypeError` before mutating the bind array.
A subclass with an untyped pagination override still accepts its custom value
and can delegate ordinary integers to `super`.

The remaining visitor inputs describe expression or statement protocols and
remain dynamic. Upsert assignment maps also remain dynamic: built-in writes supply
`Hash | Nil`, but these helpers can be reached from custom statements exposing
`structure()`. Their operations (`length`, `key_at`, and indexing) can also be
provided by an adapter. Narrowing that path requires an explicit protocol
decision, rather than assuming every statement uses the built-in constructor.

Performance compares `e344281e` packages with this batch using the same saved
release binary and environment controls as the write-statement batch. Five
interleaved rendering samples and a seven-sample builder repeat produced:

| Benchmark | Mode | Baseline median | Annotated median | Change |
| --- | --- | ---: | ---: | ---: |
| Arel render | Interpreter | 1.4549 s | 1.4706 s | +1.1% |
| Arel render | JIT enabled | 1.3796 s | 1.3918 s | +0.9% |
| Arel builder chain | Interpreter | 1.6288 s | 1.6474 s | +1.1% |
| Arel builder chain | JIT enabled | 1.4414 s | 1.4591 s | +1.2% |

The first five-sample builder pass measured +0.8% interpreter and +3.6% JIT
with upward drift during the JIT samples; the seven-sample repeat above reduced
that difference. Outputs matched in every sample. Treat the retained batch as
adding a small measured cost, not as a performance optimization. Builder loops
do not call pagination renderers, so their change cannot come from executing
the new pagination checks alone.

Next: investigate the deferred hot Query constructor checks and dispatch/cache
sensitivity before adding more constructor annotations. Leave expression and
statement adapters dynamic unless their protocol is deliberately changed.

Validation: `make -j6 test` passed all 1,788 tests (1,635 corpus cases;
39 cases skipped without an expectation or `run!` marker). All 25 Arel case
files also passed with forced JIT and fresh compilation. This batch changes
parameter checks and documentation only; dialect SQL generation is unchanged.
