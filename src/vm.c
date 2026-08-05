#include "vm.h"

#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DIAMOND_REGISTER_COUNT = 256 };
enum { DIAMOND_MAX_CALL_DEPTH = 256 };

typedef struct DiamondFrame {
    struct DiamondFrame *previous;
    DiamondValue *registers;
} DiamondFrame;

static void mark_value(DiamondValue value);

static void mark_object(DiamondObject *object) {
    if (object == nullptr || object->marked) return;
    object->marked = true;
    if (object->kind == DIAMOND_OBJECT_INSTANCE) {
        DiamondInstance *instance=(DiamondInstance *)object;
        for(size_t i=0;i<instance->field_count;i++) mark_value(instance->fields[i]);
    } else if(object->kind==DIAMOND_OBJECT_ARRAY) {
        DiamondArray *array=(DiamondArray *)object;
        for(size_t i=0;i<array->count;i++) mark_value(array->values[i]);
    } else if(object->kind==DIAMOND_OBJECT_HASH) {
        DiamondHash *hash=(DiamondHash *)object;
        for(size_t i=0;i<hash->count;i++) {
            mark_value(hash->entries[i].key);
            mark_value(hash->entries[i].value);
        }
    }
}

static void mark_value(DiamondValue value) {
    if (value.kind == DIAMOND_VALUE_OBJECT) mark_object(value.as.object);
}

void diamond_vm_collect(DiamondVm *vm) {
    for (DiamondFrame *frame = vm->frames; frame != nullptr;
         frame = frame->previous) {
        for (size_t index = 0; index < DIAMOND_REGISTER_COUNT; index++) {
            mark_value(frame->registers[index]);
        }
    }

    DiamondObject **object = &vm->objects;
    while (*object != nullptr) {
        if ((*object)->marked) {
            (*object)->marked = false;
            object = &(*object)->next;
            continue;
        }
        DiamondObject *unreached = *object;
        *object = unreached->next;
        size_t size=sizeof(DiamondObject);
        if(unreached->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)unreached;
            size=sizeof(DiamondString)+string->length+1;
        } else if(unreached->kind==DIAMOND_OBJECT_INSTANCE) {
            const DiamondInstance *instance=(const DiamondInstance *)unreached;
            size=sizeof(DiamondInstance)+instance->field_count*sizeof(DiamondValue);
        } else if(unreached->kind==DIAMOND_OBJECT_ARRAY) {
            const DiamondArray *array=(const DiamondArray *)unreached;
            size=sizeof(DiamondArray)+array->count*sizeof(DiamondValue);
        } else {
            DiamondHash *hash=(DiamondHash *)unreached;
            size=sizeof(DiamondHash)+hash->capacity*sizeof(DiamondHashEntry);
            free(hash->entries);
        }
        vm->bytes_allocated -= size;
        free(unreached);
    }
    vm->next_gc = vm->bytes_allocated < 1024
        ? 2048 : vm->bytes_allocated * 2;
}

void diamond_vm_init(DiamondVm *vm) {
    *vm = (DiamondVm){.next_gc = 2048};
}

void diamond_vm_free(DiamondVm *vm) {
    DiamondObject *object = vm->objects;
    while (object != nullptr) {
        DiamondObject *next = object->next;
        if(object->kind==DIAMOND_OBJECT_HASH)
            free(((DiamondHash *)object)->entries);
        free(object);
        object = next;
    }
    *vm = (DiamondVm){};
}

static DiamondString *allocate_string(DiamondVm *vm, const char *chars,
                                      size_t length) {
    if (vm->stress_gc || vm->bytes_allocated >= vm->next_gc) {
        diamond_vm_collect(vm);
    }
    DiamondString *string = malloc(sizeof(DiamondString) + length + 1);
    if (string == nullptr) return nullptr;
    string->object = (DiamondObject){
        .next = vm->objects,
        .kind = DIAMOND_OBJECT_STRING,
    };
    string->length = length;
    memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    vm->objects = &string->object;
    vm->bytes_allocated += sizeof(DiamondString) + length + 1;
    return string;
}

static DiamondInstance *allocate_instance(DiamondVm *vm,const DiamondClass *class) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    const size_t size=sizeof(DiamondInstance)+class->field_count*sizeof(DiamondValue);
    DiamondInstance *instance=malloc(size); if(instance==nullptr)return nullptr;
    instance->object=(DiamondObject){.next=vm->objects,.kind=DIAMOND_OBJECT_INSTANCE};
    instance->class=class; instance->field_count=class->field_count;
    for(size_t i=0;i<instance->field_count;i++) instance->fields[i]=DIAMOND_NIL;
    vm->objects=&instance->object; vm->bytes_allocated+=size; return instance;
}

static DiamondArray *allocate_array(DiamondVm *vm,const DiamondValue *values,
                                    size_t count) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    const size_t size=sizeof(DiamondArray)+count*sizeof(DiamondValue);
    DiamondArray *array=malloc(size); if(array==nullptr)return nullptr;
    array->object=(DiamondObject){.next=vm->objects,.kind=DIAMOND_OBJECT_ARRAY};
    array->count=count;
    for(size_t i=0;i<count;i++) array->values[i]=values[i];
    vm->objects=&array->object;vm->bytes_allocated+=size;return array;
}

static DiamondHash *allocate_hash(DiamondVm *vm) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    DiamondHash *hash=malloc(sizeof(DiamondHash)); if(hash==nullptr)return nullptr;
    *hash=(DiamondHash){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_HASH}};
    vm->objects=&hash->object;vm->bytes_allocated+=sizeof(DiamondHash);return hash;
}

static bool values_equal(DiamondValue left, DiamondValue right) {
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
        case DIAMOND_VALUE_NIL:
            return true;
        case DIAMOND_VALUE_BOOL:
            return left.as.boolean == right.as.boolean;
        case DIAMOND_VALUE_INT:
            return left.as.integer == right.as.integer;
        case DIAMOND_VALUE_OBJECT: {
            if (left.as.object->kind != right.as.object->kind) return false;
            if(left.as.object->kind==DIAMOND_OBJECT_INSTANCE ||
               left.as.object->kind==DIAMOND_OBJECT_ARRAY ||
               left.as.object->kind==DIAMOND_OBJECT_HASH)
                return left.as.object==right.as.object;
            const DiamondString *a = (const DiamondString *)left.as.object;
            const DiamondString *b = (const DiamondString *)right.as.object;
            return a->length == b->length &&
                   memcmp(a->chars, b->chars, a->length) == 0;
        }
    }
    return false;
}

static ptrdiff_t hash_find(const DiamondHash *hash,DiamondValue key) {
    for(size_t i=0;i<hash->count;i++)
        if(values_equal(hash->entries[i].key,key)) return (ptrdiff_t)i;
    return -1;
}

static bool hash_set(DiamondVm *vm,DiamondHash *hash,DiamondValue key,
                     DiamondValue value) {
    const ptrdiff_t existing=hash_find(hash,key);
    if(existing>=0) {hash->entries[(size_t)existing].value=value;return true;}
    if(hash->count==hash->capacity) {
        const size_t old_capacity=hash->capacity;
        const size_t capacity=old_capacity<8?8:old_capacity*2;
        DiamondHashEntry *entries=realloc(hash->entries,
            capacity*sizeof(DiamondHashEntry));
        if(entries==nullptr)return false;
        hash->entries=entries;hash->capacity=capacity;
        vm->bytes_allocated+=(capacity-old_capacity)*sizeof(DiamondHashEntry);
    }
    hash->entries[hash->count++]=(DiamondHashEntry){.key=key,.value=value};
    return true;
}

static bool is_truthy(DiamondValue value) {
    return value.kind != DIAMOND_VALUE_NIL &&
           !(value.kind == DIAMOND_VALUE_BOOL && !value.as.boolean);
}

static const DiamondMethod *lookup_method(const DiamondChunk *chunk,
                                          const DiamondClass *class,
                                          const char *name, size_t length) {
    const DiamondClass *current = class;
    while (current != nullptr) {
        for (size_t index = 0; index < current->method_count; index++) {
            const DiamondMethod *method = &current->methods[index];
            if (strlen(method->name) == length &&
                memcmp(method->name, name, length) == 0) {
                return method;
            }
        }
        current = current->superclass == UINT8_MAX
            ? nullptr : &chunk->classes[current->superclass];
    }
    return nullptr;
}

static bool value_matches_type(const DiamondChunk *chunk, DiamondValue value,
                               uint8_t type) {
    const bool nilable=(type&DIAMOND_TYPE_NILABLE)!=0;
    type&=(uint8_t)~DIAMOND_TYPE_NILABLE;
    if(nilable && value.kind==DIAMOND_VALUE_NIL) return true;
    if(type==DIAMOND_TYPE_INT) return value.kind==DIAMOND_VALUE_INT;
    if(type==DIAMOND_TYPE_BOOL) return value.kind==DIAMOND_VALUE_BOOL;
    if(type==DIAMOND_TYPE_NIL) return value.kind==DIAMOND_VALUE_NIL;
    if(type==DIAMOND_TYPE_STRING) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_STRING;
    if(type==DIAMOND_TYPE_ARRAY) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_ARRAY;
    if(type==DIAMOND_TYPE_HASH) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_HASH;
    const size_t class_index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
    if(class_index>=chunk->class_count || value.kind!=DIAMOND_VALUE_OBJECT ||
       value.as.object->kind!=DIAMOND_OBJECT_INSTANCE) return false;
    const DiamondClass *wanted=&chunk->classes[class_index];
    const DiamondClass *actual=((DiamondInstance *)value.as.object)->class;
    while(actual!=nullptr) {
        if(actual==wanted) return true;
        actual=actual->superclass==UINT8_MAX?nullptr:&chunk->classes[actual->superclass];
    }
    return false;
}

static void format_type(char *buffer, size_t capacity,
                        const DiamondChunk *chunk, uint8_t encoded) {
    const bool nilable=(encoded&DIAMOND_TYPE_NILABLE)!=0;
    const uint8_t type=encoded&(uint8_t)~DIAMOND_TYPE_NILABLE;
    const char *name="<invalid type>";
    if(type==DIAMOND_TYPE_INT) name="Int";
    else if(type==DIAMOND_TYPE_STRING) name="String";
    else if(type==DIAMOND_TYPE_BOOL) name="Bool";
    else if(type==DIAMOND_TYPE_NIL) name="Nil";
    else if(type==DIAMOND_TYPE_ARRAY) name="Array";
    else if(type==DIAMOND_TYPE_HASH) name="Hash";
    else {
        const size_t index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
        if(index<chunk->class_count) name=chunk->classes[index].name;
    }
    snprintf(buffer,capacity,"%s%s",name,nilable?" | Nil":"");
}

static void format_value_type(char *buffer, size_t capacity,
                              DiamondValue value) {
    const char *name="<unknown>";
    if(value.kind==DIAMOND_VALUE_NIL) name="Nil";
    else if(value.kind==DIAMOND_VALUE_BOOL) name="Bool";
    else if(value.kind==DIAMOND_VALUE_INT) name="Int";
    else if(value.as.object->kind==DIAMOND_OBJECT_STRING) name="String";
    else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY) name="Array";
    else if(value.as.object->kind==DIAMOND_OBJECT_HASH) name="Hash";
    else {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        name=instance->class->name;
    }
    snprintf(buffer,capacity,"%s",name);
}

static DiamondVmStatus run_chunk(const DiamondChunk *chunk,
                                 DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 DiamondValue *result) {
    if (depth >= DIAMOND_MAX_CALL_DEPTH) {
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    DiamondValue registers[DIAMOND_REGISTER_COUNT] = {};
    if (argument_count > DIAMOND_REGISTER_COUNT) {
        return DIAMOND_VM_ARITY_ERROR;
    }
    for (size_t index = 0; index < argument_count; index++) {
        registers[index] = arguments[index];
    }
    DiamondFrame frame = {
        .previous = vm->frames,
        .registers = registers,
    };
    vm->frames = &frame;
    size_t ip = 0;
    size_t instruction_offset = 0;

    #define RECORD_ERROR(status_) do {                                      \
        if ((status_) != DIAMOND_VM_OK) {                                   \
            size_t used = strlen(vm->error);                                \
            if (used == 0) {                                                \
                used = (size_t)snprintf(vm->error, sizeof(vm->error), "%s", \
                                        diamond_vm_status_name(status_));    \
            }                                                               \
            const char *frame_name = chunk->name != nullptr ? chunk->name    \
                                                              : "<chunk>"; \
            const uint32_t line = chunk->lines != nullptr                    \
                ? chunk->lines[instruction_offset] : 0;                      \
            const uint32_t column = chunk->columns != nullptr                \
                ? chunk->columns[instruction_offset] : 0;                    \
            if (used < sizeof(vm->error)) {                                  \
                (void)snprintf(vm->error + used, sizeof(vm->error) - used,   \
                               "\n  at %s:%u:%u", frame_name, line, column);  \
            }                                                               \
        }                                                                   \
    } while (false)

#define VM_RETURN(status_)                   \
    do {                                     \
        RECORD_ERROR(status_);               \
        vm->frames = frame.previous;          \
        return (status_);                     \
    } while (false)

#define READ_BYTE(target_)                   \
    do {                                     \
        if (ip >= chunk->code_count) {       \
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE); \
        }                                    \
        (target_) = chunk->code[ip++];       \
    } while (false)

    while (ip < chunk->code_count) {
        instruction_offset = ip;
        uint8_t instruction = 0;
        READ_BYTE(instruction);

        switch ((DiamondOpCode)instruction) {
            case DIAMOND_OP_CONSTANT: {
                uint8_t destination = 0;
                uint8_t constant = 0;
                READ_BYTE(destination);
                READ_BYTE(constant);
                if ((size_t)constant >= chunk->constant_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[destination] = chunk->constants[constant];
                break;
            }
            case DIAMOND_OP_STRING: {
                uint8_t destination = 0;
                uint8_t string_index = 0;
                READ_BYTE(destination);
                READ_BYTE(string_index);
                if ((size_t)string_index >= chunk->string_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondStringConstant *constant =
                    &chunk->strings[string_index];
                DiamondString *string = allocate_string(
                    vm, constant->chars, constant->length);
                if (string == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination] = DIAMOND_OBJECT(string);
                break;
            }
            case DIAMOND_OP_NIL: {
                uint8_t destination = 0;
                READ_BYTE(destination);
                registers[destination] = DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_BOOL: {
                uint8_t destination = 0;
                uint8_t boolean = 0;
                READ_BYTE(destination);
                READ_BYTE(boolean);
                registers[destination] = DIAMOND_BOOL(boolean != 0);
                break;
            }
            case DIAMOND_OP_MOVE: {
                uint8_t destination = 0;
                uint8_t source = 0;
                READ_BYTE(destination);
                READ_BYTE(source);
                registers[destination] = registers[source];
                break;
            }
            case DIAMOND_OP_ADD: {
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                if (registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT) {
                    int64_t sum = 0;
                    if (ckd_add(&sum, registers[left].as.integer,
                                registers[right].as.integer)) {
                        VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                    }
                    registers[destination] = DIAMOND_INT(sum);
                    break;
                }
                if (registers[left].kind == DIAMOND_VALUE_OBJECT &&
                    registers[right].kind == DIAMOND_VALUE_OBJECT &&
                    registers[left].as.object->kind == DIAMOND_OBJECT_STRING &&
                    registers[right].as.object->kind == DIAMOND_OBJECT_STRING) {
                    const DiamondString *left_string =
                        (const DiamondString *)registers[left].as.object;
                    const DiamondString *right_string =
                        (const DiamondString *)registers[right].as.object;
                    const size_t length = left_string->length + right_string->length;
                    char *chars = malloc(length + 1);
                    if (chars == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    memcpy(chars, left_string->chars, left_string->length);
                    memcpy(chars + left_string->length, right_string->chars,
                           right_string->length);
                    DiamondString *string = allocate_string(vm, chars, length);
                    free(chars);
                    if (string == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination] = DIAMOND_OBJECT(string);
                    break;
                }
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                break;
            }
            case DIAMOND_OP_SUBTRACT_INT:
            case DIAMOND_OP_MULTIPLY_INT:
            case DIAMOND_OP_DIVIDE_INT: {
                const DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                if (registers[left].kind != DIAMOND_VALUE_INT ||
                    registers[right].kind != DIAMOND_VALUE_INT) {
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t left_value = registers[left].as.integer;
                const int64_t right_value = registers[right].as.integer;
                int64_t result_value = 0;
                bool overflow = false;
                if (opcode == DIAMOND_OP_SUBTRACT_INT) {
                    overflow = ckd_sub(&result_value, left_value, right_value);
                } else if (opcode == DIAMOND_OP_MULTIPLY_INT) {
                    overflow = ckd_mul(&result_value, left_value, right_value);
                } else {
                    if (right_value == 0) {
                        VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                    }
                    if (left_value == INT64_MIN && right_value == -1) {
                        VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                    }
                    result_value = left_value / right_value;
                }
                if (overflow) {
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                registers[destination] = DIAMOND_INT(result_value);
                break;
            }
            case DIAMOND_OP_NEGATE_INT: {
                uint8_t destination = 0;
                uint8_t operand = 0;
                READ_BYTE(destination);
                READ_BYTE(operand);
                if (registers[operand].kind != DIAMOND_VALUE_INT) {
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                int64_t result_value = 0;
                if (ckd_sub(&result_value, 0, registers[operand].as.integer)) {
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                registers[destination] = DIAMOND_INT(result_value);
                break;
            }
            case DIAMOND_OP_EQUAL:
            case DIAMOND_OP_NOT_EQUAL: {
                const DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                const bool equal = values_equal(registers[left], registers[right]);
                registers[destination] = DIAMOND_BOOL(
                    opcode == DIAMOND_OP_EQUAL ? equal : !equal);
                break;
            }
            case DIAMOND_OP_LESS_INT:
            case DIAMOND_OP_LESS_EQUAL_INT:
            case DIAMOND_OP_GREATER_INT:
            case DIAMOND_OP_GREATER_EQUAL_INT: {
                const DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                if (registers[left].kind != DIAMOND_VALUE_INT ||
                    registers[right].kind != DIAMOND_VALUE_INT) {
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t a = registers[left].as.integer;
                const int64_t b = registers[right].as.integer;
                bool comparison = false;
                if (opcode == DIAMOND_OP_LESS_INT) comparison = a < b;
                if (opcode == DIAMOND_OP_LESS_EQUAL_INT) comparison = a <= b;
                if (opcode == DIAMOND_OP_GREATER_INT) comparison = a > b;
                if (opcode == DIAMOND_OP_GREATER_EQUAL_INT) comparison = a >= b;
                registers[destination] = DIAMOND_BOOL(comparison);
                break;
            }
            case DIAMOND_OP_JUMP: {
                uint8_t high = 0;
                uint8_t low = 0;
                READ_BYTE(high);
                READ_BYTE(low);
                const size_t target = ((size_t)high << 8) | low;
                if (target > chunk->code_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                ip = target;
                break;
            }
            case DIAMOND_OP_JUMP_IF_FALSE: {
                uint8_t condition = 0;
                uint8_t high = 0;
                uint8_t low = 0;
                READ_BYTE(condition);
                READ_BYTE(high);
                READ_BYTE(low);
                const size_t target = ((size_t)high << 8) | low;
                if (target > chunk->code_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                if (!is_truthy(registers[condition])) {
                    ip = target;
                }
                break;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: {
                uint8_t condition=0,high=0,low=0;
                READ_BYTE(condition);READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                if(target>chunk->code_count) VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(is_truthy(registers[condition])) ip=target;
                break;
            }
            case DIAMOND_OP_CALL: {
                uint8_t destination = 0;
                uint8_t function_index = 0;
                uint8_t argument_base = 0;
                uint8_t call_argument_count = 0;
                READ_BYTE(destination);
                READ_BYTE(function_index);
                READ_BYTE(argument_base);
                READ_BYTE(call_argument_count);
                if ((size_t)function_index >= chunk->function_count ||
                    (size_t)argument_base + call_argument_count >
                        DIAMOND_REGISTER_COUNT) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondFunction *function =
                    &chunk->functions[function_index];
                if (call_argument_count != function->arity) {
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                const DiamondChunk called_chunk = {
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
                DiamondValue call_result = DIAMOND_NIL;
                const DiamondVmStatus status = run_chunk(
                    &called_chunk, vm, &registers[argument_base],
                    call_argument_count, depth + 1, &call_result);
                if (status != DIAMOND_VM_OK) VM_RETURN(status);
                registers[destination] = call_result;
                break;
            }
            case DIAMOND_OP_NEW: {
                uint8_t dest=0,ci=0,base=0,argc=0;
                READ_BYTE(dest);READ_BYTE(ci);READ_BYTE(base);READ_BYTE(argc);
                if(argc>16) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if((size_t)ci>=chunk->class_count) VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondClass *class=&chunk->classes[ci];
                DiamondInstance *instance=allocate_instance(vm,class);
                if(instance==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(instance);
                const DiamondMethod *init=lookup_method(
                    chunk,class,"initialize",sizeof("initialize")-1);
                if(init!=nullptr) {
                    if(init->arity!=argc) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue args[17];args[0]=registers[dest];
                    for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                    const DiamondFunction *fn=&chunk->functions[init->function_index];
                    DiamondChunk child={.name=fn->name,.code=fn->code,
                      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                      .constants=fn->constants,.constant_count=fn->constant_count,
                      .strings=fn->strings,.string_count=fn->string_count,
                      .functions=chunk->functions,.function_count=chunk->function_count,
                      .classes=chunk->classes,.class_count=chunk->class_count};
                    DiamondValue ignored=DIAMOND_NIL;
                    DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,&ignored);
                    if(s!=DIAMOND_VM_OK)VM_RETURN(s);
                } else if(argc!=0) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                break;
            }
            case DIAMOND_OP_INVOKE: {
                uint8_t dest=0,recv=0,name=0,base=0,argc=0;
                READ_BYTE(dest);READ_BYTE(recv);READ_BYTE(name);READ_BYTE(base);READ_BYTE(argc);
                if(argc>16) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE||
                   (size_t)name>=chunk->string_count) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const DiamondStringConstant *method_name=&chunk->strings[name];
                const DiamondMethod *method=lookup_method(
                    chunk,instance->class,method_name->chars,method_name->length);
                if(method==nullptr) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(method->arity!=argc) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17];args[0]=registers[recv];
                for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=&chunk->functions[method->function_index];
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,&call_result);
                if(s!=DIAMOND_VM_OK) VM_RETURN(s);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_SUPER: {
                uint8_t dest=0,owner_index=0,name=0,base=0,argc=0;
                READ_BYTE(dest); READ_BYTE(owner_index); READ_BYTE(name);
                READ_BYTE(base); READ_BYTE(argc);
                if(argc>16 || (size_t)owner_index>=chunk->class_count ||
                   (size_t)name>=chunk->string_count ||
                   registers[0].kind!=DIAMOND_VALUE_OBJECT ||
                   registers[0].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondClass *owner=&chunk->classes[owner_index];
                if(owner->superclass==UINT8_MAX) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondStringConstant *method_name=&chunk->strings[name];
                const DiamondMethod *method=lookup_method(chunk,
                    &chunk->classes[owner->superclass],method_name->chars,
                    method_name->length);
                if(method==nullptr) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(method->arity!=argc) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17]; args[0]=registers[0];
                for(size_t i=0;i<argc;i++) args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=&chunk->functions[method->function_index];
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus status=run_chunk(&child,vm,args,(size_t)argc+1,
                                                  depth+1,&call_result);
                if(status!=DIAMOND_VM_OK) VM_RETURN(status);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_GET_IVAR: {
                uint8_t dest=0,recv=0,field=0;READ_BYTE(dest);READ_BYTE(recv);READ_BYTE(field);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                if((size_t)field>=instance->field_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=instance->fields[field];break;
            }
            case DIAMOND_OP_SET_IVAR: {
                uint8_t recv=0,field=0,source=0;READ_BYTE(recv);READ_BYTE(field);READ_BYTE(source);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                if((size_t)field>=instance->field_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                instance->fields[field]=registers[source];break;
            }
            case DIAMOND_OP_CHECK_TYPE: {
                uint8_t source=0,type=0; READ_BYTE(source); READ_BYTE(type);
                if(!value_matches_type(chunk,registers[source],type)) {
                    char expected[80]; char actual[80];
                    format_type(expected,sizeof expected,chunk,type);
                    format_value_type(actual,sizeof actual,registers[source]);
                    snprintf(vm->error,sizeof vm->error,"expected %s, got %s",
                             expected,actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                break;
            }
            case DIAMOND_OP_ARRAY: {
                uint8_t destination=0,base=0,count=0;
                READ_BYTE(destination);READ_BYTE(base);READ_BYTE(count);
                if((size_t)base+count>DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondArray *array=allocate_array(vm,&registers[base],count);
                if(array==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(array);
                break;
            }
            case DIAMOND_OP_INDEX_GET: {
                uint8_t destination=0,receiver=0,index_register=0;
                READ_BYTE(destination);READ_BYTE(receiver);READ_BYTE(index_register);
                if(registers[receiver].kind!=DIAMOND_VALUE_OBJECT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_HASH) {
                    DiamondHash *hash=(DiamondHash *)registers[receiver].as.object;
                    const ptrdiff_t found=hash_find(hash,registers[index_register]);
                    registers[destination]=found<0 ? DIAMOND_NIL
                        : hash->entries[(size_t)found].value;
                    break;
                }
                if(registers[receiver].as.object->kind!=DIAMOND_OBJECT_ARRAY ||
                   registers[index_register].kind!=DIAMOND_VALUE_INT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondArray *array=(DiamondArray *)registers[receiver].as.object;
                const int64_t index=registers[index_register].as.integer;
                if(index<0 || (uint64_t)index>=array->count) {
                    snprintf(vm->error,sizeof vm->error,
                             "index %" PRId64 " out of bounds for Array of length %zu",
                             index,array->count);
                    VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                }
                registers[destination]=array->values[(size_t)index];
                break;
            }
            case DIAMOND_OP_INDEX_SET: {
                uint8_t receiver=0,index_register=0,source=0;
                READ_BYTE(receiver);READ_BYTE(index_register);READ_BYTE(source);
                if(registers[receiver].kind!=DIAMOND_VALUE_OBJECT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_HASH) {
                    if(!hash_set(vm,(DiamondHash *)registers[receiver].as.object,
                                 registers[index_register],registers[source]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    break;
                }
                if(registers[receiver].as.object->kind!=DIAMOND_OBJECT_ARRAY ||
                   registers[index_register].kind!=DIAMOND_VALUE_INT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondArray *array=(DiamondArray *)registers[receiver].as.object;
                const int64_t index=registers[index_register].as.integer;
                if(index<0 || (uint64_t)index>=array->count) {
                    snprintf(vm->error,sizeof vm->error,
                             "index %" PRId64 " out of bounds for Array of length %zu",
                             index,array->count);
                    VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                }
                array->values[(size_t)index]=registers[source];
                break;
            }
            case DIAMOND_OP_HASH: {
                uint8_t destination=0,base=0,count=0;
                READ_BYTE(destination);READ_BYTE(base);READ_BYTE(count);
                if((size_t)base+(size_t)count*2>DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondHash *hash=allocate_hash(vm);
                if(hash==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(hash);
                for(size_t i=0;i<count;i++) {
                    if(!hash_set(vm,hash,registers[(size_t)base+i*2],
                                 registers[(size_t)base+i*2+1]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                break;
            }
            case DIAMOND_OP_NOT: {
                uint8_t destination=0,source=0;
                READ_BYTE(destination);READ_BYTE(source);
                registers[destination]=DIAMOND_BOOL(!is_truthy(registers[source]));
                break;
            }
            case DIAMOND_OP_RETURN: {
                uint8_t source = 0;
                READ_BYTE(source);
                *result = registers[source];
                VM_RETURN(DIAMOND_VM_OK);
            }
            default:
                VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        }
    }

#undef READ_BYTE
#undef VM_RETURN
#undef RECORD_ERROR

    vm->frames = frame.previous;
    return DIAMOND_VM_INVALID_BYTECODE;
}

DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result) {
    vm->error[0]='\0';
    return run_chunk(chunk, vm, nullptr, 0, 0, result);
}

const char *diamond_vm_error(const DiamondVm *vm) {
    return vm->error[0]=='\0' ? nullptr : vm->error;
}

const char *diamond_vm_status_name(DiamondVmStatus status) {
    switch (status) {
        case DIAMOND_VM_OK:
            return "ok";
        case DIAMOND_VM_INVALID_BYTECODE:
            return "invalid bytecode";
        case DIAMOND_VM_TYPE_ERROR:
            return "type error";
        case DIAMOND_VM_INTEGER_OVERFLOW:
            return "integer overflow";
        case DIAMOND_VM_DIVISION_BY_ZERO:
            return "division by zero";
        case DIAMOND_VM_ARITY_ERROR:
            return "wrong number of arguments";
        case DIAMOND_VM_STACK_OVERFLOW:
            return "call stack overflow";
        case DIAMOND_VM_OUT_OF_MEMORY:
            return "out of memory";
        case DIAMOND_VM_INDEX_ERROR:
            return "array index out of bounds";
    }
    return "unknown VM status";
}
