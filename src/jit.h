#ifndef DIAMOND_JIT_H
#define DIAMOND_JIT_H

#include "value.h"
#include "vm.h"

/* A minimal, narrowly-scoped baseline JIT -- Phases 2, 2b, and 2c of the
 * plan recorded in docs/internal/jit-design.md. Compiles functions built
 * entirely from a small whitelist of arithmetic/control-flow/Hash-read/
 * ivar-write/string-construction opcodes (see jit.c's diamond_jit_try_
 * compile for the exact list) straight to x86-64 machine code --
 * non-generic, non-variadic, any arity (including instance methods,
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
 */

/* Duplicates vm.c's own file-local DIAMOND_INLINE_REGISTER_COUNT (not
 * visible outside vm.c) -- a JIT-eligible function's register_count must
 * fit this v1's call-site stack array (src/vm.c's DIAMOND_OP_CALL
 * interception), which never heap-allocates the way run_chunk itself does
 * for an oversized function. If vm.c's own constant ever changes, this one
 * must be updated to match by hand; there is no shared header value to
 * derive it from without exposing vm.c's own array-sizing choice more
 * broadly than it needs to be. */
enum { DIAMOND_JIT_MAX_REGISTERS = 256 };

/* bool(DiamondVm *vm, DiamondValue *registers, DiamondValue *result,
 * size_t argument_count, const DiamondChunk *chunk) -- `registers` is a
 * zeroed, argument-populated array exactly like run_chunk's own callee
 * registers (see run_chunk's own entry setup in vm.c for the shape this
 * mirrors); `vm` and `chunk` are threaded through purely so generated code
 * can pass them to a trampoline call (SET_IVAR/INDEX_GET/CHECK_TYPE),
 * never dereferenced by generated code itself -- `chunk` specifically is
 * the same DiamondChunk view the interpreter itself would have built for
 * this exact call (see both jit_call_or_interpret call sites in vm.c),
 * needed because a type set index is only meaningful relative to it.
 * `argument_count` is the caller's own raw count (registers[] alone can't
 * answer DIAMOND_OP_ARGUMENT_PROVIDED's "was this parameter actually
 * supplied" question -- a defaulted-and-unsupplied parameter and an
 * explicitly-nil one are indistinguishable in registers[] alone). Known,
 * accepted gap: this doesn't account for DIAMOND_VALUE_UNDEFINED sparse-
 * keyword-call gaps (registers[] never receives an UNDEFINED slot at all,
 * only the interpreter's own `arguments[]` would show one) -- fine for
 * every call site this JIT is wired into today (DIAMOND_OP_CALL, plain
 * positional NEW/SUPER/INVOKE_TYPED dispatch via invoke_resolved_method_
 * helper), none of which are keyword calls; would need revisiting before
 * ever wiring a keyword-call site to this dispatch path. Returns true and
 * writes *result on a normal RETURN reached with no exceptional
 * condition; returns false (leaving *result untouched) the moment any
 * bailout condition above is hit -- the caller must then fall back to
 * run_chunk for a fully correct interpreted execution of the same
 * function. */
typedef bool (*DiamondJitFn)(DiamondVm *vm, DiamondValue *registers,
        DiamondValue *result, size_t argument_count, const DiamondChunk *chunk);

/* Attempts to compile `function` to native code. Returns a callable
 * DiamondJitFn on success (and writes the mmap'd region's size to
 * *out_size, needed later by diamond_jit_free), or nullptr if the function
 * contains anything outside the supported whitelist (checked once; the
 * caller should record that outcome, e.g. via DiamondFunction's own
 * jit_ineligible field, and never retry). The returned pointer is
 * executable memory owned by this module -- see diamond_jit_free. */
void *diamond_jit_try_compile(const DiamondFunction *function, size_t *out_size);

/* Releases the executable memory returned by a prior diamond_jit_try_compile
 * call. Safe to call with nullptr (no-op). */
void diamond_jit_free(void *jit_code, size_t jit_code_size);

/* Trampolines called *from* generated machine code for the two operations
 * too risky to hand-roll directly in x86-64 (field-cache/shape-transition
 * bookkeeping + the GC write barrier for SET_IVAR; Hash lookup for
 * INDEX_GET) -- both defined in vm.c, where the internal helpers they
 * reuse (lookup_field_cached, gc_write_barrier, hash_find) already live
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
DiamondVmStatus diamond_jit_hash_get(const DiamondValue *receiver,
        const DiamondValue *key, DiamondValue *out);
DiamondVmStatus diamond_jit_check_type(const DiamondChunk *chunk,
        const DiamondValue *value, uint16_t set_index);

/* Phase 2c: opaque DiamondFrame management -- DiamondFrame's own layout is
 * private to vm.c (not declared in vm.h), so a JIT'd function can never
 * construct or link one directly; it only ever reserves
 * diamond_jit_frame_size() bytes on its own native stack (queried once per
 * *compile*, not per generated call, so this self-syncs against the
 * struct's real layout instead of duplicating a hardcoded constant) and
 * calls these two to manage it:
 *
 *   diamond_jit_frame_push(storage, vm, registers, register_count, chunk)
 *     placement-constructs a frame into `storage` and links it onto
 *     vm->frames, exactly like run_chunk's own entry setup -- called once,
 *     in a compiled function's own prologue, before anything that can
 *     allocate.
 *   diamond_jit_frame_pop(vm)
 *     unlinks it (vm->frames = vm->frames->previous) -- called once, on
 *     every exit path (the success RETURN and the shared bailout stub),
 *     mirroring run_chunk's own single pop on every return path.
 *
 * A JIT'd frame's `pending` is always nullptr (mark_frame_chain already
 * treats that as "nothing to mark here," and this JIT compiles no begin/
 * rescue, so nothing could ever populate it) and its `instruction_offset`
 * points at a synthetic size_t (zero-initialized, reserved as part of the
 * same storage block) rather than a real, advancing bytecode position --
 * a backtrace captured through a JIT'd frame is therefore approximate,
 * not exact, which is fine today since nothing this JIT calls through
 * these trampolines can itself raise. */
size_t diamond_jit_frame_size(void);
void diamond_jit_frame_push(void *frame_storage, DiamondVm *vm,
        DiamondValue *registers, size_t register_count, const DiamondChunk *chunk);
void diamond_jit_frame_pop(DiamondVm *vm);

/* JIT trampoline for DIAMOND_OP_STRING -- the first trampoline that CAN
 * allocate (allocate_string calls maybe_collect unconditionally), so any
 * function compiling this opcode must have already pushed a DiamondFrame
 * via the pair above before calling it. */
DiamondVmStatus diamond_jit_new_string(DiamondVm *vm, const DiamondChunk *chunk,
        uint16_t string_index, DiamondValue *out);

#endif
