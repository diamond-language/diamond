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

/* Call parsing: argument lists (keyword, spread, block and closure arguments), `parse_call`, and the
 * receiver-less and forward-call handling the tooling relies on. */

/* Explicit generic arguments trail every typed call opcode: their count,
 * then each type-set register. */
void emit_type_argument_trailer(Compiler *compiler,const uint16_t *type_arguments,
        size_t type_argument_count) {
    if(type_argument_count==0)return;
    emit_byte(compiler,(uint8_t)type_argument_count);
    for(size_t index=0;index<type_argument_count;index++)
        emit_register(compiler,type_arguments[index]);
}

/* The shared head of a CALL_SINGLETON_KEYWORDS/_SPREAD (or typed) call:
 * destination, callee, argument register, literal receiver class and
 * whether the callee takes a receiver. Returns the destination. */
static uint16_t emit_singleton_call_head(Compiler *compiler,DiamondOpCode opcode,
        const DiamondMethod *method,uint16_t arguments,int receiver_class_index) {
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,opcode);
    emit_register(compiler,destination);
    emit_function_index(compiler,method->function_index);
    emit_register(compiler,arguments);
    emit_register(compiler,receiver_class_index<0?DIAMOND_NO_CLASS:
        (uint16_t)receiver_class_index);
    emit_byte(compiler,method->needs_receiver?1:0);
    return destination;
}

static int32_t homogeneous_spread_expectation(Compiler *compiler,
        const DiamondFunction *target,size_t first,size_t parameter_count);
static uint16_t compile_callable_value_block(Compiler *compiler,
        int32_t callable_set_index);

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

/* Discovery pass only: an unresolved `name` inside a nested def's body
 * that is that def's own name makes it a recursive local helper. */
void note_self_reference(Compiler *compiler,DiamondSpan name) {
    if(!compiler->discovery_pass||compiler->current_definition==nullptr)return;
    const DiamondSpan own=compiler->current_definition_name;
    if(own.length==name.length&&
       memcmp(compiler->source+own.start,compiler->source+name.start,name.length)==0)
        compiler->current_definition->calls_itself=true;
}

/* The discovery pass can encounter a call before its top-level function's
 * signature has been seen. It only needs to keep walking far enough to record
 * that later declaration; the real pass will validate the call against the
 * copied signature. Parse the complete call shape here without emitting a
 * callable function index that does not exist in this throwaway program yet. */
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

/* A trailing do block fills the callee's last parameter. When that
 * parameter's declared type can't hold a Callable -- `message: String`
 * after `action: Callable[0]` -- the call would fail at runtime with a
 * confusing "missing argument", so say what happened here instead. */
static bool block_slot_rejects_callable(const DiamondFunction *target,size_t parameter_index) {
    if(target==nullptr||parameter_index>=DIAMOND_MAX_DECLARED_PARAMETERS)return false;
    const uint16_t set_index=target->parameter_type_sets[parameter_index];
    if(set_index==DIAMOND_NO_TYPE_SET||set_index>=target->type_set_count)return false;
    const DiamondTypeSet *set=&target->type_sets[set_index];
    if(set->count==0)return false;
    for(size_t index=0;index<set->count;index++) {
        const uint16_t id=set->members[index].id;
        const bool plain=id==DIAMOND_TYPE_INT||id==DIAMOND_TYPE_FLOAT||
            id==DIAMOND_TYPE_STRING||id==DIAMOND_TYPE_BOOL||id==DIAMOND_TYPE_NIL||
            id==DIAMOND_TYPE_ARRAY||id==DIAMOND_TYPE_HASH||id==DIAMOND_TYPE_SYMBOL||
            (id>=DIAMOND_TYPE_CLASS_BASE&&id<DIAMOND_TYPE_CLASS_LIMIT)||
            id==DIAMOND_TYPE_NATIVE;
        if(!plain)return false;
    }
    return true;
}

uint16_t compile_contextual_typed_block(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_index,
        const uint16_t *type_arguments,size_t type_argument_count) {
    if(!compiler->discovery_pass&&block_slot_rejects_callable(target,parameter_index)) {
        char message[DIAMOND_MAX_FUNCTION_NAME*3+160];
        snprintf(message,sizeof message,
            "a do block fills the last parameter of '%s', '%s', which does not take a "
            "Callable; pass the callable as an argument or give '%s' a &block parameter",
            target->name,target->parameter_names[parameter_index],target->name);
        fail(compiler,compiler->current.span,message);
        return 0;
    }
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
    if(target->class_type_variable||parameter>=DIAMOND_MAX_DECLARED_PARAMETERS)return;
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

/* `name(...)` where name is a local holding a callable. */
static uint16_t parse_local_callable_call(Compiler *compiler, int callable_local) {
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

/* `block_given?()`: the current token is the '('. */
static uint16_t parse_block_given_call(Compiler *compiler) {
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

/* A call to a name that is not a function. Discovery tolerates it (the
 * function may be declared later); otherwise it is an error, with a hint when
 * a module function of that name exists. */
static uint16_t parse_unknown_function_call(Compiler *compiler, DiamondSpan name) {
    note_self_reference(compiler,name);
    if(compiler->discovery_pass)
        return parse_discovery_unknown_call(compiler);
    /* A module function of that name is reached through its module. */
    for(size_t module=0;module<compiler->program->module_count;module++) {
        const DiamondModule *owner=compiler->program->modules[module];
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

/* Explicit generic arguments, `[Type, ...]`, up to 8: the current token is the
 * '['; consumes through the ']'. Callers apply their own count checks. */
bool parse_explicit_type_arguments(Compiler *compiler,uint16_t *type_arguments,
        size_t *type_argument_count_out) {
    size_t type_argument_count=0;
    advance_token(compiler);
    skip_newlines(compiler);
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
          !compiler->failed) {
        if(type_argument_count==8) {
            fail(compiler,compiler->current.span,"too many generic arguments");
            return false;
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
             "expected ']' after generic arguments");return false;
    }
    advance_token(compiler);
    *type_argument_count_out=type_argument_count;
    return true;
}

/* Explicit generic arguments, `name[Type, ...](...)`: the current token is
 * the '['. */
static bool parse_call_type_arguments(Compiler *compiler, const DiamondFunction *function,
        DiamondSpan name, uint16_t *type_arguments, size_t *type_argument_count_out) {
    size_t type_argument_count=0;
    if(!parse_explicit_type_arguments(compiler,type_arguments,&type_argument_count))
        return false;
    if(type_argument_count!=function->type_variable_count) {
        fail(compiler,name,"wrong number of generic arguments");return false;
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             "expected '(' after generic arguments");return false;
    }
    *type_argument_count_out=type_argument_count;
    return true;
}

/* A function call whose arguments include a spread: the current token is the
 * first argument. */
static uint16_t parse_spread_function_call(Compiler *compiler, const DiamondFunction *function,
        int function_index, const uint16_t *type_arguments, size_t type_argument_count) {
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
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    publish_call_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
    return destination;
}

/* Parses a call's positional and keyword arguments, through the closing
 * ')', into the parameter slots they fill. */
static bool parse_call_slot_arguments(Compiler *compiler, const DiamondFunction *function,
        DiamondSpan name, uint16_t *slot_registers, bool *slot_filled) {
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
                    return false;
                }
                seen_keyword=true;
            } else {
                if(seen_keyword) {
                    fail(compiler,compiler->current.span,
                         "positional argument cannot follow a keyword argument");
                    return false;
                }
                if(next_positional_slot==DIAMOND_MAX_ARGUMENTS) {
                    fail(compiler,compiler->current.span,"too many call arguments");
                    return false;
                }
                slot=next_positional_slot++;
            }
            if(slot_filled[slot]) {
                fail(compiler,name,"multiple values for the same argument");
                return false;
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
        return false;
    }
    advance_token(compiler);
    return true;
}

/* A direct call whose keyword arguments leave a defaulted parameter unfilled
 * (`f(1, c: 5)` against `def f(a, b = 2, c = 3)`). CALL's arguments are
 * contiguous, so this goes through CALL_CLOSURE_KEYWORDS instead: the slots
 * before the first gap are positional and every later filled slot is passed
 * by name. The runtime merge leaves each gap UNDEFINED, which
 * ARGUMENT_PROVIDED reads as "not supplied", so the default runs. block is
 * the trailing `do` block's register when has_block. */
static uint16_t emit_sparse_keyword_call(Compiler *compiler,const DiamondFunction *function,
        int function_index,const uint16_t *slot_registers,const bool *slot_filled,
        size_t argument_count,bool has_block,uint16_t block,
        const uint16_t *resolved_arguments,size_t resolved_count,DiamondSpan name) {
    size_t first_gap=0;
    while(first_gap<argument_count&&slot_filled[first_gap])first_gap++;
    const uint16_t callable=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,callable);
    emit_function_index(compiler,(size_t)function_index);emit_byte(compiler,0);
    const uint16_t positional=emit_argument_array(compiler,slot_registers,first_gap);
    uint16_t names[DIAMOND_MAX_ARGUMENTS];
    size_t keyword_count=0;
    for(size_t slot=first_gap;slot<argument_count;slot++)
        if(slot_filled[slot])
            names[keyword_count++]=add_name_text(compiler,function->parameter_names[slot],
                strlen(function->parameter_names[slot]),name);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
    emit_register(compiler,destination);emit_register(compiler,callable);
    emit_register(compiler,positional);
    emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
    size_t keyword=0;
    for(size_t slot=first_gap;slot<argument_count;slot++)
        if(slot_filled[slot]) {
            emit_register(compiler,names[keyword++]);
            emit_register(compiler,slot_registers[slot]);
        }
    if(has_block)emit_register(compiler,block);
    publish_call_return_type(compiler,destination,function,resolved_arguments,resolved_count);
    return destination;
}

/* A trailing `do ... end` block with no parameter left to land in. Ruby
 * passes blocks implicitly; Diamond only binds one through `&block`, so
 * say that instead of a bare "wrong number of arguments". */
static bool fail_unaccepted_block(Compiler *compiler,const DiamondFunction *function,
        size_t arity,bool variadic,size_t argument_count,DiamondSpan name) {
    if(variadic||argument_count<arity||(function&&function->has_block_parameter))
        return false;
    char message[sizeof compiler->diagnostic->message];
    snprintf(message,sizeof message,
        "'%.*s' does not take a block; add a &block parameter to accept one",
        (int)name.length,compiler->source+name.start);
    fail(compiler,name,message);
    return true;
}

/* True when every unfilled slot below argument_count is a defaulted
 * parameter, so a sparse keyword call can leave it to its default. */
static bool gaps_are_defaulted(const DiamondFunction *function,const bool *slot_filled,
        size_t argument_count) {
    for(size_t index=0;index<argument_count;index++)
        if(!slot_filled[index]&&index<function->required_arity)return false;
    return true;
}

uint16_t parse_call(Compiler *compiler, DiamondSpan name) {
    const int callable_local=find_local(compiler,name);
    if(callable_local>=0&&compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
        return parse_local_callable_call(compiler,callable_local);
    if(name_equals(compiler,"block_given?",name,false))
        return parse_block_given_call(compiler);
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
    if (function_index < 0)
        return parse_unknown_function_call(compiler,name);
    const DiamondFunction *function =
        compiler->program->functions[(size_t)function_index];
    uint16_t type_arguments[8];
    size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
       !parse_call_type_arguments(compiler,function,name,type_arguments,
           &type_argument_count))
        return 0;
    advance_token(compiler);
    skip_newlines(compiler);
    if(call_arguments_have_spread(compiler))
        return parse_spread_function_call(compiler,function,function_index,
            type_arguments,type_argument_count);
    /* Keyword arguments (direct top-level calls only):
     * each argument is placed into its declared positional slot rather than
     * appended, so a keyword can fill any parameter regardless of the order
     * it's written at the call site. Positional arguments still fill slots
     * left-to-right in declaration order. */
    uint16_t slot_registers[DIAMOND_MAX_ARGUMENTS];
    bool slot_filled[DIAMOND_MAX_ARGUMENTS]={};
    if(!parse_call_slot_arguments(compiler,function,name,slot_registers,slot_filled))
        return 0;
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
        if(fail_unaccepted_block(compiler,function,function->arity,
            function->has_variadic,argument_count,name))return 0;
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
        if(type_argument_count==0&&!contiguous&&argument_count<=block_slot&&
           gaps_are_defaulted(function,slot_filled,argument_count)) {
            for(size_t slot=0;slot<argument_count;slot++)
                if(slot_filled[slot])
                    check_call_argument_types(compiler,function,&slot_registers[slot],1,slot,name);
            return emit_sparse_keyword_call(compiler,function,function_index,slot_registers,
                slot_filled,argument_count,true,block,resolved_arguments,resolved_count,name);
        }
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
    /* A slot below the highest filled one may stay empty only when its
     * parameter has a default; emit_sparse_keyword_call then passes the
     * later slots by name. A required parameter left empty is an error, and
     * so is a gap with explicit type arguments, which CALL_CLOSURE_KEYWORDS
     * cannot carry. */
    bool has_gap=false;
    for(size_t index=0;index<argument_count;index++)
        if(!slot_filled[index])has_gap=true;
    if(has_gap) {
        if(type_argument_count>0||!gaps_are_defaulted(function,slot_filled,argument_count)) {
            fail(compiler,name,"missing argument");
            return 0;
        }
        if(argument_count>function->arity&&!function->has_variadic) {
            fail(compiler,name,"wrong number of arguments");
            return 0;
        }
        for(size_t slot=0;slot<argument_count;slot++)
            if(slot_filled[slot])
                check_call_argument_types(compiler,function,&slot_registers[slot],1,slot,name);
        return emit_sparse_keyword_call(compiler,function,function_index,slot_registers,
            slot_filled,argument_count,false,0,resolved_arguments,resolved_count,name);
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
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
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
    if(target==nullptr||target->class_type_variable||target->type_variable_count==0||
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
        emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    } else {
        /* An argument the reference wasn't called with is nil by now in
         * this frame, so pass fewer instead and let the target's default
         * run -- parse_bound_method_reference's ARGUMENT_PROVIDED chain. */
        for(size_t count=method->required_arity;count<method->arity;count++) {
            const uint16_t provided=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,provided,
                (uint16_t)count,0,2);
            const size_t next=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,provided);
            const uint16_t partial=emit_singleton_call(compiler,method,
                receiver_class_index,arguments,count,namespace_name,
                (uint8_t)type_argument_count,type_arguments);
            emit_instruction(compiler,DIAMOND_OP_RETURN,partial,0,0,1);
            patch_jump(compiler,next,compiler->function->code_count);
        }
        body_result=emit_singleton_call(compiler,method,
            receiver_class_index,arguments,method->arity,namespace_name,
            (uint8_t)type_argument_count,type_arguments);
    }
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

/* `Namespace.method[Type, ...]`: the current token is the '['. */
static bool parse_singleton_type_arguments(Compiler *compiler,const DiamondFunction *function,
        DiamondSpan name,uint16_t *type_arguments,size_t *type_argument_count_out) {
    size_t type_argument_count=0;
    if(!parse_explicit_type_arguments(compiler,type_arguments,&type_argument_count))
        return false;
    if(function==nullptr) {
        fail(compiler,name,
            "explicit generic arguments are not supported on a forward-"
            "referenced module_function call");return false;
    }
    if(type_argument_count!=function->type_variable_count) {
        fail(compiler,name,"wrong number of generic arguments");return false;
    }
    *type_argument_count_out=type_argument_count;
    return true;
}

/* A singleton call whose arguments include a keyword argument: the current
 * token is the first argument. */
static uint16_t parse_singleton_keyword_call(Compiler *compiler,const DiamondMethod *method,const DiamondFunction *function,
        int receiver_class_index,const uint16_t *type_arguments,size_t type_argument_count) {
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
    const uint16_t destination=emit_singleton_call_head(compiler,type_argument_count==0?
        DIAMOND_OP_CALL_SINGLETON_KEYWORDS:DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS,
        method,positional,receiver_class_index);
    emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
    for(size_t index=0;index<keyword_count;index++) {
        emit_register(compiler,add_name_string(compiler,keyword_names[index]));
        emit_register(compiler,keyword_values[index]);
    }
    if(has_block)emit_register(compiler,block);
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    publish_call_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
    return destination;
}

/* A singleton call whose arguments include a spread: the current token is
 * the first argument. */
static uint16_t parse_singleton_spread_call(Compiler *compiler,const DiamondMethod *method,const DiamondFunction *function,
        int receiver_class_index,const uint16_t *type_arguments,size_t type_argument_count) {
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
        const uint16_t destination=emit_singleton_call_head(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_KEYWORDS:DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS,
            method,spread,receiver_class_index);
        emit_byte(compiler,0x80u);emit_register(compiler,block);
        emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
        publish_call_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    const uint16_t destination=emit_singleton_call_head(compiler,type_argument_count==0?
        DIAMOND_OP_CALL_SINGLETON_SPREAD:DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD,
        method,spread,receiver_class_index);
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    publish_call_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
    return destination;
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
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
       !parse_singleton_type_arguments(compiler,function,name,type_arguments,
           &type_argument_count))
        return 0;
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        /* `Namespace.method`, no call following -- a bare reference to
         * the method as a value (parse_singleton_reference, above)
         * rather than a call. Explicit generic arguments
         * (`Namespace.method[T]`) are baked into the reference
         * wrapper's typed call. */
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
    if(call_arguments_have_keyword(compiler))
        return parse_singleton_keyword_call(compiler,method,function,receiver_class_index,type_arguments,type_argument_count);
    if(call_arguments_have_spread(compiler))
        return parse_singleton_spread_call(compiler,method,function,receiver_class_index,type_arguments,type_argument_count);
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
        if(fail_unaccepted_block(compiler,function,method->arity,
            method->has_variadic,argument_count,name))return 0;
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
            const uint16_t destination=emit_singleton_call_head(compiler,type_argument_count==0?
                DIAMOND_OP_CALL_SINGLETON_KEYWORDS:DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS,
                method,positional,receiver_class_index);
            emit_byte(compiler,0x80u);emit_register(compiler,block);
            emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
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
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
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
