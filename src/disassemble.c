/* See bignum.c's own identical comment: needed transitively for vm.h's
 * <ucontext.h> use, only under musl (docs/roadmap.md's "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "disassemble.h"
#include <stdlib.h>

static bool require_bytes(FILE *stream, const DiamondChunk *chunk,
                          size_t offset, size_t count) {
    if (offset + count <= chunk->code_count) {
        return true;
    }
    fprintf(stream, "%04zu <truncated instruction>\n", offset);
    return false;
}

static const char *math_function_name(uint8_t id) {
    switch((DiamondMathFunction)id) {
        case DIAMOND_MATH_SQRT: return "sqrt";
        case DIAMOND_MATH_SIN: return "sin";
        case DIAMOND_MATH_COS: return "cos";
        case DIAMOND_MATH_TAN: return "tan";
        case DIAMOND_MATH_POW: return "pow";
        default: return "<invalid math function>";
    }
}

static const char *file_path_function_name(uint8_t id) {
    switch((DiamondFilePathFunction)id) {
        case DIAMOND_FILE_PATH_DIRNAME: return "dirname";
        case DIAMOND_FILE_PATH_BASENAME: return "basename";
        case DIAMOND_FILE_PATH_EXTNAME: return "extname";
        case DIAMOND_FILE_PATH_ABSOLUTE: return "absolute?";
        case DIAMOND_FILE_PATH_EXPAND: return "expand_path";
        default: return "<invalid file path function>";
    }
}

/* Every operand read here is 2 bytes, big-endian, matching emit_register/
 * emit_instruction's own uniform widening (src/compiler.c) -- including
 * operands that are logically a narrower index (a constant/string/type-set/
 * capture index, an enum id, a boolean flag, ...) rather than a register,
 * since emit_instruction doesn't distinguish between them either. See its
 * own comment for why. */
static uint16_t read_operand(const DiamondChunk *chunk, size_t offset) {
    return (uint16_t)(((unsigned)chunk->code[offset] << 8) | chunk->code[offset + 1]);
}

/* Ordinary compiler-emitted bytecode can never reference a register past
 * its own function's high-water mark (allocate_register guarantees it by
 * construction) -- but ProgramBuilder-emitted bytecode bypasses the
 * compiler entirely (emit_byte just appends a raw byte with no idea what
 * instruction it's part of), so a register-bearing operand here is the
 * one thing this walk can't just trust. chunk->register_count==0 mirrors
 * run_chunk's own fallback for a hand-authored DiamondChunk that predates
 * the field: treat it as the full DIAMOND_REGISTER_COUNT width rather
 * than rejecting every register outright. */
static bool register_in_range(const DiamondChunk *chunk, uint16_t value) {
    const size_t effective = chunk->register_count == 0
        ? DIAMOND_REGISTER_COUNT : chunk->register_count;
    return (size_t)value < effective;
}

static uint16_t checked_register(const DiamondChunk *chunk, FILE *stream,
                                 uint16_t value, bool *valid) {
    if (!register_in_range(chunk, value)) {
        fprintf(stream, "<register r%u out of range> ", value);
        *valid = false;
    }
    return value;
}

/* CALL/CALL_TYPED/CALL_CLOSURE/NEW/INVOKE (any variant)/SUPER/THREAD_NEW all pass a
 * contiguous run of `count` registers starting at `base` to the callee
 * (run_chunk gets &registers[base] and reads `count` values forward from
 * there) -- checking `base` alone the way checked_register does isn't
 * enough, since base itself can be well in range while base+count still
 * walks off the end of a small function's register array. */
static uint16_t checked_register_range(const DiamondChunk *chunk, FILE *stream,
                                       uint16_t base, size_t count, bool *valid) {
    const size_t effective = chunk->register_count == 0
        ? DIAMOND_REGISTER_COUNT : chunk->register_count;
    if ((size_t)base + count > effective) {
        fprintf(stream, "<register range r%u..%zu out of range> ",
                base, (size_t)base + count);
        *valid = false;
    }
    return base;
}

static size_t one_register(FILE *stream, const DiamondChunk *chunk,
                           const char *name, size_t offset, bool *valid) {
    if (!require_bytes(stream, chunk, offset, 3)) return chunk->code_count;
    const uint16_t reg = checked_register(chunk, stream,
        read_operand(chunk, offset + 1), valid);
    fprintf(stream, "%-18s r%u\n", name, reg);
    return offset + 3;
}

static size_t two_registers(FILE *stream, const DiamondChunk *chunk,
                            const char *name, size_t offset, bool *valid) {
    if (!require_bytes(stream, chunk, offset, 5)) return chunk->code_count;
    const uint16_t first = checked_register(chunk, stream,
        read_operand(chunk, offset + 1), valid);
    const uint16_t second = checked_register(chunk, stream,
        read_operand(chunk, offset + 3), valid);
    fprintf(stream, "%-18s r%u, r%u\n", name, first, second);
    return offset + 5;
}

static size_t three_registers(FILE *stream, const DiamondChunk *chunk,
                              const char *name, size_t offset, bool *valid) {
    if (!require_bytes(stream, chunk, offset, 7)) return chunk->code_count;
    const uint16_t first = checked_register(chunk, stream,
        read_operand(chunk, offset + 1), valid);
    const uint16_t second = checked_register(chunk, stream,
        read_operand(chunk, offset + 3), valid);
    const uint16_t third = checked_register(chunk, stream,
        read_operand(chunk, offset + 5), valid);
    fprintf(stream, "%-18s r%u, r%u, r%u\n", name, first, second, third);
    return offset + 7;
}

static size_t four_registers(FILE *stream, const DiamondChunk *chunk,
                             const char *name, size_t offset, bool *valid) {
    if (!require_bytes(stream, chunk, offset, 9)) return chunk->code_count;
    const uint16_t first = checked_register(chunk, stream,
        read_operand(chunk, offset + 1), valid);
    const uint16_t second = checked_register(chunk, stream,
        read_operand(chunk, offset + 3), valid);
    const uint16_t third = checked_register(chunk, stream,
        read_operand(chunk, offset + 5), valid);
    const uint16_t fourth = checked_register(chunk, stream,
        read_operand(chunk, offset + 7), valid);
    fprintf(stream, "%-18s r%u, r%u, r%u, r%u\n", name, first, second, third, fourth);
    return offset + 9;
}

/* TLS_LISTEN needs five (dest, port, cert, key, options). */
static size_t five_registers(FILE *stream, const DiamondChunk *chunk,
                             const char *name, size_t offset, bool *valid) {
    if (!require_bytes(stream, chunk, offset, 11)) return chunk->code_count;
    const uint16_t first = checked_register(chunk, stream,
        read_operand(chunk, offset + 1), valid);
    const uint16_t second = checked_register(chunk, stream,
        read_operand(chunk, offset + 3), valid);
    const uint16_t third = checked_register(chunk, stream,
        read_operand(chunk, offset + 5), valid);
    const uint16_t fourth = checked_register(chunk, stream,
        read_operand(chunk, offset + 7), valid);
    const uint16_t fifth = checked_register(chunk, stream,
        read_operand(chunk, offset + 9), valid);
    fprintf(stream, "%-18s r%u, r%u, r%u, r%u, r%u\n", name,
        first, second, third, fourth, fifth);
    return offset + 11;
}

/* Only MYSQL_OPEN needs more than five register operands (dest plus
 * host/user/password/database/port), so this is a one-off rather than a
 * generalized n_registers helper -- same shape as two_registers/
 * three_registers above, just six wide. */
static size_t six_registers(FILE *stream, const DiamondChunk *chunk,
                            const char *name, size_t offset, bool *valid) {
    if (!require_bytes(stream, chunk, offset, 13)) return chunk->code_count;
    const uint16_t first = checked_register(chunk, stream,
        read_operand(chunk, offset + 1), valid);
    const uint16_t second = checked_register(chunk, stream,
        read_operand(chunk, offset + 3), valid);
    const uint16_t third = checked_register(chunk, stream,
        read_operand(chunk, offset + 5), valid);
    const uint16_t fourth = checked_register(chunk, stream,
        read_operand(chunk, offset + 7), valid);
    const uint16_t fifth = checked_register(chunk, stream,
        read_operand(chunk, offset + 9), valid);
    const uint16_t sixth = checked_register(chunk, stream,
        read_operand(chunk, offset + 11), valid);
    fprintf(stream, "%-18s r%u, r%u, r%u, r%u, r%u, r%u", name,
        first, second, third, fourth, fifth, sixth);
    fputc('\n', stream);
    return offset + 13;
}

static bool print_type_set(FILE *stream,const DiamondChunk *chunk,
                           uint16_t set_index) {
    if((size_t)set_index>=chunk->type_set_count) {
        fputs("<invalid type set>",stream);return false;
    }
    bool valid=true;const DiamondTypeSet *set=&chunk->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        if(index>0)fputs(" | ",stream);
        const DiamondTypeMember member=set->members[index];
        const uint8_t type=member.id;
        if(type==DIAMOND_TYPE_INT) fputs("Int",stream);
        else if(type==DIAMOND_TYPE_FLOAT) fputs("Float",stream);
        else if(type==DIAMOND_TYPE_STRING) fputs("String",stream);
        else if(type==DIAMOND_TYPE_BOOL) fputs("Bool",stream);
        else if(type==DIAMOND_TYPE_NIL) fputs("Nil",stream);
        else if(type==DIAMOND_TYPE_ARRAY) fputs("Array",stream);
        else if(type==DIAMOND_TYPE_HASH) fputs("Hash",stream);
        else if(type==DIAMOND_TYPE_CALLABLE) fputs("Callable",stream);
        else if(type==DIAMOND_TYPE_SIZED) fputs("Sized",stream);
        else if(type==DIAMOND_TYPE_SYMBOL) fputs("Symbol",stream);
        else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE)
            fprintf(stream,"T%u",type-DIAMOND_TYPE_VARIABLE_BASE);
        else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
                (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
            fputs(chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE].name,stream);
        else if((size_t)(type-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count)
            fputs(chunk->classes[type-DIAMOND_TYPE_CLASS_BASE].name,stream);
        else {fputs("<invalid type>",stream);valid=false;}
        if(member.argument_set!=DIAMOND_NO_TYPE_SET) {
            fputc('[',stream);
            valid=print_type_set(stream,chunk,member.argument_set)&&valid;
            if(member.second_argument_set!=DIAMOND_NO_TYPE_SET) {
                fputs(", ",stream);
                valid=print_type_set(stream,chunk,member.second_argument_set)&&valid;
            }
            fputc(']',stream);
        } else if(member.id==DIAMOND_TYPE_CALLABLE&&
                  member.callable_arity!=UINT8_MAX) {
            fputc('[',stream);
            if(member.callable_parameters_typed) {
                fputc('[',stream);
                for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                    if(parameter>0)fputs(", ",stream);
                    valid=print_type_set(stream,chunk,
                        member.callable_parameter_sets[parameter])&&valid;
                }
                fputc(']',stream);
            } else fprintf(stream,"%u",member.callable_arity);
            if(member.callable_return_set!=DIAMOND_NO_TYPE_SET) {
                fputs(", ",stream);
                valid=print_type_set(stream,chunk,member.callable_return_set)&&valid;
            }
            fputc(']',stream);
        }
    }
    return valid;
}

bool diamond_print_type_set(FILE *stream, const DiamondChunk *chunk, uint16_t set_index) {
    return print_type_set(stream, chunk, set_index);
}

static bool disassemble_chunk(FILE *stream, const char *name,
                              const DiamondChunk *chunk) {
    fprintf(stream, "== %s ==\n", name);
    size_t offset = 0;
    bool valid = true;

    /* Every offset this walk visits as the start of an instruction gets
     * marked here, so every jump-like target (recorded below, checked
     * once the walk finishes and this is fully populated) can be
     * confirmed to land on a real instruction rather than into the
     * middle of one -- landing mid-instruction would make the *runtime*
     * interpret a completely different byte as the next opcode than
     * anything this walk ever validated, register-bounds checks
     * included, since ProgramBuilder-emitted bytecode can construct a
     * jump to any offset in [0, code_count]. Sized to DIAMOND_MAX_CODE
     * (this function's own code[] bound), not chunk->code_count, so
     * `offset < chunk->code_count` below never risks writing past it. */
    bool starts[DIAMOND_MAX_CODE] = {};
    size_t pending_targets[DIAMOND_MAX_CODE];
    size_t pending_count = 0;

    while (offset < chunk->code_count) {
        starts[offset] = true;
        if (chunk->lines != nullptr && chunk->lines[offset] != 0) {
            fprintf(stream, "%04zu %4u:%-3u ", offset, chunk->lines[offset],
                    chunk->columns[offset]);
        } else {
            fprintf(stream, "%04zu          ", offset);
        }
        const DiamondOpCode opcode = (DiamondOpCode)chunk->code[offset];
        switch (opcode) {
            case DIAMOND_OP_CONSTANT: {
                if (!require_bytes(stream, chunk, offset, 5)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const uint16_t destination =
                    checked_register(chunk, stream, read_operand(chunk, offset + 1), &valid);
                const uint16_t constant = read_operand(chunk, offset + 3);
                fprintf(stream, "%-18s r%u, k%u", "CONSTANT", destination,
                        constant);
                if ((size_t)constant < chunk->constant_count) {
                    fputs(" (", stream);
                    diamond_value_fprint(stream, chunk->constants[constant]);
                    fputc(')', stream);
                } else {
                    fputs(" <invalid constant>", stream);
                    valid = false;
                }
                fputc('\n', stream);
                offset += 5;
                break;
            }
            case DIAMOND_OP_STRING: {
                if (!require_bytes(stream, chunk, offset, 5)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const uint16_t destination =
                    checked_register(chunk, stream, read_operand(chunk, offset + 1), &valid);
                const uint16_t string = read_operand(chunk, offset + 3);
                fprintf(stream, "%-18s r%u, s%u", "STRING", destination, string);
                if ((size_t)string < chunk->string_count) {
                    const DiamondStringConstant *constant = &chunk->strings[string];
                    fprintf(stream, " (\"%.*s\")", (int)constant->length,
                            constant->chars);
                } else {
                    fputs(" <invalid string>", stream);
                    valid = false;
                }
                fputc('\n', stream);
                offset += 5;
                break;
            }
            case DIAMOND_OP_SYMBOL: {
                if (!require_bytes(stream, chunk, offset, 5)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const uint16_t destination =
                    checked_register(chunk, stream, read_operand(chunk, offset + 1), &valid);
                const uint16_t string = read_operand(chunk, offset + 3);
                fprintf(stream, "%-18s r%u, s%u", "SYMBOL", destination, string);
                if ((size_t)string < chunk->string_count) {
                    const DiamondStringConstant *constant = &chunk->strings[string];
                    fprintf(stream, " (:%.*s)", (int)constant->length,
                            constant->chars);
                } else {
                    fputs(" <invalid string>", stream);
                    valid = false;
                }
                fputc('\n', stream);
                offset += 5;
                break;
            }
            case DIAMOND_OP_NIL:
                offset = one_register(stream, chunk, "NIL", offset, &valid);
                break;
            case DIAMOND_OP_BOOL:
                if (!require_bytes(stream, chunk, offset, 5)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                fprintf(stream, "%-18s r%u, %s\n", "BOOL",
                        checked_register(chunk, stream, read_operand(chunk, offset + 1), &valid),
                        read_operand(chunk, offset + 3) ? "true" : "false");
                offset += 5;
                break;
            case DIAMOND_OP_ARGUMENT_PROVIDED:
                /* Second operand is a parameter position (checked against
                 * argument_count at runtime, DIAMOND_OP_ARGUMENT_PROVIDED
                 * in vm.c), not a register -- despite sharing two_
                 * registers' 2-byte-per-operand shape (both are uniform-
                 * width per emit_instruction, see read_operand's own
                 * comment), it isn't bounded by register_count. */
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, %u\n","ARGUMENT_PROVIDED",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3));
                offset+=5;
                break;
            case DIAMOND_OP_COLLECT_VARIADIC:
                /* The final operands are fixed and preserved-trailing
                 * parameter counts, not registers -- same reasoning as
                 * ARGUMENT_PROVIDED just above. */
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, fixed=%u, trailing=%u\n","COLLECT_VARIADIC",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3),read_operand(chunk,offset+5));
                offset+=7;
                break;
            case DIAMOND_OP_TO_STRING:
                offset=two_registers(stream,chunk,"TO_STRING",offset, &valid);break;
            case DIAMOND_OP_MOVE:
                offset = two_registers(stream, chunk, "MOVE", offset, &valid);
                break;
            case DIAMOND_OP_ADD:
                offset = three_registers(stream, chunk, "ADD", offset, &valid);
                break;
            case DIAMOND_OP_ADD_INT:
                offset = three_registers(stream, chunk, "ADD_INT", offset, &valid);
                break;
            case DIAMOND_OP_SUBTRACT:
                offset = three_registers(stream, chunk, "SUBTRACT", offset, &valid);
                break;
            case DIAMOND_OP_MULTIPLY:
                offset = three_registers(stream, chunk, "MULTIPLY", offset, &valid);
                break;
            case DIAMOND_OP_DIVIDE:
                offset = three_registers(stream, chunk, "DIVIDE", offset, &valid);
                break;
            case DIAMOND_OP_SUBTRACT_INT:
                offset = three_registers(stream, chunk, "SUBTRACT_INT", offset, &valid);
                break;
            case DIAMOND_OP_MULTIPLY_INT:
                offset = three_registers(stream, chunk, "MULTIPLY_INT", offset, &valid);
                break;
            case DIAMOND_OP_DIVIDE_INT:
                offset = three_registers(stream, chunk, "DIVIDE_INT", offset, &valid);
                break;
            case DIAMOND_OP_LESS:
                offset = three_registers(stream, chunk, "LESS", offset, &valid);
                break;
            case DIAMOND_OP_LESS_EQUAL:
                offset = three_registers(stream, chunk, "LESS_EQUAL", offset, &valid);
                break;
            case DIAMOND_OP_GREATER:
                offset = three_registers(stream, chunk, "GREATER", offset, &valid);
                break;
            case DIAMOND_OP_GREATER_EQUAL:
                offset = three_registers(stream, chunk, "GREATER_EQUAL", offset, &valid);
                break;
            case DIAMOND_OP_NEGATE:
                offset = two_registers(stream, chunk, "NEGATE", offset, &valid);
                break;
            case DIAMOND_OP_EQUAL:
                offset = three_registers(stream, chunk, "EQUAL", offset, &valid);
                break;
            case DIAMOND_OP_NOT_EQUAL:
                offset = three_registers(stream, chunk, "NOT_EQUAL", offset, &valid);
                break;
            case DIAMOND_OP_EQUAL_INT:
                offset = three_registers(stream, chunk, "EQUAL_INT", offset, &valid);
                break;
            case DIAMOND_OP_NOT_EQUAL_INT:
                offset = three_registers(stream, chunk, "NOT_EQUAL_INT", offset, &valid);
                break;
            case DIAMOND_OP_LESS_INT:
                offset = three_registers(stream, chunk, "LESS_INT", offset, &valid);
                break;
            case DIAMOND_OP_LESS_EQUAL_INT:
                offset = three_registers(stream, chunk, "LESS_EQUAL_INT", offset, &valid);
                break;
            case DIAMOND_OP_GREATER_INT:
                offset = three_registers(stream, chunk, "GREATER_INT", offset, &valid);
                break;
            case DIAMOND_OP_GREATER_EQUAL_INT:
                offset = three_registers(stream, chunk, "GREATER_EQUAL_INT", offset, &valid);
                break;
            case DIAMOND_OP_JUMP: {
                if (!require_bytes(stream, chunk, offset, 3)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const size_t target = ((size_t)chunk->code[offset + 1] << 8) |
                                      chunk->code[offset + 2];
                fprintf(stream, "%-18s -> %04zu\n", "JUMP", target);
                if (target > chunk->code_count) valid = false;
                else pending_targets[pending_count++] = target;
                offset += 3;
                break;
            }
            case DIAMOND_OP_JUMP_IF_FALSE: {
                if (!require_bytes(stream, chunk, offset, 5)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const size_t target = ((size_t)chunk->code[offset + 3] << 8) |
                                      chunk->code[offset + 4];
                fprintf(stream, "%-18s r%u -> %04zu\n", "JUMP_IF_FALSE",
                        checked_register(chunk, stream, read_operand(chunk, offset + 1), &valid),
                        target);
                if (target > chunk->code_count) valid = false;
                else pending_targets[pending_count++] = target;
                offset += 5;
                break;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: {
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                fprintf(stream,"%-18s r%u -> %04zu\n","JUMP_IF_TRUE",
                    checked_register(chunk, stream, read_operand(chunk, offset+1), &valid),target);
                if(target>chunk->code_count) valid=false;
                else pending_targets[pending_count++] = target;
                offset+=5;
                break;
            }
            case DIAMOND_OP_CALL: {
                if (!require_bytes(stream, chunk, offset, 8)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                fprintf(stream, "%-18s r%u, f%zu, r%u, %u args\n", "CALL",
                        checked_register(chunk, stream, read_operand(chunk, offset + 1), &valid),
                        function_index,
                        checked_register_range(chunk, stream, read_operand(chunk, offset + 5),
                            chunk->code[offset + 7], &valid),
                        chunk->code[offset + 7]);
                if (function_index >= chunk->function_count) {
                    valid = false;
                }
                offset += 8;
                break;
            }
            case DIAMOND_OP_CALL_SPREAD: {
                if(!require_bytes(stream,chunk,offset,7)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                fprintf(stream,"%-18s r%u, f%zu, r%u\n","CALL_SPREAD",
                        checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                        function_index,
                        checked_register(chunk,stream,read_operand(chunk,offset+5),&valid));
                if(function_index>=chunk->function_count) valid=false;
                offset+=7;
                break;
            }
            case DIAMOND_OP_CALL_TYPED_SPREAD: {
                if(!require_bytes(stream,chunk,offset,8)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                const uint8_t type_count=chunk->code[offset+7];
                if(!require_bytes(stream,chunk,offset,(size_t)8+type_count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, f%zu, r%u, %u types\n",
                    "CALL_TYPED_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    type_count);
                if(function_index>=chunk->function_count||type_count>8)
                    valid=false;
                for(size_t index=0;index<type_count;index++)
                    if((size_t)read_operand(chunk,offset+8+index*2)>=chunk->type_set_count)
                        valid=false;
                offset+=(size_t)8+type_count*2;break;
            }
            case DIAMOND_OP_CALL_KEYWORD_SPREAD:
            case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD: {
                if(!require_bytes(stream,chunk,offset,8)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const size_t function_index=read_operand(chunk,offset+3);
                const uint8_t keyword_count=chunk->code[offset+7];
                const size_t keyword_end=8+(size_t)keyword_count*3;
                const bool typed=(DiamondOpCode)opcode==
                    DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD;
                if(keyword_count==0||keyword_count>16||
                   !require_bytes(stream,chunk,offset,keyword_end+(typed?1:0))) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, f%zu, r%u, %u keywords\n",
                    typed?"CALL_TYPED_KW_SPREAD":"CALL_KW_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    keyword_count);
                if(function_index>=chunk->function_count)valid=false;
                for(size_t index=0;index<keyword_count;index++) {
                    const size_t entry=offset+8+index*3;
                    const uint8_t slot=chunk->code[entry];
                    const uint16_t value=checked_register(chunk,stream,
                        read_operand(chunk,entry+1),&valid);
                    fprintf(stream,"                     slot %u <- r%u\n",slot,value);
                }
                if(!typed) {offset+=keyword_end;break;}
                const uint8_t type_count=chunk->code[offset+keyword_end];
                if(type_count>8||!require_bytes(stream,chunk,offset,
                        keyword_end+1+(size_t)type_count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                for(size_t index=0;index<type_count;index++)
                    if((size_t)read_operand(chunk,offset+keyword_end+1+index*2)>=
                       chunk->type_set_count)valid=false;
                offset+=keyword_end+1+(size_t)type_count*2;break;
            }
            case DIAMOND_OP_INVOKE_SPREAD: {
                if(!require_bytes(stream,chunk,offset,9)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint16_t method_name=read_operand(chunk,offset+5);
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u\n","INVOKE_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    method_name,
                    checked_register(chunk,stream,read_operand(chunk,offset+7),&valid));
                if((size_t)method_name>=chunk->string_count)valid=false;
                offset+=9;break;
            }
            case DIAMOND_OP_INVOKE_KEYWORDS:
            case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: {
                if(!require_bytes(stream,chunk,offset,10)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint16_t method_name=read_operand(chunk,offset+5);
                const uint8_t encoded_keyword_count=chunk->code[offset+9];
                const bool has_block=(encoded_keyword_count&0x80u)!=0;
                const uint8_t keyword_count=encoded_keyword_count&0x7fu;
                const size_t keyword_end=10+(size_t)keyword_count*4+
                    (has_block?2u:0u);
                const bool typed=(DiamondOpCode)opcode==
                    DIAMOND_OP_INVOKE_TYPED_KEYWORDS;
                if(keyword_count==0||keyword_count>16||
                   !require_bytes(stream,chunk,offset,keyword_end+(typed?1:0))) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u, %u keywords\n",
                    typed?"INVOKE_TYPED_KW":"INVOKE_KEYWORDS",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    method_name,
                    checked_register(chunk,stream,read_operand(chunk,offset+7),&valid),
                    keyword_count);
                if((size_t)method_name>=chunk->string_count)valid=false;
                for(size_t index=0;index<keyword_count;index++) {
                    const size_t entry=offset+10+index*4;
                    if((size_t)read_operand(chunk,entry)>=chunk->string_count)valid=false;
                    (void)checked_register(chunk,stream,
                        read_operand(chunk,entry+2),&valid);
                }
                if(has_block)(void)checked_register(chunk,stream,
                    read_operand(chunk,keyword_end-2),&valid);
                if(!typed) {offset+=keyword_end;break;}
                const uint8_t type_count=chunk->code[offset+keyword_end];
                if(type_count>8||!require_bytes(stream,chunk,offset,
                        keyword_end+1+(size_t)type_count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                for(size_t index=0;index<type_count;index++)
                    if((size_t)read_operand(chunk,offset+keyword_end+1+index*2)>=
                       chunk->type_set_count)valid=false;
                offset+=keyword_end+1+(size_t)type_count*2;break;
            }
            case DIAMOND_OP_INVOKE_TYPED_SPREAD: {
                if(!require_bytes(stream,chunk,offset,10)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint16_t method_name=read_operand(chunk,offset+5);
                const uint8_t type_count=chunk->code[offset+9];
                if(!require_bytes(stream,chunk,offset,(size_t)10+type_count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u, %u types\n",
                    "INVOKE_TYPED_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    method_name,
                    checked_register(chunk,stream,read_operand(chunk,offset+7),&valid),
                    type_count);
                if((size_t)method_name>=chunk->string_count||type_count>8)
                    valid=false;
                for(size_t index=0;index<type_count;index++)
                    if((size_t)read_operand(chunk,offset+10+index*2)>=chunk->type_set_count)
                        valid=false;
                offset+=(size_t)10+type_count*2;break;
            }
            case DIAMOND_OP_CALL_CLOSURE_SPREAD: {
                if(!require_bytes(stream,chunk,offset,7)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, r%u, r%u\n","CALL_CLOSURE_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid));
                offset+=7;break;
            }
            case DIAMOND_OP_CALL_CLOSURE_KEYWORDS: {
                if(!require_bytes(stream,chunk,offset,8)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t encoded_keyword_count=chunk->code[offset+7];
                const bool has_block=(encoded_keyword_count&0x80u)!=0;
                const uint8_t keyword_count=encoded_keyword_count&0x7fu;
                const size_t total=8+(size_t)keyword_count*4+(has_block?2u:0u);
                if(keyword_count==0||keyword_count>16||
                   !require_bytes(stream,chunk,offset,total)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, r%u, r%u, %u keywords\n",
                    "CALL_CLOSURE_KW",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    keyword_count);
                for(size_t index=0;index<keyword_count;index++) {
                    const size_t entry=offset+8+index*4;
                    if((size_t)read_operand(chunk,entry)>=chunk->string_count)valid=false;
                    (void)checked_register(chunk,stream,
                        read_operand(chunk,entry+2),&valid);
                }
                if(has_block)(void)checked_register(chunk,stream,
                    read_operand(chunk,total-2),&valid);
                offset+=total;break;
            }
            case DIAMOND_OP_NEW_SPREAD: {
                if(!require_bytes(stream,chunk,offset,6)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t class_index=chunk->code[offset+3];
                fprintf(stream,"%-18s r%u, c%u, r%u\n","NEW_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    class_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+4),&valid));
                if((size_t)class_index>=chunk->class_count)valid=false;
                offset+=6;break;
            }
            case DIAMOND_OP_NEW_KEYWORDS: {
                if(!require_bytes(stream,chunk,offset,7)) {valid=false;offset=chunk->code_count;break;}
                const uint8_t class_index=chunk->code[offset+3];
                const uint8_t encoded_keyword_count=chunk->code[offset+6];
                const bool has_block=(encoded_keyword_count&0x80u)!=0;
                const uint8_t keyword_count=encoded_keyword_count&0x7fu;
                const size_t total=7+(size_t)keyword_count*4+(has_block?2u:0u);
                if(keyword_count==0||keyword_count>16||
                   !require_bytes(stream,chunk,offset,total)) {valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, c%u, r%u, %u keywords\n","NEW_KEYWORDS",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),class_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+4),&valid),keyword_count);
                if((size_t)class_index>=chunk->class_count)valid=false;
                for(size_t index=0;index<keyword_count;index++) {
                    const size_t entry=offset+7+index*4;
                    if((size_t)read_operand(chunk,entry)>=chunk->string_count)valid=false;
                    (void)checked_register(chunk,stream,read_operand(chunk,entry+2),&valid);
                }
                if(has_block)(void)checked_register(chunk,stream,
                    read_operand(chunk,total-2),&valid);
                offset+=total;break;
            }
            case DIAMOND_OP_CALL_SINGLETON_SPREAD: {
                if(!require_bytes(stream,chunk,offset,9)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                const uint8_t class_index=chunk->code[offset+7];
                const uint8_t needs_receiver=chunk->code[offset+8];
                fprintf(stream,"%-18s r%u, f%zu, r%u, c%u, self=%u\n",
                    "CALL_SINGLETON_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    class_index,needs_receiver);
                if(function_index>=chunk->function_count||needs_receiver>1||
                   (class_index!=UINT8_MAX&&(size_t)class_index>=chunk->class_count))
                    valid=false;
                offset+=9;break;
            }
            case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: {
                if(!require_bytes(stream,chunk,offset,10)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                const uint8_t class_index=chunk->code[offset+7];
                const uint8_t needs_receiver=chunk->code[offset+8];
                const uint8_t type_count=chunk->code[offset+9];
                if(!require_bytes(stream,chunk,offset,(size_t)10+type_count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, f%zu, r%u, c%u, self=%u, %u types\n",
                    "CALL_TYPED_SINGLETON_SPREAD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    class_index,needs_receiver,type_count);
                if(function_index>=chunk->function_count||needs_receiver>1||
                   type_count>8||(class_index!=UINT8_MAX&&
                    (size_t)class_index>=chunk->class_count))valid=false;
                for(size_t index=0;index<type_count;index++)
                    if((size_t)read_operand(chunk,offset+10+index*2)>=chunk->type_set_count)
                        valid=false;
                offset+=(size_t)10+type_count*2;break;
            }
            case DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
            case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS: {
                if(!require_bytes(stream,chunk,offset,10)) {valid=false;offset=chunk->code_count;break;}
                const size_t function_index=read_operand(chunk,offset+3);
                const uint8_t keyword_count=chunk->code[offset+9];
                const size_t keyword_end=10+(size_t)keyword_count*4;
                const bool typed=(DiamondOpCode)opcode==DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS;
                if(keyword_count==0||keyword_count>16||
                   !require_bytes(stream,chunk,offset,keyword_end+(typed?1:0))) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, f%zu, r%u, %u keywords\n",
                    typed?"CALL_TYPED_SINGLE_KW":"CALL_SINGLETON_KW",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),keyword_count);
                if(function_index>=chunk->function_count)valid=false;
                for(size_t index=0;index<keyword_count;index++) {
                    const size_t entry=offset+10+index*4;
                    if((size_t)read_operand(chunk,entry)>=chunk->string_count)valid=false;
                    (void)checked_register(chunk,stream,read_operand(chunk,entry+2),&valid);
                }
                if(!typed) {offset+=keyword_end;break;}
                const uint8_t type_count=chunk->code[offset+keyword_end];
                if(type_count>8||!require_bytes(stream,chunk,offset,
                        keyword_end+1+(size_t)type_count*2)) {valid=false;offset=chunk->code_count;break;}
                for(size_t index=0;index<type_count;index++)
                    if((size_t)read_operand(chunk,offset+keyword_end+1+index*2)>=
                       chunk->type_set_count)valid=false;
                offset+=keyword_end+1+(size_t)type_count*2;break;
            }
            case DIAMOND_OP_BUILD_SPREAD_ARGS: {
                if(!require_bytes(stream,chunk,offset,11)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t prefix_count=chunk->code[offset+5];
                const uint8_t encoded_suffix_count=chunk->code[offset+10];
                const uint8_t suffix_count=encoded_suffix_count&0x7fu;
                const bool optional_block=(encoded_suffix_count&0x80u)!=0;
                if(optional_block&&suffix_count==0)valid=false;
                fprintf(stream,"%-18s r%u, r%u/%u, *r%u, r%u/%u%s\n",
                    "BUILD_SPREAD_ARGS",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+3),
                        prefix_count,&valid),prefix_count,
                    checked_register(chunk,stream,read_operand(chunk,offset+6),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+8),
                        suffix_count,&valid),suffix_count,
                    optional_block?" optional-block":"");
                offset+=11;break;
            }
            case DIAMOND_OP_CALL_TYPED: {
                if(!require_bytes(stream,chunk,offset,9)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t count=chunk->code[offset+8];
                if(!require_bytes(stream,chunk,offset,(size_t)9+count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                fprintf(stream,"%-18s r%u, f%zu, r%u, %u args, [",
                    "CALL_TYPED",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,
                    checked_register_range(chunk,stream,read_operand(chunk,offset+5),
                        chunk->code[offset+7],&valid),
                    chunk->code[offset+7]);
                for(size_t index=0;index<count;index++) {
                    if(index>0)fputs(", ",stream);
                    const uint16_t set=read_operand(chunk,offset+9+index*2);
                    valid=print_type_set(stream,chunk,set)&&valid;
                }
                fputs("]\n",stream);
                if(function_index>=chunk->function_count)
                    valid=false;
                offset+=(size_t)9+count*2;
                break;
            }
            case DIAMOND_OP_CALL_CLOSURE:
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, r%u, %u args\n","CALL_CLOSURE",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+5),
                        chunk->code[offset+7],&valid),
                    chunk->code[offset+7]);
                offset+=8;break;
            /* GET_CAPTURE/GET_CAPTURE_CELL's second operand and SET_
             * CAPTURE's first are a capture-slot index (checked against
             * closure->capture_count at runtime), not a register -- same
             * uniform-width-but-not-a-register situation as ARGUMENT_
             * PROVIDED above. */
            case DIAMOND_OP_GET_CAPTURE:
            case DIAMOND_OP_GET_CAPTURE_CELL:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, c%u\n",
                    chunk->code[offset]==DIAMOND_OP_GET_CAPTURE
                        ? "GET_CAPTURE" : "GET_CAPTURE_CELL",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3));
                offset+=5;break;
            case DIAMOND_OP_SET_CAPTURE:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s c%u, r%u\n","SET_CAPTURE",
                    read_operand(chunk,offset+1),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid));
                offset+=5;break;
            case DIAMOND_OP_BOX_LOCAL:
                offset=one_register(stream,chunk,"BOX_LOCAL",offset, &valid);break;
            case DIAMOND_OP_GET_CELL:
                offset=two_registers(stream,chunk,"GET_CELL",offset, &valid);break;
            case DIAMOND_OP_SET_CELL:
                offset=two_registers(stream,chunk,"SET_CELL",offset, &valid);break;
            case DIAMOND_OP_CLOSURE: {
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                const size_t count=chunk->code[offset+5];
                if(!require_bytes(stream,chunk,offset,6+count*2)){valid=false;offset=chunk->code_count;break;}
                const size_t function_index=
                    ((size_t)chunk->code[offset+3]<<8)|chunk->code[offset+4];
                fprintf(stream,"%-18s r%u, f%zu, %zu captures\n","CLOSURE",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    function_index,count);
                for(size_t index=0;index<count;index++)
                    checked_register(chunk,stream,read_operand(chunk,offset+6+index*2),&valid);
                offset+=6+count*2;break;
            }
            case DIAMOND_OP_NEW:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, r%u, %u args\n","NEW",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3],
                    checked_register_range(chunk,stream,read_operand(chunk,offset+4),
                        chunk->code[offset+6],&valid),
                    chunk->code[offset+6]);
                offset+=7;break;
            case DIAMOND_OP_REDEFINE_METHOD:
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, r%u, r%u\n","REDEFINE_METHOD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3],
                    checked_register(chunk,stream,read_operand(chunk,offset+4),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+6),&valid));
                offset+=8;break;
            case DIAMOND_OP_DEFINE_METHOD:
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, r%u, r%u\n","DEFINE_METHOD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3],
                    checked_register(chunk,stream,read_operand(chunk,offset+4),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+6),&valid));
                offset+=8;break;
            case DIAMOND_OP_COMPILE_METHOD:
                if(!require_bytes(stream,chunk,offset,12)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, r%u, r%u, r%u, r%u\n","COMPILE_METHOD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3],
                    checked_register(chunk,stream,read_operand(chunk,offset+4),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+6),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+8),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+10),&valid));
                offset+=12;break;
            case DIAMOND_OP_LOAD_CLASS:
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u\n","LOAD_CLASS",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3]);
                offset+=4;break;
            case DIAMOND_OP_INVOKE_SELF_METHOD:
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                if((size_t)read_operand(chunk,offset+3)>=chunk->string_count)
                    valid=false;
                fprintf(stream,"%-18s r%u, s%u, r%u, %u args\n","INVOKE_SELF_METHOD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    chunk->code[offset+7]);
                offset+=8;break;
            case DIAMOND_OP_YIELD:
                offset=two_registers(stream,chunk,"YIELD",offset, &valid);break;
            case DIAMOND_OP_FIBER_NEW:
                offset=two_registers(stream,chunk,"FIBER_NEW",offset, &valid);break;
            case DIAMOND_OP_THREAD_NEW:
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, r%u, %u args\n","THREAD_NEW",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+5),
                        chunk->code[offset+7],&valid),
                    chunk->code[offset+7]);
                offset+=8;break;
            case DIAMOND_OP_PRINT:
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, newline=%u\n","PRINT",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    chunk->code[offset+5]);
                offset+=6;break;
            case DIAMOND_OP_FILE_JOIN:
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, %u args\n","FILE_JOIN",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+3),
                        chunk->code[offset+5],&valid),
                    chunk->code[offset+5]);
                offset+=6;break;
            case DIAMOND_OP_TIME_PARSE:
                offset=two_registers(stream,chunk,"TIME_PARSE",offset,&valid);break;
            case DIAMOND_OP_TIME_BUILD:
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, mode=%u\n","TIME_BUILD",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+3),
                        chunk->code[offset+5]==1?7:6,&valid),chunk->code[offset+5]);
                offset+=6;break;
            case DIAMOND_OP_FILE_PATH: {
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, r%u, %s\n","FILE_PATH",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    file_path_function_name(chunk->code[offset+7]));
                offset+=8;break;
            }
            case DIAMOND_OP_GETS:
                offset=one_register(stream,chunk,"GETS",offset, &valid);break;
            case DIAMOND_OP_FILE_OPEN:
                offset=three_registers(stream,chunk,"FILE_OPEN",offset, &valid);break;
            case DIAMOND_OP_FILE_DELETE:
                offset=two_registers(stream,chunk,"FILE_DELETE",offset, &valid);break;
            case DIAMOND_OP_TCP_CONNECT:
                offset=four_registers(stream,chunk,"TCP_CONNECT",offset, &valid);break;
            case DIAMOND_OP_TCP_LISTEN:
                offset=three_registers(stream,chunk,"TCP_LISTEN",offset, &valid);break;
            case DIAMOND_OP_TCP_LISTEN_NONBLOCK:
                offset=three_registers(stream,chunk,"TCP_LISTEN_NONBLOCK",offset, &valid);break;
            case DIAMOND_OP_IO_POLL:
                if(!require_bytes(stream,chunk,offset,9)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, r%u, r%u\n","IO_POLL",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+7),&valid));
                offset+=9;break;
            case DIAMOND_OP_UDP_BIND:
                offset=two_registers(stream,chunk,"UDP_BIND",offset, &valid);break;
            case DIAMOND_OP_UDP_OPEN:
                offset=one_register(stream,chunk,"UDP_OPEN",offset, &valid);break;
            case DIAMOND_OP_SIGNAL_TRAP:
                offset=three_registers(stream,chunk,"SIGNAL_TRAP",offset, &valid);break;
            case DIAMOND_OP_TLS_CONNECT:
                offset=four_registers(stream,chunk,"TLS_CONNECT",offset, &valid);break;
            case DIAMOND_OP_TLS_LISTEN:
                offset=five_registers(stream,chunk,"TLS_LISTEN",offset, &valid);break;
            case DIAMOND_OP_CHR:
                offset=two_registers(stream,chunk,"CHR",offset, &valid);break;
            case DIAMOND_OP_TO_FLOAT:
                offset=two_registers(stream,chunk,"TO_FLOAT",offset, &valid);break;
            case DIAMOND_OP_TO_INT:
                offset=two_registers(stream,chunk,"TO_INT",offset, &valid);break;
            case DIAMOND_OP_TO_SYMBOL:
                offset=two_registers(stream,chunk,"TO_SYMBOL",offset, &valid);break;
            case DIAMOND_OP_MATH_UNARY: {
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, %s\n","MATH_UNARY",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    math_function_name((uint8_t)chunk->code[offset+5]));
                offset+=6;break;
            }
            case DIAMOND_OP_MATH_BINARY: {
                if(!require_bytes(stream,chunk,offset,8)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, r%u, %s\n","MATH_BINARY",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid),
                    math_function_name((uint8_t)chunk->code[offset+7]));
                offset+=8;break;
            }
            case DIAMOND_OP_REGEXP_NEW:
                offset=three_registers(stream,chunk,"REGEXP_NEW",offset, &valid);break;
            case DIAMOND_OP_PROGRAM_BUILDER_NEW:
                offset=one_register(stream,chunk,"PROGRAM_BUILDER_NEW",offset, &valid);break;
            case DIAMOND_OP_INVOKE:
            case DIAMOND_OP_INVOKE_MONO:
                if(!require_bytes(stream,chunk,offset,10)){valid=false;offset=chunk->code_count;break;}
                if((size_t)read_operand(chunk,offset+5)>=chunk->string_count)
                    valid=false;
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u, %u args\n",
                    (DiamondOpCode)opcode==DIAMOND_OP_INVOKE?"INVOKE":"INVOKE_MONO",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+7),
                        chunk->code[offset+9],&valid),
                    chunk->code[offset+9]);offset+=10;break;
            case DIAMOND_OP_INVOKE_TYPED: {
                if(!require_bytes(stream,chunk,offset,11)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t count=chunk->code[offset+10];
                if(!require_bytes(stream,chunk,offset,(size_t)11+count*2)) {
                    valid=false;offset=chunk->code_count;break;
                }
                if((size_t)read_operand(chunk,offset+5)>=chunk->string_count)
                    valid=false;
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u, %u args, [",
                    "INVOKE_TYPED",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+7),
                        chunk->code[offset+9],&valid),
                    chunk->code[offset+9]);
                for(size_t index=0;index<count;index++) {
                    if(index>0)fputs(", ",stream);
                    valid=print_type_set(stream,chunk,
                        read_operand(chunk,offset+11+index*2))&&valid;
                }
                fputs("]\n",stream);offset+=(size_t)11+count*2;break;
            }
            case DIAMOND_OP_SUPER:
                if(!require_bytes(stream,chunk,offset,9)){valid=false;offset=chunk->code_count;break;}
                if((size_t)chunk->code[offset+3]>=chunk->class_count||
                   (size_t)read_operand(chunk,offset+4)>=chunk->string_count)
                    valid=false;
                fprintf(stream,"%-18s r%u, class%u, s%u, r%u, %u args\n","SUPER",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3],read_operand(chunk,offset+4),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+6),
                        chunk->code[offset+8],&valid),
                    chunk->code[offset+8]);offset+=9;break;
            /* GET_IVAR/SET_IVAR's third operand is a field index, checked
             * against the receiver's own instance->field_count at runtime
             * (a per-instance/per-class bound, not something this purely
             * bytecode-level walk can know statically anyway) -- not a
             * register despite the uniform 2-byte width. */
            case DIAMOND_OP_GET_IVAR:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, field%u\n","GET_IVAR",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5));
                offset+=7;break;
            case DIAMOND_OP_SET_IVAR:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, field%u, r%u\n","SET_IVAR",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid));
                offset+=7;break;
            /* GET_IVAR_NAME's third operand and SET_IVAR_NAME's second are
             * a string-pool index (the field's name, resolved to an
             * index at runtime and checked against chunk->string_count),
             * not a register -- see DIAMOND_OP_GET_IVAR_NAME/SET_IVAR_
             * NAME's shared handler in vm.c for the write-dependent
             * operand-role split this mirrors. */
            case DIAMOND_OP_GET_IVAR_NAME:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, s%u\n","GET_IVAR_NAME",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5));
                offset+=7;break;
            case DIAMOND_OP_SET_IVAR_NAME:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, s%u, r%u\n","SET_IVAR_NAME",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid));
                offset+=7;break;
            /* GET_NAMESPACE_CONSTANT/SET_NAMESPACE_CONSTANT's index operand
             * is checked against DIAMOND_MAX_NAMESPACE_CONSTANTS at
             * runtime, not a register. */
            case DIAMOND_OP_GET_NAMESPACE_CONSTANT:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, ns%u\n","GET_NAMESPACE_CONST",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3));
                offset+=5;break;
            case DIAMOND_OP_SET_NAMESPACE_CONSTANT:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s ns%u, r%u\n","SET_NAMESPACE_CONST",
                    read_operand(chunk,offset+1),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid));
                offset+=5;break;
            /* GET_CVAR/SET_CVAR's class/slot operands are both compile-time
             * constants (class_variable_index resolves them once per
             * `@@name`, see compiler.c), not registers -- same convention
             * as GET_IVAR's own field operand just above. */
            case DIAMOND_OP_GET_CVAR:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, c%u, cv%u\n","GET_CVAR",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3),
                    read_operand(chunk,offset+5));
                offset+=7;break;
            case DIAMOND_OP_SET_CVAR:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s c%u, cv%u, r%u\n","SET_CVAR",
                    read_operand(chunk,offset+1),
                    read_operand(chunk,offset+3),
                    checked_register(chunk,stream,read_operand(chunk,offset+5),&valid));
                offset+=7;break;
            case DIAMOND_OP_SQLITE3_OPEN:
                offset=three_registers(stream,chunk,"SQLITE3_OPEN",offset, &valid);break;
            case DIAMOND_OP_CHECK_DESTRUCTURE_COUNT:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, %u\n","CHECK_DESTRUCTURE_COUNT",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    read_operand(chunk,offset+3)&0x7fffu);
                offset+=5;break;
            case DIAMOND_OP_TIME_MONOTONIC:
                offset=one_register(stream,chunk,"TIME_MONOTONIC",offset, &valid);break;
            case DIAMOND_OP_TIME_NOW:
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, utc=%u\n","TIME_NOW",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    chunk->code[offset+3]);
                offset+=4;break;
            case DIAMOND_OP_TIME_AT:
                offset=two_registers(stream,chunk,"TIME_AT",offset, &valid);break;
            case DIAMOND_OP_SHIFT_LEFT:
                offset=three_registers(stream,chunk,"SHIFT_LEFT",offset, &valid);break;
            case DIAMOND_OP_PROCESS_RUN:
                offset=two_registers(stream,chunk,"PROCESS_RUN",offset, &valid);break;
            case DIAMOND_OP_PROCESS_SPAWN:
                offset=two_registers(stream,chunk,"PROCESS_SPAWN",offset, &valid);break;
            case DIAMOND_OP_BCRYPT_HASH:
                offset=three_registers(stream,chunk,"BCRYPT_HASH",offset, &valid);break;
            case DIAMOND_OP_BCRYPT_VERIFY:
                offset=three_registers(stream,chunk,"BCRYPT_VERIFY",offset, &valid);break;
            case DIAMOND_OP_SECURE_RANDOM_BYTES:
                offset=two_registers(stream,chunk,"SECURE_RANDOM_BYTES",offset, &valid);break;
            case DIAMOND_OP_SECURE_RANDOM_HEX:
                offset=two_registers(stream,chunk,"SECURE_RANDOM_HEX",offset, &valid);break;
            case DIAMOND_OP_DIGEST_SHA256:
                offset=two_registers(stream,chunk,"DIGEST_SHA256",offset,&valid);break;
            case DIAMOND_OP_HMAC_SHA256:
                offset=three_registers(stream,chunk,"HMAC_SHA256",offset,&valid);break;
            case DIAMOND_OP_HMAC_VERIFY:
                offset=four_registers(stream,chunk,"HMAC_VERIFY",offset,&valid);break;
            case DIAMOND_OP_DIGEST_SHA1:
                offset=two_registers(stream,chunk,"DIGEST_SHA1",offset,&valid);break;
            case DIAMOND_OP_HMAC_SHA1:
                offset=three_registers(stream,chunk,"HMAC_SHA1",offset,&valid);break;
            case DIAMOND_OP_CIPHER_ENCRYPT:
                offset=three_registers(stream,chunk,"CIPHER_ENCRYPT",offset,&valid);break;
            case DIAMOND_OP_CIPHER_DECRYPT:
                offset=three_registers(stream,chunk,"CIPHER_DECRYPT",offset,&valid);break;
            case DIAMOND_OP_GZIP_COMPRESS:
                offset=two_registers(stream,chunk,"GZIP_COMPRESS",offset,&valid);break;
            case DIAMOND_OP_GZIP_DECOMPRESS:
                offset=three_registers(stream,chunk,"GZIP_DECOMPRESS",offset,&valid);break;
            case DIAMOND_OP_BASE64_ENCODE:
                offset=two_registers(stream,chunk,"BASE64_ENCODE",offset,&valid);break;
            case DIAMOND_OP_BASE64_DECODE:
                offset=two_registers(stream,chunk,"BASE64_DECODE",offset,&valid);break;
            case DIAMOND_OP_EXIT:
                offset=one_register(stream,chunk,"EXIT",offset, &valid);break;
            case DIAMOND_OP_DEBUGGER: {
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                const uint16_t dest=checked_register(chunk,stream,
                    read_operand(chunk,offset+1),&valid);
                const uint8_t local_count=chunk->code[offset+3];
                const size_t total=4+(size_t)local_count*4;
                if(!require_bytes(stream,chunk,offset,total)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, %u locals\n","DEBUGGER",dest,local_count);
                for(size_t index=0;index<local_count;index++) {
                    const size_t entry_offset=offset+4+index*4;
                    const uint16_t name_index=read_operand(chunk,entry_offset);
                    const uint16_t local_register=checked_register(chunk,stream,
                        read_operand(chunk,entry_offset+2),&valid);
                    const char *local_name="?";size_t local_name_length=1;
                    if((size_t)name_index<chunk->string_count) {
                        local_name=chunk->strings[name_index].chars;
                        local_name_length=chunk->strings[name_index].length;
                    }
                    fprintf(stream,"                     %.*s -> r%u\n",
                        (int)local_name_length,local_name,local_register);
                }
                offset+=total;break;
            }
            case DIAMOND_OP_ARGV:
                offset=one_register(stream,chunk,"ARGV",offset, &valid);break;
            case DIAMOND_OP_ENV:
                offset=one_register(stream,chunk,"ENV",offset, &valid);break;
            case DIAMOND_OP_MODULO:
                offset=three_registers(stream,chunk,"MODULO",offset, &valid);break;
            case DIAMOND_OP_COMPARE:
                offset=three_registers(stream,chunk,"COMPARE",offset, &valid);break;
            case DIAMOND_OP_CASE_MATCH:
                offset=three_registers(stream,chunk,"CASE_MATCH",offset,&valid);break;
            case DIAMOND_OP_CASE_ARRAY_SHAPE:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, %u elements\n","CASE_ARRAY_SHAPE",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5)&0x7fffu);
                offset+=7;break;
            case DIAMOND_OP_ARRAY_REST:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, start=%u\n","ARRAY_REST",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5));offset+=7;break;
            case DIAMOND_OP_CASE_HASH_SHAPE:
                offset=two_registers(stream,chunk,"CASE_HASH_SHAPE",offset,&valid);break;
            case DIAMOND_OP_CASE_HASH_HAS:
                offset=three_registers(stream,chunk,"CASE_HASH_HAS",offset,&valid);break;
            case DIAMOND_OP_HASH_REST:
                offset=three_registers(stream,chunk,"HASH_REST",offset,&valid);break;
            case DIAMOND_OP_ARRAY_MIDDLE:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                {
                    const uint16_t bounds=read_operand(chunk,offset+5);
                    fprintf(stream,"%-18s r%u, r%u, prefix=%u, suffix=%u\n",
                        "ARRAY_MIDDLE",
                        checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                        checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                        bounds>>8,bounds&0xffu);
                    offset+=7;
                }
                break;
            case DIAMOND_OP_ARRAY_SUFFIX:
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, reverse=%u\n","ARRAY_SUFFIX",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register(chunk,stream,read_operand(chunk,offset+3),&valid),
                    read_operand(chunk,offset+5));offset+=7;break;
            case DIAMOND_OP_CHECK_HASH_KEY:
                offset=two_registers(stream,chunk,"CHECK_HASH_KEY",offset,&valid);break;
            case DIAMOND_OP_POSTGRES_OPEN:
                offset=two_registers(stream,chunk,"POSTGRES_OPEN",offset, &valid);break;
            case DIAMOND_OP_MYSQL_OPEN:
                offset=six_registers(stream,chunk,"MYSQL_OPEN",offset, &valid);break;
            case DIAMOND_OP_CHECK_TYPE:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, ","CHECK_TYPE",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid));
                const uint16_t set_index=read_operand(chunk,offset+3);
                valid=print_type_set(stream,chunk,set_index)&&valid;
                fputc('\n',stream);offset+=5;break;
            case DIAMOND_OP_IS_TYPE: {
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, ","IS_TYPE",
                        checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                        checked_register(chunk,stream,read_operand(chunk,offset+3),&valid));
                const uint8_t type=(uint8_t)read_operand(chunk,offset+5);
                if(type==DIAMOND_TYPE_INT) fputs("Int",stream);
                else if(type==DIAMOND_TYPE_FLOAT) fputs("Float",stream);
                else if(type==DIAMOND_TYPE_STRING) fputs("String",stream);
                else if(type==DIAMOND_TYPE_BOOL) fputs("Bool",stream);
                else if(type==DIAMOND_TYPE_NIL) fputs("Nil",stream);
                else if(type==DIAMOND_TYPE_ARRAY) fputs("Array",stream);
                else if(type==DIAMOND_TYPE_HASH) fputs("Hash",stream);
                else if(type==DIAMOND_TYPE_CALLABLE) fputs("Callable",stream);
                else if(type==DIAMOND_TYPE_SIZED) fputs("Sized",stream);
        else if(type==DIAMOND_TYPE_SYMBOL) fputs("Symbol",stream);
                else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&
                        type<DIAMOND_TYPE_INTERFACE_BASE)
                    fprintf(stream,"T%u",type-DIAMOND_TYPE_VARIABLE_BASE);
                else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
                        (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
                    fputs(chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE].name,stream);
                else if((size_t)(type-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count)
                    fputs(chunk->classes[type-DIAMOND_TYPE_CLASS_BASE].name,stream);
                else {fputs("<invalid type>",stream);valid=false;}
                fputc('\n',stream);offset+=7;break;
            }
            case DIAMOND_OP_CLASS_NAME:
                offset=two_registers(stream,chunk,"CLASS_NAME",offset,&valid);break;
            case DIAMOND_OP_ARRAY: {
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                const uint16_t items=read_operand(chunk,offset+5);
                fprintf(stream,"%-18s r%u, r%u, %u items\n","ARRAY",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+3),items,&valid),
                    items);
                offset+=7;break;
            }
            case DIAMOND_OP_INDEX_GET:
                offset=three_registers(stream,chunk,"INDEX_GET",offset, &valid);break;
            case DIAMOND_OP_INDEX_SET:
                offset=three_registers(stream,chunk,"INDEX_SET",offset, &valid);break;
            case DIAMOND_OP_HASH: {
                if(!require_bytes(stream,chunk,offset,7)){valid=false;offset=chunk->code_count;break;}
                const uint16_t pairs=read_operand(chunk,offset+5);
                fprintf(stream,"%-18s r%u, r%u, %u pairs\n","HASH",
                    checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                    checked_register_range(chunk,stream,read_operand(chunk,offset+3),
                        (size_t)pairs*2,&valid),
                    pairs);
                offset+=7;break;
            }
            case DIAMOND_OP_NOT:
                offset=two_registers(stream,chunk,"NOT",offset, &valid);break;
            case DIAMOND_OP_RETURN:
                offset = one_register(stream, chunk, "RETURN", offset, &valid);
                break;
            case DIAMOND_OP_RAISE:
                offset = one_register(stream, chunk, "RAISE", offset, &valid);
                break;
            case DIAMOND_OP_PUSH_RESCUE: {
                if(!require_bytes(stream,chunk,offset,14)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+12]<<8)|chunk->code[offset+13];
                fprintf(stream,"%-18s r%u, %u types -> %04zu\n","PUSH_RESCUE",
                        checked_register(chunk,stream,read_operand(chunk,offset+1),&valid),
                        chunk->code[offset+3],target);
                if(target>chunk->code_count) valid=false;
                else pending_targets[pending_count++]=target;
                offset+=14;break;
            }
            case DIAMOND_OP_POP_RESCUE:
                fprintf(stream,"%-18s\n","POP_RESCUE");offset++;break;
            case DIAMOND_OP_PUSH_ENSURE:
            case DIAMOND_OP_RUN_ENSURE: {
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+1]<<8)|chunk->code[offset+2];
                if(target>chunk->code_count) valid=false;
                else pending_targets[pending_count++]=target;
                fprintf(stream,"%-18s -> %04zu\n",
                    chunk->code[offset]==DIAMOND_OP_PUSH_ENSURE
                        ? "PUSH_ENSURE" : "RUN_ENSURE",target);
                offset+=3;break;
            }
            case DIAMOND_OP_END_ENSURE:
                fprintf(stream,"%-18s\n","END_ENSURE");offset++;break;
            default:
                fprintf(stream, "<unknown opcode %u>\n", chunk->code[offset]);
                valid = false;
                offset++;
                break;
        }
    }

    /* Every jump-like target recorded above gets checked here, now that
     * `starts` is fully populated -- a target within [0, code_count] that
     * doesn't land on an instruction boundary (target==code_count, falling
     * off the end, is the one exception: already handled the same way a
     * normal fall-through completion is) would make the interpreter read
     * a byte this walk never validated as an opcode, bypassing every
     * register-bounds check above for whatever it decides to do next. */
    for (size_t index = 0; index < pending_count; index++) {
        const size_t target = pending_targets[index];
        if (target != chunk->code_count && !starts[target]) {
            fprintf(stream, "<jump target %04zu is not an instruction boundary>\n", target);
            valid = false;
        }
    }
    return valid;
}

bool diamond_disassemble(FILE *stream, const char *name,
                         const DiamondChunk *chunk) {
    bool valid = disassemble_chunk(stream, name, chunk);
    for (size_t index = 0; index < chunk->function_count; index++) {
        const DiamondFunction *function = chunk->functions[index];
        const DiamondChunk function_chunk = {
            .name = function->name,
            .code = function->code,
            .lines = function->lines,
            .columns = function->columns,
            .code_count = function->code_count,
            .constants = function->constants,
            .constant_count = function->constant_count,
            .strings = function->strings,
            .string_count = function->string_count,
            .type_sets = function->type_sets,
            .type_set_count = function->type_set_count,
            .functions = chunk->functions,
            .function_count = chunk->function_count,
            .classes = chunk->classes,
            .class_count = chunk->class_count,
            .interfaces=chunk->interfaces,
            .interface_count=chunk->interface_count,
            .register_count=function->register_count,
        };
        if (!disassemble_chunk(stream, function->name, &function_chunk)) {
            valid = false;
        }
    }
    return valid;
}

bool diamond_verify_bytecode(const DiamondChunk *chunk) {
    FILE *sink = getenv("DIAMOND_DEBUG_VERIFY") ? stderr : fopen("/dev/null", "w");
    if (sink == nullptr) return false;
    const bool valid = diamond_disassemble(sink, chunk->name, chunk);
    if (sink != stderr) fclose(sink);
    return valid;
}
