# Baseline benchmarks (pre-JIT)

Recorded at the start of `jit-experimentation`, branched from `main` @
`0bafbc4` (2026-08-08). Purpose: a durable reference point to compare
against as JIT work lands — re-run `bash bench/run.sh quicken` on any
later commit and diff against the numbers here.

## Environment

- CPU: AMD Ryzen 5 Pro 7535U (12 logical cores; single-threaded workload,
  no parallelism in the VM)
- `gcc (GCC) 16.1.1`, built via `make release` (`-O3 -DNDEBUG -march=native`)
- No `perf` available in this environment — hot-path data instead comes
  from the interpreter's own exact per-opcode execution counters
  (`DIAMOND_TRACE_OPCODES`), which is arguably better than sampling for
  a bytecode interpreter: exact counts, zero sampling error.

## Methodology

- `bench/*.di` are hand-written microbenchmarks, each targeting one
  hot-path category (see file comments for what each exercises and why).
- Timed via `DIAMOND_REPEAT=N`, which re-runs the *already-compiled*
  chunk N times inside one process — this measures interpreter
  throughput, not process startup or compile time. Per-iteration time =
  total wall time / N. `bench/run.sh` picks N per-benchmark to land
  around 2-5s of total measured time.
- Every benchmark's numeric result was manually verified for
  correctness (e.g. `fibonacci.di` → `832040`, the known `fib(30)`)
  before being trusted for timing.
- Two passes: default (`DIAMOND_QUICKEN` unset) and `DIAMOND_QUICKEN=1`
  (enables the VM's existing runtime opcode-specialization tier).
- Opcode breakdown is one untimed single run per benchmark with
  `DIAMOND_TRACE_OPCODES=1`, top 12 opcodes by execution count.

## Results

| Benchmark | Iterations | Default (per-iter) | Quicken=1 (per-iter) | Delta |
|---|---:|---:|---:|---:|
| `int_arithmetic` | 5,000,000 | 0.2978s /5M ≈ 59.6ns/iter | 0.2854s ≈ 57.1ns/iter | ~4%, noise-level |
| `int_arithmetic_dynamic` | 5,000,000 | 0.6568s ≈ 131ns/iter | 0.6629s ≈ 133ns/iter | ~1% *slower*, noise-level |
| `dispatch_monomorphic` | 2,000,000 | 0.2663s ≈ 133ns/iter | 0.2661s ≈ 133ns/iter | none |
| `dispatch_polymorphic` | 2,000,000 | 0.3244s ≈ 162ns/iter | 0.3374s ≈ 169ns/iter | ~4% *slower*, noise-level |
| `fibonacci` (fib(30), 2.69M calls) | 1 | 0.3012s | 0.2976s | ~1%, noise-level |
| `array_ops` | 1,000,000 | 0.0873s ≈ 87ns/iter | 0.0816s ≈ 82ns/iter | ~6%, near noise |
| `hash_ops` | 5,000 ins + 5,000 lookups | 0.01835s | 0.01843s | none |
| `string_ops` | 200,000 | 0.0981s ≈ 490ns/iter | 0.1015s ≈ 508ns/iter | none |
| `closures` | 1,000,000 | 0.1090s ≈ 109ns/iter | 0.1147s ≈ 115ns/iter | none |
| `fiber_switch` | 500,000 resume/yield pairs | 0.4183s ≈ 837ns/iter | 0.4223s ≈ 845ns/iter | none |

(Raw `bench/run.sh` output, including the opcode breakdowns, is
reproducible on demand — not duplicated here to keep this table
scannable. Numbers above are one run each, not averaged across
multiple trials; treat single-digit percent deltas as noise, not
signal, given the methodology.)

## Findings

**1. `DIAMOND_QUICKEN=1` shows no measurable benefit anywhere in this
suite — including in a benchmark built specifically to require it.**
`int_arithmetic.di` uses plain, locally-typed Int locals, which the
*compiler* already statically specializes to `ADD_INT`/`SUBTRACT_INT`/
`MULTIPLY_INT`/`DIVIDE_INT` at compile time (`src/compiler.c:2217-
2260`, gated on `known_types[left]==DIAMOND_TYPE_INT` for both
operands) — independent of `DIAMOND_QUICKEN`. So that benchmark never
exercises the runtime tier at all.

`int_arithmetic_dynamic.di` was added specifically to close that gap —
it routes the same arithmetic through an untyped function parameter,
confirmed via `--dump-bytecode` to compile to generic `ADD`/
`SUBTRACT`/`MULTIPLY`/`DIVIDE` (no static specialization possible: the
compiler's `known_types` inference is local to one function body, and
an untyped parameter carries no type information into it). With
`DIAMOND_QUICKEN=1`, `DIAMOND_TRACE_QUICKEN=1` confirms the mechanism
*does* fire correctly — "quickened sites: 4, deoptimized sites: 0" —
and `DIAMOND_TRACE_OPCODES` confirms the specialized opcodes actually
ran (`SUBTRACT_INT`/`MULTIPLY_INT`/`DIVIDE_INT` each executed
4,999,999 of 5,000,000 times, vs. their generic forms running only
once, before the first-call quickening kicked in). So the tier isn't
broken — it's just that eliminating one `registers[left].kind==
DIAMOND_VALUE_INT` branch check per arithmetic op doesn't move the
needle when `CALL`/`RETURN`/frame-setup overhead (`NIL`/`MOVE`/
`CONSTANT` account for far more total instructions than the arithmetic
itself in this benchmark — see opcode breakdown) already dominates.
**Implication for JIT scope**: opcode-level specialization of
individual arithmetic ops is not where the time is going in call-heavy
code; call/dispatch overhead is a more promising target than further
specializing arithmetic.

**2. Inline caching for method dispatch clearly works and clearly
matters.** `dispatch_monomorphic.di`'s opcode trace shows `INVOKE_MONO`
(the monomorphic-cache fast path) used 1,999,998 of 2,000,000 calls —
essentially every call after the first upgrades to the cached path.
`dispatch_polymorphic.di` (4 alternating receiver classes) never
upgrades — plain `INVOKE` runs all 2,000,000 times, as expected for a
call site that can't stay monomorphic. Per-iteration cost: ~133ns
(mono) vs. ~162-169ns (poly) — roughly 20-25% slower, though note the
polymorphic benchmark also does extra `Array` indexing per iteration
to select the receiver, so this isn't a perfectly isolated A/B (a
cleaner poly-vs-mono comparison would be a good early JIT-branch task).

**3. Fiber resume/yield is the single most expensive per-operation
primitive measured.** ~837ns per round trip vs. ~133ns for a
monomorphic method call — roughly 6x. Expected going in (`docs/fibers.md`
already documents the `ucontext`-based stackful-coroutine design), now
has a concrete number. Worth deciding early whether the JIT effort
touches fiber boundaries at all, or explicitly scopes them out given
this cost is dominated by the OS-level context switch, not bytecode
dispatch.

**4. `Hash` is a genuine O(n) linear scan (`hash_find`, `src/vm.c`),
confirmed by reading the code, not just inferred from timing** — every
`.length()`-preserving insert or lookup scans up to `count` entries.
`hash_ops.di` is deliberately small (5,000 keys, not 50,000) because
the insert phase is O(n²) overall; at 5,000 keys it's already fast in
absolute terms (~18.4ms per repeat of the full 5,000-insert + 5,000-
lookup workload) but this degrades quadratically — a `Hash` with tens
of thousands of entries would be measurably slow today. Whether that's
in scope for the JIT branch (a real hash table is a data-structure
change, not a dispatch optimization) is worth an explicit decision
rather than silently falling out of scope.

**5. Function-call overhead is substantial and highly repetitive.**
`fibonacci.di`'s opcode trace: `NIL` (5.39M) and `MOVE`/`CONSTANT`
(5.39M each) each outnumber `CALL`/`RETURN` (2.69M each) roughly 2:1 —
every call pays for local-slot `NIL`-initialization and register
shuffling on top of the call/return machinery itself. This matches
finding #1's implication: call overhead, not arithmetic dispatch, is
where a JIT's early wins are most likely to be.

## Coverage gaps in this baseline (for later rounds)

- No benchmark exercises `CHECK_TYPE`/`INVOKE_TYPED`/`CALL_TYPED`
  (typed-parameter/generic dispatch) as a hot path — none of the
  current benchmarks show these opcodes in their top-12.
- No benchmark isolates GC pressure specifically (allocation-heavy
  code without a dominating loop-body cost elsewhere) — `array_ops.di`
  touches this via array growth but doesn't isolate it.
- The `dispatch_polymorphic` vs `dispatch_monomorphic` comparison has a
  confound (extra Array indexing in the polymorphic variant) noted
  above.
