#include "vm_internal.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"

#include <ctype.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Account for the program container here; dynamically added function records
 * are accounted for by ProgramBuilder#declare_function. */
DiamondProgramBuilder *allocate_program_builder(DiamondVm *vm) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondProgram *built=calloc(1,sizeof *built);
    if(built==nullptr)return nullptr;
    /* _fresh, not diamond_program_init: `built` is freshly calloc'd right
     * above and never reused -- each ProgramBuilder gets its own new
     * DiamondProgram. See diamond_program_init_fresh's own comment
     * (src/compiler.c). */
    diamond_program_init_fresh(built);
    DiamondProgramBuilder *handle=malloc(sizeof(DiamondProgramBuilder));
    if(handle==nullptr){diamond_program_free(built);free(built);return nullptr;}
    *handle=(DiamondProgramBuilder){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_PROGRAM_BUILDER},
        .program=built,.source_bundle=nullptr,.source_line=0,.source_column=0};
    vm->young_objects=&handle->object;
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
    return program->functions[function_index];
}

static DiamondClass *program_builder_class(DiamondProgram *program,
                                           int64_t class_index) {
    if(class_index<0||(uint64_t)class_index>=program->class_count)
        return nullptr;
    return program->classes[class_index];
}

/* Recomputes shapes[0..field_count] for a class -- mirrors the loop
 * diamond_compile itself runs once, over every class, right after
 * compilation finishes (src/compiler.c). ProgramBuilder#declare_field
 * needs the same recomputation done incrementally, since a
 * ProgramBuilder-built program never goes through diamond_compile at
 * all. */
static void program_builder_recompute_shapes(DiamondClass *class) {
    for(size_t field_count=0;field_count<=class->field_count;field_count++)
        class->shapes[field_count]=(DiamondShape){
            .class=class,.field_count=(uint8_t)field_count};
}

/* ProgramBuilder#run's real body, factored out of run_chunk's own opcode
 * switch for the same stack-frame-isolation reason as regexp_new_helper
 * above -- but far more load-bearing here: a bare local DiamondVm is
 * ~80KB (method/field caches, opcode
 * counters, namespace constants), not the ~100 bytes regexp_new_helper's
 * own locals needed. At -O0, every local anywhere in run_chunk's switch
 * contributes to its one shared stack frame regardless of which case
 * actually runs, so leaving a `DiamondVm` local inline in the INVOKE case
 * body would have added that to *every* recursive run_chunk level
 * unconditionally -- confirmed by a real crash: depth(5000) (the existing
 * regression test for the DIAMOND_MAX_CALL_DEPTH guard) segfaulted before
 * that guard could trip, a worse version of the exact bug the Regexp
 * round already found and fixed this way. Returns
 * DIAMOND_VM_PROGRAM_ERROR (not the constructed program's own status) for
 * a nonzero exit. A heap-object result is deep-copied into the caller's
 * own vm via copy_value_into_vm before run_vm is freed; kinds that
 * function can't safely copy (see its own comment) still report
 * DIAMOND_VM_TYPE_ERROR, a narrower version of this helper's original
 * scalar-only restriction. Takes `builder` itself (not just its
 * ->program) so that if the result actually contains an Instance,
 * ownership of `built` can be transferred into vm's adopted_programs
 * list (copy_value_into_vm's own adopt_program call) -- builder->program
 * is set to nullptr in that case so DIAMOND_OBJECT_PROGRAM_BUILDER's own
 * GC destructor (see diamond_vm_free) no longer frees it out from under
 * the copied instance still using it. */
DiamondVmStatus program_builder_run_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, DiamondValue *result) {
    DiamondProgram *built=builder->program;
    if(built==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#run: the program was handed to the VM by an earlier run");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    /* Mirrors diamond_compile's own post-compile-pass finalization
     * (src/compiler.c, end of run_compile_pass's caller) -- every
     * declare_interface/declare_interface_method call earlier left this
     * interface's type_sets pointer null (a ProgramBuilder-built
     * DiamondInterface is calloc-zeroed and there is no per-interface
     * ProgramBuilder method that ever sets it, unlike the native
     * compiler which points it at compiler->program->entry.type_sets
     * immediately at declaration and re-fixes it here, since that array
     * can still grow/realloc after an interface is declared). Left
     * unset, any later interface-satisfaction check reaching
     * runtime_set_satisfies through this interface's own type_sets
     * dereferences null -- confirmed as a real SEGV (not theoretical)
     * compiling a self-hosted program that checks `x is SomeInterface`
     * against a typed interface method. */
    for(size_t index=0;index<built->interface_count;index++)
        built->interfaces[index]->type_sets=built->entry.type_sets;
    /* Range indexing (`items[1..]`) recognizes the prelude's Range class
     * by this index; resolved by name as diamond_compile does. */
    built->range_class_index=DIAMOND_NO_CLASS;
    for(size_t index=0;index<built->class_count;index++)
        if(strcmp(built->classes[index]->name,"Range")==0)
            built->range_class_index=(uint16_t)index;
    const DiamondChunk built_chunk=diamond_program_chunk(built);
    /* #emit_byte/#patch_byte let Diamond code append raw bytes to a
     * function's code array with no idea what instruction it's building
     * -- unlike ordinary compiled bytecode, nothing here guarantees a
     * register operand stays within the function's own declared register
     * count, or that a jump target lands on a real instruction rather
     * than the middle of one. run_chunk (below, via diamond_vm_run) trusts
     * every register operand it reads with no bounds check of its own, so
     * an out-of-range one is a real out-of-bounds read/write on whatever
     * backs the callee's register array (the C stack for an ordinary-
     * sized function, a heap allocation past DIAMOND_INLINE_REGISTER_
     * COUNT) -- reachable from plain Diamond source, no native embedding
     * required. Reject it before it ever reaches run_chunk. */
    if(!diamond_verify_bytecode(&built_chunk)) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "run: constructed bytecode is invalid");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    /* Heap-allocated: a DiamondVm is ~80KB, and this helper can nest
     * (ProgramBuilder#run inside a program that ProgramBuilder#run built),
     * so a stack-local one would put that much on the C stack per level. */
    DiamondVm *run_vm=malloc(sizeof *run_vm);
    if(run_vm==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    diamond_vm_init(run_vm);
    DiamondValue run_result=DIAMOND_NIL;
    const DiamondVmStatus run_status=diamond_vm_run(run_vm,&built_chunk,&run_result);
    if(run_status!=DIAMOND_VM_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",
            run_vm->error[0]!='\0'?run_vm->error:diamond_vm_status_name(run_status));
        diamond_vm_free(run_vm);
        free(run_vm);
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondValue copied_result=DIAMOND_NIL;
    const DiamondChunk *adopted_owner=nullptr;
    if(!copy_value_into_vm(vm,run_result,built,nullptr,nullptr,&adopted_owner,&copied_result)) {
        /* A partial copy may already have adopted the program into `vm`; it owns it then. */
        if(adopted_owner!=nullptr)builder->program=nullptr;
        diamond_vm_free(run_vm);
        free(run_vm);
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "run does not support this result type");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(adopted_owner!=nullptr)builder->program=nullptr;
    diamond_vm_free(run_vm);
    free(run_vm);
    *result=copied_result;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_function(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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
       arity_value<0||arity_value>DIAMOND_MAX_DECLARED_PARAMETERS||
       required_value<0||required_value>arity_value) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_function has an invalid name or arity");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(built->function_count==DIAMOND_MAX_FUNCTIONS) {
        snprintf(vm->error,sizeof vm->error,"program has too many functions");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondFunction *function=diamond_program_add_function(built);
    if(function==nullptr) {
        snprintf(vm->error,sizeof vm->error,"program function allocation failed");
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    vm->bytes_allocated+=sizeof *function+sizeof function;
    *function=(DiamondFunction){};
    memcpy(function->name,fname->chars,fname->length);
    function->name[fname->length]='\0';
    function->owner_class=DIAMOND_OWNER_NONE;
    function->arity=(uint8_t)arity_value;
    function->required_arity=(uint8_t)required_value;
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
        function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
    const int64_t new_index=(int64_t)built->function_count-1;
    *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_emit_byte(DiamondVm *vm, DiamondProgramBuilder *builder,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
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
    if(target->code_count==target->code_capacity) {
        size_t capacity=target->code_capacity==0?256:
            target->code_capacity*2;
        if(capacity>DIAMOND_MAX_CODE)capacity=DIAMOND_MAX_CODE;
        if(!diamond_function_reserve_code(target,capacity)) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#emit_byte could not grow bytecode");
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    target->code[target->code_count]=(uint8_t)byte_value;
    target->lines[target->code_count]=builder->source_line;
    target->columns[target->code_count]=builder->source_column;
    target->code_count++;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Needed for jump backpatching: compiler.c's own patch_jump (see
 * src/compiler.c) directly overwrites function->code[operand] after
 * the fact, once a forward jump's real target is known -- a single-pass
 * emitter can't know a forward target's offset before emitting the
 * jump itself. Phase 3's Diamond-language parser needs the same
 * capability for if/while/loop, so this mirrors patch_jump exactly
 * (overwrite an already-emitted byte, never append). */
static DiamondVmStatus program_builder_patch_byte(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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

static DiamondVmStatus program_builder_add_constant(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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
    if(target->constant_count==target->constant_capacity) {
        size_t capacity=target->constant_capacity==0?32:
            target->constant_capacity*2;
        if(capacity>DIAMOND_MAX_CONSTANTS)capacity=DIAMOND_MAX_CONSTANTS;
        if(!diamond_function_reserve_constants(target,capacity)) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_constant could not grow constants");
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    const int64_t new_index=(int64_t)target->constant_count;
    target->constants[target->constant_count++]=value;
    *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_add_string(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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
    if(target->string_count==target->string_capacity&&
       !diamond_function_reserve_strings(target,
          target->string_capacity==0?16:target->string_capacity*2)) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#add_string could not grow strings");
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondStringConstant *slot=&target->strings[target->string_count];
    memcpy(slot->chars,text->chars,text->length);
    slot->chars[text->length]='\0';
    slot->length=text->length;
    const int64_t new_index=(int64_t)target->string_count;
    target->string_count++;
    *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_register_count(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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

/* Phase 3 sub-phase 3 (classes): declare_class/declare_field/
 * declare_method mirror compile_class/field_index/compile_definition's
 * own class-registration side effects in compiler.c -- fields not
 * needed by any sub-phase before this one (Phase 1's own design note
 * flagged them as deferred until class-compiling logic actually
 * needed them). */
static DiamondVmStatus program_builder_declare_class(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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
    DiamondClass *class=diamond_program_class_slot(built,built->class_count);
    if(class==nullptr) {
        snprintf(vm->error,sizeof vm->error,"out of memory declaring a class");
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    built->class_count++;
    *class=(DiamondClass){.index=(uint16_t)new_index};
    memcpy(class->name,cname->chars,cname->length);
    class->name[cname->length]='\0';
    class->superclass=parent==nullptr?DIAMOND_NO_CLASS:(uint16_t)superclass_index;
    if(parent!=nullptr&&parent->field_count>0) {
        if(!diamond_class_reserve_fields(class,parent->field_count))
            return DIAMOND_VM_OUT_OF_MEMORY;
        class->field_count=parent->field_count;
        memcpy(class->fields,parent->fields,
            parent->field_count*DIAMOND_MAX_FUNCTION_NAME);
    }
    program_builder_recompute_shapes(class);
    *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_module(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_module argument must be String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *name=(const DiamondString *)registers[base].as.object;
    if(built->module_count==DIAMOND_MAX_MODULES) {
        snprintf(vm->error,sizeof vm->error,
            "too many modules: a program holds at most %u modules",
            (unsigned)DIAMOND_MAX_MODULES);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_module has an invalid name");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<built->module_count;index++)
        if(strlen(built->modules[index]->name)==name->length&&
           memcmp(built->modules[index]->name,name->chars,name->length)==0) {
            snprintf(vm->error,sizeof vm->error,"module name is already defined");
            return DIAMOND_VM_TYPE_ERROR;
        }
    const int64_t index=(int64_t)built->module_count;
    DiamondModule *module=diamond_program_module_slot(built,built->module_count);
    if(module==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    built->module_count++;
    *module=(DiamondModule){};
    memcpy(module->name,name->chars,name->length);
    module->name[name->length]='\0';
    *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_namespace_constant(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_namespace_constant argument must be String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *name=(const DiamondString *)registers[base].as.object;
    if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
       !diamond_program_reserve_namespace_constants(built,
           built->namespace_constant_count+1)) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_namespace_constant has an invalid name");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<built->namespace_constant_count;index++)
        if(strlen(built->namespace_constants[index])==name->length&&
           memcmp(built->namespace_constants[index],name->chars,name->length)==0) {
            snprintf(vm->error,sizeof vm->error,"constant is already defined");
            return DIAMOND_VM_TYPE_ERROR;
        }
    const int64_t index=(int64_t)built->namespace_constant_count;
    memcpy(built->namespace_constants[built->namespace_constant_count],
        name->chars,name->length);
    built->namespace_constants[built->namespace_constant_count][name->length]='\0';
    built->namespace_constant_count++;
    *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_field(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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
    if(!diamond_class_reserve_fields(class,class->field_count+1)) {
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

/* Same find-or-create-by-name shape as declare_field immediately
 * above, targeting class->class_variables[]/class_variable_count
 * (src/compiler.c's own class_variable_index uses the same storage
 * natively) instead of class->fields[]/field_count -- a class
 * variable is per-class state, not part of an instance's own field
 * layout, so unlike declare_field this never touches
 * program_builder_recompute_shapes. */
static DiamondVmStatus program_builder_declare_class_variable(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_class_variable arguments must be (Int, String)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondClass *class=
        program_builder_class(built,registers[base].as.integer);
    const DiamondString *vname=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    if(class==nullptr||vname->length==0||
       vname->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_class_variable has an invalid class index or variable name");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<class->class_variable_count;index++)
        if(strlen(class->class_variables[index])==vname->length&&
           memcmp(class->class_variables[index],vname->chars,vname->length)==0) {
            *result=DIAMOND_INT((int64_t)index);return DIAMOND_VM_OK;
        }
    if(!diamond_class_reserve_class_variables(class,class->class_variable_count+1)) {
        snprintf(vm->error,sizeof vm->error,"class has too many class variables");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t new_index=(int64_t)class->class_variable_count;
    memcpy(class->class_variables[class->class_variable_count],
        vname->chars,vname->length);
    class->class_variables[class->class_variable_count][vname->length]='\0';
    class->class_variable_count++;
    *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_module_field(DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t module_index=registers[base].as.integer;
    const DiamondString *name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    if(module_index<0||(uint64_t)module_index>=built->module_count||
       name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondModule *module=built->modules[(size_t)module_index];
    for(size_t index=0;index<module->field_count;index++)
        if(strlen(module->fields[index])==name->length&&
           memcmp(module->fields[index],name->chars,name->length)==0) {
            *result=DIAMOND_INT((int64_t)index);return DIAMOND_VM_OK;
        }
    if(!diamond_module_reserve_fields(module,module->field_count+1))return DIAMOND_VM_TYPE_ERROR;
    const int64_t index=(int64_t)module->field_count;
    memcpy(module->fields[module->field_count],name->chars,name->length);
    module->fields[module->field_count][name->length]='\0';
    module->field_count++;
    *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_method(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
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
       arity_value<0||arity_value>DIAMOND_MAX_DECLARED_PARAMETERS||
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
    if(!diamond_reserve_methods(&class->methods,&class->method_capacity,class->method_count+1)) {
        snprintf(vm->error,sizeof vm->error,"class has too many methods");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondMethod *method=&class->methods[class->method_count++];
    *method=(DiamondMethod){};
    memcpy(method->name,mname->chars,mname->length);
    method->name[mname->length]='\0';
    method->function_index=(uint16_t)target_function;
    method->has_variadic=built->functions[target_function]->has_variadic;
    method->arity=(uint8_t)arity_value;
    method->required_arity=(uint8_t)required_value;
    method->is_private=registers[(size_t)base+5].as.boolean;
    method->is_protected=false;
    /* declare_function always leaves owner_class at UINT8_MAX (not a
     * method) since it runs before the caller knows whether this
     * function will end up registered as one -- diamond_compile's own
     * compile_definition sets it inline instead, once current_class is
     * known. Matched here now that it's known: parameter_offset (see
     * run_chunk's INVOKE handler) derives from owner_class, and a
     * method whose owner_class is still UINT8_MAX gets parameter_offset
     * 0 instead of 1, which silently breaks the private-method
     * "explicit self receiver" bypass for every ProgramBuilder-built
     * class -- caught by the self-hosted parser's own private/public
     * support calling a private method via `self.foo()`, not by any
     * existing scalar-argument differential case. */
    built->functions[target_function]->owner_class=(uint16_t)registers[base].as.integer;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_module_method(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=6)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+5].kind!=DIAMOND_VALUE_BOOL)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t module_index=registers[base].as.integer;
    const DiamondString *name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    const int64_t function_index=registers[(size_t)base+2].as.integer;
    const int64_t arity=registers[(size_t)base+3].as.integer;
    const int64_t required=registers[(size_t)base+4].as.integer;
    if(module_index<0||(uint64_t)module_index>=built->module_count||
       name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
       function_index<0||(uint64_t)function_index>=built->function_count||
       arity<0||arity>DIAMOND_MAX_DECLARED_PARAMETERS||required<0||required>arity)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondModule *module=built->modules[(size_t)module_index];
    for(size_t index=0;index<module->method_count;index++)
        if(!module->methods[index].included&&
           strlen(module->methods[index].name)==name->length&&
           memcmp(module->methods[index].name,name->chars,name->length)==0) {
            snprintf(vm->error,sizeof vm->error,"method is already defined");
            return DIAMOND_VM_TYPE_ERROR;
        }
    if(!diamond_reserve_methods(&module->methods,&module->method_capacity,module->method_count+1))return DIAMOND_VM_TYPE_ERROR;
    DiamondMethod *method=&module->methods[module->method_count++];
    *method=(DiamondMethod){};
    memcpy(method->name,name->chars,name->length);
    method->name[name->length]='\0';
    method->function_index=(uint16_t)function_index;
    method->has_variadic=built->functions[function_index]->has_variadic;
    method->arity=(uint8_t)arity;
    method->required_arity=(uint8_t)required;
    method->is_private=registers[(size_t)base+5].as.boolean;
    method->is_protected=false;
    /* Same owner_class fix as declare_method just above, using
     * diamond_compile's own module-method sentinel (UINT8_MAX-1,
     * distinct from UINT8_MAX's "not a method at all" so the private-
     * bypass's parameter_offset==1 check still fires for module
     * methods too). */
    built->functions[function_index]->owner_class=DIAMOND_OWNER_MODULE;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Declares a function directly as a class/module singleton method
 * (`def self.foo` syntax) -- distinct from export_module_method,
 * which instead re-exports an *already-declared* regular method
 * (the `module_function :name` syntax). Neither has a needs_receiver
 * counterpart here: diamond_compile's own module_singleton branch
 * (src/compiler.c's compile_definition) never sets it either, since
 * a directly-declared singleton never reserves register 0 for an
 * implicit self the way an ordinary method does. */
static DiamondVmStatus program_builder_declare_class_singleton_method(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=5)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_class_singleton_method arguments must be "
            "(Int, String, Int, Int, Int)");
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
       arity_value<0||arity_value>DIAMOND_MAX_DECLARED_PARAMETERS||
       required_value<0||required_value>arity_value) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_class_singleton_method has invalid arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<class->singleton_method_count;index++)
        if(strlen(class->singleton_methods[index].name)==mname->length&&
           memcmp(class->singleton_methods[index].name,mname->chars,
                  mname->length)==0) {
            snprintf(vm->error,sizeof vm->error,
                "duplicate or excessive class singleton method");
            return DIAMOND_VM_TYPE_ERROR;
        }
    if(!diamond_reserve_methods(&class->singleton_methods,&class->singleton_method_capacity,class->singleton_method_count+1)) {
        snprintf(vm->error,sizeof vm->error,
            "duplicate or excessive class singleton method");
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* A class's `def self.foo` reserves register 0 for the class it is
     * called on, as compiler.c's direct_class_singleton_member does: the
     * function takes one more argument than the method's arity, and
     * needs_receiver makes callers (INVOKE_SELF_METHOD included) fill that
     * slot. Its owner_class is the class too, so typed parameters skip
     * that slot (parameter_offset), as declare_method's do. */
    DiamondFunction *function=built->functions[target_function];
    if(function->arity!=arity_value+1) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_class_singleton_method needs a function with a receiver slot");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondMethod *method=
        &class->singleton_methods[class->singleton_method_count++];
    *method=(DiamondMethod){};
    memcpy(method->name,mname->chars,mname->length);
    method->name[mname->length]='\0';
    method->function_index=(uint16_t)target_function;
    method->has_variadic=function->has_variadic;
    method->arity=(uint8_t)arity_value;
    method->required_arity=(uint8_t)required_value;
    method->needs_receiver=true;
    function->owner_class=(uint16_t)registers[base].as.integer;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_module_singleton_method(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=5)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_module_singleton_method arguments must be "
            "(Int, String, Int, Int, Int)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t module_index=registers[base].as.integer;
    const DiamondString *mname=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    const int64_t target_function=registers[(size_t)base+2].as.integer;
    const int64_t arity_value=registers[(size_t)base+3].as.integer;
    const int64_t required_value=registers[(size_t)base+4].as.integer;
    if(module_index<0||(uint64_t)module_index>=built->module_count||
       mname->length==0||mname->length>=DIAMOND_MAX_FUNCTION_NAME||
       target_function<0||(uint64_t)target_function>=built->function_count||
       arity_value<0||arity_value>DIAMOND_MAX_DECLARED_PARAMETERS||
       required_value<0||required_value>arity_value) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_module_singleton_method has invalid arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondModule *module=built->modules[(size_t)module_index];
    for(size_t index=0;index<module->singleton_method_count;index++)
        if(strlen(module->singleton_methods[index].name)==mname->length&&
           memcmp(module->singleton_methods[index].name,mname->chars,
                  mname->length)==0) {
            snprintf(vm->error,sizeof vm->error,
                "duplicate or excessive module singleton function");
            return DIAMOND_VM_TYPE_ERROR;
        }
    if(!diamond_reserve_methods(&module->singleton_methods,&module->singleton_method_capacity,module->singleton_method_count+1)) {
        snprintf(vm->error,sizeof vm->error,
            "duplicate or excessive module singleton function");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondMethod *method=
        &module->singleton_methods[module->singleton_method_count++];
    *method=(DiamondMethod){};
    memcpy(method->name,mname->chars,mname->length);
    method->name[mname->length]='\0';
    method->function_index=(uint16_t)target_function;
    method->has_variadic=built->functions[target_function]->has_variadic;
    method->arity=(uint8_t)arity_value;
    method->required_arity=(uint8_t)required_value;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* A `def` nested directly inside a method body (never registered as
 * a named class method itself -- it stays a plain Closure value,
 * only ever installed via redefine_method) still needs owner_class
 * set so REDEFINE_METHOD's "callable must be a method of X" check
 * (which compares owner_class against the target class operand)
 * accepts it. declare_method/declare_module_method set this as a
 * side effect of registering a *named* method; this is the same
 * fix for a function that's deliberately never named. Mirrors
 * diamond_compile's own compile_definition, which sets
 * function->owner_class from current_class/current_module
 * unconditionally, independent of at_top_level. */
static DiamondVmStatus program_builder_set_function_owner_class(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "set_function_owner_class arguments must be (Int, Int)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t function_index=registers[base].as.integer;
    const int64_t owner_class=registers[(size_t)base+1].as.integer;
    if(function_index<0||(uint64_t)function_index>=built->function_count||
       owner_class<0||owner_class>UINT16_MAX) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "set_function_owner_class has invalid arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    built->functions[function_index]->owner_class=(uint16_t)owner_class;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* `sealed class X`: NEW rejects X itself; its subclasses instantiate normally.
 * Never cleared -- reopening a sealed class without `sealed` keeps it sealed. */
static DiamondVmStatus program_builder_seal_class(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    DiamondClass *class=registers[base].kind==DIAMOND_VALUE_INT?
        program_builder_class(built,registers[base].as.integer):nullptr;
    if(class==nullptr||registers[base].as.integer<DIAMOND_BUILTIN_CLASS_COUNT) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#seal_class has an invalid class index");
        return DIAMOND_VM_TYPE_ERROR;
    }
    class->sealed=true;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_include_module(DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondClass *class=program_builder_class(built,registers[base].as.integer);
    const int64_t module_index=registers[(size_t)base+1].as.integer;
    if(class==nullptr||module_index<0||
       (uint64_t)module_index>=built->module_count)return DIAMOND_VM_TYPE_ERROR;
    const DiamondModule *module=built->modules[(size_t)module_index];
    if(!diamond_class_reserve_fields(class,class->field_count+module->field_count)||
       !diamond_reserve_methods(&class->methods,&class->method_capacity,
           class->method_count+module->method_count))
        return DIAMOND_VM_TYPE_ERROR;
    for(size_t field=0;field<module->field_count;field++) {
        bool present=false;
        for(size_t existing=0;existing<class->field_count;existing++)
            if(strcmp(class->fields[existing],module->fields[field])==0)
                present=true;
        if(!present) {
            (void)snprintf(class->fields[class->field_count++],
                DIAMOND_MAX_FUNCTION_NAME,"%s",module->fields[field]);
        }
    }
    for(size_t method=0;method<module->method_count;method++) {
        if(methods_have_own_named(class->methods,class->method_count,
                                  module->methods[method].name))continue;
        class->methods[class->method_count]=module->methods[method];
        class->methods[class->method_count++].included=true;
    }
    program_builder_recompute_shapes(class);
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_include_module_in_module(DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t target_index=registers[base].as.integer;
    const int64_t source_index=registers[(size_t)base+1].as.integer;
    if(target_index<0||source_index<0||target_index==source_index||
       (uint64_t)target_index>=built->module_count||
       (uint64_t)source_index>=built->module_count)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondModule *target=built->modules[(size_t)target_index];
    const DiamondModule *source=built->modules[(size_t)source_index];
    if(!diamond_module_reserve_fields(target,target->field_count+source->field_count)||
       !diamond_reserve_methods(&target->methods,&target->method_capacity,
           target->method_count+source->method_count))
        return DIAMOND_VM_TYPE_ERROR;
    for(size_t field=0;field<source->field_count;field++) {
        bool present=false;
        for(size_t existing=0;existing<target->field_count;existing++)
            if(strcmp(target->fields[existing],source->fields[field])==0)
                present=true;
        if(!present) {
            const size_t length=strlen(source->fields[field]);
            memcpy(target->fields[target->field_count],source->fields[field],length+1);
            target->field_count++;
        }
    }
    for(size_t method=0;method<source->method_count;method++) {
        if(methods_have_own_named(target->methods,target->method_count,
                                  source->methods[method].name))continue;
        target->methods[target->method_count]=source->methods[method];
        target->methods[target->method_count++].included=true;
    }
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_module_method_visibility(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_BOOL)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t module_index=registers[base].as.integer;
    const DiamondString *name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    if(module_index<0||(uint64_t)module_index>=built->module_count)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondModule *module=built->modules[(size_t)module_index];
    DiamondMethod *found=nullptr;
    for(size_t index=0;index<module->method_count;index++)
        if(!module->methods[index].included&&
           strlen(module->methods[index].name)==name->length&&
           memcmp(module->methods[index].name,name->chars,name->length)==0)
            found=&module->methods[index];
    if(found==nullptr) {
        snprintf(vm->error,sizeof vm->error,"undefined method for visibility change");
        return DIAMOND_VM_TYPE_ERROR;
    }
    found->is_private=registers[(size_t)base+2].as.boolean;
    found->is_protected=false;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Named `private`/`public` visibility targets (`private foo, bar`)
 * for a class -- the class-side counterpart to
 * set_module_method_visibility, retroactively flipping an
 * already-declared method's is_private flag by name. */
static DiamondVmStatus program_builder_set_class_method_visibility(DiamondVm *vm,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=3&&argc!=4)return DIAMOND_VM_ARITY_ERROR;
    if(argc==4&&registers[(size_t)base+3].kind!=DIAMOND_VALUE_BOOL)return DIAMOND_VM_TYPE_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_BOOL)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondClass *class=program_builder_class(built,registers[base].as.integer);
    const DiamondString *name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    if(class==nullptr)return DIAMOND_VM_TYPE_ERROR;
    DiamondMethod *found=nullptr;
    for(size_t index=0;index<class->method_count;index++)
        if(!class->methods[index].included&&
           strlen(class->methods[index].name)==name->length&&
           memcmp(class->methods[index].name,name->chars,name->length)==0)
            found=&class->methods[index];
    if(found==nullptr) {
        snprintf(vm->error,sizeof vm->error,"undefined method for visibility change");
        return DIAMOND_VM_TYPE_ERROR;
    }
    found->is_private=registers[(size_t)base+2].as.boolean;
    found->is_protected=argc==4&&registers[(size_t)base+3].as.boolean;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* `alias_method new_name, existing_name` (compiler.c's
 * compile_alias_method): copies an already-declared method's
 * DiamondMethod struct verbatim under a new name -- same
 * function_index/arity/required_arity/is_private, just re-registered
 * so a second name can call the identical implementation. The
 * caller (selfhost/parser.d6) is expected to have already appended
 * "=" to either name string when aliasing a writer method, the same
 * way declare_method's own name argument already does. */
static DiamondVmStatus program_builder_alias_class_method(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+2].as.object->kind!=DIAMOND_OBJECT_STRING)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondClass *class=program_builder_class(built,registers[base].as.integer);
    const DiamondString *alias_name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    const DiamondString *original_name=
        (const DiamondString *)registers[(size_t)base+2].as.object;
    if(class==nullptr||alias_name->length==0||
       alias_name->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "alias_class_method has invalid arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondMethod *source=nullptr;
    for(size_t index=class->method_count;index>0;index--)
        if(!class->methods[index-1].included&&
           strlen(class->methods[index-1].name)==original_name->length&&
           memcmp(class->methods[index-1].name,original_name->chars,
                  original_name->length)==0) {
            source=&class->methods[index-1];break;
        }
    if(source==nullptr) {
        snprintf(vm->error,sizeof vm->error,"alias source is not defined here");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<class->method_count;index++)
        if(!class->methods[index].included&&
           strlen(class->methods[index].name)==alias_name->length&&
           memcmp(class->methods[index].name,alias_name->chars,
                  alias_name->length)==0) {
            snprintf(vm->error,sizeof vm->error,"alias name is already defined");
            return DIAMOND_VM_TYPE_ERROR;
        }
    DiamondMethod copied=*source;
    if(!diamond_reserve_methods(&class->methods,&class->method_capacity,class->method_count+1)) {
        snprintf(vm->error,sizeof vm->error,"too many methods");
        return DIAMOND_VM_TYPE_ERROR;
    }
    memcpy(copied.name,alias_name->chars,alias_name->length);
    copied.name[alias_name->length]='\0';copied.included=false;
    class->methods[class->method_count++]=copied;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_alias_module_method(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+2].as.object->kind!=DIAMOND_OBJECT_STRING)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t module_index=registers[base].as.integer;
    const DiamondString *alias_name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    const DiamondString *original_name=
        (const DiamondString *)registers[(size_t)base+2].as.object;
    if(module_index<0||(uint64_t)module_index>=built->module_count||
       alias_name->length==0||alias_name->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "alias_module_method has invalid arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondModule *module=built->modules[(size_t)module_index];
    DiamondMethod *source=nullptr;
    for(size_t index=module->method_count;index>0;index--)
        if(!module->methods[index-1].included&&
           strlen(module->methods[index-1].name)==original_name->length&&
           memcmp(module->methods[index-1].name,original_name->chars,
                  original_name->length)==0) {
            source=&module->methods[index-1];break;
        }
    if(source==nullptr) {
        snprintf(vm->error,sizeof vm->error,"alias source is not defined here");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<module->method_count;index++)
        if(!module->methods[index].included&&
           strlen(module->methods[index].name)==alias_name->length&&
           memcmp(module->methods[index].name,alias_name->chars,
                  alias_name->length)==0) {
            snprintf(vm->error,sizeof vm->error,"alias name is already defined");
            return DIAMOND_VM_TYPE_ERROR;
        }
    DiamondMethod copied=*source;
    if(!diamond_reserve_methods(&module->methods,&module->method_capacity,module->method_count+1)) {
        snprintf(vm->error,sizeof vm->error,"too many methods");
        return DIAMOND_VM_TYPE_ERROR;
    }
    memcpy(copied.name,alias_name->chars,alias_name->length);
    copied.name[alias_name->length]='\0';copied.included=false;
    module->methods[module->method_count++]=copied;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_export_module_method(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t module_index=registers[base].as.integer;
    const DiamondString *name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    if(module_index<0||(uint64_t)module_index>=built->module_count)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondModule *module=built->modules[(size_t)module_index];
    DiamondMethod *source=nullptr;
    for(size_t index=module->method_count;index>0;index--)
        if(!module->methods[index-1].included&&
           strlen(module->methods[index-1].name)==name->length&&
           memcmp(module->methods[index-1].name,name->chars,name->length)==0) {
            source=&module->methods[index-1];break;
        }
    if(source==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "module_function target is not defined here");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<module->singleton_method_count;index++)
        if(strcmp(module->singleton_methods[index].name,source->name)==0) {
            snprintf(vm->error,sizeof vm->error,
                "module singleton function is already defined");
            return DIAMOND_VM_TYPE_ERROR;
        }
    if(!diamond_reserve_methods(&module->singleton_methods,&module->singleton_method_capacity,module->singleton_method_count+1))
        return DIAMOND_VM_TYPE_ERROR;
    source->is_private=true;source->is_protected=false;
    DiamondMethod exported=*source;
    exported.is_private=false;exported.is_protected=false;
    exported.needs_receiver=true;
    module->singleton_methods[module->singleton_method_count++]=exported;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_expand_source(DiamondVm *vm, DiamondProgramBuilder *builder,
        DiamondProgram *built, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING)
        return DIAMOND_VM_TYPE_ERROR;
    const DiamondString *name=(const DiamondString *)registers[base].as.object;
    const DiamondString *source=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    /* Resolving a `require` reads files from disk, and the expanded text
     * is handed back to the caller: without this check a sandboxed
     * script could read any file whose name ends in `.d6` (any absolute
     * or `../` path, or a symlink named so), and learn which paths exist
     * from the error text, while File.open and File.read are denied.
     * The check is the same as VM_SANDBOX_GUARD's, which only works
     * inside run_chunk. */
    if(diamond_sandbox_enabled()&&!sandbox_category_allowed("filesystem")) {
        snprintf(vm->error,sizeof vm->error,
            "sandbox denies ProgramBuilder#expand_source");
        return DIAMOND_VM_SANDBOX_ERROR;
    }
    char path[DIAMOND_MAX_SOURCE_PATH];
    if(name->length>=sizeof path)return DIAMOND_VM_TYPE_ERROR;
    memcpy(path,name->chars,name->length);path[name->length]='\0';
    /* entry_path (not entry.name's 64-byte function-name buffer) holds
     * the root chunk's display name -- see src/compiler.h. */
    memcpy(built->entry_path,path,name->length+1);
    char *source_text=malloc(source->length+1);
    if(source_text==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    memcpy(source_text,source->chars,source->length);source_text[source->length]='\0';
    DiamondSourceBundle *bundle=malloc(sizeof *bundle);char error[512];
    if(bundle==nullptr){free(source_text);return DIAMOND_VM_OUT_OF_MEMORY;}
    const bool loaded=diamond_load_program(path,source_text,bundle,error,sizeof error);
    free(source_text);
    if(!loaded) {
        free(bundle);
        snprintf(vm->error,sizeof vm->error,"%s",error);
        return DIAMOND_VM_IO_ERROR;
    }
    DiamondString *expanded=allocate_string(vm,bundle->source,strlen(bundle->source));
    if(expanded==nullptr) {
        diamond_source_bundle_free(bundle);free(bundle);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    if(builder->source_bundle!=nullptr) {
        diamond_source_bundle_free(builder->source_bundle);
        free(builder->source_bundle);
    }
    builder->source_bundle=bundle;
    *result=DIAMOND_OBJECT(expanded);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_source_location(DiamondVm *vm,
        DiamondProgramBuilder *builder, DiamondValue *registers, uint16_t base, uint8_t argc,
        DiamondValue *result) {
    if(argc!=3||registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t offset=registers[base].as.integer;
    const int64_t line=registers[(size_t)base+1].as.integer;
    const int64_t column=registers[(size_t)base+2].as.integer;
    if(offset<0||line<1||column<1||builder->source_bundle==nullptr)
        return DIAMOND_VM_TYPE_ERROR;
    const char *mapped_path="<expanded>";
    uint64_t mapped_line=(uint64_t)line;
    for(size_t index=0;index<builder->source_bundle->segment_count;index++) {
        const DiamondSourceSegment *segment=&builder->source_bundle->segments[index];
        const uint64_t source_offset=(uint64_t)offset;
        if(source_offset<segment->start||
           (source_offset>segment->end&&source_offset-segment->end>9))continue;
        mapped_path=segment->path;mapped_line=segment->original_line;
        size_t limit=source_offset<segment->end
            ?(size_t)source_offset:segment->end;
        if(limit==segment->end&&limit>segment->start&&
           builder->source_bundle->source[limit-1]=='\n')limit--;
        for(size_t cursor=segment->start;cursor<limit;cursor++)
            if(builder->source_bundle->source[cursor]=='\n')mapped_line++;
        break;
    }
    char location[DIAMOND_MAX_SOURCE_PATH+64];
    const int written=snprintf(location,sizeof location,"%s:%lld:%lld",
        mapped_path,(long long)mapped_line,(long long)column);
    if(written<0||(size_t)written>=sizeof location)return DIAMOND_VM_TYPE_ERROR;
    DiamondString *mapped=allocate_string(vm,location,(size_t)written);
    if(mapped==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(mapped);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_source_location(DiamondProgramBuilder *builder,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2||registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t line=registers[base].as.integer;
    const int64_t column=registers[(size_t)base+1].as.integer;
    if(line<0||line>UINT32_MAX||column<0||column>UINT32_MAX)
        return DIAMOND_VM_TYPE_ERROR;
    builder->source_line=(uint32_t)line;
    builder->source_column=(uint32_t)column;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Type-set descriptors carry primitive, class, interface, type-variable,
 * collection and callable metadata used by the compiler. */
/* The type id of a native type name (Time, Channel, ...), or nil. */
static DiamondVmStatus program_builder_native_type_id(DiamondValue *registers, uint16_t base,
        uint8_t argc, DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||registers[base].as.object->kind!=DIAMOND_OBJECT_STRING)
        return DIAMOND_VM_TYPE_ERROR;
    uint8_t kind=0;
    *result=diamond_native_type_kind(((const DiamondString *)registers[base].as.object)->chars,&kind)?
        DIAMOND_INT((int64_t)DIAMOND_NATIVE_TYPE_OPERAND_BASE+kind):DIAMOND_NIL;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_type_set(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_type_set arguments must be (Int, Array)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondFunction *target=
        program_builder_target(built,registers[base].as.integer);
    const DiamondArray *type_ids=
        (const DiamondArray *)registers[(size_t)base+1].as.object;
    if(target==nullptr||type_ids->count==0||
       type_ids->count>DIAMOND_MAX_UNION_TYPES) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "declare_type_set has an invalid function index or type list");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<type_ids->count;index++) {
        if(type_ids->values[index].kind!=DIAMOND_VALUE_OBJECT||
           type_ids->values[index].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_type_set has an invalid function index or type list");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *descriptor=
            (const DiamondArray *)type_ids->values[index].as.object;
        if(descriptor->count!=6||
           descriptor->values[0].kind!=DIAMOND_VALUE_INT||
           descriptor->values[1].kind!=DIAMOND_VALUE_INT||
           descriptor->values[2].kind!=DIAMOND_VALUE_INT||
           descriptor->values[3].kind!=DIAMOND_VALUE_INT||
           descriptor->values[4].kind!=DIAMOND_VALUE_INT||
           descriptor->values[5].kind!=DIAMOND_VALUE_OBJECT||
           descriptor->values[5].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_type_set has an invalid function index or type list");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t type_id=descriptor->values[0].as.integer;
        const int64_t argument_set=descriptor->values[1].as.integer;
        const int64_t second_argument_set=descriptor->values[2].as.integer;
        const int64_t callable_arity=descriptor->values[3].as.integer;
        const int64_t callable_return=descriptor->values[4].as.integer;
        const DiamondArray *callable_parameters=
            (const DiamondArray *)descriptor->values[5].as.object;
        const bool primitive=type_id>=0&&type_id<DIAMOND_TYPE_CLASS_BASE;
        const bool class_type=type_id>=DIAMOND_TYPE_CLASS_BASE&&
            type_id<DIAMOND_TYPE_VARIABLE_BASE&&
            (uint64_t)(type_id-DIAMOND_TYPE_CLASS_BASE)<built->class_count;
        const bool interface_type=type_id>=DIAMOND_TYPE_INTERFACE_BASE&&
            (uint64_t)(type_id-DIAMOND_TYPE_INTERFACE_BASE)<
                built->interface_count;
        const bool variable_type=type_id>=DIAMOND_TYPE_VARIABLE_BASE&&
            type_id<DIAMOND_TYPE_INTERFACE_BASE&&
            (uint64_t)(type_id-DIAMOND_TYPE_VARIABLE_BASE)<
                target->type_variable_count;
        const bool valid_argument=argument_set==-1||
            (argument_set>=0&&(uint64_t)argument_set<target->type_set_count);
        const bool valid_second=second_argument_set==-1||
            (second_argument_set>=0&&
             (uint64_t)second_argument_set<target->type_set_count);
        const bool valid_return=callable_return==-1||
            (callable_return>=0&&
             (uint64_t)callable_return<target->type_set_count);
        bool valid_parameters=callable_parameters->count<=16;
        for(size_t parameter=0;parameter<callable_parameters->count;
            parameter++) {
            const DiamondValue value=callable_parameters->values[parameter];
            if(value.kind!=DIAMOND_VALUE_INT||value.as.integer<0||
               (uint64_t)value.as.integer>=target->type_set_count)
                valid_parameters=false;
        }
        const bool collection_arguments=
            (type_id==DIAMOND_TYPE_ARRAY&&valid_argument&&second_argument_set==-1)||
            (type_id==DIAMOND_TYPE_HASH&&valid_argument&&valid_second)||
            (class_type&&valid_argument&&valid_second)||
            (argument_set==-1&&second_argument_set==-1);
        const bool callable_arguments=type_id!=DIAMOND_TYPE_CALLABLE?
            callable_arity==-1&&callable_return==-1&&
                callable_parameters->count==0:
            /* Arity -1 is a Callable of any arity (bare `Callable`), which
             * takes no parameter list. */
            callable_arity>=-1&&callable_arity<=16&&valid_return&&
                valid_parameters&&
                (callable_parameters->count==0||
                 callable_parameters->count==(size_t)callable_arity);
        const bool native_type=diamond_type_operand_is_native((int)type_id);
        if(!(primitive||class_type||interface_type||variable_type||native_type)||
           !collection_arguments||
           !callable_arguments) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_type_set has an invalid function index or type list");
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    if(target->type_set_count==DIAMOND_MAX_TYPE_SETS) {
        snprintf(vm->error,sizeof vm->error,
            "function has too many type annotations");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(target->type_set_count==target->type_set_capacity) {
        size_t capacity=target->type_set_capacity==0?8:
            target->type_set_capacity*2;
        if(capacity>DIAMOND_MAX_TYPE_SETS)capacity=DIAMOND_MAX_TYPE_SETS;
        if(!diamond_function_reserve_type_sets(target,capacity))
            return DIAMOND_VM_OUT_OF_MEMORY;
    }
    const int64_t new_index=(int64_t)target->type_set_count;
    DiamondTypeSet *set=&target->type_sets[target->type_set_count++];
    *set=(DiamondTypeSet){.count=(uint8_t)type_ids->count};
    for(size_t member=0;member<type_ids->count;member++) {
        const DiamondArray *descriptor=
            (const DiamondArray *)type_ids->values[member].as.object;
        const int64_t argument_set=descriptor->values[1].as.integer;
        const int64_t second_argument_set=descriptor->values[2].as.integer;
        const int64_t callable_arity=descriptor->values[3].as.integer;
        const int64_t callable_return=descriptor->values[4].as.integer;
        const DiamondArray *callable_parameters=
            (const DiamondArray *)descriptor->values[5].as.object;
        /* For a native type the arity byte holds the object kind, as in
         * compiler.c's parse_type_annotation. */
        const int64_t member_type=descriptor->values[0].as.integer;
        const bool native_member=diamond_type_operand_is_native((int)member_type);
        set->members[member]=(DiamondTypeMember){
            .id=native_member?DIAMOND_TYPE_NATIVE:(uint16_t)member_type,
            .argument_set=argument_set<0?DIAMOND_NO_TYPE_SET:(uint16_t)argument_set,
            .second_argument_set=second_argument_set<0?DIAMOND_NO_TYPE_SET:
                (uint16_t)second_argument_set,
            .callable_arity=native_member?
                (uint8_t)(member_type-DIAMOND_NATIVE_TYPE_OPERAND_BASE):
                callable_arity<0?UINT8_MAX:(uint8_t)callable_arity,
            .callable_return_set=callable_return<0?DIAMOND_NO_TYPE_SET:
                (uint16_t)callable_return,
            .callable_parameters_typed=callable_parameters->count>0};
        for(size_t index=0;index<16;index++)
            set->members[member].callable_parameter_sets[index]=DIAMOND_NO_TYPE_SET;
        for(size_t index=0;index<callable_parameters->count;index++)
            set->members[member].callable_parameter_sets[index]=
                (uint16_t)callable_parameters->values[index].as.integer;
    }
    *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_parameter_type(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "set_parameter_type arguments must be (Int, Int, Int)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondFunction *target=
        program_builder_target(built,registers[base].as.integer);
    const int64_t parameter=registers[(size_t)base+1].as.integer;
    const int64_t set=registers[(size_t)base+2].as.integer;
    if(target==nullptr||parameter<0||parameter>=target->arity||
       set<0||(uint64_t)set>=target->type_set_count) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_parameter_type has an invalid index");
        return DIAMOND_VM_TYPE_ERROR;
    }
    target->parameter_type_sets[(size_t)parameter]=(uint16_t)set;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Records a parameter's name, which keyword calls (CALL_CLOSURE_KEYWORDS, INVOKE_KEYWORDS) match
 * against; truncated to fit, as the native compiler's record_parameter_name does. */
static DiamondVmStatus program_builder_set_parameter_name(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+2].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "set_parameter_name arguments must be (Int, Int, String)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondFunction *target=
        program_builder_target(built,registers[base].as.integer);
    const int64_t parameter=registers[(size_t)base+1].as.integer;
    if(target==nullptr||parameter<0||parameter>=target->arity) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_parameter_name has an invalid index");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *name=(const DiamondString *)registers[(size_t)base+2].as.object;
    size_t length=name->length;
    if(length>=DIAMOND_MAX_FUNCTION_NAME)length=DIAMOND_MAX_FUNCTION_NAME-1;
    memcpy(target->parameter_names[(size_t)parameter],name->chars,length);
    target->parameter_names[(size_t)parameter][length]='\0';
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_parameter_flags(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_BOOL||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_BOOL) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "set_parameter_flags arguments must be (Int, Bool, Bool)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* A variadic slot (and a trailing &block after it) occupies a declared
     * parameter, so a function with no parameters can't carry either. The
     * flags must be set before declare_method/declare_*_singleton_method,
     * which copy has_variadic into the method entry. */
    DiamondFunction *target=
        program_builder_target(built,registers[base].as.integer);
    const bool variadic=registers[(size_t)base+1].as.boolean;
    const bool block=registers[(size_t)base+2].as.boolean;
    if(target==nullptr||target==&built->entry||
       (unsigned)variadic+(unsigned)block>target->arity) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_parameter_flags has an invalid function or too few parameters");
        return DIAMOND_VM_TYPE_ERROR;
    }
    target->has_variadic=variadic;
    target->has_block_parameter=block;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* A `break` or `return` in a block lands in this function's frame, which
 * keeps it out of the JIT -- compile_block/compile_block_return set the same
 * flag in compiler.c. */
static DiamondVmStatus program_builder_set_nonlocal_landing(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    DiamondFunction *target=registers[base].kind==DIAMOND_VALUE_INT?
        program_builder_target(built,registers[base].as.integer):nullptr;
    if(target==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_nonlocal_landing has an invalid function index");
        return DIAMOND_VM_TYPE_ERROR;
    }
    target->nonlocal_landing=true;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* The offset of the call a block that can `break` is passed to, in the
 * function that creates it: BLOCK_BREAK lands only in that very call.
 * compiler.c's emit_opcode records the same offset. */
static DiamondVmStatus program_builder_set_block_call_offset(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    DiamondFunction *target=registers[base].kind==DIAMOND_VALUE_INT?
        program_builder_target(built,registers[base].as.integer):nullptr;
    const DiamondValue offset=registers[(size_t)base+1];
    if(target==nullptr||target==&built->entry||offset.kind!=DIAMOND_VALUE_INT||
       offset.as.integer<=0||offset.as.integer>=DIAMOND_MAX_CODE) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_block_call_offset has an invalid function or offset");
        return DIAMOND_VM_TYPE_ERROR;
    }
    target->block_call_offset=(uint32_t)offset.as.integer;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_return_type(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "set_return_type arguments must be (Int, Int)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondFunction *target=
        program_builder_target(built,registers[base].as.integer);
    const int64_t set=registers[(size_t)base+1].as.integer;
    if(target==nullptr||set<0||(uint64_t)set>=target->type_set_count) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_return_type has an invalid index");
        return DIAMOND_VM_TYPE_ERROR;
    }
    target->return_type_set=(uint16_t)set;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* The `class Box[T]` / `class Pair[K, V]` parameters of a class just declared. */
static DiamondVmStatus program_builder_set_class_type_parameters(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    DiamondClass *class=registers[base].kind==DIAMOND_VALUE_INT?
        program_builder_class(built,registers[base].as.integer):nullptr;
    const DiamondValue names=registers[(size_t)base+1];
    if(class==nullptr||names.kind!=DIAMOND_VALUE_OBJECT||names.as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_class_type_parameters arguments must be (class index, Array)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondArray *list=(const DiamondArray *)names.as.object;
    if(list->count==0||list->count>2) {
        snprintf(vm->error,sizeof vm->error,"generic classes require one or two type parameters");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t index=0;index<list->count;index++) {
        if(list->values[index].kind!=DIAMOND_VALUE_OBJECT||
           list->values[index].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        const DiamondString *name=(const DiamondString *)list->values[index].as.object;
        if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME)return DIAMOND_VM_TYPE_ERROR;
        char *target=index==0?class->type_parameter:class->second_type_parameter;
        memcpy(target,name->chars,name->length);
        target[name->length]='\0';
    }
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Maps a class's parameters onto its superclass's, as compile_class does for
 * `class Child[T] < Box[T]` and `class IntBox < Box[Int]`. `sets` holds the
 * written arguments as type sets of the given function, and is empty when
 * there were none. */
static DiamondVmStatus program_builder_set_parent_type_arguments(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
    DiamondClass *class=registers[base].kind==DIAMOND_VALUE_INT?
        program_builder_class(built,registers[base].as.integer):nullptr;
    const DiamondFunction *function=registers[(size_t)base+1].kind==DIAMOND_VALUE_INT?
        program_builder_target(built,registers[(size_t)base+1].as.integer):nullptr;
    const DiamondValue list=registers[(size_t)base+2];
    if(class==nullptr||function==nullptr||class->superclass==DIAMOND_NO_CLASS||
       list.kind!=DIAMOND_VALUE_OBJECT||list.as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#set_parent_type_arguments arguments must be (subclass index, function index, Array)");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondArray *sets=(const DiamondArray *)list.as.object;
    const DiamondClass *parent=program_builder_class(built,class->superclass);
    const size_t subclass_parameters=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class);
    const size_t parent_parameters=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(parent);
    const char *problem=nullptr;
    for(size_t index=0;index<sets->count;index++)
        if(sets->values[index].kind!=DIAMOND_VALUE_INT||sets->values[index].as.integer<0||
           (uint64_t)sets->values[index].as.integer>=function->type_set_count)
            problem="ProgramBuilder#set_parent_type_arguments has an invalid type set";
    if(problem==nullptr&&parent_parameters==0) {
        if(sets->count>0)problem="non-generic superclass does not accept type arguments";
    } else if(problem==nullptr&&parent->fixed_type_arguments) {
        if(subclass_parameters>0) {
            memcpy(class->parent_type_bindings,parent->fixed_type_bindings,sizeof class->parent_type_bindings);
            class->transforms_parent_arguments=true;
        } else memcpy(class->fixed_type_bindings,parent->fixed_type_bindings,sizeof class->fixed_type_bindings);
    } else if(problem==nullptr&&sets->count!=parent_parameters) {
        problem=sets->count==0?(subclass_parameters>0?"generic superclass requires forwarded type arguments":
            "generic superclass requires fixed type arguments"):"wrong number of superclass type arguments";
    } else if(problem==nullptr&&subclass_parameters>0) {
        unsigned occurrences[2]={};
        bool simple=parent_parameters==subclass_parameters;uint8_t first=0;
        for(size_t variable=0;variable<parent_parameters&&problem==nullptr;variable++) {
            DiamondTypeBinding *binding=&class->parent_type_bindings[variable];
            if(!diamond_bind_type_template(function->type_sets,function->type_set_count,
                (uint16_t)sets->values[variable].as.integer,binding)) {
                problem="superclass type template exceeds the binding graph";break;
            }
            for(size_t n=0;n<binding->node_count;n++)
                for(size_t m=0;m<binding->nodes[n].count;m++) {
                    const uint16_t id=binding->nodes[n].members[m].id;
                    if(id>=DIAMOND_TYPE_VARIABLE_BASE&&id<DIAMOND_TYPE_VARIABLE_BASE+subclass_parameters) {
                        occurrences[id-DIAMOND_TYPE_VARIABLE_BASE]++;
                        if(binding->nodes[n].count!=1)problem="parent type variables cannot be union alternatives";
                    }
                }
            if(binding->node_count!=1||binding->nodes[0].count!=1||
               binding->nodes[0].members[0].id<DIAMOND_TYPE_VARIABLE_BASE||
               binding->nodes[0].members[0].id>=DIAMOND_TYPE_VARIABLE_BASE+subclass_parameters)simple=false;
            else if(variable==0)first=(uint8_t)(binding->nodes[0].members[0].id-DIAMOND_TYPE_VARIABLE_BASE);
        }
        for(size_t variable=0;variable<subclass_parameters;variable++)
            if(occurrences[variable]!=1)simple=false;
        class->transforms_parent_arguments=!simple;
        class->swaps_parent_arguments=simple&&first==1;
    } else if(problem==nullptr) {
        for(size_t variable=0;variable<parent_parameters;variable++)
            if(!diamond_bind_fixed_type(function->type_sets,function->type_set_count,
                (uint16_t)sets->values[variable].as.integer,&class->fixed_type_bindings[variable]))
                problem="superclass type arguments must be concrete and fit the binding graph";
    }
    if(problem!=nullptr) {
        snprintf(vm->error,sizeof vm->error,"%s",problem);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(subclass_parameters==0&&parent_parameters>0) {
        class->fixed_type_arguments=true;
        snprintf(class->type_parameter,sizeof class->type_parameter,"$parent0");
        if(parent_parameters==2)snprintf(class->second_type_parameter,sizeof class->second_type_parameter,"$parent1");
    }
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* A closure of this function keeps the type bindings of the function that
 * created it, whose variables it declares as its own. */
static DiamondVmStatus program_builder_set_captures_type_variables(DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    DiamondFunction *target=registers[base].kind==DIAMOND_VALUE_INT?
        program_builder_target(built,registers[base].as.integer):nullptr;
    if(target==nullptr||target->type_variable_count==0)return DIAMOND_VM_TYPE_ERROR;
    target->captures_type_variables=true;
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_set_type_variables(DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2&&argc!=3)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondFunction *target=
        program_builder_target(built,registers[base].as.integer);
    const DiamondArray *variables=
        (const DiamondArray *)registers[(size_t)base+1].as.object;
    if(target==nullptr||variables->count>8)return DIAMOND_VM_TYPE_ERROR;
    /* The optional count says how many leading names are the class's own. */
    if(argc==3) {
        const DiamondValue class_count=registers[(size_t)base+2];
        if(class_count.kind!=DIAMOND_VALUE_INT||class_count.as.integer<0||class_count.as.integer>2||
           (uint64_t)class_count.as.integer>variables->count)return DIAMOND_VM_TYPE_ERROR;
        target->class_type_variable=(uint8_t)class_count.as.integer;
    }
    target->type_variable_count=(uint8_t)variables->count;
    for(size_t index=0;index<variables->count;index++) {
        if(variables->values[index].kind!=DIAMOND_VALUE_OBJECT||
           variables->values[index].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        const DiamondString *name=
            (const DiamondString *)variables->values[index].as.object;
        if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME)
            return DIAMOND_VM_TYPE_ERROR;
        memcpy(target->type_variables[index],name->chars,name->length);
        target->type_variables[index][name->length]='\0';
    }
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_interface(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_interface argument must be String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *name=(const DiamondString *)registers[base].as.object;
    if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
       built->interface_count==DIAMOND_MAX_INTERFACES) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder#declare_interface has an invalid name");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t index=(int64_t)built->interface_count;
    DiamondInterface *interface=diamond_program_interface_slot(built,built->interface_count);
    if(interface==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    built->interface_count++;
    *interface=(DiamondInterface){.type_sets=built->entry.type_sets};
    memcpy(interface->name,name->chars,name->length);
    interface->name[name->length]='\0';
    *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_inherit_interface(DiamondVm *vm, DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t target_index=registers[base].as.integer;
    const int64_t base_index=registers[(size_t)base+1].as.integer;
    if(target_index<0||base_index<0||
       (uint64_t)target_index>=built->interface_count||
       (uint64_t)base_index>=built->interface_count||
       target_index==base_index)return DIAMOND_VM_TYPE_ERROR;
    DiamondInterface *target=built->interfaces[(size_t)target_index];
    const DiamondInterface *source=built->interfaces[(size_t)base_index];
    if(!diamond_reserve_interface_methods(&target->methods,&target->method_capacity,
           target->method_count+source->method_count))
        return DIAMOND_VM_TYPE_ERROR;
    for(size_t method=0;method<source->method_count;method++) {
        for(size_t existing=0;existing<target->method_count;existing++)
            if(strcmp(target->methods[existing].name,
                      source->methods[method].name)==0) {
                snprintf(vm->error,sizeof vm->error,
                    "duplicate interface method");
                return DIAMOND_VM_TYPE_ERROR;
            }
        target->methods[target->method_count++]=source->methods[method];
    }
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_declare_interface_method(DiamondProgram *built,
        DiamondValue *registers, uint16_t base, uint8_t argc, DiamondValue *result) {
    if(argc!=5)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
       registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+3].kind!=DIAMOND_VALUE_OBJECT||
       registers[(size_t)base+3].as.object->kind!=DIAMOND_OBJECT_ARRAY||
       registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    const int64_t interface_index=registers[base].as.integer;
    const DiamondString *name=
        (const DiamondString *)registers[(size_t)base+1].as.object;
    const int64_t arity=registers[(size_t)base+2].as.integer;
    const DiamondArray *sets=
        (const DiamondArray *)registers[(size_t)base+3].as.object;
    const int64_t return_set=registers[(size_t)base+4].as.integer;
    if(interface_index<0||(uint64_t)interface_index>=built->interface_count||
       name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
       arity<0||arity>DIAMOND_MAX_DECLARED_PARAMETERS||sets->count!=(size_t)arity||
       (return_set>=0&&(uint64_t)return_set>=built->entry.type_set_count))
        return DIAMOND_VM_TYPE_ERROR;
    DiamondInterface *interface=built->interfaces[(size_t)interface_index];
    if(!diamond_reserve_interface_methods(&interface->methods,&interface->method_capacity,interface->method_count+1))return DIAMOND_VM_TYPE_ERROR;
    DiamondInterfaceMethod *method=&interface->methods[interface->method_count++];
    *method=(DiamondInterfaceMethod){.arity=(uint8_t)arity,
        .return_type_set=return_set<0?DIAMOND_NO_TYPE_SET:(uint16_t)return_set};
    memcpy(method->name,name->chars,name->length);
    method->name[name->length]='\0';
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
        method->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<sets->count;index++) {
        if(sets->values[index].kind!=DIAMOND_VALUE_INT)return DIAMOND_VM_TYPE_ERROR;
        const int64_t set=sets->values[index].as.integer;
        if(set>=0&&(uint64_t)set>=built->entry.type_set_count)
            return DIAMOND_VM_TYPE_ERROR;
        method->parameter_type_sets[index]=set<0?DIAMOND_NO_TYPE_SET:(uint16_t)set;
    }
    *result=DIAMOND_NIL;return DIAMOND_VM_OK;
}

static DiamondVmStatus program_builder_run(DiamondVm *vm, DiamondProgramBuilder *builder,
        uint8_t argc, size_t depth, DiamondValue *result) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    /* Refuses to nest past a conservative depth rather than the full
     * DIAMOND_MAX_CALL_DEPTH: program_builder_run_helper's diamond_vm_run
     * starts a *fresh* run_chunk recursion (depth 0) on top of this
     * call's own C stack frame, which is itself already `depth` levels of
     * run_chunk deep -- so real C-stack usage is the *sum* of outer and
     * inner depth, not bounded by either guard alone. Capping outer depth
     * at 10 keeps that sum within the same call-depth budget already
     * verified safe under ASan even if the inner program recurses to its
     * own full limit. */
    if(depth>=10) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#run nested too deeply");
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    return program_builder_run_helper(vm,builder,result);
}

/* Keep method dispatch separate from implementations so each method's
 * validation, mutations and result can be reviewed together. These helpers
 * remain outside run_chunk to preserve its recursive stack-frame budget. */
DiamondVmStatus program_builder_invoke_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, const DiamondStringConstant *method_name,
        DiamondValue *registers, uint16_t base, uint8_t argc, size_t depth,
        DiamondValue *result) {
    DiamondProgram *built=builder->program;
    if(built==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "ProgramBuilder: the program was handed to the VM by an earlier run");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    if(method_name->length==sizeof("declare_function")-1&&
       memcmp(method_name->chars,"declare_function",
           sizeof("declare_function")-1)==0)
        return program_builder_declare_function(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("emit_byte")-1&&
       memcmp(method_name->chars,"emit_byte",sizeof("emit_byte")-1)==0)
        return program_builder_emit_byte(vm,builder,built,registers,base,argc,result);
    if(method_name->length==sizeof("patch_byte")-1&&
       memcmp(method_name->chars,"patch_byte",sizeof("patch_byte")-1)==0)
        return program_builder_patch_byte(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("add_constant")-1&&
       memcmp(method_name->chars,"add_constant",
           sizeof("add_constant")-1)==0)
        return program_builder_add_constant(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("add_string")-1&&
       memcmp(method_name->chars,"add_string",sizeof("add_string")-1)==0)
        return program_builder_add_string(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_register_count")-1&&
       memcmp(method_name->chars,"set_register_count",
           sizeof("set_register_count")-1)==0)
        return program_builder_set_register_count(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_class")-1&&
       memcmp(method_name->chars,"declare_class",sizeof("declare_class")-1)==0)
        return program_builder_declare_class(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_module")-1&&
       memcmp(method_name->chars,"declare_module",sizeof("declare_module")-1)==0)
        return program_builder_declare_module(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_namespace_constant")-1&&
       memcmp(method_name->chars,"declare_namespace_constant",
           sizeof("declare_namespace_constant")-1)==0)
        return program_builder_declare_namespace_constant(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_field")-1&&
       memcmp(method_name->chars,"declare_field",sizeof("declare_field")-1)==0)
        return program_builder_declare_field(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_class_variable")-1&&
       memcmp(method_name->chars,"declare_class_variable",
           sizeof("declare_class_variable")-1)==0)
        return program_builder_declare_class_variable(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_module_field")-1&&
       memcmp(method_name->chars,"declare_module_field",
           sizeof("declare_module_field")-1)==0)
        return program_builder_declare_module_field(built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_method")-1&&
       memcmp(method_name->chars,"declare_method",sizeof("declare_method")-1)==0)
        return program_builder_declare_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_module_method")-1&&
       memcmp(method_name->chars,"declare_module_method",
           sizeof("declare_module_method")-1)==0)
        return program_builder_declare_module_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_class_singleton_method")-1&&
       memcmp(method_name->chars,"declare_class_singleton_method",
           sizeof("declare_class_singleton_method")-1)==0)
        return program_builder_declare_class_singleton_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_module_singleton_method")-1&&
       memcmp(method_name->chars,"declare_module_singleton_method",
           sizeof("declare_module_singleton_method")-1)==0)
        return program_builder_declare_module_singleton_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_function_owner_class")-1&&
       memcmp(method_name->chars,"set_function_owner_class",
           sizeof("set_function_owner_class")-1)==0)
        return program_builder_set_function_owner_class(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("seal_class")-1&&
       memcmp(method_name->chars,"seal_class",sizeof("seal_class")-1)==0)
        return program_builder_seal_class(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("include_module")-1&&
       memcmp(method_name->chars,"include_module",sizeof("include_module")-1)==0)
        return program_builder_include_module(built,registers,base,argc,result);
    if(method_name->length==sizeof("include_module_in_module")-1&&
       memcmp(method_name->chars,"include_module_in_module",
           sizeof("include_module_in_module")-1)==0)
        return program_builder_include_module_in_module(built,registers,base,argc,result);
    if(method_name->length==sizeof("set_module_method_visibility")-1&&
       memcmp(method_name->chars,"set_module_method_visibility",
           sizeof("set_module_method_visibility")-1)==0)
        return program_builder_set_module_method_visibility(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_class_method_visibility")-1&&
       memcmp(method_name->chars,"set_class_method_visibility",
           sizeof("set_class_method_visibility")-1)==0)
        return program_builder_set_class_method_visibility(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("alias_class_method")-1&&
       memcmp(method_name->chars,"alias_class_method",
           sizeof("alias_class_method")-1)==0)
        return program_builder_alias_class_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("alias_module_method")-1&&
       memcmp(method_name->chars,"alias_module_method",
           sizeof("alias_module_method")-1)==0)
        return program_builder_alias_module_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("export_module_method")-1&&
       memcmp(method_name->chars,"export_module_method",
           sizeof("export_module_method")-1)==0)
        return program_builder_export_module_method(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("expand_source")-1&&
       memcmp(method_name->chars,"expand_source",sizeof("expand_source")-1)==0)
        return program_builder_expand_source(vm,builder,built,registers,base,argc,result);
    if(method_name->length==sizeof("source_location")-1&&
       memcmp(method_name->chars,"source_location",sizeof("source_location")-1)==0)
        return program_builder_source_location(vm,builder,registers,base,argc,result);
    if(method_name->length==sizeof("set_source_location")-1&&
       memcmp(method_name->chars,"set_source_location",
           sizeof("set_source_location")-1)==0)
        return program_builder_set_source_location(builder,registers,base,argc,result);
    if(method_name->length==sizeof("declare_type_set")-1&&
       memcmp(method_name->chars,"declare_type_set",
           sizeof("declare_type_set")-1)==0)
        return program_builder_declare_type_set(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_parameter_type")-1&&
       memcmp(method_name->chars,"set_parameter_type",
           sizeof("set_parameter_type")-1)==0)
        return program_builder_set_parameter_type(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_parameter_flags")-1&&
       memcmp(method_name->chars,"set_parameter_flags",
           sizeof("set_parameter_flags")-1)==0)
        return program_builder_set_parameter_flags(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_nonlocal_landing")-1&&
       memcmp(method_name->chars,"set_nonlocal_landing",
           sizeof("set_nonlocal_landing")-1)==0)
        return program_builder_set_nonlocal_landing(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_block_call_offset")-1&&
       memcmp(method_name->chars,"set_block_call_offset",
           sizeof("set_block_call_offset")-1)==0)
        return program_builder_set_block_call_offset(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_parameter_name")-1&&
       memcmp(method_name->chars,"set_parameter_name",
           sizeof("set_parameter_name")-1)==0)
        return program_builder_set_parameter_name(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_return_type")-1&&
       memcmp(method_name->chars,"set_return_type",
           sizeof("set_return_type")-1)==0)
        return program_builder_set_return_type(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_class_type_parameters")-1&&
       memcmp(method_name->chars,"set_class_type_parameters",
           sizeof("set_class_type_parameters")-1)==0)
        return program_builder_set_class_type_parameters(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_parent_type_arguments")-1&&
       memcmp(method_name->chars,"set_parent_type_arguments",
           sizeof("set_parent_type_arguments")-1)==0)
        return program_builder_set_parent_type_arguments(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("set_captures_type_variables")-1&&
       memcmp(method_name->chars,"set_captures_type_variables",
           sizeof("set_captures_type_variables")-1)==0)
        return program_builder_set_captures_type_variables(built,registers,base,argc,result);
    if(method_name->length==sizeof("native_type_id")-1&&
       memcmp(method_name->chars,"native_type_id",sizeof("native_type_id")-1)==0)
        return program_builder_native_type_id(registers,base,argc,result);
    if(method_name->length==sizeof("set_type_variables")-1&&
       memcmp(method_name->chars,"set_type_variables",
           sizeof("set_type_variables")-1)==0)
        return program_builder_set_type_variables(built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_interface")-1&&
       memcmp(method_name->chars,"declare_interface",
           sizeof("declare_interface")-1)==0)
        return program_builder_declare_interface(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("inherit_interface")-1&&
       memcmp(method_name->chars,"inherit_interface",
           sizeof("inherit_interface")-1)==0)
        return program_builder_inherit_interface(vm,built,registers,base,argc,result);
    if(method_name->length==sizeof("declare_interface_method")-1&&
       memcmp(method_name->chars,"declare_interface_method",
           sizeof("declare_interface_method")-1)==0)
        return program_builder_declare_interface_method(built,registers,base,argc,result);
    if(method_name->length==sizeof("run")-1&&
        memcmp(method_name->chars,"run",sizeof("run")-1)==0)
        return program_builder_run(vm,builder,argc,depth,result);
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"ProgramBuilder");
    return DIAMOND_VM_TYPE_ERROR;
}

/* ClassName.compile_method(name, params, body_source) -- see
 * docs/design.md's "Runtime method synthesis" section for the full
 * design. Validates every identifier that gets spliced into the
 * synthesized source itself (name, each param); body_source is not
 * restricted, since letting it be arbitrary Diamond source is the whole
 * point -- same trust level as Ruby's class_eval/define_method, not
 * something to sandbox against here. */
static bool is_valid_identifier(const char *chars,size_t length) {
    if(length==0)return false;
    if(!(isalpha((unsigned char)chars[0])||chars[0]=='_'))return false;
    for(size_t index=1;index<length;index++)
        if(!(isalnum((unsigned char)chars[index])||chars[index]=='_'))return false;
    return true;
}

/* A method name as `def` accepts one: an identifier, optionally ending in
 * one `?` (predicate), `!`, or `=` (writer, as `obj.name = value` calls). */
static bool is_valid_method_name(const char *chars,size_t length) {
    if(length>1&&(chars[length-1]=='?'||chars[length-1]=='!'||chars[length-1]=='='))
        length--;
    return is_valid_identifier(chars,length);
}

bool grow_buffer_append(GrowBuffer *buffer,const char *text,size_t text_length) {
    size_t needed;
    if(ckd_add(&needed,buffer->length,text_length)||ckd_add(&needed,needed,1))return false;
    if(needed>buffer->capacity) {
        size_t new_capacity=buffer->capacity==0?256:buffer->capacity;
        while(new_capacity<needed)
            if(ckd_mul(&new_capacity,new_capacity,2))return false;
        char *new_data=realloc(buffer->data,new_capacity);
        if(new_data==nullptr)return false;
        buffer->data=new_data;buffer->capacity=new_capacity;
    }
    memcpy(buffer->data+buffer->length,text,text_length);
    buffer->length+=text_length;
    buffer->data[buffer->length]='\0';
    return true;
}

/* Synthesizes "class <Name>\n  attr_accessor <fields>\n  def <method_name>
 * (<params>)\n<body_source>\n  end\nend\n" -- the attr_accessor line
 * (skipped when the class has no fields) re-declares target_class's own
 * existing field names, in order, using the compiler's completely
 * unmodified @ivar auto-declare-on-first-reference machinery to seed an
 * identically-shaped field table in the throwaway program compiled
 * below, rather than adding any new compiler entry point for this. */
/* `bound_values_hash` is nullptr-safe (an empty bound_values Hash still
 * has a live, zero-count DiamondHash object -- this file never passes
 * nullptr here, but treating it as "no bound values" defensively costs
 * nothing). Each key becomes an extra trailing parameter name after
 * params_array's own, in the same order compile_method_helper iterates
 * the same hash separately to collect the *values* -- the two orders
 * must match, so both walk `hash->entries[0..count)` directly rather
 * than one of them going through some other iteration order. */
static bool build_compiled_method_source(GrowBuffer *buffer,const DiamondClass *target_class,
        const DiamondString *name_string,const DiamondArray *params_array,
        const DiamondString *body_string,const DiamondHash *bound_values_hash,
        DiamondVm *vm,bool *validation_failed) {
    *validation_failed=false;
    if(!is_valid_method_name(name_string->chars,name_string->length)) {
        snprintf(vm->error,sizeof vm->error,"compile_method name must be a valid method name");
        *validation_failed=true;return false;
    }
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"class "))return false;
    const size_t class_name_length=strlen(target_class->name);
    if(!grow_buffer_append(buffer,target_class->name,class_name_length))return false;
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\n"))return false;
    if(target_class->field_count>0) {
        if(!GROW_BUFFER_APPEND_LITERAL(buffer,"  attr_accessor "))return false;
        for(size_t index=0;index<target_class->field_count;index++) {
            if(index>0&&!GROW_BUFFER_APPEND_LITERAL(buffer,", "))return false;
            const size_t field_length=strlen(target_class->fields[index]);
            if(!grow_buffer_append(buffer,target_class->fields[index],field_length))return false;
        }
        if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\n"))return false;
    }
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"  def "))return false;
    if(!grow_buffer_append(buffer,name_string->chars,name_string->length))return false;
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"("))return false;
    bool any_param=false;
    for(size_t index=0;index<params_array->count;index++) {
        const DiamondValue param=params_array->values[index];
        if(param.kind!=DIAMOND_VALUE_OBJECT||param.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"compile_method params must be an Array of Strings");
            *validation_failed=true;return false;
        }
        const DiamondString *param_string=(const DiamondString *)param.as.object;
        if(!is_valid_identifier(param_string->chars,param_string->length)) {
            snprintf(vm->error,sizeof vm->error,
                "compile_method param names must be valid identifiers");
            *validation_failed=true;return false;
        }
        if(any_param&&!GROW_BUFFER_APPEND_LITERAL(buffer,", "))return false;
        if(!grow_buffer_append(buffer,param_string->chars,param_string->length))return false;
        any_param=true;
    }
    for(size_t index=0;index<bound_values_hash->count;index++) {
        const DiamondValue key=bound_values_hash->entries[index].key;
        if(key.kind!=DIAMOND_VALUE_OBJECT||key.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "compile_method bound_values keys must be Strings");
            *validation_failed=true;return false;
        }
        const DiamondString *key_string=(const DiamondString *)key.as.object;
        if(!is_valid_identifier(key_string->chars,key_string->length)) {
            snprintf(vm->error,sizeof vm->error,
                "compile_method bound_values keys must be valid identifiers");
            *validation_failed=true;return false;
        }
        if(any_param&&!GROW_BUFFER_APPEND_LITERAL(buffer,", "))return false;
        if(!grow_buffer_append(buffer,key_string->chars,key_string->length))return false;
        any_param=true;
    }
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,")\n"))return false;
    if(!grow_buffer_append(buffer,body_string->chars,body_string->length))return false;
    return GROW_BUFFER_APPEND_LITERAL(buffer,"\n  end\nend\n");
}

/* Finds the DiamondClass this compile compiled (by name -- the prelude
 * already declares many classes of its own, so the synthesized class
 * is not reliably at any fixed index) and validates its field_count
 * exactly matches target_class's own -- the one check that actually
 * proves body_source didn't reference/auto-declare any field beyond
 * the ones build_compiled_method_source seeded via attr_accessor, i.e.
 * that every @field offset the compiled bytecode assumes is valid
 * against target_class's real instances. */
static const DiamondClass *find_matching_synthetic_class(const DiamondChunk *synthetic_chunk,
        const DiamondClass *target_class,DiamondVm *vm) {
    const size_t class_name_length=strlen(target_class->name);
    for(size_t index=0;index<synthetic_chunk->class_count;index++) {
        const DiamondClass *candidate=synthetic_chunk->classes[index];
        if(strlen(candidate->name)==class_name_length&&
           memcmp(candidate->name,target_class->name,class_name_length)==0) {
            if(candidate->field_count!=target_class->field_count) {
                snprintf(vm->error,sizeof vm->error,
                    "compile_method body references a field '%s' does not have",
                    target_class->name);
                return nullptr;
            }
            return candidate;
        }
    }
    snprintf(vm->error,sizeof vm->error,
        "compile_method: internal error locating compiled class '%s'",target_class->name);
    return nullptr;
}

DiamondVmStatus compile_method_helper(DiamondVm *vm,const DiamondClass *target_class,
        const DiamondString *name_string,const DiamondArray *params_array,
        const DiamondString *body_string,const DiamondHash *bound_values_hash,
        DiamondValue *out_result) {
    if(name_string->length==0||name_string->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method name must be 1 to %d characters",DIAMOND_MAX_FUNCTION_NAME-1);
        return DIAMOND_VM_ARITY_ERROR;
    }
    if(bound_values_hash->count>DIAMOND_MAX_BOUND_VALUES) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method bound_values must have at most %d entries",
            DIAMOND_MAX_BOUND_VALUES);
        return DIAMOND_VM_ARITY_ERROR;
    }
    GrowBuffer source={0};
    bool validation_failed=false;
    if(!build_compiled_method_source(&source,target_class,name_string,params_array,
            body_string,bound_values_hash,vm,&validation_failed)) {
        free(source.data);
        if(validation_failed)return DIAMOND_VM_ARITY_ERROR;
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    const bool include_json=diamond_prelude_needs_json(source.data);
    const size_t prelude_length=diamond_prelude_length(include_json);
    char *combined=malloc(prelude_length+source.length+1);
    if(combined==nullptr) {
        free(source.data);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    const size_t offset=diamond_prelude_write(combined,include_json);
    memcpy(combined+offset,source.data,source.length+1);
    free(source.data);
    DiamondProgram *program=calloc(1,sizeof *program);
    if(program==nullptr) {
        free(combined);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    diamond_program_init(program);
    DiamondDiagnostic diagnostic;
    const bool compiled=diamond_compile(combined,program,&diagnostic);
    free(combined);
    if(!compiled) {
        snprintf(vm->error,sizeof vm->error,"compile_method: %.1007s",
            diagnostic.message);
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondChunk synthetic_chunk=diamond_program_chunk(program);
    const DiamondClass *synthetic_class=
        find_matching_synthetic_class(&synthetic_chunk,target_class,vm);
    if(synthetic_class==nullptr) {
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondMethod *synthetic_method=nullptr;
    for(size_t index=0;index<synthetic_class->method_count;index++)
        if(strlen(synthetic_class->methods[index].name)==name_string->length&&
           memcmp(synthetic_class->methods[index].name,name_string->chars,
                  name_string->length)==0)
            synthetic_method=&synthetic_class->methods[index];
    if(synthetic_method==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method: internal error locating compiled method '%.*s'",
            (int)name_string->length,name_string->chars);
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const uint16_t function_index=synthetic_method->function_index;
    /* Collected in the same order build_compiled_method_source walked
     * bound_values_hash->entries to emit their names as trailing
     * parameters -- see that function's own comment on why both must
     * agree. */
    DiamondValue bound_values[DIAMOND_MAX_BOUND_VALUES];
    const uint8_t bound_value_count=(uint8_t)bound_values_hash->count;
    for(uint8_t index=0;index<bound_value_count;index++)
        bound_values[index]=bound_values_hash->entries[index].value;
    DiamondAdoptedProgram *adopted=
        adopt_program(vm,program,bound_values,bound_value_count);
    if(adopted==nullptr) {
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondClosure *closure=allocate_closure(vm,function_index,nullptr,0);
    if(closure==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    closure->foreign_chunk=&adopted->chunk;
    closure->intended_class=target_class;
    closure->bound_values=adopted->bound_values;
    closure->bound_value_count=bound_value_count;
    *out_result=DIAMOND_OBJECT(closure);
    return DIAMOND_VM_OK;
}

/* Whether `methods` already holds a method defined directly on the class or
 * module (not copied in by an earlier include) under `name`. An include skips
 * such names so a class's own method always beats an included module's, as in
 * the compiler's own `include` handling (has_own_method_named there). */
bool methods_have_own_named(const DiamondMethod *methods,size_t count,
        const char *name) {
    for(size_t index=0;index<count;index++)
        if(!methods[index].included&&strcmp(methods[index].name,name)==0)
            return true;
    return false;
}
