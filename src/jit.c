/* Minimal x86-64 baseline JIT -- see jit.h for the full scope statement.
 * Phase 2 (register moves/constants/_INT arithmetic/comparisons/jumps/
 * return, zero-argument functions only) plus Phase 2b's extension: Hash
 * reads and typed-field writes via C trampolines (jit.h/vm.c), and
 * arguments/`self` (methods and functions, generic and variadic ones included).
 * Anything outside this whitelist (including any exceptional runtime
 * condition within a supported opcode -- overflow, division by zero,
 * INT64_MIN/-1, a trampoline reporting anything but DIAMOND_VM_OK) is a
 * bailout, not a partial compile: the caller falls back to the ordinary,
 * fully-correct run_chunk interpreter.
 *
 * Register plan, persistent for a whole compiled function's execution
 * (pushed in the prologue, popped before every `ret`):
 *   RBX  - the registers array base pointer (2nd incoming argument)
 *   R12  - the DiamondVm pointer (1st incoming argument), only ever moved
 *          into RDI before a trampoline call, never used as a memory base
 *          itself (sidesteps the SIB-byte encoding RSP/R12's shared low-3-
 *          bits-100 rm encoding would otherwise force)
 *   R13  - the *result output pointer (3rd incoming argument), used as a
 *          memory base only at RETURN
 * Scratch (caller-saved, freely clobbered by any trampoline call, so never
 * relied on to survive one): RAX, RCX, RDX.
 *
 * Encoding notes (all instructions below were hand-verified against the
 * Intel SDM, not assumed): every register reference goes through emit_rex,
 * which computes REX.W/R/B generically for any of the 16 GPRs -- Phase 2's
 * "never touch r8-r15" simplification no longer holds now that RBX/R12/R13
 * are persistent roles, so encoding is fully general instead.
 */

/* vm.h reaches <ucontext.h>; expose its POSIX declarations consistently on
 * musl and Darwin before any project header can process libc feature guards.
 * This translation unit is easy to miss because the JIT itself is x86-64,
 * but it is still compiled on every supported target. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "jit.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <stdckdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    REG_RAX = 0, REG_RCX = 1, REG_RDX = 2, REG_RBX = 3,
    REG_RSP = 4, REG_RBP = 5, REG_RSI = 6, REG_RDI = 7,
    REG_R8 = 8, REG_R9 = 9, REG_R10 = 10, REG_R11 = 11,
    REG_R12 = 12, REG_R13 = 13, REG_R14 = 14, REG_R15 = 15,
};

enum {
    JIT_REGISTERS_BASE = REG_RBX,
    JIT_VM = REG_R12,
    JIT_RESULT_PTR = REG_R13,
    JIT_ARGUMENT_COUNT = REG_R14,
    JIT_CHUNK = REG_R15,
    /* Phase 2d: this call's own logical depth, threaded through so a
     * compiled function's own SUPER call can pass it on unchanged (see
     * DiamondJitFn's own comment in jit.h). RBP was free -- this JIT never
     * uses a traditional frame pointer, so repurposing it as a plain
     * 6th persistent GPR is safe, matching the "fully general encoding,
     * no register off-limits" approach already established since Phase
     * 2b. 6 persistent registers is an even count, unlike the prior odd
     * 5 -- see the prologue's own comment for the alignment consequence. */
    JIT_DEPTH = REG_RBP,
};

typedef struct JitBuffer {
    uint8_t *code;
    size_t length;
    size_t capacity;
    bool failed; /* set on allocation failure; checked once at the end */
    /* Phase 2c: when set, every emit_* call becomes a no-op (checked once,
     * at the top of emit_u8, which everything else bottoms out through) --
     * used for a cheap first pass that walks the exact same decode logic
     * as a real compile purely to learn whether the function needs a
     * DiamondFrame (see JitCompiler.needs_frame) before the real pass
     * emits its own prologue, without a second, separately-maintained
     * bytecode-width table that could drift out of sync with the real
     * one. */
    bool dry_run;
    /* Net bytes pushed onto the stack since compile_body began (push/pop and
     * RSP adds/subs adjust it), so emit_body_call can address the prologue's
     * pad slot from wherever RSP has moved to while a call's arguments are
     * staged. Meaningful only inside the body. */
    int32_t rsp_delta;
} JitBuffer;

/* Deferred fixups for jumps to a *bytecode* target -- the native offset for
 * that target may not exist yet (a forward jump) when the jump itself is
 * emitted, so every such jump records where its rel32 field lives and which
 * bytecode offset it must eventually resolve to; a second pass after the
 * whole function is emitted patches every one of these using the by-then
 * complete bytecode-to-native offset table. Purely-local jumps within one
 * opcode's own stencil (see jump_if_false's fallthrough) are NOT deferred --
 * their target is known immediately since it's a fixed, small number of
 * bytes ahead in the same stencil, so those are patched inline instead. */
typedef struct JitPatch {
    size_t native_rel32_offset;
    size_t bytecode_target;
} JitPatch;

typedef struct JitCompiler {
    JitBuffer buf;
    const DiamondFunction *function;
    size_t *bytecode_to_native; /* size code_count; SIZE_MAX = not an instruction boundary */
    JitPatch *patches;
    size_t patch_count;
    size_t patch_capacity;
    bool bailed; /* unsupported construct found; stop compiling immediately */
    /* Phase 2c: set the moment an allocating opcode (STRING) is decoded,
     * in both the dry-run and real passes alike -- read back after the
     * dry run to decide whether the real pass's own prologue needs to
     * reserve a DiamondFrame at all. */
    bool needs_frame;
    /* Byte count reserved on the stack for the frame (0 if !needs_frame),
     * rounded up to a multiple of 16 to keep every trampoline call inside
     * the body correctly aligned -- set once, right before emitting the
     * real prologue, from DIAMOND_JIT_FRAME_SIZE. */
    size_t frame_reserve_bytes;
    /* A generic function's DiamondJitGenericState: its offset in the frame
     * reservation, past the frame (and handler state), for the prologue's
     * emit_generic_bind. Unused (0) in other functions. */
    size_t generic_state_at;
    /* Set by an opcode that reads the closure the function was entered with
     * (GET_CAPTURE, GET_CAPTURE_CELL, SET_CAPTURE, CLOSURE), in the dry run
     * as in the real pass, so the real pass knows it from the start. */
    bool reads_closure;
    /* Set by a slow path that can allocate but sits behind an inline fast
     * path (arithmetic, comparison and == on operands the fast path does
     * not take). A function with only these reserves the frame's space but
     * links the frame just around each slow call (emit_slow_frame_link),
     * so the fast path pays two instructions, not the frame write. */
    bool slow_frame;
    /* The activation can have externally visible effects: a call or a
     * shared-cell store. Set in the dry run too, so every status-bearing
     * bailout uses the catch/propagate path rather than replaying the
     * function against state it may already have changed. */
    bool has_called;
    /* Bytecode offset of the instruction compile_body is emitting, so a
     * trampoline call can record where it is (see emit_body_call). */
    size_t current_instruction_start;
    /* Index of the function being compiled in its program's function table
     * (SIZE_MAX if unknown): a TAIL_CALL to it is a loop, to anything else
     * is left to the interpreter. */
    size_t self_function_index;
    /* Set by PUSH_RESCUE (dry run as real pass): the function keeps a
     * DiamondJitHandlerState after its frame, and a failure from a call
     * goes to the catch stub, which continues at a rescue clause when one
     * matches. `resume_targets` are the bytecode offsets a caught failure
     * can continue at (every PUSH_RESCUE's target), and `resume_keys` the
     * state->resume values the resume stub dispatches on: the target
     * itself, or for a block's break landing after a call,
     * DIAMOND_JIT_RESUME_AFTER_CALL | the call's offset. */
    bool has_handlers;
    bool has_raise;
    size_t resume_keys[64];
    size_t resume_targets[64];
    size_t resume_target_count;
    /* The program's function table (try_compile's `functions`), and the
     * offsets of the calls a block's break lands at in this function,
     * collected from its CLOSURE instructions (the block's
     * block_call_offset). Each call's next instruction is a resume target. */
    struct DiamondFunction *const *functions;
    size_t function_count;
    size_t landing_calls[16];
    size_t landing_call_count;
} JitCompiler;

static void jit_buf_ensure(JitBuffer *buf, size_t extra) {
    if (buf->failed) return;
    size_t needed;
    if (ckd_add(&needed, buf->length, extra)) {
        buf->failed = true;
        return;
    }
    if (needed <= buf->capacity) return;
    size_t new_capacity = buf->capacity == 0 ? 256 : buf->capacity;
    while (new_capacity < needed) {
        if (ckd_mul(&new_capacity, new_capacity, 2)) {
            buf->failed = true;
            return;
        }
    }
    uint8_t *grown = realloc(buf->code, new_capacity);
    if (grown == nullptr) {
        buf->failed = true;
        return;
    }
    buf->code = grown;
    buf->capacity = new_capacity;
}

static void emit_u8(JitBuffer *buf, uint8_t byte) {
    if (buf->dry_run) return;
    jit_buf_ensure(buf, 1);
    if (buf->failed) return;
    buf->code[buf->length++] = byte;
}

static void emit_u32_le(JitBuffer *buf, uint32_t value) {
    emit_u8(buf, (uint8_t)(value & 0xFF));
    emit_u8(buf, (uint8_t)((value >> 8) & 0xFF));
    emit_u8(buf, (uint8_t)((value >> 16) & 0xFF));
    emit_u8(buf, (uint8_t)((value >> 24) & 0xFF));
}

static void emit_u64_le(JitBuffer *buf, uint64_t value) {
    emit_u32_le(buf, (uint32_t)(value & 0xFFFFFFFFu));
    emit_u32_le(buf, (uint32_t)(value >> 32));
}

/* REX prefix, generic over any of the 16 GPRs: W selects 64-bit operand
 * size; reg_field/rm_field are the ModRM "reg" and "rm" (or SIB base 3-bit
 * field, when used for a memory operand's base register) values *before*
 * masking to 3 bits -- this function reads their high bit to decide
 * REX.R/REX.B. Every memory base register this file actually uses (RBX=3,
 * R13=13) and every scratch register (RAX/RCX/RDX=0-2) has low-3-bits != 100,
 * so none of them ever needs a SIB byte -- ModRM mod=10 (disp32) + rm alone
 * always means plain [reg+disp32] addressing here, never RSP/R12-relative
 * or RIP-relative (those are the only two rm=100/rm=101-with-mod=00
 * special cases in the ModRM encoding, and neither applies to mod=10). */
static uint8_t emit_rex(bool w, int reg_field, int rm_field) {
    return (uint8_t)(0x40 | (w ? 0x08 : 0) | (reg_field >= 8 ? 0x04 : 0) | (rm_field >= 8 ? 0x01 : 0));
}

/* mov reg64, [base + disp32] */
static void emit_load_r64(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x8B);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov reg32, [base + disp32], zero-extending into reg64 */
static void emit_load_r32(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(false, reg, base));
    emit_u8(buf, 0x8B);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov [base + disp32], reg64 */
static void emit_store_r64(JitBuffer *buf, int base, int32_t disp, int reg) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x89);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov [base + disp32], reg64 for base rsp or r12, whose rm encoding needs a
 * SIB byte (emit_store_r64 would address something else). */
static void emit_store_r64_sib(JitBuffer *buf, int base, int32_t disp, int reg) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x89);
    emit_u8(buf, (uint8_t)(0x84 | ((reg & 7) << 3)));
    emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov reg64, [base + disp32] for base rsp or r12. */
static void emit_load_r64_sib(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x8B);
    emit_u8(buf, (uint8_t)(0x84 | ((reg & 7) << 3)));
    emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)disp);
}

/* xorps xmm0, xmm0 */
static void emit_zero_xmm0(JitBuffer *buf) {
    emit_u8(buf, 0x0F); emit_u8(buf, 0x57); emit_u8(buf, 0xC0);
}
/* movups [rsp + disp32], xmm<n> (n < 8) */
static void emit_movups_store_rsp(JitBuffer *buf, int32_t disp, int xmm) {
    emit_u8(buf, 0x0F); emit_u8(buf, 0x11);
    emit_u8(buf, (uint8_t)(0x84 | (xmm << 3)));  /* mod 10, rm 100: SIB follows */
    emit_u8(buf, 0x24);                          /* SIB: base rsp, no index */
    emit_u32_le(buf, (uint32_t)disp);
}
/* movups xmm<n>, [base + disp32] (n < 8; base not rsp or r12) */
static void emit_movups_load(JitBuffer *buf, int xmm, int base, int32_t disp) {
    if (base >= 8) emit_u8(buf, emit_rex(false, xmm, base));
    emit_u8(buf, 0x0F); emit_u8(buf, 0x10);
    emit_u8(buf, (uint8_t)(0x80 | (xmm << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* lea reg64, [base + disp32] */
static void emit_lea(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x8D);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* lea reg, [rsp + disp32]: an RSP base needs a SIB byte, which emit_lea
 * does not emit. */
static void emit_lea_rsp(JitBuffer *buf, int reg, int32_t disp) {
    emit_u8(buf, emit_rex(true, reg, REG_RSP));
    emit_u8(buf, 0x8D);
    emit_u8(buf, (uint8_t)(0x84 | ((reg & 7) << 3)));
    emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)disp);
}

/* movzx reg32, byte [base + disp32] -- zero-extends into full reg64.
 * Only ever called with reg/base in {RAX,RCX,RDX,RBX,R13} in this file, all
 * of which read as their "new," REX-independent low byte (AL/CL/DL/BL) or
 * need REX only for the >=8 extension, never the legacy AH/CH/DH/BH
 * ambiguity (that's specific to encodings 4-7 *without* REX, none of which
 * this function is ever asked to target as the 8-bit destination here --
 * this reads a byte from memory into a full register, so only `base`'s
 * addressing matters, not a competing 8-bit register-file selection). */
static void emit_load_byte_zx(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(false, reg, base));
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov byte [base + disp32], imm8 */
static void emit_store_kind_imm(JitBuffer *buf, int base, int32_t disp, uint8_t kind) {
    emit_u8(buf, emit_rex(false, 0, base));
    emit_u8(buf, 0xC6);
    emit_u8(buf, (uint8_t)(0x80 | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
    emit_u8(buf, kind);
}

/* mov byte [base + disp32], reg8 -- reg must be RAX/RCX/RDX/RBX (AL/CL/DL/BL),
 * the only 8-bit sources this file ever stores from. */
static void emit_store_byte_reg(JitBuffer *buf, int base, int32_t disp, int reg) {
    emit_u8(buf, 0x88);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov reg64, imm64 */
static void emit_mov_imm64(JitBuffer *buf, int reg, uint64_t imm) {
    emit_u8(buf, emit_rex(true, 0, reg));
    emit_u8(buf, (uint8_t)(0xB8 + (reg & 7)));
    emit_u64_le(buf, imm);
}

/* mov dst64, src64 (register-register) */
static void emit_mov_rr(JitBuffer *buf, int dst, int src) {
    emit_u8(buf, emit_rex(true, src, dst));
    emit_u8(buf, 0x89);
    emit_u8(buf, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

/* cmp reg64, imm8 (sign-extended) */
static void emit_cmp_imm8(JitBuffer *buf, int reg, int8_t imm) {
    emit_u8(buf, emit_rex(true, 0, reg));
    emit_u8(buf, 0x83);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | (reg & 7))); /* /7 = CMP */
    emit_u8(buf, (uint8_t)imm);
}

/* cmp reg32, imm32 -- used only for the small kind-tag comparisons (0-6) */
static void emit_cmp_imm32_32(JitBuffer *buf, int reg, uint32_t imm) {
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | (reg & 7)));
    emit_u32_le(buf, imm);
}

/* cmp reg64, imm32 (sign-extended) -- for comparing a full 64-bit value
 * (JIT_ARGUMENT_COUNT) against a small non-negative constant. */
static void emit_cmp_imm32_64(JitBuffer *buf, int reg, uint32_t imm) {
    emit_u8(buf, emit_rex(true, 0, reg));
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | (reg & 7)));
    emit_u32_le(buf, imm);
}

/* test reg64, reg64 */
static void emit_test_r64(JitBuffer *buf, int a, int b) {
    emit_u8(buf, emit_rex(true, b, a));
    emit_u8(buf, 0x85);
    emit_u8(buf, (uint8_t)(0xC0 | ((b & 7) << 3) | (a & 7)));
}

/* test al/cl/dl/bl, same reg (8-bit) */
static void emit_test_r8(JitBuffer *buf, int reg) {
    emit_u8(buf, 0x84);
    emit_u8(buf, (uint8_t)(0xC0 | ((reg & 7) << 3) | (reg & 7)));
}

/* add/sub/cmp dst64, src64 (register-register) */
static void emit_alu_rr(JitBuffer *buf, uint8_t opcode, int dst, int src) {
    emit_u8(buf, emit_rex(true, src, dst));
    emit_u8(buf, opcode);
    emit_u8(buf, (uint8_t)(0xC0 | ((src & 7) << 3) | (dst & 7)));
}
enum { ALU_ADD = 0x01, ALU_SUB = 0x29, ALU_XOR = 0x31, ALU_CMP = 0x39 };

/* Call site ids (method calls, `new`, `==`), unique across every compiled
 * function (compilation can run on several threads at once): each indexes
 * vm->jit_call_sites. */
static atomic_uint_fast32_t jit_next_call_site;

/* cmp reg64, [base + disp32] (base not rsp or r12) */
static void emit_cmp_r64_mem(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x3B);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}


/* imul dst64, src64 (two-operand form; sets OF/CF on signed 64-bit overflow) */
static void emit_imul_rr(JitBuffer *buf, int dst, int src) {
    emit_u8(buf, emit_rex(true, dst, src));
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xAF);
    emit_u8(buf, (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

/* cqo: sign-extend RAX into RDX:RAX */
static void emit_cqo(JitBuffer *buf) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x99);
}

/* idiv reg64: RDX:RAX / reg -> quotient in RAX, remainder in RDX */
static void emit_idiv(JitBuffer *buf, int reg) {
    emit_u8(buf, emit_rex(true, 0, reg));
    emit_u8(buf, 0xF7);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | (reg & 7))); /* /7 = IDIV */
}

/* add/sub/and/cmp reg64, imm32 (sign-extended); `ext` is the ModRM
 * opcode extension of the 0x81 group. */
enum { ALU_IMM_ADD = 0, ALU_IMM_AND = 4, ALU_IMM_SUB = 5, ALU_IMM_CMP = 7 };
static void emit_alu_imm32(JitBuffer *buf, int ext, int reg, int32_t imm) {
    emit_u8(buf, emit_rex(true, 0, reg));
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (ext << 3) | (reg & 7)));
    emit_u32_le(buf, (uint32_t)imm);
}

/* imul dst64, src64, imm32 (sets OF on signed 64-bit overflow) */
static void emit_imul_imm32(JitBuffer *buf, int dst, int src, int32_t imm) {
    emit_u8(buf, emit_rex(true, dst, src));
    emit_u8(buf, 0x69);
    emit_u8(buf, (uint8_t)(0xC0 | ((dst & 7) << 3) | (src & 7)));
    emit_u32_le(buf, (uint32_t)imm);
}

/* imul src64, one-operand form: RDX:RAX = RAX * src, signed */
static void emit_imul_wide(JitBuffer *buf, int src) {
    emit_u8(buf, emit_rex(true, 0, src));
    emit_u8(buf, 0xF7);
    emit_u8(buf, (uint8_t)(0xC0 | (5 << 3) | (src & 7)));
}

/* shl/shr/sar reg64, imm8 */
enum { SHIFT_SHL = 4, SHIFT_SHR = 5, SHIFT_SAR = 7 };
static void emit_shift_imm(JitBuffer *buf, int ext, int reg, uint8_t count) {
    emit_u8(buf, emit_rex(true, 0, reg));
    emit_u8(buf, 0xC1);
    emit_u8(buf, (uint8_t)(0xC0 | (ext << 3) | (reg & 7)));
    emit_u8(buf, count);
}


/* Phase 5: setcc al, condition byte generic -- mirrors emit_jcc_
 * placeholder's own generic-condition convention. Replaces the
 * originally-separate, LESS_INT-only emit_setl_al now that compile_
 * binary_int_op's own comparison branch covers all four orderings
 * (LESS/LESS_EQUAL/GREATER/GREATER_EQUAL) and both the generic and
 * _INT form of each. */
enum {
    SETCC_L = 0x9C,
    SETCC_GE = 0x9D,
    SETCC_LE = 0x9E,
    SETCC_G = 0x9F,
    SETCC_A = 0x97,
    SETCC_AE = 0x93,
};
static void emit_setcc_al(JitBuffer *buf, uint8_t condition) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, condition);
    emit_u8(buf, 0xC0);
}

/* sete al */
static void emit_sete_al(JitBuffer *buf) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x94);
    emit_u8(buf, 0xC0);
}

/* seta al -- unsigned "above" (CF=0 and ZF=0) */
static void emit_seta_al(JitBuffer *buf) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x97);
    emit_u8(buf, 0xC0);
}

/* xor al, 1 -- flips a 0/1 boolean-in-AL in place */
static void emit_xor_al_1(JitBuffer *buf) {
    emit_u8(buf, 0x34);
    emit_u8(buf, 0x01);
}

/* movzx reg64, al -- zero-extends AL into a full 64-bit register */
static void emit_movzx_r64_al(JitBuffer *buf, int reg) {
    emit_u8(buf, emit_rex(true, reg, 0));
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, (uint8_t)(0xC0 | ((reg & 7) << 3)));
}

/* push/pop reg64 (no REX.W needed -- push/pop default to 64-bit in long
 * mode; REX.B is still needed for r8-r15). */
static void emit_push(JitBuffer *buf, int reg) {
    buf->rsp_delta += 8;
    if (reg >= 8) emit_u8(buf, 0x41);
    emit_u8(buf, (uint8_t)(0x50 + (reg & 7)));
}
static void emit_pop(JitBuffer *buf, int reg) {
    buf->rsp_delta -= 8;
    if (reg >= 8) emit_u8(buf, 0x41);
    emit_u8(buf, (uint8_t)(0x58 + (reg & 7)));
}

/* sub rsp, imm32 / add rsp, imm32 -- reserving/releasing stack space for a
 * DiamondFrame (Phase 2c). /0 = ADD, /5 = SUB, per the Intel SDM's opcode
 * 81 extension table. */
static void emit_add_rsp_imm32(JitBuffer *buf, uint32_t imm) {
    buf->rsp_delta -= (int32_t)imm;
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (0 << 3) | REG_RSP));
    emit_u32_le(buf, imm);
}
static void emit_sub_rsp_imm32(JitBuffer *buf, uint32_t imm) {
    buf->rsp_delta += (int32_t)imm;
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (5 << 3) | REG_RSP));
    emit_u32_le(buf, imm);
}

/* call reg64 */
static void emit_call_reg(JitBuffer *buf, int reg) {
    if (reg >= 8) emit_u8(buf, 0x41);
    emit_u8(buf, 0xFF);
    emit_u8(buf, (uint8_t)(0xC0 | (2 << 3) | (reg & 7))); /* /2 = CALL r/m64 */
}

/* mov rdi, <imm64 function address>; call rdi -- the shared "call a C
 * trampoline" stencil. Arguments must already be loaded into
 * RDI/RSI/RDX/RCX/R8/R9 by the caller (SysV order) before this runs. */
static void emit_call_trampoline(JitBuffer *buf, void *function_address) {
    emit_mov_imm64(buf, REG_R11, (uint64_t)(uintptr_t)function_address);
    emit_call_reg(buf, REG_R11);
}

/* ret */
/* A trampoline call from the function body. Stores the current instruction's
 * bytecode offset into the stack slot just above the DiamondFrame reservation
 * (the prologue's alignment pad, which a published frame's instruction_offset
 * points at), so that
 *   - a backtrace taken while the callee runs names this function and the
 *     call's source line, and
 *   - a function that propagates a failure (no re-run, see has_called) can
 *     append its own "at name:line:col" trace line from the exit stub, with or
 *     without a frame (see emit_epilogue_propagate).
 * Not used by the prologue/epilogue, where RSP is not yet/no longer the body's. */
static void emit_store_instruction_offset(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    emit_u8(buf, 0x48);                       /* REX.W */
    emit_u8(buf, 0xC7);                       /* MOV r/m64, imm32 */
    emit_u8(buf, 0x84);                       /* ModRM: [rsp + disp32] */
    emit_u8(buf, 0x24);                       /* SIB: rsp */
    emit_u32_le(buf, (uint32_t)((int32_t)jc->frame_reserve_bytes + buf->rsp_delta));
    emit_u32_le(buf, (uint32_t)jc->current_instruction_start);
}

/* Loads the closure the function was entered with (the entry's seventh
 * argument, nullptr outside a closure call) into `reg`. The caller left it
 * on the stack just above the return address; the prologue's six pushes and
 * pad slot (56 bytes) and the frame reservation sit below it, plus whatever
 * the current instruction has pushed (rsp_delta). */
static void emit_load_closure(JitCompiler *jc, int reg) {
    emit_load_r64_sib(&jc->buf, reg, REG_RSP,
        (int32_t)jc->frame_reserve_bytes + jc->buf.rsp_delta + 7 * 8 + 8);
}

/* Writes this function's DiamondFrame into its reservation (layout in
 * jit.h) and links it onto vm->frames: zero the fields nothing else sets,
 * fill the five run_chunk's entry sets, point instruction_offset at the
 * pad slot just above the reservation. Only RAX and XMM0 are touched. Each
 * field is stored once: zeroing the whole frame first measured as
 * store-buffer stalls. `at` is the reservation's offset from RSP: 0 in the
 * prologue, rsp_delta for a slow path mid-instruction. */
static void emit_frame_link(JitCompiler *jc, int32_t at) {
    JitBuffer *buf = &jc->buf;
    const int32_t reserve = (int32_t)jc->frame_reserve_bytes;
    const DiamondFunction *function = jc->function;
    const size_t live_register_count = function->register_count == 0
        ? DIAMOND_JIT_MAX_REGISTERS : function->register_count;
    emit_zero_xmm0(buf);
    emit_movups_store_rsp(buf, at + DIAMOND_JIT_FRAME_PENDING, 0);
    for (int32_t field = DIAMOND_JIT_FRAME_SERIAL; field < DIAMOND_JIT_FRAME_SIZE; field += 16)
        emit_movups_store_rsp(buf, at + field, 0);
    emit_u8(buf, 0x31); emit_u8(buf, 0xC0);   /* xor eax, eax */
    emit_store_r64_sib(buf, REG_RSP, at + DIAMOND_JIT_FRAME_HANDLER_COUNT, REG_RAX);
    emit_store_r64_sib(buf, REG_RSP, at + reserve, REG_RAX);
    emit_load_r64_sib(buf, REG_RAX, JIT_VM, (int32_t)offsetof(DiamondVm, frames));
    emit_store_r64_sib(buf, REG_RSP, at + DIAMOND_JIT_FRAME_PREVIOUS, REG_RAX);
    emit_store_r64_sib(buf, REG_RSP, at + DIAMOND_JIT_FRAME_REGISTERS, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RAX, live_register_count);
    emit_store_r64_sib(buf, REG_RSP, at + DIAMOND_JIT_FRAME_REGISTER_COUNT, REG_RAX);
    emit_store_r64_sib(buf, REG_RSP, at + DIAMOND_JIT_FRAME_CHUNK, JIT_CHUNK);
    emit_u8(buf, 0x48); emit_u8(buf, 0x8D);   /* lea rax, [rsp + at + reserve] */
    emit_u8(buf, 0x84); emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)(at + reserve));
    emit_store_r64_sib(buf, REG_RSP, at + DIAMOND_JIT_FRAME_INSTRUCTION_OFFSET, REG_RAX);
    if (at == 0) {
        emit_store_r64_sib(buf, JIT_VM, (int32_t)offsetof(DiamondVm, frames), REG_RSP);
    } else {
        emit_u8(buf, 0x48); emit_u8(buf, 0x8D);   /* lea rax, [rsp + at] */
        emit_u8(buf, 0x84); emit_u8(buf, 0x24);
        emit_u32_le(buf, (uint32_t)at);
        emit_store_r64_sib(buf, JIT_VM, (int32_t)offsetof(DiamondVm, frames), REG_RAX);
    }
}

/* Around a slow call that can allocate (see JitCompiler.slow_frame): when
 * the function has no frame of its own, link one for the call's duration so
 * a collection sees this function's registers. The unlink goes through RCX
 * and keeps AL, the call's status. Both are no-ops in a function that
 * already keeps its frame linked. */
static void emit_slow_frame_link(JitCompiler *jc) {
    jc->slow_frame = true;
    if (!jc->needs_frame) emit_frame_link(jc, jc->buf.rsp_delta);
}
static void emit_slow_frame_unlink(JitCompiler *jc) {
    if (jc->needs_frame) return;
    JitBuffer *buf = &jc->buf;
    emit_load_r64_sib(buf, REG_RCX, REG_RSP, buf->rsp_delta + DIAMOND_JIT_FRAME_PREVIOUS);
    emit_store_r64_sib(buf, JIT_VM, (int32_t)offsetof(DiamondVm, frames), REG_RCX);
}

static void emit_body_call(JitCompiler *jc, void *function_address) {
    emit_store_instruction_offset(jc);
    emit_call_trampoline(&jc->buf, function_address);
}

/* The function's DiamondJitHandlerState (jit.h) into `reg`: it sits at a
 * fixed offset in the frame reservation, below whatever the current
 * instruction has pushed. */
static void emit_handler_state_address(JitCompiler *jc, int reg) {
    emit_lea_rsp(&jc->buf, reg, jc->buf.rsp_delta + DIAMOND_JIT_HANDLER_STATE_AT);
}

/* Prologue of a function with handlers, right after emit_frame_link: point
 * the frame's pending, handlers and handler_count at the handler state and
 * empty it (count 0, pending.kind PENDING_NONE), as run_chunk's locals
 * start. Only RAX is touched. */
static void emit_handler_state_link(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    const int32_t state = DIAMOND_JIT_HANDLER_STATE_AT;
    emit_lea_rsp(buf, REG_RAX, state + DIAMOND_JIT_HANDLER_PENDING_FIELD);
    emit_store_r64_sib(buf, REG_RSP, DIAMOND_JIT_FRAME_PENDING, REG_RAX);
    emit_lea_rsp(buf, REG_RAX, state + DIAMOND_JIT_HANDLER_HANDLERS_FIELD);
    emit_store_r64_sib(buf, REG_RSP, DIAMOND_JIT_FRAME_PENDING + 8, REG_RAX);
    emit_lea_rsp(buf, REG_RAX, state + DIAMOND_JIT_HANDLER_COUNT_FIELD);
    emit_store_r64_sib(buf, REG_RSP, DIAMOND_JIT_FRAME_HANDLER_COUNT, REG_RAX);
    emit_u8(buf, 0x31); emit_u8(buf, 0xC0);   /* xor eax, eax */
    emit_store_r64_sib(buf, REG_RSP, state + DIAMOND_JIT_HANDLER_COUNT_FIELD, REG_RAX);
    emit_store_r64_sib(buf, REG_RSP, state + DIAMOND_JIT_HANDLER_PENDING_FIELD, REG_RAX);
}

static void emit_ret(JitBuffer *buf) { emit_u8(buf, 0xC3); }

/* mov al, imm8 -- the two bool/status return-value stencils */
static void emit_mov_al_imm8(JitBuffer *buf, uint8_t imm) {
    emit_u8(buf, 0xB0);
    emit_u8(buf, imm);
}

/* Restores all 6 persistent registers in reverse push order, undoing the
 * prologue's own alignment pad first. 6 is an even count (unlike Phase
 * 2c's 5), so unlike before this JIT needs one, unconditionally -- see the
 * prologue's own comment in diamond_jit_try_compile. Shared by every exit
 * stub (success, retry, and Phase 2d's propagate). */
static void emit_pop_persistent_registers(JitBuffer *buf) {
    emit_add_rsp_imm32(buf, 8);
    emit_pop(buf, JIT_DEPTH);
    emit_pop(buf, JIT_CHUNK);
    emit_pop(buf, JIT_ARGUMENT_COUNT);
    emit_pop(buf, JIT_RESULT_PTR);
    emit_pop(buf, JIT_VM);
    emit_pop(buf, JIT_REGISTERS_BASE);
}

/* If the function needed a DiamondFrame (Phase 2c), pops and unlinks it
 * and releases its stack space FIRST -- symmetric with the prologue's own
 * push-then-reserve order, and must happen before the persistent-register
 * pops below since it still needs JIT_VM live and still owns the stack
 * space directly above those registers' own saved values. Every exit path
 * that isn't Phase 2d's propagate stub (the success RETURN and the retry
 * bailout stub) calls this immediately before `ret`, after its own AL has
 * already been set. */
/* Whether the caller (jit_run_native, src/vm_calls.c) hands this function a
 * register file from the heap rather than its stack buffer: more registers
 * than the buffer holds (try_compile refuses a register_count of 0). The
 * function frees that file itself on the way out (emit_free_heap_registers),
 * so the caller's common path has nothing to check after the call. Must
 * match jit_run_native's own test exactly. */
static bool uses_heap_registers(const DiamondFunction *function) {
    return function->register_count > DIAMOND_JIT_MAX_REGISTERS;
}

/* free(registers) on an exit path, keeping the status in AL: it rides in
 * JIT_RESULT_PTR, which no exit path needs again (the persistent-register
 * pops restore it). The prologue's pad keeps RSP 16-byte aligned at the body
 * level whether or not the function reserved a frame, so the call is safe
 * wherever an epilogue starts. */
static void emit_free_heap_registers(JitCompiler *jc) {
    if (!uses_heap_registers(jc->function)) return;
    JitBuffer *buf = &jc->buf;
    emit_u8(buf, 0x0F); emit_u8(buf, 0xB6); emit_u8(buf, 0xC0);   /* movzx eax, al */
    emit_mov_rr(buf, JIT_RESULT_PTR, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_REGISTERS_BASE);
    emit_call_trampoline(buf, (void *)(uintptr_t)free);
    emit_mov_rr(buf, REG_RAX, JIT_RESULT_PTR);
}

static void emit_epilogue(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    /* An exit stub unwinds the stack on its own path only: code emitted after
     * it (a RETURN inside the body) still runs with the body's RSP. */
    const int32_t saved_rsp_delta = buf->rsp_delta;
    emit_free_heap_registers(jc);
    if (jc->needs_frame) {
        /* vm->frames = frame->previous, through RCX: AL holds the status. */
        emit_load_r64_sib(buf, REG_RCX, REG_RSP, DIAMOND_JIT_FRAME_PREVIOUS);
        emit_store_r64_sib(buf, JIT_VM, (int32_t)offsetof(DiamondVm, frames), REG_RCX);
        emit_add_rsp_imm32(buf, (uint32_t)jc->frame_reserve_bytes);
    } else if (jc->slow_frame) {
        emit_add_rsp_imm32(buf, (uint32_t)jc->frame_reserve_bytes);
    }
    emit_pop_persistent_registers(buf);
    buf->rsp_delta = saved_rsp_delta;
}

/* Phase 2d: shared tail for the "propagate" bailout stub -- like
 * emit_epilogue, but preserves AL (the trampoline's own real
 * DiamondVmStatus, which the caller must see unchanged) across the
 * frame_pop call, which -- being an ordinary C function -- clobbers
 * caller-saved registers, AL included. Stashes it in JIT_RESULT_PTR
 * temporarily: none of the 6 persistent registers' *current* meanings are
 * needed again on this exit path except JIT_VM (for the call itself), and
 * emit_pop_persistent_registers' own pops restore each register's real
 * (caller's) value from the stack regardless of what's briefly stored in
 * it here.
 *
 * Bug fixed here (found empirically, a real SIGSEGV, not by inspection):
 * this function used to pop a DiamondFrame unconditionally, on the belief
 * that "jc->has_called is only ever set alongside jc->needs_frame (SUPER
 * sets both)". That was already false the moment compile_equal_op's
 * general case (Phase 2e) started setting has_called without needs_frame
 * (EQUAL's own override branch never allocates), and every one of Phase
 * 3/5's arithmetic/comparison opcodes does the exact same thing -- so a
 * function whose only has_called-setting opcode is one of those, followed
 * by a *different* opcode (SET_IVAR, INDEX_GET/SET, ...) that genuinely
 * needs to propagate, reaches this stub with no frame ever having been
 * pushed. Popping one anyway pops whatever frame
 * actually is on top of vm->frames -- the caller's, not this function's
 * own (nonexistent) one -- corrupting the frame chain, not a crash at this
 * call site itself but on some later, unrelated frame operation. Guarded
 * behind jc->needs_frame now, exactly like emit_epilogue's own success/
 * retry exit already correctly does -- see tests/cases/jit_propagate_
 * without_frame.d6, which reproduces this exact shape. */
static void emit_epilogue_propagate(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    const int32_t saved_rsp_delta = buf->rsp_delta;
    /* The trampoline's status is a uint8_t: only AL is defined, so zero-extend
     * (movzx eax, al) before handing it on as an argument. */
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, 0xC0);
    emit_mov_rr(buf, JIT_RESULT_PTR, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_RESULT_PTR);
    if (jc->needs_frame) {
        emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_frame_pop_propagating);
    } else {
        /* No frame, but the failing call still stored its offset in the pad
         * slot at [rsp]: record this function's trace line from there. */
        emit_mov_rr(buf, REG_RDX, JIT_CHUNK);
        /* mov rcx, [rsp + reserve]: above the space a slow call links its
         * frame in, if any (already unlinked by then). */
        emit_load_r64_sib(buf, REG_RCX, REG_RSP, (int32_t)jc->frame_reserve_bytes);
        emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_record_propagating);
    }
    if (uses_heap_registers(jc->function)) {
        emit_mov_rr(buf, REG_RDI, JIT_REGISTERS_BASE);
        emit_call_trampoline(buf, (void *)(uintptr_t)free);
    }
    emit_mov_rr(buf, REG_RAX, JIT_RESULT_PTR);
    if (jc->needs_frame || jc->slow_frame)
        emit_add_rsp_imm32(buf, (uint32_t)jc->frame_reserve_bytes);
    emit_pop_persistent_registers(buf);
    buf->rsp_delta = saved_rsp_delta;
}

/* Near conditional jump to a not-yet-known local offset: emits `0F 8x
 * rel32` with a zero placeholder and returns the buffer offset of the
 * rel32 field, to be patched via patch_rel32_here once the fallthrough
 * point is reached (still within the same instruction's own stencil, so
 * this is never deferred to the cross-function patch list). */
enum {
    JCC_O = 0x80,
    JCC_AE = 0x83,
    JCC_E = 0x84,
    JCC_NE = 0x85,
    JCC_BE = 0x86,
    JCC_A = 0x87,
    JCC_NS = 0x89,
};
static size_t emit_jcc_placeholder(JitBuffer *buf, uint8_t condition) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, condition);
    size_t at = buf->length;
    emit_u32_le(buf, 0);
    return at;
}
static size_t emit_jmp_placeholder(JitBuffer *buf) {
    emit_u8(buf, 0xE9);
    size_t at = buf->length;
    emit_u32_le(buf, 0);
    return at;
}
/* Patches a rel32 field (at buffer offset `field_offset`) so it jumps to
 * the current end of the buffer -- valid only when that target is already
 * known, i.e. always for a local, same-stencil fallthrough. */
static void patch_rel32_to_here(JitBuffer *buf, size_t field_offset) {
    if (buf->dry_run || buf->failed) return;
    int32_t rel = (int32_t)(buf->length - (field_offset + 4));
    buf->code[field_offset] = (uint8_t)(rel & 0xFF);
    buf->code[field_offset + 1] = (uint8_t)((rel >> 8) & 0xFF);
    buf->code[field_offset + 2] = (uint8_t)((rel >> 16) & 0xFF);
    buf->code[field_offset + 3] = (uint8_t)((rel >> 24) & 0xFF);
}

static void record_global_patch(JitCompiler *jc, size_t rel32_offset, size_t bytecode_target) {
    if (jc->buf.dry_run) return;
    if (jc->patch_count >= jc->patch_capacity) {
        size_t new_capacity = 16, bytes;
        if ((jc->patch_capacity != 0 && ckd_mul(&new_capacity, jc->patch_capacity, 2)) ||
            ckd_mul(&bytes, new_capacity, sizeof *jc->patches)) {
            jc->bailed = true;
            return;
        }
        JitPatch *grown = realloc(jc->patches, bytes);
        if (grown == nullptr) {
            jc->bailed = true;
            return;
        }
        jc->patches = grown;
        jc->patch_capacity = new_capacity;
    }
    jc->patches[jc->patch_count].native_rel32_offset = rel32_offset;
    jc->patches[jc->patch_count].bytecode_target = bytecode_target;
    jc->patch_count++;
}

/* Two shared bailout stubs, not one (Phase 2d) -- a single tail (epilogue
 * + `mov al,<sentinel>` + `ret`) is no longer enough once a compiled
 * function can make a real call (SUPER): "retry" is the original Phase
 * 2/2b/2c behavior (discard this attempt, fall back to run_chunk), safe
 * only before the first call has run; "propagate" hands back the
 * trampoline's own already-real DiamondVmStatus unchanged instead (see
 * emit_epilogue_propagate's own comment) -- required from the first call
 * onward, since retrying would risk invoking it a second time. Both
 * native offsets are recorded once and resolved via the same deferred-
 * patch mechanism as any other jump target, just not through
 * bytecode_to_native (see compile function below) since neither is a
 * real bytecode offset. */

#define BAILOUT_RETRY_SENTINEL SIZE_MAX
#define BAILOUT_PROPAGATE_SENTINEL (SIZE_MAX - 1)
/* A function with handlers: "catch" offers a failing status to its rescue
 * clauses first (diamond_jit_catch) and propagates only what none takes;
 * "resume" continues at the clause a caught failure picked. */
#define BAILOUT_CATCH_SENTINEL (SIZE_MAX - 2)
#define BAILOUT_RESUME_SENTINEL (SIZE_MAX - 3)

/* A trampoline's failing DiamondVmStatus in AL: retry only when the
 * activation cannot have made visible changes. Otherwise offer the error
 * to this activation's handlers, or propagate it if there are none. */
static size_t bailout_target(const JitCompiler *jc) {
    if (!jc->has_called) return BAILOUT_RETRY_SENTINEL;
    return jc->has_handlers ? BAILOUT_CATCH_SENTINEL : BAILOUT_PROPAGATE_SENTINEL;
}

static int32_t reg_disp(size_t index, int32_t field_offset) {
    return (int32_t)(index * sizeof(DiamondValue)) + field_offset;
}

static const int32_t KIND_OFF = offsetof(DiamondValue, kind);

/* Inline cell and capture access (GET_CELL, SET_CELL and a block's
 * GET_CAPTURE, GET_CAPTURE_CELL, SET_CAPTURE). Each guard appends the jumps
 * taken on anything unusual to `fail`; those land on the instruction's
 * trampoline, which repeats the access with run_chunk's checks and errors,
 * so the inline path only ever handles the plain case. */
static void emit_object_kind_guard(JitBuffer *buf, int base, int32_t disp, uint32_t kind,
        size_t *fail, size_t *count) {
    /* The value at [base + disp] is an object of kind `kind`; the object
     * pointer is left in RDX. Clobbers RCX. */
    emit_load_byte_zx(buf, REG_RCX, base, disp + KIND_OFF);
    emit_cmp_imm32_32(buf, REG_RCX, DIAMOND_VALUE_OBJECT);
    fail[(*count)++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_r64(buf, REG_RDX, base, disp + 8);
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondObject, kind));
    emit_cmp_imm32_32(buf, REG_RCX, kind);
    fail[(*count)++] = emit_jcc_placeholder(buf, JCC_NE);
}

static void emit_cell_guard(JitBuffer *buf, int base, int32_t disp, size_t *fail, size_t *count) {
    emit_object_kind_guard(buf, base, disp, DIAMOND_OBJECT_CELL, fail, count);
}

/* Copies a 16-byte DiamondValue as two 8-byte moves through RSI, as MOVE
 * does (a 16-byte load right after 8-byte stores would stall on store
 * forwarding). */
static void emit_copy_value(JitBuffer *buf, int dst, int32_t dst_disp, int src, int32_t src_disp) {
    emit_load_r64(buf, REG_RSI, src, src_disp);
    emit_store_r64(buf, dst, dst_disp, REG_RSI);
    emit_load_r64(buf, REG_RSI, src, src_disp + 8);
    emit_store_r64(buf, dst, dst_disp + 8, REG_RSI);
}

/* gc_write_barrier's test on the cell in RDX: an old object not yet in the
 * remembered set takes the trampoline, which stores again and remembers
 * it. Clobbers RCX. */
static void emit_barrier_guard(JitBuffer *buf, size_t *fail, size_t *count) {
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondObject, old));
    emit_test_r8(buf, REG_RCX);
    const size_t young = emit_jcc_placeholder(buf, JCC_E);
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondObject, remembered));
    emit_test_r8(buf, REG_RCX);
    fail[(*count)++] = emit_jcc_placeholder(buf, JCC_E);
    patch_rel32_to_here(buf, young);
}

static int32_t capture_disp(uint16_t index) {
    return (int32_t)(offsetof(DiamondClosure, captures) + (size_t)index * sizeof(DiamondValue));
}

/* The closure the function was entered with into RAX, failing to `fail`
 * when there is none or it has no capture `index`. Clobbers RCX. */
static void emit_capture_guard(JitCompiler *jc, uint16_t index, size_t *fail, size_t *count) {
    JitBuffer *buf = &jc->buf;
    emit_load_closure(jc, REG_RAX);
    emit_u8(buf, 0x48); emit_u8(buf, 0x85); emit_u8(buf, 0xC0);   /* test rax, rax */
    fail[(*count)++] = emit_jcc_placeholder(buf, JCC_E);
    emit_load_byte_zx(buf, REG_RCX, REG_RAX, (int32_t)offsetof(DiamondClosure, capture_count));
    emit_cmp_imm32_32(buf, REG_RCX, index);
    fail[(*count)++] = emit_jcc_placeholder(buf, JCC_BE);
}
static const int32_t AS_OFF = offsetof(DiamondValue, as);

/* Phase 3: unlike the removed emit_check_kind_int_or_bail, this jumps to a
 * LOCAL, not-yet-known target within the same opcode's own stencil (the
 * slow-path trampoline call compile_binary_int_op emits further down),
 * patched via patch_rel32_to_here once that target's address is known --
 * the same local-jump convention compile_equal_op's own kind checks
 * already use, not the deferred cross-function patch list. */
static size_t emit_check_kind_int_or_jump(JitCompiler *jc, uint16_t reg_index, int scratch) {
    emit_load_byte_zx(&jc->buf, scratch, JIT_REGISTERS_BASE, reg_disp(reg_index, KIND_OFF));
    emit_cmp_imm32_32(&jc->buf, scratch, DIAMOND_VALUE_INT);
    return emit_jcc_placeholder(&jc->buf, JCC_NE);
}

/* A supervised attempt's VM carries an interrupt flag another thread sets to
 * stop it (Supervisor#stop(timeout_ms), a sibling restart). The interpreter
 * looks at it between instructions; native code has no such point, so a
 * loop would never notice. Emitted on every backward jump and tail-call
 * loop: load vm->interrupt_flag, and if it is set and true, leave through
 * the propagate stub with DIAMOND_VM_INTERRUPTED (never a retry: the
 * interruption must not be re-run). The flag lives on the VM and compiled
 * code is shared between VMs, so the check is unconditional; a VM with no
 * flag costs one load, one test and one not-taken branch per iteration. */
static void emit_interrupt_poll(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    emit_store_instruction_offset(jc);
    /* JIT_VM is r12, whose ModRM base encoding needs a SIB byte the load
     * helpers do not emit: go through RAX. */
    emit_mov_rr(buf, REG_RAX, JIT_VM);
    emit_load_r64(buf, REG_RAX, REG_RAX, (int32_t)offsetof(DiamondVm, interrupt_flag));
    emit_test_r64(buf, REG_RAX, REG_RAX);
    const size_t no_flag = emit_jcc_placeholder(buf, JCC_E);
    emit_load_byte_zx(buf, REG_RAX, REG_RAX, 0);
    emit_test_r8(buf, REG_RAX);
    const size_t not_set = emit_jcc_placeholder(buf, JCC_E);
    emit_mov_al_imm8(buf, (uint8_t)DIAMOND_VM_INTERRUPTED);
    const size_t field = emit_jmp_placeholder(buf);
    record_global_patch(jc, field, BAILOUT_PROPAGATE_SENTINEL);
    patch_rel32_to_here(buf, no_flag);
    patch_rel32_to_here(buf, not_set);
}

/* Jumps to the appropriate shared bailout stub (see bailout_target) the
 * moment AL (a just-returned DiamondVmStatus, zero-extended by the SysV
 * ABI's own small-return-type convention) is nonzero -- i.e. anything but
 * DIAMOND_VM_OK. Used after every trampoline call. */
static void emit_bail_if_al_nonzero(JitCompiler *jc) {
    emit_test_r8(&jc->buf, REG_RAX);
    emit_u8(&jc->buf, 0x0F);
    emit_u8(&jc->buf, JCC_NE);
    size_t field = jc->buf.length;
    emit_u32_le(&jc->buf, 0);
    record_global_patch(jc, field, bailout_target(jc));
}

static bool decode_u8(const DiamondFunction *fn, size_t *pc, uint8_t *out) {
    if (*pc >= fn->code_count) return false;
    *out = fn->code[(*pc)++];
    return true;
}
static bool decode_u16(const DiamondFunction *fn, size_t *pc, uint16_t *out) {
    if (*pc + 1 >= fn->code_count) return false;
    *out = (uint16_t)(((unsigned)fn->code[*pc] << 8) | fn->code[*pc + 1]);
    *pc += 2;
    return true;
}

/* Phase 3: shared tail for every edge case compile_binary_int_op's
 * arithmetic branches (ADD_INT/SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT) can
 * hit -- a non-plain-int operand, overflow, or DIVIDE_INT's own zero/
 * INT64_MIN cases -- calling diamond_jit_arith_slow (src/vm.c) to
 * recompute the fully correct answer (bignum promotion, Float/String/
 * Instance-override/Time handling, the same deopt-to-generic bytecode
 * rewrite the interpreter's own case already does) and either write it
 * into registers[dest] and let generated code continue, or bail with the
 * trampoline's own real status via emit_bail_if_al_nonzero. 8 arguments;
 * the last two (opcode, out) go on the stack per the SysV ABI's own
 * overflow convention, pushed in reverse (out, then opcode) -- the same
 * shape compile_equal_op's own trampoline call already uses. */
static void emit_arith_slow_call(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                                  uint16_t left, uint16_t right, DiamondOpCode op) {
    JitBuffer *buf = &jc->buf;
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);
    emit_mov_imm64(buf, REG_RAX, (uint64_t)(unsigned)op);
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(left, 0));
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(right, 0));
    emit_slow_frame_link(jc);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_arith_slow);
    emit_slow_frame_unlink(jc);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
}

/* Same shape as emit_arith_slow_call, for compile_binary_int_op's
 * LESS_INT branch, calling diamond_jit_compare_slow instead -- comparisons
 * never overflow, so a non-plain-int operand is the only edge case here. */
static void emit_compare_slow_call(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                                    uint16_t left, uint16_t right, DiamondOpCode op) {
    JitBuffer *buf = &jc->buf;
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);
    emit_mov_imm64(buf, REG_RAX, (uint64_t)(unsigned)op);
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(left, 0));
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(right, 0));
    emit_slow_frame_link(jc);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_compare_slow);
    emit_slow_frame_unlink(jc);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
}

/* Phase 3: ADD_INT/SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT/LESS_INT no
 * longer reject compiling once jc->has_called is already true (see
 * compile_body's own switch case) -- every edge case that previously had
 * nowhere safe to go but "discard and retry the whole function" (a
 * non-Int operand, arithmetic overflow, division by zero, INT64_MIN/-1)
 * now calls a dedicated slow-path trampoline instead (emit_arith_slow_
 * call/emit_compare_slow_call above), which recomputes the correct
 * result from scratch and either lets generated code continue or
 * propagates a real status -- the same "call a trampoline, never retry"
 * shape every other call-capable opcode here already uses. Because the
 * Instance-override branch inside either trampoline can genuinely invoke
 * arbitrary user code, compiling any of these opcodes at all sets
 * jc->has_called = true unconditionally -- the same conservative,
 * compile-time-only choice compile_equal_op already makes, regardless of
 * whether a given occurrence's actual runtime operands ever reach that
 * branch. Every local bail jump below targets the shared slow-call block
 * emitted once per occurrence, patched via patch_rel32_to_here exactly
 * like compile_equal_op's own kinds_differ/general-case branches.
 *
 * Phase 5: also compiles the *generic* (non-`_INT`) forms of every one
 * of these opcodes -- ADD/SUBTRACT/MULTIPLY/DIVIDE/LESS/LESS_EQUAL/
 * GREATER/GREATER_EQUAL -- plus the three `_INT` comparison forms that
 * were never added even for the typed case (LESS_EQUAL_INT/GREATER_INT/
 * GREATER_EQUAL_INT). This needed no new trampoline or vm.c change at
 * all: emit_arith_slow_call/emit_compare_slow_call already forward
 * whatever opcode was actually compiled, and int_arith_slow/compare_
 * int_slow (src/vm.c, Phase 3) already handle being called with an
 * already-generic opcode correctly -- they only attempt the deopt-to-
 * generic bytecode rewrite when the opcode passed in actually *is* one
 * of the `_INT` forms (`generic != opcode`), skipping that step and
 * dispatching directly otherwise. The fast native path's own kind check
 * (emit_check_kind_int_or_jump: "is this DIAMOND_VALUE_INT") is already
 * exactly as correct for a generic opcode as for an `_INT` one -- a
 * generic ADD with two plain Int operands is exactly as safe to add
 * natively as ADD_INT's own confirmed-Int case, and the slow call
 * handles every other kind (String, Float, Instance, Time, bignum)
 * either way. This is why a value the compiler could never prove `Int`
 * at compile time (an untyped parameter, a Hash/Array element) no
 * longer disables the JIT for its entire containing function the moment
 * it's used in arithmetic. */
/* SSE2 scalar-double forms used by the Float fast path below. Every
 * memory operand is [base + disp32] with base RBX (see emit_rex), and
 * only xmm0/xmm1 are used, so a REX byte is only emitted for a base of
 * R8-R15. */
enum { SSE_ADDSD = 0x58, SSE_MULSD = 0x59, SSE_SUBSD = 0x5C, SSE_DIVSD = 0x5E };
static void emit_sse_mem(JitBuffer *buf, uint8_t prefix, uint8_t opcode, int xmm, int base,
                         int32_t disp) {
    emit_u8(buf, prefix);
    if (base >= 8) emit_u8(buf, emit_rex(false, xmm, base));
    emit_u8(buf, 0x0F);
    emit_u8(buf, opcode);
    emit_u8(buf, (uint8_t)(0x80 | ((xmm & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}
/* movsd xmm, [base + disp32] */
static void emit_movsd_load(JitBuffer *buf, int xmm, int base, int32_t disp) {
    emit_sse_mem(buf, 0xF2, 0x10, xmm, base, disp);
}
/* movsd [base + disp32], xmm */
static void emit_movsd_store(JitBuffer *buf, int base, int32_t disp, int xmm) {
    emit_sse_mem(buf, 0xF2, 0x11, xmm, base, disp);
}
/* ucomisd xmm_a, xmm_b (xmm0-xmm7 only) */
static void emit_ucomisd_rr(JitBuffer *buf, int a, int b) {
    emit_u8(buf, 0x66);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x2E);
    emit_u8(buf, (uint8_t)(0xC0 | ((a & 7) << 3) | (b & 7)));
}

static bool binary_op_is_comparison(DiamondOpCode op) {
    return op == DIAMOND_OP_LESS || op == DIAMOND_OP_LESS_INT ||
        op == DIAMOND_OP_LESS_EQUAL || op == DIAMOND_OP_LESS_EQUAL_INT ||
        op == DIAMOND_OP_GREATER || op == DIAMOND_OP_GREATER_INT ||
        op == DIAMOND_OP_GREATER_EQUAL || op == DIAMOND_OP_GREATER_EQUAL_INT;
}

/* Both operands Float: the arithmetic or comparison done with SSE2, the
 * same IEEE double operation the interpreter's C code does (so 1.0 / 0.0
 * is Infinity and every comparison with NaN is false). Emitted where the
 * Int path's kind checks land; any other kind pair (Int with Float
 * included) jumps to the slow call through `slow_jumps`. Returns the
 * placeholder of the jump past the slow call. Comparisons compare with the
 * operands ordered so the answer is "above" or "above or equal", the two
 * conditions an unordered (NaN) result never sets. */
static size_t emit_float_binary(JitCompiler *jc, uint16_t dest, uint16_t left, uint16_t right,
                                DiamondOpCode op, size_t *slow_jumps, size_t *slow_jump_count) {
    JitBuffer *buf = &jc->buf;
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, KIND_OFF));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_FLOAT);
    slow_jumps[(*slow_jump_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(right, KIND_OFF));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_FLOAT);
    slow_jumps[(*slow_jump_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    if (binary_op_is_comparison(op)) {
        const bool less = op == DIAMOND_OP_LESS || op == DIAMOND_OP_LESS_INT ||
            op == DIAMOND_OP_LESS_EQUAL || op == DIAMOND_OP_LESS_EQUAL_INT;
        const bool or_equal = op == DIAMOND_OP_LESS_EQUAL || op == DIAMOND_OP_LESS_EQUAL_INT ||
            op == DIAMOND_OP_GREATER_EQUAL || op == DIAMOND_OP_GREATER_EQUAL_INT;
        /* left < right is right above left; left > right is left above right. */
        emit_movsd_load(buf, 0, JIT_REGISTERS_BASE, reg_disp(less ? right : left, AS_OFF));
        emit_movsd_load(buf, 1, JIT_REGISTERS_BASE, reg_disp(less ? left : right, AS_OFF));
        emit_ucomisd_rr(buf, 0, 1);
        emit_setcc_al(buf, or_equal ? SETCC_AE : SETCC_A);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
        emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
        return emit_jmp_placeholder(buf);
    }
    const uint8_t sse_op =
        (op == DIAMOND_OP_ADD || op == DIAMOND_OP_ADD_INT) ? SSE_ADDSD :
        (op == DIAMOND_OP_SUBTRACT || op == DIAMOND_OP_SUBTRACT_INT) ? SSE_SUBSD :
        (op == DIAMOND_OP_MULTIPLY || op == DIAMOND_OP_MULTIPLY_INT) ? SSE_MULSD : SSE_DIVSD;
    emit_movsd_load(buf, 0, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
    emit_sse_mem(buf, 0xF2, sse_op, 0, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
    emit_movsd_store(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), 0);
    emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_FLOAT);
    return emit_jmp_placeholder(buf);
}

/* The tail every compile_binary_int_op branch shares: `done_int` is the
 * Int path's jump past everything else. A kind mismatch (`kind_jumps`)
 * tries the Float path first unless `float_capable` is false (MODULO);
 * every remaining edge case reaches the slow call. */
static void finish_binary_op(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                             uint16_t left, uint16_t right, DiamondOpCode op, size_t done_int,
                             const size_t *kind_jumps, size_t *slow_jumps, size_t slow_jump_count,
                             bool float_capable) {
    JitBuffer *buf = &jc->buf;
    size_t done_float = 0;
    bool has_float = false;
    if (float_capable) {
        for (size_t i = 0; i < 2; i++) patch_rel32_to_here(buf, kind_jumps[i]);
        done_float = emit_float_binary(jc, dest, left, right, op, slow_jumps, &slow_jump_count);
        has_float = true;
    } else {
        for (size_t i = 0; i < 2; i++) slow_jumps[slow_jump_count++] = kind_jumps[i];
    }
    for (size_t i = 0; i < slow_jump_count; i++) patch_rel32_to_here(buf, slow_jumps[i]);
    if (binary_op_is_comparison(op))
        emit_compare_slow_call(jc, instruction_start, dest, left, right, op);
    else
        emit_arith_slow_call(jc, instruction_start, dest, left, right, op);
    patch_rel32_to_here(buf, done_int);
    if (has_float) patch_rel32_to_here(buf, done_float);
}

static void compile_binary_int_op(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                                   uint16_t left, uint16_t right, DiamondOpCode op) {
    JitBuffer *buf = &jc->buf;
    jc->has_called = true;
    size_t kind_jumps[2];
    size_t slow_jumps[8];
    size_t slow_jump_count = 0;
    kind_jumps[0] = emit_check_kind_int_or_jump(jc, left, REG_RAX);
    kind_jumps[1] = emit_check_kind_int_or_jump(jc, right, REG_RAX);
    if (binary_op_is_comparison(op)) {
        const uint8_t condition =
            (op == DIAMOND_OP_LESS || op == DIAMOND_OP_LESS_INT) ? SETCC_L :
            (op == DIAMOND_OP_LESS_EQUAL || op == DIAMOND_OP_LESS_EQUAL_INT) ? SETCC_LE :
            (op == DIAMOND_OP_GREATER || op == DIAMOND_OP_GREATER_INT) ? SETCC_G : SETCC_GE;
        emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
        emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
        emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
        emit_setcc_al(buf, condition);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
        emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
        size_t done = emit_jmp_placeholder(buf);
        finish_binary_op(jc, instruction_start, dest, left, right, op, done, kind_jumps,
                         slow_jumps, slow_jump_count, true);
        return;
    }
    if (op == DIAMOND_OP_MODULO) {
        /* Floored, like the interpreter's MODULO case: idiv leaves C's
         * truncated remainder in RDX, and a nonzero remainder whose sign
         * differs from the divisor's gets the divisor added. A divisor of
         * 0 (division by zero) or -1 (INT64_MIN % -1 traps in idiv) goes
         * to the slow call, which answers 0 for -1. */
        emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
        emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
        emit_test_r64(buf, REG_RCX, REG_RCX);
        slow_jumps[slow_jump_count++] = emit_jcc_placeholder(buf, JCC_E);
        emit_cmp_imm8(buf, REG_RCX, -1);
        slow_jumps[slow_jump_count++] = emit_jcc_placeholder(buf, JCC_E);
        emit_cqo(buf);
        emit_idiv(buf, REG_RCX);
        emit_test_r64(buf, REG_RDX, REG_RDX);
        size_t exact = emit_jcc_placeholder(buf, JCC_E);
        emit_mov_rr(buf, REG_RAX, REG_RDX);
        emit_alu_rr(buf, ALU_XOR, REG_RAX, REG_RCX);
        size_t same_sign = emit_jcc_placeholder(buf, JCC_NS);
        emit_alu_rr(buf, ALU_ADD, REG_RDX, REG_RCX);
        patch_rel32_to_here(buf, exact);
        patch_rel32_to_here(buf, same_sign);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
        emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RDX);
        size_t done = emit_jmp_placeholder(buf);
        finish_binary_op(jc, instruction_start, dest, left, right, op, done, kind_jumps,
                         slow_jumps, slow_jump_count, false);
        return;
    }
    if (op == DIAMOND_OP_DIVIDE_INT || op == DIAMOND_OP_DIVIDE) {
        emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
        emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
        emit_test_r64(buf, REG_RCX, REG_RCX);
        slow_jumps[slow_jump_count++] = emit_jcc_placeholder(buf, JCC_E);
        /* left==INT64_MIN && right==-1 -> slow call (bignum negate) */
        emit_cmp_imm8(buf, REG_RCX, -1);
        size_t skip = emit_jcc_placeholder(buf, JCC_NE);
        emit_mov_imm64(buf, REG_RDX, (uint64_t)INT64_MIN);
        emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RDX);
        size_t jne_ok = emit_jcc_placeholder(buf, JCC_NE);
        slow_jumps[slow_jump_count++] = emit_jmp_placeholder(buf);
        patch_rel32_to_here(buf, jne_ok);
        patch_rel32_to_here(buf, skip);
        emit_cqo(buf);
        emit_idiv(buf, REG_RCX);
        /* Floored, like diamond_int_floor_divide: idiv truncates, so a
         * nonzero remainder (RDX) whose sign differs from the divisor's
         * steps the quotient (RAX) down by one. */
        emit_test_r64(buf, REG_RDX, REG_RDX);
        size_t exact = emit_jcc_placeholder(buf, JCC_E);
        emit_alu_rr(buf, ALU_XOR, REG_RDX, REG_RCX);
        size_t same_sign = emit_jcc_placeholder(buf, JCC_NS);
        emit_mov_imm64(buf, REG_RDX, 1);
        emit_alu_rr(buf, ALU_SUB, REG_RAX, REG_RDX);
        patch_rel32_to_here(buf, exact);
        patch_rel32_to_here(buf, same_sign);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
        emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
        size_t done = emit_jmp_placeholder(buf);
        finish_binary_op(jc, instruction_start, dest, left, right, op, done, kind_jumps,
                         slow_jumps, slow_jump_count, true);
        return;
    }
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
    emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
    if (op == DIAMOND_OP_ADD_INT || op == DIAMOND_OP_ADD) {
        emit_alu_rr(buf, ALU_ADD, REG_RAX, REG_RCX);
    } else if (op == DIAMOND_OP_SUBTRACT_INT || op == DIAMOND_OP_SUBTRACT) {
        emit_alu_rr(buf, ALU_SUB, REG_RAX, REG_RCX);
    } else {
        emit_imul_rr(buf, REG_RAX, REG_RCX);
    }
    slow_jumps[slow_jump_count++] = emit_jcc_placeholder(buf, JCC_O);
    emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
    size_t done = emit_jmp_placeholder(buf);
    finish_binary_op(jc, instruction_start, dest, left, right, op, done, kind_jumps,
                     slow_jumps, slow_jump_count, true);
}

/* The magic multiplier and shift that turn signed 64-bit division by a
 * constant `divisor` (at least 3, not a power of two) into a multiply
 * (Hacker's Delight, figure 10-1): q = (mulhi(x, M) [+ x if M < 0]) >> s,
 * plus one when x is negative. */
static void signed_division_magic(uint64_t divisor, int64_t *multiplier, int *shift) {
    const uint64_t two63 = UINT64_C(1) << 63;
    const uint64_t anc = two63 - 1 - two63 % divisor;
    int p = 63;
    uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
    uint64_t q2 = two63 / divisor, r2 = two63 - q2 * divisor;
    uint64_t delta = 0;
    do {
        p++;
        q1 *= 2; r1 *= 2;
        if (r1 >= anc) { q1++; r1 -= anc; }
        q2 *= 2; r2 *= 2;
        if (r2 >= divisor) { q2++; r2 -= divisor; }
        delta = divisor - r2;
    } while (q1 < delta || (q1 == delta && r1 == 0));
    *multiplier = (int64_t)(q2 + 1);
    *shift = p - 64;
}

/* The constant-operand forms (ADD_INT_K and friends but EQUAL/NOT_EQUAL):
 * `right` is a known Int, so only `left`'s kind is checked, the constant is
 * an immediate, and `% constant` for a positive constant up to INT32_MAX is
 * a mask (powers of two) or a multiply by the magic reciprocal instead of
 * idiv, whose latency bounded `x = (x + y) % k` loops. The scratch register
 * the interpreter never reads is filled only on the slow path, which calls
 * the generic opcode's slow helper with it as the right operand. */
static void compile_int_k_op(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                             uint16_t left, uint16_t scratch, int64_t constant,
                             DiamondOpCode generic) {
    JitBuffer *buf = &jc->buf;
    jc->has_called = true;
    size_t slow[4];
    size_t slow_count = 0;
    const bool fits = constant >= INT32_MIN && constant <= INT32_MAX;
    slow[slow_count++] = emit_check_kind_int_or_jump(jc, left, REG_RAX);
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
    if (!fits) emit_mov_imm64(buf, REG_RCX, (uint64_t)constant);
    if (binary_op_is_comparison(generic)) {
        const uint8_t condition =
            generic == DIAMOND_OP_LESS ? SETCC_L :
            generic == DIAMOND_OP_LESS_EQUAL ? SETCC_LE :
            generic == DIAMOND_OP_GREATER ? SETCC_G : SETCC_GE;
        if (fits) emit_alu_imm32(buf, ALU_IMM_CMP, REG_RAX, (int32_t)constant);
        else emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
        emit_setcc_al(buf, condition);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
        emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
    } else if (generic == DIAMOND_OP_MODULO && constant > 0 && constant <= INT32_MAX) {
        if ((constant & (constant - 1)) == 0) {
            /* Floored modulo by a positive power of two is the low bits,
             * negative dividends included (-7 % 8 == 1). */
            emit_alu_imm32(buf, ALU_IMM_AND, REG_RAX, (int32_t)(constant - 1));
        } else {
            int64_t multiplier = 0;
            int shift = 0;
            signed_division_magic((uint64_t)constant, &multiplier, &shift);
            emit_mov_rr(buf, REG_RCX, REG_RAX);                  /* rcx = x */
            emit_mov_imm64(buf, REG_RAX, (uint64_t)multiplier);
            emit_imul_wide(buf, REG_RCX);                        /* rdx = mulhi(x, M) */
            if (multiplier < 0) emit_alu_rr(buf, ALU_ADD, REG_RDX, REG_RCX);
            if (shift > 0) emit_shift_imm(buf, SHIFT_SAR, REG_RDX, (uint8_t)shift);
            emit_mov_rr(buf, REG_RAX, REG_RCX);
            emit_shift_imm(buf, SHIFT_SHR, REG_RAX, 63);
            emit_alu_rr(buf, ALU_ADD, REG_RDX, REG_RAX);         /* rdx = x / k, truncated */
            emit_imul_imm32(buf, REG_RDX, REG_RDX, (int32_t)constant);
            emit_mov_rr(buf, REG_RAX, REG_RCX);
            emit_alu_rr(buf, ALU_SUB, REG_RAX, REG_RDX);         /* rax = truncated remainder */
            /* Floored: a negative remainder gets the (positive) divisor added. */
            emit_mov_rr(buf, REG_RDX, REG_RAX);
            emit_shift_imm(buf, SHIFT_SAR, REG_RDX, 63);
            emit_alu_imm32(buf, ALU_IMM_AND, REG_RDX, (int32_t)constant);
            emit_alu_rr(buf, ALU_ADD, REG_RAX, REG_RDX);
        }
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
        emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
    } else if (generic == DIAMOND_OP_MODULO) {
        /* Zero, negative or wide divisors: idiv, floored as in
         * compile_binary_int_op. 0 and -1 always take the slow path. */
        if (constant == 0 || constant == -1) {
            slow[slow_count++] = emit_jmp_placeholder(buf);
        } else {
            if (fits) emit_mov_imm64(buf, REG_RCX, (uint64_t)constant);
            emit_cqo(buf);
            emit_idiv(buf, REG_RCX);
            emit_test_r64(buf, REG_RDX, REG_RDX);
            size_t exact = emit_jcc_placeholder(buf, JCC_E);
            emit_mov_rr(buf, REG_RAX, REG_RDX);
            emit_alu_rr(buf, ALU_XOR, REG_RAX, REG_RCX);
            size_t same_sign = emit_jcc_placeholder(buf, JCC_NS);
            emit_alu_rr(buf, ALU_ADD, REG_RDX, REG_RCX);
            patch_rel32_to_here(buf, exact);
            patch_rel32_to_here(buf, same_sign);
            emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
            emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RDX);
        }
    } else {
        if (generic == DIAMOND_OP_MULTIPLY) {
            if (fits) emit_imul_imm32(buf, REG_RAX, REG_RAX, (int32_t)constant);
            else emit_imul_rr(buf, REG_RAX, REG_RCX);
        } else {
            const bool add = generic == DIAMOND_OP_ADD;
            if (fits) emit_alu_imm32(buf, add ? ALU_IMM_ADD : ALU_IMM_SUB, REG_RAX, (int32_t)constant);
            else emit_alu_rr(buf, add ? ALU_ADD : ALU_SUB, REG_RAX, REG_RCX);
        }
        slow[slow_count++] = emit_jcc_placeholder(buf, JCC_O);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
        emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
    }
    const size_t done = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(scratch, KIND_OFF), DIAMOND_VALUE_INT);
    emit_mov_imm64(buf, REG_RAX, (uint64_t)constant);
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(scratch, AS_OFF), REG_RAX);
    if (binary_op_is_comparison(generic))
        emit_compare_slow_call(jc, instruction_start, dest, left, scratch, generic);
    else
        emit_arith_slow_call(jc, instruction_start, dest, left, scratch, generic);
    patch_rel32_to_here(buf, done);
}

/* EQUAL/NOT_EQUAL. A fast CPU-comparison path handles both operands NIL,
 * BOOL, or INT with *matching* kinds -- never a real call, no bail check
 * needed, matches DIAMOND_OP_EQUAL/NOT_EQUAL's own INT fast path exactly.
 * Everything else (mismatched kinds, or matching FLOAT/OBJECT kinds)
 * calls diamond_jit_equal_general (Phase 2e), which checks for a `==`
 * override on an Instance operand before falling back to values_equal --
 * see that trampoline's own comment in jit.h for why this replaced Phase
 * 2d's diamond_jit_values_equal (a real, shipped correctness bug: the old
 * trampoline skipped the override check entirely). Because the general
 * case can now genuinely invoke arbitrary code, jc->has_called is set
 * unconditionally here -- every compiled EQUAL/NOT_EQUAL structurally
 * *could* reach the override branch at runtime, regardless of whether a
 * given call's actual operands turn out to be Instances, so this is a
 * conservative, compile-time-only decision exactly like compile_super_
 * call's own jc->has_called = true. */
static void compile_equal_op(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                              uint16_t left, uint16_t right, bool negate) {
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, KIND_OFF));
    emit_load_byte_zx(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, KIND_OFF));
    emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
    size_t kinds_differ = emit_jcc_placeholder(buf, JCC_NE); /* differ -> general case */
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_NIL);
    size_t not_nil = emit_jcc_placeholder(buf, JCC_NE);
    /* both NIL: always equal */
    emit_mov_imm64(buf, REG_RAX, negate ? 0 : 1);
    size_t nil_done = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, not_nil);
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_BOOL);
    size_t not_bool = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
    emit_load_byte_zx(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
    emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
    emit_sete_al(buf);
    if (negate) emit_xor_al_1(buf);
    emit_movzx_r64_al(buf, REG_RAX);
    size_t bool_done = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, not_bool);
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_INT);
    size_t not_int = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
    emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
    emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
    emit_sete_al(buf);
    if (negate) emit_xor_al_1(buf);
    emit_movzx_r64_al(buf, REG_RAX);
    size_t int_done = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, not_int);
    /* General case: mismatched kinds (kinds_differ also lands here), or
     * matching FLOAT/OBJECT kinds. Unlike the fast paths above, this
     * writes the full result (kind + value) directly into registers[dest]
     * via the trampoline's own `out` pointer, so it must jump past the
     * fast paths' own shared "store RAX as a Bool" tail below rather than
     * falling into it (which would clobber the just-written result with
     * whatever garbage the trampoline call left in RAX). diamond_jit_
     * equal_general takes 8 arguments; the last two (negate, out) go on
     * the stack per the SysV ABI's own overflow convention -- pushed in
     * reverse (out, then negate) so the last push (negate) ends up at the
     * lowest address, [rsp+0], matching that layout; the caller (this
     * generated code) reclaims the 16 bytes after the call returns. */
    patch_rel32_to_here(buf, kinds_differ);
    /* nil against an object is unequal; so is an Instance against nil when
     * its class defines no `==`, which this site's entries list (jit.h,
     * diamond_jit_equal_site_fill). RAX and RCX are the kinds. */
    size_t unequal[DIAMOND_JIT_CALL_WAYS + 1];
    size_t unequal_count = 0;
    size_t general[6];
    size_t general_count = 0;
    const uint_fast32_t site_id = atomic_fetch_add_explicit(&jit_next_call_site, 1,
        memory_order_relaxed);
    const bool cached = site_id < (uint_fast32_t)(INT32_MAX / sizeof(DiamondJitCallSite)) - 1;
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_NIL);
    size_t left_not_nil = emit_jcc_placeholder(buf, JCC_NE);
    emit_cmp_imm32_32(buf, REG_RCX, DIAMOND_VALUE_OBJECT);
    unequal[unequal_count++] = emit_jcc_placeholder(buf, JCC_E);
    general[general_count++] = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, left_not_nil);
    if (cached) {
        const int32_t site_at = (int32_t)(site_id * sizeof(DiamondJitCallSite));
        emit_cmp_imm32_32(buf, REG_RCX, DIAMOND_VALUE_NIL);
        general[general_count++] = emit_jcc_placeholder(buf, JCC_NE);
        emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(left, 0),
            DIAMOND_OBJECT_INSTANCE, general, &general_count);
        emit_load_r64(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondInstance, class));
        emit_mov_rr(buf, REG_RDX, JIT_VM);
        emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_site_count));
        emit_cmp_imm32_64(buf, REG_RCX, (uint32_t)site_id);
        size_t no_table = emit_jcc_placeholder(buf, JCC_BE);
        emit_load_r64(buf, REG_RDX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_sites));
        emit_lea(buf, REG_RDX, REG_RDX, site_at);
        for (int way = 0; way < DIAMOND_JIT_CALL_WAYS; way++) {
            emit_cmp_r64_mem(buf, REG_RAX, REG_RDX,
                (int32_t)((size_t)way * sizeof(DiamondJitCallEntry)));
            unequal[unequal_count++] = emit_jcc_placeholder(buf, JCC_E);
        }
        /* cmp dword [rdx + failed_fills], DIAMOND_JIT_CALL_FILL_TRIES */
        emit_u8(buf, 0x81); emit_u8(buf, 0xBA);
        emit_u32_le(buf, (uint32_t)offsetof(DiamondJitCallSite, failed_fills));
        emit_u32_le(buf, DIAMOND_JIT_CALL_FILL_TRIES);
        general[general_count++] = emit_jcc_placeholder(buf, JCC_AE);
        patch_rel32_to_here(buf, no_table);
        emit_mov_rr(buf, REG_RDI, JIT_VM);
        emit_mov_imm64(buf, REG_RSI, (uint64_t)site_id);
        emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(left, 0));
        emit_body_call(jc, (void *)(uintptr_t)diamond_jit_equal_site_fill);
    }
    for (size_t i = 0; i < general_count; i++) patch_rel32_to_here(buf, general[i]);
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);
    emit_mov_imm64(buf, REG_RAX, negate ? 1 : 0);
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(left, 0));
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(right, 0));
    emit_slow_frame_link(jc);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_equal_general);
    emit_slow_frame_unlink(jc);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    size_t general_done = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < unequal_count; i++) patch_rel32_to_here(buf, unequal[i]);
    emit_mov_imm64(buf, REG_RAX, negate ? 1 : 0);
    patch_rel32_to_here(buf, nil_done);
    patch_rel32_to_here(buf, bool_done);
    patch_rel32_to_here(buf, int_done);
    emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
    emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
    patch_rel32_to_here(buf, general_done);
}

/* SET_IVAR: field-cache/shape-transition bookkeeping and the GC write
 * barrier are too risky to hand-roll (see jit.h) -- calls
 * diamond_jit_set_ivar(vm, site, &registers[recv], field, &registers[source]).
 * `site` is this occurrence's own bytecode offset in the ORIGINAL
 * function, taken at compile time -- a stable, unique-per-occurrence
 * address, giving the same per-site cache-key identity an interpreted
 * execution would have gotten from &chunk->code[instruction_offset]. */
static void compile_set_ivar(JitCompiler *jc, size_t instruction_start, uint16_t recv,
                              uint16_t field, uint16_t source) {
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_imm64(buf, REG_RSI, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(recv, 0));
    emit_mov_imm64(buf, REG_RCX, field);
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(source, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_set_ivar);
    emit_bail_if_al_nonzero(jc);
}

/* GET_IVAR: mirrors compile_set_ivar's own trampoline call exactly, except
 * the last argument is an `out` pointer (diamond_jit_get_ivar writes the
 * full destination value -- kind and payload -- since an ivar can hold any
 * type, unlike the ALU opcodes' own direct-store fast paths which already
 * know the result kind at compile time). All 5 real arguments fit in the
 * SysV register slots (rdi/rsi/rdx/rcx/r8); no stack arguments needed. */
static void compile_get_ivar(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                              uint16_t recv, uint16_t field) {
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_imm64(buf, REG_RSI, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(recv, 0));
    emit_mov_imm64(buf, REG_RCX, field);
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_get_ivar);
    emit_bail_if_al_nonzero(jc);
}

/* GET_IVAR read inline: an Instance whose shape covers the field gives the
 * field, one whose shape stops short of it gives nil (instance_field_value).
 * Anything else, an error included, takes diamond_jit_get_ivar. */
static void compile_get_ivar_inline(JitCompiler *jc, size_t instruction_start, uint16_t dest,
        uint16_t recv, uint16_t field) {
    JitBuffer *buf = &jc->buf;
    size_t slow[4];
    size_t slow_count = 0;
    const int32_t field_at = (int32_t)(offsetof(DiamondInstance, fields) +
        (size_t)field * sizeof(DiamondValue));
    emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(recv, 0),
        DIAMOND_OBJECT_INSTANCE, slow, &slow_count);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondInstance, shape));
    emit_load_byte_zx(buf, REG_RAX, REG_RCX, (int32_t)offsetof(DiamondShape, field_count));
    emit_cmp_imm32_32(buf, REG_RAX, field);
    const size_t past_shape = emit_jcc_placeholder(buf, JCC_BE);
    emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RDX, field_at);
    const size_t read = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, past_shape);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondInstance, field_count));
    emit_cmp_imm32_64(buf, REG_RCX, field);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_BE);
    emit_mov_imm64(buf, REG_RCX, DIAMOND_VALUE_NIL);
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RCX);
    emit_u8(buf, 0x31); emit_u8(buf, 0xC9);                       /* xor ecx, ecx */
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RCX);
    const size_t niled = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    compile_get_ivar(jc, instruction_start, dest, recv, field);
    patch_rel32_to_here(buf, read);
    patch_rel32_to_here(buf, niled);
}

/* SET_IVAR written inline when nothing about it is unusual: an unfrozen
 * Instance with the field, moved to the next shape if this is the field's
 * first write (instance_materialize_field), and no write barrier needed
 * (a young object, or an old one already remembered). Anything else takes
 * diamond_jit_set_ivar, which repeats the store, so a value already written
 * before the barrier check is simply written again. */
static void compile_set_ivar_inline(JitCompiler *jc, size_t instruction_start, uint16_t recv,
        uint16_t field, uint16_t source) {
    JitBuffer *buf = &jc->buf;
    size_t slow[8];
    size_t slow_count = 0;
    const int32_t field_at = (int32_t)(offsetof(DiamondInstance, fields) +
        (size_t)field * sizeof(DiamondValue));
    emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(recv, 0),
        DIAMOND_OBJECT_INSTANCE, slow, &slow_count);
    emit_load_byte_zx(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondObject, frozen));
    emit_test_r8(buf, REG_RAX);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondInstance, field_count));
    emit_cmp_imm32_64(buf, REG_RCX, field);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_BE);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondInstance, shape));
    emit_load_byte_zx(buf, REG_RAX, REG_RCX, (int32_t)offsetof(DiamondShape, field_count));
    emit_cmp_imm32_32(buf, REG_RAX, field);
    const size_t shaped = emit_jcc_placeholder(buf, JCC_A);
    emit_load_r64(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondInstance, class));
    emit_lea(buf, REG_RAX, REG_RAX, (int32_t)(offsetof(DiamondClass, shapes) +
        ((size_t)field + 1) * sizeof(DiamondShape)));
    emit_store_r64(buf, REG_RDX, (int32_t)offsetof(DiamondInstance, shape), REG_RAX);
    emit_mov_rr(buf, REG_RCX, JIT_VM);
    /* add qword [rcx + shape_transitions], 1 */
    emit_u8(buf, 0x48); emit_u8(buf, 0x83); emit_u8(buf, 0x81);
    emit_u32_le(buf, (uint32_t)offsetof(DiamondVm, shape_transitions));
    emit_u8(buf, 1);
    patch_rel32_to_here(buf, shaped);
    emit_copy_value(buf, REG_RDX, field_at, JIT_REGISTERS_BASE, reg_disp(source, 0));
    emit_load_byte_zx(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondObject, old));
    emit_test_r8(buf, REG_RAX);
    const size_t young = emit_jcc_placeholder(buf, JCC_E);
    emit_load_byte_zx(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondObject, remembered));
    emit_test_r8(buf, REG_RAX);
    const size_t remembered = emit_jcc_placeholder(buf, JCC_NE);
    slow[slow_count++] = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    compile_set_ivar(jc, instruction_start, recv, field, source);
    patch_rel32_to_here(buf, young);
    patch_rel32_to_here(buf, remembered);
}

/* INDEX_GET (Phase 2e: full Hash/String/Array/Instance-overload support,
 * replacing Phase 2b's Hash-only version -- see diamond_jit_index_get's
 * own comment in jit.h for why). Can allocate (String/Array paths) and
 * can genuinely invoke arbitrary code (the Instance `[]` override), so
 * sets both jc->needs_frame and jc->has_called unconditionally. Calls
 * diamond_jit_index_get(vm, chunk, depth, site, &registers[recv],
 * &registers[index], &registers[dest]) -- 7 arguments, so the last
 * (`out`) goes on the stack per the SysV ABI's overflow convention; a
 * padding push keeps the total an even (16-byte-aligned) count. */
static void compile_index_get(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                               uint16_t recv, uint16_t index) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    /* An Array read at a non-negative Int index inside it is copied
     * inline; anything else (a negative index, a Range, out of bounds, any
     * other receiver) takes diamond_jit_index_get. */
    size_t slow[4];
    size_t slow_count = 0;
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(index, KIND_OFF));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_INT);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(recv, 0), DIAMOND_OBJECT_ARRAY,
        slow, &slow_count);
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(index, AS_OFF));
    /* Unsigned: a negative index compares above any count. */
    emit_cmp_r64_mem(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondArray, count));
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_AE);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondArray, values));
    emit_u8(buf, 0x48); emit_u8(buf, 0xC1); emit_u8(buf, 0xE0); emit_u8(buf, 0x04);  /* shl rax, 4 */
    emit_u8(buf, 0x48); emit_u8(buf, 0x01); emit_u8(buf, 0xC1);                      /* add rcx, rax */
    emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RCX, 0);
    const size_t done = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(recv, 0));
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(index, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_index_get);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    patch_rel32_to_here(buf, done);
}

/* INDEX_GET_STRING: compile_index_get with the key given as a string constant index (passed
 * as an immediate in R9) instead of a register. Same unconditional needs_frame/has_called
 * (the non-Hash path allocates and can run an Instance `[]` override), same 7-argument
 * stack-plus-padding shape. */
static void compile_index_get_string(JitCompiler *jc, size_t instruction_start, uint16_t dest,
                                     uint16_t recv, uint16_t string_index) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(recv, 0));
    emit_mov_imm64(buf, REG_R9, string_index);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_index_get_string);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
}

/* INDEX_SET (Phase 2e, new -- was entirely unsupported before). Full
 * Hash/String/Instance-overload/Array support, mirroring compile_index_
 * get's own treatment (jc->needs_frame and jc->has_called both
 * unconditional). Calls diamond_jit_index_set(vm, chunk, depth, site,
 * &registers[recv], &registers[index], &registers[source]) -- same
 * 7-argument, stack-plus-padding shape as compile_index_get. No
 * destination register: INDEX_SET never writes one (see diamond_jit_
 * index_set's own comment). */
static void compile_index_set(JitCompiler *jc, size_t instruction_start,
                               uint16_t recv, uint16_t index, uint16_t source) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    /* An Array write at a non-negative Int index inside it is stored inline
     * when the Array is young, unfrozen and has no element-type constraints:
     * a young owner needs no write barrier (gc_write_barrier_index returns at
     * once). Anything else -- an old Array, a negative index, a Range, out of
     * bounds, another receiver -- takes diamond_jit_index_set, errors and all.
     * This is the interpreter's INDEX_SET fast path, minus its inline barrier. */
    size_t slow[7];
    size_t slow_count = 0;
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(index, KIND_OFF));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_INT);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(recv, 0), DIAMOND_OBJECT_ARRAY,
        slow, &slow_count);
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondObject, old));
    emit_cmp_imm32_32(buf, REG_RCX, 0);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondObject, frozen));
    emit_cmp_imm32_32(buf, REG_RCX, 0);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondArray, constraint_count));
    emit_cmp_imm32_32(buf, REG_RCX, 0);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(index, AS_OFF));
    /* Unsigned: a negative index compares above any count. */
    emit_cmp_r64_mem(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondArray, count));
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_AE);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondArray, values));
    emit_u8(buf, 0x48); emit_u8(buf, 0xC1); emit_u8(buf, 0xE0); emit_u8(buf, 0x04);  /* shl rax, 4 */
    emit_u8(buf, 0x48); emit_u8(buf, 0x01); emit_u8(buf, 0xC1);                      /* add rcx, rax */
    emit_copy_value(buf, REG_RCX, 0, JIT_REGISTERS_BASE, reg_disp(source, 0));
    const size_t done = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(source, 0));
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(recv, 0));
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(index, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_index_set);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    patch_rel32_to_here(buf, done);
}

/* Only wholly unconstrained built-in unions can skip the matcher. A generic
 * member may attach collection constraints even when a later plain member
 * accepts the value, so preserve the complete matcher for such unions. */
static bool simple_guard_member(DiamondTypeMember member) {
    if (member.argument_set != DIAMOND_NO_TYPE_SET ||
        member.second_argument_set != DIAMOND_NO_TYPE_SET) return false;
    /* diamond_plain_type_match (vm.h) is the single definition of which
     * ids are plain; the kind switches below emit its tests as code. */
    return diamond_plain_type_match(DIAMOND_NIL, member.id) >= 0;
}

/* A user class with no type arguments: an Instance of exactly that class
 * passes it (a subclass instance is left to the full matcher). */
static bool class_guard_member(DiamondTypeMember member) {
    return member.argument_set == DIAMOND_NO_TYPE_SET &&
        member.second_argument_set == DIAMOND_NO_TYPE_SET &&
        member.id >= DIAMOND_TYPE_CLASS_BASE && member.id < DIAMOND_TYPE_CLASS_LIMIT;
}

static void compile_check_type(JitCompiler *jc, uint16_t source, uint16_t set_index) {
    JitBuffer *buf = &jc->buf;
    size_t accepted[DIAMOND_MAX_UNION_TYPES * 3];
    size_t accepted_count = 0;
    const DiamondTypeSet *set = set_index < jc->function->type_set_count
        ? &jc->function->type_sets[set_index] : nullptr;
    bool simple = set != nullptr && set->count != 0;
    bool classes = false;
    if (simple) {
        for (size_t i = 0; i < set->count; i++) {
            if (class_guard_member(set->members[i])) classes = true;
            else if (!simple_guard_member(set->members[i])) simple = false;
        }
    }
    if (simple) {
        emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(source, KIND_OFF));
        bool objects = false;
        for (size_t i = 0; i < set->count; i++) {
            uint32_t kind;
            switch (set->members[i].id) {
                case DIAMOND_TYPE_INT: kind = DIAMOND_VALUE_INT; objects = true; break;
                case DIAMOND_TYPE_FLOAT: kind = DIAMOND_VALUE_FLOAT; break;
                case DIAMOND_TYPE_BOOL: kind = DIAMOND_VALUE_BOOL; break;
                case DIAMOND_TYPE_NIL: kind = DIAMOND_VALUE_NIL; break;
                default: objects = true; continue;
            }
            emit_cmp_imm32_32(buf, REG_RAX, kind);
            accepted[accepted_count++] = emit_jcc_placeholder(buf, JCC_E);
        }
        if (objects) {
            emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_OBJECT);
            size_t not_object = emit_jcc_placeholder(buf, JCC_NE);
            emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(source, AS_OFF));
            emit_load_byte_zx(buf, REG_RAX, REG_RAX, offsetof(DiamondObject, kind));
            for (size_t i = 0; i < set->count; i++) {
                uint32_t kind;
                switch (set->members[i].id) {
                    case DIAMOND_TYPE_INT: kind = DIAMOND_OBJECT_BIGNUM; break;
                    case DIAMOND_TYPE_STRING: kind = DIAMOND_OBJECT_STRING; break;
                    case DIAMOND_TYPE_SYMBOL: kind = DIAMOND_OBJECT_SYMBOL; break;
                    case DIAMOND_TYPE_ARRAY: kind = DIAMOND_OBJECT_ARRAY; break;
                    case DIAMOND_TYPE_HASH: kind = DIAMOND_OBJECT_HASH; break;
                    default: continue;
                }
                emit_cmp_imm32_32(buf, REG_RAX, kind);
                accepted[accepted_count++] = emit_jcc_placeholder(buf, JCC_E);
            }
            if (classes) {
                /* RAX is the object kind: an Instance's class against
                 * chunk->classes[index] for each class member. */
                emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_OBJECT_INSTANCE);
                size_t not_instance = emit_jcc_placeholder(buf, JCC_NE);
                emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(source, AS_OFF));
                emit_load_r64(buf, REG_RAX, REG_RAX, (int32_t)offsetof(DiamondInstance, class));
                emit_load_r64(buf, REG_RCX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, class_count));
                emit_load_r64(buf, REG_RDX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, classes));
                for (size_t i = 0; i < set->count; i++) {
                    if (!class_guard_member(set->members[i])) continue;
                    const uint32_t index = (uint32_t)(set->members[i].id - DIAMOND_TYPE_CLASS_BASE);
                    emit_cmp_imm32_64(buf, REG_RCX, index);
                    size_t out_of_range = emit_jcc_placeholder(buf, JCC_BE);
                    emit_cmp_r64_mem(buf, REG_RAX, REG_RDX,
                        (int32_t)((size_t)index * sizeof(DiamondClass *)));
                    accepted[accepted_count++] = emit_jcc_placeholder(buf, JCC_E);
                    patch_rel32_to_here(buf, out_of_range);
                }
                patch_rel32_to_here(buf, not_instance);
            }
            patch_rel32_to_here(buf, not_object);
        }
    }
    /* Failure retains the existing status and propagation path. */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(source, 0));
    emit_mov_imm64(buf, REG_RCX, set_index);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_check_type);
    emit_bail_if_al_nonzero(jc);
    for (size_t i = 0; i < accepted_count; i++) patch_rel32_to_here(buf, accepted[i]);
}

/* IS_TYPE -- calls diamond_jit_is_type(chunk, &registers[source], type,
 * &registers[dest]). Same 4-argument, no-stack-overflow shape as compile_
 * check_type plus one more register-width arg -- see jit.h's own comment
 * for why this needs neither jc->needs_frame nor jc->has_called. */
static void compile_is_type(JitCompiler *jc, uint16_t dest, uint16_t source, uint16_t type) {
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_CHUNK);
    emit_lea(buf, REG_RSI, JIT_REGISTERS_BASE, reg_disp(source, 0));
    emit_mov_imm64(buf, REG_RDX, type);
    emit_lea(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_is_type);
    emit_bail_if_al_nonzero(jc);
}

/* STRING -- the first allocating opcode this JIT supports; sets
 * jc->needs_frame (both in the dry-run scan and the real pass alike, so
 * the real pass's own prologue already knows to reserve one by the time
 * it's emitted -- see diamond_jit_try_compile). Calls
 * diamond_jit_new_string(vm, chunk, string_index, &registers[dest]). */
static void compile_new_string(JitCompiler *jc, uint16_t dest, uint16_t string_index) {
    jc->needs_frame = true;
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_imm64(buf, REG_RDX, string_index);
    emit_lea(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_new_string);
    emit_bail_if_al_nonzero(jc);
}

/* HASH (Phase 2d) -- also allocates (allocate_hash calls maybe_collect
 * unconditionally, same as allocate_string), so this sets jc->needs_frame
 * too. `base`/`count` are passed straight through as immediates/the
 * registers-base pointer; the whole key/value-pair loop lives in
 * diamond_jit_new_hash itself (see jit.h), not generated code. Calls
 * diamond_jit_new_hash(vm, registers, base, count, &registers[dest]). */
static void compile_new_hash(JitCompiler *jc, uint16_t dest, uint16_t base, uint16_t count) {
    jc->needs_frame = true;
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RDX, base);
    emit_mov_imm64(buf, REG_RCX, count);
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_new_hash);
    emit_bail_if_al_nonzero(jc);
}

/* ARRAY uses the same rooted allocation path as HASH, including empty
 * literals used by default arguments. Preserve element registers across GC. */
static void compile_new_array(JitCompiler *jc, uint16_t dest, uint16_t base, uint16_t count) {
    if ((size_t)base + count > DIAMOND_REGISTER_COUNT) {
        jc->bailed = true;
        return;
    }
    jc->needs_frame = true;
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RDX, base);
    emit_mov_imm64(buf, REG_RCX, count);
    emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_new_array);
    emit_bail_if_al_nonzero(jc);
}

/* SUPER (Phase 2d) -- the first opcode that can run arbitrary interpreted
 * code with real side effects; sets both jc->needs_frame (invoke_
 * resolved_method_helper can allocate/trigger GC arbitrarily deep inside
 * whatever it calls) and jc->has_called (from this point on, every later
 * status-bearing bail site in this same compile -- including this call's
 * own -- targets the "propagate" stub, not "retry": see bailout_target
 * and DiamondJitFn's own comment in jit.h for why). `owner_index`/`name`
 * are compile-time-known immediates (the same operands DIAMOND_OP_SUPER's
 * own interpreter case decodes); `base`/`argc` likewise. Calls
 * diamond_jit_super_call(vm, chunk, owner_index, name, registers, base,
 * argc, depth, &registers[dest]) -- `depth` is this compiled function's
 * own incoming depth (JIT_DEPTH), passed on unchanged, exactly mirroring
 * the interpreter's own `depth` (not `depth+1`) argument to invoke_
 * resolved_method_helper. */
static void compile_super_call(JitCompiler *jc, uint16_t dest, uint16_t owner_index,
                                uint16_t name, uint16_t base, uint8_t argc) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    /* diamond_jit_super_call takes 9 arguments -- SysV passes the first 6
     * (vm, chunk, owner_index, name, registers, base) in RDI/RSI/RDX/RCX/
     * R8/R9, and the remaining 3 (argc, depth, out) on the stack, in that
     * order, at [rsp+0]/[rsp+8]/[rsp+16] at the moment of the call. Pushed
     * here in the reverse order (a dummy pad, then out, then depth, then
     * argc last) so the last-pushed value (argc) ends up at the lowest
     * address, [rsp+0], matching the ABI's own layout -- each push lands
     * at a strictly lower address than the one before it. The caller
     * (this generated code, not the callee) is responsible for reclaiming
     * this stack space after the call returns, per SysV's own caller-
     * cleanup convention for stack arguments. 4 pushes (32 bytes, the pad
     * included purely to keep the count a multiple of 2 -- 3 alone would
     * misalign the following call by 8 bytes) preserves this function's
     * own 16-byte call-alignment invariant. */
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);                 /* alignment pad, unused */
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);                 /* out -> [rsp+16] after the next 2 pushes */
    emit_push(buf, JIT_DEPTH);               /* depth -> [rsp+8] after the next push */
    emit_mov_imm64(buf, REG_RAX, argc);
    emit_push(buf, REG_RAX);                 /* argc -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_imm64(buf, REG_RDX, owner_index);
    emit_mov_imm64(buf, REG_RCX, name);
    emit_mov_rr(buf, REG_R8, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_R9, base);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_super_call);
    emit_add_rsp_imm32(buf, 32); /* reclaim the 4 pushed stack slots */
    emit_bail_if_al_nonzero(jc);
}

/* DIAMOND_OP_CALL -- calls diamond_jit_call_function, which performs the same
 * checks as the interpreter's case and runs the callee as native code when it
 * has been compiled. Sets needs_frame and has_called for the same reasons as
 * compile_super_call: the callee can allocate and collect, and has real side
 * effects that a retry from the start would repeat. 8 arguments: the first 6
 * (vm, chunk, registers, function_index, base, argc) in registers, and
 * (depth, out) on the stack, pushed in reverse so depth ends at [rsp+0]. */
/* xorps xmm0, xmm0 */

/* The body of a plain CALL of this function itself (recursion), made without
 * the C round trip through diamond_jit_call_self and jit_run_native: the
 * callee's register file is reserved on the native stack, zeroed and given the
 * arguments, and this function's own entry (buffer offset 0) is called
 * directly, with the caller's destination register as the callee's result
 * slot. It does at run time what diamond_jit_call_self checks: that the chunk
 * is this function's own and the depth is under vm->jit_depth_limit; when
 * either fails the trampoline below runs instead, which takes the general
 * path, raises the overflow, or (the limit is 0 until then) fills the limit
 * in on the first call. A status other than OK
 * goes to diamond_jit_self_call_finish, which retries, runs staged tail calls
 * or propagates exactly as jit_run_native's callers do. */
static void compile_self_call_native(JitCompiler *jc, uint16_t dest, uint16_t base,
                                     uint8_t argc, size_t slow_jumps[4]) {
    const DiamondFunction *fn = jc->function;
    JitBuffer *buf = &jc->buf;
    const size_t count = fn->register_count == 0 ? DIAMOND_JIT_MAX_REGISTERS
                                                 : fn->register_count;
    const uint32_t reserve = (uint32_t)(count * sizeof(DiamondValue));
    /* The chunk is the one this function was compiled against. */
    emit_load_r64(buf, REG_RAX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, code));
    emit_mov_imm64(buf, REG_R11, (uint64_t)(uintptr_t)fn->code);
    emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_R11);
    slow_jumps[0] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_r64(buf, REG_RAX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, function_count));
    emit_cmp_imm32_64(buf, REG_RAX, (uint32_t)jc->self_function_index);
    slow_jumps[1] = emit_jcc_placeholder(buf, JCC_BE);
    emit_load_r64(buf, REG_RAX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, functions));
    emit_load_r64(buf, REG_RAX, REG_RAX,
        (int32_t)(jc->self_function_index * sizeof(DiamondFunction *)));
    emit_mov_imm64(buf, REG_R11, (uint64_t)(uintptr_t)fn);
    emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_R11);
    slow_jumps[2] = emit_jcc_placeholder(buf, JCC_NE);
    /* depth + 1 < limit, as jit_call_core checks */
    emit_lea(buf, REG_RCX, JIT_DEPTH, 1);
    emit_mov_rr(buf, REG_RAX, JIT_VM);
    emit_load_r64(buf, REG_RAX, REG_RAX, (int32_t)offsetof(DiamondVm, jit_depth_limit));
    emit_alu_rr(buf, ALU_CMP, REG_RCX, REG_RAX);
    slow_jumps[3] = emit_jcc_placeholder(buf, JCC_AE);
    /* The callee's register file: arguments, then zeroes. */
    emit_sub_rsp_imm32(buf, reserve);
    for (uint8_t i = 0; i < argc; i++) {
        emit_movups_load(buf, 1, JIT_REGISTERS_BASE, reg_disp((uint16_t)(base + i), 0));
        emit_movups_store_rsp(buf, (int32_t)(i * sizeof(DiamondValue)), 1);
    }
    if (count - argc <= 32) {
        emit_zero_xmm0(buf);
        for (size_t i = argc; i < count; i++)
            emit_movups_store_rsp(buf, (int32_t)(i * sizeof(DiamondValue)), 0);
    } else {
        /* lea rdi, [rsp + argc*16]; mov ecx, qwords; xor eax, eax; rep stosq */
        emit_mov_rr(buf, REG_RDI, REG_RSP);
        emit_u8(buf, 0x48); emit_u8(buf, 0x81); emit_u8(buf, 0xC7); /* add rdi, imm32 */
        emit_u32_le(buf, (uint32_t)(argc * sizeof(DiamondValue)));
        emit_mov_imm64(buf, REG_RCX, (count - argc) * sizeof(DiamondValue) / 8);
        emit_u8(buf, 0x31); emit_u8(buf, 0xC0);                    /* xor eax, eax */
        emit_u8(buf, 0xF3); emit_u8(buf, 0x48); emit_u8(buf, 0xAB); /* rep stosq */
    }
    emit_store_instruction_offset(jc);
    /* (vm, registers, result, argument_count, chunk, depth), as jit_run_native
     * passes them. */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, REG_RSP);
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_mov_imm64(buf, REG_RCX, argc);
    emit_mov_rr(buf, REG_R8, JIT_CHUNK);
    emit_lea(buf, REG_R9, JIT_DEPTH, 1);
    emit_u8(buf, 0xE8);                                            /* call rel32 */
    emit_u32_le(buf, (uint32_t)(-(int32_t)(buf->length + 4)));     /* -> offset 0 */
    emit_add_rsp_imm32(buf, reserve);
    emit_test_r8(buf, REG_RAX);
    const size_t returned_ok = emit_jcc_placeholder(buf, JCC_E);
    /* (vm, chunk, arguments, argc, depth, out) in registers, status on the
     * stack above one pad slot. */
    emit_movzx_r64_al(buf, REG_RAX);
    emit_push(buf, REG_RAX);                 /* pad */
    emit_push(buf, REG_RAX);                 /* status -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(base, 0));
    emit_mov_imm64(buf, REG_RCX, argc);
    emit_mov_rr(buf, REG_R8, JIT_DEPTH);
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_self_call_finish);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    patch_rel32_to_here(buf, returned_ok);
}

/* The native call shared by calls to other compiled functions and methods
 * (compile_call_native, emit_invoke_native_begin, compile_new_native): what
 * jit_run_native does, without the C round trip. On entry R10 holds the
 * callee's entry, R11 the chunk it runs in and, when frame_bytes is 0, RAX the
 * size of its register file in bytes (a multiple of 16; otherwise frame_bytes
 * is that size). The register file goes on the native stack above a
 * NATIVE_CALL_HEADER-byte header: the entry's seventh argument (the closure,
 * nullptr as for any plain call), the header and file size to release
 * afterwards, the chunk (kept for the status path because the callee may
 * change the call site's cache entry it came from) and a 16-byte slot for a
 * result the caller discards. The arguments are registers[self_reg] when
 * self_reg >= 0, then registers[base..base+argc); the rest of the file is
 * zeroed. The callee writes its result straight into registers[dest], or into
 * the header's slot when discard_result (`initialize`, whose value `new`
 * ignores). Leaves the status in AL, the chunk in RDX and RSP where it was. */
enum { NATIVE_CALL_HEADER = 48, NATIVE_CALL_DISCARD = 32 };

static void emit_native_entry_call(JitCompiler *jc, uint16_t dest, int self_reg,
        uint16_t base, uint8_t argc, uint32_t frame_bytes, bool discard_result) {
    JitBuffer *buf = &jc->buf;
    const uint32_t count = (uint32_t)argc + (self_reg >= 0 ? 1u : 0u);
    emit_store_instruction_offset(jc);
    if (frame_bytes != 0) emit_mov_imm64(buf, REG_RAX, frame_bytes);
    /* The header and file are reserved by an amount only known at run time,
     * so RSP moves here without rsp_delta: nothing in between reads it. */
    emit_lea(buf, REG_RCX, REG_RAX, NATIVE_CALL_HEADER);
    emit_u8(buf, 0x48); emit_u8(buf, 0x29); emit_u8(buf, 0xCC);   /* sub rsp, rcx */
    emit_store_r64_sib(buf, REG_RSP, 8, REG_RCX);
    emit_store_r64_sib(buf, REG_RSP, 16, REG_R11);
    emit_u8(buf, 0x31); emit_u8(buf, 0xC9);                       /* xor ecx, ecx */
    emit_store_r64_sib(buf, REG_RSP, 0, REG_RCX);
    uint32_t slot = 0;
    if (self_reg >= 0) {
        emit_movups_load(buf, 1, JIT_REGISTERS_BASE, reg_disp((uint16_t)self_reg, 0));
        emit_movups_store_rsp(buf, (int32_t)(NATIVE_CALL_HEADER + 16 * slot++), 1);
    }
    for (uint8_t i = 0; i < argc; i++) {
        emit_movups_load(buf, 1, JIT_REGISTERS_BASE, reg_disp((uint16_t)(base + i), 0));
        emit_movups_store_rsp(buf, (int32_t)(NATIVE_CALL_HEADER + 16 * slot++), 1);
    }
    emit_zero_xmm0(buf);
    if (frame_bytes != 0 && frame_bytes / 16 - count <= 16) {
        for (uint32_t i = count; i < frame_bytes / 16; i++)
            emit_movups_store_rsp(buf, (int32_t)(NATIVE_CALL_HEADER + 16 * i), 0);
    } else {
        emit_lea_rsp(buf, REG_RDI, (int32_t)(NATIVE_CALL_HEADER + 16 * count));
        /* lea rcx, [rsp + rax + NATIVE_CALL_HEADER] */
        emit_u8(buf, 0x48); emit_u8(buf, 0x8D); emit_u8(buf, 0x4C);
        emit_u8(buf, 0x04); emit_u8(buf, NATIVE_CALL_HEADER);
        emit_u8(buf, 0x48); emit_u8(buf, 0x39); emit_u8(buf, 0xCF); /* cmp rdi, rcx */
        emit_u8(buf, 0x73); emit_u8(buf, 12);                       /* jae past the loop */
        emit_u8(buf, 0x0F); emit_u8(buf, 0x11); emit_u8(buf, 0x07); /* movups [rdi], xmm0 */
        emit_u8(buf, 0x48); emit_u8(buf, 0x83); emit_u8(buf, 0xC7); /* add rdi, 16 */
        emit_u8(buf, 0x10);
        emit_u8(buf, 0x48); emit_u8(buf, 0x39); emit_u8(buf, 0xCF); /* cmp rdi, rcx */
        emit_u8(buf, 0x72); emit_u8(buf, (uint8_t)-12);             /* jb the loop */
    }
    /* (vm, registers, result, argument_count, chunk, depth), as jit_run_native
     * passes them; the closure is at [rsp]. */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_lea_rsp(buf, REG_RSI, NATIVE_CALL_HEADER);
    if (discard_result) emit_lea_rsp(buf, REG_RDX, NATIVE_CALL_DISCARD);
    else emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_mov_imm64(buf, REG_RCX, count);
    emit_mov_rr(buf, REG_R8, REG_R11);
    emit_lea(buf, REG_R9, JIT_DEPTH, 1);
    emit_call_reg(buf, REG_R10);
    emit_load_r64_sib(buf, REG_RCX, REG_RSP, 8);
    emit_load_r64_sib(buf, REG_RDX, REG_RSP, 16);
    emit_u8(buf, 0x48); emit_u8(buf, 0x01); emit_u8(buf, 0xCC);   /* add rsp, rcx */
}

/* depth + 1 < vm->jit_depth_limit, as jit_call_core checks (the limit is 0
 * until a slow path fills it in, which sends the first call there). Clobbers
 * RCX and RDI; appends the failing jump to `slow`. */
static void emit_native_depth_check(JitCompiler *jc, size_t *slow, size_t *slow_count) {
    JitBuffer *buf = &jc->buf;
    emit_lea(buf, REG_RCX, JIT_DEPTH, 1);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_cmp_r64_mem(buf, REG_RCX, REG_RDI, (int32_t)offsetof(DiamondVm, jit_depth_limit));
    slow[(*slow_count)++] = emit_jcc_placeholder(buf, JCC_AE);
}

/* A plain CALL of another function the compiler knows from the function
 * table, made natively once that function has compiled code: the run-time
 * checks are what diamond_jit_call_function and plain_call_chunk_for would
 * find (the same function at that index, its cached call chunk valid for
 * this table) plus the code and the depth limit. Anything else takes
 * diamond_jit_call_function, which interprets the callee, compiles it when
 * it warms up and fills in the depth limit. A status other than OK goes to
 * diamond_jit_self_call_finish, as for the native self-call. */
static void compile_call_native(JitCompiler *jc, uint16_t dest, uint16_t function_index,
        uint16_t base, uint8_t argc, const DiamondFunction *target,
        size_t *slow, size_t *slow_count) {
    JitBuffer *buf = &jc->buf;
    emit_load_r64(buf, REG_RAX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, function_count));
    emit_cmp_imm32_64(buf, REG_RAX, function_index);
    slow[(*slow_count)++] = emit_jcc_placeholder(buf, JCC_BE);
    emit_load_r64(buf, REG_RAX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, functions));
    emit_load_r64(buf, REG_RCX, REG_RAX, (int32_t)(function_index * sizeof(DiamondFunction *)));
    emit_mov_imm64(buf, REG_R11, (uint64_t)(uintptr_t)target);
    emit_alu_rr(buf, ALU_CMP, REG_RCX, REG_R11);
    slow[(*slow_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_cmp_r64_mem(buf, REG_RAX, REG_R11,
        (int32_t)(offsetof(DiamondFunction, call_chunk) + offsetof(DiamondChunk, functions)));
    slow[(*slow_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_lea(buf, REG_RAX, REG_R11, (int32_t)offsetof(DiamondFunction, entry_check));
    emit_cmp_r64_mem(buf, REG_RAX, REG_R11,
        (int32_t)(offsetof(DiamondFunction, call_chunk) + offsetof(DiamondChunk, entry_check)));
    slow[(*slow_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_r64(buf, REG_R10, REG_R11, (int32_t)offsetof(DiamondFunction, jit_code));
    emit_test_r64(buf, REG_R10, REG_R10);
    slow[(*slow_count)++] = emit_jcc_placeholder(buf, JCC_E);
    emit_native_depth_check(jc, slow, slow_count);
    emit_lea(buf, REG_R11, REG_R11, (int32_t)offsetof(DiamondFunction, call_chunk));
    emit_native_entry_call(jc, dest, -1, base, argc,
        (uint32_t)(target->register_count * sizeof(DiamondValue)), false);
    emit_test_r8(buf, REG_RAX);
    const size_t returned_ok = emit_jcc_placeholder(buf, JCC_E);
    emit_movzx_r64_al(buf, REG_RAX);
    emit_push(buf, REG_RAX);                 /* pad */
    emit_push(buf, REG_RAX);                 /* status -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, REG_RDX);
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(base, 0));
    emit_mov_imm64(buf, REG_RCX, argc);
    emit_mov_rr(buf, REG_R8, JIT_DEPTH);
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_self_call_finish);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    patch_rel32_to_here(buf, returned_ok);
}

static void compile_call_function(JitCompiler *jc, uint16_t dest,
                                  uint16_t function_index, uint16_t base, uint8_t argc) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    /* Recursion: the target is this function, so its arity and the
     * register bound are checked here once, not on every call. */
    const DiamondFunction *self = jc->function;
    /* A function that reads its closure gets it as the entry's seventh
     * argument, which the native self-call does not pass: its self-calls
     * keep the trampoline, which enters with no closure as run_chunk's CALL
     * does. A generic function keeps it too: its plain call to itself infers
     * fresh bindings from the new arguments (emit_generic_bind), where
     * the native self-call would carry the caller's chunk and bindings on.
     */
    const bool calls_self = function_index == jc->self_function_index && !jc->reads_closure &&
        self->type_variable_count == 0 &&
        self->register_count <= DIAMOND_JIT_MAX_REGISTERS &&
        argc >= self->required_arity && argc <= self->arity && !self->has_variadic &&
        (size_t)base + argc <= DIAMOND_REGISTER_COUNT;
    size_t slow_jumps[4] = {0, 0, 0, 0};
    size_t done = 0;
    /* Another function in the table, called natively once it compiles,
     * when nothing about it needs the C path's handling: a rest parameter,
     * arguments its registers cannot hold, a heap register file. */
    const DiamondFunction *target = !calls_self && jc->functions != nullptr &&
        function_index < jc->function_count ? jc->functions[function_index] : nullptr;
    const bool calls_other = target != nullptr && target != self &&
        target->register_count != 0 && target->register_count <= DIAMOND_JIT_MAX_REGISTERS &&
        target->register_count >= argc &&
        argc >= target->required_arity && argc <= target->arity && !target->has_variadic &&
        (size_t)base + argc <= DIAMOND_REGISTER_COUNT;
    size_t other_slow[8];
    size_t other_slow_count = 0;
    if (calls_self) {
        compile_self_call_native(jc, dest, base, argc, slow_jumps);
        done = emit_jmp_placeholder(buf);
        for (size_t i = 0; i < 4; i++) patch_rel32_to_here(buf, slow_jumps[i]);
    } else if (calls_other) {
        compile_call_native(jc, dest, function_index, base, argc, target,
            other_slow, &other_slow_count);
        done = emit_jmp_placeholder(buf);
        for (size_t i = 0; i < other_slow_count; i++) patch_rel32_to_here(buf, other_slow[i]);
    }
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);                 /* out -> [rsp+8] after the next push */
    emit_push(buf, JIT_DEPTH);               /* depth -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX, function_index);
    emit_mov_imm64(buf, REG_R8, base);
    emit_mov_imm64(buf, REG_R9, argc);
    emit_body_call(jc, calls_self ? (void *)(uintptr_t)diamond_jit_call_self
                                  : (void *)(uintptr_t)diamond_jit_call_function);
    emit_add_rsp_imm32(buf, 16); /* reclaim the 2 pushed stack slots */
    emit_bail_if_al_nonzero(jc);
    if (calls_self || calls_other) patch_rel32_to_here(buf, done);
}

/* A tail call to a function other than this one: the trampoline stages the
 * callee and arguments and this function exits with DIAMOND_JIT_TAIL, so
 * jit_call_or_interpret continues with the callee at the same depth and a
 * cycle of such calls costs no native stack. Where the interpreter would
 * make an ordinary call instead (variadic callee, arguments its registers
 * cannot hold) the trampoline performs it, and the result is returned here
 * exactly as a CALL followed by RETURN would. */
static void compile_tail_call_other(JitCompiler *jc, uint16_t dest,
        uint16_t function_index, uint16_t base, uint8_t argc) {
    jc->needs_frame = true;
    jc->has_called = true;
    emit_interrupt_poll(jc);
    JitBuffer *buf = &jc->buf;
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);
    emit_push(buf, JIT_DEPTH);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX, function_index);
    emit_mov_imm64(buf, REG_R8, base);
    emit_mov_imm64(buf, REG_R9, argc);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_tail_call_function);
    emit_add_rsp_imm32(buf, 16);
    /* movzx eax, al; cmp eax, DIAMOND_JIT_TAIL; jne -> ordinary status */
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, 0xC0);
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_JIT_TAIL);
    size_t not_staged = emit_jcc_placeholder(buf, JCC_NE);
    emit_epilogue(jc);
    emit_mov_al_imm8(buf, DIAMOND_JIT_TAIL);
    emit_ret(buf);
    patch_rel32_to_here(buf, not_staged);
    emit_bail_if_al_nonzero(jc);
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(dest, 8));
    emit_store_r64(buf, JIT_RESULT_PTR, 0, REG_RAX);
    emit_store_r64(buf, JIT_RESULT_PTR, 8, REG_RCX);
    emit_epilogue(jc);
    emit_mov_al_imm8(buf, DIAMOND_VM_OK);
    emit_ret(buf);
}

/* self.method()/typed-param.method()-shaped DIAMOND_OP_INVOKE/INVOKE_MONO --
 * calls diamond_jit_invoke_instance with type_argument_count/type_arguments
 * always 0/nullptr and a compile-time-known `recv` register index (the
 * trampoline itself does registers[recv] internally, exactly like the
 * interpreter's own case -- recv is a register *number*, not a pointer, so
 * passing it through as an immediate needs no register-pointer arithmetic).
 * This function's own callers (compile_body's DIAMOND_OP_INVOKE case) only
 * ever reach here once they've independently proven registers[recv] is
 * always an Instance for the entire function body -- either recv==0 and
 * jc->function->owner_class proves register 0 is always the receiver (Phase
 * 7: a real class index, not the DIAMOND_OWNER_NONE/MODULE/CAPTURED_SELF
 * plain-function/module-method/closure sentinels -- every function with a real
 * owner_class is only ever entered via invoke_resolved_method_helper, which
 * unconditionally sets args[0] to the receiver), or recv names a declared
 * parameter whose static type is a single concrete class and which the
 * function body never reassigns (Phase 9: see parameter_is_single_class/
 * parameter_never_reassigned below). DIAMOND_OP_INVOKE_TYPED never comes
 * here (it compiles to diamond_jit_invoke_typed, which passes its type
 * arguments), so type_argument_count/type_arguments are always the "no type
 * arguments" constants.
 * Sets both jc->needs_frame (invoke_resolved_method_helper/method_missing_
 * helper/tap's own closure call can all allocate/trigger GC arbitrarily
 * deep inside whatever they call) and jc->has_called (a real method call
 * with real side effects), mirroring compile_super_call's own identical
 * justification exactly. `site` is computed the same compile-time-known
 * way compile_get_ivar's own does: jc->function->code + instruction_start. */
static void compile_invoke_dispatch(JitCompiler *jc, size_t instruction_start,
                                 uint16_t dest, uint16_t recv, uint16_t name, uint16_t base,
                                 uint8_t argc, bool monomorphic) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    /* diamond_jit_invoke_instance takes 13 arguments -- SysV passes the
     * first 6 (vm, chunk, site, instruction, registers, recv) in RDI/RSI/
     * RDX/RCX/R8/R9, and the remaining 7 (name, base, argc, type_argument_
     * count, type_arguments, depth, out) on the stack, in that order, at
     * [rsp+0]/[rsp+8]/.../[rsp+48] at the moment of the call -- mirrors
     * compile_super_call's own push-in-reverse-order convention exactly,
     * just with 4 more stack slots (7 real args here vs. 3 there). 8
     * pushes (64 bytes, 1 pad) keeps this function's own 16-byte call-
     * alignment invariant, same reasoning as compile_super_call's own
     * comment. */
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);                 /* alignment pad, unused */
    emit_lea(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_push(buf, REG_RAX);                 /* out -> [rsp+48] */
    emit_push(buf, JIT_DEPTH);               /* depth -> [rsp+40] */
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);                 /* type_arguments (nullptr) -> [rsp+32] */
    emit_mov_imm64(buf, REG_RAX, 0);
    emit_push(buf, REG_RAX);                 /* type_argument_count (0) -> [rsp+24] */
    emit_mov_imm64(buf, REG_RAX, argc);
    emit_push(buf, REG_RAX);                 /* argc -> [rsp+16] */
    emit_mov_imm64(buf, REG_RAX, base);
    emit_push(buf, REG_RAX);                 /* base -> [rsp+8] */
    emit_mov_imm64(buf, REG_RAX, name);
    emit_push(buf, REG_RAX);                 /* name -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_imm64(buf, REG_RDX,
        (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_mov_imm64(buf, REG_RCX,
        monomorphic ? DIAMOND_OP_INVOKE_MONO : DIAMOND_OP_INVOKE);
    emit_mov_rr(buf, REG_R8, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_R9, recv);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_invoke_instance);
    emit_add_rsp_imm32(buf, 64); /* reclaim the 8 pushed stack slots */
    emit_bail_if_al_nonzero(jc);
}

/* DIAMOND_OP_INVOKE/INVOKE_MONO on a receiver none of compile_invoke's proofs
 * cover: calls diamond_jit_invoke_any, the interpreter's own dispatch, so any
 * receiver and method name compiles instead of rejecting the whole function.
 * It reads the operands from the instruction at run time, opcode byte
 * included, so it sees the INVOKE/INVOKE_MONO quickening the interpreter
 * applies to the site (the two share a layout). */
static void compile_invoke_helper(JitCompiler *jc, size_t instruction_start, void *helper) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX,
        (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_mov_rr(buf, REG_R8, JIT_DEPTH);
    emit_body_call(jc, helper);
    emit_bail_if_al_nonzero(jc);
}

static void compile_invoke_any(JitCompiler *jc, size_t instruction_start) {
    compile_invoke_helper(jc, instruction_start, (void *)(uintptr_t)diamond_jit_invoke_any);
}

static void compile_native_call(JitCompiler *jc,uint16_t dest,uint16_t recv,
        uint16_t base,DiamondJitNativeReadOp operation) {
    if(operation==DIAMOND_JIT_NATIVE_STRING_SLICE) {
        jc->needs_frame=true;
        jc->has_called=true;
    }
    JitBuffer *buf=&jc->buf;
    emit_mov_rr(buf,REG_RDI,JIT_VM);
    emit_lea(buf,REG_RSI,JIT_REGISTERS_BASE,reg_disp(recv,0));
    emit_lea(buf,REG_RDX,JIT_REGISTERS_BASE,reg_disp(base,0));
    emit_mov_imm64(buf,REG_RCX,(uint64_t)operation);
    emit_lea(buf,REG_R8,JIT_REGISTERS_BASE,reg_disp(dest,0));
    emit_body_call(jc,(void *)(uintptr_t)diamond_jit_native_call);
    emit_bail_if_al_nonzero(jc);
}

/* Phase 10: DIAMOND_OP_NEW -- calls diamond_jit_new_instance with every
 * operand a compile-time-known immediate (dest/class_index/base/argc all
 * come straight from the decoded bytecode, same as compile_super_call's
 * own owner_index/name/base/argc). Unconditional: this opcode has no
 * receiver-kind gate to check (unlike compile_invoke_dispatch) -- the
 * trampoline itself validates class_index<chunk->class_count and returns
 * DIAMOND_VM_INVALID_BYTECODE if not, exactly like the interpreter's own
 * case did before extraction. Sets both jc->needs_frame (allocate_
 * instance's own maybe_collect can trigger GC) and jc->has_called (a
 * user-defined `initialize` can run arbitrary code), mirroring every
 * other call-capable trampoline here. */
static void compile_new(JitCompiler *jc, uint16_t dest, uint16_t class_index,
                         uint16_t base, uint8_t argc) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    /* diamond_jit_new_instance takes 8 arguments -- SysV passes the first
     * 6 (vm, chunk, registers, dest, class_index, base) in RDI/RSI/RDX/
     * RCX/R8/R9, and the remaining 2 (argc, depth) on the stack, at
     * [rsp+0]/[rsp+8] at the moment of the call. 2 pushes (16 bytes)
     * already keeps this function's own 16-byte call-alignment invariant
     * with no pad needed (unlike compile_super_call's own 3-stack-arg
     * case, or compile_invoke_dispatch's 7-stack-arg case) -- pushed in
     * reverse order (depth, then argc last) so the last-pushed value
     * (argc) ends up at the lowest address, [rsp+0], matching the ABI's
     * own layout, same convention as every other trampoline call here. */
    emit_push(buf, JIT_DEPTH);               /* depth -> [rsp+8] after the next push */
    emit_mov_imm64(buf, REG_RAX, argc);
    emit_push(buf, REG_RAX);                 /* argc -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX, dest);
    emit_mov_imm64(buf, REG_R8, class_index);
    emit_mov_imm64(buf, REG_R9, base);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_new_instance);
    emit_add_rsp_imm32(buf, 16); /* reclaim the 2 pushed stack slots */
    emit_bail_if_al_nonzero(jc);
}

/* Phase 9: true iff `recv_register` is a genuine declared-parameter home
 * register whose static type is a single concrete class (not a union, and
 * not DIAMOND_NO_TYPE_SET). compile_definition (src/compiler.c) always
 * allocates register 0 for self first for any owner_class!=DIAMOND_OWNER_NONE (real
 * class, module-method UINT8_MAX-1, and closure-capturing-self UINT8_MAX-2
 * alike -- see DiamondFunction.owner_class's own comment, src/vm.h), then
 * allocates the declared parameters' own registers in source order right
 * after -- so parameter i's home register is always self_offset+i, and
 * fn->parameter_type_sets[] is indexed from 0 regardless (self slots are
 * "deliberately excluded" from it, same comment). Converting recv_register
 * back to a parameter_type_sets index needs that offset; getting it wrong
 * silently mis-attributes a different parameter's type (or a plain local's
 * register) -- confirmed against the real allocation order at
 * src/compiler.c:13949-13953 before writing this. No DiamondChunk/class-
 * table access needed: the encoded id range alone already proves "this is
 * definitely some Instance" (the compiler only ever writes a real,
 * already-resolved class's own id here -- same encoding lsp/receiver.c's
 * own decode_class_type trusts), and diamond_jit_invoke_instance dispatches
 * by the *runtime* instance's own class already, so which compile-time
 * class it is doesn't matter here. A union type_set (set->count!=1) is
 * rejected unconditionally, including a nilable `Type?` (represented as
 * Type|Nil) -- correct, since a nil receiver must still raise through the
 * interpreter, not hit this Instance-only trampoline. */
static bool parameter_is_single_class(const DiamondFunction *fn, uint16_t recv_register) {
    const uint16_t self_offset = fn->owner_class == DIAMOND_OWNER_NONE ? 0 : 1;
    if (recv_register < self_offset) return false;
    const uint16_t param_index = (uint16_t)(recv_register - self_offset);
    if (param_index >= DIAMOND_MAX_DECLARED_PARAMETERS) return false;
    const uint16_t set_index = fn->parameter_type_sets[param_index];
    if (set_index == DIAMOND_NO_TYPE_SET || set_index >= fn->type_set_count) return false;
    const DiamondTypeSet *set = &fn->type_sets[set_index];
    if (set->count != 1) return false;
    const uint16_t id = set->members[0].id;
    /* Below DIAMOND_TYPE_NATIVE: a native object type (Time, Channel, ...)
     * shares the byte space above the class range but is not an Instance. */
    return id >= DIAMOND_TYPE_CLASS_BASE && id < DIAMOND_TYPE_NATIVE;
}

/* Phase 9: true iff `target_register` is never written by any instruction
 * in fn's whole body -- proves a parameter register that passed
 * parameter_is_single_class() above still holds its original argument value
 * (and therefore its declared type) at every point in the function,
 * including at a specific DIAMOND_OP_INVOKE site reached later than this
 * scan cares about (deliberately position-insensitive: simpler and
 * strictly safe-or-equal versus a flow-sensitive "not yet reassigned by
 * this point" check, at the cost of rejecting a register that's reassigned
 * only *after* the call site in question -- an intentional, documented
 * trade, see this phase's own design doc section).
 * Mirrors compile_body's own switch (immediately below) case-for-case
 * rather than any generic "decode this opcode's operands" utility, since
 * none exists -- deliberately fails safe: any opcode not explicitly
 * recognized here as "writes no register" or "writes register named by its
 * own known dest field" is treated as writing `target_register`, so an
 * unrecognized/future opcode can only ever cause a missed optimization,
 * never a wrong one. compile_body already bails the whole function on any
 * opcode outside this same set before this scan is ever reached, so the
 * two switches necessarily cover the same closed opcode set -- kept in
 * sync by hand (checked case-by-case against compile_body's own switch
 * while writing this), not by a shared table, since compile_body's own
 * per-case decode shapes aren't expressed as data anywhere to share from. */
static bool opcode_writes_no_register(DiamondOpCode opcode) {
    switch (opcode) {
        case DIAMOND_OP_POP_RESCUE:
        case DIAMOND_OP_PUSH_ENSURE:
        case DIAMOND_OP_RUN_ENSURE:
        case DIAMOND_OP_END_ENSURE:
        case DIAMOND_OP_UNWIND_JUMP:
        case DIAMOND_OP_RAISE:
        case DIAMOND_OP_SET_IVAR:
        case DIAMOND_OP_CHECK_TYPE:
        case DIAMOND_OP_INDEX_SET:
        case DIAMOND_OP_SET_CELL:
        case DIAMOND_OP_SET_CAPTURE:
        case DIAMOND_OP_JUMP:
        case DIAMOND_OP_JUMP_IF_TRUE:
        case DIAMOND_OP_JUMP_IF_FALSE:
        case DIAMOND_OP_RETURN:
        case DIAMOND_OP_SET_CVAR:
        case DIAMOND_OP_CHECK_DESTRUCTURE_COUNT:
        case DIAMOND_OP_CHECK_SELF_RETURN:
            return true;
        default:
            return false;
    }
}

/* Opcodes compiled as a call to diamond_jit_outlined (src/vm_calls.c, whose
 * switch must list the same ones): the interpreter's own out-of-line body
 * runs them, so they behave exactly as interpreted. Each needs an entry in
 * decode_instruction_operands too (its operands and the register it
 * writes), which compile_outlined_instruction uses to step over it. */
/* The builtins compiled code runs through diamond_jit_outlined (each the
 * vm_outlined.c body run_chunk calls too). Their operand layouts are those
 * bodies' READ_SHORT/READ_BYTE sequences: the first u16 is the register the
 * instruction writes (`*writes_dest`, false for the four that write none),
 * then `*shorts` more u16 operands and `*bytes` trailing bytes. Each writes
 * only that register; the rest are read. False for any other opcode. */
static bool builtin_operand_layout(DiamondOpCode opcode, bool *writes_dest, uint8_t *shorts,
        uint8_t *bytes) {
    *writes_dest = true;
    *bytes = 0;
    switch (opcode) {
        case DIAMOND_OP_ARGV: case DIAMOND_OP_ENV: case DIAMOND_OP_GETS: case DIAMOND_OP_UDP_OPEN:
        case DIAMOND_OP_PROGRAM_BUILDER_NEW: case DIAMOND_OP_TIME_MONOTONIC:
            *shorts = 0; return true;
        case DIAMOND_OP_TIME_NOW:
            *shorts = 0; *bytes = 1; return true;
        case DIAMOND_OP_BASE64_DECODE: case DIAMOND_OP_BASE64_ENCODE: case DIAMOND_OP_CASE_HASH_SHAPE:
        case DIAMOND_OP_CHR: case DIAMOND_OP_DIGEST_SHA1: case DIAMOND_OP_DIGEST_SHA256:
        case DIAMOND_OP_FIBER_NEW: case DIAMOND_OP_FILE_DELETE: case DIAMOND_OP_GZIP_COMPRESS:
        case DIAMOND_OP_JSON_STRINGIFY: case DIAMOND_OP_PROCESS_RUN: case DIAMOND_OP_PROCESS_SPAWN:
        case DIAMOND_OP_SECURE_RANDOM_BYTES: case DIAMOND_OP_SECURE_RANDOM_HEX:
        case DIAMOND_OP_TENSOR_FROM_ARRAY: case DIAMOND_OP_TIME_AT: case DIAMOND_OP_TIME_PARSE:
        case DIAMOND_OP_TO_FLOAT: case DIAMOND_OP_TO_INT: case DIAMOND_OP_TO_SYMBOL:
        case DIAMOND_OP_YIELD: case DIAMOND_OP_CHANNEL_NEW: case DIAMOND_OP_SUPERVISOR_NEW:
        case DIAMOND_OP_DIR_ENTRIES: case DIAMOND_OP_POSTGRES_OPEN:
            *shorts = 1; return true;
        case DIAMOND_OP_FILE_JOIN:      /* dest, base, argc: reads base..base+argc-1 */
        case DIAMOND_OP_TIME_BUILD:     /* dest, base, mode */
            *shorts = 1; *bytes = 1; return true;
        case DIAMOND_OP_ARRAY_MIDDLE: case DIAMOND_OP_ARRAY_REST: case DIAMOND_OP_ARRAY_SUFFIX:
        case DIAMOND_OP_BCRYPT_HASH: case DIAMOND_OP_BCRYPT_VERIFY: case DIAMOND_OP_BITWISE_AND:
        case DIAMOND_OP_BITWISE_OR: case DIAMOND_OP_BITWISE_XOR: case DIAMOND_OP_CASE_ARRAY_SHAPE:
        case DIAMOND_OP_CASE_HASH_HAS: case DIAMOND_OP_CASE_MATCH: case DIAMOND_OP_CIPHER_DECRYPT:
        case DIAMOND_OP_CIPHER_ENCRYPT: case DIAMOND_OP_COMPARE: case DIAMOND_OP_FILE_OPEN:
        case DIAMOND_OP_GET_IVAR_NAME: case DIAMOND_OP_GZIP_DECOMPRESS: case DIAMOND_OP_HASH_REST:
        case DIAMOND_OP_HMAC_SHA1: case DIAMOND_OP_HMAC_SHA256: case DIAMOND_OP_JSON_STRINGIFY_OPTIONS:
        case DIAMOND_OP_PROCESS_RUN_OPTIONS: case DIAMOND_OP_PROCESS_SPAWN_OPTIONS:
        case DIAMOND_OP_REGEXP_NEW: case DIAMOND_OP_SHIFT_LEFT: case DIAMOND_OP_SHIFT_RIGHT:
        case DIAMOND_OP_SIGNAL_TRAP: case DIAMOND_OP_SQLITE3_OPEN: case DIAMOND_OP_TCP_CONNECT_NONBLOCK:
        case DIAMOND_OP_TENSOR_ZEROS: case DIAMOND_OP_UDP_BIND: case DIAMOND_OP_CHANNEL_SELECT:
        case DIAMOND_OP_TCP_LISTEN: case DIAMOND_OP_TCP_LISTEN_NONBLOCK:
            *shorts = 2; return true;
        case DIAMOND_OP_THREAD_NEW:     /* dest, callable, base, argc */
        case DIAMOND_OP_SEMVER: case DIAMOND_OP_FILE_PATH:   /* dest, two operands, selector */
            *shorts = 2; *bytes = 1; return true;
        case DIAMOND_OP_DNS_RESOLVE: case DIAMOND_OP_HMAC_VERIFY: case DIAMOND_OP_TCP_CONNECT:
        case DIAMOND_OP_TENSOR_RANDOM: case DIAMOND_OP_CHANNEL_SELECT_SEND:
        case DIAMOND_OP_DEFINE_METHOD: case DIAMOND_OP_REDEFINE_METHOD:
        case DIAMOND_OP_TLS_CONNECT: case DIAMOND_OP_TLS_START_HANDSHAKE: case DIAMOND_OP_IO_POLL:
            *shorts = 3; return true;
        case DIAMOND_OP_TLS_LISTEN:
            *shorts = 4; return true;
        case DIAMOND_OP_COMPILE_METHOD:
        case DIAMOND_OP_MYSQL_OPEN:
            *shorts = 5; return true;
        case DIAMOND_OP_EXIT:
            *writes_dest = false; *shorts = 0; return true;
        case DIAMOND_OP_CHECK_HASH_KEY: case DIAMOND_OP_SET_NAMESPACE_CONSTANT:
            *writes_dest = false; *shorts = 1; return true;
        case DIAMOND_OP_SET_IVAR_NAME:  /* receiver, name, value */
            *writes_dest = false; *shorts = 2; return true;
        default:
            return false;
    }
}

static bool opcode_runs_outlined(DiamondOpCode opcode) {
    bool writes_dest = false; uint8_t shorts = 0, bytes = 0;
    if (builtin_operand_layout(opcode, &writes_dest, &shorts, &bytes)) return true;
    switch (opcode) {
        case DIAMOND_OP_PRINT:
        case DIAMOND_OP_SYMBOL:
        case DIAMOND_OP_CLASS_NAME:
        case DIAMOND_OP_NEGATE:
        case DIAMOND_OP_CHECK_DESTRUCTURE_COUNT:
        case DIAMOND_OP_INVOKE_SELF_METHOD:
        case DIAMOND_OP_GET_CVAR:
        case DIAMOND_OP_SET_CVAR:
        case DIAMOND_OP_CHECK_SELF_RETURN:
        case DIAMOND_OP_CALL_TYPED:
        case DIAMOND_OP_CALL_SPREAD: case DIAMOND_OP_CALL_TYPED_SPREAD:
        case DIAMOND_OP_CALL_KEYWORD_SPREAD: case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD:
        case DIAMOND_OP_CALL_SINGLETON_KEYWORDS: case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS:
        case DIAMOND_OP_CALL_SINGLETON_SPREAD: case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD:
        case DIAMOND_OP_NEW_SPREAD: case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_NEW_TYPED:
        case DIAMOND_OP_INVOKE_KEYWORDS: case DIAMOND_OP_INVOKE_TYPED_KEYWORDS:
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD:
        case DIAMOND_OP_CALL_CLOSURE_KEYWORDS: case DIAMOND_OP_CALL_CLOSURE_SPREAD:
        case DIAMOND_OP_BUILD_SPREAD_ARGS:
            return true;
        default:
            return false;
    }
}

static bool opcode_dest_is_first_u16(DiamondOpCode opcode) {
    switch (opcode) {
        case DIAMOND_OP_COLLECT_VARIADIC:
        case DIAMOND_OP_CALL_TYPED:
        case DIAMOND_OP_CALL_SPREAD: case DIAMOND_OP_CALL_TYPED_SPREAD:
        case DIAMOND_OP_CALL_KEYWORD_SPREAD: case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD:
        case DIAMOND_OP_CALL_SINGLETON_KEYWORDS: case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS:
        case DIAMOND_OP_CALL_SINGLETON_SPREAD: case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD:
        case DIAMOND_OP_NEW_SPREAD: case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_NEW_TYPED:
        case DIAMOND_OP_INVOKE_KEYWORDS: case DIAMOND_OP_INVOKE_TYPED_KEYWORDS:
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD:
        case DIAMOND_OP_CALL_CLOSURE_KEYWORDS: case DIAMOND_OP_CALL_CLOSURE_SPREAD:
        case DIAMOND_OP_BUILD_SPREAD_ARGS:
        case DIAMOND_OP_NOT:
        case DIAMOND_OP_LOAD_CLASS:
        case DIAMOND_OP_PRINT:
        case DIAMOND_OP_SYMBOL:
        case DIAMOND_OP_CLASS_NAME:
        case DIAMOND_OP_NEGATE:
        case DIAMOND_OP_INVOKE_SELF_METHOD:
        case DIAMOND_OP_GET_CVAR:
        case DIAMOND_OP_ADD_INT_K: case DIAMOND_OP_SUBTRACT_INT_K: case DIAMOND_OP_MULTIPLY_INT_K:
        case DIAMOND_OP_MODULO_INT_K: case DIAMOND_OP_LESS_INT_K: case DIAMOND_OP_LESS_EQUAL_INT_K:
        case DIAMOND_OP_GREATER_INT_K: case DIAMOND_OP_GREATER_EQUAL_INT_K:
        case DIAMOND_OP_EQUAL_INT_K: case DIAMOND_OP_NOT_EQUAL_INT_K:
        case DIAMOND_OP_NIL:
        case DIAMOND_OP_BOOL:
        case DIAMOND_OP_MOVE:
        case DIAMOND_OP_CONSTANT:
        case DIAMOND_OP_ADD_INT:
        case DIAMOND_OP_SUBTRACT_INT:
        case DIAMOND_OP_MULTIPLY_INT:
        case DIAMOND_OP_DIVIDE_INT:
        case DIAMOND_OP_LESS_INT:
        case DIAMOND_OP_ADD:
        case DIAMOND_OP_SUBTRACT:
        case DIAMOND_OP_MULTIPLY:
        case DIAMOND_OP_DIVIDE:
        case DIAMOND_OP_MODULO:
        case DIAMOND_OP_LESS:
        case DIAMOND_OP_LESS_EQUAL:
        case DIAMOND_OP_LESS_EQUAL_INT:
        case DIAMOND_OP_GREATER:
        case DIAMOND_OP_GREATER_INT:
        case DIAMOND_OP_GREATER_EQUAL:
        case DIAMOND_OP_GREATER_EQUAL_INT:
        case DIAMOND_OP_EQUAL:
        case DIAMOND_OP_NOT_EQUAL:
        case DIAMOND_OP_EQUAL_INT:
        case DIAMOND_OP_NOT_EQUAL_INT:
        case DIAMOND_OP_GET_IVAR:
        case DIAMOND_OP_ARGUMENT_PROVIDED:
        case DIAMOND_OP_STRING:
        case DIAMOND_OP_INDEX_GET:
        case DIAMOND_OP_INDEX_GET_STRING:
        case DIAMOND_OP_ARRAY:
        case DIAMOND_OP_HASH:
        case DIAMOND_OP_SUPER:
        case DIAMOND_OP_INVOKE:
        case DIAMOND_OP_INVOKE_MONO:
        case DIAMOND_OP_INVOKE_TYPED:
        case DIAMOND_OP_NEW:
        case DIAMOND_OP_CALL:
        case DIAMOND_OP_TAIL_CALL:
        case DIAMOND_OP_MATH_UNARY:
        case DIAMOND_OP_MATH_BINARY:
        case DIAMOND_OP_IS_TYPE:
        case DIAMOND_OP_SAME_CLASS:
        case DIAMOND_OP_BOX_LOCAL:
        case DIAMOND_OP_GET_CELL:
        case DIAMOND_OP_GET_CAPTURE:
        case DIAMOND_OP_GET_CAPTURE_CELL:
        case DIAMOND_OP_GET_NAMESPACE_CONSTANT:
        case DIAMOND_OP_PUSH_RESCUE:
        case DIAMOND_OP_TO_STRING:
        case DIAMOND_OP_CALL_CLOSURE:
        case DIAMOND_OP_CLOSURE:
            return true;
        default:
            return false;
    }
}

/* Decodes the operands of one whitelisted instruction, whose opcode byte
 * `*pc` has just been read past, advancing `*pc` to the next opcode. Reuses
 * compile_body's exact per-case decode calls (no generic width table
 * exists). `*dest` is the register it writes, -1 for an instruction that
 * writes none; `*jump_target` is its bytecode target, -1 for a non-jump.
 * The _INT_K forms also write their scratch register, returned in
 * `*second_dest` (-1 for every other instruction).
 * Returns false for an opcode outside the whitelist or a truncated stream,
 * which every caller treats as "unknown, assume the worst". */
/* A typed call's trailer: a count byte (at most 8), then that many u16 type
 * sets. */
static bool skip_type_trailer(const DiamondFunction *fn, size_t *pc) {
    uint8_t count = 0;
    if (!decode_u8(fn, pc, &count) || count > 8 || *pc + 2 * (size_t)count > fn->code_count)
        return false;
    *pc += 2 * (size_t)count;
    return true;
}

/* The keyword trailer of CALL_CLOSURE_KEYWORDS, CALL_SINGLETON_KEYWORDS,
 * NEW_KEYWORDS and INVOKE_KEYWORDS: a count byte with 0x80 set when a block
 * register follows, that many (u16 name, u16 register) pairs, then the block
 * register. */
static bool skip_keyword_trailer(const DiamondFunction *fn, size_t *pc) {
    uint8_t encoded = 0;
    if (!decode_u8(fn, pc, &encoded)) return false;
    const size_t bytes = 4 * (size_t)(encoded & 0x7Fu) + ((encoded & 0x80u) != 0 ? 2 : 0);
    if (*pc + bytes > fn->code_count) return false;
    *pc += bytes;
    return true;
}

static bool decode_instruction_operands(const DiamondFunction *fn, size_t *pc,
        DiamondOpCode opcode, int32_t *dest, int32_t *jump_target, int32_t *second_dest) {
    *dest = -1;
    *second_dest = -1;
    *jump_target = -1;
    bool writes_dest = false; uint8_t shorts = 0, bytes = 0;
    if (builtin_operand_layout(opcode, &writes_dest, &shorts, &bytes)) {
        uint16_t first = 0, operand = 0; uint8_t trailing = 0;
        if (!decode_u16(fn, pc, &first)) return false;
        if (writes_dest) *dest = first;
        for (uint8_t i = 0; i < shorts; i++) if (!decode_u16(fn, pc, &operand)) return false;
        for (uint8_t i = 0; i < bytes; i++) if (!decode_u8(fn, pc, &trailing)) return false;
        return true;
    }
    if (opcode_writes_no_register(opcode)) {
        switch (opcode) {
            case DIAMOND_OP_SET_IVAR: case DIAMOND_OP_INDEX_SET: {
                uint16_t a = 0, b = 0, c = 0;
                return decode_u16(fn, pc, &a) && decode_u16(fn, pc, &b) &&
                    decode_u16(fn, pc, &c);
            }
            case DIAMOND_OP_CHECK_TYPE: case DIAMOND_OP_SET_CELL: case DIAMOND_OP_SET_CAPTURE: {
                uint16_t a = 0, b = 0;
                return decode_u16(fn, pc, &a) && decode_u16(fn, pc, &b);
            }
            case DIAMOND_OP_JUMP: {
                uint8_t high = 0, low = 0;
                if (!decode_u8(fn, pc, &high) || !decode_u8(fn, pc, &low)) return false;
                *jump_target = (int32_t)(((unsigned)high << 8) | low);
                return true;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: case DIAMOND_OP_JUMP_IF_FALSE: {
                uint16_t condition = 0; uint8_t high = 0, low = 0;
                if (!decode_u16(fn, pc, &condition) || !decode_u8(fn, pc, &high) ||
                    !decode_u8(fn, pc, &low)) return false;
                *jump_target = (int32_t)(((unsigned)high << 8) | low);
                return true;
            }
            case DIAMOND_OP_POP_RESCUE: case DIAMOND_OP_END_ENSURE:
                return true;
            case DIAMOND_OP_PUSH_ENSURE: case DIAMOND_OP_RUN_ENSURE: {
                uint8_t high = 0, low = 0;
                if (!decode_u8(fn, pc, &high) || !decode_u8(fn, pc, &low)) return false;
                *jump_target = (int32_t)(((unsigned)high << 8) | low);
                return true;
            }
            case DIAMOND_OP_UNWIND_JUMP: {
                uint8_t levels = 0, high = 0, low = 0;
                if (!decode_u8(fn, pc, &levels) || !decode_u8(fn, pc, &high) ||
                    !decode_u8(fn, pc, &low)) return false;
                *jump_target = (int32_t)(((unsigned)high << 8) | low);
                return true;
            }
            case DIAMOND_OP_RAISE:
            case DIAMOND_OP_BLOCK_BREAK:
            case DIAMOND_OP_BLOCK_RETURN: {
                uint16_t a = 0;
                return decode_u16(fn, pc, &a);
            }
            case DIAMOND_OP_RETURN: {
                uint16_t a = 0;
                return decode_u16(fn, pc, &a);
            }
            case DIAMOND_OP_SET_CVAR: {
                uint16_t class_index = 0, slot = 0, source = 0;
                return decode_u16(fn, pc, &class_index) && decode_u16(fn, pc, &slot) &&
                    decode_u16(fn, pc, &source);
            }
            case DIAMOND_OP_CHECK_DESTRUCTURE_COUNT: {
                uint16_t array = 0, expected = 0;
                return decode_u16(fn, pc, &array) && decode_u16(fn, pc, &expected);
            }
            case DIAMOND_OP_CHECK_SELF_RETURN: {
                uint16_t source = 0, others = 0, receiver = 0;
                return decode_u16(fn, pc, &source) && decode_u16(fn, pc, &others) &&
                    decode_u16(fn, pc, &receiver);
            }
            default: return false; /* unreachable given opcode_writes_no_register */
        }
    }
    if (!opcode_dest_is_first_u16(opcode)) return false;
    uint16_t destination = 0;
    if (!decode_u16(fn, pc, &destination)) return false;
    *dest = destination;
    switch (opcode) {
        case DIAMOND_OP_COLLECT_VARIADIC: {
            /* Also writes the register after dest when it keeps a block,
             * and clears the spilled temporaries past it (see run_chunk). */
            uint16_t fixed = 0, preserved = 0;
            if (!decode_u16(fn, pc, &fixed) || !decode_u16(fn, pc, &preserved)) return false;
            if (preserved != 0) *second_dest = (int32_t)destination + 1;
            return true;
        }
        case DIAMOND_OP_NIL: case DIAMOND_OP_BOX_LOCAL:
            return true; /* dest only; BOX_LOCAL replaces its register with a cell */
        case DIAMOND_OP_ADD_INT_K: case DIAMOND_OP_SUBTRACT_INT_K: case DIAMOND_OP_MULTIPLY_INT_K:
        case DIAMOND_OP_MODULO_INT_K: case DIAMOND_OP_LESS_INT_K: case DIAMOND_OP_LESS_EQUAL_INT_K:
        case DIAMOND_OP_GREATER_INT_K: case DIAMOND_OP_GREATER_EQUAL_INT_K:
        case DIAMOND_OP_EQUAL_INT_K: case DIAMOND_OP_NOT_EQUAL_INT_K: {
            uint16_t left = 0, scratch = 0, constant = 0;
            if (!decode_u16(fn, pc, &left) || !decode_u16(fn, pc, &scratch) ||
                !decode_u16(fn, pc, &constant)) return false;
            *second_dest = scratch;
            return true;
        }
        case DIAMOND_OP_BOOL: case DIAMOND_OP_MOVE: case DIAMOND_OP_CONSTANT:
        case DIAMOND_OP_GET_CELL: case DIAMOND_OP_GET_CAPTURE: case DIAMOND_OP_GET_CAPTURE_CELL:
        case DIAMOND_OP_GET_NAMESPACE_CONSTANT:
        case DIAMOND_OP_ARGUMENT_PROVIDED: case DIAMOND_OP_STRING: case DIAMOND_OP_TO_STRING: {
            uint16_t a = 0;
            return decode_u16(fn, pc, &a);
        }
        case DIAMOND_OP_ADD_INT: case DIAMOND_OP_SUBTRACT_INT: case DIAMOND_OP_MULTIPLY_INT:
        case DIAMOND_OP_DIVIDE_INT: case DIAMOND_OP_LESS_INT: case DIAMOND_OP_ADD:
        case DIAMOND_OP_SUBTRACT: case DIAMOND_OP_MULTIPLY: case DIAMOND_OP_DIVIDE: case DIAMOND_OP_MODULO:
        case DIAMOND_OP_LESS: case DIAMOND_OP_LESS_EQUAL: case DIAMOND_OP_LESS_EQUAL_INT:
        case DIAMOND_OP_GREATER: case DIAMOND_OP_GREATER_INT: case DIAMOND_OP_GREATER_EQUAL:
        case DIAMOND_OP_GREATER_EQUAL_INT: case DIAMOND_OP_EQUAL: case DIAMOND_OP_NOT_EQUAL:
        case DIAMOND_OP_EQUAL_INT: case DIAMOND_OP_NOT_EQUAL_INT:
        case DIAMOND_OP_GET_IVAR: case DIAMOND_OP_INDEX_GET: case DIAMOND_OP_ARRAY:
        case DIAMOND_OP_HASH: case DIAMOND_OP_INDEX_GET_STRING: case DIAMOND_OP_IS_TYPE:
        case DIAMOND_OP_SAME_CLASS: {
            uint16_t a = 0, b = 0;
            return decode_u16(fn, pc, &a) && decode_u16(fn, pc, &b);
        }
        case DIAMOND_OP_SUPER: {
            uint16_t a = 0, b = 0, c = 0; uint8_t d = 0;
            return decode_u16(fn, pc, &c) && decode_u16(fn, pc, &a) &&
                decode_u16(fn, pc, &b) && decode_u8(fn, pc, &d);
        }
        case DIAMOND_OP_INVOKE: case DIAMOND_OP_INVOKE_MONO: {
            uint16_t a = 0, b = 0, c = 0; uint8_t d = 0;
            return decode_u16(fn, pc, &a) && decode_u16(fn, pc, &b) &&
                decode_u16(fn, pc, &c) && decode_u8(fn, pc, &d);
        }
        case DIAMOND_OP_INVOKE_TYPED: {
            uint16_t a = 0, b = 0, c = 0; uint8_t d = 0;
            return decode_u16(fn, pc, &a) && decode_u16(fn, pc, &b) &&
                decode_u16(fn, pc, &c) && decode_u8(fn, pc, &d) &&
                skip_type_trailer(fn, pc);
        }
        case DIAMOND_OP_NEW: {
            uint16_t a = 0, b = 0; uint8_t c = 0;
            return decode_u16(fn, pc, &b) && decode_u16(fn, pc, &a) &&
                decode_u8(fn, pc, &c);
        }
        case DIAMOND_OP_CALL_CLOSURE: {
            uint16_t callable = 0, base = 0; uint8_t argc = 0;
            return decode_u16(fn, pc, &callable) && decode_u16(fn, pc, &base) &&
                decode_u8(fn, pc, &argc);
        }
        case DIAMOND_OP_PUSH_RESCUE: {
            /* The clause's exception variable is this instruction's dest: catching
             * writes it. */
            uint8_t skipped = 0;
            for (size_t i = 0; i < 11; i++)
                if (!decode_u8(fn, pc, &skipped)) return false;
            return true;
        }
        case DIAMOND_OP_CLOSURE: {
            uint16_t index = 0; uint8_t count = 0;
            if (!decode_u16(fn, pc, &index) || !decode_u8(fn, pc, &count) ||
                *pc + 2 * (size_t)count > fn->code_count) return false;
            *pc += 2 * (size_t)count;
            return true;
        }
        case DIAMOND_OP_MATH_UNARY: {
            uint16_t source = 0; uint8_t function_id = 0;
            return decode_u16(fn, pc, &source) && decode_u8(fn, pc, &function_id);
        }
        case DIAMOND_OP_MATH_BINARY: {
            uint16_t left = 0, right = 0; uint8_t function_id = 0;
            return decode_u16(fn, pc, &left) && decode_u16(fn, pc, &right) &&
                decode_u8(fn, pc, &function_id);
        }
        case DIAMOND_OP_CALL:
        case DIAMOND_OP_TAIL_CALL: {
            uint16_t function_index = 0, base = 0; uint8_t argc = 0;
            return decode_u16(fn, pc, &function_index) &&
                decode_u16(fn, pc, &base) && decode_u8(fn, pc, &argc);
        }
        case DIAMOND_OP_NOT: case DIAMOND_OP_SYMBOL: case DIAMOND_OP_CLASS_NAME:
        case DIAMOND_OP_NEGATE: case DIAMOND_OP_LOAD_CLASS: {
            uint16_t a = 0;
            return decode_u16(fn, pc, &a);
        }
        case DIAMOND_OP_PRINT: {
            uint16_t source = 0; uint8_t flags = 0;
            return decode_u16(fn, pc, &source) && decode_u8(fn, pc, &flags);
        }
        case DIAMOND_OP_INVOKE_SELF_METHOD: {
            uint16_t name = 0, base = 0; uint8_t argc = 0;
            return decode_u16(fn, pc, &name) && decode_u16(fn, pc, &base) &&
                decode_u8(fn, pc, &argc);
        }
        case DIAMOND_OP_GET_CVAR: {
            uint16_t class_index = 0, slot = 0;
            return decode_u16(fn, pc, &class_index) && decode_u16(fn, pc, &slot);
        }
        /* The calls run through diamond_jit_outlined. Their layouts are the
         * READ_SHORT/READ_BYTE sequences of the matching vm_outlined.c
         * bodies; every one writes only its dest. */
        case DIAMOND_OP_CALL_TYPED: {
            uint16_t function_index = 0, base = 0; uint8_t argc = 0;
            return decode_u16(fn, pc, &function_index) && decode_u16(fn, pc, &base) &&
                decode_u8(fn, pc, &argc) && skip_type_trailer(fn, pc);
        }
        case DIAMOND_OP_CALL_SPREAD: case DIAMOND_OP_CALL_TYPED_SPREAD:
        case DIAMOND_OP_CALL_KEYWORD_SPREAD: case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD: {
            uint16_t function_index = 0, array = 0;
            if (!decode_u16(fn, pc, &function_index) || !decode_u16(fn, pc, &array)) return false;
            if (opcode == DIAMOND_OP_CALL_KEYWORD_SPREAD ||
                opcode == DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD) {
                /* count, then (slot byte, u16 register) entries: no block */
                uint8_t count = 0;
                if (!decode_u8(fn, pc, &count) || *pc + 3 * (size_t)count > fn->code_count)
                    return false;
                *pc += 3 * (size_t)count;
            }
            return (opcode != DIAMOND_OP_CALL_TYPED_SPREAD &&
                    opcode != DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD) ||
                skip_type_trailer(fn, pc);
        }
        case DIAMOND_OP_CALL_SINGLETON_KEYWORDS: case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS: {
            uint16_t function_index = 0, positional = 0, class_index = 0; uint8_t receiver = 0;
            return decode_u16(fn, pc, &function_index) && decode_u16(fn, pc, &positional) &&
                decode_u16(fn, pc, &class_index) && decode_u8(fn, pc, &receiver) &&
                skip_keyword_trailer(fn, pc) &&
                (opcode != DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS || skip_type_trailer(fn, pc));
        }
        case DIAMOND_OP_CALL_SINGLETON_SPREAD: case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: {
            uint16_t function_index = 0, spread = 0, class_index = 0; uint8_t receiver = 0;
            return decode_u16(fn, pc, &function_index) && decode_u16(fn, pc, &spread) &&
                decode_u16(fn, pc, &class_index) && decode_u8(fn, pc, &receiver) &&
                (opcode != DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD || skip_type_trailer(fn, pc));
        }
        case DIAMOND_OP_NEW_SPREAD: case DIAMOND_OP_CALL_CLOSURE_SPREAD: {
            uint16_t a = 0, b = 0;
            return decode_u16(fn, pc, &a) && decode_u16(fn, pc, &b);
        }
        case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_NEW_TYPED: {
            uint16_t class_index = 0, explicit_type = 0, positional = 0;
            return decode_u16(fn, pc, &class_index) &&
                (opcode != DIAMOND_OP_NEW_TYPED || decode_u16(fn, pc, &explicit_type)) &&
                decode_u16(fn, pc, &positional) && skip_keyword_trailer(fn, pc);
        }
        case DIAMOND_OP_INVOKE_KEYWORDS: case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: {
            uint16_t recv = 0, name = 0, positional = 0;
            return decode_u16(fn, pc, &recv) && decode_u16(fn, pc, &name) &&
                decode_u16(fn, pc, &positional) && skip_keyword_trailer(fn, pc) &&
                (opcode != DIAMOND_OP_INVOKE_TYPED_KEYWORDS || skip_type_trailer(fn, pc));
        }
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD: {
            uint16_t recv = 0, name = 0, spread = 0;
            return decode_u16(fn, pc, &recv) && decode_u16(fn, pc, &name) &&
                decode_u16(fn, pc, &spread) &&
                (opcode != DIAMOND_OP_INVOKE_TYPED_SPREAD || skip_type_trailer(fn, pc));
        }
        case DIAMOND_OP_CALL_CLOSURE_KEYWORDS: {
            uint16_t callable = 0, positional = 0;
            return decode_u16(fn, pc, &callable) && decode_u16(fn, pc, &positional) &&
                skip_keyword_trailer(fn, pc);
        }
        case DIAMOND_OP_BUILD_SPREAD_ARGS: {
            uint16_t prefix = 0, spread = 0, suffix = 0; uint8_t prefix_count = 0, suffix_count = 0;
            return decode_u16(fn, pc, &prefix) && decode_u8(fn, pc, &prefix_count) &&
                decode_u16(fn, pc, &spread) && decode_u16(fn, pc, &suffix) &&
                decode_u8(fn, pc, &suffix_count);
        }
        default: return false; /* unreachable given opcode_dest_is_first_u16 */
    }
}

/* Whether `fn` contains a backward (or self) jump, i.e. a loop. Only the
 * whitelisted opcodes are understood: any other, or a truncated stream,
 * answers false -- such a function is not compilable anyway. Used to compile
 * a loop-carrying function on its first call (see jit_call_or_interpret). */
bool diamond_jit_function_has_loop(const DiamondFunction *fn) {
    size_t pc = 0;
    while (pc < fn->code_count) {
        const size_t start = pc;
        uint8_t raw_opcode = 0;
        if (!decode_u8(fn, &pc, &raw_opcode)) return false;
        int32_t dest = -1, jump_target = -1, second_dest = -1;
        if (!decode_instruction_operands(fn, &pc, (DiamondOpCode)raw_opcode,
                &dest, &jump_target, &second_dest)) return false;
        if (raw_opcode == DIAMOND_OP_TAIL_CALL) return true; /* a loop when self */
        if (jump_target >= 0 && (size_t)jump_target <= start) return true;
    }
    return false;
}

/* Whether no instruction before COLLECT_VARIADIC (at `collect_start`) writes
 * a register at or past `fixed_count`, where the rest arguments (and a kept
 * block) wait. run_chunk collects from the call's untouched arguments;
 * compiled code collects from the registers, so parameter setup that puts a
 * temporary there (ARGUMENT_PROVIDED for a default before the rest
 * parameter) would overwrite an argument first. Such a function stays with
 * the interpreter. */
static bool variadic_arguments_untouched(const DiamondFunction *fn, size_t collect_start,
        uint16_t fixed_count) {
    size_t pc = 0;
    while (pc < collect_start) {
        uint8_t raw_opcode = 0;
        if (!decode_u8(fn, &pc, &raw_opcode)) return false;
        int32_t dest = -1, jump_target = -1, second_dest = -1;
        if (!decode_instruction_operands(fn, &pc, (DiamondOpCode)raw_opcode,
                &dest, &jump_target, &second_dest)) return false;
        if (dest >= (int32_t)fixed_count || second_dest >= (int32_t)fixed_count) return false;
    }
    return pc == collect_start;
}

static bool parameter_never_reassigned(const DiamondFunction *fn, uint16_t target_register) {
    size_t pc = 0;
    while (pc < fn->code_count) {
        uint8_t raw_opcode = 0;
        if (!decode_u8(fn, &pc, &raw_opcode)) return false;
        int32_t dest = -1, jump_target = -1, second_dest = -1;
        if (!decode_instruction_operands(fn, &pc, (DiamondOpCode)raw_opcode,
                &dest, &jump_target, &second_dest)) return false;
        if (dest == (int32_t)target_register || second_dest == (int32_t)target_register)
            return false;
    }
    return true;
}

/* Phase 10 (NEW), extended by Phase 11 (GET_IVAR): single-hop helper for
 * register_new_class_if_sole_writer below. Scans fn's whole body once for
 * `target_register`'s writers. Returns the compile-time class index (>=0)
 * if there's EXACTLY ONE writer and it's either DIAMOND_OP_NEW, or a
 * DIAMOND_OP_GET_IVAR of a field DiamondFunction.ivar_known_class already
 * proves is a single concrete class (Phase 11: `x = self.assoc; ...;
 * x.method()`, closing the roadmap's own named "an ivar load" INVOKE-
 * receiver gap -- see that field's own comment, src/vm.h, for why this is
 * sound where reusing the LSP's per-register scope_type_facts table, tried
 * and rejected in Phase 10, would not have been); sets `*move_src` and
 * returns -2 if there's exactly one writer and it's DIAMOND_OP_MOVE (the
 * caller re-scans from `*move_src`); returns -1 for anything else (zero
 * writers, multiple writers, or a single writer that's neither). The MOVE
 * case is needed because `x = SomeClass.new(...)` doesn't compile to a NEW
 * that directly targets `x`'s own register -- the compiler emits NEW into
 * its own temp register, then a separate MOVE into whichever register the
 * local `x` actually lives in (confirmed against a real `--dump-bytecode`
 * disassembly before writing this: `NEW r11, classN, r10, 1 args` followed
 * immediately by `MOVE r12, r11`, `r12` being the local INVOKE later reads
 * as its receiver, not `r11`) -- `x = self.assoc` compiles the same way
 * (GET_IVAR into a temp, then MOVE into `x`'s own register), so the same
 * MOVE-chasing loop covers both without any change to it. Same fail-safe-
 * by-construction default as parameter_never_reassigned/this file's other
 * scanner. */
static int32_t register_new_class_or_move_src(const DiamondFunction *fn,
        uint16_t target_register, uint16_t *move_src) {
    /* Walks with decode_instruction_operands, the same decoder every other
     * scan here uses, so an opcode the JIT learns is understood here too
     * (a separate copy of the decoding drifted: functions using a newer
     * opcode lost every class fact). Only the sole writer's own operands
     * are read again, from its start. */
    size_t pc = 0;
    size_t write_count = 0;
    size_t writer_start = SIZE_MAX;
    while (pc < fn->code_count) {
        const size_t start = pc;
        uint8_t raw_opcode = 0;
        if (!decode_u8(fn, &pc, &raw_opcode)) return -1;
        int32_t dest = -1, jump_target = -1, second_dest = -1;
        if (!decode_instruction_operands(fn, &pc, (DiamondOpCode)raw_opcode,
                &dest, &jump_target, &second_dest)) return -1;
        if (dest == (int32_t)target_register) { write_count++; writer_start = start; }
        if (second_dest == (int32_t)target_register) { write_count++; writer_start = SIZE_MAX; }
        if (write_count > 1) return -1;
    }
    if (write_count != 1 || writer_start == SIZE_MAX) return -1;
    /* The writer's operands after its dest (decoded above, so in bounds). */
    size_t operands = writer_start + 3;
    uint16_t first = 0;
    switch ((DiamondOpCode)fn->code[writer_start]) {
        case DIAMOND_OP_MOVE:
            if (!decode_u16(fn, &operands, &first)) return -1;
            *move_src = first;
            return -2;
        /* Phase 11: an ivar load is a second class-producing terminal,
         * alongside NEW below -- see DiamondFunction.ivar_known_class's
         * own comment (src/vm.h). ivar_known_class covers only the
         * first DIAMOND_MAX_JIT_KNOWN_FIELDS fields; a later field just
         * has no known class here. */
        case DIAMOND_OP_GET_IVAR: {
            uint16_t field = 0;
            if (!decode_u16(fn, &operands, &first) || !decode_u16(fn, &operands, &field))
                return -1;
            /* Only a class method's own self has this class's snapshot.
             * Blocks capture self but are not class-table methods, so their
             * zero-initialized table is not a proof (class zero is valid).
             * A different receiver's field index belongs to its own class. */
            if (fn->owner_class < DIAMOND_OWNER_CAPTURED_SELF && first == 0 &&
                field < DIAMOND_MAX_JIT_KNOWN_FIELDS &&
                fn->ivar_known_class[field] != DIAMOND_NO_CLASS)
                return (int32_t)fn->ivar_known_class[field];
            return -1;
        }
        /* Phase 12 (docs/internal/jit-design.md): a third class-
         * producing terminal, alongside NEW and GET_IVAR above -- an
         * INVOKE result the *compiler* already proved (real
         * semantics, via publish_instance_return_type, not LSP
         * heuristics) holds a single concrete class. Gated on
         * !fn->redefine_method_used_anywhere -- see that field's own
         * comment (src/vm.h) for why a method's declared return type,
         * unlike NEW's own class-index operand or Phase 11's ivar
         * fact, can go stale at runtime via redefine_method, in a way
         * nothing else this scan trusts can. */
        case DIAMOND_OP_INVOKE: case DIAMOND_OP_INVOKE_MONO:
            if (!fn->redefine_method_used_anywhere &&
                target_register < DIAMOND_JIT_MAX_REGISTERS &&
                fn->register_known_class[target_register] != DIAMOND_NO_CLASS)
                return (int32_t)fn->register_known_class[target_register];
            return -1;
        case DIAMOND_OP_NEW:
            if (!decode_u16(fn, &operands, &first)) return -1;
            return (int32_t)first;
        /* Any other writer (a call's result included: a plain function's
         * declared return type is not trusted by this scan) proves no
         * class. */
        default:
            return -1;
    }
}

/* Phase 10 (NEW) / Phase 11 (GET_IVAR): returns the compile-time class
 * index if `target_register` provably always holds the result of one
 * specific DIAMOND_OP_NEW, or one specific known-single-class
 * DIAMOND_OP_GET_IVAR (see register_new_class_or_move_src's own updated
 * comment), chasing through its single MOVE-hop result for up to 8 hops
 * (generous for any real compiler-generated local-assignment shape, cheap
 * to bound) -- else -1. Each hop re-scans fn's whole body, same
 * O(function body length) cost class as parameter_never_reassigned already
 * accepts per INVOKE site. Despite the name (kept as-is rather than
 * renamed to avoid a repo-wide rename of every INVOKE-site call/comment
 * that already says "register_new_class_if_sole_writer" for a still-
 * accurate, if now incomplete, description), this now proves either
 * shape. */
static int32_t register_new_class_if_sole_writer(const DiamondFunction *fn, uint16_t target_register) {
    uint16_t current = target_register;
    for (int hop = 0; hop < 8; hop++) {
        uint16_t move_src = 0;
        const int32_t result = register_new_class_or_move_src(fn, current, &move_src);
        if (result != -2) return result;
        current = move_src;
    }
    return -1;
}

/* The DiamondJitNativeReadOp for a native String/Array/Hash method the JIT
 * reads inline (length, index_of, ord, slice, key_at, value_at), or false
 * when the call is not one of them. */
static bool jit_native_read_operation(uint16_t site_type,
        const DiamondStringConstant *native_name, uint8_t argc,
        DiamondJitNativeReadOp *operation_out) {
    DiamondJitNativeReadOp operation=DIAMOND_JIT_NATIVE_ARRAY_LENGTH;
    bool supported=false;
    if(native_name->length==6&&
       memcmp(native_name->chars,"length",6)==0&&argc==0) {
        operation=site_type==DIAMOND_TYPE_STRING?
            DIAMOND_JIT_NATIVE_STRING_LENGTH:
            (site_type==DIAMOND_TYPE_ARRAY?
                DIAMOND_JIT_NATIVE_ARRAY_LENGTH:
                DIAMOND_JIT_NATIVE_HASH_LENGTH);
        supported=true;
    } else if(site_type==DIAMOND_TYPE_STRING&&argc==1&&
              native_name->length==8&&
              memcmp(native_name->chars,"index_of",8)==0) {
        operation=DIAMOND_JIT_NATIVE_STRING_INDEX_OF;supported=true;
    } else if(site_type==DIAMOND_TYPE_STRING&&argc==0&&
              native_name->length==3&&
              memcmp(native_name->chars,"ord",3)==0) {
        operation=DIAMOND_JIT_NATIVE_STRING_ORD;supported=true;
    } else if(site_type==DIAMOND_TYPE_STRING&&argc==2&&
              native_name->length==5&&
              memcmp(native_name->chars,"slice",5)==0) {
        operation=DIAMOND_JIT_NATIVE_STRING_SLICE;supported=true;
    } else if(site_type==DIAMOND_TYPE_HASH&&argc==1&&
              native_name->length==6&&
              memcmp(native_name->chars,"key_at",6)==0) {
        operation=DIAMOND_JIT_NATIVE_HASH_KEY_AT;supported=true;
    } else if(site_type==DIAMOND_TYPE_HASH&&argc==1&&
              native_name->length==8&&
              memcmp(native_name->chars,"value_at",8)==0) {
        operation=DIAMOND_JIT_NATIVE_HASH_VALUE_AT;supported=true;
    }
    *operation_out=operation;
    return supported;
}

/* Compiles DIAMOND_OP_INVOKE/INVOKE_MONO. A site whose receiver type is
 * known gets the cheapest call that type allows, tried in order:
 *  0. A String/Array/Hash receiver known at the site: an inline native
 *     read (length, index_of, ...) through compile_native_call.
 *  1. Phase 7 (self.method(), any name): when recv==0 in a
 *     function whose owner_class proves register 0 is always
 *     the receiver (a real class index, not the DIAMOND_OWNER_NONE/
 *     MODULE/CAPTURED_SELF plain-function/module-method/
 *     closure sentinels -- see compile_invoke_dispatch's own
 *     comment for the invariant this relies on).
 *  2. Phase 9 (typed-param.method(), any name): when recv is
 *     statically proven to always hold an Instance of a
 *     single concrete class for the whole function body -- see
 *     parameter_is_single_class/parameter_never_reassigned's
 *     own comments.
 *  3. Phase 10 (x = SomeClass.new(...); ...; x.method()),
 *     extended by Phase 11 (x = self.assoc; ...; x.method(),
 *     an ivar load instead of a constructor call): when recv
 *     has exactly one writer in the whole function and that
 *     writer is DIAMOND_OP_NEW, or a DIAMOND_OP_GET_IVAR of a
 *     field DiamondFunction.ivar_known_class already proves is
 *     a single concrete class -- see register_new_class_if_
 *     sole_writer's own comment. Phase 15 adds the same for a
 *     receiver the compiler narrowed at this one site.
 * Strategies 1-3 call diamond_jit_invoke_instance, which skips the
 * receiver-kind checks. Every other site -- an untyped receiver, a native
 * per-type method, a polymorphic Instance site -- goes through
 * compile_invoke_any, the interpreter's own dispatch, so it no longer keeps
 * the whole function interpreted. DIAMOND_OP_INVOKE_TYPED always calls
 * diamond_jit_invoke_typed (compile_body's own case). */
/* The native half of a method call on an Instance (compile_invoke_native).
 * `done` is the jump past the slow path; `active` false when the site got
 * no native path (ids exhausted), so only the slow path was emitted. */
typedef struct {
    size_t done;
    size_t accessor_done;
    bool active;
    bool has_accessor;
} NativeInvoke;

/* A listed class whose method is a trivial accessor (DiamondJitCallEntry's
 * accessor fields, entry in RDX): the getter's field is copied to dest, or
 * the setter's argument stored and copied to dest, here and without a call,
 * when trivial_getter_result or trivial_setter_result would answer it. Its
 * shape must already cover the field, and a setter's receiver must be
 * unfrozen and need no write barrier. Anything else jumps to `call` and
 * the method runs. Returns the jump past the call. Keeps RDX; clobbers RAX,
 * RCX, RSI, R8 and R9. */
static size_t emit_invoke_accessor_inline(JitCompiler *jc, uint16_t dest, uint16_t recv,
        uint16_t base, uint8_t argc, size_t *call, size_t *call_count) {
    JitBuffer *buf = &jc->buf;
    emit_load_byte_zx(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, accessor));
    emit_cmp_imm32_32(buf, REG_RAX, argc == 0 ? DIAMOND_ACCESSOR_GETTER : DIAMOND_ACCESSOR_SETTER);
    call[(*call_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_byte_zx(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, accessor_field));
    emit_load_r64(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(recv, AS_OFF));
    emit_cmp_r64_mem(buf, REG_RCX, REG_R8, (int32_t)offsetof(DiamondInstance, field_count));
    call[(*call_count)++] = emit_jcc_placeholder(buf, JCC_AE);
    emit_load_r64(buf, REG_R9, REG_R8, (int32_t)offsetof(DiamondInstance, shape));
    emit_load_byte_zx(buf, REG_RAX, REG_R9, (int32_t)offsetof(DiamondShape, field_count));
    emit_u8(buf, 0x39); emit_u8(buf, 0xC8);                       /* cmp eax, ecx */
    call[(*call_count)++] = emit_jcc_placeholder(buf, JCC_BE);
    if (argc == 1) {
        emit_load_byte_zx(buf, REG_RAX, REG_R8, (int32_t)offsetof(DiamondObject, frozen));
        emit_test_r8(buf, REG_RAX);
        call[(*call_count)++] = emit_jcc_placeholder(buf, JCC_NE);
        emit_load_byte_zx(buf, REG_RAX, REG_R8, (int32_t)offsetof(DiamondObject, old));
        emit_test_r8(buf, REG_RAX);
        const size_t young = emit_jcc_placeholder(buf, JCC_E);
        emit_load_byte_zx(buf, REG_RAX, REG_R8, (int32_t)offsetof(DiamondObject, remembered));
        emit_test_r8(buf, REG_RAX);
        call[(*call_count)++] = emit_jcc_placeholder(buf, JCC_E);
        patch_rel32_to_here(buf, young);
    }
    /* r9 = &instance->fields[field] */
    emit_u8(buf, 0x48); emit_u8(buf, 0xC1); emit_u8(buf, 0xE1); emit_u8(buf, 0x04);  /* shl rcx, 4 */
    emit_u8(buf, 0x4D); emit_u8(buf, 0x8D); emit_u8(buf, 0x8C); emit_u8(buf, 0x08);  /* lea r9, [r8+rcx+d32] */
    emit_u32_le(buf, (uint32_t)offsetof(DiamondInstance, fields));
    /* The value the declared type checks: the field read, or the argument. */
    emit_load_byte_zx(buf, REG_RAX, REG_RDX,
        (int32_t)offsetof(DiamondJitCallEntry, accessor_value_kind));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_JIT_ANY_VALUE_KIND);
    const size_t untyped = emit_jcc_placeholder(buf, JCC_E);
    if (argc == 0) emit_load_byte_zx(buf, REG_RCX, REG_R9, KIND_OFF);
    else emit_load_byte_zx(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(base, KIND_OFF));
    emit_u8(buf, 0x39); emit_u8(buf, 0xC8);                       /* cmp eax, ecx */
    call[(*call_count)++] = emit_jcc_placeholder(buf, JCC_NE);
    patch_rel32_to_here(buf, untyped);
    if (argc == 0) {
        emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_R9, 0);
    } else {
        emit_copy_value(buf, REG_R9, 0, JIT_REGISTERS_BASE, reg_disp(base, 0));
        if (dest != base)
            emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0),
                JIT_REGISTERS_BASE, reg_disp(base, 0));
    }
    return emit_jmp_placeholder(buf);
}

/* Emits the fast path of an INVOKE/INVOKE_MONO whose receiver is
 * registers[recv]: an Instance whose class this site's DiamondJitCallSite
 * lists (jit.h) is called straight into the method's compiled code with
 * emit_native_entry_call. A status other than OK goes to
 * diamond_jit_method_call_finish. Everything else lands after this, where
 * diamond_jit_call_site_fill may list the class and the caller then emits
 * its usual C dispatch; emit_invoke_native_end joins the two. */
static NativeInvoke emit_invoke_native_begin(JitCompiler *jc, size_t instruction_start,
        uint16_t dest, uint16_t recv, uint16_t base, uint8_t argc) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    const uint_fast32_t site_id = atomic_fetch_add_explicit(&jit_next_call_site, 1,
        memory_order_relaxed);
    if (site_id >= (uint_fast32_t)(INT32_MAX / sizeof(DiamondJitCallSite)) - 1)
        return (NativeInvoke){.active = false};
    const int32_t site_at = (int32_t)(site_id * sizeof(DiamondJitCallSite));
    /* `slow` lands on the fill, `skip` past it: a receiver that is not an
     * Instance, or a site that has stopped trying, goes straight to C. */
    size_t slow[8];
    size_t slow_count = 0;
    size_t skip[4];
    size_t skip_count = 0;
    emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(recv, 0),
        DIAMOND_OBJECT_INSTANCE, skip, &skip_count);
    emit_load_r64(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondInstance, class));
    emit_mov_rr(buf, REG_RDX, JIT_VM);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_site_count));
    emit_cmp_imm32_64(buf, REG_RCX, (uint32_t)site_id);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_BE);
    emit_load_r64(buf, REG_RDX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_sites));
    emit_lea(buf, REG_RDX, REG_RDX, site_at);
    size_t hits[DIAMOND_JIT_CALL_WAYS];
    for (int way = 0; way < DIAMOND_JIT_CALL_WAYS; way++) {
        emit_cmp_r64_mem(buf, REG_RAX, REG_RDX, (int32_t)((size_t)way * sizeof(DiamondJitCallEntry)));
        hits[way] = emit_jcc_placeholder(buf, JCC_E);
    }
    /* cmp dword [rdx + failed_fills], DIAMOND_JIT_CALL_FILL_TRIES */
    emit_u8(buf, 0x81); emit_u8(buf, 0xBA);
    emit_u32_le(buf, (uint32_t)offsetof(DiamondJitCallSite, failed_fills));
    emit_u32_le(buf, DIAMOND_JIT_CALL_FILL_TRIES);
    skip[skip_count++] = emit_jcc_placeholder(buf, JCC_AE);
    slow[slow_count++] = emit_jmp_placeholder(buf);
    size_t joins[DIAMOND_JIT_CALL_WAYS];
    for (int way = DIAMOND_JIT_CALL_WAYS - 1; way >= 0; way--) {
        patch_rel32_to_here(buf, hits[way]);
        if (way > 0) {
            emit_lea(buf, REG_RDX, REG_RDX, (int32_t)((size_t)way * sizeof(DiamondJitCallEntry)));
            joins[way] = emit_jmp_placeholder(buf);
        }
    }
    for (int way = 1; way < DIAMOND_JIT_CALL_WAYS; way++) patch_rel32_to_here(buf, joins[way]);
    /* RDX is the entry. */
    size_t accessor_done = 0;
    const bool has_accessor = argc <= 1;
    if (has_accessor) {
        size_t call[8];
        size_t call_count = 0;
        accessor_done = emit_invoke_accessor_inline(jc, dest, recv, base, argc, call, &call_count);
        for (size_t i = 0; i < call_count; i++) patch_rel32_to_here(buf, call[i]);
    }
    emit_native_depth_check(jc, slow, &slow_count);
    emit_load_r64(buf, REG_R10, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, code));
    emit_load_r64(buf, REG_R11, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, chunk));
    emit_load_r32(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, frame_bytes));
    emit_native_entry_call(jc, dest, recv, base, argc, 0, false);
    emit_test_r8(buf, REG_RAX);
    const size_t returned_ok = emit_jcc_placeholder(buf, JCC_E);
    emit_movzx_r64_al(buf, REG_RAX);
    emit_push(buf, REG_RAX);                 /* pad */
    emit_push(buf, REG_RAX);                 /* status -> [rsp+0] */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, REG_RDX);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX,
        (uint64_t)recv | ((uint64_t)base << 16) | ((uint64_t)argc << 32));
    emit_mov_rr(buf, REG_R8, JIT_DEPTH);
    emit_lea(buf, REG_R9, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_method_call_finish);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    patch_rel32_to_here(buf, returned_ok);
    const size_t done = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(recv, 0));
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_mov_imm64(buf, REG_R8, (uint64_t)site_id);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_call_site_fill);
    for (size_t i = 0; i < skip_count; i++) patch_rel32_to_here(buf, skip[i]);
    return (NativeInvoke){.done = done, .accessor_done = accessor_done, .active = true,
        .has_accessor = has_accessor};
}

static void emit_invoke_native_end(JitCompiler *jc, NativeInvoke native) {
    if (native.active) patch_rel32_to_here(&jc->buf, native.done);
    if (native.has_accessor) patch_rel32_to_here(&jc->buf, native.accessor_done);
}

/* `Class.new(...)` in front of compile_new: when this site's entry (jit.h,
 * DiamondJitCallSite) names chunk->classes[class_index], the instance is
 * allocated through diamond_jit_new_object, stored in registers[dest] and
 * handed to the compiled `initialize` with emit_native_entry_call, its
 * result discarded. A status other than OK goes to
 * diamond_jit_method_call_finish with the new instance as the receiver.
 * Anything else, a failed allocation included, takes
 * diamond_jit_new_site_fill and then compile_new's C path from the start. */
static void compile_new_native(JitCompiler *jc, uint16_t dest, uint16_t class_index,
        uint16_t base, uint8_t argc) {
    const uint_fast32_t site_id = atomic_fetch_add_explicit(&jit_next_call_site, 1,
        memory_order_relaxed);
    if (site_id >= (uint_fast32_t)(INT32_MAX / sizeof(DiamondJitCallSite)) - 1) {
        compile_new(jc, dest, class_index, base, argc);
        return;
    }
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    const int32_t site_at = (int32_t)(site_id * sizeof(DiamondJitCallSite));
    size_t slow[8];
    size_t slow_count = 0;
    emit_mov_rr(buf, REG_RDX, JIT_VM);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_site_count));
    emit_cmp_imm32_64(buf, REG_RCX, (uint32_t)site_id);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_BE);
    emit_load_r64(buf, REG_RDX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_sites));
    emit_lea(buf, REG_RDX, REG_RDX, site_at);
    emit_load_r64(buf, REG_RCX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, class_count));
    emit_cmp_imm32_64(buf, REG_RCX, class_index);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_BE);
    emit_load_r64(buf, REG_RSI, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, classes));
    emit_load_r64(buf, REG_RSI, REG_RSI, (int32_t)((size_t)class_index * sizeof(DiamondClass *)));
    emit_test_r64(buf, REG_RSI, REG_RSI);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_E);
    emit_cmp_r64_mem(buf, REG_RSI, REG_RDX,
        (int32_t)offsetof(DiamondJitCallEntry, receiver_class));
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_NE);
    emit_native_depth_check(jc, slow, &slow_count);
    /* (vm, class) */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_new_object);
    emit_test_r64(buf, REG_RAX, REG_RAX);
    slow[slow_count++] = emit_jcc_placeholder(buf, JCC_E);
    emit_mov_imm64(buf, REG_RCX, DIAMOND_VALUE_OBJECT);
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RCX);
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
    /* The entry again: the allocation ran C. */
    emit_mov_rr(buf, REG_RDX, JIT_VM);
    emit_load_r64(buf, REG_RDX, REG_RDX, (int32_t)offsetof(DiamondVm, jit_call_sites));
    emit_lea(buf, REG_RDX, REG_RDX, site_at);
    emit_load_r64(buf, REG_R10, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, code));
    emit_test_r64(buf, REG_R10, REG_R10);
    const size_t no_initialize = emit_jcc_placeholder(buf, JCC_E);
    emit_load_r64(buf, REG_R11, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, chunk));
    emit_load_r32(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondJitCallEntry, frame_bytes));
    emit_native_entry_call(jc, dest, dest, base, argc, 0, true);
    emit_test_r8(buf, REG_RAX);
    const size_t returned_ok = emit_jcc_placeholder(buf, JCC_E);
    /* status at [rsp], a pad, then 16 bytes for the discarded result */
    emit_movzx_r64_al(buf, REG_RAX);
    emit_push(buf, REG_RAX);
    emit_push(buf, REG_RAX);
    emit_push(buf, REG_RAX);
    emit_push(buf, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, REG_RDX);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX,
        (uint64_t)dest | ((uint64_t)base << 16) | ((uint64_t)argc << 32));
    emit_mov_rr(buf, REG_R8, JIT_DEPTH);
    emit_lea_rsp(buf, REG_R9, 16);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_method_call_finish);
    emit_add_rsp_imm32(buf, 32);
    emit_bail_if_al_nonzero(jc);
    patch_rel32_to_here(buf, returned_ok);
    patch_rel32_to_here(buf, no_initialize);
    const size_t done = emit_jmp_placeholder(buf);
    for (size_t i = 0; i < slow_count; i++) patch_rel32_to_here(buf, slow[i]);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_imm64(buf, REG_RDX, (uint64_t)site_id);
    emit_mov_imm64(buf, REG_RCX, class_index);
    emit_mov_imm64(buf, REG_R8, argc);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_new_site_fill);
    compile_new(jc, dest, class_index, base, argc);
    patch_rel32_to_here(buf, done);
}

static void compile_invoke_dispatch_native(JitCompiler *jc, size_t instruction_start,
        uint16_t dest, uint16_t recv, uint16_t name, uint16_t base, uint8_t argc,
        bool monomorphic) {
    const NativeInvoke native = emit_invoke_native_begin(jc, instruction_start, dest, recv,
        base, argc);
    compile_invoke_dispatch(jc, instruction_start, dest, recv, name, base, argc, monomorphic);
    emit_invoke_native_end(jc, native);
}

/* A receiver the compiler knows is a String, an Array, a Hash or another
 * built-in value is never an Instance, so that site skips the native path. */
static bool site_type_is_builtin(uint16_t site_type) {
    switch (site_type) {
        case DIAMOND_TYPE_INT: case DIAMOND_TYPE_FLOAT: case DIAMOND_TYPE_STRING:
        case DIAMOND_TYPE_BOOL: case DIAMOND_TYPE_NIL: case DIAMOND_TYPE_ARRAY:
        case DIAMOND_TYPE_HASH: case DIAMOND_TYPE_SYMBOL: case DIAMOND_TYPE_NATIVE:
            return true;
        default:
            return false;
    }
}

static void compile_invoke_any_native(JitCompiler *jc, size_t instruction_start,
        uint16_t dest, uint16_t recv, uint16_t base, uint8_t argc, uint16_t site_type) {
    if (site_type_is_builtin(site_type)) {
        compile_invoke_any(jc, instruction_start);
        return;
    }
    const NativeInvoke native = emit_invoke_native_begin(jc, instruction_start, dest, recv,
        base, argc);
    compile_invoke_any(jc, instruction_start);
    emit_invoke_native_end(jc, native);
}

/* `array.each() do |x| ... end` and `array.map() do |x| ... end` (see jit.h,
 * DiamondJitLoop): diamond_jit_loop_begin decides, then the loop runs here,
 * calling the block's compiled entry for each element as
 * emit_native_entry_call would, with the closure as the seventh argument and
 * the element as its one argument (after a nil self slot for a block written
 * in a method). The count is re-read every time round, as the C loop does.
 * A status other than OK, and every map result, goes through
 * diamond_jit_loop_step; the end through diamond_jit_loop_end. When begin
 * says no, `fallback` emits the usual call. */
static void compile_block_loop(JitCompiler *jc, size_t instruction_start, uint16_t dest,
        uint16_t recv, uint16_t base, uint8_t argc, uint16_t site_type) {
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    const int32_t L = 0;   /* the loop state sits at [rsp] while the loop runs */
    emit_sub_rsp_imm32(buf, DIAMOND_JIT_LOOP_BYTES);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RCX, (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
    emit_mov_rr(buf, REG_R8, JIT_DEPTH);
    emit_lea_rsp(buf, REG_R9, L);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_loop_begin);
    emit_test_r8(buf, REG_RAX);
    const size_t declined = emit_jcc_placeholder(buf, JCC_E);

    const size_t loop_top = buf->length;
    /* This loop bypasses block_call_run. Send cancellation through step's
     * failure path so map releases its protected result before unwinding. */
    emit_mov_rr(buf, REG_RAX, JIT_VM);
    emit_load_r64(buf, REG_RAX, REG_RAX, (int32_t)offsetof(DiamondVm, interrupt_flag));
    emit_test_r64(buf, REG_RAX, REG_RAX);
    const size_t no_interrupt_flag = emit_jcc_placeholder(buf, JCC_E);
    emit_load_byte_zx(buf, REG_RAX, REG_RAX, 0);
    emit_test_r8(buf, REG_RAX);
    const size_t not_interrupted = emit_jcc_placeholder(buf, JCC_E);
    emit_mov_imm64(buf, REG_RAX, DIAMOND_VM_INTERRUPTED);
    const size_t interrupted = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, no_interrupt_flag);
    patch_rel32_to_here(buf, not_interrupted);
    emit_load_r64_sib(buf, REG_RDX, REG_RSP, L + (int32_t)offsetof(DiamondJitLoop, array));
    emit_load_r64_sib(buf, REG_RAX, REG_RSP, L + (int32_t)offsetof(DiamondJitLoop, index));
    emit_cmp_r64_mem(buf, REG_RAX, REG_RDX, (int32_t)offsetof(DiamondArray, count));
    const size_t finished = emit_jcc_placeholder(buf, JCC_AE);
    emit_load_r64(buf, REG_RCX, REG_RDX, (int32_t)offsetof(DiamondArray, values));
    emit_u8(buf, 0x48); emit_u8(buf, 0xC1); emit_u8(buf, 0xE0); emit_u8(buf, 0x04);  /* shl rax, 4 */
    emit_u8(buf, 0x0F); emit_u8(buf, 0x10); emit_u8(buf, 0x14); emit_u8(buf, 0x01);  /* movups xmm2, [rcx+rax] */
    emit_movups_store_rsp(buf, L + (int32_t)offsetof(DiamondJitLoop, current), 2);
    /* Everything the call needs, read before RSP moves. */
    emit_load_r64_sib(buf, REG_R10, REG_RSP, L + (int32_t)offsetof(DiamondJitLoop, code));
    emit_load_r64_sib(buf, REG_R11, REG_RSP, L + (int32_t)offsetof(DiamondJitLoop, child));
    emit_load_r64_sib(buf, REG_R8, REG_RSP, L + (int32_t)offsetof(DiamondJitLoop, closure));
    /* mov eax, [rsp + frame_bytes] */
    emit_u8(buf, 0x8B); emit_u8(buf, 0x84); emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)(L + (int32_t)offsetof(DiamondJitLoop, frame_bytes)));
    /* movzx r9d, byte [rsp + self_slot] */
    emit_u8(buf, 0x44); emit_u8(buf, 0x0F); emit_u8(buf, 0xB6); emit_u8(buf, 0x8C); emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)(L + (int32_t)offsetof(DiamondJitLoop, self_slot)));
    /* The header and register file, as emit_native_entry_call lays them out. */
    emit_lea(buf, REG_RCX, REG_RAX, NATIVE_CALL_HEADER);
    emit_u8(buf, 0x48); emit_u8(buf, 0x29); emit_u8(buf, 0xCC);   /* sub rsp, rcx */
    emit_store_r64_sib(buf, REG_RSP, 8, REG_RCX);
    emit_store_r64_sib(buf, REG_RSP, 16, REG_R11);
    emit_store_r64_sib(buf, REG_RSP, 0, REG_R8);
    emit_zero_xmm0(buf);
    emit_lea_rsp(buf, REG_RDI, NATIVE_CALL_HEADER);
    /* lea rcx, [rsp + rax + NATIVE_CALL_HEADER] */
    emit_u8(buf, 0x48); emit_u8(buf, 0x8D); emit_u8(buf, 0x4C);
    emit_u8(buf, 0x04); emit_u8(buf, NATIVE_CALL_HEADER);
    emit_u8(buf, 0x0F); emit_u8(buf, 0x11); emit_u8(buf, 0x07);   /* movups [rdi], xmm0 */
    emit_u8(buf, 0x48); emit_u8(buf, 0x83); emit_u8(buf, 0xC7);   /* add rdi, 16 */
    emit_u8(buf, 0x10);
    emit_u8(buf, 0x48); emit_u8(buf, 0x39); emit_u8(buf, 0xCF);   /* cmp rdi, rcx */
    emit_u8(buf, 0x72); emit_u8(buf, (uint8_t)-12);               /* jb the loop */
    /* The element: register 0, or register 1 behind a nil self slot. */
    emit_u8(buf, 0x45); emit_u8(buf, 0x84); emit_u8(buf, 0xC9);   /* test r9b, r9b */
    const size_t with_self = emit_jcc_placeholder(buf, JCC_NE);
    emit_movups_store_rsp(buf, NATIVE_CALL_HEADER, 2);
    emit_mov_imm64(buf, REG_RCX, 1);
    const size_t placed = emit_jmp_placeholder(buf);
    patch_rel32_to_here(buf, with_self);
    emit_movups_store_rsp(buf, NATIVE_CALL_HEADER + 16, 2);
    emit_mov_imm64(buf, REG_RCX, 2);
    patch_rel32_to_here(buf, placed);
    /* (vm, registers, &loop.result, count, chunk, depth + 1): the loop state
     * is now [rsp + header-and-file size]. */
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_lea_rsp(buf, REG_RSI, NATIVE_CALL_HEADER);
    emit_load_r64_sib(buf, REG_RDX, REG_RSP, 8);
    emit_u8(buf, 0x48); emit_u8(buf, 0x01); emit_u8(buf, 0xE2);   /* add rdx, rsp */
    emit_u8(buf, 0x48); emit_u8(buf, 0x83); emit_u8(buf, 0xC2);   /* add rdx, result */
    emit_u8(buf, (uint8_t)(L + (int32_t)offsetof(DiamondJitLoop, result)));
    emit_mov_rr(buf, REG_R8, REG_R11);
    emit_lea(buf, REG_R9, JIT_DEPTH, 1);
    emit_call_reg(buf, REG_R10);
    emit_load_r64_sib(buf, REG_RCX, REG_RSP, 8);
    emit_u8(buf, 0x48); emit_u8(buf, 0x01); emit_u8(buf, 0xCC);   /* add rsp, rcx */
    /* Status in AL. each skips the step when it is OK; map always takes it. */
    emit_movzx_r64_al(buf, REG_RAX);
    /* movzx ecx, byte [rsp + map] */
    emit_u8(buf, 0x0F); emit_u8(buf, 0xB6); emit_u8(buf, 0x8C); emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)(L + (int32_t)offsetof(DiamondJitLoop, map)));
    emit_u8(buf, 0x08); emit_u8(buf, 0xC1);                       /* or cl, al */
    const size_t no_step = emit_jcc_placeholder(buf, JCC_E);
    patch_rel32_to_here(buf, interrupted);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_lea_rsp(buf, REG_RSI, L);
    emit_mov_rr(buf, REG_RDX, REG_RAX);
    emit_mov_rr(buf, REG_RCX, JIT_DEPTH);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_loop_step);
    emit_test_r8(buf, REG_RAX);
    const size_t failed = emit_jcc_placeholder(buf, JCC_NE);
    patch_rel32_to_here(buf, no_step);
    /* add qword [rsp + index], 1 */
    emit_u8(buf, 0x48); emit_u8(buf, 0x83); emit_u8(buf, 0x84); emit_u8(buf, 0x24);
    emit_u32_le(buf, (uint32_t)(L + (int32_t)offsetof(DiamondJitLoop, index)));
    emit_u8(buf, 1);
    /* jmp loop_top */
    emit_u8(buf, 0xE9);
    emit_u32_le(buf, (uint32_t)((int32_t)loop_top - (int32_t)(buf->length + 4)));

    patch_rel32_to_here(buf, finished);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_lea_rsp(buf, REG_RSI, L);
    emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_loop_end);
    emit_add_rsp_imm32(buf, DIAMOND_JIT_LOOP_BYTES);
    const size_t done = emit_jmp_placeholder(buf);

    /* The two other ways out release the loop state too; rsp_delta is put
     * back by hand, since the code above already counted the release once. */
    patch_rel32_to_here(buf, failed);
    buf->rsp_delta += DIAMOND_JIT_LOOP_BYTES;
    emit_add_rsp_imm32(buf, DIAMOND_JIT_LOOP_BYTES);
    emit_bail_if_al_nonzero(jc);

    patch_rel32_to_here(buf, declined);
    buf->rsp_delta += DIAMOND_JIT_LOOP_BYTES;
    emit_add_rsp_imm32(buf, DIAMOND_JIT_LOOP_BYTES);
    compile_invoke_any_native(jc, instruction_start, dest, recv, base, argc, site_type);
    patch_rel32_to_here(buf, done);
}

/* Whether an INVOKE could be an Array each/map with a block (compile_block_loop). */
static bool site_may_be_block_loop(const DiamondFunction *fn, DiamondOpCode opcode, uint16_t name,
        uint8_t argc, uint16_t site_type) {
    if ((opcode != DIAMOND_OP_INVOKE && opcode != DIAMOND_OP_INVOKE_MONO) || argc != 1 ||
        name >= fn->string_count)
        return false;
    if (site_type != DIAMOND_TYPE_NONE && site_type != DIAMOND_TYPE_ARRAY) return false;
    const DiamondStringConstant *text = &fn->strings[name];
    return (text->length == 4 && memcmp(text->chars, "each", 4) == 0) ||
           (text->length == 3 && memcmp(text->chars, "map", 3) == 0);
}

static void compile_invoke(JitCompiler *jc, size_t instruction_start, DiamondOpCode opcode,
        uint16_t dest, uint16_t recv, uint16_t name, uint16_t base, uint8_t argc) {
    const DiamondFunction *fn = jc->function;
    uint16_t site_type=DIAMOND_TYPE_NONE;
    for(size_t site=0;site<fn->invoke_site_known_class_count;site++)
        if(fn->invoke_site_known_class[site].offset==
                (uint32_t)instruction_start) {
            site_type=fn->invoke_site_known_class[site].known_type;
            break;
        }
    if(opcode==DIAMOND_OP_INVOKE&&name<fn->string_count&&
       (site_type==DIAMOND_TYPE_STRING||site_type==DIAMOND_TYPE_ARRAY||
        site_type==DIAMOND_TYPE_HASH)) {
        const DiamondStringConstant *native_name=&fn->strings[name];
        DiamondJitNativeReadOp operation=DIAMOND_JIT_NATIVE_ARRAY_LENGTH;
        const bool supported=jit_native_read_operation(site_type,native_name,argc,&operation);
        if(supported) {
            compile_native_call(jc,dest,recv,base,operation);
            return;
        }
    }
    if (recv == 0 && jc->function->owner_class != DIAMOND_OWNER_NONE &&
        jc->function->owner_class != DIAMOND_OWNER_MODULE &&
        jc->function->owner_class != DIAMOND_OWNER_CAPTURED_SELF) {
        compile_invoke_dispatch_native(jc, instruction_start, dest, 0, name, base, argc,
                             opcode == DIAMOND_OP_INVOKE_MONO);
        return;
    }
    /* Phase 9: recv statically proven to always be an Instance
     * of a single concrete class -- see parameter_is_single_
     * class/parameter_never_reassigned's own comments. No
     * explicit recv!=0 guard here: for a genuinely self-less
     * function (owner_class==DIAMOND_OWNER_NONE, self_offset==0 inside
     * parameter_is_single_class), register 0 legitimately IS
     * the first declared parameter's own home register, not a
     * self slot -- parameter_is_single_class's own self_offset
     * arithmetic (and its recv_register<self_offset bounds
     * check) already correctly rejects recv==0 for every
     * owner_class where register 0 actually means self
     * instead, so gating on recv!=0 here would only wrongly
     * exclude that legitimate self_offset==0/recv==0 case, not
     * add any real safety. */
    if (parameter_is_single_class(jc->function, recv) &&
        parameter_never_reassigned(jc->function, recv)) {
        compile_invoke_dispatch_native(jc, instruction_start, dest, recv, name, base, argc,
                             opcode == DIAMOND_OP_INVOKE_MONO);
        return;
    }
    /* Phase 10: recv statically proven to always hold the
     * result of one specific DIAMOND_OP_NEW -- see register_
     * new_class_if_sole_writer's own comment. Doesn't matter
     * *which* class for eligibility purposes (only that it's
     * exactly one NEW and nothing else ever writes this
     * register) -- diamond_jit_invoke_instance dispatches by
     * the runtime instance's own class regardless, same as
     * Phase 9's own reasoning. */
    if (register_new_class_if_sole_writer(jc->function, recv) >= 0) {
        compile_invoke_dispatch_native(jc, instruction_start, dest, recv, name, base, argc,
                             opcode == DIAMOND_OP_INVOKE_MONO);
        return;
    }
    /* Phase 15: this exact call site's own receiver, proven a
     * single concrete class by the compiler's real known_types
     * tracking at the moment it emitted this instruction --
     * unlike the three proofs above (all position-insensitive:
     * "true everywhere in the function, or not at all"), this
     * is position-sensitive, the only way to cover a receiver
     * narrowed to *different* classes at different call sites
     * (`if x is A; x.a(); elsif x is B; x.b(); end`). See
     * DiamondFunction.invoke_site_known_class's own comment
     * for the redefine_method_used_anywhere gate's reasoning. */
    if (!fn->redefine_method_used_anywhere) {
        if(site_type>=DIAMOND_TYPE_CLASS_BASE&&
           site_type<DIAMOND_TYPE_NATIVE) {
            compile_invoke_dispatch_native(jc, instruction_start, dest, recv, name, base, argc,
                                 opcode == DIAMOND_OP_INVOKE_MONO);
            return;
        }
    }
    if (name >= fn->string_count) { jc->bailed = true; return; }
    if (site_may_be_block_loop(jc->function, opcode, name, argc, site_type))
        compile_block_loop(jc, instruction_start, dest, recv, base, argc, site_type);
    else
        compile_invoke_any_native(jc, instruction_start, dest, recv, base, argc, site_type);
}

/* JUMP_IF_TRUE: jump to target unless the condition is nil or false. */
static void compile_jump_if_true(JitCompiler *jc, size_t instruction_start,
        uint16_t condition, size_t target) {
    if (target <= instruction_start) emit_interrupt_poll(jc);
    JitBuffer *buf = &jc->buf;
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(condition, KIND_OFF));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_NIL);
    size_t nil_is_falsy = emit_jcc_placeholder(buf, JCC_E); /* nil -> fall through */
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_BOOL);
    size_t not_bool_is_truthy = emit_jcc_placeholder(buf, JCC_NE); /* not nil, not bool -> jump */
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(condition, AS_OFF));
    emit_test_r8(buf, REG_RAX);
    size_t bool_false_is_falsy = emit_jcc_placeholder(buf, JCC_E); /* bool false -> fall through */
    /* falls through here only when kind==BOOL && boolean==true */
    patch_rel32_to_here(buf, not_bool_is_truthy);
    emit_u8(buf, 0xE9);
    size_t target_field = jc->buf.length;
    emit_u32_le(buf, 0);
    record_global_patch(jc, target_field, target);
    patch_rel32_to_here(buf, nil_is_falsy);
    patch_rel32_to_here(buf, bool_false_is_falsy);
}

/* JUMP_IF_FALSE: jump to target when the condition is nil or false. */
static void compile_jump_if_false(JitCompiler *jc, size_t instruction_start,
        uint16_t condition, size_t target) {
    if (target <= instruction_start) emit_interrupt_poll(jc);
    JitBuffer *buf = &jc->buf;
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(condition, KIND_OFF));
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_NIL);
    emit_u8(buf, 0x0F);
    emit_u8(buf, JCC_E);
    size_t nil_field = jc->buf.length;
    emit_u32_le(buf, 0);
    record_global_patch(jc, nil_field, target);
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_BOOL);
    size_t not_bool = emit_jcc_placeholder(buf, JCC_NE);
    emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(condition, AS_OFF));
    emit_test_r8(buf, REG_RAX);
    size_t bool_true = emit_jcc_placeholder(buf, JCC_NE);
    /* falls through here only when kind==BOOL && boolean==false */
    emit_u8(buf, 0xE9);
    size_t target_field = jc->buf.length;
    emit_u32_le(buf, 0);
    record_global_patch(jc, target_field, target);
    patch_rel32_to_here(buf, not_bool);
    patch_rel32_to_here(buf, bool_true);
}

/* A function's own tail call becomes a loop: the new
 * arguments replace the registers and control returns to
 * the first instruction (whose CHECK_TYPEs re-run, as in
 * the interpreter). The argument count register is reset
 * to the call's own, so a default left out of the tail call
 * is taken again. A generic or variadic function's own tail call is staged
 * through compile_tail_call_other instead, to bind its type variables or
 * collect its rest afresh. A retry after an iteration re-runs from the
 * call's original arguments, which is safe for the same
 * reason every other retry is: effects set has_called. */
static void compile_self_tail_call(JitCompiler *jc, uint16_t base, uint8_t argc) {
    const DiamondFunction *fn = jc->function;
    if (argc < fn->required_arity || argc > fn->arity || base < argc) {
        jc->bailed = true; return;
    }
    emit_interrupt_poll(jc);
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_REGISTERS_BASE);
    emit_mov_imm64(buf, REG_RSI, base);
    emit_mov_imm64(buf, REG_RDX, argc);
    emit_mov_imm64(buf, REG_RCX, fn->register_count == 0
        ? DIAMOND_JIT_MAX_REGISTERS : fn->register_count);
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_tail_reset);
    /* The iteration's own argument count: ARGUMENT_PROVIDED
     * reads it to tell a defaulted parameter from a supplied
     * one, and a tail call may leave defaults out. */
    emit_mov_imm64(buf, JIT_ARGUMENT_COUNT, argc);
    size_t field = emit_jmp_placeholder(buf);
    record_global_patch(jc, field, 0);
}

/* CLOSURE of function `index` in a function a block's break lands in: a
 * block with a break names the call it is passed to (block_call_offset, a
 * later instruction of this function), whose next instruction compile_body
 * then makes a resume target. False, with jc->bailed set, for a block
 * outside the table or too many landing calls. */
static bool record_landing_call(JitCompiler *jc, size_t index) {
    if (jc->functions == nullptr || index >= jc->function_count ||
        jc->functions[index] == nullptr) {
        jc->bailed = true;
        return false;
    }
    const size_t call = jc->functions[index]->block_call_offset;
    if (call == 0) return true;   /* no break in this block */
    if (call >= jc->function->code_count) { jc->bailed = true; return false; }
    for (size_t i = 0; i < jc->landing_call_count; i++)
        if (jc->landing_calls[i] == call) return true;
    if (jc->landing_call_count == 16) { jc->bailed = true; return false; }
    jc->landing_calls[jc->landing_call_count++] = call;
    return true;
}

/* Opcode group: loads, moves, arithmetic, comparisons, ivars, type checks and collections. Returns false, emitting nothing, for an opcode outside the group; otherwise
 * advances *pc_ref past the operands, or sets jc->bailed. Part of compile_body. */
static bool compile_value_instruction(JitCompiler *jc, DiamondOpCode opcode, size_t instruction_start,
        size_t *pc_ref) {
    const DiamondFunction *fn = jc->function;
    size_t pc = *pc_ref;
    switch (opcode) {
        case DIAMOND_OP_NOT: {
            /* true for nil and false, false for anything else (is_truthy).
             * The source is read in full before dest is written, so the two
             * may be the same register. */
            uint16_t dest = 0, src = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &src)) {
                jc->bailed = true; return true;
            }
            JitBuffer *buf = &jc->buf;
            emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(src, KIND_OFF));
            emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_NIL);
            const size_t is_nil = emit_jcc_placeholder(buf, JCC_E);
            emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_VALUE_BOOL);
            const size_t not_bool = emit_jcc_placeholder(buf, JCC_NE);
            emit_load_byte_zx(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(src, AS_OFF));
            emit_test_r8(buf, REG_RAX);
            emit_sete_al(buf);
            const size_t bool_done = emit_jmp_placeholder(buf);
            patch_rel32_to_here(buf, not_bool);
            emit_mov_imm64(buf, REG_RAX, 0);
            const size_t truthy_done = emit_jmp_placeholder(buf);
            patch_rel32_to_here(buf, is_nil);
            emit_mov_imm64(buf, REG_RAX, 1);
            patch_rel32_to_here(buf, bool_done);
            patch_rel32_to_here(buf, truthy_done);
            emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
            emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
            break;
        }
        case DIAMOND_OP_LOAD_CLASS: {
            /* A Class value, after run_chunk's own bounds check of the index
             * against the chunk the function runs in. */
            uint16_t dest = 0, class_index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &class_index)) {
                jc->bailed = true; return true;
            }
            JitBuffer *buf = &jc->buf;
            emit_load_r64(buf, REG_RAX, JIT_CHUNK, (int32_t)offsetof(DiamondChunk, class_count));
            emit_cmp_imm32_64(buf, REG_RAX, class_index);
            const size_t in_range = emit_jcc_placeholder(buf, JCC_A);
            emit_store_instruction_offset(jc);
            emit_mov_al_imm8(buf, (uint8_t)DIAMOND_VM_INVALID_BYTECODE);
            record_global_patch(jc, emit_jmp_placeholder(buf), bailout_target(jc));
            patch_rel32_to_here(buf, in_range);
            emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_CLASS);
            emit_mov_imm64(buf, REG_RAX, class_index);
            emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
            break;
        }
        case DIAMOND_OP_NIL: {
            uint16_t dest = 0;
            if (!decode_u16(fn, &pc, &dest)) { jc->bailed = true; return true; }
            /* DIAMOND_NIL is kind=0 with an all-zero union -- registers
             * arrive pre-zeroed, so this is a real store, not a no-op,
             * only because a register can be reassigned to something
             * else and then back to nil within one function body (a
             * while loop's own always-nil result register). */
            emit_store_kind_imm(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_NIL);
            emit_mov_imm64(&jc->buf, REG_RAX, 0);
            emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
            break;
        }
        case DIAMOND_OP_BOOL: {
            uint16_t dest = 0, boolean = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &boolean)) { jc->bailed = true; return true; }
            emit_store_kind_imm(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
            emit_mov_imm64(&jc->buf, REG_RAX, boolean != 0 ? 1 : 0);
            emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
            break;
        }
        case DIAMOND_OP_MOVE: {
            uint16_t dest = 0, src = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &src)) { jc->bailed = true; return true; }
            emit_load_r64(&jc->buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(src, 0));
            emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RAX);
            emit_load_r64(&jc->buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(src, 8));
            emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, 8), REG_RAX);
            break;
        }
        case DIAMOND_OP_CONSTANT: {
            uint16_t dest = 0, index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) { jc->bailed = true; return true; }
            if (index >= fn->constant_count) { jc->bailed = true; return true; }
            DiamondValue constant = fn->constants[index];
            /* Int and Float constants are plain 64-bit payloads, so
             * they're stored inline; any other kind keeps the function
             * in the interpreter. */
            if (constant.kind != DIAMOND_VALUE_INT && constant.kind != DIAMOND_VALUE_FLOAT) {
                jc->bailed = true; return true;
            }
            uint64_t payload = 0;
            if (constant.kind == DIAMOND_VALUE_INT) payload = (uint64_t)constant.as.integer;
            else memcpy(&payload, &constant.as.real, sizeof payload);
            emit_store_kind_imm(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), (uint8_t)constant.kind);
            emit_mov_imm64(&jc->buf, REG_RAX, payload);
            emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
            break;
        }
        case DIAMOND_OP_ADD_INT:
        case DIAMOND_OP_SUBTRACT_INT:
        case DIAMOND_OP_MULTIPLY_INT:
        case DIAMOND_OP_DIVIDE_INT:
        case DIAMOND_OP_LESS_INT:
        /* Phase 5: the generic (non-_INT) forms, and the three _INT
         * comparison forms that were never added even for the typed
         * case -- see compile_binary_int_op's own updated comment.
         * Every one of these opcodes shares the identical dest/left/
         * right decode shape (confirmed against src/vm.c's own
         * interpreter case for this opcode family). */
        case DIAMOND_OP_ADD:
        case DIAMOND_OP_SUBTRACT:
        case DIAMOND_OP_MULTIPLY:
        case DIAMOND_OP_DIVIDE:
        case DIAMOND_OP_MODULO:
        case DIAMOND_OP_LESS:
        case DIAMOND_OP_LESS_EQUAL:
        case DIAMOND_OP_LESS_EQUAL_INT:
        case DIAMOND_OP_GREATER:
        case DIAMOND_OP_GREATER_INT:
        case DIAMOND_OP_GREATER_EQUAL:
        case DIAMOND_OP_GREATER_EQUAL_INT: {
            /* Phase 3: no longer rejected once jc->has_called is
             * already true -- see compile_binary_int_op's own comment
             * for why their edge cases (overflow, division by zero,
             * INT64_MIN/-1, non-INT operand) no longer depend on
             * "discard and retry the whole function" at all. */
            uint16_t dest = 0, left = 0, right = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                !decode_u16(fn, &pc, &right)) { jc->bailed = true; return true; }
            compile_binary_int_op(jc, instruction_start, dest, left, right, opcode);
            break;
        }
        case DIAMOND_OP_EQUAL:
        case DIAMOND_OP_EQUAL_INT:
        case DIAMOND_OP_NOT_EQUAL:
        case DIAMOND_OP_NOT_EQUAL_INT: {
            uint16_t dest = 0, left = 0, right = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                !decode_u16(fn, &pc, &right)) { jc->bailed = true; return true; }
            compile_equal_op(jc, instruction_start, dest, left, right,
                opcode == DIAMOND_OP_NOT_EQUAL || opcode == DIAMOND_OP_NOT_EQUAL_INT);
            break;
        }
        /* The constant-operand forms: store the constant into `scratch`,
         * exactly as the CONSTANT they replace did, then compile the
         * register form against it. The generic opcode goes to the slow
         * paths, whose deopt rewrite only applies to the _INT forms -- it
         * would write a register-form opcode over this instruction. */
        case DIAMOND_OP_ADD_INT_K:
        case DIAMOND_OP_SUBTRACT_INT_K:
        case DIAMOND_OP_MULTIPLY_INT_K:
        case DIAMOND_OP_MODULO_INT_K:
        case DIAMOND_OP_LESS_INT_K:
        case DIAMOND_OP_LESS_EQUAL_INT_K:
        case DIAMOND_OP_GREATER_INT_K:
        case DIAMOND_OP_GREATER_EQUAL_INT_K:
        case DIAMOND_OP_EQUAL_INT_K:
        case DIAMOND_OP_NOT_EQUAL_INT_K: {
            uint16_t dest = 0, left = 0, scratch = 0, index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                !decode_u16(fn, &pc, &scratch) || !decode_u16(fn, &pc, &index)) {
                jc->bailed = true; return true;
            }
            if (index >= fn->constant_count || fn->constants[index].kind != DIAMOND_VALUE_INT) {
                jc->bailed = true; return true;
            }
            if (opcode == DIAMOND_OP_EQUAL_INT_K || opcode == DIAMOND_OP_NOT_EQUAL_INT_K) {
                /* An Int `left` compares against the immediate; any other
                 * kind fills the scratch register and runs the full ==. */
                JitBuffer *buf = &jc->buf;
                const int64_t constant = fn->constants[index].as.integer;
                const bool negate = opcode == DIAMOND_OP_NOT_EQUAL_INT_K;
                const size_t other = emit_check_kind_int_or_jump(jc, left, REG_RAX);
                emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
                if (constant >= INT32_MIN && constant <= INT32_MAX) {
                    emit_alu_imm32(buf, ALU_IMM_CMP, REG_RAX, (int32_t)constant);
                } else {
                    emit_mov_imm64(buf, REG_RCX, (uint64_t)constant);
                    emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
                }
                emit_sete_al(buf);
                if (negate) emit_xor_al_1(buf);
                emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
                emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
                const size_t done = emit_jmp_placeholder(buf);
                patch_rel32_to_here(buf, other);
                emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(scratch, KIND_OFF),
                    DIAMOND_VALUE_INT);
                emit_mov_imm64(buf, REG_RAX, (uint64_t)constant);
                emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(scratch, AS_OFF), REG_RAX);
                compile_equal_op(jc, instruction_start, dest, left, scratch, negate);
                patch_rel32_to_here(buf, done);
                break;
            }
            const DiamondOpCode generic =
                opcode == DIAMOND_OP_ADD_INT_K ? DIAMOND_OP_ADD :
                opcode == DIAMOND_OP_SUBTRACT_INT_K ? DIAMOND_OP_SUBTRACT :
                opcode == DIAMOND_OP_MULTIPLY_INT_K ? DIAMOND_OP_MULTIPLY :
                opcode == DIAMOND_OP_MODULO_INT_K ? DIAMOND_OP_MODULO :
                opcode == DIAMOND_OP_LESS_INT_K ? DIAMOND_OP_LESS :
                opcode == DIAMOND_OP_LESS_EQUAL_INT_K ? DIAMOND_OP_LESS_EQUAL :
                opcode == DIAMOND_OP_GREATER_INT_K ? DIAMOND_OP_GREATER :
                DIAMOND_OP_GREATER_EQUAL;
            compile_int_k_op(jc, instruction_start, dest, left, scratch,
                fn->constants[index].as.integer, generic);
            break;
        }
        case DIAMOND_OP_SET_IVAR: {
            uint16_t recv = 0, field = 0, source = 0;
            if (!decode_u16(fn, &pc, &recv) || !decode_u16(fn, &pc, &field) ||
                !decode_u16(fn, &pc, &source)) { jc->bailed = true; return true; }
            if (field > UINT8_MAX) { jc->bailed = true; return true; }
            compile_set_ivar_inline(jc, instruction_start, recv, field, source);
            break;
        }
        case DIAMOND_OP_GET_IVAR: {
            uint16_t dest = 0, recv = 0, field = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                !decode_u16(fn, &pc, &field)) { jc->bailed = true; return true; }
            if (field > UINT8_MAX) { jc->bailed = true; return true; }
            compile_get_ivar_inline(jc, instruction_start, dest, recv, field);
            break;
        }
        case DIAMOND_OP_COLLECT_VARIADIC: {
            uint16_t dest = 0, fixed = 0, preserved = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &fixed) ||
                !decode_u16(fn, &pc, &preserved)) { jc->bailed = true; return true; }
            if (preserved > 1 || (size_t)dest + preserved >= fn->register_count) {
                jc->bailed = true; return true;
            }
            if (!variadic_arguments_untouched(fn, instruction_start, fixed)) {
                jc->bailed = true; return true;
            }
            /* The arguments are all in the registers: the prologue sent a
             * call with more arguments than registers back to the
             * interpreter (emit_variadic_guard). Allocates, so it needs the
             * frame. */
            jc->needs_frame = true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
            emit_mov_rr(buf, REG_RDX, JIT_ARGUMENT_COUNT);
            emit_mov_imm64(buf, REG_RCX, dest);
            emit_mov_imm64(buf, REG_R8, (uint64_t)fixed | ((uint64_t)preserved << 16));
            emit_mov_imm64(buf, REG_R9, fn->register_count);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_collect_variadic);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_ARGUMENT_PROVIDED: {
            uint16_t dest = 0, index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) { jc->bailed = true; return true; }
            /* Known simplification: doesn't check for a
             * DIAMOND_VALUE_UNDEFINED sparse-keyword-call gap -- see
             * DiamondJitFn's own comment in jit.h for why that's fine
             * at every call site this JIT is wired into today. */
            JitBuffer *buf = &jc->buf;
            emit_cmp_imm32_64(buf, JIT_ARGUMENT_COUNT, index);
            emit_seta_al(buf);
            emit_movzx_r64_al(buf, REG_RAX);
            emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
            emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
            break;
        }
        case DIAMOND_OP_CHECK_TYPE: {
            uint16_t source = 0, set_index = 0;
            if (!decode_u16(fn, &pc, &source) || !decode_u16(fn, &pc, &set_index)) { jc->bailed = true; return true; }
            compile_check_type(jc, source, set_index);
            break;
        }
        case DIAMOND_OP_BOX_LOCAL: {
            /* Allocates the cell, so the frame keeps registers visible to
             * the GC; no user code runs. */
            uint16_t reg = 0;
            if (!decode_u16(fn, &pc, &reg)) { jc->bailed = true; return true; }
            jc->needs_frame = true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RDX, reg);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_box_local);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_GET_CELL: {
            uint16_t dest = 0, cell = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &cell)) {
                jc->bailed = true; return true;
            }
            JitBuffer *buf = &jc->buf;
            size_t fail[2]; size_t fail_count = 0;
            emit_cell_guard(buf, JIT_REGISTERS_BASE, reg_disp(cell, 0), fail, &fail_count);
            emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RDX,
                (int32_t)offsetof(DiamondCell, value));
            const size_t done = emit_jmp_placeholder(buf);
            for (size_t i = 0; i < fail_count; i++) patch_rel32_to_here(buf, fail[i]);
            emit_mov_rr(buf, REG_RDI, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RSI, dest);
            emit_mov_imm64(buf, REG_RDX, cell);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_get_cell);
            emit_bail_if_al_nonzero(jc);
            patch_rel32_to_here(buf, done);
            break;
        }
        case DIAMOND_OP_SET_CELL: {
            uint16_t cell = 0, source = 0;
            if (!decode_u16(fn, &pc, &cell) || !decode_u16(fn, &pc, &source)) {
                jc->bailed = true; return true;
            }
            /* A shared cell is observable outside this activation. Once
             * stored, a later failure must propagate/catch, never replay. */
            jc->has_called = true;
            JitBuffer *buf = &jc->buf;
            size_t fail[3]; size_t fail_count = 0;
            emit_cell_guard(buf, JIT_REGISTERS_BASE, reg_disp(cell, 0), fail, &fail_count);
            emit_copy_value(buf, REG_RDX, (int32_t)offsetof(DiamondCell, value),
                JIT_REGISTERS_BASE, reg_disp(source, 0));
            emit_barrier_guard(buf, fail, &fail_count);
            const size_t done = emit_jmp_placeholder(buf);
            for (size_t i = 0; i < fail_count; i++) patch_rel32_to_here(buf, fail[i]);
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RDX, cell);
            emit_mov_imm64(buf, REG_RCX, source);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_set_cell);
            emit_bail_if_al_nonzero(jc);
            patch_rel32_to_here(buf, done);
            break;
        }
        case DIAMOND_OP_GET_CAPTURE:
        case DIAMOND_OP_GET_CAPTURE_CELL: {
            /* A block body reading a captured local (or its cell, to pass
             * on to a nested block) through the closure it was entered
             * with. Neither allocates nor runs user code. */
            uint16_t dest = 0, index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) {
                jc->bailed = true; return true;
            }
            jc->reads_closure = true;
            JitBuffer *buf = &jc->buf;
            size_t fail[4]; size_t fail_count = 0;
            emit_capture_guard(jc, index, fail, &fail_count);
            emit_cell_guard(buf, REG_RAX, capture_disp(index), fail, &fail_count);
            if (opcode == DIAMOND_OP_GET_CAPTURE_CELL)
                emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RAX,
                    capture_disp(index));
            else
                emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RDX,
                    (int32_t)offsetof(DiamondCell, value));
            const size_t done = emit_jmp_placeholder(buf);
            for (size_t i = 0; i < fail_count; i++) patch_rel32_to_here(buf, fail[i]);
            emit_load_closure(jc, REG_RDI);
            emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RDX, dest);
            emit_mov_imm64(buf, REG_RCX, index);
            emit_mov_imm64(buf, REG_R8, opcode == DIAMOND_OP_GET_CAPTURE_CELL ? 1 : 0);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_get_capture);
            emit_bail_if_al_nonzero(jc);
            patch_rel32_to_here(buf, done);
            break;
        }
        case DIAMOND_OP_GET_NAMESPACE_CONSTANT: {
            uint16_t dest = 0, index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) {
                jc->bailed = true; return true;
            }
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RDX, dest);
            emit_mov_imm64(buf, REG_RCX, index);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_get_namespace_constant);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_SET_CAPTURE: {
            uint16_t index = 0, source = 0;
            if (!decode_u16(fn, &pc, &index) || !decode_u16(fn, &pc, &source)) {
                jc->bailed = true; return true;
            }
            jc->reads_closure = true;
            jc->has_called = true; /* Captured state cannot be replayed. */
            JitBuffer *buf = &jc->buf;
            size_t fail[5]; size_t fail_count = 0;
            emit_capture_guard(jc, index, fail, &fail_count);
            emit_cell_guard(buf, REG_RAX, capture_disp(index), fail, &fail_count);
            emit_copy_value(buf, REG_RDX, (int32_t)offsetof(DiamondCell, value),
                JIT_REGISTERS_BASE, reg_disp(source, 0));
            emit_barrier_guard(buf, fail, &fail_count);
            const size_t done = emit_jmp_placeholder(buf);
            for (size_t i = 0; i < fail_count; i++) patch_rel32_to_here(buf, fail[i]);
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_load_closure(jc, REG_RSI);
            emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RCX, index);
            emit_mov_imm64(buf, REG_R8, source);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_set_capture);
            emit_bail_if_al_nonzero(jc);
            patch_rel32_to_here(buf, done);
            break;
        }
        case DIAMOND_OP_TO_STRING: {
            /* A String is its own text: copied inline. Anything else goes
             * through diamond_jit_to_string, which can call a to_s method. */
            uint16_t dest = 0, source = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &source)) {
                jc->bailed = true; return true;
            }
            jc->needs_frame = true;
            jc->has_called = true;
            JitBuffer *buf = &jc->buf;
            size_t fail[2]; size_t fail_count = 0;
            emit_object_kind_guard(buf, JIT_REGISTERS_BASE, reg_disp(source, 0),
                DIAMOND_OBJECT_STRING, fail, &fail_count);
            emit_copy_value(buf, JIT_REGISTERS_BASE, reg_disp(dest, 0),
                JIT_REGISTERS_BASE, reg_disp(source, 0));
            const size_t done = emit_jmp_placeholder(buf);
            for (size_t i = 0; i < fail_count; i++) patch_rel32_to_here(buf, fail[i]);
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
            emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RCX, dest);
            emit_mov_imm64(buf, REG_R8, source);
            emit_mov_rr(buf, REG_R9, JIT_DEPTH);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_to_string);
            emit_bail_if_al_nonzero(jc);
            patch_rel32_to_here(buf, done);
            break;
        }
        case DIAMOND_OP_CALL_CLOSURE: {
            /* run_chunk's whole closure call (call_closure_value), which
             * enters a compiled block's native code itself. */
            uint16_t dest = 0, callable = 0, base = 0; uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &callable) ||
                !decode_u16(fn, &pc, &base) || !decode_u8(fn, &pc, &argc)) {
                jc->bailed = true; return true;
            }
            jc->needs_frame = true;
            jc->has_called = true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
            emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RCX,
                (uint64_t)(uintptr_t)(jc->function->code + instruction_start));
            emit_mov_rr(buf, REG_R8, JIT_DEPTH);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_call_closure);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_CLOSURE: {
            /* diamond_jit_closure reads the capture list from the
             * instruction; here it is only skipped. It allocates and reads
             * the frame's serial, so the function always gets a frame. */
            uint16_t dest = 0, index = 0; uint8_t count = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index) ||
                !decode_u8(fn, &pc, &count) || pc + 2 * (size_t)count > fn->code_count) {
                jc->bailed = true; return true;
            }
            pc += 2 * (size_t)count;
            jc->needs_frame = true;
            if (fn->nonlocal_landing && !record_landing_call(jc, index)) return true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
            emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
            emit_mov_imm64(buf, REG_RCX,
                (uint64_t)(uintptr_t)(fn->code + instruction_start));
            jc->reads_closure = true;
            emit_load_closure(jc, REG_R8);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_closure);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_MATH_UNARY:
        case DIAMOND_OP_MATH_BINARY: {
            /* Math.sqrt/sin/.../pow: diamond_jit_math, which neither
             * allocates nor runs user code, so no frame and no has_called;
             * its only failure (a non-numeric operand) is a plain status. */
            uint16_t dest = 0, left = 0, right = 0; uint8_t function_id = 0;
            const bool binary = opcode == DIAMOND_OP_MATH_BINARY;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                (binary && !decode_u16(fn, &pc, &right)) ||
                !decode_u8(fn, &pc, &function_id)) { jc->bailed = true; return true; }
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_lea(buf, REG_RSI, JIT_REGISTERS_BASE, reg_disp(left, 0));
            if (binary) emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(right, 0));
            else emit_mov_imm64(buf, REG_RDX, 0);
            emit_mov_imm64(buf, REG_RCX, function_id);
            emit_lea(buf, REG_R8, JIT_REGISTERS_BASE, reg_disp(dest, 0));
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_math);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_IS_TYPE: {
            uint16_t dest = 0, source = 0, type = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &source) ||
                !decode_u16(fn, &pc, &type)) { jc->bailed = true; return true; }
            if (diamond_type_operand_is_native(type)) { jc->bailed = true; return true; }
            compile_is_type(jc, dest, source, type);
            break;
        }
        case DIAMOND_OP_SAME_CLASS: {
            uint16_t dest = 0, left = 0, right = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                !decode_u16(fn, &pc, &right)) { jc->bailed = true; return true; }
            JitBuffer *buf = &jc->buf;
            emit_lea(buf, REG_RDI, JIT_REGISTERS_BASE, reg_disp(left, 0));
            emit_lea(buf, REG_RSI, JIT_REGISTERS_BASE, reg_disp(right, 0));
            emit_lea(buf, REG_RDX, JIT_REGISTERS_BASE, reg_disp(dest, 0));
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_same_class);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_STRING: {
            uint16_t dest = 0, string_index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &string_index)) { jc->bailed = true; return true; }
            compile_new_string(jc, dest, string_index);
            break;
        }
        case DIAMOND_OP_INDEX_GET: {
            uint16_t dest = 0, recv = 0, index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                !decode_u16(fn, &pc, &index)) { jc->bailed = true; return true; }
            compile_index_get(jc, instruction_start, dest, recv, index);
            break;
        }
        case DIAMOND_OP_INDEX_GET_STRING: {
            uint16_t dest = 0, recv = 0, string_index = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                !decode_u16(fn, &pc, &string_index)) { jc->bailed = true; return true; }
            compile_index_get_string(jc, instruction_start, dest, recv, string_index);
            break;
        }
        case DIAMOND_OP_INDEX_SET: {
            uint16_t recv = 0, index = 0, source = 0;
            if (!decode_u16(fn, &pc, &recv) || !decode_u16(fn, &pc, &index) ||
                !decode_u16(fn, &pc, &source)) { jc->bailed = true; return true; }
            compile_index_set(jc, instruction_start, recv, index, source);
            break;
        }
        case DIAMOND_OP_ARRAY: {
            uint16_t dest = 0, base = 0, count = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &base) ||
                !decode_u16(fn, &pc, &count)) { jc->bailed = true; return true; }
            compile_new_array(jc, dest, base, count);
            break;
        }
        case DIAMOND_OP_HASH: {
            uint16_t dest = 0, base = 0, count = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &base) ||
                !decode_u16(fn, &pc, &count)) { jc->bailed = true; return true; }
            compile_new_hash(jc, dest, base, count);
            break;
        }
        default:
            return false;
    }
    *pc_ref = pc;
    return true;
}

/* Opcode group: calls, jumps and returns. Returns false, emitting nothing, for an opcode outside the group; otherwise
 * advances *pc_ref past the operands, or sets jc->bailed. Part of compile_body. */
/* Marks the function as having handlers and records `target` as a place a
 * handler can continue at (see emit_catch_stubs). False, with jc->bailed
 * set, for a target outside the code or too many distinct targets. */
static bool add_resume_entry(JitCompiler *jc, size_t key, size_t target) {
    if (target >= jc->function->code_count) { jc->bailed = true; return false; }
    jc->has_handlers = true;
    jc->needs_frame = true;
    for (size_t i = 0; i < jc->resume_target_count; i++)
        if (jc->resume_keys[i] == key) return true;
    if (jc->resume_target_count == 64) { jc->bailed = true; return false; }
    jc->resume_keys[jc->resume_target_count] = key;
    jc->resume_targets[jc->resume_target_count++] = target;
    return true;
}
static bool add_resume_target(JitCompiler *jc, size_t target) {
    return add_resume_entry(jc, target, target);
}

/* After a handler helper whose 0 means "continue at state->resume": any
 * other status propagates (the helper has already offered it to the
 * handlers). With `may_return`, DIAMOND_JIT_HANDLER_RETURNED returns from
 * the function, its result already written. */
static void emit_handler_outcome(JitCompiler *jc, bool may_return) {
    JitBuffer *buf = &jc->buf;
    size_t returned = 0;
    if (may_return) {
        emit_u8(buf, 0x0F); emit_u8(buf, 0xB6); emit_u8(buf, 0xC0);   /* movzx eax, al */
        emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_JIT_HANDLER_RETURNED);
        returned = emit_jcc_placeholder(buf, JCC_E);
    }
    emit_test_r8(buf, REG_RAX);
    record_global_patch(jc, emit_jcc_placeholder(buf, JCC_NE), BAILOUT_PROPAGATE_SENTINEL);
    record_global_patch(jc, emit_jmp_placeholder(buf), BAILOUT_RESUME_SENTINEL);
    if (may_return) {
        patch_rel32_to_here(buf, returned);
        emit_epilogue(jc);
        emit_mov_al_imm8(buf, DIAMOND_VM_OK);
        emit_ret(buf);
    }
}

static bool compile_control_instruction(JitCompiler *jc, DiamondOpCode opcode, size_t instruction_start,
        size_t *pc_ref) {
    const DiamondFunction *fn = jc->function;
    size_t pc = *pc_ref;
    switch (opcode) {
        case DIAMOND_OP_PUSH_RESCUE: {
            /* diamond_jit_push_rescue re-reads the operands from the
             * instruction; here they are only checked and the clause's
             * target recorded for the resume stub. */
            uint16_t dest = 0; uint8_t type_count = 0, skipped = 0, high = 0, low = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u8(fn, &pc, &type_count)) {
                jc->bailed = true; return true;
            }
            for (size_t i = 0; i < 8; i++)
                if (!decode_u8(fn, &pc, &skipped)) { jc->bailed = true; return true; }
            if (!decode_u8(fn, &pc, &high) || !decode_u8(fn, &pc, &low)) {
                jc->bailed = true; return true;
            }
            if (!add_resume_target(jc, ((size_t)high << 8) | low)) return true;
            JitBuffer *buf = &jc->buf;
            emit_handler_state_address(jc, REG_RDI);
            emit_mov_imm64(buf, REG_RSI, (uint64_t)(uintptr_t)(fn->code + instruction_start));
            emit_mov_imm64(buf, REG_RDX, fn->code_count);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_push_rescue);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_POP_RESCUE: {
            jc->has_handlers = true;
            jc->needs_frame = true;
            emit_handler_state_address(jc, REG_RDI);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_pop_rescue);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_PUSH_ENSURE: {
            uint8_t high = 0, low = 0;
            if (!decode_u8(fn, &pc, &high) || !decode_u8(fn, &pc, &low)) {
                jc->bailed = true; return true;
            }
            const size_t target = ((size_t)high << 8) | low;
            if (!add_resume_target(jc, target)) return true;
            JitBuffer *buf = &jc->buf;
            emit_handler_state_address(jc, REG_RDI);
            emit_mov_imm64(buf, REG_RSI, target);
            emit_mov_imm64(buf, REG_RDX, fn->code_count);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_push_ensure);
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_RUN_ENSURE: {
            /* Enters the innermost ensure clause; its END_ENSURE continues
             * at `continuation`. */
            uint8_t high = 0, low = 0;
            if (!decode_u8(fn, &pc, &high) || !decode_u8(fn, &pc, &low)) {
                jc->bailed = true; return true;
            }
            const size_t continuation = ((size_t)high << 8) | low;
            if (!add_resume_target(jc, continuation)) return true;
            JitBuffer *buf = &jc->buf;
            emit_handler_state_address(jc, REG_RDI);
            emit_mov_imm64(buf, REG_RSI, continuation);
            emit_mov_imm64(buf, REG_RDX, fn->code_count);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_run_ensure);
            emit_handler_outcome(jc, false);
            break;
        }
        case DIAMOND_OP_UNWIND_JUMP: {
            /* break/next leaving `levels` begin blocks: each ensure clause
             * on the way runs first. */
            uint8_t levels = 0, high = 0, low = 0;
            if (!decode_u8(fn, &pc, &levels) || !decode_u8(fn, &pc, &high) ||
                !decode_u8(fn, &pc, &low)) {
                jc->bailed = true; return true;
            }
            const size_t target = ((size_t)high << 8) | low;
            if (!add_resume_target(jc, target)) return true;
            JitBuffer *buf = &jc->buf;
            emit_handler_state_address(jc, REG_RDI);
            emit_mov_imm64(buf, REG_RSI, levels);
            emit_mov_imm64(buf, REG_RDX, target);
            emit_mov_imm64(buf, REG_RCX, fn->code_count);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_unwind_jump);
            emit_handler_outcome(jc, false);
            break;
        }
        case DIAMOND_OP_END_ENSURE: {
            jc->has_handlers = true;
            jc->needs_frame = true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
            emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
            emit_handler_state_address(jc, REG_RCX);
            emit_mov_rr(buf, REG_R8, JIT_RESULT_PTR);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_end_ensure);
            emit_handler_outcome(jc, true);
            break;
        }
        case DIAMOND_OP_BLOCK_BREAK:
        case DIAMOND_OP_BLOCK_RETURN: {
            /* diamond_jit_block_exit checks the target frame is still live and
             * starts the non-local exit, which leaves as any failing status
             * does: through this block's ensure clauses (the catch stub) and
             * out. Never a retry, which would run the block's work again. */
            uint16_t source = 0;
            if (!decode_u16(fn, &pc, &source)) { jc->bailed = true; return true; }
            jc->needs_frame = true;
            jc->has_called = true;
            jc->reads_closure = true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_lea(buf, REG_RSI, JIT_REGISTERS_BASE, reg_disp(source, 0));
            emit_load_closure(jc, REG_RDX);
            emit_mov_imm64(buf, REG_RCX, opcode == DIAMOND_OP_BLOCK_BREAK);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_block_exit);
            /* Always nonzero; the jump keeps the stub choice in one place. */
            emit_bail_if_al_nonzero(jc);
            break;
        }
        case DIAMOND_OP_RAISE: {
            /* diamond_jit_raise records the backtrace (so the frame is
             * linked) and, in a function with handlers, tries them: 0 means
             * a clause caught it. Anything else is final and propagates; a
             * retry would raise the same exception a second time. */
            uint16_t source = 0;
            if (!decode_u16(fn, &pc, &source)) { jc->bailed = true; return true; }
            jc->has_raise = true;
            jc->needs_frame = true;
            JitBuffer *buf = &jc->buf;
            emit_mov_rr(buf, REG_RDI, JIT_VM);
            emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
            emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
            if (jc->has_handlers) emit_handler_state_address(jc, REG_RCX);
            else emit_mov_imm64(buf, REG_RCX, 0);
            emit_mov_imm64(buf, REG_R8, source);
            emit_body_call(jc, (void *)(uintptr_t)diamond_jit_raise);
            emit_test_r8(buf, REG_RAX);
            const size_t raised = emit_jcc_placeholder(buf, JCC_NE);
            record_global_patch(jc, raised, BAILOUT_PROPAGATE_SENTINEL);
            if (jc->has_handlers) {
                const size_t caught = emit_jmp_placeholder(buf);
                record_global_patch(jc, caught, BAILOUT_RESUME_SENTINEL);
            }
            break;
        }
        case DIAMOND_OP_SUPER: {
            uint16_t dest = 0, name = 0, base = 0;
            uint16_t owner_index = 0; uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &owner_index) ||
                !decode_u16(fn, &pc, &name) || !decode_u16(fn, &pc, &base) ||
                !decode_u8(fn, &pc, &argc)) { jc->bailed = true; return true; }
            compile_super_call(jc, dest, owner_index, name, base, argc);
            break;
        }
        case DIAMOND_OP_TAIL_CALL: {
            uint16_t dest = 0, function_index = 0, base = 0;
            uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &function_index) ||
                !decode_u16(fn, &pc, &base) || !decode_u8(fn, &pc, &argc)) {
                jc->bailed = true; return true;
            }
            /* A tail call leaves the frame; with handlers it stays with the
             * interpreter. */
            if (jc->has_handlers) { jc->bailed = true; return true; }
            /* A generic function's own tail call infers its type variables again
             * from the new arguments, so it re-enters through
             * jit_run_native_generic rather than looping in place. */
            /* A variadic function's tail call to itself goes through the staging
             * loop, which takes only arguments that fit its registers and
             * buffer (diamond_jit_tail_call_function); a wider one stays with
             * the interpreter, which runs it in place. */
            if (fn->has_variadic && (size_t)function_index == jc->self_function_index &&
                (argc > fn->register_count || argc > DIAMOND_MAX_DECLARED_PARAMETERS)) {
                jc->bailed = true; return true;
            }
            if ((size_t)function_index != jc->self_function_index ||
                fn->type_variable_count > 0 || fn->has_variadic) {
                compile_tail_call_other(jc, dest, function_index, base, argc);
                break;
            }
            compile_self_tail_call(jc, base, argc);
            break;
        }
        case DIAMOND_OP_CALL: {
            uint16_t dest = 0, function_index = 0, base = 0;
            uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &function_index) ||
                !decode_u16(fn, &pc, &base) || !decode_u8(fn, &pc, &argc)) {
                jc->bailed = true; return true;
            }
            compile_call_function(jc, dest, function_index, base, argc);
            break;
        }
        case DIAMOND_OP_NEW: {
            uint16_t dest = 0, base = 0, class_index = 0;
            uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &class_index) ||
                !decode_u16(fn, &pc, &base) || !decode_u8(fn, &pc, &argc)) {
                jc->bailed = true; return true;
            }
            compile_new_native(jc, dest, class_index, base, argc);
            break;
        }
        case DIAMOND_OP_JUMP: {
            uint8_t high = 0, low = 0;
            if (!decode_u8(fn, &pc, &high) || !decode_u8(fn, &pc, &low)) { jc->bailed = true; return true; }
            size_t target = ((size_t)high << 8) | low;
            if (target <= instruction_start) emit_interrupt_poll(jc);
            size_t field = emit_jmp_placeholder(&jc->buf);
            record_global_patch(jc, field, target);
            break;
        }
        case DIAMOND_OP_JUMP_IF_TRUE: {
            uint16_t condition = 0;
            uint8_t high = 0, low = 0;
            if (!decode_u16(fn, &pc, &condition) || !decode_u8(fn, &pc, &high) ||
                !decode_u8(fn, &pc, &low)) { jc->bailed = true; return true; }
            size_t target = ((size_t)high << 8) | low;
            compile_jump_if_true(jc, instruction_start, condition, target);
            break;
        }
        case DIAMOND_OP_JUMP_IF_FALSE: {
            uint16_t condition = 0;
            uint8_t high = 0, low = 0;
            if (!decode_u16(fn, &pc, &condition) || !decode_u8(fn, &pc, &high) ||
                !decode_u8(fn, &pc, &low)) { jc->bailed = true; return true; }
            size_t target = ((size_t)high << 8) | low;
            compile_jump_if_false(jc, instruction_start, condition, target);
            break;
        }
        case DIAMOND_OP_RETURN: {
            uint16_t source = 0;
            if (!decode_u16(fn, &pc, &source)) { jc->bailed = true; return true; }
            JitBuffer *buf = &jc->buf;
            if (jc->has_handlers) {
                /* An enclosing ensure clause runs before the return. */
                emit_handler_state_address(jc, REG_RDI);
                emit_lea(buf, REG_RSI, JIT_REGISTERS_BASE, reg_disp(source, 0));
                emit_mov_rr(buf, REG_RDX, JIT_RESULT_PTR);
                emit_body_call(jc, (void *)(uintptr_t)diamond_jit_return);
                emit_handler_outcome(jc, true);
                break;
            }
            emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(source, 0));
            emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(source, 8));
            emit_store_r64(buf, JIT_RESULT_PTR, 0, REG_RAX);
            emit_store_r64(buf, JIT_RESULT_PTR, 8, REG_RCX);
            emit_epilogue(jc);
            emit_mov_al_imm8(buf, DIAMOND_VM_OK);
            emit_ret(buf);
            break;
        }
        case DIAMOND_OP_INVOKE:
        case DIAMOND_OP_INVOKE_MONO: {
            uint16_t dest = 0, recv = 0, name = 0, base = 0;
            uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                !decode_u16(fn, &pc, &name) || !decode_u16(fn, &pc, &base) ||
                !decode_u8(fn, &pc, &argc)) { jc->bailed = true; return true; }
            compile_invoke(jc, instruction_start, opcode, dest, recv, name, base, argc);
            break;
        }
        /* Explicit type arguments (`recv.method[Type]()`): the interpreter's
         * own dispatch (diamond_jit_invoke_typed), which reads them from the
         * instruction. */
        case DIAMOND_OP_INVOKE_TYPED: {
            uint16_t dest = 0, recv = 0, name = 0, base = 0;
            uint8_t argc = 0;
            if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                !decode_u16(fn, &pc, &name) || !decode_u16(fn, &pc, &base) ||
                !decode_u8(fn, &pc, &argc) || !skip_type_trailer(fn, &pc)) {
                jc->bailed = true; return true;
            }
            compile_invoke_helper(jc, instruction_start,
                (void *)(uintptr_t)diamond_jit_invoke_typed);
            break;
        }
        default:
            return false;
    }
    *pc_ref = pc;
    return true;
}

/* An opcode the interpreter runs out of line (opcode_runs_outlined): a call
 * to diamond_jit_outlined with the instruction's offset, after which the
 * body has written its dest itself. decode_instruction_operands steps over
 * the operands, the same decoder the register scans use, so the two cannot
 * disagree about the instruction's length. Returns false for any other
 * opcode. */
static bool compile_outlined_instruction(JitCompiler *jc, DiamondOpCode opcode,
        size_t instruction_start, size_t *pc_ref) {
    if (!opcode_runs_outlined(opcode)) return false;
    int32_t dest = -1, jump_target = -1, second_dest = -1;
    if (!decode_instruction_operands(jc->function, pc_ref, opcode, &dest, &jump_target,
            &second_dest)) {
        jc->bailed = true;
        return true;
    }
    jc->needs_frame = true;
    jc->has_called = true;
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_rr(buf, REG_RCX, JIT_DEPTH);
    emit_mov_imm64(buf, REG_R8, instruction_start);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_outlined);
    emit_bail_if_al_nonzero(jc);
    return true;
}

/* Returns false (jc->bailed set) the moment anything outside the supported
 * whitelist is found -- the caller must then discard the whole attempt. */
static void compile_body(JitCompiler *jc) {
    const DiamondFunction *fn = jc->function;
    size_t pc = 0;
    jc->buf.rsp_delta = 0; /* the body starts at the prologue's RSP */
    while (pc < fn->code_count && !jc->bailed) {
        size_t instruction_start = pc;
        /* emit_body_call addresses the offset cell relative to RSP using the
         * tracked delta; every bytecode boundary must be back at the
         * prologue's RSP, or the cell would be written to the wrong slot.
         * Refuse to compile rather than risk corrupting a saved register. */
        if (jc->buf.rsp_delta != 0) { jc->bailed = true; return; }
        jc->current_instruction_start = instruction_start;
        uint8_t raw_opcode = 0;
        if (!decode_u8(fn, &pc, &raw_opcode)) { jc->bailed = true; return; }
        DiamondOpCode opcode = (DiamondOpCode)raw_opcode;
        jc->bytecode_to_native[instruction_start] = jc->buf.length;

        if (!compile_value_instruction(jc, opcode, instruction_start, &pc) &&
            !compile_control_instruction(jc, opcode, instruction_start, &pc) &&
            !compile_outlined_instruction(jc, opcode, instruction_start, &pc)) {
            jc->bailed = true;
            return;
        }
        /* A block's break landing at this call continues with the next
         * instruction (DIAMOND_JIT_RESUME_AFTER_CALL). */
        for (size_t i = 0; i < jc->landing_call_count; i++)
            if (jc->landing_calls[i] == instruction_start &&
                !add_resume_entry(jc, (size_t)DIAMOND_JIT_RESUME_AFTER_CALL | instruction_start,
                    pc)) return;
    }
}

/* Points the rel32 field at `field_offset` at an already-emitted
 * `target_offset` (a backward jump to a stub). */
static void patch_rel32_to(JitBuffer *buf, size_t field_offset, size_t target_offset) {
    if (buf->dry_run || buf->failed) return;
    const int32_t rel = (int32_t)(target_offset - (field_offset + 4));
    buf->code[field_offset] = (uint8_t)(rel & 0xFF);
    buf->code[field_offset + 1] = (uint8_t)((rel >> 8) & 0xFF);
    buf->code[field_offset + 2] = (uint8_t)((rel >> 16) & 0xFF);
    buf->code[field_offset + 3] = (uint8_t)((rel >> 24) & 0xFF);
}

/* The catch and resume stubs of a function with handlers, emitted after the
 * propagate stub. Both run at the body's RSP with the frame linked.
 *   catch: AL holds a failing status; diamond_jit_catch offers it to the
 *     rescue clauses. 0 falls into resume; DIAMOND_JIT_HANDLER_RETURNED (a
 *     block's `return` landed here) returns, the result written; anything
 *     else (the status, or DIAMOND_VM_EXCEPTION once it became an
 *     exception) propagates. The
 *     pad slot still holds the failing instruction's offset, so the trace
 *     line and backtrace name it, as run_chunk's would.
 *   resume: jumps to the clause in state->resume, one compare per resume
 *     key (a rescue target, or the end of a call a break lands at). An
 *     offset matching none cannot happen; it propagates as invalid bytecode
 *     rather than jumping anywhere. */
static void emit_catch_stubs(JitCompiler *jc, size_t propagate_offset,
        size_t *catch_offset, size_t *resume_offset) {
    JitBuffer *buf = &jc->buf;
    const int32_t saved_rsp_delta = buf->rsp_delta;
    buf->rsp_delta = 0;
    *catch_offset = buf->length;
    emit_u8(buf, 0x0F); emit_u8(buf, 0xB6); emit_u8(buf, 0xC0);   /* movzx eax, al */
    emit_mov_rr(buf, REG_R8, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_handler_state_address(jc, REG_RCX);
    emit_mov_rr(buf, REG_R9, JIT_RESULT_PTR);
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_catch);
    emit_u8(buf, 0x0F); emit_u8(buf, 0xB6); emit_u8(buf, 0xC0);   /* movzx eax, al */
    emit_cmp_imm32_32(buf, REG_RAX, DIAMOND_JIT_HANDLER_RETURNED);
    const size_t not_returned = emit_jcc_placeholder(buf, JCC_NE);
    emit_epilogue(jc);
    emit_mov_al_imm8(buf, DIAMOND_VM_OK);
    emit_ret(buf);
    patch_rel32_to_here(buf, not_returned);
    emit_test_r8(buf, REG_RAX);
    patch_rel32_to(buf, emit_jcc_placeholder(buf, JCC_NE), propagate_offset);
    *resume_offset = buf->length;
    emit_load_r64_sib(buf, REG_RAX, REG_RSP,
        DIAMOND_JIT_HANDLER_STATE_AT + DIAMOND_JIT_HANDLER_RESUME_FIELD);
    for (size_t i = 0; i < jc->resume_target_count; i++) {
        emit_cmp_imm32_64(buf, REG_RAX, (uint32_t)jc->resume_keys[i]);
        record_global_patch(jc, emit_jcc_placeholder(buf, JCC_E), jc->resume_targets[i]);
    }
    emit_mov_al_imm8(buf, (uint8_t)DIAMOND_VM_INVALID_BYTECODE);
    patch_rel32_to(buf, emit_jmp_placeholder(buf), propagate_offset);
    buf->rsp_delta = saved_rsp_delta;
}

/* Prologue of a generic function, right after the frame link: binds this
 * call's type variables as run_chunk's entry does (diamond_jit_bind_generic,
 * into the reservation's DiamondJitGenericState) and runs the body in the
 * bound chunk: JIT_CHUNK and the frame's chunk both point at it, so type
 * checks, nested calls and closures read the bindings from there. A failed
 * binding retries the call in the interpreter before anything has run. The
 * prologue's pushes and reservation sit in rsp_delta; the body counts from
 * this RSP, as the calls below do. Non-generic functions never emit this,
 * so their calls pay nothing for it. */
/* A variadic function's prologue: a call passing more arguments than the
 * function has registers, or an UNDEFINED placeholder (a keyword-path call
 * with a block), retries in the interpreter. jit_run_native copies only as
 * many arguments as there are registers, unfiltered, and COLLECT_VARIADIC
 * (diamond_jit_collect_variadic) and ARGUMENT_PROVIDED read the registers.
 * Runs before the body, so retrying repeats nothing. */
static void emit_variadic_guard(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    buf->rsp_delta = 0;
    emit_mov_rr(buf, REG_RDI, JIT_REGISTERS_BASE);
    emit_mov_rr(buf, REG_RSI, JIT_ARGUMENT_COUNT);
    emit_mov_imm64(buf, REG_RDX, jc->function->register_count);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_variadic_entry_ok);
    emit_u8(buf, 0x84); emit_u8(buf, 0xC0);   /* test al, al */
    record_global_patch(jc, emit_jcc_placeholder(buf, JCC_E), BAILOUT_RETRY_SENTINEL);
}

static void emit_generic_bind(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    buf->rsp_delta = 0;
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_mov_rr(buf, REG_RSI, JIT_CHUNK);
    emit_mov_rr(buf, REG_RDX, JIT_REGISTERS_BASE);
    emit_mov_rr(buf, REG_RCX, JIT_ARGUMENT_COUNT);
    emit_load_closure(jc, REG_R8);
    emit_lea_rsp(buf, REG_R9, (int32_t)jc->generic_state_at);
    emit_body_call(jc, (void *)(uintptr_t)diamond_jit_bind_generic);
    emit_u8(buf, 0x48); emit_u8(buf, 0x85); emit_u8(buf, 0xC0);   /* test rax, rax */
    record_global_patch(jc, emit_jcc_placeholder(buf, JCC_E), BAILOUT_RETRY_SENTINEL);
    emit_mov_rr(buf, JIT_CHUNK, REG_RAX);
    emit_store_r64_sib(buf, REG_RSP, DIAMOND_JIT_FRAME_CHUNK, JIT_CHUNK);
}

void *diamond_jit_try_compile(const DiamondFunction *function,
        struct DiamondFunction *const *functions, size_t function_count,
        size_t self_function_index, size_t *out_size) {
    *out_size = 0;
    /* A register_count of 0 is a hand-built chunk's "unset" (run_chunk reads
     * it as the full width); refusing it lets jit_run_native size its
     * register file with one compare (uses_heap_registers). */
    if ((function->nonlocal_landing && functions == nullptr) ||
        function->code_count == 0 ||
        function->register_count == 0 ||
        function->register_count > DIAMOND_REGISTER_COUNT) {
        return nullptr;
    }

    JitCompiler jc = {0};
    jc.self_function_index = self_function_index;
    jc.function = function;
    jc.functions = functions;
    jc.function_count = function_count;
    /* A block's break or return landing here goes through the catch stub
     * (diamond_jit_catch), so the function keeps handler state, and a frame
     * whose serial the block's targets name. */
    if (function->nonlocal_landing) {
        jc.has_handlers = true;
        jc.needs_frame = true;
    }
    jc.bytecode_to_native = malloc(function->code_count * sizeof(size_t));
    if (jc.bytecode_to_native == nullptr) return nullptr;
    for (size_t i = 0; i < function->code_count; i++) jc.bytecode_to_native[i] = SIZE_MAX;

    /* Dry-run pass: walks the exact same decode logic as the real compile
     * below, with every emit_* call suppressed (JitBuffer.dry_run), purely
     * to learn (a) whether this function is compilable at all and (b)
     * whether it needs a DiamondFrame (jc.needs_frame, set by
     * compile_new_string) -- knowable only after seeing the whole body,
     * but needed *before* the real pass's own prologue is emitted. Reuses
     * compile_body itself rather than a second, separately-maintained
     * bytecode-width table that could silently drift out of sync with it. */
    jc.buf.dry_run = true;
    compile_body(&jc);
    if (jc.bailed) {
        free(jc.bytecode_to_native);
        return nullptr;
    }
    /* The dry run never touched jc.buf.code/length/capacity/failed --
     * emit_u8's own dry_run check is unconditional and first -- so only
     * jc.bailed and jc.bytecode_to_native (rewritten with dry-run-only
     * placeholder offsets) need resetting before the real pass; jc.
     * needs_frame is left as-is, since the real pass can only ever
     * (redundantly, consistently) set it again, never clear it. jc.
     * has_called (Phase 2d) is left as-is for the same reason -- and even
     * though this means a bail site *before* the real pass's own SUPER
     * opcode could see has_called already true (from the dry run) and
     * choose the "propagate" stub where "retry" would otherwise apply,
     * that's still correct, not just harmless: every status-bearing bail
     * this JIT compiles is a deterministic function of state the
     * interpreter would see identically on a retry, so "propagate the
     * status directly" and "retry, which independently reaches the exact
     * same status" are observably identical outcomes -- propagating is
     * just strictly cheaper (skips a redundant full re-interpretation). */
    jc.bailed = false;
    jc.buf.dry_run = false;
    for (size_t i = 0; i < function->code_count; i++) jc.bytecode_to_native[i] = SIZE_MAX;
    jc.resume_target_count = 0;
    jc.landing_call_count = 0;
    /* A raise a rescue clause catches carries on in native code, so from
     * then on nothing may retry the function in the interpreter (it would
     * raise again): every failure goes to the catch stub instead. */
    if (jc.has_handlers && jc.has_raise) jc.has_called = true;
    /* A generic function's bound chunk lives in its frame reservation, so it
     * links its frame (emit_generic_bind runs right after). */
    if (function->type_variable_count != 0) jc.needs_frame = true;
    if (function->has_variadic) jc.needs_frame = true;
    if (jc.has_handlers) {
        jc.frame_reserve_bytes = (size_t)DIAMOND_JIT_HANDLER_STATE_AT +
            (size_t)DIAMOND_JIT_HANDLER_STATE_SIZE;
    } else if (jc.needs_frame || jc.slow_frame) {
        jc.frame_reserve_bytes = ((size_t)DIAMOND_JIT_FRAME_SIZE + 15) & ~(size_t)15;
    }
    /* A generic function binds its type variables in its prologue into a
     * DiamondJitGenericState after the rest of the reservation. */
    if (function->type_variable_count != 0) {
        jc.generic_state_at = jc.frame_reserve_bytes;
        jc.frame_reserve_bytes += (sizeof(DiamondJitGenericState) + 15) & ~(size_t)15;
    }

    /* Prologue: save the 6 persistent registers, then load them from the
     * incoming (vm, registers, result, argument_count, chunk, depth)
     * arguments (RDI, RSI, RDX, RCX, R8, R9 per SysV). Unlike Phase 2c's
     * 5 registers (odd, self-aligning), 6 is even, so an explicit 8-byte
     * pad is needed to land back on RSP%16==0 -- correctly aligned for
     * the ABI's pre-call requirement every trampoline call needs (the
     * frame reservation further below is itself rounded up to a multiple
     * of 16, so it doesn't disturb this once established). */
    emit_push(&jc.buf, JIT_REGISTERS_BASE);
    emit_push(&jc.buf, JIT_VM);
    emit_push(&jc.buf, JIT_RESULT_PTR);
    emit_push(&jc.buf, JIT_ARGUMENT_COUNT);
    emit_push(&jc.buf, JIT_CHUNK);
    emit_push(&jc.buf, JIT_DEPTH);
    /* The pad slot doubles as the body's "current instruction offset" cell
     * (see emit_body_call), so it is pushed as a zero rather than just
     * reserved: a propagate exit can never read stale stack from it. It must
     * stay the single 8-byte slot directly above the frame reservation. */
    emit_u8(&jc.buf, 0x6A);                   /* push imm8 0 */
    emit_u8(&jc.buf, 0x00);
    emit_mov_rr(&jc.buf, JIT_VM, REG_RDI);
    emit_mov_rr(&jc.buf, JIT_REGISTERS_BASE, REG_RSI);
    emit_mov_rr(&jc.buf, JIT_RESULT_PTR, REG_RDX);
    emit_mov_rr(&jc.buf, JIT_ARGUMENT_COUNT, REG_RCX);
    emit_mov_rr(&jc.buf, JIT_CHUNK, REG_R8);
    emit_mov_rr(&jc.buf, JIT_DEPTH, REG_R9);

    if (jc.needs_frame) {
        emit_sub_rsp_imm32(&jc.buf, (uint32_t)jc.frame_reserve_bytes);
        emit_frame_link(&jc, 0);
        if (jc.has_handlers) emit_handler_state_link(&jc);
        if (function->has_variadic) emit_variadic_guard(&jc);
        if (function->type_variable_count != 0) emit_generic_bind(&jc);
    } else if (jc.slow_frame) {
        /* Space only: each slow call links the frame around itself. */
        emit_sub_rsp_imm32(&jc.buf, (uint32_t)jc.frame_reserve_bytes);
    }

    compile_body(&jc);

    void *result = nullptr;
    if (!jc.bailed && !jc.buf.failed) {
        size_t retry_offset = jc.buf.length;
        emit_epilogue(&jc);
        emit_mov_al_imm8(&jc.buf, DIAMOND_JIT_RETRY);
        emit_ret(&jc.buf);
        /* Phase 2d: the "propagate" stub only exists (and is only ever a
         * patch target) for a function that actually contains a call --
         * jc.has_called implies jc.needs_frame (see compile_super_call),
         * so emit_epilogue_propagate's own frame-pop is always valid here
         * when reached. Emitted unconditionally rather than gated on
         * jc.has_called purely because a handful of always-dead bytes in
         * every OTHER compiled function's own buffer costs nothing
         * functionally and keeps this code simpler than conditionally
         * tracking whether the sentinel was ever actually used. */
        size_t propagate_offset = jc.buf.length;
        emit_epilogue_propagate(&jc);
        emit_ret(&jc.buf);
        size_t catch_offset = SIZE_MAX, resume_offset = SIZE_MAX;
        if (jc.has_handlers) emit_catch_stubs(&jc, propagate_offset, &catch_offset, &resume_offset);

        for (size_t i = 0; i < jc.patch_count && !jc.buf.failed; i++) {
            size_t target_native = jc.patches[i].bytecode_target == BAILOUT_RETRY_SENTINEL
                ? retry_offset
                : jc.patches[i].bytecode_target == BAILOUT_PROPAGATE_SENTINEL
                    ? propagate_offset
                : jc.patches[i].bytecode_target == BAILOUT_CATCH_SENTINEL
                    ? catch_offset
                : jc.patches[i].bytecode_target == BAILOUT_RESUME_SENTINEL
                    ? resume_offset
                : jc.patches[i].bytecode_target < function->code_count
                    ? jc.bytecode_to_native[jc.patches[i].bytecode_target]
                    : SIZE_MAX;
            if (target_native == SIZE_MAX) { jc.buf.failed = true; break; }
            int32_t rel = (int32_t)(target_native - (jc.patches[i].native_rel32_offset + 4));
            size_t at = jc.patches[i].native_rel32_offset;
            jc.buf.code[at] = (uint8_t)(rel & 0xFF);
            jc.buf.code[at + 1] = (uint8_t)((rel >> 8) & 0xFF);
            jc.buf.code[at + 2] = (uint8_t)((rel >> 16) & 0xFF);
            jc.buf.code[at + 3] = (uint8_t)((rel >> 24) & 0xFF);
        }

        if (!jc.buf.failed) {
            size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
            size_t mapped_size = ((jc.buf.length + page_size - 1) / page_size) * page_size;
            void *executable = mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (executable != MAP_FAILED) {
                memcpy(executable, jc.buf.code, jc.buf.length);
                if (mprotect(executable, mapped_size, PROT_READ | PROT_EXEC) == 0) {
                    result = executable;
                    *out_size = mapped_size;
                } else {
                    munmap(executable, mapped_size);
                }
            }
        }
    }

    free(jc.bytecode_to_native);
    free(jc.patches);
    free(jc.buf.code);
    return result;
}

void diamond_jit_free(void *jit_code, size_t jit_code_size) {
    if (jit_code == nullptr) return;
    munmap(jit_code, jit_code_size);
}
