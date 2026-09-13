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

**Status (2026-09-12, same day): Phase 2b extended the whitelist without
needing the general contract yet either** -- arguments/`self` (any arity,
methods included, not just zero-arg top-level functions), `EQUAL`/
`NOT_EQUAL` on primitives, and two new operations that read like "calls"
but aren't full method dispatch: `SET_IVAR` and `INDEX_GET` (Hash only),
each via a narrow C trampoline (`diamond_jit_set_ivar`/`diamond_jit_hash_
get` in `vm.c`) rather than hand-rolled machine code, plus `CHECK_TYPE`
via `diamond_jit_check_type` the same way. All three trampolines were
individually confirmed allocation-free by reading their call chains, so
this extension *still* needs no `DiamondFrame`/GC-root publishing -- see
`src/jit.h`'s own updated comment. The dispatch check is now wired into
two call sites, not one: `DIAMOND_OP_CALL` (top-level functions) and
`invoke_resolved_method_helper` (which `NEW`/`SUPER`/`INVOKE_TYPED`'s
ordinary instance dispatch all route through, plain `INVOKE`/`INVOKE_MONO`
still not among them) -- both now go through one shared `jit_call_or_
interpret` helper in `vm.c`.

**What this did *not* reach, found the hard way**: the actual motivating
target, skindicate's own `User#initialize`-shaped row hydration
(`bench/object_hydration.di`'s `HydratedUser#initialize`), still isn't
JIT-eligible. `attributes["email"]`-style Hash access compiles a fresh
`DIAMOND_OP_STRING` construction for the literal key on *every* call --
this is not a benchmark-specific quirk, string-literal Hash keys are
pervasive in ordinary Diamond code -- and string construction is a real
allocation, landing squarely back in "needs the general frame/GC-root
contract" territory this whole extension had been sidestepping. Confirmed
end-to-end on a smaller case built specifically to isolate this (`tests/
cases/jit_hash_ivar_construct.di`: same `SET_IVAR`/`INDEX_GET`/
`CHECK_TYPE`/self/argument machinery, Hash key passed as a parameter
instead of a literal) -- that one compiles and measures correctly,
confirming the extension itself works; `object_hydration.di`'s own
`initialize` does not, purely because of the string-literal keys. A real
"Phase 2c" -- an allocation-capable trampoline for string/Hash/Array
construction, plus actually building the `DiamondFrame`-publishing
contract this document describes -- is the concrete next gate, not
optional polish.

**Status (2026-09-12, same day): Phase 2c closed the gap above** -- the
real frame/GC-root contract described below is now built, not just
designed, and gated behind a per-function pre-scan so it costs nothing for
functions that don't need it. New trampolines in `vm.c` (declared in
`src/jit.h`): `diamond_jit_frame_size()` (a `sizeof`-based accessor, called
once per *compile*, so the byte count a compiled function reserves on its
own native stack self-syncs against `DiamondFrame`'s real layout rather
than duplicating a hardcoded constant), `diamond_jit_frame_push`/
`diamond_jit_frame_pop` (placement-construct/unlink a `DiamondFrame` into
caller-reserved stack space, mirroring `run_chunk`'s own one-frame-per-
activation model), and `diamond_jit_new_string` (wraps `allocate_string`,
the first trampoline that can actually trigger a collection). Whether a
given function needs any of this is decided by a **dry-run compile pass**:
`compile_body` runs once with a `dry_run` flag (checked in `emit_u8`,
which every other emitter bottoms out through, so it's a true no-op) purely
to learn whether the body contains `STRING` before the real pass emits its
prologue -- reusing the exact same decode/dispatch logic as the real
compile rather than a second, separately-maintained opcode-width table
that could drift out of sync with it.

Reached the actual target: `bench/object_hydration.di`'s `HydratedUser#
initialize` now compiles (previously always `jit_ineligible`), verified
against three configurations agreeing on output (`59000`/the dedicated
`tests/cases/jit_string_construct*` cases' `2800`): plain interpreted,
`DIAMOND_JIT=1`, and `DIAMOND_JIT=1` with `DIAMOND_STRESS_GC=1` (forces a
collection on *every* allocation) -- the last one is the sharpest available
proof that a register held live across a `STRING`-triggered allocation
(`self`, the Hash argument, and intermediate temporaries) actually survives
via the published frame, not by good luck. Measured honestly, this closes
the compile-eligibility gap but the speedup on this specific shape is
modest (~2-3% on `object_hydration.di`, see `bench/RESULTS.md`'s "Phase 2c"
section) because most of the per-call cost is inside the trampolines
themselves (Hash lookup, ivar write, type check, string allocation) doing
the same work the interpreter would -- the JIT only removes bytecode
dispatch overhead on top, not the underlying work, same pattern already
observed with Phase 2b's `hash_ivar_construct.di`. Full suite (1331 cases)
verified green under both debug and ASan/UBSan sanitizer builds, with
`DIAMOND_JIT` unset (default, zero behavior change) and with
`DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1` (compiles every
eligible function immediately, forces a collection on every allocation) --
no missed GC root surfaced. Confirmed no regression on Phase 2/2b's
existing allocation-free benchmarks from adding the pre-scan mechanism:
`int_arithmetic.di` still measures ~2.9x (previously ~2.95x, within noise),
`hash_ivar_construct.di`'s `Box` still measures ~6-7% (previously 4-7%).

Still out of scope, deliberately: `HASH`/`ARRAY`/`NEW` construction (the
pre-scan only checks for `STRING`), and skindicate's *real*
`User#initialize`, which additionally calls `super(attributes)` into
`ActiveRecord::Model#initialize` -- a call this JIT still can't compile
through. Whether that's worth a follow-on phase is not yet decided.

**Status (2026-09-13): Phase 2d adds SUPER call support and closes that
exact gap** -- `HASH` (needed for a `= {}` default argument) and
`DIAMOND_OP_SUPER` itself, plus a fix removing `EQUAL`/`NOT_EQUAL`'s own
bailout entirely (see below). skindicate's real `User#initialize`
(`skindicate.dia/lib/models/user.di`) now compiles, verified directly
against the real file (not just the `bench/object_hydration.di` mirror).

This phase looked structurally different from every one before it: SUPER
is the first opcode that can run arbitrary interpreted code with real
side effects, including raising a genuine exception -- breaking the
invariant every prior phase relied on ("any bailout is safe to handle by
discarding the whole attempt and re-running the function from scratch").
If SUPER succeeds and a *later* opcode then bails, restarting the whole
function would invoke the super() call a second time. Resolved without
full on-stack replacement: `DiamondJitFn`'s return convention became
3-way (`DIAMOND_VM_OK` / `DIAMOND_JIT_RETRY` / any other value is a real
`DiamondVmStatus` to propagate directly, never retried) with two shared
bailout stubs instead of one, selected by a compiler-tracked
`jc->has_called` flag; arithmetic opcodes (whose own bailouts have no
real status to propagate -- they need the interpreter's own bignum/raise
logic) are rejected outright at compile time once a call has already run,
since retrying is the only thing they can do and it's no longer safe.
`EQUAL`/`NOT_EQUAL`'s own bailout was eliminated (not just gated) by
adding a trampoline for `values_equal` (pure, always succeeds) --
required, not cosmetic, since `User#initialize`'s own `role`/`is_seed`
fields use `==` *after* the SUPER call. `DiamondJitFn` also gained a 6th
parameter/persistent register (`depth`, `JIT_DEPTH = REG_RBP`) so a
compiled function's own SUPER call still respects
`DIAMOND_MAX_CALL_DEPTH` -- this also fixed a latent gap in
`jit_call_or_interpret` itself, which previously dispatched straight to
compiled code with no depth check at all (harmless before Phase 2d, since
no compiled function could make a further call; a real gap the moment one
could).

Verified via three dedicated regression cases exercising exactly the
hazard this phase exists to prevent, not just "doesn't crash":
`jit_super_raise_propagates` (a superclass constructor that raises;
confirms the exception propagates and the raising call runs exactly
once, checked via a Hash-mutation counter -- a bug here would show 2, not
1), `jit_super_then_bail_propagates` (SUPER succeeds, then a *later*
`INDEX_GET` on a non-Hash bails; confirms the same "ran exactly once"
property when the failure is a different opcode entirely), and
`jit_super_chain` (6 levels of SUPER, all independently JIT-compiled,
confirming `depth` threads correctly across multiple compiled hops).

**Honest end-to-end result, and why it's smaller than object_hydration's
own ~2-3%**: `User#initialize` itself compiles and is proven correct, but
its own `super(attributes)` call reaches `ActiveRecord::Model#initialize`
(`packages/active_record/lib/active_record/model.di:36-49`), which
remains fully interpreted -- its own body loops over `attributes.keys()`
calling ordinary methods (`.keys()`, `.length()`) and uses `INDEX_GET` on
an *Array* (unsupported -- this JIT's `INDEX_GET` trampoline is Hash-only)
and `INDEX_SET` (not in the whitelist at all). Confirmed via
`DIAMOND_TRACE_JIT=1` against a real end-to-end `User.new` benchmark:
exactly 1 compiled function (`User#initialize`), not 2 -- `Model#
initialize` never compiles, and dominates the real per-call cost. Full
`User.new` end-to-end measured within noise of the interpreted baseline
(~1-3%, not the larger win `User#initialize`'s own compiled body would
suggest in isolation) -- see `bench/RESULTS.md`'s "Phase 2d" section.
**Deploying this build to skindicate today still would not show a
meaningful `/` improvement**, for a new and now well-understood reason
(previously "0 compiled functions"; now "the dominant cost is a still-
interpreted superclass method"). Making `Model#initialize` itself
JIT-eligible would need ordinary method-call/`INVOKE` support plus
Array `INDEX_GET`/`INDEX_SET` -- a materially larger feature than
anything built so far, not scoped or decided.

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
