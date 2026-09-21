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

## Phase 2d: SUPER call support -- the real User#initialize compiles (2026-09-13)

Added `DIAMOND_OP_SUPER` and `DIAMOND_OP_HASH` (for a `= {}` default
argument), plus a fix eliminating `EQUAL`/`NOT_EQUAL`'s own bailout
entirely (a `values_equal` trampoline -- pure, always succeeds). Full
design rationale (the 3-way `DiamondJitFn` return convention, the
`jc->has_called`-gated dual bailout stub, why arithmetic-after-a-call is
rejected outright at compile time) is in `docs/internal/jit-design.md`'s
own Phase 2d status note.

**The actual target now compiles.** Verified directly against
`skindicate.dia/lib/models/user.di`'s real `User#initialize` (not just
the `object_hydration.di` mirror) via `DIAMOND_TRACE_JIT=1`: 1 compiled
function, 0 bailouts, correct output identical to the interpreted
baseline across representative attribute Hashes (including the `= {}`
default-argument path).

**Three dedicated regression cases prove the actual hazard this phase
exists to prevent is prevented, not just "doesn't crash"**:
`tests/cases/jit_super_raise_propagates.di` (a superclass constructor
that raises -- confirms it runs exactly once, not twice, via a Hash-
mutation counter a double-invocation would double), `jit_super_then_
bail_propagates.di` (SUPER succeeds, then a *later*, unrelated opcode
bails -- same "ran exactly once" proof for a different failure site), and
`jit_super_chain.di` (6 levels of SUPER, all independently compiled,
proving `depth` threads correctly across multiple compiled hops without
a false-positive stack-overflow or crash). All three pass identically
under interpreted, `DIAMOND_JIT=1`, and `DIAMOND_JIT=1
DIAMOND_STRESS_GC=1`.

**No regression on Phase 2/2b/2c's existing benchmarks** from the 6th
persistent register (`depth`) and its alignment pad, or the new
`jc->has_called` dispatch: `int_arithmetic.di` still ~2.9-3x (release,
`DIAMOND_JIT_THRESHOLD=1`, 5 rounds, JIT side steady at ~2.70-2.73s per
round vs Phase 2c's own ~2.65-2.69s), `hash_ivar_construct.di` still
~4-6% faster, `object_hydration.di` still ~2-8% faster (both within the
same range previously measured).

**Honest end-to-end result -- smaller than object_hydration's own ~2-3%,
and worth understanding why**: measured a real `User.new(row)` loop
(100,000 iterations, same skindicate checkout, `boot.di` required
directly) interpreted vs `DIAMOND_JIT=1`:

| | wall time (100,000 `User.new` calls) |
|---|---:|
| Interpreted | ~7.4-7.8s (5 rounds) |
| JIT | ~7.3-7.6s (5 rounds) |

**Within noise -- not a real win yet.** `DIAMOND_TRACE_JIT=1` on this
exact benchmark shows exactly **1** compiled function, not 2:
`User#initialize` compiles, but its own `super(attributes)` call reaches
`ActiveRecord::Model#initialize`
(`packages/active_record/lib/active_record/model.di:36-49`), which
never does -- its body loops over `attributes.keys()` calling ordinary
methods (`.keys()`, `.length()`) and uses `INDEX_GET` on an *Array*
(this JIT's own `INDEX_GET` trampoline is Hash-only) plus `INDEX_SET`
(not in the whitelist at all), so it falls back to full interpretation on
every single call. That interpreted `Model#initialize` call dominates the
real per-call cost, swamping whatever `User#initialize`'s own now-
compiled body saves. **Deploying this build to skindicate today still
would not show a meaningful `/` improvement** -- a different, now
precisely understood reason than Phase 2/2b's "0 compiled functions":
the compile succeeds, but the dominant cost lives one level up the call
chain, in a superclass method this phase deliberately doesn't reach.

**Not yet scoped or decided**: making `Model#initialize` itself
JIT-eligible would need ordinary method-call (`INVOKE`) support plus
Array `INDEX_GET`/`INDEX_SET` -- each individually a materially larger
feature than anything built across Phases 2-2d, not a narrow extension
of the existing trampoline pattern.

## Phase 2e: two correctness bugs found and fixed, plus Array INDEX_GET/INDEX_SET (2026-09-13)

Continuing to scope `Model#initialize` meant reading `DIAMOND_OP_INDEX_GET`'s
and the generic `DIAMOND_OP_LESS` family's *full* real interpreter case
bodies for the first time -- Phase 2b only read enough of `INDEX_GET` to
build a Hash-only trampoline, and Phase 2d's `EQUAL`/`NOT_EQUAL` fix never
re-checked `EQUAL`'s own full case. That surfaced two real, already-shipped
correctness bugs (silent wrong answers, not crashes):

1. `EQUAL`/`NOT_EQUAL` (Phase 2d) skipped a user-defined `==` override on
   an Instance operand, silently falling back to identity comparison
   instead.
2. `INDEX_GET`'s Hash-only trampoline (Phase 2b) would, once a bailout
   could "propagate" instead of retry (true from Phase 2d onward), have
   incorrectly propagated `TYPE_ERROR` for an Instance with a real `[]`
   override reached after an earlier call, instead of invoking it.

**Both fixed, and verified as real fixes, not just "the new test passes"**:
for each bug, the new regression test was run against the pre-fix code
(via a temporary `git stash` of the fix) and confirmed to actually fail
there before trusting that it passing afterward means anything --
`jit_equal_overload.di` returns `0` instead of the correct `10` without
the fix; `jit_index_get_overload_after_super.di` raises an uncaught
`TypeError` instead of returning `1050` without the fix. Both trampolines
were rewritten as full extractions of their real opcode's case body
(`diamond_jit_equal_general`, `diamond_jit_index_get`, and new
`diamond_jit_index_set`), the same drift-proof pattern
`diamond_jit_super_call`/`diamond_jit_new_hash` already used -- a
standing rule now, not a one-off fix: never hand-pick a subset of an
opcode's real behavior into a trampoline again.

**New real capability, not just a fix**: `INDEX_SET` is JIT-compiled for
the first time (Hash/String/Instance-overload/Array, full parity with the
real opcode), and `INDEX_GET` now handles Array receivers too (previously
Hash-only). `tests/cases/jit_array_index.di` exercises both directly.

No regression on Phase 2/2b/2c/2d's benchmarks: `int_arithmetic.di` still
~2.9-3x, `hash_ivar_construct.di` still ~6-8% faster, `object_hydration.di`
still ~4-8% faster (release, 3 alternating rounds each, `DIAMOND_JIT_
THRESHOLD=1` where applicable). The real end-to-end `User.new` benchmark
(skindicate's actual `User#initialize`, 100,000 calls) is unchanged from
Phase 2d -- still within noise of interpreted (~7.1-7.3s both), since
`Model#initialize` still doesn't compile.

Full suite (1340 cases: 1336 plus these 4 new) green under debug +
ASan/UBSan, both with `DIAMOND_JIT` unset and with `DIAMOND_JIT=1
DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1`.

**`Model#initialize` still doesn't compile, confirmed as a clean bailout,
not a regression**: its loop condition uses the generic `DIAMOND_OP_LESS`
(never quickened to `LESS_INT` under `DIAMOND_JIT=1` alone), which has its
own Instance `<` override branch and so must also conservatively set
`jc->has_called = true` -- but that flag is compile-time-only and
monotonic, so `index += 1` immediately after it, every loop iteration, is
then rejected by the same "no arithmetic once a call could have happened"
rule Phase 2d's own safety depends on. Reaching `Model#initialize` would
need either a genuinely different runtime-checked (not compile-time-only)
has-a-call-happened flag, or local type inference proving `LESS`'s
operands are always Int -- a materially bigger design change than
anything in Phases 2-2e, not scoped or decided (see
`docs/internal/jit-design.md`'s own Phase 2e status note for the full
reasoning).

## Addition: seven new workloads for previously unbenchmarked primitives (2026-09-17)

`Tensor`, `Channel`, `Supervisor`, `Regexp`, `struct`, and `freeze`/
`frozen?` had zero `bench/*.di` coverage before this session, despite
several landing across the current release cycle -- only ad-hoc
one-off numbers in their own commit messages (Tensor's own GFLOPS
figures) existed anywhere. Seven new files close that gap, one
workload per primitive, `Thread` included as `Supervisor`'s own
steady-state baseline:

- `tensor_matmul.di` -- `Tensor#matmul` at 512x512 (real, threaded,
  k-blocked native path: total FLOPS is far above
  `DIAMOND_TENSOR_MATMUL_THREAD_FLOOR`, not the single-threaded
  small-shape fallback).
- `channel_message_passing.di` -- 20,000 `Channel#send`/`#receive`
  round trips between two real OS threads (every payload deep-copied
  across the heap boundary).
- `thread_pool.di` / `supervisor_pool.di` -- the identical 16-worker,
  fixed-arithmetic workload run under plain `Thread.new` versus
  `Supervisor`, to isolate supervision's own steady-state overhead
  (nothing crashes in either) from plain thread spawn/join cost.
- `regexp_match.di` -- `Regexp#match` with two capture groups, 20,000
  freshly-built subject strings (reginold, `docs/runtime-reference.md`).
- `freeze_mutation_check.di` -- `Array#push`/`Hash#[]=`/instance-variable
  writes on ordinary, never-frozen receivers, 200,000 iterations each --
  every one of these now pays a frozen check before the mutation
  proceeds regardless of whether the receiver is actually frozen; this
  is the common (unfrozen) path's own baseline cost.
- `struct_field_access.di` -- `struct Point(x: Int, y: Int)`
  construction plus both generated readers, 200,000 iterations, to
  compare against `hash_ivar_construct.di`/`object_hydration.di`'s own
  hand-written-class numbers below.

Same environment/methodology as the rest of this file (`gcc (GCC)
16.2.1`, `make release` `-O3 -march=native`, `DIAMOND_REPEAT`-based
in-process re-execution, default pass only -- `DIAMOND_QUICKEN` has no
effect on any of these, all dominated by native calls or thread
scheduling rather than the plain-Int arithmetic quickening targets):

| Benchmark | Iterations (repeat) | Default (per-iter) |
|---|---:|---:|
| `tensor_matmul` (new) | 20 | ~0.00751s |
| `channel_message_passing` (new) | 15 | ~0.02421s |
| `thread_pool` (new) | 15 | ~0.07585s |
| `supervisor_pool` (new) | 15 | ~0.07858s |
| `regexp_match` (new) | 15 | ~0.02358s |
| `freeze_mutation_check` (new) | 10 | ~0.20095s |
| `struct_field_access` (new) | 15 | ~0.31933s |

**`tensor_matmul`**: ~268M FLOPs/call (2 x 512^3) at ~7.5ms/call is
~35.7 GFLOPS -- in the same ballpark as, if a bit under, the 38-62
GFLOPS range cited in `Tensor#matmul`'s own commit message, since this
number also includes two `Tensor.random` fills per iteration, not
matmul alone. Its own opcode trace is nearly empty (`CONSTANT` 6,
`MOVE` 3, `TENSOR_RANDOM` 2, `RETURN`/`INVOKE`/`CALL` 1 each) --
confirms the real work happens entirely inside the native, threaded C
matmul, not the bytecode interpreter loop, exactly as intended.

**`thread_pool` vs. `supervisor_pool`**: ~0.0759s vs. ~0.0786s per
16-worker iteration -- roughly 3.6% slower under supervision with
nothing ever crashing. Real, but small next to thread-spawn cost
itself dominating both numbers (16 real OS threads spun up and joined
per iteration in each case); not a number worth optimizing against
without a concrete workload that cares.

**`struct_field_access` vs. hand-written-class equivalents**: ~0.319s
for 200,000 struct construct+read cycles here versus
`hash_ivar_construct.di`'s ~100x100-per-batch hand-written-class shape
and `object_hydration.di`'s own 4-field hand-written class -- opcode
trace (`CHECK_TYPE`/`SET_IVAR`/`GET_IVAR`/`INVOKE_MONO` all at
~400,000, matching two typed fields x 200,000 iterations) shows
`struct`'s generated `initialize`/readers compiling to the exact same
opcode shape a hand-written class's own typed `attr_accessor` already
would -- no generic/reflective struct-specific dispatch path, as
documented.

**A real bug found while writing `tensor_matmul.di`, not a benchmark
result**: `puts(tensor_result)` printed `#<Closure>` instead of
anything Tensor-shaped. `src/vm.c`'s `builder_format_value` (backs
`puts`/string interpolation) and `src/value.c`'s `diamond_value_fprint`
(the CLI's own top-level-result auto-print) each had one hardcoded
`"#<Closure>"` fallback for *every* object kind neither explicitly
handled -- correct only for a real `Closure`, silently wrong for
`Tensor`, `Channel`, `Supervisor`, `Regexp`, `Fiber`, `File`, `Thread`,
`SQLite3`, and every other native resource kind added since either
function was last extended. Fixed by consolidating both functions onto
one canonical per-kind name table (`diamond_format_value_type`,
exported from `src/vm.c` via `vm.h`, previously `format_value_type`
and `static`) instead of two independently hand-maintained copies --
the same "two duplicated implementations silently drift" failure shape
this project has hit before (`DIAMOND_OP_SET_IVAR`'s interpreter case
versus the JIT's own trampoline, `freeze`/`frozen?`'s own addition).
Regression test: `tests/cases/native_resource_default_to_s.di`.

**Follow-up**: `diamond_format_value_type` alone left `Time` printing
the generic `#<Time>` placeholder from the CLI's own top-level-result
auto-print, rather than an actually-wrong label -- `src/value.c` had no
`Time` case at all there (unlike `puts`/interpolation, which already
formatted it correctly via `src/vm.c`'s own `format_time_default`).
Fixed by extracting that formatting into a buffer-based
`diamond_format_time_default` and exporting it too, so both paths
share the exact same real date formatting instead of one lacking it.
`tests/cases/time_stringify.di` now exercises both paths directly.

## Addition: three new workloads for JIT Phase 3/4 and self-recursive TCO (2026-09-18)

Self-recursive tail-call optimization and the two most recent JIT
phases (`has_called` no longer blocking int arithmetic/comparison;
`dup`/`freeze`/`frozen?` support) had no dedicated `bench/*.di`
coverage -- `object_hydration.di`'s `run_dup()` exercises Phase 4
incidentally inside an ActiveRecord-shaped benchmark, but nothing
isolated either JIT phase on its own, and TCO had no workload at all.

- `tail_call_optimization.di` -- a qualifying self-recursive tail call
  (explicit `return` guard clause, `return selfcall(...)` as the
  function's own trailing statement -- see docs/callables.md) 3,000,000
  levels deep. Opcode trace confirms `TAIL_CALL` (not plain `CALL`)
  for all 3,000,000 occurrences -- direct proof the optimization is
  active, not just "didn't crash": ordinary recursion this deep would
  overflow `DIAMOND_MAX_CALL_DEPTH` (95) almost immediately.
- `jit_arith_after_call.di` -- a Hash `INDEX_GET` (sets `jc->has_called`)
  followed by a provably-`Int` loop counter's own `LESS_INT`/`ADD_INT`
  in the same function -- the exact shape Phase 3 closed
  (`tests/cases/jit_int_arith_after_index_get.di`'s own sized-for-
  timing sibling). Before Phase 3 this function was permanently
  JIT-ineligible (0 compiled functions, no matter how hot); now it
  compiles and measurably helps.
- `jit_dup.di` -- isolates just `dup` (Phase 4) in its own function,
  called repeatedly from a separate driver loop (the same shape
  `Model#initialize` is actually reached: many separate calls, not one
  call in a hot internal loop -- a `dup` call *inside* a loop condition
  never compiles at all, since the loop's own `LESS_INT` already sets
  `has_called` first; see `docs/internal/jit-design.md`'s Phase 4 note).

Neither JIT benchmark is part of the automatic default-vs-quicken sweep
below (`DIAMOND_JIT` is a separate opt-in tier) -- measured by hand,
same `make release` binary, `DIAMOND_JIT` toggled purely via env var:

| Benchmark | Interpreted | JIT'd | Speedup |
|---|---:|---:|---:|
| `jit_arith_after_call` (`DIAMOND_JIT_THRESHOLD=1`, single call) | ~0.283s | ~0.17-0.23s | ~1.3-1.7x |
| `jit_dup` (default threshold, 500,000 calls) | ~0.171s | ~0.137s | ~1.25x |

`jit_arith_after_call`'s own win is real but noisier and more modest
than the ~2.9-3x `int_arithmetic.di` gets (a debug-build comparison of
this same file showed a misleadingly large ~7x, the same debug-baseline
distortion `bench/RESULTS.md`'s own Phase 2 section already warns
about) -- this function does far less arithmetic per iteration relative
to its `INDEX_GET`/`STRING`-key-allocation overhead than `int_
arithmetic.di` does, so bytecode-dispatch removal buys proportionally
less. `jit_dup`'s own ~25% win is consistent with, if a little higher
than, Phase 4's own `object_hydration.di` measurement (~8-10% end to
end) -- expected, since this file isolates `dup` alone rather than
diluting it with `run()`'s own un-JIT'd Hash-literal-construction and
`.length()`/`.attribute()` calls the way `object_hydration.di`'s
`run_dup()` does.

`tail_call_optimization` and both JIT files also ran in the default
sweep below for their own interpreted-baseline numbers (repeat=15,
repeat=15, repeat=30 respectively) -- see the table.

**`thread_pool.di`/`supervisor_pool.di` also changed this session**:
capped from 16 workers down to 5, at the user's own request (headroom
for other work on the same dev machine, less cache pressure from
running a benchmark locally). Re-measured at the new worker count
(same `make release` binary, default pass): `thread_pool` ~0.0375s,
`supervisor_pool` ~0.0405s per 5-worker iteration -- supervision now
reads about 8% slower rather than the previous ~3.6% at 16 workers,
but both absolute numbers are small enough (real OS thread spawn/join
dominating either way) that this delta is within the kind of run-to-run
noise a 5-thread, sub-50ms measurement is naturally more sensitive to
than the old 16-thread one was, not evidence supervision itself got
proportionally more expensive.

## Correction: `hash_ops.di`'s own comment was stale, not a live finding (2026-09-18)

Reviewing this session's full sweep with the user, `hash_ops.di`'s own
comment ("hash_find is a genuine O(n) linear scan... not a real hash
table") looked like the standout actionable finding -- until checking
`git log` on `hash_find` itself before acting on it. `Hash` was
rewritten to a real open-addressing hash table on 2026-08-07
(`15d8d868`, "Replace Hash's O(n) linear scan with a real hash table")
-- over a month before this session. `hash_ops.di`'s own comment and
5,000-entry sizing (deliberately small to avoid an O(n^2) insert phase)
were simply never updated afterward. Confirmed directly: 100x more
entries (5,000 -> 500,000) cost only ~10x more total time for
insert+lookup, not the ~10,000x a real linear scan would produce --
the signature of O(1) average-case lookups, not O(n). Fixed the stale
comment and rescaled the benchmark to 200,000 entries (repeat=40) now
that there's no O(n^2) blowup to size around: ~89ns/op, matching the
old 5,000-entry number (~86ns/op, within noise) at 40x the data --
direct confirmation the O(1) cost holds at scale, not just a small
sample. `bench/BASELINE.md`'s own near-identical claim was left
untouched -- it's an explicitly dated snapshot ("branched from main @
`0bafbc4`"), and that commit genuinely predates the hash-table fix
(confirmed via `git merge-base --is-ancestor`), so the claim was
accurate when written and rewriting a dated historical record to
reflect later truth would defeat its own stated purpose as a frozen
reference point.

**Lesson for next time, stated plainly rather than filed away**: a
benchmark file's own doc comment is not a substitute for reading the
current implementation before recommending work based on it, even
when the comment reads as confident and specific ("confirmed by
reading the code, not just inferred from timing" was `BASELINE.md`'s
own phrasing for the *original*, now-superseded finding). `git log`
on the function in question settled this in under a minute and should
have been the first move, not an afterthought.

## Follow-up: closing the loop -- generic arithmetic JIT support (2026-09-18, same day)

The real reason `hash_ops.di`'s `run()` never JIT-compiled turned out to
have nothing to do with Hash at all: `total = total + values[index]` is
generic `ADD` (a Hash's values have no static type), and the JIT had zero
cases for any generic arithmetic/comparison opcode -- only the runtime-
quickened `_INT` forms, and not even all of those (`LESS_EQUAL_INT`/
`GREATER_INT`/`GREATER_EQUAL_INT` were never added). Closed this session
(`docs/internal/jit-design.md`'s own "Phase 5") -- both `hash_ops.di` and
`int_arithmetic_dynamic.di` (the file that originally named the typed-
vs-untyped gap) now compile and measurably help:

| Benchmark | Interpreted | JIT'd | Speedup |
|---|---:|---:|---:|
| `hash_ops` (`DIAMOND_JIT_THRESHOLD=1`) | ~0.057s | ~0.038s | ~1.5x |
| `int_arithmetic_dynamic` (`DIAMOND_JIT_THRESHOLD=1`) | ~1.07s | ~0.51s | ~2.1x |

`int_arithmetic_dynamic`'s own win closes most (though not all) of the
~2.8x gap this file's own comment names against `int_arithmetic.di` --
the remainder is inherent to the runtime kind check every generic-opcode
call site now pays that a statically-proven-`Int` `_INT` site's fast
path skips.

A real, already-deployed bug (not related to Hash or this fix's own
correctness, found empirically while testing it) was also caught and
fixed in the same phase -- see `docs/internal/jit-design.md`'s own Phase
5 section for the full account. Worth noting here too since it's the
kind of thing a benchmark-driven investigation is well-positioned to
surface: chasing a modest perceived slowness turned up a real crash bug
already live in production, not just the perf question originally asked.

## `self.method()` dispatch -- JIT Phase 7 (2026-09-18, same day)

Generic `DIAMOND_OP_INVOKE` had been the one named-but-untouched JIT gap
since Phase 2g: any dynamic method call, including a plain
`self.other_method()`, bailed the whole containing function out of JIT
eligibility. Closed for the `self`-receiver case (`docs/internal/
jit-design.md`'s own "Phase 7") -- a real, ordinary loop calling a method
on `self` every iteration now compiles the *entire* loop, not just an
isolated call:

| Benchmark | Interpreted | JIT'd | Speedup |
|---|---:|---:|---:|
| `jit_invoke_self` (`self.method()` in a 3M-iteration loop) | ~0.75s | ~0.49s | ~1.53x |

Any receiver other than `self` remains unsupported -- see
`docs/internal/jit-design.md`'s Phase 7 section for exactly why that's a
real, not-yet-buildable boundary (no compile-time proof of receiver type,
no runtime deopt mechanism to guard and fall back mid-function).

## Typed-parameter and freshly-`NEW`'d-local dispatch -- JIT Phases 9-10 (2026-09-18/19)

Two narrow, provably-sound slices of non-`self` `DIAMOND_OP_INVOKE`,
closed back to back (`docs/internal/jit-design.md`'s own "Phase 9" and
"Phase 10" sections have the full design/verification detail): a
declared parameter with a single concrete class type that the function
body never reassigns (Phase 9), and a local holding the result of one
`SomeClass.new(...)` call, never reassigned after (Phase 10). Both reuse
Phase 7's own `diamond_jit_invoke_instance` trampoline unchanged --
`recv` was already a real compile-time-known register-index parameter
there for exactly this reuse.

| Benchmark | Interpreted | JIT'd | Speedup |
|---|---:|---:|---:|
| `jit_invoke_typed_param` (typed-param `.method()` in a loop) | ~0.55s | ~0.34s | ~1.62x |
| `jit_new_local` (`.new()` + `.method()` in a loop) | ~1.02s | ~0.67s | ~1.53x |

A controlled A/B on skindicate's own `/` route (same dev server/DB/
instrumentation, only the `diamond` binary differing via a `git worktree`)
measured a real but modest ~4-6% end-to-end win from Phase 9 alone --
`SkinPlatform.platforms_for` barely moved (~0.6%), confirming most of
that route's own remaining ORM overhead flows through neither typed
parameters nor `.new()` results, but through a method call's own return
value assigned to a local -- still unsupported, see `docs/internal/
jit-design.md`'s Phase 10 section for why that's a materially harder
case (no compile-time-known class to attach, unlike `NEW`'s own operand).
# Standalone Skindicate JIT (2026-09-20)

After the standalone launcher began honoring `DIAMOND_JIT`, a local A/B
used one `diamond build` executable and a copied development database
(1,352 skins, 1,041 Winamp imports, migrated to the current schema). The
program called Skindicate's real `app` handler for `GET /` directly, with
70 warm-up requests followed by 80 timed requests. `LOG_LEVEL=error` kept
request logging out of the timing. Four alternating runs, default JIT
threshold (50 calls):

| JIT | ms per timed request | Compiled functions | Bailouts |
| --- | ---: | ---: | ---: |
| off | 25.93 | 0 | 0 |
| on | 23.96 | 67 | 0 |
| on | 24.38 | 67 | 0 |
| off | 25.58 | 0 | 0 |

This is about a 6% local request-handler improvement. It excludes socket,
proxy, and network time, and uses a smaller database than production; it is
evidence that the standalone JIT executes real Skindicate code, not a
production throughput claim.

On the Ubuntu 26.04 production droplet, the same locally cross-built
Skindicate executable served 70 warm-up requests and 40 timed sequential
HTTP requests to `127.0.0.1:18110/` per setting. With JIT off, median
latency was 209.13 ms (mean 208.38 ms, p90 234.40 ms); with JIT on it was
200.97 ms (mean 200.95 ms, p90 215.68 ms). That is a 3.9% median
improvement in this single off-then-on run. The full production database,
socket and HTTP handling are included; proxy and external network time are
excluded. A single sequential run is sensitive to server load and cache
state, so this is a deployment sanity check rather than a throughput result.

## Ivar receiver result chaining -- JIT Phase 16 (2026-09-20)

`bench/jit_invoke_result_ivar.di` exercises `result = @factory.make(@value)`
followed by a hot loop calling a declared method on `result`. Release build,
`DIAMOND_JIT_THRESHOLD=1`, three alternating runs:

| Mode | Times | Mean |
|---|---:|---:|
| Interpreted | 8.05s, 9.00s, 7.95s | 8.33s |
| JIT | 4.73s, 4.14s, 3.90s | 4.26s |

The JIT path was about 49% faster. This isolates the newly eligible receiver
shape; it is not an end-to-end application measurement.

## Native collection reads -- JIT Phase 17 (2026-09-20)

`bench/jit_native_collection_reads.di` runs five million iterations containing
Array/Hash `length`, Hash `key_at`, and Hash `value_at`. One release-build run
took 4.86s interpreted and 0.19s with JIT (`DIAMOND_JIT_THRESHOLD=1`), about
96% faster. This intentionally isolates dispatch overhead; it does not predict
the end-to-end Skindicate gain.

The follow-up `bench/jit_native_string_reads.di` runs five million iterations
containing String `length`, `index_of`, and `ord`. Three alternating release
runs averaged 0.80s interpreted and 0.27s with JIT, about 67% faster.
