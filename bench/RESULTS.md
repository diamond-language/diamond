# Benchmark suite: expanded, fresh run (2026-08-31)

A fresh `bash bench/run.sh quicken` run after expanding the suite from
14 to 18 microbenchmarks. This is a snapshot, not a replacement for
`BASELINE.md` — that file is a fixed historical anchor for the
`jit-experimentation` branch's own before/after comparisons and is
left untouched. This doc exists to record what's now covered and give
a current reference point for the expanded suite going forward.

## Environment

- CPU: 12 logical cores (see `BASELINE.md` for the original machine's
  physical/logical split — same machine)
- `gcc (GCC) 16.2.1`, built via `make release` (`-O3 -DNDEBUG -march=native`)
- Commit `a6b91f84`
- Methodology unchanged from `BASELINE.md`: `DIAMOND_REPEAT=N` re-runs
  the already-compiled chunk N times in-process (interpreter
  throughput, not startup/compile cost); two passes (default,
  `DIAMOND_QUICKEN=1`); one untimed `DIAMOND_TRACE_OPCODES=1` run per
  benchmark for the opcode breakdown.

## What's new in this pass

Five new benchmarks, each targeting a hot-path category the original
14 didn't touch at all:

- **`exception_handling.di`** — `begin`/`rescue`/`ensure` in a loop
  that both raises-and-catches *and* runs an ensure block every
  iteration (the common "expected, handled" case, not a rare uncaught
  crash). First and only benchmark to exercise `PUSH_RESCUE`/
  `POP_RESCUE`/`PUSH_ENSURE`/`RUN_ENSURE`/`END_ENSURE`.
- **`iterator_blocks.di`** — `Array#each`/`#map`/`#select` with a
  block, the overwhelmingly common way real Diamond code iterates
  (every other benchmark in the suite uses a hand-written `while`
  loop instead). First to exercise block/closure-call machinery
  (`GET_CAPTURE_CELL`, `BOX_LOCAL`, `CALL_CLOSURE`) as its dominant
  cost rather than a side detail.
- **`range_iteration.di`** — `Range#each`, a distinct code path from
  both a hand-written loop and iterating a materialized `Array` (no
  backing `Array` ever gets allocated).
- **`case_pattern_matching.di`** — `case`/`when` dispatch over a plain
  `Int` scrutinee. First to exercise `CASE_MATCH` at all.
- **`typed_dispatch.di`** — a typed instance method (checked
  parameter/return type) plus a generic method called with an
  explicit type argument every iteration. **Closes a gap
  `BASELINE.md`'s own "Coverage gaps" section explicitly flagged**:
  "No benchmark exercises `CHECK_TYPE`/`INVOKE_TYPED`/`CALL_TYPED`... 
  none of the current benchmarks show these opcodes in their top-12."
  Confirmed via this run's own opcode trace: `CHECK_TYPE` (3.0M) and
  `INVOKE_TYPED` (1.0M) both appear prominently.

`BASELINE.md`'s other listed gap — isolating GC pressure — is not
addressed here; it's already covered, more rigorously than a short
microbenchmark could, by `bench/gc_churn/`'s dedicated
`DIAMOND_TRACE_GC`-based live-set/churn sweep methodology (added
2026-08-28, after `BASELINE.md` was written). Not duplicated. The
third listed gap, `dispatch_polymorphic` vs `dispatch_monomorphic`'s
extra-indexing confound, is unaddressed — out of scope for this pass.

Also fixed while touching this file: `bench/run.sh`'s
`OPCODE_NAMES` array (used to translate `DIAMOND_TRACE_OPCODES`'
numeric indices back to opcode names) was a hand-maintained mirror of
`src/vm.h`'s `DiamondOpCode` enum, last updated 2026-08-20 — stale
enough to have the wrong name for every opcode from index 2 onward
(missing `SYMBOL`, plus ~80 opcodes added since). This is the exact
same silent-desync failure mode as the `selfhost/parser.di` opcode
mirror bug fixed the same day (see project memory) — rather than
patch the hardcoded list, `run.sh` now derives `OPCODE_NAMES` from
`src/vm.h` at run time (`sed`/`grep` over the enum block), so it can't
go stale again.

## Results

| Benchmark | Iterations (repeat) | Default (per-iter) | Quicken=1 (per-iter) |
|---|---:|---:|---:|
| `array_ops` | 30 | 0.74244s | 0.75810s |
| `case_pattern_matching` *(new)* | 7 | 0.44099s | 0.43863s |
| `closures` | 25 | 0.12730s | 0.12866s |
| `dispatch_megamorphic` | 12 | 0.57140s | 0.57230s |
| `dispatch_monomorphic` | 15 | 0.28109s | 0.27646s |
| `dispatch_polymorphic` | 12 | 0.38296s | 0.38137s |
| `dispatch_polymorphic_no_index` | 12 | 0.49439s | 0.49503s |
| `dispatch_reassign_control` | 15 | 0.48874s | 0.48634s |
| `exception_handling` *(new)* | 12 | 0.20142s | 0.20233s |
| `fiber_switch` | 10 | 0.44705s | 0.44917s |
| `fibonacci` | 15 | 0.27580s | 0.29220s |
| `hash_ops` | 100 | 0.00110s | 0.00113s |
| `int_arithmetic` | 15 | 0.53523s | 0.52901s |
| `int_arithmetic_dynamic` | 10 | 0.79259s | 0.83197s |
| `iterator_blocks` *(new)* | 5 | 0.69107s | 0.68980s |
| `range_iteration` *(new)* | 20 | 0.12288s | 0.12786s |
| `string_ops` | 40 | 0.15661s | 0.15292s |
| `typed_dispatch` *(new)* | 6 | 0.47681s | 0.46870s |

(One run each, not averaged — treat single-digit-percent deltas as
noise, matching `BASELINE.md`'s own stated methodology. Full raw
output including all 18 opcode breakdowns is reproducible on demand
via `bash bench/run.sh quicken`, not duplicated here.)

## Findings from the new benchmarks

**`DIAMOND_QUICKEN=1` still shows no measurable benefit**, consistent
with `BASELINE.md`'s original finding #1 — every new benchmark's
default-vs-quicken delta here is within noise (≤2%), including
`typed_dispatch` and `exception_handling`, neither of which existed
when that finding was first written.

**Block/closure iteration has real, measurable overhead over a
hand-written loop.** `iterator_blocks.di`'s opcode trace is dominated
by `GET_CAPTURE_CELL` (7.2M) ahead of even `MOVE` (4.1M) — the block
capturing `total`/`values` by reference costs more dispatch than the
loop body's own arithmetic. `range_iteration.di` (1M `Range#each`
iterations, a much smaller per-iteration body) shows `GET_CAPTURE_CELL`,
`BOX_LOCAL`, and `MOVE` all landing within 2% of each other (~2.0-2.04M
each) — comparable in magnitude to the loop's own `ADD` (2.005M), not
dwarfed by it the way a hand-written `while` loop's captureless
arithmetic would be. Neither benchmark's block does anything
nontrivial with its capture — this cost is inherent to the capture
mechanism itself, not incidental to what these particular blocks
happen to do.

**Exception handling's steady-state cost is modest relative to other
suite benchmarks.** `exception_handling.di`'s top opcodes are
`CONSTANT`/`MOVE`/`JUMP` ahead of any of `PUSH_RESCUE`/`PUSH_ENSURE`/
`RUN_ENSURE`/`END_ENSURE`/`POP_RESCUE`. Confirmed via `--dump-bytecode`
that each of the source's two nesting levels (the outer `begin...
ensure` and the inner `begin...rescue`) installs its own
`PUSH_ENSURE`+`PUSH_RESCUE` pair — even the inner block, which has no
explicit `ensure` clause of its own — so these opcodes run twice per
loop iteration (~1.0M for 500K iterations), not once; not a "hidden"
multiple, just the direct, expected cost of two nested handler scopes.
Per-iteration cost (~403ns per full nested begin/rescue/ensure cycle,
500K internal iterations) is still cheaper than both `fiber_switch`'s
~894ns/iter and `array_ops`'s ~742ns/iter — exception handling in the
"expected, handled" case is not the expensive path some might assume
it to be, even accounting for the two-level nesting cost above.

**`when a..b` allocates a fresh Range object on every evaluation, not
just once.** `case_pattern_matching.di`'s ~441ns/iter (1M internal
iterations) is *higher* than both `dispatch_polymorphic`'s ~191ns/iter
and `dispatch_polymorphic_no_index`'s ~247ns/iter (2M internal
iterations each) despite `case_pattern_matching.di` doing less apparent work per call
(one function call plus one pattern match, vs. `dispatch_polymorphic_
no_index`'s if/elsif receiver selection plus one polymorphic method
call). The opcode trace's `CHECK_TYPE`/`SET_IVAR` (each
~2.0M, exactly equal) turn out not to be from `case`/`when`'s own
dispatch machinery at all — confirmed via `--dump-bytecode`, they're
`Range#initialize` running to completion (3 typed parameters checked,
3 fields assigned) every single time execution reaches the `when
4..10` arm, because the compiler lowers that pattern to a real `NEW
Range, 4, 10, false` call rather than treating it as a cacheable
constant the way `when 1, 2, 3`'s bare Int literals are (those compile
to a plain `CONSTANT` + `CASE_MATCH`, no allocation at all). Since a
Range literal's bounds are fixed at compile time here, constructing it
once and reusing the same instance across every `case` evaluation
would be a real, concrete optimization — worth a closer look if
`case`/`when` performance becomes an actual target later.

**Typed dispatch's overhead over untyped dispatch is small but
present.** `typed_dispatch.di` (~477ns/iter, 1M internal iterations,
one typed call + one generic call each) vs. `dispatch_monomorphic.di`
(~141ns/iter, 2M internal iterations, one untyped call each) — an
average ~239ns/call for `typed_dispatch.di` vs. ~141ns/call for plain
monomorphic dispatch, roughly 70% higher, not free. `CHECK_TYPE` runs
3 times per
iteration, confirmed via `--dump-bytecode`: once for `process`'s `Int`
parameter (its `Int` return needs no check — `value * 2` is already
provably `Int`, matching `BASELINE.md`'s own finding #1 about static
type inference eliding runtime checks where possible), and twice for
`wrap` (once for its `T`-bound parameter, once for its `Array[T]`
return, since a generic return type can't be statically proven the
same way). Each check is a real, measured cost, not free at runtime.

## Addition: `object_hydration.di` (2026-09-12)

Added while gathering representative benchmark evidence ahead of planning
real JIT work (`docs/roadmap.md`'s "Native-code execution" section requires
this before any codegen). Models `ActiveRecord::Repository`-style row
hydration — skindicate's own `User` class (a real application, not a
synthetic worst case) is the template: a class with typed `attr_accessor`
fields and an `initialize(attributes: Hash)` that pulls values out of a
Hash "row" with simple conditional defaulting, called once per fetched
database row. No real database involved (deliberately, for a fast,
reproducible microbenchmark) — 100 batches × 100 hydrations of a 4-field
class from a freshly-built Hash each time.

Same environment/methodology as above (`gcc`, `make release`
`-march=native`, `DIAMOND_REPEAT`-based in-process re-execution). Verified
stable across 5 independent runs before trusting the number (21.16–21.53ms
per 100-hydration iteration, <2% spread):

| Benchmark | Iterations (repeat) | Default (per-iter) |
|---|---:|---:|
| `object_hydration` (new) | 120 | ~0.0213s |

Opcode trace (`DIAMOND_TRACE_OPCODES=1`, top opcodes by count over the full
run): `MOVE` 140,202; `STRING` 130,000; `INDEX_GET` 50,000; `ADD` 40,000;
`SET_IVAR` 40,000 (the 4 typed field writes × 10,000 hydrations); `CONSTANT`
20,403; `CHECK_TYPE` 20,000 (typed `attr_accessor` writes being checked).
No `INVOKE`/`INVOKE_MONO` in the top 15 — this workload's cost is
dominated by object construction and typed field assignment, not method
dispatch, distinct from `typed_dispatch.di` above.

**Motivating context**: this shape was found to dominate a real
application's (skindicate) front-page response time this session — a
direct SQLite timing of the exact same query took ~8ms against a real
8,000+ row table, while hydrating the ~100 resulting rows into model
objects (`ActiveRecord::Repository`, batched, no N+1) took ~25ms. The
query was never the bottleneck; object construction was, and it's already
linear (confirmed by profiling at n=10/25/50/100/200) with no algorithmic
fix available at the query or application level. This benchmark exists so
that claim has a reproducible, application-independent number behind it,
per `docs/roadmap.md`'s explicit requirement not to pursue JIT work
"without representative profiling evidence."

## Phase 2 baseline JIT: first real measurement (2026-09-12)

`DIAMOND_JIT=1` (see `docs/internal/jit-design.md`) against `bench/
int_arithmetic.di`, same environment as above (`make release`,
`-march=native`, one binary, `DIAMOND_JIT` toggled purely via env var so
this is a true same-binary A/B, not a rebuild comparison). Interpreted
figures here use the **release** (`-O3`) build specifically -- an earlier
debug-build (`-O0`) comparison during development showed a much larger
apparent win (~22x) purely because the unoptimized interpreter baseline
itself was artificially slow; the release-build number below is the
honest one.

5 alternating rounds, single call each (`DIAMOND_JIT_THRESHOLD=1`, so the
very first call compiles and every call in a `DIAMOND_REPEAT` run after
the first amortizes that one-time cost):

| | per-iteration (repeat=15) |
|---|---:|
| Interpreted | ~0.530s |
| JIT | ~0.180s |

**~2.95x speedup**, consistent to within 1% across all 5 rounds (both
single-call and `DIAMOND_REPEAT=15` measurements agree closely, so the
one-time compile cost is not a meaningfully confounding factor here).
Verified correct against the interpreted baseline's own output
(`12499992500000`) in every configuration tested, including the two
runtime bailout paths this narrow slice deliberately doesn't handle
inline (integer overflow promoting to bignum, confirmed via a dedicated
overflow test producing the identical bignum-promoted result; truncating
division, confirmed via a dedicated division test) -- both fall back
mid-function to a full, correct interpreted re-run of that same function,
exactly as designed.

This is the first real native-code-generation result for Diamond (the
`jit-experimentation` branch predating this was interpreter-loop tuning,
not codegen -- see that section above). Scope is deliberately narrow: only
zero-argument, non-generic, non-method top-level functions built entirely
from register moves/constants/`_INT` arithmetic/comparisons/jumps/return
-- see `src/jit.c`'s own header comment and `docs/internal/jit-design.md`
for exactly what is and isn't covered, and why (a call-free,
allocation-free, exception-free subset needs none of the general frame/
GC-root contract the design doc lays out for a future call-compiling
tier). `bench/object_hydration.di` and `bench/typed_dispatch.di` both
correctly report 0 compiled functions under `DIAMOND_JIT=1` -- neither
fits this narrow subset yet -- and produce output identical to the
non-JIT baseline, confirming the bailout-at-compile-time gate is safe for
code it was never meant to touch.

## Phase 2b: arguments/self plus Hash-read/ivar-write trampolines (2026-09-12)

Extended the same day, without needing the general frame/GC-root contract
either -- see `docs/internal/jit-design.md`'s own updated status note for
the full design. New: any arity (including instance methods, `self` is
just register 0), `EQUAL`/`NOT_EQUAL` on primitives, and `SET_IVAR`/
`INDEX_GET`(Hash)/`CHECK_TYPE` via three narrow C trampolines
(`diamond_jit_set_ivar`/`diamond_jit_hash_get`/`diamond_jit_check_type` in
`vm.c`) rather than hand-rolled machine code -- each individually
confirmed allocation-free by reading its own call chain. The dispatch
check is now wired into `invoke_resolved_method_helper` too (covers
`NEW`/`SUPER`/`INVOKE_TYPED`'s ordinary dispatch), not just
`DIAMOND_OP_CALL`.

**The actual motivating target still isn't reachable.**
`bench/object_hydration.di`'s own `HydratedUser#initialize` -- modeling
skindicate's real row hydration -- still doesn't compile:
`attributes["email"]`-style Hash access compiles a fresh `DIAMOND_OP_
STRING` construction for the literal key on every call, and string
construction is a real allocation. This isn't specific to this benchmark
-- string-literal Hash keys are pervasive in ordinary Diamond code -- so
it's a real, general gap, not an edge case to special-case around.
Confirmed directly: deploying this build to skindicate would show 0
compiled functions on its real controller/model code and no measurable
difference on `/`, exactly like Phase 2 did. `object_hydration.di`'s own
comment now documents this in detail.

**What does work, measured honestly:** `bench/hash_ivar_construct.di` (new)
isolates the same `SET_IVAR`/`INDEX_GET`/`CHECK_TYPE`/self/argument
machinery with the Hash key passed as a parameter instead of a literal,
sidestepping the string-construction gap -- `Box#initialize` compiles and
produces correct output. Its own end-to-end driver-loop timing shows *no*
measurable difference (interpreted and JIT both ~0.0236s for the full
100×100 loop, `DIAMOND_JIT_THRESHOLD=1`), because the surrounding loop's
own Hash-literal construction and `Box.new`'s own allocation (both
necessarily still interpreted) dominate the total time far more than the
now-cheap `initialize` call itself.

Isolating `initialize` specifically (2,000,000 calls against one already-
constructed, reused Hash and key -- no fresh allocation per call in the
timed loop) shows the real, honest effect size:

| | wall time (2,000,000 calls) |
|---|---:|
| Interpreted | ~1.91-1.98s (3 rounds) |
| JIT | ~1.85-1.87s (3 rounds) |

**~4-7% faster, not a multiple.** Consistent and reproducible across
rounds, but genuinely modest compared to Phase 2's ~2.95x on pure
arithmetic -- and the reason why is itself the finding: `initialize`'s own
compiled body does almost the same work either way, since `CHECK_TYPE`/
`INDEX_GET`/`SET_IVAR` all immediately call into the *same* C trampoline
functions the interpreter's own opcode handlers would call. The JIT only
saves bytecode fetch/decode/dispatch overhead for those three opcodes, not
the underlying work -- unlike Phase 2's pure-native arithmetic, which
replaced interpretation with real, dependency-free machine instructions
end to end. A future phase that can also compile calls/allocation
natively (or find a way to shrink the per-trampoline-call overhead itself)
would need to reduce trampoline-call weight specifically to see a larger
win here, not just widen opcode coverage further.

Full test suite (1329 cases: the existing 1327 plus 2 new regression
cases, `jit_hash_ivar_construct`/`jit_hash_ivar_construct_stress_gc` --
the latter run under `DIAMOND_STRESS_GC=1`, forcing a collection on every
allocation, specifically to stress-test the "no GC frame needed" claim
above) passes unchanged under both debug and ASan/UBSan sanitizer builds.

## Phase 2c: allocation-capable trampoline + real frame/GC-root contract (2026-09-12)

Closed the gap Phase 2b identified: added `STRING` opcode support via a
real `DiamondFrame`-publishing prologue/epilogue (built, not just
designed -- see `docs/internal/jit-design.md`'s own Phase 2c status note
for the trampolines and the dry-run-compile-pass mechanism that decides,
per function, whether it's needed at all). `bench/object_hydration.di`'s
own `HydratedUser#initialize` -- the actual motivating target since
Phase 0 -- now compiles for the first time.

Same environment as above (`make release`, `-march=native`, one binary,
`DIAMOND_JIT` toggled via env var). 5 alternating rounds, `DIAMOND_REPEAT`
matching `bench/run.sh`'s own table:

| benchmark | repeat | interpreted (per round) | JIT (per round) |
|---|---:|---:|---:|
| `object_hydration.di` | 120 | ~2.34-2.41s | ~2.28-2.33s |
| `hash_ivar_construct.di` | 250 | ~2.70-2.78s | ~2.59-2.61s |
| `int_arithmetic.di` (regression check) | 15 | ~7.6-8.1s | ~2.65-2.69s |

**`object_hydration.di`: ~2-3% faster end to end.** Modest, and consistent
with Phase 2b's own finding: `initialize`'s compiled body still calls the
same C trampolines (`diamond_jit_hash_get`/`diamond_jit_set_ivar`/
`diamond_jit_check_type`/`diamond_jit_new_string`) the interpreter's own
opcode handlers would call, plus the new frame push/pop itself is not
free -- the JIT removes bytecode dispatch overhead, not the underlying
allocation/lookup/write work. Verified correct against the interpreted
baseline's own output (`59000`) in every configuration tested.

**No regression on Phase 2/2b's allocation-free benchmarks** from adding
the pre-scan/dry-run mechanism, which was the explicit condition for
landing this: `int_arithmetic.di` still measures **~2.9x**
(`DIAMOND_JIT_THRESHOLD=1`, within noise of Phase 2's own ~2.95x),
`hash_ivar_construct.di`'s `Box` still measures **~6-7%** faster
(within Phase 2b's own measured 4-7% range) -- neither function triggers
the pre-scan's `needs_frame` path (no `STRING` in either body), so neither
pays anything for the mechanism existing.

**Dedicated GC-root stress test** (`tests/cases/jit_string_construct.di`/
`jit_string_construct_stress_gc.di`, new): a class whose `initialize`
does three sequential `STRING`+`INDEX_GET`+`SET_IVAR` sequences from
literal Hash keys, run under `DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1` with
and without `DIAMOND_STRESS_GC=1` (forces a collection on *every*
allocation, so three collections happen mid-function, mid-compiled-code).
All three configurations (interpreted, JIT, JIT+stress-GC) agree on output
(`2800`) -- the sharpest available proof that `self`, the Hash argument,
and intermediate temporaries held live across a `STRING`-triggered
allocation actually survive via the published frame, not by good luck.

Full test suite (1331 cases: the existing 1329 plus these 2 new cases)
passes unchanged under both debug and ASan/UBSan sanitizer builds, with
`DIAMOND_JIT` unset (default) and with
`DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1` (compiles
every eligible function immediately, forces a collection on every
allocation) -- no missed GC root surfaced under sanitizer instrumentation.

**Still not skindicate's real bottleneck end-to-end**: skindicate's actual
`User#initialize` additionally calls `super(attributes)` into
`ActiveRecord::Model#initialize`, which this JIT still can't compile
through (no call support). Deploying this build would now compile
`HydratedUser`-shaped `initialize` methods, but skindicate's own model
classes won't be JIT-eligible until a follow-on phase addresses `super`
call compilation -- not yet scoped or decided.
