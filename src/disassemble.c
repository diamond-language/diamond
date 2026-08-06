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

static bool print_type_set(FILE *stream,const DiamondChunk *chunk,
                           uint8_t set_index) {
    if((size_t)set_index>=chunk->type_set_count) {
        fputs("<invalid type set>",stream);return false;
    }
    bool valid=true;const DiamondTypeSet *set=&chunk->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        if(index>0)fputs(" | ",stream);
        const DiamondTypeMember member=set->members[index];
        const uint8_t type=member.id;
        if(type==DIAMOND_TYPE_INT) fputs("Int",stream);
        else if(type==DIAMOND_TYPE_STRING) fputs("String",stream);
        else if(type==DIAMOND_TYPE_BOOL) fputs("Bool",stream);
        else if(type==DIAMOND_TYPE_NIL) fputs("Nil",stream);
        else if(type==DIAMOND_TYPE_ARRAY) fputs("Array",stream);
        else if(type==DIAMOND_TYPE_HASH) fputs("Hash",stream);
        else if(type==DIAMOND_TYPE_CALLABLE) fputs("Callable",stream);
        else if(type==DIAMOND_TYPE_SIZED) fputs("Sized",stream);
        else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE)
            fprintf(stream,"T%u",type-DIAMOND_TYPE_VARIABLE_BASE);
        else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
                (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
            fputs(chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE].name,stream);
        else if((size_t)(type-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count)
            fputs(chunk->classes[type-DIAMOND_TYPE_CLASS_BASE].name,stream);
        else {fputs("<invalid type>",stream);valid=false;}
        if(member.argument_set!=UINT8_MAX) {
            fputc('[',stream);
            valid=print_type_set(stream,chunk,member.argument_set)&&valid;
            if(member.second_argument_set!=UINT8_MAX) {
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
            if(member.callable_return_set!=UINT8_MAX) {
                fputs(", ",stream);
                valid=print_type_set(stream,chunk,member.callable_return_set)&&valid;
            }
            fputc(']',stream);
        }
    }
    return valid;
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
            case DIAMOND_OP_ARGUMENT_PROVIDED:
                offset=two_registers(stream,chunk,"ARGUMENT_PROVIDED",offset);
                break;
            case DIAMOND_OP_TO_STRING:
                offset=two_registers(stream,chunk,"TO_STRING",offset);break;
            case DIAMOND_OP_MOVE:
                offset = two_registers(stream, chunk, "MOVE", offset);
                break;
            case DIAMOND_OP_ADD:
                offset = three_registers(stream, chunk, "ADD", offset);
                break;
            case DIAMOND_OP_ADD_INT:
                offset = three_registers(stream, chunk, "ADD_INT", offset);
                break;
            case DIAMOND_OP_SUBTRACT:
                offset = three_registers(stream, chunk, "SUBTRACT", offset);
                break;
            case DIAMOND_OP_MULTIPLY:
                offset = three_registers(stream, chunk, "MULTIPLY", offset);
                break;
            case DIAMOND_OP_DIVIDE:
                offset = three_registers(stream, chunk, "DIVIDE", offset);
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
            case DIAMOND_OP_LESS:
                offset = three_registers(stream, chunk, "LESS", offset);
                break;
            case DIAMOND_OP_LESS_EQUAL:
                offset = three_registers(stream, chunk, "LESS_EQUAL", offset);
                break;
            case DIAMOND_OP_GREATER:
                offset = three_registers(stream, chunk, "GREATER", offset);
                break;
            case DIAMOND_OP_GREATER_EQUAL:
                offset = three_registers(stream, chunk, "GREATER_EQUAL", offset);
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
            case DIAMOND_OP_EQUAL_INT:
                offset = three_registers(stream, chunk, "EQUAL_INT", offset);
                break;
            case DIAMOND_OP_NOT_EQUAL_INT:
                offset = three_registers(stream, chunk, "NOT_EQUAL_INT", offset);
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
            case DIAMOND_OP_CALL_TYPED: {
                if(!require_bytes(stream,chunk,offset,6)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t count=chunk->code[offset+5];
                if(!require_bytes(stream,chunk,offset,(size_t)6+count)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, f%u, r%u, %u args, [",
                    "CALL_TYPED",chunk->code[offset+1],chunk->code[offset+2],
                    chunk->code[offset+3],chunk->code[offset+4]);
                for(size_t index=0;index<count;index++) {
                    if(index>0)fputs(", ",stream);
                    const uint8_t set=chunk->code[offset+6+index];
                    valid=print_type_set(stream,chunk,set)&&valid;
                }
                fputs("]\n",stream);
                if((size_t)chunk->code[offset+2]>=chunk->function_count)
                    valid=false;
                offset+=(size_t)6+count;
                break;
            }
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
            case DIAMOND_OP_INVOKE_TYPED: {
                if(!require_bytes(stream,chunk,offset,7)) {
                    valid=false;offset=chunk->code_count;break;
                }
                const uint8_t count=chunk->code[offset+6];
                if(!require_bytes(stream,chunk,offset,(size_t)7+count)) {
                    valid=false;offset=chunk->code_count;break;
                }
                fprintf(stream,"%-18s r%u, r%u, s%u, r%u, %u args, [",
                    "INVOKE_TYPED",chunk->code[offset+1],chunk->code[offset+2],
                    chunk->code[offset+3],chunk->code[offset+4],
                    chunk->code[offset+5]);
                for(size_t index=0;index<count;index++) {
                    if(index>0)fputs(", ",stream);
                    valid=print_type_set(stream,chunk,
                        chunk->code[offset+7+index])&&valid;
                }
                fputs("]\n",stream);offset+=(size_t)7+count;break;
            }
            case DIAMOND_OP_SUPER:
                if(!require_bytes(stream,chunk,offset,6)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, class%u, s%u, r%u, %u args\n","SUPER",
                    chunk->code[offset+1],chunk->code[offset+2],chunk->code[offset+3],
                    chunk->code[offset+4],chunk->code[offset+5]);offset+=6;break;
            case DIAMOND_OP_GET_IVAR:
                offset=three_registers(stream,chunk,"GET_IVAR",offset);break;
            case DIAMOND_OP_SET_IVAR:
                offset=three_registers(stream,chunk,"SET_IVAR",offset);break;
            case DIAMOND_OP_GET_IVAR_NAME:
                offset=three_registers(stream,chunk,"GET_IVAR_NAME",offset);break;
            case DIAMOND_OP_SET_IVAR_NAME:
                offset=three_registers(stream,chunk,"SET_IVAR_NAME",offset);break;
            case DIAMOND_OP_GET_NAMESPACE_CONSTANT:
                offset=two_registers(stream,chunk,"GET_NAMESPACE_CONST",offset);break;
            case DIAMOND_OP_SET_NAMESPACE_CONSTANT:
                offset=two_registers(stream,chunk,"SET_NAMESPACE_CONST",offset);break;
            case DIAMOND_OP_CHECK_TYPE:
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, ","CHECK_TYPE",chunk->code[offset+1]);
                const uint8_t set_index=chunk->code[offset+2];
                valid=print_type_set(stream,chunk,set_index)&&valid;
                fputc('\n',stream);offset+=3;break;
            case DIAMOND_OP_IS_TYPE: {
                if(!require_bytes(stream,chunk,offset,4)){valid=false;offset=chunk->code_count;break;}
                fprintf(stream,"%-18s r%u, r%u, ","IS_TYPE",
                        chunk->code[offset+1],chunk->code[offset+2]);
                const uint8_t type=chunk->code[offset+3];
                if(type==DIAMOND_TYPE_INT) fputs("Int",stream);
                else if(type==DIAMOND_TYPE_STRING) fputs("String",stream);
                else if(type==DIAMOND_TYPE_BOOL) fputs("Bool",stream);
                else if(type==DIAMOND_TYPE_NIL) fputs("Nil",stream);
                else if(type==DIAMOND_TYPE_ARRAY) fputs("Array",stream);
                else if(type==DIAMOND_TYPE_HASH) fputs("Hash",stream);
                else if(type==DIAMOND_TYPE_CALLABLE) fputs("Callable",stream);
                else if(type==DIAMOND_TYPE_SIZED) fputs("Sized",stream);
                else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&
                        type<DIAMOND_TYPE_INTERFACE_BASE)
                    fprintf(stream,"T%u",type-DIAMOND_TYPE_VARIABLE_BASE);
                else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
                        (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
                    fputs(chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE].name,stream);
                else if((size_t)(type-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count)
                    fputs(chunk->classes[type-DIAMOND_TYPE_CLASS_BASE].name,stream);
                else {fputs("<invalid type>",stream);valid=false;}
                fputc('\n',stream);offset+=4;break;
            }
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
                if(!require_bytes(stream,chunk,offset,13)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+11]<<8)|chunk->code[offset+12];
                fprintf(stream,"%-18s r%u, %u types -> %04zu\n","PUSH_RESCUE",
                        chunk->code[offset+1],chunk->code[offset+2],target);
                offset+=13;break;
            }
            case DIAMOND_OP_POP_RESCUE:
                fprintf(stream,"%-18s\n","POP_RESCUE");offset++;break;
            case DIAMOND_OP_PUSH_ENSURE:
            case DIAMOND_OP_RUN_ENSURE: {
                if(!require_bytes(stream,chunk,offset,3)){valid=false;offset=chunk->code_count;break;}
                const size_t target=((size_t)chunk->code[offset+1]<<8)|chunk->code[offset+2];
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
            .type_sets = function->type_sets,
            .type_set_count = function->type_set_count,
            .functions = chunk->functions,
            .function_count = chunk->function_count,
            .classes = chunk->classes,
            .class_count = chunk->class_count,
            .interfaces=chunk->interfaces,
            .interface_count=chunk->interface_count,
        };
        if (!disassemble_chunk(stream, function->name, &function_chunk)) {
            valid = false;
        }
    }
    return valid;
}
