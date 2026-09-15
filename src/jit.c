/* Minimal x86-64 baseline JIT -- see jit.h for the full scope statement.
 * Phase 2 (register moves/constants/_INT arithmetic/comparisons/jumps/
 * return, zero-argument functions only) plus Phase 2b's extension: Hash
 * reads and typed-field writes via C trampolines (jit.h/vm.c), and
 * arguments/`self` (non-generic, non-variadic methods and functions).
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

#include "jit.h"

#include <stdlib.h>
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
     * real prologue, from diamond_jit_frame_size(). */
    size_t frame_reserve_bytes;
    /* Phase 2d: set the moment SUPER is compiled, in both the dry-run and
     * real passes alike (mirroring needs_frame). Once true: (a) every
     * later status-bearing bail site (a trampoline call's own non-OK
     * check) targets the "propagate" stub instead of "retry" -- see
     * bailout_target() -- since a real call with real side effects has
     * already run and re-running the whole function from scratch is no
     * longer safe; (b) ADD_INT/SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT/
     * LESS_INT are rejected outright at compile time (jc->bailed = true),
     * since their own overflow/div-by-zero/kind-mismatch bails have no
     * DiamondVmStatus to propagate and genuinely need the interpreter's
     * own bignum/raise logic, not just a status to hand back. */
    bool has_called;
} JitCompiler;

static void jit_buf_ensure(JitBuffer *buf, size_t extra) {
    if (buf->failed) return;
    if (buf->length + extra <= buf->capacity) return;
    size_t new_capacity = buf->capacity == 0 ? 256 : buf->capacity * 2;
    while (new_capacity < buf->length + extra) new_capacity *= 2;
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

/* mov [base + disp32], reg64 */
static void emit_store_r64(JitBuffer *buf, int base, int32_t disp, int reg) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x89);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
    emit_u32_le(buf, (uint32_t)disp);
}

/* lea reg64, [base + disp32] */
static void emit_lea(JitBuffer *buf, int reg, int base, int32_t disp) {
    emit_u8(buf, emit_rex(true, reg, base));
    emit_u8(buf, 0x8D);
    emit_u8(buf, (uint8_t)(0x80 | ((reg & 7) << 3) | (base & 7)));
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
enum { ALU_ADD = 0x01, ALU_SUB = 0x29, ALU_CMP = 0x39 };

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

/* setl al */
static void emit_setl_al(JitBuffer *buf) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x9C);
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
    if (reg >= 8) emit_u8(buf, 0x41);
    emit_u8(buf, (uint8_t)(0x50 + (reg & 7)));
}
static void emit_pop(JitBuffer *buf, int reg) {
    if (reg >= 8) emit_u8(buf, 0x41);
    emit_u8(buf, (uint8_t)(0x58 + (reg & 7)));
}

/* sub rsp, imm32 / add rsp, imm32 -- reserving/releasing stack space for a
 * DiamondFrame (Phase 2c). /0 = ADD, /5 = SUB, per the Intel SDM's opcode
 * 81 extension table. */
static void emit_add_rsp_imm32(JitBuffer *buf, uint32_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (0 << 3) | REG_RSP));
    emit_u32_le(buf, imm);
}
static void emit_sub_rsp_imm32(JitBuffer *buf, uint32_t imm) {
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
static void emit_epilogue(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    if (jc->needs_frame) {
        emit_mov_rr(buf, REG_RDI, JIT_VM);
        emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_frame_pop);
        emit_add_rsp_imm32(buf, (uint32_t)jc->frame_reserve_bytes);
    }
    emit_pop_persistent_registers(buf);
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
 * it here. Only ever reached when jc->has_called is true, which is only
 * ever set alongside jc->needs_frame (SUPER sets both), so a frame is
 * always present here to pop. */
static void emit_epilogue_propagate(JitCompiler *jc) {
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, JIT_RESULT_PTR, REG_RAX);
    emit_mov_rr(buf, REG_RDI, JIT_VM);
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_frame_pop);
    emit_mov_rr(buf, REG_RAX, JIT_RESULT_PTR);
    emit_add_rsp_imm32(buf, (uint32_t)jc->frame_reserve_bytes);
    emit_pop_persistent_registers(buf);
}

/* Near conditional jump to a not-yet-known local offset: emits `0F 8x
 * rel32` with a zero placeholder and returns the buffer offset of the
 * rel32 field, to be patched via patch_rel32_here once the fallthrough
 * point is reached (still within the same instruction's own stencil, so
 * this is never deferred to the cross-function patch list). */
enum {
    JCC_E = 0x84,
    JCC_NE = 0x85,
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
        size_t new_capacity = jc->patch_capacity == 0 ? 16 : jc->patch_capacity * 2;
        JitPatch *grown = realloc(jc->patches, new_capacity * sizeof(JitPatch));
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

/* Which stub a status-bearing bail site (one that already holds a real
 * DiamondVmStatus in AL, from a trampoline's own return value) should
 * target: "retry" before any call has run this attempt, "propagate" from
 * the first call onward. Never used by a bail site that has no real
 * status to hand back (arithmetic overflow/div-by-zero/kind-mismatch) --
 * those always use BAILOUT_RETRY_SENTINEL directly and are rejected
 * outright at compile time once jc->has_called is true (see compile_
 * body's ADD_INT/SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT/LESS_INT case). */
static size_t bailout_target(const JitCompiler *jc) {
    return jc->has_called ? BAILOUT_PROPAGATE_SENTINEL : BAILOUT_RETRY_SENTINEL;
}

static int32_t reg_disp(size_t index, int32_t field_offset) {
    return (int32_t)(index * sizeof(DiamondValue)) + field_offset;
}

static const int32_t KIND_OFF = offsetof(DiamondValue, kind);
static const int32_t AS_OFF = offsetof(DiamondValue, as);

static void emit_check_kind_int_or_bail(JitCompiler *jc, uint16_t reg_index, int scratch) {
    emit_load_byte_zx(&jc->buf, scratch, JIT_REGISTERS_BASE, reg_disp(reg_index, KIND_OFF));
    emit_cmp_imm32_32(&jc->buf, scratch, DIAMOND_VALUE_INT);
    emit_u8(&jc->buf, 0x0F);
    emit_u8(&jc->buf, JCC_NE);
    size_t field = jc->buf.length;
    emit_u32_le(&jc->buf, 0);
    record_global_patch(jc, field, BAILOUT_RETRY_SENTINEL);
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

static void compile_binary_int_op(JitCompiler *jc, uint16_t dest, uint16_t left,
                                   uint16_t right, DiamondOpCode op) {
    JitBuffer *buf = &jc->buf;
    emit_check_kind_int_or_bail(jc, left, REG_RAX);
    emit_check_kind_int_or_bail(jc, right, REG_RAX);
    if (op == DIAMOND_OP_DIVIDE_INT) {
        emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
        emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
        emit_test_r64(buf, REG_RCX, REG_RCX);
        emit_u8(buf, 0x0F);
        emit_u8(buf, JCC_E);
        size_t jz_field = jc->buf.length;
        emit_u32_le(buf, 0);
        record_global_patch(jc, jz_field, BAILOUT_RETRY_SENTINEL);
        /* left==INT64_MIN && right==-1 -> bail (bignum negate territory) */
        emit_cmp_imm8(buf, REG_RCX, -1);
        size_t skip = emit_jcc_placeholder(buf, JCC_NE);
        emit_mov_imm64(buf, REG_RDX, (uint64_t)INT64_MIN);
        emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RDX);
        size_t jne_ok = emit_jcc_placeholder(buf, JCC_NE);
        emit_u8(buf, 0xE9);
        size_t bail_field = jc->buf.length;
        emit_u32_le(buf, 0);
        record_global_patch(jc, bail_field, BAILOUT_RETRY_SENTINEL);
        patch_rel32_to_here(buf, jne_ok);
        patch_rel32_to_here(buf, skip);
        emit_cqo(buf);
        emit_idiv(buf, REG_RCX);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
        emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
        return;
    }
    emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(left, AS_OFF));
    emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(right, AS_OFF));
    if (op == DIAMOND_OP_LESS_INT) {
        emit_alu_rr(buf, ALU_CMP, REG_RAX, REG_RCX);
        emit_setl_al(buf);
        emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
        emit_store_byte_reg(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
        return;
    }
    if (op == DIAMOND_OP_ADD_INT) {
        emit_alu_rr(buf, ALU_ADD, REG_RAX, REG_RCX);
    } else if (op == DIAMOND_OP_SUBTRACT_INT) {
        emit_alu_rr(buf, ALU_SUB, REG_RAX, REG_RCX);
    } else {
        emit_imul_rr(buf, REG_RAX, REG_RCX);
    }
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x80); /* JO */
    size_t jo_field = jc->buf.length;
    emit_u32_le(buf, 0);
    record_global_patch(jc, jo_field, BAILOUT_RETRY_SENTINEL);
    emit_store_kind_imm(buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
    emit_store_r64(buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_equal_general);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
    size_t general_done = emit_jmp_placeholder(buf);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_set_ivar);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_get_ivar);
    emit_bail_if_al_nonzero(jc);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_index_get);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_index_set);
    emit_add_rsp_imm32(buf, 16);
    emit_bail_if_al_nonzero(jc);
}

/* CHECK_TYPE -- calls diamond_jit_check_type(chunk, &registers[source], set_index). */
static void compile_check_type(JitCompiler *jc, uint16_t source, uint16_t set_index) {
    JitBuffer *buf = &jc->buf;
    emit_mov_rr(buf, REG_RDI, JIT_CHUNK);
    emit_lea(buf, REG_RSI, JIT_REGISTERS_BASE, reg_disp(source, 0));
    emit_mov_imm64(buf, REG_RDX, set_index);
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_check_type);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_new_string);
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_new_hash);
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
static void compile_super_call(JitCompiler *jc, uint16_t dest, uint8_t owner_index,
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
    emit_call_trampoline(buf, (void *)(uintptr_t)diamond_jit_super_call);
    emit_add_rsp_imm32(buf, 32); /* reclaim the 4 pushed stack slots */
    emit_bail_if_al_nonzero(jc);
}

/* Returns false (jc->bailed set) the moment anything outside the supported
 * whitelist is found -- the caller must then discard the whole attempt. */
static void compile_body(JitCompiler *jc) {
    const DiamondFunction *fn = jc->function;
    size_t pc = 0;
    while (pc < fn->code_count && !jc->bailed) {
        size_t instruction_start = pc;
        uint8_t raw_opcode = 0;
        if (!decode_u8(fn, &pc, &raw_opcode)) { jc->bailed = true; return; }
        DiamondOpCode opcode = (DiamondOpCode)raw_opcode;
        jc->bytecode_to_native[instruction_start] = jc->buf.length;

        switch (opcode) {
            case DIAMOND_OP_NIL: {
                uint16_t dest = 0;
                if (!decode_u16(fn, &pc, &dest)) { jc->bailed = true; return; }
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
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &boolean)) { jc->bailed = true; return; }
                emit_store_kind_imm(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
                emit_mov_imm64(&jc->buf, REG_RAX, boolean != 0 ? 1 : 0);
                emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
                break;
            }
            case DIAMOND_OP_MOVE: {
                uint16_t dest = 0, src = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &src)) { jc->bailed = true; return; }
                emit_load_r64(&jc->buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(src, 0));
                emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, 0), REG_RAX);
                emit_load_r64(&jc->buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(src, 8));
                emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, 8), REG_RAX);
                break;
            }
            case DIAMOND_OP_CONSTANT: {
                uint16_t dest = 0, index = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) { jc->bailed = true; return; }
                if (index >= fn->constant_count) { jc->bailed = true; return; }
                DiamondValue constant = fn->constants[index];
                if (constant.kind != DIAMOND_VALUE_INT) { jc->bailed = true; return; }
                emit_store_kind_imm(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
                emit_mov_imm64(&jc->buf, REG_RAX, (uint64_t)constant.as.integer);
                emit_store_r64(&jc->buf, JIT_REGISTERS_BASE, reg_disp(dest, AS_OFF), REG_RAX);
                break;
            }
            case DIAMOND_OP_ADD_INT:
            case DIAMOND_OP_SUBTRACT_INT:
            case DIAMOND_OP_MULTIPLY_INT:
            case DIAMOND_OP_DIVIDE_INT:
            case DIAMOND_OP_LESS_INT: {
                /* Phase 2d: these opcodes' own bailout paths (overflow,
                 * division by zero, INT64_MIN/-1, non-INT operand) have no
                 * real DiamondVmStatus to hand back -- they need the
                 * interpreter's own bignum-promotion/raise logic, not just
                 * a status to propagate. Once a call has already run
                 * (jc->has_called), bailing here can only mean "discard
                 * and retry," which is no longer safe (see jc->has_called's
                 * own comment) -- so a function containing one of these
                 * opcodes after a call is rejected outright at compile
                 * time, the same structural treatment as any unsupported
                 * opcode. Confirmed harmless for skindicate's real
                 * User#initialize: it contains no arithmetic at all. */
                if (jc->has_called) { jc->bailed = true; return; }
                uint16_t dest = 0, left = 0, right = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                    !decode_u16(fn, &pc, &right)) { jc->bailed = true; return; }
                compile_binary_int_op(jc, dest, left, right, opcode);
                break;
            }
            case DIAMOND_OP_EQUAL:
            case DIAMOND_OP_NOT_EQUAL: {
                uint16_t dest = 0, left = 0, right = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                    !decode_u16(fn, &pc, &right)) { jc->bailed = true; return; }
                compile_equal_op(jc, instruction_start, dest, left, right, opcode == DIAMOND_OP_NOT_EQUAL);
                break;
            }
            case DIAMOND_OP_SET_IVAR: {
                uint16_t recv = 0, field = 0, source = 0;
                if (!decode_u16(fn, &pc, &recv) || !decode_u16(fn, &pc, &field) ||
                    !decode_u16(fn, &pc, &source)) { jc->bailed = true; return; }
                if (field > UINT8_MAX) { jc->bailed = true; return; }
                compile_set_ivar(jc, instruction_start, recv, field, source);
                break;
            }
            case DIAMOND_OP_GET_IVAR: {
                uint16_t dest = 0, recv = 0, field = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                    !decode_u16(fn, &pc, &field)) { jc->bailed = true; return; }
                if (field > UINT8_MAX) { jc->bailed = true; return; }
                compile_get_ivar(jc, instruction_start, dest, recv, field);
                break;
            }
            case DIAMOND_OP_ARGUMENT_PROVIDED: {
                uint16_t dest = 0, index = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) { jc->bailed = true; return; }
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
                if (!decode_u16(fn, &pc, &source) || !decode_u16(fn, &pc, &set_index)) { jc->bailed = true; return; }
                compile_check_type(jc, source, set_index);
                break;
            }
            case DIAMOND_OP_STRING: {
                uint16_t dest = 0, string_index = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &string_index)) { jc->bailed = true; return; }
                compile_new_string(jc, dest, string_index);
                break;
            }
            case DIAMOND_OP_INDEX_GET: {
                uint16_t dest = 0, recv = 0, index = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &recv) ||
                    !decode_u16(fn, &pc, &index)) { jc->bailed = true; return; }
                compile_index_get(jc, instruction_start, dest, recv, index);
                break;
            }
            case DIAMOND_OP_INDEX_SET: {
                uint16_t recv = 0, index = 0, source = 0;
                if (!decode_u16(fn, &pc, &recv) || !decode_u16(fn, &pc, &index) ||
                    !decode_u16(fn, &pc, &source)) { jc->bailed = true; return; }
                compile_index_set(jc, instruction_start, recv, index, source);
                break;
            }
            case DIAMOND_OP_HASH: {
                uint16_t dest = 0, base = 0, count = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &base) ||
                    !decode_u16(fn, &pc, &count)) { jc->bailed = true; return; }
                compile_new_hash(jc, dest, base, count);
                break;
            }
            case DIAMOND_OP_SUPER: {
                uint16_t dest = 0, name = 0, base = 0;
                uint8_t owner_index = 0, argc = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u8(fn, &pc, &owner_index) ||
                    !decode_u16(fn, &pc, &name) || !decode_u16(fn, &pc, &base) ||
                    !decode_u8(fn, &pc, &argc)) { jc->bailed = true; return; }
                compile_super_call(jc, dest, owner_index, name, base, argc);
                break;
            }
            case DIAMOND_OP_JUMP: {
                uint8_t high = 0, low = 0;
                if (!decode_u8(fn, &pc, &high) || !decode_u8(fn, &pc, &low)) { jc->bailed = true; return; }
                size_t target = ((size_t)high << 8) | low;
                size_t field = emit_jmp_placeholder(&jc->buf);
                record_global_patch(jc, field, target);
                break;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: {
                uint16_t condition = 0;
                uint8_t high = 0, low = 0;
                if (!decode_u16(fn, &pc, &condition) || !decode_u8(fn, &pc, &high) ||
                    !decode_u8(fn, &pc, &low)) { jc->bailed = true; return; }
                size_t target = ((size_t)high << 8) | low;
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
                break;
            }
            case DIAMOND_OP_JUMP_IF_FALSE: {
                uint16_t condition = 0;
                uint8_t high = 0, low = 0;
                if (!decode_u16(fn, &pc, &condition) || !decode_u8(fn, &pc, &high) ||
                    !decode_u8(fn, &pc, &low)) { jc->bailed = true; return; }
                size_t target = ((size_t)high << 8) | low;
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
                break;
            }
            case DIAMOND_OP_RETURN: {
                uint16_t source = 0;
                if (!decode_u16(fn, &pc, &source)) { jc->bailed = true; return; }
                JitBuffer *buf = &jc->buf;
                emit_load_r64(buf, REG_RAX, JIT_REGISTERS_BASE, reg_disp(source, 0));
                emit_load_r64(buf, REG_RCX, JIT_REGISTERS_BASE, reg_disp(source, 8));
                emit_store_r64(buf, JIT_RESULT_PTR, 0, REG_RAX);
                emit_store_r64(buf, JIT_RESULT_PTR, 8, REG_RCX);
                emit_epilogue(jc);
                emit_mov_al_imm8(buf, DIAMOND_VM_OK);
                emit_ret(buf);
                break;
            }
            default:
                jc->bailed = true;
                return;
        }
    }
}

void *diamond_jit_try_compile(const DiamondFunction *function, size_t *out_size) {
    *out_size = 0;
    if (function->has_variadic || function->type_variable_count != 0 ||
        function->code_count == 0 ||
        function->register_count > DIAMOND_JIT_MAX_REGISTERS ||
        (size_t)function->arity + 1 > DIAMOND_JIT_MAX_REGISTERS) {
        return nullptr;
    }

    JitCompiler jc = {0};
    jc.function = function;
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
    if (jc.needs_frame) {
        jc.frame_reserve_bytes = (diamond_jit_frame_size() + 15) & ~(size_t)15;
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
    emit_sub_rsp_imm32(&jc.buf, 8);
    emit_mov_rr(&jc.buf, JIT_VM, REG_RDI);
    emit_mov_rr(&jc.buf, JIT_REGISTERS_BASE, REG_RSI);
    emit_mov_rr(&jc.buf, JIT_RESULT_PTR, REG_RDX);
    emit_mov_rr(&jc.buf, JIT_ARGUMENT_COUNT, REG_RCX);
    emit_mov_rr(&jc.buf, JIT_CHUNK, REG_R8);
    emit_mov_rr(&jc.buf, JIT_DEPTH, REG_R9);

    if (jc.needs_frame) {
        emit_sub_rsp_imm32(&jc.buf, (uint32_t)jc.frame_reserve_bytes);
        const size_t live_register_count = function->register_count == 0
            ? DIAMOND_JIT_MAX_REGISTERS : function->register_count;
        emit_mov_rr(&jc.buf, REG_RDI, REG_RSP);
        emit_mov_rr(&jc.buf, REG_RSI, JIT_VM);
        emit_mov_rr(&jc.buf, REG_RDX, JIT_REGISTERS_BASE);
        emit_mov_imm64(&jc.buf, REG_RCX, live_register_count);
        emit_mov_rr(&jc.buf, REG_R8, JIT_CHUNK);
        emit_call_trampoline(&jc.buf, (void *)(uintptr_t)diamond_jit_frame_push);
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

        for (size_t i = 0; i < jc.patch_count && !jc.buf.failed; i++) {
            size_t target_native = jc.patches[i].bytecode_target == BAILOUT_RETRY_SENTINEL
                ? retry_offset
                : jc.patches[i].bytecode_target == BAILOUT_PROPAGATE_SENTINEL
                    ? propagate_offset
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
