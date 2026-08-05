#include "disassemble.h"

static bool require_bytes(FILE *stream, const DiamondChunk *chunk,
                          size_t offset, size_t count) {
    if (offset + count <= chunk->code_count) {
        return true;
    }
    fprintf(stream, "%04zu <truncated instruction>\n", offset);
    return false;
}

static size_t one_register(FILE *stream, const DiamondChunk *chunk,
                           const char *name, size_t offset) {
    if (!require_bytes(stream, chunk, offset, 2)) return chunk->code_count;
    fprintf(stream, "%-18s r%u\n", name, chunk->code[offset + 1]);
    return offset + 2;
}

static size_t two_registers(FILE *stream, const DiamondChunk *chunk,
                            const char *name, size_t offset) {
    if (!require_bytes(stream, chunk, offset, 3)) return chunk->code_count;
    fprintf(stream, "%-18s r%u, r%u\n", name, chunk->code[offset + 1],
            chunk->code[offset + 2]);
    return offset + 3;
}

static size_t three_registers(FILE *stream, const DiamondChunk *chunk,
                              const char *name, size_t offset) {
    if (!require_bytes(stream, chunk, offset, 4)) return chunk->code_count;
    fprintf(stream, "%-18s r%u, r%u, r%u\n", name,
            chunk->code[offset + 1], chunk->code[offset + 2],
            chunk->code[offset + 3]);
    return offset + 4;
}

static bool disassemble_chunk(FILE *stream, const char *name,
                              const DiamondChunk *chunk) {
    fprintf(stream, "== %s ==\n", name);
    size_t offset = 0;
    bool valid = true;

    while (offset < chunk->code_count) {
        if (chunk->lines != nullptr && chunk->lines[offset] != 0) {
            fprintf(stream, "%04zu %4u:%-3u ", offset, chunk->lines[offset],
                    chunk->columns[offset]);
        } else {
            fprintf(stream, "%04zu          ", offset);
        }
        const DiamondOpCode opcode = (DiamondOpCode)chunk->code[offset];
        switch (opcode) {
            case DIAMOND_OP_CONSTANT: {
                if (!require_bytes(stream, chunk, offset, 3)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const uint8_t destination = chunk->code[offset + 1];
                const uint8_t constant = chunk->code[offset + 2];
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
                offset += 3;
                break;
            }
            case DIAMOND_OP_STRING: {
                if (!require_bytes(stream, chunk, offset, 3)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const uint8_t destination = chunk->code[offset + 1];
                const uint8_t string = chunk->code[offset + 2];
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
                offset += 3;
                break;
            }
            case DIAMOND_OP_NIL:
                offset = one_register(stream, chunk, "NIL", offset);
                break;
            case DIAMOND_OP_BOOL:
                if (!require_bytes(stream, chunk, offset, 3)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                fprintf(stream, "%-18s r%u, %s\n", "BOOL",
                        chunk->code[offset + 1],
                        chunk->code[offset + 2] ? "true" : "false");
                offset += 3;
                break;
            case DIAMOND_OP_MOVE:
                offset = two_registers(stream, chunk, "MOVE", offset);
                break;
            case DIAMOND_OP_ADD:
                offset = three_registers(stream, chunk, "ADD", offset);
                break;
            case DIAMOND_OP_SUBTRACT_INT:
                offset = three_registers(stream, chunk, "SUBTRACT_INT", offset);
                break;
            case DIAMOND_OP_MULTIPLY_INT:
                offset = three_registers(stream, chunk, "MULTIPLY_INT", offset);
                break;
            case DIAMOND_OP_DIVIDE_INT:
                offset = three_registers(stream, chunk, "DIVIDE_INT", offset);
                break;
            case DIAMOND_OP_NEGATE_INT:
                offset = two_registers(stream, chunk, "NEGATE_INT", offset);
                break;
            case DIAMOND_OP_EQUAL:
                offset = three_registers(stream, chunk, "EQUAL", offset);
                break;
            case DIAMOND_OP_NOT_EQUAL:
                offset = three_registers(stream, chunk, "NOT_EQUAL", offset);
                break;
            case DIAMOND_OP_LESS_INT:
                offset = three_registers(stream, chunk, "LESS_INT", offset);
                break;
            case DIAMOND_OP_LESS_EQUAL_INT:
                offset = three_registers(stream, chunk, "LESS_EQUAL_INT", offset);
                break;
            case DIAMOND_OP_GREATER_INT:
                offset = three_registers(stream, chunk, "GREATER_INT", offset);
                break;
            case DIAMOND_OP_GREATER_EQUAL_INT:
                offset = three_registers(stream, chunk, "GREATER_EQUAL_INT", offset);
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
                offset += 3;
                break;
            }
            case DIAMOND_OP_JUMP_IF_FALSE: {
                if (!require_bytes(stream, chunk, offset, 4)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                const size_t target = ((size_t)chunk->code[offset + 2] << 8) |
                                      chunk->code[offset + 3];
                fprintf(stream, "%-18s r%u -> %04zu\n", "JUMP_IF_FALSE",
                        chunk->code[offset + 1], target);
                if (target > chunk->code_count) valid = false;
                offset += 4;
                break;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: {
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+2]<<8)|chunk->code[offset+3];
                fprintf(stream,"%-18s r%u -> %04zu\n","JUMP_IF_TRUE",
                    chunk->code[offset+1],target);
                if(target>chunk->code_count) valid=false;
                offset+=4;
                break;
            }
            case DIAMOND_OP_CALL:
                if (!require_bytes(stream, chunk, offset, 5)) {
                    valid = false;
                    offset = chunk->code_count;
                    break;
                }
                fprintf(stream, "%-18s r%u, f%u, r%u, %u args\n", "CALL",
                        chunk->code[offset + 1], chunk->code[offset + 2],
                        chunk->code[offset + 3], chunk->code[offset + 4]);
                if ((size_t)chunk->code[offset + 2] >= chunk->function_count) {
                    valid = false;
                }
                offset += 5;
                break;
            case DIAMOND_OP_CALL_CLOSURE:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, r%u, %u args\n","CALL_CLOSURE",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3],chunk->code[offset+4]);
                offset+=5;break;
            case DIAMOND_OP_GET_CAPTURE:
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, c%u\n","GET_CAPTURE",chunk->code[offset+1],chunk->code[offset+2]);
                offset+=3;break;
            case DIAMOND_OP_GET_CAPTURE_CELL:
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, c%u\n","GET_CAPTURE_CELL",chunk->code[offset+1],chunk->code[offset+2]);
                offset+=3;break;
            case DIAMOND_OP_SET_CAPTURE:
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s c%u, r%u\n","SET_CAPTURE",chunk->code[offset+1],chunk->code[offset+2]);
                offset+=3;break;
            case DIAMOND_OP_BOX_LOCAL:
                offset=one_register(stream,chunk,"BOX_LOCAL",offset);break;
            case DIAMOND_OP_GET_CELL:
                offset=two_registers(stream,chunk,"GET_CELL",offset);break;
            case DIAMOND_OP_SET_CELL:
                offset=two_registers(stream,chunk,"SET_CELL",offset);break;
            case DIAMOND_OP_CLOSURE: {
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                const size_t count=chunk->code[offset+3];
                if(!require_bytes(stream,chunk,offset,4+count)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, f%u, %zu captures\n","CLOSURE",
                    chunk->code[offset+1],chunk->code[offset+2],count);
                offset+=4+count;break;
            }
            case DIAMOND_OP_NEW:
                if(!require_bytes(stream,chunk,offset,5)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, r%u, %u args\n","NEW",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3],chunk->code[offset+4]);
                offset+=5;break;
            case DIAMOND_OP_INVOKE:
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u, %u args\n","INVOKE",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3],
                    chunk->code[offset+4],chunk->code[offset+5]);offset+=6;break;
            case DIAMOND_OP_SUPER:
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, s%u, r%u, %u args\n","SUPER",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3],
                    chunk->code[offset+4],chunk->code[offset+5]);offset+=6;break;
            case DIAMOND_OP_GET_IVAR:
                offset=three_registers(stream,chunk,"GET_IVAR",offset);break;
            case DIAMOND_OP_SET_IVAR:
                offset=three_registers(stream,chunk,"SET_IVAR",offset);break;
            case DIAMOND_OP_CHECK_TYPE:
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, ","CHECK_TYPE",chunk->code[offset+1]);
                const uint8_t encoded_type=chunk->code[offset+2];
                const bool nilable=(encoded_type&DIAMOND_TYPE_NILABLE)!=0;
                const uint8_t type=encoded_type&(uint8_t)~DIAMOND_TYPE_NILABLE;
                if(type==DIAMOND_TYPE_INT) fputs("Int",stream);
                else if(type==DIAMOND_TYPE_STRING) fputs("String",stream);
                else if(type==DIAMOND_TYPE_BOOL) fputs("Bool",stream);
                else if(type==DIAMOND_TYPE_NIL) fputs("Nil",stream);
                else if(type==DIAMOND_TYPE_ARRAY) fputs("Array",stream);
                else if(type==DIAMOND_TYPE_HASH) fputs("Hash",stream);
                else if((size_t)(type-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count)
                    fputs(chunk->classes[type-DIAMOND_TYPE_CLASS_BASE].name,stream);
                else {fputs("<invalid type>",stream);valid=false;}
                if(nilable) fputs(" | Nil",stream);
                fputc('\n',stream);offset+=3;break;
            case DIAMOND_OP_ARRAY:
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, %u items\n","ARRAY",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3]);
                offset+=4;break;
            case DIAMOND_OP_INDEX_GET:
                offset=three_registers(stream,chunk,"INDEX_GET",offset);break;
            case DIAMOND_OP_INDEX_SET:
                offset=three_registers(stream,chunk,"INDEX_SET",offset);break;
            case DIAMOND_OP_HASH:
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, %u pairs\n","HASH",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3]);
                offset+=4;break;
            case DIAMOND_OP_NOT:
                offset=two_registers(stream,chunk,"NOT",offset);break;
            case DIAMOND_OP_RETURN:
                offset = one_register(stream, chunk, "RETURN", offset);
                break;
            case DIAMOND_OP_RAISE:
                offset = one_register(stream, chunk, "RAISE", offset);
                break;
            case DIAMOND_OP_PUSH_RESCUE: {
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+2]<<8)|chunk->code[offset+3];
                fprintf(stream,"%-18s r%u -> %04zu\n","PUSH_RESCUE",chunk->code[offset+1],target);
                offset+=4;break;
            }
            case DIAMOND_OP_POP_RESCUE:
                fprintf(stream,"%-18s\n","POP_RESCUE");offset++;break;
            default:
                fprintf(stream, "<unknown opcode %u>\n", chunk->code[offset]);
                valid = false;
                offset++;
                break;
        }
    }
    return valid;
}

bool diamond_disassemble(FILE *stream, const char *name,
                         const DiamondChunk *chunk) {
    bool valid = disassemble_chunk(stream, name, chunk);
    for (size_t index = 0; index < chunk->function_count; index++) {
        const DiamondFunction *function = &chunk->functions[index];
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
            .functions = chunk->functions,
            .function_count = chunk->function_count,
            .classes = chunk->classes,
            .class_count = chunk->class_count,
        };
        if (!disassemble_chunk(stream, function->name, &function_chunk)) {
            valid = false;
        }
    }
    return valid;
}
