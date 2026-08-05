#ifndef DIAMOND_VM_H
#define DIAMOND_VM_H

#include "value.h"
#include "object.h"

#include <stddef.h>
#include <stdint.h>

enum {
    DIAMOND_MAX_CODE = 1024,
    DIAMOND_MAX_CONSTANTS = 256,
    DIAMOND_MAX_FUNCTIONS = 32,
    DIAMOND_MAX_FUNCTION_NAME = 64,
    DIAMOND_MAX_STRING_CONSTANTS = 64,
    DIAMOND_MAX_STRING_LENGTH = 255,
    DIAMOND_MAX_CLASSES = 32,
    DIAMOND_MAX_METHODS = 32,
    DIAMOND_MAX_FIELDS = 32,
};

typedef enum DiamondOpCode : uint8_t {
    DIAMOND_OP_CONSTANT,
    DIAMOND_OP_STRING,
    DIAMOND_OP_NIL,
    DIAMOND_OP_BOOL,
    DIAMOND_OP_MOVE,
    DIAMOND_OP_ADD,
    DIAMOND_OP_SUBTRACT_INT,
    DIAMOND_OP_MULTIPLY_INT,
    DIAMOND_OP_DIVIDE_INT,
    DIAMOND_OP_NEGATE_INT,
    DIAMOND_OP_EQUAL,
    DIAMOND_OP_NOT_EQUAL,
    DIAMOND_OP_LESS_INT,
    DIAMOND_OP_LESS_EQUAL_INT,
    DIAMOND_OP_GREATER_INT,
    DIAMOND_OP_GREATER_EQUAL_INT,
    DIAMOND_OP_JUMP,
    DIAMOND_OP_JUMP_IF_FALSE,
    DIAMOND_OP_CALL,
    DIAMOND_OP_NEW,
    DIAMOND_OP_INVOKE,
    DIAMOND_OP_SUPER,
    DIAMOND_OP_GET_IVAR,
    DIAMOND_OP_SET_IVAR,
    DIAMOND_OP_CHECK_TYPE,
    DIAMOND_OP_ARRAY,
    DIAMOND_OP_INDEX_GET,
    DIAMOND_OP_INDEX_SET,
    DIAMOND_OP_HASH,
    DIAMOND_OP_NOT,
    DIAMOND_OP_JUMP_IF_TRUE,
    DIAMOND_OP_RETURN,
} DiamondOpCode;

typedef enum DiamondTypeId : uint8_t {
    DIAMOND_TYPE_INT,
    DIAMOND_TYPE_STRING,
    DIAMOND_TYPE_BOOL,
    DIAMOND_TYPE_NIL,
    DIAMOND_TYPE_ARRAY,
    DIAMOND_TYPE_HASH,
    DIAMOND_TYPE_CLASS_BASE,
} DiamondTypeId;

enum { DIAMOND_TYPE_NILABLE = 0x80 };

typedef struct DiamondStringConstant {
    char chars[DIAMOND_MAX_STRING_LENGTH + 1];
    size_t length;
} DiamondStringConstant;

typedef struct DiamondMethod {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t function_index;
    uint8_t arity;
} DiamondMethod;

typedef struct DiamondClass {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t superclass;
    DiamondMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    char fields[DIAMOND_MAX_FIELDS][DIAMOND_MAX_FUNCTION_NAME];
    size_t field_count;
} DiamondClass;

typedef struct DiamondFunction {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t code[DIAMOND_MAX_CODE];
    size_t code_count;
    DiamondValue constants[DIAMOND_MAX_CONSTANTS];
    size_t constant_count;
    DiamondStringConstant strings[DIAMOND_MAX_STRING_CONSTANTS];
    size_t string_count;
    uint8_t arity;
    uint8_t owner_class;
} DiamondFunction;

typedef struct DiamondChunk {
    const uint8_t *code;
    size_t code_count;
    const DiamondValue *constants;
    size_t constant_count;
    const DiamondStringConstant *strings;
    size_t string_count;
    const DiamondFunction *functions;
    size_t function_count;
    const DiamondClass *classes;
    size_t class_count;
} DiamondChunk;

typedef enum DiamondVmStatus : uint8_t {
    DIAMOND_VM_OK,
    DIAMOND_VM_INVALID_BYTECODE,
    DIAMOND_VM_TYPE_ERROR,
    DIAMOND_VM_INTEGER_OVERFLOW,
    DIAMOND_VM_DIVISION_BY_ZERO,
    DIAMOND_VM_ARITY_ERROR,
    DIAMOND_VM_STACK_OVERFLOW,
    DIAMOND_VM_OUT_OF_MEMORY,
    DIAMOND_VM_INDEX_ERROR,
} DiamondVmStatus;

typedef struct DiamondVm {
    DiamondObject *objects;
    size_t bytes_allocated;
    size_t next_gc;
    void *frames;
    bool stress_gc;
    char error[192];
} DiamondVm;

void diamond_vm_init(DiamondVm *vm);
void diamond_vm_free(DiamondVm *vm);
void diamond_vm_collect(DiamondVm *vm);
DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result);
const char *diamond_vm_status_name(DiamondVmStatus status);
const char *diamond_vm_error(const DiamondVm *vm);

#endif
