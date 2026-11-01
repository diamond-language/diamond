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

/* Control-flow statements: return, yield, raise, begin/rescue/ensure, retry, and loop control. */

/* Is the `if`/`unless` at the current token a one-line if-expression
 * (`if ok then 0 else 1 end`) rather than a trailing modifier? A modifier's
 * condition never contains `then`, so a `then` later on the same line
 * settles it. */
static bool if_expression_follows(const Compiler *compiler) {
    DiamondLexer lookahead=compiler->lexer;
    for(DiamondToken token=diamond_lexer_next(&lookahead);
        token.kind!=DIAMOND_TOKEN_NEWLINE&&token.kind!=DIAMOND_TOKEN_EOF;
        token=diamond_lexer_next(&lookahead))
        if(token.kind==DIAMOND_TOKEN_THEN)return true;
    return false;
}

/* Is there no value after `return`/`break`/`next`? A trailing modifier
 * (`return if done`) starts no value; an if-expression does. */
static bool at_statement_end(Compiler *compiler) {
    if(compiler->current.kind==DIAMOND_TOKEN_IF||
       compiler->current.kind==DIAMOND_TOKEN_UNLESS)
        return !if_expression_follows(compiler);
    return compiler->current.kind==DIAMOND_TOKEN_NEWLINE ||
           compiler->current.kind==DIAMOND_TOKEN_END ||
           compiler->current.kind==DIAMOND_TOKEN_ELSE ||
           compiler->current.kind==DIAMOND_TOKEN_EOF;
}

static uint16_t nil_register(Compiler *compiler) {
    const uint16_t value=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    compiler->known_types[value]=DIAMOND_TYPE_NIL;
    return value;
}

/* `return` inside a do-block returns from the enclosing def, Ruby-style:
 * DIAMOND_OP_BLOCK_RETURN unwinds every frame between the block and that
 * def's frame, running their ensure handlers on the way. The value is
 * checked against the def's `-> Type` here, in the block, since the def's
 * own RETURN is skipped. */
static uint16_t compile_block_return(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    if(compiler->method_function==nullptr) {
        fail(compiler,keyword,
             "'return' in a block outside any def; use 'next' to end the block");
        return 0;
    }
    advance_token(compiler);
    int32_t expected=-1;
    if(compiler->method_return_type>=0)
        expected=clone_type_set_into_current(compiler,
            compiler->method_function->type_sets,
            compiler->method_function->type_set_count,
            (uint16_t)compiler->method_return_type);
    if(expected==DIAMOND_NO_TYPE_SET)expected=-1;
    const uint16_t value=at_statement_end(compiler)?nil_register(compiler):
        expected>=0?parse_with_expected_set(compiler,(uint16_t)expected):
        parse_expression(compiler);
    if(compiler->method_function->returns_self) {
        const uint16_t others=compiler->method_function->self_return_others;
        emit_self_return_check(compiler,value,others==DIAMOND_NO_TYPE_SET?others:
            clone_type_set_into_current(compiler,compiler->method_function->type_sets,
                compiler->method_function->type_set_count,others));
    } else if(expected>=0)
        emit_type_check(compiler,value,(uint16_t)expected,
                        compiler->method_return_type_span);
    compiler->method_function->nonlocal_landing=true;
    compiler->method_has_block_return=true;
    emit_instruction(compiler,DIAMOND_OP_BLOCK_RETURN,value,0,0,1);
    return value;
}

uint16_t compile_return(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    if(!compiler->in_function) {
        fail(compiler,keyword,"'return' used outside a function");
        return 0;
    }
    if(compiler->in_block&&compiler->current.kind==DIAMOND_TOKEN_RETURN)
        return compile_block_return(compiler);
    advance_token(compiler);
    uint16_t value=0;
    if(at_statement_end(compiler)) {
        value=nil_register(compiler);
    } else {
        value=compiler->current_return_type>=0?
            parse_with_expected_set(compiler,
                (uint16_t)compiler->current_return_type):
            parse_expression(compiler);
    }
    if(compiler->function->returns_self)
        emit_self_return_check(compiler,value,compiler->function->self_return_others);
    else if(compiler->current_return_type>=0)
        emit_type_check(compiler,value,(uint16_t)compiler->current_return_type,
                        compiler->current_return_type_span);
    /* See return_flow_seen's own comment (the Compiler struct) --
     * compile_definition's inference reads this back once the whole body
     * finishes compiling; harmless to keep accumulating even when an
     * explicit `-> Type` annotation already makes that inference moot. */
    const int32_t tooling_set=compiler->tooling_type_sets[value]>=0?
        compiler->tooling_type_sets[value]:compiler->known_type_sets[value];
    if(!compiler->return_flow_seen) {
        compiler->return_tooling_type=compiler->known_types[value];
        compiler->return_tooling_set=tooling_set;
    } else {
        merge_flow_types(compiler,compiler->return_tooling_type,
            compiler->return_tooling_set,compiler->known_types[value],tooling_set,
            &compiler->return_tooling_type,&compiler->return_tooling_set);
    }
    if(!compiler->return_flow_seen) {
        compiler->return_flow_type=compiler->known_types[value];
        compiler->return_flow_set=compiler->known_type_sets[value];
        compiler->return_flow_seen=true;
    } else {
        merge_flow_types(compiler,compiler->return_flow_type,compiler->return_flow_set,
            compiler->known_types[value],compiler->known_type_sets[value],
            &compiler->return_flow_type,&compiler->return_flow_set);
    }
    maybe_rewrite_self_tail_call(compiler,value);
    emit_instruction(compiler,DIAMOND_OP_RETURN,value,0,0,1);
    return value;
}

uint16_t compile_yield(Compiler *compiler) {
    if(compiler->has_current_block) {
        const uint16_t block=load_current_block(compiler);
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,block,
            callable_type_set_index(compiler),0,2);
        uint16_t destination=0;
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN) {
            destination=parse_closure_call_arguments(compiler,block);
        } else {
            const uint16_t base=allocate_register(compiler);
            destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE);
            emit_register(compiler,destination);
            emit_register(compiler,block);
            emit_register(compiler,base);emit_byte(compiler,0);
        }
        const uint16_t block_set=compiler->current_block_type_set;
        if(block_set!=DIAMOND_NO_TYPE_SET&&
           block_set<compiler->function->type_set_count) {
            const DiamondTypeSet *set=&compiler->function->type_sets[block_set];
            uint16_t return_set=DIAMOND_NO_TYPE_SET;
            bool consistent=set->count>0;
            for(size_t index=0;index<set->count;index++) {
                const DiamondTypeMember *member=&set->members[index];
                if(member->id!=DIAMOND_TYPE_CALLABLE||
                   member->callable_return_set==DIAMOND_NO_TYPE_SET) {
                    consistent=false;break;
                }
                if(return_set==DIAMOND_NO_TYPE_SET)
                    return_set=member->callable_return_set;
                else if(return_set!=member->callable_return_set) {
                    consistent=false;break;
                }
            }
            if(consistent&&return_set<compiler->function->type_set_count) {
                /* As in publish_callable_return_type: check before trusting. */
                if(compiler->known_type_sets[destination]!=(int32_t)return_set)
                    emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,destination,
                        return_set,0,2);
                compiler->known_type_sets[destination]=(int32_t)return_set;
                const DiamondTypeSet *returns=
                    &compiler->function->type_sets[return_set];
                if(returns->count==1)
                    compiler->known_types[destination]=returns->members[0].id;
            }
        }
        return destination;
    }
    uint16_t source;
    if (compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN) {
        advance_token(compiler);
        skip_newlines(compiler);
        if (compiler->current.kind == DIAMOND_TOKEN_RIGHT_PAREN) {
            /* `yield()` yields nil, like `Fiber.yield()` and bare `yield`. */
            source = allocate_register(compiler);
            emit_instruction(compiler, DIAMOND_OP_NIL, source, 0, 0, 1);
        } else {
            source = parse_expression(compiler);
            skip_newlines(compiler);
        }
        if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler, compiler->current.span, "expected ')' after yield value");
            return 0;
        }
        advance_token(compiler);
    } else {
        source = allocate_register(compiler);
        /* Sole writer; run_chunk's zero-init already covers this. */
    }
    const uint16_t dest = allocate_register(compiler);
    emit_instruction(compiler, DIAMOND_OP_YIELD, dest, source, 0, 2);
    return dest;
}

uint16_t compile_raise(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE ||
       compiler->current.kind==DIAMOND_TOKEN_END ||
       compiler->current.kind==DIAMOND_TOKEN_ELSE ||
       compiler->current.kind==DIAMOND_TOKEN_ENSURE ||
       compiler->current.kind==DIAMOND_TOKEN_IF ||
       compiler->current.kind==DIAMOND_TOKEN_UNLESS ||
       compiler->current.kind==DIAMOND_TOKEN_EOF) {
        if(compiler->current_exception<0) {
            fail(compiler,keyword,"bare 'raise' used outside rescue");return 0;
        }
        emit_instruction(compiler,DIAMOND_OP_RAISE,
                         (uint8_t)compiler->current_exception,0,0,1);
        return (uint8_t)compiler->current_exception;
    }
    const uint16_t value=parse_expression(compiler);
    emit_instruction(compiler,DIAMOND_OP_RAISE,value,0,0,1);
    return value;
}

/* Snapshots every Local in compiler->locals[start_index, end_index) --
 * a scope that's about to close, whether a whole function body
 * (compile_definition) or just one `rescue` clause (compile_begin) --
 * into compiler->function->scope_locals (DiamondScopeLocal, src/vm.h).
 * A local's own valid_start is just its name token's own byte offset
 * (already exactly right: the moment its declaring `Local` was pushed
 * is the moment it became valid), so the only new information this
 * needs from the caller is `valid_end`, the closing scope's own byte
 * offset. Silently stops recording past DIAMOND_MAX_SCOPE_LOCALS
 * (see that constant's own comment) rather than failing compilation --
 * this is an LSP-only convenience feature (docs/lsp.md), losing some
 * completions for a pathological function isn't worth a hard error
 * over. */
void record_scope_locals(Compiler *compiler,size_t start_index,
        size_t end_index,size_t valid_end) {
    DiamondFunction *function=compiler->function;
    for(size_t index=start_index;index<end_index;index++) {
        if(function->scope_local_count==DIAMOND_MAX_SCOPE_LOCALS)return;
        const Local *local=&compiler->locals[index];
        DiamondScopeLocal *recorded=
            &function->scope_locals[function->scope_local_count++];
        size_t length=local->name.length;
        if(length>=DIAMOND_MAX_FUNCTION_NAME)length=DIAMOND_MAX_FUNCTION_NAME-1;
        for(size_t char_index=0;char_index<length;char_index++)
            recorded->name[char_index]=compiler->source[local->name.start+char_index];
        recorded->name[length]='\0';
        recorded->valid_start=local->name.start;
        recorded->valid_end=valid_end;
        recorded->reg=local->reg;
        recorded->known_type=compiler->known_types[local->reg];
        recorded->known_type_set=compiler->known_type_sets[local->reg];
        recorded->tooling_type_set=compiler->tooling_type_sets[local->reg];
    }
}

void record_scope_type_fact(Compiler *compiler,uint16_t reg,
        size_t effective_start) {
    DiamondFunction *function=compiler->function;
    if(function->scope_type_fact_count==DIAMOND_MAX_SCOPE_TYPE_FACTS)return;
    DiamondScopeTypeFact *fact=
        &function->scope_type_facts[function->scope_type_fact_count++];
    *fact=(DiamondScopeTypeFact){.reg=reg,.effective_start=effective_start,
        .known_type=compiler->known_types[reg],
        .known_type_set=compiler->known_type_sets[reg],
        .tooling_type_set=compiler->tooling_type_sets[reg]};
}

/* The handlers a begin block installs before its body: the operands its
 * rescue/else/ensure clauses patch once they are compiled. */
typedef struct BeginFrame {
    size_t ensure_operand;
    size_t handler_type_operand;
    size_t handler_operand;
    size_t retry_target;
    uint16_t exception;
} BeginFrame;

static void open_begin_frame(Compiler *compiler,BeginFrame *frame) {
    compiler->begin_depth++;
    frame->ensure_operand=compiler->function->code_count+1;
    emit_opcode(compiler,DIAMOND_OP_PUSH_ENSURE);
    emit_byte(compiler,0);emit_byte(compiler,0);
    frame->exception=allocate_register(compiler);
    frame->retry_target=compiler->function->code_count;
    /* Byte layout after PUSH_RESCUE's opcode: exception (2-byte register,
     * emit_register below), then this 1-byte catch-all flag, then 8
     * 1-byte rescue-type-id slots, then a 2-byte jump-target placeholder
     * -- opcode(+0) + exception(+1,+2) puts the flag at +3 and the jump
     * placeholder at +3+1+8=+12. */
    frame->handler_type_operand=compiler->function->code_count+3;
    frame->handler_operand=compiler->function->code_count+12;
    emit_opcode(compiler,DIAMOND_OP_PUSH_RESCUE);
    emit_register(compiler,frame->exception);emit_byte(compiler,0x80);
    for(size_t i=0;i<8;i++)emit_byte(compiler,0);
    emit_byte(compiler,0);emit_byte(compiler,0);
}

/* Compiles the rescue/else/ensure clauses after `body`, up to but not
 * including the closing `end`, and returns the register holding the
 * block's value. */
static uint16_t close_begin_frame(Compiler *compiler,const BeginFrame *frame,uint16_t body) {
    const size_t ensure_operand=frame->ensure_operand;
    const size_t handler_type_operand=frame->handler_type_operand;
    const size_t handler_operand=frame->handler_operand;
    const size_t retry_target=frame->retry_target;
    const uint16_t exception=frame->exception;
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,body,0,2);
    emit_instruction(compiler,DIAMOND_OP_POP_RESCUE,0,0,0,0);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,handler_operand,compiler->function->code_count);
    size_t rescue_end_jumps[16];size_t rescue_count=0;
    uint16_t seen_rescue_types[128];size_t seen_rescue_type_count=0;
    bool saw_rescue=false,catch_all=false;
    while(compiler->current.kind==DIAMOND_TOKEN_RESCUE&&!compiler->failed) {
        if(catch_all) {
            fail(compiler,compiler->current.span,
                 "rescue clause after catch-all is unreachable");break;
        }
        if(rescue_count==16) {
            fail(compiler,compiler->current.span,"too many rescue clauses");break;
        }
        saw_rescue=true;
        advance_token(compiler);
        compiler->function->code[handler_type_operand]=0;
        const size_t rescue_local_count=compiler->local_count;
        if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER) {
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->current.span,"too many local variables");
                return destination;
            }
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->current.span,.reg=exception,
                .captured=compiler->loop_captures_pending};
            advance_token(compiler);
        }
        uint16_t rescue_types[8];size_t type_count=0;
        if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
            advance_token(compiler);
            while(!compiler->failed) {
                if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                    fail(compiler,compiler->current.span,"expected rescue type");break;
                }
                if(type_count==8) {
                    fail(compiler,compiler->current.span,"too many rescue types");break;
                }
                const DiamondSpan rescue_type_span=compiler->current.span;
                char rescue_type_name[DIAMOND_MAX_FUNCTION_NAME];
                if(!consume_qualified_name(compiler,rescue_type_name,
                                           sizeof rescue_type_name)) {
                    fail(compiler,rescue_type_span,"rescue type name is too long");break;
                }
                const int rescue_resolved=resolve_type_name(
                    compiler,rescue_type_name,rescue_type_span);
                if(diamond_type_operand_is_native(rescue_resolved)) {
                    fail(compiler,rescue_type_span,"rescue type must be an exception class");
                    break;
                }
                const uint16_t rescue_type=(uint16_t)rescue_resolved;
                for(size_t existing=0;existing<type_count;existing++)
                    if(rescue_types[existing]==rescue_type)
                        fail(compiler,rescue_type_span,
                             "duplicate rescue type");
                for(size_t existing=0;existing<seen_rescue_type_count;existing++)
                    if(seen_rescue_types[existing]==rescue_type)
                        fail(compiler,rescue_type_span,
                             "rescue type was already handled");
                    else if(seen_rescue_types[existing]>=DIAMOND_TYPE_CLASS_BASE&&
                            rescue_type>=DIAMOND_TYPE_CLASS_BASE) {
                        size_t child=(size_t)(rescue_type-DIAMOND_TYPE_CLASS_BASE);
                        const size_t ancestor=(size_t)(
                            seen_rescue_types[existing]-DIAMOND_TYPE_CLASS_BASE);
                        while(child<compiler->program->class_count&&child!=ancestor&&
                              compiler->program->classes[child]->superclass!=DIAMOND_NO_CLASS)
                            child=compiler->program->classes[child]->superclass;
                        if(child==ancestor)
                            fail(compiler,rescue_type_span,
                                 "rescue type is covered by an earlier clause");
                    }
                rescue_types[type_count++]=rescue_type;
                seen_rescue_types[seen_rescue_type_count++]=rescue_type;
                if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
                advance_token(compiler);
            }
        }
        size_t match_jumps[8];size_t mismatch_jump=SIZE_MAX;
        for(size_t index=0;index<type_count;index++) {
            const uint16_t matched=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,matched,exception,
                             rescue_types[index],3);
            match_jumps[index]=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,matched);
        }
        if(type_count>0)mismatch_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
        if(!consume_block_start(compiler))return destination;
        for(size_t index=0;index<type_count;index++)
            patch_jump(compiler,match_jumps[index],compiler->function->code_count);
        const int outer_exception=compiler->current_exception;
        const size_t outer_retry_target=compiler->current_retry_target;
        compiler->current_exception=exception;
        compiler->current_retry_target=retry_target;
        const uint16_t rescued=compile_sequence(compiler);
        compiler->current_exception=outer_exception;
        compiler->current_retry_target=outer_retry_target;
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,rescued,0,2);
        if(!compiler->failed)
            record_scope_locals(compiler,rescue_local_count,compiler->local_count,
                compiler->previous.span.start+compiler->previous.span.length);
        compiler->local_count=rescue_local_count;
        rescue_end_jumps[rescue_count++]=emit_jump(compiler,DIAMOND_OP_JUMP,0);
        if(mismatch_jump!=SIZE_MAX)
            patch_jump(compiler,mismatch_jump,compiler->function->code_count);
        else catch_all=true;
    }
    if(saw_rescue&&!catch_all)
        emit_instruction(compiler,DIAMOND_OP_RAISE,exception,0,0,1);
    if(compiler->current.kind!=DIAMOND_TOKEN_ENSURE&&!saw_rescue) {
        fail(compiler,compiler->current.span,"expected 'rescue' or 'ensure' after begin body");
        return destination;
    }
    patch_jump(compiler,end_jump,compiler->function->code_count);
    if(compiler->current.kind==DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(!consume_block_start(compiler))return destination;
        const uint16_t normal=compile_sequence(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,normal,0,2);
    }
    for(size_t index=0;index<rescue_count;index++)
        patch_jump(compiler,rescue_end_jumps[index],compiler->function->code_count);
    emit_opcode(compiler,DIAMOND_OP_RUN_ENSURE);
    const size_t continuation_operand=compiler->function->code_count;
    emit_byte(compiler,0);emit_byte(compiler,0);
    patch_jump(compiler,ensure_operand,compiler->function->code_count);
    if(compiler->current.kind==DIAMOND_TOKEN_ENSURE) {
        advance_token(compiler);
        if(!consume_block_start(compiler))return destination;
        (void)compile_sequence(compiler);
    }
    emit_instruction(compiler,DIAMOND_OP_END_ENSURE,0,0,0,0);
    patch_jump(compiler,continuation_operand,compiler->function->code_count);
    compiler->begin_depth--;
    return destination;
}

uint16_t compile_begin(Compiler *compiler) {
    if(!consume_block_start(compiler))return 0;
    BeginFrame frame;
    open_begin_frame(compiler,&frame);
    const uint16_t body=compile_sequence(compiler);
    const uint16_t destination=close_begin_frame(compiler,&frame,body);
    if(compiler->failed)return destination;
    if(compiler->current.kind!=DIAMOND_TOKEN_END) {
        fail(compiler,compiler->current.span,"expected 'end' after begin body");
        return destination;
    }
    advance_token(compiler);
    return destination;
}

static bool implicit_begin_recorded(const Compiler *compiler,size_t offset) {
    const ImplicitBegins *set=compiler->implicit_begins;
    if(set==nullptr)return false;
    for(size_t index=0;index<set->count;index++)
        if(set->offsets[index]==offset)return true;
    return false;
}

static void record_implicit_begin(Compiler *compiler,size_t offset) {
    ImplicitBegins *set=compiler->implicit_begins;
    if(set==nullptr)return;
    if(set->count==set->capacity) {
        const size_t capacity=set->capacity==0?8:set->capacity*2;
        size_t *grown=realloc(set->offsets,capacity*sizeof *grown);
        if(grown==nullptr) {
            fail(compiler,compiler->current.span,"out of memory");return;
        }
        set->offsets=grown;set->capacity=capacity;
    }
    set->offsets[set->count++]=offset;
}

/* A def or do-block body, which may end in rescue/else/ensure clauses of
 * its own the way a begin block does: `def f ... rescue ... end`. Stops
 * at the closing `end` without consuming it. *wrapped says whether the
 * body had handlers, so the caller knows its value may come from one. */
uint16_t compile_body(Compiler *compiler,bool *wrapped) {
    const size_t offset=compiler->current.span.start;
    BeginFrame frame;
    *wrapped=!compiler->discovery_pass&&implicit_begin_recorded(compiler,offset);
    if(*wrapped)open_begin_frame(compiler,&frame);
    uint16_t body=compile_sequence(compiler);
    const bool has_handlers=compiler->current.kind==DIAMOND_TOKEN_RESCUE||
        compiler->current.kind==DIAMOND_TOKEN_ENSURE;
    if(compiler->failed||(!*wrapped&&!has_handlers))return body;
    if(!*wrapped) {
        if(!compiler->discovery_pass) {
            fail(compiler,compiler->current.span,
                 "internal error: body handlers were not found by the discovery pass");
            return body;
        }
        /* The discovery pass's bytecode is thrown away, so the handlers
         * only need to parse here; the real pass opens them before the
         * body. */
        record_implicit_begin(compiler,offset);
        open_begin_frame(compiler,&frame);
        *wrapped=true;
    }
    return close_begin_frame(compiler,&frame,body);
}

uint16_t compile_retry(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current_retry_target==SIZE_MAX) {
        fail(compiler,keyword,"'retry' used outside rescue");return 0;
    }
    emit_absolute_jump(compiler,compiler->current_retry_target);
    const uint16_t result=allocate_register(compiler);
    /* Dead code: the unconditional jump above means this NIL would
     * never execute at runtime, on top of being a sole-writer register
     * run_chunk's zero-init already covers. */
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}

/* The jump a break/next/redo ends in. From inside begin blocks that sit within the loop it is an
 * UNWIND_JUMP, which leaves those blocks (running their ensure bodies) before landing; otherwise a
 * plain JUMP. Returns the operand offset to patch when the target is not known yet (a break). */
static size_t emit_loop_jump(Compiler *compiler,const LoopContext *loop,size_t target,bool forward) {
    const size_t levels=compiler->begin_depth>loop->begin_depth?
        compiler->begin_depth-loop->begin_depth:0;
    if(levels==0) {
        if(forward)return emit_jump(compiler,DIAMOND_OP_JUMP,0);
        emit_absolute_jump(compiler,target);
        return 0;
    }
    if(levels>UINT8_MAX||target>UINT16_MAX) {
        fail(compiler,compiler->previous.span,"loop control is nested too deeply in begin blocks");
        return 0;
    }
    emit_opcode(compiler,DIAMOND_OP_UNWIND_JUMP);
    emit_byte(compiler,(uint8_t)levels);
    const size_t operand=compiler->function->code_count;
    emit_byte(compiler,(uint8_t)(target>>8));
    emit_byte(compiler,(uint8_t)(target&UINT8_MAX));
    return operand;
}

uint16_t compile_loop_control(Compiler *compiler) {
    const DiamondTokenKind kind=compiler->current.kind;
    const DiamondSpan keyword=compiler->current.span;
    /* `next` in a do-block, outside any loop inside that block, ends this
     * call of the block -- with a value, if given -- which is exactly what
     * `return` already does there. */
    if(compiler->current_loop==nullptr&&kind==DIAMOND_TOKEN_NEXT&&
       compiler->in_block)
        return compile_return(compiler);
    /* `break` in a do-block, outside any loop inside it, ends the call the
     * block was passed to: that call's result becomes the break value. */
    if(compiler->current_loop==nullptr&&kind==DIAMOND_TOKEN_BREAK&&
       compiler->in_block) {
        advance_token(compiler);
        const uint16_t value=at_statement_end(compiler)?nil_register(compiler):
            parse_expression(compiler);
        compiler->block_has_break=true;
        emit_instruction(compiler,DIAMOND_OP_BLOCK_BREAK,value,0,0,1);
        return value;
    }
    if(compiler->current_loop==nullptr) {
        fail(compiler,keyword,kind==DIAMOND_TOKEN_BREAK
            ? "'break' used outside a loop":kind==DIAMOND_TOKEN_NEXT
            ? "'next' used outside a loop":"'redo' used outside a loop");
        return 0;
    }
    advance_token(compiler);
    const bool has_value=compiler->current.kind!=DIAMOND_TOKEN_NEWLINE&&
       compiler->current.kind!=DIAMOND_TOKEN_END&&
       compiler->current.kind!=DIAMOND_TOKEN_ELSE&&
       compiler->current.kind!=DIAMOND_TOKEN_EOF;
    const bool has_modifier = compiler->current.kind == DIAMOND_TOKEN_IF ||
                              compiler->current.kind == DIAMOND_TOKEN_UNLESS;
    const bool actual_value = has_value && !has_modifier;
    if(kind!=DIAMOND_TOKEN_BREAK&&actual_value) {
        fail(compiler,compiler->current.span,
             "next and redo do not accept values");
        return 0;
    }
    if(kind==DIAMOND_TOKEN_BREAK) {
        if(compiler->current_loop->break_count==64) {
            fail(compiler,keyword,"too many break statements in loop");
            return 0;
        }
        uint16_t break_type=DIAMOND_TYPE_NIL;int32_t break_set=-1;
        int32_t break_tooling_set=-1;
        if(actual_value) {
            const uint16_t value=parse_expression(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,
                             compiler->current_loop->result_register,value,0,2);
            break_type=compiler->known_types[value];
            break_set=compiler->known_type_sets[value];
            break_tooling_set=compiler->tooling_type_sets[value];
        }
        merge_loop_exit(compiler,compiler->current_loop,break_type,break_set,
            break_tooling_set);
        compiler->current_loop->breaks[compiler->current_loop->break_count++]=
            emit_loop_jump(compiler,compiler->current_loop,0,true);
    } else if(kind==DIAMOND_TOKEN_NEXT) {
        (void)emit_loop_jump(compiler,compiler->current_loop,
            compiler->current_loop->continue_target,false);
    } else {
        (void)emit_loop_jump(compiler,compiler->current_loop,
            compiler->current_loop->redo_target,false);
    }
    const uint16_t result=allocate_register(compiler);
    /* Dead code: break/next/redo all jump unconditionally above, so
     * this NIL never executes -- also a sole-writer register run_chunk's
     * zero-init already covers even if it somehow did. */
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}
