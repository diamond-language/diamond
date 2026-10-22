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

/* `if`/`unless`, `while`/`until` and `loop` compilation. */

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
        apply_scope_narrowing_facts(compiler,
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
        if(register_is_local(compiler,(uint16_t)index)&&
           (then_types[index]!=before_types[index]||
            then_sets[index]!=before_sets[index]||
            then_tooling[index]!=before_tooling[index]))
            record_scope_type_fact(compiler,(uint16_t)index,compiler->current.span.start);
    }
    for(size_t index=0;index<flow_local_count;index++)
        compiler->locals[index].alias_identity=before_alias[index];
    if(narrowing.valid)
        apply_scope_narrowing_facts(compiler,
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
void mark_locals_captured(Compiler *compiler) {
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
bool statement_may_capture(const Compiler *compiler) {
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

uint16_t parse_while(Compiler *compiler,bool inverted) {
    return parse_while_core(compiler,inverted,nullptr);
}

uint16_t parse_postfix_while(Compiler *compiler,bool inverted,DiamondLexer after_modifier) {
    const PostfixLoop postfix={.after_modifier=after_modifier};
    return parse_while_core(compiler,inverted,&postfix);
}

uint16_t parse_loop(Compiler *compiler) {
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
