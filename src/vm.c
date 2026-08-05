#include "vm.h"

#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DIAMOND_REGISTER_COUNT = 256 };
enum { DIAMOND_MAX_CALL_DEPTH = 256 };

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
    } else if(object->kind==DIAMOND_OBJECT_CLOSURE) {
        DiamondClosure *closure=(DiamondClosure *)object;
        for(size_t i=0;i<closure->capture_count;i++)mark_value(closure->captures[i]);
    } else if(object->kind==DIAMOND_OBJECT_CELL) {
        mark_value(((DiamondCell *)object)->value);
    }
}

static void mark_value(DiamondValue value) {
    if (value.kind == DIAMOND_VALUE_OBJECT) mark_object(value.as.object);
}

void diamond_vm_collect(DiamondVm *vm) {
    if(vm->has_exception)mark_value(vm->exception);
    for (DiamondFrame *frame = vm->frames; frame != nullptr;
         frame = frame->previous) {
        for (size_t index = 0; index < DIAMOND_REGISTER_COUNT; index++) {
            mark_value(frame->registers[index]);
        }
        if(frame->pending!=nullptr && frame->pending->kind!=PENDING_NONE)
            mark_value(frame->pending->value);
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
            DiamondArray *array=(DiamondArray *)unreached;
            size=sizeof(DiamondArray)+array->capacity*sizeof(DiamondValue);
            free(array->values);
        } else if(unreached->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)unreached;
            size=sizeof(DiamondHash)+hash->capacity*sizeof(DiamondHashEntry);
            free(hash->entries);
        } else if(unreached->kind==DIAMOND_OBJECT_CLOSURE) {
            size=sizeof(DiamondClosure);
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
}

void diamond_vm_free(DiamondVm *vm) {
    DiamondObject *object = vm->objects;
    while (object != nullptr) {
        DiamondObject *next = object->next;
        if(object->kind==DIAMOND_OBJECT_HASH)
            free(((DiamondHash *)object)->entries);
        else if(object->kind==DIAMOND_OBJECT_ARRAY)
            free(((DiamondArray *)object)->values);
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

static const DiamondMethod *lookup_method_cached(
    DiamondVm *vm, const DiamondChunk *chunk, const uint8_t *site,
    const DiamondClass *class, const char *name, size_t length) {
    const size_t slot=((size_t)(uintptr_t)site>>2)%DIAMOND_INLINE_CACHE_COUNT;
    DiamondMethodCache *cache=&vm->method_caches[slot];
    if(cache->site!=site) {
        *cache=(DiamondMethodCache){.site=site};
    } else {
        for(size_t index=0;index<cache->entry_count;index++) {
            if(cache->entries[index].receiver_class!=class)continue;
            vm->inline_cache_hits++;
            return cache->entries[index].method;
        }
    }
    vm->inline_cache_misses++;
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

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index);

static bool value_matches_type(const DiamondChunk *chunk, DiamondValue value,
                               uint8_t type) {
    if(type==DIAMOND_TYPE_INT) return value.kind==DIAMOND_VALUE_INT;
    if(type==DIAMOND_TYPE_BOOL) return value.kind==DIAMOND_VALUE_BOOL;
    if(type==DIAMOND_TYPE_NIL) return value.kind==DIAMOND_VALUE_NIL;
    if(type==DIAMOND_TYPE_STRING) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_STRING;
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
                const bool length=strcmp(method->name,"length")==0&&method->arity==0;
                const bool array_method=builtin==DIAMOND_TYPE_ARRAY&&
                    ((strcmp(method->name,"push")==0&&method->arity==1)||
                     (strcmp(method->name,"pop")==0&&method->arity==0));
                const bool hash_method=builtin==DIAMOND_TYPE_HASH&&method->arity==1&&
                    (strcmp(method->name,"key_at")==0||
                     strcmp(method->name,"value_at")==0);
                if(!length&&!array_method&&!hash_method)return false;
                if(method->return_type_set!=UINT8_MAX) {
                    uint8_t result=UINT8_MAX;
                    if(length)result=DIAMOND_TYPE_INT;
                    else if(builtin==DIAMOND_TYPE_ARRAY&&
                            strcmp(method->name,"push")==0)result=DIAMOND_TYPE_ARRAY;
                    if(result==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=result,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,chunk->type_sets,
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
                                !runtime_set_satisfies(chunk,chunk->type_sets,
                                    required_set,implementation->type_sets,
                                    actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,chunk->type_sets,
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
                const bool length=strcmp(method->name,"length")==0&&method->arity==0;
                const bool array_method=known==DIAMOND_TYPE_ARRAY&&
                    ((strcmp(method->name,"push")==0&&method->arity==1)||
                     (strcmp(method->name,"pop")==0&&method->arity==0));
                const bool hash_method=known==DIAMOND_TYPE_HASH&&method->arity==1&&
                    (strcmp(method->name,"key_at")==0||
                     strcmp(method->name,"value_at")==0);
                if(!length&&!array_method&&!hash_method)return false;
                if(method->return_type_set!=UINT8_MAX) {
                    uint8_t result=UINT8_MAX;
                    if(length)result=DIAMOND_TYPE_INT;
                    else if(known==DIAMOND_TYPE_ARRAY&&
                            strcmp(method->name,"push")==0)result=DIAMOND_TYPE_ARRAY;
                    if(result==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=result,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,chunk->type_sets,
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
                                !runtime_set_satisfies(chunk,chunk->type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,chunk->type_sets,
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
            .interface_count=chunk->interface_count};
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
            .interface_count=constraint->interface_count};
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
            .interface_count=constraint->interface_count};
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
        default: return UINT8_MAX;
    }
}

static bool catch_runtime_error(DiamondVm *vm,const DiamondChunk *chunk,
                                DiamondVmStatus status,UnwindHandler *handlers,
                                size_t *handler_count,PendingUnwind *pending,
                                DiamondValue *registers,size_t *ip) {
    const uint8_t class_index=exception_class_for_status(status);
    if(class_index==UINT8_MAX || (size_t)class_index>=chunk->class_count)return false;
    DiamondInstance *exception=allocate_instance(vm,&chunk->classes[class_index]);
    if(exception==nullptr)return false;
    vm->exception=DIAMOND_OBJECT(exception);vm->has_exception=true;
    (void)snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                   exception->class->name);
    return catch_exception(vm,chunk,handlers,handler_count,pending,registers,ip);
}

static const char *type_name(const DiamondChunk *chunk,uint8_t type) {
    const char *name="<invalid type>";
    if(type==DIAMOND_TYPE_INT) name="Int";
    else if(type==DIAMOND_TYPE_STRING) name="String";
    else if(type==DIAMOND_TYPE_BOOL) name="Bool";
    else if(type==DIAMOND_TYPE_NIL) name="Nil";
    else if(type==DIAMOND_TYPE_ARRAY) name="Array";
    else if(type==DIAMOND_TYPE_HASH) name="Hash";
    else if(type==DIAMOND_TYPE_CALLABLE) name="Callable";
    else if(type==DIAMOND_TYPE_SIZED) name="Sized";
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
            const int arity=snprintf(buffer+used,capacity-used,"[%u",
                set->members[index].callable_arity);
            if(arity<0)return;
            used+=(size_t)arity;
            if(set->members[index].callable_return_set!=UINT8_MAX&&used<capacity) {
                char returns[80];
                format_type_set_index(returns,sizeof returns,chunk,
                    set->members[index].callable_return_set);
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
    else if(value.as.object->kind==DIAMOND_OBJECT_STRING) name="String";
    else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY) name="Array";
    else if(value.as.object->kind==DIAMOND_OBJECT_HASH) name="Hash";
    else if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE) name="Callable";
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
    const DiamondObject *object=value.as.object;
    if(object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)object;
        return builder_append(builder,string->chars,string->length);
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

static DiamondVmStatus run_chunk(const DiamondChunk *chunk,
                                 DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
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
    PendingUnwind pending={};
    DiamondFrame frame = {
        .previous = vm->frames,
        .registers = registers,
        .pending = &pending,
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
            case DIAMOND_OP_ARGUMENT_PROVIDED: {
                uint8_t destination=0,index=0;
                READ_BYTE(destination);READ_BYTE(index);
                registers[destination]=DIAMOND_BOOL(index<argument_count);
                break;
            }
            case DIAMOND_OP_TO_STRING: {
                uint8_t destination=0,source=0;
                READ_BYTE(destination);READ_BYTE(source);
                if(registers[source].kind==DIAMOND_VALUE_OBJECT&&
                   registers[source].as.object->kind==DIAMOND_OBJECT_STRING) {
                    registers[destination]=registers[source];break;
                }
                if(registers[source].kind==DIAMOND_VALUE_OBJECT&&
                   registers[source].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    const DiamondInstance *instance=
                        (const DiamondInstance *)registers[source].as.object;
                    const DiamondMethod *method=lookup_method(chunk,instance->class,
                        "to_s",sizeof("to_s")-1);
                    if(method!=nullptr) {
                        if(method->required_arity>0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const DiamondFunction *fn=&chunk->functions[method->function_index];
                        const DiamondChunk child={.name=fn->name,.code=fn->code,
                          .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                          .constants=fn->constants,.constant_count=fn->constant_count,
                          .strings=fn->strings,.string_count=fn->string_count,
                          .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                          .functions=chunk->functions,.function_count=chunk->function_count,
                          .classes=chunk->classes,.class_count=chunk->class_count,
                          .interfaces=chunk->interfaces,.interface_count=chunk->interface_count};
                        DiamondValue converted=DIAMOND_NIL;
                        const DiamondValue argument=registers[source];
                        DiamondVmStatus status=run_chunk(&child,vm,&argument,1,
                            depth+1,nullptr,&converted);
                        VM_PROPAGATE(status);
                        if(converted.kind!=DIAMOND_VALUE_OBJECT||
                           converted.as.object->kind!=DIAMOND_OBJECT_STRING) {
                            snprintf(vm->error,sizeof vm->error,
                                     "to_s must return String");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        registers[destination]=converted;break;
                    }
                }
                StringBuilder builder={};
                if(!builder_format_value(&builder,registers[source])) {
                    free(builder.chars);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
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
                };
                DiamondValue call_result = DIAMOND_NIL;
                const DiamondVmStatus status = run_chunk(
                    &called_chunk, vm, &registers[argument_base],
                    call_argument_count, depth + 1, nullptr, &call_result);
                VM_PROPAGATE(status);
                registers[destination] = call_result;
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
                uint8_t reg=0;READ_BYTE(reg);DiamondCell *cell=allocate_cell(vm,registers[reg]);
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
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count};
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
                      .interfaces=chunk->interfaces,.interface_count=chunk->interface_count};
                    DiamondValue ignored=DIAMOND_NIL;
                    DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,nullptr,&ignored);
                    VM_PROPAGATE(s);
                } else if(argc!=0) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                break;
            }
            case DIAMOND_OP_INVOKE: {
                uint8_t dest=0,recv=0,name=0,base=0,argc=0;
                READ_BYTE(dest);READ_BYTE(recv);READ_BYTE(name);READ_BYTE(base);READ_BYTE(argc);
                if(argc>16) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   (size_t)name>=chunk->string_count) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondStringConstant *method_name=&chunk->strings[name];
                const DiamondObjectKind receiver_kind=registers[recv].as.object->kind;
                if(receiver_kind==DIAMOND_OBJECT_ARRAY||
                   receiver_kind==DIAMOND_OBJECT_HASH||
                   receiver_kind==DIAMOND_OBJECT_STRING) {
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
                    if(receiver_kind==DIAMOND_OBJECT_HASH) {
                        const bool key_method=method_name->length==6&&
                            memcmp(method_name->chars,"key_at",6)==0;
                        const bool value_method=method_name->length==8&&
                            memcmp(method_name->chars,"value_at",8)==0;
                        if(!key_method&&!value_method)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
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
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(receiver_kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const DiamondMethod *method=lookup_method_cached(
                    vm,chunk,chunk->code+instruction_offset,instance->class,
                    method_name->chars,method_name->length);
                if(method==nullptr) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(argc<method->required_arity||argc>method->arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17];args[0]=registers[recv];
                for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=&chunk->functions[method->function_index];
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count,
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count};
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
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count};
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
                else if(vm->exception.as.object->kind==DIAMOND_OBJECT_STRING) {
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

    vm->frames = frame.previous;
    return DIAMOND_VM_INVALID_BYTECODE;
}

DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result) {
    vm->error[0]='\0';
    vm->has_exception=false;
    memset(vm->method_caches,0,sizeof(vm->method_caches));
    memset(vm->field_caches,0,sizeof(vm->field_caches));
    vm->inline_cache_hits=0;
    vm->inline_cache_misses=0;
    vm->field_cache_hits=0;
    vm->field_cache_misses=0;
    vm->shape_transitions=0;
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
    }
    return "unknown VM status";
}
