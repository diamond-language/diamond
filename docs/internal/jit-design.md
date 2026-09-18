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

**Status (2026-09-13): Phase 2e found and fixed two real, already-shipped
correctness bugs while continuing to scope `Model#initialize`, and closed
the `INDEX_GET`/`INDEX_SET` gap above (Array support is now real) without
reaching `Model#initialize` itself.** Reading `DIAMOND_OP_INDEX_GET`'s and
the generic `DIAMOND_OP_LESS` family's *full* real case bodies for the
first time (Phase 2b only read enough of `INDEX_GET` to build a Hash-only
trampoline, and Phase 2d's `EQUAL`/`NOT_EQUAL` fix was written without
re-checking `EQUAL`'s own full case) surfaced:

1. **`EQUAL`/`NOT_EQUAL` silently skipped a `==` override.** The real case
   checks `invoke_operator_method` for a user-defined `==` on a
   `DIAMOND_OBJECT_INSTANCE` operand *before* falling back to
   `values_equal` -- Phase 2d's `diamond_jit_values_equal` called
   `values_equal` directly, unconditionally. A class defining `def ==`
   compared via JIT'd `EQUAL` silently got identity comparison instead.
   Confirmed as a real, catchable bug: `tests/cases/jit_equal_overload.di`
   (two equal-by-value `Point` instances compared inside a small JIT'd
   `compare(a, b) = a == b`) returns `10` with the fix and `0` without it
   -- verified directly by temporarily reverting the fix (`git stash`) and
   re-running the test against the old code before restoring it.
2. **`INDEX_GET`'s Hash-only trampoline became unsafe once "propagate"
   existed.** Before Phase 2d, every `INDEX_GET` bailout retried via full
   interpretation, which correctly checks the real case's Instance `[]`
   override -- accidentally safe. Once a bailout could "propagate" instead
   (`jc->has_called` already true from an earlier call), a JIT'd
   `INDEX_GET` on an Instance with a real `[]` override, reached *after*
   such a call, would have incorrectly propagated `TYPE_ERROR` rather than
   invoking the override. Confirmed the same way:
   `tests/cases/jit_index_get_overload_after_super.di` (a `Box` defining
   `def [](key)`, indexed from inside a JIT'd constructor right after its
   own `super()` call) returns `1050` with the fix and raises an uncaught
   `TypeError` without it.

**The fix, generalized into a standing rule**: stop hand-picking which
sub-cases of an opcode's real body to port into a trampoline -- extract
the *entire* real case verbatim (the pattern `diamond_jit_super_call`/
`diamond_jit_new_hash` already used) and make the interpreter's own case a
thin wrapper calling it, so the two can never drift apart again.
`diamond_jit_equal_general` (replacing `diamond_jit_values_equal`) and
`diamond_jit_index_get`/`diamond_jit_index_set` (replacing the old
`diamond_jit_hash_get`, and adding `INDEX_SET` support for the first
time) were built this way -- each a full Hash/String/Array/Instance-
overload extraction. Since the Instance-overload branch is structurally
present in every compiled occurrence regardless of the runtime receiver,
all three trampolines are compiled with `jc->has_called = true`
unconditionally (the same conservative, compile-time-only choice
`compile_super_call` already makes), and `diamond_jit_index_get`/`_set`
additionally set `jc->needs_frame = true` (their String/Array paths can
allocate). A new `tests/cases/jit_array_index.di` exercises Array
`INDEX_GET`/`INDEX_SET` directly as a real new capability, not just a bug
fix. Full suite (1340 cases) green under debug + ASan/UBSan, `DIAMOND_JIT`
unset and `DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1`; no
regression on Phase 2/2b/2c/2d's benchmarks or on the real end-to-end
`User.new` measurement (still within noise of interpreted, unchanged from
Phase 2d, since `Model#initialize` still doesn't compile).

**Still does not reach `Model#initialize`, and the reason is now a
genuinely different, bigger design question than "add more opcodes."**
Its loop condition (`while index < keys.length()`) compiles to the
*generic* `DIAMOND_OP_LESS` (never quickened to `LESS_INT` under
`DIAMOND_JIT=1` alone, since quickening is a separate opt-in flag), which
has its own Instance `<` override branch, structurally identical to
`EQUAL`'s -- so it too must conservatively set `jc->has_called = true` on
every compiled occurrence. But `jc->has_called` is compile-time-only and
monotonic: once true, every later opcode in the same compile is affected,
regardless of which runtime branch actually executes. Since this `LESS`
sits *inside* the loop, every iteration, `index += 1` (immediately after
it) is then rejected outright by the same "no arithmetic once a call
could have happened" rule this whole safety mechanism depends on.
Reaching `Model#initialize` would need either a genuinely different
**runtime-checked** has-a-call-actually-happened flag (generated code
itself branches on whether the override path was taken *this specific
execution*, not a fixed compile-time decision) in place of today's
compile-time-only `jc->has_called`, or enough local type inference to
prove a given `LESS` site's operands are always Int. Neither is scoped or
decided -- a materially bigger design change than anything in Phases
2-2e, since it would replace the core exception-safety mechanism Phase 2d
built rather than extend it.

**Resolved differently in Phase 3, below, without either of those two
options**: the actual blocker wasn't the *flag*, it was that these
opcodes' own edge cases had no resumable fallback at all -- once they
got one (a trampoline, exactly like every other call-capable opcode
already has), they stopped needing `jc->has_called` to be false in the
first place, so neither a runtime-checked flag nor whole-program type
inference turned out to be necessary.

### Phase 2f: compile-time Int return type for native scalar methods -- a real, narrower win, *not* a JIT-eligibility fix

Investigated whether the local-type-inference half of the paragraph
above was reachable as a smaller, standalone step. It was, but turned
out to matter for a different reason than expected. The actual gap:
`compile_binary_op` already picks `DIAMOND_OP_LESS_INT` at compile time
whenever both operands' `known_types` are already `DIAMOND_TYPE_INT`
(this is compile-time selection, unrelated to `DIAMOND_QUICKEN`'s own
separate runtime rewrite) -- `attributes.keys()`'s own destination
register already gets `known_types` published as `DIAMOND_TYPE_ARRAY`
(`"keys"` is in `collection_relay`'s own table), but `.length()` had no
equivalent publisher at all, leaving `keys.length()`'s own register
`TYPE_UNKNOWN` and forcing generic `LESS`. Fixed by a new `publish_
native_scalar_method_return_type` (`src/compiler.c`) consulting
`DIAMOND_NATIVE_METHODS`/`diamond_native_method_satisfies` (`src/vm.c`,
`src/vm.h`) -- a table already built for structural interface
conformance checking, already exposed across the compiler/vm boundary,
and already correctly declaring `.length()`/`.to_i()`/`.ord()`/etc.'s
own fixed scalar return types -- rather than inventing a second,
parallel list of the same facts.

**This does not move `Model#initialize` (or any `.length()`-bounded
loop) any closer to JIT-eligible**, and saying so plainly here to avoid
this section repeating the same "should have been enough" mistake the
original phase's own investigation made. Confirmed directly, empirically,
not just by re-reading the opcode whitelist: a method whose body reads
an ivar (`while i < @n`, no calls at all) still fails to compile --
`DIAMOND_OP_GET_IVAR` has no case in `compile_body`'s own switch (only
`SET_IVAR` does, which is why an `initialize`-shaped method that only
*writes* ivars from its own parameters was ever JIT-eligible in the
first place). A method that calls a native collection method
(`.length()`, `.keys()`, ...) fails for the separate, additional reason
that generic `DIAMOND_OP_INVOKE` -- dynamic dispatch by name, used for
*every* `.method()` call regardless of receiver type, not a dedicated
per-native-method opcode -- also has no case anywhere in `compile_body`.
Both are unconditional, whole-function bails via the same `default:
jc->bailed = true` every other unrecognized opcode already hits.

So the accurate picture, updated: reaching `Model#initialize`-shaped
code needs `has_called`'s own redesign (above) *and* real `GET_IVAR`
support *and* either real `INVOKE` support or hoisting/caching its
result outside the JIT'd region -- three separate, independently real
gaps, not one. This phase's own real, shipped value is narrower and
unconditional: any `.length()`/`.to_i()`/etc.-bounded comparison now
gets the cheaper `LESS_INT`/etc. dispatch from its very first execution
under the plain interpreter, with no dependence on `DIAMOND_QUICKEN`
ever observing enough int-int comparisons to rewrite it in place --
confirmed via `--dump-bytecode` showing identical `LESS_INT` output
with `DIAMOND_QUICKEN` unset entirely. A real, safe, always-on
interpreter improvement; not a JIT-coverage one.

**Correction (2026-09-15, same day)**: the phase as originally shipped
only actually fired for a receiver that already had a *registered type
set* (`compiler->known_type_sets[receiver]>=0`) -- true for Array/Hash
locals (which always get one via `record_collection_type_set`, needed
for element-type tracking regardless), but never true for a plain
`String` local (`parse_string` sets only the scalar `known_types[reg]`
tag, no type set -- there's no element type to track). Confirmed
directly: `while i < s.length()` for a plain String `s` still compiled
to generic `LESS`, not `LESS_INT`, despite the identically-shaped Array
case working. Found investigating a related LSP hover gap (see
`docs/roadmap.md`'s "Improve receiver-aware tooling"). Fixed by widening
`publish_native_scalar_method_return_type` to fall back to the
receiver's own plain scalar `known_types[]` tag when no type set is
registered at all (`receiver_set_index<0`) -- purely additive; the
type-set branch's own existing behavior for Array/Hash is unchanged.
`tests/cases/jit_length_less_int_string.di` is the regression case.

### Phase 2g: `DIAMOND_OP_GET_IVAR` support -- closes one of the three gaps above

Added a JIT trampoline, `diamond_jit_get_ivar` (`src/vm.c`, declared
`src/jit.h`), and a `compile_get_ivar`/switch case (`src/jit.c`)
mirroring `SET_IVAR`'s own existing treatment exactly: same field-cache
lookup, same "no `jc->needs_frame`/`jc->has_called`" treatment (a plain
ivar read can never invoke user code or allocate -- there is no
operator-overload equivalent for field access), same 5-argument
register-only calling convention (no stack args needed, simpler than
`INDEX_GET`'s 7-argument shape). The interpreter's own `DIAMOND_OP_
GET_IVAR` case is now a thin wrapper around the same trampoline,
matching `SET_IVAR`'s own existing anti-duplication shape rather than
adding a second, independent copy of the read logic.

Verified empirically, not just by re-reading the whitelist: a method
that reads one of its own ivars in a loop with no other calls (`while
i < 100000; v = @value; i = i + 1; end`) failed to compile before this
change (`jit_ineligible` set, no counter change -- compile-time
rejection isn't counted as a runtime `jit_bailouts`, only a runtime
`DIAMOND_JIT_RETRY` is) and compiles cleanly after
(`tests/cases/jit_get_ivar.*`, asserting `DIAMOND_TRACE_JIT`'s own
`jit: 2 compiled function(s)` line).

**Still doesn't reach `Model#initialize`-shaped code on its own**: that
method's loop body does `INDEX_GET`/`INDEX_SET` (real Hash access)
*before* any hypothetical ivar read, and those trampolines still set
`jc->has_called` unconditionally, which is still what blocks `ADD_INT`
afterward. Closing this gap and leaving the other two (`has_called`,
generic `INVOKE`) means a method that *only* reads/writes its own
ivars plus does int arithmetic is now fully JIT-eligible, which was not
true before -- a real, if narrow, class of methods (simple accessors,
counters, accumulators) -- but nothing that also calls another method
or does Hash/Array indexing is any closer than the Phase 2f picture
already described.

### Phase 3: `has_called` no longer blocks integer arithmetic/comparison -- closes a second of the three gaps above

Investigated whether reaching `Model#initialize`-shaped code needed a
genuinely new mechanism -- a runtime-checked replacement for
`jc->has_called`'s compile-time-only, monotonic flag, or full mid-function
deoptimization -- as the Phase 2e status note above speculated. It didn't.

The real crux, found by reading `ADD_INT`/`SUBTRACT_INT`/`MULTIPLY_INT`/
`DIVIDE_INT`/`LESS_INT`'s full interpreter case bodies for the first time
(previous phases only read enough to build the fast native path): these
opcodes are **self-modifying** the same way method dispatch is
(`vm->quickening` rewrites a generic `ADD`/`LESS`/etc. to its `_INT` form
in place after enough int-int observations, and deopts back to generic on
a later non-Int operand), and their overflow/div-by-zero/`INT64_MIN`
cases call real bignum-promotion logic (`diamond_bignum_add`, ...), not a
fixed-width error. Before this phase, none of that had anywhere safe to
go once a call had already run: retrying would re-invoke it, and
"propagate" only forwards an already-real `DiamondVmStatus` from a
trampoline call that already happened -- there was no trampoline on the
fast native-arithmetic path at all, so there was nothing to propagate.
A runtime-checked flag alone would not have fixed this: the actual gap
was that these opcodes' edge cases had no *resumable* fallback, the same
shape every other call-capable opcode here already has.

**The fix**: extract the *entire* real slow-path body for each opcode
family into a shared function -- `int_arith_slow` (`ADD`/`SUBTRACT`/
`MULTIPLY`/`DIVIDE` and their `_INT` forms) and `compare_int_slow`
(`LESS`/`LESS_EQUAL`/`GREATER`/`GREATER_EQUAL` and their `_INT` forms),
both in `src/vm.c` -- covering the deopt-to-generic bytecode rewrite,
bignum promotion, Float/String/Instance-override/Time dispatch, and
(for arithmetic) division-by-zero/`INT64_MIN`, reusing `add_fallback`
for `ADD`'s own dispatch rather than duplicating its String-concat/Time-
offset cases. `run_chunk`'s own case for each opcode family is now a
thin wrapper: a fast, purely-native path for two confirmed-int,
non-overflowing operands (unchanged), falling through to the shared
function for everything else -- the same anti-duplication discipline
Phase 2e established for `EQUAL`/`INDEX_GET`. Two new JIT trampolines,
`diamond_jit_arith_slow`/`diamond_jit_compare_slow` (`src/jit.h`),
wrap these same functions for `compile_binary_int_op` (`src/jit.c`):
every bail condition (a non-Int operand, overflow, division by zero,
`INT64_MIN`/`-1`) now calls the matching trampoline and either continues
inline with the written result or bails via the trampoline's own real
status -- the same "call a trampoline, never retry" shape `SET_IVAR`/
`GET_IVAR`/`INDEX_GET`/`SET`/`EQUAL` already use. Since this never needs
"retry the whole function" any more, the outright compile-time rejection
`if (jc->has_called) { jc->bailed = true; return; }` is gone: these five
opcodes compile whether or not a call has already run. Because the
trampolines' own Instance-operator-override branch can genuinely invoke
arbitrary user code, compiling any of them at all still sets
`jc->has_called = true` unconditionally -- the same conservative,
compile-time-only choice `compile_equal_op` already makes.

Verified empirically against the exact shape the Phase 2f/2g notes
above named as still blocked: a Hash argument's `INDEX_GET` (sets
`jc->has_called`) followed by a provably-Int loop counter's own
`ADD_INT`/`LESS_INT` failed to compile before this change (`jit: 0
compiled function(s)`, confirmed by checking out the pre-Phase-3 tree
and rerunning the identical program) and compiles cleanly after
(`tests/cases/jit_int_arith_after_index_get.*`, asserting `jit: 1
compiled function(s)`). A second case
(`tests/cases/jit_arith_overflow_after_index_get.*`) confirms the
bignum-promotion path itself computes correctly when reached through
the new trampoline *after* `has_called` is already true, under both
plain `DIAMOND_JIT=1` and `DIAMOND_JIT=1 DIAMOND_STRESS_GC=1` (the Hash
argument stays correctly published across the allocating bignum-promote
call). Full suite (1548 cases) green under debug, under ASan/UBSan, and
under `DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1`
(compiles every eligible function immediately, forces a collection on
every allocation) -- no missed GC root, no regression on Phase 2/2b/2c/
2d/2e's existing benchmarks. Extensive pre-existing interpreter coverage
for these exact deopt/bignum/division/operator-override paths (`tests/
cases/add_int_deopt_*`, `operator_deopt_gap_*`, `spaceship_bignum.di`,
...) passed unchanged throughout, giving strong confidence the
extraction preserved the interpreter's own behavior exactly rather than
introducing subtle drift.

**Still does not reach `Model#initialize`-shaped code on its own**:
generic `DIAMOND_OP_INVOKE` (dynamic dispatch by name, used for every
`.method()` call regardless of receiver type) still has no case in
`compile_body` at all -- the one remaining gap of the original three,
and the only one left. A method that also calls `.keys()`/`.length()`-
style native methods, or any user method, is no closer to JIT-eligible
than the Phase 2f/2g picture already described; only the narrower class
of methods that read/write ivars and do int arithmetic/comparisons
*after* a call has already run (rather than only before) is newly
reachable.

### Phase 4: `dup`/`freeze`/`frozen?` support -- reaches `Model#initialize` as it exists today, without attempting generic `INVOKE`

Investigated what closing the last gap ("generic `DIAMOND_OP_INVOKE`")
would actually take. Two findings from reading the real code (not
assuming from this document's own prior wording) substantially reframed
the task before any codegen was written:

1. **`DIAMOND_OP_INVOKE`'s real case body is ~2740 lines**
   (`src/vm.c:17652-20392` as of this phase), not one extractable case
   like `EQUAL`/`INDEX_GET` were -- it covers the full native-method
   dispatch table for every builtin type (String/Array/Hash/Symbol/Int/
   Float/Time/Regexp/File/Socket/SQLite3/Postgres/MySQL/Process/Thread/
   Channel/Supervisor/...) plus Instance method dispatch with inline
   caching. "Generic `INVOKE` support" as a single phase was never a
   realistic scope; this document's own earlier wording undersold that.
2. **The real motivating target had already changed.** `packages/
   active_record/lib/active_record/model.di`'s `Model#initialize` no
   longer loops over `attributes.keys()`/`.length()` the way Phase 2d/2e's
   own notes describe -- the 2026-09-15 ORM hydration hotspot fix (already
   on record) rewrote it to `@attributes = attributes.dup()`, ~20x faster
   on its own terms. Confirmed via `--dump-bytecode` that its *entire*
   current body is `ARGUMENT_PROVIDED`, `JUMP_IF_TRUE`, `HASH`, `MOVE`,
   `CHECK_TYPE`, one `INVOKE` (`.dup()`), `SET_IVAR` x2, `HASH`, `RETURN`
   -- every opcode already JIT-supported except that single `INVOKE`.
   Also confirmed the real call site (`skindicate.dia/lib/models/
   user.di`: `User.new(row)`) compiles to plain `DIAMOND_OP_NEW`, not
   `NEW_KEYWORDS` (which bypasses JIT dispatch entirely via a synthetic
   `run_chunk` call rather than `jit_call_or_interpret`) -- so a JIT'd
   `initialize` really would be reached by real construction.

So the actual next step wasn't "generic `INVOKE`" -- it was the small,
well-bounded slice already sitting at the very top of that giant case:
`dup`/`tap`/`freeze`/`frozen?`/`public_send`, a receiver-kind-agnostic
block checked before any per-type or Instance dispatch
(`src/vm.c:17664-17796` as of Phase 3). Of those five, `dup`/`freeze`/
`frozen?` are pure, deterministic, native-kind-based dispatch that can
**never** invoke arbitrary user code (no operator-override equivalent) --
`tap`/`public_send` do (a Closure call, or dispatch by a *runtime*
string), real SUPER-level complexity not needed for this target and not
attempted.

**Implementation**: three shared functions in `src/vm.c` --
`diamond_jit_dup`/`diamond_jit_freeze`/`diamond_jit_frozen(DiamondVm *vm,
const DiamondValue *receiver, DiamondValue *out) -> DiamondVmStatus`,
verbatim extractions of the interpreter's own former inline blocks, with
the `dup_defined` gate (Array/Hash/String/Symbol, or any non-Object
primitive) folded directly into each function -- a receiver that doesn't
qualify (an Instance, or any other Object kind) returns a plain nonzero
status. The interpreter's own three `DIAMOND_OP_INVOKE` blocks are now
thin wrappers around these, matching the anti-duplication discipline
Phase 2e/2g already established. A new `compile_body` case for
`DIAMOND_OP_INVOKE` only (`INVOKE_MONO`/`INVOKE_TYPED` deliberately
excluded -- this call shape never produces either) recognizes exactly
these three method names with `argc == 0`, compiling a direct 3-register
trampoline call (`vm`/`&registers[recv]`/`&registers[dest]`, no stack
args -- simpler than every prior trampoline here) followed by
`emit_bail_if_al_nonzero`. `jc->needs_frame = true` only when compiling
`dup` (the only one that can allocate, via `allocate_array`/
`allocate_hash`). None of the three ever sets `jc->has_called` (they can
never invoke arbitrary code) -- **but, by the same reasoning that makes
that safe, compiling one at all requires `jc->has_called` to already be
false**, since a receiver that turns out not to be `dup_defined` at
runtime can only be handled by a safe *retry*, never a *propagate*.
Anything else -- wrong name, nonzero `argc`, or reached once `has_called`
is already true -- bails the whole function, the same structural
treatment as any other unsupported construct.

**A real, sharper-than-expected consequence of that last point, found
empirically**: Phase 3 made `ADD_INT`/`SUBTRACT_INT`/`MULTIPLY_INT`/
`DIVIDE_INT`/`LESS_INT` set `jc->has_called = true` unconditionally the
moment any of them compile at all (their own Instance-operator-override
branch is the reason, see Phase 3 above) -- which means **any loop
condition using an ordinary integer comparison** (`while index < n`,
`LESS_INT`) already sets `has_called` before a `dup`/`freeze`/`frozen?`
call reached later in the same function, and that call is then correctly
rejected by this phase's own gate. Confirmed directly: a `dup()` call
placed *after* a `while index < 3` loop condition in the same function
does not compile, while the exact same call placed under a plain `if`
(no loop, no preceding comparison) does. `Model#initialize` itself is
unaffected -- its own body has no comparison or other call before its one
`.dup()` -- but this means the practical reach of Phase 4 is narrower
than "any dup/freeze/frozen? call, JIT-wide": specifically, one reached
after any arithmetic comparison, `EQUAL`, `INDEX_GET`/`SET`, or `SUPER`
earlier in the same function will not compile. Not fixed here -- doing so
would need the same kind of resumable-fallback redesign Phase 3 already
gave arithmetic, generalized to `dup`/`freeze`/`frozen?`'s own "not
`dup_defined`" case, which is a real but small follow-on, not attempted
in this phase.

**Verified**: `Model#initialize`'s exact current shape now compiles
(`DIAMOND_TRACE_JIT=1` showing `jit: 1 compiled function(s)` where it
showed `0` before this phase, confirmed by checking out the pre-Phase-4
tree and rerunning the identical program) --
`tests/cases/jit_invoke_dup.di` also confirms `dup` produces a real,
independent copy (mutating the original Hash after construction doesn't
affect the constructed instance). `dup`/`freeze`/`frozen?` verified
correct across every `dup_defined` receiver kind (Int, String, Array,
Hash) under `DIAMOND_JIT=1 DIAMOND_STRESS_GC=1`
(`tests/cases/jit_dup_freeze_frozen_kinds.di`). The "not `dup_defined` at
runtime" fallback verified directly: a `dup()` call compiled against an
Instance receiver correctly reports one runtime bailout and falls back
to full, correct interpretation rather than mishandling it
(`tests/cases/jit_dup_instance_fallback.di`). Full suite (1551 cases)
green under debug, ASan/UBSan, and `DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1
DIAMOND_STRESS_GC=1` -- the sharpest available check for `dup`'s own
allocation, since its live registers (the Hash argument, in particular)
must survive a collection forced on every allocation.

**Honest end-to-end measurement**: `bench/object_hydration.di` gained a
new `HydratedModel`/`run_dup()` pair mirroring `Model#initialize`'s exact
current shape (the pre-existing `HydratedUser`/`run()` benchmark is
unchanged -- it still demonstrates a *different*, still-open gap:
string-literal Hash keys compiling to a fresh `DIAMOND_OP_STRING`
construction per call, unrelated to this phase). Direct A/B release-build
timing (isolated `run_dup()` alone, `DIAMOND_JIT_THRESHOLD=1`): ~330-345ms
interpreted vs. ~300-320ms JIT'd, a real but modest ~8-10% end-to-end win
-- most of the per-call cost is inside `diamond_jit_dup`'s own Hash-copy
work either way (same cost whether reached via the interpreter or a
trampoline call), matching the exact pattern Phase 2c's own
`object_hydration.di` measurement (~2-3%) and Phase 2b's
`hash_ivar_construct.di` (~4-8%) already established: this JIT tier
removes bytecode dispatch overhead, not the underlying native work.

### Phase 5: generic (non-`_INT`) arithmetic/comparison, and a real pre-existing bug found along the way

Investigating "why is `bench/hash_ops.di` slow" (`Hash` itself turned out
not to be -- see `bench/RESULTS.md`'s own correction) found the real
answer: `total = total + values[index]` never JIT-compiled at all, at any
threshold, because a Hash's values have no static type, so that `ADD` can
never be proven `Int` and stays generic -- and `src/jit.c` had **zero
cases for any generic arithmetic or comparison opcode** at all (`ADD`/
`SUBTRACT`/`MULTIPLY`/`DIVIDE`/`LESS`/`LESS_EQUAL`/`GREATER`/
`GREATER_EQUAL`), only their runtime-quickened `_INT` forms -- and even
then, only `ADD_INT`/`SUBTRACT_INT`/`MULTIPLY_INT`/`DIVIDE_INT`/
`LESS_INT`; `LESS_EQUAL_INT`/`GREATER_INT`/`GREATER_EQUAL_INT` were never
added either. One generic opcode anywhere in a function's body bails the
*entire function* out of JIT eligibility. This is the exact same root
cause as `int_arithmetic_dynamic.di`'s own ~2.8x gap against
`int_arithmetic.di`: any value the compiler can't statically prove `Int`
(an untyped parameter, a Hash/Array element) used in arithmetic anywhere
disables the JIT for its whole containing function.

Far more tractable than it looked: Phase 3's shared slow-path functions,
`int_arith_slow`/`compare_int_slow` (`src/vm.c`), already accept *either*
a generic or an `_INT` opcode -- they only attempt the deopt-to-generic
bytecode rewrite when the opcode passed in actually *is* one of the
`_INT` forms; called with an already-generic opcode, they skip that step
and dispatch directly. Confirmed by re-reading both functions before
writing any code: **zero `src/vm.c` changes were needed**. The fast
native path's own kind check (`emit_check_kind_int_or_jump`: "is this
`DIAMOND_VALUE_INT`") was already exactly as correct for a generic
opcode as for an `_INT` one. The whole change was in `src/jit.c`: a new
`emit_setcc_al` (generalizing the previously `LESS_INT`-only
`emit_setl_al` to also cover `SETLE`/`SETG`/`SETGE`), extending
`compile_binary_int_op`'s three branches to recognize both the generic
and `_INT` form of each opcode, and 11 new `case` labels in
`compile_body`'s switch routing them all through the same codegen. No
change to `jc->has_called = true` (already set unconditionally, already
correctly covering every new opcode too).

**A real, already-deployed bug found empirically while testing this, not
by inspection**: `tests/run.sh`'s full suite segfaulted inside
`diamond_jit_frame_pop`. Root cause: `emit_epilogue_propagate`'s own
comment claimed "`jc->has_called` is only ever set alongside
`jc->needs_frame` (SUPER sets both)" and unconditionally popped a
`DiamondFrame` on that belief. That was already false as of Phase 2e --
`compile_equal_op`'s general case sets `has_called` without `needs_frame`
(`EQUAL`'s override branch never allocates) -- and every one of Phase
3/5's arithmetic/comparison opcodes does the same. A function whose only
`has_called`-setting opcode is one of those, followed by a *different*
opcode (`SET_IVAR`, `INDEX_GET`/`SET`, ...) that genuinely needs to
propagate a real error, reaches `emit_epilogue_propagate` with no frame
ever having been pushed -- popping one anyway pops whatever's actually on
top of `vm->frames` (the caller's, or an even-more-outer one), corrupting
the frame chain. Not a crash at that call site itself, but on some later,
unrelated frame operation once the corruption is actually observed --
which is why this shipped unnoticed through Phase 3/4's own full
verification passes (ASan/UBSan and `DIAMOND_STRESS_GC` included) and
reached production: nothing in that testing repeated the exact "call a
function combining a has_called-without-needs_frame opcode with a later
genuinely-propagating one" shape enough times in one process for the
corruption to actually surface as an observable crash. Confirmed
directly: **the identical crash reproduces on the already-deployed
pre-Phase-5 commit** using only `EQUAL` (Phase 2e, no Phase 5 code
involved at all) followed by a `SET_IVAR` on a frozen instance, called
repeatedly in a loop -- Phase 5 didn't introduce this bug, it just
happened to be the first work that tripped over it while testing.

Fixed by guarding `emit_epilogue_propagate`'s own frame-pop behind
`jc->needs_frame`, exactly mirroring `emit_epilogue`'s own success/retry
exit, which already had this guard correctly. Regression test:
`tests/cases/jit_propagate_without_frame.di` (the `EQUAL`-only
reproduction, deliberately independent of any Phase 5 opcode, to prove
the fix addresses the real, general defect rather than papering over one
specific new trigger). Also fixed: `tests/cases/jit_ineligible_function_
falls_back.di` used `value * 2` (untyped parameter) as its own "still
correctly falls back to interpretation" example -- Phase 5 made that
construct JIT-eligible, so it no longer demonstrated what the test's own
name promised; replaced with a genuinely still-unsupported construct
(a user-defined Instance method call, since generic `INVOKE` remains
entirely unattempted).

**Verified**: full suite (1552 cases) green under debug, ASan/UBSan, and
`DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1` (the sharpest
available check for a frame/GC-root defect, which is exactly the class
of bug this phase found and fixed). Correctness spot-checked directly
across the fast native-Int path, bignum overflow, String concatenation,
Float mixing, an Instance operator override (`+`/`<`), division by zero,
and the three previously-entirely-unsupported `_INT` comparison forms
reached via `DIAMOND_QUICKEN` -- all correct. `bench/hash_ops.di` and
`bench/int_arithmetic_dynamic.di` (release build, `DIAMOND_JIT_
THRESHOLD=1`): `hash_ops` ~0.057s interpreted -> ~0.038s JIT'd (~1.5x);
`int_arithmetic_dynamic` ~1.07s -> ~0.51s (~2.1x), closing most of its
own ~2.8x gap against the fully-typed `int_arithmetic.di`.

**Still doesn't reach every function**: a value the JIT itself can't
prove is Int at *runtime* either (a real String, Instance, Float, ...)
still correctly falls to the slow trampoline every time, at real
per-call cost -- this phase closes the "provably-untyped-but-actually-
always-Int" gap, not the genuinely-polymorphic-arithmetic case, which
was never slow to begin with (it already needed the generic dispatch
either way). Generic `INVOKE` (native per-type methods, Instance method
dispatch, `tap`/`public_send`) remains completely unattempted -- see
Phase 4's own note on its real ~2740-line scope.

### Phase 6: `dup`/`freeze`/`frozen?` on an Instance, the common (no-override) case

A small, deliberately narrow follow-on to Phase 4's own documented
limitation: `dup`/`freeze`/`frozen?` previously only ever ran their JIT
trampoline for a non-Instance receiver (Array/Hash/String/Symbol/
primitive) -- any Instance receiver fell back to full interpretation via
the "not `dup_defined`" retry path, *every single time*, since the
trampolines had no way to know whether that Instance's own class defined
a same-named override.

Reading the interpreter's real Instance-specific `dup`/`freeze`/`frozen?`
handling (`src/vm.c`, reached only via a separate code path from the
Array/Hash/primitive one Phase 4 already covered) found this case is
actually just as simple and safe once a real user override is ruled out
first: `lookup_method(owner, instance->class, name, length)` is a pure,
deterministic table lookup -- never arbitrary code -- and when it comes
back empty, the real behavior is exactly the same shape as the non-
Instance case (`dup`: allocate a field-for-field copy, including the
source's current shape; `freeze`/`frozen?`: read or set the one flag).
Only when a class *does* define its own override does this need real
method dispatch, which remains entirely unattempted, same as before.

Extended `diamond_jit_dup`/`diamond_jit_freeze`/`diamond_jit_frozen`
(`src/vm.c`) with an Instance branch doing exactly this: check
`lookup_method` first, return the existing "not eligible, fall back"
status if an override exists, otherwise perform the real default
behavior directly. **Zero `src/jit.c` changes** -- the JIT-generated code
already calls these same trampolines unconditionally and already treats
any nonzero status generically as "not eligible, retry," so widening what
the trampoline itself can handle needed no new codegen at all. The
interpreter's own separate Instance-specific block was refactored to call
these same functions too (removing the now-duplicated inline copy of the
same logic), matching the anti-duplication discipline established since
Phase 2e.

Verified directly: a `Widget` instance with no override now compiles
`dup`/`freeze`/`frozen?` calls with zero bailouts and produces a real,
independent copy (mutating the copy doesn't affect the original) --
`tests/cases/jit_instance_dup_freeze_frozen.di`, including under
`DIAMOND_STRESS_GC=1` (the sharpest check for `allocate_instance`'s own
GC-root publishing in this exact path). A class that *does* override
`dup` (`tests/cases/jit_dup_instance_fallback.di`, updated -- it
previously demonstrated the old "any Instance always falls back"
behavior, which this phase specifically changes) still correctly falls
back to real interpretation and calls the real override, confirmed via
exactly one runtime bailout. Full suite (1553 cases) green under debug,
ASan/UBSan, and `DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1
DIAMOND_STRESS_GC=1`.

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
