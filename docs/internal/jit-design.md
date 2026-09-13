# JIT design: deoptimization and GC-root contracts

This document records the design contracts a future JIT tier would need to
satisfy to interoperate safely with the existing interpreter, GC, and
threading model -- written ahead of any codegen, per
[`docs/roadmap.md`](../roadmap.md)'s "Native-code execution" section, which
gates real JIT work on exactly this plus representative benchmark evidence
(see [`bench/RESULTS.md`](../../bench/RESULTS.md)'s `object_hydration.di`
addition and the existing `typed_dispatch.di`).

**Status (2026-09-12): a first, deliberately narrow implementation now
exists** (`src/jit.c`/`src/jit.h`, opt-in via `DIAMOND_JIT=1`) -- everything
below was written before that code and describes the *general* contract a
future, broader JIT (one that compiles calls, allocation, or anything that
can trigger GC or raise) will need. The v1 slice sidesteps almost all of it
on purpose: it only compiles zero-argument, call-free, allocation-free,
exception-free functions, which by construction never hit a GC safepoint or
need to unwind -- so it needs no `DiamondFrame`, no GC-root publishing, and
no mid-function deopt, only a compile-time "don't compile this at all"
bailout for anything outside its whitelist plus a handful of runtime
bailouts (integer overflow, division edge cases) that re-run the *whole*
function via the interpreter rather than resuming mid-flight. See `src/
jit.c`'s own header comment for the precise, current scope, and `bench/
RESULTS.md`'s "Phase 2 baseline JIT" section for the first real measurement
(~2.95x on `bench/int_arithmetic.di`, release build). The frame contract,
tier-up trigger, and deopt design below remain the target for whatever
comes after this slice -- most concretely, compiling `INVOKE_MONO`-guarded
calls, which is the first thing that would actually need them.

## Why the interop seam is already clean

Every Diamond call recurses `run_chunk` (`src/vm.c:13823`), which pushes a
`DiamondFrame` onto the singly-linked `vm->frames` chain and pops it on every
return path:

```c
typedef struct DiamondFrame {
    struct DiamondFrame *previous;
    DiamondValue *registers;
    PendingUnwind *pending;
    size_t register_count;
    const DiamondChunk *chunk;
    const size_t *instruction_offset;
} DiamondFrame;
```

(`src/vm.c:202-216`). GC marking, backtraces, and exception unwinding all
walk this same chain and know nothing about *how* a frame's registers got
populated. A JIT'd function that pushes a conforming `DiamondFrame` is
indistinguishable from an interpreted one to every one of those consumers --
no new marking, backtrace, or unwinding machinery is needed, only conformance
to the existing one.

## Frame contract

A JIT'd function must, for the duration of its execution:

1. Allocate a `DiamondValue` array for its live values (arguments plus
   whatever the JIT decides needs to survive a GC safepoint -- see
   "What must be published" below) and link a `DiamondFrame` pointing at it
   onto `vm->frames`, exactly where `run_chunk` does today (`src/vm.c:13916-13924`).
2. Pop that frame on every exit path (normal return, exception, deopt bailout)
   exactly where `run_chunk` does today (`src/vm.c:13955`, `21456-21460`) --
   including on the deopt path in "Deopt trigger" below, which is really just
   another exit path that happens to jump back into `run_chunk` first.
3. Set `chunk` to the real `DiamondChunk` being executed (backtraces read
   `chunk`/`instruction_offset` together to report a call site).
4. Set `instruction_offset` correctly: this field is a **pointer to a live
   `size_t`, not a copied value** (`run_chunk` points it at its own local
   `instruction_offset` variable, which keeps advancing as bytecode executes
   in that frame -- see the struct's own comment at `src/vm.c:208-215`,
   verified directly). A JIT'd frame has no bytecode dispatch loop advancing
   an instruction offset, so it needs a real `size_t` lvalue to point at --
   the simplest choice is a per-JIT-frame local holding a synthetic "logical"
   offset, updated at safepoints (calls, allocations) so a backtrace/deopt
   taken mid-JIT-frame still reports *something* meaningful rather than
   dangling or stale. This is the one place the existing struct's shape
   doesn't fall out for free and needs an explicit decision when codegen
   is designed.

### What must be published (GC-root contract)

Diamond's GC is non-moving generational mark/sweep (write barrier + card
marking; see [`gc-generational-design.md`](gc-generational-design.md)) with
**precise, explicit roots** -- there is no conservative native-stack scan.
`mark_frame_chain` (`src/vm.c:543-552`) marks exactly
`frame->registers[0..register_count)` plus a pending-unwind value, for every
frame on `vm->frames`. This is close to the best case for a JIT:

- **Non-moving** means a JIT'd function can hold a raw `DiamondObject*` in a
  machine register or on its own native stack across a call *as long as
  nothing else needs to find it there* -- the collector never relocates it
  out from under the JIT, so there's no read-barrier or handle-indirection
  problem to solve.
- **Precise roots** mean the collector will never find a `DiamondValue` the
  JIT didn't explicitly publish through `frame->registers`. Any live Diamond
  value the JIT is keeping in a machine register or spilled to its own native
  stack (not the published `DiamondValue` array) **is invisible to `mark_frame_chain`**
  and will be collected out from under it the moment a GC-triggering
  operation runs (any allocation, effectively any call to interpreter/runtime
  helpers).
- The concrete contract: **at every safepoint** (any call into `run_chunk`,
  any allocation, any runtime helper that can allocate), every Diamond value
  the JIT'd code still needs afterward must already be written into its
  published `frame->registers` array, not sitting only in a machine register.
  This is exactly what `run_chunk` already does by construction (its
  registers *are* `frame->registers`); a JIT's own register allocator needs
  to reconcile its native register assignments back to this array at each
  safepoint rather than only at function exit.

## Tier-up trigger

No per-function invocation or loop-backedge counter exists today -- inline
cache counters (`inline_cache_hits`/`misses`, `monomorphic_dispatches`,
`method_cache_probes` at `src/vm.h:1204-1221`) are keyed per cache-*slot*
(64 slots total, `DIAMOND_INLINE_CACHE_COUNT`, `src/vm.h:523`), not per
`DiamondFunction`. A tier-up trigger needs new state: a single invocation
counter added to `DiamondFunction`, checked at call entry against a threshold
field on `DiamondVm` (mirroring `quickening_threshold`'s existing precedent
at `src/vm.c:1130`, default 1 -- a JIT's own default should almost certainly
be much higher, since compiling is far more expensive than an opcode rewrite).

**This is new state added to a hot path, which the project has directly
measured being dangerous before.** The reverted `jit-experimentation`
experiment #4 (`b7e91e64`) regressed monomorphic dispatch ~9% by growing
`run_chunk`'s own parameter count from 7 to 9 -- more state visible to a
~2000-line function increased register pressure across its *entire* body,
not just at the new state's own use site, the opposite effect of the
struct-copy-removal win landed right before it. A per-function counter lives
on `DiamondFunction`, not as a new `run_chunk` parameter, so it isn't the
same mechanism -- but the lesson generalizes: **measure the counter check's
own cost on `run_chunk`'s call-entry path in isolation** (via the
alternating-worktree-rounds methodology `bench/BASELINE.md` established)
before assuming a cheap-looking read-and-increment is actually free once
inlined into the hottest function in the codebase.

## Deopt trigger

Model directly on the mechanism that already exists for method dispatch, not
a new one: a hot monomorphic `INVOKE` site self-rewrites to `INVOKE_MONO`
(`src/vm.c:18848-18854`) and reverts to plain `INVOKE` the moment its cached
`(receiver_class, method)` pair stops matching (`src/vm.c:18842-18845`) --
the interpreter's existing "compile a shortcut, cheaply verify it still
applies, fall back the instant it doesn't" pattern. A JIT'd function's own
entry guard should do the same thing at a coarser grain: check whatever
assumptions the compiled code baked in (argument shapes/classes, in the
simplest case), and on mismatch, **do not attempt to resume mid-JIT-frame** --
bail to `run_chunk` on the function's original, always-correct bytecode from
the top, exactly as if the JIT'd version had never been tried. This makes an
incorrect compile structurally unobservable: a guard failure can only cost
speed (falling back to the interpreter), never correctness, and needs no
mid-function state reconstruction (no "resume the interpreter at bytecode
offset N with register file matching what the JIT had" problem to solve) --
the same reason picking a narrow, guard-checked opcode subset in
implementation is a correctness simplification, not just a scoping one (see
the approved plan's Phase 2).

## Threading

Every `Thread.new`/`gremlin_serve(threads: N)` worker gets its own
independent `DiamondVm`, and critically, its own **deep copy** of every
function's bytecode/constants (`clone_program_from_chunk`, `src/vm.c:2152-2185`,
via `diamond_function_copy`, `src/compiler.c:15249` -- confirmed a real
`malloc` + `memcpy`, not a shared pointer). This is also why quickening's
self-modifying opcode rewrite needs no synchronization today: `chunk->code`
is never aliased between two `DiamondVm`s. The design's own comment states
the intent directly (`src/vm.c:2141-2145`): give each thread its own program
so cross-thread mutation races are sidestepped by construction, not
synchronized.

**v1 scope: JIT'd code is not shared across this boundary.** If a JIT
attaches a compiled-code pointer to a chunk the same way quickening rewrites
an opcode, that attachment lives in one thread's private clone and would
need to be redone (re-JIT'd) in every other worker independently -- for
`gremlin_serve(threads: N)`, that's N redundant compilations of the same hot
function. Not a blocker for the motivating workload (skindicate runs
`threads: 1` today), but a real, documented limitation rather than an
assumed-away one: a cross-thread compiled-code cache (keyed by something
stable like a function's source location, looked up rather than attached to
the per-thread chunk pointer) is explicit future work, not v1.

## Call-depth interaction

`DIAMOND_MAX_CALL_DEPTH = 95` (`src/vm.c:168`) is calibrated against
interpreter C-stack usage: the long rationale at `src/vm.c:122-167` sizes it
against `run_chunk`'s own ~8KB-per-activation C-stack frame (giant
opcode-switch locals), not against Diamond call semantics directly. A JIT'd
frame does not run `run_chunk`'s opcode switch and should use dramatically
less native C stack per Diamond-level call. Two consequences to decide, not
yet decided:

- If JIT'd frames count against the same 95-deep counter, JIT'd code gets no
  recursion-depth benefit from being faster/smaller, only a speed one.
- If JIT'd frames are exempted or counted differently, the guard needs
  re-derivation against the JIT's own actual C-stack usage per frame (however
  large that turns out to be), not silently inherited from an unrelated
  constant sized for a different code path.

Either is workable; the wrong move is leaving this implicit and finding out
via a stack-depth-related crash after codegen exists.
