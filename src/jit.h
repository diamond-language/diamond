#ifndef DIAMOND_JIT_H
#define DIAMOND_JIT_H

#include "value.h"
#include "vm.h"

/* A minimal, narrowly-scoped baseline JIT -- Phase 2 of the plan recorded in
 * docs/internal/jit-design.md. Compiles ONLY zero-argument, call-free,
 * allocation-free, exception-free functions built entirely from a small
 * whitelist of arithmetic/control-flow opcodes (see jit.c's
 * diamond_jit_try_compile for the exact list) straight to x86-64 machine
 * code. Any function containing anything outside that whitelist is never
 * compiled at all -- there is no partial/mixed-mode execution and no deopt
 * needed for *unsupported opcodes*, only for the runtime-exceptional cases
 * within the supported ones (integer overflow promoting to bignum, division
 * by zero, INT64_MIN/-1) that this narrow slice deliberately doesn't handle
 * inline: on any of those, the compiled function returns false and the
 * caller must fall back to the ordinary run_chunk interpreter path, which
 * remains fully correct and unmodified. This is safe specifically BECAUSE
 * the whitelist excludes anything with an observable side effect (no calls,
 * no object construction, no raises) -- re-running the whole function via
 * the interpreter after a bailout is indistinguishable from having never
 * attempted the JIT at all.
 *
 * Because the whitelist also excludes every allocation/call opcode, a
 * compiled function can never trigger a GC collection or an exception
 * while running -- there is no safepoint inside it at all. It therefore
 * does not need to push a DiamondFrame or publish live registers to the
 * collector the way docs/internal/jit-design.md's general frame contract
 * requires; that contract becomes necessary starting with a Phase 2b that
 * compiles calls, not this slice. Documented here explicitly so it reads
 * as a deliberate, scoped simplification, not an oversight.
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

/* bool(DiamondValue *registers, DiamondValue *result) -- `registers` is a
 * zeroed, argument-populated array exactly like run_chunk's own callee
 * registers (see run_chunk's own entry setup in vm.c for the shape this
 * mirrors). Returns true and writes *result on a normal RETURN reached
 * with no exceptional condition; returns false (leaving *result
 * untouched) the moment any bailout condition above is hit -- the caller
 * must then fall back to run_chunk for a fully correct interpreted
 * execution of the same function. */
typedef bool (*DiamondJitFn)(DiamondValue *registers, DiamondValue *result);

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

#endif
