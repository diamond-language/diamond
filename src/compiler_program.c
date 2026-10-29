#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiler_internal.h"
#include "jit.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* DiamondProgram and DiamondFunction storage: allocation, growth, initialisation and teardown. */

DiamondFunction *diamond_program_add_function(DiamondProgram *program) {
    if(program->function_count>=DIAMOND_MAX_FUNCTIONS)return nullptr;
    if(program->function_count==program->function_capacity) {
        size_t capacity=program->function_capacity==0?64:
            program->function_capacity*2;
        if(capacity>DIAMOND_MAX_FUNCTIONS)capacity=DIAMOND_MAX_FUNCTIONS;
        DiamondFunction **functions=realloc(program->functions,
            capacity*sizeof *functions);
        if(functions==nullptr)return nullptr;
        program->functions=functions;
        program->function_capacity=capacity;
    }
    DiamondFunction *function=calloc(1,sizeof *function);
    if(function==nullptr)return nullptr;
    program->functions[program->function_count++]=function;
    return function;
}

bool diamond_function_reserve_code(DiamondFunction *function,size_t capacity) {
    /* See DiamondFunction.owns_combined_buffer's own comment (src/vm.h):
     * a combined-allocated function's code/lines/columns are interior
     * pointers into one shared block, never independently reallocable. */
    assert(!function->owns_combined_buffer);
    if(capacity<=function->code_capacity)return true;
    if(capacity>DIAMOND_MAX_CODE)return false;
    uint8_t *code=malloc(capacity*sizeof *code);
    uint32_t *lines=calloc(capacity,sizeof *lines);
    uint32_t *columns=calloc(capacity,sizeof *columns);
    if(code==nullptr||lines==nullptr||columns==nullptr) {
        free(code);free(lines);free(columns);return false;
    }
    if(function->code_count>0) {
        memcpy(code,function->code,function->code_count*sizeof *code);
        memcpy(lines,function->lines,function->code_count*sizeof *lines);
        memcpy(columns,function->columns,function->code_count*sizeof *columns);
    }
    free(function->code);free(function->lines);free(function->columns);
    function->code=code;function->lines=lines;function->columns=columns;
    function->code_capacity=capacity;
    return true;
}

bool diamond_function_reserve_constants(DiamondFunction *function,
                                         size_t capacity) {
    assert(!function->owns_combined_buffer);
    if(capacity<=function->constant_capacity)return true;
    if(capacity>DIAMOND_MAX_CONSTANTS)return false;
    DiamondValue *constants=realloc(function->constants,
        capacity*sizeof *constants);
    if(constants==nullptr)return false;
    function->constants=constants;function->constant_capacity=capacity;
    return true;
}

bool diamond_function_reserve_strings(DiamondFunction *function,size_t capacity) {
    assert(!function->owns_combined_buffer);
    if(capacity<=function->string_capacity)return true;
    if(capacity>DIAMOND_MAX_STRING_CONSTANTS)return false;
    const size_t previous_capacity=function->string_capacity;
    DiamondStringConstant *strings=realloc(function->strings,
        capacity*sizeof *strings);
    if(strings==nullptr)return false;
    memset(strings+previous_capacity,0,
        (capacity-previous_capacity)*sizeof *strings);
    function->strings=strings;function->string_capacity=capacity;
    return true;
}

bool diamond_function_reserve_type_sets(DiamondFunction *function,size_t capacity) {
    assert(!function->owns_combined_buffer);
    if(capacity<=function->type_set_capacity)return true;
    if(capacity>DIAMOND_MAX_TYPE_SETS)return false;
    const size_t previous_capacity=function->type_set_capacity;
    DiamondTypeSet *type_sets=realloc(function->type_sets,
        capacity*sizeof *type_sets);
    if(type_sets==nullptr)return false;
    memset(type_sets+previous_capacity,0,
        (capacity-previous_capacity)*sizeof *type_sets);
    function->type_sets=type_sets;function->type_set_capacity=capacity;
    return true;
}

/* Rounds `offset` up to `alignof(max_align_t)` -- diamond_function_copy's
 * own combined-buffer sub-arrays (code/lines/columns/constants/strings/
 * type_sets, each a different element type/alignment) each start at
 * such a boundary, the same margin malloc itself already guarantees for
 * the block's own base address. A few bytes of padding per boundary
 * (at most 5 boundaries) is nothing next to what this buys: one malloc
 * instead of 6 per function copied. */
static size_t align_up_max(size_t offset) {
    const size_t alignment=alignof(max_align_t);
    return (offset+alignment-1)&~(alignment-1);
}

/* Deep-copies `source` into `destination` (already-valid contents
 * overwritten, not freed -- every caller passes a fresh/zeroed
 * destination). Every dynamic array (code/lines/columns/constants/
 * strings/type_sets) is copied into ONE combined malloc'd block instead
 * of 6 separate ones, with `destination->code` as that block's real
 * base pointer and the other 5 fields as interior pointers into the
 * same allocation (see DiamondFunction.owns_combined_buffer's own
 * comment, src/vm.h, for why this is safe and what it costs: the result
 * can never be independently regrown, only freed as a whole). This
 * matters wherever many functions get cloned per call --
 * seed_program_from_template, clone_program_from_chunk (Thread.new),
 * diamond_program_read_compiled -- since 1 malloc instead of 6 per
 * function is the difference between a few hundred and a few thousand
 * allocations for a prelude-sized template; measured directly against a
 * cold process, not assumed. */
bool diamond_function_copy(DiamondFunction *destination,
                           const DiamondFunction *source) {
    *destination=*source;
    destination->code=nullptr;destination->lines=nullptr;
    destination->columns=nullptr;destination->code_capacity=0;
    destination->constants=nullptr;destination->constant_capacity=0;
    destination->strings=nullptr;destination->string_capacity=0;
    destination->type_sets=nullptr;destination->type_set_capacity=0;
    destination->owns_combined_buffer=false;
    /* jit_code is executable memory tied to the SOURCE's own lifetime/
     * process, never shared across a Thread.new/gremlin_serve clone --
     * see docs/internal/jit-design.md's threading section. The clone
     * starts back at "not yet compiled," exactly like a brand-new
     * function; it will re-warm up and (if hot enough) re-compile
     * independently in its own thread. */
    destination->jit_code=nullptr;destination->jit_code_size=0;
    destination->jit_call_count=0;destination->jit_ineligible=false;

    size_t code_offset=0,lines_offset=0,columns_offset=0;
    size_t constants_offset=0,strings_offset=0,type_sets_offset=0;
    size_t size=0;
    if(source->code_count>0) {
        code_offset=size;size+=source->code_count*sizeof *destination->code;
        size=align_up_max(size);
        lines_offset=size;size+=source->code_count*sizeof *destination->lines;
        size=align_up_max(size);
        columns_offset=size;size+=source->code_count*sizeof *destination->columns;
        size=align_up_max(size);
    }
    if(source->constant_count>0) {
        constants_offset=size;
        size+=source->constant_count*sizeof *destination->constants;
        size=align_up_max(size);
    }
    if(source->string_count>0) {
        strings_offset=size;
        size+=source->string_count*sizeof *destination->strings;
        size=align_up_max(size);
    }
    if(source->type_set_count>0) {
        type_sets_offset=size;
        size+=source->type_set_count*sizeof *destination->type_sets;
    }
    if(size==0)return true;

    uint8_t *block=malloc(size);
    if(block==nullptr)return false;

    if(source->code_count>0) {
        destination->code=block+code_offset;
        destination->lines=(uint32_t *)(void *)(block+lines_offset);
        destination->columns=(uint32_t *)(void *)(block+columns_offset);
        memcpy(destination->code,source->code,
            source->code_count*sizeof *destination->code);
        /* source->lines/columns/constants/strings/type_sets may be
         * misaligned views into a raw serialized buffer (see
         * compiled_prelude.c's read_function) -- cast to void* so
         * memcpy's copy doesn't get treated as a typed, alignment-
         * requiring access by -fsanitize=alignment. memcpy itself never
         * needs the alignment; only the C pointer *type* does. */
        memcpy((void *)destination->lines,(const void *)source->lines,
            source->code_count*sizeof *destination->lines);
        memcpy((void *)destination->columns,(const void *)source->columns,
            source->code_count*sizeof *destination->columns);
        destination->code_capacity=source->code_count;
    }
    if(source->constant_count>0) {
        destination->constants=(DiamondValue *)(void *)(block+constants_offset);
        memcpy((void *)destination->constants,(const void *)source->constants,
            source->constant_count*sizeof *destination->constants);
        destination->constant_capacity=source->constant_count;
    }
    if(source->string_count>0) {
        destination->strings=(DiamondStringConstant *)(void *)(block+strings_offset);
        memcpy((void *)destination->strings,(const void *)source->strings,
            source->string_count*sizeof *destination->strings);
        destination->string_capacity=source->string_count;
    }
    if(source->type_set_count>0) {
        destination->type_sets=(DiamondTypeSet *)(void *)(block+type_sets_offset);
        memcpy((void *)destination->type_sets,(const void *)source->type_sets,
            source->type_set_count*sizeof *destination->type_sets);
        destination->type_set_capacity=source->type_set_count;
    }
    destination->owns_combined_buffer=true;
    return true;
}

/* Frees `function`'s own dynamic arrays (but not `function` itself --
 * callers own that separately, see diamond_program_free below). A
 * combined-allocated function (diamond_function_copy's own
 * owns_combined_buffer, see its comment in src/vm.h) has only one real
 * allocation, `code`; the other 5 fields are interior pointers into
 * that same block and must never be passed to free() themselves. */
void diamond_function_free_arrays(DiamondFunction *function) {
    if(function->owns_combined_buffer) {
        free(function->code);
    } else {
        free(function->code);free(function->lines);free(function->columns);
        free(function->constants);
        free(function->strings);
        free(function->type_sets);
    }
    function->code=nullptr;function->lines=nullptr;
    function->columns=nullptr;function->code_count=0;
    function->code_capacity=0;
    function->constants=nullptr;function->constant_count=0;
    function->constant_capacity=0;
    function->strings=nullptr;function->string_count=0;
    function->string_capacity=0;
    function->type_sets=nullptr;function->type_set_count=0;
    function->type_set_capacity=0;
    function->owns_combined_buffer=false;
    /* jit_code is executable memory (mmap), never one of the arrays above --
     * see jit.h/docs/internal/jit-design.md. */
    diamond_jit_free(function->jit_code,function->jit_code_size);
    function->jit_code=nullptr;function->jit_code_size=0;
    function->jit_call_count=0;function->jit_ineligible=false;
}

DiamondClass *diamond_program_class_slot(DiamondProgram *program,size_t index) {
    if(index>=DIAMOND_MAX_CLASSES)return nullptr;
    if(program->classes[index]==nullptr) {
        DiamondClass *class=calloc(1,sizeof *class);
        if(class==nullptr)return nullptr;
        class->index=(uint16_t)index;
        program->classes[index]=class;
    }
    return program->classes[index];
}

/* Grows `items` (allocated length *capacity) to hold `count` entries of
 * `item_size` bytes, at most `limit`, zeroing the new entries. Sets *ok
 * and returns the possibly moved table; on failure returns `items`
 * unchanged with *ok false. */
static void *grow_table(void *items,size_t *capacity,size_t count,size_t item_size,
                        size_t limit,bool *ok) {
    *ok=true;
    if(count<=*capacity)return items;
    *ok=false;
    if(count>limit)return items;
    size_t grown=*capacity<8?8:*capacity*2;
    if(grown<count)grown=count;
    if(grown>limit)grown=limit;
    unsigned char *table=realloc(items,grown*item_size);
    if(table==nullptr)return items;
    memset(table+*capacity*item_size,0,(grown-*capacity)*item_size);
    *capacity=grown;
    *ok=true;
    return table;
}

/* A fresh copy of the first `count` entries, or nullptr when count is 0
 * (or on allocation failure, which the caller tells apart by count). */
static void *duplicate_table(const void *items,size_t count,size_t item_size) {
    if(count==0)return nullptr;
    void *copy=malloc(count*item_size);
    if(copy!=nullptr)memcpy(copy,items,count*item_size);
    return copy;
}

bool diamond_reserve_methods(DiamondMethod **methods,size_t *capacity,size_t count) {
    bool ok;
    DiamondMethod *table=grow_table(*methods,capacity,count,sizeof **methods,
        DIAMOND_MAX_METHODS,&ok);
    *methods=table;
    return ok;
}

bool diamond_reserve_interface_methods(DiamondInterfaceMethod **methods,size_t *capacity,
                                       size_t count) {
    bool ok;
    DiamondInterfaceMethod *table=grow_table(*methods,capacity,count,sizeof **methods,
        DIAMOND_MAX_METHODS,&ok);
    *methods=table;
    return ok;
}

bool diamond_reserve_discovered_methods(DiamondClass *class,size_t count) {
    size_t capacity=class->discovered_method_capacity;
    bool ok;
    uint16_t *table=grow_table(class->discovered_methods,&capacity,count,
        sizeof *class->discovered_methods,DIAMOND_MAX_METHODS,&ok);
    class->discovered_methods=table;
    class->discovered_method_capacity=(uint16_t)capacity;
    return ok;
}

bool diamond_class_reserve_fields(DiamondClass *class,size_t count) {
    if(count<=class->field_capacity)return true;
    /* Each table grows from the same old length; the shared capacity
     * moves only once all five have room. */
    size_t capacity=class->field_capacity;
    bool ok;
    class->fields=grow_table(class->fields,&capacity,count,sizeof *class->fields,
        DIAMOND_MAX_FIELDS,&ok);
    if(!ok)return false;
    const size_t grown=capacity;
    capacity=class->field_capacity;
    class->field_type_status=grow_table(class->field_type_status,&capacity,grown,
        sizeof *class->field_type_status,DIAMOND_MAX_FIELDS,&ok);
    if(!ok)return false;
    capacity=class->field_capacity;
    class->field_known_class=grow_table(class->field_known_class,&capacity,grown,
        sizeof *class->field_known_class,DIAMOND_MAX_FIELDS,&ok);
    if(!ok)return false;
    capacity=class->field_capacity;
    class->discovered_field_type_status=grow_table(class->discovered_field_type_status,
        &capacity,grown,sizeof *class->discovered_field_type_status,DIAMOND_MAX_FIELDS,&ok);
    if(!ok)return false;
    capacity=class->field_capacity;
    class->discovered_field_known_class=grow_table(class->discovered_field_known_class,
        &capacity,grown,sizeof *class->discovered_field_known_class,DIAMOND_MAX_FIELDS,&ok);
    if(!ok)return false;
    class->field_capacity=grown;
    return true;
}

bool diamond_module_reserve_fields(DiamondModule *module,size_t count) {
    bool ok;
    module->fields=grow_table(module->fields,&module->field_capacity,count,
        sizeof *module->fields,DIAMOND_MAX_FIELDS,&ok);
    return ok;
}

bool diamond_class_reserve_class_variables(DiamondClass *class,size_t count) {
    bool ok;
    class->class_variables=grow_table(class->class_variables,
        &class->class_variable_capacity,count,sizeof *class->class_variables,
        DIAMOND_MAX_CLASS_VARIABLES,&ok);
    return ok;
}

/* diamond_reserve_methods for a class whose program is already running
 * (ClassName.define_method): the outgrown table moves to retired_methods
 * instead of being freed, so a DiamondMethod pointer a call in flight
 * still holds stays valid. Callers invalidate the method caches. */
bool diamond_class_grow_methods_while_running(DiamondClass *class,size_t count) {
    if(count<=class->method_capacity)return true;
    DiamondMethod **retired=realloc(class->retired_methods,
        (class->retired_method_count+1)*sizeof *retired);
    if(retired==nullptr)return false;
    class->retired_methods=retired;
    size_t wanted=class->method_capacity*2;
    if(wanted>DIAMOND_MAX_METHODS)wanted=DIAMOND_MAX_METHODS;
    if(wanted<count)wanted=count;
    DiamondMethod *grown=nullptr;
    size_t capacity=0;
    if(!diamond_reserve_methods(&grown,&capacity,wanted))return false;
    if(class->method_count>0)
        memcpy(grown,class->methods,class->method_count*sizeof *grown);
    if(class->methods!=nullptr)
        retired[class->retired_method_count++]=class->methods;
    class->methods=grown;
    class->method_capacity=capacity;
    return true;
}

void diamond_class_free_tables(DiamondClass *class) {
    free(class->methods);
    free(class->singleton_methods);
    free(class->discovered_methods);
    free(class->fields);
    free(class->field_type_status);
    free(class->field_known_class);
    free(class->discovered_field_type_status);
    free(class->discovered_field_known_class);
    free(class->class_variables);
    for(size_t index=0;index<class->retired_method_count;index++)
        free(class->retired_methods[index]);
    free(class->retired_methods);
    class->methods=nullptr;class->method_count=0;class->method_capacity=0;
    class->singleton_methods=nullptr;class->singleton_method_count=0;
    class->singleton_method_capacity=0;
    class->discovered_methods=nullptr;class->discovered_method_count=0;
    class->discovered_method_capacity=0;
    class->retired_methods=nullptr;class->retired_method_count=0;
    class->fields=nullptr;class->field_type_status=nullptr;class->field_known_class=nullptr;
    class->discovered_field_type_status=nullptr;class->discovered_field_known_class=nullptr;
    class->field_count=0;class->field_capacity=0;
    class->class_variables=nullptr;class->class_variable_count=0;
    class->class_variable_capacity=0;
}

void diamond_module_free_tables(DiamondModule *module) {
    free(module->methods);
    free(module->singleton_methods);
    free(module->fields);
    module->fields=nullptr;module->field_count=0;module->field_capacity=0;
    module->methods=nullptr;module->method_count=0;module->method_capacity=0;
    module->singleton_methods=nullptr;module->singleton_method_count=0;
    module->singleton_method_capacity=0;
}

void diamond_interface_free_tables(DiamondInterface *interface) {
    free(interface->methods);
    interface->methods=nullptr;interface->method_count=0;interface->method_capacity=0;
}

/* Replaces `destination`'s contents (releasing its own tables) with a deep
 * copy of `source`. Copied tables hold exactly the used entries. False on
 * allocation failure, leaving `destination` with empty tables. */
/* Target slot i reads actual slot order[i]. Each supported edge is a
 * permutation of at most two slots; composition is therefore its own inverse. */
bool diamond_class_argument_order(DiamondClass *const *classes,size_t count,
        size_t actual,size_t owner,uint8_t order[2]) {
    order[0]=0;order[1]=1;
    if(actual>=count||owner>=count)return false;
    for(size_t step=0;step<count;step++) {
        if(actual==owner)return true;
        const DiamondClass *class=classes[actual];
        if(class->transforms_parent_arguments)return false;
        if(class->swaps_parent_arguments) {
            if(class->fixed_type_arguments||DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)!=2||
               class->superclass>=count||classes[class->superclass]->fixed_type_arguments||
               DIAMOND_CLASS_TYPE_PARAMETER_COUNT(classes[class->superclass])!=2)return false;
            const uint8_t first=order[0];order[0]=order[1];order[1]=first;
        }
        actual=class->superclass;
        if(actual==DIAMOND_NO_CLASS||actual>=count)return false;
    }
    return false;
}

bool diamond_align_type_member(DiamondClass *const *classes,size_t count,
        uint16_t target,DiamondTypeMember *actual) {
    if(actual->id==target)return true;
    if(actual->id<DIAMOND_TYPE_CLASS_BASE||actual->id>=DIAMOND_TYPE_CLASS_LIMIT||
       target<DIAMOND_TYPE_CLASS_BASE||target>=DIAMOND_TYPE_CLASS_LIMIT)return false;
    uint8_t order[2];
    if(!diamond_class_argument_order(classes,count,actual->id-DIAMOND_TYPE_CLASS_BASE,
        target-DIAMOND_TYPE_CLASS_BASE,order)&&
       !diamond_class_argument_order(classes,count,target-DIAMOND_TYPE_CLASS_BASE,
        actual->id-DIAMOND_TYPE_CLASS_BASE,order))return false;
    if(order[0]==1) {
        const uint16_t first=actual->argument_set;
        actual->argument_set=actual->second_argument_set;actual->second_argument_set=first;
    }
    return true;
}

bool diamond_class_copy(DiamondClass *destination,const DiamondClass *source) {
    diamond_class_free_tables(destination);
    memcpy(destination,source,sizeof *destination);
    destination->methods=duplicate_table(source->methods,source->method_count,
        sizeof *source->methods);
    destination->method_capacity=source->method_count;
    destination->singleton_methods=duplicate_table(source->singleton_methods,
        source->singleton_method_count,sizeof *source->singleton_methods);
    destination->singleton_method_capacity=source->singleton_method_count;
    destination->discovered_methods=duplicate_table(source->discovered_methods,
        source->discovered_method_count,sizeof *source->discovered_methods);
    destination->discovered_method_capacity=source->discovered_method_count;
    destination->retired_methods=nullptr;destination->retired_method_count=0;
    const size_t fields=source->field_count;
    destination->fields=duplicate_table(source->fields,fields,sizeof *source->fields);
    destination->field_type_status=duplicate_table(source->field_type_status,fields,
        sizeof *source->field_type_status);
    destination->field_known_class=duplicate_table(source->field_known_class,fields,
        sizeof *source->field_known_class);
    destination->discovered_field_type_status=duplicate_table(
        source->discovered_field_type_status,fields,
        sizeof *source->discovered_field_type_status);
    destination->discovered_field_known_class=duplicate_table(
        source->discovered_field_known_class,fields,
        sizeof *source->discovered_field_known_class);
    destination->field_capacity=fields;
    destination->class_variables=duplicate_table(source->class_variables,
        source->class_variable_count,sizeof *source->class_variables);
    destination->class_variable_capacity=source->class_variable_count;
    if((source->method_count>0&&destination->methods==nullptr)||
       (source->singleton_method_count>0&&destination->singleton_methods==nullptr)||
       (source->discovered_method_count>0&&destination->discovered_methods==nullptr)||
       (fields>0&&(destination->fields==nullptr||destination->field_type_status==nullptr||
                   destination->field_known_class==nullptr||
                   destination->discovered_field_type_status==nullptr||
                   destination->discovered_field_known_class==nullptr))||
       (source->class_variable_count>0&&destination->class_variables==nullptr)) {
        diamond_class_free_tables(destination);
        return false;
    }
    return true;
}

bool diamond_module_copy(DiamondModule *destination,const DiamondModule *source) {
    diamond_module_free_tables(destination);
    memcpy(destination,source,sizeof *destination);
    destination->methods=duplicate_table(source->methods,source->method_count,
        sizeof *source->methods);
    destination->method_capacity=source->method_count;
    destination->singleton_methods=duplicate_table(source->singleton_methods,
        source->singleton_method_count,sizeof *source->singleton_methods);
    destination->singleton_method_capacity=source->singleton_method_count;
    destination->fields=duplicate_table(source->fields,source->field_count,
        sizeof *source->fields);
    destination->field_capacity=source->field_count;
    if((source->method_count>0&&destination->methods==nullptr)||
       (source->singleton_method_count>0&&destination->singleton_methods==nullptr)||
       (source->field_count>0&&destination->fields==nullptr)) {
        diamond_module_free_tables(destination);
        return false;
    }
    return true;
}

bool diamond_interface_copy(DiamondInterface *destination,const DiamondInterface *source) {
    diamond_interface_free_tables(destination);
    *destination=*source;
    destination->methods=duplicate_table(source->methods,source->method_count,
        sizeof *source->methods);
    destination->method_capacity=source->method_count;
    if(source->method_count>0&&destination->methods==nullptr) {
        diamond_interface_free_tables(destination);
        return false;
    }
    return true;
}

/* Interfaces and modules use the same lazily allocated slots as classes:
 * zeroed when first allocated, then owned by `program` until
 * diamond_program_free. */
DiamondInterface *diamond_program_interface_slot(DiamondProgram *program,size_t index) {
    if(index>=DIAMOND_MAX_INTERFACES)return nullptr;
    if(program->interfaces[index]==nullptr)
        program->interfaces[index]=calloc(1,sizeof *program->interfaces[index]);
    return program->interfaces[index];
}

DiamondModule *diamond_program_module_slot(DiamondProgram *program,size_t index) {
    if(index>=DIAMOND_MAX_MODULES)return nullptr;
    if(program->modules[index]==nullptr)
        program->modules[index]=calloc(1,sizeof *program->modules[index]);
    return program->modules[index];
}

void diamond_program_free(DiamondProgram *program) {
    if(program==nullptr)return;
    diamond_function_free_arrays(&program->entry);
    for(size_t index=0;index<program->function_count;index++) {
        diamond_function_free_arrays(program->functions[index]);
        free(program->functions[index]);
    }
    free(program->functions);
    program->functions=nullptr;
    program->function_count=0;
    program->function_capacity=0;
    for(size_t index=0;index<DIAMOND_MAX_CLASSES;index++) {
        if(program->classes[index]!=nullptr)
            diamond_class_free_tables(program->classes[index]);
        free(program->classes[index]);
        program->classes[index]=nullptr;
    }
    program->class_count=0;
    for(size_t index=0;index<DIAMOND_MAX_INTERFACES;index++) {
        if(program->interfaces[index]!=nullptr)
            diamond_interface_free_tables(program->interfaces[index]);
        free(program->interfaces[index]);
        program->interfaces[index]=nullptr;
    }
    program->interface_count=0;
    for(size_t index=0;index<DIAMOND_MAX_MODULES;index++) {
        if(program->modules[index]!=nullptr)
            diamond_module_free_tables(program->modules[index]);
        free(program->modules[index]);
        program->modules[index]=nullptr;
    }
    program->module_count=0;
    free(program->namespace_constants);
    free(program->namespace_constant_defined);
    program->namespace_constants=nullptr;
    program->namespace_constant_defined=nullptr;
    program->namespace_constant_count=0;
    program->namespace_constant_capacity=0;
}

bool diamond_program_reserve_namespace_constants(DiamondProgram *program,size_t count) {
    if(count>DIAMOND_MAX_NAMESPACE_CONSTANTS)return false;
    if(count<=program->namespace_constant_capacity)return true;
    size_t capacity=program->namespace_constant_capacity<16?16:
        program->namespace_constant_capacity*2;
    if(capacity<count)capacity=count;
    if(capacity>DIAMOND_MAX_NAMESPACE_CONSTANTS)capacity=DIAMOND_MAX_NAMESPACE_CONSTANTS;
    char (*names)[DIAMOND_MAX_FUNCTION_NAME]=
        realloc(program->namespace_constants,capacity*sizeof *names);
    if(names==nullptr)return false;
    program->namespace_constants=names;
    bool *defined=realloc(program->namespace_constant_defined,capacity*sizeof *defined);
    if(defined==nullptr)return false;
    program->namespace_constant_defined=defined;
    const size_t old=program->namespace_constant_capacity;
    memset(names+old,0,(capacity-old)*sizeof *names);
    memset(defined+old,0,(capacity-old)*sizeof *defined);
    program->namespace_constant_capacity=capacity;
    return true;
}

/* The non-memset half of diamond_program_init: populate `program`'s
 * builtin exception classes and entry.name. Requires `program` to
 * already be all-zero -- diamond_program_init itself guarantees that
 * via its own memset just before calling this; diamond_compile_impl's
 * own `discovery` program (src/compiler.c) calls this directly instead,
 * skipping that memset, because it's always a local `calloc(1, sizeof
 * *discovery)` created fresh for exactly one compile call and never
 * reused -- calloc's own zero-fill already satisfies this function's
 * precondition, so diamond_program_init's memset would just be
 * re-zeroing memory that was already zero. Confirmed to matter, not
 * assumed: memset(program,0,sizeof *program) alone measured ~6.3ms
 * (release build, cold process) purely from the first-touch page faults
 * committing DiamondProgram's ~14.2MB, on every single diamond_compile/
 * diamond_compile_incremental call regardless of source size -- see
 * CHANGELOG.md's "Performance". Never call this
 * directly on a `program` that might carry a previous compile's
 * leftover data (tests/run_cases.c's own batch loop reuses one
 * DiamondProgram across 1285+ calls) -- only diamond_program_init
 * itself is safe there, since its memset is what actually wipes stale
 * content in that case, not redundant waste. */
/* Recomputes every class's own shapes[] (DiamondClass, src/vm.h) --
 * self-referential (`shape->class` points back at the owning class)
 * entries a fresh compile always gets for free from run_compile_pass's
 * own tail, so anything that populates program->classes[] *without*
 * running a real compile pass afterward needs to call this explicitly
 * or every shape lookup reads stale/dangling `class` pointers instead.
 * Two callers: diamond_program_init_fresh below (bootstrapping the
 * built-in exception classes with no compile pass involved at all) and
 * diamond_program_read_compiled (src/compiled_prelude.c) -- a
 * deserialized program's own classes[] array lives at a different
 * memory address than whatever program it was originally serialized
 * from, so the raw shapes[] bytes the dump carries over point at the
 * *wrong* classes[] array entirely; nothing else ever calls a compile
 * pass over an already-fully-compiled deserialized program to fix that
 * up implicitly the way diamond_compile_incremental against a template
 * already does for the prelude's own template-seeded classes. Confirmed
 * directly, not assumed: a user-defined class's own instance-variable
 * reads returned Nil after an uncorrected write_compiled/read_compiled
 * round-trip, since field access resolves through a shape whose
 * `class` pointer no longer matched the class actually being read. */
void diamond_program_recompute_shapes(DiamondProgram *program) {
    for(size_t class_index=0;class_index<program->class_count;class_index++) {
        DiamondClass *class=program->classes[class_index];
        for(size_t field_count=0;field_count<=class->field_count;field_count++) {
            class->shapes[field_count]=(DiamondShape){
                .class=class,.field_count=(uint8_t)field_count};
        }
    }
}

void diamond_program_init_fresh(DiamondProgram *program) {
    static const struct {
        const char *name;
        uint16_t superclass;
    } builtins[DIAMOND_BUILTIN_CLASS_COUNT] = {
        [DIAMOND_CLASS_EXCEPTION]={"Exception",DIAMOND_NO_CLASS},
        [DIAMOND_CLASS_STANDARD_ERROR]={"StandardError",DIAMOND_CLASS_EXCEPTION},
        [DIAMOND_CLASS_RUNTIME_ERROR]={"RuntimeError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_TYPE_ERROR]={"TypeError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_ARGUMENT_ERROR]={"ArgumentError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_INDEX_ERROR]={"IndexError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_ZERO_DIVISION_ERROR]={"ZeroDivisionError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_RANGE_ERROR]={"RangeError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_SYSTEM_STACK_ERROR]={"SystemStackError",DIAMOND_CLASS_EXCEPTION},
        [DIAMOND_CLASS_FIBER_ERROR]={"FiberError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_IO_ERROR]={"IOError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_REGEXP_ERROR]={"RegexpError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_WOULD_BLOCK_ERROR]={"WouldBlockError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_THREAD_ERROR]={"ThreadError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_SQLITE3_ERROR]={"SQLite3Error",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_POSTGRES_ERROR]={"PostgreSQLError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_MYSQL_ERROR]={"MySQLError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_NO_METHOD_ERROR]={"NoMethodError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_JSON_ERROR]={"JSONError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_SUPERVISOR_ERROR]={"SupervisorError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_SANDBOX_ERROR]={"SandboxError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_RESOURCE_LIMIT_ERROR]={"ResourceLimitError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_FROZEN_ERROR]={"FrozenError",DIAMOND_CLASS_STANDARD_ERROR},
    };
    program->range_class_index=DIAMOND_NO_CLASS;
    program->class_count=DIAMOND_BUILTIN_CLASS_COUNT;
    /* Explicit here too, not just class_count above: diamond_program_init
     * (below) no longer memsets classes[]/interfaces[]/modules[] at all
     * (see its own comment) -- these two are the only fields in that
     * skipped region besides class_count that this function doesn't
     * otherwise set unconditionally. */
    program->interface_count=0;
    program->module_count=0;
    for(size_t index=0;index<DIAMOND_BUILTIN_CLASS_COUNT;index++) {
        DiamondClass *class=diamond_program_class_slot(program,index);
        if(class==nullptr) {
            (void)fputs("diamond: out of memory allocating the built-in classes\n",stderr);
            abort();
        }
        /* A builtin class can be reopened by user code (`class Exception
         * ... end` adding a method is ordinary, allowed monkey-patching,
         * same as any other class -- compile_class's own comment on
         * "a class can always be reopened") -- so unlike the fields
         * below this loop already always overwrites unconditionally,
         * methods/singleton_methods/class_variables genuinely can be
         * non-empty here already, left over from an earlier reopen *of a
         * previous compile*, if `program` is being reused (tests/
         * run_cases.c's own batch loop) rather than freshly calloc'd.
         * Explicit full reset, matching compile_class's own reopen-
         * branch reset of a claimed slot, for the same reason: nothing
         * else ever clears a builtin class's own tables between
         * compiles now that the big memset below is gone. */
        zero_table(class->methods,class->method_capacity*sizeof class->methods[0]);
        zero_table(class->singleton_methods,class->singleton_method_capacity*sizeof class->singleton_methods[0]);
        zero_table(class->field_type_status,class->field_capacity*sizeof class->field_type_status[0]);
        zero_table(class->field_known_class,class->field_capacity*sizeof class->field_known_class[0]);
        zero_table(class->discovered_field_type_status,class->field_capacity*sizeof class->discovered_field_type_status[0]);
        zero_table(class->discovered_field_known_class,class->field_capacity*sizeof class->discovered_field_known_class[0]);
        class->discovered_method_count=0;
        zero_table(class->class_variables,class->class_variable_capacity*sizeof class->class_variables[0]);
        class->method_count=0;
        class->singleton_method_count=0;
        class->class_variable_count=0;
        class->declared_by_discovery=false;
        (void)snprintf(class->name,sizeof class->name,"%s",builtins[index].name);
        class->superclass=builtins[index].superclass;
        if(!diamond_class_reserve_fields(class,3)) {
            (void)fputs("diamond: out of memory allocating the built-in classes\n",stderr);
            abort();
        }
        class->field_count=3;
        (void)snprintf(class->fields[0],DIAMOND_MAX_FUNCTION_NAME,"message");
        (void)snprintf(class->fields[1],DIAMOND_MAX_FUNCTION_NAME,"cause");
        (void)snprintf(class->fields[2],DIAMOND_MAX_FUNCTION_NAME,"backtrace");
    }
    /* diamond_compile only computes shapes for every class (built-in and
     * user-declared) once compilation finishes -- done here too, so a
     * program that never gets that far (e.g. a ProgramBuilder that only
     * ever calls this function, never diamond_compile) still has
     * instantiable built-in exception classes from construction on, the
     * same guarantee diamond_compile itself provides. See diamond_
     * program_recompute_shapes' own comment just above. */
    diamond_program_recompute_shapes(program);
    snprintf(program->entry.name, sizeof(program->entry.name), "<main>");
}

void diamond_program_init(DiamondProgram *program) {
    /* memset rather than `*program = (DiamondProgram){};`: a compound-literal
     * assignment materializes a full temporary DiamondProgram (3MB+) on this
     * function's own stack frame regardless of where `program` itself points,
     * which is unsafe for any caller running at nontrivial stack depth (e.g.
     * a required package's manifest, compiled from inside expand()'s own
     * recursive call chain, while the top-level program's own DiamondProgram
     * is still live further up the stack in main.c's run_source).
     *
     * Two memsets, not one covering the whole struct: `classes`/
     * `interfaces`/`modules` (and the class_count/interface_count/
     * module_count fields sitting between them) are deliberately
     * skipped: these tables dominate DiamondProgram's size, and a
     * full memset measured at several milliseconds per
     * call, every single diamond_compile/diamond_compile_incremental
     * call regardless of source size (see diamond_program_init_fresh's
     * own comment and CHANGELOG.md's "Performance").
     * This is provably safe now, not merely fast: every place that
     * claims a genuinely new class/module/interface slot
     * (compile_class/compile_module/compile_interface, src/compiler.c)
     * explicitly resets that exact slot's own methods/fields/counts
     * itself, rather than relying on ambient pre-zeroed memory --
     * verified directly for each, not assumed, since `program` here
     * might be a struct tests/run_cases.c's own batch loop is reusing
     * across 1285+ compiles, not a fresh calloc. class_count/
     * interface_count/module_count themselves are reset explicitly by
     * diamond_program_init_fresh below (which also finishes resetting
     * every builtin class's own methods/fields for the identical
     * reuse-safety reason -- see its own comment). */
    memset(program, 0, offsetof(DiamondProgram, classes));
    memset(&program->namespace_constants, 0,
        sizeof *program - offsetof(DiamondProgram, namespace_constants));
    diamond_program_init_fresh(program);
}

DiamondResolvedLocation diamond_resolve_diagnostic_location(
        const char *name,const char *source,DiamondDiagnostic diagnostic,
        const DiamondSourceBundle *bundle,size_t user_offset) {
    size_t line=diagnostic.span.line;
    bool mapped_segment=false;
    size_t mapped_segment_start=0;
    size_t mapped_segment_end=0;
    size_t mapped_original_line=1;
    if(diagnostic.span.start>=user_offset) {
        const size_t offset=diagnostic.span.start-user_offset;
        for(size_t index=0;index<bundle->segment_count;index++) {
            const DiamondSourceSegment *segment=&bundle->segments[index];
            if(offset<segment->start||
               (offset>segment->end&&offset-segment->end>9))continue;
            name=segment->path;
            mapped_segment=true;
            mapped_segment_start=user_offset+segment->start;
            mapped_segment_end=user_offset+segment->end;
            mapped_original_line=segment->original_line;
            break;
        }
    }
    size_t line_start=diagnostic.span.start;
    if(mapped_segment&&line_start>=mapped_segment_end)
        line_start=mapped_segment_end;
    if(mapped_segment&&line_start>0&&source[line_start]=='\n')line_start--;
    if(mapped_segment&&source[line_start]=='#'&&line_start>0) {
        line_start--;
        while(line_start>0&&source[line_start-1]!='\n')line_start--;
    }
    while(line_start>0&&source[line_start-1]!='\n')line_start--;
    if(mapped_segment) {
        line=mapped_original_line;
        for(size_t index=mapped_segment_start;index<line_start;index++)
            if(source[index]=='\n')line++;
    }
    size_t line_end=line_start;
    while(source[line_end]!='\0'&&source[line_end]!='\n')line_end++;
    return (DiamondResolvedLocation){.path=name,.line=line,
        .column=diagnostic.span.column,.line_start=line_start,.line_end=line_end};
}

size_t diamond_resolve_source_position(const char *path,const char *combined,
        const DiamondSourceBundle *bundle,size_t user_offset,
        size_t line,size_t column) {
    for(size_t index=0;index<bundle->segment_count;index++) {
        const DiamondSourceSegment *segment=&bundle->segments[index];
        if(strcmp(segment->path,path)!=0)continue;
        const size_t segment_start=user_offset+segment->start;
        const size_t segment_end=user_offset+segment->end;
        size_t segment_line=segment->original_line;
        size_t offset=segment_start;
        while(offset<segment_end&&segment_line<line) {
            if(combined[offset]=='\n')segment_line++;
            offset++;
        }
        if(segment_line!=line)continue;
        size_t result=offset;
        for(size_t moved=1;moved<column&&result<segment_end&&
                combined[result]!='\n';moved++)
            result++;
        return result;
    }
    return SIZE_MAX;
}

size_t diamond_combined_buffer_line(const char *combined,size_t offset) {
    /* Not a flat newline count: diamond_lexer_next (src/lexer.c) resets
     * its own line counter to 0 (so the *next* newline brings it to 1)
     * whenever it scans exactly "#line 1" followed by a newline or EOF --
     * the literal marker the loader (src/loader.c's own expand, "\n#line
     * 1\n") writes immediately before every segment, including the
     * top-level user source right after the prelude, not only before a
     * `require`d file's own inlined text. Line numbers the compiler
     * actually assigns (DiamondSpan.line, chunk->lines[]) are therefore
     * per-*segment*, not a monotonic count across the whole combined
     * buffer -- this walk has to recognize the identical marker the same
     * way, or it disagrees with what DiamondSpan.line/chunk->lines[]
     * will actually read for this exact byte position (and, downstream,
     * whatever line a live setBreakpoints command names). Matches the
     * lexer's own detection exactly: the marker
     * fires regardless of what precedes it (the lexer's check runs
     * unconditionally on every '#' reached while skipping whitespace/
     * comments, not only right after a newline) -- true in practice here
     * too, since "#line 1" never occurs except where the loader placed
     * it. */
    static constexpr char marker[]="#line 1";
    static constexpr size_t marker_length=sizeof(marker)-1;
    size_t line=1;
    for(size_t index=0;index<offset&&combined[index]!='\0';index++) {
        if(combined[index]=='#'&&
           strncmp(combined+index,marker,marker_length)==0&&
           (combined[index+marker_length]=='\n'||combined[index+marker_length]=='\0'))
            line=0;
        if(combined[index]=='\n')line++;
    }
    return line;
}

/* The actual compile, run twice by diamond_compile below -- once
 * (discovery_pass=true) into a throwaway DiamondProgram purely to
 * register every class/module/interface's name/fields/methods and every
 * compiler-created function slot
 * regardless of textual order, then again (discovery_pass=false) into
 * the real, caller-supplied program, now with every declaration already
 * known. `program` must already be freshly diamond_program_init'd (with
 * allow_top_level_redefinition already restored onto it) by the caller --
 * both passes need that same prologue against their own separate
 * program, so it stays there rather than duplicated in here. */
static bool run_compile_pass(const char *source, DiamondProgram *program,
                             DiamondDiagnostic *diagnostic, bool discovery_pass,
                             size_t function_claim_start, bool debug_mode,
                             const DiamondProgram *tooling_returns,
                             ToolingDependencies *tooling_forward_calls,
                             ImplicitBegins *implicit_begins) {
    *diagnostic = (DiamondDiagnostic){};
    Compiler compiler = {
        .source = source,
        .program = program,
        .function = &program->entry,
        .current_class = -1,
        .struct_replaceable_class = -1,
        .current_module = -1,
        .contextual_block_return_set = -1,
        .expected_expression_type_set = -1,
        .current_return_type = -1,
        .current_exception = -1,
        .current_retry_target = SIZE_MAX,
        .last_instruction_offset = SIZE_MAX,
        .diagnostic = diagnostic,
        .discovery_pass = discovery_pass,
        .next_function_claim = function_claim_start,
        .tooling_returns=tooling_returns,
        .tooling_forward_calls=tooling_forward_calls,
        .implicit_begins=implicit_begins,
        /* Never set for the discovery pass -- see the field's own
         * comment in the Compiler struct. */
        .debug_mode = discovery_pass?false:debug_mode,
    };
    diamond_lexer_init(&compiler.lexer, source);
    compiler.current = diamond_lexer_next(&compiler.lexer);
    if(compiler.current.kind==DIAMOND_TOKEN_ERROR)
        fail(&compiler,compiler.current.span,"unexpected character");

    const uint16_t result = compile_sequence(&compiler);
    if (!compiler.failed && compiler.current.kind != DIAMOND_TOKEN_EOF) {
        fail(&compiler, compiler.current.span, "unexpected block terminator");
    }
    program->entry.register_count = compiler.next_register;
    if (!compiler.failed) {
        record_scope_locals(&compiler,0,compiler.local_count,strlen(source));
        program->entry.body_end=strlen(source);
        emit_instruction(&compiler, DIAMOND_OP_RETURN, result, 0, 0, 1);
        diamond_program_recompute_shapes(program);
#ifdef DIAMOND_DEBUG
        /* run_chunk treats register_count==0 as "unset" and zeroes a full
         * 4096-register (64 KB) file on every call to that function -- a
         * silent ~4x slowdown on hand-built bodies (the generated attr_*
         * and struct readers shipped that way). Debug builds, which the
         * test suite runs, refuse a function that never recorded its count. */
        for(size_t function_index=0;function_index<program->function_count;function_index++)
            if(program->functions[function_index]->register_count==0) {
                fprintf(stderr,"internal compiler error: function '%s' has no register count\n",
                        program->functions[function_index]->name);
                abort();
            }
#endif
        for(size_t class_index=0;class_index<program->class_count;class_index++) {
            DiamondClass *class=program->classes[class_index];
            /* Resolved by name, once, here -- never hardcoded, since a
             * conservative prelude that skips optional modules can shift
             * which index a *later*-defined class lands at (confirmed
             * directly; Range itself is safe today since lib/core.d6
             * defines it before any conditionally-included module, but
             * nothing should assume that stays true). See DiamondChunk's
             * own comment (src/vm.h) for what this is for. */
            if(strcmp(class->name,"Range")==0)
                program->range_class_index=(uint16_t)class_index;
        }
        /* Defensive, not expected to ever actually fire: every
         * prescan_singleton_signatures placeholder names a real
         * `def` token this same pass will reach before it ends (see that
         * function's own comment on why), so every pending fixup should
         * already be resolved by the time compilation otherwise
         * succeeds. Catching a leftover one here turns "silently call
         * whatever function ends up at index 0" into a clean, if
         * generic, compile error instead. */
        if(compiler.pending_singleton_fixup_count>0)
            fail(&compiler,compiler.current.span,
                "internal error: unresolved forward reference to "
                "singleton function");
    }
    if(!compiler.failed)
        rewrite_cyclic_tail_calls(program,compiler.tail_call_candidates,
            compiler.tail_call_candidate_count);
    free(compiler.tail_call_candidates);
    return !compiler.failed;
}

/* Compiles `source` twice. The first pass (see run_compile_pass's own
 * comment) is a throwaway declaration-discovery compile: it runs into a
 * separate, temporary DiamondProgram whose bytecode is discarded in
 * full, but whose class/module/interface tables (names, fields,
 * methods -- fully populated regardless of textual order, since a real
 * error there is the only thing that can stop this pass) are copied
 * into the second, real pass's own program before it starts. This is
 * what lets `SomeClass.new(...)`/`SomeClass.someMethod(...)`/a type
 * annotation reference a class/module/interface declared *later* in the same source.
 * A real syntax/semantic error unrelated to a forward reference still
 * fails identically in the first pass, and the second pass never runs --
 * the caller sees exactly one clean error, same as a single-pass compile
 * always has. Superclass/base-interface resolution is deliberately left
 * exactly as strict as before in both passes (see compile_class's own
 * comment on `claiming`): that needs the referenced class already fully
 * compiled, not just known by name, which this doesn't attempt to fix. */
/* Seeds `destination` (just diamond_program_init'd, otherwise empty)
 * with `template`'s already-fully-compiled classes/interfaces/modules/
 * functions, so a later compile pass over *additional* source can
 * reference them by name without ever re-parsing the source that
 * declared them. Marked declared_by_discovery=false throughout (unlike
 * the discovery-pass merge below, which marks its own fresh finds
 * true): these entries are already real and finished, not pending
 * slots for the upcoming pass to claim and refill -- compile_class/
 * compile_module/compile_interface's own claiming checks (see their
 * comments) read that flag to tell the two cases apart. Safe against
 * every one of those claiming paths for the same reason: `destination`
 * only ever gets compiled against source that doesn't redeclare any of
 * template's own names, so compile_class et al. never even look these
 * slots up except by an ordinary, successful by-name reference. */
static bool seed_program_from_template(DiamondProgram *destination,
                                       const DiamondProgram *template) {
    /* Interface signatures index the entry type table. Preserve that table
     * when seeding a restored or incremental program; copying only the
     * methods would reinterpret their indexes against unrelated new types. */
    if(template->interface_count>0) {
        if(!diamond_function_reserve_type_sets(&destination->entry,template->entry.type_set_count))return false;
        if(template->entry.type_set_count>0)
            memcpy(destination->entry.type_sets,template->entry.type_sets,
                template->entry.type_set_count*sizeof *template->entry.type_sets);
        destination->entry.type_set_count=template->entry.type_set_count;
    }
    /* Copy only the used prefix of each metadata table. Unused slots are
     * initialized explicitly when claimed, just as after program_init. */
    for(size_t index=0;index<template->class_count;index++) {
        DiamondClass *copy=diamond_program_class_slot(destination,index);
        if(copy==nullptr)return false;
        if(!diamond_class_copy(copy,template->classes[index]))return false;
        copy->index=(uint16_t)index;
    }
    destination->class_count=template->class_count;
    for(size_t index=0;index<template->interface_count;index++) {
        DiamondInterface *copy=diamond_program_interface_slot(destination,index);
        if(copy==nullptr)return false;
        if(!diamond_interface_copy(copy,template->interfaces[index]))return false;
        copy->type_sets=destination->entry.type_sets;
    }
    destination->interface_count=template->interface_count;
    for(size_t index=0;index<template->module_count;index++) {
        DiamondModule *copy=diamond_program_module_slot(destination,index);
        if(copy==nullptr)return false;
        if(!diamond_module_copy(copy,template->modules[index]))return false;
    }
    destination->module_count=template->module_count;
    destination->range_class_index=template->range_class_index;
    for(size_t index=0;index<template->function_count;index++) {
        DiamondFunction *copy=diamond_program_add_function(destination);
        if(copy==nullptr)return false;
        if(!diamond_function_copy(copy,template->functions[index]))return false;
        copy->declared_by_discovery=false;
    }
    return true;
}

/* Moves the discovery pass's class, interface, module and namespace-constant tables into
 * `program`. Part of diamond_compile_impl. */
static void adopt_discovered_tables(DiamondProgram *program,DiamondProgram *discovery) {
    /* Carry discovery's fully-populated class/module/interface tables
     * (names, fields, methods -- everything but real bytecode) into the
     * second, real pass's own program, *before* that pass starts, so
     * every declaration is already known regardless of textual order.
     * compile_class/compile_module/compile_interface's own claiming logic
     * (see their comments) is what makes the second pass treat every
     * handed-over entry as its own pre-reserved slot instead of a
     * duplicate declaration. Only discovery's used prefix moves, by
     * swapping slot pointers: each side keeps owning exactly the slots it
     * points at, and freeing `discovery` later releases whatever
     * `program` held before. */
    for (size_t index = 0; index < discovery->class_count; index++) {
        DiamondClass *kept = program->classes[index];
        program->classes[index] = discovery->classes[index];
        discovery->classes[index] = kept;
    }
    program->class_count = discovery->class_count;
    for (size_t index = 0; index < discovery->interface_count; index++) {
        DiamondInterface *kept = program->interfaces[index];
        program->interfaces[index] = discovery->interfaces[index];
        discovery->interfaces[index] = kept;
    }
    program->interface_count = discovery->interface_count;
    for (size_t index = 0; index < discovery->module_count; index++) {
        DiamondModule *kept = program->modules[index];
        program->modules[index] = discovery->modules[index];
        discovery->modules[index] = kept;
    }
    program->module_count = discovery->module_count;
    /* Only the names: each pass marks its own definitions. */
    if(diamond_program_reserve_namespace_constants(program,
           discovery->namespace_constant_count)) {
        if(discovery->namespace_constant_count>0)
            memcpy(program->namespace_constants,discovery->namespace_constants,
                discovery->namespace_constant_count*sizeof program->namespace_constants[0]);
        program->namespace_constant_count=discovery->namespace_constant_count;
    }
}

/* After the discovery pass's tables reach `program`, marks the entries the real pass will declare
 * again so it resets them instead of merging. Part of diamond_compile_impl. */
static void mark_discovered_declarations(DiamondProgram *program,const DiamondProgram *template) {
    /* Mark every *user* class/module entry just copied as "populated by
     * a pass other than the one about to run" -- compile_class/
     * compile_module only reset declared_by_discovery to false the
     * instant *they themselves* touch a slot (needed so a genuine reopen
     * within one pass's own walk merges instead of resetting), so by the
     * time discovery finishes, every entry it created already reads
     * false again. Without this explicit re-marking here, the real pass
     * would treat discovery's leftover (bytecode-discarded) method/field
     * tables as "already mine" and merge its own real methods on top
     * instead of resetting first -- silently duplicating every method
     * table entry. Built-in classes (Exception and friends,
     * DIAMOND_BUILTIN_CLASS_COUNT of them, registered identically by
     * both programs' own diamond_program_init and never re-declared by
     * user code) are skipped -- nothing ever "reopens" them through this
     * path, so there's nothing to mark stale; with a template, its own
     * classes/modules are skipped the same way and for the same reason
     * (already real, never re-touched by a compile that never mentions
     * their names). */
    for(size_t index=template!=nullptr?template->class_count:DIAMOND_BUILTIN_CLASS_COUNT;
        index<program->class_count;index++)
        program->classes[index]->declared_by_discovery=true;
    for(size_t index=template!=nullptr?template->module_count:0;
        index<program->module_count;index++)
        program->modules[index]->declared_by_discovery=true;
    /* A program can also reopen a template (prelude) class or module --
     * `class Range ... def middle() ... end`. Discovery added those methods
     * to its copy of the entry, and the real pass adds them again as it
     * reaches them, so such an entry is reset too -- but only back to what
     * the template itself had (from_template), not to empty. */
    if(template!=nullptr) {
        for(size_t index=0;index<template->class_count;index++) {
            DiamondClass *class=program->classes[index];
            const DiamondClass *original=template->classes[index];
            if(class->method_count==original->method_count&&
               class->singleton_method_count==original->singleton_method_count&&
               class->field_count==original->field_count&&
               class->class_variable_count==original->class_variable_count)continue;
            class->declared_by_discovery=true;
            class->from_template=true;
            class->template_method_count=original->method_count;
            class->template_singleton_method_count=original->singleton_method_count;
            class->template_field_count=original->field_count;
            class->template_class_variable_count=original->class_variable_count;
        }
        for(size_t index=0;index<template->module_count;index++) {
            DiamondModule *module=program->modules[index];
            const DiamondModule *original=template->modules[index];
            if(module->method_count==original->method_count&&
               module->singleton_method_count==original->singleton_method_count&&
               module->field_count==original->field_count)continue;
            module->declared_by_discovery=true;
            module->from_template=true;
            module->template_method_count=original->method_count;
            module->template_singleton_method_count=original->singleton_method_count;
            module->template_field_count=original->field_count;
        }
    }
}

/* Shared by diamond_compile (template=nullptr, today's exact behavior)
 * and diamond_compile_incremental (template!=nullptr): see each
 * public wrapper's own comment for what `template` buys and costs. */
bool diamond_compile_impl(const char *source, DiamondProgram *program,
                                 const DiamondProgram *template,
                                 bool debug_mode,
                                 DiamondDiagnostic *diagnostic,
                                 const DiamondProgram *tooling_returns,
                                 ToolingDependencies *tooling_forward_calls) {
    /* DIAMOND_TRACE_COMPILE: an opt-in stderr report of where compile time
     * goes, the same env-var-gated convention as DIAMOND_TRACE_GC and
     * friends (src/run_source.c). CLOCK_MONOTONIC, matching every other
     * wall-time measurement in this codebase. */
    const bool trace_compile=getenv("DIAMOND_TRACE_COMPILE")!=nullptr;
    struct timespec trace_start={0},trace_discovery_done={0},trace_end={0};
    if(trace_compile)clock_gettime(CLOCK_MONOTONIC,&trace_start);
    const bool allow_top_level_redefinition = program->allow_top_level_redefinition;
    diamond_program_free(program);

    const size_t function_claim_start=
        template!=nullptr?template->function_count:0;

    DiamondProgram *discovery = calloc(1, sizeof *discovery);
    /* diamond_program_init_fresh, not diamond_program_init: `discovery`
     * is freshly calloc'd right above (already all-zero) and never reused
     * across calls, so diamond_program_init's own memset would just
     * re-zero memory calloc already zeroed -- see that function's own
     * comment. */
    diamond_program_init_fresh(discovery);
    discovery->allow_top_level_redefinition = allow_top_level_redefinition;
    if(template!=nullptr&&!seed_program_from_template(discovery,template)) {
        diamond_program_free(discovery);free(discovery);
        *diagnostic=(DiamondDiagnostic){.message="out of memory"};
        return false;
    }
    DiamondDiagnostic discovery_diagnostic = {0};
    ImplicitBegins implicit_begins={};
    const bool discovered = run_compile_pass(
        source, discovery, &discovery_diagnostic, /*discovery_pass=*/true,
        function_claim_start, false,nullptr,nullptr,&implicit_begins);
    if(trace_compile)clock_gettime(CLOCK_MONOTONIC,&trace_discovery_done);

    diamond_program_init(program);
    program->allow_top_level_redefinition = allow_top_level_redefinition;
    if (!discovered) {
        free(implicit_begins.offsets);
        *diagnostic = discovery_diagnostic;
        diamond_program_free(discovery);
        free(discovery);
        return false;
    }
    if(template!=nullptr&&!seed_program_from_template(program,template)) {
        diamond_program_free(discovery);free(discovery);
        free(implicit_begins.offsets);
        *diagnostic=(DiamondDiagnostic){.message="out of memory"};
        return false;
    }

    adopt_discovered_tables(program,discovery);

    /* Reserve every compiler-created function at its discovery-pass index.
     * The real pass claims the slots in the same source order, preserving
     * both early top-level calls and copied class/module method indices.
     * Starts at function_claim_start (0 with no template), not 0
     * unconditionally: `discovery`'s own [0, function_claim_start) range
     * is template's own functions, already present in `program` via
     * seed_program_from_template above -- re-copying them here would
     * append duplicates rather than harmlessly overwrite, unlike the
     * fixed-size class/interface/module arrays just above. */
    if(!allow_top_level_redefinition) {
        for(size_t index=function_claim_start;index<discovery->function_count;index++) {
            const DiamondFunction *discovered_function=discovery->functions[index];
            DiamondFunction *reserved=diamond_program_add_function(program);
            if(reserved==nullptr) {
                diamond_program_free(discovery);free(discovery);
                free(implicit_begins.offsets);
                *diagnostic=(DiamondDiagnostic){.message="out of memory"};
                return false;
            }
            if(!diamond_function_copy(reserved,discovered_function)) {
                diamond_program_free(discovery);free(discovery);
                free(implicit_begins.offsets);
                *diagnostic=(DiamondDiagnostic){.message="out of memory"};
                return false;
            }
            reserved->declared_by_discovery=true;
        }
    }

    mark_discovered_declarations(program,template);

    diamond_program_free(discovery);
    free(discovery);

    const bool compiled=run_compile_pass(
        source,program,diagnostic,/*discovery_pass=*/false,function_claim_start,
        debug_mode,tooling_returns,tooling_forward_calls,&implicit_begins);
    free(implicit_begins.offsets);
    if(compiled)
        for(size_t index=0;index<program->interface_count;index++)
            program->interfaces[index]->type_sets=program->entry.type_sets;
    /* See DiamondFunction.redefine_method_used_anywhere's own comment
     * (src/vm.h): a single whole-program fact, snapshotted onto every
     * function (including program->entry, the top-level program itself)
     * since src/jit.c never sees DiamondProgram to read it directly. */
    if(compiled) {
        program->entry.redefine_method_used_anywhere=program->uses_redefine_method;
        for(size_t index=0;index<program->function_count;index++)
            program->functions[index]->redefine_method_used_anywhere=
                program->uses_redefine_method;
    }
    if(trace_compile) {
        clock_gettime(CLOCK_MONOTONIC,&trace_end);
        const double discovery_seconds=
            (double)(trace_discovery_done.tv_sec-trace_start.tv_sec)+
            (double)(trace_discovery_done.tv_nsec-trace_start.tv_nsec)/1e9;
        const double real_seconds=
            (double)(trace_end.tv_sec-trace_discovery_done.tv_sec)+
            (double)(trace_end.tv_nsec-trace_discovery_done.tv_nsec)/1e9;
        fprintf(stderr,
            "compile: discovery %.6fs, real %.6fs, total %.6fs, source %zu bytes\n",
            discovery_seconds,real_seconds,discovery_seconds+real_seconds,
            strlen(source));
    }
    return compiled;
}

bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic) {
    return diamond_compile_impl(source,program,nullptr,false,diagnostic,nullptr,nullptr);
}

static bool tooling_returns_equal(const DiamondProgram *left,
        const DiamondProgram *right,const ToolingDependencies *dependencies) {
    if(left->function_count!=right->function_count)return false;
    for(size_t index=0;index<left->function_count;index++) {
        if(dependencies->used!=nullptr&&!dependencies->used[index])continue;
        const DiamondFunction *a=left->functions[index],*b=right->functions[index];
        if(a->inferred_return_type_set==DIAMOND_NO_TYPE_SET||
           b->inferred_return_type_set==DIAMOND_NO_TYPE_SET) {
            if(a->inferred_return_type_set!=b->inferred_return_type_set)return false;
        } else if(!type_sets_structurally_equal(a->type_sets,a->type_set_count,
                a->inferred_return_type_set,b->type_sets,b->type_set_count,
                b->inferred_return_type_set,0))return false;
    }
    return true;
}

bool diamond_compile_for_tooling(const char *source,DiamondProgram *program,
        DiamondDiagnostic *diagnostic) {
    ToolingDependencies dependencies={};
    if(!diamond_compile_impl(source,program,nullptr,false,diagnostic,
            nullptr,&dependencies))return false;
    if(!dependencies.forward_calls||program->allow_top_level_redefinition)return true;
    /* Most documents need no additional pass. Bound editor work even for a
     * long graph or a recursive generic graph whose shapes never stabilize.
     * Each pass reads an immutable completed snapshot; declaration order
     * cannot expose a half-written type-set table from the current pass. */
    for(size_t pass=0;pass<8;pass++) {
        DiamondProgram *previous=malloc(sizeof *previous);
        if(previous==nullptr)return true;
        /* Move ownership, avoiding a whole-program deep clone. No C compound
         * literal here: DiamondProgram is much too large for a stack temporary. */
        memcpy(previous,program,sizeof *previous);
        memset(program,0,sizeof *program);
        dependencies.forward_calls=false;
        dependencies.count=previous->function_count;
        dependencies.used=calloc(dependencies.count,sizeof *dependencies.used);
        /* Allocation failure falls back to comparing all returns. */
        if(!diamond_compile_impl(source,program,nullptr,false,diagnostic,
                previous,&dependencies)) {
            free(dependencies.used);
            diamond_program_free(program);
            memcpy(program,previous,sizeof *program);free(previous);
            *diagnostic=(DiamondDiagnostic){};
            return true;
        }
        /* Only returns read from the snapshot can invalidate this pass.
         * Changes to callers already compiled using current facts do not
         * require another whole-document compile. */
        const bool stable=tooling_returns_equal(previous,program,&dependencies);
        free(dependencies.used);dependencies.used=nullptr;
        diamond_program_free(previous);free(previous);
        if(stable||!dependencies.forward_calls)break;
    }
    return true;
}

/* Compiles `source` against a `template` program (itself the result of
 * an earlier, ordinary diamond_compile call, e.g. over just the
 * prelude) instead of from scratch: every one of template's classes,
 * interfaces, modules, and top-level functions is available to
 * `source` by name, exactly as if `source` had been compiled as
 * template_source + source concatenated the way diamond_run_source
 * (src/run_source.c) does today -- without re-lexing/re-parsing
 * template_source, which is the actual cost being avoided (the embedded
 * prelude dominates a from-scratch compile).
 *
 * `template` itself is read-only here and never modified or freed --
 * the caller owns its lifetime and can reuse the same compiled
 * template across many calls to this function, which is the entire
 * point. `program` is the same in/out parameter diamond_compile always
 * takes (freed and reinitialized internally regardless of its prior
 * state).
 *
 * `source` must not itself redeclare any name template already
 * declares -- doing so hits the same "already defined" compile error
 * template_source + source concatenated would have produced, not a
 * silent divergence (see seed_program_from_template's own comment). It
 * *can* freely reference and call into anything template declared, in
 * either direction size doesn't matter here: template is always fully
 * compiled before `source` is ever parsed. */
bool diamond_compile_incremental(const char *source, DiamondProgram *program,
                                 const DiamondProgram *template,
                                 DiamondDiagnostic *diagnostic) {
    return diamond_compile_impl(source,program,template,false,diagnostic,nullptr,nullptr);
}

bool diamond_compile_with_breakpoints(const char *source, DiamondProgram *program,
                                      DiamondDiagnostic *diagnostic) {
    return diamond_compile_impl(source,program,nullptr,true,diagnostic,nullptr,nullptr);
}

DiamondChunk diamond_program_chunk(const DiamondProgram *program) {
    return (DiamondChunk){
        .name = program->entry_path[0] != '\0'
            ? program->entry_path : program->entry.name,
        .code = program->entry.code,
        .lines = program->entry.lines,
        .columns = program->entry.columns,
        .code_count = program->entry.code_count,
        .constants = program->entry.constants,
        .constant_count = program->entry.constant_count,
        .strings = program->entry.strings,
        .string_count = program->entry.string_count,
        .type_sets = program->entry.type_sets,
        .type_set_count = program->entry.type_set_count,
        .functions = program->functions,
        .function_count = program->function_count,
        .classes = program->classes,
        .class_count = program->class_count,
        .interfaces=program->interfaces,
        .interface_count=program->interface_count,
        .modules=program->modules,
        .module_count=program->module_count,
        .register_count=program->entry.register_count,
        .range_class_index=program->range_class_index,
    };
}
