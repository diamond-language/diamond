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

/* Assignment: plain, compound, index, index-compound and multiple/destructuring assignment. */

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
bool compound_assignment_ahead(const Compiler *compiler) {
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
bool index_compound_assignment_ahead(const Compiler *compiler) {
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
bool multi_assignment_ahead(const Compiler *compiler) {
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

uint16_t compile_index_assignment(Compiler *compiler) {
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
uint16_t compile_index_compound_assignment(Compiler *compiler) {
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
uint16_t compile_compound_assignment(Compiler *compiler) {
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
