#ifndef DIAMOND_VM_H
#define DIAMOND_VM_H

#include "value.h"
#include "object.h"

#include <stddef.h>
#include <stdint.h>

enum {
    DIAMOND_MAX_CODE = 1024,
    DIAMOND_MAX_CONSTANTS = 256,
    DIAMOND_MAX_FUNCTIONS = 64,
    DIAMOND_MAX_FUNCTION_NAME = 64,
    DIAMOND_MAX_STRING_CONSTANTS = 64,
    DIAMOND_MAX_STRING_LENGTH = 255,
    DIAMOND_MAX_CLASSES = 32,
    DIAMOND_MAX_INTERFACES = 16,
    DIAMOND_MAX_MODULES = 16,
    DIAMOND_MAX_TYPE_SETS = 64,
    DIAMOND_MAX_UNION_TYPES = 8,
    DIAMOND_MAX_METHODS = 32,
    DIAMOND_MAX_FIELDS = 32,
    DIAMOND_MAX_NAMESPACE_CONSTANTS = 64,
    DIAMOND_MAX_FIBER_FRAMES = 256,
};

typedef enum DiamondOpCode : uint8_t {
    DIAMOND_OP_CONSTANT,
    DIAMOND_OP_STRING,
    DIAMOND_OP_NIL,
    DIAMOND_OP_BOOL,
    DIAMOND_OP_MOVE,
    DIAMOND_OP_ADD,
    DIAMOND_OP_ADD_INT,
    DIAMOND_OP_SUBTRACT,
    DIAMOND_OP_MULTIPLY,
    DIAMOND_OP_DIVIDE,
    DIAMOND_OP_SUBTRACT_INT,
    DIAMOND_OP_MULTIPLY_INT,
    DIAMOND_OP_DIVIDE_INT,
    DIAMOND_OP_LESS,
    DIAMOND_OP_LESS_EQUAL,
    DIAMOND_OP_GREATER,
    DIAMOND_OP_GREATER_EQUAL,
    DIAMOND_OP_NEGATE_INT,
    DIAMOND_OP_EQUAL,
    DIAMOND_OP_NOT_EQUAL,
    DIAMOND_OP_EQUAL_INT,
    DIAMOND_OP_NOT_EQUAL_INT,
    DIAMOND_OP_LESS_INT,
    DIAMOND_OP_LESS_EQUAL_INT,
    DIAMOND_OP_GREATER_INT,
    DIAMOND_OP_GREATER_EQUAL_INT,
    DIAMOND_OP_JUMP,
    DIAMOND_OP_JUMP_IF_FALSE,
    DIAMOND_OP_CALL,
    DIAMOND_OP_CALL_TYPED,
    DIAMOND_OP_CLOSURE,
    DIAMOND_OP_CALL_CLOSURE,
    DIAMOND_OP_GET_CAPTURE,
    DIAMOND_OP_GET_CAPTURE_CELL,
    DIAMOND_OP_SET_CAPTURE,
    DIAMOND_OP_BOX_LOCAL,
    DIAMOND_OP_GET_CELL,
    DIAMOND_OP_SET_CELL,
    DIAMOND_OP_NEW,
    DIAMOND_OP_INVOKE,
    DIAMOND_OP_INVOKE_MONO,
    DIAMOND_OP_INVOKE_TYPED,
    DIAMOND_OP_SUPER,
    DIAMOND_OP_GET_IVAR,
    DIAMOND_OP_SET_IVAR,
    DIAMOND_OP_GET_IVAR_NAME,
    DIAMOND_OP_SET_IVAR_NAME,
    DIAMOND_OP_GET_NAMESPACE_CONSTANT,
    DIAMOND_OP_SET_NAMESPACE_CONSTANT,
    DIAMOND_OP_CHECK_TYPE,
    DIAMOND_OP_ARRAY,
    DIAMOND_OP_INDEX_GET,
    DIAMOND_OP_INDEX_SET,
    DIAMOND_OP_HASH,
    DIAMOND_OP_NOT,
    DIAMOND_OP_JUMP_IF_TRUE,
    DIAMOND_OP_RETURN,
    DIAMOND_OP_RAISE,
    DIAMOND_OP_PUSH_RESCUE,
    DIAMOND_OP_POP_RESCUE,
    DIAMOND_OP_PUSH_ENSURE,
    DIAMOND_OP_RUN_ENSURE,
    DIAMOND_OP_END_ENSURE,
    DIAMOND_OP_IS_TYPE,
    DIAMOND_OP_ARGUMENT_PROVIDED,
    DIAMOND_OP_TO_STRING,
    DIAMOND_OP_COUNT,
} DiamondOpCode;

typedef enum DiamondTypeId : uint8_t {
    DIAMOND_TYPE_INT,
    DIAMOND_TYPE_STRING,
    DIAMOND_TYPE_BOOL,
    DIAMOND_TYPE_NIL,
    DIAMOND_TYPE_ARRAY,
    DIAMOND_TYPE_HASH,
    DIAMOND_TYPE_CALLABLE,
    DIAMOND_TYPE_SIZED,
    DIAMOND_TYPE_CLASS_BASE,
    DIAMOND_TYPE_VARIABLE_BASE = 96,
    DIAMOND_TYPE_INTERFACE_BASE = 128,
} DiamondTypeId;

enum { DIAMOND_INLINE_CACHE_COUNT = 64 };
enum { DIAMOND_INLINE_CACHE_WIDTH = 4 };

typedef enum DiamondBuiltinClass : uint8_t {
    DIAMOND_CLASS_EXCEPTION,
    DIAMOND_CLASS_STANDARD_ERROR,
    DIAMOND_CLASS_RUNTIME_ERROR,
    DIAMOND_CLASS_TYPE_ERROR,
    DIAMOND_CLASS_ARGUMENT_ERROR,
    DIAMOND_CLASS_INDEX_ERROR,
    DIAMOND_CLASS_ZERO_DIVISION_ERROR,
    DIAMOND_CLASS_RANGE_ERROR,
    DIAMOND_CLASS_SYSTEM_STACK_ERROR,
    DIAMOND_BUILTIN_CLASS_COUNT,
} DiamondBuiltinClass;

typedef struct DiamondStringConstant {
    char chars[DIAMOND_MAX_STRING_LENGTH + 1];
    size_t length;
} DiamondStringConstant;

typedef struct DiamondTypeMember {
    uint8_t id;
    uint8_t argument_set;
    uint8_t second_argument_set;
    uint8_t callable_arity;
    uint8_t callable_return_set;
    bool callable_parameters_typed;
    uint8_t callable_parameter_sets[16];
} DiamondTypeMember;

typedef struct DiamondTypeSet {
    DiamondTypeMember members[DIAMOND_MAX_UNION_TYPES];
    uint8_t count;
} DiamondTypeSet;

typedef struct DiamondMethod {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t function_index;
    uint8_t arity;
    uint8_t required_arity;
    bool included;
    bool is_private;
    bool needs_receiver;
} DiamondMethod;

typedef struct DiamondInterfaceMethod {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t arity;
    uint8_t parameter_type_sets[16];
    uint8_t return_type_set;
} DiamondInterfaceMethod;

typedef struct DiamondInterface {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    DiamondInterfaceMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    const DiamondTypeSet *type_sets;
} DiamondInterface;

typedef struct DiamondModule {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    DiamondMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    DiamondMethod singleton_methods[DIAMOND_MAX_METHODS];
    size_t singleton_method_count;
    char fields[DIAMOND_MAX_FIELDS][DIAMOND_MAX_FUNCTION_NAME];
    size_t field_count;
} DiamondModule;

typedef struct DiamondClass DiamondClass;

typedef struct DiamondShape {
    const DiamondClass *class;
    uint8_t field_count;
} DiamondShape;

struct DiamondClass {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t superclass;
    DiamondMethod methods[DIAMOND_MAX_METHODS];
    size_t method_count;
    DiamondMethod singleton_methods[DIAMOND_MAX_METHODS];
    size_t singleton_method_count;
    char fields[DIAMOND_MAX_FIELDS][DIAMOND_MAX_FUNCTION_NAME];
    size_t field_count;
    DiamondShape shapes[DIAMOND_MAX_FIELDS + 1];
};

typedef struct DiamondFunction {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    uint8_t code[DIAMOND_MAX_CODE];
    uint32_t lines[DIAMOND_MAX_CODE];
    uint32_t columns[DIAMOND_MAX_CODE];
    size_t code_count;
    DiamondValue constants[DIAMOND_MAX_CONSTANTS];
    size_t constant_count;
    DiamondStringConstant strings[DIAMOND_MAX_STRING_CONSTANTS];
    size_t string_count;
    DiamondTypeSet type_sets[DIAMOND_MAX_TYPE_SETS];
    size_t type_set_count;
    uint8_t arity;
    uint8_t required_arity;
    uint8_t owner_class;
    bool nested;
    uint8_t capture_count;
    uint8_t return_type_set;
    uint8_t parameter_type_sets[16];
    char type_variables[8][DIAMOND_MAX_FUNCTION_NAME];
    uint8_t type_variable_count;
    bool uses_instance_state;
} DiamondFunction;

typedef struct DiamondChunk {
    const char *name;
    const uint8_t *code;
    const uint32_t *lines;
    const uint32_t *columns;
    size_t code_count;
    const DiamondValue *constants;
    size_t constant_count;
    const DiamondStringConstant *strings;
    size_t string_count;
    const DiamondTypeSet *type_sets;
    size_t type_set_count;
    const DiamondFunction *functions;
    size_t function_count;
    const DiamondClass *classes;
    size_t class_count;
    const DiamondInterface *interfaces;
    size_t interface_count;
    const uint8_t *parameter_type_sets;
    uint8_t type_variable_count;
    uint8_t parameter_offset;
    const DiamondTypeBinding *type_variable_bindings;
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
    DIAMOND_VM_EXCEPTION,
} DiamondVmStatus;

typedef struct DiamondMethodCacheEntry {
    const DiamondClass *receiver_class;
    const DiamondMethod *method;
} DiamondMethodCacheEntry;

typedef struct DiamondMethodCache {
    const uint8_t *site;
    DiamondMethodCacheEntry entries[DIAMOND_INLINE_CACHE_WIDTH];
    uint8_t entry_count;
    uint8_t next_replace;
    size_t hits;
    size_t misses;
} DiamondMethodCache;

typedef struct DiamondFieldCacheEntry {
    const DiamondShape *input_shape;
    const DiamondShape *output_shape;
    bool materialized;
} DiamondFieldCacheEntry;

typedef struct DiamondFieldCache {
    const uint8_t *site;
    DiamondFieldCacheEntry entries[DIAMOND_INLINE_CACHE_WIDTH];
    uint8_t entry_count;
    uint8_t next_replace;
} DiamondFieldCache;

typedef enum DiamondFiberState : uint8_t {
    DIAMOND_FIBER_NEW,
    DIAMOND_FIBER_RUNNABLE,
    DIAMOND_FIBER_RUNNING,
    DIAMOND_FIBER_SUSPENDED,
    DIAMOND_FIBER_COMPLETED,
    DIAMOND_FIBER_FAILED,
} DiamondFiberState;

typedef struct DiamondFiberFrame {
    const DiamondChunk *chunk;
    size_t instruction;
    size_t depth;
} DiamondFiberFrame;

typedef struct DiamondFiber {
    DiamondFiberState state;
    const DiamondChunk *chunk;
    DiamondValue result;
    DiamondVmStatus status;
    DiamondFiberFrame *frames;
    size_t frame_count;
    size_t frame_capacity;
} DiamondFiber;

typedef enum DiamondFiberStatus : uint8_t {
    DIAMOND_FIBER_OK,
    DIAMOND_FIBER_INVALID_STATE,
} DiamondFiberStatus;

typedef struct DiamondFiberQueue {
    DiamondFiber **items;
    size_t count;
    size_t capacity;
    size_t head;
} DiamondFiberQueue;

typedef struct DiamondVm {
    DiamondObject *objects;
    size_t bytes_allocated;
    size_t next_gc;
    void *frames;
    bool stress_gc;
    DiamondMethodCache method_caches[DIAMOND_INLINE_CACHE_COUNT];
    DiamondFieldCache field_caches[DIAMOND_INLINE_CACHE_COUNT];
    size_t inline_cache_hits;
    size_t inline_cache_misses;
    size_t monomorphic_dispatches;
    size_t method_cache_probes;
    size_t monomorphic_threshold;
    size_t direct_dispatch_rewrites;
    const uint8_t *rewritten_sites[DIAMOND_MAX_CODE];
    size_t rewritten_site_count;
    size_t field_cache_hits;
    size_t field_cache_misses;
    size_t shape_transitions;
    size_t opcode_counts[DIAMOND_OP_COUNT];
    bool quickening;
    size_t quickening_threshold;
    size_t quickening_observations;
    size_t quickened_sites;
    size_t deoptimized_sites;
    DiamondValue namespace_constants[DIAMOND_MAX_NAMESPACE_CONSTANTS];
    bool namespace_constant_initialized[DIAMOND_MAX_NAMESPACE_CONSTANTS];
    DiamondValue exception;
    bool has_exception;
    char error[1024];
} DiamondVm;

void diamond_vm_init(DiamondVm *vm);
void diamond_vm_free(DiamondVm *vm);
void diamond_vm_collect(DiamondVm *vm);
void diamond_vm_invalidate_method_caches(DiamondVm *vm);
DiamondFiber *diamond_fiber_new(const DiamondChunk *chunk);
void diamond_fiber_free(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *fiber);
const char *diamond_fiber_state_name(DiamondFiberState state);
DiamondFiberStatus diamond_fiber_make_runnable(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_begin(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_suspend(DiamondFiber *fiber);
DiamondFiberStatus diamond_fiber_complete(DiamondFiber *fiber, DiamondValue result);
DiamondFiberStatus diamond_fiber_fail(DiamondFiber *fiber, DiamondVmStatus status);
bool diamond_fiber_push_frame(DiamondFiber *fiber, DiamondFiberFrame frame);
bool diamond_fiber_pop_frame(DiamondFiber *fiber, DiamondFiberFrame *frame);
bool diamond_fiber_update_instruction(DiamondFiber *fiber, size_t instruction);
bool diamond_fiber_update_depth(DiamondFiber *fiber, size_t depth);
void diamond_fiber_queue_init(DiamondFiberQueue *queue);
void diamond_fiber_queue_free(DiamondFiberQueue *queue);
bool diamond_fiber_queue_push(DiamondFiberQueue *queue, DiamondFiber *fiber);
DiamondFiber *diamond_fiber_queue_pop(DiamondFiberQueue *queue);
size_t diamond_fiber_queue_count(const DiamondFiberQueue *queue);
DiamondFiber *diamond_fiber_queue_at(const DiamondFiberQueue *queue, size_t index);
DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result);
const char *diamond_vm_status_name(DiamondVmStatus status);
const char *diamond_vm_error(const DiamondVm *vm);

#endif
