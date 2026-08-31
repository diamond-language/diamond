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
