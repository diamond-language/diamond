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

/* The core of the one-pass compiler: emission, type sets, lookups, calls, operators, literals and
 * expression parsing. The rest lives in the compiler_*.c files; what they share is compiler_internal.h. */

static bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index);
static bool type_member_satisfies(const Compiler *compiler,
                                  DiamondTypeMember known,
                                  DiamondTypeMember expected);
static bool type_members_equal(DiamondTypeMember left,DiamondTypeMember right);
static bool type_sets_equal_unordered(DiamondTypeSet left,DiamondTypeSet right);
static uint16_t parse_precedence(Compiler *compiler, Precedence precedence);
static void note_self_reference(Compiler *compiler,DiamondSpan name);
static uint16_t parse_array_literal(Compiler *compiler,
        const DiamondFunction *expected_function,size_t first_parameter,
        size_t parameter_count);
static int32_t homogeneous_spread_expectation(Compiler *compiler,
        const DiamondFunction *target,size_t first,size_t parameter_count);
static uint16_t compile_callable_value_block(Compiler *compiler,
        int32_t callable_set_index);
static uint16_t compile_expression_statement(Compiler *compiler,bool has_postfix);

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
        case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
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
        const DiamondInterface *interface=&compiler->program->interfaces[interface_index];
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

static bool type_members_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,DiamondTypeMember known,
    const DiamondTypeSet *expected_sets,DiamondTypeMember expected) {
    if(known.id==DIAMOND_TYPE_NATIVE||expected.id==DIAMOND_TYPE_NATIVE)
        return known.id==expected.id&&known.callable_arity==expected.callable_arity;
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
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        type_sets_satisfy_across(compiler,known_sets,known.second_argument_set,
                                 expected_sets,expected.second_argument_set);
}

static bool type_sets_satisfy_across(const Compiler *compiler,
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
    /* A native type (Time, Fiber, ...) matches only the same native kind,
     * which the member keeps in callable_arity. */
    if(known.id==DIAMOND_TYPE_NATIVE||expected.id==DIAMOND_TYPE_NATIVE)
        return known.id==expected.id&&known.callable_arity==expected.callable_arity;
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
                   !type_set_satisfies(compiler,wanted,actual))return false;
            }
        return expected.callable_return_set==DIAMOND_NO_TYPE_SET||
            (known.callable_return_set!=DIAMOND_NO_TYPE_SET&&
             type_set_satisfies(compiler,known.callable_return_set,
                                expected.callable_return_set));
    }
    if(expected.argument_set==DIAMOND_NO_TYPE_SET)return true;
    if(known.argument_set==DIAMOND_NO_TYPE_SET||
       !type_set_satisfies(compiler,known.argument_set,expected.argument_set))
        return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        expected.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        type_set_satisfies(compiler,known.second_argument_set,
                           expected.second_argument_set);
}

static bool type_set_contains_variable(const Compiler *compiler,uint16_t set_index) {
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
            compiler->program->interfaces[index].name:"<invalid type>";
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
                (member.argument_set==DIAMOND_NO_TYPE_SET&&
                 wanted.argument_set!=DIAMOND_NO_TYPE_SET&&
                 (member.id==DIAMOND_TYPE_ARRAY||member.id==DIAMOND_TYPE_HASH)&&
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
        if(!known_type_satisfies_one(compiler,known,set->members[index].id))continue;
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

static void check_call_argument_types(Compiler *compiler,const DiamondFunction *callee,
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

static bool grow_type_sets(DiamondFunction *function,size_t additional) {
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
static void publish_array_literal_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *values,size_t count) {
    bool have_tooling=false;
    for(size_t index=0;index<count;index++)
        if(compiler->tooling_type_sets[values[index]]>=0) {have_tooling=true;break;}
    if(!have_tooling)return;
    const int32_t joined=joined_tooling_value_type_set(compiler,values,count);
    compiler->tooling_type_sets[reg]=collection_type_set(compiler,
        DIAMOND_TYPE_ARRAY,joined,-1);
}

static void publish_array_spread_tooling_type(Compiler *compiler,uint16_t reg,
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

static void publish_callable_return_type(Compiler *compiler,uint16_t reg,
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

void publish_call_return_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target,const uint16_t *bindings,
        size_t binding_count) {
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
    publish_known_type_set(compiler,reg,return_set);
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
static uint16_t add_string_range_mode(Compiler *compiler,size_t start,size_t length,
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
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS ||
        span.length > DIAMOND_MAX_STRING_LENGTH) {
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
    for (size_t i = 0; i < span.length; i++)
        string->chars[i] = compiler->source[span.start + i];
    string->length = span.length;
    string->chars[span.length] = '\0';
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
    compiler->pending_singleton_fixups[slot].method = method;
}

/* Patches every pending call site recorded by
 * emit_pending_singleton_function_index for this exact declaration (there can
 * be more than one -- several earlier siblings can all forward-reference
 * the same later one), now that its real function_index is known.
 * Resolved entries are removed (swap-with-last), same convention as
 * every other fixed-capacity array removal in this file. */
void resolve_pending_singleton_fixups(Compiler *compiler,
        const DiamondMethod *method, size_t function_index) {
    for (size_t index = 0; index < compiler->pending_singleton_fixup_count; ) {
        if (compiler->pending_singleton_fixups[index].method == method) {
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

static Precedence token_precedence(DiamondTokenKind kind) {
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
static bool power_operator_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_STAR)return false;
    DiamondLexer lookahead=compiler->lexer;
    const DiamondToken next=diamond_lexer_next(&lookahead);
    return next.kind==DIAMOND_TOKEN_STAR&&
        next.span.start==compiler->current.span.start+1;
}

static Precedence infix_precedence(const Compiler *compiler) {
    return power_operator_ahead(compiler)?PREC_POWER:
        token_precedence(compiler->current.kind);
}

static uint16_t parse_integer(Compiler *compiler) {
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

static uint16_t parse_float(Compiler *compiler) {
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

static uint16_t parse_symbol(Compiler *compiler) {
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
        fail(compiler, compiler->previous.span, "undefined local variable"); return 0;
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
        const DiamondModule *owner=&compiler->program->modules[module];
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
            compiler->program->modules[(size_t)compiler->current_module].name);
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
            compiler->program->modules[(size_t)compiler->current_module].name,
            (int)name.length,compiler->source+name.start);
        if(written>0&&(size_t)written<sizeof qualified)
            for(size_t index=0;index<compiler->program->interface_count;index++)
                if(strcmp(compiler->program->interfaces[index].name,qualified)==0)
                    return (int)index;
    }
    for(size_t index=0;index<compiler->program->interface_count;index++)
        if(name_equals(compiler,compiler->program->interfaces[index].name,name,false))
            return (int)index;
    return -1;
}

int find_module(const Compiler *compiler,DiamondSpan name) {
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module].name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",scope,
                (int)name.length,compiler->source+name.start);
            if(written>0&&(size_t)written<sizeof qualified)
                for(size_t index=0;index<compiler->program->module_count;index++)
                    if(strcmp(compiler->program->modules[index].name,qualified)==0)
                        return (int)index;
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    for(size_t index=0;index<compiler->program->module_count;index++)
        if(name_equals(compiler,compiler->program->modules[index].name,name,false))
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
            compiler->program->modules[(size_t)compiler->current_module].name);
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
        if(stored_name_equals(compiler->program->interfaces[index].name,name))
            return (int)index;
    return -1;
}

int find_module_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->module_count;index++)
        if(stored_name_equals(compiler->program->modules[index].name,name))
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
        return compiler->program->modules[(size_t)compiler->current_module].name;
    return "";
}

/* Appends `module_index` to an include list unless already present, moving
 * nothing: a repeated `include` (discovery pass, then real pass, or a
 * reopened class) must not grow the list. The list is as large as the
 * module table, so a deduplicated append always fits. */
void record_included_module(uint8_t *list,uint8_t *count,uint8_t module_index) {
    for(size_t index=0;index<*count;index++)
        if(list[index]==module_index)return;
    list[(*count)++]=module_index;
}

/* Constants inherited through `include`: one constant in `module_index`
 * itself, then in every module it includes (latest `include` first, as
 * method resolution prefers the latest). `depth` only bounds a hypothetical
 * include cycle. */
static int find_module_chain_constant(const Compiler *compiler,int module_index,
                                      const char *name,int depth) {
    const DiamondModule *module=&compiler->program->modules[(size_t)module_index];
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
static int load_constant_receiver(Compiler *compiler,DiamondSpan name) {
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
                         (uint8_t)constant,0,2);
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
    if(constant>=0&&compiler->namespace_constant_defined[(size_t)constant]) {
        fail(compiler,name,"constant is already defined");return value;
    }
    if(constant<0) {
        if(compiler->program->namespace_constant_count==DIAMOND_MAX_NAMESPACE_CONSTANTS) {
            fail(compiler,name,"too many namespace constants");return value;
        }
        constant=(int)compiler->program->namespace_constant_count++;
        (void)snprintf(compiler->program->namespace_constants[(size_t)constant],
                       DIAMOND_MAX_FUNCTION_NAME,"%s",qualified);
    }
    compiler->namespace_constant_defined[(size_t)constant]=true;
    emit_instruction(compiler,DIAMOND_OP_SET_NAMESPACE_CONSTANT,
                     (uint8_t)constant,value,0,2);
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
            compiler->program->modules[(size_t)compiler->current_module].name;
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
            } else if(type==DIAMOND_TYPE_ARRAY||type==DIAMOND_TYPE_HASH) {
                advance_token(compiler);
                skip_newlines(compiler);
                argument_set=(uint16_t)parse_type_annotation(compiler);
                if(type==DIAMOND_TYPE_HASH) {
                    skip_newlines(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                        fail(compiler,compiler->current.span,
                             "expected ',' between Hash key and value types");break;
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
        for(size_t index=0;index<set->count;index++)
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
    if (class->field_count == DIAMOND_MAX_FIELDS) {
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
        compiler->program->modules[(size_t)compiler->current_module].name);
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
    if (class->class_variable_count == DIAMOND_MAX_FIELDS) {
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
        &compiler->program->modules[(size_t)compiler->current_module];
    const size_t length=name.length-1;
    for(size_t field=0;field<module->field_count;field++)
        if(strlen(module->fields[field])==length&&
           memcmp(module->fields[field],compiler->source+name.start+1,length)==0)
            return add_string_range(compiler,name.start+1,length,name);
    if(module->field_count==DIAMOND_MAX_FIELDS) {
        fail(compiler,name,"too many module instance variables");return 0;
    }
    if(length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,name,"instance variable name is too long");return 0;
    }
    memcpy(module->fields[module->field_count],compiler->source+name.start+1,length);
    module->fields[module->field_count][length]='\0';module->field_count++;
    return add_string_range(compiler,name.start+1,length,name);
}

bool call_arguments_have_keyword(Compiler *compiler) {
    Compiler probe=*compiler;size_t nesting=0;bool argument_start=true;
    while(!probe.failed&&probe.current.kind!=DIAMOND_TOKEN_EOF) {
        const DiamondTokenKind kind=probe.current.kind;
        if(nesting==0&&kind==DIAMOND_TOKEN_RIGHT_PAREN)return false;
        if(nesting==0&&argument_start&&kind==DIAMOND_TOKEN_IDENTIFIER) {
            DiamondLexer lookahead=probe.lexer;
            if(diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON)
                return true;
        }
        if(kind==DIAMOND_TOKEN_LEFT_PAREN||kind==DIAMOND_TOKEN_LEFT_BRACKET||
           kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_PAREN||
                kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                kind==DIAMOND_TOKEN_RIGHT_BRACE) {if(nesting>0)nesting--;}
        /* A newline after a comma (or the call's open paren) leaves the next
         * token at the start of an argument. */
        if(kind==DIAMOND_TOKEN_COMMA)argument_start=nesting==0;
        else if(kind!=DIAMOND_TOKEN_NEWLINE)argument_start=false;
        advance_token(&probe);
    }
    return false;
}

uint16_t emit_argument_array(Compiler *compiler,const uint16_t *values,
        size_t count) {
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<count;index++)(void)allocate_register(compiler);
    for(size_t index=0;index<count;index++)emit_instruction(compiler,
        DIAMOND_OP_MOVE,(uint16_t)(base+index),values[index],0,2);
    const uint16_t result=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,result,base,(uint16_t)count,3);
    compiler->known_types[result]=DIAMOND_TYPE_ARRAY;return result;
}

/* Dynamic keyword targets retain names until runtime lookup identifies the
 * concrete DiamondFunction. Positional values are normalized to one Array,
 * whether or not the source used `*`. */
uint16_t parse_dynamic_keyword_arguments(Compiler *compiler,
        DiamondSpan *keyword_names,uint16_t *keyword_values,
        size_t *keyword_count,const DiamondFunction *target) {
    uint16_t fixed[DIAMOND_MAX_DECLARED_PARAMETERS];size_t fixed_count=0,spread_index=0;
    uint16_t spread=0;bool saw_spread=false,seen_keyword=false;
    bool literal_positions=false;size_t literal_first=0,literal_count=0;
    uint16_t literal_elements[DIAMOND_MAX_ARGUMENTS];
    compiler->positional_spread_literal=false;
    compiler->positional_spread_first=0;
    compiler->positional_spread_count=0;
    compiler->positional_spread_fixed_count=0;
    compiler->positional_spread_index=0;
    *keyword_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        DiamondLexer lookahead=compiler->lexer;
        const bool keyword=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
            diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON;
        if(keyword) {
            seen_keyword=true;
            if(*keyword_count==DIAMOND_MAX_DECLARED_PARAMETERS) {
                fail(compiler,compiler->current.span,"too many keyword arguments");return 0;
            }
            const DiamondSpan name=compiler->current.span;
            for(size_t index=0;index<*keyword_count;index++)
                if(keyword_names[index].length==name.length&&
                   memcmp(compiler->source+keyword_names[index].start,
                          compiler->source+name.start,name.length)==0) {
                    fail(compiler,name,"multiple values for the same argument");return 0;
                }
            keyword_names[*keyword_count]=name;
            advance_token(compiler);advance_token(compiler);
            size_t parameter=SIZE_MAX;
            if(target!=nullptr)
                for(size_t index=0;index<target->arity&&index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
                    if(name_equals(compiler,target->parameter_names[index],
                            name,false)) {parameter=index;break;}
            keyword_values[*keyword_count]=parameter==SIZE_MAX?
                parse_expression(compiler):
                parse_expected_argument(compiler,target,parameter);
            (*keyword_count)++;
        } else if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
            if(seen_keyword) {fail(compiler,compiler->current.span,
                "positional argument cannot follow a keyword argument");return 0;}
            if(saw_spread) {fail(compiler,compiler->current.span,
                "a call can contain only one spread argument");return 0;}
            saw_spread=true;spread_index=fixed_count;
            advance_token(compiler);skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
               target!=nullptr) {
                advance_token(compiler);
                spread=parse_array_literal(compiler,target,fixed_count,
                    target->arity);
                literal_positions=compiler->positional_spread_literal;
                literal_first=compiler->positional_spread_first;
                literal_count=compiler->positional_spread_count;
                for(size_t index=0;index<literal_count;index++)
                    literal_elements[index]=
                        compiler->positional_spread_elements[index];
            } else {
                const int32_t outer_expected=
                    compiler->expected_expression_type_set;
                compiler->expected_expression_type_set=
                    homogeneous_spread_expectation(compiler,target,
                        fixed_count,target==nullptr?0:target->arity);
                spread=parse_expression(compiler);
                compiler->expected_expression_type_set=outer_expected;
            }
        } else {
            if(seen_keyword) {fail(compiler,compiler->current.span,
                "positional argument cannot follow a keyword argument");return 0;}
            if(fixed_count==DIAMOND_MAX_DECLARED_PARAMETERS) {fail(compiler,compiler->current.span,
                "too many fixed call arguments");return 0;}
            fixed[fixed_count]=parse_expected_argument(compiler,target,
                fixed_count);
            fixed_count++;
        }
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;
    }
    advance_token(compiler);
    if(!saw_spread) {
        compiler->positional_spread_fixed_count=fixed_count;
        compiler->positional_spread_index=fixed_count;
        for(size_t index=0;index<fixed_count;index++)
            compiler->positional_spread_fixed[index]=fixed[index];
        return emit_argument_array(compiler,fixed,fixed_count);
    }
    compiler->positional_spread_literal=literal_positions;
    compiler->positional_spread_first=literal_first;
    compiler->positional_spread_count=literal_count;
    for(size_t index=0;index<literal_count;index++)
        compiler->positional_spread_elements[index]=literal_elements[index];
    compiler->positional_spread_fixed_count=fixed_count;
    compiler->positional_spread_index=spread_index;
    for(size_t index=0;index<fixed_count;index++)
        compiler->positional_spread_fixed[index]=fixed[index];
    return emit_build_spread_arguments(compiler,fixed,spread_index,spread,
        fixed+spread_index,fixed_count-spread_index,false);
}

bool call_arguments_have_spread(Compiler *compiler) {
    Compiler probe=*compiler;
    size_t nesting=0;bool argument_start=true;
    while(!probe.failed&&probe.current.kind!=DIAMOND_TOKEN_EOF) {
        const DiamondTokenKind kind=probe.current.kind;
        if(nesting==0&&kind==DIAMOND_TOKEN_RIGHT_PAREN)return false;
        if(nesting==0&&argument_start&&kind==DIAMOND_TOKEN_STAR)return true;
        if(kind==DIAMOND_TOKEN_LEFT_PAREN||kind==DIAMOND_TOKEN_LEFT_BRACKET||
           kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_PAREN||
                kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                kind==DIAMOND_TOKEN_RIGHT_BRACE) {
            if(nesting>0)nesting--;
        }
        /* A newline after a comma (or the call's open paren) leaves the next
         * token at the start of an argument. */
        if(kind==DIAMOND_TOKEN_COMMA)argument_start=nesting==0;
        else if(kind!=DIAMOND_TOKEN_NEWLINE)argument_start=false;
        advance_token(&probe);
    }
    return false;
}

static int32_t homogeneous_spread_expectation(Compiler *compiler,
        const DiamondFunction *target,size_t first,size_t parameter_count) {
    if(target==nullptr||first>=parameter_count||parameter_count>target->arity)
        return -1;
    const uint16_t source=target->parameter_type_sets[first];
    if(source==DIAMOND_NO_TYPE_SET||source>=target->type_set_count)return -1;
    for(size_t parameter=first+1;parameter<parameter_count;parameter++) {
        const uint16_t other=target->parameter_type_sets[parameter];
        if(other==DIAMOND_NO_TYPE_SET||other>=target->type_set_count||
           !type_sets_structurally_equal(target->type_sets,
               target->type_set_count,source,target->type_sets,
               target->type_set_count,other,0))return -1;
    }
    const uint16_t element=target==compiler->function?source:
        clone_type_set_into_current(compiler,target->type_sets,
            target->type_set_count,source);
    if(element==DIAMOND_NO_TYPE_SET||!reserve_type_sets(compiler,1))return -1;
    const size_t array_index=compiler->function->type_set_count++;
    DiamondTypeSet *array=&compiler->function->type_sets[array_index];
    array->count=1;array->inferred=true;
    array->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_ARRAY,
        .argument_set=element,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        array->members[0].callable_parameter_sets[parameter]=
            DIAMOND_NO_TYPE_SET;
    return (int32_t)array_index;
}

/* Parses the argument-list interior with exactly one `*expression`; current
 * is the first argument and the closing ')' is consumed. */
uint16_t parse_spread_argument_array(Compiler *compiler,
        const DiamondFunction *keyword_function,uint8_t *keyword_slots,
        uint16_t *keyword_registers,size_t *keyword_count,
        const DiamondFunction *expected_function,size_t expected_count) {
    uint16_t fixed[DIAMOND_MAX_DECLARED_PARAMETERS];size_t fixed_count=0,spread_index=0;
    uint16_t spread=0;bool saw_spread=false,seen_keyword=false;
    bool optional_block=false;
    bool literal_positions=false;size_t literal_first=0,literal_count=0;
    uint16_t literal_elements[DIAMOND_MAX_ARGUMENTS];
    compiler->positional_spread_literal=false;
    compiler->positional_spread_first=0;
    compiler->positional_spread_count=0;
    compiler->positional_spread_fixed_count=0;
    compiler->positional_spread_index=0;
    if(keyword_count!=nullptr)*keyword_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        DiamondLexer keyword_lookahead=compiler->lexer;
        const bool is_keyword=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
            diamond_lexer_next(&keyword_lookahead).kind==DIAMOND_TOKEN_COLON;
        if(is_keyword) {
            const DiamondSpan keyword_name=compiler->current.span;
            if(keyword_function==nullptr) {
                fail(compiler,keyword_name,
                    "keyword arguments are only supported for direct function calls");
                return 0;
            }
            seen_keyword=true;advance_token(compiler);advance_token(compiler);
            size_t slot=SIZE_MAX;
            for(size_t index=0;index<keyword_function->arity&&index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
                if(name_equals(compiler,keyword_function->parameter_names[index],
                               keyword_name,false)) {slot=index;break;}
            if(slot==SIZE_MAX) {
                fail(compiler,keyword_name,"no parameter with this name");return 0;
            }
            for(size_t index=0;index<*keyword_count;index++)
                if(keyword_slots[index]==slot) {
                    fail(compiler,keyword_name,
                        "multiple values for the same argument");return 0;
                }
            keyword_slots[*keyword_count]=(uint8_t)slot;
            keyword_registers[*keyword_count]=parse_expression(compiler);
            (*keyword_count)++;
        } else if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
            if(seen_keyword) {
                fail(compiler,compiler->current.span,
                    "positional argument cannot follow a keyword argument");return 0;
            }
            if(saw_spread) {
                fail(compiler,compiler->current.span,
                    "a call can contain only one spread argument");return 0;
            }
            saw_spread=true;spread_index=fixed_count;
            advance_token(compiler);skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
               expected_function!=nullptr) {
                advance_token(compiler);
                spread=parse_array_literal(compiler,expected_function,
                    fixed_count,expected_count);
                literal_positions=compiler->positional_spread_literal;
                literal_first=compiler->positional_spread_first;
                literal_count=compiler->positional_spread_count;
                for(size_t index=0;index<literal_count;index++)
                    literal_elements[index]=
                        compiler->positional_spread_elements[index];
            } else {
                const int32_t outer_expected=
                    compiler->expected_expression_type_set;
                compiler->expected_expression_type_set=
                    homogeneous_spread_expectation(compiler,
                        expected_function,fixed_count,expected_count);
                spread=parse_expression(compiler);
                compiler->expected_expression_type_set=outer_expected;
            }
        } else {
            if(seen_keyword) {
                fail(compiler,compiler->current.span,
                    "positional argument cannot follow a keyword argument");return 0;
            }
            if(fixed_count==DIAMOND_MAX_DECLARED_PARAMETERS) {
                fail(compiler,compiler->current.span,"too many fixed call arguments");
                return 0;
            }
            const bool forwarded_block=
                compiler->current.kind==DIAMOND_TOKEN_AMPERSAND;
            fixed[fixed_count++]=parse_expression(compiler);
            if(forwarded_block) {
                optional_block=true;
                if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
                    fail(compiler,compiler->current.span,
                        "a forwarded block argument must be last");return 0;
                }
            }
        }
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    if(!saw_spread) {
        fail(compiler,compiler->previous.span,"expected spread argument");return 0;
    }
    compiler->positional_spread_literal=literal_positions;
    compiler->positional_spread_first=literal_first;
    compiler->positional_spread_count=literal_count;
    for(size_t index=0;index<literal_count;index++)
        compiler->positional_spread_elements[index]=literal_elements[index];
    compiler->positional_spread_fixed_count=fixed_count;
    compiler->positional_spread_index=spread_index;
    for(size_t index=0;index<fixed_count;index++)
        compiler->positional_spread_fixed[index]=fixed[index];
    if(fixed_count==0)return spread;
    return emit_build_spread_arguments(compiler,fixed,spread_index,spread,
        fixed+spread_index,fixed_count-spread_index,optional_block);
}

/* Parses `(arg, arg, ...)` (the `(` itself still current) and emits a
 * DIAMOND_OP_CALL_CLOSURE against `callable` -- shared between a local
 * variable holding a Callable followed by `(...)` (parse_call's own
 * shape, below) and an instance variable holding one, followed by
 * `(...)` (`@field(...)`, parse_prefix's DIAMOND_TOKEN_INSTANCE_VARIABLE
 * case) -- previously only the local-variable shape was supported at
 * all; `@cb()` failed to parse outright ("expected newline after
 * expression"), forcing an extra `cb = @cb; cb()` local-binding step for
 * a stored-callback-field pattern that's otherwise completely ordinary. */
uint16_t parse_closure_call_arguments(Compiler *compiler, uint16_t callable) {
    const int32_t callable_type_set=compiler->known_type_sets[callable];
    size_t contextual_block_slot=SIZE_MAX;
    if(callable_type_set>=0&&
       (size_t)callable_type_set<compiler->function->type_set_count) {
        const DiamondTypeSet *set=
            &compiler->function->type_sets[(size_t)callable_type_set];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_CALLABLE&&
           set->members[0].callable_parameters_typed&&
           set->members[0].callable_arity>0)
            contextual_block_slot=set->members[0].callable_arity-1;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(call_arguments_have_keyword(compiler)) {
        DiamondSpan keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];uint16_t keyword_values[DIAMOND_MAX_DECLARED_PARAMETERS];
        size_t keyword_count=0;
        const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
            keyword_names,keyword_values,&keyword_count,nullptr);
        bool has_block=false;uint16_t block=0;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t callable_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,callable_snapshot,
                callable,0,2);callable=callable_snapshot;
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_values[index],0,2);
                keyword_values[index]=snapshot;
            }
            block=compile_callable_value_block(compiler,callable_type_set);
            has_block=true;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
        emit_register(compiler,destination);emit_register(compiler,callable);
        emit_register(compiler,positional);
        emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
        for(size_t index=0;index<keyword_count;index++) {
            emit_register(compiler,add_name_string(compiler,keyword_names[index]));
            emit_register(compiler,keyword_values[index]);
        }
        if(has_block)emit_register(compiler,block);
        publish_callable_return_type(compiler,destination,callable_type_set);
        return destination;
    }
    if(call_arguments_have_spread(compiler)) {
        uint16_t spread=parse_spread_argument_array(compiler,nullptr,
            nullptr,nullptr,nullptr,nullptr,0);
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t callable_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,callable_snapshot,
                callable,0,2);
            callable=callable_snapshot;
            const uint16_t block=
                compile_callable_value_block(compiler,callable_type_set);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
            emit_register(compiler,destination);emit_register(compiler,callable);
            emit_register(compiler,spread);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_callable_return_type(compiler,destination,callable_type_set);
            return destination;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_SPREAD);
        emit_register(compiler,destination);emit_register(compiler,callable);
        emit_register(compiler,spread);
        publish_callable_return_type(compiler,destination,callable_type_set);
        return destination;
    }
    uint16_t arguments[DIAMOND_MAX_ARGUMENTS]; size_t argument_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if(argument_count==DIAMOND_MAX_ARGUMENTS){fail(compiler,compiler->current.span,"too many call arguments");return 0;}
        arguments[argument_count++]=parse_expression(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN){fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;}
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(argument_count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        const uint16_t callable_snapshot=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,callable_snapshot,callable,0,2);
        callable=callable_snapshot;
        for(size_t index=0;index<argument_count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,arguments[index],0,2);
            arguments[index]=snapshot;
        }
        const uint16_t block=
            compile_callable_value_block(compiler,callable_type_set);
        if(argument_count<contextual_block_slot) {
            const uint16_t positional=
                emit_argument_array(compiler,arguments,argument_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
            emit_register(compiler,destination);emit_register(compiler,callable);
            emit_register(compiler,positional);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_callable_return_type(compiler,destination,callable_type_set);
            return destination;
        }
        arguments[argument_count++]=block;
    }
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<argument_count;i++)(void)allocate_register(compiler);
    for(size_t i=0;i<argument_count;i++)emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i),arguments[i],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE);emit_register(compiler,destination);
    emit_register(compiler,callable);emit_register(compiler,base);emit_byte(compiler,(uint8_t)argument_count);
    publish_callable_return_type(compiler,destination,callable_type_set);
    return destination;
}

/* The discovery pass can encounter a call before its top-level function's
 * signature has been seen. It only needs to keep walking far enough to record
 * that later declaration; the real pass will validate the call against the
 * copied signature. Parse the complete call shape here without emitting a
 * callable function index that does not exist in this throwaway program yet. */
/* Discovery pass only: an unresolved `name` inside a nested def's body
 * that is that def's own name makes it a recursive local helper. */
static void note_self_reference(Compiler *compiler,DiamondSpan name) {
    if(!compiler->discovery_pass||compiler->current_definition==nullptr)return;
    const DiamondSpan own=compiler->current_definition_name;
    if(own.length==name.length&&
       memcmp(compiler->source+own.start,compiler->source+name.start,name.length)==0)
        compiler->current_definition->calls_itself=true;
}

static uint16_t parse_discovery_unknown_call(Compiler *compiler) {
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            (void)parse_type_annotation(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after function name");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    if(call_arguments_have_spread(compiler)) {
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&
              !compiler->failed) {
            DiamondLexer lookahead=compiler->lexer;
            if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
               diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);advance_token(compiler);
                skip_newlines(compiler);
            } else if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
                advance_token(compiler);skip_newlines(compiler);
            }
            (void)parse_expression(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        }
    } else if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            DiamondLexer lookahead=compiler->lexer;
            if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
               diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);advance_token(compiler);
                skip_newlines(compiler);
            }
            (void)parse_expression(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    /* A trailing block (an implicit-self call such as `transaction() do
     * ... end`, resolved in the real pass) still has to be parsed. No
     * call is emitted for it here, so nothing may claim its break site. */
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        (void)compile_block(compiler);
        compiler->pending_block_call=nullptr;
        compiler->block_break_pending=false;
    }
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    return destination;
}

uint16_t load_current_block(Compiler *compiler) {
    uint16_t block=compiler->current_block_register;
    for(size_t index=0;index<compiler->local_count;index++) {
        if(compiler->locals[index].reg!=block||
           !compiler->locals[index].captured)continue;
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,block,0,0,1);
        const uint16_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,block,0,2);
        return loaded;
    }
    return block;
}

uint16_t callable_type_set_index(Compiler *compiler) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_CALLABLE&&
           set->members[0].callable_arity==UINT8_MAX)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[index];
    set->count=1;
    set->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_CALLABLE,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        set->members[0].callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)index;
}

static void prepare_contextual_block(Compiler *compiler,
        const DiamondTypeSet *sets,size_t set_count,
        const DiamondTypeMember *callable) {
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    uint16_t parameter_sets[16];
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<16;index++) {
        compiler->contextual_block_type_sets[index]=-1;
        parameter_sets[index]=DIAMOND_NO_TYPE_SET;
    }
    if(callable!=nullptr&&callable->id==DIAMOND_TYPE_CALLABLE&&
       callable->callable_parameters_typed&&callable->callable_arity<=16) {
        compiler->has_contextual_block_types=true;
        compiler->contextual_block_arity=callable->callable_arity;
        return_set=callable->callable_return_set;
        for(size_t index=0;index<callable->callable_arity;index++) {
            compiler->contextual_block_types[index]=TYPE_UNKNOWN;
            const uint16_t parameter_set=callable->callable_parameter_sets[index];
            parameter_sets[index]=parameter_set;
            if(parameter_set>=set_count)continue;
            const DiamondTypeSet *parameter=&sets[parameter_set];
            if(parameter->count==1&&
               parameter->members[0].id<DIAMOND_TYPE_VARIABLE_BASE)
                compiler->contextual_block_types[index]=parameter->members[0].id;
        }
    }
    /* Clone last: `sets` and `callable` can point into the current function's
     * table, whose reserve may move while cloning. Capture every source index
     * before the first clone, then let the index-aware cloner reacquire it. */
    for(size_t index=0;index<compiler->contextual_block_arity;index++)
        if(parameter_sets[index]!=DIAMOND_NO_TYPE_SET) {
            const uint16_t cloned=clone_type_set_into_current(compiler,sets,
                set_count,parameter_sets[index]);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->contextual_block_type_sets[index]=(int32_t)cloned;
        }
    if(return_set!=DIAMOND_NO_TYPE_SET) {
        const uint16_t cloned=clone_type_set_into_current(compiler,sets,
            set_count,return_set);
        if(cloned!=DIAMOND_NO_TYPE_SET)
            compiler->contextual_block_return_set=(int32_t)cloned;
    }
}

uint16_t compile_contextual_typed_block(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_index,
        const uint16_t *type_arguments,size_t type_argument_count) {
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    if(target!=nullptr&&parameter_index<DIAMOND_MAX_DECLARED_PARAMETERS) {
        const uint16_t set_index=target->parameter_type_sets[parameter_index];
        if(set_index!=DIAMOND_NO_TYPE_SET&&set_index<target->type_set_count) {
            const DiamondTypeSet *set=&target->type_sets[set_index];
            if(set->count==1) {
                const DiamondTypeMember *callable=&set->members[0];
                const bool callable_typed=
                    callable->id==DIAMOND_TYPE_CALLABLE&&
                    callable->callable_parameters_typed;
                const uint16_t callable_return_set=
                    callable->callable_return_set;
                prepare_contextual_block(compiler,target->type_sets,
                    target->type_set_count,callable);
                if(callable_typed&&target->type_variable_count>0)
                    for(size_t index=0;index<callable->callable_arity;index++) {
                        const uint16_t parameter_set=
                            callable->callable_parameter_sets[index];
                        if(parameter_set>=target->type_set_count)continue;
                        bool resolved=true;
                        const uint16_t substituted=clone_substituted_type_set(
                            compiler,target,parameter_set,type_arguments,
                            type_argument_count,&resolved);
                        if(!resolved||substituted==DIAMOND_NO_TYPE_SET)continue;
                        compiler->contextual_block_type_sets[index]=
                            (int32_t)substituted;
                        const DiamondTypeSet *bound=&compiler->function->
                            type_sets[substituted];
                        if(bound->count==1&&bound->members[0].id<
                               DIAMOND_TYPE_VARIABLE_BASE)
                            compiler->contextual_block_types[index]=
                                bound->members[0].id;
                    }
                if(callable_typed&&callable_return_set!=DIAMOND_NO_TYPE_SET) {
                    /* The first clone preserves non-generic contracts. A
                     * generic return must instead be rebuilt against this
                     * call site's concrete bindings; never publish an
                     * unbound type variable on the anonymous closure. */
                    compiler->contextual_block_return_set=-1;
                    bool resolved=true;
                    const uint16_t substituted=clone_substituted_type_set(
                        compiler,target,callable_return_set,
                        type_arguments,type_argument_count,&resolved);
                    if(resolved&&substituted!=DIAMOND_NO_TYPE_SET)
                        compiler->contextual_block_return_set=
                            (int32_t)substituted;
                }
            }
        }
    }
    const uint16_t block=compile_block(compiler);
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    return block;
}

static void infer_contextual_type_set(Compiler *compiler,
        const DiamondFunction *target,uint16_t expected_index,
        uint16_t actual_index,uint16_t *bindings) {
    if(expected_index>=target->type_set_count||
       actual_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet *expected=&target->type_sets[expected_index];
    const DiamondTypeSet *actual=&compiler->function->type_sets[actual_index];
    for(size_t wanted_index=0;wanted_index<expected->count;wanted_index++) {
        const DiamondTypeMember wanted=expected->members[wanted_index];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=
                (size_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable<8) {
                if(bindings[variable]==DIAMOND_NO_TYPE_SET)
                    bindings[variable]=actual_index;
                else if(bindings[variable]!=actual_index&&
                        !type_sets_structurally_equal(
                            compiler->function->type_sets,
                            compiler->function->type_set_count,
                            bindings[variable],compiler->function->type_sets,
                            compiler->function->type_set_count,actual_index,0))
                    bindings[variable]=(uint16_t)(DIAMOND_NO_TYPE_SET-1u);
            }
            continue;
        }
        for(size_t known_index=0;known_index<actual->count;known_index++) {
            const DiamondTypeMember known=actual->members[known_index];
            if(known.id!=wanted.id)continue;
            if(wanted.argument_set!=DIAMOND_NO_TYPE_SET&&
               known.argument_set!=DIAMOND_NO_TYPE_SET)
                infer_contextual_type_set(compiler,target,wanted.argument_set,
                    known.argument_set,bindings);
            if(wanted.second_argument_set!=DIAMOND_NO_TYPE_SET&&
               known.second_argument_set!=DIAMOND_NO_TYPE_SET)
                infer_contextual_type_set(compiler,target,
                    wanted.second_argument_set,known.second_argument_set,
                    bindings);
            if(wanted.id==DIAMOND_TYPE_CALLABLE&&
               wanted.callable_parameters_typed&&
               known.callable_parameters_typed&&
               wanted.callable_arity==known.callable_arity) {
                for(size_t parameter=0;parameter<wanted.callable_arity;
                    parameter++) {
                    const uint16_t wanted_parameter=
                        wanted.callable_parameter_sets[parameter];
                    const uint16_t known_parameter=
                        known.callable_parameter_sets[parameter];
                    if(wanted_parameter!=DIAMOND_NO_TYPE_SET&&
                       known_parameter!=DIAMOND_NO_TYPE_SET)
                        infer_contextual_type_set(compiler,target,
                            wanted_parameter,known_parameter,bindings);
                }
                if(wanted.callable_return_set!=DIAMOND_NO_TYPE_SET&&
                   known.callable_return_set!=DIAMOND_NO_TYPE_SET)
                    infer_contextual_type_set(compiler,target,
                        wanted.callable_return_set,
                        known.callable_return_set,bindings);
            }
        }
    }
}

static void infer_contextual_argument(Compiler *compiler,
        const DiamondFunction *target,size_t parameter,uint16_t argument,
        uint16_t *bindings) {
    if(parameter>=DIAMOND_MAX_DECLARED_PARAMETERS)return;
    const uint16_t expected=target->parameter_type_sets[parameter];
    if(expected==DIAMOND_NO_TYPE_SET||expected>=target->type_set_count)return;
    int32_t actual=compiler->known_type_sets[argument];
    if(actual<0) {
        const uint16_t known=compiler->known_types[argument];
        if(known==TYPE_UNKNOWN||known>=DIAMOND_TYPE_VARIABLE_BASE)return;
        actual=(int32_t)concrete_type_set(compiler,known);
    }
    if(actual>=0&&(size_t)actual<compiler->function->type_set_count)
        infer_contextual_type_set(compiler,target,expected,(uint16_t)actual,
            bindings);
}

/* Parses one argument while exposing its declared parameter contract to the
 * expression itself. This is deliberately a one-expression conduit: generic
 * method references can consume it bidirectionally, while every nested or
 * subsequent expression sees the prior context restored. */
uint16_t parse_expected_argument(Compiler *compiler,
        const DiamondFunction *target,size_t parameter) {
    const int32_t outer=compiler->expected_expression_type_set;
    compiler->expected_expression_type_set=-1;
    if(target!=nullptr&&parameter<target->arity) {
        const uint16_t source=target->parameter_type_sets[parameter];
        if(source!=DIAMOND_NO_TYPE_SET&&source<target->type_set_count) {
            const uint16_t cloned=target==compiler->function?source:
                clone_type_set_into_current(compiler,target->type_sets,
                    target->type_set_count,source);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->expected_expression_type_set=(int32_t)cloned;
        }
    }
    const uint16_t result=parse_expression(compiler);
    compiler->expected_expression_type_set=outer;
    return result;
}

size_t infer_contextual_type_arguments(Compiler *compiler,
        const DiamondFunction *target,const uint16_t *arguments,
        size_t argument_count,uint16_t *bindings) {
    for(size_t index=0;index<8;index++)bindings[index]=DIAMOND_NO_TYPE_SET;
    if(target==nullptr)return 0;
    for(size_t parameter=0;parameter<argument_count&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++)
        infer_contextual_argument(compiler,target,parameter,
            arguments[parameter],bindings);
    return target->type_variable_count;
}

size_t infer_contextual_spread_arguments(Compiler *compiler,
        const DiamondFunction *target,uint16_t spread,size_t parameter_count,
        uint16_t *bindings) {
    for(size_t index=0;index<8;index++)bindings[index]=DIAMOND_NO_TYPE_SET;
    if(target==nullptr)return 0;
    const size_t prefix_count=compiler->positional_spread_index;
    for(size_t index=0;index<prefix_count&&index<parameter_count;index++)
        infer_contextual_argument(compiler,target,index,
            compiler->positional_spread_fixed[index],bindings);
    const size_t fixed_count=compiler->positional_spread_fixed_count;
    const size_t suffix_count=fixed_count>prefix_count?
        fixed_count-prefix_count:0;
    const size_t suffix_start=compiler->positional_spread_literal?
        prefix_count+compiler->positional_spread_count:
        parameter_count>=suffix_count?parameter_count-suffix_count:
            parameter_count;
    if(suffix_start+suffix_count<=parameter_count)
        for(size_t index=0;index<suffix_count;index++)
            infer_contextual_argument(compiler,target,
                suffix_start+index,
                compiler->positional_spread_fixed[prefix_count+index],
                bindings);
    if(compiler->positional_spread_literal) {
        for(size_t index=0;index<compiler->positional_spread_count;index++) {
            const size_t parameter=compiler->positional_spread_first+index;
            if(parameter>=parameter_count||parameter>=DIAMOND_MAX_DECLARED_PARAMETERS)break;
            infer_contextual_argument(compiler,target,parameter,
                compiler->positional_spread_elements[index],bindings);
        }
        return target->type_variable_count;
    }
    const int32_t element_set=array_element_type_set(compiler,spread);
    if(element_set<0)return target->type_variable_count;
    const size_t spread_end=parameter_count>=suffix_count?
        parameter_count-suffix_count:prefix_count;
    for(size_t parameter=prefix_count;
        parameter<spread_end&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++) {
        const uint16_t expected=target->parameter_type_sets[parameter];
        if(expected!=DIAMOND_NO_TYPE_SET&&expected<target->type_set_count)
            infer_contextual_type_set(compiler,target,expected,
                (uint16_t)element_set,bindings);
    }
    return target->type_variable_count;
}

void infer_contextual_keyword_arguments(Compiler *compiler,
        const DiamondFunction *target,const DiamondSpan *names,
        const uint16_t *values,size_t count,uint16_t *bindings) {
    if(target==nullptr)return;
    for(size_t keyword=0;keyword<count;keyword++)
        for(size_t parameter=0;parameter<target->arity&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;
            parameter++)
            if(name_equals(compiler,target->parameter_names[parameter],
                    names[keyword],false)) {
                infer_contextual_argument(compiler,target,parameter,
                    values[keyword],bindings);
                break;
            }
}

static uint16_t compile_callable_value_block(Compiler *compiler,
        int32_t callable_set_index) {
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    if(callable_set_index>=0&&
       (size_t)callable_set_index<compiler->function->type_set_count) {
        const DiamondTypeSet *outer=
            &compiler->function->type_sets[(size_t)callable_set_index];
        uint16_t block_sets[DIAMOND_MAX_UNION_TYPES];
        size_t block_set_count=0;
        bool compatible=outer->count>0;
        for(size_t index=0;index<outer->count&&compatible;index++) {
            const DiamondTypeMember *callable=&outer->members[index];
            if(callable->id!=DIAMOND_TYPE_CALLABLE||
               !callable->callable_parameters_typed||
               callable->callable_arity==0) {compatible=false;break;}
            const uint16_t block_set=callable->callable_parameter_sets[
                callable->callable_arity-1];
            if(block_set>=compiler->function->type_set_count) {
                compatible=false;break;
            }
            const DiamondTypeSet *set=&compiler->function->type_sets[block_set];
            if(set->count!=1||set->members[0].id!=DIAMOND_TYPE_CALLABLE) {
                compatible=false;break;
            }
            block_sets[block_set_count++]=block_set;
        }
        uint16_t shared_block_set=DIAMOND_NO_TYPE_SET;
        for(size_t candidate=0;candidate<block_set_count&&
            shared_block_set==DIAMOND_NO_TYPE_SET;candidate++) {
            const DiamondTypeMember actual=compiler->function->type_sets[
                block_sets[candidate]].members[0];
            bool satisfies_all=true;
            for(size_t expected=0;expected<block_set_count;expected++)
                if(!type_members_satisfy_across(compiler,
                        compiler->function->type_sets,actual,
                        compiler->function->type_sets,
                        compiler->function->type_sets[
                            block_sets[expected]].members[0])) {
                    satisfies_all=false;break;
                }
            if(satisfies_all)shared_block_set=block_sets[candidate];
        }
        DiamondTypeMember synthesized={.id=DIAMOND_TYPE_CALLABLE,
            .argument_set=DIAMOND_NO_TYPE_SET,
            .second_argument_set=DIAMOND_NO_TYPE_SET,
            .callable_arity=UINT8_MAX,
            .callable_return_set=DIAMOND_NO_TYPE_SET,
            .callable_parameters_typed=false};
        bool synthesized_context=false;
        if(compatible&&shared_block_set==DIAMOND_NO_TYPE_SET&&
           block_set_count>0) {
            const DiamondTypeMember first=compiler->function->type_sets[
                block_sets[0]].members[0];
            synthesized.callable_arity=first.callable_arity;
            synthesized.callable_parameters_typed=
                first.callable_parameters_typed;
            synthesized_context=first.callable_parameters_typed;
            for(size_t arm=1;arm<block_set_count&&synthesized_context;arm++) {
                const DiamondTypeMember member=compiler->function->type_sets[
                    block_sets[arm]].members[0];
                if(!member.callable_parameters_typed||
                   member.callable_arity!=synthesized.callable_arity)
                    synthesized_context=false;
            }
            for(size_t parameter=0;parameter<synthesized.callable_arity&&
                synthesized_context;parameter++) {
                uint16_t selected=DIAMOND_NO_TYPE_SET;
                for(size_t candidate=0;candidate<block_set_count&&
                    selected==DIAMOND_NO_TYPE_SET;candidate++) {
                    const uint16_t candidate_set=
                        compiler->function->type_sets[block_sets[candidate]].
                            members[0].callable_parameter_sets[parameter];
                    if(candidate_set==DIAMOND_NO_TYPE_SET)continue;
                    bool accepts_all=true;
                    for(size_t arm=0;arm<block_set_count;arm++) {
                        const uint16_t arm_set=compiler->function->type_sets[
                            block_sets[arm]].members[0].
                                callable_parameter_sets[parameter];
                        if(arm_set==DIAMOND_NO_TYPE_SET||
                           !type_sets_satisfy_across(compiler,
                               compiler->function->type_sets,arm_set,
                               compiler->function->type_sets,candidate_set)) {
                            accepts_all=false;break;
                        }
                    }
                    if(accepts_all)selected=candidate_set;
                }
                if(selected==DIAMOND_NO_TYPE_SET) {
                    int32_t joined=-1;
                    for(size_t arm=0;arm<block_set_count;arm++) {
                        const uint16_t arm_set=compiler->function->type_sets[
                            block_sets[arm]].members[0].
                                callable_parameter_sets[parameter];
                        if(arm_set==DIAMOND_NO_TYPE_SET) {joined=-1;break;}
                        joined=joined<0?(int32_t)arm_set:
                            join_type_set_indices(compiler,joined,arm_set);
                        if(joined<0)break;
                    }
                    if(joined>=0)selected=(uint16_t)joined;
                }
                if(selected==DIAMOND_NO_TYPE_SET)synthesized_context=false;
                else synthesized.callable_parameter_sets[parameter]=selected;
            }
            /* Callable returns are covariant. Select an existing declared
             * return graph only when it can satisfy every arm; unlike input
             * synthesis above, this therefore chooses a shared subtype. */
            for(size_t candidate=0;candidate<block_set_count&&
                synthesized.callable_return_set==DIAMOND_NO_TYPE_SET;
                candidate++) {
                const uint16_t candidate_set=compiler->function->type_sets[
                    block_sets[candidate]].members[0].callable_return_set;
                if(candidate_set==DIAMOND_NO_TYPE_SET)continue;
                bool satisfies_all=true;
                for(size_t arm=0;arm<block_set_count;arm++) {
                    const uint16_t arm_set=compiler->function->type_sets[
                        block_sets[arm]].members[0].callable_return_set;
                    if(arm_set==DIAMOND_NO_TYPE_SET||
                       !type_sets_satisfy_across(compiler,
                           compiler->function->type_sets,candidate_set,
                           compiler->function->type_sets,arm_set)) {
                        satisfies_all=false;break;
                    }
                }
                if(satisfies_all)
                    synthesized.callable_return_set=candidate_set;
            }
        }
        if(compatible&&shared_block_set!=DIAMOND_NO_TYPE_SET) {
            const DiamondTypeSet *set=
                &compiler->function->type_sets[shared_block_set];
            prepare_contextual_block(compiler,compiler->function->type_sets,
                compiler->function->type_set_count,&set->members[0]);
        } else if(synthesized_context)
            prepare_contextual_block(compiler,compiler->function->type_sets,
                compiler->function->type_set_count,&synthesized);
    }
    const uint16_t block=compile_block(compiler);
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    return block;
}

/* Does the class being compiled have an instance method `name` -- one
 * already registered (its own so far, a superclass's, an included
 * module's), or one the discovery pass found further down the class? */
static bool self_has_method(const Compiler *compiler,DiamondSpan name) {
    const uint16_t type=(uint16_t)(DIAMOND_TYPE_CLASS_BASE+compiler->current_class);
    if(class_instance_signature(compiler,type,name)!=nullptr)return true;
    const DiamondClass *class=compiler->program->classes[(size_t)compiler->current_class];
    for(size_t index=0;index<class->discovered_method_count;index++) {
        const uint16_t function=class->discovered_methods[index];
        if(function<compiler->program->function_count&&
           singleton_call_name_equals(compiler,
               compiler->program->functions[function]->name,name))
            return true;
    }
    return false;
}

/* Does the class being compiled, or a superclass, define a singleton method
 * `name`? Class singleton methods are registered before any body compiles,
 * so one defined further down the class counts too. */
static bool self_has_singleton_method(const Compiler *compiler,DiamondSpan name) {
    const DiamondClass *owner=compiler->program->classes[(size_t)compiler->current_class];
    for(size_t depth=0;owner!=nullptr&&depth<=DIAMOND_MAX_CLASSES;depth++) {
        for(size_t index=0;index<owner->singleton_method_count;index++)
            if(singleton_call_name_equals(compiler,owner->singleton_methods[index].name,name))
                return true;
        owner=owner->superclass==DIAMOND_NO_CLASS?nullptr:
            compiler->program->classes[owner->superclass];
    }
    return false;
}

uint16_t parse_call(Compiler *compiler, DiamondSpan name) {
    const int callable_local=find_local(compiler,name);
    if(callable_local>=0&&compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN) {
        uint16_t callable=compiler->locals[(size_t)callable_local].reg;
        if(compiler->locals[(size_t)callable_local].captured) {
            /* See parse_identifier's own BOX_LOCAL re-emission for why this
             * defensive re-box is needed: `captured` doesn't imply this
             * control-flow path actually ran the boxing site. */
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,callable,0,0,1);
            const uint16_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,callable,0,2);
            /* Keep the local's type facts, as parse_identifier's own
             * captured read does, so a trailing block still gets its
             * Callable parameter types. */
            compiler->known_types[loaded]=compiler->known_types[callable];
            compiler->known_type_sets[loaded]=compiler->known_type_sets[callable];
            compiler->tooling_type_sets[loaded]=compiler->tooling_type_sets[callable];
            callable=loaded;
        }
        return parse_closure_call_arguments(compiler, callable);
    }
    if(name_equals(compiler,"block_given?",name,false)) {
        advance_token(compiler);skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,
                "block_given? does not accept arguments");return 0;
        }
        advance_token(compiler);
        const uint16_t destination=allocate_register(compiler);
        if(!compiler->has_current_block) {
            emit_instruction(compiler,DIAMOND_OP_BOOL,destination,false,0,2);
        } else {
            const uint16_t block=load_current_block(compiler);
            const uint16_t absent=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NIL,absent,0,0,1);
            const uint16_t missing=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_EQUAL,missing,
                block,absent,3);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,missing,0,2);
        }
        compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
        return destination;
    }
    const int function_index = find_function(compiler, name);
    /* `name(...)` inside an instance method (or a block or closure that
     * has its self in register 0 -- a plain nested def doesn't), with no
     * function of that name, calls self's method `name` if its class (or
     * a superclass) has one -- `self.` is optional there, as in Ruby. A function of
     * the same name still wins, so this never changes what an existing
     * call means. */
    if(function_index<0&&!compiler->discovery_pass&&compiler->in_method&&
       !compiler->in_singleton_method&&compiler->current_class>=0&&
       compiler->function->owner_class!=DIAMOND_OWNER_NONE&&
       self_has_method(compiler,name)) {
        return parse_invoke_named(compiler,0,0,compiler->known_type_sets[0],name);
    }
    /* The same for a class singleton method: inside `def self.x`, a bare
     * `name(...)` is `self.name(...)`, dispatched on self's actual class so
     * a subclass inherits it. */
    if(function_index<0&&!compiler->discovery_pass&&compiler->in_method&&
       compiler->in_singleton_method&&compiler->current_class>=0&&
       self_has_singleton_method(compiler,name))
        return parse_self_class_method_arguments(compiler,name,false);
    /* Last resort for a bare `p(...)`: only when no function, local, or method of
     * self by that name exists (all handled above), so it never changes the meaning
     * of a program that defines its own `p`. */
    if(function_index<0&&!compiler->discovery_pass&&name_equals(compiler,"p",name,false))
        return parse_p_call(compiler);
    if (function_index < 0) {
        note_self_reference(compiler,name);
        if(compiler->discovery_pass)
            return parse_discovery_unknown_call(compiler);
        /* A module function of that name is reached through its module. */
        for(size_t module=0;module<compiler->program->module_count;module++) {
            const DiamondModule *owner=&compiler->program->modules[module];
            for(size_t index=0;index<owner->singleton_method_count;index++) {
                if(!name_equals(compiler,owner->singleton_methods[index].name,name,false))
                    continue;
                char message[sizeof compiler->diagnostic->message];
                snprintf(message,sizeof message,
                    "undefined function; did you mean %s.%.*s(...)?",owner->name,
                    (int)name.length,compiler->source+name.start);
                fail(compiler,name,message);
                return 0;
            }
        }
        fail(compiler, name, "undefined function");
        return 0;
    }
    const DiamondFunction *function =
        compiler->program->functions[(size_t)function_index];
    uint16_t type_arguments[8];
    size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            if(type_argument_count==8) {
                fail(compiler,compiler->current.span,"too many generic arguments");
                return 0;
            }
            type_arguments[type_argument_count++]=
                (uint16_t)parse_type_annotation(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
        if(type_argument_count!=function->type_variable_count) {
            fail(compiler,name,"wrong number of generic arguments");return 0;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler,compiler->current.span,
                 "expected '(' after generic arguments");return 0;
        }
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(call_arguments_have_spread(compiler)) {
        uint8_t keyword_slots[DIAMOND_MAX_DECLARED_PARAMETERS];uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
        size_t keyword_count=0;
        uint16_t array_register=parse_spread_argument_array(compiler,
            function,keyword_slots,keyword_registers,&keyword_count,
            function,function->arity);
        uint16_t inferred_arguments[8];
        const size_t spread_parameter_count=
            compiler->current.kind==DIAMOND_TOKEN_DO?
                (function->arity==0?0:function->arity-1):function->arity;
        const size_t inferred_count=type_argument_count==0?
            infer_contextual_spread_arguments(compiler,function,
                array_register,spread_parameter_count,
                inferred_arguments):0;
        if(type_argument_count==0)
            for(size_t index=0;index<keyword_count;index++)
                infer_contextual_argument(compiler,function,
                    keyword_slots[index],keyword_registers[index],
                    inferred_arguments);
        const uint16_t *resolved_arguments=type_argument_count>0?
            type_arguments:inferred_arguments;
        const size_t resolved_count=type_argument_count>0?
            type_argument_count:inferred_count;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t spread_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
                array_register,0,2);
            array_register=spread_snapshot;
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_registers[index],0,2);
                keyword_registers[index]=snapshot;
            }
            const uint16_t block=compile_contextual_typed_block(compiler,
                function,function->arity==0?0:function->arity-1,
                resolved_arguments,resolved_count);
            if(keyword_count>0) {
                if(keyword_count==DIAMOND_MAX_DECLARED_PARAMETERS) {
                    fail(compiler,compiler->previous.span,
                        "too many keyword arguments");return 0;
                }
                keyword_slots[keyword_count]=(uint8_t)(function->arity-1);
                keyword_registers[keyword_count]=block;
                keyword_count++;
            } else array_register=emit_build_spread_arguments(compiler,
                nullptr,0,array_register,&block,1,false);
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,keyword_count==0?
            (type_argument_count==0?DIAMOND_OP_CALL_SPREAD:
             DIAMOND_OP_CALL_TYPED_SPREAD):
            (type_argument_count==0?DIAMOND_OP_CALL_KEYWORD_SPREAD:
             DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD));
        emit_register(compiler,destination);
        emit_function_index(compiler,(size_t)function_index);
        emit_register(compiler,array_register);
        if(keyword_count>0) {
            emit_byte(compiler,(uint8_t)keyword_count);
            for(size_t index=0;index<keyword_count;index++) {
                emit_byte(compiler,keyword_slots[index]);
                emit_register(compiler,keyword_registers[index]);
            }
        }
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
        publish_call_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    /* Keyword arguments (direct top-level calls only):
     * each argument is placed into its declared positional slot rather than
     * appended, so a keyword can fill any parameter regardless of the order
     * it's written at the call site. Positional arguments still fill slots
     * left-to-right in declaration order. */
    uint16_t slot_registers[DIAMOND_MAX_ARGUMENTS];
    bool slot_filled[DIAMOND_MAX_ARGUMENTS]={};
    size_t next_positional_slot=0;
    bool seen_keyword=false;
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            DiamondLexer keyword_lookahead=compiler->lexer;
            const bool is_keyword=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
                diamond_lexer_next(&keyword_lookahead).kind==DIAMOND_TOKEN_COLON;
            size_t slot;
            if(is_keyword) {
                const DiamondSpan keyword_name=compiler->current.span;
                advance_token(compiler); /* consume the name */
                advance_token(compiler); /* consume ':' */
                slot=SIZE_MAX;
                for(size_t index=0;index<function->arity&&index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
                    if(name_equals(compiler,function->parameter_names[index],
                                  keyword_name,false)) {slot=index;break;}
                if(slot==SIZE_MAX) {
                    fail(compiler,keyword_name,"no parameter with this name");
                    return 0;
                }
                seen_keyword=true;
            } else {
                if(seen_keyword) {
                    fail(compiler,compiler->current.span,
                         "positional argument cannot follow a keyword argument");
                    return 0;
                }
                if(next_positional_slot==DIAMOND_MAX_ARGUMENTS) {
                    fail(compiler,compiler->current.span,"too many call arguments");
                    return 0;
                }
                slot=next_positional_slot++;
            }
            if(slot_filled[slot]) {
                fail(compiler,name,"multiple values for the same argument");
                return 0;
            }
            slot_registers[slot]=parse_expected_argument(compiler,function,slot);
            slot_filled[slot]=true;
            skip_newlines(compiler);
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        } while (!compiler->failed);
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    size_t argument_count=0;
    for(size_t index=0;index<DIAMOND_MAX_ARGUMENTS;index++)
        if(slot_filled[index])argument_count=index+1;
    uint16_t inferred_arguments[8];
    for(size_t index=0;index<8;index++)
        inferred_arguments[index]=DIAMOND_NO_TYPE_SET;
    if(type_argument_count==0&&function->type_variable_count>0)
        for(size_t parameter=0;parameter<argument_count&&
            parameter<function->arity;parameter++) {
            if(!slot_filled[parameter])continue;
            infer_contextual_argument(compiler,function,parameter,
                slot_registers[parameter],inferred_arguments);
        }
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:function->type_variable_count;
    /* `function_name(args) do |x| ... end` -- same "block is the last
     * positional argument" desugaring parse_invoke's own DIAMOND_TOKEN_DO
     * check does; if a keyword argument already filled this slot too
     * (nonsensical: a callback passed both by name and by trailing
     * block), the ordinary arity check a few lines down catches it, no
     * special-casing needed here. */
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(argument_count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many arguments");return 0;
        }
        /* Same register-aliasing hazard parse_invoke's own DIAMOND_TOKEN_DO
         * branch guards against (see its comment): every slot filled above
         * may still be a bare alias of a local the block below is about to
         * eagerly capture and BOX_LOCAL. Snapshot each filled slot into a
         * fresh (never-a-local) temp first. */
        for(size_t index=0;index<argument_count;index++) {
            if(!slot_filled[index])continue;
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,slot_registers[index],0,2);
            slot_registers[index]=snapshot;
        }
        const uint16_t block=compile_contextual_typed_block(compiler,
            function,function->arity==0?0:function->arity-1,
            resolved_arguments,resolved_count);
        const size_t block_slot=function->arity==0?0:function->arity-1;
        bool contiguous=true;
        for(size_t index=0;index<argument_count;index++)
            if(!slot_filled[index])contiguous=false;
        if(type_argument_count==0&&contiguous&&argument_count<block_slot) {
            const uint16_t callable=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,callable);
            emit_function_index(compiler,(size_t)function_index);emit_byte(compiler,0);
            const uint16_t positional=emit_argument_array(compiler,slot_registers,
                argument_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
            emit_register(compiler,destination);emit_register(compiler,callable);
            emit_register(compiler,positional);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_call_return_type(compiler,destination,function,
                resolved_arguments,resolved_count);
            return destination;
        }
        slot_registers[argument_count]=block;
        slot_filled[argument_count]=true;
        argument_count++;
    }
    /* Every slot below the highest filled one must be filled too -- a
     * keyword argument can fill any slot, but a gap below the highest one
     * (an earlier default relied on while a later slot is explicitly
     * supplied) isn't supported: Diamond's default values are compiled
     * inline into the callee's own bytecode, conditioned on a contiguous
     * argument_count, not stored as independently re-evaluable
     * expressions a call site could reach around a gap. */
    for(size_t index=0;index<argument_count;index++)
        if(!slot_filled[index]) {
            fail(compiler,name,"missing argument");
            return 0;
        }
    if (argument_count < function->required_arity||
        (argument_count > function->arity && !function->has_variadic)) {
        fail(compiler, name, "wrong number of arguments");
        return 0;
    }

    if(type_argument_count==0)
        check_call_argument_types(compiler,function,slot_registers,argument_count,0,name);
    const uint16_t argument_base = allocate_register(compiler);
    for (size_t index = 1; index < argument_count; index++) {
        (void)allocate_register(compiler);
    }
    for (size_t index = 0; index < argument_count; index++) {
        emit_instruction(compiler, DIAMOND_OP_MOVE,
                         (uint16_t)(argument_base + index), slot_registers[index], 0, 2);
    }
    const uint16_t destination = allocate_register(compiler);
    const size_t call_opcode_offset=compiler->function->code_count;
    emit_opcode(compiler,type_argument_count==0?
        DIAMOND_OP_CALL:DIAMOND_OP_CALL_TYPED);
    emit_register(compiler,destination);
    emit_function_index(compiler, (size_t)function_index);
    emit_register(compiler,argument_base);
    emit_byte(compiler, (uint8_t)argument_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    } else {
        /* Self-tail-call eligibility tracking -- see compiler->last_
         * plain_call_* fields' own comment and maybe_rewrite_self_tail_
         * call (both above compile_return). Recorded here, the one place
         * a plain (non-generic) CALL is ever emitted for an ordinary
         * function-name call, rather than reconstructed later by
         * decoding bytes backward from code_count. */
        compiler->last_plain_call_function=compiler->function;
        compiler->last_plain_call_code_count=compiler->function->code_count;
        compiler->last_plain_call_offset=call_opcode_offset;
        compiler->last_plain_call_label_after=false;
        compiler->last_plain_call_destination=destination;
        compiler->last_plain_call_function_index=(uint16_t)function_index;
    }
    publish_call_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
    return destination;
}

/* receiver_class_index: the literal class named at this call site
 * (`Author.find(...)` -> Author's own class index), or -1 for a module
 * singleton call (modules have no receiver value at all). Only used when
 * method->needs_receiver -- populates the callee's implicit self slot
 * (register 0) with a real DIAMOND_VALUE_CLASS literal for a class-owned
 * singleton method, so `self` inside an inherited method's body reflects
 * the actual receiver rather than owner_class. A module call's slot stays
 * zero-inited (DIAMOND_VALUE_NIL) exactly as before -- modules have no
 * `self` story and nothing reads that slot as one. */
/* Shared tail of an ordinary `Namespace.method(args)` call and a bare
 * `Namespace.method` *reference* (parse_singleton_reference, below) --
 * arity-checks `argument_count` against `method`'s own (with the same
 * has_variadic upper-bound relaxation every other call site has),
 * allocates the contiguous call-argument register range, loads the
 * literal receiver class for a class singleton call, MOVEs each
 * already-resolved argument register into place, and emits the CALL/
 * CALL_TYPED instruction against method->function_index -- the one
 * place `emit_function_index(method->function_index)` happens, so a
 * synthesized reference wrapper's body is byte-for-byte the same shape
 * an ordinary hand-written call site would compile to. */
static uint16_t emit_singleton_call(Compiler *compiler,const DiamondMethod *method,
        int receiver_class_index,const uint16_t *arguments,size_t argument_count,
        DiamondSpan name,uint8_t type_argument_count,const uint16_t *type_arguments) {
    if(argument_count<method->required_arity||
       (argument_count>method->arity && !method->has_variadic)) {
        fail(compiler,name,"wrong number of arguments");return 0;
    }
    const size_t call_count=argument_count+(method->needs_receiver?1:0);
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<call_count;index++)(void)allocate_register(compiler);
    /* No NIL for `base` when needs_receiver and there's no literal class
     * (a module call): sole writer, already zero-inited by run_chunk's
     * [0, register_count) init. A class singleton call instead loads the
     * literal receiver class right here, the one place this slot gets a
     * real value instead of relying on zero-init. */
    if(method->needs_receiver&&receiver_class_index>=0) {
        emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
        emit_register(compiler,base);
        emit_register(compiler,(uint16_t)receiver_class_index);
    }
    for(size_t index=0;index<argument_count;index++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,
                         (uint16_t)(base+index+(method->needs_receiver?1:0)),
                         arguments[index],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_CALL:
                DIAMOND_OP_CALL_TYPED);
    emit_register(compiler,destination);
    /* A module's own pre-scan (prescan_singleton_signatures) can
     * hand back a method whose real function_index isn't known yet --
     * an earlier sibling forward-referencing a later one. Reserve the
     * same two placeholder bytes emit_jump's own forward-jump case does
     * and record a fixup instead of writing a real (nonexistent) index;
     * resolve_pending_singleton_fixups patches it once the later
     * sibling's own def actually compiles. See DIAMOND_UNRESOLVED_
     * SINGLETON_FUNCTION's own comment. */
    const bool unresolved=
        method->function_index==DIAMOND_UNRESOLVED_SINGLETON_FUNCTION;
    if(unresolved)
        emit_pending_singleton_function_index(compiler,name,method);
    else
        emit_function_index(compiler,method->function_index);
    emit_register(compiler,base);emit_byte(compiler,(uint8_t)call_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    if(!unresolved) {
        const DiamondFunction *target=
            compiler->program->functions[method->function_index];
        publish_call_return_type(compiler,destination,target,
            type_arguments,type_argument_count);
    }
    return destination;
}

bool infer_reference_type_arguments(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_offset,size_t arity,
        uint16_t *bindings) {
    if(target==nullptr||target->type_variable_count==0||
       compiler->expected_expression_type_set<0||
       (size_t)compiler->expected_expression_type_set>=
           compiler->function->type_set_count)return false;
    const DiamondTypeSet *expected=&compiler->function->type_sets[
        (size_t)compiler->expected_expression_type_set];
    if(expected->count!=1||expected->members[0].id!=DIAMOND_TYPE_CALLABLE||
       !expected->members[0].callable_parameters_typed||
       expected->members[0].callable_arity!=arity)return false;
    const DiamondTypeMember callable=expected->members[0];
    for(size_t index=0;index<8;index++)bindings[index]=DIAMOND_NO_TYPE_SET;
    for(size_t parameter=0;parameter<arity;parameter++) {
        const size_t target_parameter=parameter+parameter_offset;
        if(target_parameter>=target->arity) return false;
        const uint16_t wanted=target->parameter_type_sets[target_parameter];
        const uint16_t actual=callable.callable_parameter_sets[parameter];
        if(wanted!=DIAMOND_NO_TYPE_SET&&actual!=DIAMOND_NO_TYPE_SET)
            infer_contextual_type_set(compiler,target,wanted,actual,bindings);
    }
    if(target->return_type_set!=DIAMOND_NO_TYPE_SET&&
       callable.callable_return_set!=DIAMOND_NO_TYPE_SET)
        infer_contextual_type_set(compiler,target,target->return_type_set,
            callable.callable_return_set,bindings);
    for(size_t index=0;index<target->type_variable_count;index++)
        if(bindings[index]==DIAMOND_NO_TYPE_SET)return false;
    return true;
}

/* `Namespace.method`, no call following -- a bare reference to a class/
 * module singleton method as a value, not a call. Synthesizes a small
 * hidden top-level function (owner_class=DIAMOND_OWNER_NONE, nested=true -- not
 * findable by bare name, only reachable via the DIAMOND_OP_CLOSURE this
 * emits at the reference site) whose entire body forwards its own
 * parameters into method via emit_singleton_call, the exact same call
 * shape parse_singleton_call itself would compile -- so the resulting
 * value is exactly as correct as any hand-written `Namespace.method
 * (args)` call site already is (see docs/design.md's "Bare singleton
 * method references" section for why: this call is already resolved
 * fully at compile time, never re-dispatched at runtime, so a wrapper
 * reusing this same compiled shape has nothing dynamic left to get
 * wrong). Mirrors compile_block's own save/restore-outer-compiler-state
 * shape (immediately above `compile_definition` further down), but
 * skips everything block-specific: no real parameter list to parse (the
 * wrapper's signature is copied from `method`'s own), no eager capture
 * materialization (this closure captures nothing at all -- the target
 * class/module is a compile-time constant baked directly into the
 * synthesized body via emit_function_index, same as any ordinary call),
 * no user-written body to compile_sequence.
 *
 * Variadic wrappers collect their trailing arguments and forward through
 * CALL_SINGLETON_SPREAD; explicit generic bindings are baked into the wrapper's
 * CALL_TYPED/CALL_TYPED_SINGLETON_SPREAD instruction. */
static uint16_t parse_singleton_reference(Compiler *compiler,
                                         const DiamondMethod *method,
                                         DiamondSpan namespace_name,
                                         int receiver_class_index,
                                         const uint16_t *type_arguments,
                                         size_t type_argument_count) {
    const DiamondFunction *target=
        method->function_index==DIAMOND_UNRESOLVED_SINGLETON_FUNCTION?nullptr:
        compiler->program->functions[method->function_index];
    uint16_t inferred_arguments[8];
    if(target!=nullptr&&target->type_variable_count>0&&type_argument_count==0&&
       infer_reference_type_arguments(compiler,target,0,method->arity,
           inferred_arguments)) {
        type_arguments=inferred_arguments;
        type_argument_count=target->type_variable_count;
    }
    if(target!=nullptr&&target->type_variable_count>0&&type_argument_count==0) {
        fail(compiler,namespace_name,
             "generic singleton method reference requires explicit bindings");
        return 0;
    }
    uint16_t parameter_sets[DIAMOND_MAX_DECLARED_PARAMETERS];
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
        parameter_sets[index]=DIAMOND_NO_TYPE_SET;
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    bool resolved=true;
    const size_t original_type_set_count=compiler->function->type_set_count;
    for(size_t parameter=0;target!=nullptr&&parameter<method->arity&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++) {
        const uint16_t source=target->parameter_type_sets[parameter];
        if(source==DIAMOND_NO_TYPE_SET)continue;
        parameter_sets[parameter]=target->type_variable_count>0?
            clone_substituted_type_set(compiler,target,source,type_arguments,
                type_argument_count,&resolved):clone_type_set_into_current(
                compiler,target->type_sets,target->type_set_count,source);
    }
    if(target!=nullptr&&target->return_type_set!=DIAMOND_NO_TYPE_SET)
        return_set=target->type_variable_count>0?
            clone_substituted_type_set(compiler,target,target->return_type_set,
                type_arguments,type_argument_count,&resolved):
            clone_type_set_into_current(compiler,target->type_sets,
                target->type_set_count,target->return_type_set);
    if(!resolved) {
        compiler->function->type_set_count=original_type_set_count;
        fail(compiler,namespace_name,
            "could not resolve generic method reference contract");
        return 0;
    }
    if(compiler->program->function_count==DIAMOND_MAX_FUNCTIONS) {
        fail(compiler,namespace_name,"too many functions");
        return 0;
    }
    size_t function_index=0;
    DiamondFunction *function=compiler_add_function(compiler,&function_index);
    if(function==nullptr) {
        fail(compiler,namespace_name,"out of memory");
        return 0;
    }
    function->owner_class=DIAMOND_OWNER_NONE;
    function->nested=true;
    function->return_type_set=return_set;
    function->arity=method->arity;
    function->required_arity=method->required_arity;
    function->has_variadic=method->has_variadic;
    function->type_set_count=compiler->function->type_set_count;
    if(!diamond_function_reserve_type_sets(function,function->type_set_count)) {
        fail(compiler,namespace_name,"out of memory");return 0;
    }
    if(function->type_set_count>0)
        memcpy(function->type_sets,compiler->function->type_sets,
            function->type_set_count*sizeof function->type_sets[0]);
    static const char reference_name[]="<method reference>";
    for(size_t index=0;index<sizeof(reference_name);index++)
        function->name[index]=reference_name[index];
    function->declaration_line=(uint32_t)compiler->previous.span.line;
    function->declaration_column=(uint32_t)compiler->previous.span.column;
    function->declaration_start=compiler->previous.span.start;
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++) {
        function->parameter_type_sets[index]=parameter_sets[index];
        (void)snprintf(function->parameter_names[index],
            DIAMOND_MAX_FUNCTION_NAME,"%s",target!=nullptr?target->parameter_names[index]:"");
    }

    DiamondFunction *outer_function=compiler->function;
    const uint16_t outer_next_register=compiler->next_register;
    const size_t outer_local_count=compiler->local_count;
    const bool outer_in_function=compiler->in_function;
    const bool outer_in_block=compiler->in_block;
    const size_t outer_begin_depth=compiler->begin_depth;

    compiler->function=function;
    compiler->next_register=0;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->local_count=0;
    compiler->in_function=true;
    compiler->in_block=false;
    compiler->begin_depth=0;

    uint16_t arguments[DIAMOND_MAX_DECLARED_PARAMETERS];
    for(size_t index=0;index<method->arity;index++) {
        arguments[index]=allocate_register(compiler);
    }
    uint16_t body_result=0;
    if(method->has_variadic) {
        const size_t fixed_count=method->arity-1;
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            arguments[fixed_count],(uint16_t)fixed_count,0,3);
        uint16_t spread=arguments[fixed_count];
        if(fixed_count>0)spread=emit_build_spread_arguments(compiler,arguments,
            fixed_count,spread,nullptr,0,false);
        body_result=allocate_register(compiler);
        emit_opcode(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_SPREAD:
            DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD);
        emit_register(compiler,body_result);
        if(target==nullptr)
            emit_pending_singleton_function_index(compiler,namespace_name,method);
        else
            emit_function_index(compiler,method->function_index);
        emit_register(compiler,spread);
        emit_register(compiler,receiver_class_index<0?DIAMOND_NO_CLASS:
            (uint16_t)receiver_class_index);
        emit_byte(compiler,method->needs_receiver?1:0);
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
    } else body_result=emit_singleton_call(compiler,method,
        receiver_class_index,arguments,method->arity,namespace_name,
        (uint8_t)type_argument_count,type_arguments);
    emit_instruction(compiler,DIAMOND_OP_RETURN,body_result,0,0,1);

    function->register_count=compiler->next_register;
    function->body_end=compiler->previous.span.start+compiler->previous.span.length;

    compiler->function=outer_function;
    compiler->next_register=outer_next_register;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->local_count=outer_local_count;
    compiler->in_function=outer_in_function;
    compiler->in_block=outer_in_block;
    compiler->begin_depth=outer_begin_depth;

    const uint16_t result=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
    emit_function_index(compiler,function_index);emit_byte(compiler,0);
    publish_function_callable_type(compiler,result,function);
    return result;
}

uint16_t parse_singleton_call(Compiler *compiler,
                                    const DiamondMethod *method,
                                    DiamondSpan namespace_name,
                                    int receiver_class_index) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected singleton function after module name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    /* A module_function/`def self.x` signature a module's own pre-scan
     * found ahead of the real def (prescan_singleton_signatures)
     * has no real function yet -- method->function_index stays
     * DIAMOND_UNRESOLVED_SINGLETON_FUNCTION until the real def compiles
     * and resolves the pending call-site fixup this function's own
     * emit_singleton_call records below. Every consumer of `function`
     * from here down already tolerates nullptr (the same "statically
     * unknown callee" case a dynamically dispatched Callable *value*
     * call already exercises -- parse_expected_argument, publish_
     * declared_return_type, compile_contextual_typed_block, and
     * friends), so this degrades to "no contextual type hints for this
     * call's arguments/return value," not a crash: the callee's own
     * body still separately declares and enforces its real parameter/
     * return types regardless of what the caller side could infer. */
    const DiamondFunction *function=
        method->function_index==DIAMOND_UNRESOLVED_SINGLETON_FUNCTION?nullptr:
        compiler->program->functions[method->function_index];
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL)advance_token(compiler);
    uint16_t type_arguments[8];size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            if(type_argument_count==8) {
                fail(compiler,compiler->current.span,"too many generic arguments");
                return 0;
            }
            type_arguments[type_argument_count++]=
                (uint16_t)parse_type_annotation(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
        if(function==nullptr) {
            fail(compiler,name,
                "explicit generic arguments are not supported on a forward-"
                "referenced module_function call");return 0;
        }
        if(type_argument_count!=function->type_variable_count) {
            fail(compiler,name,"wrong number of generic arguments");return 0;
        }
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        /* `Namespace.method`, no call following -- a bare reference to
         * the method as a value (parse_singleton_reference, above)
         * rather than a call, unless it was given generic type
         * arguments (`Namespace.method[T]` with no call after): a
         * reference wrapper can't itself be generic, so that combination
         * stays a real error, not silently falling through. */
        return parse_singleton_reference(compiler,method,namespace_name,
            receiver_class_index,type_arguments,type_argument_count);
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(function==nullptr&&
       (call_arguments_have_keyword(compiler)||call_arguments_have_spread(compiler))) {
        /* Keyword and spread-argument calls both need the real callee's
         * declared parameter types/count for correct contextual
         * inference (see e.g. this function's own function->
         * type_variable_count reads just below, unguarded for good
         * reason everywhere else in this file -- every *other* caller
         * always has a real function). A forward-referenced module_
         * function call only supports plain positional arguments;
         * reject the rarer shapes with a clear error instead of
         * building out (and fully auditing) contextual inference for a
         * callee that doesn't exist yet. */
        fail(compiler,name,
            "keyword and spread arguments are not supported on a forward-"
            "referenced module_function call");return 0;
    }
    if(call_arguments_have_keyword(compiler)) {
        DiamondSpan keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];uint16_t keyword_values[DIAMOND_MAX_DECLARED_PARAMETERS];
        size_t keyword_count=0;
        const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
            keyword_names,keyword_values,&keyword_count,function);
        uint16_t inferred_arguments[8];
        if(type_argument_count==0) {
            const size_t block_count=
                compiler->current.kind==DIAMOND_TOKEN_DO?1u:0u;
            infer_contextual_spread_arguments(compiler,function,positional,
                method->arity>block_count?method->arity-block_count:0,
                inferred_arguments);
            infer_contextual_keyword_arguments(compiler,function,
                keyword_names,keyword_values,keyword_count,inferred_arguments);
        }
        const uint16_t *resolved_arguments=type_argument_count>0?
            type_arguments:inferred_arguments;
        const size_t resolved_count=type_argument_count>0?
            type_argument_count:function->type_variable_count;
        bool has_block=false;uint16_t block=0;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_values[index],0,2);
                keyword_values[index]=snapshot;
            }
            block=compile_contextual_typed_block(compiler,function,
                method->arity==0?0:method->arity-1,
                resolved_arguments,resolved_count);
            has_block=true;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
            DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS);
        emit_register(compiler,destination);
        emit_function_index(compiler,method->function_index);
        emit_register(compiler,positional);
        emit_register(compiler,receiver_class_index<0?DIAMOND_NO_CLASS:
            (uint16_t)receiver_class_index);
        emit_byte(compiler,method->needs_receiver?1:0);
        emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
        for(size_t index=0;index<keyword_count;index++) {
            emit_register(compiler,add_name_string(compiler,keyword_names[index]));
            emit_register(compiler,keyword_values[index]);
        }
        if(has_block)emit_register(compiler,block);
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
        publish_call_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    if(call_arguments_have_spread(compiler)) {
        uint16_t spread=parse_spread_argument_array(compiler,nullptr,
            nullptr,nullptr,nullptr,function,method->arity);
        uint16_t inferred_arguments[8];
        const size_t block_count=
            compiler->current.kind==DIAMOND_TOKEN_DO?1u:0u;
        const size_t spread_parameter_count=method->arity>block_count?
            method->arity-block_count:0;
        const size_t inferred_count=type_argument_count==0?
            infer_contextual_spread_arguments(compiler,function,spread,
                spread_parameter_count,inferred_arguments):0;
        const uint16_t *resolved_arguments=type_argument_count>0?
            type_arguments:inferred_arguments;
        const size_t resolved_count=type_argument_count>0?
            type_argument_count:inferred_count;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t spread_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
                spread,0,2);
            spread=spread_snapshot;
            const uint16_t block=compile_contextual_typed_block(compiler,
                function,method->arity==0?0:method->arity-1,
                resolved_arguments,resolved_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,type_argument_count==0?
                DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
                DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS);
            emit_register(compiler,destination);
            emit_function_index(compiler,method->function_index);
            emit_register(compiler,spread);
            emit_register(compiler,receiver_class_index<0?DIAMOND_NO_CLASS:
            (uint16_t)receiver_class_index);
            emit_byte(compiler,method->needs_receiver?1:0);
            emit_byte(compiler,0x80u);emit_register(compiler,block);
            if(type_argument_count>0) {
                emit_byte(compiler,(uint8_t)type_argument_count);
                for(size_t index=0;index<type_argument_count;index++)
                    emit_register(compiler,type_arguments[index]);
            }
            publish_call_return_type(compiler,destination,function,
                resolved_arguments,resolved_count);
            return destination;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_SPREAD:
            DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD);
        emit_register(compiler,destination);
        emit_function_index(compiler,method->function_index);
        emit_register(compiler,spread);
        emit_register(compiler,receiver_class_index<0?DIAMOND_NO_CLASS:
            (uint16_t)receiver_class_index);
        emit_byte(compiler,method->needs_receiver?1:0);
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
        publish_call_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    uint16_t arguments[DIAMOND_MAX_ARGUMENTS];size_t argument_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        if(argument_count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        arguments[argument_count]=parse_expected_argument(compiler,function,
            argument_count);
        argument_count++;
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;
    }
    advance_token(compiler);
    uint16_t inferred_arguments[8];
    const size_t inferred_count=type_argument_count==0?
        infer_contextual_type_arguments(compiler,function,arguments,
            argument_count,inferred_arguments):0;
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:inferred_count;
    /* `Namespace.method(args) do |x| ... end` -- same trailing-block-as-
     * last-positional-argument desugaring parse_invoke's own DIAMOND_TOKEN_DO
     * handling does for instance-method calls (see its comment there for
     * the full design, including why every argument is snapshotted into a
     * fresh temp first: compile_block's eager, unconditional BOX_LOCAL
     * over every enclosing local could otherwise retarget a bare local
     * register one of these arguments already resolved to). Singleton
     * calls have no separate caller-side receiver register to snapshot
     * (unlike parse_invoke's `receiver`) -- `self`/the owning module is
     * filled in by the call machinery itself when method->needs_receiver,
     * not read from a register here. */
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(argument_count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        for(size_t index=0;index<argument_count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,arguments[index],0,2);
            arguments[index]=snapshot;
        }
        const uint16_t block=compile_contextual_typed_block(compiler,
            function,method->arity==0?0:method->arity-1,
            resolved_arguments,resolved_count);
        const size_t block_slot=method->arity==0?0:method->arity-1;
        if(argument_count<block_slot) {
            const uint16_t positional=
                emit_argument_array(compiler,arguments,argument_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,type_argument_count==0?
                DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
                DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS);
            emit_register(compiler,destination);
            emit_function_index(compiler,method->function_index);
            emit_register(compiler,positional);
            emit_register(compiler,receiver_class_index<0?DIAMOND_NO_CLASS:
            (uint16_t)receiver_class_index);
            emit_byte(compiler,method->needs_receiver?1:0);
            emit_byte(compiler,0x80u);emit_register(compiler,block);
            if(type_argument_count>0) {
                emit_byte(compiler,(uint8_t)type_argument_count);
                for(size_t index=0;index<type_argument_count;index++)
                    emit_register(compiler,type_arguments[index]);
            }
            publish_call_return_type(compiler,destination,function,
                resolved_arguments,resolved_count);
            return destination;
        }
        arguments[argument_count++]=block;
    }
    if(argument_count<method->required_arity||
       (argument_count>method->arity && !method->has_variadic)) {
        fail(compiler,name,"wrong number of arguments");return 0;
    }
    const size_t call_count=argument_count+(method->needs_receiver?1:0);
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<call_count;index++)(void)allocate_register(compiler);
    /* No NIL for `base` when needs_receiver and there's no literal class
     * (a module call): sole writer, already zero-inited by run_chunk's
     * [0, register_count) init. A class singleton call instead loads the
     * literal receiver class right here, the one place this slot gets a
     * real value instead of relying on zero-init. */
    if(method->needs_receiver&&receiver_class_index>=0) {
        emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
        emit_register(compiler,base);
        emit_register(compiler,(uint16_t)receiver_class_index);
    }
    for(size_t index=0;index<argument_count;index++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,
                         (uint16_t)(base+index+(method->needs_receiver?1:0)),
                         arguments[index],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_CALL:
                DIAMOND_OP_CALL_TYPED);
    emit_register(compiler,destination);
    emit_function_index(compiler,method->function_index);
    emit_register(compiler,base);emit_byte(compiler,(uint8_t)call_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    publish_call_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
    return destination;
}

bool singleton_call_name_equals(const Compiler *compiler,
                                       const char *candidate,DiamondSpan name) {
    const size_t length=strlen(candidate);
    if(length==name.length)return name_equals(compiler,candidate,name,false);
    if(length!=name.length+1||candidate[length-1]!='=')return false;
    DiamondLexer lookahead=compiler->lexer;
    if(diamond_lexer_next(&lookahead).kind!=DIAMOND_TOKEN_EQUAL)return false;
    return memcmp(candidate,compiler->source+name.start,name.length)==0;
}

static uint16_t parse_grouping(Compiler *compiler) {
    const uint16_t result = parse_expression(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after expression");
        return result;
    }
    advance_token(compiler);
    return result;
}

uint16_t parse_with_expected_set(Compiler *compiler,
        uint16_t expected_set) {
    const int32_t outer=compiler->expected_expression_type_set;
    compiler->expected_expression_type_set=expected_set==DIAMOND_NO_TYPE_SET?
        -1:(int32_t)expected_set;
    const uint16_t result=parse_expression(compiler);
    compiler->expected_expression_type_set=outer;
    return result;
}

static uint16_t parse_array_literal(Compiler *compiler,
        const DiamondFunction *expected_function,size_t first_parameter,
        size_t parameter_count) {
    uint16_t elements[DIAMOND_MAX_ARGUMENTS];
    size_t count=0;
    uint16_t expected_element=DIAMOND_NO_TYPE_SET;
    if(compiler->expected_expression_type_set>=0&&
       (size_t)compiler->expected_expression_type_set<
           compiler->function->type_set_count) {
        const DiamondTypeSet *expected=&compiler->function->type_sets[
            (size_t)compiler->expected_expression_type_set];
        if(expected->count==1&&expected->members[0].id==DIAMOND_TYPE_ARRAY)
            expected_element=expected->members[0].argument_set;
    }
    skip_newlines(compiler);
    /* `[head, *rest, tail]`: at most one splat, whose Array's elements are
     * spliced in place. spread_at is its position among `elements`. */
    size_t spread_at=SIZE_MAX;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        do {
            if(count==DIAMOND_MAX_ARGUMENTS) {
                fail(compiler,compiler->current.span,"array literal has too many elements");
                return 0;
            }
            if(compiler->current.kind==DIAMOND_TOKEN_STAR&&expected_function==nullptr) {
                if(spread_at!=SIZE_MAX) {
                    fail(compiler,compiler->current.span,
                        "an array literal can splat only one array");
                    return 0;
                }
                advance_token(compiler);
                spread_at=count;
                elements[count++]=parse_expression(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
                advance_token(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET) break;
                continue;
            }
            if(expected_function!=nullptr&&
               first_parameter+count<parameter_count)
                elements[count]=parse_expected_argument(compiler,
                    expected_function,first_parameter+count);
            else elements[count]=parse_with_expected_set(compiler,
                expected_element);
            count++;
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET) break;
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after array literal");
        return 0;
    }
    advance_token(compiler);
    if(spread_at!=SIZE_MAX) {
        /* The same builder a call's `f(a, *rest, z)` uses: fixed prefix,
         * one spread Array (TypeError if it isn't one), fixed suffix. */
        const uint16_t destination=emit_build_spread_arguments(compiler,elements,
            spread_at,elements[spread_at],elements+spread_at+1,
            count-spread_at-1,false);
        publish_array_spread_tooling_type(compiler,destination,elements,spread_at,
            elements[spread_at],elements+spread_at+1,count-spread_at-1);
        return destination;
    }
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) emit_instruction(compiler,DIAMOND_OP_MOVE,
        (uint16_t)(base+i),elements[i],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,destination,base,(uint8_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_ARRAY;
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_ARRAY,
        joined_value_type_set(compiler,elements,count),-1);
    publish_array_literal_tooling_type(compiler,destination,elements,count);
    if(expected_function!=nullptr) {
        compiler->positional_spread_literal=true;
        compiler->positional_spread_first=first_parameter;
        compiler->positional_spread_count=count;
        for(size_t index=0;index<count;index++)
            compiler->positional_spread_elements[index]=elements[index];
    }
    return destination;
}

static uint16_t parse_array(Compiler *compiler) {
    return parse_array_literal(compiler,nullptr,0,0);
}

static uint16_t parse_hash(Compiler *compiler) {
    uint16_t keys[DIAMOND_MAX_ARGUMENTS],values[DIAMOND_MAX_ARGUMENTS];
    size_t count=0;
    uint16_t expected_key=DIAMOND_NO_TYPE_SET;
    uint16_t expected_value=DIAMOND_NO_TYPE_SET;
    if(compiler->expected_expression_type_set>=0&&
       (size_t)compiler->expected_expression_type_set<
           compiler->function->type_set_count) {
        const DiamondTypeSet *expected=&compiler->function->type_sets[
            (size_t)compiler->expected_expression_type_set];
        if(expected->count==1&&expected->members[0].id==DIAMOND_TYPE_HASH) {
            expected_key=expected->members[0].argument_set;
            expected_value=expected->members[0].second_argument_set;
        }
    }
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        do {
            if(count==DIAMOND_MAX_ARGUMENTS) {
                fail(compiler,compiler->current.span,"hash literal has too many entries");
                return 0;
            }
            keys[count]=parse_with_expected_set(compiler,expected_key);
            /* `{"a": 1}` and Ruby's `{"a" => 1}` mean the same thing. */
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON&&
               compiler->current.kind!=DIAMOND_TOKEN_FAT_ARROW) {
                fail(compiler,compiler->current.span,"expected ':' or '=>' after hash key");
                return 0;
            }
            advance_token(compiler);
            values[count]=parse_with_expected_set(compiler,expected_value);
            count++;
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE) break;
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        fail(compiler,compiler->current.span,"expected '}' after hash literal");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count*2;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) {
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2),keys[i],0,2);
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2+1),values[i],0,2);
    }
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_HASH,destination,base,(uint16_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_HASH;
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_HASH,
        joined_value_type_set(compiler,keys,count),
        joined_value_type_set(compiler,values,count));
    return destination;
}

int32_t type_set_with_nil(Compiler *compiler,uint16_t source_index) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    for(size_t index=0;index<source.count;index++)
        if(source.members[index].id==DIAMOND_TYPE_NIL)return (int32_t)source_index;
    if(source.count==DIAMOND_MAX_UNION_TYPES||
       !grow_type_sets(compiler->function,1))return -1;
    const size_t result=compiler->function->type_set_count++;
    compiler->function->type_sets[result]=source;
    DiamondTypeSet *set=&compiler->function->type_sets[result];
    set->members[set->count++]=(DiamondTypeMember){
        .id=DIAMOND_TYPE_NIL,.argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,.callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET};
    return (int32_t)result;
}

static bool split_nil_type_set(Compiler *compiler,uint16_t source_index,
                               int32_t *without_nil,int32_t *only_nil) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet narrowed={};bool found_nil=false;
    for(size_t index=0;index<source.count;index++) {
        if(source.members[index].id==DIAMOND_TYPE_NIL)found_nil=true;
        else narrowed.members[narrowed.count++]=source.members[index];
    }
    if(!found_nil||narrowed.count==0||
       !grow_type_sets(compiler->function,2))return false;
    *without_nil=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=narrowed;
    *only_nil=(int32_t)compiler->function->type_set_count;
    DiamondTypeSet *nil_set=&compiler->function->type_sets[
        compiler->function->type_set_count++];
    *nil_set=(DiamondTypeSet){.members={{.id=DIAMOND_TYPE_NIL,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
    return true;
}

static bool split_type_set(Compiler *compiler,uint16_t source_index,
                           uint16_t tested_type,int32_t *matching,
                           int32_t *remaining) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet yes={},no={};
    for(size_t index=0;index<source.count;index++) {
        const DiamondTypeMember member=source.members[index];
        if(known_type_satisfies_one(compiler,member.id,tested_type))
            yes.members[yes.count++]=member;
        else no.members[no.count++]=member;
    }
    if(yes.count==0||no.count==0||
       !grow_type_sets(compiler->function,2))return false;
    *matching=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=yes;
    *remaining=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=no;
    return true;
}

void apply_type_set_fact(Compiler *compiler,uint16_t reg,int32_t set_index) {
    compiler->known_type_sets[reg]=set_index;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)set_index];
    compiler->known_types[reg]=set->count==1?set->members[0].id:TYPE_UNKNOWN;
}

void apply_narrowing_facts(Compiler *compiler,const NarrowingFact *facts,
                                  size_t count) {
    for(size_t index=0;index<count;index++)
        apply_type_set_fact(compiler,facts[index].reg,facts[index].type_set);
}

static void append_narrowing_facts(NarrowingFact *destination,size_t *destination_count,
                                   const NarrowingFact *source,size_t source_count) {
    for(size_t index=0;index<source_count&&
        *destination_count<DIAMOND_MAX_NARROWING_FACTS;index++)
        destination[(*destination_count)++]=source[index];
}

static bool type_members_equal(DiamondTypeMember left,DiamondTypeMember right) {
    if(left.id!=right.id||left.argument_set!=right.argument_set||
       left.second_argument_set!=right.second_argument_set||
       left.callable_arity!=right.callable_arity||
       left.callable_return_set!=right.callable_return_set||
       left.callable_parameters_typed!=right.callable_parameters_typed)return false;
    if(!left.callable_parameters_typed)return true;
    for(size_t index=0;index<left.callable_arity;index++)
        if(left.callable_parameter_sets[index]!=right.callable_parameter_sets[index])return false;
    return true;
}

static bool type_sets_equal_unordered(DiamondTypeSet left,DiamondTypeSet right) {
    if(left.count!=right.count)return false;
    for(size_t left_index=0;left_index<left.count;left_index++) {
        bool found=false;
        for(size_t right_index=0;right_index<right.count;right_index++)
            if(type_members_equal(left.members[left_index],right.members[right_index])) {
                found=true;break;
            }
        if(!found)return false;
    }
    return true;
}

static DiamondTypeMember plain_type_member(uint16_t type) {
    return (DiamondTypeMember){.id=type,.argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,.callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET};
}

static bool append_merged_member(DiamondTypeSet *merged,DiamondTypeMember member) {
    for(size_t index=0;index<merged->count;index++) {
        if(type_members_equal(merged->members[index],member))return true;
    }
    if(merged->count==DIAMOND_MAX_UNION_TYPES)return false;
    merged->members[merged->count++]=member;return true;
}

static bool append_flow_type(const Compiler *compiler,DiamondTypeSet *merged,
        uint16_t known_type,int32_t known_set) {
    if(known_set>=0) {
        if((size_t)known_set>=compiler->function->type_set_count)return false;
        const DiamondTypeSet set=compiler->function->type_sets[(size_t)known_set];
        for(size_t index=0;index<set.count;index++)
            if(!append_merged_member(merged,set.members[index]))return false;
        return set.count>0;
    }
    return known_type!=TYPE_UNKNOWN&&
        append_merged_member(merged,plain_type_member(known_type));
}

/* Conservatively joins two control-flow type states. Unlike runtime type
 * annotations this metadata is advisory: an unrepresentable or exhausted
 * union simply becomes unknown and must never make compilation fail. */
void merge_flow_types(Compiler *compiler,uint16_t left_type,int32_t left_set,
        uint16_t right_type,int32_t right_set,uint16_t *result_type,int32_t *result_set) {
    *result_type=TYPE_UNKNOWN;*result_set=-1;
    if(left_type==right_type&&left_set==right_set) {
        *result_type=left_type;*result_set=left_set;return;
    }
    DiamondTypeSet merged={};
    if(!append_flow_type(compiler,&merged,left_type,left_set)||
       !append_flow_type(compiler,&merged,right_type,right_set))return;
    merged.inferred=true;
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        if(!compiler->function->type_sets[index].inferred)continue;
        if(!type_sets_equal_unordered(merged,compiler->function->type_sets[index]))continue;
        *result_set=(int32_t)index;
        *result_type=merged.count==1?merged.members[0].id:TYPE_UNKNOWN;
        return;
    }
    if(!grow_type_sets(compiler->function,1))return;
    const size_t index=compiler->function->type_set_count++;
    compiler->function->type_sets[index]=merged;
    *result_set=(int32_t)index;
    *result_type=merged.count==1?merged.members[0].id:TYPE_UNKNOWN;
}

/* Tooling-only receiver facts are sometimes cloned independently while
 * compiling separate control-flow arms. Their table indices then differ even
 * though they describe the same receiver graph, so compare the graphs before
 * conservatively discarding the fact at the join. */
int32_t merge_tooling_type_sets(const Compiler *compiler,int32_t left,
        int32_t right) {
    if(left==right)return left;
    if(left<0||right<0||(size_t)left>=compiler->function->type_set_count||
       (size_t)right>=compiler->function->type_set_count)return -1;
    return type_sets_structurally_equal(compiler->function->type_sets,
        compiler->function->type_set_count,(uint16_t)left,
        compiler->function->type_sets,compiler->function->type_set_count,
        (uint16_t)right,0)?left:-1;
}

/* Conservatively joins two control-flow alias-identity states for the same
 * local. Agreeing branches keep the shared identity; disagreeing branches
 * (including either being unaliased) detach to 0 rather than guessing which
 * object is actually live -- alias_identity_for_value/
 * propagate_collection_alias_fact already treat 0 as "assign a fresh identity
 * lazily on next use", so a detached local still tracks its own future
 * mutations/aliases correctly, just independently of either branch's object. */
uint32_t merge_alias_identity(uint32_t left,uint32_t right) {
    return left==right?left:0;
}

bool register_is_local(const Compiler *compiler,uint16_t reg) {
    for(size_t index=0;index<compiler->local_count;index++)
        if(compiler->locals[index].reg==reg)return true;
    return false;
}

static void set_index_provenance(Compiler *compiler,uint16_t destination,
        uint16_t source_receiver) {
    if(!register_is_local(compiler,source_receiver))return;
    compiler->has_index_provenance[destination]=true;
    compiler->index_provenance_root[destination]=source_receiver;
}

/* Joins a local first bound inside *some* branch of a multi-exit-point
 * construct (a `case`'s `when`/`else` clauses, or a loop's exit points --
 * the zero-iteration path, every `break`, and the natural loop-back/
 * condition-false path) -- shared because both need the exact same
 * seed-vs-merge shape, keyed by `new_local_count`/`new_types`/`new_sets`/
 * `new_alias` fields living on the caller's own join/loop struct (passed by
 * pointer/array here since CaseFlowJoin and LoopContext aren't the same
 * type). Slots [0, *new_local_count) have already been reached by an
 * earlier-processed branch/exit-point and merge normally; a slot reached
 * for the first time either seeds directly with this branch's value (this
 * is the very first branch/exit-point overall, `earlier_branch_exists`
 * false, so there's no earlier one to have implicitly skipped it) or seeds
 * as `merge(Nil, this branch's value)` (every other case: some earlier
 * branch/exit-point already ran without this local existing at all, so it
 * implicitly contributed Nil down that path). */
void merge_new_local_facts(Compiler *compiler,bool earlier_branch_exists,
        size_t flow_local_count,size_t *new_local_count,uint16_t *new_types,
        int32_t *new_sets,uint32_t *new_alias) {
    const size_t current_local_count=compiler->local_count;
    for(size_t index=flow_local_count;
        index<current_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const size_t slot=index-flow_local_count;
        const uint16_t reg=compiler->locals[index].reg;
        if(slot>=*new_local_count) {
            if(earlier_branch_exists)
                merge_flow_types(compiler,DIAMOND_TYPE_NIL,-1,
                    compiler->known_types[reg],compiler->known_type_sets[reg],
                    &new_types[slot],&new_sets[slot]);
            else {
                new_types[slot]=compiler->known_types[reg];
                new_sets[slot]=compiler->known_type_sets[reg];
            }
            new_alias[slot]=compiler->locals[index].alias_identity;
        } else {
            merge_flow_types(compiler,new_types[slot],new_sets[slot],
                compiler->known_types[reg],compiler->known_type_sets[reg],
                &new_types[slot],&new_sets[slot]);
            new_alias[slot]=merge_alias_identity(new_alias[slot],
                compiler->locals[index].alias_identity);
        }
    }
    if(current_local_count>flow_local_count) {
        const size_t reached=current_local_count-flow_local_count;
        if(reached>*new_local_count)
            *new_local_count=reached<DIAMOND_MAX_LOCALS?reached:DIAMOND_MAX_LOCALS;
    }
}

void merge_loop_exit(Compiler *compiler,LoopContext *loop,
        uint16_t result_type,int32_t result_set,int32_t result_tooling_set) {
    if(!loop->exit_initialized) {
        for(size_t index=0;index<loop->flow_reg_count;index++) {
            loop->exit_types[index]=compiler->known_types[index];
            loop->exit_sets[index]=compiler->known_type_sets[index];
            loop->exit_tooling_sets[index]=compiler->tooling_type_sets[index];
        }
        for(size_t index=0;index<loop->flow_local_count;index++)
            loop->exit_alias[index]=compiler->locals[index].alias_identity;
        merge_new_local_facts(compiler,false,loop->flow_local_count,
            &loop->new_local_count,loop->new_types,loop->new_sets,loop->new_alias);
        loop->result_type=result_type;loop->result_set=result_set;
        loop->result_tooling_set=result_tooling_set;
        loop->exit_initialized=true;return;
    }
    for(size_t index=0;index<loop->flow_reg_count;index++) {
        merge_flow_types(compiler,loop->exit_types[index],loop->exit_sets[index],
            compiler->known_types[index],compiler->known_type_sets[index],
            &loop->exit_types[index],&loop->exit_sets[index]);
        loop->exit_tooling_sets[index]=merge_tooling_type_sets(compiler,
            loop->exit_tooling_sets[index],compiler->tooling_type_sets[index]);
    }
    for(size_t index=0;index<loop->flow_local_count;index++)
        loop->exit_alias[index]=merge_alias_identity(loop->exit_alias[index],
            compiler->locals[index].alias_identity);
    merge_new_local_facts(compiler,true,loop->flow_local_count,
        &loop->new_local_count,loop->new_types,loop->new_sets,loop->new_alias);
    merge_flow_types(compiler,loop->result_type,loop->result_set,
        result_type,result_set,&loop->result_type,&loop->result_set);
    loop->result_tooling_set=merge_tooling_type_sets(compiler,
        loop->result_tooling_set,result_tooling_set);
}

static void finish_loop_flow(Compiler *compiler,LoopContext *loop,
        size_t effective_start) {
    if(!loop->exit_initialized)return;
    for(size_t index=0;index<loop->flow_local_count;index++)
        compiler->locals[index].alias_identity=loop->exit_alias[index];
    for(size_t index=0;index<loop->flow_reg_count;index++) {
        compiler->known_types[index]=loop->exit_types[index];
        compiler->known_type_sets[index]=loop->exit_sets[index];
        compiler->tooling_type_sets[index]=loop->exit_tooling_sets[index];
        if(register_is_local(compiler,(uint16_t)index))
            record_scope_type_fact(compiler,(uint16_t)index,effective_start);
    }
    const size_t final_local_count=compiler->local_count;
    for(size_t index=loop->flow_local_count;
        index<final_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const size_t slot=index-loop->flow_local_count;
        const uint16_t reg=compiler->locals[index].reg;
        compiler->known_types[reg]=loop->new_types[slot];
        compiler->known_type_sets[reg]=loop->new_sets[slot];
        compiler->tooling_type_sets[reg]=-1;
        compiler->locals[index].alias_identity=loop->new_alias[slot];
        record_scope_type_fact(compiler,reg,effective_start);
    }
    compiler->known_types[loop->result_register]=loop->result_type;
    compiler->known_type_sets[loop->result_register]=loop->result_set;
    compiler->tooling_type_sets[loop->result_register]=loop->result_tooling_set;
}

/* Shared by parse_index (`x[i]` as an ordinary expression) and
 * compile_index_assignment's own chain loop (`x[a][b]... = value`'s
 * every-group-but-the-last read) -- both emit an INDEX_GET and need the
 * same "what type is the indexed result" computation on `destination`, or a
 * later mutation through `destination` (nested writeback, or simply reading
 * its own hover) starts from an untyped register instead of the element/
 * value graph it actually just read. */
static void publish_indexed_result_type(Compiler *compiler,uint16_t destination,
        uint16_t receiver) {
    const int32_t tooling_element=joined_collection_argument(compiler,
        compiler->tooling_type_sets[receiver],DIAMOND_TYPE_ARRAY,false);
    if(tooling_element>=0)compiler->tooling_type_sets[destination]=tooling_element;
    int32_t receiver_set=compiler->known_type_sets[receiver];
    const bool tooling_only=receiver_set<0;
    if(tooling_only)receiver_set=compiler->tooling_type_sets[receiver];
    if(receiver_set<0)return;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)receiver_set];
    uint16_t indexed_sets[DIAMOND_MAX_UNION_TYPES];
    uint16_t collection_type=TYPE_UNKNOWN;
    const size_t member_count=set->count;
    bool compatible=member_count>0;
    for(size_t member_index=0;member_index<member_count;member_index++) {
        const DiamondTypeMember member=set->members[member_index];
        if(member.id!=DIAMOND_TYPE_ARRAY&&member.id!=DIAMOND_TYPE_HASH) {
            compatible=false;break;
        }
        if(collection_type==TYPE_UNKNOWN)collection_type=member.id;
        else if(collection_type!=member.id) {compatible=false;break;}
        indexed_sets[member_index]=member.id==DIAMOND_TYPE_ARRAY?
            member.argument_set:member.second_argument_set;
        if(indexed_sets[member_index]==DIAMOND_NO_TYPE_SET) {
            compatible=false;break;
        }
    }
    int32_t indexed=-1;
    for(size_t member_index=0;compatible&&member_index<member_count;
        member_index++) {
        indexed=indexed<0?(int32_t)indexed_sets[member_index]:
            join_type_set_indices(compiler,indexed,
                (int32_t)indexed_sets[member_index]);
        if(indexed<0)compatible=false;
    }
    /* Keep inferred Hash indexing unresolved: its value may be Nil. */
    if(tooling_only&&collection_type!=DIAMOND_TYPE_ARRAY)return;
    if(compatible&&collection_type==DIAMOND_TYPE_HASH)
        indexed=type_set_with_nil(compiler,(uint16_t)indexed);
    if(compatible&&indexed>=0) {
        /* An inferred call's collection shape can identify an assigned
         * element for the editor, but cannot change checks or dispatch. */
        if(tooling_only)compiler->tooling_type_sets[destination]=indexed;
        else publish_known_type_set(compiler,destination,(uint16_t)indexed);
    }
}

/* Is this string token a plain literal -- no unescaped `#{` interpolation -- the same scan
 * parse_string uses to decide whether it builds a single STRING or a concatenation? */
static bool string_token_is_plain(const Compiler *compiler,DiamondSpan span) {
    const size_t end=span.start+span.length-1;
    for(size_t index=span.start+1;index+1<end;) {
        if(compiler->source[index]=='\\') {index+=2;continue;}
        if(compiler->source[index]=='#'&&compiler->source[index+1]=='{')return false;
        index++;
    }
    return true;
}

static bool token_is_index_assignment(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_EQUAL||kind==DIAMOND_TOKEN_OR_OR_EQUAL||
        kind==DIAMOND_TOKEN_AND_AND_EQUAL||kind==DIAMOND_TOKEN_PLUS_EQUAL||
        kind==DIAMOND_TOKEN_MINUS_EQUAL||kind==DIAMOND_TOKEN_STAR_EQUAL||
        kind==DIAMOND_TOKEN_SLASH_EQUAL||kind==DIAMOND_TOKEN_PERCENT_EQUAL;
}

uint16_t parse_index(Compiler *compiler,uint16_t receiver) {
    advance_token(compiler);
    /* `receiver["literal"]` read: skip the STRING instruction that would allocate the key on
     * every execution and emit INDEX_GET_STRING, which looks a Hash up by the constant's
     * bytes. Only for a read (an assignment still needs the key in a register, for
     * INDEX_SET) and only for a plain literal followed directly by `]` (not the
     * `value["a", n]` slice form). */
    if(compiler->current.kind==DIAMOND_TOKEN_STRING&&
       string_token_is_plain(compiler,compiler->current.span)) {
        DiamondLexer ahead=compiler->lexer;
        if(diamond_lexer_next(&ahead).kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
           !token_is_index_assignment(diamond_lexer_next(&ahead).kind)) {
            advance_token(compiler);
            const uint16_t key=add_string(compiler,compiler->previous.span);
            advance_token(compiler);
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_INDEX_GET_STRING,destination,receiver,key,3);
            set_index_provenance(compiler,destination,receiver);
            publish_indexed_result_type(compiler,destination,receiver);
            return destination;
        }
    }
    const uint16_t index=parse_expression(compiler);
    /* `value[start, length]` reads a run, as value.slice(start, length)
     * does (Array and String both have slice). */
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        const uint16_t arguments[2]={index,parse_expression(compiler)};
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,"expected ']' after index"); return 0;
        }
        advance_token(compiler);
        return emit_invoke_named(compiler,receiver,"slice",arguments,2);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after index"); return 0;
    }
    advance_token(compiler);
    /* `expr[index] = value` (and `+=` and friends) where expr isn't a
     * plain local/@ivar/@@cvar -- `obj.table()[key] = value`,
     * `lookup()[key] += 1`. Those three simple receivers are handled
     * before expression parsing starts (compile_index_assignment and
     * compile_index_compound_assignment); any other receiver reaches here
     * as an ordinary value, and storing into it is the same INDEX_SET.
     * Only the receiver object is mutated, so there's no local whose
     * collection type facts need widening. */
    const DiamondTokenKind assign=compiler->current.kind;
    if(assign==DIAMOND_TOKEN_EQUAL) {
        advance_token(compiler);
        const uint16_t value=parse_expression(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,value,3);
        return value;
    }
    if(assign==DIAMOND_TOKEN_OR_OR_EQUAL||assign==DIAMOND_TOKEN_AND_AND_EQUAL||
       assign==DIAMOND_TOKEN_PLUS_EQUAL||assign==DIAMOND_TOKEN_MINUS_EQUAL||
       assign==DIAMOND_TOKEN_STAR_EQUAL||assign==DIAMOND_TOKEN_SLASH_EQUAL||
       assign==DIAMOND_TOKEN_PERCENT_EQUAL) {
        advance_token(compiler);
        const uint16_t left=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,left,receiver,index,3);
        const uint16_t destination=allocate_register(compiler);
        if(assign==DIAMOND_TOKEN_OR_OR_EQUAL||assign==DIAMOND_TOKEN_AND_AND_EQUAL) {
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                assign==DIAMOND_TOKEN_AND_AND_EQUAL?DIAMOND_OP_JUMP_IF_FALSE:
                    DIAMOND_OP_JUMP_IF_TRUE,left);
            const uint16_t right=parse_expression(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
            emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,destination,3);
            patch_jump(compiler,end_jump,compiler->function->code_count);
            return destination;
        }
        const DiamondTokenKind plain_op=
            assign==DIAMOND_TOKEN_PLUS_EQUAL?DIAMOND_TOKEN_PLUS:
            assign==DIAMOND_TOKEN_MINUS_EQUAL?DIAMOND_TOKEN_MINUS:
            assign==DIAMOND_TOKEN_STAR_EQUAL?DIAMOND_TOKEN_STAR:
            assign==DIAMOND_TOKEN_SLASH_EQUAL?DIAMOND_TOKEN_SLASH:
            DIAMOND_TOKEN_PERCENT;
        const uint16_t right=parse_expression(compiler);
        const uint16_t result=compile_binary_op(compiler,plain_op,left,right);
        emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,result,3);
        return result;
    }
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_GET,destination,receiver,index,3);
    set_index_provenance(compiler,destination,receiver);
    publish_indexed_result_type(compiler,destination,receiver);
    return destination;
}

bool consume_block_start(Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_NEWLINE) {
        fail(compiler, compiler->current.span, "expected newline before block body");
        return false;
    }
    skip_newlines(compiler);
    return true;
}

bool consume_conditional_start(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_THEN)
        return consume_block_start(compiler);
    advance_token(compiler);
    skip_newlines(compiler);
    return true;
}

static bool consume_loop_start(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_DO)
        return consume_block_start(compiler);
    advance_token(compiler);
    skip_newlines(compiler);
    return true;
}

uint16_t parse_if(Compiler *compiler,bool inverted) {
    compiler->narrowing=(Narrowing){};
    const uint16_t condition = parse_expression(compiler);
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    if (!consume_conditional_start(compiler)) return 0;

    uint16_t branch_condition=condition;
    if(inverted) {
        branch_condition=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NOT,branch_condition,condition,0,2);
    }
    const size_t false_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, branch_condition);
    const uint16_t destination = allocate_register(compiler);
    const size_t flow_reg_count=compiler->next_register;
    const size_t flow_local_count=compiler->local_count;
    uint32_t before_alias[DIAMOND_MAX_LOCALS];uint32_t then_alias[DIAMOND_MAX_LOCALS];
    /* A local first bound *inside* the then-branch (no binding before the
     * if at all) sits outside flow_local_count, so none of the loops below
     * that snapshot/restore/merge across it would otherwise touch it -- the same gap `known_types` already has.
     * Bridged here the same way parse_if already models a missing `else`
     * for the if-expression's own result: the branch that doesn't bind it
     * contributes Nil (confirmed against the VM: an if-only-assigned local
     * really does read back as nil down the untaken path), so `if cond
     * then x = 1 end; x` infers `Int | Nil` instead of silently keeping
     * whatever the last-compiled branch happened to leave in `x`'s
     * register. Bounded by DIAMOND_MAX_LOCALS (64) like before_alias/
     * then_alias above, so no malloc dance needed. */
    uint16_t then_new_types[DIAMOND_MAX_LOCALS];int32_t then_new_sets[DIAMOND_MAX_LOCALS];
    int32_t then_new_tooling[DIAMOND_MAX_LOCALS];
    uint32_t then_new_alias[DIAMOND_MAX_LOCALS];
    /* Inline arrays cover the overwhelming majority of if/elsif sites (a
     * function rarely has more than 256 registers live before one) at no
     * heap cost; only a flow_reg_count beyond that -- reachable in real
     * programs, since allocate_register never recycles a slot within one
     * function body -- falls back to malloc. Mirrors run_chunk's own
     * inline-then-heap register-array fallback (vm.c,
     * DIAMOND_INLINE_REGISTER_COUNT) rather than widening these to a
     * fixed DIAMOND_REGISTER_COUNT, which would cost every ordinary
     * if/elsif four ~12KB stack arrays whether it needs them or not.
     * Found via ASan: the previous fixed-256 arrays, indexed up to
     * flow_reg_count with no bounds check at all, were a confirmed
     * stack-buffer-overflow once a function had allocated more than 256
     * registers before reaching an if/unless -- not just theoretical,
     * reproduced with a 260-line register-churning program. */
    uint16_t inline_before_types[256];int32_t inline_before_sets[256];
    uint16_t inline_then_types[256];int32_t inline_then_sets[256];
    int32_t inline_before_tooling[256],inline_then_tooling[256];
    uint16_t *before_types=inline_before_types,*then_types=inline_then_types;
    int32_t *before_sets=inline_before_sets,*then_sets=inline_then_sets;
    int32_t *before_tooling=inline_before_tooling,*then_tooling=inline_then_tooling;
    uint16_t *heap_types=nullptr;int32_t *heap_sets=nullptr,*heap_tooling=nullptr;
    if(flow_reg_count>256) {
        heap_types=malloc(flow_reg_count*2*sizeof(uint16_t));
        heap_sets=malloc(flow_reg_count*2*sizeof(int32_t));
        heap_tooling=malloc(flow_reg_count*2*sizeof(int32_t));
        if(heap_types==nullptr||heap_sets==nullptr||heap_tooling==nullptr) {
            fail(compiler,compiler->previous.span,
                 "out of memory compiling if expression");
            free(heap_types);free(heap_sets);free(heap_tooling);
            return destination;
        }
        before_types=heap_types;then_types=heap_types+flow_reg_count;
        before_sets=heap_sets;then_sets=heap_sets+flow_reg_count;
        before_tooling=heap_tooling;then_tooling=heap_tooling+flow_reg_count;
    }
    for(size_t index=0;index<flow_reg_count;index++) {
        before_types[index]=compiler->known_types[index];
        before_sets[index]=compiler->known_type_sets[index];
        before_tooling[index]=compiler->tooling_type_sets[index];
    }
    for(size_t index=0;index<flow_local_count;index++)
        before_alias[index]=compiler->locals[index].alias_identity;
    if(narrowing.valid)
        apply_narrowing_facts(compiler,
            inverted?narrowing.when_false:narrowing.when_true,
            inverted?narrowing.when_false_count:narrowing.when_true_count);
    const uint16_t then_result = compile_sequence(compiler);
    const uint16_t then_type=compiler->known_types[then_result];
    const int32_t then_set=compiler->known_type_sets[then_result];
    const int32_t then_result_tooling=compiler->tooling_type_sets[then_result];
    for(size_t index=0;index<flow_reg_count;index++) {
        then_types[index]=compiler->known_types[index];
        then_sets[index]=compiler->known_type_sets[index];
        then_tooling[index]=compiler->tooling_type_sets[index];
    }
    for(size_t index=0;index<flow_local_count;index++)
        then_alias[index]=compiler->locals[index].alias_identity;
    const size_t then_local_count=compiler->local_count;
    for(size_t index=flow_local_count;
        index<then_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const uint16_t reg=compiler->locals[index].reg;
        then_new_types[index-flow_local_count]=compiler->known_types[reg];
        then_new_sets[index-flow_local_count]=compiler->known_type_sets[reg];
        then_new_tooling[index-flow_local_count]=compiler->tooling_type_sets[reg];
        then_new_alias[index-flow_local_count]=compiler->locals[index].alias_identity;
        compiler->known_types[reg]=DIAMOND_TYPE_NIL;
        compiler->known_type_sets[reg]=-1;
        compiler->tooling_type_sets[reg]=-1;
        compiler->locals[index].alias_identity=0;
    }
    emit_instruction(compiler, DIAMOND_OP_MOVE, destination, then_result, 0, 2);
    const size_t end_jump = emit_jump(compiler, DIAMOND_OP_JUMP, 0);
    patch_jump(compiler, false_jump, compiler->function->code_count);

    for(size_t index=0;index<flow_reg_count;index++) {
        compiler->known_types[index]=before_types[index];
        compiler->known_type_sets[index]=before_sets[index];
        compiler->tooling_type_sets[index]=before_tooling[index];
    }
    for(size_t index=0;index<flow_local_count;index++)
        compiler->locals[index].alias_identity=before_alias[index];
    if(narrowing.valid)
        apply_narrowing_facts(compiler,
            inverted?narrowing.when_true:narrowing.when_false,
            inverted?narrowing.when_true_count:narrowing.when_false_count);

    uint16_t false_result_type=DIAMOND_TYPE_NIL;int32_t false_result_set=-1;
    int32_t false_result_tooling=-1;
    bool end_consumed=false;
    if (compiler->current.kind == DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE)skip_newlines(compiler);
        const uint16_t else_result = compile_sequence(compiler);
        const uint16_t else_type=compiler->known_types[else_result];
        const int32_t else_set=compiler->known_type_sets[else_result];
        false_result_tooling=compiler->tooling_type_sets[else_result];
        emit_instruction(compiler, DIAMOND_OP_MOVE, destination, else_result, 0, 2);
        false_result_type=else_type;false_result_set=else_set;
    } else if(compiler->current.kind==DIAMOND_TOKEN_ELSIF) {
        advance_token(compiler);
        const uint16_t else_result=parse_if(compiler,false);
        const uint16_t else_type=compiler->known_types[else_result];
        const int32_t else_set=compiler->known_type_sets[else_result];
        false_result_tooling=compiler->tooling_type_sets[else_result];
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,else_result,0,2);
        false_result_type=else_type;false_result_set=else_set;
        end_consumed=true;
    } else {
        emit_instruction(compiler, DIAMOND_OP_NIL, destination, 0, 0, 1);
    }

    for(size_t index=0;index<flow_reg_count;index++) {
        const uint16_t false_type=compiler->known_types[index];
        const int32_t false_set=compiler->known_type_sets[index];
        merge_flow_types(compiler,then_types[index],then_sets[index],
            false_type,false_set,&compiler->known_types[index],
            &compiler->known_type_sets[index]);
        const int32_t false_tooling=compiler->tooling_type_sets[index];
        compiler->tooling_type_sets[index]=merge_tooling_type_sets(compiler,
            then_tooling[index],false_tooling);
        if(register_is_local(compiler,(uint16_t)index))
            record_scope_type_fact(compiler,(uint16_t)index,
                compiler->current.span.start);
    }
    for(size_t index=0;index<flow_local_count;index++)
        compiler->locals[index].alias_identity=merge_alias_identity(
            then_alias[index],compiler->locals[index].alias_identity);
    /* Locals bound inside either branch but not before the if: a
     * then-branch-new local (index<then_local_count) already has its real
     * then-side snapshot in then_new_types/then_new_sets/then_new_alias
     * (captured before this local got reset to Nil for the false-branch
     * compile above); an else/elsif-branch-new local
     * (index>=then_local_count) never existed on the then-side at all, so
     * that side is modeled as Nil directly, the same "missing arm is Nil"
     * treatment the if-expression's own missing-else result already gets. */
    const size_t final_local_count=compiler->local_count;
    for(size_t index=flow_local_count;
        index<final_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const uint16_t reg=compiler->locals[index].reg;
        const uint16_t then_side_type=index<then_local_count?
            then_new_types[index-flow_local_count]:DIAMOND_TYPE_NIL;
        const int32_t then_side_set=index<then_local_count?
            then_new_sets[index-flow_local_count]:-1;
        const int32_t then_side_tooling=index<then_local_count?
            then_new_tooling[index-flow_local_count]:-1;
        const uint32_t then_side_alias=index<then_local_count?
            then_new_alias[index-flow_local_count]:0;
        const uint16_t false_type=compiler->known_types[reg];
        const int32_t false_set=compiler->known_type_sets[reg];
        merge_flow_types(compiler,then_side_type,then_side_set,
            false_type,false_set,&compiler->known_types[reg],
            &compiler->known_type_sets[reg]);
        const int32_t false_tooling=compiler->tooling_type_sets[reg];
        compiler->tooling_type_sets[reg]=merge_tooling_type_sets(compiler,
            then_side_tooling,false_tooling);
        compiler->locals[index].alias_identity=
            merge_alias_identity(then_side_alias,compiler->locals[index].alias_identity);
        record_scope_type_fact(compiler,reg,compiler->current.span.start);
    }
    free(heap_types);free(heap_sets);free(heap_tooling);
    merge_flow_types(compiler,then_type,then_set,false_result_type,
        false_result_set,&compiler->known_types[destination],
        &compiler->known_type_sets[destination]);
    compiler->tooling_type_sets[destination]=merge_tooling_type_sets(compiler,
        then_result_tooling,false_result_tooling);

    if (!end_consumed&&compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after if expression");
        return destination;
    }
    if(!end_consumed)advance_token(compiler);
    patch_jump(compiler, end_jump, compiler->function->code_count);
    return destination;
}

/* Cheap lookahead: does a def/closure declaration, or a `do...end` block
 * argument (`.each() do |x| ... end` and friends -- itself just another
 * closure, captures and all), appear anywhere within the loop body about
 * to be compiled -- from the current parse position through *this*
 * loop's own matching `end`, at any nesting depth inside it (any of
 * these nested inside an if/case/etc. within the loop still counts,
 * since enclosing_locals/capture threading already works transitively
 * through those) -- without scanning past it into whatever follows. A
 * throwaway DiamondLexer copy, the same zero-cost snapshot idiom
 * postfix_modifier_ahead/others already use for lookahead; never
 * advances the real parse position. Tracks end-terminated block nesting
 * via the complete set of opener keywords (confirmed against every
 * compiler function that consumes a trailing DIAMOND_TOKEN_END):
 * if/unless/while/until/loop/case/begin/class/module/interface/def/
 * closure. `then` is decoration, not an opener (consume_conditional_
 * start); else/elsif/when/rescue/ensure are mid-block separators the
 * *same* opener already accounts for, not new openers of their own.
 * Deliberately doesn't special-case the endless `def foo() = ...`/
 * `closure foo() = ...` form (no matching `end` at all): it returns
 * true the instant it sees the def/closure/do token, before it would
 * ever need to know whether one exists.
 *
 * `do` is treated the same way even though, unlike def/closure, it can
 * also be this very loop's own start decorator (`while cond do ... end`,
 * consume_loop_start) rather than a nested block -- once seen it's
 * simplest to treat it as a potential capture site regardless of which
 * one it is: a false positive here only costs some unnecessary (but
 * always correct) boxing of the loop's own locals, whereas missing a
 * real `do` block here is the actual bug this function exists to avoid
 * (a pre-existing outer local read by the loop's own condition, then
 * captured by a `do` block later in the body, needs that condition read
 * to already be box-aware -- see mark_locals_captured's own comment;
 * confirmed via a real spurious runtime "type error" a growing-Array
 * BFS loop hit, `ids.length()` reading a stale un-boxed register after
 * `ids` was boxed for a `children.each() do |child| ids.push(...) end`
 * block later in the same loop body). */
static bool loop_body_may_capture(const Compiler *compiler) {
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=compiler->current;
    size_t depth=0;
    while(token.kind!=DIAMOND_TOKEN_EOF) {
        if(token.kind==DIAMOND_TOKEN_DEF||token.kind==DIAMOND_TOKEN_CLOSURE||
           token.kind==DIAMOND_TOKEN_DO)return true;
        if(token.kind==DIAMOND_TOKEN_IF||token.kind==DIAMOND_TOKEN_UNLESS||
           token.kind==DIAMOND_TOKEN_WHILE||token.kind==DIAMOND_TOKEN_UNTIL||
           token.kind==DIAMOND_TOKEN_LOOP||token.kind==DIAMOND_TOKEN_CASE||
           token.kind==DIAMOND_TOKEN_BEGIN||token.kind==DIAMOND_TOKEN_CLASS||
           token.kind==DIAMOND_TOKEN_MODULE||token.kind==DIAMOND_TOKEN_INTERFACE) {
            depth++;
        } else if(token.kind==DIAMOND_TOKEN_END) {
            if(depth==0)return false;
            depth--;
        }
        token=diamond_lexer_next(&lookahead);
    }
    return false;
}

/* Marks every currently-visible local in the enclosing function
 * captured, so its subsequent reads/writes -- including ones textually
 * before wherever the def/closure that triggered this actually sits --
 * go through the existing box-aware codegen from here on. Called once,
 * at a capturing loop's own entry; see loop_captures_pending's own
 * comment for why this needs to happen before the condition/body
 * compiles rather than lazily at the real capture site. */
static void mark_locals_captured(Compiler *compiler) {
    for(size_t index=0;index<compiler->local_count;index++)
        compiler->locals[index].captured=true;
}

/* The statement-level twin of loop_body_may_capture: does the statement
 * starting at the current token contain a `do` block (or a def/closure)
 * before its first line ends? Brackets are tracked so a call whose
 * arguments span lines still counts as one statement. Stops at a newline
 * (or `;`, which lexes as one) outside any bracket.
 *
 * Why this matters: reading a not-yet-captured local yields that local's
 * own register, and the value is only consumed later in the statement.
 * A block compiled in between boxes that register in place, so the
 * consumer would find the Cell instead of the value -- `[x, list.map() do
 * |v| v end]` built [#<Cell>, ...]. Marking the locals captured before the
 * statement compiles makes every read in it go through GET_CELL instead.
 * It costs nothing extra in practice: the block captures every visible
 * local anyway, so they would all be boxed moments later. */
static bool statement_may_capture(const Compiler *compiler) {
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=compiler->current;
    size_t depth=0;
    while(token.kind!=DIAMOND_TOKEN_EOF) {
        switch(token.kind) {
        case DIAMOND_TOKEN_DO:
        case DIAMOND_TOKEN_DEF:
        case DIAMOND_TOKEN_CLOSURE:
            return true;
        case DIAMOND_TOKEN_LEFT_PAREN:
        case DIAMOND_TOKEN_LEFT_BRACKET:
        case DIAMOND_TOKEN_LEFT_BRACE:
            depth++;break;
        case DIAMOND_TOKEN_RIGHT_PAREN:
        case DIAMOND_TOKEN_RIGHT_BRACKET:
        case DIAMOND_TOKEN_RIGHT_BRACE:
            if(depth==0)return false;
            depth--;break;
        case DIAMOND_TOKEN_NEWLINE:
            if(depth==0)return false;
            break;
        default:
            break;
        }
        token=diamond_lexer_next(&lookahead);
    }
    return false;
}

static uint16_t parse_while_core(Compiler *compiler,bool inverted,const PostfixLoop *postfix) {
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    const size_t loop_start = compiler->function->code_count;
    const bool outer_loop_captures_pending=compiler->loop_captures_pending;
    /* A postfix loop's statement was already checked for blocks by compile_sequence. */
    if(postfix==nullptr&&!compiler->loop_captures_pending&&loop_body_may_capture(compiler)) {
        compiler->loop_captures_pending=true;
        mark_locals_captured(compiler);
    }
    DiamondLexer body_lexer={},end_lexer={};
    DiamondToken body_current={},body_previous={},end_current={},end_previous={};
    if(postfix!=nullptr) {
        body_lexer=compiler->lexer;body_current=compiler->current;
        body_previous=compiler->previous;
        compiler->lexer=postfix->after_modifier;
        compiler->current=diamond_lexer_next(&compiler->lexer);
    }
    const uint16_t condition = parse_expression(compiler);
    if(postfix!=nullptr) {
        end_lexer=compiler->lexer;end_current=compiler->current;
        end_previous=compiler->previous;
        compiler->lexer=body_lexer;compiler->current=body_current;
        compiler->previous=body_previous;
    } else if (!consume_loop_start(compiler)) {
        compiler->loop_captures_pending=outer_loop_captures_pending;
        return 0;
    }
    uint16_t branch_condition=condition;
    if(inverted) {
        branch_condition=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NOT,branch_condition,condition,0,2);
    }
    const size_t exit_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, branch_condition);
    const size_t flow_reg_count=compiler->next_register;
    const size_t flow_local_count=compiler->local_count;
    uint16_t *exit_types=malloc(flow_reg_count*sizeof(uint16_t));
    int32_t *exit_sets=malloc(flow_reg_count*sizeof(int32_t));
    int32_t *exit_tooling_sets=malloc(flow_reg_count*sizeof(int32_t));
    if(exit_types==nullptr||exit_sets==nullptr||exit_tooling_sets==nullptr) {
        free(exit_types);free(exit_sets);free(exit_tooling_sets);
        fail(compiler,compiler->previous.span,"out of memory compiling loop flow");
        compiler->loop_captures_pending=outer_loop_captures_pending;
        return destination;
    }
    LoopContext loop={
        .previous=compiler->current_loop,
        .begin_depth=compiler->begin_depth,
        .continue_target=loop_start,
        .redo_target=compiler->function->code_count,
        .result_register=destination,
        .flow_reg_count=flow_reg_count,
        .exit_types=exit_types,
        .exit_sets=exit_sets,
        .exit_tooling_sets=exit_tooling_sets,
        .flow_local_count=flow_local_count,
    };
    /* The condition may be false before the first iteration, so the entry
     * state and Nil result are always one real exit path. */
    merge_loop_exit(compiler,&loop,DIAMOND_TYPE_NIL,-1,-1);
    compiler->current_loop=&loop;
    if(postfix!=nullptr) {
        (void)compile_expression_statement(compiler,false);
        if(!compiler->failed&&compiler->current.kind!=
               (inverted?DIAMOND_TOKEN_UNTIL:DIAMOND_TOKEN_WHILE))
            fail(compiler,compiler->current.span,"expected the loop modifier after the statement");
        compiler->lexer=end_lexer;compiler->current=end_current;
        compiler->previous=end_previous;
    } else {
        (void)compile_sequence(compiler);
    }
    compiler->current_loop=loop.previous;
    /* A completed body reaches the condition again and may then exit. One
     * conservative source-level join is sufficient for advisory metadata. */
    merge_loop_exit(compiler,&loop,DIAMOND_TYPE_NIL,-1,-1);
    compiler->loop_captures_pending=outer_loop_captures_pending;
    emit_absolute_jump(compiler, loop_start);
    patch_jump(compiler, exit_jump, compiler->function->code_count);
    for(size_t index=0;index<loop.break_count;index++)
        patch_jump(compiler,loop.breaks[index],compiler->function->code_count);

    if (postfix==nullptr&&compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after while expression");
        free(exit_types);free(exit_sets);free(exit_tooling_sets);
        return 0;
    }
    const size_t join_offset=compiler->current.span.start;
    if(postfix==nullptr)advance_token(compiler);
    /* A break-free loop may have no reachable expression result at all
     * (`while true` with only return/raise exits). Without constant-condition
     * reachability analysis, keep that result unknown instead of claiming Nil
     * and rejecting an otherwise valid enclosing return annotation. */
    if(loop.break_count==0) {
        loop.result_type=TYPE_UNKNOWN;loop.result_set=-1;
        loop.result_tooling_set=-1;
    }
    finish_loop_flow(compiler,&loop,join_offset);
    free(exit_types);free(exit_sets);free(exit_tooling_sets);
    return destination;
}

static uint16_t parse_while(Compiler *compiler,bool inverted) {
    return parse_while_core(compiler,inverted,nullptr);
}

static uint16_t parse_postfix_while(Compiler *compiler,bool inverted,DiamondLexer after_modifier) {
    const PostfixLoop postfix={.after_modifier=after_modifier};
    return parse_while_core(compiler,inverted,&postfix);
}

static uint16_t parse_loop(Compiler *compiler) {
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    if(!consume_loop_start(compiler))return destination;
    const bool outer_loop_captures_pending=compiler->loop_captures_pending;
    if(!compiler->loop_captures_pending&&loop_body_may_capture(compiler)) {
        compiler->loop_captures_pending=true;
        mark_locals_captured(compiler);
    }
    const size_t body_start=compiler->function->code_count;
    const size_t flow_reg_count=compiler->next_register;
    const size_t flow_local_count=compiler->local_count;
    uint16_t *exit_types=malloc(flow_reg_count*sizeof(uint16_t));
    int32_t *exit_sets=malloc(flow_reg_count*sizeof(int32_t));
    int32_t *exit_tooling_sets=malloc(flow_reg_count*sizeof(int32_t));
    if(exit_types==nullptr||exit_sets==nullptr||exit_tooling_sets==nullptr) {
        free(exit_types);free(exit_sets);free(exit_tooling_sets);
        fail(compiler,compiler->previous.span,"out of memory compiling loop flow");
        compiler->loop_captures_pending=outer_loop_captures_pending;
        return destination;
    }
    LoopContext loop={.previous=compiler->current_loop,
        .begin_depth=compiler->begin_depth,
        .continue_target=body_start,.redo_target=body_start,
        .result_register=destination,.flow_reg_count=flow_reg_count,
        .exit_types=exit_types,.exit_sets=exit_sets,
        .exit_tooling_sets=exit_tooling_sets,
        .flow_local_count=flow_local_count};
    compiler->current_loop=&loop;
    (void)compile_sequence(compiler);
    compiler->current_loop=loop.previous;
    compiler->loop_captures_pending=outer_loop_captures_pending;
    emit_absolute_jump(compiler,body_start);
    for(size_t index=0;index<loop.break_count;index++)
        patch_jump(compiler,loop.breaks[index],compiler->function->code_count);
    if(compiler->current.kind!=DIAMOND_TOKEN_END) {
        fail(compiler,compiler->current.span,"expected 'end' after loop");
        free(exit_types);free(exit_sets);free(exit_tooling_sets);
        return destination;
    }
    const size_t join_offset=compiler->current.span.start;
    advance_token(compiler);
    finish_loop_flow(compiler,&loop,join_offset);
    free(exit_types);free(exit_sets);free(exit_tooling_sets);return destination;
}

/* /pattern/imx -- the same as Regexp.new("pattern", options), compiled each time the literal is
 * evaluated. The pattern is kept as written, so \d reaches the regex engine intact. */
static uint16_t parse_regex_literal(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    size_t close=span.start+span.length;
    int64_t options=0;
    while(compiler->source[close-1]!='/') {
        const char flag=compiler->source[--close];
        options|=flag=='i'?1:flag=='m'?2:4;
    }
    const size_t pattern_start=span.start+1,pattern_length=close-1-pattern_start;
    for(size_t index=0;index+1<pattern_length;index++) {
        if(compiler->source[pattern_start+index]=='\\') {index++;continue;}
        if(compiler->source[pattern_start+index]=='#'&&
           compiler->source[pattern_start+index+1]=='{') {
            fail(compiler,span,"regex literals do not interpolate; build the pattern with Regexp.new");
            return 0;
        }
    }
    const uint16_t pattern_register=allocate_register(compiler);
    const uint16_t pattern=add_string_range_mode(compiler,pattern_start,pattern_length,span,true);
    emit_instruction(compiler,DIAMOND_OP_STRING,pattern_register,pattern,0,2);
    const uint16_t options_register=allocate_register(compiler);
    const uint16_t options_constant=add_constant(compiler,DIAMOND_INT(options));
    emit_instruction(compiler,DIAMOND_OP_CONSTANT,options_register,options_constant,0,2);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_REGEXP_NEW);
    emit_register(compiler,dest);
    emit_register(compiler,pattern_register);
    emit_register(compiler,options_register);
    return dest;
}

uint16_t parse_prefix(Compiler *compiler) {
    advance_token(compiler);
    switch (compiler->previous.kind) {
        case DIAMOND_TOKEN_INTEGER:
            return parse_integer(compiler);
        case DIAMOND_TOKEN_FLOAT:
            return parse_float(compiler);
        case DIAMOND_TOKEN_STRING:
            return parse_string(compiler);
        case DIAMOND_TOKEN_REGEX:
            return parse_regex_literal(compiler);
        case DIAMOND_TOKEN_SYMBOL:
            return parse_symbol(compiler);
        case DIAMOND_TOKEN_TRUE:
        case DIAMOND_TOKEN_FALSE:
        case DIAMOND_TOKEN_NIL:
            return parse_literal(compiler);
        case DIAMOND_TOKEN_IDENTIFIER:
            return parse_name(compiler);
        case DIAMOND_TOKEN_INSTANCE_VARIABLE: {
            const uint16_t destination = allocate_register(compiler);
            if(compiler->current_module>=0&&compiler->current_class<0) {
                const uint16_t field=module_field_name(
                    compiler,compiler->previous.span);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,destination,0,
                                 field,3);
            } else {
                const int field=field_index(compiler,compiler->previous.span,true);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,destination,0,
                                 (uint8_t)field,3);
                /* The discovery pass has already seen every write in every
                 * reopening of this class. Its final field fact is therefore
                 * safe to use here regardless of where this method appears
                 * in source. This feeds the ordinary declared-return path in
                 * publish_instance_return_type, allowing @field.make().use()
                 * to reach Phase 12's existing JIT proof. */
                if(!compiler->discovery_pass&&field>=0&&
                   compiler->current_class>=0) {
                    const DiamondClass *class=compiler->program->classes[
                        (size_t)compiler->current_class];
                    if(class->discovered_field_type_status[(size_t)field]==1)
                        compiler->known_types[destination]=(uint8_t)(
                            DIAMOND_TYPE_CLASS_BASE+
                            class->discovered_field_known_class[(size_t)field]);
                }
            }
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
                return parse_closure_call_arguments(compiler,destination);
            return destination;
        }
        case DIAMOND_TOKEN_CLASS_VARIABLE: {
            const uint16_t destination = allocate_register(compiler);
            const int slot = class_variable_index(compiler,compiler->previous.span,true);
            emit_instruction(compiler,DIAMOND_OP_GET_CVAR,destination,
                             (uint16_t)class_variable_owner(compiler,compiler->previous.span),(uint8_t)slot,3);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
                return parse_closure_call_arguments(compiler,destination);
            return destination;
        }
        case DIAMOND_TOKEN_SELF:
            if (!compiler->in_method) {
                fail(compiler, compiler->previous.span, "'self' used outside a method");
                return 0;
            }
            /* self.foo(...) inside a class-owned singleton method's own
             * body needs real dynamic dispatch (see
             * parse_self_class_method_call's own comment) -- everywhere
             * else (an ordinary instance method, or bare `self` with no
             * following call inside a singleton method), self stays an
             * ordinary register-0 value, unchanged. A `closure name()
             * ... end` body reuses this exact same code unmodified: its
             * own register 0 already holds a materialized copy of the
             * captured self (see compile_definition's captures_self
             * handling), and in_singleton_method is set true there too
             * when appropriate, so this check and the plain `return 0`
             * both already do the right thing with no capture-specific
             * branch needed here at all. */
            if(compiler->in_singleton_method&&compiler->current_class>=0&&
               compiler->current.kind==DIAMOND_TOKEN_DOT) {
                return parse_self_class_method_call(compiler);
            }
            return 0;
        case DIAMOND_TOKEN_SUPER:
            return parse_super(compiler);
        case DIAMOND_TOKEN_LEFT_PAREN:
            return parse_grouping(compiler);
        case DIAMOND_TOKEN_LEFT_BRACKET:
            return parse_array(compiler);
        case DIAMOND_TOKEN_LEFT_BRACE:
            return parse_hash(compiler);
        case DIAMOND_TOKEN_MINUS: {
            const uint16_t operand = parse_precedence(compiler, PREC_PREFIX);
            const uint16_t destination = allocate_register(compiler);
            emit_instruction(compiler, DIAMOND_OP_NEGATE, destination,
                             operand, 0, 2);
            if(compiler->known_types[operand]==DIAMOND_TYPE_INT)
                compiler->known_types[destination]=DIAMOND_TYPE_INT;
            else if(compiler->known_types[operand]==DIAMOND_TYPE_FLOAT)
                compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
            return destination;
        }
        case DIAMOND_TOKEN_BANG:
        case DIAMOND_TOKEN_NOT: {
            const uint16_t operand=parse_precedence(compiler,PREC_PREFIX);
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,operand,0,2);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            return destination;
        }
        case DIAMOND_TOKEN_AMPERSAND:
            /* A block is already represented as an ordinary Callable value.
             * `&block` therefore marks forwarding intent syntactically while
             * preserving the closure register unchanged. */
            return parse_precedence(compiler,PREC_PREFIX);
        case DIAMOND_TOKEN_CASE:
            return parse_case(compiler);
        case DIAMOND_TOKEN_IF:
            return parse_if(compiler,false);
        case DIAMOND_TOKEN_UNLESS:
            return parse_if(compiler,true);
        case DIAMOND_TOKEN_WHILE:
            return parse_while(compiler,false);
        case DIAMOND_TOKEN_UNTIL:
            return parse_while(compiler,true);
        case DIAMOND_TOKEN_LOOP:
            return parse_loop(compiler);
        case DIAMOND_TOKEN_BEGIN:
            return compile_begin(compiler);
        case DIAMOND_TOKEN_YIELD:
            return compile_yield(compiler);
        default:
            fail(compiler, compiler->previous.span, "expected expression");
            return 0;
    }
}

static DiamondOpCode binary_opcode(DiamondTokenKind operator) {
    switch (operator) {
        case DIAMOND_TOKEN_PLUS: return DIAMOND_OP_ADD;
        case DIAMOND_TOKEN_MINUS: return DIAMOND_OP_SUBTRACT;
        case DIAMOND_TOKEN_STAR: return DIAMOND_OP_MULTIPLY;
        case DIAMOND_TOKEN_SLASH: return DIAMOND_OP_DIVIDE;
        case DIAMOND_TOKEN_EQUAL_EQUAL: return DIAMOND_OP_EQUAL;
        case DIAMOND_TOKEN_BANG_EQUAL: return DIAMOND_OP_NOT_EQUAL;
        case DIAMOND_TOKEN_LESS: return DIAMOND_OP_LESS;
        case DIAMOND_TOKEN_LESS_EQUAL: return DIAMOND_OP_LESS_EQUAL;
        case DIAMOND_TOKEN_GREATER: return DIAMOND_OP_GREATER;
        case DIAMOND_TOKEN_GREATER_EQUAL: return DIAMOND_OP_GREATER_EQUAL;
        case DIAMOND_TOKEN_LESS_LESS: return DIAMOND_OP_SHIFT_LEFT;
        case DIAMOND_TOKEN_GREATER_GREATER: return DIAMOND_OP_SHIFT_RIGHT;
        case DIAMOND_TOKEN_AMPERSAND: return DIAMOND_OP_BITWISE_AND;
        case DIAMOND_TOKEN_PIPE: return DIAMOND_OP_BITWISE_OR;
        case DIAMOND_TOKEN_CARET: return DIAMOND_OP_BITWISE_XOR;
        case DIAMOND_TOKEN_PERCENT: return DIAMOND_OP_MODULO;
        case DIAMOND_TOKEN_SPACESHIP: return DIAMOND_OP_COMPARE;
        default: return DIAMOND_OP_ADD;
    }
}

/* The generic (non-short-circuit, non-`is`) binary-operator codegen --
 * factored out of parse_precedence's own infix loop so
 * compile_compound_assignment (below) can reuse it verbatim for `+=`/
 * `-=`/`*=`/`/=`/`%=` instead of re-parsing the desugared `x = x + y`
 * form or duplicating the Int-fast-path/type-narrowing logic. Takes
 * `left`/`right` as already-evaluated registers -- this is deliberately
 * NOT `right = parse_precedence(...)` itself, since a compound
 * assignment's RHS is parsed as a full expression (`parse_expression`),
 * not at `operator_precedence + 1` the way a chained infix operand is. */
uint16_t compile_binary_op(Compiler *compiler, DiamondTokenKind operator,
                                   uint16_t left, uint16_t right) {
    const uint16_t destination = allocate_register(compiler);
    DiamondOpCode opcode=binary_opcode(operator);
    if(operator==DIAMOND_TOKEN_PLUS&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_ADD_INT;
    if(operator==DIAMOND_TOKEN_MINUS&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_SUBTRACT_INT;
    if(operator==DIAMOND_TOKEN_STAR&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_MULTIPLY_INT;
    if(operator==DIAMOND_TOKEN_SLASH&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_DIVIDE_INT;
    if(operator==DIAMOND_TOKEN_EQUAL_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_EQUAL_INT;
    if(operator==DIAMOND_TOKEN_BANG_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_NOT_EQUAL_INT;
    if(operator==DIAMOND_TOKEN_LESS&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_LESS_INT;
    if(operator==DIAMOND_TOKEN_LESS_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_LESS_EQUAL_INT;
    if(operator==DIAMOND_TOKEN_GREATER&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_GREATER_INT;
    if(operator==DIAMOND_TOKEN_GREATER_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_GREATER_EQUAL_INT;
    emit_instruction(compiler, opcode, destination,
                     left, right, 3);
    if(operator==DIAMOND_TOKEN_EQUAL_EQUAL || operator==DIAMOND_TOKEN_BANG_EQUAL ||
       operator==DIAMOND_TOKEN_LESS || operator==DIAMOND_TOKEN_LESS_EQUAL ||
       operator==DIAMOND_TOKEN_GREATER || operator==DIAMOND_TOKEN_GREATER_EQUAL) {
        compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
        if(operator==DIAMOND_TOKEN_EQUAL_EQUAL||operator==DIAMOND_TOKEN_BANG_EQUAL) {
            uint16_t narrowed=left,nil_value=right;
            if(compiler->known_types[left]==DIAMOND_TYPE_NIL) {
                narrowed=right;nil_value=left;
            }
            if(compiler->known_types[nil_value]==DIAMOND_TYPE_NIL&&
               compiler->known_type_sets[narrowed]>=0) {
                int32_t non_nil=-1,nil_only=-1;
                if(split_nil_type_set(compiler,
                   (uint16_t)compiler->known_type_sets[narrowed],
                   &non_nil,&nil_only)) {
                    const int32_t when_true=operator==DIAMOND_TOKEN_BANG_EQUAL
                        ?non_nil:nil_only;
                    const int32_t when_false=operator==DIAMOND_TOKEN_BANG_EQUAL
                        ?nil_only:non_nil;
                    compiler->narrowing=(Narrowing){.valid=true,
                        .condition=destination,
                        .when_true={{.reg=narrowed,.type_set=when_true}},
                        .when_true_count=1,
                        .when_false={{.reg=narrowed,.type_set=when_false}},
                        .when_false_count=1};
                }
            }
        }
    } else if(compiler->known_types[left]==DIAMOND_TYPE_INT &&
              compiler->known_types[right]==DIAMOND_TYPE_INT) {
        compiler->known_types[destination]=DIAMOND_TYPE_INT;
    } else if(operator!=DIAMOND_TOKEN_SPACESHIP &&
              (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[left]==DIAMOND_TYPE_INT) &&
              (compiler->known_types[right]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[right]==DIAMOND_TYPE_INT) &&
              (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[right]==DIAMOND_TYPE_FLOAT)) {
        /* Mixed Int/Float statically known to auto-promote to Float --
         * excludes spaceship explicitly: unlike the arithmetic operators,
         * its own result is always Int-or-Nil, never Float, regardless of
         * operand types (the DIAMOND_TYPE_INT branch just above stays
         * correct for spaceship unmodified, since two known Ints really
         * do always produce an Int result under this design). Found and
         * fixed during this feature's own design, before any code was
         * written -- see the Comparable design doc. */
        compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
    } else if(operator==DIAMOND_TOKEN_PLUS &&
              compiler->known_types[left]==DIAMOND_TYPE_STRING &&
              compiler->known_types[right]==DIAMOND_TYPE_STRING) {
        compiler->known_types[destination]=DIAMOND_TYPE_STRING;
    }
    return destination;
}

/* A call whose do-block can `break` returns the break value instead of its
 * usual result, so none of the call's type facts hold. */
static void drop_facts_after_block_break(Compiler *compiler,uint16_t reg) {
    if(!compiler->block_break_pending)return;
    compiler->block_break_pending=false;
    compiler->known_types[reg]=TYPE_UNKNOWN;
    compiler->known_type_sets[reg]=-1;
}

/* After `..` or `...`: is this an endless range (`a[1..]`, `(1..)`, `when 5..`) with no end
 * expression? That is so when the next token closes or separates rather than starts one. */
static bool range_end_absent(const Compiler *compiler) {
    switch(compiler->current.kind) {
        case DIAMOND_TOKEN_RIGHT_BRACKET:
        case DIAMOND_TOKEN_RIGHT_PAREN:
        case DIAMOND_TOKEN_RIGHT_BRACE:
        case DIAMOND_TOKEN_COMMA:
        case DIAMOND_TOKEN_NEWLINE:
        case DIAMOND_TOKEN_EOF:
        case DIAMOND_TOKEN_THEN:
        case DIAMOND_TOKEN_END:
        case DIAMOND_TOKEN_DO:
            return true;
        default:
            return false;
    }
}

static uint16_t parse_precedence(Compiler *compiler, Precedence precedence) {
    uint16_t left = parse_prefix(compiler);
    drop_facts_after_block_break(compiler,left);
    while (!compiler->failed &&
           (compiler->current.kind == DIAMOND_TOKEN_DOT ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACKET ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN)) {
        /* `expr(...)` directly after an already-fully-parsed expression
         * (no `.method`/`[index]` in between) means "call the Callable
         * value `left` itself" -- `type.coerce_input()(value)`,
         * `(a)(b)`, `arr[0]()`, and so on. A previously-undiscovered
         * gap, not a deliberate cut: nothing in this grammar has any
         * other meaning for two adjacent expressions with no operator
         * between them, so this can only ever turn a program that used
         * to be a hard parse error ("expected newline after
         * expression", since the leftover `(...)` had nowhere to go)
         * into a working one -- it can't reinterpret anything that
         * used to compile. parse_closure_call_arguments already exists
         * for exactly this "call whatever's in this register" shape
         * (shared with a local variable or `@ivar`/`@@cvar` holding a
         * Callable, both followed by `(...)`); reusing it here just
         * extends where its receiver register is allowed to come from. */
        if(compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN) {
            left = parse_closure_call_arguments(compiler,left);
        } else {
            left = compiler->current.kind == DIAMOND_TOKEN_DOT
                ? parse_invoke(compiler,left) : parse_index(compiler,left);
        }
        drop_facts_after_block_break(compiler,left);
    }
    while (!compiler->failed && infix_precedence(compiler) >= precedence) {
        /* `base ** exponent`: a call of the `**` method, so Int/Float get
         * it from the prelude (numeric_pow) and a class can define its
         * own. The right side parses at the same precedence, making it
         * right-associative. */
        if(power_operator_ahead(compiler)) {
            const DiamondSpan operator_span={.start=compiler->current.span.start,
                .length=2,.line=compiler->current.span.line,
                .column=compiler->current.span.column};
            advance_token(compiler);advance_token(compiler);
            skip_newlines(compiler);
            const uint16_t exponent=parse_precedence(compiler,PREC_POWER);
            left=emit_invoke_call(compiler,left,operator_span,false,nullptr,0,
                                  &exponent,1);
            continue;
        }
        const DiamondTokenKind operator = compiler->current.kind;
        const Precedence operator_precedence = token_precedence(operator);
        advance_token(compiler);
        /* A newline right after a binary operator can only mean "the
         * right operand continues on the next line" -- the while
         * condition above already confirmed `operator` is a genuine
         * infix operator at or above the caller's minimum precedence,
         * and no such operator can legally end a statement, so there's
         * no ambiguity to preserve by leaving this newline for the
         * statement-separator logic elsewhere to see. Fixes `x = 1 +\n
         * 2` and `if a &&\n b` (previously "expected expression"):
         * newline-skipping had only ever existed at bracket-delimited
         * list boundaries, never inside a general binary expression. */
        skip_newlines(compiler);
        if(operator==DIAMOND_TOKEN_IS) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,"expected type after 'is'");
                return left;
            }
            const DiamondSpan tested_type_span=compiler->current.span;
            char tested_type_name[DIAMOND_MAX_FUNCTION_NAME];
            if(!consume_qualified_name(compiler,tested_type_name,
                                       sizeof tested_type_name)) {
                fail(compiler,tested_type_span,"type name is too long");
                return left;
            }
            const int tested=resolve_type_name(
                compiler,tested_type_name,tested_type_span);
            const bool tested_native=diamond_type_operand_is_native(tested);
            const uint16_t tested_type=tested_native?DIAMOND_TYPE_NATIVE:(uint16_t)tested;
            if(tested_type>=DIAMOND_TYPE_VARIABLE_BASE&&
               tested_type<DIAMOND_TYPE_INTERFACE_BASE)
                fail(compiler,tested_type_span,
                     "generic type variables cannot be used with 'is' before binding");
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,destination,left,
                             (uint16_t)tested,3);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            /* Narrowing splits by type id alone, which can't tell native
             * kinds apart; a native test doesn't narrow. */
            if(!tested_native&&compiler->known_type_sets[left]>=0) {
                int32_t matching=-1,remaining=-1;
                if(split_type_set(compiler,
                   (uint16_t)compiler->known_type_sets[left],tested_type,
                   &matching,&remaining))
                    compiler->narrowing=(Narrowing){.valid=true,
                        .condition=destination,
                        .when_true={{.reg=left,.type_set=matching}},
                        .when_true_count=1,
                        .when_false={{.reg=left,.type_set=remaining}},
                        .when_false_count=1};
            }
            left=destination;continue;
        }
        if(operator==DIAMOND_TOKEN_AND_AND||operator==DIAMOND_TOKEN_AND||
           operator==DIAMOND_TOKEN_OR_OR||operator==DIAMOND_TOKEN_OR) {
            const bool is_and=operator==DIAMOND_TOKEN_AND_AND||
                operator==DIAMOND_TOKEN_AND;
            const Narrowing left_narrowing=compiler->narrowing.condition==left
                ?compiler->narrowing:(Narrowing){};
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
            const uint16_t right=parse_precedence(
                compiler,(Precedence)(operator_precedence+1));
            const Narrowing right_narrowing=compiler->narrowing.condition==right
                ?compiler->narrowing:(Narrowing){};
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
            patch_jump(compiler,end_jump,compiler->function->code_count);
            if(compiler->known_types[left]==compiler->known_types[right])
                compiler->known_types[destination]=compiler->known_types[left];
            /* AND: both sides must hold for the whole expression to be
             * true, so the when-true facts compose (conjunction is
             * sound). The false case of AND is a disjunction of
             * failures, not soundly reducible to independent
             * per-variable facts, so when_false stays empty rather
             * than guessed. OR is the mirror image: when_false
             * composes (both sides must have failed), when_true stays
             * empty. An empty side is always safe to concatenate --
             * it contributes nothing -- so this composes correctly
             * through chains and mixed &&/|| automatically, with no
             * special-casing for either. */
            Narrowing merged={.condition=destination};
            if(is_and) {
                append_narrowing_facts(merged.when_true,&merged.when_true_count,
                    left_narrowing.when_true,left_narrowing.when_true_count);
                append_narrowing_facts(merged.when_true,&merged.when_true_count,
                    right_narrowing.when_true,right_narrowing.when_true_count);
            } else {
                append_narrowing_facts(merged.when_false,&merged.when_false_count,
                    left_narrowing.when_false,left_narrowing.when_false_count);
                append_narrowing_facts(merged.when_false,&merged.when_false_count,
                    right_narrowing.when_false,right_narrowing.when_false_count);
            }
            merged.valid=merged.when_true_count>0||merged.when_false_count>0;
            compiler->narrowing=merged;
            left=destination;
            continue;
        }
        if(operator==DIAMOND_TOKEN_DOT_DOT||operator==DIAMOND_TOKEN_DOT_DOT_DOT) {
            const bool exclusive=operator==DIAMOND_TOKEN_DOT_DOT_DOT;
            /* `1..` with nothing after it is an endless range: the end is nil. */
            uint16_t right=0;
            if(range_end_absent(compiler)) {
                right=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_NIL,right,0,0,1);
            } else {
                right=parse_precedence(compiler,(Precedence)(operator_precedence+1));
            }
            const int class_index=find_class_name(compiler,"Range");
            if(class_index<0) {
                fail(compiler,compiler->previous.span,
                     "'Range' is not defined -- is the prelude loaded?");
                return left;
            }
            const uint16_t exclusive_reg=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_BOOL,exclusive_reg,exclusive,0,2);
            const uint16_t base=allocate_register(compiler);
            (void)allocate_register(compiler);
            (void)allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,base,left,0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+1),right,0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+2),
                             exclusive_reg,0,2);
            const uint16_t dest=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_NEW); emit_register(compiler,dest);
            emit_register(compiler,(uint16_t)class_index); emit_register(compiler,base);
            emit_byte(compiler,3);
            compiler->known_types[dest]=(uint16_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
            left=dest;continue;
        }
        const uint16_t right = parse_precedence(
            compiler, (Precedence)(operator_precedence + 1));
        left = compile_binary_op(compiler, operator, left, right);
    }
    return left;
}

/* `cond ? a : b` -- binds looser than every binary operator (including
 * `..`/`&&`/`||`), tighter than assignment, matching Ruby's own
 * precedence table. Implemented as the wrapper around parse_expression's
 * own top-level entry point (parse_precedence(PREC_RANGE) used to be
 * that entry point directly; it's now just how a ternary's condition
 * gets parsed) rather than folded into parse_precedence's infix loop --
 * unlike every operator that loop handles, the "operator" here isn't a
 * single token, and the right-hand side needs its own nested-ternary-
 * aware entry point (a true/false branch can itself contain a `?:`,
 * right-associating the same way `a ? b : c ? d : e` does in Ruby),
 * which is exactly what recursing into parse_expression itself gives
 * for free, the same way parse_if's own elsif chain recurses into
 * itself. No new DiamondOpCode: same JUMP_IF_FALSE/MOVE/JUMP shape
 * parse_if's then/else already uses, just without a full compile_
 * sequence body on either side (a ternary's branches are expressions,
 * not statement lists, so -- unlike parse_if/parse_case_branches --
 * there's no local-variable-reassignment-inside-a-branch hazard here
 * that would need a register-snapshot save/restore: an expression alone
 * can't reassign a local, only compile_assignment/compile_compound_
 * assignment can, and neither is reachable from here). */
static uint16_t parse_ternary(Compiler *compiler) {
    const uint16_t condition = parse_precedence(compiler, PREC_RANGE);
    /* Only touch compiler->narrowing once this has actually turned out to
     * be a ternary -- parse_expression (this function) is also how every
     * non-ternary condition gets parsed (parse_if's own condition, for
     * one), and those callers read compiler->narrowing themselves right
     * after calling parse_expression to pick up whatever the condition's
     * own comparison set. Resetting it here unconditionally, even on the
     * plain "no '?' follows, just return condition" path, would silently
     * erase that for every single caller -- confirmed as a real
     * regression (parse_if's own then/else type-narrowing broke,
     * de-optimizing an elidable CHECK_TYPE back on) before catching it
     * in tests/run.sh's existing diagnostic-only-on-failure assertion. */
    if(compiler->current.kind!=DIAMOND_TOKEN_QUESTION) return condition;
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    advance_token(compiler);
    skip_newlines(compiler);
    const size_t false_jump=
        emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,condition);
    const uint16_t destination=allocate_register(compiler);
    if(narrowing.valid)
        apply_narrowing_facts(compiler,narrowing.when_true,narrowing.when_true_count);
    const uint16_t true_result=parse_expression(compiler);
    const uint16_t true_type=compiler->known_types[true_result];
    const int32_t true_set=compiler->known_type_sets[true_result];
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,true_result,0,2);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,false_jump,compiler->function->code_count);
    if(narrowing.valid)
        apply_narrowing_facts(compiler,narrowing.when_false,narrowing.when_false_count);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
        fail(compiler,compiler->current.span,"expected ':' in ternary expression");
        return destination;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t false_result=parse_expression(compiler);
    const uint16_t false_type=compiler->known_types[false_result];
    const int32_t false_set=compiler->known_type_sets[false_result];
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,false_result,0,2);
    patch_jump(compiler,end_jump,compiler->function->code_count);
    merge_flow_types(compiler,true_type,true_set,false_type,false_set,
        &compiler->known_types[destination],
        &compiler->known_type_sets[destination]);
    return destination;
}

uint16_t parse_expression(Compiler *compiler) {
    return parse_ternary(compiler);
}

bool assignment_ahead(const Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER &&
        compiler->current.kind != DIAMOND_TOKEN_INSTANCE_VARIABLE &&
        compiler->current.kind != DIAMOND_TOKEN_CLASS_VARIABLE) {
        return false;
    }
    DiamondLexer lookahead = compiler->lexer;
    return diamond_lexer_next(&lookahead).kind == DIAMOND_TOKEN_EQUAL;
}

static bool compound_assignment_token(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_PLUS_EQUAL||kind==DIAMOND_TOKEN_MINUS_EQUAL||
           kind==DIAMOND_TOKEN_STAR_EQUAL||kind==DIAMOND_TOKEN_SLASH_EQUAL||
           kind==DIAMOND_TOKEN_PERCENT_EQUAL||kind==DIAMOND_TOKEN_OR_OR_EQUAL||
           kind==DIAMOND_TOKEN_AND_AND_EQUAL;
}

/* Same shape as assignment_ahead above (only a plain local/@ivar/@@cvar
 * target -- indexed (`arr[i] += 1`) compound assignment is a deliberate
 * v1 scope cut, same spirit as `<<`/`%`'s own cuts), but for `+=`/`-=`/
 * `*=`/`/=`/`%=`/`||=`/`&&=` instead of plain `=`. */
static bool compound_assignment_ahead(const Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER &&
        compiler->current.kind != DIAMOND_TOKEN_INSTANCE_VARIABLE &&
        compiler->current.kind != DIAMOND_TOKEN_CLASS_VARIABLE) {
        return false;
    }
    DiamondLexer lookahead = compiler->lexer;
    return compound_assignment_token(diamond_lexer_next(&lookahead).kind);
}

/* `x[a][b]... = value` -- any number of chained `[...]` groups before
 * the `=`, not just one. Originally scanned only a single balanced
 * `[...]` group before checking for `=`, so `x[a][b] = value` (a
 * genuine, previously-undiscovered gap, not a deliberate cut) fell
 * through to ordinary expression parsing instead -- `x[a][b]` compiled
 * fine as a *read* (parse_precedence's own postfix-chaining loop
 * already handles repeated `[...]`/`.` unconditionally), leaving the
 * trailing `= value` as unconsumed tokens, surfacing as "expected
 * newline after expression" at the statement level. After each closing
 * `]`, peek one more token: another `[` means "keep scanning, this
 * wasn't the last index"; anything else ends the scan the same way it
 * always did. */
bool index_assignment_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER&&
       compiler->current.kind!=DIAMOND_TOKEN_INSTANCE_VARIABLE&&
       compiler->current.kind!=DIAMOND_TOKEN_CLASS_VARIABLE) return false;
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=diamond_lexer_next(&lookahead);
    if(token.kind!=DIAMOND_TOKEN_LEFT_BRACKET) return false;
    bool more_groups=true;
    while(more_groups) {
        size_t depth=1;
        while(depth>0) {
            token=diamond_lexer_next(&lookahead);
            if(token.kind==DIAMOND_TOKEN_EOF || token.kind==DIAMOND_TOKEN_ERROR)
                return false;
            if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) depth++;
            if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET) depth--;
        }
        DiamondLexer probe=lookahead;
        token=diamond_lexer_next(&probe);
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            lookahead=probe;
            more_groups=true;
        } else {
            more_groups=false;
        }
    }
    return diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_EQUAL;
}

/* Same balanced-`[...]`-bracket scan as index_assignment_ahead just
 * above, but for `arr[i] += 1`-shaped indexed compound assignment
 * instead of plain `arr[i] = v` -- checks for a compound-assignment
 * operator (compound_assignment_token, shared with compound_assignment_
 * ahead) after the closing `]` instead of a bare `=`. */
static bool index_compound_assignment_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER&&
       compiler->current.kind!=DIAMOND_TOKEN_INSTANCE_VARIABLE&&
       compiler->current.kind!=DIAMOND_TOKEN_CLASS_VARIABLE) return false;
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=diamond_lexer_next(&lookahead);
    if(token.kind!=DIAMOND_TOKEN_LEFT_BRACKET) return false;
    size_t depth=1;
    while(depth>0) {
        token=diamond_lexer_next(&lookahead);
        if(token.kind==DIAMOND_TOKEN_EOF || token.kind==DIAMOND_TOKEN_ERROR)
            return false;
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) depth++;
        if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET) depth--;
    }
    return compound_assignment_token(diamond_lexer_next(&lookahead).kind);
}

static bool destructuring_target_kind(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_IDENTIFIER||kind==DIAMOND_TOKEN_INSTANCE_VARIABLE||
           kind==DIAMOND_TOKEN_CLASS_VARIABLE;
}

static DiamondToken scan_destructuring_target_suffix(DiamondLexer *lexer,
        DiamondToken token) {
    for(;;) {
        if(token.kind==DIAMOND_TOKEN_DOT) {
            token=diamond_lexer_next(lexer);
            if(token.kind!=DIAMOND_TOKEN_IDENTIFIER)return token;
            token=diamond_lexer_next(lexer);
            continue;
        }
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            size_t depth=1;
            while(depth>0) {
                token=diamond_lexer_next(lexer);
                if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR)
                    return token;
                if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET)depth++;
                if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET)depth--;
            }
            token=diamond_lexer_next(lexer);
            continue;
        }
        return token;
    }
}

static bool scan_destructuring_pattern(DiamondLexer *lexer,DiamondToken token,
        DiamondToken *after,size_t depth) {
    if(depth>8)return false;
    if(token.kind==DIAMOND_TOKEN_STAR) {
        token=diamond_lexer_next(lexer);
        if(!destructuring_target_kind(token.kind))return false;
        *after=scan_destructuring_target_suffix(lexer,diamond_lexer_next(lexer));
        return true;
    }
    if(destructuring_target_kind(token.kind)) {
        *after=scan_destructuring_target_suffix(lexer,diamond_lexer_next(lexer));
        return true;
    }
    if(token.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        token=diamond_lexer_next(lexer);
        if(token.kind==DIAMOND_TOKEN_RIGHT_BRACE)return false;
        for(;;) {
            if(token.kind==DIAMOND_TOKEN_STAR) {
                token=diamond_lexer_next(lexer);
                if(token.kind!=DIAMOND_TOKEN_STAR)return false;
                token=diamond_lexer_next(lexer);
                if(!destructuring_target_kind(token.kind))return false;
                token=scan_destructuring_target_suffix(
                    lexer,diamond_lexer_next(lexer));
                if(token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                    *after=diamond_lexer_next(lexer);return true;
                }
                if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
                token=diamond_lexer_next(lexer);
                continue;
            }
            size_t nesting=0;
            for(;;) {
                if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR||
                   token.kind==DIAMOND_TOKEN_NEWLINE)return false;
                if(token.kind==DIAMOND_TOKEN_COLON&&nesting==0)break;
                if(token.kind==DIAMOND_TOKEN_LEFT_PAREN||
                   token.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                   token.kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
                if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN||
                   token.kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                   token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                    if(nesting==0)return false;
                    nesting--;
                }
                token=diamond_lexer_next(lexer);
            }
            token=diamond_lexer_next(lexer);
            if(!scan_destructuring_pattern(lexer,token,&token,depth+1))return false;
            if(token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                *after=diamond_lexer_next(lexer);return true;
            }
            if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
            token=diamond_lexer_next(lexer);
        }
    }
    if(token.kind!=DIAMOND_TOKEN_LEFT_BRACKET)return false;
    token=diamond_lexer_next(lexer);
    if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET)return false;
    for(;;) {
        if(!scan_destructuring_pattern(lexer,token,&token,depth+1))return false;
        if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET) {
            *after=diamond_lexer_next(lexer);return true;
        }
        if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
        token=diamond_lexer_next(lexer);
    }
}

/* Same clone-the-lexer-and-scan-forward technique as assignment_ahead/
 * index_assignment_ahead above. Recognizes the historical comma-root form
 * (`a, b =`) and bracketed recursive patterns (`[a, [b, c]] =`) without
 * stealing an ordinary Array literal from expression parsing. */
static bool multi_assignment_ahead(const Compiler *compiler) {
    DiamondLexer lookahead = compiler->lexer;
    DiamondToken token=compiler->current;
    if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET||token.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        return scan_destructuring_pattern(&lookahead,token,&token,0)&&
            token.kind==DIAMOND_TOKEN_EQUAL;
    }
    if(!destructuring_target_kind(token.kind))return false;
    token=scan_destructuring_target_suffix(
        &lookahead,diamond_lexer_next(&lookahead));
    if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
    do {
        token=diamond_lexer_next(&lookahead);
        if(!scan_destructuring_pattern(&lookahead,token,&token,0))return false;
    } while(token.kind==DIAMOND_TOKEN_COMMA);
    return token.kind==DIAMOND_TOKEN_EQUAL;
}

/* The trailing modifier of the statement starting at the current token: IF, UNLESS, WHILE or
 * UNTIL, or EOF when there is none. *modifier_end, when given, is the lexer just past the
 * modifier token (the first token of its condition comes next). */
DiamondTokenKind postfix_modifier_ahead(const Compiler *compiler,
        DiamondLexer *modifier_end) {
    if (compiler->current.kind == DIAMOND_TOKEN_DEF ||
        compiler->current.kind == DIAMOND_TOKEN_CLOSURE ||
        compiler->current.kind == DIAMOND_TOKEN_CLASS ||
        compiler->current.kind == DIAMOND_TOKEN_SEALED ||
        compiler->current.kind == DIAMOND_TOKEN_STRUCT ||
        compiler->current.kind == DIAMOND_TOKEN_INTERFACE ||
        compiler->current.kind == DIAMOND_TOKEN_MODULE) {
        return DIAMOND_TOKEN_EOF;
    }
    DiamondLexer lookahead = compiler->lexer;
    /* compiler->current may itself already be an opening bracket (this
     * whole statement is a bracketed literal, e.g. `[if x ... end]`) --
     * the scan below only sees tokens *after* current, so depth has to
     * start accounting for that already-consumed bracket, or a nested
     * if/unless at the literal's own top level would be miscounted as
     * depth 0 and misread as a trailing modifier on the statement. */
    size_t depth = compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN ||
                   compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACKET ||
                   compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACE ? 1 : 0;
    bool seen = true;
    /* An if/unless immediately after '=' (or a compound `+=`, `||=`, ...)
     * is the start of the
     * assignment's own RHS expression (`x = if ... end`), not a
     * trailing postfix modifier on a value that hasn't been parsed yet.
     * Deliberately NOT extended to 'return'/'raise' in general (a
     * one-line if-expression with `then` is the exception, see below):
     * `return if cond`/`raise if cond` already have an established,
     * different meaning
     * (a bare return/raise, postfix-conditioned on cond -- see
     * compile_return/compile_raise's own current.kind==IF/UNLESS
     * branches) that this function's callers already rely on; treating
     * a leading if/unless there as the start of a value instead would
     * silently change what `return if flag` means. Narrower than fully
     * structure-aware (an if/unless immediately after some other
     * operator, e.g. `x = y && if ... end`, is still misread the old
     * way), but covers the position this most commonly comes up in. */
    bool expression_expected = false;
    /* do...end blocks opened after the statement's start: an if/unless
     * inside one belongs to that block's own body. An `end` with none open
     * closes whatever this statement is inside (a one-line block's body,
     * `do |x| x * 10 end, ...`), so the statement stops there. */
    size_t block_depth = 0;
    for (;;) {
        DiamondToken token = diamond_lexer_next(&lookahead);
        if (token.kind == DIAMOND_TOKEN_EOF ||
            token.kind == DIAMOND_TOKEN_ERROR ||
            token.kind == DIAMOND_TOKEN_NEWLINE) {
            return DIAMOND_TOKEN_EOF;
        }
        if (token.kind == DIAMOND_TOKEN_DO) {
            block_depth++;
            continue;
        }
        if (token.kind == DIAMOND_TOKEN_END) {
            if (block_depth == 0) return DIAMOND_TOKEN_EOF;
            block_depth--;
            continue;
        }
        if (block_depth > 0) continue;
        if (depth == 0 && seen && !expression_expected &&
            (token.kind == DIAMOND_TOKEN_WHILE || token.kind == DIAMOND_TOKEN_UNTIL)) {
            if (modifier_end != nullptr) *modifier_end = lookahead;
            return token.kind;
        }
        if (depth == 0 && seen &&
            (token.kind == DIAMOND_TOKEN_IF ||
             token.kind == DIAMOND_TOKEN_UNLESS)) {
            if (expression_expected) return DIAMOND_TOKEN_EOF;
            /* `return if ok then 0 else 1 end`: a `then` later on the line
             * makes this a one-line if-expression -- the value -- since a
             * trailing modifier's condition never contains `then`. */
            DiamondLexer rest = lookahead;
            for (DiamondToken after = diamond_lexer_next(&rest);
                 after.kind != DIAMOND_TOKEN_NEWLINE && after.kind != DIAMOND_TOKEN_EOF;
                 after = diamond_lexer_next(&rest))
                if (after.kind == DIAMOND_TOKEN_THEN) return DIAMOND_TOKEN_EOF;
            if (modifier_end != nullptr) *modifier_end = lookahead;
            return token.kind;
        }
        switch (token.kind) {
            case DIAMOND_TOKEN_LEFT_PAREN:
            case DIAMOND_TOKEN_LEFT_BRACKET:
            case DIAMOND_TOKEN_LEFT_BRACE:
                depth++;
                break;
            case DIAMOND_TOKEN_RIGHT_PAREN:
            case DIAMOND_TOKEN_RIGHT_BRACKET:
            case DIAMOND_TOKEN_RIGHT_BRACE:
                if (depth > 0) depth--;
                break;
            default:
                break;
        }
        expression_expected = depth == 0 &&
            (token.kind == DIAMOND_TOKEN_EQUAL || compound_assignment_token(token.kind));
        seen = true;
    }
}

static uint16_t compile_index_assignment(Compiler *compiler) {
    const DiamondSpan name=compiler->current.span;
    uint16_t receiver,mutation_receiver;
    /* @ivar[key] = value / @@cvar[key] = value: unlike the plain local
     * case below, there's no boxed-Cell concern here -- Hash/Array are
     * heap-allocated, mutable-in-place reference objects (confirmed
     * directly: mutating through a freshly loaded register affects the
     * same object @ivar/@@cvar itself still points at), so loading the
     * current value into a fresh register and running DIAMOND_OP_INDEX_SET
     * against that register is enough; there's no separate "write the
     * mutated value back to the ivar/cvar" step. This mirrors exactly how
     * parse_prefix's own DIAMOND_TOKEN_INSTANCE_VARIABLE/CLASS_VARIABLE
     * cases read one as an ordinary expression. */
    if(compiler->current.kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        receiver=allocate_register(compiler);
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,receiver,0,field,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR,receiver,0,(uint8_t)field,3);
        }
        mutation_receiver=receiver;
    } else if(compiler->current.kind==DIAMOND_TOKEN_CLASS_VARIABLE) {
        receiver=allocate_register(compiler);
        const int slot=class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_GET_CVAR,receiver,
                         (uint16_t)class_variable_owner(compiler,name),(uint8_t)slot,3);
        mutation_receiver=receiver;
    } else {
        const int local=find_local(compiler,name);
        if(local<0) {
            const int constant_receiver=load_constant_receiver(compiler,name);
            if(constant_receiver<0) {
                fail(compiler,name,"undefined local variable");return 0;
            }
            receiver=(uint16_t)constant_receiver;
        } else {
            receiver=compiler->locals[(size_t)local].reg;
        }
        mutation_receiver=receiver;
        if(local>=0&&compiler->locals[(size_t)local].captured) {
            /* See parse_identifier's own BOX_LOCAL re-emission for why this
             * defensive re-box is needed: `captured` doesn't imply this
             * control-flow path actually ran the boxing site. */
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,receiver,0,0,1);
            const uint16_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
            receiver=loaded;
        }
    }
    advance_token(compiler);
    advance_token(compiler);
    uint16_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after assignment index");
        return 0;
    }
    advance_token(compiler);
    /* `x[a][b]... = value`: every `[...]` group before the last one is
     * an ordinary read (INDEX_GET) that produces the *next* receiver --
     * only the final group before `=` becomes the actual assignment
     * target (INDEX_SET). index_assignment_ahead's own lookahead already
     * confirmed a chain shaped exactly like this exists, so this loop
     * always terminates at a real `=`, never falls off the end. */
    while(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        const uint16_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,loaded,receiver,index,3);
        set_index_provenance(compiler,loaded,receiver);
        publish_indexed_result_type(compiler,loaded,receiver);
        receiver=loaded;
        mutation_receiver=receiver;
        advance_token(compiler);
        index=parse_expression(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,"expected ']' after assignment index");
            return 0;
        }
        advance_token(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_EQUAL) {
        fail(compiler,compiler->current.span,"expected '=' after indexed target");
        return 0;
    }
    advance_token(compiler);
    const uint16_t value=parse_expression(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,value,3);
    update_collection_mutation_type(compiler,mutation_receiver,&index,1,&value,1);
    propagate_nested_mutation_writeback(compiler,mutation_receiver);
    return value;
}

/* `arr[i] += 1`-shaped indexed compound assignment -- the receiver-
 * loading prologue is a verbatim copy of compile_index_assignment's own
 * just above (identical regardless of what follows the index), and the
 * operator dispatch is the same shape compile_compound_assignment uses
 * below for a plain local/@ivar/@@cvar target (||=/&&= short-circuit via
 * a conditional jump around evaluating the right-hand side at all;
 * +=, -=, *=, /=, and %= all reduce to one compile_binary_op call). The one new
 * concern beyond compile_index_assignment's own is avoiding double-
 * evaluation of the index expression -- solved by keeping the single
 * register parse_expression already returns for it and reusing that
 * same register for both the read (INDEX_GET) and the write back
 * (INDEX_SET), the same way the receiver register is already reused for
 * both today. */
static uint16_t compile_index_compound_assignment(Compiler *compiler) {
    const DiamondSpan name=compiler->current.span;
    uint16_t receiver,mutation_receiver;
    if(compiler->current.kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        receiver=allocate_register(compiler);
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,receiver,0,field,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR,receiver,0,(uint8_t)field,3);
        }
        mutation_receiver=receiver;
    } else if(compiler->current.kind==DIAMOND_TOKEN_CLASS_VARIABLE) {
        receiver=allocate_register(compiler);
        const int slot=class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_GET_CVAR,receiver,
                         (uint16_t)class_variable_owner(compiler,name),(uint8_t)slot,3);
        mutation_receiver=receiver;
    } else {
        const int local=find_local(compiler,name);
        if(local<0) {
            const int constant_receiver=load_constant_receiver(compiler,name);
            if(constant_receiver<0) {
                fail(compiler,name,"undefined local variable");return 0;
            }
            receiver=(uint16_t)constant_receiver;
        } else {
            receiver=compiler->locals[(size_t)local].reg;
        }
        mutation_receiver=receiver;
        if(local>=0&&compiler->locals[(size_t)local].captured) {
            /* See compile_index_assignment's own comment on this exact
             * defensive re-box. */
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,receiver,0,0,1);
            const uint16_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
            receiver=loaded;
        }
    }
    advance_token(compiler);
    advance_token(compiler);
    const uint16_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after assignment index");
        return 0;
    }
    advance_token(compiler);
    const uint16_t left=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_GET,left,receiver,index,3);
    const DiamondTokenKind op_kind=compiler->current.kind;
    advance_token(compiler);
    if(op_kind==DIAMOND_TOKEN_OR_OR_EQUAL||op_kind==DIAMOND_TOKEN_AND_AND_EQUAL) {
        const bool is_and=op_kind==DIAMOND_TOKEN_AND_AND_EQUAL;
        const uint16_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
        const size_t end_jump=emit_jump(compiler,
            is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
        const uint16_t right=parse_expression(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
        patch_jump(compiler,end_jump,compiler->function->code_count);
        emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,destination,3);
        update_collection_mutation_type(compiler,mutation_receiver,&index,1,
            &destination,1);
        return destination;
    }
    const DiamondTokenKind plain_op=
        op_kind==DIAMOND_TOKEN_PLUS_EQUAL?DIAMOND_TOKEN_PLUS:
        op_kind==DIAMOND_TOKEN_MINUS_EQUAL?DIAMOND_TOKEN_MINUS:
        op_kind==DIAMOND_TOKEN_STAR_EQUAL?DIAMOND_TOKEN_STAR:
        op_kind==DIAMOND_TOKEN_SLASH_EQUAL?DIAMOND_TOKEN_SLASH:
        DIAMOND_TOKEN_PERCENT;
    const uint16_t right=parse_expression(compiler);
    const uint16_t destination=compile_binary_op(compiler,plain_op,left,right);
    emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,destination,3);
    update_collection_mutation_type(compiler,mutation_receiver,&index,1,
        &destination,1);
    return destination;
}

/* If the most recently emitted plain CALL (tracked precisely at emission
 * time by parse_call -- see compiler->last_plain_call_* fields' own
 * comment for why this is sound and a byte-guessing alternative was not)
 * is still exactly the last thing emitted for the function currently
 * being compiled, its own destination is exactly `return_value_register`,
 * and its own target is this same function (self-recursion), rewrites
 * that CALL's opcode byte to DIAMOND_OP_TAIL_CALL in place -- see that
 * opcode's own comment (src/vm.h) for the runtime side and the full
 * eligibility rule. Called from every site about to emit a RETURN for an
 * ordinary user function body (an explicit `return EXPR`, or a body's own
 * trailing implicit return) -- harmless, silent no-op whenever nothing
 * eligible was just emitted, so it's safe to call unconditionally rather
 * than needing each call site to first prove its own context is eligible.
 *
 * Deliberately excludes: any call inside a begin/rescue/ensure block
 * (compiler->begin_depth>0 -- a live handler's own jump target is an
 * offset into the *current* chunk's bytecode, which reusing the same
 * run_chunk invocation for "the next iteration" must never invalidate);
 * and, structurally, anything that isn't a plain positional CALL to
 * a function by name (a method/singleton call or a CALL_TYPED generic call
 * uses a different opcode, or never touches last_plain_call_* at all). A call
 * to a different function is only recorded here; rewrite_cyclic_tail_calls
 * decides, once every function exists, whether it lies on a cycle. A generic
 * or variadic function qualifies:
 * run_chunk's TAIL_CALL replaces its `arguments`/`argument_count` and
 * re-infers type-variable bindings, as a fresh call would. */
void maybe_rewrite_self_tail_call(Compiler *compiler,
        uint16_t return_value_register) {
    if(compiler->begin_depth>0)return;
    /* A block's break lands in its call, which a tail call would replace. */
    if(compiler->function->nonlocal_landing)return;
    if(compiler->last_plain_call_function!=compiler->function)return;
    /* Either the call is the last instruction and writes the return register,
     * or only a `MOVE return_register, call_destination` follows it with no
     * jump landing between them: the join of an `if`/`else` or `case` whose
     * branch ends in the call. The MOVE is then dead code after the rewrite. */
    constexpr size_t move_size=5;
    const DiamondFunction *current=compiler->function;
    if(compiler->last_plain_call_code_count==current->code_count) {
        if(compiler->last_plain_call_destination!=return_value_register)return;
    } else {
        const size_t move_at=current->code_count-move_size;
        if(current->code_count<move_size||
           compiler->last_plain_call_code_count!=move_at||
           compiler->last_plain_call_label_after||
           current->code[move_at]!=DIAMOND_OP_MOVE||
           (uint16_t)((current->code[move_at+1]<<8)|current->code[move_at+2])!=
               return_value_register||
           (uint16_t)((current->code[move_at+3]<<8)|current->code[move_at+4])!=
               compiler->last_plain_call_destination)return;
    }
    const uint16_t function_index=compiler->last_plain_call_function_index;
    if((size_t)function_index>=compiler->program->function_count)return;
    if(compiler->program->functions[function_index]!=compiler->function) {
        /* A different function: only a candidate, decided once every
         * function exists (see rewrite_cyclic_tail_calls). A failed
         * allocation just leaves the call an ordinary one. */
        if(compiler->discovery_pass)return;
        if(compiler->tail_call_candidate_count==
           compiler->tail_call_candidate_capacity) {
            const size_t capacity=compiler->tail_call_candidate_capacity==0?
                16:compiler->tail_call_candidate_capacity*2;
            TailCallCandidate *grown=realloc(compiler->tail_call_candidates,
                capacity*sizeof *grown);
            if(grown==nullptr)return;
            compiler->tail_call_candidates=grown;
            compiler->tail_call_candidate_capacity=capacity;
        }
        compiler->tail_call_candidates[compiler->tail_call_candidate_count++]=
            (TailCallCandidate){.caller=compiler->function,
                .offset=compiler->last_plain_call_offset,.callee=function_index};
        return;
    }
    compiler->function->code[compiler->last_plain_call_offset]=
        DIAMOND_OP_TAIL_CALL;
}

/* Rewrites a recorded cross-function tail call to DIAMOND_OP_TAIL_CALL when
 * it lies on a cycle of recorded calls (mutual recursion: strongly connected
 * components of more than one function, Tarjan's algorithm, iterative so a
 * program with thousands of functions cannot overflow the C stack). A tail
 * call that is not on a cycle cannot recurse without bound through tail
 * calls alone, and leaving it an ordinary CALL keeps a plain delegating
 * wrapper compilable by the JIT, which has no TAIL_CALL. */
void rewrite_cyclic_tail_calls(const DiamondProgram *program,
        const TailCallCandidate *candidates,size_t candidate_count) {
    if(candidate_count==0)return;
    const size_t function_count=program->function_count;
    /* Compact node numbering: only functions that are the caller or callee
     * of a candidate. node_of[function index] is 1-based, 0 = unnumbered. */
    uint32_t *node_of=calloc(function_count,sizeof *node_of);
    uint32_t *edge_from=malloc(candidate_count*sizeof *edge_from);
    uint32_t *edge_to=malloc(candidate_count*sizeof *edge_to);
    bool ok=node_of!=nullptr&&edge_from!=nullptr&&edge_to!=nullptr;
    size_t node_count=0;
    size_t edge_count=0;
    size_t *edge_candidate=ok?malloc(candidate_count*sizeof *edge_candidate):nullptr;
    ok=ok&&edge_candidate!=nullptr;
    for(size_t index=0;ok&&index<candidate_count;index++) {
        if(candidates[index].caller->nonlocal_landing)continue;
        size_t caller=function_count;
        for(size_t probe=0;probe<function_count;probe++)
            if(program->functions[probe]==candidates[index].caller) {
                caller=probe;break;
            }
        const size_t callee=candidates[index].callee;
        if(caller==function_count||callee>=function_count)continue;
        if(node_of[caller]==0)node_of[caller]=(uint32_t)++node_count;
        if(node_of[callee]==0)node_of[callee]=(uint32_t)++node_count;
        edge_from[edge_count]=node_of[caller]-1;
        edge_to[edge_count]=node_of[callee]-1;
        edge_candidate[edge_count++]=index;
    }
    /* CSR adjacency over the numbered nodes. */
    size_t *first=ok?calloc(node_count+1,sizeof *first):nullptr;
    uint32_t *targets=ok?malloc((edge_count+1)*sizeof *targets):nullptr;
    int64_t *discovery=ok?malloc((node_count+1)*sizeof *discovery):nullptr;
    int64_t *lowlink=ok?malloc((node_count+1)*sizeof *lowlink):nullptr;
    uint32_t *component=ok?malloc((node_count+1)*sizeof *component):nullptr;
    uint32_t *component_size=ok?calloc(node_count+1,sizeof *component_size):nullptr;
    uint32_t *tarjan_stack=ok?malloc((node_count+1)*sizeof *tarjan_stack):nullptr;
    uint32_t *walk_node=ok?malloc((node_count+1)*sizeof *walk_node):nullptr;
    size_t *walk_edge=ok?malloc((node_count+1)*sizeof *walk_edge):nullptr;
    bool *on_stack=ok?calloc(node_count+1,sizeof *on_stack):nullptr;
    ok=ok&&first!=nullptr&&targets!=nullptr&&discovery!=nullptr&&
        lowlink!=nullptr&&component!=nullptr&&component_size!=nullptr&&
        tarjan_stack!=nullptr&&walk_node!=nullptr&&walk_edge!=nullptr&&
        on_stack!=nullptr;
    if(ok) {
        for(size_t edge=0;edge<edge_count;edge++)first[edge_from[edge]+1]++;
        for(size_t node=0;node<node_count;node++)first[node+1]+=first[node];
        size_t *fill=malloc((node_count+1)*sizeof *fill);
        if(fill==nullptr)ok=false;
        else {
            memcpy(fill,first,(node_count+1)*sizeof *fill);
            for(size_t edge=0;edge<edge_count;edge++)
                targets[fill[edge_from[edge]]++]=edge_to[edge];
            free(fill);
        }
    }
    if(ok) {
        for(size_t node=0;node<node_count;node++)discovery[node]=-1;
        int64_t clock=0;
        size_t stack_top=0;
        uint32_t components=0;
        for(size_t root=0;root<node_count;root++) {
            if(discovery[root]>=0)continue;
            size_t depth=0;
            walk_node[0]=(uint32_t)root;walk_edge[0]=first[root];
            discovery[root]=lowlink[root]=clock++;
            tarjan_stack[stack_top++]=(uint32_t)root;on_stack[root]=true;
            while(true) {
                const uint32_t node=walk_node[depth];
                if(walk_edge[depth]<first[node+1]) {
                    const uint32_t next=targets[walk_edge[depth]++];
                    if(discovery[next]<0) {
                        discovery[next]=lowlink[next]=clock++;
                        tarjan_stack[stack_top++]=next;on_stack[next]=true;
                        depth++;
                        walk_node[depth]=next;walk_edge[depth]=first[next];
                    } else if(on_stack[next]&&discovery[next]<lowlink[node])
                        lowlink[node]=discovery[next];
                    continue;
                }
                if(lowlink[node]==discovery[node]) {
                    uint32_t member;
                    do {
                        member=tarjan_stack[--stack_top];
                        on_stack[member]=false;
                        component[member]=components;
                        component_size[components]++;
                    } while(member!=node);
                    components++;
                }
                if(depth==0)break;
                depth--;
                const uint32_t parent=walk_node[depth];
                if(lowlink[node]<lowlink[parent])lowlink[parent]=lowlink[node];
            }
        }
        for(size_t edge=0;edge<edge_count;edge++) {
            if(component[edge_from[edge]]!=component[edge_to[edge]])continue;
            if(component_size[component[edge_from[edge]]]<2)continue;
            const TailCallCandidate *candidate=&candidates[edge_candidate[edge]];
            if(candidate->caller->code[candidate->offset]==DIAMOND_OP_CALL)
                candidate->caller->code[candidate->offset]=DIAMOND_OP_TAIL_CALL;
        }
    }
    free(node_of);free(edge_from);free(edge_to);free(edge_candidate);
    free(first);free(targets);free(discovery);free(lowlink);free(component);
    free(component_size);free(tarjan_stack);free(walk_node);free(walk_edge);
    free(on_stack);
}

uint16_t compile_assignment(Compiler *compiler) {
    const DiamondSpan name = compiler->current.span;
    const bool instance_variable =
        compiler->current.kind == DIAMOND_TOKEN_INSTANCE_VARIABLE;
    const bool class_variable =
        compiler->current.kind == DIAMOND_TOKEN_CLASS_VARIABLE;
    advance_token(compiler);
    advance_token(compiler);
    const uint16_t rhs_start_register = compiler->next_register;
    const uint16_t value = parse_expression(compiler);
    return compile_assignment_store(compiler, name, instance_variable, class_variable, value,
        rhs_start_register);
}

/* `x += y`/`x -= y`/.../`x ||= y`/`x &&= y` -- pure sugar, expanded here
 * into the same shape the equivalent spelled-out form would produce:
 * `x = x + y` for the arithmetic ones, `x = x || y`/`x = x && y`
 * (genuinely short-circuit -- `y` is never evaluated, and never
 * assigned, unless the short-circuit check requires it) for the other
 * two. Reads the target's current value via parse_prefix -- the exact
 * same prefix-dispatch parse_precedence itself uses, so a captured
 * local/an ivar/a cvar all read correctly with no special-casing here,
 * the same way compile_assignment_store's write side already handles
 * all three uniformly. Only ever called once compound_assignment_ahead
 * has confirmed the next token really is one of these -- indexed
 * targets (`arr[i] += 1`) aren't recognized at all (see that function's
 * own comment), so this never needs to guard against one. */
static uint16_t compile_compound_assignment(Compiler *compiler) {
    const DiamondSpan name = compiler->current.span;
    const bool instance_variable =
        compiler->current.kind == DIAMOND_TOKEN_INSTANCE_VARIABLE;
    const bool class_variable =
        compiler->current.kind == DIAMOND_TOKEN_CLASS_VARIABLE;
    const uint16_t left = parse_prefix(compiler);
    const DiamondTokenKind op_kind = compiler->current.kind;
    advance_token(compiler);
    if(op_kind==DIAMOND_TOKEN_OR_OR_EQUAL||op_kind==DIAMOND_TOKEN_AND_AND_EQUAL) {
        const bool is_and = op_kind==DIAMOND_TOKEN_AND_AND_EQUAL;
        const uint16_t destination = allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
        const size_t end_jump = emit_jump(compiler,
            is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
        const uint16_t right = parse_expression(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
        patch_jump(compiler,end_jump,compiler->function->code_count);
        /* rhs_start_register=compiler->next_register: a no-op threshold
         * that always disables compile_assignment_store's own register-
         * reuse optimization here, same as this function's other call
         * just below -- compound assignment's target always already
         * exists (parse_prefix's own read would fail otherwise, see this
         * function's own comment), so `local < 0` never holds for either
         * call and the passed value is moot either way. */
        return compile_assignment_store(compiler,name,instance_variable,
            class_variable,destination,compiler->next_register);
    }
    const DiamondTokenKind plain_op =
        op_kind==DIAMOND_TOKEN_PLUS_EQUAL?DIAMOND_TOKEN_PLUS:
        op_kind==DIAMOND_TOKEN_MINUS_EQUAL?DIAMOND_TOKEN_MINUS:
        op_kind==DIAMOND_TOKEN_STAR_EQUAL?DIAMOND_TOKEN_STAR:
        op_kind==DIAMOND_TOKEN_SLASH_EQUAL?DIAMOND_TOKEN_SLASH:
        DIAMOND_TOKEN_PERCENT;
    const uint16_t right = parse_expression(compiler);
    const uint16_t destination = compile_binary_op(compiler, plain_op, left, right);
    return compile_assignment_store(compiler, name, instance_variable,
        class_variable, destination, compiler->next_register);
}

/* Finds (or, the first time in this function, registers) a one-member
 * type set matching plain `Array` (no element-type argument) -- the
 * same DiamondTypeSet shape parse_type_annotation builds when it reads
 * a literal `Array` annotation from source, just constructed directly
 * here since there's no source text driving it. Reused across every
 * destructuring statement in the same function rather than burning a
 * fresh type-set slot per statement. */
static uint16_t array_type_set_index(Compiler *compiler) {
    return plain_type_set_index(compiler,DIAMOND_TYPE_ARRAY);
}

static uint16_t hash_type_set_index(Compiler *compiler) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_HASH&&
           set->members[0].argument_set==DIAMOND_NO_TYPE_SET)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;
    set->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_HASH,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t member_index=0;member_index<16;member_index++)
        set->members[0].callable_parameter_sets[member_index]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)set_index;
}

static uint16_t load_destructure_target(Compiler *compiler,DiamondSpan name,
        DiamondTokenKind kind) {
    if(kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        const uint16_t receiver=allocate_register(compiler);
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,receiver,0,field,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR,receiver,0,(uint8_t)field,3);
        }
        return receiver;
    }
    if(kind==DIAMOND_TOKEN_CLASS_VARIABLE) {
        const uint16_t receiver=allocate_register(compiler);
        const int slot=class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_GET_CVAR,receiver,
            (uint16_t)class_variable_owner(compiler,name),(uint8_t)slot,3);
        return receiver;
    }
    const int local=find_local(compiler,name);
    if(local<0) {
        const int receiver=load_constant_receiver(compiler,name);
        if(receiver>=0)return (uint16_t)receiver;
        fail(compiler,name,"undefined local variable");return 0;
    }
    uint16_t receiver=compiler->locals[(size_t)local].reg;
    if(compiler->locals[(size_t)local].captured) {
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,receiver,0,0,1);
        const uint16_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
        receiver=loaded;
    }
    return receiver;
}

static uint8_t parse_destructure_node(Compiler *compiler,DestructureNode *nodes,
        size_t *node_count,size_t depth) {
    if(*node_count==64||depth>8) {
        fail(compiler,compiler->current.span,"destructuring pattern is too complex");
        return 0;
    }
    const uint8_t node_index=(uint8_t)(*node_count);
    DestructureNode *node=&nodes[(*node_count)++];
    *node=(DestructureNode){};
    if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
        node->rest=true;advance_token(compiler);
        if(!destructuring_target_kind(compiler->current.kind)) {
            fail(compiler,compiler->current.span,"expected target after '*' in destructuring pattern");
            return node_index;
        }
    }
    if(destructuring_target_kind(compiler->current.kind)) {
        const DiamondTokenKind target_kind=compiler->current.kind;
        node->leaf=true;node->name=compiler->current.span;
        node->instance_variable=target_kind==DIAMOND_TOKEN_INSTANCE_VARIABLE;
        node->class_variable=target_kind==DIAMOND_TOKEN_CLASS_VARIABLE;
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
           compiler->current.kind==DIAMOND_TOKEN_DOT) {
            uint16_t receiver=load_destructure_target(
                compiler,node->name,target_kind);
            while(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                  compiler->current.kind==DIAMOND_TOKEN_DOT) {
                if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
                    advance_token(compiler);
                    const uint16_t index=parse_expression(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                        fail(compiler,compiler->current.span,
                            "expected ']' after destructuring target index");
                        return node_index;
                    }
                    advance_token(compiler);
                    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                       compiler->current.kind==DIAMOND_TOKEN_DOT) {
                        const uint16_t loaded=allocate_register(compiler);
                        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,
                            loaded,receiver,index,3);
                        receiver=loaded;
                    } else {
                        node->indexed=true;
                        node->target_receiver=receiver;
                        node->target_index=index;
                    }
                } else {
                    advance_token(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                        fail(compiler,compiler->current.span,
                            "expected member after '.' in destructuring target");
                        return node_index;
                    }
                    const DiamondSpan member=compiler->current.span;
                    advance_token(compiler);
                    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                       compiler->current.kind==DIAMOND_TOKEN_DOT)
                        receiver=emit_invoke_call(compiler,receiver,member,false,
                            nullptr,0,nullptr,0);
                    else {
                        node->member=true;
                        node->target_receiver=receiver;
                        node->target_member=member;
                    }
                }
            }
        }
        return node_index;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        node->hash=true;advance_token(compiler);skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE&&!compiler->failed) {
            if(node->child_count==16) {
                fail(compiler,compiler->current.span,"too many Hash destructuring targets");
                return node_index;
            }
            DiamondLexer rest_lookahead=compiler->lexer;
            const DiamondToken second_star=diamond_lexer_next(&rest_lookahead);
            if(compiler->current.kind==DIAMOND_TOKEN_STAR&&
               second_star.kind==DIAMOND_TOKEN_STAR) {
                advance_token(compiler);advance_token(compiler);
                const uint8_t child=parse_destructure_node(
                    compiler,nodes,node_count,depth+1);
                if(!nodes[child].leaf) {
                    fail(compiler,compiler->previous.span,
                        "Hash rest target must be a variable");return node_index;
                }
                nodes[child].hash_rest=true;
                node->children[node->child_count++]=child;
                skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
                    fail(compiler,compiler->current.span,
                        "Hash rest target must be last");return node_index;
                }
                break;
            }
            const uint16_t key=parse_expression(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                fail(compiler,compiler->current.span,
                    "expected ':' after Hash destructuring key");return node_index;
            }
            advance_token(compiler);
            const uint8_t child=parse_destructure_node(
                compiler,nodes,node_count,depth+1);
            if(nodes[child].rest||nodes[child].hash_rest) {
                fail(compiler,nodes[child].name,
                    "rest target is not valid as a Hash entry target");
                return node_index;
            }
            nodes[child].key_register=key;
            node->children[node->child_count++]=child;
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE)break;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                fail(compiler,compiler->current.span,
                    "expected ',' in Hash destructuring pattern");return node_index;
            }
            advance_token(compiler);skip_newlines(compiler);
        }
        if(node->child_count==0) {
            fail(compiler,compiler->current.span,
                "Hash destructuring pattern cannot be empty");return node_index;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
            fail(compiler,compiler->current.span,
                "expected '}' after Hash destructuring pattern");return node_index;
        }
        advance_token(compiler);return node_index;
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_BRACKET) {
        fail(compiler,compiler->current.span,"expected destructuring target");return 0;
    }
    advance_token(compiler);
    bool rest_seen=false;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&!compiler->failed) {
        if(node->child_count==16) {
            fail(compiler,compiler->current.span,"too many destructuring targets");
            return node_index;
        }
        node->children[node->child_count++]=
            parse_destructure_node(compiler,nodes,node_count,depth+1);
        if(nodes[node->children[node->child_count-1]].rest) {
            if(rest_seen) {
                fail(compiler,nodes[node->children[node->child_count-1]].name,
                    "destructuring pattern may contain only one rest target");
                return node_index;
            }
            rest_seen=true;
        }
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET)break;
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,"expected ',' in destructuring pattern");
            return node_index;
        }
        advance_token(compiler);
    }
    if(node->child_count==0) {
        fail(compiler,compiler->current.span,"destructuring pattern cannot be empty");
        return node_index;
    }
    advance_token(compiler);return node_index;
}

static void emit_destructure_extract(Compiler *compiler,
        const DestructureNode *nodes,uint8_t node_index,uint16_t value,
        uint16_t array_set,uint16_t hash_set,uint16_t *node_values) {
    const DestructureNode *node=&nodes[node_index];
    node_values[node_index]=value;
    if(node->leaf)return;
    if(node->hash) {
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,value,hash_set,0,2);
        size_t fixed_count=0;
        for(size_t child=0;child<node->child_count;child++)
            if(!nodes[node->children[child]].hash_rest)fixed_count++;
        for(size_t child=0;child<node->child_count;child++) {
            const DestructureNode *child_node=&nodes[node->children[child]];
            if(child_node->hash_rest) {
                const uint16_t key_base=allocate_register(compiler);
                for(size_t key=1;key<fixed_count;key++)(void)allocate_register(compiler);
                size_t key_index=0;
                for(size_t fixed=0;fixed<node->child_count;fixed++) {
                    const DestructureNode *fixed_node=&nodes[node->children[fixed]];
                    if(fixed_node->hash_rest)continue;
                    emit_instruction(compiler,DIAMOND_OP_MOVE,
                        (uint16_t)(key_base+key_index++),fixed_node->key_register,0,2);
                }
                const uint16_t excluded=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARRAY,excluded,key_base,
                    (uint16_t)fixed_count,3);
                compiler->known_types[excluded]=DIAMOND_TYPE_ARRAY;
                const uint16_t rest=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_HASH_REST,rest,value,excluded,3);
                compiler->known_types[rest]=DIAMOND_TYPE_HASH;
                node_values[node->children[child]]=rest;
                continue;
            }
            emit_instruction(compiler,DIAMOND_OP_CHECK_HASH_KEY,value,
                child_node->key_register,0,2);
            const uint16_t element=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_INDEX_GET,element,value,
                child_node->key_register,3);
            emit_destructure_extract(compiler,nodes,node->children[child],element,
                array_set,hash_set,node_values);
        }
        return;
    }
    emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,value,array_set,0,2);
    size_t rest_index=SIZE_MAX;
    for(size_t child=0;child<node->child_count;child++)
        if(nodes[node->children[child]].rest)rest_index=child;
    const bool has_rest=rest_index!=SIZE_MAX;
    const uint16_t fixed_count=(uint16_t)(node->child_count-(has_rest?1u:0u));
    emit_instruction(compiler,DIAMOND_OP_CHECK_DESTRUCTURE_COUNT,value,
        fixed_count|(has_rest?0x8000u:0u),0,2);
    for(size_t index=0;index<node->child_count;index++) {
        if(nodes[node->children[index]].rest) {
            const uint16_t rest=allocate_register(compiler);
            const uint16_t suffix=(uint16_t)(node->child_count-index-1);
            emit_instruction(compiler,DIAMOND_OP_ARRAY_MIDDLE,rest,value,
                (uint16_t)((index<<8)|suffix),3);
            compiler->known_types[rest]=DIAMOND_TYPE_ARRAY;
            emit_destructure_extract(compiler,nodes,node->children[index],rest,
                array_set,hash_set,node_values);continue;
        }
        if(has_rest&&index>rest_index) {
            const uint16_t element=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ARRAY_SUFFIX,element,value,
                (uint16_t)(node->child_count-index),3);
            emit_destructure_extract(compiler,nodes,node->children[index],element,
                array_set,hash_set,node_values);continue;
        }
        const uint16_t index_constant=add_constant(compiler,DIAMOND_INT((int64_t)index));
        const uint16_t index_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,index_register,index_constant,0,2);
        const uint16_t element=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,element,value,index_register,3);
        emit_destructure_extract(compiler,nodes,node->children[index],element,
            array_set,hash_set,node_values);
    }
}

static uint16_t emit_destructure_stores(Compiler *compiler,
        const DestructureNode *nodes,uint8_t node_index,const uint16_t *node_values) {
    const DestructureNode *node=&nodes[node_index];
    if(node->leaf) {
        if(node->indexed) {
            emit_instruction(compiler,DIAMOND_OP_INDEX_SET,node->target_receiver,
                node->target_index,node_values[node_index],3);
            return node_values[node_index];
        }
        if(node->member) {
            const uint16_t value=node_values[node_index];
            (void)emit_invoke_call(compiler,node->target_receiver,
                node->target_member,true,nullptr,0,&value,1);
            return value;
        }
        /* rhs_start_register=compiler->next_register: node_values[...] was
         * resolved earlier (destructuring the already-evaluated source),
         * not a fresh RHS expression here, so this unconditionally
         * disables compile_assignment_store's own register-reuse
         * optimization for a destructuring target -- see that function's
         * own comment. */
        return compile_assignment_store(compiler,node->name,
            node->instance_variable,node->class_variable,node_values[node_index],
            compiler->next_register);
    }
    uint16_t last=node_values[node_index];
    for(size_t index=0;index<node->child_count;index++)
        last=emit_destructure_stores(compiler,nodes,node->children[index],node_values);
    return last;
}

/* Existing `a, b = expr` plus bracketed/nested `[a, [b, c]] = expr`.
 * The RHS is evaluated once; every group independently enforces an exact
 * Array shape before its leaves use ordinary local/@ivar/@@cvar stores. */
uint16_t compile_multi_assignment(Compiler *compiler) {
    DestructureNode nodes[64]={};size_t node_count=0;
    const uint8_t root=(uint8_t)node_count++;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
       compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        node_count=0;
        (void)parse_destructure_node(compiler,nodes,&node_count,0);
    } else {
        bool rest_seen=false;
        while(!compiler->failed) {
            DestructureNode *root_node=&nodes[root];
            if(root_node->child_count==16) {
                fail(compiler,compiler->current.span,"too many destructuring targets");
                return 0;
            }
            root_node->children[root_node->child_count++]=
                parse_destructure_node(compiler,nodes,&node_count,0);
            if(nodes[root_node->children[root_node->child_count-1]].rest) {
                if(rest_seen) {
                    fail(compiler,
                        nodes[root_node->children[root_node->child_count-1]].name,
                        "destructuring pattern may contain only one rest target");
                    return 0;
                }
                rest_seen=true;
            }
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
        }
    }
    if(compiler->failed)return 0;
    if(compiler->current.kind!=DIAMOND_TOKEN_EQUAL) {
        fail(compiler,compiler->current.span,"expected '=' after destructuring pattern");
        return 0;
    }
    advance_token(compiler);
    uint16_t rhs_values[16];size_t rhs_count=0;
    rhs_values[rhs_count++]=parse_expression(compiler);
    while(compiler->current.kind==DIAMOND_TOKEN_COMMA&&!compiler->failed) {
        if(rhs_count==16) {
            fail(compiler,compiler->current.span,
                "too many destructuring right-hand values");return 0;
        }
        advance_token(compiler);skip_newlines(compiler);
        rhs_values[rhs_count++]=parse_expression(compiler);
    }
    const uint16_t value=rhs_count==1?rhs_values[0]:
        emit_argument_array(compiler,rhs_values,rhs_count);
    const uint16_t array_set = array_type_set_index(compiler);
    const uint16_t hash_set = hash_type_set_index(compiler);
    uint16_t node_values[64]={};
    emit_destructure_extract(compiler,nodes,root,value,array_set,hash_set,node_values);
    return emit_destructure_stores(compiler,nodes,root,node_values);
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
static uint16_t compile_expression_statement(Compiler *compiler,bool has_postfix) {
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
            const Narrowing postfix_narrowing=
                compiler->narrowing.valid&&compiler->narrowing.condition==condition?
                    compiler->narrowing:(Narrowing){};
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
                    apply_narrowing_facts(compiler,postfix_narrowing.when_false,
                        postfix_narrowing.when_false_count);
                else
                    apply_narrowing_facts(compiler,postfix_narrowing.when_true,
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
