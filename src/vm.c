#define _DEFAULT_SOURCE

#include "vm.h"
#include "bignum.h"
#include "compiler.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/* Each run_chunk activation unconditionally allocates DiamondValue
 * registers[256] (4KB), DiamondTypeBinding bindings[8] (~3.2KB), and
 * UnwindHandler handlers[16] (~0.5KB) as C-stack locals, regardless of the
 * called function's actual complexity. Measured against this machine's
 * default 8MB stack, real (non-instrumented) recursion segfaults around
 * depth ~210-220 in a debug (-O0) build and ~150-160 under AddressSanitizer's
 * redzone-inflated frames -- both well below the depth this counter used to
 * allow, so the guard never had a chance to trip before the native stack
 * actually overflowed. 100 leaves comfortable margin under the tightest
 * (sanitizer) measurement. */
enum { DIAMOND_MAX_CALL_DEPTH = 100 };

typedef enum HandlerKind : uint8_t { HANDLER_RESCUE, HANDLER_ENSURE } HandlerKind;

typedef struct UnwindHandler {
    HandlerKind kind;
    size_t target;
    uint8_t destination;
    uint8_t type_count;
    uint8_t types[8];
    bool enabled;
} UnwindHandler;

typedef enum PendingKind : uint8_t {
    PENDING_NONE,
    PENDING_NORMAL,
    PENDING_RETURN,
    PENDING_EXCEPTION,
} PendingKind;

typedef struct PendingUnwind {
    PendingKind kind;
    DiamondValue value;
    size_t continuation;
} PendingUnwind;

typedef struct DiamondFrame {
    struct DiamondFrame *previous;
    DiamondValue *registers;
    PendingUnwind *pending;
    size_t register_count;
} DiamondFrame;

static void mark_value(DiamondValue value);
static void mark_frame_chain(void *frames);
static DiamondVmStatus run_chunk(const DiamondChunk *chunk, DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
                                 DiamondValue *result);

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
    } else if(object->kind==DIAMOND_OBJECT_CLOSURE) {
        DiamondClosure *closure=(DiamondClosure *)object;
        for(size_t i=0;i<closure->capture_count;i++)mark_value(closure->captures[i]);
    } else if(object->kind==DIAMOND_OBJECT_CELL) {
        mark_value(((DiamondCell *)object)->value);
    } else if(object->kind==DIAMOND_OBJECT_FIBER) {
        DiamondFiber *fiber=((DiamondFiberHandle *)object)->fiber;
        if(fiber!=nullptr) {
            mark_frame_chain(fiber->native_frames);
            mark_value(fiber->result);
            mark_value(fiber->resume_value);
            if(fiber->entry_closure!=nullptr)
                mark_object((DiamondObject *)fiber->entry_closure);
        }
    }
}

static void mark_value(DiamondValue value) {
    if (value.kind == DIAMOND_VALUE_OBJECT) mark_object(value.as.object);
}

static void mark_frame_chain(void *frames) {
    for (DiamondFrame *frame = frames; frame != nullptr;
         frame = frame->previous) {
        for (size_t index = 0; index < frame->register_count; index++) {
            mark_value(frame->registers[index]);
        }
        if(frame->pending!=nullptr && frame->pending->kind!=PENDING_NONE)
            mark_value(frame->pending->value);
    }
}

static void mark_fiber(const DiamondFiber *fiber) {
    if (fiber == nullptr) return;
    mark_frame_chain(fiber->native_frames);
}

void diamond_vm_collect(DiamondVm *vm) {
    if(vm->has_exception)mark_value(vm->exception);
    for(size_t index=0;index<DIAMOND_MAX_NAMESPACE_CONSTANTS;index++)
        if(vm->namespace_constant_initialized[index])
            mark_value(vm->namespace_constants[index]);
    mark_frame_chain(vm->frames);
    for (DiamondFiber *ancestor = vm->running_fiber; ancestor != nullptr;
         ancestor = ancestor->resumer_fiber)
        mark_frame_chain(ancestor->resumer_frames);
    if (vm->root_queue != nullptr)
        for (size_t index = 0; index < diamond_fiber_queue_count(vm->root_queue); index++)
            mark_fiber(diamond_fiber_queue_at(vm->root_queue, index));

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
            DiamondArray *array=(DiamondArray *)unreached;
            size=sizeof(DiamondArray)+array->capacity*sizeof(DiamondValue);
            for(size_t index=0;index<array->constraint_count;index++)
                free(array->constraints[index].type_variable_bindings);
            free(array->values);
        } else if(unreached->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)unreached;
            size=sizeof(DiamondHash)+hash->capacity*sizeof(DiamondHashEntry)+
                hash->bucket_capacity*sizeof(size_t);
            for(size_t index=0;index<hash->constraint_count;index++)
                free(hash->constraints[index].type_variable_bindings);
            free(hash->entries);free(hash->buckets);
        } else if(unreached->kind==DIAMOND_OBJECT_CLOSURE) {
            size=sizeof(DiamondClosure);
        } else if(unreached->kind==DIAMOND_OBJECT_FIBER) {
            size=sizeof(DiamondFiberHandle);
            diamond_fiber_free(((DiamondFiberHandle *)unreached)->fiber);
        } else if(unreached->kind==DIAMOND_OBJECT_FILE) {
            size=sizeof(DiamondFileHandle);
            FILE *stream=((DiamondFileHandle *)unreached)->stream;
            if(stream!=nullptr)fclose(stream);
        } else if(unreached->kind==DIAMOND_OBJECT_LISTENER) {
            size=sizeof(DiamondListenerHandle);
            const int fd=((DiamondListenerHandle *)unreached)->fd;
            if(fd>=0)close(fd);
        } else if(unreached->kind==DIAMOND_OBJECT_BIGNUM) {
            const DiamondBignum *bignum=(const DiamondBignum *)unreached;
            size=sizeof(DiamondBignum)+bignum->limb_count*sizeof(uint32_t);
        } else if(unreached->kind==DIAMOND_OBJECT_SYMBOL) {
            const DiamondSymbol *symbol=(const DiamondSymbol *)unreached;
            size=sizeof(DiamondSymbol)+symbol->length+1;
        } else if(unreached->kind==DIAMOND_OBJECT_REGEXP) {
            size=sizeof(DiamondRegexp);
            reginold_regex_free(((DiamondRegexp *)unreached)->handle);
        } else if(unreached->kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
            size=sizeof(DiamondProgramBuilder)+sizeof(DiamondProgram);
            free(((DiamondProgramBuilder *)unreached)->program);
        } else {
            size=sizeof(DiamondCell);
        }
        vm->bytes_allocated -= size;
        free(unreached);
    }
    vm->next_gc = vm->bytes_allocated < 1024
        ? 2048 : vm->bytes_allocated * 2;
}

void diamond_vm_init(DiamondVm *vm) {
    *vm = (DiamondVm){.next_gc = 2048};
    vm->quickening_threshold = 1;
    vm->monomorphic_threshold = 1;
}

void diamond_vm_bind_fiber_queue(DiamondVm *vm, const DiamondFiberQueue *queue) {
    if(vm==nullptr)return;
    vm->root_queue=queue;
}

void diamond_vm_free(DiamondVm *vm) {
    DiamondObject *object = vm->objects;
    while (object != nullptr) {
        DiamondObject *next = object->next;
        if(object->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)object;
            for(size_t index=0;index<hash->constraint_count;index++)
                free(hash->constraints[index].type_variable_bindings);
            free(hash->entries);free(hash->buckets);
        } else if(object->kind==DIAMOND_OBJECT_ARRAY) {
            DiamondArray *array=(DiamondArray *)object;
            for(size_t index=0;index<array->constraint_count;index++)
                free(array->constraints[index].type_variable_bindings);
            free(array->values);
        } else if(object->kind==DIAMOND_OBJECT_FIBER) {
            diamond_fiber_free(((DiamondFiberHandle *)object)->fiber);
        } else if(object->kind==DIAMOND_OBJECT_FILE) {
            FILE *stream=((DiamondFileHandle *)object)->stream;
            if(stream!=nullptr)fclose(stream);
        } else if(object->kind==DIAMOND_OBJECT_LISTENER) {
            const int fd=((DiamondListenerHandle *)object)->fd;
            if(fd>=0)close(fd);
        } else if(object->kind==DIAMOND_OBJECT_REGEXP) {
            reginold_regex_free(((DiamondRegexp *)object)->handle);
        } else if(object->kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
            free(((DiamondProgramBuilder *)object)->program);
        }
        free(object);
        object = next;
    }
    *vm = (DiamondVm){};
}

void diamond_vm_invalidate_method_caches(DiamondVm *vm) {
    memset(vm->method_caches,0,sizeof vm->method_caches);
    vm->inline_cache_hits=0;
    vm->inline_cache_misses=0;
    vm->monomorphic_dispatches=0;
    vm->method_cache_probes=0;
}

enum { DIAMOND_FIBER_STACK_SIZE = 8 * 1024 * 1024 };

static bool allocate_fiber_stack(DiamondFiber *fiber) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t total = DIAMOND_FIBER_STACK_SIZE + page;
    void *base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return false;
    if (mprotect(base, page, PROT_NONE) != 0) {
        munmap(base, total);
        return false;
    }
    fiber->stack = base;
    fiber->stack_size = total;
    return true;
}

static void free_fiber_stack(DiamondFiber *fiber) {
    if (fiber->stack != nullptr) {
        munmap(fiber->stack, fiber->stack_size);
        fiber->stack = nullptr;
        fiber->stack_size = 0;
    }
}

static DiamondFiber *diamond_fiber_entering;

static void diamond_fiber_trampoline(void) {
    DiamondFiber *self = diamond_fiber_entering;
    if (self->entry_closure != nullptr) {
        const DiamondFunction *fn=
            &self->program_tables.functions[self->entry_closure->function_index];
        DiamondChunk child={.name=fn->name,.code=fn->code,.lines=fn->lines,
          .columns=fn->columns,.code_count=fn->code_count,.constants=fn->constants,
          .constant_count=fn->constant_count,.strings=fn->strings,.string_count=fn->string_count,
          .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
          .functions=self->program_tables.functions,.function_count=self->program_tables.function_count,
          .classes=self->program_tables.classes,.class_count=self->program_tables.class_count,
          .interfaces=self->program_tables.interfaces,.interface_count=self->program_tables.interface_count,
          .parameter_type_sets=fn->parameter_type_sets,
          .type_variable_count=fn->type_variable_count,
          .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
          .register_count=fn->register_count};
        self->status=run_chunk(&child,self->vm,nullptr,0,0,self->entry_closure,&self->result);
    } else {
        self->status = run_chunk(self->chunk, self->vm, nullptr, 0, 0, nullptr,
                                 &self->result);
    }
    swapcontext(&self->context, self->resume_target);
}

DiamondFiber *diamond_fiber_new(const DiamondChunk *chunk) {
    DiamondFiber *fiber=calloc(1,sizeof *fiber);
    if(fiber==nullptr)return nullptr;
    fiber->state=DIAMOND_FIBER_NEW;
    fiber->chunk=chunk;
    fiber->result=DIAMOND_NIL;
    fiber->status=DIAMOND_VM_OK;
    return fiber;
}

/* Builds a fiber that invokes a specific closure rather than running a whole
 * chunk from ip=0. Only the sub-tables that are provably stable for the
 * process's lifetime are copied by value -- never chunk->code/constants,
 * which belong to whatever transient stack-local chunk existed at the call
 * site -- so the fiber never retains a pointer that could dangle once that
 * call site returns, however deeply nested Fiber.new(...) was called from. */
static DiamondFiber *diamond_fiber_new_for_closure(
        const DiamondChunk *chunk, const DiamondClosure *closure) {
    DiamondFiber *fiber=diamond_fiber_new(nullptr);
    if(fiber==nullptr)return nullptr;
    fiber->entry_closure=closure;
    fiber->program_tables=(DiamondChunk){
        .functions=chunk->functions,.function_count=chunk->function_count,
        .classes=chunk->classes,.class_count=chunk->class_count,
        .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
    };
    return fiber;
}

void diamond_fiber_free(DiamondFiber *fiber) {
    if(fiber==nullptr)return;
    free_fiber_stack(fiber);
    free(fiber);
}

DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_NEW||
       (fiber->chunk==nullptr&&fiber->entry_closure==nullptr))
        return DIAMOND_FIBER_INVALID_STATE;
    if(!allocate_fiber_stack(fiber))return DIAMOND_FIBER_INVALID_STATE;
    if(getcontext(&fiber->context)!=0) {
        free_fiber_stack(fiber);
        return DIAMOND_FIBER_INVALID_STATE;
    }
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    fiber->context.uc_stack.ss_sp=(char *)fiber->stack+page;
    fiber->context.uc_stack.ss_size=DIAMOND_FIBER_STACK_SIZE;
    fiber->context.uc_link=nullptr;
    makecontext(&fiber->context,diamond_fiber_trampoline,0);
    fiber->state=DIAMOND_FIBER_RUNNABLE;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *fiber, DiamondVm *vm) {
    if(fiber==nullptr||vm==nullptr||fiber->state==DIAMOND_FIBER_COMPLETED||
       fiber->state==DIAMOND_FIBER_FAILED)return DIAMOND_FIBER_INVALID_STATE;
    fiber->vm=vm;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_run(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING||fiber->vm==nullptr||
       (fiber->chunk==nullptr&&fiber->entry_closure==nullptr))
        return DIAMOND_FIBER_INVALID_STATE;
    DiamondVm *vm=fiber->vm;
    void *saved_frames=vm->frames;
    DiamondFiber *saved_running=vm->running_fiber;
    fiber->resumer_frames=saved_frames;
    fiber->resumer_fiber=saved_running;
    vm->frames=fiber->native_frames;
    vm->running_fiber=fiber;
    diamond_fiber_entering=fiber;
    ucontext_t caller_context;
    fiber->resume_target=&caller_context;
    swapcontext(&caller_context,&fiber->context);
    fiber->native_frames=vm->frames;
    vm->frames=saved_frames;
    vm->running_fiber=saved_running;
    fiber->resumer_frames=nullptr;
    fiber->resumer_fiber=nullptr;
    fiber->state=fiber->status==DIAMOND_VM_OK?DIAMOND_FIBER_COMPLETED:
        (fiber->status==DIAMOND_VM_YIELDED?DIAMOND_FIBER_SUSPENDED:
         DIAMOND_FIBER_FAILED);
    return DIAMOND_FIBER_OK;
}

DiamondValue diamond_fiber_result(const DiamondFiber *fiber) {
    return fiber == nullptr ? DIAMOND_NIL : fiber->result;
}

DiamondVmStatus diamond_fiber_status(const DiamondFiber *fiber) {
    return fiber == nullptr ? DIAMOND_VM_INVALID_BYTECODE : fiber->status;
}

bool diamond_fiber_resumable(const DiamondFiber *fiber) {
    return fiber!=nullptr && (fiber->state==DIAMOND_FIBER_RUNNABLE||
        fiber->state==DIAMOND_FIBER_SUSPENDED);
}

const char *diamond_fiber_state_name(DiamondFiberState state) {
    switch(state) {
        case DIAMOND_FIBER_NEW:return "new";
        case DIAMOND_FIBER_RUNNABLE:return "runnable";
        case DIAMOND_FIBER_RUNNING:return "running";
        case DIAMOND_FIBER_SUSPENDED:return "suspended";
        case DIAMOND_FIBER_COMPLETED:return "completed";
        case DIAMOND_FIBER_FAILED:return "failed";
    }
    return "unknown";
}

DiamondFiberStatus diamond_fiber_make_runnable(DiamondFiber *fiber) {
    if(fiber==nullptr||(fiber->state!=DIAMOND_FIBER_NEW&&
       fiber->state!=DIAMOND_FIBER_SUSPENDED))return DIAMOND_FIBER_INVALID_STATE;
    fiber->state=DIAMOND_FIBER_RUNNABLE;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_begin(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNABLE)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->state=DIAMOND_FIBER_RUNNING;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_resume(DiamondFiber *fiber, DiamondValue value) {
    if(fiber==nullptr||(fiber->state!=DIAMOND_FIBER_RUNNABLE&&
       fiber->state!=DIAMOND_FIBER_SUSPENDED))return DIAMOND_FIBER_INVALID_STATE;
    fiber->resume_value=value;
    fiber->state=DIAMOND_FIBER_RUNNING;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_suspend(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->state=DIAMOND_FIBER_SUSPENDED;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_yield(DiamondFiber *fiber) {
    return diamond_fiber_suspend(fiber);
}

DiamondFiberStatus diamond_fiber_complete(DiamondFiber *fiber, DiamondValue result) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->result=result;fiber->status=DIAMOND_VM_OK;
    fiber->state=DIAMOND_FIBER_COMPLETED;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_fail(DiamondFiber *fiber, DiamondVmStatus status) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->status=status;fiber->state=DIAMOND_FIBER_FAILED;
    return DIAMOND_FIBER_OK;
}

void diamond_fiber_queue_init(DiamondFiberQueue *queue) {
    *queue=(DiamondFiberQueue){};
}

void diamond_fiber_queue_free(DiamondFiberQueue *queue) {
    free(queue->items);*queue=(DiamondFiberQueue){};
}

bool diamond_fiber_queue_push(DiamondFiberQueue *queue, DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNABLE)return false;
    if(queue->head>0&&queue->count==queue->capacity) {
        const size_t pending=queue->count-queue->head;
        memmove(queue->items,queue->items+queue->head,pending*sizeof *queue->items);
        queue->head=0;queue->count=pending;
    }
    if(queue->count==queue->capacity) {
        const size_t capacity=queue->capacity==0?8:queue->capacity*2;
        DiamondFiber **items=realloc(queue->items,capacity*sizeof *items);
        if(items==nullptr)return false;
        queue->items=items;queue->capacity=capacity;
    }
    queue->items[queue->count++]=fiber;return true;
}

DiamondFiber *diamond_fiber_queue_pop(DiamondFiberQueue *queue) {
    if(queue->head==queue->count)return nullptr;
    DiamondFiber *fiber=queue->items[queue->head++];
    if(queue->head==queue->count)queue->head=queue->count=0;
    if(fiber->state==DIAMOND_FIBER_RUNNABLE)fiber->state=DIAMOND_FIBER_RUNNING;
    return fiber;
}

size_t diamond_fiber_queue_count(const DiamondFiberQueue *queue) {
    return queue==nullptr?0:queue->count-queue->head;
}

DiamondFiber *diamond_fiber_queue_at(const DiamondFiberQueue *queue, size_t index) {
    if(queue==nullptr||index>=diamond_fiber_queue_count(queue))return nullptr;
    return queue->items[queue->head+index];
}

DiamondFiber *diamond_fiber_scheduler_step(DiamondFiberQueue *queue) {
    return diamond_fiber_queue_pop(queue);
}

bool diamond_fiber_scheduler_requeue(DiamondFiberQueue *queue, DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_SUSPENDED)return false;
    fiber->state=DIAMOND_FIBER_RUNNABLE;
    if(!diamond_fiber_queue_push(queue,fiber)) {
        fiber->state=DIAMOND_FIBER_SUSPENDED;return false;
    }
    return true;
}

DiamondFiberStatus diamond_fiber_scheduler_run_once(DiamondFiberQueue *queue) {
    DiamondFiber *fiber=diamond_fiber_scheduler_step(queue);
    if(fiber==nullptr)return DIAMOND_FIBER_INVALID_STATE;
    if(diamond_fiber_run(fiber)!=DIAMOND_FIBER_OK)return DIAMOND_FIBER_INVALID_STATE;
    if(fiber->state==DIAMOND_FIBER_SUSPENDED&&
       !diamond_fiber_scheduler_requeue(queue,fiber))return DIAMOND_FIBER_INVALID_STATE;
    return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_scheduler_run_all(DiamondFiberQueue *queue) {
    if(queue==nullptr)return DIAMOND_FIBER_INVALID_STATE;
    while(diamond_fiber_queue_count(queue)>0) {
        if(diamond_fiber_scheduler_run_once(queue)!=DIAMOND_FIBER_OK)
            return DIAMOND_FIBER_INVALID_STATE;
    }
    return DIAMOND_FIBER_OK;
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

static DiamondSymbol *allocate_symbol(DiamondVm *vm, const char *chars,
                                      size_t length) {
    if (vm->stress_gc || vm->bytes_allocated >= vm->next_gc) {
        diamond_vm_collect(vm);
    }
    DiamondSymbol *symbol = malloc(sizeof(DiamondSymbol) + length + 1);
    if (symbol == nullptr) return nullptr;
    symbol->object = (DiamondObject){
        .next = vm->objects,
        .kind = DIAMOND_OBJECT_SYMBOL,
    };
    symbol->length = length;
    memcpy(symbol->chars, chars, length);
    symbol->chars[length] = '\0';
    vm->objects = &symbol->object;
    vm->bytes_allocated += sizeof(DiamondSymbol) + length + 1;
    return symbol;
}

static DiamondInstance *allocate_instance(DiamondVm *vm,const DiamondClass *class) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    const size_t size=sizeof(DiamondInstance)+class->field_count*sizeof(DiamondValue);
    DiamondInstance *instance=malloc(size); if(instance==nullptr)return nullptr;
    instance->object=(DiamondObject){.next=vm->objects,.kind=DIAMOND_OBJECT_INSTANCE};
    instance->class=class; instance->shape=&class->shapes[0];
    instance->field_count=class->field_count;
    for(size_t i=0;i<instance->field_count;i++) instance->fields[i]=DIAMOND_NIL;
    vm->objects=&instance->object; vm->bytes_allocated+=size; return instance;
}

static DiamondArray *allocate_array(DiamondVm *vm,const DiamondValue *values,
                                    size_t count) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    const size_t capacity=count;
    const size_t size=sizeof(DiamondArray)+capacity*sizeof(DiamondValue);
    DiamondArray *array=malloc(sizeof(DiamondArray)); if(array==nullptr)return nullptr;
    array->values=capacity==0?nullptr:malloc(capacity*sizeof(DiamondValue));
    if(capacity>0&&array->values==nullptr){free(array);return nullptr;}
    array->object=(DiamondObject){.next=vm->objects,.kind=DIAMOND_OBJECT_ARRAY};
    array->count=count;array->capacity=capacity;array->constraint_count=0;
    for(size_t i=0;i<count;i++) array->values[i]=values[i];
    vm->objects=&array->object;vm->bytes_allocated+=size;return array;
}

static DiamondHash *allocate_hash(DiamondVm *vm) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    DiamondHash *hash=malloc(sizeof(DiamondHash)); if(hash==nullptr)return nullptr;
    *hash=(DiamondHash){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_HASH}};
    vm->objects=&hash->object;vm->bytes_allocated+=sizeof(DiamondHash);return hash;
}

static DiamondClosure *allocate_closure(DiamondVm *vm,uint8_t function_index,
                                        const DiamondValue *captures,size_t count) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondClosure *closure=malloc(sizeof(DiamondClosure));if(closure==nullptr)return nullptr;
    *closure=(DiamondClosure){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_CLOSURE},
      .function_index=function_index,.capture_count=(uint8_t)count};
    for(size_t i=0;i<count;i++)closure->captures[i]=captures[i];
    vm->objects=&closure->object;vm->bytes_allocated+=sizeof(DiamondClosure);return closure;
}

static DiamondCell *allocate_cell(DiamondVm *vm,DiamondValue value) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondCell *cell=malloc(sizeof(DiamondCell));if(cell==nullptr)return nullptr;
    *cell=(DiamondCell){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_CELL},.value=value};
    vm->objects=&cell->object;vm->bytes_allocated+=sizeof(DiamondCell);return cell;
}

static DiamondFiberHandle *allocate_fiber_handle(DiamondVm *vm,DiamondFiber *fiber) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondFiberHandle *handle=malloc(sizeof(DiamondFiberHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondFiberHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_FIBER},.fiber=fiber};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondFiberHandle);return handle;
}

static DiamondFileHandle *allocate_file_handle(DiamondVm *vm,FILE *stream) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondFileHandle *handle=malloc(sizeof(DiamondFileHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondFileHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_FILE},.stream=stream};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondFileHandle);return handle;
}

static DiamondListenerHandle *allocate_listener_handle(DiamondVm *vm,int fd) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondListenerHandle *handle=malloc(sizeof(DiamondListenerHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondListenerHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_LISTENER},.fd=fd};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondListenerHandle);return handle;
}

static DiamondRegexp *allocate_regexp_handle(DiamondVm *vm,reginold_regex *compiled) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondRegexp *regexp=malloc(sizeof(DiamondRegexp));
    if(regexp==nullptr)return nullptr;
    *regexp=(DiamondRegexp){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_REGEXP},
        .handle=compiled};
    vm->objects=&regexp->object;vm->bytes_allocated+=sizeof(DiamondRegexp);return regexp;
}

/* Unlike every other allocate_* helper here, the payload
 * (sizeof(DiamondProgram), tens of MB -- see docs/roadmap.md) dwarfs the
 * handle itself, so bytes_allocated counts it too (mirroring
 * allocate_array's own capacity-inclusive accounting), keeping GC
 * pressure honest about the real memory this handle commits. */
static DiamondProgramBuilder *allocate_program_builder(DiamondVm *vm) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondProgram *built=malloc(sizeof *built);
    if(built==nullptr)return nullptr;
    diamond_program_init(built);
    DiamondProgramBuilder *handle=malloc(sizeof(DiamondProgramBuilder));
    if(handle==nullptr){free(built);return nullptr;}
    *handle=(DiamondProgramBuilder){
        .object={.next=vm->objects,.kind=DIAMOND_OBJECT_PROGRAM_BUILDER},
        .program=built};
    vm->objects=&handle->object;
    vm->bytes_allocated+=sizeof(DiamondProgramBuilder)+sizeof(DiamondProgram);
    return handle;
}

/* function_index==-1 targets the program's entry function; 0..function_count-1
 * targets program->functions[index]. Returns nullptr on any other value. */
static DiamondFunction *program_builder_target(DiamondProgram *program,
                                                int64_t function_index) {
    if(function_index==-1) return &program->entry;
    if(function_index<0||(uint64_t)function_index>=program->function_count)
        return nullptr;
    return &program->functions[function_index];
}

static DiamondClass *program_builder_class(DiamondProgram *program,
                                           int64_t class_index) {
    if(class_index<0||(uint64_t)class_index>=program->class_count)
        return nullptr;
    return &program->classes[class_index];
}

/* Recomputes shapes[0..field_count] for a class -- mirrors the loop
 * diamond_compile itself runs once, over every class, right after
 * compilation finishes (src/compiler.c). ProgramBuilder#declare_field
 * needs the same recomputation done incrementally, since a
 * ProgramBuilder-built program never goes through diamond_compile at
 * all. See docs/roadmap.md's self-hosting Phase 3 entry. */
static void program_builder_recompute_shapes(DiamondClass *class) {
    for(size_t field_count=0;field_count<=class->field_count;field_count++)
        class->shapes[field_count]=(DiamondShape){
            .class=class,.field_count=(uint8_t)field_count};
}

/* DIAMOND_OP_REGEXP_NEW's real body, factored out of run_chunk's own
 * switch statement deliberately, not just for readability: every local
 * variable declared anywhere in that switch contributes to run_chunk's
 * one stack frame regardless of which case actually runs (a -O0 build,
 * which this project's debug/sanitize builds both are, does not reuse
 * stack slots across sibling blocks), and DIAMOND_MAX_CALL_DEPTH is
 * calibrated against that frame's worst-case size to stay safe under
 * AddressSanitizer's redzone-inflated frames (see docs/design.md). A
 * reginold_error alone is ~112 bytes (its message buffer is
 * REGINOLD_ERROR_MSG_MAX=90); adding it and reginold_match's fields
 * directly into run_chunk's frame regressed depth(5000)-style recursion
 * into a genuine ASan stack-overflow crash before the depth counter ever
 * tripped -- caught by make test-sanitize, not by hand-testing, since the
 * regex feature itself worked perfectly right up until deep recursion
 * was exercised. Splitting this into its own function moves those locals
 * into a separate, transient frame that only exists while regex code is
 * actually running, not on every recursive run_chunk level. */
static DiamondVmStatus regexp_new_helper(DiamondVm *vm, const DiamondString *pattern,
        int64_t options, DiamondValue *result) {
    reginold_regex *compiled=nullptr;
    reginold_error compile_error={0};
    const reginold_status compile_status=reginold_compile(pattern->chars,
        pattern->length,(unsigned int)options,&compiled,&compile_error);
    if(compile_status!=REGINOLD_OK) {
        snprintf(vm->error,sizeof vm->error,"%.*s",
            (int)compile_error.message_len,compile_error.message);
        return DIAMOND_VM_REGEXP_ERROR;
    }
    DiamondRegexp *regexp=allocate_regexp_handle(vm,compiled);
    if(regexp==nullptr) {
        reginold_regex_free(compiled);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(regexp);
    return DIAMOND_VM_OK;
}

/* Regexp#match/#match? real body -- same stack-frame-isolation reasoning
 * as regexp_new_helper above (reginold_match's own fields would otherwise
 * land directly in run_chunk's frame too). */
static DiamondVmStatus regexp_match_helper(DiamondVm *vm, const DiamondRegexp *regexp,
        const DiamondString *subject, bool test_only, DiamondValue *result) {
    if(test_only) {
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,0,nullptr);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        *result=DIAMOND_BOOL(search_status==REGINOLD_OK);
        return DIAMOND_VM_OK;
    }
    reginold_match match_result={0};
    const reginold_status search_status=reginold_search(regexp->handle,
        subject->chars,subject->length,0,&match_result);
    if(search_status==REGINOLD_ERROR) {
        snprintf(vm->error,sizeof vm->error,"regexp match failed");
        return DIAMOND_VM_REGEXP_ERROR;
    }
    if(search_status==REGINOLD_MISMATCH) {
        *result=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    const size_t group_count=1+match_result.capture_count;
    DiamondValue *groups=malloc(group_count*sizeof(DiamondValue));
    if(groups==nullptr) {
        reginold_match_free(&match_result);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    bool build_ok=true;
    for(size_t index=0;index<group_count&&build_ok;index++) {
        const reginold_span span=index==0?match_result.overall:
            match_result.captures[index-1];
        if(span.beg<0||span.end<0) {
            groups[index]=DIAMOND_NIL;
            continue;
        }
        DiamondString *group_string=allocate_string(vm,
            subject->chars+span.beg,(size_t)(span.end-span.beg));
        if(group_string==nullptr) {build_ok=false;break;}
        groups[index]=DIAMOND_OBJECT(group_string);
    }
    reginold_match_free(&match_result);
    if(!build_ok) {free(groups);return DIAMOND_VM_OUT_OF_MEMORY;}
    DiamondArray *result_array=allocate_array(vm,groups,group_count);
    free(groups);
    if(result_array==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(result_array);
    return DIAMOND_VM_OK;
}

/* ProgramBuilder#run's real body, factored out of run_chunk's own opcode
 * switch for the same stack-frame-isolation reason as regexp_new_helper
 * above -- but far more load-bearing here: a bare local DiamondVm is
 * ~50KB (method/field caches, rewritten_sites[DIAMOND_MAX_CODE], opcode
 * counters, namespace constants), not the ~100 bytes regexp_new_helper's
 * own locals needed. At -O0, every local anywhere in run_chunk's switch
 * contributes to its one shared stack frame regardless of which case
 * actually runs, so leaving `DiamondVm run_vm` inline in the INVOKE case
 * body would have added that ~50KB to *every* recursive run_chunk level
 * unconditionally -- confirmed by a real crash: depth(5000) (the existing
 * regression test for the DIAMOND_MAX_CALL_DEPTH guard) segfaulted before
 * that guard could trip, a worse version of the exact bug the Regexp
 * round already found and fixed this way (see docs/roadmap.md). Returns
 * DIAMOND_VM_PROGRAM_ERROR (not the constructed program's own status) for
 * a nonzero exit, and DIAMOND_VM_TYPE_ERROR for a non-scalar result --
 * see docs/roadmap.md's self-hosting Phase 1 entry for why both are
 * deliberate v1 scope cuts rather than gaps. */
static DiamondVmStatus program_builder_run_helper(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *result) {
    const DiamondChunk built_chunk=diamond_program_chunk(built);
    DiamondVm run_vm;diamond_vm_init(&run_vm);
    DiamondValue run_result=DIAMOND_NIL;
    const DiamondVmStatus run_status=diamond_vm_run(&run_vm,&built_chunk,&run_result);
    if(run_status!=DIAMOND_VM_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",
            run_vm.error[0]!='\0'?run_vm.error:diamond_vm_status_name(run_status));
        diamond_vm_free(&run_vm);
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    if(run_result.kind==DIAMOND_VALUE_OBJECT) {
        diamond_vm_free(&run_vm);
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "run only supports a scalar (Int, Float, Bool, or Nil) result");
        return DIAMOND_VM_TYPE_ERROR;
    }
    diamond_vm_free(&run_vm);
    *result=run_result;
    return DIAMOND_VM_OK;
}

/* All six ProgramBuilder instance methods, factored out of run_chunk's own
 * INVOKE case for the same stack-frame-isolation reason as
 * program_builder_run_helper above -- not because any *one* branch here has
 * a large local (they don't), but because at -O0 every local anywhere in
 * run_chunk's switch, across *every* branch, contributes to its one shared
 * stack frame regardless of which branch actually runs. Six method-name
 * flags plus each branch's own few pointers/integers added up to enough
 * that depth(5000) -- the existing regression test for the
 * DIAMOND_MAX_CALL_DEPTH guard -- segfaulted before that guard could trip,
 * even after program_builder_run_helper's extraction alone (confirmed by
 * testing that fix in isolation first). Takes `registers`/`base`/`argc`
 * directly rather than pre-extracted arguments, unlike
 * regexp_new_helper/regexp_match_helper, since six methods with different
 * arities would otherwise need six different call signatures. */
static DiamondVmStatus program_builder_invoke_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, const DiamondStringConstant *method_name,
        DiamondValue *registers, uint8_t base, uint8_t argc, size_t depth,
        DiamondValue *result) {
    DiamondProgram *built=builder->program;
    const bool declare_function_method=
        method_name->length==sizeof("declare_function")-1&&
        memcmp(method_name->chars,"declare_function",
            sizeof("declare_function")-1)==0;
    const bool emit_byte_method=
        method_name->length==sizeof("emit_byte")-1&&
        memcmp(method_name->chars,"emit_byte",sizeof("emit_byte")-1)==0;
    /* Needed for jump backpatching: compiler.c's own patch_jump (see
     * src/compiler.c) directly overwrites function->code[operand] after
     * the fact, once a forward jump's real target is known -- a single-pass
     * emitter can't know a forward target's offset before emitting the
     * jump itself. Phase 3's Diamond-language parser needs the same
     * capability for if/while/loop, so this mirrors patch_jump exactly
     * (overwrite an already-emitted byte, never append). See
     * docs/roadmap.md's self-hosting Phase 3 entry. */
    const bool patch_byte_method=
        method_name->length==sizeof("patch_byte")-1&&
        memcmp(method_name->chars,"patch_byte",sizeof("patch_byte")-1)==0;
    const bool add_constant_method=
        method_name->length==sizeof("add_constant")-1&&
        memcmp(method_name->chars,"add_constant",
            sizeof("add_constant")-1)==0;
    const bool add_string_method=
        method_name->length==sizeof("add_string")-1&&
        memcmp(method_name->chars,"add_string",sizeof("add_string")-1)==0;
    const bool set_register_count_method=
        method_name->length==sizeof("set_register_count")-1&&
        memcmp(method_name->chars,"set_register_count",
            sizeof("set_register_count")-1)==0;
    /* Phase 3 sub-phase 3 (classes): declare_class/declare_field/
     * declare_method mirror compile_class/field_index/compile_definition's
     * own class-registration side effects in compiler.c -- fields not
     * needed by any sub-phase before this one (Phase 1's own design note
     * flagged them as deferred until class-compiling logic actually
     * needed them). See docs/roadmap.md. */
    const bool declare_class_method=
        method_name->length==sizeof("declare_class")-1&&
        memcmp(method_name->chars,"declare_class",sizeof("declare_class")-1)==0;
    const bool declare_field_method=
        method_name->length==sizeof("declare_field")-1&&
        memcmp(method_name->chars,"declare_field",sizeof("declare_field")-1)==0;
    const bool declare_method_method=
        method_name->length==sizeof("declare_method")-1&&
        memcmp(method_name->chars,"declare_method",sizeof("declare_method")-1)==0;
    const bool run_method=method_name->length==sizeof("run")-1&&
        memcmp(method_name->chars,"run",sizeof("run")-1)==0;
    if(!declare_function_method&&!emit_byte_method&&!patch_byte_method&&
       !add_constant_method&&!add_string_method&&
       !set_register_count_method&&!declare_class_method&&
       !declare_field_method&&!declare_method_method&&!run_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"ProgramBuilder");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(declare_function_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_function arguments must be (String, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *fname=
            (const DiamondString *)registers[base].as.object;
        const int64_t arity_value=registers[(size_t)base+1].as.integer;
        const int64_t required_value=registers[(size_t)base+2].as.integer;
        if(fname->length==0||fname->length>=DIAMOND_MAX_FUNCTION_NAME||
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_function has an invalid name or arity");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(built->function_count==DIAMOND_MAX_FUNCTIONS) {
            snprintf(vm->error,sizeof vm->error,"program has too many functions");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *function=&built->functions[built->function_count];
        *function=(DiamondFunction){};
        memcpy(function->name,fname->chars,fname->length);
        function->name[fname->length]='\0';
        function->owner_class=UINT8_MAX;
        function->arity=(uint8_t)arity_value;
        function->required_arity=(uint8_t)required_value;
        function->return_type_set=UINT8_MAX;
        for(size_t index=0;index<16;index++)
            function->parameter_type_sets[index]=UINT8_MAX;
        const int64_t new_index=(int64_t)built->function_count;
        built->function_count++;
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(emit_byte_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#emit_byte arguments must be (Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t byte_value=registers[(size_t)base+1].as.integer;
        if(target==nullptr||byte_value<0||byte_value>UINT8_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "emit_byte has an invalid function index or byte value");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(target->code_count==DIAMOND_MAX_CODE) {
            snprintf(vm->error,sizeof vm->error,
                "function produces too much bytecode");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->code[target->code_count]=(uint8_t)byte_value;
        target->lines[target->code_count]=0;
        target->columns[target->code_count]=0;
        target->code_count++;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(patch_byte_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#patch_byte arguments must be (Int, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t offset_value=registers[(size_t)base+1].as.integer;
        const int64_t byte_value=registers[(size_t)base+2].as.integer;
        if(target==nullptr||offset_value<0||
           (uint64_t)offset_value>=target->code_count||
           byte_value<0||byte_value>UINT8_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "patch_byte has an invalid function index, offset, or byte value");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->code[offset_value]=(uint8_t)byte_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(add_constant_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_constant's function index must be an Int");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondValue value=registers[(size_t)base+1];
        if(value.kind==DIAMOND_VALUE_OBJECT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "add_constant only accepts Int, Float, Bool, or Nil");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        if(target==nullptr) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_constant has an invalid function index");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(target->constant_count==DIAMOND_MAX_CONSTANTS) {
            snprintf(vm->error,sizeof vm->error,
                "function has too many constants");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)target->constant_count;
        target->constants[target->constant_count++]=value;
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(add_string_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_string arguments must be (Int, String)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const DiamondString *text=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(target==nullptr||text->length>DIAMOND_MAX_STRING_LENGTH) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "add_string has an invalid function index or an oversized string");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(target->string_count==DIAMOND_MAX_STRING_CONSTANTS) {
            snprintf(vm->error,sizeof vm->error,
                "function has too many string constants");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondStringConstant *slot=&target->strings[target->string_count];
        memcpy(slot->chars,text->chars,text->length);
        slot->chars[text->length]='\0';
        slot->length=text->length;
        const int64_t new_index=(int64_t)target->string_count;
        target->string_count++;
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(set_register_count_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#set_register_count arguments must be (Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t count_value=registers[(size_t)base+1].as.integer;
        if(target==nullptr||count_value<0||count_value>UINT16_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_register_count has an invalid function index or count");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->register_count=(uint16_t)count_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_class_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_class arguments must be (String, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *cname=
            (const DiamondString *)registers[base].as.object;
        const int64_t superclass_index=registers[(size_t)base+1].as.integer;
        DiamondClass *parent=nullptr;
        if(superclass_index!=-1) {
            parent=program_builder_class(built,superclass_index);
            if(parent==nullptr) {
                snprintf(vm->error,sizeof vm->error,
                    "ProgramBuilder#declare_class has an invalid superclass index");
                return DIAMOND_VM_TYPE_ERROR;
            }
        }
        if(cname->length==0||cname->length>=DIAMOND_MAX_FUNCTION_NAME) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_class has an invalid name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(built->class_count==DIAMOND_MAX_CLASSES) {
            snprintf(vm->error,sizeof vm->error,"program has too many classes");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)built->class_count;
        DiamondClass *class=&built->classes[built->class_count++];
        *class=(DiamondClass){};
        memcpy(class->name,cname->chars,cname->length);
        class->name[cname->length]='\0';
        class->superclass=parent==nullptr?UINT8_MAX:(uint8_t)superclass_index;
        if(parent!=nullptr) {
            class->field_count=parent->field_count;
            memcpy(class->fields,parent->fields,
                parent->field_count*DIAMOND_MAX_FUNCTION_NAME);
        }
        program_builder_recompute_shapes(class);
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(declare_field_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_field arguments must be (Int, String)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondClass *class=
            program_builder_class(built,registers[base].as.integer);
        const DiamondString *fname=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(class==nullptr||fname->length==0||
           fname->length>=DIAMOND_MAX_FUNCTION_NAME) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_field has an invalid class index or field name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<class->field_count;index++)
            if(strlen(class->fields[index])==fname->length&&
               memcmp(class->fields[index],fname->chars,fname->length)==0) {
                *result=DIAMOND_INT((int64_t)index);return DIAMOND_VM_OK;
            }
        if(class->field_count==DIAMOND_MAX_FIELDS) {
            snprintf(vm->error,sizeof vm->error,"class has too many fields");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)class->field_count;
        memcpy(class->fields[class->field_count],fname->chars,fname->length);
        class->fields[class->field_count][fname->length]='\0';
        class->field_count++;
        program_builder_recompute_shapes(class);
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(declare_method_method) {
        if(argc!=6)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+5].kind!=DIAMOND_VALUE_BOOL) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_method arguments must be "
                "(Int, String, Int, Int, Int, Bool)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondClass *class=
            program_builder_class(built,registers[base].as.integer);
        const DiamondString *mname=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const int64_t target_function=registers[(size_t)base+2].as.integer;
        const int64_t arity_value=registers[(size_t)base+3].as.integer;
        const int64_t required_value=registers[(size_t)base+4].as.integer;
        if(class==nullptr||mname->length==0||
           mname->length>=DIAMOND_MAX_FUNCTION_NAME||
           target_function<0||(uint64_t)target_function>=built->function_count||
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<class->method_count;index++)
            if(!class->methods[index].included&&
               strlen(class->methods[index].name)==mname->length&&
               memcmp(class->methods[index].name,mname->chars,mname->length)==0) {
                snprintf(vm->error,sizeof vm->error,"method is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(class->method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,"class has too many methods");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod *method=&class->methods[class->method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,mname->chars,mname->length);
        method->name[mname->length]='\0';
        method->function_index=(uint8_t)target_function;
        method->arity=(uint8_t)arity_value;
        method->required_arity=(uint8_t)required_value;
        method->is_private=registers[(size_t)base+5].as.boolean;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    /* run_method: the only remaining possibility once the combined
     * "no method matched" check above passed. */
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    /* Refuses to nest past a conservative depth rather than the full
     * DIAMOND_MAX_CALL_DEPTH: program_builder_run_helper's diamond_vm_run
     * starts a *fresh* run_chunk recursion (depth 0) on top of this
     * call's own C stack frame, which is itself already `depth` levels of
     * run_chunk deep -- so real C-stack usage is the *sum* of outer and
     * inner depth, not bounded by either guard alone. Capping outer depth
     * at 10 keeps that sum within the same call-depth budget already
     * verified safe under ASan (see the DIAMOND_MAX_CALL_DEPTH regression
     * entry in docs/roadmap.md) even if the inner program recurses to its
     * own full limit. */
    if(depth>=10) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#run nested too deeply");
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    return program_builder_run_helper(vm,built,result);
}

static bool value_is_bignum(DiamondValue value) {
    return value.kind==DIAMOND_VALUE_OBJECT&&
        value.as.object->kind==DIAMOND_OBJECT_BIGNUM;
}

/* True for both representations an Int can have -- a plain inline
 * int64_t (kind==DIAMOND_VALUE_INT) or a promoted DiamondBignum. Used
 * everywhere "is this conceptually an Int" matters (type checks,
 * cross-type dispatch); arithmetic fast paths that specifically need
 * "is this a small int64_t I can compute on directly" still check
 * kind==DIAMOND_VALUE_INT alone, unchanged. */
static bool is_int_value(DiamondValue value) {
    return value.kind==DIAMOND_VALUE_INT||value_is_bignum(value);
}

static bool numeric_as_double(DiamondValue value, double *out) {
    if(value.kind==DIAMOND_VALUE_INT) {*out=(double)value.as.integer;return true;}
    if(value.kind==DIAMOND_VALUE_FLOAT) {*out=value.as.real;return true;}
    if(value_is_bignum(value)) {
        *out=diamond_bignum_to_double((const DiamondBignum *)value.as.object);
        return true;
    }
    return false;
}

static bool values_equal(DiamondValue left, DiamondValue right) {
    /* Bignum-aware equality ahead of everything else: a Float, however
     * large, is deliberately never treated as equal to a bignum (the
     * existing Int/Float cross-equality special-case below only
     * handles floats within int64 range, and that's left as-is rather
     * than extended -- see the bignum design notes). Two bignums, or a
     * bignum and a small Int, compare by value either way. */
    if(value_is_bignum(left)||value_is_bignum(right)) {
        if(left.kind==DIAMOND_VALUE_FLOAT||right.kind==DIAMOND_VALUE_FLOAT)return false;
        if(!is_int_value(left)||!is_int_value(right))return false;
        DiamondIntView left_view, right_view;
        diamond_int_view(left,&left_view);
        diamond_int_view(right,&right_view);
        return diamond_bignum_compare(left_view,right_view)==0;
    }
    /* Cross-type numeric equality (3 == 3.0) ahead of the kind guard,
     * per the auto-promotion design: Int widens to double for the
     * comparison. */
    if(left.kind==DIAMOND_VALUE_INT&&right.kind==DIAMOND_VALUE_FLOAT)
        return (double)left.as.integer==right.as.real;
    if(left.kind==DIAMOND_VALUE_FLOAT&&right.kind==DIAMOND_VALUE_INT)
        return left.as.real==(double)right.as.integer;
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
        case DIAMOND_VALUE_FLOAT:
            return left.as.real == right.as.real;
        case DIAMOND_VALUE_OBJECT: {
            if (left.as.object->kind != right.as.object->kind) return false;
            if(left.as.object->kind==DIAMOND_OBJECT_INSTANCE ||
               left.as.object->kind==DIAMOND_OBJECT_ARRAY ||
               left.as.object->kind==DIAMOND_OBJECT_HASH)
                return left.as.object==right.as.object;
            if(left.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *a=(const DiamondSymbol *)left.as.object;
                const DiamondSymbol *b=(const DiamondSymbol *)right.as.object;
                return a->length==b->length&&
                    memcmp(a->chars,b->chars,a->length)==0;
            }
            const DiamondString *a = (const DiamondString *)left.as.object;
            const DiamondString *b = (const DiamondString *)right.as.object;
            return a->length == b->length &&
                   memcmp(a->chars, b->chars, a->length) == 0;
        }
    }
    return false;
}

/* MurmurHash3's fmix64 finalizer (public domain) -- gives scalar/pointer
 * keys good bit distribution across all bits, not just the low bits a
 * power-of-two bucket mask would otherwise expose (a real risk for the
 * common case of small sequential Int keys). */
static uint64_t hash_mix64(uint64_t value) {
    value^=value>>33;value*=0xff51afd7ed558ccdULL;
    value^=value>>33;value*=0xc4ceb9fe1a85ec53ULL;
    value^=value>>33;
    return value;
}

/* FNV-1a over String content -- keys with equal bytes (values_equal's
 * String case, memcmp) must hash equal regardless of which String
 * object holds them. */
static uint64_t hash_bytes(const char *data,size_t length) {
    uint64_t hash=0xcbf29ce484222325ULL;
    for(size_t index=0;index<length;index++) {
        hash^=(unsigned char)data[index];
        hash*=0x100000001b3ULL;
    }
    return hash;
}

/* Must stay consistent with values_equal's exact equality semantics:
 * Int/Bool/Nil by value, String/Symbol by content, Array/Hash/Instance by
 * pointer identity. */
static uint64_t hash_value(DiamondValue value) {
    switch(value.kind) {
        case DIAMOND_VALUE_NIL:return hash_mix64(0);
        case DIAMOND_VALUE_BOOL:return hash_mix64(value.as.boolean?1:2);
        case DIAMOND_VALUE_INT:return hash_mix64((uint64_t)value.as.integer);
        case DIAMOND_VALUE_FLOAT: {
            const double real=value.as.real;
            /* A Float that's exactly equal to some Int64 (e.g. 3.0)
             * must hash the same way that Int64 does, since
             * values_equal treats 3 == 3.0 as true. Bounds-checked
             * before the int64 cast to avoid UB on an out-of-range or
             * non-finite double. */
            if(!isnan(real)&&!isinf(real)&&
               real>=-9223372036854775808.0&&real<9223372036854775808.0&&
               real==(double)(int64_t)real)
                return hash_mix64((uint64_t)(int64_t)real);
            uint64_t bits;
            memcpy(&bits,&real,sizeof bits);
            return hash_mix64(bits);
        }
        case DIAMOND_VALUE_OBJECT: {
            const DiamondObject *object=value.as.object;
            if(object->kind==DIAMOND_OBJECT_INSTANCE||
               object->kind==DIAMOND_OBJECT_ARRAY||
               object->kind==DIAMOND_OBJECT_HASH)
                return hash_mix64((uint64_t)(uintptr_t)object);
            /* No consistency requirement with the small-int hash path
             * above: the canonicalization invariant guarantees a
             * bignum and a small Int can never represent the same
             * value, so there's nothing for their hashes to need to
             * agree with. */
            if(object->kind==DIAMOND_OBJECT_BIGNUM)
                return diamond_bignum_hash((const DiamondBignum *)object);
            if(object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *symbol=(const DiamondSymbol *)object;
                return hash_bytes(symbol->chars,symbol->length);
            }
            const DiamondString *string=(const DiamondString *)object;
            return hash_bytes(string->chars,string->length);
        }
    }
    return 0;
}

/* Rebuilds only the bucket index table from entries[]'s already-cached
 * per-entry hash -- entries[] itself is never reordered, which is what
 * keeps insertion order (and "update doesn't move position") intact
 * across any number of rehashes. No tombstones: nothing ever deletes a
 * Hash entry, so an empty slot (SIZE_MAX) always safely ends a probe. */
static bool hash_rehash(DiamondVm *vm,DiamondHash *hash,size_t new_capacity) {
    size_t *buckets=malloc(new_capacity*sizeof(size_t));
    if(buckets==nullptr)return false;
    for(size_t index=0;index<new_capacity;index++)buckets[index]=SIZE_MAX;
    for(size_t index=0;index<hash->count;index++) {
        size_t slot=(size_t)(hash->entries[index].hash&(new_capacity-1));
        while(buckets[slot]!=SIZE_MAX)slot=(slot+1)&(new_capacity-1);
        buckets[slot]=index;
    }
    free(hash->buckets);
    if(hash->bucket_capacity>0)
        vm->bytes_allocated-=hash->bucket_capacity*sizeof(size_t);
    hash->buckets=buckets;hash->bucket_capacity=new_capacity;
    vm->bytes_allocated+=new_capacity*sizeof(size_t);
    return true;
}

static ptrdiff_t hash_find(const DiamondHash *hash,DiamondValue key) {
    if(hash->bucket_capacity==0)return -1;
    const uint64_t key_hash=hash_value(key);
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    for(size_t probe=0;probe<hash->bucket_capacity;probe++) {
        const size_t index=hash->buckets[slot];
        if(index==SIZE_MAX)return -1;
        if(hash->entries[index].hash==key_hash&&
           values_equal(hash->entries[index].key,key))
            return (ptrdiff_t)index;
        slot=(slot+1)&(hash->bucket_capacity-1);
    }
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
    /* Load factor >= 0.75, checked with integer arithmetic. */
    if(hash->bucket_capacity==0||
       (hash->count+1)*4>=hash->bucket_capacity*3) {
        const size_t new_capacity=
            hash->bucket_capacity==0?8:hash->bucket_capacity*2;
        if(!hash_rehash(vm,hash,new_capacity))return false;
    }
    const uint64_t key_hash=hash_value(key);
    const size_t new_index=hash->count;
    hash->entries[hash->count++]=
        (DiamondHashEntry){.key=key,.value=value,.hash=key_hash};
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    while(hash->buckets[slot]!=SIZE_MAX)slot=(slot+1)&(hash->bucket_capacity-1);
    hash->buckets[slot]=new_index;
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
        for(size_t index=current->method_count;index>0;index--) {
            const DiamondMethod *method=&current->methods[index-1];
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

static const DiamondMethod *lookup_method_cached(
    DiamondVm *vm, const DiamondChunk *chunk, const uint8_t *site,
    const DiamondClass *class, const char *name, size_t length) {
    const size_t slot=((size_t)(uintptr_t)site>>2)%DIAMOND_INLINE_CACHE_COUNT;
    DiamondMethodCache *cache=&vm->method_caches[slot];
    if(cache->site!=site) {
        *cache=(DiamondMethodCache){.site=site};
    } else {
        if (cache->entry_count == 1 &&
            cache->hits >= vm->monomorphic_threshold &&
            cache->entries[0].receiver_class == class) {
            vm->inline_cache_hits++;
            vm->monomorphic_dispatches++;
            cache->hits++;
            return cache->entries[0].method;
        }
        for(size_t index=0;index<cache->entry_count;index++) {
            vm->method_cache_probes++;
            if(cache->entries[index].receiver_class!=class)continue;
            vm->inline_cache_hits++;
            cache->hits++;
            return cache->entries[index].method;
        }
    }
    vm->inline_cache_misses++;
    cache->misses++;
    const DiamondMethod *method=lookup_method(chunk,class,name,length);
    size_t entry=cache->entry_count;
    if(entry<DIAMOND_INLINE_CACHE_WIDTH) {
        cache->entry_count++;
    } else {
        entry=cache->next_replace;
        cache->next_replace=(uint8_t)((cache->next_replace+1)%DIAMOND_INLINE_CACHE_WIDTH);
    }
    cache->entries[entry]=(DiamondMethodCacheEntry){
        .receiver_class=class,.method=method};
    return method;
}

/* Operator overloading (see docs/syntax.md): dispatches to a user-class
 * method named "+"/"=="/"negate"/etc. when a builtin arithmetic/comparison
 * opcode's operand doesn't otherwise know how to combine with the other
 * one. `argument` is the other operand for a binary operator; nullptr for
 * a unary one (negate), meaning the receiver is the method's only
 * argument. `*found` tells the caller whether a method was located at
 * all -- when false, the caller falls through to its own existing
 * TypeError path unchanged, so this never changes behavior for a class
 * that doesn't define the operator. Uses lookup_method_cached (the same
 * cache INVOKE/INVOKE_MONO use, keyed off `site`) rather than a plain
 * lookup_method, so a hot operator-overload call site gets the same
 * monomorphic-class fast path any other polymorphic call site does, with
 * no new caching mechanism needed. Deliberately does not check
 * method->is_private: `a + b` is operator syntax, not an explicit-receiver
 * method call the way `a.plus(b)` would be -- matches how the to_s
 * dispatch in stringify_value below also ignores privacy. */
static DiamondVmStatus invoke_operator_method(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondInstance *receiver,
        const char *name, size_t name_length, const DiamondValue *argument,
        DiamondValue *result, bool *found) {
    const DiamondMethod *method=lookup_method_cached(vm,chunk,site,
        receiver->class,name,name_length);
    if(method==nullptr) {*found=false;return DIAMOND_VM_OK;}
    *found=true;
    /* method->arity/required_arity are stored receiver-exclusive (see
     * compile_definition's `function->arity-1` when registering a class
     * method), matching how the real INVOKE site checks its own `argc`
     * (also receiver-exclusive) against them -- only the explicit operand
     * counts here, not the receiver. */
    const size_t explicit_argument_count=argument==nullptr?0:1;
    if(explicit_argument_count<method->required_arity||
       explicit_argument_count>method->arity)
        return DIAMOND_VM_ARITY_ERROR;
    const size_t argument_count=argument==nullptr?1:2;
    DiamondValue args[2]={DIAMOND_OBJECT((DiamondObject *)receiver)};
    if(argument!=nullptr)args[1]=*argument;
    const DiamondFunction *fn=&chunk->functions[method->function_index];
    const DiamondChunk child={.name=fn->name,.code=fn->code,
      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
      .constants=fn->constants,.constant_count=fn->constant_count,
      .strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=chunk->functions,.function_count=chunk->function_count,
      .classes=chunk->classes,.class_count=chunk->class_count,
      .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
      .register_count=fn->register_count};
    return run_chunk(&child,vm,args,argument_count,depth+1,nullptr,result);
}

/* Mirrors find_function's two filters (compiler.c) exactly, operating on
 * the runtime DiamondChunk instead of the compile-time DiamondProgram:
 * excludes class/module methods (owner_class!=UINT8_MAX) and nested
 * def's, so a same-named local closure can never shadow a real
 * top-level prelude function. */
static const DiamondFunction *find_top_level_function(
        const DiamondChunk *chunk, const char *name, size_t length) {
    for (size_t index = 0; index < chunk->function_count; index++) {
        const DiamondFunction *candidate = &chunk->functions[index];
        if (candidate->owner_class != UINT8_MAX || candidate->nested) continue;
        if (strlen(candidate->name) == length &&
            memcmp(candidate->name, name, length) == 0) return candidate;
    }
    return nullptr;
}

static void record_rewritten_site(DiamondVm *vm, const uint8_t *site) {
    if (vm->rewritten_site_count >= DIAMOND_MAX_CODE)return;
    for (size_t index=0;index<vm->rewritten_site_count;index++)
        if (vm->rewritten_sites[index]==site)return;
    vm->rewritten_sites[vm->rewritten_site_count++]=site;
}

static DiamondFieldCacheEntry *lookup_field_cached(
    DiamondVm *vm, const uint8_t *site, const DiamondInstance *instance,
    uint8_t field, bool write) {
    const size_t slot=((size_t)(uintptr_t)site>>2)%DIAMOND_INLINE_CACHE_COUNT;
    DiamondFieldCache *cache=&vm->field_caches[slot];
    if(cache->site!=site) {
        *cache=(DiamondFieldCache){.site=site};
    } else {
        for(size_t index=0;index<cache->entry_count;index++) {
            if(cache->entries[index].input_shape!=instance->shape)continue;
            vm->field_cache_hits++;
            return &cache->entries[index];
        }
    }
    vm->field_cache_misses++;
    size_t entry=cache->entry_count;
    if(entry<DIAMOND_INLINE_CACHE_WIDTH) {
        cache->entry_count++;
    } else {
        entry=cache->next_replace;
        cache->next_replace=(uint8_t)((cache->next_replace+1)%DIAMOND_INLINE_CACHE_WIDTH);
    }
    const size_t needed=(size_t)field+1;
    cache->entries[entry]=(DiamondFieldCacheEntry){
        .input_shape=instance->shape,
        .output_shape=write && instance->shape->field_count<needed
            ? &instance->class->shapes[needed] : instance->shape,
        .materialized=(size_t)field<instance->shape->field_count};
    return &cache->entries[entry];
}

static int named_field_index(const DiamondInstance *instance,
                             const DiamondStringConstant *name) {
    for(size_t field=0;field<instance->class->field_count;field++)
        if(strlen(instance->class->fields[field])==name->length&&
           memcmp(instance->class->fields[field],name->chars,name->length)==0)
            return (int)field;
    return -1;
}

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index);

static bool value_matches_type(const DiamondChunk *chunk,DiamondValue value,
                               uint8_t type);

/* Every native method String/Array/Hash actually implement, for
 * structural-interface matching (see diamond_native_method_satisfies
 * below). Kept next to the real DIAMOND_OP_INVOKE dispatch further
 * down in this file so a future native-method addition is naturally
 * visible nearby. return_type is UINT8_MAX when a method's result
 * isn't one fixed scalar type (pop/key_at/value_at return the
 * container's stored element type; index_of returns Int | Nil) --
 * such a method can still satisfy an interface method with no return
 * annotation, just never one that requires a specific return type,
 * matching the same conservative convention already used for
 * user-class methods with an unannotated return. Enumerable-style
 * receiver methods (.each/.select/.count/.any?/.all?/.reduce/.map)
 * are deliberately not listed here -- they forward to ordinary prelude
 * Diamond functions rather than being native primitives, so an
 * interface already matches them the normal way, through a class's
 * own method table. */
typedef struct DiamondNativeMethod {
    uint8_t receiver_type;
    const char *name;
    uint8_t arity;
    uint8_t return_type;
} DiamondNativeMethod;

static const DiamondNativeMethod DIAMOND_NATIVE_METHODS[]={
    {DIAMOND_TYPE_STRING,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_ARRAY,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_HASH,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"repeat",1,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"ord",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"split",1,DIAMOND_TYPE_ARRAY},
    {DIAMOND_TYPE_STRING,"strip",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"reverse",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"downcase",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"upcase",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"to_i",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"to_f",0,DIAMOND_TYPE_FLOAT},
    {DIAMOND_TYPE_STRING,"index_of",1,UINT8_MAX},
    {DIAMOND_TYPE_STRING,"slice",2,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_ARRAY,"push",1,DIAMOND_TYPE_ARRAY},
    {DIAMOND_TYPE_ARRAY,"pop",0,UINT8_MAX},
    {DIAMOND_TYPE_HASH,"key_at",1,UINT8_MAX},
    {DIAMOND_TYPE_HASH,"value_at",1,UINT8_MAX},
};

bool diamond_native_method_satisfies(uint8_t receiver_type,const char *name,
                                     uint8_t arity,uint8_t *return_type) {
    for(size_t index=0;index<sizeof DIAMOND_NATIVE_METHODS/
        sizeof DIAMOND_NATIVE_METHODS[0];index++) {
        const DiamondNativeMethod *method=&DIAMOND_NATIVE_METHODS[index];
        if(method->receiver_type==receiver_type&&
           strcmp(method->name,name)==0&&method->arity==arity) {
            *return_type=method->return_type;
            return true;
        }
    }
    return false;
}

static bool value_matches_bound_node(const DiamondChunk *chunk,DiamondValue value,
                                     const DiamondTypeBinding *binding,
                                     uint8_t node_index) {
    if(node_index>=binding->node_count)return true;
    const DiamondBoundTypeNode *node=&binding->nodes[node_index];
    if(node->count==0)return true;
    for(size_t index=0;index<node->count;index++) {
        const DiamondBoundTypeMember member=node->members[index];
        if(!value_matches_type(chunk,value,member.id))continue;
        if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_node!=UINT8_MAX) {
            const DiamondArray *array=(const DiamondArray *)value.as.object;
            bool matches=true;
            for(size_t item=0;item<array->count&&matches;item++)
                matches=value_matches_bound_node(chunk,array->values[item],binding,
                                                 member.argument_node);
            if(matches)return true;
        } else if(member.id==DIAMOND_TYPE_HASH&&
                  member.argument_node!=UINT8_MAX&&
                  member.second_argument_node!=UINT8_MAX) {
            const DiamondHash *hash=(const DiamondHash *)value.as.object;
            bool matches=true;
            for(size_t item=0;item<hash->count&&matches;item++)
                matches=value_matches_bound_node(chunk,hash->entries[item].key,binding,
                                                 member.argument_node)&&
                    value_matches_bound_node(chunk,hash->entries[item].value,binding,
                                             member.second_argument_node);
            if(matches)return true;
        } else return true;
    }
    return false;
}

static bool value_matches_type(const DiamondChunk *chunk, DiamondValue value,
                               uint8_t type) {
    if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr)return true;
        return value_matches_bound_node(chunk,value,
            &chunk->type_variable_bindings[variable],0);
    }
    if(type==DIAMOND_TYPE_INT) return is_int_value(value);
    if(type==DIAMOND_TYPE_FLOAT) return value.kind==DIAMOND_VALUE_FLOAT;
    if(type==DIAMOND_TYPE_BOOL) return value.kind==DIAMOND_VALUE_BOOL;
    if(type==DIAMOND_TYPE_NIL) return value.kind==DIAMOND_VALUE_NIL;
    if(type==DIAMOND_TYPE_STRING) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_STRING;
    if(type==DIAMOND_TYPE_SYMBOL) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_SYMBOL;
    if(type==DIAMOND_TYPE_ARRAY) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_ARRAY;
    if(type==DIAMOND_TYPE_HASH) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_HASH;
    if(type==DIAMOND_TYPE_CALLABLE) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_CLOSURE;
    if(type==DIAMOND_TYPE_SIZED) {
        if(value.kind!=DIAMOND_VALUE_OBJECT)return false;
        if(value.as.object->kind==DIAMOND_OBJECT_STRING||
           value.as.object->kind==DIAMOND_OBJECT_ARRAY||
           value.as.object->kind==DIAMOND_OBJECT_HASH)return true;
        if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
        const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
        while(class!=nullptr) {
            for(size_t index=0;index<class->method_count;index++)
                if(strcmp(class->methods[index].name,"length")==0&&
                   class->methods[index].arity==0)return true;
            class=class->superclass==UINT8_MAX?nullptr:
                &chunk->classes[class->superclass];
        }
        return false;
    }
    if(type>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(type-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=chunk->interface_count||value.kind!=DIAMOND_VALUE_OBJECT)
            return false;
        const DiamondInterface *interface=&chunk->interfaces[interface_index];
        uint8_t builtin=UINT8_MAX;
        if(value.as.object->kind==DIAMOND_OBJECT_STRING)builtin=DIAMOND_TYPE_STRING;
        else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY)builtin=DIAMOND_TYPE_ARRAY;
        else if(value.as.object->kind==DIAMOND_OBJECT_HASH)builtin=DIAMOND_TYPE_HASH;
        if(builtin!=UINT8_MAX) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint8_t native_return=UINT8_MAX;
                if(!diamond_native_method_satisfies(builtin,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=UINT8_MAX) {
                    if(native_return==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,interface->type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;
            const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
            while(class!=nullptr&&!found) {
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=
                            &interface->methods[required];
                        const DiamondFunction *implementation=
                            &chunk->functions[class->methods[method].function_index];
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint8_t required_set=wanted->parameter_type_sets[parameter];
                            const uint8_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==UINT8_MAX) {
                                if(actual_set!=UINT8_MAX)found=false;
                            } else if(actual_set!=UINT8_MAX&&
                                !runtime_set_satisfies(chunk,interface->type_sets,
                                    required_set,implementation->type_sets,
                                    actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,interface->type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                class=class->superclass==UINT8_MAX?nullptr:
                    &chunk->classes[class->superclass];
            }
            if(!found)return false;
        }
        return true;
    }
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

static bool value_matches_set(const DiamondChunk *chunk,DiamondValue value,
                              uint8_t set_index,bool attach);

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index);

static bool runtime_type_id_satisfies(const DiamondChunk *chunk,uint8_t known,
                                      uint8_t expected) {
    if(known==expected)return true;
    if(expected>=DIAMOND_TYPE_VARIABLE_BASE&&
       expected<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(expected-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr||
           chunk->type_variable_bindings[variable].node_count==0)return true;
        const DiamondBoundTypeNode *root=
            &chunk->type_variable_bindings[variable].nodes[0];
        for(size_t index=0;index<root->count;index++)
            if(runtime_type_id_satisfies(chunk,known,
               root->members[index].id))return true;
        return false;
    }
    if(known>=DIAMOND_TYPE_VARIABLE_BASE&&known<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(known-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr||
           chunk->type_variable_bindings[variable].node_count==0)return true;
        const DiamondBoundTypeNode *root=
            &chunk->type_variable_bindings[variable].nodes[0];
        for(size_t index=0;index<root->count;index++)
            if(!runtime_type_id_satisfies(chunk,root->members[index].id,expected))
                return false;
        return true;
    }
    if(expected==DIAMOND_TYPE_SIZED) {
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH)return true;
        if(known<DIAMOND_TYPE_CLASS_BASE)return false;
        size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
        while(index<chunk->class_count) {
            const DiamondClass *class=&chunk->classes[index];
            for(size_t method=0;method<class->method_count;method++)
                if(strcmp(class->methods[method].name,"length")==0&&
                   class->methods[method].arity==0)return true;
            if(class->superclass==UINT8_MAX)break;
            index=class->superclass;
        }
        return false;
    }
    if(expected>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(expected-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=chunk->interface_count)return false;
        const DiamondInterface *interface=&chunk->interfaces[interface_index];
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint8_t native_return=UINT8_MAX;
                if(!diamond_native_method_satisfies(known,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=UINT8_MAX) {
                    if(native_return==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,interface->type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(known<DIAMOND_TYPE_CLASS_BASE||known>=DIAMOND_TYPE_INTERFACE_BASE)
            return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
            while(index<chunk->class_count&&!found) {
                const DiamondClass *class=&chunk->classes[index];
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=&interface->methods[required];
                        const DiamondFunction *implementation=
                            &chunk->functions[class->methods[method].function_index];
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint8_t required_set=wanted->parameter_type_sets[parameter];
                            const uint8_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==UINT8_MAX) {
                                if(actual_set!=UINT8_MAX)found=false;
                            } else if(actual_set!=UINT8_MAX&&
                                !runtime_set_satisfies(chunk,interface->type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,interface->type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                if(found||class->superclass==UINT8_MAX)break;
                index=class->superclass;
            }
            if(!found)return false;
        }
        return true;
    }
    if(known<DIAMOND_TYPE_CLASS_BASE||expected<DIAMOND_TYPE_CLASS_BASE)return false;
    size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
    const size_t wanted=(size_t)(expected-DIAMOND_TYPE_CLASS_BASE);
    while(index<chunk->class_count) {
        if(index==wanted)return true;
        const uint8_t parent=chunk->classes[index].superclass;
        if(parent==UINT8_MAX)break;
        index=parent;
    }
    return false;
}

static bool runtime_member_satisfies(const DiamondChunk *chunk,
                                     const DiamondTypeSet *known_sets,
                                     DiamondTypeMember known,
                                     const DiamondTypeSet *expected_sets,
                                     DiamondTypeMember expected) {
    if(!runtime_type_id_satisfies(chunk,known.id,expected.id))return false;
    if(expected.id==DIAMOND_TYPE_CALLABLE) {
        if(expected.callable_arity!=UINT8_MAX&&
           known.callable_arity!=expected.callable_arity)return false;
        if(expected.callable_parameters_typed)
            for(size_t parameter=0;parameter<expected.callable_arity;parameter++) {
                const uint8_t wanted=expected.callable_parameter_sets[parameter];
                const uint8_t actual=known.callable_parameter_sets[parameter];
                if(actual!=UINT8_MAX&&
                   !runtime_set_satisfies(chunk,expected_sets,wanted,
                                           known_sets,actual))return false;
            }
        return expected.callable_return_set==UINT8_MAX||
            (known.callable_return_set!=UINT8_MAX&&
             runtime_set_satisfies(chunk,known_sets,known.callable_return_set,
                                   expected_sets,expected.callable_return_set));
    }
    if(expected.argument_set==UINT8_MAX)return true;
    if(known.argument_set==UINT8_MAX||
       !runtime_set_satisfies(chunk,known_sets,known.argument_set,
                              expected_sets,expected.argument_set))return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=UINT8_MAX&&
        expected.second_argument_set!=UINT8_MAX&&
        runtime_set_satisfies(chunk,known_sets,known.second_argument_set,
                              expected_sets,expected.second_argument_set);
}

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=runtime_member_satisfies(chunk,known_sets,
                known->members[source],expected_sets,expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

static bool value_matches_member(const DiamondChunk *chunk,DiamondValue value,
                                 DiamondTypeMember member,bool attach) {
    if(!value_matches_type(chunk,value,member.id))return false;
    if(member.id==DIAMOND_TYPE_CALLABLE) {
        const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
        if(closure->function_index>=chunk->function_count)return false;
        const DiamondFunction *function=&chunk->functions[closure->function_index];
        if(member.callable_arity!=UINT8_MAX&&
           (member.callable_arity<function->required_arity||
            member.callable_arity>function->arity))return false;
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                const uint8_t actual=function->parameter_type_sets[parameter];
                if(actual!=UINT8_MAX&&
                   !runtime_set_satisfies(chunk,chunk->type_sets,
                       member.callable_parameter_sets[parameter],
                       function->type_sets,actual))return false;
            }
        return member.callable_return_set==UINT8_MAX||
            ((size_t)member.callable_return_set<chunk->type_set_count&&
             function->return_type_set!=UINT8_MAX&&
             (size_t)function->return_type_set<function->type_set_count&&
             runtime_set_satisfies(chunk,function->type_sets,
                function->return_type_set,chunk->type_sets,
                member.callable_return_set));
    }
    if(member.argument_set==UINT8_MAX)return true;
    if((size_t)member.argument_set>=chunk->type_set_count)return false;
    if(member.id==DIAMOND_TYPE_ARRAY) {
        DiamondArray *array=(DiamondArray *)value.as.object;
        for(size_t index=0;index<array->count;index++)
            if(!value_matches_set(chunk,array->values[index],member.argument_set,false))
                return false;
        if(!attach)return true;
        for(size_t index=0;index<array->count;index++)
            if(!value_matches_set(chunk,array->values[index],member.argument_set,true))
                return false;
        for(size_t index=0;index<array->constraint_count;index++)
            if(array->constraints[index].type_sets==chunk->type_sets&&
               array->constraints[index].set_index==member.argument_set)return true;
        if(array->constraint_count==4)return false;
        array->constraints[array->constraint_count++]=(typeof(array->constraints[0])){
            .type_sets=chunk->type_sets,.type_set_count=chunk->type_set_count,
            .set_index=member.argument_set,.classes=chunk->classes,
            .class_count=chunk->class_count,.interfaces=chunk->interfaces,
            .interface_count=chunk->interface_count,
            .type_variable_count=chunk->type_variable_count};
        typeof(array->constraints[0]) *constraint=
            &array->constraints[array->constraint_count-1];
        if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr) {
            constraint->type_variable_bindings=malloc(
                chunk->type_variable_count*sizeof(DiamondTypeBinding));
            if(constraint->type_variable_bindings==nullptr) {
                array->constraint_count--;return false;
            }
            memcpy(constraint->type_variable_bindings,chunk->type_variable_bindings,
                   chunk->type_variable_count*sizeof(DiamondTypeBinding));
        }
        return true;
    }
    if(member.id!=DIAMOND_TYPE_HASH||member.second_argument_set==UINT8_MAX||
       (size_t)member.second_argument_set>=chunk->type_set_count)return false;
    DiamondHash *hash=(DiamondHash *)value.as.object;
    for(size_t index=0;index<hash->count;index++)
        if(!value_matches_set(chunk,hash->entries[index].key,member.argument_set,false)||
           !value_matches_set(chunk,hash->entries[index].value,
                              member.second_argument_set,false))return false;
    if(!attach)return true;
    for(size_t index=0;index<hash->count;index++)
        if(!value_matches_set(chunk,hash->entries[index].key,member.argument_set,true)||
           !value_matches_set(chunk,hash->entries[index].value,
                              member.second_argument_set,true))return false;
    for(size_t index=0;index<hash->constraint_count;index++)
        if(hash->constraints[index].type_sets==chunk->type_sets&&
           hash->constraints[index].key_set==member.argument_set&&
           hash->constraints[index].value_set==member.second_argument_set)return true;
    if(hash->constraint_count==4)return false;
    hash->constraints[hash->constraint_count++]=(typeof(hash->constraints[0])){
        .type_sets=chunk->type_sets,.type_set_count=chunk->type_set_count,
        .key_set=member.argument_set,.value_set=member.second_argument_set,
        .classes=chunk->classes,.class_count=chunk->class_count};
    hash->constraints[hash->constraint_count-1].interfaces=chunk->interfaces;
    hash->constraints[hash->constraint_count-1].interface_count=chunk->interface_count;
    hash->constraints[hash->constraint_count-1].type_variable_count=
        chunk->type_variable_count;
    if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr) {
        hash->constraints[hash->constraint_count-1].type_variable_bindings=malloc(
            chunk->type_variable_count*sizeof(DiamondTypeBinding));
        if(hash->constraints[hash->constraint_count-1].
           type_variable_bindings==nullptr) {
            hash->constraint_count--;return false;
        }
        memcpy(hash->constraints[hash->constraint_count-1].type_variable_bindings,
               chunk->type_variable_bindings,
               chunk->type_variable_count*sizeof(DiamondTypeBinding));
    }
    return true;
}

static bool value_matches_set(const DiamondChunk *chunk,DiamondValue value,
                              uint8_t set_index,bool attach) {
    if((size_t)set_index>=chunk->type_set_count)return false;
    const DiamondTypeSet *set=&chunk->type_sets[set_index];
    for(size_t index=0;index<set->count;index++)
        if(value_matches_member(chunk,value,set->members[index],attach))return true;
    return false;
}

static bool array_value_satisfies_constraints(DiamondArray *array,
                                               DiamondValue value) {
    for(size_t index=0;index<array->constraint_count;index++) {
        const typeof(array->constraints[0]) *constraint=&array->constraints[index];
        const DiamondChunk context={.type_sets=constraint->type_sets,
            .type_set_count=constraint->type_set_count,.classes=constraint->classes,
            .class_count=constraint->class_count,.interfaces=constraint->interfaces,
            .interface_count=constraint->interface_count,
            .type_variable_count=constraint->type_variable_count,
            .type_variable_bindings=constraint->type_variable_bindings};
        if(!value_matches_set(&context,value,constraint->set_index,true))return false;
    }
    return true;
}

static bool array_push(DiamondVm *vm,DiamondArray *array,DiamondValue value) {
    if(array->count==array->capacity) {
        if(array->capacity>SIZE_MAX/2/sizeof(DiamondValue))return false;
        const size_t old_capacity=array->capacity;
        const size_t capacity=old_capacity<8?8:old_capacity*2;
        DiamondValue *values=realloc(array->values,capacity*sizeof(DiamondValue));
        if(values==nullptr)return false;
        array->values=values;array->capacity=capacity;
        vm->bytes_allocated+=(capacity-old_capacity)*sizeof(DiamondValue);
    }
    array->values[array->count++]=value;return true;
}

static bool hash_entry_satisfies_constraints(DiamondHash *hash,
                                              DiamondValue key,
                                              DiamondValue value) {
    for(size_t index=0;index<hash->constraint_count;index++) {
        const typeof(hash->constraints[0]) *constraint=&hash->constraints[index];
        const DiamondChunk context={.type_sets=constraint->type_sets,
            .type_set_count=constraint->type_set_count,.classes=constraint->classes,
            .class_count=constraint->class_count,.interfaces=constraint->interfaces,
            .interface_count=constraint->interface_count,
            .type_variable_count=constraint->type_variable_count,
            .type_variable_bindings=constraint->type_variable_bindings};
        if(!value_matches_set(&context,key,constraint->key_set,true)||
           !value_matches_set(&context,value,constraint->value_set,true))return false;
    }
    return true;
}

static bool catch_exception(DiamondVm *vm,const DiamondChunk *chunk,
                            UnwindHandler *handlers,size_t *handler_count,
                            PendingUnwind *pending,DiamondValue *registers,
                            size_t *ip) {
    while(*handler_count>0) {
        (*handler_count)--;
        UnwindHandler *handler=&handlers[*handler_count];
        if(handler->kind==HANDLER_ENSURE) {
            *pending=(PendingUnwind){.kind=PENDING_EXCEPTION,
                                     .value=vm->exception};
            vm->has_exception=false;*ip=handler->target;return true;
        }
        if(!handler->enabled)continue;
        bool matches=handler->type_count==0;
        for(size_t i=0;i<handler->type_count&&!matches;i++)
            matches=value_matches_type(chunk,vm->exception,handler->types[i]);
        if(!matches)continue;
        *ip=handler->target;
        registers[handler->destination]=vm->exception;
        vm->has_exception=false;vm->error[0]='\0';return true;
    }
    return false;
}

static uint8_t exception_class_for_status(DiamondVmStatus status) {
    switch(status) {
        case DIAMOND_VM_TYPE_ERROR: return DIAMOND_CLASS_TYPE_ERROR;
        case DIAMOND_VM_INTEGER_OVERFLOW: return DIAMOND_CLASS_RANGE_ERROR;
        case DIAMOND_VM_DIVISION_BY_ZERO: return DIAMOND_CLASS_ZERO_DIVISION_ERROR;
        case DIAMOND_VM_ARITY_ERROR: return DIAMOND_CLASS_ARGUMENT_ERROR;
        case DIAMOND_VM_STACK_OVERFLOW: return DIAMOND_CLASS_SYSTEM_STACK_ERROR;
        case DIAMOND_VM_INDEX_ERROR: return DIAMOND_CLASS_INDEX_ERROR;
        case DIAMOND_VM_FIBER_NOT_RESUMABLE: return DIAMOND_CLASS_FIBER_ERROR;
        case DIAMOND_VM_YIELD_WITHOUT_FIBER: return DIAMOND_CLASS_FIBER_ERROR;
        case DIAMOND_VM_IO_ERROR: return DIAMOND_CLASS_IO_ERROR;
        case DIAMOND_VM_REGEXP_ERROR: return DIAMOND_CLASS_REGEXP_ERROR;
        case DIAMOND_VM_PROGRAM_ERROR: return DIAMOND_CLASS_RUNTIME_ERROR;
        default: return UINT8_MAX;
    }
}

static bool catch_runtime_error(DiamondVm *vm,const DiamondChunk *chunk,
                                DiamondVmStatus status,UnwindHandler *handlers,
                                size_t *handler_count,PendingUnwind *pending,
                                DiamondValue *registers,size_t *ip) {
    const uint8_t class_index=exception_class_for_status(status);
    if(class_index==UINT8_MAX || (size_t)class_index>=chunk->class_count)return false;
    char message[sizeof vm->error];
    (void)snprintf(message,sizeof message,"%s",vm->error[0]!='\0'?vm->error:
                   diamond_vm_status_name(status));
    DiamondInstance *exception=allocate_instance(vm,&chunk->classes[class_index]);
    if(exception==nullptr)return false;
    vm->exception=DIAMOND_OBJECT(exception);vm->has_exception=true;
    DiamondString *text=allocate_string(vm,message,strlen(message));
    if(text==nullptr)return false;
    if(exception->field_count>0)exception->fields[0]=DIAMOND_OBJECT(text);
    (void)snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                   exception->class->name);
    return catch_exception(vm,chunk,handlers,handler_count,pending,registers,ip);
}

static const char *type_name(const DiamondChunk *chunk,uint8_t type) {
    const char *name="<invalid type>";
    if(type==DIAMOND_TYPE_INT) name="Int";
    else if(type==DIAMOND_TYPE_FLOAT) name="Float";
    else if(type==DIAMOND_TYPE_STRING) name="String";
    else if(type==DIAMOND_TYPE_SYMBOL) name="Symbol";
    else if(type==DIAMOND_TYPE_BOOL) name="Bool";
    else if(type==DIAMOND_TYPE_NIL) name="Nil";
    else if(type==DIAMOND_TYPE_ARRAY) name="Array";
    else if(type==DIAMOND_TYPE_HASH) name="Hash";
    else if(type==DIAMOND_TYPE_CALLABLE) name="Callable";
    else if(type==DIAMOND_TYPE_SIZED) name="Sized";
    else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE)
        name="TypeVariable";
    else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
            (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
        name=chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE].name;
    else {
        const size_t index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
        if(index<chunk->class_count) name=chunk->classes[index].name;
    }
    return name;
}

static void format_type_set_index(char *buffer,size_t capacity,
                                  const DiamondChunk *chunk,uint8_t set_index) {
    if((size_t)set_index>=chunk->type_set_count) {
        snprintf(buffer,capacity,"<invalid type set>");return;
    }
    const DiamondTypeSet *set=&chunk->type_sets[set_index];
    size_t used=0;buffer[0]='\0';
    for(size_t index=0;index<set->count && used<capacity;index++) {
        const int written=snprintf(buffer+used,capacity-used,"%s%s",
            index==0?"":" | ",type_name(chunk,set->members[index].id));
        if(written<0)return;
        used+=(size_t)written;
        if(set->members[index].argument_set!=UINT8_MAX&&used<capacity) {
            const int open=snprintf(buffer+used,capacity-used,"[");
            if(open<0)return;
            used+=(size_t)open;
            char nested[80];format_type_set_index(nested,sizeof nested,chunk,
                set->members[index].argument_set);
            char second[80]="";
            if(set->members[index].second_argument_set!=UINT8_MAX)
                format_type_set_index(second,sizeof second,chunk,
                    set->members[index].second_argument_set);
            const int close=snprintf(buffer+used,capacity-used,"%s%s%s]",nested,
                second[0]=='\0'?"":", ",second);
            if(close<0)return;
            used+=(size_t)close;
        } else if(set->members[index].id==DIAMOND_TYPE_CALLABLE&&
                  set->members[index].callable_arity!=UINT8_MAX&&used<capacity) {
            const DiamondTypeMember member=set->members[index];
            const int arity=snprintf(buffer+used,capacity-used,
                member.callable_parameters_typed?"[[":"[%u",member.callable_arity);
            if(arity<0)return;
            used+=(size_t)arity;
            if(member.callable_parameters_typed) {
                for(size_t parameter=0;parameter<member.callable_arity&&used<capacity;
                    parameter++) {
                    char parameter_type[80];
                    format_type_set_index(parameter_type,sizeof parameter_type,chunk,
                        member.callable_parameter_sets[parameter]);
                    const int result=snprintf(buffer+used,capacity-used,"%s%s",
                        parameter==0?"":", ",parameter_type);
                    if(result<0)return;
                    used+=(size_t)result;
                }
                if(used<capacity) {
                    const int close=snprintf(buffer+used,capacity-used,"]");
                    if(close<0)return;
                    used+=(size_t)close;
                }
            }
            if(member.callable_return_set!=UINT8_MAX&&used<capacity) {
                char returns[80];
                format_type_set_index(returns,sizeof returns,chunk,
                    member.callable_return_set);
                const int result=snprintf(buffer+used,capacity-used,", %s",returns);
                if(result<0)return;
                used+=(size_t)result;
            }
            if(used<capacity) {
                const int close=snprintf(buffer+used,capacity-used,"]");
                if(close<0)return;
                used+=(size_t)close;
            }
        }
    }
}

static void format_value_type(char *buffer, size_t capacity,
                              DiamondValue value) {
    const char *name="<unknown>";
    if(value.kind==DIAMOND_VALUE_NIL) name="Nil";
    else if(value.kind==DIAMOND_VALUE_BOOL) name="Bool";
    else if(value.kind==DIAMOND_VALUE_INT) name="Int";
    else if(value.kind==DIAMOND_VALUE_FLOAT) name="Float";
    else if(value.as.object->kind==DIAMOND_OBJECT_STRING) name="String";
    else if(value.as.object->kind==DIAMOND_OBJECT_SYMBOL) name="Symbol";
    else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY) name="Array";
    else if(value.as.object->kind==DIAMOND_OBJECT_HASH) name="Hash";
    else if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE) name="Callable";
    /* A promoted Int (see object.h's DiamondBignum) is still
     * conceptually an Int, not a distinct user-facing type -- must be
     * checked before the catch-all DiamondInstance branch below, or its
     * memory gets misread through an unrelated struct's layout. */
    else if(value.as.object->kind==DIAMOND_OBJECT_BIGNUM) name="Int";
    else {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        name=instance->class->name;
    }
    snprintf(buffer,capacity,"%s",name);
}

typedef struct StringBuilder {
    char *chars;
    size_t length;
    size_t capacity;
    const DiamondObject *active[32];
    size_t active_count;
} StringBuilder;

static bool builder_append(StringBuilder *builder,const char *chars,size_t length) {
    if(builder->length+length+1>builder->capacity) {
        size_t capacity=builder->capacity==0?64:builder->capacity;
        while(capacity<builder->length+length+1)capacity*=2;
        char *grown=realloc(builder->chars,capacity);
        if(grown==nullptr)return false;
        builder->chars=grown;builder->capacity=capacity;
    }
    memcpy(builder->chars+builder->length,chars,length);
    builder->length+=length;builder->chars[builder->length]='\0';return true;
}

/* Reads one line from stream into builder, growing across as many
 * underlying fgets calls as the line needs, then strips a trailing \n
 * and, if present, \r -- stripping happens on the accumulated line so
 * it's correct regardless of where an fgets chunk boundary falls
 * relative to the line ending. *saw_any is false only when zero bytes
 * were read before EOF. */
static DiamondVmStatus read_line(DiamondVm *vm,FILE *stream,StringBuilder *builder,
                                 bool *saw_any) {
    char chunk_buffer[256];
    *saw_any=false;
    errno=0;
    for(;;) {
        if(fgets(chunk_buffer,sizeof chunk_buffer,stream)==nullptr)break;
        *saw_any=true;
        const size_t piece_length=strlen(chunk_buffer);
        if(!builder_append(builder,chunk_buffer,piece_length))return DIAMOND_VM_OUT_OF_MEMORY;
        if(piece_length>0&&chunk_buffer[piece_length-1]=='\n')break;
    }
    if(ferror(stream)) {
        snprintf(vm->error,sizeof vm->error,"read error: %s",strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    if(builder->length>0&&builder->chars[builder->length-1]=='\n') {
        builder->length--;
        if(builder->length>0&&builder->chars[builder->length-1]=='\r')builder->length--;
        builder->chars[builder->length]='\0';
    }
    return DIAMOND_VM_OK;
}

static bool builder_format_value(StringBuilder *builder,DiamondValue value) {
    char scalar[96];int length=0;
    if(value.kind==DIAMOND_VALUE_NIL)return builder_append(builder,"nil",3);
    if(value.kind==DIAMOND_VALUE_BOOL)
        return builder_append(builder,value.as.boolean?"true":"false",
                              value.as.boolean?4:5);
    if(value.kind==DIAMOND_VALUE_INT) {
        length=snprintf(scalar,sizeof scalar,"%" PRId64,value.as.integer);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    if(value_is_bignum(value)) {
        /* Writes into a freshly-sized buffer rather than through the
         * fixed 96-byte `scalar` array above -- a bignum has no bound
         * on its digit count. */
        const DiamondBignum *bignum=(const DiamondBignum *)value.as.object;
        const size_t capacity=diamond_bignum_string_length(bignum);
        char *digits=malloc(capacity);
        if(digits==nullptr)return false;
        const size_t digit_length=diamond_bignum_to_string(bignum,digits,capacity);
        const bool ok=builder_append(builder,digits,digit_length);
        free(digits);
        return ok;
    }
    if(value.kind==DIAMOND_VALUE_FLOAT) {
        const double real=value.as.real;
        if(isnan(real))return builder_append(builder,"NaN",3);
        if(isinf(real))
            return real<0?builder_append(builder,"-Infinity",9):
                          builder_append(builder,"Infinity",8);
        /* Shortest decimal that round-trips exactly -- see value.c's
         * fprint_float for the full rationale (same algorithm,
         * duplicated per this codebase's existing Int/Float
         * formatting convention between the two call sites), including
         * probing the exponent first so %g doesn't jump to scientific
         * notation for round values like 10.0 just because the search
         * started at a low precision. */
        uint64_t real_bits;memcpy(&real_bits,&real,sizeof real_bits);
        char probe[32];
        snprintf(probe,sizeof probe,"%.0e",real);
        const char *exponent_marker=strchr(probe,'e');
        const int exponent=exponent_marker?atoi(exponent_marker+1):0;
        /* Only bump the starting precision when it can actually keep %g
         * in fixed-point mode (exponent 1..16) -- see value.c's
         * fprint_float for the full rationale (past that, %g would use
         * scientific notation at every precision anyway, so starting at
         * 1 costs nothing and finds a genuinely shorter form when one
         * exists). */
        const int start_precision=(exponent>=1&&exponent<=16)?exponent+1:1;
        for(int precision=start_precision;precision<=17;precision++) {
            length=snprintf(scalar,sizeof scalar,"%.*g",precision,real);
            if(length<0||(size_t)length>=sizeof scalar)continue;
            char *end=nullptr;
            const double parsed=strtod(scalar,&end);
            uint64_t parsed_bits;memcpy(&parsed_bits,&parsed,sizeof parsed_bits);
            if(end!=scalar&&parsed_bits==real_bits)break;
        }
        if(length<0||(size_t)length>=sizeof scalar)return false;
        bool has_marker=false;
        for(int index=0;index<length;index++)
            if(scalar[index]=='.'||scalar[index]=='e'||scalar[index]=='E') {
                has_marker=true;break;
            }
        if(!builder_append(builder,scalar,(size_t)length))return false;
        return has_marker||builder_append(builder,".0",2);
    }
    const DiamondObject *object=value.as.object;
    if(object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)object;
        return builder_append(builder,string->chars,string->length);
    }
    if(object->kind==DIAMOND_OBJECT_SYMBOL) {
        /* Bare name, no leading ':' -- matches Ruby's to_s/puts/
         * interpolation convention (only inspect/p show the colon there,
         * and Diamond has no separate inspect mechanism), and keeps
         * to_sym(to_s(sym)) == sym a true round trip. */
        const DiamondSymbol *symbol=(const DiamondSymbol *)object;
        return builder_append(builder,symbol->chars,symbol->length);
    }
    for(size_t index=0;index<builder->active_count;index++)
        if(builder->active[index]==object)
            return builder_append(builder,
                object->kind==DIAMOND_OBJECT_HASH?"{...}":"[...]",5);
    if(object->kind==DIAMOND_OBJECT_ARRAY||object->kind==DIAMOND_OBJECT_HASH) {
        if(builder->active_count==32)return builder_append(builder,"...",3);
        builder->active[builder->active_count++]=object;
        const bool hash=object->kind==DIAMOND_OBJECT_HASH;
        if(!builder_append(builder,hash?"{":"[",1))return false;
        const size_t count=hash?((const DiamondHash *)object)->count:
                                ((const DiamondArray *)object)->count;
        for(size_t index=0;index<count;index++) {
            if(index>0&&!builder_append(builder,", ",2))return false;
            if(hash) {
                const DiamondHashEntry entry=((const DiamondHash *)object)->entries[index];
                if(!builder_format_value(builder,entry.key)||
                   !builder_append(builder,": ",2)||
                   !builder_format_value(builder,entry.value))return false;
            } else if(!builder_format_value(builder,
                ((const DiamondArray *)object)->values[index]))return false;
        }
        builder->active_count--;
        return builder_append(builder,hash?"}":"]",1);
    }
    if(object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)object;
        length=snprintf(scalar,sizeof scalar,"#<%s>",instance->class->name);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    return builder_append(builder,"#<Closure>",10);
}

static DiamondVmStatus stringify_value(DiamondVm *vm,const DiamondChunk *chunk,
                                        size_t depth,DiamondValue value,
                                        DiamondValue *out) {
    if(value.kind==DIAMOND_VALUE_OBJECT&&
       value.as.object->kind==DIAMOND_OBJECT_STRING) {
        *out=value;return DIAMOND_VM_OK;
    }
    if(value.kind==DIAMOND_VALUE_OBJECT&&
       value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        const DiamondMethod *method=lookup_method(chunk,instance->class,
            "to_s",sizeof("to_s")-1);
        if(method!=nullptr) {
            if(method->required_arity>0)return DIAMOND_VM_ARITY_ERROR;
            const DiamondFunction *fn=&chunk->functions[method->function_index];
            const DiamondChunk child={.name=fn->name,.code=fn->code,
              .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
              .constants=fn->constants,.constant_count=fn->constant_count,
              .strings=fn->strings,.string_count=fn->string_count,
              .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
              .functions=chunk->functions,.function_count=chunk->function_count,
              .classes=chunk->classes,.class_count=chunk->class_count,
              .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
              .parameter_type_sets=fn->parameter_type_sets,
              .type_variable_count=fn->type_variable_count,
              .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
              .register_count=fn->register_count};
            DiamondValue converted=DIAMOND_NIL;
            DiamondVmStatus status=run_chunk(&child,vm,&value,1,
                                              depth+1,nullptr,&converted);
            if(status!=DIAMOND_VM_OK)return status;
            if(converted.kind!=DIAMOND_VALUE_OBJECT||
               converted.as.object->kind!=DIAMOND_OBJECT_STRING) {
                snprintf(vm->error,sizeof vm->error,"to_s must return String");
                return DIAMOND_VM_TYPE_ERROR;
            }
            *out=converted;return DIAMOND_VM_OK;
        }
    }
    StringBuilder builder={};
    if(!builder_format_value(&builder,value)) {
        free(builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondString *string=allocate_string(vm,builder.chars,builder.length);
    free(builder.chars);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

static uint8_t runtime_value_type(const DiamondChunk *chunk,DiamondValue value) {
    if(value.kind==DIAMOND_VALUE_NIL)return DIAMOND_TYPE_NIL;
    if(value.kind==DIAMOND_VALUE_BOOL)return DIAMOND_TYPE_BOOL;
    if(value.kind==DIAMOND_VALUE_INT)return DIAMOND_TYPE_INT;
    if(value.kind==DIAMOND_VALUE_FLOAT)return DIAMOND_TYPE_FLOAT;
    if(value.as.object->kind==DIAMOND_OBJECT_STRING)return DIAMOND_TYPE_STRING;
    if(value.as.object->kind==DIAMOND_OBJECT_ARRAY)return DIAMOND_TYPE_ARRAY;
    if(value.as.object->kind==DIAMOND_OBJECT_HASH)return DIAMOND_TYPE_HASH;
    if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE)return DIAMOND_TYPE_CALLABLE;
    const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
    for(size_t index=0;index<chunk->class_count;index++)
        if(&chunk->classes[index]==class)return (uint8_t)(DIAMOND_TYPE_CLASS_BASE+index);
    return UINT8_MAX;
}

static uint8_t binding_node(DiamondTypeBinding *binding) {
    if(binding->node_count==DIAMOND_BOUND_TYPE_NODES)return UINT8_MAX;
    return binding->node_count++;
}

static DiamondBoundTypeMember *binding_member(DiamondTypeBinding *binding,
                                               uint8_t node,uint8_t id) {
    if(node>=binding->node_count)return nullptr;
    DiamondBoundTypeNode *target=&binding->nodes[node];
    for(size_t index=0;index<target->count;index++)
        if(target->members[index].id==id)return &target->members[index];
    if(target->count==DIAMOND_BOUND_TYPE_MEMBERS)return nullptr;
    target->members[target->count]=(DiamondBoundTypeMember){.id=id,
        .argument_node=UINT8_MAX,.second_argument_node=UINT8_MAX};
    return &target->members[target->count++];
}

static void bind_value_graph(const DiamondChunk *chunk,DiamondTypeBinding *binding,
                             uint8_t node,DiamondValue value) {
    const uint8_t type=runtime_value_type(chunk,value);
    DiamondBoundTypeMember *member=binding_member(binding,node,type);
    if(member==nullptr)return;
    if(type==DIAMOND_TYPE_ARRAY) {
        if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
        if(member->argument_node==UINT8_MAX)return;
        const DiamondArray *array=(const DiamondArray *)value.as.object;
        for(size_t index=0;index<array->count;index++)
            bind_value_graph(chunk,binding,member->argument_node,array->values[index]);
    } else if(type==DIAMOND_TYPE_HASH) {
        if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
        if(member->second_argument_node==UINT8_MAX)
            member->second_argument_node=binding_node(binding);
        if(member->argument_node==UINT8_MAX||member->second_argument_node==UINT8_MAX)return;
        const DiamondHash *hash=(const DiamondHash *)value.as.object;
        for(size_t index=0;index<hash->count;index++) {
            bind_value_graph(chunk,binding,member->argument_node,hash->entries[index].key);
            bind_value_graph(chunk,binding,member->second_argument_node,
                             hash->entries[index].value);
        }
    }
}

static void bind_known_set(DiamondTypeBinding *binding,uint8_t node,
                           const DiamondTypeSet *sets,uint8_t set_index) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember known=set->members[index];
        DiamondBoundTypeMember *member=binding_member(binding,node,known.id);
        if(member==nullptr)continue;
        if(known.argument_set!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
            if(member->argument_node!=UINT8_MAX)
                bind_known_set(binding,member->argument_node,sets,known.argument_set);
        }
        if(known.second_argument_set!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(binding);
            if(member->second_argument_node!=UINT8_MAX)
                bind_known_set(binding,member->second_argument_node,sets,
                               known.second_argument_set);
        }
    }
}

static void bind_bound_node(DiamondTypeBinding *target,uint8_t target_node,
                            const DiamondTypeBinding *source,uint8_t source_node) {
    if(source_node>=source->node_count)return;
    const DiamondBoundTypeNode *node=&source->nodes[source_node];
    for(size_t index=0;index<node->count;index++) {
        const DiamondBoundTypeMember known=node->members[index];
        DiamondBoundTypeMember *member=
            binding_member(target,target_node,known.id);
        if(member==nullptr)continue;
        if(known.argument_node!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)
                member->argument_node=binding_node(target);
            if(member->argument_node!=UINT8_MAX)
                bind_bound_node(target,member->argument_node,source,
                                known.argument_node);
        }
        if(known.second_argument_node!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(target);
            if(member->second_argument_node!=UINT8_MAX)
                bind_bound_node(target,member->second_argument_node,source,
                                known.second_argument_node);
        }
    }
}

static void bind_context_set(DiamondTypeBinding *binding,uint8_t node,
    const DiamondChunk *context,const DiamondTypeSet *sets,uint8_t set_index) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember known=set->members[index];
        if(known.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           known.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(known.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable<context->type_variable_count&&
               context->type_variable_bindings!=nullptr)
                bind_bound_node(binding,node,
                    &context->type_variable_bindings[variable],0);
            continue;
        }
        DiamondBoundTypeMember *member=binding_member(binding,node,known.id);
        if(member==nullptr)continue;
        if(known.argument_set!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)
                member->argument_node=binding_node(binding);
            if(member->argument_node!=UINT8_MAX)
                bind_context_set(binding,member->argument_node,context,sets,
                                 known.argument_set);
        }
        if(known.second_argument_set!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(binding);
            if(member->second_argument_node!=UINT8_MAX)
                bind_context_set(binding,member->second_argument_node,context,sets,
                                 known.second_argument_set);
        }
    }
}

static void infer_from_context_set(const DiamondChunk *known_context,
    const DiamondTypeSet *known_sets,uint8_t known_index,
    const DiamondTypeSet *expected_sets,uint8_t expected_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t target=0;target<expected->count;target++) {
        const DiamondTypeMember wanted=expected->members[target];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=
                (uint8_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)
                (void)binding_node(&bindings[variable]);
            bind_context_set(&bindings[variable],0,known_context,known_sets,
                             known_index);
            continue;
        }
        for(size_t source=0;source<known->count;source++) {
            const DiamondTypeMember actual=known->members[source];
            if(actual.id!=wanted.id)continue;
            if(wanted.argument_set!=UINT8_MAX&&actual.argument_set!=UINT8_MAX)
                infer_from_context_set(known_context,known_sets,
                    actual.argument_set,expected_sets,wanted.argument_set,bindings);
            if(wanted.second_argument_set!=UINT8_MAX&&
               actual.second_argument_set!=UINT8_MAX)
                infer_from_context_set(known_context,known_sets,
                    actual.second_argument_set,expected_sets,
                    wanted.second_argument_set,bindings);
        }
    }
}

static void infer_from_known_set(const DiamondChunk *chunk,
    const DiamondTypeSet *known_sets,uint8_t known_index,
    const DiamondTypeSet *expected_sets,uint8_t expected_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t target=0;target<expected->count;target++) {
        const DiamondTypeMember wanted=expected->members[target];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=
                (uint8_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)
                (void)binding_node(&bindings[variable]);
            bind_known_set(&bindings[variable],0,known_sets,known_index);
        } else {
            for(size_t source=0;source<known->count;source++) {
                const DiamondTypeMember actual=known->members[source];
                if(actual.id!=wanted.id)continue;
                if(wanted.argument_set!=UINT8_MAX&&actual.argument_set!=UINT8_MAX)
                    infer_from_known_set(chunk,known_sets,actual.argument_set,
                        expected_sets,wanted.argument_set,bindings);
                if(wanted.second_argument_set!=UINT8_MAX&&
                   actual.second_argument_set!=UINT8_MAX)
                    infer_from_known_set(chunk,known_sets,actual.second_argument_set,
                        expected_sets,wanted.second_argument_set,bindings);
            }
        }
    }
    (void)chunk;
}

static void infer_from_value(const DiamondChunk *chunk,DiamondValue value,
    const DiamondTypeSet *sets,uint8_t set_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember member=set->members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=(uint8_t)(member.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)(void)binding_node(&bindings[variable]);
            bind_value_graph(chunk,&bindings[variable],0,value);continue;
        }
        if(!value_matches_type(chunk,value,member.id))continue;
        if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_set!=UINT8_MAX) {
            const DiamondArray *array=(const DiamondArray *)value.as.object;
            for(size_t constraint=0;constraint<array->constraint_count;constraint++) {
                const typeof(array->constraints[0]) *known=
                    &array->constraints[constraint];
                const DiamondChunk context={.type_sets=known->type_sets,
                    .type_set_count=known->type_set_count,.classes=known->classes,
                    .class_count=known->class_count,.interfaces=known->interfaces,
                    .interface_count=known->interface_count,
                    .type_variable_bindings=known->type_variable_bindings,
                    .type_variable_count=known->type_variable_count};
                infer_from_context_set(&context,known->type_sets,known->set_index,
                    sets,member.argument_set,bindings);
            }
            for(size_t item=0;item<array->count;item++)
                infer_from_value(chunk,array->values[item],sets,
                                 member.argument_set,bindings);
        } else if(member.id==DIAMOND_TYPE_HASH&&member.argument_set!=UINT8_MAX&&
                  member.second_argument_set!=UINT8_MAX) {
            const DiamondHash *hash=(const DiamondHash *)value.as.object;
            for(size_t constraint=0;constraint<hash->constraint_count;constraint++) {
                const typeof(hash->constraints[0]) *known=
                    &hash->constraints[constraint];
                const DiamondChunk context={.type_sets=known->type_sets,
                    .type_set_count=known->type_set_count,.classes=known->classes,
                    .class_count=known->class_count,.interfaces=known->interfaces,
                    .interface_count=known->interface_count,
                    .type_variable_bindings=known->type_variable_bindings,
                    .type_variable_count=known->type_variable_count};
                infer_from_context_set(&context,known->type_sets,known->key_set,
                    sets,member.argument_set,bindings);
                infer_from_context_set(&context,known->type_sets,known->value_set,
                    sets,member.second_argument_set,bindings);
            }
            for(size_t item=0;item<hash->count;item++) {
                infer_from_value(chunk,hash->entries[item].key,sets,
                                 member.argument_set,bindings);
                infer_from_value(chunk,hash->entries[item].value,sets,
                                 member.second_argument_set,bindings);
            }
        } else if(member.id==DIAMOND_TYPE_CALLABLE) {
            const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
            if(closure->function_index<chunk->function_count) {
                const DiamondFunction *function=&chunk->functions[closure->function_index];
                if(member.callable_parameters_typed)
                    for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                        const uint8_t actual=function->parameter_type_sets[parameter];
                        if(actual!=UINT8_MAX)
                            infer_from_known_set(chunk,function->type_sets,actual,
                                sets,member.callable_parameter_sets[parameter],bindings);
                    }
                if(function->return_type_set!=UINT8_MAX)
                    if(member.callable_return_set!=UINT8_MAX)
                    infer_from_known_set(chunk,function->type_sets,
                        function->return_type_set,sets,member.callable_return_set,
                        bindings);
            }
        }
    }
}

static DiamondVmStatus run_chunk(const DiamondChunk *chunk,
                                 DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
                                 DiamondValue *result) {
    if (depth >= DIAMOND_MAX_CALL_DEPTH) {
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    DiamondChunk execution;
    DiamondTypeBinding bindings[8]={};
    if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr)
        memcpy(bindings,chunk->type_variable_bindings,
               chunk->type_variable_count*sizeof(DiamondTypeBinding));
    if(chunk->type_variable_count>0&&chunk->parameter_type_sets!=nullptr&&
       chunk->type_variable_bindings==nullptr) {
        /* Only copy `*chunk` when this generic-function-with-unbound-
         * type-variable path is actually taken -- the common case
         * (type_variable_count==0, essentially every non-generic call)
         * never reads `execution`, so skip the 168-byte struct copy. */
        execution=*chunk;
        for(size_t parameter=0;
            parameter+chunk->parameter_offset<argument_count;parameter++) {
            const uint8_t set=chunk->parameter_type_sets[parameter];
            if(set!=UINT8_MAX&&set<chunk->type_set_count)
                infer_from_value(chunk,arguments[parameter+chunk->parameter_offset],
                                 chunk->type_sets,set,bindings);
        }
        execution.type_variable_bindings=bindings;
        chunk=&execution;
    }
    DiamondValue registers[DIAMOND_REGISTER_COUNT];
    if (argument_count > DIAMOND_REGISTER_COUNT) {
        return DIAMOND_VM_ARITY_ERROR;
    }
    /* Only registers ever allocated by this function body (the compiler's
     * next_register high-water mark, chunk->register_count) need zeroing --
     * allocate_register() never recycles a slot within one function body,
     * so bytecode can never reference a register past this bound. Narrower
     * than the fixed 256-slot array itself, which stays fully allocated.
     * register_count==0 means an unset field -- every compiler-generated
     * function has at least one register for its return value, so 0 only
     * happens for hand-authored DiamondChunk literals (e.g. tests driving
     * the C API directly) that predate this field; fall back to the full
     * width rather than silently under-zeroing/under-scanning those. */
    const size_t live_register_count =
        chunk->register_count == 0 ? DIAMOND_REGISTER_COUNT : chunk->register_count;
    memset(registers, 0, live_register_count * sizeof(DiamondValue));
    for (size_t index = 0; index < argument_count; index++) {
        registers[index] = arguments[index];
    }
    PendingUnwind pending={};
    DiamondFrame frame = {
        .previous = vm->frames,
        .registers = registers,
        .pending = &pending,
        .register_count = live_register_count,
    };
    vm->frames = &frame;
    size_t ip = 0;
    size_t instruction_offset = 0;
    UnwindHandler handlers[16];
    size_t handler_count=0;

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

#define VM_RETURN(status_)                                           \
    do {                                                             \
        const DiamondVmStatus return_status_=(status_);               \
        if(handler_count>0 && catch_runtime_error(vm,chunk,           \
           return_status_,handlers,&handler_count,&pending,registers,&ip))\
            goto dispatch_continue;                                  \
        RECORD_ERROR(return_status_);                                \
        vm->frames = frame.previous;                                 \
        return vm->has_exception?DIAMOND_VM_EXCEPTION:return_status_;\
    } while (false)

#define VM_PROPAGATE(status_)                                      \
    if ((status_) != DIAMOND_VM_OK) {                              \
        if ((status_) == DIAMOND_VM_EXCEPTION &&                  \
            catch_exception(vm,chunk,handlers,&handler_count,&pending,registers,&ip)) {\
            break;                                                  \
        }                                                           \
        VM_RETURN(status_);                                         \
    }

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
        if (instruction < DIAMOND_OP_COUNT)
            vm->opcode_counts[instruction]++;

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
            case DIAMOND_OP_SYMBOL: {
                uint8_t destination = 0;
                uint8_t string_index = 0;
                READ_BYTE(destination);
                READ_BYTE(string_index);
                if ((size_t)string_index >= chunk->string_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondStringConstant *constant =
                    &chunk->strings[string_index];
                DiamondSymbol *symbol = allocate_symbol(
                    vm, constant->chars, constant->length);
                if (symbol == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination] = DIAMOND_OBJECT(symbol);
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
            case DIAMOND_OP_ARGUMENT_PROVIDED: {
                uint8_t destination=0,index=0;
                READ_BYTE(destination);READ_BYTE(index);
                registers[destination]=DIAMOND_BOOL(index<argument_count);
                break;
            }
            case DIAMOND_OP_TO_STRING: {
                uint8_t destination=0,source=0;
                READ_BYTE(destination);READ_BYTE(source);
                DiamondValue converted=DIAMOND_NIL;
                DiamondVmStatus status=stringify_value(vm,chunk,depth,
                    registers[source],&converted);
                VM_PROPAGATE(status);
                registers[destination]=converted;break;
            }
            case DIAMOND_OP_PRINT: {
                uint8_t destination=0,source=0,newline=0;
                READ_BYTE(destination);READ_BYTE(source);READ_BYTE(newline);
                DiamondValue converted=DIAMOND_NIL;
                DiamondVmStatus status=stringify_value(vm,chunk,depth,
                    registers[source],&converted);
                VM_PROPAGATE(status);
                const DiamondString *text=(const DiamondString *)converted.as.object;
                fwrite(text->chars,1,text->length,stdout);
                if(newline!=0)fputc('\n',stdout);
                registers[destination]=DIAMOND_NIL;break;
            }
            case DIAMOND_OP_GETS: {
                uint8_t destination=0;
                READ_BYTE(destination);
                StringBuilder builder={};
                bool saw_any=false;
                DiamondVmStatus read_status=read_line(vm,stdin,&builder,&saw_any);
                if(read_status!=DIAMOND_VM_OK) {
                    free(builder.chars);VM_RETURN(read_status);
                }
                if(!saw_any) {
                    free(builder.chars);
                    registers[destination]=DIAMOND_NIL;break;
                }
                DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                free(builder.chars);
                if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(string);break;
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
                    if (vm->quickening &&
                        ++vm->quickening_observations >= vm->quickening_threshold) {
                        uint8_t *code=(uint8_t *)(void *)chunk->code;
                        code[instruction_offset]=(uint8_t)DIAMOND_OP_ADD_INT;
                        vm->quickened_sites++;
                    }
                    int64_t sum = 0;
                    if (ckd_add(&sum, registers[left].as.integer,
                                registers[right].as.integer)) {
                        DiamondIntView left_view, right_view;
                        diamond_int_view(registers[left],&left_view);
                        diamond_int_view(registers[right],&right_view);
                        const DiamondValue bignum_result=
                            diamond_bignum_add(vm,left_view,right_view);
                        if(bignum_result.kind==DIAMOND_VALUE_NIL)
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=bignum_result;
                        break;
                    }
                    registers[destination] = DIAMOND_INT(sum);
                    break;
                }
                if (is_int_value(registers[left]) && is_int_value(registers[right]) &&
                    (value_is_bignum(registers[left]) ||
                     value_is_bignum(registers[right]))) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    const DiamondValue bignum_result=
                        diamond_bignum_add(vm,left_view,right_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                if ((registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[left].kind==DIAMOND_VALUE_INT) &&
                    (registers[right].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_INT) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    /* Mixed Int/Float auto-promotes: the Int side widens to
                     * double before the operation (user-confirmed design). */
                    const double left_value=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_value=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    registers[destination]=DIAMOND_FLOAT(left_value+right_value);
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
                if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                    registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=chunk->code+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[left].as.object,
                        "+",1,&registers[right],&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;
                        break;
                    }
                }
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                break;
            }
            case DIAMOND_OP_SUBTRACT:
            case DIAMOND_OP_MULTIPLY:
            case DIAMOND_OP_DIVIDE:
            case DIAMOND_OP_ADD_INT:
            case DIAMOND_OP_SUBTRACT_INT:
            case DIAMOND_OP_MULTIPLY_INT:
            case DIAMOND_OP_DIVIDE_INT: {
                DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                if (vm->quickening &&
                    (opcode == DIAMOND_OP_SUBTRACT ||
                     opcode == DIAMOND_OP_MULTIPLY ||
                     opcode == DIAMOND_OP_DIVIDE) &&
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_SUBTRACT
                        ? DIAMOND_OP_SUBTRACT_INT
                        : opcode == DIAMOND_OP_MULTIPLY
                            ? DIAMOND_OP_MULTIPLY_INT : DIAMOND_OP_DIVIDE_INT;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                if (opcode == DIAMOND_OP_ADD_INT &&
                    (registers[left].kind != DIAMOND_VALUE_INT ||
                     registers[right].kind != DIAMOND_VALUE_INT)) {
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)DIAMOND_OP_ADD;
                    vm->deoptimized_sites++;
                    if (registers[left].kind == DIAMOND_VALUE_OBJECT &&
                        registers[right].kind == DIAMOND_VALUE_OBJECT &&
                        registers[left].as.object->kind == DIAMOND_OBJECT_STRING &&
                        registers[right].as.object->kind == DIAMOND_OBJECT_STRING) {
                        const DiamondString *left_string=(const DiamondString *)
                            registers[left].as.object;
                        const DiamondString *right_string=(const DiamondString *)
                            registers[right].as.object;
                        const size_t length=left_string->length+right_string->length;
                        char *chars=malloc(length+1);
                        if(chars==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        memcpy(chars,left_string->chars,left_string->length);
                        memcpy(chars+left_string->length,right_string->chars,
                               right_string->length);
                        DiamondString *string=allocate_string(vm,chars,length);
                        free(chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=DIAMOND_OBJECT(string);break;
                    }
                    /* A directly-compiled ADD_INT (both operands statically
                     * known Int, not one that arrived here via runtime
                     * quickening -- that path is DIAMOND_OP_ADD's own case
                     * block, which already has this same check) needs its
                     * own bignum check here too: this block retargets the
                     * opcode and handles the deopted instruction inline
                     * rather than truly falling through to a fresh dispatch
                     * of the now-generic ADD, so ADD's own bignum handling
                     * never gets a chance to run for *this* instruction
                     * otherwise. */
                    if (is_int_value(registers[left])&&is_int_value(registers[right])&&
                        (value_is_bignum(registers[left])||
                         value_is_bignum(registers[right]))) {
                        DiamondIntView left_view, right_view;
                        diamond_int_view(registers[left],&left_view);
                        diamond_int_view(registers[right],&right_view);
                        const DiamondValue bignum_result=
                            diamond_bignum_add(vm,left_view,right_view);
                        if(bignum_result.kind==DIAMOND_VALUE_NIL)
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=bignum_result;
                        break;
                    }
                    /* Same reasoning as the bignum check just above: this
                     * deopt handles the instruction inline rather than
                     * falling through to ADD's own case block, so an
                     * operator-overload check is needed here too, not just
                     * in ADD's own already-checked TYPE_ERROR fallback. */
                    if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                        registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                        bool found=false;DiamondValue op_result=DIAMOND_NIL;
                        const uint8_t *site=chunk->code+instruction_offset;
                        const DiamondVmStatus status=invoke_operator_method(vm,chunk,
                            depth,site,(const DiamondInstance *)registers[left].as.object,
                            "+",1,&registers[right],&op_result,&found);
                        if(found) {
                            VM_PROPAGATE(status);
                            registers[destination]=op_result;
                            break;
                        }
                    }
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT never had a deopt
                 * branch at all before bignums existed: the only way an
                 * already-observed-Int operand's kind could stop being
                 * DIAMOND_VALUE_INT was a genuine type violation, so
                 * falling straight to the type-error path below was
                 * correct. Once an Int can legitimately become a bignum
                 * mid-execution that's no longer true -- mirror ADD_INT's
                 * deopt (no string special-case needed here, since only
                 * ADD supports string concatenation). Retargets and falls
                 * through rather than returning, so the bignum check just
                 * below gets a chance at it. */
                if ((opcode==DIAMOND_OP_SUBTRACT_INT||
                     opcode==DIAMOND_OP_MULTIPLY_INT||
                     opcode==DIAMOND_OP_DIVIDE_INT) &&
                    (registers[left].kind!=DIAMOND_VALUE_INT||
                     registers[right].kind!=DIAMOND_VALUE_INT)) {
                    const DiamondOpCode generic=opcode==DIAMOND_OP_SUBTRACT_INT
                        ?DIAMOND_OP_SUBTRACT
                        :opcode==DIAMOND_OP_MULTIPLY_INT
                            ?DIAMOND_OP_MULTIPLY:DIAMOND_OP_DIVIDE;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)generic;
                    opcode=generic;
                    vm->deoptimized_sites++;
                }
                /* Scoped to the three generic (non-_INT) opcodes only - the
                 * _INT forms, once past the deopt check above, always have
                 * both operands confirmed DIAMOND_VALUE_INT by this point. */
                if ((opcode==DIAMOND_OP_SUBTRACT||opcode==DIAMOND_OP_MULTIPLY||
                     opcode==DIAMOND_OP_DIVIDE) &&
                    is_int_value(registers[left])&&is_int_value(registers[right])&&
                    (value_is_bignum(registers[left])||
                     value_is_bignum(registers[right]))) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    DiamondValue bignum_result;
                    if(opcode==DIAMOND_OP_SUBTRACT)
                        bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
                    else if(opcode==DIAMOND_OP_MULTIPLY)
                        bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
                    else {
                        DiamondIntView zero_view;
                        diamond_int_view_int64(0,&zero_view);
                        if(diamond_bignum_compare(right_view,zero_view)==0)
                            VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                        bignum_result=
                            diamond_bignum_divide_truncated(vm,left_view,right_view);
                    }
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                if ((opcode==DIAMOND_OP_SUBTRACT||opcode==DIAMOND_OP_MULTIPLY||
                     opcode==DIAMOND_OP_DIVIDE) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[left].kind==DIAMOND_VALUE_INT) &&
                    (registers[right].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_INT) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    const double left_real=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_real=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    double float_result=0;
                    if(opcode==DIAMOND_OP_SUBTRACT)float_result=left_real-right_real;
                    else if(opcode==DIAMOND_OP_MULTIPLY)float_result=left_real*right_real;
                    /* DIVIDE: IEEE-754 double/0.0 naturally yields
                     * +-Infinity/NaN, no UB and no check needed, unlike Int. */
                    else float_result=left_real/right_real;
                    registers[destination]=DIAMOND_FLOAT(float_result);
                    break;
                }
                if (registers[left].kind != DIAMOND_VALUE_INT ||
                    registers[right].kind != DIAMOND_VALUE_INT) {
                    /* Reached by SUBTRACT/MULTIPLY/DIVIDE (generic, or
                     * retargeted here from their _INT deopt above) with a
                     * non-Int left operand -- ADD_INT can't reach this
                     * point with a non-Int operand, since its own deopt
                     * branch above already handles (and returns for) that
                     * case, so it's never a candidate for "+" dispatch
                     * here. */
                    if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                        registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE &&
                        (opcode==DIAMOND_OP_SUBTRACT||opcode==DIAMOND_OP_MULTIPLY||
                         opcode==DIAMOND_OP_DIVIDE)) {
                        const char *name=opcode==DIAMOND_OP_SUBTRACT?"-":
                            opcode==DIAMOND_OP_MULTIPLY?"*":"/";
                        bool found=false;DiamondValue op_result=DIAMOND_NIL;
                        const uint8_t *site=chunk->code+instruction_offset;
                        const DiamondVmStatus status=invoke_operator_method(vm,chunk,
                            depth,site,(const DiamondInstance *)registers[left].as.object,
                            name,strlen(name),&registers[right],&op_result,&found);
                        if(found) {
                            VM_PROPAGATE(status);
                            registers[destination]=op_result;
                            break;
                        }
                    }
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t left_value = registers[left].as.integer;
                const int64_t right_value = registers[right].as.integer;
                int64_t result_value = 0;
                bool overflow = false;
                if (opcode == DIAMOND_OP_ADD_INT) {
                    overflow = ckd_add(&result_value, left_value, right_value);
                } else if (opcode == DIAMOND_OP_SUBTRACT_INT ||
                           opcode == DIAMOND_OP_SUBTRACT) {
                    overflow = ckd_sub(&result_value, left_value, right_value);
                } else if (opcode == DIAMOND_OP_MULTIPLY_INT ||
                           opcode == DIAMOND_OP_MULTIPLY) {
                    overflow = ckd_mul(&result_value, left_value, right_value);
                } else {
                    if (right_value == 0) {
                        VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                    }
                    if (left_value == INT64_MIN && right_value == -1) {
                        /* -INT64_MIN doesn't fit int64_t; promote
                         * instead of erroring, matching every other
                         * overflow site here (negating left_value's
                         * bignum view flips its sign to positive,
                         * which is exactly -INT64_MIN = 2^63). */
                        DiamondIntView left_view;
                        diamond_int_view(registers[left],&left_view);
                        const DiamondValue bignum_result=
                            diamond_bignum_negate(vm,left_view);
                        if(bignum_result.kind==DIAMOND_VALUE_NIL)
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=bignum_result;
                        break;
                    }
                    result_value = left_value / right_value;
                }
                if (overflow) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    DiamondValue bignum_result;
                    if(opcode==DIAMOND_OP_ADD_INT)
                        bignum_result=diamond_bignum_add(vm,left_view,right_view);
                    else if(opcode==DIAMOND_OP_SUBTRACT_INT||opcode==DIAMOND_OP_SUBTRACT)
                        bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
                    else
                        bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                registers[destination] = DIAMOND_INT(result_value);
                break;
            }
            case DIAMOND_OP_NEGATE: {
                uint8_t destination = 0;
                uint8_t operand = 0;
                READ_BYTE(destination);
                READ_BYTE(operand);
                if (registers[operand].kind == DIAMOND_VALUE_FLOAT) {
                    registers[destination] =
                        DIAMOND_FLOAT(-registers[operand].as.real);
                    break;
                }
                if (value_is_bignum(registers[operand])) {
                    DiamondIntView operand_view;
                    diamond_int_view(registers[operand],&operand_view);
                    const DiamondValue bignum_result=
                        diamond_bignum_negate(vm,operand_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                if (registers[operand].kind==DIAMOND_VALUE_OBJECT &&
                    registers[operand].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=chunk->code+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[operand].as.object,
                        "negate",6,nullptr,&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;
                        break;
                    }
                }
                if (registers[operand].kind != DIAMOND_VALUE_INT) {
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                int64_t result_value = 0;
                if (ckd_sub(&result_value, 0, registers[operand].as.integer)) {
                    DiamondIntView operand_view;
                    diamond_int_view(registers[operand],&operand_view);
                    const DiamondValue bignum_result=
                        diamond_bignum_negate(vm,operand_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                registers[destination] = DIAMOND_INT(result_value);
                break;
            }
            case DIAMOND_OP_EQUAL:
            case DIAMOND_OP_NOT_EQUAL:
            case DIAMOND_OP_EQUAL_INT:
            case DIAMOND_OP_NOT_EQUAL_INT: {
                DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                const bool integer_operands =
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT;
                if (vm->quickening && integer_operands &&
                    (opcode == DIAMOND_OP_EQUAL || opcode == DIAMOND_OP_NOT_EQUAL) &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_EQUAL
                        ? DIAMOND_OP_EQUAL_INT : DIAMOND_OP_NOT_EQUAL_INT;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                if ((opcode == DIAMOND_OP_EQUAL_INT ||
                     opcode == DIAMOND_OP_NOT_EQUAL_INT) && !integer_operands) {
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)(opcode == DIAMOND_OP_EQUAL_INT
                        ? DIAMOND_OP_EQUAL : DIAMOND_OP_NOT_EQUAL);
                    vm->deoptimized_sites++;
                    opcode=(DiamondOpCode)code[instruction_offset];
                }
                if ((opcode == DIAMOND_OP_EQUAL_INT ||
                     opcode == DIAMOND_OP_NOT_EQUAL_INT) && integer_operands) {
                    const bool equal = registers[left].as.integer ==
                        registers[right].as.integer;
                    registers[destination] = DIAMOND_BOOL(
                        opcode == DIAMOND_OP_EQUAL_INT ? equal : !equal);
                    break;
                }
                /* By this point opcode is guaranteed plain EQUAL/NOT_EQUAL
                 * (never the _INT forms -- either integer_operands was
                 * true and the fast path above already broke out, or the
                 * deopt just above already retargeted). No override found
                 * falls through to values_equal unchanged, so two
                 * instances of a class with no "==" compare by identity
                 * exactly as before this feature existed. */
                if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                    registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=chunk->code+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[left].as.object,
                        "==",2,&registers[right],&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        const bool overloaded_equal=is_truthy(op_result);
                        registers[destination]=DIAMOND_BOOL(
                            opcode==DIAMOND_OP_EQUAL?overloaded_equal:!overloaded_equal);
                        break;
                    }
                }
                const bool equal = values_equal(registers[left], registers[right]);
                registers[destination] = DIAMOND_BOOL(
                    opcode == DIAMOND_OP_EQUAL ? equal : !equal);
                break;
            }
            case DIAMOND_OP_LESS:
            case DIAMOND_OP_LESS_EQUAL:
            case DIAMOND_OP_GREATER:
            case DIAMOND_OP_GREATER_EQUAL:
            case DIAMOND_OP_LESS_INT:
            case DIAMOND_OP_LESS_EQUAL_INT:
            case DIAMOND_OP_GREATER_INT:
            case DIAMOND_OP_GREATER_EQUAL_INT: {
                DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint8_t destination = 0;
                uint8_t left = 0;
                uint8_t right = 0;
                READ_BYTE(destination);
                READ_BYTE(left);
                READ_BYTE(right);
                if (vm->quickening &&
                    (opcode == DIAMOND_OP_LESS ||
                     opcode == DIAMOND_OP_LESS_EQUAL ||
                     opcode == DIAMOND_OP_GREATER ||
                     opcode == DIAMOND_OP_GREATER_EQUAL) &&
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_LESS
                        ? DIAMOND_OP_LESS_INT
                        : opcode == DIAMOND_OP_LESS_EQUAL
                            ? DIAMOND_OP_LESS_EQUAL_INT
                            : opcode == DIAMOND_OP_GREATER
                                ? DIAMOND_OP_GREATER_INT
                                : DIAMOND_OP_GREATER_EQUAL_INT;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                /* Same reasoning as SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT:
                 * these four _INT comparisons never had a deopt branch
                 * before bignums existed (a non-Int operand was always a
                 * genuine type error). Mirror the arithmetic block's fix. */
                if ((opcode==DIAMOND_OP_LESS_INT||opcode==DIAMOND_OP_LESS_EQUAL_INT||
                     opcode==DIAMOND_OP_GREATER_INT||
                     opcode==DIAMOND_OP_GREATER_EQUAL_INT) &&
                    (registers[left].kind!=DIAMOND_VALUE_INT||
                     registers[right].kind!=DIAMOND_VALUE_INT)) {
                    const DiamondOpCode generic=opcode==DIAMOND_OP_LESS_INT
                        ?DIAMOND_OP_LESS
                        :opcode==DIAMOND_OP_LESS_EQUAL_INT
                            ?DIAMOND_OP_LESS_EQUAL
                            :opcode==DIAMOND_OP_GREATER_INT
                                ?DIAMOND_OP_GREATER:DIAMOND_OP_GREATER_EQUAL;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)generic;
                    opcode=generic;
                    vm->deoptimized_sites++;
                }
                if ((opcode==DIAMOND_OP_LESS||opcode==DIAMOND_OP_LESS_EQUAL||
                     opcode==DIAMOND_OP_GREATER||opcode==DIAMOND_OP_GREATER_EQUAL) &&
                    is_int_value(registers[left])&&is_int_value(registers[right])&&
                    (value_is_bignum(registers[left])||
                     value_is_bignum(registers[right]))) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    const int comparison=diamond_bignum_compare(left_view,right_view);
                    bool bignum_comparison=false;
                    if(opcode==DIAMOND_OP_LESS)bignum_comparison=comparison<0;
                    else if(opcode==DIAMOND_OP_LESS_EQUAL)bignum_comparison=comparison<=0;
                    else if(opcode==DIAMOND_OP_GREATER)bignum_comparison=comparison>0;
                    else bignum_comparison=comparison>=0;
                    registers[destination]=DIAMOND_BOOL(bignum_comparison);
                    break;
                }
                /* Scoped to the four generic (non-_INT) opcodes only, same
                 * reasoning as the arithmetic block: the _INT forms, once
                 * past the deopt check above, always have both operands
                 * confirmed DIAMOND_VALUE_INT by this point. */
                if ((opcode==DIAMOND_OP_LESS||opcode==DIAMOND_OP_LESS_EQUAL||
                     opcode==DIAMOND_OP_GREATER||opcode==DIAMOND_OP_GREATER_EQUAL) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[left].kind==DIAMOND_VALUE_INT) &&
                    (registers[right].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_INT) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    const double left_real=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_real=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    bool float_comparison=false;
                    if(opcode==DIAMOND_OP_LESS)float_comparison=left_real<right_real;
                    else if(opcode==DIAMOND_OP_LESS_EQUAL)
                        float_comparison=left_real<=right_real;
                    else if(opcode==DIAMOND_OP_GREATER)
                        float_comparison=left_real>right_real;
                    else float_comparison=left_real>=right_real;
                    registers[destination]=DIAMOND_BOOL(float_comparison);
                    break;
                }
                if (registers[left].kind != DIAMOND_VALUE_INT ||
                    registers[right].kind != DIAMOND_VALUE_INT) {
                    /* By this point opcode is guaranteed one of the four
                     * generic (non-_INT) comparisons, same reasoning as
                     * EQUAL/NOT_EQUAL above. */
                    if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                        registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                        const char *name=opcode==DIAMOND_OP_LESS?"<":
                            opcode==DIAMOND_OP_LESS_EQUAL?"<=":
                            opcode==DIAMOND_OP_GREATER?">":">=";
                        bool found=false;DiamondValue op_result=DIAMOND_NIL;
                        const uint8_t *site=chunk->code+instruction_offset;
                        const DiamondVmStatus status=invoke_operator_method(vm,chunk,
                            depth,site,(const DiamondInstance *)registers[left].as.object,
                            name,strlen(name),&registers[right],&op_result,&found);
                        if(found) {
                            VM_PROPAGATE(status);
                            registers[destination]=DIAMOND_BOOL(is_truthy(op_result));
                            break;
                        }
                    }
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t a = registers[left].as.integer;
                const int64_t b = registers[right].as.integer;
                bool comparison = false;
                if (opcode == DIAMOND_OP_LESS_INT || opcode == DIAMOND_OP_LESS)
                    comparison = a < b;
                if (opcode == DIAMOND_OP_LESS_EQUAL_INT ||
                    opcode == DIAMOND_OP_LESS_EQUAL) comparison = a <= b;
                if (opcode == DIAMOND_OP_GREATER_INT ||
                    opcode == DIAMOND_OP_GREATER) comparison = a > b;
                if (opcode == DIAMOND_OP_GREATER_EQUAL_INT ||
                    opcode == DIAMOND_OP_GREATER_EQUAL) comparison = a >= b;
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
                if (call_argument_count < function->required_arity||
                    call_argument_count > function->arity) {
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
                    .type_sets = function->type_sets,
                    .type_set_count = function->type_set_count,
                    .functions = chunk->functions,
                    .function_count = chunk->function_count,
                    .classes = chunk->classes,
                    .class_count = chunk->class_count,
                    .interfaces=chunk->interfaces,
                    .interface_count=chunk->interface_count,
                    .parameter_type_sets=function->parameter_type_sets,
                    .type_variable_count=function->type_variable_count,
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
                    .register_count=function->register_count,
                };
                DiamondValue call_result = DIAMOND_NIL;
                const DiamondVmStatus status = run_chunk(
                    &called_chunk, vm, &registers[argument_base],
                    call_argument_count, depth + 1, nullptr, &call_result);
                VM_PROPAGATE(status);
                registers[destination] = call_result;
                break;
            }
            case DIAMOND_OP_CALL_TYPED: {
                uint8_t destination=0,function_index=0,argument_base=0;
                uint8_t call_argument_count=0,type_argument_count=0;
                READ_BYTE(destination);READ_BYTE(function_index);
                READ_BYTE(argument_base);READ_BYTE(call_argument_count);
                READ_BYTE(type_argument_count);
                if((size_t)function_index>=chunk->function_count||
                   (size_t)argument_base+call_argument_count>DIAMOND_REGISTER_COUNT||
                   type_argument_count>8)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *function=&chunk->functions[function_index];
                if(type_argument_count!=function->type_variable_count||
                   call_argument_count<function->required_arity||
                   call_argument_count>function->arity)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondTypeBinding explicit_bindings[8]={};
                for(size_t index=0;index<type_argument_count;index++) {
                    uint8_t set_index=0;READ_BYTE(set_index);
                    if((size_t)set_index>=chunk->type_set_count)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                                     chunk->type_sets,set_index);
                }
                const DiamondChunk called_chunk={.name=function->name,
                    .code=function->code,.lines=function->lines,
                    .columns=function->columns,.code_count=function->code_count,
                    .constants=function->constants,
                    .constant_count=function->constant_count,
                    .strings=function->strings,.string_count=function->string_count,
                    .type_sets=function->type_sets,
                    .type_set_count=function->type_set_count,
                    .functions=chunk->functions,.function_count=chunk->function_count,
                    .classes=chunk->classes,.class_count=chunk->class_count,
                    .interfaces=chunk->interfaces,
                    .interface_count=chunk->interface_count,
                    .parameter_type_sets=function->parameter_type_sets,
                    .type_variable_count=function->type_variable_count,
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
                    .type_variable_bindings=explicit_bindings,
                    .register_count=function->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&called_chunk,vm,
                    &registers[argument_base],call_argument_count,depth+1,nullptr,
                    &call_result);
                VM_PROPAGATE(status);
                registers[destination]=call_result;
                break;
            }
            case DIAMOND_OP_CLOSURE: {
                uint8_t dest=0,index=0,count=0;READ_BYTE(dest);READ_BYTE(index);READ_BYTE(count);
                if(index>=chunk->function_count||count>16)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captures[16];
                for(size_t i=0;i<count;i++){uint8_t reg=0;READ_BYTE(reg);captures[i]=registers[reg];}
                DiamondClosure *created=allocate_closure(vm,index,captures,count);
                if(created==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(created);break;
            }
            case DIAMOND_OP_GET_CAPTURE: {
                uint8_t dest=0,index=0;READ_BYTE(dest);READ_BYTE(index);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=((DiamondCell *)captured.as.object)->value;break;
            }
            case DIAMOND_OP_GET_CAPTURE_CELL: {
                uint8_t dest=0,index=0;READ_BYTE(dest);READ_BYTE(index);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=captured;break;
            }
            case DIAMOND_OP_SET_CAPTURE: {
                uint8_t index=0,source=0;READ_BYTE(index);READ_BYTE(source);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ((DiamondCell *)captured.as.object)->value=registers[source];break;
            }
            case DIAMOND_OP_BOX_LOCAL: {
                uint8_t reg=0;READ_BYTE(reg);
                if(registers[reg].kind==DIAMOND_VALUE_OBJECT&&
                   registers[reg].as.object->kind==DIAMOND_OBJECT_CELL)break;
                DiamondCell *cell=allocate_cell(vm,registers[reg]);
                if(cell==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[reg]=DIAMOND_OBJECT(cell);break;
            }
            case DIAMOND_OP_GET_CELL: {
                uint8_t dest=0,cell_reg=0;READ_BYTE(dest);READ_BYTE(cell_reg);
                if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=((DiamondCell *)registers[cell_reg].as.object)->value;break;
            }
            case DIAMOND_OP_SET_CELL: {
                uint8_t cell_reg=0,source=0;READ_BYTE(cell_reg);READ_BYTE(source);
                if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ((DiamondCell *)registers[cell_reg].as.object)->value=registers[source];break;
            }
            case DIAMOND_OP_CALL_CLOSURE: {
                uint8_t dest=0,callable=0,base=0,argc=0;
                READ_BYTE(dest);READ_BYTE(callable);READ_BYTE(base);READ_BYTE(argc);
                if(registers[callable].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondClosure *called=(DiamondClosure *)registers[callable].as.object;
                if(called->function_index>=chunk->function_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *fn=&chunk->functions[called->function_index];
                if(argc<fn->required_arity||argc>fn->arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondChunk child={.name=fn->name,.code=fn->code,.lines=fn->lines,
                  .columns=fn->columns,.code_count=fn->code_count,.constants=fn->constants,
                  .constant_count=fn->constant_count,.strings=fn->strings,.string_count=fn->string_count,
                  .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count,
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                  .parameter_type_sets=fn->parameter_type_sets,
                  .type_variable_count=fn->type_variable_count,
                  .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                  .register_count=fn->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus status=run_chunk(&child,vm,&registers[base],argc,depth+1,called,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;break;
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
                    if(argc<init->required_arity||argc>init->arity)
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue args[17];args[0]=registers[dest];
                    for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                    const DiamondFunction *fn=&chunk->functions[init->function_index];
                    DiamondChunk child={.name=fn->name,.code=fn->code,
                      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                      .constants=fn->constants,.constant_count=fn->constant_count,
                      .strings=fn->strings,.string_count=fn->string_count,
                      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                      .functions=chunk->functions,.function_count=chunk->function_count,
                      .classes=chunk->classes,.class_count=chunk->class_count,
                      .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                      .parameter_type_sets=fn->parameter_type_sets,
                      .type_variable_count=fn->type_variable_count,
                      .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                      .register_count=fn->register_count};
                    DiamondValue ignored=DIAMOND_NIL;
                    DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,nullptr,&ignored);
                    VM_PROPAGATE(s);
                } else {
                    bool exception_class=false;const DiamondClass *ancestor=class;
                    while(ancestor!=nullptr) {
                        if(ancestor==&chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
                            exception_class=true;break;
                        }
                        ancestor=ancestor->superclass==UINT8_MAX?nullptr:
                            &chunk->classes[ancestor->superclass];
                    }
                    if(exception_class) {
                        if(argc>2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(argc>0)instance->fields[0]=registers[base];
                        if(argc>1)instance->fields[1]=registers[(size_t)base+1];
                    } else if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                break;
            }
            case DIAMOND_OP_INVOKE:
            case DIAMOND_OP_INVOKE_MONO:
            case DIAMOND_OP_INVOKE_TYPED: {
                uint8_t dest=0,recv=0,name=0,base=0,argc=0;
                READ_BYTE(dest);READ_BYTE(recv);READ_BYTE(name);READ_BYTE(base);READ_BYTE(argc);
                uint8_t type_argument_count=0,type_arguments[8];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED) {
                    READ_BYTE(type_argument_count);
                    if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_argument_count;index++)
                        READ_BYTE(type_arguments[index]);
                }
                if(argc>16) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   (size_t)name>=chunk->string_count) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondStringConstant *method_name=&chunk->strings[name];
                const DiamondObjectKind receiver_kind=registers[recv].as.object->kind;
                if(receiver_kind==DIAMOND_OBJECT_ARRAY||
                   receiver_kind==DIAMOND_OBJECT_HASH||
                   receiver_kind==DIAMOND_OBJECT_STRING) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const bool length_method=method_name->length==6&&
                        memcmp(method_name->chars,"length",6)==0;
                    if(length_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        size_t length=0;
                        if(receiver_kind==DIAMOND_OBJECT_ARRAY)
                            length=((DiamondArray *)registers[recv].as.object)->count;
                        else if(receiver_kind==DIAMOND_OBJECT_HASH)
                            length=((DiamondHash *)registers[recv].as.object)->count;
                        else length=((DiamondString *)registers[recv].as.object)->length;
                        registers[dest]=DIAMOND_INT((int64_t)length);break;
                    }
                    if(receiver_kind==DIAMOND_OBJECT_ARRAY||receiver_kind==DIAMOND_OBJECT_HASH) {
                        const char *target_name=nullptr;
                        if(method_name->length==4&&memcmp(method_name->chars,"each",4)==0)
                            target_name=receiver_kind==DIAMOND_OBJECT_ARRAY?
                                "array_each":"hash_each";
                        else if(method_name->length==6&&
                                memcmp(method_name->chars,"select",6)==0)
                            target_name="enumerable_select";
                        else if(method_name->length==5&&
                                memcmp(method_name->chars,"count",5)==0)
                            target_name="enumerable_count";
                        else if(method_name->length==4&&
                                memcmp(method_name->chars,"any?",4)==0)
                            target_name="enumerable_any";
                        else if(method_name->length==4&&
                                memcmp(method_name->chars,"all?",4)==0)
                            target_name="enumerable_all";
                        else if(method_name->length==6&&
                                memcmp(method_name->chars,"reduce",6)==0)
                            target_name="enumerable_reduce";
                        else if(method_name->length==3&&
                                memcmp(method_name->chars,"map",3)==0)
                            target_name="enumerable_map";
                        if(target_name!=nullptr) {
                            const DiamondFunction *target=
                                find_top_level_function(chunk,target_name,strlen(target_name));
                            if(target==nullptr) {
                                snprintf(vm->error,sizeof vm->error,
                                    "internal error: missing standard library function '%s'",
                                    target_name);
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const size_t total_argc=(size_t)argc+1;
                            if(total_argc<target->required_arity||total_argc>target->arity)
                                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondValue forward_args[17];
                            forward_args[0]=registers[recv];
                            for(size_t i=0;i<argc;i++)
                                forward_args[i+1]=registers[(size_t)base+i];
                            const DiamondChunk child={.name=target->name,.code=target->code,
                              .lines=target->lines,.columns=target->columns,
                              .code_count=target->code_count,
                              .constants=target->constants,.constant_count=target->constant_count,
                              .strings=target->strings,.string_count=target->string_count,
                              .type_sets=target->type_sets,.type_set_count=target->type_set_count,
                              .functions=chunk->functions,.function_count=chunk->function_count,
                              .classes=chunk->classes,.class_count=chunk->class_count,
                              .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                              .parameter_type_sets=target->parameter_type_sets,
                              .type_variable_count=target->type_variable_count,
                              .parameter_offset=target->owner_class==UINT8_MAX?0:1,
                              .register_count=target->register_count};
                            DiamondValue call_result=DIAMOND_NIL;
                            const DiamondVmStatus status=run_chunk(&child,vm,forward_args,
                                total_argc,depth+1,nullptr,&call_result);
                            VM_PROPAGATE(status);
                            registers[dest]=call_result;break;
                        }
                    }
                    if(receiver_kind==DIAMOND_OBJECT_HASH) {
                        const bool key_method=method_name->length==6&&
                            memcmp(method_name->chars,"key_at",6)==0;
                        const bool value_method=method_name->length==8&&
                            memcmp(method_name->chars,"value_at",8)==0;
                        if(!key_method&&!value_method) {
                            snprintf(vm->error,sizeof vm->error,
                                "undefined method '%.*s' for %s",
                                (int)method_name->length,method_name->chars,"Hash");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(registers[base].kind!=DIAMOND_VALUE_INT)
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        DiamondHash *hash=(DiamondHash *)registers[recv].as.object;
                        const int64_t index=registers[base].as.integer;
                        if(index<0||(uint64_t)index>=hash->count) {
                            snprintf(vm->error,sizeof vm->error,
                                "index %" PRId64 " out of bounds for Hash of length %zu",
                                index,hash->count);
                            VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                        }
                        const DiamondHashEntry entry=hash->entries[(size_t)index];
                        registers[dest]=key_method?entry.key:entry.value;break;
                    }
                    if(receiver_kind==DIAMOND_OBJECT_STRING) {
                        const bool index_of_method=method_name->length==8&&
                            memcmp(method_name->chars,"index_of",8)==0;
                        const bool slice_method=method_name->length==5&&
                            memcmp(method_name->chars,"slice",5)==0;
                        const bool to_i_method=method_name->length==4&&
                            memcmp(method_name->chars,"to_i",4)==0;
                        const bool to_f_method=method_name->length==4&&
                            memcmp(method_name->chars,"to_f",4)==0;
                        const bool downcase_method=method_name->length==8&&
                            memcmp(method_name->chars,"downcase",8)==0;
                        const bool upcase_method=method_name->length==6&&
                            memcmp(method_name->chars,"upcase",6)==0;
                        const bool reverse_method=method_name->length==7&&
                            memcmp(method_name->chars,"reverse",7)==0;
                        const bool strip_method=method_name->length==5&&
                            memcmp(method_name->chars,"strip",5)==0;
                        const bool split_method=method_name->length==5&&
                            memcmp(method_name->chars,"split",5)==0;
                        const bool ord_method=method_name->length==3&&
                            memcmp(method_name->chars,"ord",3)==0;
                        const bool repeat_method=method_name->length==6&&
                            memcmp(method_name->chars,"repeat",6)==0;
                        const DiamondString *source=
                            (const DiamondString *)registers[recv].as.object;
                        if(repeat_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_INT) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#repeat argument must be an Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const int64_t count=registers[base].as.integer;
                            if(count<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#repeat argument must be a non-negative Int");
                                VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                            }
                            size_t total_length=0;
                            if(ckd_mul(&total_length,source->length,(size_t)count)) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#repeat result is too large");
                                VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                            }
                            char *buffer=malloc(total_length+1);
                            if(buffer==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t copy=0;copy<(size_t)count;copy++)
                                memcpy(buffer+copy*source->length,source->chars,
                                       source->length);
                            DiamondString *repeated=
                                allocate_string(vm,buffer,total_length);
                            free(buffer);
                            if(repeated==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(repeated);break;
                        }
                        if(ord_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(source->length==0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "cannot take ord of an empty String");
                                VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                            }
                            registers[dest]=
                                DIAMOND_INT((unsigned char)source->chars[0]);
                            break;
                        }
                        if(split_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#split argument must be a String");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *separator=
                                (const DiamondString *)registers[base].as.object;
                            DiamondArray *pieces=allocate_array(vm,nullptr,0);
                            if(pieces==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            /* Root the result array in registers[dest] before any
                             * further allocation (each piece below) can trigger a
                             * GC collection - registers are the VM's root set. */
                            registers[dest]=DIAMOND_OBJECT(pieces);
                            if(separator->length==0) {
                                for(size_t index=0;index<source->length;index++) {
                                    DiamondString *piece=
                                        allocate_string(vm,source->chars+index,1);
                                    if(piece==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                    if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                }
                            } else {
                                size_t start=0,cursor=0;
                                while(cursor+separator->length<=source->length) {
                                    if(memcmp(source->chars+cursor,separator->chars,
                                              separator->length)==0) {
                                        DiamondString *piece=allocate_string(vm,
                                            source->chars+start,cursor-start);
                                        if(piece==nullptr)
                                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                        if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                        cursor+=separator->length;start=cursor;
                                    } else {
                                        cursor++;
                                    }
                                }
                                DiamondString *piece=allocate_string(vm,
                                    source->chars+start,source->length-start);
                                if(piece==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                            break;
                        }
                        if(strip_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            size_t start=0;
                            while(start<source->length&&
                                  isspace((unsigned char)source->chars[start]))start++;
                            size_t end=source->length;
                            while(end>start&&
                                  isspace((unsigned char)source->chars[end-1]))end--;
                            DiamondString *stripped=
                                allocate_string(vm,source->chars+start,end-start);
                            if(stripped==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(stripped);break;
                        }
                        if(reverse_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *reversed=
                                allocate_string(vm,source->chars,source->length);
                            if(reversed==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<reversed->length/2;index++) {
                                const char swap=reversed->chars[index];
                                reversed->chars[index]=
                                    reversed->chars[reversed->length-1-index];
                                reversed->chars[reversed->length-1-index]=swap;
                            }
                            registers[dest]=DIAMOND_OBJECT(reversed);break;
                        }
                        if(downcase_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *lowered=
                                allocate_string(vm,source->chars,source->length);
                            if(lowered==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<lowered->length;index++)
                                lowered->chars[index]=
                                    (char)tolower((unsigned char)lowered->chars[index]);
                            registers[dest]=DIAMOND_OBJECT(lowered);break;
                        }
                        if(upcase_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *raised=
                                allocate_string(vm,source->chars,source->length);
                            if(raised==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<raised->length;index++)
                                raised->chars[index]=
                                    (char)toupper((unsigned char)raised->chars[index]);
                            registers[dest]=DIAMOND_OBJECT(raised);break;
                        }
                        if(to_i_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            size_t position=0;bool negative=false;
                            if(position<source->length&&
                               (source->chars[position]=='-'||source->chars[position]=='+')) {
                                negative=source->chars[position]=='-';position++;
                            }
                            const size_t digit_start=position;
                            int64_t value=0;bool saw_digit=false;bool overflowed=false;
                            while(position<source->length&&
                                  source->chars[position]>='0'&&source->chars[position]<='9') {
                                saw_digit=true;
                                if(!overflowed) {
                                    int64_t widened=0;
                                    if(ckd_mul(&widened,value,(int64_t)10)||
                                       ckd_add(&value,widened,
                                               (int64_t)(source->chars[position]-'0')))
                                        overflowed=true;
                                }
                                position++;
                            }
                            if(!saw_digit) {
                                registers[dest]=DIAMOND_INT(0);
                                break;
                            }
                            if(overflowed) {
                                /* Wider than int64_t: promote instead of
                                 * raising, matching every other overflow
                                 * site now that Int auto-promotes. */
                                const DiamondValue bignum_result=
                                    diamond_bignum_from_decimal_digits(vm,
                                        source->chars+digit_start,
                                        position-digit_start,negative);
                                if(bignum_result.kind==DIAMOND_VALUE_NIL)
                                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                registers[dest]=bignum_result;
                                break;
                            }
                            registers[dest]=DIAMOND_INT(negative?-value:value);
                            break;
                        }
                        if(to_f_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            /* No leading-whitespace skip, matching to_i's
                             * convention -- strtod's own grammar would
                             * otherwise skip it. Overflow is allowed to
                             * become Infinity (unlike to_i, which must
                             * reject out-of-range values): Float already
                             * has a well-defined way to represent "too
                             * large", Int does not. */
                            double value=0.0;
                            if(source->length>0) {
                                const char first=source->chars[0];
                                if((first>='0'&&first<='9')||
                                   first=='+'||first=='-'||first=='.') {
                                    char *end=nullptr;
                                    const double parsed=strtod(source->chars,&end);
                                    if(end!=source->chars)value=parsed;
                                }
                            }
                            registers[dest]=DIAMOND_FLOAT(value);
                            break;
                        }
                        if(index_of_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#index_of argument must be a String");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *needle=
                                (const DiamondString *)registers[base].as.object;
                            registers[dest]=DIAMOND_NIL;
                            if(needle->length==0) {
                                registers[dest]=DIAMOND_INT(0);
                            } else if(needle->length<=source->length) {
                                for(size_t start=0;
                                    start+needle->length<=source->length;start++) {
                                    if(memcmp(source->chars+start,needle->chars,
                                              needle->length)==0) {
                                        registers[dest]=DIAMOND_INT((int64_t)start);break;
                                    }
                                }
                            }
                            break;
                        }
                        if(slice_method) {
                            if(argc!=2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_INT||
                               registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#slice arguments must be Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const int64_t start=registers[base].as.integer;
                            const int64_t requested_length=
                                registers[(size_t)base+1].as.integer;
                            if(start<0||(uint64_t)start>source->length||
                               requested_length<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "index %" PRId64 " out of bounds for String of length %zu",
                                    start,source->length);
                                VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                            }
                            const size_t available=source->length-(size_t)start;
                            const size_t take=(size_t)requested_length<available?
                                (size_t)requested_length:available;
                            DiamondString *sliced=
                                allocate_string(vm,source->chars+(size_t)start,take);
                            if(sliced==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(sliced);break;
                        }
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"String");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(receiver_kind!=DIAMOND_OBJECT_ARRAY)
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondArray *array=(DiamondArray *)registers[recv].as.object;
                    const bool push_method=method_name->length==4&&
                        memcmp(method_name->chars,"push",4)==0;
                    const bool pop_method=method_name->length==3&&
                        memcmp(method_name->chars,"pop",3)==0;
                    if(push_method) {
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(!array_value_satisfies_constraints(array,registers[base])) {
                            snprintf(vm->error,sizeof vm->error,
                                     "array element violates its type annotation");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        if(!array_push(vm,array,registers[base]))
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=registers[recv];break;
                    }
                    if(pop_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        registers[dest]=array->count==0?DIAMOND_NIL:
                            array->values[--array->count];break;
                    }
                    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                        (int)method_name->length,method_name->chars,"Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(receiver_kind==DIAMOND_OBJECT_FIBER) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondFiber *target_fiber=
                        ((DiamondFiberHandle *)registers[recv].as.object)->fiber;
                    const bool resume_method=method_name->length==6&&
                        memcmp(method_name->chars,"resume",6)==0;
                    const bool status_method=method_name->length==6&&
                        memcmp(method_name->chars,"status",6)==0;
                    const bool alive_method=method_name->length==6&&
                        memcmp(method_name->chars,"alive?",6)==0;
                    if(status_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const char *state_name=diamond_fiber_state_name(target_fiber->state);
                        DiamondString *string=allocate_string(vm,state_name,strlen(state_name));
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(alive_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        registers[dest]=DIAMOND_BOOL(
                            target_fiber->state!=DIAMOND_FIBER_COMPLETED&&
                            target_fiber->state!=DIAMOND_FIBER_FAILED);
                        break;
                    }
                    if(!resume_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Fiber");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc>1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    const DiamondValue resume_argument=argc==1?registers[base]:DIAMOND_NIL;
                    if(target_fiber->state!=DIAMOND_FIBER_RUNNABLE&&
                       target_fiber->state!=DIAMOND_FIBER_SUSPENDED) {
                        snprintf(vm->error,sizeof vm->error,
                                 "cannot resume a fiber that is not runnable or suspended");
                        VM_RETURN(DIAMOND_VM_FIBER_NOT_RESUMABLE);
                    }
                    diamond_fiber_resume(target_fiber,resume_argument);
                    diamond_fiber_run(target_fiber);
                    if(target_fiber->status!=DIAMOND_VM_OK&&
                       target_fiber->status!=DIAMOND_VM_YIELDED)
                        VM_PROPAGATE(target_fiber->status);
                    registers[dest]=target_fiber->result;break;
                }
                if(receiver_kind==DIAMOND_OBJECT_FILE) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondFileHandle *target_file=
                        (DiamondFileHandle *)registers[recv].as.object;
                    const bool read_method=method_name->length==4&&
                        memcmp(method_name->chars,"read",4)==0;
                    const bool gets_method=method_name->length==4&&
                        memcmp(method_name->chars,"gets",4)==0;
                    const bool write_method=method_name->length==5&&
                        memcmp(method_name->chars,"write",5)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!read_method&&!gets_method&&!write_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"File");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(close_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(target_file->stream!=nullptr) {
                            fclose(target_file->stream);
                            target_file->stream=nullptr;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(target_file->stream==nullptr) {
                        snprintf(vm->error,sizeof vm->error,"file is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    if(read_method) {
                        if(argc>1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        bool bounded=false;size_t limit=0;
                        if(argc==1) {
                            if(registers[base].kind!=DIAMOND_VALUE_INT||
                               registers[base].as.integer<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "File#read argument must be a non-negative Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            bounded=true;limit=(size_t)registers[base].as.integer;
                        }
                        StringBuilder builder={};
                        char chunk_buffer[4096];
                        size_t read_count=0;
                        errno=0;
                        while(!bounded||builder.length<limit) {
                            const size_t remaining=bounded?limit-builder.length:sizeof chunk_buffer;
                            const size_t want=remaining<sizeof chunk_buffer?
                                remaining:sizeof chunk_buffer;
                            read_count=fread(chunk_buffer,1,want,target_file->stream);
                            if(read_count==0)break;
                            if(!builder_append(&builder,chunk_buffer,read_count)) {
                                free(builder.chars);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                        }
                        if(ferror(target_file->stream)) {
                            free(builder.chars);
                            snprintf(vm->error,sizeof vm->error,"read error: %s",strerror(errno));
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                        free(builder.chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(gets_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        StringBuilder builder={};
                        bool saw_any=false;
                        DiamondVmStatus read_status=
                            read_line(vm,target_file->stream,&builder,&saw_any);
                        if(read_status!=DIAMOND_VM_OK) {
                            free(builder.chars);VM_RETURN(read_status);
                        }
                        if(!saw_any) {
                            free(builder.chars);
                            registers[dest]=DIAMOND_NIL;break;
                        }
                        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                        free(builder.chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue converted=DIAMOND_NIL;
                    DiamondVmStatus status=stringify_value(vm,chunk,depth,
                        registers[base],&converted);
                    VM_PROPAGATE(status);
                    const DiamondString *text=(const DiamondString *)converted.as.object;
                    errno=0;
                    const size_t written=fwrite(text->chars,1,text->length,target_file->stream);
                    if(written!=text->length||ferror(target_file->stream)) {
                        snprintf(vm->error,sizeof vm->error,"write error: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    registers[dest]=DIAMOND_NIL;break;
                }
                if(receiver_kind==DIAMOND_OBJECT_LISTENER) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondListenerHandle *listener=
                        (DiamondListenerHandle *)registers[recv].as.object;
                    const bool accept_method=method_name->length==6&&
                        memcmp(method_name->chars,"accept",6)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!accept_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Listener");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(close_method) {
                        if(listener->fd>=0) {
                            close(listener->fd);
                            listener->fd=-1;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(listener->fd<0) {
                        snprintf(vm->error,sizeof vm->error,"listener is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    errno=0;
                    const int client_fd=accept(listener->fd,nullptr,nullptr);
                    if(client_fd<0) {
                        snprintf(vm->error,sizeof vm->error,"accept failed: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    FILE *client_stream=fdopen(client_fd,"r+");
                    if(client_stream==nullptr) {
                        close(client_fd);
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    DiamondFileHandle *client_handle=allocate_file_handle(vm,client_stream);
                    if(client_handle==nullptr) {
                        fclose(client_stream);
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                        .as.object=(DiamondObject *)client_handle};
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_REGEXP) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const bool match_method=method_name->length==5&&
                        memcmp(method_name->chars,"match",5)==0;
                    const bool match_p_method=method_name->length==6&&
                        memcmp(method_name->chars,"match?",6)==0;
                    if(!match_method&&!match_p_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Regexp");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                        snprintf(vm->error,sizeof vm->error,
                            "Regexp#%.*s argument must be a String",
                            (int)method_name->length,method_name->chars);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    DiamondValue match_dest=DIAMOND_NIL;
                    const DiamondVmStatus match_status=regexp_match_helper(vm,
                        (const DiamondRegexp *)registers[recv].as.object,
                        (const DiamondString *)registers[base].as.object,
                        match_p_method,&match_dest);
                    VM_PROPAGATE(match_status);
                    registers[dest]=match_dest;
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondValue invoke_result=DIAMOND_NIL;
                    const DiamondVmStatus invoke_status=program_builder_invoke_helper(vm,
                        (DiamondProgramBuilder *)registers[recv].as.object,method_name,
                        registers,base,argc,depth,&invoke_result);
                    VM_PROPAGATE(invoke_status);
                    registers[dest]=invoke_result;break;
                }
                if(receiver_kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                bool exception_instance=false;
                const DiamondClass *ancestor=instance->class;
                while(ancestor!=nullptr) {
                    if(ancestor==&chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
                        exception_instance=true;break;
                    }
                    ancestor=ancestor->superclass==UINT8_MAX?nullptr:
                        &chunk->classes[ancestor->superclass];
                }
                if(exception_instance&&method_name->length==7&&
                   memcmp(method_name->chars,"message",7)==0) {
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    registers[dest]=instance->field_count>0?
                        instance->fields[0]:DIAMOND_NIL;break;
                }
                if(exception_instance&&method_name->length==5&&
                   memcmp(method_name->chars,"cause",5)==0) {
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    registers[dest]=instance->field_count>1?
                        instance->fields[1]:DIAMOND_NIL;break;
                }
                const uint8_t *site=chunk->code+instruction_offset;
                const size_t cache_slot=((size_t)(uintptr_t)site>>2)%
                    DIAMOND_INLINE_CACHE_COUNT;
                DiamondMethodCache *cache=&vm->method_caches[cache_slot];
                const DiamondMethod *method=nullptr;
                if ((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_MONO &&
                    cache->site==site && cache->entry_count==1 &&
                    cache->entries[0].receiver_class==instance->class) {
                    method=cache->entries[0].method;
                    vm->inline_cache_hits++;
                    vm->monomorphic_dispatches++;
                } else {
                    if ((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_MONO) {
                        uint8_t *code=(uint8_t *)(void *)chunk->code;
                        code[instruction_offset]=(uint8_t)DIAMOND_OP_INVOKE;
                    }
                    method=lookup_method_cached(vm,chunk,site,instance->class,
                        method_name->chars,method_name->length);
                    if ((DiamondOpCode)instruction==DIAMOND_OP_INVOKE &&
                        cache->entry_count==1 &&
                        cache->hits>=vm->monomorphic_threshold) {
                        uint8_t *code=(uint8_t *)(void *)chunk->code;
                        code[instruction_offset]=(uint8_t)DIAMOND_OP_INVOKE_MONO;
                        record_rewritten_site(vm,site);
                        vm->direct_dispatch_rewrites++;
                    }
                }
                if(method==nullptr) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(method->is_private&&!(chunk->parameter_offset==1&&recv==0)) {
                    snprintf(vm->error,sizeof vm->error,
                        "private method '%.*s' called with an explicit receiver",
                        (int)method_name->length,method_name->chars);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(argc<method->required_arity||argc>method->arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17];args[0]=registers[recv];
                for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=&chunk->functions[method->function_index];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED&&
                   type_argument_count!=fn->type_variable_count)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondTypeBinding explicit_bindings[8]={};
                for(size_t index=0;index<type_argument_count;index++) {
                    if((size_t)type_arguments[index]>=chunk->type_set_count)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                        chunk->type_sets,type_arguments[index]);
                }
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count,
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                  .parameter_type_sets=fn->parameter_type_sets,
                  .type_variable_count=fn->type_variable_count,
                  .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                  .type_variable_bindings=type_argument_count==0?nullptr:
                      explicit_bindings,
                  .register_count=fn->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,nullptr,&call_result);
                VM_PROPAGATE(s);
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
                if(argc<method->required_arity||argc>method->arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17]; args[0]=registers[0];
                for(size_t i=0;i<argc;i++) args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=&chunk->functions[method->function_index];
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count,
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                  .parameter_type_sets=fn->parameter_type_sets,
                  .type_variable_count=fn->type_variable_count,
                  .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                  .register_count=fn->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus status=run_chunk(&child,vm,args,(size_t)argc+1,
                                                  depth+1,nullptr,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_GET_IVAR: {
                const uint8_t *site=&chunk->code[instruction_offset];
                uint8_t dest=0,recv=0,field=0;READ_BYTE(dest);READ_BYTE(recv);READ_BYTE(field);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                if((size_t)field>=instance->field_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFieldCacheEntry *cached=lookup_field_cached(
                    vm,site,instance,field,false);
                registers[dest]=cached->materialized
                    ? instance->fields[field] : DIAMOND_NIL;break;
            }
            case DIAMOND_OP_SET_IVAR: {
                const uint8_t *site=&chunk->code[instruction_offset];
                uint8_t recv=0,field=0,source=0;READ_BYTE(recv);READ_BYTE(field);READ_BYTE(source);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                if((size_t)field>=instance->field_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFieldCacheEntry *cached=lookup_field_cached(
                    vm,site,instance,field,true);
                if(instance->shape!=cached->output_shape) {
                    instance->shape=cached->output_shape;
                    vm->shape_transitions++;
                }
                instance->fields[field]=registers[source];break;
            }
            case DIAMOND_OP_GET_IVAR_NAME:
            case DIAMOND_OP_SET_IVAR_NAME: {
                const uint8_t *site=&chunk->code[instruction_offset];
                uint8_t first=0,receiver=0,name=0;
                READ_BYTE(first);READ_BYTE(receiver);READ_BYTE(name);
                const bool write=(DiamondOpCode)instruction==DIAMOND_OP_SET_IVAR_NAME;
                const uint8_t recv=write?first:receiver;
                const uint8_t source=write?name:0;
                const uint8_t string_index=write?receiver:name;
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE||
                   (size_t)string_index>=chunk->string_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const int resolved=named_field_index(instance,
                    &chunk->strings[string_index]);
                if(resolved<0||(size_t)resolved>=instance->field_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const uint8_t field=(uint8_t)resolved;
                DiamondFieldCacheEntry *cached=lookup_field_cached(
                    vm,site,instance,field,write);
                if(write) {
                    if(instance->shape!=cached->output_shape) {
                        instance->shape=cached->output_shape;
                        vm->shape_transitions++;
                    }
                    instance->fields[field]=registers[source];
                } else registers[first]=cached->materialized?
                    instance->fields[field]:DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_GET_NAMESPACE_CONSTANT: {
                uint8_t destination=0,index=0;READ_BYTE(destination);READ_BYTE(index);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS||
                   !vm->namespace_constant_initialized[index])
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[destination]=vm->namespace_constants[index];break;
            }
            case DIAMOND_OP_SET_NAMESPACE_CONSTANT: {
                uint8_t index=0,source=0;READ_BYTE(index);READ_BYTE(source);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS||
                   vm->namespace_constant_initialized[index])
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                vm->namespace_constants[index]=registers[source];
                vm->namespace_constant_initialized[index]=true;break;
            }
            case DIAMOND_OP_CHECK_TYPE: {
                uint8_t source=0,set_index=0; READ_BYTE(source); READ_BYTE(set_index);
                if((size_t)set_index>=chunk->type_set_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const bool matches=value_matches_set(chunk,registers[source],
                                                     set_index,true);
                if(!matches) {
                    char expected[80]; char actual[80];
                    format_type_set_index(expected,sizeof expected,chunk,set_index);
                    format_value_type(actual,sizeof actual,registers[source]);
                    snprintf(vm->error,sizeof vm->error,"expected %s, got %s",
                             expected,actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                break;
            }
            case DIAMOND_OP_IS_TYPE: {
                uint8_t destination=0,source=0,type=0;
                READ_BYTE(destination);READ_BYTE(source);READ_BYTE(type);
                registers[destination]=DIAMOND_BOOL(
                    value_matches_type(chunk,registers[source],type));
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
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_STRING) {
                    if(registers[index_register].kind!=DIAMOND_VALUE_INT)
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondString *source=
                        (const DiamondString *)registers[receiver].as.object;
                    const int64_t index=registers[index_register].as.integer;
                    if(index<0 || (uint64_t)index>=source->length) {
                        snprintf(vm->error,sizeof vm->error,
                                 "index %" PRId64 " out of bounds for String of length %zu",
                                 index,source->length);
                        VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                    }
                    DiamondString *character=
                        allocate_string(vm,source->chars+(size_t)index,1);
                    if(character==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=DIAMOND_OBJECT(character);
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
                    DiamondHash *hash=(DiamondHash *)registers[receiver].as.object;
                    if(!hash_entry_satisfies_constraints(hash,
                       registers[index_register],registers[source])) {
                        snprintf(vm->error,sizeof vm->error,
                                 "hash entry violates its type annotation");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(!hash_set(vm,hash,registers[index_register],registers[source]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    break;
                }
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                             "String does not support element assignment");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
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
                if(!array_value_satisfies_constraints(array,registers[source])) {
                    snprintf(vm->error,sizeof vm->error,
                             "array element violates its type annotation");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
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
                while(handler_count>0 &&
                      handlers[handler_count-1].kind!=HANDLER_ENSURE)
                    handler_count--;
                if(handler_count>0) {
                    const UnwindHandler handler=handlers[--handler_count];
                    pending=(PendingUnwind){.kind=PENDING_RETURN,
                                            .value=registers[source]};
                    ip=handler.target;break;
                }
                *result = registers[source];
                VM_RETURN(DIAMOND_VM_OK);
            }
            case DIAMOND_OP_RAISE: {
                uint8_t source=0;READ_BYTE(source);
                vm->exception=registers[source];vm->has_exception=true;
                if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                   registers,&ip))break;
                if(vm->exception.kind==DIAMOND_VALUE_INT)
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %" PRId64,
                             vm->exception.as.integer);
                else if(vm->exception.kind==DIAMOND_VALUE_BOOL)
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                             vm->exception.as.boolean?"true":"false");
                else if(vm->exception.kind==DIAMOND_VALUE_NIL)
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: nil");
                else if(vm->exception.kind==DIAMOND_VALUE_FLOAT) {
                    StringBuilder message_builder={};
                    if(builder_format_value(&message_builder,vm->exception))
                        snprintf(vm->error,sizeof vm->error,"uncaught exception: %.*s",
                                 (int)message_builder.length,message_builder.chars);
                    else
                        snprintf(vm->error,sizeof vm->error,"uncaught exception: <float>");
                    free(message_builder.chars);
                } else if(vm->exception.as.object->kind==DIAMOND_OBJECT_STRING) {
                    const DiamondString *string=(const DiamondString *)vm->exception.as.object;
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %.*s",
                             (int)string->length,string->chars);
                } else if(vm->exception.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    const DiamondInstance *instance=(const DiamondInstance *)vm->exception.as.object;
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                             instance->class->name);
                } else snprintf(vm->error,sizeof vm->error,"uncaught exception: object");
                VM_RETURN(DIAMOND_VM_EXCEPTION);
            }
            case DIAMOND_OP_PUSH_RESCUE: {
                uint8_t destination=0,type_count=0,types[8],high=0,low=0;
                READ_BYTE(destination);READ_BYTE(type_count);
                for(size_t i=0;i<8;i++)READ_BYTE(types[i]);
                READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                const bool enabled=(type_count&0x80)==0;
                type_count&=0x7f;
                if(handler_count==16||type_count>8||target>chunk->code_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                UnwindHandler *handler=&handlers[handler_count++];
                *handler=(UnwindHandler){.kind=HANDLER_RESCUE,.target=target,
                    .destination=destination,.type_count=type_count,.enabled=enabled};
                for(size_t i=0;i<type_count;i++)handler->types[i]=types[i];
                break;
            }
            case DIAMOND_OP_POP_RESCUE:
                if(handler_count==0||handlers[handler_count-1].kind!=HANDLER_RESCUE)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                handler_count--;break;
            case DIAMOND_OP_PUSH_ENSURE: {
                uint8_t high=0,low=0;READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                if(handler_count==16||target>chunk->code_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                handlers[handler_count++]=(UnwindHandler){
                    .kind=HANDLER_ENSURE,.target=target,.enabled=true};
                break;
            }
            case DIAMOND_OP_RUN_ENSURE: {
                uint8_t high=0,low=0;READ_BYTE(high);READ_BYTE(low);
                const size_t continuation=((size_t)high<<8)|low;
                if(handler_count==0||continuation>chunk->code_count||
                   handlers[handler_count-1].kind!=HANDLER_ENSURE)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const UnwindHandler handler=handlers[--handler_count];
                pending=(PendingUnwind){.kind=PENDING_NORMAL,
                                        .continuation=continuation};
                ip=handler.target;break;
            }
            case DIAMOND_OP_END_ENSURE: {
                const PendingUnwind resume=pending;pending=(PendingUnwind){};
                if(resume.kind==PENDING_NORMAL) {ip=resume.continuation;break;}
                if(resume.kind==PENDING_RETURN) {
                    while(handler_count>0 &&
                          handlers[handler_count-1].kind!=HANDLER_ENSURE)
                        handler_count--;
                    if(handler_count>0) {
                        const UnwindHandler handler=handlers[--handler_count];
                        pending=resume;ip=handler.target;break;
                    }
                    *result=resume.value;VM_RETURN(DIAMOND_VM_OK);
                }
                if(resume.kind==PENDING_EXCEPTION) {
                    vm->exception=resume.value;vm->has_exception=true;
                    if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                       registers,&ip))break;
                    VM_RETURN(DIAMOND_VM_EXCEPTION);
                }
                VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
            }
            case DIAMOND_OP_YIELD: {
                uint8_t dest=0,source=0;
                READ_BYTE(dest);READ_BYTE(source);
                if(vm->running_fiber==nullptr) VM_RETURN(DIAMOND_VM_YIELD_WITHOUT_FIBER);
                vm->running_fiber->status=DIAMOND_VM_YIELDED;
                vm->running_fiber->result=registers[source];
                swapcontext(&vm->running_fiber->context,vm->running_fiber->resume_target);
                registers[dest]=vm->running_fiber->resume_value;
                break;
            }
            case DIAMOND_OP_REDEFINE_METHOD: {
                uint8_t dest=0,class_operand=0,name_reg=0,callable_reg=0;
                READ_BYTE(dest);READ_BYTE(class_operand);READ_BYTE(name_reg);READ_BYTE(callable_reg);
                if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"redefine_method name must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,"redefine_method callable must be a Callable value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
                DiamondClosure *replacement=(DiamondClosure *)registers[callable_reg].as.object;
                DiamondClass *class=(DiamondClass *)(void *)&chunk->classes[class_operand];
                DiamondMethod *target=nullptr;
                for(size_t index=0;index<class->method_count;index++)
                    if(strlen(class->methods[index].name)==name_string->length&&
                       memcmp(class->methods[index].name,name_string->chars,name_string->length)==0) {
                        target=&class->methods[index];break;
                    }
                if(target==nullptr) {
                    snprintf(vm->error,sizeof vm->error,"class '%s' has no method '%.*s' to redefine",
                             class->name,(int)name_string->length,name_string->chars);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(replacement->capture_count!=0) {
                    snprintf(vm->error,sizeof vm->error,
                             "redefine_method callable must not capture any variables");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if((size_t)replacement->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *new_function=&chunk->functions[replacement->function_index];
                if(new_function->owner_class!=class_operand) {
                    snprintf(vm->error,sizeof vm->error,
                             "redefine_method callable must be a method of '%s'",class->name);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const uint8_t new_arity=(uint8_t)(new_function->arity-1);
                const uint8_t new_required_arity=(uint8_t)(new_function->required_arity-1);
                if(new_arity!=target->arity||new_required_arity!=target->required_arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                target->function_index=replacement->function_index;
                diamond_vm_invalidate_method_caches(vm);
                registers[dest]=DIAMOND_NIL;break;
            }
            case DIAMOND_OP_FIBER_NEW: {
                uint8_t dest=0,callable_reg=0;
                READ_BYTE(dest);READ_BYTE(callable_reg);
                if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,"Fiber.new argument must be a Callable value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondClosure *callable=(const DiamondClosure *)registers[callable_reg].as.object;
                if((size_t)callable->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *target_fn=&chunk->functions[callable->function_index];
                if(target_fn->arity!=0) {
                    snprintf(vm->error,sizeof vm->error,"Fiber.new callable must take no arguments");
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                DiamondFiber *new_fiber=diamond_fiber_new_for_closure(chunk,callable);
                if(new_fiber==nullptr||diamond_fiber_bind_vm(new_fiber,vm)!=DIAMOND_FIBER_OK||
                   diamond_fiber_prepare(new_fiber)!=DIAMOND_FIBER_OK) {
                    diamond_fiber_free(new_fiber);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                DiamondFiberHandle *handle=allocate_fiber_handle(vm,new_fiber);
                if(handle==nullptr) {
                    diamond_fiber_free(new_fiber);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_FILE_OPEN: {
                uint8_t dest=0,path_reg=0,mode_reg=0;
                READ_BYTE(dest);READ_BYTE(path_reg);READ_BYTE(mode_reg);
                if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[mode_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[mode_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"File.open arguments must be String values");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
                const DiamondString *mode=(const DiamondString *)registers[mode_reg].as.object;
                errno=0;
                FILE *stream=fopen(path->chars,mode->chars);
                if(stream==nullptr) {
                    snprintf(vm->error,sizeof vm->error,"cannot open '%.*s': %s",
                             (int)path->length,path->chars,strerror(errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                DiamondFileHandle *handle=allocate_file_handle(vm,stream);
                if(handle==nullptr) {
                    fclose(stream);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_REGEXP_NEW: {
                uint8_t dest=0,pattern_reg=0,options_reg=0;
                READ_BYTE(dest);READ_BYTE(pattern_reg);READ_BYTE(options_reg);
                if(registers[pattern_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[pattern_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[options_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                        "Regexp.new arguments must be a String pattern and an Int options");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondValue new_result=DIAMOND_NIL;
                const DiamondVmStatus new_status=regexp_new_helper(vm,
                    (const DiamondString *)registers[pattern_reg].as.object,
                    registers[options_reg].as.integer,&new_result);
                VM_PROPAGATE(new_status);
                registers[dest]=new_result;
                break;
            }
            case DIAMOND_OP_PROGRAM_BUILDER_NEW: {
                uint8_t dest=0;
                READ_BYTE(dest);
                DiamondProgramBuilder *handle=allocate_program_builder(vm);
                if(handle==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_TCP_CONNECT: {
                uint8_t dest=0,host_reg=0,port_reg=0;
                READ_BYTE(dest);READ_BYTE(host_reg);READ_BYTE(port_reg);
                if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "TCPSocket.connect arguments must be a String host and an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
                char port_text[32];
                (void)snprintf(port_text,sizeof port_text,"%" PRId64,
                               registers[port_reg].as.integer);
                struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM};
                struct addrinfo *results=nullptr;
                const int resolve_status=getaddrinfo(host->chars,port_text,&hints,&results);
                if(resolve_status!=0) {
                    snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                             (int)host->length,host->chars,port_text,gai_strerror(resolve_status));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                int connected_fd=-1;
                int last_errno=0;
                for(struct addrinfo *candidate=results;candidate!=nullptr;
                    candidate=candidate->ai_next) {
                    const int fd=socket(candidate->ai_family,candidate->ai_socktype,
                                         candidate->ai_protocol);
                    if(fd<0) {last_errno=errno;continue;}
                    if(connect(fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
                        connected_fd=fd;break;
                    }
                    last_errno=errno;close(fd);
                }
                freeaddrinfo(results);
                if(connected_fd<0) {
                    snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                             (int)host->length,host->chars,port_text,strerror(last_errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                FILE *stream=fdopen(connected_fd,"r+");
                if(stream==nullptr) {
                    close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                DiamondFileHandle *handle=allocate_file_handle(vm,stream);
                if(handle==nullptr) {
                    fclose(stream);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_TCP_LISTEN: {
                uint8_t dest=0,port_reg=0;
                READ_BYTE(dest);READ_BYTE(port_reg);
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "TCPServer.listen argument must be an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                char port_text[32];
                (void)snprintf(port_text,sizeof port_text,"%" PRId64,
                               registers[port_reg].as.integer);
                struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM,
                    .ai_flags=AI_PASSIVE};
                struct addrinfo *results=nullptr;
                const int resolve_status=getaddrinfo(nullptr,port_text,&hints,&results);
                if(resolve_status!=0) {
                    snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                             port_text,gai_strerror(resolve_status));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                int listening_fd=-1;
                int last_errno=0;
                for(struct addrinfo *candidate=results;candidate!=nullptr;
                    candidate=candidate->ai_next) {
                    const int fd=socket(candidate->ai_family,candidate->ai_socktype,
                                         candidate->ai_protocol);
                    if(fd<0) {last_errno=errno;continue;}
                    const int yes=1;
                    (void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
                    if(bind(fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
                        listening_fd=fd;break;
                    }
                    last_errno=errno;close(fd);
                }
                freeaddrinfo(results);
                if(listening_fd<0) {
                    snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                             port_text,strerror(last_errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                if(listen(listening_fd,16)!=0) {
                    snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                             port_text,strerror(errno));
                    close(listening_fd);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                DiamondListenerHandle *listener_handle=
                    allocate_listener_handle(vm,listening_fd);
                if(listener_handle==nullptr) {
                    close(listening_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)listener_handle};
                break;
            }
            case DIAMOND_OP_CHR: {
                uint8_t dest=0,source=0;
                READ_BYTE(dest);READ_BYTE(source);
                if(registers[source].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,"chr argument must be an Int");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t code=registers[source].as.integer;
                if(code<0||code>255) {
                    snprintf(vm->error,sizeof vm->error,
                             "chr argument must be between 0 and 255");
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                const char byte=(char)code;
                DiamondString *string=allocate_string(vm,&byte,1);
                if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(string);break;
            }
            case DIAMOND_OP_TO_FLOAT: {
                uint8_t dest=0,source=0;
                READ_BYTE(dest);READ_BYTE(source);
                double as_double=0.0;
                if(!is_int_value(registers[source])||
                   !numeric_as_double(registers[source],&as_double)) {
                    snprintf(vm->error,sizeof vm->error,"to_f argument must be an Int");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                registers[dest]=DIAMOND_FLOAT(as_double);
                break;
            }
            case DIAMOND_OP_TO_INT: {
                uint8_t dest=0,source=0;
                READ_BYTE(dest);READ_BYTE(source);
                if(registers[source].kind!=DIAMOND_VALUE_FLOAT) {
                    snprintf(vm->error,sizeof vm->error,"to_i argument must be a Float");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const double real=registers[source].as.real;
                if(isnan(real)||isinf(real)) {
                    snprintf(vm->error,sizeof vm->error,
                             "to_i argument must be a finite Float");
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                if(real>=9223372036854775808.0||real<-9223372036854775808.0) {
                    /* Outside int64_t range: promote instead of raising,
                     * matching every other overflow site now that Int
                     * auto-promotes to a bignum. */
                    const DiamondValue bignum_result=diamond_bignum_from_double(vm,real);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[dest]=bignum_result;
                    break;
                }
                registers[dest]=DIAMOND_INT((int64_t)real);
                break;
            }
            case DIAMOND_OP_TO_SYMBOL: {
                uint8_t dest=0,source=0;
                READ_BYTE(dest);READ_BYTE(source);
                if(registers[source].kind!=DIAMOND_VALUE_OBJECT||
                   registers[source].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"to_sym argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *source_string=
                    (const DiamondString *)registers[source].as.object;
                DiamondSymbol *symbol=allocate_symbol(vm,source_string->chars,
                    source_string->length);
                if(symbol==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(symbol);
                break;
            }
            case DIAMOND_OP_MATH_UNARY: {
                uint8_t dest=0,source=0,function_id=0;
                READ_BYTE(dest);READ_BYTE(source);READ_BYTE(function_id);
                double operand=0.0;
                if(!numeric_as_double(registers[source],&operand)) {
                    snprintf(vm->error,sizeof vm->error,
                             "math function argument must be an Int or Float");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                double math_result=0.0;
                switch((DiamondMathFunction)function_id) {
                    case DIAMOND_MATH_SQRT: math_result=sqrt(operand); break;
                    case DIAMOND_MATH_SIN: math_result=sin(operand); break;
                    case DIAMOND_MATH_COS: math_result=cos(operand); break;
                    case DIAMOND_MATH_TAN: math_result=tan(operand); break;
                    default: VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[dest]=DIAMOND_FLOAT(math_result);
                break;
            }
            case DIAMOND_OP_MATH_BINARY: {
                uint8_t dest=0,left=0,right=0,function_id=0;
                READ_BYTE(dest);READ_BYTE(left);READ_BYTE(right);READ_BYTE(function_id);
                double left_value=0.0,right_value=0.0;
                if(!numeric_as_double(registers[left],&left_value)||
                   !numeric_as_double(registers[right],&right_value)) {
                    snprintf(vm->error,sizeof vm->error,
                             "math function argument must be an Int or Float");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                double math_result=0.0;
                switch((DiamondMathFunction)function_id) {
                    case DIAMOND_MATH_POW: math_result=pow(left_value,right_value); break;
                    default: VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[dest]=DIAMOND_FLOAT(math_result);
                break;
            }
            default:
                VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        }
dispatch_continue:
        continue;
    }

#undef READ_BYTE
#undef VM_RETURN
#undef VM_PROPAGATE
#undef RECORD_ERROR

    if(depth==0&&vm->running_fiber!=nullptr&&ip>=chunk->code_count) {
        *result=DIAMOND_NIL;
        vm->frames=frame.previous;
        return DIAMOND_VM_OK;
    }
    vm->frames = frame.previous;
    return DIAMOND_VM_INVALID_BYTECODE;
}

DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result) {
    for (size_t index=0;index<vm->rewritten_site_count;index++)
        ((uint8_t *)(void *)vm->rewritten_sites[index])[0]=(uint8_t)DIAMOND_OP_INVOKE;
    vm->rewritten_site_count=0;
    vm->error[0]='\0';
    vm->has_exception=false;
    diamond_vm_invalidate_method_caches(vm);
    memset(vm->field_caches,0,sizeof(vm->field_caches));
    vm->direct_dispatch_rewrites=0;
    vm->field_cache_hits=0;
    vm->field_cache_misses=0;
    vm->shape_transitions=0;
    vm->quickened_sites=0;
    vm->deoptimized_sites=0;
    return run_chunk(chunk, vm, nullptr, 0, 0, nullptr, result);
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
        case DIAMOND_VM_EXCEPTION:
            return "uncaught exception";
        case DIAMOND_VM_YIELDED:
            return "yielded";
        case DIAMOND_VM_YIELD_WITHOUT_FIBER:
            return "yield outside a fiber";
        case DIAMOND_VM_FIBER_NOT_RESUMABLE:
            return "fiber is not resumable";
        case DIAMOND_VM_IO_ERROR:
            return "I/O error";
        case DIAMOND_VM_REGEXP_ERROR:
            return "regexp error";
        case DIAMOND_VM_PROGRAM_ERROR:
            return "constructed program failed";
    }
    return "unknown VM status";
}
