#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiler_internal.h"
#include "jit.h"
#include "interface_inference.h"

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

/* The core of the one-pass compiler: emission, type sets, name and constant lookups, literals and
 * statement sequencing. Calls, expressions, control flow and assignment live in the compiler_*.c files
 * beside it; what they share is compiler_internal.h. */

static bool type_member_satisfies(const Compiler *compiler,
                                  DiamondTypeMember known,
                                  DiamondTypeMember expected);

void fail(Compiler *compiler, DiamondSpan span, const char *message) {
    if (!compiler->failed) {
        compiler->diagnostic->span = span;
        snprintf(compiler->diagnostic->message,
            sizeof compiler->diagnostic->message,"%s",message);
        compiler->failed = true;
    }
}

const char *closed_type_name(const Compiler *compiler,uint16_t id) {
    if(id==DIAMOND_TYPE_NIL)return "Nil";
    if(id>=DIAMOND_TYPE_CLASS_BASE&&id<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t index=(size_t)(id-DIAMOND_TYPE_CLASS_BASE);
        if(index<compiler->program->class_count)
            return compiler->program->classes[index]->name;
    }
    return "unknown";
}

void advance_token(Compiler *compiler) {
    compiler->previous = compiler->current;
    compiler->current = diamond_lexer_next(&compiler->lexer);
    if (compiler->current.kind == DIAMOND_TOKEN_ERROR) {
        fail(compiler, compiler->current.span, "unexpected character");
    }
}

void skip_newlines(Compiler *compiler) {
    while (compiler->current.kind == DIAMOND_TOKEN_NEWLINE) {
        advance_token(compiler);
    }
}

bool emit_byte(Compiler *compiler, uint8_t byte) {
    if (compiler->function->code_count == DIAMOND_MAX_CODE) {
        fail(compiler, compiler->previous.span, "program produces too much bytecode");
        return false;
    }
    if(compiler->function->code_count==compiler->function->code_capacity) {
        size_t capacity=compiler->function->code_capacity==0?256:
            compiler->function->code_capacity*2;
        if(capacity>DIAMOND_MAX_CODE)capacity=DIAMOND_MAX_CODE;
        if(!diamond_function_reserve_code(compiler->function,capacity)) {
            fail(compiler,compiler->previous.span,
                "out of memory growing function bytecode");
            return false;
        }
    }
    compiler->function->code[compiler->function->code_count++] = byte;
    return true;
}

static bool is_call_opcode(DiamondOpCode opcode) {
    switch(opcode) {
        case DIAMOND_OP_CALL: case DIAMOND_OP_CALL_TYPED: case DIAMOND_OP_CALL_CLOSURE:
        case DIAMOND_OP_NEW: case DIAMOND_OP_INVOKE: case DIAMOND_OP_INVOKE_MONO:
        case DIAMOND_OP_INVOKE_TYPED: case DIAMOND_OP_SUPER:
        case DIAMOND_OP_INVOKE_SELF_METHOD: case DIAMOND_OP_CALL_SPREAD:
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_CALL_CLOSURE_SPREAD:
        case DIAMOND_OP_NEW_SPREAD: case DIAMOND_OP_CALL_SINGLETON_SPREAD:
        case DIAMOND_OP_CALL_TYPED_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD:
        case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: case DIAMOND_OP_CALL_KEYWORD_SPREAD:
        case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD: case DIAMOND_OP_INVOKE_KEYWORDS:
        case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: case DIAMOND_OP_CALL_CLOSURE_KEYWORDS:
        case DIAMOND_OP_NEW_TYPED: case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
        case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS: case DIAMOND_OP_TAIL_CALL:
        case DIAMOND_OP_FIBER_NEW: case DIAMOND_OP_THREAD_NEW:
        case DIAMOND_OP_SUPERVISOR_NEW: case DIAMOND_OP_CHANNEL_NEW:
        case DIAMOND_OP_PROGRAM_BUILDER_NEW: case DIAMOND_OP_REGEXP_NEW:
        case DIAMOND_OP_YIELD:
            return true;
        default:
            return false;
    }
}

bool emit_opcode(Compiler *compiler, DiamondOpCode opcode) {
    const size_t offset = compiler->function->code_count;
    if(compiler->pending_block_call!=nullptr&&is_call_opcode(opcode)) {
        compiler->pending_block_call->block_call_offset=(uint32_t)offset;
        compiler->pending_block_call=nullptr;
    }
    if (!emit_byte(compiler, (uint8_t)opcode)) return false;
    compiler->function->lines[offset] = (uint32_t)compiler->previous.span.line;
    compiler->function->columns[offset] = (uint32_t)compiler->previous.span.column;
    compiler->last_instruction_offset = offset;
    return true;
}

/* Big-endian, matching patch_jump/emit_absolute_jump's existing 16-bit
 * operand convention -- function indices are a CALL/CALL_TYPED/CLOSURE
 * operand wide enough to exceed one byte now that DIAMOND_MAX_FUNCTIONS
 * is 512 (see its own comment in src/vm.h). */
bool emit_function_index(Compiler *compiler, size_t function_index) {
    return emit_byte(compiler, (uint8_t)(function_index >> 8)) &&
           emit_byte(compiler, (uint8_t)(function_index & UINT8_MAX));
}

/* Same big-endian 16-bit convention as emit_function_index, for a register
 * operand -- registers are wide enough to exceed one byte now that
 * DIAMOND_REGISTER_COUNT is 4096 (see its own comment in src/vm.h). */
bool emit_register(Compiler *compiler, uint16_t reg) {
    return emit_byte(compiler, (uint8_t)(reg >> 8)) &&
           emit_byte(compiler, (uint8_t)(reg & UINT8_MAX));
}

/* Every emit_instruction operand is emitted 2 bytes wide via
 * emit_register, even the (more common) ones that are logically a
 * register -- and even the few that are actually a narrower index
 * (a type-set/constant/field/capture index, each with its own separate,
 * much smaller cap; see e.g. DIAMOND_MAX_TYPE_SETS/DIAMOND_MAX_FIELDS in
 * src/vm.h). This opcode operand slot mix varies per opcode, not
 * uniformly by position, so emit_instruction deliberately doesn't try to
 * track which -- uniformly widening every slot keeps this one shared
 * helper (and run_chunk's matching per-opcode reads, see READ_SHORT)
 * simple and consistent, at the cost of a couple of harmless extra bytes
 * per instruction for the operands that didn't strictly need them. */
bool emit_instruction(Compiler *compiler, DiamondOpCode opcode,
                             uint16_t a, uint16_t b, uint16_t c, size_t operands) {
    if (!emit_opcode(compiler, opcode)) {
        return false;
    }
    const uint16_t values[] = {a, b, c};
    for (size_t index = 0; index < operands; index++) {
        if (!emit_register(compiler, values[index])) {
            return false;
        }
    }
    return true;
}

uint16_t allocate_register(Compiler *compiler) {
    if (compiler->next_register >= DIAMOND_REGISTER_COUNT) {
        fail(compiler, compiler->previous.span, "program needs too many registers");
        return 0;
    }
    const uint16_t reg=(uint16_t)compiler->next_register++;
    compiler->known_types[reg]=TYPE_UNKNOWN;
    compiler->known_type_sets[reg]=-1;
    compiler->tooling_type_sets[reg]=-1;
    return reg;
}

/* Specialize a method signature in an owned table. Class arguments come from
 * the receiver's table, never the implementation's variable namespace. */
typedef struct InterfaceTypeArena {
    DiamondTypeSet *sets;
    size_t count,capacity;
    const DiamondFunction *implementation;
    const DiamondTypeSet *arguments;
    size_t argument_count;
    uint16_t argument_sets[2];
    int32_t *signature_map,*argument_map,*method_map;
    const DiamondTypeSet *method_arguments;
    size_t method_argument_count;
    uint16_t method_sets[8];
    struct InterfaceTypeArena *parent;
} InterfaceTypeArena;

static thread_local InterfaceTypeArena *active_interface_arena;

static bool interface_append_member(InterfaceTypeArena *arena,DiamondTypeSet *set,
        DiamondTypeMember member) {
    for(size_t i=0;i<set->count;i++)
        if(type_members_structurally_equal(&set->members[i],arena->sets,arena->count,
                &member,arena->sets,arena->count,0))return true;
    if(set->count==DIAMOND_MAX_UNION_TYPES)return false;
    set->members[set->count++]=member;return true;
}

static uint16_t interface_clone_set(InterfaceTypeArena *arena,uint16_t index,
        unsigned source,size_t depth);

static uint16_t interface_clone_members(InterfaceTypeArena *arena,const DiamondTypeSet *set,
        unsigned source,size_t depth) {
    if(depth>32)return DIAMOND_NO_TYPE_SET;
    const bool signature=source==1;
    DiamondTypeSet result={};
    for(size_t i=0;i<set->count;i++) {
        DiamondTypeMember member=set->members[i];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=member.id-DIAMOND_TYPE_VARIABLE_BASE;
            if(!signature||variable>=arena->implementation->type_variable_count||variable>=8)
                return DIAMOND_NO_TYPE_SET;
            const bool receiver=variable<arena->implementation->class_type_variable;
            if(receiver&&variable>=2)return DIAMOND_NO_TYPE_SET;
            const uint16_t argument=receiver?
                interface_clone_set(arena,arena->argument_sets[variable],0,depth+1):arena->method_sets[variable];
            if(argument==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
            const DiamondTypeSet replacement=arena->sets[argument];
            for(size_t j=0;j<replacement.count;j++)
                if(!interface_append_member(arena,&result,replacement.members[j]))return DIAMOND_NO_TYPE_SET;
            continue;
        }
        uint16_t *children[3]={&member.argument_set,&member.second_argument_set,&member.callable_return_set};
        for(size_t child=0;child<3;child++)if(*children[child]!=DIAMOND_NO_TYPE_SET) {
            *children[child]=interface_clone_set(arena,*children[child],source,depth+1);
            if(*children[child]==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
        }
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                uint16_t *child=&member.callable_parameter_sets[parameter];
                if(*child==DIAMOND_NO_TYPE_SET)continue;
                *child=interface_clone_set(arena,*child,source,depth+1);
                if(*child==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
            }
        if(!interface_append_member(arena,&result,member))return DIAMOND_NO_TYPE_SET;
    }
    if(arena->count==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
    if(arena->count==arena->capacity) {
        size_t capacity=arena->capacity==0?16:arena->capacity*2;
        if(capacity>DIAMOND_NO_TYPE_SET)capacity=DIAMOND_NO_TYPE_SET;
        DiamondTypeSet *grown=realloc(arena->sets,capacity*sizeof *grown);
        if(grown==nullptr)return DIAMOND_NO_TYPE_SET;
        arena->sets=grown;arena->capacity=capacity;
    }
    const uint16_t added=(uint16_t)arena->count++;
    arena->sets[added]=result;return added;
}

static uint16_t interface_clone_set(InterfaceTypeArena *arena,uint16_t index,
        unsigned source,size_t depth) {
    const bool signature=source==1;
    const DiamondTypeSet *sets=source==2?arena->method_arguments:
        signature?arena->implementation->type_sets:arena->arguments;
    const size_t count=source==2?arena->method_argument_count:
        signature?arena->implementation->type_set_count:arena->argument_count;
    int32_t *map=source==2?arena->method_map:signature?arena->signature_map:arena->argument_map;
    if(index==DIAMOND_NO_TYPE_SET||index>=count||depth>32)return DIAMOND_NO_TYPE_SET;
    if(map[index]>=0)return (uint16_t)map[index];
    if(map[index]==-2)return DIAMOND_NO_TYPE_SET;
    map[index]=-2;
    const uint16_t added=interface_clone_members(arena,&sets[index],source,depth);
    if(added!=DIAMOND_NO_TYPE_SET)map[index]=added;
    return added;
}

static size_t interface_argument_count(const Compiler *compiler,const DiamondTypeSet *sets) {
    if(sets==nullptr)return 0;
    for(const InterfaceTypeArena *arena=active_interface_arena;arena!=nullptr;arena=arena->parent)
        if(sets==arena->sets)return arena->count;
    if(sets==compiler->function->type_sets)return compiler->function->type_set_count;
    if(sets==compiler->program->entry.type_sets)return compiler->program->entry.type_set_count;
    for(size_t i=0;i<compiler->program->function_count;i++)
        if(sets==compiler->program->functions[i]->type_sets)
            return compiler->program->functions[i]->type_set_count;
    return 0;
}

/* Only synchronous type comparisons use this guard; it holds no compiler
 * state and remains independent across compilation threads. */
static thread_local unsigned generic_interface_depth;

static bool generic_interfaces_pending(const Compiler *compiler) {
    for(size_t i=0;i<compiler->program->interface_count;i++)
        if(compiler->program->interfaces[i]->declared_by_discovery)return true;
    for(size_t i=0;i<compiler->program->class_count;i++) {
        const DiamondClass *class=compiler->program->classes[i];
        if(class->declared_by_discovery)return true;
        for(size_t method=0;method<class->method_count;method++) {
            const uint16_t index=class->methods[method].function_index;
            if(index>=compiler->program->function_count||
               compiler->program->functions[index]->declared_by_discovery)return true;
        }
    }
    return false;
}

static bool generic_interface_signature(const Compiler *compiler,
        const DiamondFunction *implementation,const DiamondInterfaceMethod *wanted,
        const DiamondTypeSet *arguments,DiamondTypeMember receiver) {
    if(generic_interfaces_pending(compiler)||implementation->type_variable_count>8||
       generic_interface_depth>=32)return false;
    InterfaceTypeArena arena={.implementation=implementation,.arguments=arguments,
        .argument_count=interface_argument_count(compiler,arguments),
        .argument_sets={receiver.argument_set,receiver.second_argument_set},
        .method_arguments=compiler->program->entry.type_sets,
        .method_argument_count=compiler->program->entry.type_set_count};
    InterfaceWitness witnesses[8]={};
    for(size_t i=0;i<8;i++)arena.method_sets[i]=DIAMOND_NO_TYPE_SET;
    for(size_t p=0;p<wanted->arity;p++)
        interface_infer_variables(implementation,implementation->parameter_type_sets[p],
            arena.method_arguments,wanted->parameter_type_sets[p],witnesses,0);
    for(size_t variable=implementation->class_type_variable;
        variable<implementation->type_variable_count;variable++) {
        if(!witnesses[variable].inferred)continue;
        size_t nodes=0;
        if(witnesses[variable].invalid||
           !interface_members_supported(arena.method_arguments,&witnesses[variable].set,&nodes))return false;
    }
    arena.signature_map=malloc(implementation->type_set_count*sizeof *arena.signature_map);
    arena.argument_map=malloc(arena.argument_count*sizeof *arena.argument_map);
    arena.method_map=malloc(arena.method_argument_count*sizeof *arena.method_map);
    if((implementation->type_set_count>0&&arena.signature_map==nullptr)||
       (arena.argument_count>0&&arena.argument_map==nullptr)||
       (arena.method_argument_count>0&&arena.method_map==nullptr)) {
        free(arena.signature_map);free(arena.argument_map);free(arena.method_map);return false;
    }
    for(size_t i=0;i<implementation->type_set_count;i++)arena.signature_map[i]=-1;
    for(size_t i=0;i<arena.argument_count;i++)arena.argument_map[i]=-1;
    for(size_t i=0;i<arena.method_argument_count;i++)arena.method_map[i]=-1;
    arena.parent=active_interface_arena;active_interface_arena=&arena;
    generic_interface_depth++;
    bool found=true;
    for(size_t variable=implementation->class_type_variable;
        variable<implementation->type_variable_count&&found;variable++) {
        if(!witnesses[variable].inferred)continue;
        arena.method_sets[variable]=interface_clone_members(&arena,&witnesses[variable].set,2,0);
        found=arena.method_sets[variable]!=DIAMOND_NO_TYPE_SET;
    }
    for(size_t parameter=0;parameter<wanted->arity&&found;parameter++) {
        const uint16_t required=wanted->parameter_type_sets[parameter];
        const uint16_t actual=implementation->parameter_type_sets[parameter];
        if(required==DIAMOND_NO_TYPE_SET) {found=actual==DIAMOND_NO_TYPE_SET;continue;}
        if(actual==DIAMOND_NO_TYPE_SET)continue;
        const uint16_t specialized=interface_clone_set(&arena,actual,true,0);
        found=specialized!=DIAMOND_NO_TYPE_SET&&type_sets_satisfy_across(compiler,
            compiler->program->entry.type_sets,required,arena.sets,specialized);
    }
    if(found&&wanted->return_type_set!=DIAMOND_NO_TYPE_SET) {
        const uint16_t specialized=interface_clone_set(&arena,implementation->return_type_set,true,0);
        found=specialized!=DIAMOND_NO_TYPE_SET&&type_sets_satisfy_across(compiler,
            arena.sets,specialized,compiler->program->entry.type_sets,wanted->return_type_set);
    }
    active_interface_arena=arena.parent;
    generic_interface_depth--;
    free(arena.sets);free(arena.signature_map);free(arena.argument_map);free(arena.method_map);return found;
}

static bool generic_class_satisfies_interface(const Compiler *compiler,
        const DiamondTypeSet *arguments,DiamondTypeMember receiver,uint16_t expected) {
    if(generic_interface_depth>=32)return false;
    const size_t index=receiver.id-DIAMOND_TYPE_CLASS_BASE;
    const size_t interface_index=expected-DIAMOND_TYPE_INTERFACE_BASE;
    if(index>=compiler->program->class_count||interface_index>=compiler->program->interface_count)return false;
    const DiamondClass *class=compiler->program->classes[index];
    const DiamondInterface *interface=compiler->program->interfaces[interface_index];
    if(generic_interfaces_pending(compiler))return false;
    generic_interface_depth++;
    bool matches=true;
    for(size_t required=0;required<interface->method_count&&matches;required++) {
        const DiamondInterfaceMethod *wanted=&interface->methods[required];
        bool found=false;
        for(size_t method=0;method<class->method_count&&!found;method++) {
            const DiamondMethod *candidate=&class->methods[method];
            if(strcmp(candidate->name,wanted->name)!=0||wanted->arity<candidate->required_arity||
               wanted->arity>candidate->arity||candidate->function_index>=compiler->program->function_count||
               candidate->source_chunk!=nullptr)continue;
            const DiamondFunction *implementation=compiler->program->functions[candidate->function_index];
            if(implementation->class_type_variable!=0&&
               implementation->class_type_variable!=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class))continue;
            found=generic_interface_signature(compiler,implementation,wanted,arguments,receiver);
        }
        matches=found;
    }
    generic_interface_depth--;return matches;
}

bool known_type_satisfies_one(const Compiler *compiler, uint16_t known,
                                     uint16_t expected) {
    if(known==expected) return true;
    if(expected>=DIAMOND_TYPE_VARIABLE_BASE&&
       expected<DIAMOND_TYPE_INTERFACE_BASE)return true;
    if(known>=DIAMOND_TYPE_VARIABLE_BASE&&known<DIAMOND_TYPE_INTERFACE_BASE)
        return false;
    if(expected>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(expected-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=compiler->program->interface_count)return false;
        const DiamondInterface *interface=compiler->program->interfaces[interface_index];
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint16_t native_return=DIAMOND_TYPE_NONE;
                if(!diamond_native_method_satisfies(known,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=DIAMOND_NO_TYPE_SET) {
                    if(native_return==DIAMOND_TYPE_NONE)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
                        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
                    if(!type_sets_satisfy_across(compiler,&native,0,
                        compiler->program->entry.type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(known<DIAMOND_TYPE_CLASS_BASE||known>=DIAMOND_TYPE_INTERFACE_BASE)
            return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;
            size_t class_index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
            while(class_index<compiler->program->class_count&&!found) {
                const DiamondClass *class=compiler->program->classes[class_index];
                /* Class identity alone cannot prove an instance's T. */
                if(class->type_parameter[0]!='\0')return generic_class_satisfies_interface(compiler,nullptr,
                    (DiamondTypeMember){.id=known,.argument_set=DIAMOND_NO_TYPE_SET,
                        .second_argument_set=DIAMOND_NO_TYPE_SET},expected);
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=
                            &interface->methods[required];
                        const DiamondFunction *implementation=
                            compiler->program->functions[class->methods[method].function_index];
                        if(implementation->type_variable_count>0) {
                            found=class->methods[method].source_chunk==nullptr&&
                                generic_interface_signature(compiler,implementation,wanted,nullptr,
                                    (DiamondTypeMember){.argument_set=DIAMOND_NO_TYPE_SET,
                                        .second_argument_set=DIAMOND_NO_TYPE_SET});
                            if(found)break;
                            continue;
                        }
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint16_t required_set=wanted->parameter_type_sets[parameter];
                            const uint16_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==DIAMOND_NO_TYPE_SET) {
                                if(actual_set!=DIAMOND_NO_TYPE_SET)found=false;
                            } else if(actual_set!=DIAMOND_NO_TYPE_SET&&
                                !type_sets_satisfy_across(compiler,
                                    compiler->program->entry.type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=DIAMOND_NO_TYPE_SET&&
                           (implementation->return_type_set==DIAMOND_NO_TYPE_SET||
                            !type_sets_satisfy_across(compiler,
                                implementation->type_sets,
                                implementation->return_type_set,
                                compiler->program->entry.type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                if(found||class->superclass==DIAMOND_NO_CLASS)break;
                class_index=class->superclass;
            }
            if(!found)return false;
        }
        return true;
    }
    if(expected==DIAMOND_TYPE_SIZED) {
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH)return true;
        if(known<DIAMOND_TYPE_CLASS_BASE)return false;
        size_t class_index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
        while(class_index<compiler->program->class_count) {
            const DiamondClass *class=compiler->program->classes[class_index];
            for(size_t index=0;index<class->method_count;index++)
                if(strcmp(class->methods[index].name,"length")==0&&
                   class->methods[index].arity==0)return true;
            if(class->superclass==DIAMOND_NO_CLASS)break;
            class_index=class->superclass;
        }
        return false;
    }
    if(known<DIAMOND_TYPE_CLASS_BASE || expected<DIAMOND_TYPE_CLASS_BASE)
        return false;
    size_t class_index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
    const size_t wanted=(size_t)(expected-DIAMOND_TYPE_CLASS_BASE);
    while(class_index<compiler->program->class_count) {
        if(class_index==wanted) return true;
        const uint16_t parent=compiler->program->classes[class_index]->superclass;
        if(parent==DIAMOND_NO_CLASS) break;
        class_index=parent;
    }
    return false;
}

static bool type_set_satisfies(const Compiler *compiler,uint16_t known_index,
                               uint16_t expected_index) {
    const DiamondTypeSet *known=&compiler->function->type_sets[known_index];
    const DiamondTypeSet *expected=&compiler->function->type_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=type_member_satisfies(compiler,known->members[source],
                                           expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

bool type_members_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,DiamondTypeMember known,
    const DiamondTypeSet *expected_sets,DiamondTypeMember expected) {
    if(known.id==DIAMOND_TYPE_NATIVE||expected.id==DIAMOND_TYPE_NATIVE)
        return known.id==expected.id&&known.callable_arity==expected.callable_arity;
    if(expected.id>=DIAMOND_TYPE_INTERFACE_BASE&&known.id>=DIAMOND_TYPE_CLASS_BASE&&
       known.id<DIAMOND_TYPE_CLASS_LIMIT&&
       (size_t)(known.id-DIAMOND_TYPE_CLASS_BASE)<compiler->program->class_count&&
       DIAMOND_CLASS_TYPE_PARAMETER_COUNT(compiler->program->classes[known.id-DIAMOND_TYPE_CLASS_BASE])>0)
        return generic_class_satisfies_interface(compiler,known_sets,known,expected.id);
    if(!known_type_satisfies_one(compiler,known.id,expected.id))return false;
    if(expected.id==DIAMOND_TYPE_CALLABLE) {
        if(expected.callable_arity!=UINT8_MAX&&
           known.callable_arity!=expected.callable_arity)return false;
        if(expected.callable_parameters_typed)
            for(size_t parameter=0;parameter<expected.callable_arity;parameter++) {
                const uint16_t wanted=expected.callable_parameter_sets[parameter];
                if(wanted==DIAMOND_NO_TYPE_SET)continue;
                const uint16_t actual=known.callable_parameter_sets[parameter];
                if(actual!=DIAMOND_NO_TYPE_SET&&
                   !type_sets_satisfy_across(compiler,expected_sets,wanted,
                                              known_sets,actual))return false;
            }
        return expected.callable_return_set==DIAMOND_NO_TYPE_SET||
            (known.callable_return_set!=DIAMOND_NO_TYPE_SET&&
             type_sets_satisfy_across(compiler,known_sets,
                 known.callable_return_set,expected_sets,
                 expected.callable_return_set));
    }
    if(expected.argument_set==DIAMOND_NO_TYPE_SET)return true;
    if(known.argument_set==DIAMOND_NO_TYPE_SET||
       !type_sets_satisfy_across(compiler,known_sets,known.argument_set,
                                 expected_sets,expected.argument_set))return false;
    if(expected.id>=DIAMOND_TYPE_CLASS_BASE&&expected.id<DIAMOND_TYPE_VARIABLE_BASE)
        return type_sets_satisfy_across(compiler,expected_sets,expected.argument_set,
            known_sets,known.argument_set)&&
            (expected.second_argument_set==DIAMOND_NO_TYPE_SET||
             (known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
              type_sets_satisfy_across(compiler,known_sets,known.second_argument_set,expected_sets,expected.second_argument_set)&&
              type_sets_satisfy_across(compiler,expected_sets,expected.second_argument_set,known_sets,known.second_argument_set)));
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        type_sets_satisfy_across(compiler,known_sets,known.second_argument_set,
                                 expected_sets,expected.second_argument_set);
}

bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=type_members_satisfy_across(compiler,known_sets,
                known->members[source],expected_sets,expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

static bool type_member_satisfies(const Compiler *compiler,
                                  DiamondTypeMember known,
                                  DiamondTypeMember expected) {
    return type_members_satisfy_across(compiler,compiler->function->type_sets,known,
        compiler->function->type_sets,expected);
}

bool type_set_contains_variable(const Compiler *compiler,uint16_t set_index) {
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember member=set->members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           member.id<DIAMOND_TYPE_INTERFACE_BASE)return true;
        if(member.argument_set!=DIAMOND_NO_TYPE_SET&&
           type_set_contains_variable(compiler,member.argument_set))return true;
        if(member.second_argument_set!=DIAMOND_NO_TYPE_SET&&
           type_set_contains_variable(compiler,member.second_argument_set))return true;
        if(member.callable_return_set!=DIAMOND_NO_TYPE_SET&&
           type_set_contains_variable(compiler,member.callable_return_set))return true;
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++)
                if(type_set_contains_variable(compiler,
                       member.callable_parameter_sets[parameter]))return true;
    }
    return false;
}

static const char *compiler_type_name(const Compiler *compiler,uint16_t type) {
    if(type==DIAMOND_TYPE_INT)return "Int";
    if(type==DIAMOND_TYPE_FLOAT)return "Float";
    if(type==DIAMOND_TYPE_STRING)return "String";
    if(type==DIAMOND_TYPE_SYMBOL)return "Symbol";
    if(type==DIAMOND_TYPE_BOOL)return "Bool";
    if(type==DIAMOND_TYPE_NIL)return "Nil";
    if(type==DIAMOND_TYPE_ARRAY)return "Array";
    if(type==DIAMOND_TYPE_HASH)return "Hash";
    if(type==DIAMOND_TYPE_CALLABLE)return "Callable";
    if(type==DIAMOND_TYPE_SIZED)return "Sized";
    if(type==DIAMOND_TYPE_NATIVE)return "Native";
    if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t index=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
        return index<compiler->function->type_variable_count?
            compiler->function->type_variables[index]:"TypeVariable";
    }
    if(type>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t index=(size_t)(type-DIAMOND_TYPE_INTERFACE_BASE);
        return index<compiler->program->interface_count?
            compiler->program->interfaces[index]->name:"<invalid type>";
    }
    if(type>=DIAMOND_TYPE_CLASS_BASE) {
        const size_t index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
        return index<compiler->program->class_count?
            compiler->program->classes[index]->name:"<invalid type>";
    }
    return "<invalid type>";
}

static void format_compiler_type_set(char *buffer,size_t capacity,
        const Compiler *compiler,uint16_t set_index) {
    if(capacity==0)return;
    buffer[0]='\0';
    if((size_t)set_index>=compiler->function->type_set_count) {
        snprintf(buffer,capacity,"<invalid type set>");return;
    }
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    size_t used=0;
    for(size_t index=0;index<set->count&&used<capacity;index++) {
        const DiamondTypeMember member=set->members[index];
        const int name=snprintf(buffer+used,capacity-used,"%s%s",
            index==0?"":" | ",member.id==DIAMOND_TYPE_NATIVE?
                diamond_native_type_name(member.callable_arity):
                compiler_type_name(compiler,member.id));
        if(name<0)return;
        used+=(size_t)name;
        if(member.argument_set!=DIAMOND_NO_TYPE_SET&&used<capacity) {
            char first[192],second[192]="";
            format_compiler_type_set(first,sizeof first,compiler,member.argument_set);
            if(member.second_argument_set!=DIAMOND_NO_TYPE_SET)
                format_compiler_type_set(second,sizeof second,compiler,
                    member.second_argument_set);
            const int nested=snprintf(buffer+used,capacity-used,"[%s%s%s]",first,
                second[0]=='\0'?"":", ",second);
            if(nested<0)return;
            used+=(size_t)nested;
        } else if(member.id==DIAMOND_TYPE_CALLABLE&&
                  member.callable_arity!=UINT8_MAX&&used<capacity) {
            const int open=snprintf(buffer+used,capacity-used,
                member.callable_parameters_typed?"[[":"[%u",member.callable_arity);
            if(open<0)return;
            used+=(size_t)open;
            if(member.callable_parameters_typed) {
                for(size_t parameter=0;parameter<member.callable_arity&&used<capacity;
                    parameter++) {
                    char parameter_type[192];
                    format_compiler_type_set(parameter_type,sizeof parameter_type,
                        compiler,member.callable_parameter_sets[parameter]);
                    const int written=snprintf(buffer+used,capacity-used,"%s%s",
                        parameter==0?"":", ",parameter_type);
                    if(written<0)return;
                    used+=(size_t)written;
                }
                if(used<capacity) {
                    const int close=snprintf(buffer+used,capacity-used,"]");
                    if(close<0)return;
                    used+=(size_t)close;
                }
            }
            if(member.callable_return_set!=DIAMOND_NO_TYPE_SET&&used<capacity) {
                char returns[192];
                format_compiler_type_set(returns,sizeof returns,compiler,
                    member.callable_return_set);
                const int written=snprintf(buffer+used,capacity-used,", %s",returns);
                if(written<0)return;
                used+=(size_t)written;
            }
            if(used<capacity)snprintf(buffer+used,capacity-used,"]");
        }
    }
}

static void fail_type_mismatch(Compiler *compiler,DiamondSpan span,
        uint16_t reg,uint16_t expected_set) {
    char expected[384],actual[384];
    format_compiler_type_set(expected,sizeof expected,compiler,expected_set);
    if(compiler->known_type_sets[reg]>=0)
        format_compiler_type_set(actual,sizeof actual,compiler,
            (uint16_t)compiler->known_type_sets[reg]);
    else
        snprintf(actual,sizeof actual,"%s",
            compiler_type_name(compiler,compiler->known_types[reg]));
    char message[sizeof compiler->diagnostic->message];
    snprintf(message,sizeof message,"expected %s, got %s",expected,actual);
    fail(compiler,span,message);
}

/* Could a value of `known_set` still satisfy `expected_set` once its
 * contents are checked? True when every member that doesn't already
 * satisfy is a bare container (`Array`, `Hash`, with no element types)
 * of a kind the expected set accepts with element types, or a bare
 * `Callable` where a `Callable[N]` is expected: a bare Array says nothing
 * about its elements, so whether it's an Array[String] is the runtime
 * check's call, as for an untyped value. */
static bool type_set_may_satisfy(const Compiler *compiler,uint16_t known_set,
        uint16_t expected_set) {
    const DiamondTypeSet *known=&compiler->function->type_sets[known_set];
    const DiamondTypeSet *expected=&compiler->function->type_sets[expected_set];
    for(size_t source=0;source<known->count;source++) {
        const DiamondTypeMember member=known->members[source];
        bool possible=false;
        for(size_t target=0;target<expected->count&&!possible;target++) {
            const DiamondTypeMember wanted=expected->members[target];
            possible=type_member_satisfies(compiler,member,wanted)||
                (wanted.id>=DIAMOND_TYPE_INTERFACE_BASE&&
                 (size_t)(wanted.id-DIAMOND_TYPE_INTERFACE_BASE)<compiler->program->interface_count&&
                 member.id>=DIAMOND_TYPE_CLASS_BASE&&
                 member.id<DIAMOND_TYPE_CLASS_LIMIT&&
                 (size_t)(member.id-DIAMOND_TYPE_CLASS_BASE)<compiler->program->class_count&&
                 (generic_interfaces_pending(compiler)||
                  (DIAMOND_CLASS_TYPE_PARAMETER_COUNT(compiler->program->classes[member.id-DIAMOND_TYPE_CLASS_BASE])>0&&
                  (member.argument_set==DIAMOND_NO_TYPE_SET||
                  type_set_contains_variable(compiler,member.argument_set)||
                  (member.second_argument_set!=DIAMOND_NO_TYPE_SET&&type_set_contains_variable(compiler,member.second_argument_set))))))||
                (member.argument_set==DIAMOND_NO_TYPE_SET&&
                 wanted.argument_set!=DIAMOND_NO_TYPE_SET&&
                 (member.id==DIAMOND_TYPE_ARRAY||member.id==DIAMOND_TYPE_HASH||
                  (member.id>=DIAMOND_TYPE_CLASS_BASE&&member.id<DIAMOND_TYPE_VARIABLE_BASE))&&
                 member.id==wanted.id)||
                /* Likewise a bare Callable, whose arity isn't known. */
                (member.id==DIAMOND_TYPE_CALLABLE&&wanted.id==DIAMOND_TYPE_CALLABLE&&
                 member.callable_arity==UINT8_MAX);
        }
        if(!possible)return false;
    }
    return true;
}

static TypeCheckVerdict type_check_verdict(Compiler *compiler,uint16_t reg,
        uint16_t set_index) {
    if(type_set_contains_variable(compiler,set_index))return TYPE_CHECK_NEEDS_RUNTIME;
    if(compiler->known_type_sets[reg]>=0) {
        const uint16_t known_set=(uint16_t)compiler->known_type_sets[reg];
        if(type_set_satisfies(compiler,known_set,set_index))return TYPE_CHECK_SATISFIED;
        if(compiler->function->type_sets[known_set].inferred||
           type_set_may_satisfy(compiler,known_set,set_index))
            return TYPE_CHECK_NEEDS_RUNTIME;
        return TYPE_CHECK_VIOLATED;
    }
    const uint16_t known=compiler->known_types[reg];
    if(known==TYPE_UNKNOWN)return TYPE_CHECK_NEEDS_RUNTIME;
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        if(!known_type_satisfies_one(compiler,known,set->members[index].id)) {
            if(set->members[index].id>=DIAMOND_TYPE_INTERFACE_BASE&&known>=DIAMOND_TYPE_CLASS_BASE&&
               known<DIAMOND_TYPE_CLASS_LIMIT&&(size_t)(known-DIAMOND_TYPE_CLASS_BASE)<compiler->program->class_count&&
               (generic_interfaces_pending(compiler)||
                DIAMOND_CLASS_TYPE_PARAMETER_COUNT(compiler->program->classes[known-DIAMOND_TYPE_CLASS_BASE])>0))
                return TYPE_CHECK_NEEDS_RUNTIME;
            continue;
        }
        return set->members[index].argument_set!=DIAMOND_NO_TYPE_SET?
            TYPE_CHECK_NEEDS_RUNTIME:TYPE_CHECK_SATISFIED;
    }
    return TYPE_CHECK_VIOLATED;
}

void emit_type_check(Compiler *compiler, uint16_t reg, uint16_t set_index,
                            DiamondSpan span) {
    switch(type_check_verdict(compiler,reg,set_index)) {
    case TYPE_CHECK_SATISFIED:
        return;
    case TYPE_CHECK_NEEDS_RUNTIME:
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
        return;
    case TYPE_CHECK_VIOLATED:
        fail_type_mismatch(compiler,span,reg,set_index);
        return;
    }
}

void check_call_argument_types(Compiler *compiler,const DiamondFunction *callee,
        const uint16_t *arguments,size_t count,size_t parameter_offset,
        DiamondSpan span) {
    /* A variadic callee's positional arguments don't map one-to-one onto
     * its parameter slots (extras go into the rest Array), so it's left to
     * the runtime check. */
    if(compiler->discovery_pass||callee==nullptr||callee->type_variable_count>0||
       callee->has_variadic)return;
    for(size_t index=0;index<count;index++) {
        const size_t parameter=index+parameter_offset;
        if(parameter>=callee->arity||parameter>=DIAMOND_MAX_DECLARED_PARAMETERS)break;
        const uint16_t declared=callee->parameter_type_sets[parameter];
        if(declared==DIAMOND_NO_TYPE_SET||declared>=callee->type_set_count)continue;
        const uint16_t reg=arguments[index];
        if(compiler->known_type_sets[reg]<0&&compiler->known_types[reg]==TYPE_UNKNOWN)
            continue;
        const uint16_t expected=clone_type_set_into_current(compiler,
            callee->type_sets,callee->type_set_count,declared);
        if(expected==DIAMOND_NO_TYPE_SET)continue;
        if(type_check_verdict(compiler,reg,expected)!=TYPE_CHECK_VIOLATED)continue;
        char expected_text[384],actual[384];
        format_compiler_type_set(expected_text,sizeof expected_text,compiler,expected);
        if(compiler->known_type_sets[reg]>=0)
            format_compiler_type_set(actual,sizeof actual,compiler,
                (uint16_t)compiler->known_type_sets[reg]);
        else
            snprintf(actual,sizeof actual,"%s",
                compiler_type_name(compiler,compiler->known_types[reg]));
        char message[sizeof compiler->diagnostic->message];
        snprintf(message,sizeof message,"argument '%s' of %s: expected %s, got %s",
            callee->parameter_names[parameter],callee->name,expected_text,actual);
        fail(compiler,span,message);
        return;
    }
}

uint16_t add_constant(Compiler *compiler, DiamondValue value) {
    if (compiler->function->constant_count == DIAMOND_MAX_CONSTANTS) {
        fail(compiler, compiler->previous.span, "program has too many constants");
        return 0;
    }
    if(compiler->function->constant_count==compiler->function->constant_capacity) {
        size_t capacity=compiler->function->constant_capacity==0?32:
            compiler->function->constant_capacity*2;
        if(capacity>DIAMOND_MAX_CONSTANTS)capacity=DIAMOND_MAX_CONSTANTS;
        if(!diamond_function_reserve_constants(compiler->function,capacity)) {
            fail(compiler,compiler->previous.span,
                "out of memory growing function constants");return 0;
        }
    }
    const size_t index = compiler->function->constant_count++;
    compiler->function->constants[index] = value;
    return (uint16_t)index;
}

bool grow_type_sets(DiamondFunction *function,size_t additional) {
    if(additional>DIAMOND_MAX_TYPE_SETS-function->type_set_count)
        return false;
    const size_t needed=function->type_set_count+additional;
    if(needed>function->type_set_capacity) {
        size_t capacity=function->type_set_capacity==0?8:
            function->type_set_capacity*2;
        if(capacity<needed)capacity=needed;
        if(capacity>DIAMOND_MAX_TYPE_SETS)capacity=DIAMOND_MAX_TYPE_SETS;
        if(!diamond_function_reserve_type_sets(function,capacity))return false;
    }
    return true;
}

bool reserve_type_sets(Compiler *compiler,size_t additional) {
    if(additional>DIAMOND_MAX_TYPE_SETS-compiler->function->type_set_count) {
        fail(compiler,compiler->current.span,"function has too many type annotations");
        return false;
    }
    if(grow_type_sets(compiler->function,additional))return true;
    fail(compiler,compiler->current.span,"out of memory");
    return false;
}

uint16_t concrete_type_set(Compiler *compiler,uint16_t type) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==type&&
           set->members[0].argument_set==DIAMOND_NO_TYPE_SET&&
           set->members[0].second_argument_set==DIAMOND_NO_TYPE_SET)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return DIAMOND_NO_TYPE_SET;
    const size_t index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[index];
    set->count=1;set->inferred=true;
    set->members[0]=(DiamondTypeMember){.id=type,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        set->members[0].callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)index;
}

int32_t join_type_set_indices(Compiler *compiler,int32_t left,
        int32_t right) {
    if(left<0||right<0||(size_t)left>=compiler->function->type_set_count||
       (size_t)right>=compiler->function->type_set_count)return -1;
    if(left==right)return left;
    DiamondTypeSet joined=compiler->function->type_sets[(size_t)left];
    const DiamondTypeSet addition=compiler->function->type_sets[(size_t)right];
    for(size_t source=0;source<addition.count;source++) {
        const DiamondTypeMember member=addition.members[source];
        bool duplicate=false;
        for(size_t target=0;target<joined.count;target++) {
            if(type_members_equal(joined.members[target],member)) {
                duplicate=true;break;
            }
        }
        if(duplicate)continue;
        if(joined.count==DIAMOND_MAX_UNION_TYPES)return -1;
        joined.members[joined.count++]=member;
    }
    joined.inferred=true;
    for(size_t index=0;index<compiler->function->type_set_count;index++)
        if(type_sets_equal_unordered(joined,
                compiler->function->type_sets[index]))return (int32_t)index;
    if(!reserve_type_sets(compiler,1))return -1;
    const size_t index=compiler->function->type_set_count++;
    compiler->function->type_sets[index]=joined;
    return (int32_t)index;
}

int32_t joined_value_type_set(Compiler *compiler,
        const uint16_t *values,size_t count) {
    if(count==0)return -1;
    int32_t joined=-1;
    for(size_t index=0;index<count;index++) {
        int32_t current=compiler->known_type_sets[values[index]];
        if(current<0) {
            const uint16_t type=compiler->known_types[values[index]];
            if(type==TYPE_UNKNOWN||type>=DIAMOND_TYPE_VARIABLE_BASE)return -1;
            current=(int32_t)concrete_type_set(compiler,type);
        }
        if(current<0||(size_t)current>=compiler->function->type_set_count)
            return -1;
        joined=joined<0?current:join_type_set_indices(compiler,joined,current);
        if(joined<0)return -1;
    }
    return joined;
}

int32_t collection_type_set(Compiler *compiler,
        uint16_t type,int32_t argument_set,int32_t second_argument_set) {
    if(argument_set<0||
       (type==DIAMOND_TYPE_HASH&&second_argument_set<0))return -1;
    const uint16_t second=second_argument_set<0?DIAMOND_NO_TYPE_SET:
        (uint16_t)second_argument_set;
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *known=&compiler->function->type_sets[index];
        if(known->count==1&&known->members[0].id==type&&
           known->members[0].argument_set==(uint16_t)argument_set&&
           known->members[0].second_argument_set==second) {
            return (int32_t)index;
        }
    }
    if(!reserve_type_sets(compiler,1))return -1;
    const size_t index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[index];
    set->count=1;set->inferred=true;
    set->members[0]=(DiamondTypeMember){.id=type,
        .argument_set=(uint16_t)argument_set,
        .second_argument_set=second,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        set->members[0].callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    return (int32_t)index;
}

void record_collection_type_set(Compiler *compiler,uint16_t reg,
        uint16_t type,int32_t argument_set,int32_t second_argument_set) {
    const int32_t set=collection_type_set(compiler,type,argument_set,
        second_argument_set);
    if(set>=0)compiler->known_type_sets[reg]=set;
}

/* Joins editor facts without publishing them as checked compiler types. */
static int32_t joined_tooling_value_type_set(Compiler *compiler,
        const uint16_t *values,size_t count) {
    if(count==0)return -1;
    int32_t joined=-1;
    for(size_t index=0;index<count;index++) {
        const uint16_t value=values[index];
        int32_t current=compiler->tooling_type_sets[value];
        if(current<0)current=compiler->known_type_sets[value];
        if(current<0) {
            const uint16_t type=compiler->known_types[value];
            if(type==TYPE_UNKNOWN||type>=DIAMOND_TYPE_VARIABLE_BASE)return -1;
            current=(int32_t)concrete_type_set(compiler,type);
        }
        if(current<0||(size_t)current>=compiler->function->type_set_count)return -1;
        joined=joined<0?current:join_type_set_indices(compiler,joined,current);
        if(joined<0)return -1;
    }
    return joined;
}

/* A literal built from unannotated factory results can still describe its
 * elements to the editor. Keep this graph separate from checked contracts,
 * and give up if any element has no representable result. */
void publish_array_literal_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *values,size_t count) {
    bool have_tooling=false;
    for(size_t index=0;index<count;index++)
        if(compiler->tooling_type_sets[values[index]]>=0) {have_tooling=true;break;}
    if(!have_tooling)return;
    const int32_t joined=joined_tooling_value_type_set(compiler,values,count);
    compiler->tooling_type_sets[reg]=collection_type_set(compiler,
        DIAMOND_TYPE_ARRAY,joined,-1);
}

void publish_hash_literal_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *keys,const uint16_t *values,size_t count) {
    bool have_tooling=false;
    for(size_t index=0;index<count;index++)
        if(compiler->tooling_type_sets[keys[index]]>=0||
           compiler->tooling_type_sets[values[index]]>=0) {have_tooling=true;break;}
    if(!have_tooling)return;
    const int32_t key_set=joined_tooling_value_type_set(compiler,keys,count);
    const int32_t value_set=joined_tooling_value_type_set(compiler,values,count);
    compiler->tooling_type_sets[reg]=collection_type_set(compiler,
        DIAMOND_TYPE_HASH,key_set,value_set);
}

void publish_array_spread_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *prefix,size_t prefix_count,uint16_t spread,
        const uint16_t *suffix,size_t suffix_count) {
    bool have_tooling=compiler->tooling_type_sets[spread]>=0;
    for(size_t index=0;index<prefix_count&&!have_tooling;index++)
        have_tooling=compiler->tooling_type_sets[prefix[index]]>=0;
    for(size_t index=0;index<suffix_count&&!have_tooling;index++)
        have_tooling=compiler->tooling_type_sets[suffix[index]]>=0;
    if(!have_tooling)return;
    int32_t spread_set=compiler->tooling_type_sets[spread];
    if(spread_set<0)spread_set=compiler->known_type_sets[spread];
    if(spread_set<0||(size_t)spread_set>=compiler->function->type_set_count)return;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)spread_set];
    if(set->count!=1||set->members[0].id!=DIAMOND_TYPE_ARRAY||
       set->members[0].argument_set==DIAMOND_NO_TYPE_SET)return;
    int32_t elements=(int32_t)set->members[0].argument_set;
    if(prefix_count>0)elements=join_type_set_indices(compiler,elements,
        joined_tooling_value_type_set(compiler,prefix,prefix_count));
    if(suffix_count>0)elements=join_type_set_indices(compiler,elements,
        joined_tooling_value_type_set(compiler,suffix,suffix_count));
    compiler->tooling_type_sets[reg]=collection_type_set(compiler,
        DIAMOND_TYPE_ARRAY,elements,-1);
}

int32_t array_element_type_set(const Compiler *compiler,uint16_t reg) {
    const int32_t outer=compiler->known_type_sets[reg];
    if(outer<0||(size_t)outer>=compiler->function->type_set_count)return -1;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)outer];
    if(set->count!=1||set->members[0].id!=DIAMOND_TYPE_ARRAY||
       set->members[0].argument_set==DIAMOND_NO_TYPE_SET)return -1;
    return (int32_t)set->members[0].argument_set;
}

static uint16_t clone_type_set_into_current_impl(Compiler *compiler,
        const DiamondTypeSet *source_sets,size_t source_count,
        uint16_t source_index,bool source_is_current) {
    if(source_index==DIAMOND_NO_TYPE_SET||source_index>=source_count)
        return DIAMOND_NO_TYPE_SET;
    DiamondTypeSet cloned=source_is_current?
        compiler->function->type_sets[source_index]:source_sets[source_index];
    if(!reserve_type_sets(compiler,1))return DIAMOND_NO_TYPE_SET;
    const size_t destination_index=compiler->function->type_set_count++;
    for(size_t member_index=0;member_index<cloned.count;member_index++) {
        DiamondTypeMember *member=&cloned.members[member_index];
        member->argument_set=clone_type_set_into_current_impl(compiler,
            source_sets,source_count,member->argument_set,source_is_current);
        member->second_argument_set=clone_type_set_into_current_impl(compiler,
            source_sets,source_count,member->second_argument_set,
            source_is_current);
        member->callable_return_set=clone_type_set_into_current_impl(compiler,
            source_sets,source_count,member->callable_return_set,
            source_is_current);
        if(member->callable_parameters_typed)
            for(size_t parameter=0;parameter<member->callable_arity;parameter++)
                member->callable_parameter_sets[parameter]=
                    clone_type_set_into_current_impl(compiler,source_sets,
                        source_count,member->callable_parameter_sets[parameter],
                        source_is_current);
    }
    compiler->function->type_sets[destination_index]=cloned;
    return (uint16_t)destination_index;
}

uint16_t clone_type_set_into_current(Compiler *compiler,
        const DiamondTypeSet *source_sets,size_t source_count,
        uint16_t source_index) {
    return clone_type_set_into_current_impl(compiler,source_sets,source_count,
        source_index,source_sets==compiler->function->type_sets);
}

uint16_t clone_substituted_type_set(Compiler *compiler,
        const DiamondFunction *target,uint16_t source_index,
        const uint16_t *bindings,size_t binding_count,bool *resolved) {
    if(source_index==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
    if(source_index>=target->type_set_count) {
        *resolved=false;return DIAMOND_NO_TYPE_SET;
    }
    const DiamondTypeSet source=target->type_sets[source_index];
    if(source.count==1&&
       source.members[0].id>=DIAMOND_TYPE_VARIABLE_BASE&&
       source.members[0].id<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(source.members[0].id-
            DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=binding_count||bindings==nullptr||
           bindings[variable]>=compiler->function->type_set_count) {
            *resolved=false;return DIAMOND_NO_TYPE_SET;
        }
        return bindings[variable];
    }
    if(!reserve_type_sets(compiler,1)) {
        *resolved=false;return DIAMOND_NO_TYPE_SET;
    }
    const size_t destination_index=compiler->function->type_set_count++;
    DiamondTypeSet substituted={.inferred=source.inferred};
    for(size_t member_index=0;member_index<source.count;member_index++) {
        DiamondTypeMember source_member=source.members[member_index];
        if(source_member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           source_member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(source_member.id-
                DIAMOND_TYPE_VARIABLE_BASE);
            if(variable>=binding_count||bindings==nullptr||
               bindings[variable]>=compiler->function->type_set_count) {
                *resolved=false;return DIAMOND_NO_TYPE_SET;
            }
            const DiamondTypeSet bound=
                compiler->function->type_sets[bindings[variable]];
            for(size_t bound_index=0;bound_index<bound.count;bound_index++) {
                bool duplicate=false;
                for(size_t known=0;known<substituted.count;known++)
                    if(type_members_equal(substituted.members[known],
                            bound.members[bound_index]))duplicate=true;
                if(duplicate)continue;
                if(substituted.count==DIAMOND_MAX_UNION_TYPES) {
                    *resolved=false;return DIAMOND_NO_TYPE_SET;
                }
                substituted.members[substituted.count++]=
                    bound.members[bound_index];
            }
            continue;
        }
        DiamondTypeMember member_value=source_member;
        DiamondTypeMember *member=&member_value;
        if(member->argument_set!=DIAMOND_NO_TYPE_SET)
            member->argument_set=clone_substituted_type_set(compiler,target,
                member->argument_set,bindings,binding_count,resolved);
        if(member->second_argument_set!=DIAMOND_NO_TYPE_SET)
            member->second_argument_set=clone_substituted_type_set(compiler,
                target,member->second_argument_set,bindings,binding_count,
                resolved);
        if(member->callable_return_set!=DIAMOND_NO_TYPE_SET)
            member->callable_return_set=clone_substituted_type_set(compiler,
                target,member->callable_return_set,bindings,binding_count,
                resolved);
        if(member->callable_parameters_typed)
            for(size_t parameter=0;parameter<member->callable_arity;parameter++)
                if(member->callable_parameter_sets[parameter]!=
                        DIAMOND_NO_TYPE_SET)
                    member->callable_parameter_sets[parameter]=
                        clone_substituted_type_set(compiler,target,
                            member->callable_parameter_sets[parameter],bindings,
                            binding_count,resolved);
        if(!*resolved)return DIAMOND_NO_TYPE_SET;
        if(substituted.count==DIAMOND_MAX_UNION_TYPES) {
            *resolved=false;return DIAMOND_NO_TYPE_SET;
        }
        substituted.members[substituted.count++]=member_value;
    }
    compiler->function->type_sets[destination_index]=substituted;
    return (uint16_t)destination_index;
}

void publish_known_type_set(Compiler *compiler,uint16_t reg,
        uint16_t set_index) {
    if(set_index==DIAMOND_NO_TYPE_SET||
       set_index>=compiler->function->type_set_count)return;
    compiler->known_type_sets[reg]=(int32_t)set_index;
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    if(set->count==0)return;
    const uint16_t outer=set->members[0].id;
    for(size_t index=1;index<set->count;index++)
        if(set->members[index].id!=outer)return;
    compiler->known_types[reg]=outer;
}

void publish_callable_return_type(Compiler *compiler,uint16_t reg,
        int32_t callable_set_index) {
    if(callable_set_index<0||
       (size_t)callable_set_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet *set=
        &compiler->function->type_sets[(size_t)callable_set_index];
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember *member=&set->members[index];
        if(member->id!=DIAMOND_TYPE_CALLABLE||
           member->callable_return_set==DIAMOND_NO_TYPE_SET)return;
        if(return_set==DIAMOND_NO_TYPE_SET)
            return_set=member->callable_return_set;
        else if(return_set!=member->callable_return_set&&
                !type_sets_structurally_equal(
                    compiler->function->type_sets,
                    compiler->function->type_set_count,return_set,
                    compiler->function->type_sets,
                    compiler->function->type_set_count,
                    member->callable_return_set,0))return;
    }
    publish_known_type_set(compiler,reg,return_set);
}

static void publish_resolved_call_return_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target,const uint16_t *bindings,
        size_t binding_count,bool tooling_only) {
    if(target==nullptr)return;
    if(target->return_type_set==DIAMOND_NO_TYPE_SET&&
       target->declared_by_discovery&&compiler->tooling_forward_calls!=nullptr) {
        compiler->tooling_forward_calls->forward_calls=true;
        if(compiler->tooling_returns!=nullptr) {
            size_t index=0;
            while(index<compiler->program->function_count&&
                  compiler->program->functions[index]!=target)index++;
            if(index>=compiler->tooling_returns->function_count)return;
            const DiamondFunction *previous=compiler->tooling_returns->functions[index];
            if(previous->declaration_start!=target->declaration_start||
               previous->owner_class!=target->owner_class||
               strcmp(previous->name,target->name)!=0)return;
            if(compiler->tooling_forward_calls->used!=nullptr&&
               index<compiler->tooling_forward_calls->count)
                compiler->tooling_forward_calls->used[index]=true;
            target=previous;
        }
    }

    if(target->return_type_set==DIAMOND_NO_TYPE_SET) {
        /* Unlike a declared return, this remains tooling-only: it may feed
         * scope facts for receiver completion after assignment, but never
         * known_type_sets (and therefore never type checks or opcode choice). */
        if(target->inferred_return_type_set==DIAMOND_NO_TYPE_SET||
           target->inferred_return_type_set>=target->type_set_count)return;
        uint16_t set;
        if(target->type_variable_count>0) {
            bool resolved=true;
            const size_t original_count=compiler->function->type_set_count;
            set=clone_substituted_type_set(compiler,target,
                target->inferred_return_type_set,bindings,binding_count,&resolved);
            if(!resolved) {
                compiler->function->type_set_count=original_count;return;
            }
        } else set=clone_type_set_into_current(compiler,target->type_sets,
            target->type_set_count,target->inferred_return_type_set);
        if(set!=DIAMOND_NO_TYPE_SET)compiler->tooling_type_sets[reg]=(int32_t)set;
        return;
    }
    uint16_t return_set;
    if(target->type_variable_count>0) {
        bool resolved=true;
        const size_t original_count=compiler->function->type_set_count;
        return_set=clone_substituted_type_set(compiler,target,
            target->return_type_set,bindings,binding_count,&resolved);
        if(!resolved) {
            compiler->function->type_set_count=original_count;return;
        }
    } else return_set=target==compiler->function?
        target->return_type_set:clone_type_set_into_current(compiler,
            target->type_sets,target->type_set_count,target->return_type_set);
    if(tooling_only) {
        if(return_set!=DIAMOND_NO_TYPE_SET)
            compiler->tooling_type_sets[reg]=(int32_t)return_set;
    } else publish_known_type_set(compiler,reg,return_set);
}

void publish_call_return_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target,const uint16_t *bindings,
        size_t binding_count) {
    if(target==nullptr||target->class_type_variable)return;
    publish_resolved_call_return_type(compiler,reg,target,bindings,binding_count,false);
}

void publish_bound_instance_return_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target,const uint16_t *bindings,
        size_t binding_count,bool tooling_only) {
    publish_resolved_call_return_type(compiler,reg,target,bindings,binding_count,tooling_only);
}

void publish_function_callable_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target) {
    compiler->known_types[reg]=DIAMOND_TYPE_CALLABLE;
    /* Cloning from the function currently being compiled could reallocate
     * its type-set table while that same table is the clone source. A
     * recursive self-reference remains a known Callable, but conservatively
     * omits structural signature facts until its declaration is complete. */
    if(target==compiler->function)return;
    /* target->arity counts an implicit self slot for some owner_class
     * shapes (diamond_function_self_offset's own comment, src/vm.h) --
     * a Callable *value*'s type only ever describes its real, self-less
     * arguments (parameter_type_sets is already indexed that way), so
     * that offset must come out of the published arity here too. */
    const uint8_t self_offset=diamond_function_self_offset(target);
    const uint8_t declared_arity=(uint8_t)(target->arity-self_offset);
    DiamondTypeMember callable={.id=DIAMOND_TYPE_CALLABLE,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=declared_arity,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=true};
    for(size_t parameter=0;parameter<16;parameter++)
        callable.callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    for(size_t parameter=0;parameter<declared_arity;parameter++) {
        if(target->parameter_type_sets[parameter]==DIAMOND_NO_TYPE_SET) {
            callable.callable_parameters_typed=false;break;
        }
        callable.callable_parameter_sets[parameter]=
            clone_type_set_into_current(compiler,target->type_sets,
                target->type_set_count,target->parameter_type_sets[parameter]);
    }
    callable.callable_return_set=clone_type_set_into_current(compiler,
        target->type_sets,target->type_set_count,target->return_type_set);
    if(!reserve_type_sets(compiler,1))return;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;set->inferred=true;set->members[0]=callable;
    compiler->known_type_sets[reg]=(int32_t)set_index;
    /* An unannotated function still has an inferred result graph: describe it
     * to the editor so `map(make_pet)` can relay it. */
    if(target->return_type_set==DIAMOND_NO_TYPE_SET&&
       target->inferred_return_type_set!=DIAMOND_NO_TYPE_SET&&
       target->inferred_return_type_set<target->type_set_count)
        publish_block_tooling_callable(compiler,reg,target,
            target->inferred_return_type_set);
}

static unsigned hex_digit_value(char digit) {
    if(digit>='0'&&digit<='9')return (unsigned)(digit-'0');
    if(digit>='a'&&digit<='f')return (unsigned)(digit-'a'+10);
    return (unsigned)(digit-'A'+10);
}

/* Writes `code_point` (already checked to be a Unicode scalar value) as
 * UTF-8 into `out`, returning how many bytes that took. */
static size_t utf8_encode_code_point(unsigned long code_point,char out[4]) {
    if(code_point<0x80) {out[0]=(char)code_point;return 1;}
    if(code_point<0x800) {
        out[0]=(char)(0xC0|(code_point>>6));out[1]=(char)(0x80|(code_point&0x3F));
        return 2;
    }
    if(code_point<0x10000) {
        out[0]=(char)(0xE0|(code_point>>12));out[1]=(char)(0x80|((code_point>>6)&0x3F));
        out[2]=(char)(0x80|(code_point&0x3F));return 3;
    }
    out[0]=(char)(0xF0|(code_point>>18));out[1]=(char)(0x80|((code_point>>12)&0x3F));
    out[2]=(char)(0x80|((code_point>>6)&0x3F));out[3]=(char)(0x80|(code_point&0x3F));
    return 4;
}

/* raw: regex literal text -- bytes are kept as written (the regex engine reads its own escapes),
 * except that \/ stands for a plain slash. */
uint16_t add_string_range_mode(Compiler *compiler,size_t start,size_t length,
                                DiamondSpan span,bool raw) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS) {
        fail(compiler, span, "function has too many string literals");
        return 0;
    }
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,span,"out of memory growing function strings");return 0;
    }
    DiamondStringConstant *string =
        &compiler->function->strings[compiler->function->string_count];
    for(size_t index=0;index<length;index++) {
        char character=compiler->source[start+index];
        char encoded[4];
        size_t encoded_length=0;
        if(raw) {
            if(character=='\\'&&index+1<length) {
                if(compiler->source[start+index+1]=='/')character=compiler->source[start+(++index)];
                else {
                    encoded[0]=character;encoded[1]=compiler->source[start+(++index)];
                    encoded_length=2;
                }
            }
        } else if (character == '\\') {
            index++;
            if(index==length) {fail(compiler,span,"incomplete string escape");return 0;}
            character=compiler->source[start+index];
            switch (character) {
                case 'n': character = '\n'; break;
                case 'r': character = '\r'; break;
                case 't': character = '\t'; break;
                case '0': character = '\0'; break;
                case 'e': character = '\x1b'; break;
                case '"': character = '"'; break;
                case '\\': character = '\\'; break;
                case '#': character = '#'; break;
                case 'x': {
                    /* \xH or \xHH: one byte, as in Ruby. */
                    unsigned value=0;size_t digits=0;
                    while(digits<2&&index+1<length&&
                          isxdigit((unsigned char)compiler->source[start+index+1])) {
                        value=value*16+hex_digit_value(compiler->source[start+index+1]);
                        index++;digits++;
                    }
                    if(digits==0) {fail(compiler,span,"\\x needs a hex digit");return 0;}
                    character=(char)value;
                    break;
                }
                case 'u': {
                    /* \uHHHH or \u{H...}: a code point, stored as UTF-8. */
                    unsigned long value=0;size_t digits=0;
                    const bool braced=index+1<length&&compiler->source[start+index+1]=='{';
                    if(braced)index++;
                    while((braced?digits<6:digits<4)&&index+1<length&&
                          isxdigit((unsigned char)compiler->source[start+index+1])) {
                        value=value*16+hex_digit_value(compiler->source[start+index+1]);
                        index++;digits++;
                    }
                    if(braced) {
                        if(index+1>=length||compiler->source[start+index+1]!='}') {
                            fail(compiler,span,"\\u{ needs hex digits and a closing }");
                            return 0;
                        }
                        index++;
                    }
                    if(digits==0||(!braced&&digits!=4)||value>0x10FFFF||
                       (value>=0xD800&&value<=0xDFFF)) {
                        fail(compiler,span,"invalid \\u escape");return 0;
                    }
                    encoded_length=utf8_encode_code_point(value,encoded);
                    break;
                }
                default:
                    fail(compiler, span, "unsupported string escape");
                    return 0;
            }
        }
        if(encoded_length==0) {encoded[0]=character;encoded_length=1;}
        if (string->length+encoded_length > DIAMOND_MAX_STRING_LENGTH) {
            fail(compiler, span, "string literal is too long");
            return 0;
        }
        for(size_t byte=0;byte<encoded_length;byte++)
            string->chars[string->length++] = encoded[byte];
    }
    string->chars[string->length] = '\0';
    return (uint16_t)compiler->function->string_count++;
}

uint16_t add_string_range(Compiler *compiler,size_t start,size_t length,
                                DiamondSpan span) {
    return add_string_range_mode(compiler,start,length,span,false);
}

uint16_t add_string(Compiler *compiler,DiamondSpan span) {
    return add_string_range(compiler,span.start+1,span.length-2,span);
}

uint16_t add_name_string(Compiler *compiler, DiamondSpan span) {
    return add_name_text(compiler,compiler->source+span.start,span.length,span);
}

/* add_name_string for a name that is not in the source text, such as a
 * parameter name read from a compiled function; span is for diagnostics. */
uint16_t add_name_text(Compiler *compiler,const char *text,size_t length,DiamondSpan span) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS ||
        length > DIAMOND_MAX_STRING_LENGTH) {
        fail(compiler, span, "too many or oversized names in function");
        return 0;
    }
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,span,"out of memory growing function strings");return 0;
    }
    DiamondStringConstant *string =
        &compiler->function->strings[compiler->function->string_count];
    for (size_t i = 0; i < length; i++)
        string->chars[i] = text[i];
    string->length = length;
    string->chars[length] = '\0';
    return (uint16_t)compiler->function->string_count++;
}

/* Hand-rolled rather than routed through emit_instruction: the jump-target
 * placeholder deliberately stays exactly 2 raw bytes (matching
 * patch_jump's own direct code[]-array writes below), independent of
 * emit_instruction's own operand width -- these two placeholder bytes are
 * never "an operand" from emit_instruction's point of view, just reserved
 * space patched in later once the jump's real destination is known. */
size_t emit_jump(Compiler *compiler, DiamondOpCode opcode,
                        uint16_t condition) {
    const bool conditional=opcode==DIAMOND_OP_JUMP_IF_FALSE ||
                           opcode==DIAMOND_OP_JUMP_IF_TRUE;
    emit_opcode(compiler, opcode);
    if (conditional) emit_register(compiler, condition);
    const size_t operand = compiler->function->code_count;
    emit_byte(compiler, 0);
    emit_byte(compiler, 0);
    return operand;
}

void patch_jump(Compiler *compiler, size_t operand, size_t target) {
    if (target > UINT16_MAX) {
        fail(compiler, compiler->previous.span, "jump target is too distant");
        return;
    }
    compiler->function->code[operand] = (uint8_t)(target >> 8);
    compiler->function->code[operand + 1] = (uint8_t)(target & UINT8_MAX);
    if (compiler->last_plain_call_function == compiler->function &&
        target == compiler->last_plain_call_code_count)
        compiler->last_plain_call_label_after = true;
}

/* Same two-byte placeholder as emit_jump's own, but for a forward-
 * referenced module_function/`def self.x` call site instead of a jump --
 * unlike a jump target, this patch can land in a *different* function's
 * own code[] than whatever compiler->function is when the patch actually
 * happens (the call site is in the earlier sibling's body; the patch
 * happens once the later sibling's real def compiles), so the fixup
 * records the exact DiamondFunction* to patch, not just an offset. See
 * DiamondMethod's own function_index field and
 * DIAMOND_UNRESOLVED_SINGLETON_FUNCTION for the sentinel this exists to
 * eventually replace. */
/* A pending singleton method's identity that survives its table growing:
 * the owner's table field (a class or module slot never moves) plus the
 * entry's index. A raw DiamondMethod pointer would go stale when a later
 * append reallocates the table. */
static PendingSingletonKey pending_singleton_key(const Compiler *compiler,
        const DiamondMethod *method) {
    const uintptr_t address = (uintptr_t)method;
    for (size_t index = 0; index < compiler->program->class_count; index++) {
        DiamondClass *class = compiler->program->classes[index];
        const uintptr_t start = (uintptr_t)class->singleton_methods;
        if (class->singleton_method_count > 0 && address >= start &&
            address < start + class->singleton_method_count * sizeof *method)
            return (PendingSingletonKey){&class->singleton_methods,
                (address - start) / sizeof *method};
    }
    for (size_t index = 0; index < compiler->program->module_count; index++) {
        DiamondModule *module = compiler->program->modules[index];
        const uintptr_t start = (uintptr_t)module->singleton_methods;
        if (module->singleton_method_count > 0 && address >= start &&
            address < start + module->singleton_method_count * sizeof *method)
            return (PendingSingletonKey){&module->singleton_methods,
                (address - start) / sizeof *method};
    }
    return (PendingSingletonKey){nullptr, address};
}

void emit_pending_singleton_function_index(Compiler *compiler,
        DiamondSpan span, const DiamondMethod *method) {
    const size_t operand = compiler->function->code_count;
    emit_byte(compiler, 0);
    emit_byte(compiler, 0);
    if (compiler->pending_singleton_fixup_count >=
        sizeof compiler->pending_singleton_fixups /
        sizeof compiler->pending_singleton_fixups[0]) {
        fail(compiler, span,
            "too many forward-referenced singleton calls pending");
        return;
    }
    const size_t slot = compiler->pending_singleton_fixup_count++;
    compiler->pending_singleton_fixups[slot].function = compiler->function;
    compiler->pending_singleton_fixups[slot].operand = operand;
    compiler->pending_singleton_fixups[slot].method = pending_singleton_key(compiler, method);
}

/* Patches every pending call site recorded by
 * emit_pending_singleton_function_index for this exact declaration (there can
 * be more than one -- several earlier siblings can all forward-reference
 * the same later one), now that its real function_index is known.
 * Resolved entries are removed (swap-with-last), same convention as
 * every other fixed-capacity array removal in this file. */
void resolve_pending_singleton_fixups(Compiler *compiler,
        const DiamondMethod *method, size_t function_index) {
    const PendingSingletonKey key = pending_singleton_key(compiler, method);
    for (size_t index = 0; index < compiler->pending_singleton_fixup_count; ) {
        const PendingSingletonKey pending = compiler->pending_singleton_fixups[index].method;
        if (pending.table == key.table && pending.index == key.index) {
            DiamondFunction *target = compiler->pending_singleton_fixups[index].function;
            const size_t operand = compiler->pending_singleton_fixups[index].operand;
            target->code[operand] = (uint8_t)(function_index >> 8);
            target->code[operand + 1] = (uint8_t)(function_index & UINT8_MAX);
            compiler->pending_singleton_fixup_count--;
            compiler->pending_singleton_fixups[index] =
                compiler->pending_singleton_fixups[compiler->pending_singleton_fixup_count];
        } else {
            index++;
        }
    }
}

void emit_absolute_jump(Compiler *compiler, size_t target) {
    if (target > UINT16_MAX) {
        fail(compiler, compiler->previous.span, "jump target is too distant");
        return;
    }
    emit_opcode(compiler, DIAMOND_OP_JUMP);
    emit_byte(compiler, (uint8_t)(target >> 8));
    emit_byte(compiler, (uint8_t)(target & UINT8_MAX));
}

bool spans_equal(const Compiler *compiler, DiamondSpan left,
                        DiamondSpan right) {
    if (left.length != right.length) {
        return false;
    }
    for (size_t index = 0; index < left.length; index++) {
        if (compiler->source[left.start + index] !=
            compiler->source[right.start + index]) {
            return false;
        }
    }
    return true;
}

int find_local(const Compiler *compiler, DiamondSpan name) {
    for (size_t index = compiler->local_count; index > 0; index--) {
        if (spans_equal(compiler, compiler->locals[index - 1].name, name)) {
            return (int)(index - 1);
        }
    }
    return -1;
}

/* Whether `reg` is already some existing local's home -- covers every
 * named entity that lives in compiler->locals[], not just ordinary
 * locals: self (registered with .reg=self_register) and every parameter
 * (registered the same way as its own parameter register) both go
 * through this same table (see their own `compiler->locals[...++]=`
 * sites), so one scan here also protects those, with no separate check
 * needed. Used by compile_assignment_store's own "reuse the RHS's
 * temporary register as a brand-new local's home" optimization (see its
 * own comment) to confirm a candidate register is a genuine, freshly
 * computed temporary rather than a bare reference to an existing name
 * (`x = y`, `x = self`, `x = some_param`) -- reusing one of *those*
 * registers as `x`'s own home would alias two different variables onto
 * one register. */
bool register_is_named(const Compiler *compiler, uint16_t reg) {
    for (size_t index = 0; index < compiler->local_count; index++)
        if (compiler->locals[index].reg == reg) return true;
    return false;
}

/* Register recycling Stage 2 (docs/internal/register-recycling-design.md):
 * is `opcode` one this compiler already knows never touches
 * compiler->narrowing or any other lasting per-register compiler state
 * beyond compiler->known_types/known_type_sets/tooling_type_sets (which
 * compile_assignment_store's own caller already copies from `value` to
 * `destination` regardless of how `destination` got its value, so
 * rewriting the producer changes nothing there)? Deliberately a narrow,
 * individually-vetted allowlist, not "every opcode compile_binary_op can
 * emit" -- EQUAL/NOT_EQUAL (and their _INT forms) are excluded because
 * compile_binary_op sets compiler->narrowing.condition to their own
 * destination register for a nil-comparison, and a later `if`/`unless`
 * reading that field expects it to still refer to a live, meaningful
 * condition register, not one a rewrite silently repurposed. The rest of
 * the comparison family (LESS*, GREATER*, COMPARE) is excluded too, on
 * the same "don't extend the allowlist past what's actually been
 * checked" principle -- see the design doc for why Stage 1 alone getting
 * this kind of check wrong already cost a real, if different, bug. */
static bool opcode_is_rewritable_arithmetic(DiamondOpCode opcode) {
    switch (opcode) {
        case DIAMOND_OP_ADD: case DIAMOND_OP_ADD_INT:
        case DIAMOND_OP_SUBTRACT: case DIAMOND_OP_SUBTRACT_INT:
        case DIAMOND_OP_MULTIPLY: case DIAMOND_OP_MULTIPLY_INT:
        case DIAMOND_OP_DIVIDE: case DIAMOND_OP_DIVIDE_INT:
        case DIAMOND_OP_MODULO:
        case DIAMOND_OP_SHIFT_LEFT: case DIAMOND_OP_SHIFT_RIGHT:
        case DIAMOND_OP_BITWISE_AND: case DIAMOND_OP_BITWISE_OR:
        case DIAMOND_OP_BITWISE_XOR:
            return true;
        default:
            return false;
    }
}

/* Register recycling Stage 2: reassigning an *existing* local
 * (`total = total + 1`) can't reuse Stage 1's trick (`destination` is
 * `total`'s own permanent register, fixed since it was first declared,
 * not something this statement can relabel) -- but if `value` is a
 * fresh arithmetic result about to be thrown away after one MOVE, the
 * instruction that produced it can be rewritten to write `destination`
 * directly instead, and the MOVE skipped entirely. Every check here is
 * required and independently necessary (see the design doc for the
 * reasoning behind each, including a near-miss found while writing it):
 *
 * - `!register_is_named(compiler, value)`: `value` isn't any existing
 *   local's/parameter's/self's own register -- rules out `total = y`
 *   (`value` would be `y`'s own permanent register, not a throwaway one).
 * - `compiler->last_instruction_offset < compiler->function->code_count`:
 *   there genuinely is a most-recent instruction to inspect (defends
 *   against a stale offset even though every entry/exit to a nested
 *   function body already resets this to SIZE_MAX -- always checked
 *   before code[] is ever indexed with it, never just assumed).
 * - The instruction at that offset actually is `opcode_is_rewritable_
 *   arithmetic` and its own first operand -- read back from the bytecode
 *   itself, not assumed from control flow -- equals `value` exactly:
 *   confirms this is truly `value`'s own producer, not merely the most
 *   recently emitted instruction for an unrelated reason.
 *
 * Returns whether the rewrite happened (the MOVE must be skipped, not
 * just made optional, when it did -- `value` is no longer meaningful
 * once its producer's own destination has been repointed). Reclaiming
 * `value`'s register slot for reuse (decrementing next_register) is a
 * separate, additional step, gated on `value` being the current
 * highest-numbered register -- always safe to skip when it doesn't
 * apply, just less optimal. */
bool try_rewrite_producer_destination(Compiler *compiler, uint16_t value,
                                              uint16_t destination) {
    if (register_is_named(compiler, value)) return false;
    const size_t offset = compiler->last_instruction_offset;
    if (offset >= compiler->function->code_count) return false;
    const size_t operand_offset = offset + 1;
    if (operand_offset + 1 >= compiler->function->code_count) return false;
    if (!opcode_is_rewritable_arithmetic(
            (DiamondOpCode)compiler->function->code[offset])) return false;
    const uint16_t written = (uint16_t)(
        ((uint16_t)compiler->function->code[operand_offset] << 8) |
        compiler->function->code[operand_offset + 1]);
    if (written != value) return false;
    compiler->function->code[operand_offset] = (uint8_t)(destination >> 8);
    compiler->function->code[operand_offset + 1] = (uint8_t)(destination & UINT8_MAX);
    if (value == compiler->next_register - 1) compiler->next_register--;
    return true;
}

/* `preferred_register` lets a caller that already has a register ready to
 * become this local's home skip allocate_register entirely --
 * compile_assignment_store's own "reuse the RHS's temporary register
 * directly" optimization (see its own comment) is the one user of this
 * today. DIAMOND_REGISTER_COUNT (4096) never reaches UINT16_MAX, so that
 * value unambiguously means "no preference, allocate a fresh one" -- the
 * plain define_local wrapper below, every other caller's own case. */
uint16_t define_local_with_register(Compiler *compiler, DiamondSpan name,
                                            uint16_t preferred_register) {
    if (compiler->local_count == DIAMOND_MAX_LOCALS) {
        fail(compiler, name, "too many local variables");
        return 0;
    }
    const uint16_t reg = preferred_register == UINT16_MAX
        ? allocate_register(compiler) : preferred_register;
    /* .captured starts true, rather than the usual false, whenever this
     * declaration happens inside a loop body a def/closure was found
     * somewhere in (loop_captures_pending) -- a local declared fresh
     * inside such a loop, then captured later in that same body, needs
     * every one of its own subsequent reads/writes (including ones
     * compiled before that later def/closure is even reached) to already
     * be box-aware, for the same reason a pre-existing outer local does
     * (see loop_captures_pending's own comment). */
    compiler->locals[compiler->local_count++] =
        (Local){.name = name, .reg = reg, .captured = compiler->loop_captures_pending};
    return reg;
}

uint16_t define_local(Compiler *compiler, DiamondSpan name) {
    return define_local_with_register(compiler, name, UINT16_MAX);
}

Precedence token_precedence(DiamondTokenKind kind) {
    switch (kind) {
        case DIAMOND_TOKEN_DOT_DOT:
        case DIAMOND_TOKEN_DOT_DOT_DOT:
            return PREC_RANGE;
        case DIAMOND_TOKEN_OR_OR:
        case DIAMOND_TOKEN_OR:
            return PREC_OR;
        case DIAMOND_TOKEN_AND_AND:
        case DIAMOND_TOKEN_AND:
            return PREC_AND;
        case DIAMOND_TOKEN_EQUAL_EQUAL:
        case DIAMOND_TOKEN_BANG_EQUAL:
        case DIAMOND_TOKEN_SPACESHIP:
            return PREC_EQUALITY;
        case DIAMOND_TOKEN_LESS:
        case DIAMOND_TOKEN_LESS_EQUAL:
        case DIAMOND_TOKEN_GREATER:
        case DIAMOND_TOKEN_GREATER_EQUAL:
        case DIAMOND_TOKEN_IS:
            return PREC_COMPARISON;
        case DIAMOND_TOKEN_LESS_LESS:
        case DIAMOND_TOKEN_GREATER_GREATER:
        case DIAMOND_TOKEN_AMPERSAND:
        case DIAMOND_TOKEN_PIPE:
        case DIAMOND_TOKEN_CARET:
            return PREC_SHIFT;
        case DIAMOND_TOKEN_PLUS:
        case DIAMOND_TOKEN_MINUS:
            return PREC_TERM;
        case DIAMOND_TOKEN_STAR:
        case DIAMOND_TOKEN_SLASH:
        case DIAMOND_TOKEN_PERCENT:
            return PREC_FACTOR;
        default:
            return PREC_NONE;
    }
}

/* `**` lexes as two `*` tokens (a hash-rest pattern `{**rest}` needs them
 * separate); in operator position, two touching `*`s are exponentiation. */
bool power_operator_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_STAR)return false;
    DiamondLexer lookahead=compiler->lexer;
    const DiamondToken next=diamond_lexer_next(&lookahead);
    return next.kind==DIAMOND_TOKEN_STAR&&
        next.span.start==compiler->current.span.start+1;
}

Precedence infix_precedence(const Compiler *compiler) {
    return power_operator_ahead(compiler)?PREC_POWER:
        token_precedence(compiler->current.kind);
}

uint16_t parse_integer(Compiler *compiler) {
    const DiamondSpan span = compiler->previous.span;
    int64_t value = 0;
    int base = 10;
    size_t first = 0;
    if (span.length > 2 && compiler->source[span.start] == '0') {
        const char prefix = compiler->source[span.start + 1];
        base = prefix == 'x' || prefix == 'X' ? 16 : prefix == 'b' || prefix == 'B' ? 2 :
               prefix == 'o' || prefix == 'O' ? 8 : 10;
        if (base != 10) first = 2;
    }
    bool too_large = false;
    for (size_t index = first; index < span.length; index++) {
        if(compiler->source[span.start+index]=='_') continue;
        const int digit = (int)hex_digit_value(compiler->source[span.start + index]);
        if (value > (INT64_MAX - digit) / base) {
            too_large = true;
            break;
        }
        value = value * base + digit;
    }
    if (too_large) {
        /* Past 64 bits there is no inline constant to hold the value, so the
         * literal is built at run time from its digits: "digits".to_i(base)
         * yields the arbitrary-precision Int, and needs no new constant kind
         * in the bytecode, the cache or standalone builds. */
        const uint16_t text = allocate_register(compiler);
        const uint16_t literal = add_string_range(compiler, span.start + first,
            span.length - first, span);
        emit_instruction(compiler, DIAMOND_OP_STRING, text, literal, 0, 2);
        const uint16_t base_register = allocate_register(compiler);
        const uint16_t base_constant = add_constant(compiler, DIAMOND_INT(base));
        emit_instruction(compiler, DIAMOND_OP_CONSTANT, base_register, base_constant, 0, 2);
        const uint16_t result = emit_invoke_named(compiler, text, "to_i", &base_register, 1);
        compiler->known_types[result] = DIAMOND_TYPE_INT;
        return result;
    }
    const uint16_t destination = allocate_register(compiler);
    const uint16_t constant = add_constant(compiler, DIAMOND_INT(value));
    emit_instruction(compiler, DIAMOND_OP_CONSTANT, destination, constant, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_INT;
    return destination;
}

uint16_t parse_float(Compiler *compiler) {
    const DiamondSpan span = compiler->previous.span;
    char buffer[80];
    size_t length = 0;
    for (size_t index = 0; index < span.length; index++) {
        const char ch = compiler->source[span.start + index];
        if (ch == '_') continue;
        if (length >= sizeof(buffer) - 1) {
            fail(compiler, span, "float literal is too long");
            return 0;
        }
        buffer[length++] = ch;
    }
    buffer[length] = '\0';
    errno = 0;
    char *end = nullptr;
    const double value = strtod(buffer, &end);
    if (end != buffer + length ||
        (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL))) {
        fail(compiler, span, "float literal is too large");
        return 0;
    }
    const uint16_t destination = allocate_register(compiler);
    const uint16_t constant = add_constant(compiler, DIAMOND_FLOAT(value));
    emit_instruction(compiler, DIAMOND_OP_CONSTANT, destination, constant, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
    return destination;
}

uint16_t parse_string(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    const size_t end=span.start+span.length-1;
    size_t piece=span.start+1;uint16_t result=UINT16_MAX;
    while(piece<end&&!compiler->failed) {
        size_t index=piece;
        while(index+1<end) {
            if(compiler->source[index]=='\\') {index+=2;continue;}
            if(compiler->source[index]=='#'&&compiler->source[index+1]=='{')break;
            index++;
        }
        if(index+1>=end)index=end;
        const uint16_t literal_register=allocate_register(compiler);
        const uint16_t literal=add_string_range(compiler,piece,index-piece,span);
        emit_instruction(compiler,DIAMOND_OP_STRING,literal_register,literal,0,2);
        if(result==UINT16_MAX)result=literal_register;
        else {
            const uint16_t joined=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ADD,joined,result,literal_register,3);
            result=joined;
        }
        if(index>=end)break;
        DiamondLexer outer_lexer=compiler->lexer;
        const DiamondToken outer_current=compiler->current;
        const DiamondToken outer_previous=compiler->previous;
        DiamondLexer embedded={.source=compiler->source,.current=index+2,
            .start=index+2,.line=span.line,.column=span.column+(index-span.start)+2,
            .token_line=span.line,.token_column=span.column+(index-span.start)+2};
        compiler->lexer=embedded;
        compiler->current=diamond_lexer_next(&compiler->lexer);
        const uint16_t value=parse_expression(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE)
            fail(compiler,compiler->current.span,"expected '}' after interpolation");
        const size_t close=compiler->current.span.start;
        compiler->lexer=outer_lexer;compiler->current=outer_current;
        compiler->previous=outer_previous;
        const uint16_t converted=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_TO_STRING,converted,value,0,2);
        const uint16_t joined=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_ADD,joined,result,converted,3);
        result=joined;piece=close+1;
    }
    if(result==UINT16_MAX) {
        result=allocate_register(compiler);
        const uint16_t string=add_string(compiler,span);
        emit_instruction(compiler,DIAMOND_OP_STRING,result,string,0,2);
    }
    compiler->known_types[result]=DIAMOND_TYPE_STRING;return result;
}

uint16_t parse_symbol(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    const DiamondSpan name_span={.start=span.start+1,.length=span.length-1,
        .line=span.line,.column=span.column+1};
    const uint16_t destination=allocate_register(compiler);
    const uint16_t name=add_name_string(compiler,name_span);
    emit_instruction(compiler,DIAMOND_OP_SYMBOL,destination,name,0,2);
    compiler->known_types[destination]=DIAMOND_TYPE_SYMBOL;
    return destination;
}

uint16_t parse_literal(Compiler *compiler) {
    const uint16_t destination = allocate_register(compiler);
    if (compiler->previous.kind == DIAMOND_TOKEN_NIL) {
        /* No NIL opcode needed: run_chunk already zero-inits every
         * register in [0, register_count), and this destination is a
         * sole writer (freshly allocated for this literal). */
        compiler->known_types[destination]=DIAMOND_TYPE_NIL;
    } else {
        emit_instruction(compiler, DIAMOND_OP_BOOL, destination,
                         compiler->previous.kind == DIAMOND_TOKEN_TRUE, 0, 2);
        compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
    }
    return destination;
}

/* A bare name that is no local, capture or function. A capitalized one is
 * most likely a class or module written as a value, as in Ruby's
 * `assert_raises(RuntimeError)`; Diamond's classes are not values, so say
 * that rather than calling it a local variable. */
static void fail_undefined_name(Compiler *compiler,DiamondSpan name) {
    const char first=compiler->source[name.start];
    if(first<'A'||first>'Z') {
        fail(compiler,name,"undefined local variable");
        return;
    }
    char message[DIAMOND_MAX_FUNCTION_NAME*2+200];
    const int length=name.length>DIAMOND_MAX_FUNCTION_NAME?DIAMOND_MAX_FUNCTION_NAME:(int)name.length;
    const char *text=compiler->source+name.start;
    if(find_class(compiler,name)>=0)
        snprintf(message,sizeof message,"'%.*s' is a class, and a class is not a value: "
            "call %.*s.new(...) or a class method, or pass it as a type argument, as in "
            "f[%.*s](...)",length,text,length,text,length,text);
    else if(find_module(compiler,name)>=0)
        snprintf(message,sizeof message,"'%.*s' is a module, and a module is not a value: "
            "call its methods, as in %.*s.name(...)",length,text,length,text);
    else
        snprintf(message,sizeof message,"undefined constant '%.*s'",length,text);
    fail(compiler,name,message);
}

uint16_t parse_identifier(Compiler *compiler) {
    const int local = find_local(compiler, compiler->previous.span);
    if (local < 0) {
        for(size_t i=compiler->enclosing_local_count;i>0;i--) {
            if(!spans_equal(compiler,compiler->enclosing_locals[i-1].name,
                            compiler->previous.span)) continue;
            size_t capture=0;
            while(capture<compiler->capture_count &&
                  compiler->capture_registers[capture]!=compiler->enclosing_locals[i-1].reg)
                capture++;
            if(capture==compiler->capture_count) {
                if(capture==DIAMOND_MAX_CAPTURES){fail(compiler,compiler->previous.span,"too many captured variables");return 0;}
                compiler->capture_registers[compiler->capture_count++]=compiler->enclosing_locals[i-1].reg;
            }
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,destination,(uint8_t)capture,0,2);
            return destination;
        }
        /* A top-level `def`'s bare name, used as a value rather than
         * called outright (parse_name already routed the call-with-
         * parens case to parse_call, never reaching here) -- previously
         * always "undefined local variable", since a top-level function
         * is otherwise never registered as anything parse_identifier's
         * local/capture lookups above can find. A nested `def` already
         * works this way (compile_definition registers it as an
         * ordinary local holding a zero-or-more-capture DIAMOND_OP_
         * CLOSURE value); this is the same closure value with zero
         * captures, for a function that needs none since it isn't
         * nested inside anything with locals to close over. find_function
         * already applies the same owner_class==DIAMOND_OWNER_NONE && !nested
         * filter find_top_level_function (src/vm.c) uses at runtime, so
         * this can never resolve a class/module method or another
         * function's own nested def by bare name. */
        const int function_index = find_function(compiler, compiler->previous.span);
        if (function_index >= 0) {
            const uint16_t destination = allocate_register(compiler);
            emit_opcode(compiler, DIAMOND_OP_CLOSURE);
            emit_register(compiler,destination);
            emit_function_index(compiler, (size_t)function_index);
            emit_byte(compiler, 0);
            publish_function_callable_type(compiler,destination,
                compiler->program->functions[(size_t)function_index]);
            return destination;
        }
        if(compiler->discovery_pass) {
            note_self_reference(compiler,compiler->previous.span);
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
            return destination;
        }
        fail_undefined_name(compiler,compiler->previous.span); return 0;
    }
    if(!compiler->locals[(size_t)local].captured)
        return compiler->locals[(size_t)local].reg;
    /* `captured` may have been set by a nested def in a sibling if/else
     * branch that doesn't dominate this read, so the register may not
     * actually be boxed yet on this runtime path. BOX_LOCAL is idempotent
     * (a no-op once the register already holds a Cell), so re-emitting it
     * here guarantees GET_CELL is always safe, regardless of which branch
     * ran. */
    emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,
                     compiler->locals[(size_t)local].reg,0,0,1);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_GET_CELL,destination,
                     compiler->locals[(size_t)local].reg,0,2);
    compiler->known_types[destination]=
        compiler->known_types[compiler->locals[(size_t)local].reg];
    compiler->known_type_sets[destination]=
        compiler->known_type_sets[compiler->locals[(size_t)local].reg];
    compiler->tooling_type_sets[destination]=
        compiler->tooling_type_sets[compiler->locals[(size_t)local].reg];
    return destination;
}

/* Is `function_index` a `def self.x` function of a module other than the
 * one being compiled? Those are reached as `Module.x(...)`; a bare `x(...)`
 * outside the module must not find one (it would shadow a builtin such
 * as sqrt, or a top-level function of the same name). */
static bool function_belongs_to_other_module(const Compiler *compiler,
                                             size_t function_index) {
    for(size_t module=0;module<compiler->program->module_count;module++) {
        if((int)module==compiler->current_module)continue;
        const DiamondModule *owner=compiler->program->modules[module];
        for(size_t index=0;index<owner->singleton_method_count;index++)
            if(owner->singleton_methods[index].function_index==function_index&&
               !owner->singleton_methods[index].needs_receiver)
                return true;
    }
    return false;
}

int find_function(const Compiler *compiler, DiamondSpan name) {
    for (size_t index = compiler->program->function_count; index > 0; index--) {
        const size_t function_index = index - 1;
        const char *candidate = compiler->program->functions[function_index]->name;
        if (compiler->program->functions[function_index]->owner_class != DIAMOND_OWNER_NONE ||
            compiler->program->functions[function_index]->nested) continue;
        size_t length = 0;
        while (candidate[length] != '\0') length++;
        if (length != name.length) continue;
        bool equal = true;
        for (size_t character = 0; character < length; character++) {
            if (candidate[character] != compiler->source[name.start + character]) {
                equal = false;
                break;
            }
        }
        if (equal && !function_belongs_to_other_module(compiler, function_index))
            return (int)function_index;
    }
    return -1;
}

bool name_equals(const Compiler *compiler, const char *candidate,
                        DiamondSpan name, bool skip_at) {
    const size_t start = skip_at ? 1 : 0;
    size_t length = 0;
    while (candidate[length] != '\0') length++;
    if (name.length - start != length) return false;
    for (size_t index = 0; index < length; index++) {
        if (candidate[index] != compiler->source[name.start + start + index]) return false;
    }
    return true;
}

DiamondFunction *compiler_add_function(Compiler *compiler,
                                               size_t *function_index) {
    if(!compiler->discovery_pass&&
       compiler->next_function_claim<compiler->program->function_count&&
       compiler->program->functions[compiler->next_function_claim]
           ->declared_by_discovery) {
        *function_index=compiler->next_function_claim++;
        DiamondFunction *function=compiler->program->functions[*function_index];
        /* This slot was populated moments ago by diamond_compile_impl's
         * own "reserve every compiler-created function" loop, which
         * clones it from `discovery` via diamond_function_copy -- always
         * combined-allocated, template or not -- so this must go through
         * the combined-aware free, not raw free() on each field. */
        const bool calls_itself=function->calls_itself;
        diamond_function_free_arrays(function);
        memset(function,0,sizeof *function);
        function->calls_itself=calls_itself;
        return function;
    }
    DiamondFunction *function=diamond_program_add_function(compiler->program);
    if(function!=nullptr)*function_index=compiler->program->function_count-1;
    return function;
}

int find_class(const Compiler *compiler, DiamondSpan name) {
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module]->name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",scope,
                (int)name.length,compiler->source+name.start);
            if(written>0&&(size_t)written<sizeof qualified)
                for(size_t index=0;index<compiler->program->class_count;index++)
                    if(strcmp(compiler->program->classes[index]->name,qualified)==0)
                        return (int)index;
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    for (size_t index = 0; index < compiler->program->class_count; index++) {
        if (name_equals(compiler, compiler->program->classes[index]->name, name, false))
            return (int)index;
    }
    return -1;
}

int find_interface(const Compiler *compiler,DiamondSpan name) {
    if(compiler->current_module>=0) {
        char qualified[DIAMOND_MAX_FUNCTION_NAME];
        const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",
            compiler->program->modules[(size_t)compiler->current_module]->name,
            (int)name.length,compiler->source+name.start);
        if(written>0&&(size_t)written<sizeof qualified)
            for(size_t index=0;index<compiler->program->interface_count;index++)
                if(strcmp(compiler->program->interfaces[index]->name,qualified)==0)
                    return (int)index;
    }
    for(size_t index=0;index<compiler->program->interface_count;index++)
        if(name_equals(compiler,compiler->program->interfaces[index]->name,name,false))
            return (int)index;
    return -1;
}

int find_module(const Compiler *compiler,DiamondSpan name) {
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module]->name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",scope,
                (int)name.length,compiler->source+name.start);
            if(written>0&&(size_t)written<sizeof qualified)
                for(size_t index=0;index<compiler->program->module_count;index++)
                    if(strcmp(compiler->program->modules[index]->name,qualified)==0)
                        return (int)index;
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    for(size_t index=0;index<compiler->program->module_count;index++)
        if(name_equals(compiler,compiler->program->modules[index]->name,name,false))
            return (int)index;
    return -1;
}

static bool stored_name_equals(const char *stored,const char *name) {
    return strcmp(stored,name)==0;
}

int find_class_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->class_count;index++)
        if(stored_name_equals(compiler->program->classes[index]->name,name))
            return (int)index;
    return -1;
}

/* Resolves a class name consume_qualified_name already collected into a
 * plain C string -- used by resolve_type_name (parameter/return type
 * annotations and, via resolve_type_name, `is` checks) and by superclass
 * resolution, neither of which could previously represent an explicit
 * `A::B` path at all (find_class_name alone is exact-string-only; find_class
 * takes a single DiamondSpan token, one identifier, never a qualified
 * path). Walks the current-module scope outward first, trying `name`
 * qualified against each enclosing module in turn -- the same order
 * find_class already uses for bare `.new()`/superclass references -- and
 * only once none of those match falls back to `name` taken bare/as-given
 * (handles a fully-qualified path like `Shapes::Base`, or an ordinary
 * un-nested class name). Trying the bare name first (the previous order)
 * meant a bare sibling reference (`Table` from code lexically inside
 * `module Arel`) would resolve to an unrelated top-level class of the same
 * name whenever the enclosing program happened to declare one, instead of
 * the module's own `Arel::Table` -- silently wrong instead of merely
 * unresolved, since both names exist and only one is intended. Fixed so a
 * bare sibling name resolves for type annotations and `is` checks the same
 * way it already does for construction and inheritance. */
int find_class_qualified_or_scoped(const Compiler *compiler,const char *name) {
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module]->name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%s",scope,name);
            if(written>0&&(size_t)written<sizeof qualified) {
                const int found=find_class_name(compiler,qualified);
                if(found>=0)return found;
            }
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    return find_class_name(compiler,name);
}

int find_interface_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->interface_count;index++)
        if(stored_name_equals(compiler->program->interfaces[index]->name,name))
            return (int)index;
    return -1;
}

int find_module_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->module_count;index++)
        if(stored_name_equals(compiler->program->modules[index]->name,name))
            return (int)index;
    return -1;
}

static int find_namespace_constant_name(const Compiler *compiler,
                                        const char *name) {
    for(size_t index=0;index<compiler->program->namespace_constant_count;index++)
        if(strcmp(compiler->program->namespace_constants[index],name)==0)
            return (int)index;
    return -1;
}

/* Constants use lexical namespace lookup, ending at the program scope. */
static const char *constant_scope(const Compiler *compiler) {
    if(compiler->current_class>=0)
        return compiler->program->classes[(size_t)compiler->current_class]->name;
    if(compiler->current_module>=0)
        return compiler->program->modules[(size_t)compiler->current_module]->name;
    return "";
}

/* Appends `module_index` to an include list unless already present, moving
 * nothing: a repeated `include` (discovery pass, then real pass, or a
 * reopened class) must not grow the list. False when the list already
 * holds DIAMOND_MAX_INCLUDED_MODULES other modules. */
bool record_included_module(uint16_t *list,uint8_t *count,uint16_t module_index) {
    for(size_t index=0;index<*count;index++)
        if(list[index]==module_index)return true;
    if(*count>=DIAMOND_MAX_INCLUDED_MODULES)return false;
    list[(*count)++]=module_index;
    return true;
}

/* Constants inherited through `include`: one constant in `module_index`
 * itself, then in every module it includes (latest `include` first, as
 * method resolution prefers the latest). `depth` only bounds a hypothetical
 * include cycle. */
static int find_module_chain_constant(const Compiler *compiler,int module_index,
                                      const char *name,int depth) {
    const DiamondModule *module=compiler->program->modules[(size_t)module_index];
    char qualified[DIAMOND_MAX_FUNCTION_NAME];
    const int written=snprintf(qualified,sizeof qualified,"%s::%s",module->name,name);
    if(written>0&&(size_t)written<sizeof qualified) {
        const int found=find_namespace_constant_name(compiler,qualified);
        if(found>=0)return found;
    }
    if(depth>=DIAMOND_MAX_MODULES)return -1;
    for(size_t index=module->included_module_count;index>0;index--) {
        const int found=find_module_chain_constant(compiler,
            module->included_modules[index-1],name,depth+1);
        if(found>=0)return found;
    }
    return -1;
}

/* A class's own constant, its included modules', then its superclass's, up
 * the chain -- the order Ruby's ancestor walk uses. */
static int find_class_chain_constant(const Compiler *compiler,int class_index,
                                     const char *name) {
    for(size_t depth=0;class_index>=0&&depth<=DIAMOND_MAX_CLASSES;depth++) {
        const DiamondClass *class=compiler->program->classes[(size_t)class_index];
        char qualified[DIAMOND_MAX_FUNCTION_NAME];
        const int written=snprintf(qualified,sizeof qualified,"%s::%s",class->name,name);
        if(written>0&&(size_t)written<sizeof qualified) {
            const int found=find_namespace_constant_name(compiler,qualified);
            if(found>=0)return found;
        }
        for(size_t index=class->included_module_count;index>0;index--) {
            const int found=find_module_chain_constant(compiler,
                class->included_modules[index-1],name,0);
            if(found>=0)return found;
        }
        class_index=class->superclass==DIAMOND_NO_CLASS?-1:(int)class->superclass;
    }
    return -1;
}

/* Lexical namespaces first, then the ancestors of the innermost class or
 * module (superclasses and included modules), then the program scope --
 * Ruby's order, so a subclass sees its parents' constants but an
 * enclosing namespace's constant still beats an inherited one. */
int find_lexical_constant_name(const Compiler *compiler,const char *name) {
    char scope[DIAMOND_MAX_FUNCTION_NAME];
    (void)snprintf(scope,sizeof scope,"%s",constant_scope(compiler));
    while(scope[0]!='\0') {
        char qualified[DIAMOND_MAX_FUNCTION_NAME];
        const int written=snprintf(qualified,sizeof qualified,"%s::%s",scope,name);
        if(written>0&&(size_t)written<sizeof qualified) {
            const int found=find_namespace_constant_name(compiler,qualified);
            if(found>=0)return found;
        }
        char *separator=strrchr(scope,':');
        if(separator==nullptr)break;
        separator[-1]='\0';
    }
    int inherited=-1;
    if(compiler->current_class>=0)
        inherited=find_class_chain_constant(compiler,compiler->current_class,name);
    else if(compiler->current_module>=0)
        inherited=find_module_chain_constant(compiler,compiler->current_module,name,0);
    if(inherited>=0)return inherited;
    return find_namespace_constant_name(compiler,name);
}

/* `Namespace::NAME` where NAME lives in an ancestor of Namespace (a
 * superclass or included module) rather than in Namespace itself. */
int find_qualified_inherited_constant(const Compiler *compiler,
                                             const char *qualified) {
    const char *separator=strrchr(qualified,':');
    if(separator==nullptr||separator==qualified||separator[-1]!=':')return -1;
    char prefix[DIAMOND_MAX_FUNCTION_NAME];
    const size_t prefix_length=(size_t)(separator-1-qualified);
    if(prefix_length>=sizeof prefix)return -1;
    memcpy(prefix,qualified,prefix_length);
    prefix[prefix_length]='\0';
    const char *name=separator+1;
    const int class_index=find_class_qualified_or_scoped(compiler,prefix);
    if(class_index>=0)return find_class_chain_constant(compiler,class_index,name);
    const int module_index=find_module_name(compiler,prefix);
    if(module_index>=0)
        return find_module_chain_constant(compiler,module_index,name,0);
    return -1;
}

int find_namespace_constant(const Compiler *compiler,DiamondSpan name) {
    char text[DIAMOND_MAX_FUNCTION_NAME];
    if(name.length>=sizeof text)return -1;
    (void)snprintf(text,sizeof text,"%.*s",(int)name.length,
                   compiler->source+name.start);
    return find_lexical_constant_name(compiler,text);
}

/* Indexed writes mutate the value, not the constant binding. Discovery
 * must tolerate forward constants here just as it does in parse_name. */
int load_constant_receiver(Compiler *compiler,DiamondSpan name) {
    /* ENV["K"] = v and ARGV << v at the start of a statement reach here, since ENV and ARGV
     * are not constants; read them as the expression form does. */
    if(find_function(compiler,name)<0) {
        const bool is_env=name_equals(compiler,"ENV",name,false);
        if(is_env||name_equals(compiler,"ARGV",name,false)) {
            const uint16_t receiver=allocate_register(compiler);
            emit_opcode(compiler,is_env?DIAMOND_OP_ENV:DIAMOND_OP_ARGV);
            emit_register(compiler,receiver);
            compiler->known_types[receiver]=is_env?DIAMOND_TYPE_HASH:DIAMOND_TYPE_ARRAY;
            return (int)receiver;
        }
    }
    const int constant=find_namespace_constant(compiler,name);
    if(constant<0&&!(compiler->discovery_pass&&
       compiler->source[name.start]>='A'&&compiler->source[name.start]<='Z'))
        return -1;
    const uint16_t receiver=allocate_register(compiler);
    if(constant>=0)
        emit_instruction(compiler,DIAMOND_OP_GET_NAMESPACE_CONSTANT,receiver,
                         (uint16_t)constant,0,2);
    else
        emit_instruction(compiler,DIAMOND_OP_NIL,receiver,0,0,1);
    return (int)receiver;
}

uint16_t store_namespace_constant(Compiler *compiler,DiamondSpan name,
                                         uint16_t value) {
    if(compiler->in_function) {
        fail(compiler,name,"constants cannot be defined inside functions");return value;
    }
    char qualified[DIAMOND_MAX_FUNCTION_NAME];
    const char *scope=constant_scope(compiler);
    const int written=snprintf(qualified,sizeof qualified,"%s%s%.*s",scope,
        scope[0]=='\0'?"":"::",(int)name.length,compiler->source+name.start);
    if(written<0||(size_t)written>=sizeof qualified) {
        fail(compiler,name,"constant name is too long");return value;
    }
    int constant=find_namespace_constant_name(compiler,qualified);
    if(constant>=0&&compiler->program->namespace_constant_defined[(size_t)constant]) {
        fail(compiler,name,"constant is already defined");return value;
    }
    if(constant<0) {
        if(!diamond_program_reserve_namespace_constants(compiler->program,
               compiler->program->namespace_constant_count+1)) {
            fail(compiler,name,"too many namespace constants");return value;
        }
        constant=(int)compiler->program->namespace_constant_count++;
        (void)snprintf(compiler->program->namespace_constants[(size_t)constant],
                       DIAMOND_MAX_FUNCTION_NAME,"%s",qualified);
    }
    compiler->program->namespace_constant_defined[(size_t)constant]=true;
    emit_instruction(compiler,DIAMOND_OP_SET_NAMESPACE_CONSTANT,
                     (uint16_t)constant,value,0,2);
    return value;
}

bool append_span_name(Compiler *compiler,char *buffer,size_t capacity,
                             DiamondSpan span) {
    const size_t used=strlen(buffer);
    if(used+span.length>=capacity)return false;
    for(size_t index=0;index<span.length;index++)
        buffer[used+index]=compiler->source[span.start+index];
    buffer[used+span.length]='\0';return true;
}

bool declaration_name(Compiler *compiler,char *buffer,size_t capacity,
                             DiamondSpan local) {
    buffer[0]='\0';
    if(compiler->current_module>=0) {
        const char *parent=
            compiler->program->modules[(size_t)compiler->current_module]->name;
        if(strlen(parent)+2>=capacity)return false;
        (void)snprintf(buffer,capacity,"%s::",parent);
    }
    return append_span_name(compiler,buffer,capacity,local);
}

bool consume_qualified_name(Compiler *compiler,char *buffer,
                                   size_t capacity) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER)return false;
    buffer[0]='\0';
    if(!append_span_name(compiler,buffer,capacity,compiler->current.span))return false;
    advance_token(compiler);
    while(compiler->current.kind==DIAMOND_TOKEN_DOUBLE_COLON) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
           strlen(buffer)+2>=capacity)return false;
        (void)strncat(buffer,"::",capacity-strlen(buffer)-1);
        if(!append_span_name(compiler,buffer,capacity,compiler->current.span))return false;
        advance_token(compiler);
    }
    return true;
}

int resolve_type_name(Compiler *compiler,const char *name,
                             DiamondSpan diagnostic) {
    static const struct {const char *name;uint16_t type;} builtins[]={
        {"Int",DIAMOND_TYPE_INT},{"Float",DIAMOND_TYPE_FLOAT},
        {"String",DIAMOND_TYPE_STRING},
        {"Bool",DIAMOND_TYPE_BOOL},{"Nil",DIAMOND_TYPE_NIL},
        {"Array",DIAMOND_TYPE_ARRAY},{"Hash",DIAMOND_TYPE_HASH},
        {"Callable",DIAMOND_TYPE_CALLABLE},{"Sized",DIAMOND_TYPE_SIZED},
        {"Symbol",DIAMOND_TYPE_SYMBOL}};
    for(size_t index=0;index<sizeof builtins/sizeof builtins[0];index++)
        if(strcmp(name,builtins[index].name)==0)return builtins[index].type;
    for(size_t index=0;index<compiler->function->type_variable_count;index++)
        if(strcmp(name,compiler->function->type_variables[index])==0)
            return DIAMOND_TYPE_VARIABLE_BASE+(int)index;
    int found=find_interface_name(compiler,name);
    if(found>=0)return DIAMOND_TYPE_INTERFACE_BASE+found;
    found=find_class_qualified_or_scoped(compiler,name);
    if(found>=0)return DIAMOND_TYPE_CLASS_BASE+found;
    /* Native types come after classes, so a program's own class named
     * Channel or Statement still wins. Encoded above every real id; see
     * DIAMOND_NATIVE_TYPE_OPERAND_BASE. */
    uint8_t native_kind=0;
    if(diamond_native_type_kind(name,&native_kind))
        return DIAMOND_NATIVE_TYPE_OPERAND_BASE+native_kind;
    /* During diamond_compile's own discovery pass only: a type annotation
     * naming a class/interface declared *later* in the source hasn't
     * been discovered yet by this point in discovery's own walk (that's
     * the whole reason discovery exists -- see diamond_compile's own
     * comment). Tolerate it here rather than aborting discovery, so it
     * can keep walking to the real, later declaration; the real second
     * pass runs with every declaration already known up front, so it
     * either resolves this correctly there or, if the name genuinely
     * never exists, hits this exact fail() again for real. */
    if(!compiler->discovery_pass)
        fail(compiler,diagnostic,"unknown type annotation");
    return DIAMOND_TYPE_NIL;
}

int parse_type_annotation(Compiler *compiler) {
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    while(!compiler->failed) {
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected type annotation");break;
        }
        const DiamondSpan member_span=compiler->current.span;
        char type_name_buffer[DIAMOND_MAX_FUNCTION_NAME];
        if(!consume_qualified_name(compiler,type_name_buffer,
                                   sizeof type_name_buffer)) {
            fail(compiler,member_span,"type name is too long");break;
        }
        const int resolved=resolve_type_name(compiler,type_name_buffer,member_span);
        const bool native=diamond_type_operand_is_native(resolved);
        const uint16_t type=native?DIAMOND_TYPE_NATIVE:(uint16_t)resolved;
        if(set->count==DIAMOND_MAX_UNION_TYPES) {
            fail(compiler,compiler->current.span,"too many types in union");break;
        }
        uint16_t argument_set=DIAMOND_NO_TYPE_SET,second_argument_set=DIAMOND_NO_TYPE_SET;
        /* For a native type this byte is the object kind, not an arity. */
        uint8_t callable_arity=native?
            (uint8_t)(resolved-DIAMOND_NATIVE_TYPE_OPERAND_BASE):UINT8_MAX;
        uint16_t callable_return_set=DIAMOND_NO_TYPE_SET;
        bool callable_parameters_typed=false;
        uint16_t callable_parameter_sets[16];
        for(size_t index=0;index<16;index++)callable_parameter_sets[index]=DIAMOND_NO_TYPE_SET;
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            if(type==DIAMOND_TYPE_CALLABLE) {
                advance_token(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_INTEGER) {
                    size_t arity=0;
                    for(size_t index=0;index<compiler->current.span.length;index++) {
                        const char ch=compiler->source[compiler->current.span.start+index];
                        if(ch=='_')continue;
                        arity=arity*10+(size_t)(ch-'0');
                    }
                    if(arity>16) {
                        fail(compiler,compiler->current.span,
                             "Callable arity cannot exceed 16");break;
                    }
                    callable_arity=(uint8_t)arity;advance_token(compiler);
                } else if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
                    advance_token(compiler);callable_arity=0;
                    callable_parameters_typed=true;
                    skip_newlines(compiler);
                    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
                          !compiler->failed) {
                        if(callable_arity==16) {
                            fail(compiler,compiler->current.span,
                                 "Callable cannot exceed 16 parameters");break;
                        }
                        callable_parameter_sets[callable_arity++]=
                            (uint16_t)parse_type_annotation(compiler);
                        skip_newlines(compiler);
                        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
                        advance_token(compiler);
                        skip_newlines(compiler);
                    }
                    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                        fail(compiler,compiler->current.span,
                             "expected ']' after Callable parameters");break;
                    }
                    advance_token(compiler);
                } else {
                    fail(compiler,compiler->current.span,
                         "expected Callable arity or parameter list");break;
                }
                if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                    advance_token(compiler);
                    callable_return_set=(uint16_t)parse_type_annotation(compiler);
                }
            } else if(type==DIAMOND_TYPE_ARRAY||type==DIAMOND_TYPE_HASH||
                (compiler->discovery_pass&&(type==DIAMOND_TYPE_NIL||
                 (type>=DIAMOND_TYPE_CLASS_BASE&&type<DIAMOND_TYPE_CLASS_LIMIT)))||
                (type>=DIAMOND_TYPE_CLASS_BASE&&type<DIAMOND_TYPE_VARIABLE_BASE&&
                 (size_t)(type-DIAMOND_TYPE_CLASS_BASE)<compiler->program->class_count&&
                 compiler->program->classes[type-DIAMOND_TYPE_CLASS_BASE]->type_parameter[0]!='\0')) {
                advance_token(compiler);
                skip_newlines(compiler);
                argument_set=(uint16_t)parse_type_annotation(compiler);
                if(type==DIAMOND_TYPE_HASH||
                   (type>=DIAMOND_TYPE_CLASS_BASE&&type<DIAMOND_TYPE_CLASS_LIMIT&&
                    (size_t)(type-DIAMOND_TYPE_CLASS_BASE)<compiler->program->class_count&&
                    DIAMOND_CLASS_TYPE_PARAMETER_COUNT(compiler->program->classes[type-DIAMOND_TYPE_CLASS_BASE])==2)||
                   (compiler->discovery_pass&&compiler->current.kind==DIAMOND_TOKEN_COMMA)) {
                    skip_newlines(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                        fail(compiler,compiler->current.span,
                             type==DIAMOND_TYPE_HASH?
                             "expected ',' between Hash key and value types":
                             "expected ',' between class type arguments");break;
                    }
                    advance_token(compiler);
                    skip_newlines(compiler);
                    second_argument_set=(uint16_t)parse_type_annotation(compiler);
                }
            } else {
                fail(compiler,member_span,
                     "this type does not accept arguments");break;
            }
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                fail(compiler,compiler->current.span,
                     "expected ']' after collection type arguments");break;
            }
            advance_token(compiler);
        }
        DiamondTypeMember parsed=(DiamondTypeMember){
            .id=type,.argument_set=argument_set,
            .second_argument_set=second_argument_set,
            .callable_arity=callable_arity,
            .callable_return_set=callable_return_set,
            .callable_parameters_typed=callable_parameters_typed};
        for(size_t index=0;index<16;index++)
            parsed.callable_parameter_sets[index]=callable_parameter_sets[index];
        set=&compiler->function->type_sets[set_index];
        /* Discovery resolves a not-yet-declared class to Nil (see
         * resolve_type_name), so `Later | Nil` would look duplicated here;
         * the real pass, with every declaration known, checks for real. */
        for(size_t index=0;!compiler->discovery_pass&&index<set->count;index++)
            if(type_members_structurally_equal(&set->members[index],
                    compiler->function->type_sets,
                    compiler->function->type_set_count,&parsed,
                    compiler->function->type_sets,
                    compiler->function->type_set_count,0)) {
                fail(compiler,member_span,"duplicate type in union");break;
            }
        if(compiler->failed)break;
        set->members[set->count++]=parsed;
        if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
        advance_token(compiler);
    }
    return (int)set_index;
}

int field_index(Compiler *compiler, DiamondSpan name, bool create) {
    if (compiler->current_class < 0) {
        fail(compiler, name, "instance variable used outside a method");
        return -1;
    }
    DiamondClass *class = compiler->program->classes[(size_t)compiler->current_class];
    for (size_t index = 0; index < class->field_count; index++) {
        if (name_equals(compiler, class->fields[index], name, true)) return (int)index;
    }
    if (!create) return -1;
    if (!diamond_class_reserve_fields(class,class->field_count+1)) {
        fail(compiler, name, "too many instance variables");
        return -1;
    }
    char *field = class->fields[class->field_count];
    const size_t length = name.length - 1;
    if (length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "instance variable name is too long");
        return -1;
    }
    for (size_t i = 0; i < length; i++) field[i] = compiler->source[name.start + 1 + i];
    field[length] = '\0';
    return (int)class->field_count++;
}

/* Same shape as field_index just above (compile-time name -> slot
 * resolution, scoped to the current class), but for `@@name` class
 * variables rather than `@name` instance fields -- kept as its own
 * function rather than generalizing field_index/name_equals to a
 * variable skip count, since `@@` is the only two-char sigil in the
 * language and threading a skip-count parameter through name_equals'
 * 70-odd other call sites (all of which pass a plain bool today) isn't
 * worth it for one caller. */
static bool class_variable_name_equals(const Compiler *compiler,
        const char *candidate, DiamondSpan name) {
    size_t length = 0;
    while (candidate[length] != '\0') length++;
    if (name.length - 2 != length) return false;
    for (size_t index = 0; index < length; index++) {
        if (candidate[index] != compiler->source[name.start + 2 + index]) return false;
    }
    return true;
}

/* The class whose storage `@@var` refers to here: the enclosing class, or,
 * in a module's own body (its `def self.` functions and the instance
 * methods it contributes to including classes), a hidden class named
 * "<Module>.@@" that exists only to hold that module's class variables --
 * shared by all of them, as in Ruby. Source can't name the hidden class
 * (it isn't a valid constant). Created on the module's first @@ use; the
 * real pass finds the copy the discovery pass made by that same name.
 * -1 (after failing) outside any class or module. */
int class_variable_owner(Compiler *compiler, DiamondSpan name) {
    if (compiler->current_class >= 0) return compiler->current_class;
    if (compiler->current_module < 0) {
        fail(compiler, name, "class variable used outside a class or module");
        return -1;
    }
    char hidden[DIAMOND_MAX_FUNCTION_NAME];
    const int written = snprintf(hidden, sizeof hidden, "%s.@@",
        compiler->program->modules[(size_t)compiler->current_module]->name);
    if (written < 0 || (size_t)written >= sizeof hidden) {
        fail(compiler, name, "module name is too long for class variables");
        return -1;
    }
    for (size_t index = 0; index < compiler->program->class_count; index++) {
        DiamondClass *existing = compiler->program->classes[index];
        if (strcmp(existing->name, hidden) != 0) continue;
        if (!compiler->discovery_pass) existing->declared_by_discovery = false;
        return (int)index;
    }
    if (compiler->program->class_count == DIAMOND_MAX_CLASSES) {
        fail(compiler, name, "too many classes");
        return -1;
    }
    const size_t index = compiler->program->class_count;
    DiamondClass *class = diamond_program_class_slot(compiler->program, index);
    if (class == nullptr) {
        fail(compiler, name, "out of memory");
        return -1;
    }
    compiler->program->class_count++;
    memset(class, 0, sizeof *class);
    class->index = (uint16_t)index;
    memcpy(class->name, hidden, (size_t)written + 1);
    class->superclass = DIAMOND_NO_CLASS;
    return (int)index;
}

int class_variable_index(Compiler *compiler, DiamondSpan name, bool create) {
    const int owner = class_variable_owner(compiler, name);
    if (owner < 0) return -1;
    DiamondClass *class = compiler->program->classes[(size_t)owner];
    for (size_t index = 0; index < class->class_variable_count; index++) {
        if (class_variable_name_equals(compiler, class->class_variables[index], name))
            return (int)index;
    }
    if (!create) return -1;
    if (!diamond_class_reserve_class_variables(class,class->class_variable_count+1)) {
        fail(compiler, name, "too many class variables");
        return -1;
    }
    char *variable = class->class_variables[class->class_variable_count];
    const size_t length = name.length - 2;
    if (length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "class variable name is too long");
        return -1;
    }
    for (size_t i = 0; i < length; i++) variable[i] = compiler->source[name.start + 2 + i];
    variable[length] = '\0';
    return (int)class->class_variable_count++;
}

uint16_t module_field_name(Compiler *compiler,DiamondSpan name) {
    compiler->function->uses_instance_state=true;
    DiamondModule *module=
        compiler->program->modules[(size_t)compiler->current_module];
    const size_t length=name.length-1;
    for(size_t field=0;field<module->field_count;field++)
        if(strlen(module->fields[field])==length&&
           memcmp(module->fields[field],compiler->source+name.start+1,length)==0)
            return add_string_range(compiler,name.start+1,length,name);
    if(!diamond_module_reserve_fields(module,module->field_count+1)) {
        fail(compiler,name,"too many module instance variables");return 0;
    }
    if(length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,name,"instance variable name is too long");return 0;
    }
    memcpy(module->fields[module->field_count],compiler->source+name.start+1,length);
    module->fields[module->field_count][length]='\0';module->field_count++;
    return add_string_range(compiler,name.start+1,length,name);
}

static bool at_block_end(const Compiler *compiler) {
    return compiler->current.kind == DIAMOND_TOKEN_EOF ||
           compiler->current.kind == DIAMOND_TOKEN_ELSE ||
           compiler->current.kind == DIAMOND_TOKEN_ELSIF ||
           compiler->current.kind == DIAMOND_TOKEN_WHEN ||
           compiler->current.kind == DIAMOND_TOKEN_RESCUE ||
           compiler->current.kind == DIAMOND_TOKEN_ENSURE ||
           compiler->current.kind == DIAMOND_TOKEN_END;
}

static bool expression_finishes_block(const Compiler *compiler) {
    DiamondLexer lookahead=compiler->lexer;
    size_t nesting=0;
    DiamondToken token=compiler->current;
    for(;;) {
        if(token.kind==DIAMOND_TOKEN_EOF)return true;
        if(token.kind==DIAMOND_TOKEN_LEFT_PAREN||
           token.kind==DIAMOND_TOKEN_LEFT_BRACKET||
           token.kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
        else if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN||
                token.kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
            if(nesting>0)nesting--;
        } else if(token.kind==DIAMOND_TOKEN_NEWLINE&&nesting==0) {
            do token=diamond_lexer_next(&lookahead);
            while(token.kind==DIAMOND_TOKEN_NEWLINE);
            return token.kind==DIAMOND_TOKEN_EOF||
                   token.kind==DIAMOND_TOKEN_ELSE||
                   token.kind==DIAMOND_TOKEN_ELSIF||
                   token.kind==DIAMOND_TOKEN_WHEN||
                   token.kind==DIAMOND_TOKEN_RESCUE||
                   token.kind==DIAMOND_TOKEN_ENSURE||
                   token.kind==DIAMOND_TOKEN_END;
        }
        token=diamond_lexer_next(&lookahead);
        if(token.kind==DIAMOND_TOKEN_ERROR)return false;
    }
}

/* One assignment or expression statement. has_postfix: an if/unless modifier follows it, which
 * stops the block's return-type expectation from applying to the statement. */
uint16_t compile_expression_statement(Compiler *compiler,bool has_postfix) {
    if(index_assignment_ahead(compiler))
        return compile_index_assignment(compiler);
    if(index_compound_assignment_ahead(compiler))
        return compile_index_compound_assignment(compiler);
    if(multi_assignment_ahead(compiler))
        return compile_multi_assignment(compiler);
    if(compound_assignment_ahead(compiler))
        return compile_compound_assignment(compiler);
    if(assignment_ahead(compiler))
        return compile_assignment(compiler);
    if(!has_postfix&&
       compiler->function->return_type_set!=DIAMOND_NO_TYPE_SET&&
       expression_finishes_block(compiler))
        return parse_with_expected_set(compiler,
            compiler->function->return_type_set);
    return parse_expression(compiler);
}

/* Can the statement starting at the current token take a `while`/`until` modifier? Only a plain
 * expression or assignment: not a declaration, a keyword statement, or a block-form
 * control structure (whose own `while`/`until`/`end` the modifier scan could misread). */
static bool statement_takes_loop_modifier(const Compiler *compiler) {
    switch(compiler->current.kind) {
    case DIAMOND_TOKEN_DEF: case DIAMOND_TOKEN_CLOSURE: case DIAMOND_TOKEN_CLASS:
    case DIAMOND_TOKEN_SEALED: case DIAMOND_TOKEN_STRUCT: case DIAMOND_TOKEN_INTERFACE:
    case DIAMOND_TOKEN_MODULE: case DIAMOND_TOKEN_RETURN: case DIAMOND_TOKEN_RAISE:
    case DIAMOND_TOKEN_RETRY: case DIAMOND_TOKEN_BEGIN: case DIAMOND_TOKEN_BREAK:
    case DIAMOND_TOKEN_NEXT: case DIAMOND_TOKEN_REDO: case DIAMOND_TOKEN_IF:
    case DIAMOND_TOKEN_UNLESS: case DIAMOND_TOKEN_WHILE: case DIAMOND_TOKEN_UNTIL:
    case DIAMOND_TOKEN_LOOP: case DIAMOND_TOKEN_CASE:
        return false;
    default:
        return true;
    }
}

uint16_t compile_sequence(Compiler *compiler) {
    skip_newlines(compiler);
    uint16_t result = allocate_register(compiler);
    /* No NIL here: `result` is a sole writer for an empty block (still
     * correctly nil via run_chunk's zero-init), and is superseded by
     * the first statement's own result register whenever the block is
     * non-empty -- the highest-frequency NIL-elision site, since this
     * fires once per compiled block. */
    bool last_statement_diverges = false;

    while (!compiler->failed && !at_block_end(compiler)) {
        /* A narrowing belongs to the condition that produced it. One left over from the
         * previous statement (or the previous function body, whose last expression
         * was `v is String`) is keyed by a register number the next statement can
         * reuse, so a later `x ? a : b` would apply type sets that belong to another
         * function. */
        compiler->narrowing=(Narrowing){};
        if(compiler->debug_mode) {
            /* emit_opcode (called by emit_breakpoint_check, by way of
             * emit_instruction) always tags a freshly emitted instruction
             * with compiler->previous.span -- the *last consumed* token,
             * which at this exact point is still whatever ended the
             * *previous* statement, not this statement's own line. Every
             * other emit_debugger_pause/emit_breakpoint_check caller
             * (parse_debugger_call) doesn't have this problem: it always
             * runs after consuming its own call's tokens, so
             * compiler->previous is already the right line. Patch the
             * just-emitted opcode's own line/column table entry to this
             * statement's real position afterward, rather than threading
             * an explicit span through emit_opcode for every other caller
             * just for this one. */
            const DiamondSpan statement_span=compiler->current.span;
            const size_t check_offset=compiler->function->code_count;
            size_t source_line_offset=statement_span.start;
            while(source_line_offset>0&&compiler->source[source_line_offset-1]!='\n')
                source_line_offset--;
            emit_breakpoint_check(compiler,source_line_offset);
            if(check_offset<compiler->function->code_count) {
                compiler->function->lines[check_offset]=(uint32_t)statement_span.line;
                compiler->function->columns[check_offset]=(uint32_t)statement_span.column;
            }
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_DEF&&
           compiler->current.kind!=DIAMOND_TOKEN_CLOSURE&&
           statement_may_capture(compiler))
            mark_locals_captured(compiler);
        bool statement_is_raise = false;
        /* A statement that never falls through to the next one. With a
         * postfix condition (`return x if y == nil`), everything after it
         * runs only when the condition didn't hold -- see the narrowing
         * applied after the postfix condition below. */
        const bool statement_exits =
            compiler->current.kind == DIAMOND_TOKEN_RETURN ||
            compiler->current.kind == DIAMOND_TOKEN_RAISE ||
            compiler->current.kind == DIAMOND_TOKEN_BREAK ||
            compiler->current.kind == DIAMOND_TOKEN_NEXT ||
            compiler->current.kind == DIAMOND_TOKEN_REDO ||
            compiler->current.kind == DIAMOND_TOKEN_RETRY;
        DiamondLexer modifier_end={};
        const DiamondTokenKind postfix = postfix_modifier_ahead(compiler,&modifier_end);
        const bool has_postfix = postfix == DIAMOND_TOKEN_IF ||
                                 postfix == DIAMOND_TOKEN_UNLESS;
        const bool postfix_loop = (postfix == DIAMOND_TOKEN_WHILE ||
                                   postfix == DIAMOND_TOKEN_UNTIL) &&
                                  statement_takes_loop_modifier(compiler);
        const uint16_t body_result_slot = result;
        const uint16_t postfix_result = has_postfix
            ? allocate_register(compiler)
            : body_result_slot;
        const size_t condition_jump = has_postfix
            ? emit_jump(compiler, DIAMOND_OP_JUMP, 0)
            : SIZE_MAX;
        const size_t body_start = compiler->function->code_count;
        const bool declaration_statement =
            compiler->current.kind == DIAMOND_TOKEN_DEF ||
            compiler->current.kind == DIAMOND_TOKEN_CLOSURE ||
            compiler->current.kind == DIAMOND_TOKEN_CLASS ||
            compiler->current.kind == DIAMOND_TOKEN_SEALED ||
            compiler->current.kind == DIAMOND_TOKEN_STRUCT ||
            compiler->current.kind == DIAMOND_TOKEN_INTERFACE ||
            compiler->current.kind == DIAMOND_TOKEN_MODULE;
        if (postfix_loop) {
            result = parse_postfix_while(compiler,postfix == DIAMOND_TOKEN_UNTIL,
                                         modifier_end);
        } else if (compiler->current.kind == DIAMOND_TOKEN_DEF) {
            result = compile_definition(compiler,false);
        } else if (compiler->current.kind == DIAMOND_TOKEN_CLOSURE) {
            result = compile_definition(compiler,true);
        } else if (compiler->current.kind == DIAMOND_TOKEN_CLASS ||
                   compiler->current.kind == DIAMOND_TOKEN_SEALED) {
            result = compile_class(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_STRUCT) {
            result = compile_struct(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_INTERFACE) {
            result = compile_interface(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE) {
            result=compile_module(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RETURN) {
            result=compile_return(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RAISE) {
            result=compile_raise(compiler);
            statement_is_raise = true;
        } else if (compiler->current.kind == DIAMOND_TOKEN_RETRY) {
            result=compile_retry(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_BEGIN) {
            advance_token(compiler);
            result=compile_begin(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_BREAK ||
                   compiler->current.kind == DIAMOND_TOKEN_NEXT ||
                   compiler->current.kind == DIAMOND_TOKEN_REDO) {
            result=compile_loop_control(compiler);
        } else {
            result=compile_expression_statement(compiler,has_postfix);
        }
        if (has_postfix) {
            if (compiler->current.kind != postfix) {
                fail(compiler, compiler->current.span,
                     "expected postfix condition");
                break;
            }
            advance_token(compiler);
            emit_instruction(compiler, DIAMOND_OP_MOVE, postfix_result,
                             result, 0, 2);
            const size_t body_exit = emit_jump(compiler, DIAMOND_OP_JUMP, 0);
            const size_t condition_start = compiler->function->code_count;
            compiler->narrowing=(Narrowing){};
            const uint16_t condition = parse_expression(compiler);
            const Narrowing postfix_narrowing=condition_narrowing(compiler,condition);
            compiler->narrowing=(Narrowing){};
            const size_t body_jump = emit_jump(
                compiler,
                postfix == DIAMOND_TOKEN_IF
                    ? DIAMOND_OP_JUMP_IF_TRUE
                    : DIAMOND_OP_JUMP_IF_FALSE,
                condition);
            emit_instruction(compiler, DIAMOND_OP_NIL, postfix_result,
                             0, 0, 1);
            patch_jump(compiler, condition_jump, condition_start);
            patch_jump(compiler, body_exit, compiler->function->code_count);
            patch_jump(compiler, body_jump, body_start);
            result = postfix_result;
            /* `return "none" if x == nil`: past this line x isn't nil.
             * `raise ... unless x is String`: past it, x is a String. */
            if(statement_exits&&postfix_narrowing.valid) {
                if(postfix==DIAMOND_TOKEN_IF)
                    apply_scope_narrowing_facts(compiler,postfix_narrowing.when_false,
                        postfix_narrowing.when_false_count);
                else
                    apply_scope_narrowing_facts(compiler,postfix_narrowing.when_true,
                        postfix_narrowing.when_true_count);
            }
        }
        last_statement_diverges = statement_is_raise && !has_postfix;
        if (!has_postfix && declaration_statement &&
            (compiler->current.kind == DIAMOND_TOKEN_IF ||
             compiler->current.kind == DIAMOND_TOKEN_UNLESS)) {
            fail(compiler, compiler->current.span,
                 "postfix modifiers cannot follow declarations");
        } else if (compiler->current.kind == DIAMOND_TOKEN_NEWLINE) {
            skip_newlines(compiler);
        } else if (!at_block_end(compiler)) {
            fail(compiler, compiler->current.span, "expected newline after expression");
        }
    }
    compiler->sequence_diverges = last_statement_diverges;
    return result;
}
