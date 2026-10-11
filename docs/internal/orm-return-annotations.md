# ORM return annotation audit

Working notes for annotating the Arel and ActiveRecord returns and parameters: what is
fixed, what stays dynamic, and the measurements behind each decision.

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
25,800 to 81,096 bytes). That step was a mitigation: the tables were still
direct-mapped and address-sensitive. The next section replaces the mapping.

## Set-associative site tables

The three tables are now 256 slots organized as 64 sets of 4 ways
(`diamond_cache_slot`, `src/vm_internal.h`). A site's set comes from a
multiplicative hash of its bytecode address, and it may take any way of that
set: the way already tagged with it, else an empty way, else a victim rotated by a
per-VM cursor. Lookup semantics, receiver and shape guards, replacement within a
site's entries, and invalidation are unchanged. `INVOKE_MONO` probes with
`diamond_cache_find`, which never claims a way, and then reuses the cache that
`lookup_method_cached` returns.

Method-cache misses on `bench/arel_render.di` (20,000 renders, interpreter) for
the two package layouts `55c363ed` and the annotated package:

| Cache | Baseline package | Annotated package |
| --- | ---: | ---: |
| 256 slots, direct-mapped | 200,049 | 280,045 |
| 64 slots, 4-way | 299,187 | 310,060 |
| 128 slots, 4-way | 100,054 | 180,057 |
| 256 slots, 4-way | 59 | 59 |

Associativity at 256 slots removes both the collisions and the sensitivity to
layout: the miss count no longer depends on which package is loaded. 64 and 128
slots miss on capacity, because the render path's hot call sites exceed them even
with perfect placement, so the size stays at 256. Timings are within noise of the
direct-mapped 256 build (interleaved, one pinned CPU, five samples per
combination; every ratio between 0.985 and 1.012 except 64-slot Arel at 1.02-1.05),
so this change buys stability rather than speed: miss counts stop depending on
where code lands.

`DIAMOND_INLINE_CACHE_COUNT` must stay a multiple of `DIAMOND_INLINE_CACHE_WAYS`;
`tests/inline_cache_slots.c` checks placement, residency and eviction.

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
one `build/diamond` release binary built with the 256-slot inline cache for both
snapshots. Five alternating
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

## Isolated hot Query constructor investigation

`bench/audit_arel_query_parameters.py` isolates Query constructor annotations,
copy annotations, and both from all other changes. It archives the baseline
package into temporary snapshots, applies only those signatures, and times
inside each benchmark loop. It rotates sample order, compares complete outputs,
and collects tracing separately from timing. It also probes default Array and
Hash allocation with the JIT threshold forced to one. Production Arel
signatures and runtime type guards are unchanged by this investigation.

Three samples per variant/mode with the same release binary, baseline
`771765dc`, fresh compilation, and one CPU produced these loop medians:

| Variant | Render interpreter | Render JIT enabled | Builder interpreter | Builder JIT enabled |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 1.4155 s | 1.3746 s | 1.4824 s | 1.3826 s |
| Constructor only | 1.4153 s (−0.0%) | 1.3304 s (−3.2%) | 1.7653 s (+19.1%) | 1.7041 s (+23.3%) |
| Copy only | 1.4056 s (−0.7%) | 1.3145 s (−4.4%) | 1.6376 s (+10.5%) | 1.4246 s (+3.0%) |
| Both | 1.4018 s (−1.0%) | 1.3412 s (−2.4%) | 1.8297 s (+23.4%) | 1.6680 s (+20.6%) |

The isolated experiment reproduces the large construction regression, without
the rendering cliff seen when several unrelated signatures changed together.
Do not treat the small rendering differences as established improvements.

Interpreter builder traces explain the main cost:

| Variant | CHECK_TYPE executions | Field-cache misses | Method-cache misses |
| --- | ---: | ---: | ---: |
| Baseline | 4,400,002 | 9,400,046 | 15 |
| Constructor only | 19,400,002 | 2,400,054 | 15 |
| Copy only | 8,400,002 | 9,800,048 | 15 |
| Both | 23,400,002 | 2,800,053 | 15 |

The 200,000 builder iterations create 1,000,000 Queries and call `copy` 800,000
times. Fifteen constructor annotations add exactly 15,000,000 guards; five
copy annotations add exactly 4,000,000. The constructor-only variant slows
substantially despite *fewer* field-cache misses and unchanged method-cache
misses. Thus cache deterioration cannot explain that variant's regression;
executing the extra guards is the primary observed cost. Plain `Array` checks
return after matching the collection type: unlike `Array[T]`, they do not walk
elements or attach element constraints (`value_matches_member`, `src/vm.c`).

Cache sensitivity remains real and separate. Cache slots use the low bits of
the bytecode-site address (`site >> 2` modulo 256), so instruction and allocation
layout changes can alter collisions. In the render trace, constructor-only
annotations change field misses from 160,083 to 80,083 and method misses from
420,044 to 240,049, while adding only 45 startup guards. These are layout
changes rather than checks executed in the render loop. They explain why
construction and rendering results need separate measurements and why the
combined variants' costs are not additive.

The JIT does not eliminate the constructor's 15,000,000 interpreted checks.
`Query#initialize` includes default empty-array construction; `DIAMOND_OP_ARRAY`
is unsupported in `src/jit.c`'s opcode switch, making the whole method
ineligible even when callers supply those arguments. A minimal typed
constructor with `values: Array = []` compiles zero functions at threshold one;
the otherwise identical `values: Hash = {}` probe compiles one. The audit tool
records both probe sources and diagnostics. Copy checks mostly leave the
interpreter under JIT (only 245 additional interpreted CHECK_TYPE executions
after warmup); native checks still run through `diamond_jit_check_type` and
are not included in interpreter opcode counts.

Reproduce with a release binary built from the shared annotation branch:

```sh
python3 bench/audit_arel_query_parameters.py \
  --baseline-ref 771765dc --binary build/diamond --runs 5 \
  --output /tmp/arel-query-guards.json
```

The JSON records timing samples, complete diagnostics, baseline commit,
binary path and SHA-256, parameter maps, and default-allocation probes.
Verification exercised all four variants in both modes for both benchmarks,
with identical outputs, plus the separate default-allocation probes. Python
syntax and CLI validation passed. Native code and production package contracts
are unchanged, so the already-passing repository suite was not repeated.

Next: add GC-safe JIT support for `DIAMOND_OP_ARRAY`, then repeat this audit
before adding the hot constructor annotations. Interpreter guard cost remains
a separate issue and must also be addressed or explicitly accepted; JIT
eligibility alone does not make the interpreter regression disappear.

## GC-safe JIT array construction

The shared branch now compiles `DIAMOND_OP_ARRAY`, including default `[]`
arguments, through a rooted allocation trampoline. The interpreter uses the
same allocation/bounds-check helper. Both JIT register-write scans recognize
the new opcode; compilation rejects operands outside the JIT register frame.
No Arel parameter contracts or type guards are removed or broadened.

A five-sample alternating comparison used identical `c9f71779` Arel packages
with the previous and new release binaries, measuring inside each loop. Fresh
compilation, one CPU, sanitized `DIAMOND_*` environment, and output-identity
checks match the isolated audit controls:

| Benchmark | Mode | Previous median | Array-JIT median | Change |
| --- | --- | ---: | ---: | ---: |
| Arel render | Interpreter | 1.4319 s | 1.4106 s | −1.5% |
| Arel render | JIT enabled | 1.3449 s | 1.3326 s | −0.9% |
| Arel builder chain | Interpreter | 1.4831 s | 1.5038 s | +1.4% |
| Arel builder chain | JIT enabled | 1.3240 s | 1.0868 s | −17.9% |

The builder JIT improvement is material; the other differences are small.

Repeating the four-variant guard audit with this release binary (three samples
per variant/mode) shows why the hot annotations remain deferred:

| Variant | Builder interpreter | Builder JIT enabled |
| --- | ---: | ---: |
| Baseline | 1.4927 s | 1.0790 s |
| Constructor only | 1.6994 s (+13.9%) | 1.2465 s (+15.5%) |
| Copy only | 1.5549 s (+4.2%) | 1.1292 s (+4.6%) |
| Both | 1.8055 s (+21.0%) | 1.3136 s (+21.7%) |

Both default-allocation probes now compile one function. The Arel builder
trace compiles 13 functions instead of the previous 10, with no bailout or
hard propagation. Constructor annotations add only 735 interpreted guards
after warmup, rather than 15,000,000: the remaining checks execute natively and
still cost time. Interpreter opcode and cache counts match the prior isolated
experiment, including the 15,000,000 extra constructor guards. This change
closes JIT eligibility, not the remaining guard-overhead problem.

New fixtures check nested object elements, fresh default arrays, explicit
array arguments, full/minor stress GC, and propagation of a failed return
guard after array allocation without replaying a prior hash mutation. The
CLI eligibility probe asserts actual compilation at threshold one.

Next: investigate the cost of primitive and non-generic collection guards in
both execution modes before accepting the deferred Query annotations. Retain
all type-check behavior, including generic constraints and failure diagnostics.

Validation: the debug full suite passed 1,790 tests before the final
post-mutation failure fixture was added. That fixture passed separately on
the release binary, and the subsequent ASan/UBSan full suite passed all
1,791 tests (1,638 corpus cases; 39 skipped without an expectation or `run!`
marker), on its first attempt. All 25 Arel case files passed on the new release
binary with forced JIT and fresh compilation. The four-variant audit completed
in both modes for both benchmarks, with matching outputs and successful
Array/Hash default-allocation probes. A normal debug build was restored after
sanitizer validation.

## Plain built-in JIT guards

Native CHECK_TYPE now accepts plain built-in types by comparing value tags
and object kinds. Int includes Bignum. Mismatches retain the existing C
matcher and error propagation, and any union with a generic or structural
member retains the complete matcher. The interpreter is unchanged.

Five alternating, CPU-pinned release samples compared the previous ARRAY-JIT
binary with the new runtime, using the same committed Arel package. Timing
covered the benchmark loop, with fresh compilation, sanitized DIAMOND
variables, and tracing performed separately. Outputs matched.

| Existing unannotated Query | Previous median | New median | Change |
| --- | ---: | ---: | ---: |
| Builder, interpreter | 1.4912 s | 1.4878 s | -0.2% |
| Builder, JIT | 1.0995 s | 1.0611 s | -3.5% |
| Render, interpreter | 1.4011 s | 1.3969 s | -0.3% |
| Render, JIT | 1.3279 s | 1.3153 s | -1.0% |

The four-variant audit then used three rotated samples per variant, the
previous committed Arel package, and the new runtime. Builder medians:

| Execution mode | Baseline | Constructor | Copy | Both |
| --- | ---: | ---: | ---: | ---: |
| Interpreter | 1.4862 s | 1.6869 s (+13.5%) | 1.5433 s (+3.8%) | 1.7830 s (+20.0%) |
| JIT | 1.0565 s | 1.0912 s (+3.3%) | 1.0693 s (+1.2%) | 1.0708 s (+1.4%) |

The prior runtime's corresponding JIT constructor penalty was +15.5%,
measured in the earlier audit rather than this alternating comparison.
Render changes across variants were small (-1.4% to -2.8% with JIT) and
remain sensitive to instruction/cache layout. These results support the
native guard optimization, but not landing the hot Query annotations:
the interpreter constructor regression remains substantial.

Next: reduce interpreter guard overhead while preserving generic constraints,
then repeat this audit before accepting Query constructor/copy annotations.

Validation: the debug and ASan/UBSan full suites each passed all 1,792 tests
(1,639 corpus cases; 39 skipped), with the sanitizer suite passing on its
first attempt. All 25 Arel files passed on the release binary with forced
JIT. The dedicated guard fixture checks big integers, primitive/object tags,
unions, rejected tags, generic constraint attachment, and plain collection
fallback under forced JIT and major GC stress. It and the existing
failure-after-mutation fixture produced matching output on the old/new
release binaries in both execution modes. The four-variant audit preserved
benchmark output and passed its default-allocation probes.

## Interpreter plain-member matching and Query.copy

The shared runtime matcher now reads members by reference and compares plain
built-in types directly, avoiding a member copy and the deep matcher call.
It still processes union members in source order; generic collections,
Callable, structural/class/native types, and type variables retain their
full matching behavior. Both immediate and promoted integers remain Int.

Five alternating, CPU-pinned release samples on the same unannotated Query
package measured builder changes of -1.5% in interpreter mode and -0.3%
with JIT. Rendering changed -0.5% and +0.6%, respectively. Outputs matched;
these small differences should not be treated as broad performance gains.

Three rotated samples per variant with the new runtime measured:

| Builder mode | Untyped control | Constructor | Copy | Both |
| --- | ---: | ---: | ---: | ---: |
| Interpreter | 1.4975 s | 1.6023 s (+7.0%) | 1.5176 s (+1.3%) | 1.6392 s (+9.5%) |
| JIT | 1.0682 s | 1.0628 s (-0.5%) | 1.0402 s (-2.6%) | 1.0781 s (+0.9%) |

The interpreter constructor penalty is lower than the previous audit's
+13.5%, but remains substantial, so constructor annotations stay deferred.
The copy-only variant's small changes support accepting its five fixed
parameters: predicates/orderings/projections are plain Array, and
limit_value/offset_value are Int | Nil. The production signature exactly
matches the measured copy variant. Dynamic node contents remain unrestricted.
The contract fixture covers nil/zero/promoted integers, plain array mutation,
all five rejected parameter shapes, and unchanged original query state.

The audit now removes only the known audited annotations from temporary
copies before producing its four variants, so HEAD remains a valid input
after some annotations land. It records this untyped-control normalization
in the JSON output and still rejects unexpected signatures.

Next: investigate further constructor guard cost or safe compiler proofs
before accepting the remaining Query constructor annotations.

Validation: the release and ASan/UBSan full suites each passed all 1,794
tests (1,641 corpus cases; 39 skipped), with the sanitizer run passing on
its first attempt. All 26 Arel files passed with forced JIT. The new
Array/Hash union fixture, existing plain-guard fixture, and post-mutation
failure fixture matched before/after in both modes under minor GC stress;
focused contract checks also passed under major GC stress. The audit's
annotation removal was checked against the original untyped source, and
the production copy signature was checked against the measured variant.

## Constructor Bool flags and rejected runtime experiments

Two runtime experiments were discarded. Inlining plain matching into the
interpreter dispatch did not improve the existing Arel workloads, and its
three-sample audit still measured a +9.5% interpreter constructor penalty.
Making opcode diagnostics opt-in improved some Arel timings but regressed
the arithmetic control by +11.0% when the flag was read on each dispatch.
Caching a counter pointer removed that arithmetic regression (-2.6%), but
its Arel interpreter builder comparison was +2.8% and its constructor audit
still cost +6.8%. These experiments did not establish a worthwhile runtime
improvement. No runtime or diagnostic-counter changes were retained.
Plain-before-generic Array/Hash union coverage was retained for future work.

A smaller annotation batch was then tested on the established runtime:
quoted_identifiers, bind_limits, and distinct_value as Bool. Five rotated,
CPU-pinned samples per variant used the previous committed package and the
saved release binary, with tracing separate from timing and outputs equal.

| Builder mode | Untyped control | Bool flags only | Existing copy only | Flags + copy |
| --- | ---: | ---: | ---: | ---: |
| Interpreter | 1.5248 s | 1.5490 s (+1.6%) | 1.5739 s (+3.2%) | 1.5981 s (+4.8%) |
| JIT | 1.0878 s | 1.0749 s (-1.2%) | 1.0945 s (+0.6%) | 1.1088 s (+1.9%) |

Relative to the current copy-annotated branch, the added Bool flags cost
+1.5% in interpreter mode and +1.3% with JIT. This small incremental cost
supports accepting these three annotations, while the other twelve fixed
constructor parameters remain deferred. The production signature exactly
matches the measured flags-plus-copy variant. True, false, the default
false distinct flag, and invalid values for all three flags are covered.

The audit now supports constructor subsets, for example:

```sh
python3 bench/audit_arel_query_parameters.py \
  --baseline-ref b24c3260 --binary /path/to/release-diamond --runs 5 \
  --constructor-parameters quoted_identifiers bind_limits distinct_value \
  --output /tmp/arel-flags-audit.json
```

The control removes all known audited constructor/copy annotations from
its temporary source. The constructor variant adds only the selected
parameters; the copy and both variants retain their previous meanings.
The selected parameter map is recorded in the JSON output.

Next: measure the remaining constructor parameters in smaller groups,
without accepting the full constructor regression or broadening the
runtime based on inconclusive microbenchmarks.

Validation: the full debug suite passed all 1,795 tests (1,642 corpus cases;
39 skipped). All 27 Arel files passed with forced JIT on the established
release runtime. The new flag contracts passed in both modes, the expanded
union-order fixture passed under major GC stress, and the production
constructor signature matched the measured temporary variant exactly.
The five-sample subset audit preserved outputs and passed default-allocation
probes. No C runtime changes remained after the experiments were reverted.

## Incremental table name and alias annotations

The next constructor group is table_name: String and table_alias: String | Nil.
Both getters already declared these return types; the entry annotations now
reject invalid state before it is stored. Nil/default aliases, empty string
aliases, quoted names, and normal aliased/unaliased rendering remain supported.

The audit gained --incremental for this decision. Its control retains the
committed constructor and copy annotations exactly; its only candidate adds
selected, currently untyped constructor parameters. The two variants alternate
order each round. The original four-variant mode retains its earlier behavior.
An explicit constructor selection is required in incremental mode.

Five CPU-pinned release samples per variant, with tracing separate from timing
and equal outputs, measured against commit 51cb7579:

| Workload | Current branch median | Add name/alias median | Change |
| --- | ---: | ---: | ---: |
| Builder, interpreter | 1.5528 s | 1.5788 s | +1.7% |
| Builder, JIT | 1.1067 s | 1.1197 s | +1.2% |
| Render, interpreter | 1.3509 s | 1.3483 s | -0.2% |
| Render, JIT | 1.3220 s | 1.3073 s | -1.1% |

These small incremental costs support adding the pair. The production
signature exactly matches the measured candidate. No runtime changes were
needed. Ten other fixed constructor parameters remain deferred; source_query
continues to use its dynamic protocol.

```sh
python3 bench/audit_arel_query_parameters.py \
  --baseline-ref 51cb7579 --binary /path/to/release-diamond --runs 5 \
  --incremental --constructor-parameters table_name table_alias \
  --output /tmp/arel-identity-audit.json
```

Next: use the incremental audit to measure limit_value and offset_value before
adding their nullable Int annotations.

Validation: the full debug suite passed all 1,796 tests (1,643 corpus cases;
39 skipped), and all 28 Arel files passed with forced JIT on the established
release runtime. The new contracts passed in both modes, including major GC
stress, and the five-sample incremental audit preserved outputs and passed
its default-allocation probes. The measured signature matched production.

Before accepting the cumulative constructor changes for the consolidated PR,
repeat the untyped-control comparison as well: small incremental costs do
not by themselves establish acceptable total overhead.

## Nullable pagination and cumulative constructor audit

limit_value and offset_value now declare Int | Nil at Query constructor
entry, matching the existing getters and visitor pagination contracts.
The checks retain nil, zero, negative and promoted integers without adding
numeric policy. Bound LIMIT/OFFSET parameters and offset-only SQLite
rendering are covered, along with rejection of float/string/Bool/Array
values forwarded dynamically into the constructor.

Five alternating, CPU-pinned samples of the incremental candidate against
commit 6746a207 measured:

| Builder mode | Current branch median | Add pagination median | Change |
| --- | ---: | ---: | ---: |
| Interpreter | 1.5889 s | 1.5835 s | -0.3% |
| JIT | 1.0805 s | 1.0677 s | -1.2% |

Rendering was essentially unchanged. These small changes show no incremental
slowdown, rather than establishing a general performance improvement.

A separate five-sample, four-variant audit measured the cumulative seven
constructor annotations (names/alias, three Bool flags, nullable pagination)
and five existing copy annotations against an untyped Query-parameter control:

| Builder mode | Untyped control | Constructor only | Copy only | Both |
| --- | ---: | ---: | ---: | ---: |
| Interpreter | 1.5007 s | 1.5556 s (+3.7%) | 1.5174 s (+1.1%) | 1.5897 s (+5.9%) |
| JIT | 1.0692 s | 1.0645 s (-0.4%) | 1.0640 s (-0.5%) | 1.1034 s (+3.2%) |

Both production annotations and the cumulative measured candidate match
exactly. Outputs were equal, with tracing separate from timing. Combined
rendering changed -1.2% in both modes. The cumulative builder cost is material:
small incremental groups do not eliminate it. The pagination pair is accepted
because it did not worsen the current branch; the remaining eight collection
constructor annotations are deferred while this total cost is investigated.
source_query retains its dynamic protocol.

Reproduce the incremental audit with --baseline-ref 6746a207 --incremental
--constructor-parameters limit_value offset_value. For the cumulative audit,
omit --incremental and select table_name table_alias quoted_identifiers
bind_limits distinct_value limit_value offset_value. Both used --runs 5 and
the established release runtime.

Next: investigate the cumulative builder overhead before adding the remaining
collection constructor annotations or declaring the consolidated PR ready.

Validation: all 1,797 debug tests passed (1,644 corpus cases; 39 skipped),
and all 29 Arel case files passed with forced JIT on the established release
runtime. Pagination contracts also passed with major GC stress in both modes.

The preceding CI run exposed two test portability errors: the interpreter
fixture imported a JIT-only case removed on unsupported platforms, and the
native array compilation probe ran on musl despite the runtime disabling JIT
there. Shared guard definitions now live in tests/support, and the probe
matches the runtime's glibc x86-64 gate. The interpreter fixture passed in an
isolated tree with all JIT cases absent, and the full suite passed after both
fixes. Remote platform verification requires the new CI run.

## Cumulative guard workload confirmation

A seven-sample cumulative audit of committed pagination head b625934d used
the same established release runtime, CPU pinning, alternating variant order,
and seven-constructor/five-copy selection as the previous audit. The
interpreter builder medians were 1.4906 s untyped, 1.5494 s constructor-only
(+3.9%), 1.5220 s copy-only (+2.1%), and 1.5744 s combined (+5.6%). The combined
range (1.5472–1.6069 s) did not overlap the control range (1.4634–1.5259 s).
This reproduces the earlier +5.9% combined result.

The JIT-enabled combined median was 1.0676 s versus 1.0536 s (+1.3%), rather
than the earlier +3.2%. Its ranges overlap (combined 1.0507–1.1311 s; control
1.0330–1.1090 s), so a stable native slowdown of exactly 3% is not established.
Rendering changed -1.2% interpreter and -3.6% JIT; layout-sensitive results
remain separate from the repeated construction guard cost. Outputs matched
throughout, and both default-allocation probes passed.

| Builder variant | Interpreted CHECK_TYPE executions | Field-cache misses |
| --- | ---: | ---: |
| Untyped Query parameter control | 4,400,002 | 9,400,046 |
| Seven constructor parameters | 11,400,002 | 2,400,052 |
| Five copy parameters | 8,400,002 | 9,800,048 |
| Combined | 15,400,002 | 2,400,052 |

The extra 11 million checks follow directly from one million constructor
entries and 800,000 copy entries. Constructor entry still checks parameters
after a typed copy caller forwards them. Lower field misses and unchanged
method misses (15) argue against cache deterioration as the cause of this
interpreter slowdown. With JIT enabled, all variants compile 13 functions
with zero bailouts or hard propagations; only 588 additional interpreted
checks remain in the combined variant. Interpreter guard dispatch and native
guard execution therefore need separate optimization evidence.

The audit now includes sample ranges and named diagnostic counts alongside
raw traces, deriving CHECK_TYPE's opcode from the repository header. Tracing
remains outside measured samples. The decoder was checked against all 16
existing benchmark/mode/variant traces and rejects missing diagnostics.
The updated audit also passed end to end with reduced loops in all four
variants and both modes, including output equality, JSON range/count fields,
and default-allocation probes. Reduced-loop timings are validation only.

Next: measure a safe way to reduce repeated parameter guard work, preserving
dynamic-call errors and generic constraint attachment. Do not remove entry
checks merely because this benchmark's callers already provide valid values.

## Rejected consecutive-guard dispatch experiment

An interpreter-only candidate handled adjacent CHECK_TYPE instructions in
one switch case. After a successful check, it consumed the next guard only
when resource limits were inactive and no signal was pending. It retained
the existing matcher, declaration order, error instruction offsets, and
per-opcode counters; other instructions still returned to normal dispatch.
This reduced dispatch boundaries without eliding any parameter checks or
changing type-set metadata. The candidate is discarded in full.

Five CPU-pinned, alternating baseline/candidate runs used identical current
Arel signatures and timed only each benchmark loop. The baseline was the
established release runtime; the candidate used the same release/LTO build
configuration. All DIAMOND environment variables were cleared before enabling
the requested mode, and caching was disabled. Tracing ran separately.

| Workload | Baseline median | Candidate median | Change |
| --- | ---: | ---: | ---: |
| Builder, interpreter | 1.5853 s | 1.5667 s | -1.2% |
| Builder, JIT enabled | 1.0750 s | 1.0907 s | +1.5% |
| Rendering, interpreter | 1.4324 s | 1.4457 s | +0.9% |
| Rendering, JIT enabled | 1.3239 s | 1.3436 s | +1.5% |
| Arithmetic, interpreter | 0.3054 s | 0.3191 s | +4.5% |
| Arithmetic, JIT enabled | 0.3032 s | 0.3229 s | +6.5% |

Builder sample ranges overlapped in both modes; the modest interpreter
median gain does not establish a reliable improvement. Arithmetic calls
its function only once, so enabling JIT with the default threshold does not
compile that function; both arithmetic rows measure interpreter execution.
The general interpreter slowdown outweighs the uncertain builder gain.
Outputs matched for all samples. All four Arel diagnostic summaries were
identical across binaries, including interpreted guard counts, method/field
cache counts, and JIT compilation/bailout/propagation counts. The existing
interpreter guard fixture returned 40 on the candidate. No candidate runtime
change is retained, and these checks do not constitute full runtime validation.
After discarding the patch, the established runtime was rebuilt in debug mode.
Its guard fixture passed normally, with a configured instruction budget, and
with forced JIT plus major GC stress. The only retained change is this record.

Next: investigate reducing matcher cost through prepared plain-type metadata,
rather than changing general dispatch layout. Preserve ordered generic-union
behavior and measure arithmetic/rendering controls alongside the builder.

## Rejected prepared plain-type masks

Two candidates prepared a builtin membership mask for wholly plain type sets
at compilation completion, function copying (including cache loading and
thread/template copies), and ProgramBuilder type-set creation. Execution
never mutated shared sets. A zero mask retained ordered structural matching
for any generic, callable, class, interface, native, or type-variable member.
Plain matching classified the value once, including Bignum as Int, then
tested membership. The cache format version was advanced for the changed
serialized layout. Both candidates and the format change were discarded.

The first candidate added a uint16_t field, expanding DiamondTypeSet from
354 to 356 bytes. The second packed count, inferred status, and mask into
four, one, and ten bits respectively, retaining the baseline's measured
354-byte size and two-byte alignment. Separate five-sample, CPU-pinned,
alternating comparisons used the established release runtime as control,
identical production annotations, loop-only timing, equal outputs, fresh
compilation, sanitized environments, and tracing outside measured samples.

| Workload | Larger mask variant | Same-size mask variant |
| --- | ---: | ---: |
| Builder, interpreter | +5.4% | +2.5% |
| Builder, JIT enabled | +5.2% | +2.2% |
| Rendering, interpreter | +8.2% | +0.3% |
| Rendering, JIT enabled | +8.1% | +0.9% |
| Arithmetic, interpreter | -0.3% | +1.6% |
| Arithmetic, JIT enabled | -0.2% | +2.0% |

The larger variant shifted builder method-cache misses from 15 to 400,013
and field misses from 2,400,052 to 2,000,052. Rendering method misses rose
from 120,053 to 280,046 and field misses from 120,079 to 240,096. Its changed
allocation layout introduces cache effects, so its slowdown cannot be
attributed solely to matcher execution. The same-size variant restored all
four Arel diagnostic summaries exactly to baseline, yet did not improve
builder time. Neither variant changes JIT compilation coverage or bailout
counts. Arithmetic remains interpreted in both modes at the default threshold.

Both candidates passed the existing interpreter guard fixture with major GC
stress (result 40). Performance audits checked all sample outputs and guard/
JIT counters. Full runtime validation was not performed because neither
candidate met the performance gate. The runtime, compiler, ProgramBuilder,
type-set layout, and cache format are restored to the established versions.
The restored debug build passed the guard fixture normally, with an instruction
budget, and with forced JIT plus major GC stress. CI run 38108719326 completed
successfully on commit 0317f7f4, verifying the preceding portability fixes.

Next: profile the established matcher and guard dispatch before another
optimization. The retained Query annotation overhead remains unresolved;
the remaining collection constructor annotations are still deferred.

## Loop-only hardware profiles

The reproducible tool bench/profile_arel_query_guards.py archives the selected
commit's Arel package, removes only Query constructor/copy annotations for
its control, and runs one million builder iterations in the interpreter.
It pins a CPU, clears inherited DIAMOND settings, disables caching, and records
cycles:u at 1,999 Hz with DWARF call stacks. Driver timestamps and perf use
CLOCK_MONOTONIC; reports restrict samples to the loop's actual start/finish
interval, excluding compilation and package loading. Raw data, symbol/source
reports, binary hash, inputs, timestamps, and separately collected diagnostic
counts are retained in the requested output directory. Profiling elapsed
times are not treated as uninstrumented benchmark measurements.

Two profile pairs on the established optimized runtime with debug symbols
identified the same costs. The second pair contained approximately 15,000
untyped and 16,000 typed loop samples, with zero lost samples:

| Symbol self-cycle share | Untyped control | Production Query annotations |
| --- | ---: | ---: |
| run_chunk | 52.51% | 54.42% |
| value_matches_set.constprop.0 | 1.83% | 4.85% |

The first pair measured matcher shares of 1.80% and 4.72%. These percentages
are sampled relative shares, not exact costs or additive benchmark deltas.
Diagnostics confirm 22,000,002 versus 77,000,002 interpreted checks, 15 method
misses in both variants, and zero compiled functions/bailouts/propagations.
Production annotations reduce field misses from 47,000,046 to 12,000,052.

Source samples concentrate in the union/member loop and plain member switch.
The optimized matcher reserves 72 stack bytes and saves six callee-saved
registers before plain checks, keeping loop state available for structural
matching calls. That suggests a focused experiment: move the structural
remainder into a separate helper reached only when a member needs it,
retaining declaration order, attachment effects, and error behavior. Do not
change general dispatch or type-set layout to test this hypothesis.

Reproduce with an optimized symbol-bearing build:

```sh
make -j6 release CFLAGS_RELEASE='-O3 -DNDEBUG -march=native -g'
python3 bench/profile_arel_query_guards.py --binary build/diamond \
  --output-dir /tmp/arel-guard-profile-new
```

Validation: the tool passed a reduced-loop end-to-end run and a full one-million
iteration run, including report generation, output checks, JSON metadata and
separate trace decoding. Output directories must be new to preserve evidence.

## Separate structural matcher remainder

The retained candidate gives plain-member matching its own small function.
At the first member requiring structural matching, it tail-calls a separate
non-inlined helper starting at that member. The helper retains the original
ordered matching loop, including generic constraint attachment and later
plain fallbacks. Earlier plain members have no attachment effects and are
not revisited. No dispatch, compiler, type-set layout, cache format, or native
plain-guard emission changes are involved.

The optimized plain matcher no longer sets up the 72-byte stack frame or
saves six callee-saved registers on entry. Five alternating, CPU-pinned
baseline/candidate samples measured the following loop-only changes on the
production annotation signature:

| Workload | Baseline median | Candidate median | Change |
| --- | ---: | ---: | ---: |
| Builder, interpreter | 1.5969 s | 1.5631 s | -2.1% |
| Builder, JIT enabled | 1.0976 s | 1.0907 s | -0.6% |
| Rendering, interpreter | 1.4281 s | 1.4207 s | -0.5% |
| Rendering, JIT enabled | 1.3371 s | 1.3387 s | +0.1% |
| Arithmetic, interpreter | 0.3035 s | 0.3029 s | -0.2% |
| Arithmetic, JIT enabled | 0.3028 s | 0.3018 s | -0.3% |

A separate nine-sample builder repeat measured -1.1% interpreter (1.5993 s
to 1.5817 s) and -0.3% JIT enabled (1.0931 s to 1.0898 s). Sample ranges
overlap, so do not claim a precise 2% speedup. Outputs and all four Arel
diagnostic summaries match exactly between binaries in both audits, including
opcode, cache, compilation, bailout and hard-propagation counts. Arithmetic
remains interpreted at the default JIT threshold.

Two alternating hardware counter comparisons of one million builder
iterations measured 89.1204/89.1204 billion retired instructions on the
baseline and 87.9544/87.9548 billion on the candidate, about 1.3% fewer in
both runs. These counters cover the whole process, including compilation;
they support the reduced instruction-work hypothesis without attributing
an exact loop-only cycle gain. The small repeated builder gains and neutral
rendering/arithmetic controls justify retaining the narrower matcher split.

The new ordered-fallback fixture exercises a successful plain prefix,
promoted Int values, generic Array/Hash attachment after a failed plain prefix,
heterogeneous plain fallbacks, plain-first unions, and invalid dynamically
forwarded values. It passed with major GC stress in interpreter and forced
JIT modes. The full release suite passed all 1,798 tests (1,645 corpus cases;
39 skipped). The full ASan/UBSan suite also passed all 1,798 tests, all 29 Arel
case files passed with forced JIT on the sanitizer build, and the new ordered
fixture passed with forced JIT and major GC stress under ASan/UBSan. Focused
sanitizer checks used the same execution permissions as the full suite because
LeakSanitizer cannot inspect threads in the restricted ptrace environment.

Next: repeat the cumulative constructor/copy audit on this runtime before
adding the remaining collection constructor annotations. This local matcher
improvement does not establish that the earlier cumulative cost is resolved.
