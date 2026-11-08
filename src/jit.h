#ifndef DIAMOND_JIT_H
#define DIAMOND_JIT_H

#include "value.h"
#include "vm.h"

/* A minimal, narrowly-scoped baseline JIT -- Phases 2, 2b, and 2c of the
 * plan recorded in docs/internal/jit-design.md. Compiles functions built
 * entirely from a small whitelist of arithmetic/control-flow/Hash-read/
 * ivar-write/string-construction opcodes (see jit.c's diamond_jit_try_
 * compile for the exact list) straight to x86-64 machine code --
 * non-variadic (generic ones included), any arity (including instance methods,
 * `self` just being register 0 like any other argument). Any function
 * containing anything outside that whitelist is never compiled at all --
 * there is no partial/mixed-mode execution and no deopt needed for
 * *unsupported opcodes*, only for the runtime-exceptional cases within the
 * supported ones (integer overflow promoting to bignum, division by zero,
 * INT64_MIN/-1, a trampoline reporting anything but DIAMOND_VM_OK) that
 * this JIT deliberately doesn't handle inline: on any of those, the
 * compiled function returns false and the caller must fall back to the
 * ordinary run_chunk interpreter path, which remains fully correct and
 * unmodified. This is safe specifically BECAUSE the whitelist excludes
 * anything with an *uncontrolled* observable side effect (no arbitrary
 * method calls, no object construction, no raises) -- every side effect
 * that is possible (an ivar write, a string allocation) happens through a
 * real, correct C function call, not hand-rolled machine code, and
 * re-running the whole function via the interpreter after a bailout is
 * indistinguishable from having never attempted the JIT at all.
 *
 * Phase 2c adds the first *allocating* opcode (STRING) and, with it, the
 * real DiamondFrame/GC-root contract docs/internal/jit-design.md
 * describes: a function that can allocate reserves stack space for an
 * opaque DiamondFrame (see the diamond_jit_frame_* trampolines below) at
 * entry and publishes it before doing anything that could trigger a
 * collection, so mark_frame_chain can still find every live register the
 * same way it already finds an interpreted frame's. This is decided per-
 * function at compile time (a cheap pre-scan for any allocating opcode
 * before the real compile pass) specifically so every allocation-free
 * function compiled under Phase 2/2b (arithmetic, Hash-read/ivar-write
 * with a non-literal key, ...) keeps paying nothing for a mechanism it
 * doesn't need -- see jit.c's own diamond_jit_try_compile for exactly how.
 *
 * Phase 2d adds the first opcode that can run arbitrary interpreted
 * Diamond code with real, externally-visible side effects: SUPER (plus
 * HASH, needed for a `= {}` default argument, and a fix to EQUAL/NOT_EQUAL
 * so they never need to bail at all -- see jit.c's own compile_equal_op
 * comment). This breaks the invariant every prior phase relied on --
 * "any bailout is safe to handle by discarding the whole attempt and
 * re-running the entire function from scratch" -- since SUPER's callee can
 * raise a genuine exception, and if SUPER already succeeded before a
 * *later* opcode bails, restarting the whole function would invoke it a
 * second time. See DiamondJitFn's own updated comment below for how this
 * is resolved (a 3-way return convention) without needing full on-stack
 * replacement.
 */

/* DIAMOND_JIT_MAX_REGISTERS itself now lives in src/vm.h (see that file's
 * own comment on it) -- DiamondFunction.register_known_class (Phase 12,
 * docs/internal/jit-design.md) needed the same bound visible from
 * src/compiler.c, which doesn't include this header, so the canonical
 * definition moved to the lower-level header both already include rather
 * than staying duplicated. jit.h's own #include "vm.h" above already
 * brings it in. */

/* Phase 2d: the sentinel DiamondJitFn returns to mean "discard this
 * attempt, fall back to run_chunk" -- every value that isn't this AND
 * isn't DIAMOND_VM_OK is a real DiamondVmStatus to propagate directly
 * (see DiamondJitFn's own comment below). 0xFF, guarded by the
 * static_assert immediately below against DiamondVmStatus's own last
 * member so it can never collide with a genuine status as the enum
 * grows. */
enum { DIAMOND_JIT_RETRY = 0xFF };
/* A compiled function's tail call to a different function: the callee and
 * its arguments are staged in the VM (jit_tail_*) and the compiled frame is
 * gone. jit_call_or_interpret runs the callee in a loop at the same depth,
 * so a cycle of such calls costs no native stack, as in the interpreter. */
enum { DIAMOND_JIT_TAIL = 0xFE };
static_assert((unsigned)DIAMOND_VM_SANDBOX_ERROR < (unsigned)DIAMOND_JIT_TAIL,
        "DiamondVmStatus has grown into DIAMOND_JIT_TAIL's reserved sentinel value");
static_assert((unsigned)DIAMOND_VM_SANDBOX_ERROR < (unsigned)DIAMOND_JIT_RETRY,
        "DiamondVmStatus has grown into DIAMOND_JIT_RETRY's reserved sentinel value");

/* uint8_t(DiamondVm *vm, DiamondValue *registers, DiamondValue *result,
 * size_t argument_count, const DiamondChunk *chunk, size_t depth) --
 * `registers` is a zeroed, argument-populated array exactly like
 * run_chunk's own callee registers (see run_chunk's own entry setup in
 * vm.c for the shape this mirrors); `vm` and `chunk` are threaded through
 * purely so generated code can pass them to a trampoline call (SET_IVAR/
 * INDEX_GET/CHECK_TYPE/STRING/ARRAY/HASH/SUPER), never dereferenced by
 * generated code itself -- `chunk` specifically is the same DiamondChunk
 * view the interpreter itself would have built for this exact call (see
 * both jit_call_or_interpret call sites in vm.c), needed because a type
 * set index (or, since Phase 2d, a SUPER call's own owner-class/method-
 * name indices) is only meaningful relative to it. `argument_count` is
 * the caller's own raw count (registers[] alone can't answer DIAMOND_OP_
 * ARGUMENT_PROVIDED's "was this parameter actually supplied" question --
 * a defaulted-and-unsupplied parameter and an explicitly-nil one are
 * indistinguishable in registers[] alone). `depth` (Phase 2d) is this
 * call's own logical depth, exactly the value that would have been passed
 * to run_chunk had this call not been compiled -- needed so a SUPER call
 * made from *inside* generated code can pass it on to invoke_resolved_
 * method_helper unchanged, keeping DIAMOND_MAX_CALL_DEPTH enforced
 * end-to-end even across a chain of calls that happen to all be compiled
 * (see jit_call_or_interpret's own depth check in vm.c, which is where
 * the actual limit is enforced -- generated code never checks it itself).
 * Known, accepted gap: this doesn't account for DIAMOND_VALUE_UNDEFINED
 * sparse-keyword-call gaps (registers[] never receives an UNDEFINED slot
 * at all, only the interpreter's own `arguments[]` would show one) -- fine
 * for every call site this JIT is wired into today (DIAMOND_OP_CALL, plain
 * positional NEW/SUPER/INVOKE_TYPED dispatch via invoke_resolved_method_
 * helper), none of which are keyword calls; would need revisiting before
 * ever wiring a keyword-call site to this dispatch path.
 *
 * Returns one of three things (Phase 2d changed this from a plain bool):
 *   DIAMOND_VM_OK (0)   -- success, *result holds the real return value.
 *   DIAMOND_JIT_RETRY   -- discard this attempt; the caller must fall
 *                          back to run_chunk for a fully correct
 *                          interpreted execution of the same function
 *                          from scratch. Safe exactly when nothing with
 *                          an externally-visible side effect has already
 *                          run on this attempt -- true for every bailout
 *                          in Phases 2/2b/2c, and for any bailout in a
 *                          Phase 2d function *before* its first SUPER call
 *                          (see jit.c's own jc->has_called).
 *   anything else        -- a real DiamondVmStatus (most notably
 *                          DIAMOND_VM_EXCEPTION) to return to the caller
 *                          of jit_call_or_interpret exactly as-is; *result
 *                          is NOT written and must not be read. This
 *                          happens once a SUPER call has already run in
 *                          this same attempt -- re-running the whole
 *                          function via run_chunk would invoke it again,
 *                          so every bail site from that point onward
 *                          propagates directly instead. */
typedef uint8_t (*DiamondJitFn)(DiamondVm *vm, DiamondValue *registers,
        DiamondValue *result, size_t argument_count, const DiamondChunk *chunk,
        size_t depth, const struct DiamondClosure *closure);

/* Attempts to compile `function` to native code. Returns a callable
 * DiamondJitFn on success (and writes the mmap'd region's size to
 * *out_size, needed later by diamond_jit_free), or nullptr if the function
 * contains anything outside the supported whitelist (checked once; the
 * caller should record that outcome, e.g. via DiamondFunction's own
 * jit_ineligible field, and never retry). The returned pointer is
 * executable memory owned by this module -- see diamond_jit_free. */
/* `self_function_index` is the function's own index in its program's table
 * (SIZE_MAX if unknown): only a TAIL_CALL to it is compiled. */
void *diamond_jit_try_compile(const DiamondFunction *function,
        size_t self_function_index, size_t *out_size);

/* A compiled function's own tail call: moves the `argc` arguments at
 * registers[base..] to registers[0..] and zeroes the rest of the `live`
 * registers, exactly the state a fresh call would start from. */
void diamond_jit_tail_reset(DiamondValue *registers, size_t base, size_t argc,
        size_t live);

/* Releases the executable memory returned by a prior diamond_jit_try_compile
 * call. Safe to call with nullptr (no-op). */
void diamond_jit_free(void *jit_code, size_t jit_code_size);

/* Trampolines called *from* generated machine code for the operations too
 * risky to hand-roll directly in x86-64 (shape-transition
 * bookkeeping + the GC write barrier for SET_IVAR; the shape check for
 * GET_IVAR; Hash lookup for INDEX_GET) -- all defined in vm.c,
 * where the internal helpers they
 * reuse (gc_write_barrier, hash_find) already live
 * with internal linkage. Every DiamondValue is passed by pointer, not by
 * value, specifically to keep every argument a plain 8-byte pointer for
 * the SysV calling convention generated code uses -- passing a 16-byte
 * DiamondValue by value would need struct-classification rules this
 * hand-written caller side never implements.
 *
 * None of the three trampolines below can trigger a GC collection
 * (confirmed by reading all three call chains: gc_write_barrier's own
 * realloc is the remembered-set's *bookkeeping* array, entirely separate
 * from the Diamond heap's own maybe_collect-gated allocator; hash_find is
 * a pure lookup; value_matches_set's structural/generic matching binds
 * type variables into a caller-owned stack array, same as invoke_
 * resolved_method_helper's own explicit_bindings, never the heap) -- so a
 * caller made entirely of these plus arithmetic/comparisons still needs
 * no DiamondFrame of its own, per docs/internal/jit-design.md's own
 * reasoning for why the call-free Phase 2 slice didn't need one either.
 * This stops being true the moment a future phase adds a trampoline that
 * *can* allocate (NEW, Hash/Array construction, string building) -- that
 * one will need the real frame contract the design doc describes. */
DiamondVmStatus diamond_jit_set_ivar(DiamondVm *vm, const uint8_t *site,
        const DiamondValue *receiver, uint8_t field, const DiamondValue *value);
/* GET_IVAR's counterpart -- unlike SET_IVAR, can never allocate or invoke
 * user code (plain field access has no operator-overload equivalent), so
 * its caller in jit.c needs neither a DiamondFrame nor jc->has_called. */
DiamondVmStatus diamond_jit_get_ivar(DiamondVm *vm, const uint8_t *site,
        const DiamondValue *receiver, uint8_t field, DiamondValue *out);
DiamondVmStatus diamond_jit_check_type(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *value, uint16_t set_index);
/* IS_TYPE -- value_matches_type's own single-type-tag matching (unlike
 * CHECK_TYPE's value_matches_set, no union/interface/generic-binding
 * cases reachable here: the compiler hard-rejects `is` against a type
 * variable before this can ever be emitted with one, src/compiler.c's
 * own "generic type variables cannot be used with 'is'" check). Pure
 * value-kind/class-hierarchy inspection, no allocation or user-code
 * invocation, so -- like GET_IVAR -- needs neither jc->needs_frame nor
 * jc->has_called. Always returns DIAMOND_VM_OK; the status return exists
 * only to match every other trampoline's emit_bail_if_al_nonzero tail. */
DiamondVmStatus diamond_jit_is_type(const DiamondChunk *chunk,
        const DiamondValue *value, uint16_t type, DiamondValue *out);

/* SAME_CLASS: whether both values are instances of exactly one class. Pure
 * inspection like IS_TYPE, so it needs neither a frame nor has_called. */
DiamondVmStatus diamond_jit_same_class(const DiamondValue *left,
        const DiamondValue *right, DiamondValue *out);
bool diamond_values_same_class(DiamondValue left, DiamondValue right);

/* Phase 2e. JIT trampolines for DIAMOND_OP_INDEX_GET/INDEX_SET -- full
 * extractions of those opcodes' own real case bodies (src/vm.c), replacing
 * Phase 2b's Hash-only diamond_jit_hash_get. That narrower trampoline
 * returned DIAMOND_VM_TYPE_ERROR for any non-Hash receiver, which was
 * *accidentally* safe before Phase 2d (every bailout retried via full
 * interpretation, which correctly checks the Instance `[]` override
 * below) but became a real, if narrow, latent correctness bug the moment
 * a bailout could "propagate" instead (see docs/internal/jit-design.md's
 * Phase 2e status note). These handle Hash/String/Array/Instance-overload
 * (GET) and Hash/String/Instance-overload/Array (SET) exactly like the
 * real opcodes. Both can genuinely invoke arbitrary code via the
 * Instance-overload branch, so both are compiled with jc->has_called =
 * true unconditionally (see compile_index_get/compile_index_set in
 * jit.c) -- the same conservative, compile-time-only choice every other
 * overload-checking trampoline here makes. `site` is this occurrence's
 * own bytecode address, the same per-occurrence method-cache key
 * `diamond_jit_equal_general` and `diamond_jit_super_call` already use.
 * INDEX_SET has no `out` parameter -- see its own real case's comment in
 * vm.c for why (`x[i] = v` already evaluates to `v` itself, computed
 * before this opcode runs, not a destination register it writes). */
DiamondVmStatus diamond_jit_index_get(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        const DiamondValue *index, DiamondValue *out);
/* DIAMOND_OP_INDEX_GET_STRING's trampoline: `string_index` names a string constant of `chunk`
 * used as the key. A Hash receiver is searched by the constant's bytes with no allocation;
 * anything else allocates the key and defers to diamond_jit_index_get, so it can invoke an
 * Instance `[]` override and is compiled with jc->has_called like INDEX_GET. */
DiamondVmStatus diamond_jit_index_get_string(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        uint16_t string_index, DiamondValue *out);
DiamondVmStatus diamond_jit_index_set(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        const DiamondValue *index, const DiamondValue *source);

/* DiamondFrame management. A compiled function that needs a frame reserves
 * DIAMOND_JIT_FRAME_SIZE bytes on its own native stack and its prologue
 * writes the frame inline, exactly like run_chunk's own entry setup: zero
 * the fields nothing else sets (pending, handlers, handler_count, and serial
 * through the end), then store `previous` (the old vm->frames), `registers`,
 * `register_count`, `chunk` and `instruction_offset` (the prologue's pad
 * slot, just above the frame, which every call from the body updates with
 * its bytecode offset), and link it onto vm->frames. Every exit unlinks it
 * inline too (vm->frames = frame->previous). DiamondFrame itself is private
 * to the VM, so the offsets the generated code writes are spelled out here
 * and vm.c static_asserts each one against the struct. A JIT'd frame's
 * `pending` and handler fields stay zero unless the function has exception
 * handlers (then they point into its DiamondJitHandlerState, below), and its
 * serial is assigned lazily, only when a closure needs it. */
enum {
    DIAMOND_JIT_FRAME_SIZE = 96,
    DIAMOND_JIT_FRAME_PREVIOUS = 0,
    DIAMOND_JIT_FRAME_REGISTERS = 8,
    DIAMOND_JIT_FRAME_PENDING = 16,        /* pending, handlers: zeroed */
    DIAMOND_JIT_FRAME_HANDLER_COUNT = 32,  /* zeroed */
    DIAMOND_JIT_FRAME_REGISTER_COUNT = 40,
    DIAMOND_JIT_FRAME_CHUNK = 48,
    DIAMOND_JIT_FRAME_INSTRUCTION_OFFSET = 56,
    DIAMOND_JIT_FRAME_SERIAL = 64,         /* serial to the end: zeroed */
};
/* A function with exception handlers (begin/rescue) keeps run_chunk's handler
 * state, a DiamondJitHandlerState (src/vm_internal.h), in its reservation
 * right after the frame; the frame's pending, handlers and handler_count
 * point into it. The prologue zeroes `count` and `pending.kind`. */
enum {
    DIAMOND_JIT_HANDLER_STATE_AT = 96,       /* offset in the reservation */
    DIAMOND_JIT_HANDLER_STATE_SIZE = 1344,
    DIAMOND_JIT_HANDLER_COUNT_FIELD = 0,
    DIAMOND_JIT_HANDLER_RESUME_FIELD = 8,
    DIAMOND_JIT_HANDLER_PENDING_FIELD = 16,  /* pending.kind is its first byte */
    DIAMOND_JIT_HANDLER_HANDLERS_FIELD = 64,
};
struct DiamondJitHandlerState;
/* Exception handling in a compiled function with handlers (src/vm_exceptions.c),
 * each run_chunk's own logic over the function's DiamondJitHandlerState:
 *   - diamond_jit_push_rescue/diamond_jit_pop_rescue: DIAMOND_OP_PUSH_RESCUE
 *     (read from the instruction at `site`) and DIAMOND_OP_POP_RESCUE.
 *   - diamond_jit_catch: a failing `status` meets the handlers, as run_chunk's
 *     VM_PROPAGATE/VM_RETURN do. Returns 0 when a handler caught it (continue
 *     at state->resume), or the status to propagate.
 *   - diamond_jit_raise: DIAMOND_OP_RAISE. `state` is nullptr in a function
 *     without handlers. Returns 0 when caught (continue at state->resume),
 *     else DIAMOND_VM_EXCEPTION with the uncaught message filled in. */
uint8_t diamond_jit_push_rescue(struct DiamondJitHandlerState *state, const uint8_t *site,
        size_t code_count);
uint8_t diamond_jit_pop_rescue(struct DiamondJitHandlerState *state);
uint8_t diamond_jit_catch(DiamondVm *vm, const DiamondChunk *chunk, DiamondValue *registers,
        struct DiamondJitHandlerState *state, uint8_t status);
uint8_t diamond_jit_raise(DiamondVm *vm, const DiamondChunk *chunk, DiamondValue *registers,
        struct DiamondJitHandlerState *state, uint16_t source);
/*   - diamond_jit_push_ensure, diamond_jit_run_ensure, diamond_jit_unwind_jump:
 *     DIAMOND_OP_PUSH_ENSURE, RUN_ENSURE (0: continue at the ensure clause in
 *     state->resume) and UNWIND_JUMP (0: continue at state->resume).
 *   - diamond_jit_return: DIAMOND_OP_RETURN of `*value`. 0 when an enclosing
 *     ensure clause runs first (state->resume); DIAMOND_JIT_HANDLER_RETURNED
 *     once `*result` is written and the function returns.
 *   - diamond_jit_end_ensure: DIAMOND_OP_END_ENSURE, finishing whatever the
 *     clause interrupted: 0 to continue at state->resume, RETURNED as
 *     above, or a status to propagate (it has already met the handlers). */
enum { DIAMOND_JIT_HANDLER_RETURNED = 0xFD };
uint8_t diamond_jit_push_ensure(struct DiamondJitHandlerState *state, size_t target,
        size_t code_count);
uint8_t diamond_jit_run_ensure(struct DiamondJitHandlerState *state, size_t continuation,
        size_t code_count);
uint8_t diamond_jit_unwind_jump(struct DiamondJitHandlerState *state, size_t levels,
        size_t target, size_t code_count);
uint8_t diamond_jit_return(struct DiamondJitHandlerState *state, const DiamondValue *value,
        DiamondValue *result);
uint8_t diamond_jit_end_ensure(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, struct DiamondJitHandlerState *state, DiamondValue *result);
/* Propagate-exit variant: appends the frame's "at name:line:col" trace line for
 * a failing `status` before popping. */
void diamond_jit_frame_pop_propagating(DiamondVm *vm, uint8_t status);
/* Same for a function that never pushed a frame: its failing call stored its
 * offset in the pad slot, which the exit stub passes here with its chunk. */
void diamond_jit_record_propagating(DiamondVm *vm, uint8_t status,
        const DiamondChunk *chunk, size_t offset);

/* JIT trampoline for DIAMOND_OP_STRING -- the first trampoline that CAN
 * allocate (allocate_string calls maybe_collect unconditionally), so any
 * function compiling this opcode must have already pushed a DiamondFrame
 * via the pair above before calling it. */
DiamondVmStatus diamond_jit_new_string(DiamondVm *vm, const DiamondChunk *chunk,
        uint16_t string_index, DiamondValue *out);

/* Phase 2d. JIT trampoline for DIAMOND_OP_HASH -- also allocates
 * (allocate_hash also calls maybe_collect unconditionally), needed for a
 * `= {}` default argument's own construction. `base`/`count` are the same
 * compile-time-known bytecode operands DIAMOND_OP_HASH's own interpreter
 * case reads; the whole key/value-pair loop lives in this trampoline
 * rather than generated code. */
DiamondVmStatus diamond_jit_new_hash(DiamondVm *vm, DiamondValue *registers,
        uint16_t base, uint16_t count, DiamondValue *out);

/* Array literals and default arguments allocate at a GC safepoint. The
 * generated function must publish its registers through a DiamondFrame. */
DiamondVmStatus diamond_jit_new_array(DiamondVm *vm, DiamondValue *registers,
        uint16_t base, uint16_t count, DiamondValue *out);

/* Phase 2e. JIT trampoline for DIAMOND_OP_EQUAL/NOT_EQUAL's general case
 * (mismatched primitive kinds, or FLOAT/OBJECT operands) -- extracted
 * verbatim from that case's own real tail (src/vm.c), which checks for a
 * user-defined `==` override on a DIAMOND_OBJECT_INSTANCE operand via
 * invoke_operator_method *before* falling back to values_equal. Replaces
 * Phase 2d's diamond_jit_values_equal, which called values_equal directly
 * and silently skipped that override check -- a real, shipped correctness
 * bug (see docs/internal/jit-design.md's Phase 2e status note). Because
 * the override call can genuinely invoke arbitrary interpreted code, this
 * is status-bearing (unlike its predecessor): compile_equal_op sets
 * jc->has_called = true unconditionally to account for it. `site` is
 * this occurrence's own bytecode address (`jc->function->code +
 * instruction_start`), the same per-occurrence method-cache key every
 * other overload-checking trampoline here already uses. `negate` is true
 * for NOT_EQUAL. */
DiamondVmStatus diamond_jit_equal_general(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, bool negate, DiamondValue *out);

/* Phase 2d. JIT trampoline for DIAMOND_OP_SUPER -- extracted from that
 * opcode's own interpreter case (src/vm.c) so the two share one
 * implementation. Unlike every trampoline above, this one's own failure
 * can be a genuine, already-happened outcome (most notably
 * DIAMOND_VM_EXCEPTION, an uncaught exception raised somewhere inside the
 * superclass method this invokes) rather than an internal condition safe
 * to retry -- see DiamondJitFn's own comment above for how the compiled
 * caller must treat that. `registers`/`base`/`argc` mirror
 * DIAMOND_OP_SUPER's own operands exactly (self is always registers[0]);
 * `depth` is threaded through so the arbitrarily-deep call chain this can
 * start still respects DIAMOND_MAX_CALL_DEPTH (enforced in jit_call_or_
 * interpret, not here). */
DiamondVmStatus diamond_jit_super_call(DiamondVm *vm, const DiamondChunk *chunk,
        uint16_t owner_index, uint16_t name, DiamondValue *registers, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out);

/* Phase 3. JIT trampolines for ADD_INT/SUBTRACT_INT/MULTIPLY_INT/
 * DIVIDE_INT (diamond_jit_arith_slow) and LESS_INT/LESS_EQUAL_INT/
 * GREATER_INT/GREATER_EQUAL_INT (diamond_jit_compare_slow) -- full
 * extractions of those opcode families' own real slow-path handling
 * (src/vm.c's int_arith_slow/compare_int_slow, shared with the
 * interpreter's own case so the two can never drift apart), covering
 * everything compile_binary_int_op's fast, purely-native path in
 * src/jit.c can't: a non-plain-int operand (deopts the opcode back to
 * its generic form first, exactly like the interpreter's own case
 * already did) and, for the arithmetic family, genuine Int-Int overflow
 * or DIVIDE_INT's own zero/INT64_MIN edge cases, all of which promote to
 * Diamond's real bignum representation rather than erroring.
 *
 * Before this phase, compiling any of these five opcodes was rejected
 * outright at compile time once jc->has_called was already true (see
 * jc->has_called's own comment in jit.c) -- their only edge-case handling
 * was "discard this attempt and retry the whole function," unsafe once a
 * real call with side effects had already run. Since their edge cases now
 * go through these trampolines instead (compute the fully correct answer
 * and either continue inline or propagate a real status, the same shape
 * SET_IVAR/GET_IVAR/INDEX_GET/SET/EQUAL already use), they no longer
 * depend on retry at all -- see compile_binary_int_op's own updated
 * comment in jit.c. Because the Instance-operator-overload branch inside
 * either trampoline can genuinely invoke arbitrary user code, compiling
 * any of these five opcodes at all sets jc->has_called = true
 * unconditionally, the same conservative, compile-time-only choice
 * compile_equal_op already makes. `site` is this occurrence's own
 * bytecode address (jc->function->code + instruction_start), the same
 * per-occurrence convention every other overload-checking trampoline here
 * already uses. */
DiamondVmStatus diamond_jit_arith_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, DiamondOpCode opcode, DiamondValue *out);
DiamondVmStatus diamond_jit_compare_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, DiamondOpCode opcode, DiamondValue *out);

/* DIAMOND_OP_MATH_UNARY (right is nullptr) and MATH_BINARY, sharing
 * run_chunk's arithmetic (diamond_math_apply, src/vm_internal.h). Neither
 * allocates nor runs user code; a non-numeric operand is a type error. */
DiamondVmStatus diamond_jit_math(DiamondVm *vm, const DiamondValue *left,
        const DiamondValue *right, uint8_t function_id, DiamondValue *out);

/* Phase 4. `dup`/`freeze`/`frozen?` on a built-in receiver, shared by
 * the interpreter's DIAMOND_OP_INVOKE dispatch (diamond_invoke_general)
 * and the Instance path (diamond_jit_invoke_instance). None of the three
 * can invoke user code; only `dup`'s Array/Hash branches allocate. A
 * receiver that isn't Array/Hash/String/Symbol/a primitive returns a
 * plain nonzero DiamondVmStatus. Compiled code used to call these
 * directly; it now reaches them through diamond_jit_invoke_any like any
 * other method. */
DiamondVmStatus diamond_jit_dup(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);
DiamondVmStatus diamond_jit_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);
DiamondVmStatus diamond_jit_frozen(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);
/* Not a JIT trampoline: `deep_freeze` (src/vm.c) is declared here only so
 * the interpreter's two dispatch sites share one definition. */
DiamondVmStatus diamond_deep_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out);

/* Phase 7. JIT trampoline for DIAMOND_OP_INVOKE/INVOKE_MONO/INVOKE_TYPED's
 * Instance-receiver dispatch tail -- a full extraction of that case's own
 * real handling (src/vm.c, shared with the interpreter's own case so the
 * two can never drift apart): the tap/dup/freeze/frozen?/respond_to?/
 * public_send universal-method interception (gated on lookup_method first,
 * so a real override wins), exception-instance message/cause/backtrace,
 * the INVOKE<->INVOKE_MONO inline-cache check and self-rewrite, method_
 * missing fallback, visibility checks, and the final invoke_resolved_
 * method_helper call. Kept receiver-position-general (`recv` is a real
 * parameter) for the interpreter's sake -- jit.c's own compile_invoke_
 * dispatch calls this for any compile-time-proven-Instance receiver, not
 * just self (`recv==0`): Phase 7's own self-receiver proof (a function
 * whose owner_class proves register 0 is always the receiver) and Phase
 * 9/10's typed-parameter/freshly-`NEW`'d-local proofs (see jit.c's own
 * parameter_is_single_class/register_new_class_if_sole_writer) all funnel
 * through the same compile_invoke_dispatch call site, passing whichever
 * register their own proof covers. This trampoline still keeps its own
 * unconditional registers[recv] kind check regardless of which proof
 * justified compiling the call (matching diamond_jit_super_call's own
 * belt-and-suspenders precedent), so an error in any of that compile-time
 * reasoning fails safe as a plain DIAMOND_VM_TYPE_ERROR, never a crash.
 * `site` is this occurrence's own bytecode address, the same per-
 * occurrence method-cache/quickening key every other overload-checking
 * trampoline here already uses. `instruction` is the real decoded opcode
 * (INVOKE/INVOKE_MONO/INVOKE_TYPED) -- unlike a plain monomorphic bool,
 * this also lets `type_argument_count`/`type_arguments` (needed for the
 * INVOKE_TYPED case, which the JIT itself never compiles -- see compile_
 * invoke_dispatch's own comment) resolve to the correct `typed` flag for
 * invoke_resolved_method_helper. Can invoke arbitrary user code and
 * allocate (a real method call, method_missing, tap's closure call), so
 * its JIT-side caller sets both jc->needs_frame and jc->has_called
 * unconditionally, exactly like diamond_jit_super_call's own treatment. */
DiamondVmStatus diamond_jit_invoke_instance(DiamondVm *vm, const DiamondChunk *chunk,
        const uint8_t *site, DiamondOpCode instruction, DiamondValue *registers,
        uint16_t recv, uint16_t name, uint16_t base, uint8_t argc,
        uint8_t type_argument_count, const uint16_t *type_arguments,
        size_t depth, DiamondValue *out);

/* DIAMOND_OP_INVOKE/INVOKE_MONO on a receiver the compiler could not prove
 * the type of: the interpreter's whole dispatch, from a warm monomorphic
 * site through every native receiver kind to Instance methods (src/vm_calls.c,
 * shared with run_chunk through diamond_invoke_general). Writes
 * registers[dest]. Can run arbitrary code and allocate, so its caller sets
 * jc->needs_frame and jc->has_called. */
DiamondVmStatus diamond_jit_invoke_any(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, const uint8_t *site, size_t depth);
/* DIAMOND_OP_INVOKE_TYPED, the same dispatch with the instruction's explicit
 * type arguments passed to an Instance's method. Same contract. */
DiamondVmStatus diamond_jit_invoke_typed(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, const uint8_t *site, size_t depth);

/* A generic function's bound chunk and type bindings, in its compiled frame
 * reservation (DiamondJitGenericState, filled by diamond_jit_bind_generic
 * from the prologue). The body runs with JIT_CHUNK and frame->chunk pointing
 * at `execution`, as run_chunk runs in its own bound copy. */
typedef struct DiamondJitGenericState {
    DiamondChunk execution;
    DiamondTypeBinding bindings[8];
} DiamondJitGenericState;
const DiamondChunk *diamond_jit_bind_generic(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *registers, size_t argument_count,
        const DiamondClosure *closure, DiamondJitGenericState *state);

/* DIAMOND_OP_CALL_CLOSURE, read from the instruction at `site`, and
 * DIAMOND_OP_TO_STRING for a non-String value (src/vm_calls.c): run_chunk's
 * call_closure_value and stringify_value. Each writes registers[dest], can
 * run arbitrary code and allocate, so its caller sets jc->needs_frame and
 * jc->has_called. */
DiamondVmStatus diamond_jit_call_closure(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, const uint8_t *site, size_t depth);
DiamondVmStatus diamond_jit_to_string(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t dest, uint16_t source, size_t depth);

/* Runs the instruction at `offset` in `chunk` (the compiled function's own
 * code) through the interpreter's own out-of-line body for it (src/vm_calls.c
 * lists the opcodes; jit.c's opcode_runs_outlined must match). The body
 * decodes its operands itself, so compiled code only passes where the
 * instruction is. Any of them can allocate and most can run arbitrary code,
 * so the caller sets jc->needs_frame and jc->has_called. */
DiamondVmStatus diamond_jit_outlined(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, size_t depth, size_t offset);

/* A direct native self-call's non-OK status (see compile_call_function):
 * runs the staged tail calls, interprets a retried call, or counts and
 * returns a status to propagate, as jit_run_native's callers do. */
DiamondVmStatus diamond_jit_self_call_finish(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *arguments, uint8_t argc, size_t depth, DiamondValue *out,
        uint8_t status);

/* Closures created by compiled code (src/vm_calls.c). diamond_jit_closure
 * is DIAMOND_OP_CLOSURE, read from the instruction at `site` (the same
 * diamond_make_closure run_chunk uses), with the compiled function's own
 * frame (vm->frames) as a block's break and return target; it allocates.
 * BOX_LOCAL allocates a cell; GET_CELL/SET_CELL read and write one, and
 * fail only on bytecode that names a register holding no cell (SET_CELL
 * also on its write barrier running out of memory). None runs user code. */
DiamondVmStatus diamond_jit_closure(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, const uint8_t *site,
        const struct DiamondClosure *closure);
/* DIAMOND_OP_GET_NAMESPACE_CONSTANT (a top-level constant such as `EMAIL`),
 * with run_chunk's uninitialized-constant error. */
DiamondVmStatus diamond_jit_get_namespace_constant(DiamondVm *vm, DiamondValue *registers,
        uint16_t dest, uint16_t index);
/* A block body's GET_CAPTURE (cell false) and GET_CAPTURE_CELL (cell true)
 * and SET_CAPTURE, with run_chunk's checks: `closure` is the one the
 * compiled function was entered with (nullptr outside a closure call, which
 * fails as malformed bytecode, as in the interpreter). */
DiamondVmStatus diamond_jit_get_capture(const struct DiamondClosure *closure,
        DiamondValue *registers, uint16_t dest, uint16_t index, bool cell);
DiamondVmStatus diamond_jit_set_capture(DiamondVm *vm, const struct DiamondClosure *closure,
        DiamondValue *registers, uint16_t index, uint16_t source);
DiamondVmStatus diamond_jit_box_local(DiamondVm *vm, DiamondValue *registers,
        uint16_t reg);
DiamondVmStatus diamond_jit_get_cell(DiamondValue *registers, uint16_t dest,
        uint16_t cell_reg);
DiamondVmStatus diamond_jit_set_cell(DiamondVm *vm, DiamondValue *registers,
        uint16_t cell_reg, uint16_t source);

typedef enum DiamondJitNativeReadOp {
    DIAMOND_JIT_NATIVE_STRING_LENGTH,
    DIAMOND_JIT_NATIVE_STRING_INDEX_OF,
    DIAMOND_JIT_NATIVE_STRING_ORD,
    DIAMOND_JIT_NATIVE_STRING_SLICE,
    DIAMOND_JIT_NATIVE_ARRAY_LENGTH,
    DIAMOND_JIT_NATIVE_HASH_LENGTH,
    DIAMOND_JIT_NATIVE_HASH_KEY_AT,
    DIAMOND_JIT_NATIVE_HASH_VALUE_AT,
} DiamondJitNativeReadOp;

/* Selected native String/collection calls chosen from a compiler-recorded
 * receiver type at this exact INVOKE site. Most are allocation-free reads.
 * String#slice allocates, so its compiler path publishes a DiamondFrame and
 * propagates failures instead of restarting the function. */
DiamondVmStatus diamond_jit_native_call(DiamondVm *vm,
        const DiamondValue *receiver,const DiamondValue *argument,
        DiamondJitNativeReadOp operation,DiamondValue *out);

/* Phase 10: the interpreter's own plain DIAMOND_OP_NEW case (src/vm.c),
 * extracted verbatim -- allocate, run `initialize` if the class defines
 * one (arity-checked exactly as the interpreter does), else the
 * Exception-subclass two-field fallback, else a bare argc==0 requirement.
 * Writes registers[dest] directly (unlike diamond_jit_invoke_instance's
 * separate `out` pointer) since dest already indexes the same registers
 * array both the interpreter and the JIT's own JIT_REGISTERS_BASE share.
 * DIAMOND_OP_NEW_KEYWORDS/DIAMOND_OP_NEW_SPREAD (synthetic-chunk re-entry
 * into run_chunk) are not covered here: compiled code runs them through
 * diamond_jit_outlined, the interpreter's own bodies. Can
 * invoke arbitrary user code (`initialize`) and allocate, so its JIT-side
 * caller (compile_new) sets both jc->needs_frame and jc->has_called
 * unconditionally, same as diamond_jit_super_call/diamond_jit_invoke_
 * instance's own treatment. */
DiamondVmStatus diamond_jit_new_instance(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t dest, uint16_t class_index,
        uint16_t base, uint8_t argc, size_t depth);

/* DIAMOND_OP_CALL (a plain call to a named function): validates exactly as the
 * interpreter's case does and calls the target through jit_call_or_interpret.
 * `depth` is the compiled function's own depth, as for SUPER. The call can
 * run arbitrary user code, so compile_call_function sets needs_frame and
 * has_called. */
DiamondVmStatus diamond_jit_call_function(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t function_index, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out);
/* The same call when the target is the compiled function itself, with the
 * index and argument count already checked at compile time: skips the
 * validation and tier-up checks that cannot change for a function that is
 * running its own native code (see src/vm_calls.c). Same signature, so
 * compile_call_function only swaps the target. */
DiamondVmStatus diamond_jit_call_self(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t function_index, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out);
/* DIAMOND_OP_TAIL_CALL to another function: DIAMOND_JIT_TAIL (callee and
 * arguments staged in vm->jit_tail_*) or the status of an ordinary call. */
uint8_t diamond_jit_tail_call_function(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t function_index, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out);

/* True when `function`'s bytecode contains a backward jump (a loop) and every
 * opcode in it is one the compiler understands. jit_call_or_interpret uses it
 * to compile such a function on its first call: a loop running in a function
 * called once never reaches the call-count threshold. */
bool diamond_jit_function_has_loop(const DiamondFunction *function);

#endif
