/* Minimal x86-64 baseline JIT -- see jit.h for the full scope statement.
 * Compiles a narrow whitelist of register-arithmetic/control-flow opcodes
 * for zero-argument, non-generic, non-method top-level functions straight
 * to machine code. Anything outside the whitelist (including any
 * exceptional runtime condition within a supported opcode -- overflow,
 * division by zero, INT64_MIN/-1) is a bailout, not a partial compile: the
 * caller falls back to the ordinary, fully-correct run_chunk interpreter.
 *
 * Encoding notes (all instructions below were hand-verified against the
 * Intel SDM, not assumed): every scratch register used is RAX/RCX/RDX/RBX
 * (encodings 0-3) specifically so no REX.R/X/B extension bit is ever
 * needed -- every 64-bit operation's REX prefix is the fixed byte 0x48
 * (REX.W only). RDI (the registers-array base pointer) and RSI (the result
 * out-pointer) are never clobbered by anything below.
 */

#include "jit.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

enum {
    JIT_REG_RAX = 0,
    JIT_REG_RCX = 1,
    JIT_REG_RDX = 2,
    JIT_REG_RBX = 3,
    JIT_REG_RSI = 6,
    JIT_REG_RDI = 7,
};

typedef struct JitBuffer {
    uint8_t *code;
    size_t length;
    size_t capacity;
    bool failed; /* set on allocation failure; checked once at the end */
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

/* mov reg64, [JIT_REG_RDI + disp32] */
static void emit_load_r64(JitBuffer *buf, int reg, int32_t disp) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x8B);
    emit_u8(buf, (uint8_t)(0x80 | (reg << 3) | JIT_REG_RDI));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov [JIT_REG_RDI + disp32], reg64 */
static void emit_store_r64(JitBuffer *buf, int32_t disp, int reg) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x89);
    emit_u8(buf, (uint8_t)(0x80 | (reg << 3) | JIT_REG_RDI));
    emit_u32_le(buf, (uint32_t)disp);
}

/* movzx reg32, byte [JIT_REG_RDI + disp32] -- zero-extends into full reg64 */
static void emit_load_byte_zx(JitBuffer *buf, int reg, int32_t disp) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xB6);
    emit_u8(buf, (uint8_t)(0x80 | (reg << 3) | JIT_REG_RDI));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov byte [JIT_REG_RDI + disp32], imm8 */
static void emit_store_kind_imm(JitBuffer *buf, int32_t disp, uint8_t kind) {
    emit_u8(buf, 0xC6);
    emit_u8(buf, (uint8_t)(0x80 | JIT_REG_RDI));
    emit_u32_le(buf, (uint32_t)disp);
    emit_u8(buf, kind);
}

/* mov byte [JIT_REG_RDI + disp32], reg8 (reg must be RAX/RCX/RDX/RBX: AL/CL/DL/BL) */
static void emit_store_byte_reg(JitBuffer *buf, int32_t disp, int reg) {
    emit_u8(buf, 0x88);
    emit_u8(buf, (uint8_t)(0x80 | (reg << 3) | JIT_REG_RDI));
    emit_u32_le(buf, (uint32_t)disp);
}

/* mov reg64, imm64 */
static void emit_mov_imm64(JitBuffer *buf, int reg, uint64_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, (uint8_t)(0xB8 + reg));
    emit_u64_le(buf, imm);
}

/* cmp reg64, imm8 (sign-extended) */
static void emit_cmp_imm8(JitBuffer *buf, int reg, int8_t imm) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x83);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | reg)); /* /7 = CMP */
    emit_u8(buf, (uint8_t)imm);
}

/* cmp reg32, imm32 -- used only for the small kind-tag comparisons (0-6) */
static void emit_cmp_imm32_32(JitBuffer *buf, int reg, uint32_t imm) {
    emit_u8(buf, 0x81);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | reg));
    emit_u32_le(buf, imm);
}

/* test reg64, reg64 */
static void emit_test_r64(JitBuffer *buf, int a, int b) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x85);
    emit_u8(buf, (uint8_t)(0xC0 | (b << 3) | a));
}

/* test al/cl/dl/bl, same reg (8-bit) */
static void emit_test_r8(JitBuffer *buf, int reg) {
    emit_u8(buf, 0x84);
    emit_u8(buf, (uint8_t)(0xC0 | (reg << 3) | reg));
}

/* add/sub/cmp/mov dst64, src64 (register-register) */
static void emit_alu_rr(JitBuffer *buf, uint8_t opcode, int dst, int src) {
    emit_u8(buf, 0x48);
    emit_u8(buf, opcode);
    emit_u8(buf, (uint8_t)(0xC0 | (src << 3) | dst));
}
enum { ALU_ADD = 0x01, ALU_SUB = 0x29, ALU_CMP = 0x39, ALU_MOV = 0x89 };

/* imul dst64, src64 (two-operand form; sets OF/CF on signed 64-bit overflow) */
static void emit_imul_rr(JitBuffer *buf, int dst, int src) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0xAF);
    emit_u8(buf, (uint8_t)(0xC0 | (dst << 3) | src));
}

/* cqo: sign-extend RAX into RDX:RAX */
static void emit_cqo(JitBuffer *buf) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0x99);
}

/* idiv reg64: RDX:RAX / reg -> quotient in RAX, remainder in RDX */
static void emit_idiv(JitBuffer *buf, int reg) {
    emit_u8(buf, 0x48);
    emit_u8(buf, 0xF7);
    emit_u8(buf, (uint8_t)(0xC0 | (7 << 3) | reg)); /* /7 = IDIV */
}

/* setl al */
static void emit_setl_al(JitBuffer *buf) {
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x9C);
    emit_u8(buf, 0xC0);
}

/* ret */
static void emit_ret(JitBuffer *buf) { emit_u8(buf, 0xC3); }

/* mov al, imm8 -- used only for the two bool return-value stencils */
static void emit_mov_al_imm8(JitBuffer *buf, uint8_t imm) {
    emit_u8(buf, 0xB0);
    emit_u8(buf, imm);
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
    if (buf->failed) return;
    int32_t rel = (int32_t)(buf->length - (field_offset + 4));
    buf->code[field_offset] = (uint8_t)(rel & 0xFF);
    buf->code[field_offset + 1] = (uint8_t)((rel >> 8) & 0xFF);
    buf->code[field_offset + 2] = (uint8_t)((rel >> 16) & 0xFF);
    buf->code[field_offset + 3] = (uint8_t)((rel >> 24) & 0xFF);
}

static void record_global_patch(JitCompiler *jc, size_t rel32_offset, size_t bytecode_target) {
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

/* Bailout stub: a single shared tail (`mov al, 0; ret`) every exceptional
 * path jumps to. Its native offset is recorded once, all bailout jumps are
 * deferred patches resolved against it like any other jump target -- but
 * since it isn't a real bytecode offset, it's resolved directly rather
 * than through bytecode_to_native (see compile function below). */

#define BAILOUT_SENTINEL SIZE_MAX

static int32_t reg_disp(size_t index, int32_t field_offset) {
    return (int32_t)(index * sizeof(DiamondValue)) + field_offset;
}

static const int32_t KIND_OFF = offsetof(DiamondValue, kind);
static const int32_t AS_OFF = offsetof(DiamondValue, as);

static void emit_check_kind_int_or_bail(JitCompiler *jc, uint16_t reg_index, int scratch) {
    emit_load_byte_zx(&jc->buf, scratch, reg_disp(reg_index, KIND_OFF));
    emit_cmp_imm32_32(&jc->buf, scratch, DIAMOND_VALUE_INT);
    emit_u8(&jc->buf, 0x0F);
    emit_u8(&jc->buf, JCC_NE);
    size_t field = jc->buf.length;
    emit_u32_le(&jc->buf, 0);
    record_global_patch(jc, field, BAILOUT_SENTINEL);
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
    emit_check_kind_int_or_bail(jc, left, JIT_REG_RAX);
    emit_check_kind_int_or_bail(jc, right, JIT_REG_RAX);
    if (op == DIAMOND_OP_DIVIDE_INT) {
        emit_load_r64(buf, JIT_REG_RAX, reg_disp(left, AS_OFF));
        emit_load_r64(buf, JIT_REG_RCX, reg_disp(right, AS_OFF));
        emit_test_r64(buf, JIT_REG_RCX, JIT_REG_RCX);
        emit_u8(buf, 0x0F);
        emit_u8(buf, JCC_E);
        size_t jz_field = jc->buf.length;
        emit_u32_le(buf, 0);
        record_global_patch(jc, jz_field, BAILOUT_SENTINEL);
        /* left==INT64_MIN && right==-1 -> bail (bignum negate territory) */
        emit_cmp_imm8(buf, JIT_REG_RCX, -1);
        size_t skip = emit_jcc_placeholder(buf, JCC_NE);
        emit_mov_imm64(buf, JIT_REG_RBX, (uint64_t)INT64_MIN);
        emit_alu_rr(buf, ALU_CMP, JIT_REG_RAX, JIT_REG_RBX);
        size_t jne_ok = emit_jcc_placeholder(buf, JCC_NE);
        emit_u8(buf, 0xE9);
        size_t bail_field = jc->buf.length;
        emit_u32_le(buf, 0);
        record_global_patch(jc, bail_field, BAILOUT_SENTINEL);
        patch_rel32_to_here(buf, jne_ok);
        patch_rel32_to_here(buf, skip);
        emit_cqo(buf);
        emit_idiv(buf, JIT_REG_RCX);
        emit_store_kind_imm(buf, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
        emit_store_r64(buf, reg_disp(dest, AS_OFF), JIT_REG_RAX);
        return;
    }
    emit_load_r64(buf, JIT_REG_RAX, reg_disp(left, AS_OFF));
    emit_load_r64(buf, JIT_REG_RCX, reg_disp(right, AS_OFF));
    if (op == DIAMOND_OP_LESS_INT) {
        emit_alu_rr(buf, ALU_CMP, JIT_REG_RAX, JIT_REG_RCX);
        emit_setl_al(buf);
        emit_store_kind_imm(buf, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_BOOL);
        emit_store_byte_reg(buf, reg_disp(dest, AS_OFF), JIT_REG_RAX);
        return;
    }
    if (op == DIAMOND_OP_ADD_INT) {
        emit_alu_rr(buf, ALU_ADD, JIT_REG_RAX, JIT_REG_RCX);
    } else if (op == DIAMOND_OP_SUBTRACT_INT) {
        emit_alu_rr(buf, ALU_SUB, JIT_REG_RAX, JIT_REG_RCX);
    } else {
        emit_imul_rr(buf, JIT_REG_RAX, JIT_REG_RCX);
    }
    emit_u8(buf, 0x0F);
    emit_u8(buf, 0x80); /* JO */
    size_t jo_field = jc->buf.length;
    emit_u32_le(buf, 0);
    record_global_patch(jc, jo_field, BAILOUT_SENTINEL);
    emit_store_kind_imm(buf, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
    emit_store_r64(buf, reg_disp(dest, AS_OFF), JIT_REG_RAX);
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
                 * arrive pre-zeroed (see the call site's own memset), so
                 * this is a real store, not a no-op, only because a
                 * register can be reassigned to something else and then
                 * back to nil within one function body (a while loop's own
                 * always-nil result register, matching DIAMOND_OP_NIL's own
                 * surviving emission sites post the jit-experimentation
                 * NIL-elision work -- see that opcode's comment in vm.c). */
                emit_store_kind_imm(&jc->buf, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_NIL);
                emit_mov_imm64(&jc->buf, JIT_REG_RAX, 0);
                emit_store_r64(&jc->buf, reg_disp(dest, AS_OFF), JIT_REG_RAX);
                break;
            }
            case DIAMOND_OP_MOVE: {
                uint16_t dest = 0, src = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &src)) { jc->bailed = true; return; }
                emit_load_r64(&jc->buf, JIT_REG_RAX, reg_disp(src, 0));
                emit_store_r64(&jc->buf, reg_disp(dest, 0), JIT_REG_RAX);
                emit_load_r64(&jc->buf, JIT_REG_RAX, reg_disp(src, 8));
                emit_store_r64(&jc->buf, reg_disp(dest, 8), JIT_REG_RAX);
                break;
            }
            case DIAMOND_OP_CONSTANT: {
                uint16_t dest = 0, index = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &index)) { jc->bailed = true; return; }
                if (index >= fn->constant_count) { jc->bailed = true; return; }
                DiamondValue constant = fn->constants[index];
                if (constant.kind != DIAMOND_VALUE_INT) { jc->bailed = true; return; }
                emit_store_kind_imm(&jc->buf, reg_disp(dest, KIND_OFF), DIAMOND_VALUE_INT);
                emit_mov_imm64(&jc->buf, JIT_REG_RAX, (uint64_t)constant.as.integer);
                emit_store_r64(&jc->buf, reg_disp(dest, AS_OFF), JIT_REG_RAX);
                break;
            }
            case DIAMOND_OP_ADD_INT:
            case DIAMOND_OP_SUBTRACT_INT:
            case DIAMOND_OP_MULTIPLY_INT:
            case DIAMOND_OP_DIVIDE_INT:
            case DIAMOND_OP_LESS_INT: {
                uint16_t dest = 0, left = 0, right = 0;
                if (!decode_u16(fn, &pc, &dest) || !decode_u16(fn, &pc, &left) ||
                    !decode_u16(fn, &pc, &right)) { jc->bailed = true; return; }
                compile_binary_int_op(jc, dest, left, right, opcode);
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
            case DIAMOND_OP_JUMP_IF_FALSE: {
                uint16_t condition = 0;
                uint8_t high = 0, low = 0;
                if (!decode_u16(fn, &pc, &condition) || !decode_u8(fn, &pc, &high) ||
                    !decode_u8(fn, &pc, &low)) { jc->bailed = true; return; }
                size_t target = ((size_t)high << 8) | low;
                JitBuffer *buf = &jc->buf;
                emit_load_byte_zx(buf, JIT_REG_RAX, reg_disp(condition, KIND_OFF));
                emit_cmp_imm32_32(buf, JIT_REG_RAX, DIAMOND_VALUE_NIL);
                emit_u8(buf, 0x0F);
                emit_u8(buf, JCC_E);
                size_t nil_field = jc->buf.length;
                emit_u32_le(buf, 0);
                record_global_patch(jc, nil_field, target);
                emit_cmp_imm32_32(buf, JIT_REG_RAX, DIAMOND_VALUE_BOOL);
                size_t not_bool = emit_jcc_placeholder(buf, JCC_NE);
                emit_load_byte_zx(buf, JIT_REG_RAX, reg_disp(condition, AS_OFF));
                emit_test_r8(buf, JIT_REG_RAX);
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
                emit_load_r64(buf, JIT_REG_RAX, reg_disp(source, 0));
                emit_load_r64(buf, JIT_REG_RCX, reg_disp(source, 8));
                /* store into *result via RSI */
                emit_u8(buf, 0x48); emit_u8(buf, 0x89);
                emit_u8(buf, (uint8_t)(0x80 | (JIT_REG_RAX << 3) | JIT_REG_RSI));
                emit_u32_le(buf, 0);
                emit_u8(buf, 0x48); emit_u8(buf, 0x89);
                emit_u8(buf, (uint8_t)(0x80 | (JIT_REG_RCX << 3) | JIT_REG_RSI));
                emit_u32_le(buf, 8);
                emit_mov_al_imm8(buf, 1);
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
    if (function->arity != 0 || function->required_arity != 0 ||
        function->has_variadic || function->owner_class != UINT8_MAX ||
        function->type_variable_count != 0 || function->code_count == 0 ||
        function->register_count > DIAMOND_JIT_MAX_REGISTERS) {
        return nullptr;
    }

    JitCompiler jc = {0};
    jc.function = function;
    jc.bytecode_to_native = malloc(function->code_count * sizeof(size_t));
    if (jc.bytecode_to_native == nullptr) return nullptr;
    for (size_t i = 0; i < function->code_count; i++) jc.bytecode_to_native[i] = SIZE_MAX;

    compile_body(&jc);

    void *result = nullptr;
    if (!jc.bailed && !jc.buf.failed) {
        size_t bailout_offset = jc.buf.length;
        emit_mov_al_imm8(&jc.buf, 0);
        emit_ret(&jc.buf);

        for (size_t i = 0; i < jc.patch_count && !jc.buf.failed; i++) {
            size_t target_native = jc.patches[i].bytecode_target == BAILOUT_SENTINEL
                ? bailout_offset
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
