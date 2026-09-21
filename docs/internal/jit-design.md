# JIT design: deoptimization and GC-root contracts

This document records the JIT's design history and the contracts for
interoperating with the interpreter, GC, and threading model. The initial
contract predates code generation; later sections document the implemented
phases. See [the roadmap](../roadmap.md#native-code-execution) for open work
and [benchmark results](../../bench/RESULTS.md) for measurements.

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

### Phase 7: `self.method()` dynamic dispatch -- a real, narrow slice of generic `INVOKE`

"Generic `DIAMOND_OP_INVOKE` support" had been the one named-but-untouched
gap since Phase 2g: any dynamic method call anywhere in a function's body
-- including a plain `self.other_method()`, the single most ordinary
shape of method call there is -- bailed the whole containing function out
of JIT eligibility. The honest reason nobody had attempted it: the
interpreter's `DIAMOND_OP_INVOKE`/`INVOKE_MONO`/`INVOKE_TYPED` case
(`src/vm.c`) is one monolithic ~2695-line block covering every receiver
kind's native method surface (`Int`/`Float`/`String`/`Array`/`Hash`/
`Symbol`/`Time`/`Regexp`/.../`Instance`), never extracted into a callable
helper the way arithmetic's slow path was in Phase 3. Attempting the
whole thing in one phase was correctly ruled out twice before (Phase 4's
own reframing, and a paused 2026-09-15 research pass).

That 2026-09-15 pass had already found the right *shape* of narrow slice
-- `self.method()` only -- and got it "approved-pending," paused before
implementation for lack of a motivating workload. Reviving it here found
a real error in its own reasoning, caught during re-verification before
any code was written: its core justification ("the JIT only ever
compiles functions with `owner_class` set, so `registers[0]` is
guaranteed an Instance, no runtime check needed") is false as stated --
`diamond_jit_try_compile` has zero `owner_class` gating, and
`jit_call_or_interpret` tier-up-compiles *any* `DiamondFunction*`
uniformly, methods and plain functions alike. Skipping the runtime check
on that basis would have been a real, shipped bug.

The actual sound invariant is narrower but still solid: `invoke_resolved_
method_helper` (`src/vm.c`) unconditionally sets `args[0] = self_value`
for *every* real method call, and that function is the *only* way a
function with a real `owner_class` (a genuine class index, not the
`UINT8_MAX`/`UINT8_MAX-1`/`UINT8_MAX-2` plain-function/module-method/
closure sentinels) is ever entered. So for a genuine class method,
`recv==0` really is always an Instance -- not because of anything about
*which functions the JIT compiles*, but because of *how a real method is
always called*. This phase's own trampoline keeps a cheap defensive
runtime check regardless (matching `diamond_jit_super_call`'s own
existing belt-and-suspenders precedent), so an error in this reasoning
would fail safe as a plain `TYPE_ERROR`, never a crash -- the compile-time
gate is an eligibility decision, not the only thing standing between this
code and memory corruption.

**Implementation**, mirroring `DIAMOND_OP_SUPER`'s already-shipped shape
exactly: `diamond_jit_invoke_instance` (`src/vm.c`) is a full extraction
of the interpreter's own Instance-dispatch tail -- the `tap`/`dup`/
`freeze`/`frozen?`/`respond_to?`/`public_send` universal-method
interception (gated on `lookup_method` first, so a real override wins,
same as Phase 6 relies on), exception-instance `message`/`cause`/
`backtrace`, the `INVOKE`<->`INVOKE_MONO` inline-cache check and
self-rewrite, `method_missing` fallback, visibility checks, and the final
`invoke_resolved_method_helper` call -- moved verbatim, not rewritten.
Kept receiver-position-general (`recv` is a real parameter) so the
interpreter's own case, which still needs to handle any receiver
register and `INVOKE_TYPED`, calls it unchanged; only `src/jit.c`'s new
`compile_invoke_self` restricts itself to `recv==0` in a function whose
`owner_class` proves the invariant above. The interpreter's own case is
now a five-line thin wrapper, exactly like `DIAMOND_OP_SUPER`'s.

`compile_invoke_self` sets both `jc->needs_frame` and `jc->has_called`
unconditionally (a real method call can invoke arbitrary user code and
allocate), mirroring `compile_super_call`'s identical justification, and
emits a 13-argument trampoline call using the same push-in-reverse-order
stack-argument convention `compile_super_call` established, just with 4
more stack slots. `DIAMOND_OP_INVOKE_TYPED` has deliberately no case at
all (falls to `compile_body`'s own `default:` bail), matching
`diamond_jit_super_call`'s own long-standing restriction for `SUPER` --
the shared trampoline still supports it for the interpreter's sake via a
real `type_argument_count`/`type_arguments`, the JIT-side caller just
never supplies anything but the "no type arguments" constants.

Phase 4/6's own narrower dup/freeze/frozen? trampoline call (still the
only path for a *non-self* receiver) needed to merge into the same
switch case rather than duplicate it, since C forbids two `case` labels
for one opcode value: the `recv==0`-eligible self path is tried first
(a strict superset for that receiver -- any method name, not just three,
and correctly dispatches through a real override instead of always
retrying), falling through to the original Phase 4/6 logic unchanged
for everything else. Confirmed this is a genuine no-op for the existing
Phase 4/6 test corpus: an un-overridden `dup`/`freeze`/`frozen?` call
never actually reaches `INVOKE_MONO` at the bytecode level in the first
place (it's intercepted before the interpreter's own inline-cache logic
that would rewrite it), so the merge changes no observable behavior for
any pre-existing test.

**Out of scope, explicitly**: any non-`self` receiver (no compile-time
proof exists without a runtime guard/deopt mechanism this JIT doesn't
have yet -- see "Deopt trigger" below for why a runtime type-dispatch
branch mid-JIT-function isn't buildable today); `DIAMOND_OP_INVOKE_TYPED`;
module-method (`UINT8_MAX-1`) and closure (`UINT8_MAX-2`) `owner_class`
self-calls (conservative on purpose -- these may carry the same
guarantee but weren't independently verified this phase); the ~2500-line
native-type method surface (still bails the whole containing function,
same as always).

Verified: new `tests/cases/jit_invoke_self*` cases covering a
`self.other_method()` call in a loop now compiling (confirmed against a
real pre-change bailout via `git stash`: 2 compiled functions before, 4
after, on the exact same file), `self.dup()`/`self.tap{}`/`self.freeze`/
`self.frozen?`/`self.respond_to?`/`self.public_send` all still correct
post-extraction (confirming "extract the whole tail verbatim" was
right), `self.dup()` through a real override now compiling with zero
bailouts (previously always fell back to interpretation for that one
call), a same-function non-self receiver call confirming the containing
function still correctly bails as a whole, and `self.method[Type]()`
still bailing. Full suite (1557 cases) green under debug, ASan/UBSan, and
`DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_STRESS_GC=1`. Real,
measured win: `bench/jit_invoke_self.di` (a `self.method()` call inside
a hot loop, the whole loop now compiling with zero bailouts) ~35% faster
JIT'd (0.75s -> 0.49s, release build, stable across repeated runs).

### Phase 8 (research only, not implemented): non-`self` `INVOKE` -- a real dead end, precisely characterized

Following Phase 7, a re-profile of skindicate's own front page (`/`, temporary
`Time.monotonic()` instrumentation, reverted after measuring -- not committed)
found its actual current bottleneck isn't template rendering or SQL execution
at all, overturning this doc's own earlier "grid/card template rendering"
finding: total request time is now ~38-40ms (down from the ~95ms this
session's earlier div-escape/Hash#dup fixes started from), of which the grid
step is still ~30ms (~79%) but template rendering itself is only ~4ms of
that. The other ~26ms is skindicate's own `uploaders_for`/`platforms_for`/
`screenshots_for` batch loaders -- and *not* their SQL, either: raw SQLite
execution across the whole request sums to ~1.7ms (measured directly from
the query logger), while `uploaders_for` alone costs ~11.5ms for a query
that executes in ~0.4ms. The gap is ActiveRecord/Arel's own query-building
and rendering (`to_sql` regenerated fresh every call, no caching; checked
`render`/`render_expression` directly for an algorithmic culprit like O(n^2)
string concatenation -- found none, a 100-value `IN` clause builds via a
clean O(n) array-join), made of many small Instance-to-Instance method calls
(`visitor.render_expression(...)`, `expression.left()`, `query.projections()`,
...) that are almost never `self`-calls, so Phase 7 doesn't touch any of it.
That motivated asking directly: is a *non-self* `INVOKE` slice buildable,
short of the full ~2500-line native-type surface?

**The correctness half of this turned out to already be solved**, and more
cheaply than the "Deopt trigger" section above (written before any JIT phase
existed) assumed. That section speculated a JIT'd function would need a new
"check assumptions, bail to `run_chunk` from the top" entry guard; what
actually shipped across Phases 3-7 is narrower and already sufficient: *any*
bail site reached while `jc->has_called` is still false is unconditionally
safe to retry via full interpretation, with no new mechanism, **provided
nothing with a real, non-idempotent side effect has already committed**.
Checked this directly against the one opcode that looked likely to violate
it -- `SET_IVAR` (a real heap mutation, and notably the one call-adjacent
opcode that does *not* set `jc->has_called`, `src/jit.c`'s `compile_set_ivar`)
-- and confirmed `diamond_jit_set_ivar` (`src/vm.c`) returns every non-OK
status *before* its one mutation (`instance->fields[field] = *value`); the
only nonzero return after that point is `gc_write_barrier` failing
(`DIAMOND_VM_OUT_OF_MEMORY`), an existing, already-accepted, OOM-only risk
predating this investigation entirely, not something new to it. So: extending
`diamond_jit_invoke_instance` to accept *any* receiver register (not just
`recv==0`), gated only on `!jc->has_called` rather than `compile_invoke_self`'s
`owner_class` check, would be **correct** with zero new deopt or GC-root
design -- a real, substantially smaller undertaking than the full native-type
surface, reusing 100% of Phase 7's own machinery.

**The performance half is the actual, unresolved blocker.** `dup`/`freeze`/
`frozen?` are safe to compile unconditionally because their trampolines have
*real* behavior for every receiver kind -- they never just bail for "wrong
kind." A generic method name has no such property: `diamond_jit_invoke_
instance` only knows how to dispatch on a real Instance, so any receiver that
turns out to be a String/Array/Hash/primitive at runtime bails via the exact
retry path above, unconditionally, every single call. For a function whose
non-self `INVOKE` receiver is *typically* not an Instance (a native-type
method call), this would compile the function (passing `diamond_jit_try_
compile`'s dry run) and then retry-bail on *every* invocation forever --
strictly worse than today, where such a function simply never gets a
`jit_code` pointer attached and every call goes straight to `run_chunk` with
no added overhead at all. Avoiding this needs the JIT to know, at compile
time, whether a given register is likely to hold an Instance -- and
`src/jit.c` has **zero** access to any static type information today (no
reference anywhere to the compiler's own `parameter_type_sets`/
`scope_type_fact_count` machinery, confirmed by direct search). Threading
that through is a real, separate prerequisite, not a small addition to this
phase.

**Conclusion: not attempted.** The project's own established bar --
"require benchmark evidence large enough to justify the added complexity"
(this doc's own "Before extending past the current narrow slice" list) --
isn't met here, and can't cheaply be met without first building compile-time
type-fact propagation into the JIT. Revisit only alongside that prerequisite,
or with real measurement of how often skindicate's own non-self `INVOKE`
call sites are actually Instance-typed (unmeasured; the question that would
tell you whether the regression risk bites in practice was identified but
deliberately not chased further here, to avoid scope creep past "scope this
out").

### Phase 9: `INVOKE` for typed, never-reassigned parameters -- Phase 8's "zero type info" corrected, and closed

Re-checking Phase 8's own "`src/jit.c` has zero access to any static type
information" claim (prompted by "continue JIT work" with no new profiling
input) found it's only half true. `DiamondFunction` (`src/vm.h`) already
carries `parameter_type_sets[DIAMOND_MAX_DECLARED_PARAMETERS]` and
`type_sets`/`type_set_count` -- the compiler populates these for every
explicitly-typed parameter today, for free, with no compiler changes
needed. Real, hot skindicate/arel code already uses this: e.g.
`packages/arel/lib/arel/visitor.di`'s `render_attribute(attribute:
Arel::Attribute)`, `render_table(table: Arel::Table)`, `render_join(join:
Arel::Join, params: Array)` -- exactly inside Phase 8's own identified hot
path. So the real gap wasn't "no type info exists" -- it was "the JIT never
read the type info that was already there." Phase 8's search for
`scope_type_fact_count` references was accurate as far as it went (that
LSP-only, byte-offset-keyed table genuinely isn't usable from `src/jit.c`
without solving a separate PC-to-source-offset mapping problem, still
unsolved -- see "Out of scope" below), but `parameter_type_sets` was sitting
right there on the same struct the whole time, unexamined.

This phase closes a real, narrow, **provably correct** slice on top of that
correction: `other.method()` where `other` is a declared parameter whose
static type is a single concrete class, and which the function body never
reassigns. Deliberately much safer than "any non-self receiver" (Phase 8's
own framing): no runtime type dispatch, no deopt, no new GC-root design --
the compile-time proof is either present or the function falls back to full
interpretation exactly as before, so it can never regress an existing
working function the way Phase 8 warned a blanket `!jc->has_called`-gated
extension could.

**Compile-time type proof.** `parameter_is_single_class` (`src/jit.c`)
converts a receiver register back to its declared-parameter index and
checks `fn->type_sets[fn->parameter_type_sets[index]]` decodes to exactly
one member whose id lands in `[DIAMOND_TYPE_CLASS_BASE,
DIAMOND_TYPE_VARIABLE_BASE)` -- the same encoding `lsp/receiver.c`'s own
`decode_class_type` already trusts. No `DiamondChunk`/class-table access is
needed: the range check alone already proves "this is definitely some
Instance" (the compiler only ever writes a real, already-resolved class's
own id here), and `diamond_jit_invoke_instance` (Phase 7) dispatches by the
*runtime* instance's own class regardless, so knowing *which* compile-time
class doesn't matter. A union type_set (`count != 1`) is rejected
unconditionally, including a nilable `other: Box | Nil` (Diamond has no
`Type?` sugar -- nilability is spelled as a union with `Nil`) -- correct,
since a nil receiver must still raise through the interpreter, not hit this
Instance-only trampoline.

The register-to-parameter-index conversion needs a real offset, found by
reading `compile_definition`'s own parameter-register allocation
(`src/compiler.c`) rather than assumed: register 0 is reserved for `self`
first (`self_offset = 1`) for any function that's a direct class/module
member, a direct class singleton member, or nested directly inside a
singleton method, or that captures self as a closure -- **not** simply
"`owner_class != UINT8_MAX`", which was this phase's own first (wrong)
draft. A genuinely self-less function (a plain top-level `def`, `self_
offset = 0`) allocates its first parameter directly into register 0 -- and
an early draft of the `compile_body` wiring added a blanket `recv != 0`
guard before trying this path, which silently excluded that legitimate
`self_offset == 0`/`recv == 0` case entirely. Caught immediately by the
plan's own "verify against real compiled bytecode, don't assume" step
(`bench/jit_invoke_typed_param.di`-shaped test: `DIAMOND_TRACE_JIT` showed
2 compiled instead of the expected 3) -- removed the guard; `parameter_is_
single_class`'s own `recv_register < self_offset` bounds check already
correctly rejects every case where register 0 actually means self, so the
extra guard added no real safety, only a bug.

**"Never reassigned" proof.** A new, bounded, JIT-local pre-scan --
`parameter_never_reassigned` (`src/jit.c`) -- walks a function's whole
bytecode once, decoding each opcode exactly as `compile_body`'s own switch
does, checking whether it writes the receiver's register. Deliberately
**not** a reuse of `DiamondScopeTypeFact.effective_start` (the LSP's own
per-register type-fact table): that field is keyed by source byte offset,
not bytecode PC, and no table maps one to the other on `DiamondFunction`
today -- solving that mapping was judged more work than a dedicated scanner
for a narrower payoff, so it's still unsolved (see "Out of scope"). The
scanner is deliberately **whole-body, position-insensitive** (rejects a
register reassigned anywhere, even strictly *after* the call site in
question) rather than flow-sensitive -- simpler, and strictly safe-or-equal
versus the alternative. It's also deliberately **fail-safe by
construction**: `compile_body`'s own switch handles a small, closed set of
~26 opcodes today (anything else already bails the whole function before
this scan is ever reached), and the scanner mirrors that same set
case-for-case; any opcode not explicitly recognized as "writes no
register" or "writes register named by its own known `dest` field" is
treated as writing the target register, so a future opcode added to one
switch and not the other can only cause a missed optimization, never a
wrong one.

**Verified**: `git stash`-compared before/after on a minimal
`Runner.go(box: Box, n)`-shaped script confirmed the delta (2 -> 3
compiled functions, 0 bailouts either way -- a function that never becomes
JIT-eligible in the first place isn't a "bailout" in `DIAMOND_TRACE_JIT`'s
own sense, matching Phase 7's own `jit_invoke_self_mixed_receiver.di`
precedent). New `tests/cases/jit_invoke_typed_param*` cases cover: the
basic compiling case (including the `self_offset == 0` plain-function
path specifically, the case the `recv != 0` bug above hid), a reassigned
parameter still correctly bailing, a union (`Box | Nil`) parameter still
bailing, an interface-typed parameter still bailing (no single concrete
class exists), a real override dispatched correctly through a base-typed
parameter (not incorrectly devirtualized), and a plain untyped parameter
still bailing. Full bar: debug suite (1563/1563), ASan/UBSan
(1563/1563), and the JIT+`DIAMOND_STRESS_GC` combination run directly
against each new case (all still correct) -- an initial run showed 2
compiled instead of 3 for the basic case under `DIAMOND_STRESS_GC`, which
looked like a real GC interaction at first; tracked down via added
`fprintf` tracing to `jc.bailed`/`jc.buf.failed` at every stage, and it
vanished entirely on a from-scratch clean rebuild (10/10 consistent both
ways afterward) -- a stale/mismatched `build/` artifact left over from an
earlier, unrelated `make test-sanitize`/`make` interleaving in this same
session, not a real bug; worth recording since it cost real investigation
time before the cause was found. Release A/B benchmark
(`bench/jit_invoke_typed_param.di`, a hot loop calling a method on a typed
parameter): ~0.55s interpreted vs. ~0.34s JIT'd, a consistent ~38% win
across repeated runs, comparable in magnitude to Phase 7's own ~35%.

**Out of scope (explicit)**:
- Non-parameter receivers (a local assigned from `.new()`, a prior call's
  return value, etc.) -- would need the `scope_type_facts`
  byte-offset-to-bytecode-PC mapping problem solved first, or an
  equivalent new mechanism. Still unsolved, still the real remaining gap
  for "generic non-self `INVOKE`."
- A `recv` register reassigned anywhere in the function, even to another
  instance of the exact same class, or only *after* the call site in
  question -- `parameter_never_reassigned` rejects unconditionally, no
  flow-sensitive narrowing attempted.
- `DIAMOND_OP_INVOKE_TYPED` -- same restriction as Phase 7, still bails.

This closes the specific hot-path shape Phase 8's own skindicate profiling
named (`Arel::Visitor`'s own `render_*` methods, which take explicitly
typed `Arel::Attribute`/`Arel::Table`/`Arel::Join` parameters) but not the
diffuse remainder of `uploaders_for`/`platforms_for`'s own ~11.5ms/~10ms
(see the "Phase 8" section immediately above), most of which flows through
untyped locals and return values, not typed parameters.

**Real-world measurement.** Re-profiled skindicate's own `/` route with a
controlled A/B: same dev server, same DB, same `Time.monotonic()`
instrumentation (reverted after measuring, nothing committed to
skindicate.dia) -- only the `diamond` binary differed, built via a `git
worktree` at the pre-Phase-9 commit vs. current `main`. 20 warmed-up
requests each: total request duration 31.44ms -> 30.10ms (~4.3% faster),
grid render 24.26ms -> 23.29ms (~4.0%), `uploaders_for` 8.98ms -> 8.47ms
(~5.7%), `screenshots_for` 3.90ms -> 3.67ms (~5.9%), `platforms_for`
7.90ms -> 7.85ms (~0.6%, barely moved). Real, but modest -- confirms the
prediction above: `platforms_for` barely benefiting is exactly what "most
of the diffuse remainder flows through untyped locals/return values, not
typed parameters" predicts. An initial single-sample estimate (comparing
against this doc's own older skindicate baseline number rather than a true
same-run A/B) looked like ~20-25%; the controlled comparison is the
trustworthy one.

### Phase 10: `DIAMOND_OP_NEW` + `.method()` on a never-reassigned freshly-constructed local

Phase 9's own roadmap follow-up named the byte-offset-to-bytecode-PC
mapping problem (making the LSP's own per-register type-fact table,
`DiamondScopeTypeFact`, usable from `src/jit.c`) as the prerequisite for
the general "any local or return value" non-self `INVOKE` case. Investigated
directly before writing any code, and rejected: `DiamondScopeTypeFact` is
populated by `record_scope_type_fact` at roughly 16 call sites scattered
across `src/compiler.c`, and nothing establishes that set is *exhaustive* --
i.e. that it fires at literally every place the compiler's own live
`known_type_sets[reg]` array changes. For the LSP, a stale fact is harmless
(worst case: a wrong hover tooltip). For the JIT, reusing a stale fact to
justify routing an `INVOKE` to `diamond_jit_invoke_instance` could produce a
**wrong observable answer**, not just a safe bail: that trampoline's own
runtime check (`registers[recv].kind`, `src/vm.c`) catches "not an Instance
at all" and returns a normal `DIAMOND_VM_TYPE_ERROR`, but if the register
sometimes legitimately holds e.g. a String with its own real `.foo()`
method, and a stale fact wrongly says "always an Instance," the JIT would
report "undefined method" instead of correctly dispatching to the String's
own method -- a real behavioral divergence from the interpreter. An
exhaustive audit of every `known_type_sets`-mutating site would be needed
first, and even then the PC mapping problem itself still needs solving.
Not attempted.

**A narrower, self-contained, provably sound alternative covers a real and
common shape instead**: `x = SomeClass.new(...); ...; x.method()`, `x`
never reassigned after construction. Needs zero new `DiamondFunction`
fields, zero `src/compiler.c` changes, zero snapshot/lookup table -- it
generalizes Phase 9's own `parameter_never_reassigned` (`src/jit.c`) from
"a parameter register with zero writes" to "a register with *exactly one*
write, and that write is `DIAMOND_OP_NEW` with a compile-time-known class
index" (`register_new_class_or_move_src`/`register_new_class_if_sole_
writer`). Airtight without any control-flow modeling: on any real
execution reaching a later read of the register, the value can only have
come from that one `NEW`, since nothing else in the function ever writes
it -- true regardless of loops/branches, the same reasoning Phase 9's own
whole-body scan already relies on.

**A real gap found empirically, not assumed**: the first version of this
compiled correctly but never actually fired -- `DIAMOND_TRACE_JIT` still
showed the pre-Phase-10 compiled-function count on a real test case. A
`--dump-bytecode` disassembly (this project's own established practice:
verify against real compiled bytecode before trusting an assumption)
showed why: `x = SomeClass.new(...)` doesn't compile to a `NEW` that
directly targets `x`'s own register at all -- the compiler emits `NEW`
into its own temp register, then a separate `MOVE` into whichever register
the local actually lives in (`NEW r11, classN, r10, 1 args` immediately
followed by `MOVE r12, r11`, with `r12` being what a later `INVOKE` reads
as its receiver). `register_new_class_if_sole_writer` therefore chases
through up to 8 single-hop `register_new_class_or_move_src` calls (generous
for any real compiler-generated local-assignment shape, cheap to bound),
each a fresh whole-function scan, following a `MOVE`'s own source register
until it either finds the `NEW` or gives up.

**`DIAMOND_OP_NEW` itself** wasn't in `compile_body`'s ~26-opcode
whitelist at all before this phase, so *any* function containing a
`.new()` call fully bailed, independent of what happened to the
constructed value afterward. `diamond_jit_new_instance` (`src/vm.c`) is a
verbatim extraction of the interpreter's own plain-`NEW` case (allocate,
run `initialize` if defined, else the `Exception`-subclass two-field
fallback, else a bare `argc==0` requirement) -- `compile_new` (`src/jit.c`)
emits a trampoline call mirroring `compile_super_call`'s own stack-argument
convention, needing no pad (2 real stack args = 16 bytes, already a
multiple of 16, unlike `compile_super_call`'s own 3-arg case).
`DIAMOND_OP_NEW_KEYWORDS`/`DIAMOND_OP_NEW_SPREAD` (synthetic-chunk
re-entry into `run_chunk`) are explicitly out of scope -- no case added,
so a function containing either still bails whole.

**Verified**: `git stash`-compared before/after on a minimal `x =
Box.new(...); ...; x.double()`-in-a-loop script confirmed the delta (2 ->
3 compiled functions, 0 bailouts either way). Two pre-existing tests
(`jit_dup_instance_fallback.di`, `jit_invoke_self_universal.di`) needed
their own compiled-function counts bumped -- both already contained a
`.new()` call inside an overridden `dup()`, previously uncompiled,
correctly compiling now. New `tests/cases/jit_new_local*`/`jit_new_
exception*`/`jit_new_no_initialize*`/`jit_new_keywords_bail*` cover: the
basic compiling case, a reassigned local still correctly bailing (caught
an arithmetic mistake in this test's own first draft while writing it --
`Box.new(21)` re-executes every loop iteration, so the intended `i==3`
reassignment's effect never actually becomes observable before being
overwritten again; the disqualification is still correct regardless,
just the expected output value needed recomputing), a class with no
`initialize` still compiling via `NEW`'s own bare-`argc==0` path, an
`Exception` subclass exercising the two-field special case, a real
override dispatched correctly (not devirtualized), and `NEW_KEYWORDS`
still correctly bailing. Full bar: debug suite (1569/1569 including the
new cases), ASan/UBSan (1569/1569), and the JIT+`DIAMOND_STRESS_GC`
combination run directly against each new test case (all still correct,
on a from-scratch clean rebuild -- Phase 9's own postmortem, avoided this
time). Release A/B benchmark (`bench/jit_new_local.di`, a hot loop
constructing a fresh `Box` and calling a method on it every iteration):
~1.02s interpreted vs. ~0.67s JIT'd, a consistent ~35% win across
repeated runs, in the same range as Phase 7/9's own ~35%/~38%.

**Out of scope (explicit)**: non-parameter, non-`NEW` receivers (a
method call's own return value assigned to a local -- still the largest
remaining piece of skindicate's own diffuse `uploaders_for`/`platforms_
for` cost, per Phase 8's profiling); a register reassigned anywhere in
the function, even to another instance of the same class, or only *after*
the read in question -- no flow-sensitive narrowing; `DIAMOND_OP_INVOKE_
TYPED` -- still bails, same as every prior phase.

### Phase 11: `DIAMOND_OP_GET_IVAR` joins `DIAMOND_OP_NEW` as a second class-producing terminal

Closes the roadmap's own remaining named `INVOKE`-receiver gap for "an
ivar load": `x = @box; ...; x.method()`, `x` never reassigned after the
read. A `DiamondFunction`-level field, not a call-site return-type
question, so it sidesteps the real blocker Phase 10 identified for the
general "any local or return value" case entirely -- `redefine_method`
can swap out a *method's* `function_index` on a class at runtime
(invalidating any compile-time assumption about what a method call
*returns*), but there is no equivalent "redefine a field" operation:
`DIAMOND_OP_SET_IVAR`'s only two operands are a register index and a
literal field index resolved once at compile time, so a field's
compile-time-known type story can't go stale the way a method's can.

**The mechanism itself needed no new proof, only a new terminal for an
existing one.** `register_new_class_or_move_src` (`src/jit.c`, Phase 10)
already answers "does `target_register` have exactly one writer, and is
that writer something that guarantees a single compile-time-known
class?" for `DIAMOND_OP_NEW`. `DIAMOND_OP_GET_IVAR` of a field whose
compile-time type is itself a single concrete class is exactly the same
shape of guarantee, so it slots into the same function as a second
recognized terminal opcode (alongside `DIAMOND_OP_MOVE`'s existing
pass-through hop) -- `register_new_class_if_sole_writer`'s 8-hop MOVE
chase, `parameter_never_reassigned`'s whole-body scan, and `compile_
invoke_dispatch`'s own call site all needed zero changes; the INVOKE
dispatch switch's existing `register_new_class_if_sole_writer(...) >= 0`
eligibility check transparently covers the new case for free. Confirmed
via `--dump-bytecode` before trusting the assumption (this project's own
established practice): `x = @box` compiles to `GET_IVAR` into a temp
register followed by a separate `MOVE` into `x`'s own register, the
identical indirection Phase 10 found for `x = SomeClass.new(...)`, so
the existing MOVE-chasing loop needed no new logic to reach it.

**The real work was making the field-type fact honest enough to trust.**
A new `DiamondFunction.ivar_known_class[DIAMOND_MAX_FIELDS]` (`src/vm.h`)
snapshots -- once, at the end of compiling the owning class's body
(`compile_class_body`'s own new `snapshot_ivar_known_classes` call,
`src/compiler.c`) -- `DiamondClass.field_type_status`/`field_known_class`,
the same per-field fact `lsp/receiver.c` already reads for hover support.
Deliberately a snapshot copied onto each of the class's own (non-
`included`) methods rather than a live class-table pointer: `src/jit.c`
still has zero access to `DiamondProgram`/class tables at compile time,
by design (see Phase 8/10's own identical reasoning) -- the fact has to
already be sitting on the one `DiamondFunction` object `diamond_jit_try_
compile` ever sees.

Reusing `field_type_status`/`field_known_class` as-is would have quietly
inherited a real, pre-existing soundness gap, though: that table is
populated by `compile_assignment_store`'s own inline logic for an
ordinary `self.field = value`/`@field = value` assignment, but grepping
every `DIAMOND_OP_SET_IVAR` emission site in `src/compiler.c` found two
more that bypassed it entirely -- `compile_attribute_named`'s own
attr_accessor/attr_writer-generated writer, and `compile_struct`'s own
generated `initialize` (one `SET_IVAR` per declared field). For the LSP,
this was harmless (a field only ever assigned through attr_accessor or a
struct declaration -- i.e. almost every real class -- just permanently
looked "never assigned," status 0, rather than either a real concrete
class or "unknown"). For this phase's own new JIT use, "never assigned"
and "known concrete class" need to stay distinguishable, or the whole
mechanism would only ever fire for the least common ivar-assignment
shape. Fixed by extracting the merge logic into a shared `record_field_
known_type` (`src/compiler.c`) -- first concrete class seen wins, a
second different one or any non-concrete write poisons the field to
"unknown" forever -- and calling it from all three sites uniformly.
`compile_assignment_store` itself is now a thin caller of it, not a
change in its own behavior. A struct field is always explicitly typed
(required syntax), so `compile_struct`'s own call always contributes a
real fact one way or the other.

**A real, non-obvious bug found and fixed while wiring up the struct
side, not assumed correct**: the first version decoded a struct field's
declared type *after* `begin_struct_method` had already retargeted
`compiler->function` to the freshly-created `initialize` function -- a
function with its own, still-essentially-empty `type_sets` table.
`field_type_sets[field]` is an index into the *enclosing* function's
`type_sets` (valid at the point `parse_type_annotation` captured it,
while parsing the struct's own field list), not `init`'s, so decoding
through the wrong table silently read out of `init`'s much smaller
`type_set_count` and always returned "not concrete." Caught by this
phase's own verification methodology -- comparing real JIT eligibility
(`run` compiled or not, checked by function name via a temporary local
trace, not just a raw count) with the fix landed vs. deliberately
disabled on `tests/cases/jit_ivar_local_struct.di` -- rather than trusted
by inspection; both states looked identical (`run` ineligible either
way) until the decode was moved earlier, before `begin_struct_method`
retargets `compiler->function`, into a small local array read back
inside the loop.

**Verified**: `git stash`-compared before/after on a minimal `x = @box;
...; x.double()`-in-a-loop script confirmed the delta (`run` ineligible
before, compiled after -- checked by function name via a temporary local
`fprintf` trace in `jit_call_or_interpret`, `src/vm.c`, removed again
before committing, not just a raw compiled-function count, since that
count also includes unrelated prelude functions). New `tests/cases/
jit_ivar_local*` cover: the basic compiling case (`box: Box` kept
explicitly typed on `Holder#initialize` -- an *untyped* parameter
assignment can't be proven to hold a single concrete class, confirmed
directly as a real, correct rejection, not a bug, while drafting this
test), a reassigned local still correctly bailing, a real override
dispatched correctly through an ivar-typed-as-the-base-class receiver
(not devirtualized), the attr_accessor-writer regression fix specifically
(a field whose only `SET_IVAR` site is a generated writer), the struct-
`initialize` regression fix specifically (same shape, struct-declared
field), an untyped-parameter negative case (field permanently "unknown"),
and a cross-site-conflict negative case (two differently-typed setters
for the same field, poisoning it after the second call). Full bar: debug
suite (1576/1576 including the new cases), ASan/UBSan (via `build/
run_cases` directly plus each new case run individually under `DIAMOND_
JIT=1 DIAMOND_STRESS_GC=1` -- `tests/run.sh`'s own shell-driven signal/
network case at "ready\ncaught INT\naccepted\nnil" is independently flaky
in this environment, reproduced identically with every change here
stashed away, so not a regression from this phase), and the JIT+
`DIAMOND_STRESS_GC` combination all still correct. Release A/B benchmark
(`bench/jit_ivar_local.di`, a hot loop reading an ivar once outside the
loop and calling a method on it every iteration): ~38% faster JIT'd
(~0.57s -> ~0.35s, release build, stable across repeated runs), in the
same range as Phase 7/9/10's own ~35-38%.

**Out of scope (explicit)**: the general non-parameter, non-`NEW`,
non-`GET_IVAR` receiver case (a method call's own return value assigned
to a local) -- still unattempted, still blocked on either solving the
redefine-safe return-type question Phase 10 identified, or a different
mechanism entirely; a `GET_IVAR_NAME`-addressed module attribute (module
state has no per-field class-index table the way a real class's fields[]
does); a register reassigned anywhere in the function, even to another
read of the exact same ivar -- no flow-sensitive narrowing.

### Phase 12: `DIAMOND_OP_INVOKE` joins `NEW`/`GET_IVAR` as a third class-producing terminal

Closes the roadmap's remaining named `INVOKE`-receiver gap: `x =
obj.method(); ...; x.other()`, `x` never reassigned after the call.
Unlike Phase 10/11's own proofs (an object's class is fixed forever; a
field's declared type can't change at runtime), a *method's* declared
return type genuinely can go stale -- `redefine_method` replaces a
class's own method dispatch entry at runtime with **no return-type
compatibility check at all** (confirmed directly against its own
`DIAMOND_OP_REDEFINE_METHOD` case, `src/vm.c`: only arity/variadic must
match). Traced the actual risk precisely rather than assuming a general
"might be wrong" hand-wave: `diamond_jit_invoke_instance` (the shared
trampoline every one of Phases 7/9/10/11 already calls) is **Instance-
only** -- on a non-Instance receiver it unconditionally raises
`DIAMOND_VM_TYPE_ERROR` ("undefined method X for Y") **without checking
whether Y actually has a real method named X** (native types have their
own, entirely separate dispatch block the interpreter never routes
through this trampoline). So if `redefine_method` swaps a resolved
target for one returning e.g. an `Int`, and the real `Int` happens to
have a method by the name the chain calls, this path would incorrectly
report "undefined method" instead of correctly dispatching -- a genuine
behavioral divergence from the interpreter, not just a wasted compile.
`define_method` (the sibling opcode) needed no such gate: confirmed it
hard-rejects installing a method under a name the class already has, so
it can only ever add a genuinely new name, never invalidate an already-
resolved one.

**The mechanism**: the compiler's own real method-call-compiling code
(`publish_instance_return_type`/`instance_call_signature`, `src/
compiler.c`) already resolves `obj.method()`'s target and its *declared*
return type into `known_types[reg]`/`known_type_sets[reg]` for real
semantic purposes (structural type-checking, generics) whenever
`obj`'s own class is already known to the compiler -- not new work, and
a fundamentally more trustworthy source than the LSP's
`DiamondScopeTypeFact` table Phase 10 investigated and rejected (that
one is a best-effort, non-exhaustive, hover-only heuristic; this one is
load-bearing for actual type-checking today). Crucially, `publish_call_
return_type` only feeds this real path for a target with an *explicit,
declared* `-> Type` return annotation -- an inferred-only return is
deliberately routed to a separate, tooling-only array instead, with
its own comment: "never known_type_sets (and therefore never type
checks or opcode choice)." Phase 12 only ever trusts the declared path.

A new `DiamondFunction.register_known_class[DIAMOND_JIT_MAX_REGISTERS]`
(`src/vm.h`) snapshots this per-register fact -- populated inside
`publish_instance_return_type` itself (one shared function all of its
five call sites already funnel through, not duplicated per call site,
avoiding the kind of gap that caused two of Phase 11's own bugs) --
exactly like Phase 11's `ivar_known_class`, since `src/jit.c` still has
zero `DiamondProgram`/class-table access by design. Gated on a new
whole-program `DiamondFunction.redefine_method_used_anywhere` flag
(`true` the instant `DIAMOND_OP_REDEFINE_METHOD` is emitted anywhere,
broadcast onto every function at the end of `diamond_compile_impl`'s
real pass, mirroring how `ivar_known_class` is broadcast per-class).
Coarse (whole-program, not per-method) by design: `redefine_method`'s
own target-method-name argument is a runtime `String` value, not
reliably a compile-time literal, so a precise per-(class,method) gate
would need new string-literal tracking for a precision gain not worth
it without a real program that both uses `redefine_method` *and* wants
this optimization elsewhere.

With the snapshot and gate in place, `src/jit.c`'s `register_new_class_
or_move_src` needed exactly one new case -- `DIAMOND_OP_INVOKE`/
`INVOKE_MONO` recognized as a third class-producing terminal alongside
`NEW` and `GET_IVAR` -- and **zero** changes to `register_new_class_if_
sole_writer`'s MOVE-chase, `parameter_never_reassigned`, or the
`compile_body` `INVOKE` case's own `register_new_class_if_sole_writer(
...) >= 0` eligibility check, which picks the new terminal up for free.

**A real, narrower-than-planned scope, found empirically not assumed**:
the plan going in was "this just works for any of Phases 7/9/10/11's
own already-provable receivers, since the compiler's real type-tracking
doesn't care which of those proved `obj`." Verified directly with a
temporary trace before trusting it, and found this is only half true.
`obj` being a **typed, never-reassigned parameter** (Phase 9) or a
**freshly-`NEW`'d local** (Phase 10) works exactly as designed --
`known_types[obj]` is genuinely populated in both cases, confirmed via
`--dump-bytecode` and a real compiled-function trace. `obj` being
**`self`** or an **ivar load** (Phase 7/11's own receivers) does *not*
currently chain: `self`'s own register (0) never gets a `known_types`
entry at all (nothing in `compile_definition` ever sets it -- `self.
method()` dispatch is handled entirely at the bytecode/JIT level, with
no compiler-side type-tracking counterpart), and an ivar read (`x =
@field`) likewise never touches `known_types[x]` (Phase 11's own
`ivar_known_class` is a JIT-only side channel, not wired into the
compiler's real semantic tracking at all). Confirmed both gaps
directly: `f = Factory.new(); x = f.make(); x.double()` and `x =
f.make(); x.double()` (`f` a typed parameter) both correctly compile;
`x = self.make(); x.double()` and `x = @f; y = x.make(); y.double()`
both correctly do not, with zero difference in `register_known_class`
or the `redefine_method` gate -- the resolution step itself simply
never runs for either. Left as a documented, real limitation rather
than silently overclaiming generality; closing either is real,
separate follow-on work (wiring `self`'s own register, or an ivar
read, into the compiler's `known_types` tracking the same way a typed
parameter already is), not something this phase's own mechanism needed
to (or does) cover.

**Verified**: `--dump-bytecode` confirmed `x = obj.method()` compiles to
`INVOKE` directly targeting `x`'s own register (no intervening `MOVE`
the way `NEW`/`GET_IVAR` need, since `INVOKE`'s own destination operand
already *is* wherever the compiler placed the call's result) -- Phase
10/11's own MOVE-chase still runs unconditionally afterward and simply
finds zero hops needed, no special-casing required. New `tests/cases/
jit_invoke_result*` cover: the basic typed-parameter-chain case, the
`NEW`-local-chain case, a reassigned intermediate local still bailing,
a real override on the *inner* call (`x.double()`, dispatched through a
subclass actually constructed by the outer call) still correct -- not
devirtualized, an inferred-only (no `-> Type`) return type still
correctly not trusted, and a `redefine_method` call anywhere in the
program (on a completely unrelated class) disabling the optimization
for a function nowhere near it -- each eligibility claim confirmed via
a temporary per-function compiled/ineligible trace, not just a raw
count. Full bar: debug suite (1582/1582 including the new cases),
ASan/UBSan (via `build/run_cases` directly plus the JIT+`DIAMOND_
STRESS_GC` combination -- this sandbox's own `tests/run.sh` signal test
is independently flaky, confirmed via a stash-everything control run,
unrelated to this phase). Release A/B benchmark (`bench/jit_invoke_
result.di`, a hot loop calling a typed parameter's method and then a
method on *that* result every iteration): ~33% faster JIT'd (~0.9s ->
~0.6s, release build, stable across repeated runs), in the same range
as every prior `INVOKE`-dispatch phase's own ~35-38%.

#### Addendum: closing the `self` half of Phase 12's own documented gap

Wiring `self`'s own register (0) into the same `known_types` tracking a
typed parameter already gets turned out to be small: `compile_definition`
(`src/compiler.c`) already knows, once per function, exactly which shape
of `self` (if any) it has (`direct_class_member`, `direct_class_
singleton_member`, `nested_in_singleton_method`, `direct_module_member`,
`captures_self`); register 0 is allocated for the genuine-instance shapes
at one `allocate_register(compiler)` call, which itself resets `known_
types[0]` to `TYPE_UNKNOWN` (`enum { TYPE_UNKNOWN = UINT8_MAX }`, `src/
compiler.c:18`) -- confirmed directly, since an earlier attempt at the
`owner_class=...` assignment just above that call would have been
silently overwritten by it. Right after that call, for `direct_class_
member` or `nested_in_singleton_method` (the redefine_method patch-
factory idiom, which behaves exactly like a genuine instance method,
`@ivar` access included) with a real `current_class`, `known_types[0]`
is now set to that class -- deliberately **excluding** `direct_class_
singleton_member` (`def self.x`): confirmed against `emit_singleton_
call`'s own comment that `self` there is the literal *class* value
(`DIAMOND_OP_LOAD_CLASS`), not an Instance, so tagging it as "known
instance of `current_class`" would be actively wrong, not just an
imprecise miss; `self.` dispatch inside a singleton method already
routes through the separate `parse_self_class_method_call`, confirmed
never touching `publish_instance_return_type` at all either way.
Zero `jit.c`/`vm.h` changes needed -- Phase 12's own `register_known_
class` snapshot and `redefine_method_used_anywhere` gate are already
generic over how a receiver's `known_types` entry got set.

**Verified**: new `tests/cases/jit_invoke_result_self*` cover the basic
case, a real override on the *inner* call (`self.make()` dispatched
through a subclass instance the *enclosing* method's own class doesn't
know about, confirming `self`'s own compile-time class doesn't need to
match the runtime instance for correctness -- the same property every
other `INVOKE`-dispatch phase already relies on), and the `nested_in_
singleton_method` shape chaining too (installed via `define_method`
rather than `redefine_method` specifically so the test doesn't trip its
own whole-program `redefine_method_used_anywhere` flag and mask the
thing being tested). Each eligibility claim confirmed via a temporary
trace, including the delta with the fix stashed out. Full bar: debug
suite (1585/1585), ASan/UBSan, JIT+`DIAMOND_STRESS_GC`, all green.
~37% faster on a new `bench/jit_invoke_result_self.di` (release build).

**A real, negative finding worth recording, not just the win**: rerunning
this session's own isolated skindicate ORM microbenchmark (`Skin.random_
sample` + the three `_for` batch loaders, called directly, no HTTP/
template overhead) after this fix showed **no improvement** over Phase
12 alone (~4.7%, versus ~4.5% before -- within noise, not a real
delta), despite that hot path's own receivers being exactly the `self`-
shaped calls this fix targets. Traced the reason directly rather than
leaving it a mystery: Arel's own hot accessor methods named in Phase 8's
original profile -- `Query#projections` (`def projections() = @projections`),
`BinaryNode#left` (`def left() = @left`), and similar -- have **no
explicit `-> Type` return annotation at all**. `publish_call_return_
type` only ever feeds the real (non-tooling) `known_types` path for a
*declared* return type (see Phase 12's own section) -- an inferred-only
one is deliberately kept out of opcode-selection reach regardless of
which receiver kind calls it. So the actual remaining blocker for
*this specific workload* isn't a receiver-kind gap at all (Phases 9/10/
11/12/13 between them now cover every receiver kind this JIT reasons
about) -- it's that the hot methods themselves are unannotated. Adding
`-> Type` annotations to Arel/ActiveRecord's own hot accessors is real,
separate, application-level work (skindicate.dia's own package code,
not the Diamond compiler/JIT), not attempted here.

### Phase 14: `DIAMOND_OP_IS_TYPE` gets JIT codegen (and stops poisoning unrelated eligibility scans)

Started as an attempt to wire `is`-narrowing into `known_types` so a
narrowed receiver (`if x is Derived; y = x.helper(); y.double()`, `x`
a declared `Derived | OtherBase` parameter) would chain the same way a
typed parameter already does -- the natural next step after Phase 13's
own negative finding that Arel's real hot path (`Visitor#render_
expression`, dispatching on an untyped `expression` via an `is`-chain)
needed exactly this. Diagnosed with real instrumentation before
assuming a fix shape, per the plan going in.

**The suspected bug didn't exist.** Traced every link in the existing
`Narrowing`/`NarrowingFact` mechanism (`src/compiler.c`) with temporary
tracing on the motivating case and found it all works correctly:
`compiler->known_type_sets[x]` is populated at parameter-declaration
time even for a union type, `x is Derived` correctly `split_type_set`s
it and records the fact in `compiler->narrowing`, `parse_if` correctly
picks the fact up and applies it via `apply_narrowing_facts` ->
`apply_type_set_fact` before compiling the then-branch, and `known_
types[x]` is genuinely `Derived`'s own type tag by the time `x.helper()`
compiles inside that branch. `instance_call_signature` resolves the
call and its declared return type correctly too. None of this was
broken.

**The real blocker was two levels below the compiler entirely**:
`src/jit.c`'s `compile_body` had **zero case for `DIAMOND_OP_IS_TYPE`**
-- not a narrow gap, a complete absence, present since the opcode was
first introduced. Any function containing so much as `if x is SomeType`
anywhere bailed the *whole function* outright, falling straight to
`compile_body`'s own `default: jc->bailed = true`, regardless of
whether the narrowing itself was ever going to matter for JIT purposes.
Confirmed directly: `def classify(x: Int | String) -> Int; if x is Int;
1; else; 2; end; end` -- no receiver chaining at all, the simplest
possible reproduction -- was already JIT-ineligible before this phase.

**Fixed** by adding `IS_TYPE` alongside `CHECK_TYPE` as a fourth "too
semantically deep to hand-roll in asm, call a trampoline" opcode (same
category as `SET_IVAR`/`GET_IVAR`/`INDEX_GET`/`CHECK_TYPE` already
were): a new `diamond_jit_is_type(chunk, value, type, out)` trampoline
(`src/vm.c`) wrapping the interpreter's own `value_matches_type`, and
`compile_is_type` (`src/jit.c`) emitting the same 4-argument, no-stack-
overflow call shape `compile_check_type` already uses. Needs neither
`jc->needs_frame` nor `jc->has_called`: `value_matches_type` never
allocates or invokes user code (confirmed by reading it fully -- even
its interface-matching path only ever consults a fixed native-method
table, never a real method call). The compiler already hard-rejects `is`
against a generic type variable before it can ever reach codegen
(`src/compiler.c`'s own "generic type variables cannot be used with
'is' before binding" check), so the trampoline never needs to handle
that case either.

**A second, broader bug found in the same investigation**: `src/jit.c`
has two other opcode-enumerating scans -- `parameter_never_reassigned`
(Phase 9) and `register_new_class_or_move_src` (Phase 10-13) -- each
with its own hand-maintained "does this opcode write a register, and
where" switch, deliberately kept in sync with `compile_body`'s own
switch by hand (their own comments say so explicitly) rather than a
shared table. Both scan a function's *entire* bytecode from offset 0
regardless of which register they're actually tracing, and both bail
their *entire* walk -- not just "give up on this one register" -- the
moment they hit any opcode outside their own whitelist. Since neither
had an `IS_TYPE` case, **any function containing an `is` check anywhere
in its body was silently losing Phase 9/10/11/12/13's own chaining
eligibility for every other register in that function too**, not just
whatever `is` was narrowing -- a real, pre-existing, and rather broad
gap that predates this phase and had nothing to do with narrowing per
se. Confirmed via a stash-based A/B on a function combining an ordinary
Phase 13 `self.make().double()` chain with an unrelated `if x is Int`
check elsewhere in the same body: 2 compiled functions before this
fix (the chain itself silently lost), 3 after. Fixed by adding
`DIAMOND_OP_IS_TYPE` to `opcode_dest_is_first_u16` and the matching
2-extra-operand consumption case in both scans (its own operand shape --
one dest register plus two more register-width fields -- happens to
exactly match the existing `ADD_INT`/`EQUAL`/`GET_IVAR`/`INDEX_GET`/
`HASH` group already in both switches). `IS_TYPE`'s destination is
always `DIAMOND_TYPE_BOOL`, never a class, so it's correctly *not*
added as a fourth class-producing terminal anywhere -- just recognized
as "writes a register, but never the interesting kind."

**What this phase does *not* close**: the original motivating case
(`x.helper()` on an `is`-narrowed union-typed *parameter*, inside the
narrowed branch) still does not JIT-compile, and this phase stops short
of that rather than pushing through. The reason is structural, not a
bug: every one of `compile_body`'s three INVOKE-receiver acceptance
paths (`self`/Phase 13, `parameter_is_single_class`+`parameter_never_
reassigned`/Phase 9, `register_new_class_if_sole_writer`/Phase 10-12)
is deliberately **position-insensitive** -- each scans the whole
function once, asking "is this register *always* provably one class,
everywhere," never "is it provably one class *at this specific call
site*." `parameter_is_single_class` looks only at the parameter's own
*declared* type (`fn->parameter_type_sets`), which for `x: Derived |
OtherBase` is a two-member union regardless of any `is` check later in
the body -- narrowing is a purely compile-time, lexically-scoped fact
that never rewrites `x`'s own register or leaves any trace in the
compiled bytecode itself (nothing else reads `compiler->narrowing`
after the branch it applied to finishes compiling). Making this case
JIT-eligible would need a genuinely new mechanism -- a position-
*sensitive* per-instruction-site class-fact table, unlike anything
Phases 9-13 built -- not a fix to something already there. Per this
phase's own approved plan ("if diagnosis reveals the fix is larger or
riskier than expected... stop and report back rather than pushing
through"), this is exactly that trigger: stopped here, reported the
corrected finding, left the decision of whether to build the bigger
mechanism to a future phase.

**Verified**: new `tests/cases/jit_is_type_basic` (the standalone
`classify` reproduction: 0 compiled -> 1 compiled, 0 bailouts) and
`jit_is_type_no_poison` (the self-chain-plus-unrelated-`is`-check
reproduction: 2 compiled -> 3 compiled, 0 bailouts, each confirmed via
a stash-based A/B, not just a single-sided pass). Full debug suite,
ASan/UBSan, and `DIAMOND_JIT=1 DIAMOND_STRESS_GC=1`, all green. No
compiler.c changes at all this phase -- the entire fix lives in
`src/jit.c`/`src/jit.h`/`src/vm.c`, since the compiler-side mechanism
was already correct. ~39% faster on a new `bench/jit_is_type.di`
(release build, `classify` from the `is_type_basic` reproduction above
in a 3M-iteration loop): ~0.43s interpreted, ~0.26s with `DIAMOND_JIT=1`.

### Phase 15: position-sensitive `is`-narrowed `INVOKE` receivers

Closes the gap Phase 14 explicitly stopped short of: `def run(x: Derived
| OtherBase) -> Int; if x is Derived; y = x.helper(); y.double(); end;
end` still didn't JIT-compile even after Phase 14 fixed `IS_TYPE`
codegen, because every one of `src/jit.c`'s three existing `INVOKE`-
receiver proofs (`self`/Phase 13, `parameter_is_single_class`+
`parameter_never_reassigned`/Phase 9, `register_new_class_if_sole_
writer`/Phase 10-12) is deliberately **position-insensitive**: each asks
"is this register *always* provably one class, everywhere in the
function," never "is it provably one class *at this specific call
site*." `x`'s own *declared* type is a two-member union, and nothing
ever *writes* to `x` (it's a parameter), so neither existing proof can
ever say anything about it on its own -- narrowing is a purely lexical,
compile-time-only fact with zero trace in the compiled bytecode a later,
position-insensitive scan could find.

**A position-insensitive fix was considered and rejected first.** The
obvious-looking shortcut -- reuse Phase 12's own `register_known_class
[reg]`, populating it from `known_types[reg]` at every `is`-narrowed use
and invalidating it the moment two uses disagree -- would have been a
much smaller change. But Arel's actual motivating shape (`Visitor#
render_expression`'s own `if expression is Attribute ... elsif
expression is Predicate ...` chain) narrows the *same* register to
*different* classes at *different* sites, calling a *different* method
at each one. A "must agree everywhere" check would correctly, but
uselessly, invalidate the fact for every site the moment it saw the
second one disagree -- solving the synthetic case while missing the
real one entirely. Confirmed directly: a `tests/cases/jit_is_narrowed_
invoke_multi_branch`-shaped function (three `is`-branches on one
receiver, each calling a different method) only reaches 3 compiled
functions (the three called methods, not the containing one) under a
position-insensitive design, versus 4 (the containing function too)
with the real, position-sensitive one -- verified via a stash-based A/B
before committing to the harder design, not assumed.

**The mechanism**: one new fact, recorded once at the single point the
compiler already knows it, consulted once at the single point `src/
jit.c` already visits it -- no new bytecode scanning, no new codegen.

`emit_invoke_call` (`src/compiler.c`) is the *one* shared funnel every
plain (non-`_TYPED`/`_SPREAD`/`_KEYWORDS`/`_SELF_METHOD`) `DIAMOND_OP_
INVOKE` emission goes through (`parse_invoke` and `compile_delegate`
both call it). Right before its existing `emit_opcode` call,
`compiler->known_types[receiver]` already holds whatever the compiler's
real, flow-sensitive tracking currently believes -- narrowed class
included, via the exact `apply_narrowing_facts`/`apply_type_set_fact`
chain Phase 14 already traced and confirmed correct. When that value is
a genuine single concrete class, record `{offset: compiler->function->
code_count (this instruction's own start, matching src/jit.c's own
`instruction_start = pc`-before-decode convention exactly), known_class}`
into a new fixed-size table on `DiamondFunction`:
`invoke_site_known_class[DIAMOND_MAX_INVOKE_SITES=64]` (`src/vm.h`,
same scale as `DIAMOND_MAX_FIELDS`/`DIAMOND_MAX_LOCALS`, silently stops
recording once full -- fail-safe by construction, same convention every
other side table in this file already uses).

`src/jit.c`'s `compile_body` `DIAMOND_OP_INVOKE`/`INVOKE_MONO` case gains
a fourth acceptance branch, alongside the existing three: a linear scan
(at most 64 entries, once per `INVOKE` site, at JIT-*compile* time only)
checking whether *this* instruction's own `instruction_start` has a
recorded entry. **Zero changes to `compile_invoke_dispatch` itself** --
every acceptance path, old or new, funnels into the exact same,
already-correct codegen; this only adds a new way to *qualify* for it.
`diamond_jit_invoke_instance` (the runtime trampoline) still dispatches
by the *runtime* instance's own class regardless of which compile-time
path proved eligibility, same as every earlier phase already relies on
-- so a receiver narrowed to `Derived` that turns out to be some further
subclass at runtime still dispatches correctly; the compile-time fact
only ever proves "definitely some Instance," never "definitely this
exact method."

**Safety**: recording is unconditional at compile time (the whole-
program `redefine_method_used_anywhere` flag isn't finalized until
compilation finishes, the same reason Phase 12 gates at *read* time
instead of *write* time). The gate is applied uniformly at consultation
time in `src/jit.c`, exactly mirroring Phase 12's own `register_known_
class` gate -- even though a pure `is`-narrowing fact doesn't strictly
need it (narrowing reflects a *runtime* type check via `IS_TYPE`, immune
to a stale declared return type the way `redefine_method` can make a
Phase-12-sourced fact go stale), nothing at read time distinguishes
*which* source populated a given `known_types` entry, so gating
uniformly and conservatively was the safer choice over trying to tell
them apart. Verified via `tests/cases/jit_is_narrowed_invoke_redefine`
(mirrors Phase 12's own `jit_invoke_result_redefine`): a `redefine_
method` call anywhere in the program, even on a completely unrelated
class, correctly disables this path too.

**Cache safety**: confirmed via `src/compiled_prelude.c`'s
`diamond_cache_fingerprint()` (includes `function_size=sizeof(
DiamondFunction)`): growing `DiamondFunction` automatically invalidates
any `.dic` bytecode cache written before this change (a safe cache miss,
never a crash) -- the same mechanism that already silently covered
Phase 11/12/13's own new fields, none of which are explicitly
serialized in `diamond_program_write_compiled`/`read_compiled` either.
Followed that same precedent: no serialization changes needed. A
function loaded from a stale-format cache and never recompiled fresh
just silently misses this optimization -- never dispatches incorrectly.

**A genuine, positive side effect on an existing test, not a
regression**: `tests/cases/jit_invoke_typed_param_union.di` (Phase 9's
own `box: Box | Nil` test) started compiling one more function (3, not
2) the moment this phase landed -- its own `if box is Box; box.double();
end` is exactly this phase's target shape, and its comment/expectation,
written when that was still correctly impossible, needed updating
alongside the fix. A good sign the new mechanism generalizes beyond its
own purpose-built test cases.

**Verified**: new `tests/cases/jit_is_narrowed_invoke_basic` (the
original motivating shape), `_multi_branch` (the Arel-shaped multi-site
reproduction, confirmed via stash-based A/B that a position-insensitive
design would miss it), and `_redefine` (the safety-gate negative test).
Full debug suite, ASan/UBSan, and `DIAMOND_JIT=1 DIAMOND_STRESS_GC=1`,
all green.

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

**Update, post-Phase-7/8 research**: what actually shipped needed no new
"entry guard" mechanism at all -- the existing `jc->has_called`-gated retry
(any bail while it's still false safely discards the whole attempt and
re-interprets from the top) already provides exactly this, opcode by opcode,
not as a single coarse guard. Phase 8's own research (above) leaned on this
directly to establish that a non-`self` `INVOKE` slice would be *correct*
with no new design -- confirming this section's core intuition was right,
just heavier than what was actually needed. What that research found
missing instead was static type information reaching the JIT at all, a
different gap this section didn't anticipate.

### Phase 16: declared-return chaining from an ivar-loaded receiver

Phase 12 could carry a declared concrete return through `obj.make()` into a
later call on the result, but the inner receiver `obj` had to be `self`, a
typed parameter, or a freshly constructed local. An ivar load was already a
sound JIT receiver in Phase 11, yet it never entered the compiler's
`known_types` table, so method resolution could not publish the inner call's
declared return.

The declaration-discovery pass already computes a whole-class, exhaustive
`field_type_status`/`field_known_class` result across ordinary assignments,
generated attribute writers, struct initialization, inheritance, and class
reopenings. Phase 16 preserves that final result in
`DiamondClass.discovered_field_*` before the real pass clears and rebuilds
the class. A real-pass `GET_IVAR` publishes a concrete receiver type only
when discovery saw exactly one class at every write. This makes the fact
available before any real method body is emitted and avoids source-order
assumptions. A later conflicting or untyped write has already poisoned the
discovery result, even if its source appears after the method using the field.

No JIT code changed. The existing Phase 11 receiver proof compiles the inner
call, while Phase 12's `register_known_class` and `redefine_method` guard carry
its declared return into the outer call. Focused cases cover the compiling
path and a later conflicting-write rejection. The release benchmark
`bench/jit_invoke_result_ivar.di` measured 7.95–9.00s interpreted and
3.90–4.73s JIT compiled across three alternating runs, about a 49% reduction
using the run averages.

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
### Phase 17: selected native collection reads

Call-site receiver facts also retain statically known `String`, `Array`, and
`Hash` types. The x86-64 backend uses those facts for `String#length`,
`String#index_of`, `String#ord`, `Array#length`, `Hash#length`, `Hash#key_at`,
and `Hash#value_at`. A small trampoline performs the same type, argument, and
bounds checks as the interpreter. These operations allocate nothing and
mutate nothing, so a later bailout can safely restart the bytecode function.

### Phase 18: allocating native String slice

Statically typed `String#slice` uses the same native-call selection, but marks
the compiled function as framed and call-bearing. The frame exposes receiver
and argument registers to GC while `allocate_string` runs. Any type, bounds,
or allocation failure propagates its VM status directly, avoiding a restart
that could repeat an allocation. Stress-GC regression coverage exercises the
successful, clamped, bounds-error, and type-error paths.
