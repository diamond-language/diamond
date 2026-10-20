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

/* Blocks and `def`: closure and method compilation, signatures, and return-flow inference. */

/* `do |param, param, ...| BODY end` attached right after a call's closing
 * `).
 * Deliberately a separate, much smaller function rather than a refactor
 * of compile_definition (below): that function carries a lot of ceremony
 * that doesn't apply to an anonymous block (operator-method names,
 * module-singleton `self.` binding, class/module member registration,
 * return-type annotations, docstrings, default parameter values, the
 * endless `def foo() = expr` form) and has several hard-won, comment-
 * documented correctness fixes (e.g. the redefine_method patch-factory
 * nested_in_singleton_method exception below) not worth risking by
 * routing an unrelated caller through it. What *is* reused is the
 * underlying mechanism: a block is compiled into a real new
 * DiamondFunction slot and produces a real, capturing DIAMOND_OP_CLOSURE
 * value -- the exact same runtime shape a nested `def` already produces
 * -- so this function mirrors only the subset of compile_definition that
 * builds that shape (save/restore outer compiler state, bind parameters
 * as locals, eagerly materialize every enclosing local as a directly
 * accessible local via GET_CAPTURE_CELL, compile the body, then BOX_LOCAL
 * + CLOSURE). No new DiamondOpCode. Caller has already confirmed
 * compiler->current.kind == DIAMOND_TOKEN_DO and not yet consumed it. */
uint16_t compile_block(Compiler *compiler) {
    const bool has_contextual_types=compiler->has_contextual_block_types;
    const uint8_t contextual_arity=compiler->contextual_block_arity;
    uint16_t contextual_types[16];
    int32_t contextual_type_sets[16];
    for(size_t index=0;index<contextual_arity;index++) {
        contextual_types[index]=compiler->contextual_block_types[index];
        contextual_type_sets[index]=compiler->contextual_block_type_sets[index];
    }
    const int32_t contextual_return_set=compiler->contextual_block_return_set;
    const bool contextual_tooling_only=compiler->contextual_block_tooling_only;
    compiler->contextual_block_tooling_only=false;
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    advance_token(compiler);
    if (compiler->program->function_count == DIAMOND_MAX_FUNCTIONS) {
        fail(compiler, compiler->current.span, "too many functions");
        return 0;
    }
    size_t function_index=0;
    DiamondFunction *function=compiler_add_function(compiler,&function_index);
    if(function==nullptr) {
        fail(compiler,compiler->current.span,"out of memory");
        return 0;
    }
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    function->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
        function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
    /* UINT8_MAX-2, compile_definition's own "self via capture" sentinel
     * (see its own extensive comment on this exact value), when this
     * block is written somewhere `self` already exists -- materialized
     * into its own register 0 below, the same way a `closure name()
     * ... end` (captures_self) already does; call_closure_helper
     * (src/vm.c) already shifts real arguments to register 1+ for any
     * owner_class!=DIAMOND_OWNER_NONE callable, so reusing that exact sentinel
     * here needs no further runtime changes at all. compiler->in_method
     * is deliberately left untouched anywhere in this function (still
     * whatever the enclosing function had) -- that's already why `self`
     * was never a *compile-time* error inside a block written in a
     * method; only *which* value ended up in register 0 was wrong
     * before this fix (the block's own first real parameter, since
     * nothing reserved register 0 for self at all). */
    function->owner_class=compiler->in_method?DIAMOND_OWNER_CAPTURED_SELF:DIAMOND_OWNER_NONE;
    function->nested=true;
    static const char block_name[]="<block>";
    for(size_t index=0;index<sizeof(block_name);index++)
        function->name[index]=block_name[index];
    function->declaration_line=(uint32_t)compiler->previous.span.line;
    function->declaration_column=(uint32_t)compiler->previous.span.column;
    function->declaration_start=compiler->previous.span.start;

    DiamondFunction *outer_function = compiler->function;
    Local outer_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_local_count = compiler->local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        outer_locals[index] = compiler->locals[index];
    }
    const uint16_t outer_next_register = compiler->next_register;
    const DiamondSpan outer_method = compiler->current_method;
    const bool outer_in_method = compiler->in_method;
    const bool outer_in_singleton_method = compiler->in_singleton_method;
    const bool outer_in_function=compiler->in_function;
    const bool outer_in_block=compiler->in_block;
    const size_t outer_begin_depth=compiler->begin_depth;
    const bool outer_has_current_block=compiler->has_current_block;
    const uint16_t outer_current_block_register=
        compiler->current_block_register;
    const uint16_t outer_current_block_type_set=
        compiler->current_block_type_set;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    /* See return_flow_seen's own comment (the Compiler struct): a
     * `return` inside this block's own body belongs to the block, not
     * whatever function is being compiled around it -- reset before
     * compiling the block's body, restore the outer function's own
     * accumulated state after, the same save/reset/restore every other
     * per-function field on this list already gets. */
    const bool outer_return_flow_seen=compiler->return_flow_seen;
    const uint16_t outer_return_flow_type=compiler->return_flow_type;
    const int32_t outer_return_flow_set=compiler->return_flow_set;
    const uint16_t outer_return_tooling_type=compiler->return_tooling_type;
    const int32_t outer_return_tooling_set=compiler->return_tooling_set;
    compiler->return_flow_seen=false;
    const bool outer_block_has_break=compiler->block_has_break;
    compiler->block_has_break=false;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
    LoopContext *outer_loop=compiler->current_loop;
    Local outer_enclosing_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_enclosing_local_count=compiler->enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        outer_enclosing_locals[i]=compiler->enclosing_locals[i];
    uint16_t outer_capture_registers[DIAMOND_MAX_CAPTURES];
    const size_t outer_capture_count=compiler->capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        outer_capture_registers[i]=compiler->capture_registers[i];
    /* Same inline-then-heap fallback as parse_if/compile_definition's own
     * (see either's comment) -- necessary here too, for the same reason:
     * a block can appear after arbitrarily many registers have already
     * been allocated in the enclosing function body. */
    uint16_t inline_outer_known_types[256];int32_t inline_outer_known_type_sets[256];
    int32_t inline_outer_tooling_type_sets[256];
    uint16_t *outer_known_types=inline_outer_known_types;
    int32_t *outer_known_type_sets=inline_outer_known_type_sets;
    int32_t *outer_tooling_type_sets=inline_outer_tooling_type_sets;
    uint16_t *heap_outer_known_types=nullptr;int32_t *heap_outer_known_type_sets=nullptr;
    int32_t *heap_outer_tooling_type_sets=nullptr;
    if(outer_next_register>256) {
        heap_outer_known_types=malloc((size_t)outer_next_register*sizeof(uint16_t));
        heap_outer_known_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        heap_outer_tooling_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        if(heap_outer_known_types==nullptr||heap_outer_known_type_sets==nullptr||
           heap_outer_tooling_type_sets==nullptr) {
            fail(compiler,compiler->previous.span,"out of memory compiling block");
            free(heap_outer_known_types);free(heap_outer_known_type_sets);
            free(heap_outer_tooling_type_sets);
            return 0;
        }
        outer_known_types=heap_outer_known_types;
        outer_known_type_sets=heap_outer_known_type_sets;
        outer_tooling_type_sets=heap_outer_tooling_type_sets;
    }
    for(size_t index=0;index<outer_next_register;index++)
        {outer_known_types[index]=compiler->known_types[index];
         outer_known_type_sets[index]=compiler->known_type_sets[index];
         outer_tooling_type_sets[index]=compiler->tooling_type_sets[index];}
    uint16_t captured_fact_registers[DIAMOND_MAX_LOCALS];
    for(size_t index=0;index<DIAMOND_MAX_LOCALS;index++)
        captured_fact_registers[index]=DIAMOND_NO_TYPE_SET;

    /* Copy self into a fresh register of the *enclosing* function and box
     * that copy, exactly mirroring compile_definition's own captures_self
     * handling (see its own comment for why a copy, not boxing register 0
     * itself in place: every self/@ivar access elsewhere in the enclosing
     * method hardcodes literal register 0 unconditionally, so boxing it
     * directly would corrupt every one of those). Still in the enclosing
     * function's own compiler state here -- compiler->function hasn't
     * switched yet. */
    uint16_t self_copy_register=0;
    if(compiler->in_method) {
        self_copy_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,self_copy_register,0,0,2);
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,self_copy_register,0,0,1);
    }

    compiler->function = function;
    compiler->begin_depth=0;
    uint16_t declared_return_set=DIAMOND_NO_TYPE_SET;
    if(contextual_return_set>=0&&
       (size_t)contextual_return_set<outer_function->type_set_count)
        declared_return_set=clone_type_set_into_current(compiler,
            outer_function->type_sets,outer_function->type_set_count,
            (uint16_t)contextual_return_set);
    /* A `return`/`next` in this block returns from the block, so it's
     * checked against the block's own expected result -- never the
     * enclosing function's `-> Type`, whose set index belongs to that
     * function's type-set table, not this one. */
    compiler->current_return_type=declared_return_set==DIAMOND_NO_TYPE_SET?
        -1:(int)declared_return_set;
    compiler->has_current_block=false;
    compiler->current_block_register=0;
    compiler->current_block_type_set=DIAMOND_NO_TYPE_SET;
    compiler->current_loop=nullptr;
    compiler->current_exception=-1;
    compiler->current_retry_target=SIZE_MAX;
    compiler->local_count = 0;
    compiler->next_register = 0;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->enclosing_local_count=outer_local_count;
    for(size_t i=0;i<compiler->enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_locals[i];
    compiler->capture_count=0;
    if(compiler->enclosing_local_count>DIAMOND_MAX_CAPTURES) {
        fail(compiler,compiler->previous.span,"block sees too many lexical bindings");
    } else {
        compiler->capture_count=compiler->enclosing_local_count;
        for(size_t i=0;i<compiler->capture_count;i++)
            compiler->capture_registers[i]=compiler->enclosing_locals[i].reg;
    }
    /* Materialize the captured self (see self_copy_register above) into
     * this block's own register 0 -- guaranteed to land there since
     * next_register was just reset to 0 above and nothing else has
     * allocated from it yet, exactly mirroring compile_definition's own
     * captures_self materialization (see its own comment). Every self/
     * @ivar/self.foo() code path (parse_prefix's DIAMOND_TOKEN_SELF/
     * INSTANCE_VARIABLE, DIAMOND_OP_INVOKE_SELF_METHOD) hardcodes literal
     * register 0 unconditionally, so this is what makes all of those work
     * unmodified inside a block body too. */
    if(function->owner_class==DIAMOND_OWNER_CAPTURED_SELF) {
        if(compiler->capture_count==DIAMOND_MAX_CAPTURES) {
            fail(compiler,compiler->previous.span,"block sees too many lexical bindings");
        } else {
            const size_t self_capture_index=compiler->capture_count;
            compiler->capture_registers[compiler->capture_count++]=self_copy_register;
            const uint16_t self_register=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,self_register,
                             (uint8_t)self_capture_index,0,2);
        }
    }

    /* `|x, y|` -- bare identifiers only, no type annotations, no default
     * values (deliberate v1 scope cut, see this feature's own design
     * doc). Absent entirely (`do ... end`) means a genuine zero-arity
     * block -- correct as-is, not a gap: calling it with an argument
     * raises the same "wrong number of arguments" any arity mismatch
     * already does, no Ruby-style silent leniency to replicate. */
    if(!compiler->failed&&compiler->current.kind==DIAMOND_TOKEN_PIPE) {
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_PIPE) {
            size_t parameter_count=0;
            do {
                if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                    fail(compiler,compiler->current.span,"expected block parameter name");
                    break;
                }
                if(function->arity==UINT8_MAX||parameter_count==DIAMOND_MAX_DECLARED_PARAMETERS) {
                    fail(compiler,compiler->current.span,"too many block parameters");
                    break;
                }
                if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                    fail(compiler,compiler->current.span,"too many local variables");break;
                }
                const uint16_t parameter=allocate_register(compiler);
                if(contextual_tooling_only&&has_contextual_types&&
                   parameter_count<contextual_arity&&
                   contextual_type_sets[parameter_count]>=0&&
                   (size_t)contextual_type_sets[parameter_count]<
                       outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)contextual_type_sets[parameter_count]);
                    if(cloned!=DIAMOND_NO_TYPE_SET)
                        compiler->tooling_type_sets[parameter]=(int32_t)cloned;
                } else if(has_contextual_types&&parameter_count<contextual_arity&&
                   contextual_type_sets[parameter_count]>=0&&
                   (size_t)contextual_type_sets[parameter_count]<
                       outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)contextual_type_sets[parameter_count]);
                    if(cloned!=DIAMOND_NO_TYPE_SET) {
                        compiler->known_type_sets[parameter]=(int32_t)cloned;
                        function->parameter_type_sets[parameter_count]=cloned;
                    }
                }
                if(!contextual_tooling_only&&has_contextual_types&&
                   parameter_count<contextual_arity&&
                   contextual_types[parameter_count]!=TYPE_UNKNOWN) {
                    compiler->known_types[parameter]=
                        contextual_types[parameter_count];
                    /* The contextual type is part of the block's callable
                     * signature as well as a body-local inference fact.
                     * Dynamic invocation validates typed Callable arguments
                     * from this metadata, so omitting it would compile a
                     * well-typed body and then reject its closure at runtime. */
                    if(function->parameter_type_sets[parameter_count]==
                           DIAMOND_NO_TYPE_SET&&reserve_type_sets(compiler,1)) {
                        const size_t set_index=function->type_set_count++;
                        DiamondTypeSet *set=&function->type_sets[set_index];
                        set->count=1;set->inferred=true;
                        set->members[0]=(DiamondTypeMember){
                            .id=contextual_types[parameter_count],
                            .argument_set=DIAMOND_NO_TYPE_SET,
                            .second_argument_set=DIAMOND_NO_TYPE_SET,
                            .callable_arity=UINT8_MAX,
                            .callable_return_set=DIAMOND_NO_TYPE_SET,
                            .callable_parameters_typed=false};
                        for(size_t index=0;index<16;index++)
                            set->members[0].callable_parameter_sets[index]=
                                DIAMOND_NO_TYPE_SET;
                        function->parameter_type_sets[parameter_count]=
                            (uint16_t)set_index;
                    }
                }
                compiler->locals[compiler->local_count++]=(Local){
                    .name=compiler->current.span,.reg=parameter};
                record_scope_type_fact(compiler,parameter,compiler->current.span.start);
                const DiamondSpan parameter_name_span=compiler->current.span;
                size_t parameter_name_length=parameter_name_span.length;
                if(parameter_name_length>=DIAMOND_MAX_FUNCTION_NAME)
                    parameter_name_length=DIAMOND_MAX_FUNCTION_NAME-1;
                for(size_t index=0;index<parameter_name_length;index++)
                    function->parameter_names[parameter_count][index]=
                        compiler->source[parameter_name_span.start+index];
                function->parameter_names[parameter_count][parameter_name_length]='\0';
                function->arity++;
                function->required_arity++;
                parameter_count++;
                advance_token(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
                advance_token(compiler);
                skip_newlines(compiler);
            } while(!compiler->failed);
        }
        if(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_PIPE) {
            fail(compiler,compiler->current.span,"expected '|' after block parameters");
        } else if(!compiler->failed) {
            advance_token(compiler);
        }
    }

    /* Eager capture materialization: every enclosing local not shadowed
     * by a block parameter of the same name becomes a directly-accessible
     * local right away, same as compile_definition's own nested-def
     * handling below. */
    if(!compiler->failed) {
        for(size_t i=0;i<compiler->enclosing_local_count;i++) {
            if(find_local(compiler,compiler->enclosing_locals[i].name)>=0)continue;
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->enclosing_locals[i].name,
                     "too many lexical bindings");break;
            }
            const uint16_t cell=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE_CELL,cell,(uint8_t)i,0,2);
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->enclosing_locals[i].name,.reg=cell,.captured=true,
                .alias_identity=compiler->enclosing_locals[i].alias_identity};
            captured_fact_registers[i]=cell;
            const uint16_t source=compiler->enclosing_locals[i].reg;
            if(source<outer_next_register) {
                compiler->known_types[cell]=outer_known_types[source];
                const int32_t source_set=outer_known_type_sets[source];
                if(source_set>=0&&
                   (size_t)source_set<outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)source_set);
                    if(cloned!=DIAMOND_NO_TYPE_SET)
                        compiler->known_type_sets[cell]=(int32_t)cloned;
                }
                const int32_t tooling_set=outer_tooling_type_sets[source];
                if(tooling_set>=0&&
                   (size_t)tooling_set<outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)tooling_set);
                    if(cloned!=DIAMOND_NO_TYPE_SET)
                        compiler->tooling_type_sets[cell]=(int32_t)cloned;
                }
            }
        }
    }

    /* `yield` inside this block still means the enclosing method's own
     * &block, as in Ruby: `def any?(&test) = items.find() do |x| yield(x)
     * end` calls `test`. That parameter is an ordinary local, captured
     * above like every other, so point this block's current block at its
     * captured cell -- unless a block parameter of the same name shadows it
     * (find_local then resolves to the parameter, which isn't captured). */
    if(!compiler->failed&&outer_has_current_block) {
        for(size_t i=0;i<compiler->enclosing_local_count;i++) {
            if(compiler->enclosing_locals[i].reg!=outer_current_block_register)
                continue;
            const int inner=find_local(compiler,compiler->enclosing_locals[i].name);
            if(inner>=0&&compiler->locals[(size_t)inner].captured) {
                compiler->has_current_block=true;
                compiler->current_block_register=
                    compiler->locals[(size_t)inner].reg;
                if(outer_current_block_type_set!=DIAMOND_NO_TYPE_SET&&
                   outer_current_block_type_set<outer_function->type_set_count)
                    compiler->current_block_type_set=clone_type_set_into_current(
                        compiler,outer_function->type_sets,
                        outer_function->type_set_count,
                        outer_current_block_type_set);
            }
            break;
        }
    }

    compiler->in_function=true;
    compiler->in_block=true;
    const uint16_t body_result=compiler->failed?0:compile_sequence(compiler);
    if(!compiler->failed&&declared_return_set==DIAMOND_NO_TYPE_SET&&
       compiler->return_flow_seen) {
        /* An early `return`/`next` can hand back a different type than the
         * body's final expression, so the block's inferred result is the
         * union of both -- the same join compile_definition makes. The
         * merge lands on body_result's own facts, which the inference
         * below reads. */
        uint16_t merged_type=TYPE_UNKNOWN;int32_t merged_set=-1;
        merge_flow_types(compiler,compiler->known_types[body_result],
            compiler->known_type_sets[body_result],compiler->return_flow_type,
            compiler->return_flow_set,&merged_type,&merged_set);
        compiler->known_types[body_result]=merged_type;
        compiler->known_type_sets[body_result]=merged_set;
    }
    /* The block's editor-only result graph, indexed in the block's own table;
     * it is cloned into the caller's table once that is current again. */
    int32_t block_tooling_return=-1;
    if(!compiler->failed&&declared_return_set==DIAMOND_NO_TYPE_SET&&
       !compiler->return_flow_seen&&!compiler->method_has_block_return)
        block_tooling_return=compiler->tooling_type_sets[body_result]>=0?
            compiler->tooling_type_sets[body_result]:
            compiler->known_type_sets[body_result];
    if(block_tooling_return<0&&!compiler->failed&&
       declared_return_set==DIAMOND_NO_TYPE_SET&&!compiler->return_flow_seen&&
       !compiler->method_has_block_return&&
       compiler->known_types[body_result]!=TYPE_UNKNOWN&&
       reserve_type_sets(compiler,1)) {
        DiamondTypeSet *set=&function->type_sets[function->type_set_count];
        set->count=1;set->inferred=true;
        set->members[0]=(DiamondTypeMember){
            .id=compiler->known_types[body_result],
            .argument_set=DIAMOND_NO_TYPE_SET,
            .second_argument_set=DIAMOND_NO_TYPE_SET,
            .callable_arity=UINT8_MAX,
            .callable_return_set=DIAMOND_NO_TYPE_SET,
            .callable_parameters_typed=false};
        for(size_t parameter=0;parameter<16;parameter++)
            set->members[0].callable_parameter_sets[parameter]=
                DIAMOND_NO_TYPE_SET;
        block_tooling_return=(int32_t)function->type_set_count++;
    }
    if(!compiler->failed) {
        if(declared_return_set!=DIAMOND_NO_TYPE_SET) {
            emit_type_check(compiler,body_result,declared_return_set,
                compiler->previous.span);
            function->return_type_set=declared_return_set;
        } else if(compiler->known_type_sets[body_result]>=0) {
            function->return_type_set=
                (uint16_t)compiler->known_type_sets[body_result];
        } else if(compiler->known_types[body_result]!=TYPE_UNKNOWN&&
                  reserve_type_sets(compiler,1)) {
            const size_t return_set=function->type_set_count++;
            DiamondTypeSet *set=&function->type_sets[return_set];
            set->count=1;set->inferred=true;
            set->members[0]=(DiamondTypeMember){
                .id=compiler->known_types[body_result],
                .argument_set=DIAMOND_NO_TYPE_SET,
                .second_argument_set=DIAMOND_NO_TYPE_SET,
                .callable_arity=UINT8_MAX,
                .callable_return_set=DIAMOND_NO_TYPE_SET,
                .callable_parameters_typed=false};
            for(size_t parameter=0;parameter<16;parameter++)
                set->members[0].callable_parameter_sets[parameter]=
                    DIAMOND_NO_TYPE_SET;
            function->return_type_set=(uint16_t)return_set;
        }
        emit_instruction(compiler,DIAMOND_OP_RETURN,body_result,0,0,1);
        if(compiler->current.kind!=DIAMOND_TOKEN_END) {
            fail(compiler,compiler->current.span,"expected 'end' after block body");
        } else {
            advance_token(compiler);
        }
    }

    function->capture_count=(uint8_t)compiler->capture_count;
    function->register_count=compiler->next_register;
    uint16_t captures[DIAMOND_MAX_CAPTURES];
    for(size_t i=0;i<compiler->capture_count;i++)captures[i]=compiler->capture_registers[i];
    const size_t capture_count=compiler->capture_count;
    if(!compiler->failed) {
        const size_t body_end=compiler->previous.span.start+compiler->previous.span.length;
        record_scope_locals(compiler,0,compiler->local_count,body_end);
        function->body_end=body_end;
    }

    uint16_t captured_fact_types[DIAMOND_MAX_LOCALS];
    int32_t captured_fact_sets[DIAMOND_MAX_LOCALS];
    int32_t captured_fact_tooling_sets[DIAMOND_MAX_LOCALS];
    for(size_t index=0;index<outer_local_count;index++) {
        const uint16_t reg=captured_fact_registers[index];
        captured_fact_types[index]=reg==DIAMOND_NO_TYPE_SET?TYPE_UNKNOWN:
            compiler->known_types[reg];
        captured_fact_sets[index]=reg==DIAMOND_NO_TYPE_SET?-1:
            compiler->known_type_sets[reg];
        captured_fact_tooling_sets[index]=reg==DIAMOND_NO_TYPE_SET?-1:
            compiler->tooling_type_sets[reg];
    }
    compiler->function = outer_function;
    compiler->begin_depth=outer_begin_depth;
    compiler->local_count = outer_local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        compiler->locals[index] = outer_locals[index];
    }
    compiler->next_register = outer_next_register;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->current_method = outer_method;
    compiler->in_method = outer_in_method;
    compiler->in_singleton_method = outer_in_singleton_method;
    compiler->in_function=outer_in_function;
    compiler->in_block=outer_in_block;
    if(compiler->block_has_break) {
        /* The break lands in outer_function's frame, so outer_function
         * stays out of the JIT; the call's result type is unknown. */
        outer_function->nonlocal_landing=true;
        compiler->block_break_pending=true;
        compiler->pending_block_call=function;
    }
    compiler->block_has_break=outer_block_has_break;
    compiler->has_current_block=outer_has_current_block;
    compiler->current_block_register=outer_current_block_register;
    compiler->current_block_type_set=outer_current_block_type_set;
    compiler->current_return_type=outer_return_type;
    compiler->current_return_type_span=outer_return_type_span;
    compiler->return_flow_seen=outer_return_flow_seen;
    compiler->return_flow_type=outer_return_flow_type;
    compiler->return_flow_set=outer_return_flow_set;
    compiler->return_tooling_type=outer_return_tooling_type;
    compiler->return_tooling_set=outer_return_tooling_set;
    compiler->current_exception=outer_exception;
    compiler->current_retry_target=outer_retry_target;
    compiler->current_loop=outer_loop;
    compiler->enclosing_local_count=outer_enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_enclosing_locals[i];
    compiler->capture_count=outer_capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        compiler->capture_registers[i]=outer_capture_registers[i];
    for(size_t index=0;index<outer_next_register;index++)
        {compiler->known_types[index]=outer_known_types[index];
         compiler->known_type_sets[index]=outer_known_type_sets[index];
         compiler->tooling_type_sets[index]=outer_tooling_type_sets[index];}
    for(size_t index=0;index<outer_local_count;index++) {
        if(captured_fact_registers[index]==DIAMOND_NO_TYPE_SET)continue;
        const uint16_t source=outer_locals[index].reg;
        if(source>=outer_next_register)continue;
        compiler->known_types[source]=captured_fact_types[index];
        const int32_t inner_set=captured_fact_sets[index];
        compiler->known_type_sets[source]=-1;
        if(inner_set>=0&&(size_t)inner_set<function->type_set_count) {
            const uint16_t cloned=clone_type_set_into_current(compiler,
                function->type_sets,function->type_set_count,
                (uint16_t)inner_set);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->known_type_sets[source]=(int32_t)cloned;
        }
        const int32_t inner_tooling_set=captured_fact_tooling_sets[index];
        compiler->tooling_type_sets[source]=-1;
        if(inner_tooling_set>=0&&
           (size_t)inner_tooling_set<function->type_set_count) {
            const uint16_t cloned=clone_type_set_into_current(compiler,
                function->type_sets,function->type_set_count,
                (uint16_t)inner_tooling_set);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->tooling_type_sets[source]=(int32_t)cloned;
        }
        record_scope_type_fact(compiler,source,compiler->current.span.start);
    }
    free(heap_outer_known_types);free(heap_outer_known_type_sets);
    free(heap_outer_tooling_type_sets);

    /* BOX_LOCAL + CLOSURE, emitted into the *outer* (caller's) bytecode
     * now that compiler->function/locals have been restored -- same
     * shape compile_definition ends with. */
    for(size_t i=0;i<capture_count;i++) {
        for(size_t local=0;local<compiler->local_count;local++) {
            if(compiler->locals[local].reg!=captures[i])continue;
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,captures[i],0,0,1);
            compiler->locals[local].captured=true;
            break;
        }
    }
    const uint16_t result=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
    emit_function_index(compiler,function_index);emit_byte(compiler,(uint8_t)capture_count);
    for(size_t i=0;i<capture_count;i++)emit_register(compiler,captures[i]);
    compiler->known_types[result]=TYPE_UNKNOWN;
    if(block_tooling_return>=0&&!compiler->failed&&
       (size_t)block_tooling_return<function->type_set_count)
        publish_block_tooling_callable(compiler,result,function,
            (uint16_t)block_tooling_return);
    return result;
}

uint16_t compile_definition(Compiler *compiler, bool captures_self) {
    const bool at_top_level = compiler->function == &compiler->program->entry;
    if(captures_self && !compiler->in_method) {
        fail(compiler,compiler->current.span,
             "closure requires an enclosing method -- no 'self' is available here");
        return 0;
    }
    advance_token(compiler);
    bool module_singleton=false;
    if(compiler->current.kind==DIAMOND_TOKEN_SELF&&
       (compiler->current_module>=0||compiler->current_class>=0)) {
        module_singleton=true;advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_DOT) {
            fail(compiler,compiler->current.span,"expected '.' after 'self'");
            return 0;
        }
        advance_token(compiler);
    }
    /* Operator overloading: a method literally named "+"/"=="/etc. is
     * already legal at the VM level (lookup_method dispatches purely by
     * name-string + arity, no charset restriction) -- the only barrier is
     * this check. Unary minus is deliberately NOT included here: it's
     * named "negate", an ordinary identifier, recognized by name at
     * NEGATE's dispatch point the same way "to_s" is recognized for
     * stringification, needing no parser accommodation at all. */
    bool operator_name =
        compiler->current.kind==DIAMOND_TOKEN_PLUS||
        compiler->current.kind==DIAMOND_TOKEN_MINUS||
        compiler->current.kind==DIAMOND_TOKEN_STAR||
        compiler->current.kind==DIAMOND_TOKEN_SLASH||
        compiler->current.kind==DIAMOND_TOKEN_PERCENT||
        compiler->current.kind==DIAMOND_TOKEN_EQUAL_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_LESS||
        compiler->current.kind==DIAMOND_TOKEN_LESS_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_GREATER||
        compiler->current.kind==DIAMOND_TOKEN_GREATER_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_SPACESHIP;
    /* `[]`/`[]=` -- the one operator-overload shape that isn't a single
     * lexer token, so unlike every case above it needs its own
     * multi-token lookahead/consumption here, not just a membership
     * check. `[` has no other legal meaning in "expecting a method name"
     * position (unlike right *after* a name, where DIAMOND_TOKEN_LEFT_
     * BRACKET already means generic type parameters -- see the `def
     * name[T](...)` handling below; that check runs strictly later, on
     * whatever follows the name this block already consumed, so the two
     * never conflict), so this can commit to consuming eagerly on `[`
     * rather than needing to backtrack. Requires `[`/`]` (and `=`, for
     * the setter) strictly adjacent -- no whitespace -- matching every
     * other operator name here being a single, ungappable token; `def [
     * ](i)` or `def [] =(i, v)` fall through to the ordinary "expected
     * function name" error below, same as any other malformed name. */
    bool is_index_operator=false;
    DiamondSpan index_operator_name={};
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        const DiamondSpan left=compiler->current.span;
        DiamondLexer lookahead=compiler->lexer;
        const DiamondToken right=diamond_lexer_next(&lookahead);
        if(right.kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
           right.span.start==left.start+left.length) {
            is_index_operator=true;
            advance_token(compiler);
            advance_token(compiler);
            size_t total_length=right.span.start+right.span.length-left.start;
            if(compiler->current.kind==DIAMOND_TOKEN_EQUAL&&
               compiler->current.span.start==right.span.start+right.span.length) {
                total_length+=compiler->current.span.length;
                advance_token(compiler);
            }
            index_operator_name=(DiamondSpan){.start=left.start,
                .length=total_length,.line=left.line,.column=left.column};
            operator_name=true;
        }
    }
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER && !operator_name) {
        fail(compiler, compiler->current.span, "expected function name after 'def'");
        return 0;
    }
    /* A module's own operator method is inert until some class `include`s
     * it (a bare module isn't itself an instantiable receiver), but once
     * included it's copied into the including class's own method table
     * exactly like any other module method (see the `include` handling
     * below) -- dispatch already doesn't care where a method was
     * originally *written*, only which class ends up owning it, so
     * there's nothing else to teach the VM here. This is what lets
     * `module Comparable`'s own `<`/`<=`/`>`/`>=` derive from a `<=>` an
     * including class defines, the same relationship `Enumerable`
     * already has with `each`. */
    if (operator_name &&
        !((compiler->current_class>=0||compiler->current_module>=0) &&
          !module_singleton)) {
        fail(compiler, compiler->current.span,
             "operator methods can only be defined inside a class or module");
        return 0;
    }
    const DiamondSpan name = is_index_operator?index_operator_name:compiler->current.span;
    if (name.length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "function name is too long");
        return 0;
    }
    const bool genuine_top_level=at_top_level&&compiler->current_class<0&&
        compiler->current_module<0&&!module_singleton;
    const bool claiming_discovered=!compiler->discovery_pass&&
        compiler->next_function_claim<compiler->program->function_count&&
        compiler->program->functions[compiler->next_function_claim]
            ->declared_by_discovery;
    if(!compiler->program->allow_top_level_redefinition&&genuine_top_level&&
       find_function(compiler,name)>=0&&!claiming_discovered) {
        fail(compiler, name, "function is already defined");
        return 0;
    }
    size_t function_index=0;
    DiamondFunction *function=compiler_add_function(compiler,&function_index);
    if(function==nullptr) {
        fail(compiler,name,"out of memory");
        return 0;
    }
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    function->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
        function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
    /* current_class/current_module are compiler-wide "lexically inside a
     * class/module body" flags, true for a nested closure at any depth,
     * not just a direct member -- direct_class_member/direct_module_member
     * narrow that to "is *this* def itself the direct member" (at_top_level
     * true means compiler->function, before this def switches it below,
     * was the program entry or another already-direct member's own body,
     * never a nested closure's). A closure nested *immediately* inside a
     * singleton method (`def self.make_patch(); def replacement(...); ...;
     * end; replacement; end`) is a deliberate exception: redefine_method's
     * patch-factory idiom (vm.c) relies on such a closure carrying the
     * same owner_class/self-register treatment a genuine instance method
     * gets, even though it's just a local closure value -- confirmed by
     * legacy_0093.di/legacy_0094.di/legacy_0095.di, which construct
     * exactly this shape and require it to work. A closure nested any
     * deeper, or inside an ordinary instance method, or inside a plain
     * top-level function, gets neither: it's an ordinary closure, no
     * implicit self of any kind. */
    const bool direct_class_member=
        at_top_level&&compiler->current_class>=0&&!module_singleton;
    const bool direct_module_member=
        at_top_level&&compiler->current_module>=0&&!module_singleton;
    const bool nested_in_singleton_method=
        !at_top_level&&compiler->in_singleton_method;
    /* `def self.x` directly inside a *class* (not a module -- modules have
     * no superclass chain and nothing to virtually dispatch against, so
     * this deliberately excludes compiler->current_module). Gains the same
     * owner_class/implicit-self treatment direct_class_member already
     * gets below, so `self` becomes usable inside the singleton method's
     * own body and external calls can populate it with the literal
     * receiver class (see DIAMOND_OP_LOAD_CLASS's own comment). */
    const bool direct_class_singleton_member=
        at_top_level&&compiler->current_class>=0&&module_singleton;
    /* owner_class serves two genuinely different purposes bundled into
     * one field: (1) "this function is a real class/module method,
     * eligible for redefine_method/define_method" -- checked as an
     * *exact* value match against a real class index
     * (DIAMOND_OP_REDEFINE_METHOD's own `new_function->owner_class!=
     * class_operand`, src/vm.c) -- and (2) "register 0 is already
     * spoken for, every calling convention that places arguments must
     * skip it" (parameter_offset, computed everywhere in src/vm.c as a
     * plain `fn->owner_class==DIAMOND_OWNER_NONE?0:1` binary check -- confirmed
     * nowhere in this codebase tests for one *specific* non-UINT8_MAX
     * value except the assignment sites themselves, so any non-UINT8_MAX
     * sentinel gets (2) for free with no other call site changes).
     *
     * A `closure` (captures_self) needs (2) without (1): it reserves
     * register 0 itself, internally, for the materialized captured self
     * (see the captures_self handling below), so calls to it must still
     * skip register 0 the same way a real method's implicit receiver
     * already does -- confirmed the hard way as a real bug: leaving
     * owner_class at UINT8_MAX here computed parameter_offset=0,
     * silently colliding a closure's own first *declared* parameter
     * with its self-capture register. But it must never satisfy (1) --
     * it's never a patch factory, never installable via redefine_method/
     * define_method. A third sentinel, distinct from UINT8_MAX (no
     * owner_class at all) and UINT8_MAX-1 (the existing module-method
     * sentinel, diamond_compile's own convention, comfortably above
     * DIAMOND_MAX_CLASSES=180's real range either way), gives it (2)
     * while permanently failing (1)'s exact-match check.
     *
     * One more site needed a matching fix, not just this one:
     * call_closure_helper's own needs_self_slot padding (src/vm.c) --
     * used by CALL_CLOSURE and tap's block-invocation sites, the only
     * ways a Callable *value* (as opposed to ordinary method dispatch)
     * ever actually runs -- inflates its own arity-bounds check by one
     * for *any* owner_class!=DIAMOND_OWNER_NONE function, correct for a genuine
     * method's real implicit-receiver argument but wrong here too: this
     * sentinel's whole point is that nothing external ever supplies
     * register 0's value, so the bounds check must stay un-inflated,
     * matching this closure's own true, undistorted declared arity. */
    function->owner_class=
        (direct_class_member||direct_class_singleton_member||
         (nested_in_singleton_method&&!captures_self&&compiler->current_class>=0))?
            (uint16_t)compiler->current_class:
        (direct_module_member||(nested_in_singleton_method&&!captures_self&&compiler->current_module>=0))?
            DIAMOND_OWNER_MODULE:
        captures_self?DIAMOND_OWNER_CAPTURED_SELF:DIAMOND_OWNER_NONE;
    function->nested=!at_top_level;
    size_t copy_length=name.length;
    for (size_t index = 0; index < copy_length; index++) {
        function->name[index] = compiler->source[name.start + index];
    }
    function->name[copy_length] = '\0';
    function->declaration_line=(uint32_t)name.line;
    function->declaration_column=(uint32_t)name.column;
    function->declaration_start=name.start;
    /* is_index_operator already consumed every token of its own name
     * ("[]"/"[]=") above -- nothing left to advance past here, unlike
     * every other name shape (a single token, always consumed by this
     * one advance_token). */
    if(!is_index_operator) advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        DiamondLexer lookahead=compiler->lexer;
        if(diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_LEFT_PAREN) {
            if(copy_length+1>=DIAMOND_MAX_FUNCTION_NAME) {
                fail(compiler,name,"writer method name is too long");return 0;
            }
            function->name[copy_length++]='=';
            function->name[copy_length]='\0';advance_token(compiler);
        }
    }
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
               function->type_variable_count==8) {
                fail(compiler,compiler->current.span,"expected generic type parameter");break;
            }
            for(size_t existing=0;existing<function->type_variable_count;existing++)
                if(name_equals(compiler,function->type_variables[existing],
                               compiler->current.span,false)) {
                    fail(compiler,compiler->current.span,
                         "duplicate generic type parameter");break;
                }
            if(compiler->failed)break;
            char *type_variable=
                function->type_variables[function->type_variable_count++];
            if(compiler->current.span.length>=DIAMOND_MAX_FUNCTION_NAME) {
                fail(compiler,compiler->current.span,"generic type name is too long");break;
            }
            for(size_t index=0;index<compiler->current.span.length;index++)
                type_variable[index]=compiler->source[compiler->current.span.start+index];
            type_variable[compiler->current.span.length]='\0';advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET)
            fail(compiler,compiler->current.span,
                 "expected ']' after generic type parameters");
        else advance_token(compiler);
    }
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after function name");
        return 0;
    }
    advance_token(compiler);

    /* A recursive local helper (see DiamondFunction.calls_itself): declare
     * its name in the enclosing scope first, holding nil, so the body
     * captures it like any other enclosing local. Once the closure exists
     * it's stored into that same (by then boxed) cell, which the body's
     * calls read through. */
    int self_local=-1;
    if(!at_top_level&&!compiler->discovery_pass&&function->calls_itself&&
       find_local(compiler,name)<0&&compiler->local_count<DIAMOND_MAX_LOCALS) {
        const uint16_t self_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,self_register,0,0,1);
        self_local=(int)compiler->local_count;
        compiler->locals[compiler->local_count++]=(Local){.name=name,.reg=self_register};
    }
    DiamondFunction *const outer_definition=compiler->current_definition;
    const DiamondSpan outer_definition_name=compiler->current_definition_name;
    compiler->current_definition=at_top_level?nullptr:function;
    compiler->current_definition_name=name;

    DiamondFunction *outer_function = compiler->function;
    Local outer_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_local_count = compiler->local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        outer_locals[index] = compiler->locals[index];
    }
    const uint16_t outer_next_register = compiler->next_register;
    const DiamondSpan outer_method = compiler->current_method;
    const bool outer_in_method = compiler->in_method;
    const bool outer_in_singleton_method = compiler->in_singleton_method;
    const bool outer_in_function=compiler->in_function;
    const bool outer_in_block=compiler->in_block;
    const size_t outer_begin_depth=compiler->begin_depth;
    const bool outer_has_current_block=compiler->has_current_block;
    const uint16_t outer_current_block_register=
        compiler->current_block_register;
    const uint16_t outer_current_block_type_set=
        compiler->current_block_type_set;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    /* See return_flow_seen's own comment (the Compiler struct): reset
     * before compiling this def's own body, restore the outer function's
     * (if any -- a top-level def has none live) accumulated state after,
     * so a nested def's own returns never leak into an enclosing one's
     * inference or vice versa. */
    const bool outer_return_flow_seen=compiler->return_flow_seen;
    const uint16_t outer_return_flow_type=compiler->return_flow_type;
    const int32_t outer_return_flow_set=compiler->return_flow_set;
    const uint16_t outer_return_tooling_type=compiler->return_tooling_type;
    const int32_t outer_return_tooling_set=compiler->return_tooling_set;
    compiler->return_flow_seen=false;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
    LoopContext *outer_loop=compiler->current_loop;
    Local outer_enclosing_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_enclosing_local_count=compiler->enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        outer_enclosing_locals[i]=compiler->enclosing_locals[i];
    uint16_t outer_capture_registers[DIAMOND_MAX_CAPTURES];
    const size_t outer_capture_count=compiler->capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        outer_capture_registers[i]=compiler->capture_registers[i];
    /* Same inline-then-heap fallback as parse_if's before_types/then_types
     * (see that function's own comment) -- this one had a different bug
     * shape: a fixed `index<256` bound (not indexed by outer_next_register)
     * doesn't overflow, but silently under-saves/-restores whenever the
     * enclosing scope already has more than 256 live registers, leaving
     * entries [256, outer_next_register) holding whatever the nested def's
     * own (unrelated, register-index-0-based) body happened to write into
     * those same slots after this function returns. */
    uint16_t inline_outer_known_types[256];int32_t inline_outer_known_type_sets[256];
    int32_t inline_outer_tooling_type_sets[256];
    uint16_t *outer_known_types=inline_outer_known_types;
    int32_t *outer_known_type_sets=inline_outer_known_type_sets;
    int32_t *outer_tooling_type_sets=inline_outer_tooling_type_sets;
    uint16_t *heap_outer_known_types=nullptr;int32_t *heap_outer_known_type_sets=nullptr;
    int32_t *heap_outer_tooling_type_sets=nullptr;
    if(outer_next_register>256) {
        heap_outer_known_types=malloc((size_t)outer_next_register*sizeof(uint16_t));
        heap_outer_known_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        heap_outer_tooling_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        if(heap_outer_known_types==nullptr||heap_outer_known_type_sets==nullptr||
           heap_outer_tooling_type_sets==nullptr) {
            fail(compiler,compiler->previous.span,
                 "out of memory compiling function definition");
            free(heap_outer_known_types);free(heap_outer_known_type_sets);
            free(heap_outer_tooling_type_sets);
            return 0;
        }
        outer_known_types=heap_outer_known_types;
        outer_known_type_sets=heap_outer_known_type_sets;
        outer_tooling_type_sets=heap_outer_tooling_type_sets;
    }
    for(size_t index=0;index<outer_next_register;index++)
        {outer_known_types[index]=compiler->known_types[index];
         outer_known_type_sets[index]=compiler->known_type_sets[index];
         outer_tooling_type_sets[index]=compiler->tooling_type_sets[index];}
    uint16_t definition_captured_fact_registers[DIAMOND_MAX_LOCALS];
    for(size_t index=0;index<DIAMOND_MAX_LOCALS;index++)
        definition_captured_fact_registers[index]=DIAMOND_NO_TYPE_SET;
    /* Copy self into a fresh register of the *enclosing* function and box
     * that copy, rather than boxing register 0 (self's own real home)
     * in place the way an ordinary captured Local already does. Every
     * other self/@ivar access in the enclosing method hardcodes literal
     * register 0 unconditionally (see parse_prefix's DIAMOND_TOKEN_SELF/
     * INSTANCE_VARIABLE cases and compile_assignment_store) -- none of
     * them check a `.captured`-style flag the way an ordinary Local read
     * does, so boxing register 0 itself would silently corrupt every
     * other self/@ivar access in this same method that runs after this
     * point, turning a raw Instance/Class value into a Cell those sites
     * never expect. Capturing an isolated copy instead leaves register 0
     * completely untouched. */
    uint16_t self_copy_register=0;
    if(captures_self) {
        self_copy_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,self_copy_register,0,0,2);
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,self_copy_register,0,0,1);
    }
    compiler->function = function;
    compiler->begin_depth=0;
    compiler->has_current_block=false;
    compiler->current_block_register=0;
    compiler->current_block_type_set=DIAMOND_NO_TYPE_SET;
    /* A captures_self closure nested *directly* inside a `def self.x`
     * method also needs in_singleton_method true, for exactly the same
     * reason nested_in_singleton_method's own existing arm below does:
     * self.foo(...) inside its body must resolve through
     * parse_self_class_method_call/DIAMOND_OP_INVOKE_SELF_METHOD, not an
     * ordinary instance-shaped call -- and that opcode reads its class
     * receiver from register 0 unconditionally (confirmed directly in
     * src/vm.c, not just a compiler convention the way GET_IVAR's own
     * receiver operand is), which is exactly the register this closure's
     * own self-capture materializes into below. Nested any deeper than
     * directly inside the singleton method gets neither, the same
     * pre-existing depth-1-only limit nested_in_singleton_method already
     * has for the patch-factory case (see the class_operand comment
     * above `direct_class_member`). */
    compiler->in_singleton_method =
        (at_top_level && module_singleton) ||
        (captures_self && nested_in_singleton_method);
    compiler->current_loop=nullptr;
    compiler->current_exception=-1;
    compiler->current_retry_target=SIZE_MAX;
    compiler->local_count = 0;
    compiler->next_register = 0;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->enclosing_local_count=at_top_level ? 0 : outer_local_count;
    for(size_t i=0;i<compiler->enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_locals[i];
    compiler->capture_count=0;
    if(!at_top_level) {
        if(compiler->enclosing_local_count>DIAMOND_MAX_CAPTURES) {
            fail(compiler,name,"nested function sees too many lexical bindings");
        } else {
            compiler->capture_count=compiler->enclosing_local_count;
            for(size_t i=0;i<compiler->capture_count;i++)
                compiler->capture_registers[i]=compiler->enclosing_locals[i].reg;
        }
    }
    if((direct_class_member||direct_module_member||direct_class_singleton_member||
        nested_in_singleton_method)&&!captures_self) {
        (void)allocate_register(compiler);
        function->arity = 1;
        function->required_arity=1;
        compiler->current_method = name;
        compiler->in_method = true;
        /* JIT Phase 13 (docs/internal/jit-design.md): register 0 (self)
         * genuinely holds an Instance of current_class here -- an
         * ordinary instance method, or a def nested directly inside a
         * singleton method (the redefine_method patch-factory idiom,
         * which behaves exactly like an instance method, @ivar access
         * included -- see nested_in_singleton_method's own comment
         * above). Feeding this into known_types[0] the same way any
         * other already-known-class register already is lets Phase 12's
         * existing instance_call_signature/publish_instance_return_type
         * resolve a self.foo() call's own return type, closing the
         * "self.method().other()" half of Phase 12's own documented gap
         * with no jit.c changes at all. Deliberately excludes direct_
         * class_singleton_member (`def self.x`): self there is the
         * literal *class* value (DIAMOND_OP_LOAD_CLASS, see emit_
         * singleton_call's own comment), not an Instance -- tagging it
         * as "known instance of current_class" would be actively wrong,
         * not just an imprecise miss. direct_module_member is likewise
         * excluded: a module has no class-table entry for instance_call_
         * signature to resolve against at all. Must run after the
         * allocate_register call just above, not before: allocate_
         * register itself resets known_types[0] to TYPE_UNKNOWN on
         * every call, so setting this any earlier would just be
         * silently overwritten. */
        if(direct_class_member||(nested_in_singleton_method&&compiler->current_class>=0))
            compiler->known_types[0]=
                (uint16_t)(DIAMOND_TYPE_CLASS_BASE+compiler->current_class);
    } else if(captures_self) {
        /* self arrives entirely via capture, not an implicit-receiver
         * call convention -- no extra arity (a closure is called with
         * exactly the arguments its own parameter list declares). But
         * register 0 *is* reserved here, unlike a plain nested def:
         * every ordinary self/@ivar/self.foo() code path (parse_prefix's
         * DIAMOND_TOKEN_SELF/INSTANCE_VARIABLE, compile_assignment_store,
         * DIAMOND_OP_INVOKE_SELF_METHOD's own VM handler) hardcodes
         * literal register 0 unconditionally and unchanged -- reusing
         * all of that as-is (rather than teaching each one about a
         * capture index) means this closure's own register 0 must
         * actually hold self, materialized here via GET_CAPTURE, exactly
         * once, before any of its own body compiles. Guaranteed to land
         * in register 0: next_register was just reset to 0 above and
         * nothing else has allocated from it yet on this path. */
        if(compiler->capture_count==DIAMOND_MAX_CAPTURES) {
            fail(compiler,name,"nested function sees too many lexical bindings");
        } else {
            const size_t self_capture_index=compiler->capture_count;
            compiler->capture_registers[compiler->capture_count++]=self_copy_register;
            const uint16_t self_register=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,self_register,
                             (uint8_t)self_capture_index,0,2);
        }
        compiler->in_method = true;
    }

    size_t parameter_count=0;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        parameter_count=1;DiamondLexer lookahead=compiler->lexer;
        DiamondToken token=compiler->current;size_t nesting=0;
        while(token.kind!=DIAMOND_TOKEN_EOF) {
            if(token.kind==DIAMOND_TOKEN_LEFT_PAREN||
               token.kind==DIAMOND_TOKEN_LEFT_BRACKET||
               token.kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
            else if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
                if(nesting==0)break;
                nesting--;
            } else if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                      token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                if(nesting>0)nesting--;
            } else if(token.kind==DIAMOND_TOKEN_COMMA&&nesting==0)parameter_count++;
            token=diamond_lexer_next(&lookahead);
        }
    }
    if(parameter_count>DIAMOND_MAX_DECLARED_PARAMETERS) {
        fail(compiler,name,"functions cannot declare more than 32 parameters");
        parameter_count=DIAMOND_MAX_DECLARED_PARAMETERS;
    }
    const uint16_t parameter_base=compiler->next_register;
    for(size_t index=0;index<parameter_count;index++)(void)allocate_register(compiler);
    size_t declared_parameter_count=0;bool saw_default=false;
    bool has_block_parameter=false;
    uint16_t variadic_parameter=0,variadic_fixed_count=0;
    int block_parameter_type=-1;DiamondSpan block_parameter_type_span={};
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            /* `*name` -- a trailing variadic parameter, bare name only
             * (no type annotation, no default -- neither makes sense for
             * a collected Array), and must be the last parameter (no
             * comma may follow it). See docs/design.md's "Splat/variadic
             * parameters" section. */
            bool is_variadic=false;
            bool is_block_parameter=false;
            if(compiler->current.kind==DIAMOND_TOKEN_AMPERSAND) {
                if(has_block_parameter) {
                    fail(compiler,compiler->current.span,
                         "a function can only declare one block parameter");break;
                }
                is_block_parameter=true;has_block_parameter=true;
                advance_token(compiler);
            }
            if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
                if(function->has_variadic) {
                    fail(compiler,compiler->current.span,
                         "a function can only declare one variadic parameter");
                    break;
                }
                is_variadic=true;
                advance_token(compiler);
            }
            if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler, compiler->current.span, "expected parameter name");
                break;
            }
            if (function->arity == UINT8_MAX) {
                fail(compiler, compiler->current.span, "too many parameters");
                break;
            }
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->current.span,"too many local variables");break;
            }
            const uint16_t parameter=(uint16_t)(parameter_base+declared_parameter_count);
            if(is_block_parameter) {
                compiler->has_current_block=true;
                compiler->current_block_register=parameter;
            }
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->current.span,.reg=parameter};
            if(declared_parameter_count<DIAMOND_MAX_DECLARED_PARAMETERS) {
                const DiamondSpan parameter_name_span=compiler->current.span;
                size_t parameter_name_length=parameter_name_span.length;
                if(parameter_name_length>=DIAMOND_MAX_FUNCTION_NAME)
                    parameter_name_length=DIAMOND_MAX_FUNCTION_NAME-1;
                for(size_t index=0;index<parameter_name_length;index++)
                    function->parameter_names[declared_parameter_count][index]=
                        compiler->source[parameter_name_span.start+index];
                function->parameter_names[declared_parameter_count][parameter_name_length]='\0';
            }
            function->arity++;
            advance_token(compiler);
            if(is_variadic) {
                if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
                    fail(compiler,compiler->current.span,
                         "a variadic parameter cannot have a type annotation");
                    break;
                }
                if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                    fail(compiler,compiler->current.span,
                         "a variadic parameter cannot have a default value");
                    break;
                }
                function->has_variadic=true;
                /* A captures_self closure (`closure name() ... end`) is
                 * the one shape where this doesn't reduce to plain
                 * `arity-1`: it reserves register 0 for a materialized
                 * self, exactly like a real method, but (unlike a real
                 * method) its own arity/required_arity are never inflated
                 * to account for that slot -- call_closure_helper's own
                 * needs_self_slot padding (src/vm.c) still shifts the
                 * *runtime* argument_count/arguments by +1 for it
                 * regardless, so the fixed_count operand here must add
                 * that slot back to line up with what DIAMOND_OP_
                 * COLLECT_VARIADIC actually sees at runtime -- confirmed
                 * directly (a self-capturing variadic closure's collected
                 * array was off by one, silently including its own first
                 * real parameter, before this fix). Every other owner
                 * shape (an ordinary method, whose arity already started
                 * at 1 for its own real implicit receiver; a plain
                 * top-level function or non-capturing nested def, which
                 * has no implicit slot and no runtime padding either)
                 * already has arity counted consistently with its own
                 * runtime layout, so only this one shape needs the
                 * adjustment. */
                const uint8_t fixed_count=captures_self?
                    function->arity:(uint8_t)(function->arity-1);
                variadic_parameter=parameter;variadic_fixed_count=fixed_count;
                declared_parameter_count++;
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                    advance_token(compiler);skip_newlines(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_AMPERSAND)
                        fail(compiler,compiler->current.span,
                            "only a block parameter may follow a variadic parameter");
                    else continue;
                }
                break;
            }
            if(is_block_parameter&&compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                fail(compiler,compiler->current.span,
                     "a block parameter cannot have a default value");break;
            }
            int parameter_type=-1;DiamondSpan parameter_type_span={};
            if (compiler->current.kind == DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                parameter_type_span=compiler->current.span;
                const int type = parse_type_annotation(compiler);
                parameter_type=type;
                if(declared_parameter_count<DIAMOND_MAX_DECLARED_PARAMETERS)
                    function->parameter_type_sets[declared_parameter_count]=
                        (uint16_t)type;
                if(is_block_parameter) {
                    block_parameter_type=type;
                    block_parameter_type_span=parameter_type_span;
                    compiler->current_block_type_set=(uint16_t)type;
                }
            }
            if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                saw_default=true;advance_token(compiler);
                const uint16_t provided=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,provided,
                                 (uint8_t)(function->arity-1),0,2);
                const size_t skip=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,provided);
                const uint16_t fallback=parse_expression(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,parameter,fallback,0,2);
                patch_jump(compiler,skip,compiler->function->code_count);
            } else if(saw_default&&!is_block_parameter) {
                fail(compiler,compiler->previous.span,
                     "required parameter cannot follow a default parameter");
            } else if(!is_block_parameter)
                function->required_arity++;
            if(parameter_type>=0&&!is_block_parameter) {
                emit_type_check(compiler,parameter,(uint16_t)parameter_type,
                                parameter_type_span);
                compiler->known_type_sets[parameter]=(int32_t)parameter_type;
                const DiamondTypeSet *parameter_set=
                    &function->type_sets[(size_t)parameter_type];
                if(parameter_set->count==1)
                    compiler->known_types[parameter]=parameter_set->members[0].id;
            }
            record_scope_type_fact(compiler,parameter,
                                   compiler->locals[compiler->local_count-1].name.start);
            declared_parameter_count++;
            skip_newlines(compiler);
            if(is_block_parameter&&compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                fail(compiler,compiler->current.span,
                     "a block parameter must be the last parameter");break;
            }
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        } while (!compiler->failed);
    }
    if(function->has_variadic&&!compiler->failed)
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            variadic_parameter,variadic_fixed_count,has_block_parameter?1:0,3);
    function->has_block_parameter=has_block_parameter;
    if(block_parameter_type>=0&&!compiler->failed) {
        const uint16_t absent=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,absent,0,0,1);
        const uint16_t missing=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_EQUAL,missing,
            compiler->current_block_register,absent,3);
        const size_t skip=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,missing);
        emit_type_check(compiler,compiler->current_block_register,
            (uint16_t)block_parameter_type,block_parameter_type_span);
        patch_jump(compiler,skip,compiler->function->code_count);
    }
    if (!compiler->failed && compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after parameters");
    }
    /* A module_function-mode method's exported (qualified-call-reachable)
     * singleton descriptor used to only get registered *after* this
     * whole def's body finished compiling (see the module_function_mode
     * block below, further down) -- so a self-referencing qualified call
     * inside that same body (`ModuleName.method(...)` calling itself,
     * directly or through a sibling also defined via module_function)
     * could never find its own not-yet-registered descriptor: "undefined
     * module singleton function" at compile time, confirmed as a real,
     * previously-undiscovered gap, not a documented cut, building
     * packages/graphql. Fixed by registering it *here* instead --
     * function->name/arity/required_arity/has_variadic are all already
     * final at this exact point (the parameter list, and only the
     * parameter list, is what determines them), well before body
     * compilation starts -- so a recursive qualified call inside the
     * body can resolve normally. The module_function_mode block further
     * down still runs (marking module->methods[]'s own instance-style
     * entry private, same as always), it just skips re-adding to
     * module->singleton_methods[] a second time when this flag is set. */
    bool module_function_singleton_registered_early=false;
    if(!compiler->failed && compiler->current_module>=0 && !module_singleton &&
       at_top_level && compiler->module_function_mode) {
        DiamondModule *early_module=
            &compiler->program->modules[(size_t)compiler->current_module];
        /* Deliberately NOT checking function->uses_instance_state here,
         * unlike the later (already-existing) registration site that
         * also checks it -- this flag only becomes accurate once the
         * body has actually been compiled (it's set while compiling an
         * `@ivar` access, which hasn't happened yet at this point in the
         * def), so checking it now would just always read false
         * regardless of what the body turns out to contain. The later
         * site still runs this check for real, after the body compiles;
         * if it turns out this method IS stateful, `fail` there marks
         * the whole compilation failed the same as always, and it not
         * mattering that a descriptor was already speculatively written
         * into singleton_methods[] below -- a failed compilation never
         * produces a runnable program either way. */
        /* A sibling module_function method defined *later* in the same
         * module/file still needs to be callable (qualified) from a
         * method compiled earlier -- e.g. mutually recursive is_even/
         * is_odd. diamond_compile's own discovery pass already visits
         * every def in the module in source order and reaches this same
         * early-registration code, so every sibling's descriptor exists
         * in discovery->modules[...].singleton_methods[] by the time
         * discovery finishes; that whole DiamondModule (a plain,
         * pointer-free embedded struct) gets bulk-memcpy'd into the real
         * pass's own program->modules before it starts (see
         * diamond_compile), so those descriptors -- forward references
         * included -- would already be usable, IF compile_module's own
         * module-reopen reset didn't unconditionally wipe
         * singleton_methods back to empty first (a plain "declared_by_
         * discovery means stale, zero it" rule that's correct for
         * module->methods[]/fields[] but not for this table -- see
         * compile_module's own comment on why it stopped doing that).
         * function_index carries over correctly unchanged: discovery and
         * the real pass reserve functions in identical source order (see
         * diamond_compile's own "Reserve every compiler-created function
         * at its discovery-pass index" comment), so a discovery-pass
         * function_index already names the right real-pass slot even
         * before that slot's body is recompiled.
         *
         * Claimed *positionally*, exactly like Compiler's own
         * next_function_claim, not by name: the real pass visits this
         * module's declarations in the same source order discovery did,
         * so early_module->next_singleton_claim (reset once, when
         * compile_module first re-touches this module) always names the
         * matching discovery-pass entry to refresh -- arity/required_
         * arity/has_variadic/name can't actually differ pass-to-pass for
         * the same source, but overwriting them is free and keeps this
         * from silently trusting stale data if that ever changes. Once
         * every discovery-carried-over entry has been claimed, any
         * further registration is a genuinely new entry (append), same
         * as it always was. */
        /* A prescan_singleton_signatures placeholder (this exact
         * pass, discovery only -- see that function's own comment) is
         * claimed by NAME, ahead of (and independent from) positional
         * next_singleton_claim claiming: it's how an *earlier*-compiled
         * sibling within this same discovery walk already got a
         * resolvable qualified call to THIS def, via a pending fixup
         * (emit_pending_singleton_function_index) that only this exact
         * moment -- this def's real function_index finally existing --
         * can resolve. */
        size_t prescanned=early_module->singleton_method_count;
        if(compiler->discovery_pass)
            for(size_t index=0;index<early_module->singleton_method_count;index++)
                if(early_module->singleton_methods[index].function_index==
                       DIAMOND_UNRESOLVED_SINGLETON_FUNCTION&&
                   strncmp(early_module->singleton_methods[index].name,
                           function->name,copy_length)==0&&
                   early_module->singleton_methods[index].name[copy_length]=='\0') {
                    prescanned=index;break;
                }
        const bool early_claiming=!compiler->discovery_pass&&
            early_module->next_singleton_claim<early_module->singleton_method_count;
        if(prescanned==early_module->singleton_method_count&&!early_claiming&&
           early_module->singleton_method_count==DIAMOND_MAX_METHODS) {
            fail(compiler,name,"too many module singleton functions");
        } else {
            DiamondMethod exported={0};
            for(size_t i=0;i<copy_length;i++)exported.name[i]=function->name[i];
            exported.name[copy_length]='\0';
            exported.function_index=(uint16_t)function_index;
            /* function->arity/required_arity both carry an implicit +1
             * here for the module-mixing receiver slot every direct
             * module instance method reserves in register 0 (see this
             * function's own direct_module_member seeding, well above
             * the parameter loop) -- subtracted back out, exactly like
             * the later (already-existing) registration site does when
             * copying function's arity into module->methods[]'s own
             * entry, so a qualified call's own arity check isn't off by
             * one. */
            exported.arity=(uint8_t)(function->arity-1);
            exported.required_arity=(uint8_t)(function->required_arity-1);
            exported.has_variadic=function->has_variadic;
            exported.included=false;
            exported.is_private=false;exported.is_protected=false;
            exported.needs_receiver=true;
            if(prescanned<early_module->singleton_method_count) {
                early_module->singleton_methods[prescanned]=exported;
                resolve_pending_singleton_fixups(compiler,
                    &early_module->singleton_methods[prescanned],function_index);
            } else if(early_claiming)
                early_module->singleton_methods[
                    early_module->next_singleton_claim++]=exported;
            else
                early_module->singleton_methods[
                    early_module->singleton_method_count++]=exported;
            module_function_singleton_registered_early=true;
        }
    }
    if(!compiler->failed && !at_top_level) {
        for(size_t i=0;i<compiler->enclosing_local_count;i++) {
            if(find_local(compiler,compiler->enclosing_locals[i].name)>=0)continue;
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->enclosing_locals[i].name,"too many lexical bindings");break;
            }
            const uint16_t cell=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE_CELL,cell,(uint8_t)i,0,2);
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->enclosing_locals[i].name,.reg=cell,.captured=true,
                .alias_identity=compiler->enclosing_locals[i].alias_identity};
            definition_captured_fact_registers[i]=cell;
            const uint16_t source=compiler->enclosing_locals[i].reg;
            if(source<outer_next_register) {
                compiler->known_types[cell]=outer_known_types[source];
                const int32_t source_set=outer_known_type_sets[source];
                if(source_set>=0&&
                   (size_t)source_set<outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)source_set);
                    if(cloned!=DIAMOND_NO_TYPE_SET)
                        compiler->known_type_sets[cell]=(int32_t)cloned;
                }
                const int32_t tooling_set=outer_tooling_type_sets[source];
                if(tooling_set>=0&&
                   (size_t)tooling_set<outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)tooling_set);
                    if(cloned!=DIAMOND_NO_TYPE_SET)
                        compiler->tooling_type_sets[cell]=(int32_t)cloned;
                }
            }
        }
    }
    int return_type = -1;
    DiamondSpan return_type_span = {};
    if (!compiler->failed) advance_token(compiler);
    if (!compiler->failed && compiler->current.kind == DIAMOND_TOKEN_ARROW) {
        advance_token(compiler);
        return_type_span=compiler->current.span;
        return_type = parse_type_annotation(compiler);
        function->return_type_set=(uint16_t)return_type;
    }
    compiler->in_function=true;
    compiler->in_block=false;
    compiler->current_return_type=return_type;
    compiler->current_return_type_span=return_type_span;
    DiamondFunction *const outer_method_function=compiler->method_function;
    const int outer_method_return_type=compiler->method_return_type;
    const DiamondSpan outer_method_return_type_span=compiler->method_return_type_span;
    const bool outer_method_has_block_return=compiler->method_has_block_return;
    compiler->method_function=function;
    compiler->method_return_type=return_type;
    compiler->method_return_type_span=return_type_span;
    compiler->method_has_block_return=false;
    const bool endless=!compiler->failed&&
        compiler->current.kind==DIAMOND_TOKEN_EQUAL;
    uint16_t body_result=0;
    bool body_diverges=false;
    if(endless) {
        advance_token(compiler);
        /* The body may start on the next line: `def f(x) =` / `  x + 1`. */
        skip_newlines(compiler);
        const DiamondTokenKind postfix=postfix_modifier_ahead(compiler,nullptr);
        const bool has_postfix=postfix==DIAMOND_TOKEN_IF||
                               postfix==DIAMOND_TOKEN_UNLESS;
        const uint16_t postfix_result=has_postfix?allocate_register(compiler):0;
        const size_t condition_jump=has_postfix
            ? emit_jump(compiler,DIAMOND_OP_JUMP,0):SIZE_MAX;
        const size_t body_start=compiler->function->code_count;
        body_result=return_type>=0?
            parse_with_expected_set(compiler,(uint16_t)return_type):
            parse_expression(compiler);
        if(has_postfix) {
            if(compiler->current.kind!=postfix) {
                fail(compiler,compiler->current.span,"expected postfix condition");
            } else {
                advance_token(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,postfix_result,
                                 body_result,0,2);
                const size_t body_exit=emit_jump(compiler,DIAMOND_OP_JUMP,0);
                const size_t condition_start=compiler->function->code_count;
                const uint16_t condition=parse_expression(compiler);
                const size_t body_jump=emit_jump(
                    compiler,postfix==DIAMOND_TOKEN_IF
                        ? DIAMOND_OP_JUMP_IF_TRUE:DIAMOND_OP_JUMP_IF_FALSE,
                    condition);
                emit_instruction(compiler,DIAMOND_OP_NIL,postfix_result,0,0,1);
                patch_jump(compiler,condition_jump,condition_start);
                patch_jump(compiler,body_exit,compiler->function->code_count);
                patch_jump(compiler,body_jump,body_start);
                body_result=postfix_result;
            }
        }
    } else {
        if(!compiler->failed)(void)consume_block_start(compiler);
        body_result=compiler->failed?0:compile_sequence(compiler);
        body_diverges=!compiler->failed&&compiler->sequence_diverges;
    }
    if (!compiler->failed) {
        if (return_type >= 0 && !body_diverges) {
            emit_type_check(compiler,body_result,(uint16_t)return_type,
                            return_type_span);
        } else if(return_type<0) {
            /* No explicit `-> Type`: best-effort inference for lsp/
             * receiver.c's call-chain resolution only (inferred_return_
             * type_set, not return_type_set itself -- see that field's
             * own comment in vm.h for why they're kept separate).
             * Includes advisory call-result facts; unlike compile_block's
             * checked return inference, these never form a runtime contract.
             * Combines two
             * independent sources of "what this function can actually
             * return": the body's own trailing value (skipped when it
             * always raises/returns early -- body_diverges means
             * body_result never actually produced anything real) and
             * every explicit `return value` compile_return has
             * accumulated along the way (return_flow_seen -- a function
             * using return statements across more than one branch had no
             * single inferable type before this, even though each
             * individual return's own value has a perfectly well-known
             * type at compile time; see return_flow_seen's own comment,
             * the Compiler struct). Either source alone is used as-is;
             * both together get unioned via merge_flow_types, the same
             * control-flow-join primitive an if/case expression's own
             * per-branch merge already uses (e.g. an unannotated
             * function with a bare `if`/`case` as its own last statement
             * already got this for free -- merge_flow_types already
             * writes the merged set onto that expression's own
             * destination register unconditionally, so body_result
             * already carries it by the time this reads it; the real
             * gap this closes is a function using return statements
             * across separate branches instead, which body_result alone
             * can never see). */
            uint16_t inferred_type=TYPE_UNKNOWN;int32_t inferred_set=-1;
            bool have_inference=false;
            if(!body_diverges&&!compiler->method_has_block_return) {
                inferred_type=compiler->known_types[body_result];
                inferred_set=compiler->tooling_type_sets[body_result]>=0?
                    compiler->tooling_type_sets[body_result]:
                    compiler->known_type_sets[body_result];
                /* A reachable unknown tail still participates in the join;
                 * dropping it would publish only the known early returns. */
                have_inference=true;
            }
            if(compiler->return_flow_seen&&!compiler->method_has_block_return) {
                if(have_inference)
                    merge_flow_types(compiler,inferred_type,inferred_set,
                        compiler->return_tooling_type,compiler->return_tooling_set,
                        &inferred_type,&inferred_set);
                else {
                    inferred_type=compiler->return_tooling_type;
                    inferred_set=compiler->return_tooling_set;
                }
                have_inference=true;
            }
            if(have_inference) {
                if(inferred_set>=0) {
                    function->inferred_return_type_set=(uint16_t)inferred_set;
                } else if(inferred_type!=TYPE_UNKNOWN&&
                          reserve_type_sets(compiler,1)) {
                    const size_t return_set=function->type_set_count++;
                    DiamondTypeSet *set=&function->type_sets[return_set];
                    set->count=1;set->inferred=true;
                    set->members[0]=(DiamondTypeMember){
                        .id=inferred_type,
                        .argument_set=DIAMOND_NO_TYPE_SET,
                        .second_argument_set=DIAMOND_NO_TYPE_SET,
                        .callable_arity=UINT8_MAX,
                        .callable_return_set=DIAMOND_NO_TYPE_SET,
                        .callable_parameters_typed=false};
                    for(size_t parameter=0;parameter<16;parameter++)
                        set->members[0].callable_parameter_sets[parameter]=
                            DIAMOND_NO_TYPE_SET;
                    function->inferred_return_type_set=(uint16_t)return_set;
                }
            }
        }
        maybe_rewrite_self_tail_call(compiler,body_result);
        emit_instruction(compiler, DIAMOND_OP_RETURN, body_result, 0, 0, 1);
        if (!endless&&compiler->current.kind != DIAMOND_TOKEN_END) {
            fail(compiler, compiler->current.span, "expected 'end' after function body");
        } else if(!endless) {
            advance_token(compiler);
        }
    }

    function->capture_count=(uint8_t)compiler->capture_count;
    function->register_count=compiler->next_register;
    uint16_t captures[DIAMOND_MAX_CAPTURES];
    for(size_t i=0;i<compiler->capture_count;i++)captures[i]=compiler->capture_registers[i];
    const size_t capture_count=compiler->capture_count;
    if(!compiler->failed) {
        const size_t body_end=compiler->previous.span.start+compiler->previous.span.length;
        record_scope_locals(compiler,0,compiler->local_count,body_end);
        function->body_end=body_end;
    }
    uint16_t definition_captured_fact_types[DIAMOND_MAX_LOCALS];
    int32_t definition_captured_fact_sets[DIAMOND_MAX_LOCALS];
    int32_t definition_captured_fact_tooling_sets[DIAMOND_MAX_LOCALS];
    for(size_t index=0;index<outer_local_count;index++) {
        const uint16_t reg=definition_captured_fact_registers[index];
        definition_captured_fact_types[index]=
            reg==DIAMOND_NO_TYPE_SET?TYPE_UNKNOWN:compiler->known_types[reg];
        definition_captured_fact_sets[index]=
            reg==DIAMOND_NO_TYPE_SET?-1:compiler->known_type_sets[reg];
        definition_captured_fact_tooling_sets[index]=
            reg==DIAMOND_NO_TYPE_SET?-1:compiler->tooling_type_sets[reg];
    }
    compiler->function = outer_function;
    compiler->begin_depth=outer_begin_depth;
    compiler->local_count = outer_local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        compiler->locals[index] = outer_locals[index];
    }
    compiler->next_register = outer_next_register;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->current_method = outer_method;
    compiler->in_method = outer_in_method;
    compiler->in_singleton_method = outer_in_singleton_method;
    compiler->in_function=outer_in_function;
    compiler->in_block=outer_in_block;
    compiler->current_definition=outer_definition;
    compiler->current_definition_name=outer_definition_name;
    compiler->has_current_block=outer_has_current_block;
    compiler->current_block_register=outer_current_block_register;
    compiler->current_block_type_set=outer_current_block_type_set;
    compiler->method_function=outer_method_function;
    compiler->method_return_type=outer_method_return_type;
    compiler->method_return_type_span=outer_method_return_type_span;
    compiler->method_has_block_return=outer_method_has_block_return;
    compiler->current_return_type=outer_return_type;
    compiler->current_return_type_span=outer_return_type_span;
    compiler->return_flow_seen=outer_return_flow_seen;
    compiler->return_flow_type=outer_return_flow_type;
    compiler->return_flow_set=outer_return_flow_set;
    compiler->return_tooling_type=outer_return_tooling_type;
    compiler->return_tooling_set=outer_return_tooling_set;
    compiler->current_exception=outer_exception;
    compiler->current_retry_target=outer_retry_target;
    compiler->current_loop=outer_loop;
    compiler->enclosing_local_count=outer_enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_enclosing_locals[i];
    compiler->capture_count=outer_capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        compiler->capture_registers[i]=outer_capture_registers[i];
    for(size_t index=0;index<outer_next_register;index++)
        {compiler->known_types[index]=outer_known_types[index];
         compiler->known_type_sets[index]=outer_known_type_sets[index];
         compiler->tooling_type_sets[index]=outer_tooling_type_sets[index];}
    for(size_t index=0;index<outer_local_count;index++) {
        if(definition_captured_fact_registers[index]==DIAMOND_NO_TYPE_SET)continue;
        const uint16_t source=outer_locals[index].reg;
        if(source>=outer_next_register)continue;
        compiler->known_types[source]=definition_captured_fact_types[index];
        const int32_t inner_set=definition_captured_fact_sets[index];
        compiler->known_type_sets[source]=-1;
        if(inner_set>=0&&(size_t)inner_set<function->type_set_count) {
            const uint16_t cloned=clone_type_set_into_current(compiler,
                function->type_sets,function->type_set_count,
                (uint16_t)inner_set);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->known_type_sets[source]=(int32_t)cloned;
        }
        const int32_t inner_tooling_set=
            definition_captured_fact_tooling_sets[index];
        compiler->tooling_type_sets[source]=-1;
        if(inner_tooling_set>=0&&
           (size_t)inner_tooling_set<function->type_set_count) {
            const uint16_t cloned=clone_type_set_into_current(compiler,
                function->type_sets,function->type_set_count,
                (uint16_t)inner_tooling_set);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->tooling_type_sets[source]=(int32_t)cloned;
        }
        record_scope_type_fact(compiler,source,compiler->current.span.start);
    }
    free(heap_outer_known_types);free(heap_outer_known_type_sets);
    free(heap_outer_tooling_type_sets);
    if(compiler->current_class>=0&&!module_singleton&&
       !compiler->failed&&at_top_level) {
        DiamondClass *class = compiler->program->classes[(size_t)compiler->current_class];
        bool duplicate=false;
        size_t duplicate_index=0;
        for(size_t existing=0;existing<class->method_count;existing++)
            if(!class->methods[existing].included&&
               strcmp(class->methods[existing].name,function->name)==0) {
                duplicate=true;duplicate_index=existing;
            }
        /* A struct's own generated == or to_s may be replaced by a
         * hand-written one in its body (never initialize or a field
         * reader): the generated entry is overwritten in place, so
         * dispatch finds the user's method under the same name. */
        bool replacing_generated=false;
        if(duplicate&&compiler->struct_replaceable_class==compiler->current_class) {
            if(compiler->struct_replaceable_eq&&strcmp(function->name,"==")==0) {
                compiler->struct_replaceable_eq=false;replacing_generated=true;
            } else if(compiler->struct_replaceable_to_s&&
                      strcmp(function->name,"to_s")==0) {
                compiler->struct_replaceable_to_s=false;replacing_generated=true;
            }
        }
        if(replacing_generated) {
            /* Drop the generated entry and let the ordinary append below
             * add the user's method, so it sits where any hand-written
             * method would: dispatch prefers the latest match by name, and
             * an `include Comparable` earlier in the body has already
             * appended its own derived == after the generated one. */
            for(size_t shift=duplicate_index+1;shift<class->method_count;shift++)
                class->methods[shift-1]=class->methods[shift];
            class->method_count--;
            duplicate=false;
        }
        if(class->method_count==DIAMOND_MAX_METHODS||duplicate) {
            fail(compiler, name, "duplicate or excessive method definition");
        } else {
            DiamondMethod *method = &class->methods[class->method_count++];
            for (size_t i=0;i<copy_length;i++) method->name[i]=function->name[i];
            method->name[copy_length]='\0';
            method->function_index=(uint16_t)function_index;
            method->arity=(uint8_t)(function->arity-1);
            method->required_arity=(uint8_t)(function->required_arity-1);
            method->has_variadic=function->has_variadic;
            method->included=false;
            method->is_private=compiler->methods_private;
            method->is_protected=compiler->methods_protected;
        }
    } else if(compiler->current_class>=0&&module_singleton&&
              !compiler->failed&&at_top_level) {
        DiamondClass *class=
            compiler->program->classes[(size_t)compiler->current_class];
        DiamondMethod *prescanned=nullptr;
        bool duplicate=false;
        for(size_t existing=0;existing<class->singleton_method_count;existing++) {
            DiamondMethod *method=&class->singleton_methods[existing];
            if(strcmp(method->name,function->name)!=0)continue;
            if(method->function_index==DIAMOND_UNRESOLVED_SINGLETON_FUNCTION||
               method->function_index==function_index)
                prescanned=method;
            else duplicate=true;
        }
        if(duplicate||(prescanned==nullptr&&class->singleton_method_count==DIAMOND_MAX_METHODS))
            fail(compiler,name,"duplicate or excessive class singleton method");
        else {
            DiamondMethod *method=prescanned!=nullptr?prescanned:
                &class->singleton_methods[class->singleton_method_count++];
            for(size_t i=0;i<copy_length;i++)method->name[i]=function->name[i];
            method->name[copy_length]='\0';method->function_index=(uint16_t)function_index;
            /* Class-owned (unlike module) singleton methods now reserve
             * register 0 for an implicit `self` -- function->arity/
             * required_arity already include that slot (compile_definition's
             * direct_class_singleton_member branch), so the method table's
             * own arity (what external callers are checked against, see
             * parse_singleton_call) must subtract it back out, the same
             * way an ordinary instance method's entry does just above.
             * needs_receiver tells parse_singleton_call's call site to
             * reserve and fill that slot instead of leaving it zero-inited. */
            method->arity=(uint8_t)(function->arity-1);
            method->required_arity=(uint8_t)(function->required_arity-1);
            method->has_variadic=function->has_variadic;
            method->needs_receiver=true;
            if(prescanned!=nullptr)
                resolve_pending_singleton_fixups(compiler,method,function_index);
        }
    } else if(compiler->current_module>=0&&!module_singleton&&
              !compiler->failed&&at_top_level) {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
        for(size_t existing=0;existing<module->method_count;existing++)
            if(!module->methods[existing].included&&
               strcmp(module->methods[existing].name,function->name)==0) {
                fail(compiler,name,"duplicate module method");break;
            }
        if(!compiler->failed) {
            if(module->method_count==DIAMOND_MAX_METHODS)
                fail(compiler,name,"too many module methods");
            else {
                DiamondMethod *method=&module->methods[module->method_count++];
                for(size_t i=0;i<copy_length;i++)method->name[i]=function->name[i];
                method->name[copy_length]='\0';
                method->function_index=(uint16_t)function_index;
                method->arity=(uint8_t)(function->arity-1);
                method->required_arity=(uint8_t)(function->required_arity-1);
                method->has_variadic=function->has_variadic;
                method->included=false;
                method->is_private=compiler->methods_private;
                method->is_protected=compiler->methods_protected;
                if(compiler->module_function_mode) {
                    if(function->uses_instance_state)
                        fail(compiler,name,
                            "stateful method cannot use module_function mode");
                    else if(module_function_singleton_registered_early) {
                        /* Already added to singleton_methods[] above,
                         * before the body compiled -- just the private-
                         * marking side effect remains to do here. */
                        method->is_private=true;method->is_protected=false;
                    }
                    else if(module->singleton_method_count==DIAMOND_MAX_METHODS)
                        fail(compiler,name,"too many module singleton functions");
                    else {
                        method->is_private=true;method->is_protected=false;
                        DiamondMethod exported=*method;
                        exported.is_private=false;exported.is_protected=false;
                        exported.needs_receiver=true;
                        module->singleton_methods[
                            module->singleton_method_count++]=exported;
                    }
                }
            }
        }
    } else if(compiler->current_module>=0&&module_singleton&&
              !compiler->failed&&at_top_level) {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
        /* Same positional claiming as compile_definition's own
         * module_function early-registration (see that site's comment,
         * and DiamondModule's next_singleton_claim field, for the full
         * rationale): a discovery pass that reached this point at all
         * already ran the duplicate check below and would have failed
         * compilation on a genuine `def self.x` repeated within the
         * same module, so the real pass re-visiting an as-yet-unclaimed
         * carried-over entry can safely skip straight to refreshing it
         * in place, in source order, with no risk of masking a real
         * duplicate. */
        /* See compile_definition's own module_function early-registration
         * comment on prescan_singleton_signatures/resolve_pending_
         * singleton_fixups -- identical name-based claiming for a `def
         * self.x` signature that scan already found. */
        size_t prescanned=module->singleton_method_count;
        if(compiler->discovery_pass)
            for(size_t index=0;index<module->singleton_method_count;index++)
                if(module->singleton_methods[index].function_index==
                       DIAMOND_UNRESOLVED_SINGLETON_FUNCTION&&
                   strncmp(module->singleton_methods[index].name,
                           function->name,copy_length)==0&&
                   module->singleton_methods[index].name[copy_length]=='\0') {
                    prescanned=index;break;
                }
        const bool claiming=!compiler->discovery_pass&&
            module->next_singleton_claim<module->singleton_method_count;
        bool duplicate=false;
        if(prescanned==module->singleton_method_count&&!claiming)
            for(size_t existing=0;existing<module->singleton_method_count;existing++)
                if(strcmp(module->singleton_methods[existing].name,
                          function->name)==0)duplicate=true;
        if(prescanned==module->singleton_method_count&&!claiming&&
           (duplicate||module->singleton_method_count==DIAMOND_MAX_METHODS))
            fail(compiler,name,"duplicate or excessive module singleton function");
        else {
            DiamondMethod *method=prescanned<module->singleton_method_count?
                &module->singleton_methods[prescanned]:
                (claiming?
                    &module->singleton_methods[module->next_singleton_claim++]:
                    &module->singleton_methods[module->singleton_method_count++]);
            for(size_t i=0;i<copy_length;i++)method->name[i]=function->name[i];
            method->name[copy_length]='\0';
            method->function_index=(uint16_t)function_index;
            method->arity=function->arity;
            method->required_arity=function->required_arity;
            method->has_variadic=function->has_variadic;
            if(prescanned<module->singleton_method_count)
                resolve_pending_singleton_fixups(compiler,method,function_index);
        }
    }
    /* A class/module member def's "value" is never read: both call sites
     * that reach this point with at_top_level true and current_class/
     * current_module set (compile_class's and compile_module's own
     * DIAMOND_TOKEN_DEF branches) discard compile_definition's return with
     * an explicit (void). Only a genuine top-level def -- one that could be
     * the final statement compile_sequence threads through as the whole
     * sequence's value -- needs a real, permanently-reserved register here.
     * Register allocation is monotonic and never recycled within a
     * function body, so for a large class this reservation is pure, cumulative
     * waste against the entry function's own register ceiling --
     * confirmed the hard way while porting the self-hosted parser, which is
     * also why that ceiling was later widened (256 -> 4096). */
    const bool member_result_discarded =
        at_top_level && (compiler->current_class>=0||compiler->current_module>=0);
    const uint16_t result =
        member_result_discarded ? 0 : allocate_register(compiler);
    if(!at_top_level) {
        /* Always emit BOX_LOCAL here, even if this local was already boxed
         * at an earlier capture site elsewhere in the function: that earlier
         * site might be in a sibling if/else branch that doesn't dominate
         * this one, so it may not actually have executed on this runtime
         * path. DIAMOND_OP_BOX_LOCAL is idempotent (a no-op if the register
         * already holds a Cell), so emitting it redundantly on paths where
         * the earlier boxing did run is always safe. */
        for(size_t i=0;i<capture_count;i++) {
            for(size_t local=0;local<compiler->local_count;local++) {
                if(compiler->locals[local].reg!=captures[i])continue;
                emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,captures[i],0,0,1);
                compiler->locals[local].captured=true;
                break;
            }
        }
        emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
        emit_function_index(compiler,function_index);emit_byte(compiler,(uint8_t)capture_count);
        for(size_t i=0;i<capture_count;i++)emit_register(compiler,captures[i]);
        if(self_local>=0) {
            const uint16_t self_register=compiler->locals[(size_t)self_local].reg;
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,self_register,0,0,1);
            emit_instruction(compiler,DIAMOND_OP_SET_CELL,self_register,result,0,2);
            compiler->locals[(size_t)self_local].captured=true;
        } else {
            compiler->locals[compiler->local_count++]=(Local){.name=name,.reg=result,
                .captured=compiler->loop_captures_pending};
        }
    }
    /* else: top-level def -- no NIL needed. A genuine top-level def's
     * `result` is the sole writer of its (allocated) register, already
     * zero-inited; a class/module member's `result` is the unallocated
     * placeholder 0 from above, safe only because both callers that reach
     * this path discard it. */
    return result;
}
