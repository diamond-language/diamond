#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE): only for pthread_getattr_np,
 * used to learn a thread's own native stack bounds for the ASan
 * fiber-switch annotations below. */
#define _GNU_SOURCE

#include "vm.h"
#include "jit.h"
#include "bignum.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"

/* <crypt.h> exists on glibc (libxcrypt) and musl (see BCrypt.hash's own
 * comment below for what musl's version lacks), declaring crypt_r/
 * struct crypt_data/CRYPT_GENSALT_* -- but not on FreeBSD, which declares
 * plain crypt()/crypt_r() directly in <unistd.h> (already included below)
 * instead, with no separate header at all. __has_include, not an
 * __APPLE__/__FreeBSD__-style OS check (see docs/portability.md's own
 * "What hasn't been found" on why this codebase avoids those): this is a
 * feature test, and the same reasoning applies wherever else a libc omits
 * this header. */
#if __has_include(<crypt.h>)
#include <crypt.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sqlite3.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <zlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vm_internal.h"

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
    return &program->classes[class_index];
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
 * ~20KB (method/field caches, opcode
 * counters, namespace constants), not the ~100 bytes regexp_new_helper's
 * own locals needed. At -O0, every local anywhere in run_chunk's switch
 * contributes to its one shared stack frame regardless of which case
 * actually runs, so leaving `DiamondVm run_vm` inline in the INVOKE case
 * body would have added that ~50KB to *every* recursive run_chunk level
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
        built->interfaces[index].type_sets=built->entry.type_sets;
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
    DiamondVm run_vm;diamond_vm_init(&run_vm);
    DiamondValue run_result=DIAMOND_NIL;
    const DiamondVmStatus run_status=diamond_vm_run(&run_vm,&built_chunk,&run_result);
    if(run_status!=DIAMOND_VM_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",
            run_vm.error[0]!='\0'?run_vm.error:diamond_vm_status_name(run_status));
        diamond_vm_free(&run_vm);
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondValue copied_result=DIAMOND_NIL;
    const DiamondChunk *adopted_owner=nullptr;
    if(!copy_value_into_vm(vm,run_result,built,nullptr,nullptr,&adopted_owner,&copied_result)) {
        diamond_vm_free(&run_vm);
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "run does not support this result type");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(adopted_owner!=nullptr)builder->program=nullptr;
    diamond_vm_free(&run_vm);
    *result=copied_result;
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
DiamondVmStatus program_builder_invoke_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, const DiamondStringConstant *method_name,
        DiamondValue *registers, uint16_t base, uint8_t argc, size_t depth,
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
     * (overwrite an already-emitted byte, never append). */
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
     * needed them). */
    const bool declare_class_method=
        method_name->length==sizeof("declare_class")-1&&
        memcmp(method_name->chars,"declare_class",sizeof("declare_class")-1)==0;
    const bool declare_module_method=
        method_name->length==sizeof("declare_module")-1&&
        memcmp(method_name->chars,"declare_module",sizeof("declare_module")-1)==0;
    const bool declare_namespace_constant_method=
        method_name->length==sizeof("declare_namespace_constant")-1&&
        memcmp(method_name->chars,"declare_namespace_constant",
            sizeof("declare_namespace_constant")-1)==0;
    const bool declare_field_method=
        method_name->length==sizeof("declare_field")-1&&
        memcmp(method_name->chars,"declare_field",sizeof("declare_field")-1)==0;
    /* Same find-or-create-by-name shape as declare_field immediately
     * above, targeting class->class_variables[]/class_variable_count
     * (src/compiler.c's own class_variable_index uses the same storage
     * natively) instead of class->fields[]/field_count -- a class
     * variable is per-class state, not part of an instance's own field
     * layout, so unlike declare_field this never touches
     * program_builder_recompute_shapes. */
    const bool declare_class_variable_method=
        method_name->length==sizeof("declare_class_variable")-1&&
        memcmp(method_name->chars,"declare_class_variable",
            sizeof("declare_class_variable")-1)==0;
    const bool declare_module_field_method=
        method_name->length==sizeof("declare_module_field")-1&&
        memcmp(method_name->chars,"declare_module_field",
            sizeof("declare_module_field")-1)==0;
    const bool declare_method_method=
        method_name->length==sizeof("declare_method")-1&&
        memcmp(method_name->chars,"declare_method",sizeof("declare_method")-1)==0;
    const bool declare_module_method_method=
        method_name->length==sizeof("declare_module_method")-1&&
        memcmp(method_name->chars,"declare_module_method",
            sizeof("declare_module_method")-1)==0;
    /* Declares a function directly as a class/module singleton method
     * (`def self.foo` syntax) -- distinct from export_module_method,
     * which instead re-exports an *already-declared* regular method
     * (the `module_function :name` syntax). Neither has a needs_receiver
     * counterpart here: diamond_compile's own module_singleton branch
     * (src/compiler.c's compile_definition) never sets it either, since
     * a directly-declared singleton never reserves register 0 for an
     * implicit self the way an ordinary method does. */
    const bool declare_class_singleton_method_method=
        method_name->length==sizeof("declare_class_singleton_method")-1&&
        memcmp(method_name->chars,"declare_class_singleton_method",
            sizeof("declare_class_singleton_method")-1)==0;
    const bool declare_module_singleton_method_method=
        method_name->length==sizeof("declare_module_singleton_method")-1&&
        memcmp(method_name->chars,"declare_module_singleton_method",
            sizeof("declare_module_singleton_method")-1)==0;
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
    const bool set_function_owner_class_method=
        method_name->length==sizeof("set_function_owner_class")-1&&
        memcmp(method_name->chars,"set_function_owner_class",
            sizeof("set_function_owner_class")-1)==0;
    const bool include_module_method=
        method_name->length==sizeof("include_module")-1&&
        memcmp(method_name->chars,"include_module",sizeof("include_module")-1)==0;
    const bool include_module_in_module_method=
        method_name->length==sizeof("include_module_in_module")-1&&
        memcmp(method_name->chars,"include_module_in_module",
            sizeof("include_module_in_module")-1)==0;
    const bool set_module_method_visibility_method=
        method_name->length==sizeof("set_module_method_visibility")-1&&
        memcmp(method_name->chars,"set_module_method_visibility",
            sizeof("set_module_method_visibility")-1)==0;
    /* Named `private`/`public` visibility targets (`private foo, bar`)
     * for a class -- the class-side counterpart to
     * set_module_method_visibility, retroactively flipping an
     * already-declared method's is_private flag by name. */
    const bool set_class_method_visibility_method=
        method_name->length==sizeof("set_class_method_visibility")-1&&
        memcmp(method_name->chars,"set_class_method_visibility",
            sizeof("set_class_method_visibility")-1)==0;
    /* `alias_method new_name, existing_name` (compiler.c's
     * compile_alias_method): copies an already-declared method's
     * DiamondMethod struct verbatim under a new name -- same
     * function_index/arity/required_arity/is_private, just re-registered
     * so a second name can call the identical implementation. The
     * caller (selfhost/parser.di) is expected to have already appended
     * "=" to either name string when aliasing a writer method, the same
     * way declare_method's own name argument already does. */
    const bool alias_class_method_method=
        method_name->length==sizeof("alias_class_method")-1&&
        memcmp(method_name->chars,"alias_class_method",
            sizeof("alias_class_method")-1)==0;
    const bool alias_module_method_method=
        method_name->length==sizeof("alias_module_method")-1&&
        memcmp(method_name->chars,"alias_module_method",
            sizeof("alias_module_method")-1)==0;
    const bool export_module_method_method=
        method_name->length==sizeof("export_module_method")-1&&
        memcmp(method_name->chars,"export_module_method",
            sizeof("export_module_method")-1)==0;
    const bool expand_source_method=
        method_name->length==sizeof("expand_source")-1&&
        memcmp(method_name->chars,"expand_source",sizeof("expand_source")-1)==0;
    const bool source_location_method=
        method_name->length==sizeof("source_location")-1&&
        memcmp(method_name->chars,"source_location",sizeof("source_location")-1)==0;
    const bool set_source_location_method=
        method_name->length==sizeof("set_source_location")-1&&
        memcmp(method_name->chars,"set_source_location",
            sizeof("set_source_location")-1)==0;
    /* Phase 3 sub-phase 4 (gradual typing): a scalar or union type set.
     * Nested Array[T]/Hash[K,V]/Callable/interfaces/generics remain
     * separate future extensions. */
    const bool declare_type_set_method=
        method_name->length==sizeof("declare_type_set")-1&&
        memcmp(method_name->chars,"declare_type_set",
            sizeof("declare_type_set")-1)==0;
    const bool set_parameter_type_method=
        method_name->length==sizeof("set_parameter_type")-1&&
        memcmp(method_name->chars,"set_parameter_type",
            sizeof("set_parameter_type")-1)==0;
    const bool set_return_type_method=
        method_name->length==sizeof("set_return_type")-1&&
        memcmp(method_name->chars,"set_return_type",
            sizeof("set_return_type")-1)==0;
    const bool declare_interface_method=
        method_name->length==sizeof("declare_interface")-1&&
        memcmp(method_name->chars,"declare_interface",
            sizeof("declare_interface")-1)==0;
    const bool declare_interface_method_method=
        method_name->length==sizeof("declare_interface_method")-1&&
        memcmp(method_name->chars,"declare_interface_method",
            sizeof("declare_interface_method")-1)==0;
    const bool set_type_variables_method=
        method_name->length==sizeof("set_type_variables")-1&&
        memcmp(method_name->chars,"set_type_variables",
            sizeof("set_type_variables")-1)==0;
    const bool inherit_interface_method=
        method_name->length==sizeof("inherit_interface")-1&&
        memcmp(method_name->chars,"inherit_interface",
            sizeof("inherit_interface")-1)==0;
    const bool run_method=method_name->length==sizeof("run")-1&&
        memcmp(method_name->chars,"run",sizeof("run")-1)==0;
    if(!declare_function_method&&!emit_byte_method&&!patch_byte_method&&
       !add_constant_method&&!add_string_method&&
       !set_register_count_method&&!declare_class_method&&
       !declare_module_method&&!declare_namespace_constant_method&&
       !declare_field_method&&!declare_class_variable_method&&
       !declare_module_field_method&&!declare_method_method&&
       !declare_module_method_method&&
       !declare_class_singleton_method_method&&
       !declare_module_singleton_method_method&&!set_function_owner_class_method&&
       !include_module_method&&
       !include_module_in_module_method&&
       !set_module_method_visibility_method&&!set_class_method_visibility_method&&
       !alias_class_method_method&&!alias_module_method_method&&
       !export_module_method_method&&
       !expand_source_method&&!source_location_method&&!set_source_location_method&&
       !declare_type_set_method&&!set_parameter_type_method&&
       !set_return_type_method&&!declare_interface_method&&
       !declare_interface_method_method&&!set_type_variables_method&&
       !inherit_interface_method&&!run_method) {
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
        DiamondFunction *function=diamond_program_add_function(built);
        if(function==nullptr) {
            snprintf(vm->error,sizeof vm->error,"program function allocation failed");
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        vm->bytes_allocated+=sizeof *function+sizeof function;
        *function=(DiamondFunction){};
        memcpy(function->name,fname->chars,fname->length);
        function->name[fname->length]='\0';
        function->owner_class=UINT8_MAX;
        function->arity=(uint8_t)arity_value;
        function->required_arity=(uint8_t)required_value;
        function->return_type_set=DIAMOND_NO_TYPE_SET;
        for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
            function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
        const int64_t new_index=(int64_t)built->function_count-1;
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
    if(declare_module_method) {
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
            if(strlen(built->modules[index].name)==name->length&&
               memcmp(built->modules[index].name,name->chars,name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"module name is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        const int64_t index=(int64_t)built->module_count;
        DiamondModule *module=&built->modules[built->module_count++];
        *module=(DiamondModule){};
        memcpy(module->name,name->chars,name->length);
        module->name[name->length]='\0';
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
    }
    if(declare_namespace_constant_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_namespace_constant argument must be String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *name=(const DiamondString *)registers[base].as.object;
        if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
           built->namespace_constant_count==DIAMOND_MAX_NAMESPACE_CONSTANTS) {
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
    if(declare_class_variable_method) {
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
        if(class->class_variable_count==DIAMOND_MAX_FIELDS) {
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
    if(declare_module_field_method) {
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
        DiamondModule *module=&built->modules[(size_t)module_index];
        for(size_t index=0;index<module->field_count;index++)
            if(strlen(module->fields[index])==name->length&&
               memcmp(module->fields[index],name->chars,name->length)==0) {
                *result=DIAMOND_INT((int64_t)index);return DIAMOND_VM_OK;
            }
        if(module->field_count==DIAMOND_MAX_FIELDS)return DIAMOND_VM_TYPE_ERROR;
        const int64_t index=(int64_t)module->field_count;
        memcpy(module->fields[module->field_count],name->chars,name->length);
        module->fields[module->field_count][name->length]='\0';
        module->field_count++;
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
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
        method->function_index=(uint16_t)target_function;
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
        built->functions[target_function]->owner_class=(uint8_t)registers[base].as.integer;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_module_method_method) {
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
           arity<0||arity>UINT8_MAX||required<0||required>arity)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondModule *module=&built->modules[(size_t)module_index];
        for(size_t index=0;index<module->method_count;index++)
            if(!module->methods[index].included&&
               strlen(module->methods[index].name)==name->length&&
               memcmp(module->methods[index].name,name->chars,name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"method is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(module->method_count==DIAMOND_MAX_METHODS)return DIAMOND_VM_TYPE_ERROR;
        DiamondMethod *method=&module->methods[module->method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,name->chars,name->length);
        method->name[name->length]='\0';
        method->function_index=(uint16_t)function_index;
        method->arity=(uint8_t)arity;
        method->required_arity=(uint8_t)required;
        method->is_private=registers[(size_t)base+5].as.boolean;
        method->is_protected=false;
        /* Same owner_class fix as declare_method just above, using
         * diamond_compile's own module-method sentinel (UINT8_MAX-1,
         * distinct from UINT8_MAX's "not a method at all" so the private-
         * bypass's parameter_offset==1 check still fires for module
         * methods too). */
        built->functions[function_index]->owner_class=UINT8_MAX-1;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_class_singleton_method_method) {
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
           arity_value<0||arity_value>UINT8_MAX||
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
        if(class->singleton_method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,
                "duplicate or excessive class singleton method");
            return DIAMOND_VM_TYPE_ERROR;
        }
        /* No needs_receiver/owner_class here, matching diamond_compile's
         * own module_singleton branch exactly: a directly-declared
         * singleton (`def self.foo`) never reserves register 0 for an
         * implicit self, unlike an ordinary method -- see declare_method
         * just above for the contrasting case that does. */
        DiamondMethod *method=
            &class->singleton_methods[class->singleton_method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,mname->chars,mname->length);
        method->name[mname->length]='\0';
        method->function_index=(uint16_t)target_function;
        method->arity=(uint8_t)arity_value;
        method->required_arity=(uint8_t)required_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_module_singleton_method_method) {
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
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_module_singleton_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondModule *module=&built->modules[(size_t)module_index];
        for(size_t index=0;index<module->singleton_method_count;index++)
            if(strlen(module->singleton_methods[index].name)==mname->length&&
               memcmp(module->singleton_methods[index].name,mname->chars,
                      mname->length)==0) {
                snprintf(vm->error,sizeof vm->error,
                    "duplicate or excessive module singleton function");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(module->singleton_method_count==DIAMOND_MAX_METHODS) {
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
        method->arity=(uint8_t)arity_value;
        method->required_arity=(uint8_t)required_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(set_function_owner_class_method) {
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
           owner_class<0||owner_class>UINT8_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_function_owner_class has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        built->functions[function_index]->owner_class=(uint8_t)owner_class;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(include_module_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondClass *class=program_builder_class(built,registers[base].as.integer);
        const int64_t module_index=registers[(size_t)base+1].as.integer;
        if(class==nullptr||module_index<0||
           (uint64_t)module_index>=built->module_count)return DIAMOND_VM_TYPE_ERROR;
        const DiamondModule *module=&built->modules[(size_t)module_index];
        if(class->field_count+module->field_count>DIAMOND_MAX_FIELDS||
           class->method_count+module->method_count>DIAMOND_MAX_METHODS)
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
    if(include_module_in_module_method) {
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
        DiamondModule *target=&built->modules[(size_t)target_index];
        const DiamondModule *source=&built->modules[(size_t)source_index];
        if(target->field_count+source->field_count>DIAMOND_MAX_FIELDS||
           target->method_count+source->method_count>DIAMOND_MAX_METHODS)
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
    if(set_module_method_visibility_method) {
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
        DiamondModule *module=&built->modules[(size_t)module_index];
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
    if(set_class_method_visibility_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
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
        found->is_protected=false;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(alias_class_method_method) {
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
        if(class->method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,"too many methods");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod copied=*source;
        memcpy(copied.name,alias_name->chars,alias_name->length);
        copied.name[alias_name->length]='\0';copied.included=false;
        class->methods[class->method_count++]=copied;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(alias_module_method_method) {
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
        DiamondModule *module=&built->modules[(size_t)module_index];
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
        if(module->method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,"too many methods");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod copied=*source;
        memcpy(copied.name,alias_name->chars,alias_name->length);
        copied.name[alias_name->length]='\0';copied.included=false;
        module->methods[module->method_count++]=copied;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(export_module_method_method) {
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
        DiamondModule *module=&built->modules[(size_t)module_index];
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
        if(module->singleton_method_count==DIAMOND_MAX_METHODS)
            return DIAMOND_VM_TYPE_ERROR;
        source->is_private=true;source->is_protected=false;
        DiamondMethod exported=*source;
        exported.is_private=false;exported.is_protected=false;
        exported.needs_receiver=true;
        module->singleton_methods[module->singleton_method_count++]=exported;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(expand_source_method) {
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
         * script could read any file whose name ends in `.di` (any absolute
         * or `../` path, or a symlink named so), and learn which paths exist
         * from the error text, while File.open and File.read are denied.
         * The check is the same as VM_SANDBOX_GUARD's, which only works
         * inside run_chunk. */
        if(getenv("DIAMOND_SANDBOX")!=nullptr&&!sandbox_category_allowed("filesystem")) {
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
    if(source_location_method) {
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
    if(set_source_location_method) {
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
    if(declare_type_set_method) {
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
                (argument_set==-1&&second_argument_set==-1);
            const bool callable_arguments=type_id!=DIAMOND_TYPE_CALLABLE?
                callable_arity==-1&&callable_return==-1&&
                    callable_parameters->count==0:
                callable_arity>=0&&callable_arity<=16&&valid_return&&
                    valid_parameters&&
                    (callable_parameters->count==0||
                     callable_parameters->count==(size_t)callable_arity);
            if(!(primitive||class_type||interface_type||variable_type)||
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
            set->members[member]=(DiamondTypeMember){
                .id=(uint8_t)descriptor->values[0].as.integer,
                .argument_set=argument_set<0?DIAMOND_NO_TYPE_SET:(uint16_t)argument_set,
                .second_argument_set=second_argument_set<0?DIAMOND_NO_TYPE_SET:
                    (uint16_t)second_argument_set,
                .callable_arity=callable_arity<0?UINT8_MAX:(uint8_t)callable_arity,
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
    if(set_parameter_type_method) {
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
    if(set_return_type_method) {
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
    if(set_type_variables_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const DiamondArray *variables=
            (const DiamondArray *)registers[(size_t)base+1].as.object;
        if(target==nullptr||variables->count>8)return DIAMOND_VM_TYPE_ERROR;
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
    if(declare_interface_method) {
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
        DiamondInterface *interface=&built->interfaces[built->interface_count++];
        *interface=(DiamondInterface){.type_sets=built->entry.type_sets};
        memcpy(interface->name,name->chars,name->length);
        interface->name[name->length]='\0';
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
    }
    if(inherit_interface_method) {
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
        DiamondInterface *target=&built->interfaces[(size_t)target_index];
        const DiamondInterface *source=&built->interfaces[(size_t)base_index];
        if(target->method_count+source->method_count>DIAMOND_MAX_METHODS)
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
    if(declare_interface_method_method) {
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
        DiamondInterface *interface=&built->interfaces[(size_t)interface_index];
        if(interface->method_count==DIAMOND_MAX_METHODS)return DIAMOND_VM_TYPE_ERROR;
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
     * verified safe under ASan even if the inner program recurses to its
     * own full limit. */
    if(depth>=10) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#run nested too deeply");
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    return program_builder_run_helper(vm,builder,result);
}
