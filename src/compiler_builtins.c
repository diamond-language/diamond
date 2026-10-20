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

/* Call parsers for the native builtins (File, Dir, Time, Process, TCP/TLS/UDP, crypto, tensors,
 * databases and the like): each turns one builtin call into its opcode sequence. */

static uint16_t parse_file_path_unary_call(Compiler *compiler,DiamondFilePathFunction id);
static uint16_t parse_file_path_binary_call(Compiler *compiler,DiamondFilePathFunction id);

uint16_t parse_redefine_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'redefine_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'redefine_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after redefine_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t callable_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after redefine_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_REDEFINE_METHOD);
    emit_register(compiler,dest);
    emit_register(compiler,(uint16_t)class_index);
    emit_register(compiler,name_register);
    emit_register(compiler,callable_register);
    compiler->known_types[dest] = DIAMOND_TYPE_NIL;
    /* See DiamondProgram.uses_redefine_method's own comment: this is the
     * one place DIAMOND_OP_REDEFINE_METHOD is ever emitted. */
    compiler->program->uses_redefine_method = true;
    return dest;
}

/* `ClassName.define_method(name, callable)` -- same shape as
 * redefine_method above (mirrors it line for line at the parser level;
 * the two opcodes differ only in what the VM does with an existing vs.
 * absent method slot, see vm.c's own handler comments for both). */
uint16_t parse_define_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'define_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'define_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after define_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t callable_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after define_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_DEFINE_METHOD);
    emit_register(compiler,dest);
    emit_register(compiler,(uint16_t)class_index);
    emit_register(compiler,name_register);
    emit_register(compiler,callable_register);
    compiler->known_types[dest] = DIAMOND_TYPE_NIL;
    return dest;
}

/* `ClassName.compile_method(name, params, body_source, bound_values)` --
 * compiles a new method body from a source string at runtime and returns
 * a Callable meant to be passed to define_method above (not
 * redefine_method, and not general Callable use -- see vm.c's own
 * DIAMOND_OP_COMPILE_METHOD/DIAMOND_OP_DEFINE_METHOD handler comments
 * and docs/design.md's "Runtime method synthesis" section for the full
 * scope). `bound_values` is a Hash of already-evaluated values (e.g. the
 * result of calling a *different* class's own method, which body_source
 * itself has no way to name directly -- see that same doc section)
 * spliced in as extra trailing parameters; pass `{}` when there are
 * none. Four arguments instead of redefine_method/define_method's two,
 * otherwise the same shape. */
uint16_t parse_compile_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'compile_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'compile_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after compile_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t params_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after compile_method params");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t body_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after compile_method body_source");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t bound_values_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after compile_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_COMPILE_METHOD);
    emit_register(compiler,dest);
    emit_register(compiler,(uint16_t)class_index);
    emit_register(compiler,name_register);
    emit_register(compiler,params_register);
    emit_register(compiler,body_register);
    emit_register(compiler,bound_values_register);
    compiler->known_types[dest] = DIAMOND_TYPE_CALLABLE;
    return dest;
}

uint16_t parse_fiber_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool creates=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"new",compiler->current.span,false);
    const bool yields=compiler->current.kind==DIAMOND_TOKEN_YIELD;
    if(!creates&&!yields) {
        fail(compiler,compiler->current.span,
            "expected 'new' or 'yield' after 'Fiber'");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
            creates?"expected '(' after 'Fiber.new'":
                    "expected '(' after 'Fiber.yield'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t value=0;
    if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
        if(creates) {
            fail(compiler,compiler->current.span,
                "Fiber.new requires a Callable argument");return 0;
        }
        value=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,value,0,0,1);
    } else value=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
            creates?"expected ')' after Fiber.new argument":
                    "Fiber.yield accepts at most one value");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,creates?DIAMOND_OP_FIBER_NEW:DIAMOND_OP_YIELD);
    emit_register(compiler,dest);
    emit_register(compiler,value);
    return dest;
}

/* `Thread.new(callable, *args)` -- unlike Fiber.new (exactly one zero-arg
 * closure, no other arguments: a suspended fiber gets later inputs via
 * .resume(value) instead), a spawned OS thread has no interactive resume
 * dialogue, so every input must be handed over here at construction. The
 * argument-list parsing/register-packing below mirrors
 * parse_closure_call_arguments's own shape (base register + consecutive
 * MOVEs) since the runtime call convention is the same; only the trailing
 * opcode differs (THREAD_NEW, not CALL_CLOSURE). See docs/threads.md. */
uint16_t parse_thread_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Thread'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Thread.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t callable_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t arguments[DIAMOND_MAX_ARGUMENTS]; size_t argument_count=0;
    while(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        if(argument_count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many Thread.new arguments");
            return 0;
        }
        arguments[argument_count++]=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Thread.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<argument_count;i++)(void)allocate_register(compiler);
    for(size_t i=0;i<argument_count;i++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i),arguments[i],0,2);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_THREAD_NEW);
    emit_register(compiler,dest);emit_register(compiler,callable_register);
    emit_register(compiler,base);emit_byte(compiler,(uint8_t)argument_count);
    return dest;
}

/* Channel.select(channels[, deadline]) and Channel.select_send(channels, value[, deadline])
 * -- see docs/threads.md's Channels section. The deadline is optional (nil when omitted). */
static uint16_t parse_channel_select_call(Compiler *compiler,bool send) {
    advance_token(compiler); /* consume 'select' / 'select_send' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,send?
            "expected '(' after 'Channel.select_send'":"expected '(' after 'Channel.select'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t channels_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t value_register=0;
    if(send) {
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,
                "Channel.select_send takes a channels Array and a value");
            return 0;
        }
        advance_token(compiler);
        skip_newlines(compiler);
        value_register=parse_expression(compiler);
        skip_newlines(compiler);
    }
    uint16_t deadline_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        deadline_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        deadline_register=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_NIL);
        emit_register(compiler,deadline_register);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,send?
            "expected ')' after Channel.select_send arguments":
            "expected ')' after Channel.select arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,send?DIAMOND_OP_CHANNEL_SELECT_SEND:DIAMOND_OP_CHANNEL_SELECT);
    emit_register(compiler,dest);
    emit_register(compiler,channels_register);
    if(send)emit_register(compiler,value_register);
    emit_register(compiler,deadline_register);
    return dest;
}

/* Channel.new(capacity) -- see docs/threads.md's Channels section. A
 * single required Int argument, unlike Thread.new's own variadic
 * callable+args shape just above -- closer to parse_time_at_call's own
 * single-argument construction pattern, just with the same "expect the
 * literal keyword 'new'" check parse_thread_new_call already needs
 * (Channel.new, like Thread.new/Fiber.new, is recognized by the literal
 * 'new' method name, not a dedicated bare `Channel(...)` form the way
 * File.open/SQLite3.open use their own distinct names). */
uint16_t parse_channel_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
       name_equals(compiler,"select",compiler->current.span,false))
        return parse_channel_select_call(compiler,false);
    if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
       name_equals(compiler,"select_send",compiler->current.span,false))
        return parse_channel_select_call(compiler,true);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,
            "expected 'new', 'select' or 'select_send' after 'Channel'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Channel.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t capacity_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Channel.new argument");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CHANNEL_NEW);
    emit_register(compiler,dest);
    emit_register(compiler,capacity_register);
    return dest;
}

/* Supervisor.new([strategy]) -- see docs/threads.md's Supervisors section.
 * One optional restart-strategy symbol (:one_for_one, :one_for_all,
 * :rest_for_one; checked at run time). Restart delay/child cap are fixed
 * constants, see DIAMOND_MAX_SUPERVISOR_CHILDREN's own comment in
 * src/vm.c. Just recognized by the literal 'new' method name the same
 * way Thread.new/Channel.new/Fiber.new are rather than a bare
 * `Supervisor(...)` form. */
uint16_t parse_supervisor_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Supervisor'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Supervisor.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t strategy_register;
    if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
        /* No strategy: a nil register, which the VM reads as :one_for_one. */
        strategy_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,strategy_register,0,0,1);
    } else {
        strategy_register=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Supervisor.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SUPERVISOR_NEW);
    emit_register(compiler,dest);
    emit_register(compiler,strategy_register);
    return dest;
}

/* The function's type set holding exactly one bare member of type id (no element
 * or callable detail), added on first use. */
uint16_t plain_type_set_index(Compiler *compiler,uint16_t type_id) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==type_id&&
           set->members[0].argument_set==DIAMOND_NO_TYPE_SET)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;
    set->members[0]=(DiamondTypeMember){.id=type_id,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t member_index=0;member_index<16;member_index++)
        set->members[0].callable_parameter_sets[member_index]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)set_index;
}

/* Array.new(n) and Array.new(n, fill): n copies of fill (nil when omitted),
 * compiled as `[fill] * n` so the VM's own Array * Int supplies the length
 * checks and the copying -- no opcode of its own. As in Ruby, every slot holds
 * the same fill value, not a copy of it. Array.new(n) { |i| ... } is not
 * supported. */
uint16_t parse_array_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Array'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Array.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const DiamondSpan length_span=compiler->current.span;
    const uint16_t length_register=parse_expression(compiler);
    /* `Array * String` joins, so a non-Int length must not reach the multiply. */
    emit_type_check(compiler,length_register,
        plain_type_set_index(compiler,DIAMOND_TYPE_INT),length_span);
    skip_newlines(compiler);
    uint16_t fill_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        fill_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        fill_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,fill_register,0,0,1);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
            "expected ')' after Array.new arguments (Array.new(n) { |i| ... } is not supported)");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE||
       compiler->current.kind==DIAMOND_TOKEN_DO) {
        fail(compiler,compiler->current.span,
            "Array.new does not take a block; build the array with n.times or map instead");
        return 0;
    }
    /* ARRAY reads its elements from consecutive registers starting at base. */
    const uint16_t base=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,base,fill_register,0,2);
    const uint16_t single=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,single,base,1,3);
    compiler->known_types[single]=DIAMOND_TYPE_ARRAY;
    return compile_binary_op(compiler,DIAMOND_TOKEN_STAR,single,length_register);
}

static uint16_t parse_file_open_arguments(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'File.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after File.open path");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t mode_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,path_register);
    emit_register(compiler,mode_register);
    return dest;
}

/* `File.delete(path)` -- single String argument, returns a Bool (true
 * if a file was actually removed, false if it didn't exist to begin
 * with) rather than File.open's raise-on-any-failure shape, since the
 * one real caller so far (an ingest script cleaning up a downloaded
 * file after a later step fails) wants "already gone" to be a normal,
 * checkable outcome, not an exception to rescue around. A real error
 * (permission denied, path is a directory, etc.) still raises
 * IOError, matching File.open. */
static uint16_t parse_file_delete_arguments(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'File.delete'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File.delete argument");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_DELETE);
    emit_register(compiler,dest);
    emit_register(compiler,path_register);
    compiler->known_types[dest]=DIAMOND_TYPE_BOOL;
    return dest;
}

/* `Dir.entries(path)` -- single String argument, mirrors
 * parse_file_delete_arguments's own single-fixed-argument shape
 * exactly. */
uint16_t parse_dir_entries_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    /* Dir.mkdir/delete/sync share DIAMOND_OP_FILE_PATH's fixed-arity shape. */
    if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER) {
        const DiamondSpan method=compiler->current.span;
        if(name_equals(compiler,"mkdir",method,false)) {
            advance_token(compiler);
            return parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_MKDIR);
        }
        if(name_equals(compiler,"delete",method,false)) {
            advance_token(compiler);
            return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_RMDIR);
        }
        if(name_equals(compiler,"sync",method,false)) {
            advance_token(compiler);
            return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_DIRSYNC);
        }
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"entries",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'entries', 'mkdir', 'delete' or 'sync' after 'Dir'");
        return 0;
    }
    advance_token(compiler); /* consume 'entries' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Dir.entries'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Dir.entries argument");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_DIR_ENTRIES);
    emit_register(compiler,dest);
    emit_register(compiler,path_register);
    compiler->known_types[dest]=DIAMOND_TYPE_ARRAY;
    return dest;
}

/* `File.join(*parts)` -- a variable number of String arguments, moved
 * into a contiguous register run the same way parse_thread_new_call's
 * own variadic argument list already is (DIAMOND_OP_FILE_JOIN, not the
 * fixed-arity DIAMOND_OP_FILE_PATH every other File path method below
 * shares). */
static uint16_t parse_file_join_arguments(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'File.join'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t arguments[DIAMOND_MAX_ARGUMENTS];size_t argument_count=0;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            if(argument_count==DIAMOND_MAX_ARGUMENTS) {
                fail(compiler,compiler->current.span,"too many File.join arguments");
                return 0;
            }
            arguments[argument_count++]=parse_expression(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File.join arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<argument_count;i++)(void)allocate_register(compiler);
    for(size_t i=0;i<argument_count;i++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i),arguments[i],0,2);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_JOIN);
    emit_register(compiler,dest);emit_register(compiler,base);
    emit_byte(compiler,(uint8_t)argument_count);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

/* `File.dirname(path)`/`.extname(path)` -- a single required String
 * argument, the fixed-arity DIAMOND_OP_FILE_PATH shape (a NIL second
 * argument register, unused by these two selectors). */
static uint16_t parse_file_path_unary_call(Compiler *compiler,DiamondFilePathFunction id) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after File path method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File path method argument");
        return 0;
    }
    advance_token(compiler);
    const uint16_t second_register=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,second_register,0,0,1);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_PATH);
    emit_register(compiler,dest);emit_register(compiler,path_register);
    emit_register(compiler,second_register);emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=
        (id==DIAMOND_FILE_PATH_ABSOLUTE||id==DIAMOND_FILE_PATH_DIRECTORY||
         id==DIAMOND_FILE_PATH_EXIST)?
        DIAMOND_TYPE_BOOL:DIAMOND_TYPE_STRING;
    return dest;
}

/* `File.basename(path, suffix = nil)`/`.expand_path(path, base = nil)`
 * -- a required String argument plus an optional second String,
 * defaulting to a compile-time NIL when omitted (same default-argument
 * shape parse_sqlite3_open_call's own optional mode uses). */
static uint16_t parse_file_path_binary_call(Compiler *compiler,DiamondFilePathFunction id) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after File path method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t second_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        second_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        if(id==DIAMOND_FILE_PATH_PUBLISH) {
            fail(compiler,compiler->current.span,"File.publish requires path and bytes");
            return 0;
        }
        if(id==DIAMOND_FILE_PATH_WRITE) {
            fail(compiler,compiler->current.span,"File.write requires a path and data");
            return 0;
        }
        if(id==DIAMOND_FILE_PATH_RENAME) {
            fail(compiler,compiler->current.span,"File.rename requires a source and destination path");
            return 0;
        }
        if(id==DIAMOND_FILE_PATH_COPY) {
            fail(compiler,compiler->current.span,"File.copy requires a source and destination path");
            return 0;
        }
        second_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,second_register,0,0,1);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File path method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_PATH);
    emit_register(compiler,dest);emit_register(compiler,path_register);
    emit_register(compiler,second_register);emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

/* `File.<method>(...)` dispatcher -- `.open` keeps its own two-required-
 * argument shape (DIAMOND_OP_FILE_OPEN, unchanged); `.join` is variadic
 * (DIAMOND_OP_FILE_JOIN); the rest share DIAMOND_OP_FILE_PATH's fixed-
 * arity shape, one or two String arguments picked apart by
 * parse_file_path_unary_call/parse_file_path_binary_call above. */
uint16_t parse_file_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected a method name after 'File.'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"open",method,false)) {
        advance_token(compiler);
        return parse_file_open_arguments(compiler);
    }
    if(name_equals(compiler,"sync",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_SYNC);
    }
    if(name_equals(compiler,"publish",method,false)) {
        advance_token(compiler);
        return parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_PUBLISH);
    }
    if(name_equals(compiler,"exist?",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_EXIST);
    }
    if(name_equals(compiler,"rename",method,false)) {
        advance_token(compiler);
        return parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_RENAME);
    }
    if(name_equals(compiler,"delete",method,false)) {
        advance_token(compiler);
        return parse_file_delete_arguments(compiler);
    }
    if(name_equals(compiler,"join",method,false)) {
        advance_token(compiler);
        return parse_file_join_arguments(compiler);
    }
    if(name_equals(compiler,"dirname",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_DIRNAME);
    }
    if(name_equals(compiler,"extname",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_EXTNAME);
    }
    if(name_equals(compiler,"absolute?",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_ABSOLUTE);
    }
    if(name_equals(compiler,"directory?",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_DIRECTORY);
    }
    if(name_equals(compiler,"basename",method,false)) {
        advance_token(compiler);
        return parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_BASENAME);
    }
    if(name_equals(compiler,"expand_path",method,false)) {
        advance_token(compiler);
        return parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_EXPAND);
    }
    if(name_equals(compiler,"read",method,false)) {
        advance_token(compiler);
        return parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_READ);
    }
    if(name_equals(compiler,"copy",method,false)) {
        advance_token(compiler);
        return parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_COPY);
    }
    if(name_equals(compiler,"size",method,false)) {
        advance_token(compiler);
        const uint16_t size=parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_SIZE);
        compiler->known_types[size]=DIAMOND_TYPE_INT;
        return size;
    }
    if(name_equals(compiler,"sha256",method,false)) {
        advance_token(compiler);
        const uint16_t digest=parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_SHA256);
        compiler->known_types[digest]=DIAMOND_TYPE_STRING;
        return digest;
    }
    if(name_equals(compiler,"mode",method,false)) {
        advance_token(compiler);
        const uint16_t mode=parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_MODE);
        compiler->known_types[mode]=DIAMOND_TYPE_INT;
        return mode;
    }
    if(name_equals(compiler,"file?",method,false)) {
        advance_token(compiler);
        const uint16_t result=parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_FILE);
        compiler->known_types[result]=DIAMOND_TYPE_BOOL;
        return result;
    }
    if(name_equals(compiler,"symlink?",method,false)) {
        advance_token(compiler);
        const uint16_t result=parse_file_path_unary_call(compiler,DIAMOND_FILE_PATH_SYMLINK);
        compiler->known_types[result]=DIAMOND_TYPE_BOOL;
        return result;
    }
    if(name_equals(compiler,"write",method,false)) {
        advance_token(compiler);
        const uint16_t written=parse_file_path_binary_call(compiler,DIAMOND_FILE_PATH_WRITE);
        compiler->known_types[written]=DIAMOND_TYPE_INT;
        return written;
    }
    fail(compiler,method,"unknown File method (expected open/read/write/copy/size/sha256/mode/exist?/file?/"
        "symlink?/delete/rename/publish/sync/join/dirname/basename/extname/absolute?/directory?/"
        "expand_path)");
    return 0;
}

/* `SQLite3.open(path)` or `SQLite3.open(path, mode)`. `mode`, when given,
 * is a String -- "r" (read-only, error if missing), "rw" (read-write, error
 * if missing), or "rwc" (read-write, created if missing -- sqlite3_open's
 * own default, spelled out explicitly rather than just being what you get
 * from omitting the argument) -- validated and translated to sqlite3_open_v2
 * flags at runtime (DIAMOND_OP_SQLITE3_OPEN), the same "string mode,
 * runtime-validated" shape File.open's own mode argument already uses.
 * Omitted mode compiles a NIL third register, the sentinel the VM checks to
 * fall back to plain sqlite3_open unchanged -- mirrors parse_exit_call's own
 * compile-time default-when-omitted pattern. */
uint16_t parse_sqlite3_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'SQLite3'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'SQLite3.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t mode_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        mode_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        mode_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,mode_register,0,0,1);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after SQLite3.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SQLITE3_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,path_register);
    emit_register(compiler,mode_register);
    return dest;
}

uint16_t parse_postgres_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'PostgreSQL'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'PostgreSQL.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t conninfo_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after PostgreSQL.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_POSTGRES_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,conninfo_register);
    return dest;
}

/* `MySQL.open(host, user, password, database, port)` -- unlike
 * PostgreSQL.open's single conninfo String (libpq parses that key=value
 * format itself), MariaDB Connector/C's mysql_real_connect wants these as
 * discrete arguments with no such string to parse, so this takes them the
 * same explicit way rather than inventing a DSN mini-language this project
 * would then own the parsing/escaping/documentation of. Five required
 * positional arguments, comma-separated like TCPSocket.connect's host/port
 * pair, just longer -- no optional/defaulted trailing argument the way
 * TCPServer.listen's reuse_port is, since a bind port default would hide a
 * real, easy-to-get-wrong choice (3306 vs. a nonstandard port) rather than
 * a rarely-needed knob. */
uint16_t parse_mysql_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'MySQL'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'MySQL.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t user_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open user");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t password_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open password");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t database_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open database");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after MySQL.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MYSQL_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,user_register);
    emit_register(compiler,password_register);
    emit_register(compiler,database_register);
    emit_register(compiler,port_register);
    return dest;
}

uint16_t parse_regexp_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Regexp'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Regexp.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t pattern_register=parse_expression(compiler);
    skip_newlines(compiler);
    /* options is optional -- Regexp.new(pattern) is the common case,
     * defaulting to a compile-time 0 constant (REGINOLD_OPTION_NONE)
     * rather than requiring every call site to spell it out, unlike
     * File.open's two always-required arguments. */
    uint16_t options_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        options_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        options_register=allocate_register(compiler);
        const uint16_t zero=add_constant(compiler,DIAMOND_INT(0));
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,options_register,zero,0,2);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Regexp.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_REGEXP_NEW);
    emit_register(compiler,dest);
    emit_register(compiler,pattern_register);
    emit_register(compiler,options_register);
    return dest;
}

/* ProgramBuilder.new() -- the one ProgramBuilder call needing dedicated
 * compiler recognition (constructing the object). Every instance method
 * (.declare_function/.emit_byte/.add_constant/.add_string/
 * .set_register_count/.run) dispatches through the ordinary INVOKE opcode
 * like any other native-kind receiver (Fiber/File/Regexp), needing no
 * compiler changes at all. */
uint16_t parse_program_builder_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'ProgramBuilder'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'ProgramBuilder.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after ProgramBuilder.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PROGRAM_BUILDER_NEW);
    emit_register(compiler,dest);
    return dest;
}

uint16_t parse_tcp_connect_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool nonblocking=name_equals(compiler,"connect_nonblocking",compiler->current.span,false);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       (!nonblocking&&!name_equals(compiler,"connect",compiler->current.span,false))) {
        fail(compiler,compiler->current.span,"expected 'connect' or 'connect_nonblocking' after 'TCPSocket'");
        return 0;
    }
    advance_token(compiler); /* consume 'connect' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TCPSocket.connect'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TCPSocket.connect host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t options_register=0;
    if(!nonblocking&&compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);skip_newlines(compiler);
        options_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else if(!nonblocking) {
        options_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,options_register,0,0,1);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TCPSocket.connect arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,nonblocking?DIAMOND_OP_TCP_CONNECT_NONBLOCK:DIAMOND_OP_TCP_CONNECT);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,port_register);
    if(!nonblocking)emit_register(compiler,options_register);
    return dest;
}

/* A string constant the compiler itself supplies (an option name), not source. */
static uint16_t emit_literal_string(Compiler *compiler,const char *text) {
    const size_t length=strlen(text);
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,compiler->previous.span,"out of memory growing function strings");
        return 0;
    }
    if(compiler->function->string_count==DIAMOND_MAX_STRING_CONSTANTS) {
        fail(compiler,compiler->previous.span,"too many or oversized names in function");
        return 0;
    }
    DiamondStringConstant *string=
        &compiler->function->strings[compiler->function->string_count];
    memcpy(string->chars,text,length);
    string->length=length;
    string->chars[length]='\0';
    const uint16_t index=(uint16_t)compiler->function->string_count++;
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_STRING,destination,index,0,2);
    compiler->known_types[destination]=DIAMOND_TYPE_STRING;
    return destination;
}

/* `TCPServer.listen(port)`/`listen_nonblocking(port)`, optionally followed
 * by keyword options `reuse_port:`, `host:` and `backlog:` in any order --
 * a compiler special form like Thread.new's own trailing-argument parsing,
 * so this hand-rolls the keywords rather than reusing the generic
 * call-argument path (which only exists for ordinary Diamond-defined
 * functions). The opcode always receives exactly 3 operands. With no
 * options, or only `reuse_port:`, the third is that Bool (a literal `false`
 * when omitted, so every existing single-listener caller -- http_serve,
 * arbitrary user code -- keeps exclusive-port-ownership behavior unless it
 * opts in). With `host:` or `backlog:` it is a Hash of the given options,
 * which the VM reads by name. See docs/io.md and packages/gremlin's own
 * gremlin_worker for why a caller would want reuse_port. */
uint16_t parse_tcp_listen_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool nonblocking=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"listen_nonblocking",compiler->current.span,false);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !(nonblocking||name_equals(compiler,"listen",compiler->current.span,false))) {
        fail(compiler,compiler->current.span,
             "expected 'listen' or 'listen_nonblocking' after 'TCPServer'");
        return 0;
    }
    advance_token(compiler); /* consume 'listen'/'listen_nonblocking' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TCPServer.listen'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    enum { LISTEN_REUSE_PORT,LISTEN_HOST,LISTEN_BACKLOG,LISTEN_OPTION_COUNT };
    static const char *const option_names[LISTEN_OPTION_COUNT]={"reuse_port","host","backlog"};
    uint16_t option_registers[LISTEN_OPTION_COUNT];
    bool option_given[LISTEN_OPTION_COUNT]={};
    while(compiler->current.kind==DIAMOND_TOKEN_COMMA&&!compiler->failed) {
        advance_token(compiler);
        skip_newlines(compiler);
        int matched=-1;
        if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER) {
            for(int option=0;option<LISTEN_OPTION_COUNT;option++)
                if(name_equals(compiler,option_names[option],compiler->current.span,false))
                    matched=option;
        }
        if(matched<0) {
            fail(compiler,compiler->current.span,
                 "expected 'reuse_port', 'host' or 'backlog' after ',' in TCPServer.listen arguments");
            return 0;
        }
        if(option_given[matched]) {
            fail(compiler,compiler->current.span,
                 "duplicate option in TCPServer.listen arguments");
            return 0;
        }
        advance_token(compiler); /* consume the option name */
        if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
            fail(compiler,compiler->current.span,"expected ':' after TCPServer.listen option name");
            return 0;
        }
        advance_token(compiler); /* consume ':' */
        skip_newlines(compiler);
        option_registers[matched]=parse_expression(compiler);
        option_given[matched]=true;
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TCPServer.listen arguments");
        return 0;
    }
    advance_token(compiler);
    uint16_t options_register;
    if(!option_given[LISTEN_HOST]&&!option_given[LISTEN_BACKLOG]) {
        if(option_given[LISTEN_REUSE_PORT]) options_register=option_registers[LISTEN_REUSE_PORT];
        else {
            options_register=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_BOOL,options_register,false,0,2);
        }
    } else {
        uint16_t keys[LISTEN_OPTION_COUNT],values[LISTEN_OPTION_COUNT];
        size_t count=0;
        for(int option=0;option<LISTEN_OPTION_COUNT;option++) {
            if(!option_given[option])continue;
            keys[count]=emit_literal_string(compiler,option_names[option]);
            values[count]=option_registers[option];
            count++;
        }
        const uint16_t base=allocate_register(compiler);
        for(size_t i=1;i<count*2;i++) (void)allocate_register(compiler);
        for(size_t i=0;i<count;i++) {
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2),keys[i],0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2+1),values[i],0,2);
        }
        options_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_HASH,options_register,base,(uint16_t)count,3);
        compiler->known_types[options_register]=DIAMOND_TYPE_HASH;
    }
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,nonblocking?DIAMOND_OP_TCP_LISTEN_NONBLOCK:DIAMOND_OP_TCP_LISTEN);
    emit_register(compiler,dest);
    emit_register(compiler,port_register);
    emit_register(compiler,options_register);
    return dest;
}

uint16_t parse_tls_connect_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool start_handshake=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"start_handshake",compiler->current.span,false);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       (!start_handshake&&!name_equals(compiler,"connect",compiler->current.span,false))) {
        fail(compiler,compiler->current.span,"expected 'connect' or 'start_handshake' after 'TLSSocket'");
        return 0;
    }
    advance_token(compiler); /* consume 'connect' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TLSSocket.connect'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TLSSocket.connect host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t options_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);skip_newlines(compiler);
        options_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        options_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,options_register,0,0,1);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TLSSocket.connect arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,start_handshake?DIAMOND_OP_TLS_START_HANDSHAKE:DIAMOND_OP_TLS_CONNECT);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,port_register);
    emit_register(compiler,options_register);
    return dest;
}

uint16_t parse_tls_listen_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"listen",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'listen' after 'TLSServer'");
        return 0;
    }
    advance_token(compiler); /* consume 'listen' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TLSServer.listen'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TLSServer.listen port");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t cert_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,
             "expected ',' after TLSServer.listen certificate path");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t key_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t options_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);skip_newlines(compiler);
        options_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        options_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,options_register,0,0,1);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TLSServer.listen arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TLS_LISTEN);
    emit_register(compiler,dest);
    emit_register(compiler,port_register);
    emit_register(compiler,cert_register);
    emit_register(compiler,key_register);
    emit_register(compiler,options_register);
    return dest;
}

uint16_t parse_io_poll_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"poll",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'poll' after 'IO'");
        return 0;
    }
    advance_token(compiler); /* consume 'poll' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'IO.poll'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t readable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after IO.poll readables");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t writable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after IO.poll writables");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t timeout_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after IO.poll arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_IO_POLL);
    emit_register(compiler,dest);
    emit_register(compiler,readable_register);
    emit_register(compiler,writable_register);
    emit_register(compiler,timeout_register);
    return dest;
}

uint16_t parse_dns_resolve_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"resolve",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'resolve' after 'DNS'");
        return 0;
    }
    advance_token(compiler); /* consume 'resolve' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'DNS.resolve'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after DNS.resolve host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t cancellations_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after DNS.resolve cancellations");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t deadline_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after DNS.resolve arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_DNS_RESOLVE);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,cancellations_register);
    emit_register(compiler,deadline_register);
    return dest;
}

uint16_t parse_udp_socket_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool is_bind=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"bind",compiler->current.span,false);
    const bool is_open=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"open",compiler->current.span,false);
    if(!is_bind&&!is_open) {
        fail(compiler,compiler->current.span,"expected 'bind' or 'open' after 'UDPSocket'");
        return 0;
    }
    advance_token(compiler); /* consume 'bind'/'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             is_bind?"expected '(' after 'UDPSocket.bind'":"expected '(' after 'UDPSocket.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t port_register=0;
    if(is_bind) {
        port_register=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
             is_bind?"expected ')' after UDPSocket.bind argument":
                     "expected ')' after UDPSocket.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,is_bind?DIAMOND_OP_UDP_BIND:DIAMOND_OP_UDP_OPEN);
    emit_register(compiler,dest);
    if(is_bind)emit_register(compiler,port_register);
    return dest;
}

uint16_t parse_signal_trap_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"trap",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'trap' after 'Signal'");
        return 0;
    }
    advance_token(compiler); /* consume 'trap' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Signal.trap'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Signal.trap name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t handler_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Signal.trap arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SIGNAL_TRAP);
    emit_register(compiler,dest);
    emit_register(compiler,name_register);
    emit_register(compiler,handler_register);
    return dest;
}

uint16_t parse_chr_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CHR);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

uint16_t parse_to_float_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_FLOAT);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

uint16_t parse_to_int_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_INT);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_INT;
    return dest;
}

/* diamond_json_stringify(value): the native behind JSON.stringify (see
 * lib/core/json.di and json_write_value in src/vm.c). Named with the same
 * diamond_ prefix lib/core.di's other internal natives use, so it doesn't
 * take a plain name away from user programs. */
uint16_t parse_json_stringify_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    /* An optional second argument, a Hash of options (indent, sort_keys). */
    bool has_options=false;
    uint16_t options_register=0;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        options_register=parse_expression(compiler);
        has_options=true;
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,has_options?DIAMOND_OP_JSON_STRINGIFY_OPTIONS:DIAMOND_OP_JSON_STRINGIFY);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    if(has_options)emit_register(compiler,options_register);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

uint16_t parse_to_sym_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_SYMBOL);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_SYMBOL;
    return dest;
}

uint16_t parse_math_unary_call(Compiler *compiler, DiamondMathFunction id) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MATH_UNARY);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

uint16_t parse_math_binary_call(Compiler *compiler, DiamondMathFunction id) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t left = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' between arguments");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t right = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MATH_BINARY);
    emit_register(compiler,dest);
    emit_register(compiler,left);
    emit_register(compiler,right);
    emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

/* print/puts/warn: `flags` is a set of DIAMOND_PRINT_* bits. */
uint16_t parse_print_call(Compiler *compiler, uint8_t flags) {
    advance_token(compiler); /* consume '(' */
    const uint16_t source=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PRINT);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    emit_byte(compiler,flags);
    compiler->known_types[dest]=DIAMOND_TYPE_NIL;
    return dest;
}

/* p(value): puts(value.inspect()) that then evaluates to value, so it can wrap an
 * expression while debugging. Takes exactly one argument; a user-defined `p` (function
 * or local) takes precedence over it, as for puts. */
uint16_t parse_p_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t value=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments (p takes one value)");
        return 0;
    }
    advance_token(compiler);
    const uint16_t text=emit_invoke_named(compiler,value,"inspect",nullptr,0);
    const uint16_t printed=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PRINT);
    emit_register(compiler,printed);
    emit_register(compiler,text);
    emit_byte(compiler,DIAMOND_PRINT_NEWLINE);
    compiler->known_types[printed]=DIAMOND_TYPE_NIL;
    return value;
}

uint16_t parse_gets_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_GETS);
    emit_register(compiler,dest);
    return dest;
}

/* exit(code = 0) -- code is optional, defaulting to a compile-time 0
 * constant (same shape as parse_regexp_new_call's own optional `options`
 * default). The allocated `dest` register is never actually written by
 * the VM (DIAMOND_OP_EXIT terminates the process outright on success) --
 * it exists only so this remains an ordinary expression-producing call
 * from the compiler's perspective, same as every other parse_*_call. */
uint16_t parse_exit_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    uint16_t code_register;
    if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
        code_register=allocate_register(compiler);
        const uint16_t zero=add_constant(compiler,DIAMOND_INT(0));
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,code_register,zero,0,2);
    } else {
        code_register=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after exit arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_EXIT);
    emit_register(compiler,code_register);
    compiler->known_types[dest]=DIAMOND_TYPE_NIL;
    return dest;
}

/* Emits one DIAMOND_OP_DEBUGGER pause at the current compile point:
 * pauses execution unconditionally, prints the current call site and
 * every currently-live local (name + value, read-only; no expression
 * evaluation against them, see docs/syntax.md for the scope this was
 * deliberately kept to), then blocks on a single line of stdin (EOF --
 * e.g. stdin redirected from /dev/null, the normal case under a non-
 * interactive test/CI run -- continues immediately rather than hanging)
 * before resuming normally -- or, under DIAMOND_DEBUG_FD, the structured
 * DAP pause path instead (see debugger_helper, src/vm.c). Used only by
 * the explicit debugger()/breakpoint() source calls (parse_debugger_call
 * below) -- an editor's own gutter breakpoint uses the separate,
 * conditional DIAMOND_OP_BREAKPOINT_CHECK instead (emit_breakpoint_check,
 * just below), since whether *that* one actually pauses is a runtime
 * decision (DiamondVm.debug_active_lines can change with no recompile),
 * never a compile-time one the way an explicit debugger() call always is.
 *
 * compiler->locals' (name, register) pairs *at this exact point in
 * compilation* get baked into the opcode's own operand data, the same
 * way a closure's captured-register list is baked in at CLOSURE-emission
 * time -- this is the one piece of information only the compiler has;
 * a register index doesn't carry its source variable's name once
 * compiled, so the VM has no way to reconstruct this after the fact.
 * compiler->local_count is always <= DIAMOND_MAX_LOCALS (enforced by
 * allocate_local), so no separate bounds check is needed before the
 * uint8_t cast below. */
uint16_t emit_debugger_pause(Compiler *compiler) {
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_DEBUGGER);
    emit_register(compiler,dest);
    emit_byte(compiler,(uint8_t)compiler->local_count);
    for(size_t index=0;index<compiler->local_count;index++) {
        const uint16_t name_index=add_name_string(compiler,compiler->locals[index].name);
        emit_register(compiler,name_index);
        emit_register(compiler,compiler->locals[index].reg);
    }
    return dest;
}

/* compile_sequence's own per-statement editor-breakpoint hook (see its
 * one call site, further down this file) -- same locals-baking shape as
 * emit_debugger_pause just above (this is still the only place a
 * register's source variable name is known), but a different opcode:
 * DIAMOND_OP_BREAKPOINT_CHECK checks DiamondVm.debug_active_lines at
 * *runtime* before deciding whether to actually pause, rather than
 * always pausing the instant it's reached. Emitted at every statement
 * when compiler->debug_mode is set, regardless of which lines (if any)
 * are armed yet -- see DiamondVm.debug_active_lines's own comment
 * (src/vm.h) for why compile time no longer needs to know which lines
 * matter in advance. */
uint16_t emit_breakpoint_check(Compiler *compiler,size_t source_line_offset) {
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BREAKPOINT_CHECK);
    emit_register(compiler,dest);
    emit_byte(compiler,(uint8_t)compiler->local_count);
    for(int shift=56;shift>=0;shift-=8)
        emit_byte(compiler,(uint8_t)((uint64_t)source_line_offset>>shift));
    for(size_t index=0;index<compiler->local_count;index++) {
        const uint16_t name_index=add_name_string(compiler,compiler->locals[index].name);
        emit_register(compiler,name_index);
        emit_register(compiler,compiler->locals[index].reg);
    }
    return dest;
}

/* debugger()/breakpoint() -- see emit_debugger_pause's own comment for
 * what the pause itself does; this just parses the call syntax (no
 * arguments) around it. */
uint16_t parse_debugger_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    return emit_debugger_pause(compiler);
}

/* Time.monotonic()/Time.now()/Time.utc_now() -- each zero-argument,
 * distinguished only by which opcode (and, for TIME_NOW, which utc
 * flag byte) they emit. */
static uint16_t parse_time_zero_argument_call(Compiler *compiler,
        DiamondOpCode opcode, uint8_t utc_flag,
        bool has_utc_flag, bool result_is_float) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after Time method name");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Time method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,opcode);
    emit_register(compiler,dest);
    if(has_utc_flag)emit_byte(compiler,utc_flag);
    if(result_is_float)compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

/* Time.at(epoch) -- the numeric Time constructor taking an argument;
 * mirrors parse_sqlite3_open_call's own one-argument-constructor shape. */
static uint16_t parse_time_at_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Time.at'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t epoch_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Time.at arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TIME_AT);
    emit_register(compiler,dest);
    emit_register(compiler,epoch_register);
    return dest;
}

static uint16_t parse_time_parse_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Time.parse'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t string_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Time.parse arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TIME_PARSE);
    emit_register(compiler,dest);emit_register(compiler,string_register);
    return dest;
}

/* Time.utc/local(year, month, day, hour, min, sec) and
 * Time.fixed(offset, year, month, day, hour, min, sec). Arguments are moved
 * into a contiguous register run so TIME_BUILD stays a compact instruction. */
static uint16_t parse_time_build_call(Compiler *compiler,uint8_t mode) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after Time constructor");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const bool fixed=mode==1;
    const size_t expected=fixed?7:6;uint16_t arguments[7];size_t count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        if(count==expected) {
            fail(compiler,compiler->current.span,"too many Time constructor arguments");
            return 0;
        }
        arguments[count++]=parse_expression(compiler);skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN||count!=expected) {
        fail(compiler,compiler->current.span,
            fixed?"Time.fixed expects offset plus six calendar fields":
                  "Time.utc expects six calendar fields");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<count;index++)(void)allocate_register(compiler);
    for(size_t index=0;index<count;index++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+index),arguments[index],0,2);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TIME_BUILD);
    emit_register(compiler,dest);emit_register(compiler,base);emit_byte(compiler,mode);
    return dest;
}

uint16_t parse_time_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected a Time constructor after 'Time'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"monotonic",method,false)) {
        advance_token(compiler);
        return parse_time_zero_argument_call(compiler,
            DIAMOND_OP_TIME_MONOTONIC,0,false,true);
    }
    if(name_equals(compiler,"now",method,false)) {
        advance_token(compiler);
        return parse_time_zero_argument_call(compiler,
            DIAMOND_OP_TIME_NOW,0,true,false);
    }
    if(name_equals(compiler,"utc_now",method,false)) {
        advance_token(compiler);
        return parse_time_zero_argument_call(compiler,
            DIAMOND_OP_TIME_NOW,1,true,false);
    }
    if(name_equals(compiler,"at",method,false)) {
        advance_token(compiler);
        return parse_time_at_call(compiler);
    }
    if(name_equals(compiler,"parse",method,false)) {
        advance_token(compiler);
        return parse_time_parse_call(compiler);
    }
    if(name_equals(compiler,"utc",method,false)) {
        advance_token(compiler);return parse_time_build_call(compiler,0);
    }
    if(name_equals(compiler,"fixed",method,false)) {
        advance_token(compiler);return parse_time_build_call(compiler,1);
    }
    if(name_equals(compiler,"local",method,false)) {
        advance_token(compiler);return parse_time_build_call(compiler,2);
    }
    fail(compiler,method,
        "expected a Time constructor after 'Time'");
    return 0;
}

/* Process.run(argv) -- the one Process method; mirrors parse_time_at_call's
 * one-argument-constructor shape (dest register, then a single register
 * operand -- here the argv Array rather than an epoch). argv-array-only
 * by design (see docs/syntax.md): there is no shell-string form to parse
 * at all, so no injection surface exists to guard against here. */
static uint16_t parse_process_run_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Process.run'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t argv_register=parse_expression(compiler);
    skip_newlines(compiler);
    /* An optional second argument, a Hash of options (env, cwd, stdin). */
    bool has_options=false;
    uint16_t options_register=0;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        options_register=parse_expression(compiler);
        has_options=true;
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Process.run arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,has_options?DIAMOND_OP_PROCESS_RUN_OPTIONS:DIAMOND_OP_PROCESS_RUN);
    emit_register(compiler,dest);
    emit_register(compiler,argv_register);
    if(has_options)emit_register(compiler,options_register);
    return dest;
}

/* Process.spawn(argv) -- identical fixed one-argument shape to
 * Process.run above (same argv Array, different opcode/result type). */
static uint16_t parse_process_spawn_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Process.spawn'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t argv_register=parse_expression(compiler);
    skip_newlines(compiler);
    /* An optional second argument, a Hash of options (env, cwd, stdin). */
    bool has_options=false;
    uint16_t options_register=0;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        options_register=parse_expression(compiler);
        has_options=true;
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Process.spawn arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,has_options?DIAMOND_OP_PROCESS_SPAWN_OPTIONS:DIAMOND_OP_PROCESS_SPAWN);
    emit_register(compiler,dest);
    emit_register(compiler,argv_register);
    if(has_options)emit_register(compiler,options_register);
    return dest;
}

uint16_t parse_process_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'run' or 'spawn' after 'Process'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"run",method,false)) {
        advance_token(compiler);
        return parse_process_run_call(compiler);
    }
    if(name_equals(compiler,"spawn",method,false)) {
        advance_token(compiler);
        return parse_process_spawn_call(compiler);
    }
    fail(compiler,method,"expected 'run' or 'spawn' after 'Process'");
    return 0;
}

/* BCrypt.hash(password, cost) -- both arguments always required (no
 * optional-argument support at this hand-rolled class-call parse layer);
 * `cost`'s default of 12 lives one layer up, in
 * ActiveRecord::Model#secure_password=, the one real caller that wants a
 * default. Mirrors parse_tls_connect_call's own two-fixed-argument shape. */
static uint16_t parse_bcrypt_hash_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'BCrypt.hash'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t password_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after BCrypt.hash password");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t cost_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after BCrypt.hash arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BCRYPT_HASH);
    emit_register(compiler,dest);
    emit_register(compiler,password_register);
    emit_register(compiler,cost_register);
    return dest;
}

static uint16_t parse_bcrypt_verify_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'BCrypt.verify'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t password_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after BCrypt.verify password");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t digest_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after BCrypt.verify arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BCRYPT_VERIFY);
    emit_register(compiler,dest);
    emit_register(compiler,password_register);
    emit_register(compiler,digest_register);
    return dest;
}

uint16_t parse_bcrypt_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'hash' or 'verify' after 'BCrypt'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"hash",method,false)) {
        advance_token(compiler);
        return parse_bcrypt_hash_call(compiler);
    }
    if(name_equals(compiler,"verify",method,false)) {
        advance_token(compiler);
        return parse_bcrypt_verify_call(compiler);
    }
    fail(compiler,method,"expected 'hash' or 'verify' after 'BCrypt'");
    return 0;
}

/* Cipher.encrypt(key, plaintext) / Cipher.decrypt(key, blob) -- both
 * fixed two-argument calls, mirrors parse_bcrypt_hash_call/parse_bcrypt_
 * verify_call's own shape exactly. */
static uint16_t parse_cipher_encrypt_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Cipher.encrypt'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t key_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Cipher.encrypt key");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t plaintext_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Cipher.encrypt arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CIPHER_ENCRYPT);
    emit_register(compiler,dest);
    emit_register(compiler,key_register);
    emit_register(compiler,plaintext_register);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

static uint16_t parse_cipher_decrypt_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Cipher.decrypt'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t key_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Cipher.decrypt key");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t blob_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Cipher.decrypt arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CIPHER_DECRYPT);
    emit_register(compiler,dest);
    emit_register(compiler,key_register);
    emit_register(compiler,blob_register);
    return dest;
}

static uint16_t parse_gzip_compress_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Gzip.compress'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t data_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Gzip.compress arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_GZIP_COMPRESS);
    emit_register(compiler,dest);
    emit_register(compiler,data_register);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

static uint16_t parse_gzip_decompress_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Gzip.decompress'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t data_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Gzip.decompress data");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t max_size_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Gzip.decompress arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_GZIP_DECOMPRESS);
    emit_register(compiler,dest);
    emit_register(compiler,data_register);
    emit_register(compiler,max_size_register);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

uint16_t parse_gzip_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'compress' or 'decompress' after 'Gzip'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"compress",method,false)) {
        advance_token(compiler);
        return parse_gzip_compress_call(compiler);
    }
    if(name_equals(compiler,"decompress",method,false)) {
        advance_token(compiler);
        return parse_gzip_decompress_call(compiler);
    }
    fail(compiler,method,"expected 'compress' or 'decompress' after 'Gzip'");
    return 0;
}

/* Tensor.zeros(rows, cols) -- both arguments are arbitrary Int
 * expressions, runtime-validated in the DIAMOND_OP_TENSOR_ZEROS
 * handler (compiler has no way to know their runtime type here,
 * same as SQLite3.open's own path/mode arguments). */
static uint16_t parse_tensor_zeros_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Tensor.zeros'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t rows_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Tensor.zeros rows argument");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t cols_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Tensor.zeros arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TENSOR_ZEROS);
    emit_register(compiler,dest);
    emit_register(compiler,rows_register);
    emit_register(compiler,cols_register);
    return dest;
}

/* Tensor.from_array(nested_array) -- single Array-of-Arrays argument,
 * runtime-validated (shape/element-type) in the
 * DIAMOND_OP_TENSOR_FROM_ARRAY handler. */
static uint16_t parse_tensor_from_array_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Tensor.from_array'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t array_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Tensor.from_array arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TENSOR_FROM_ARRAY);
    emit_register(compiler,dest);
    emit_register(compiler,array_register);
    return dest;
}

/* Tensor.random(rows, cols, seed) -- three arbitrary Int expressions,
 * runtime-validated in the DIAMOND_OP_TENSOR_RANDOM handler. */
static uint16_t parse_tensor_random_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Tensor.random'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t rows_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Tensor.random rows argument");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t cols_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Tensor.random cols argument");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t seed_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Tensor.random arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TENSOR_RANDOM);
    emit_register(compiler,dest);
    emit_register(compiler,rows_register);
    emit_register(compiler,cols_register);
    emit_register(compiler,seed_register);
    return dest;
}

uint16_t parse_tensor_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'zeros', 'from_array', or 'random' after 'Tensor'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"zeros",method,false)) {
        advance_token(compiler);
        return parse_tensor_zeros_call(compiler);
    }
    if(name_equals(compiler,"from_array",method,false)) {
        advance_token(compiler);
        return parse_tensor_from_array_call(compiler);
    }
    if(name_equals(compiler,"random",method,false)) {
        advance_token(compiler);
        return parse_tensor_random_call(compiler);
    }
    fail(compiler,method,"expected 'zeros', 'from_array', or 'random' after 'Tensor'");
    return 0;
}

static uint16_t parse_base64_encode_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Base64.encode'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t data_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Base64.encode arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BASE64_ENCODE);
    emit_register(compiler,dest);
    emit_register(compiler,data_register);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

static uint16_t parse_base64_decode_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Base64.decode'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t data_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Base64.decode arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BASE64_DECODE);
    emit_register(compiler,dest);
    emit_register(compiler,data_register);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

uint16_t parse_base64_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'encode' or 'decode' after 'Base64'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"encode",method,false)) {
        advance_token(compiler);
        return parse_base64_encode_call(compiler);
    }
    if(name_equals(compiler,"decode",method,false)) {
        advance_token(compiler);
        return parse_base64_decode_call(compiler);
    }
    fail(compiler,method,"expected 'encode' or 'decode' after 'Base64'");
    return 0;
}

uint16_t parse_cipher_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'encrypt' or 'decrypt' after 'Cipher'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"encrypt",method,false)) {
        advance_token(compiler);
        return parse_cipher_encrypt_call(compiler);
    }
    if(name_equals(compiler,"decrypt",method,false)) {
        advance_token(compiler);
        return parse_cipher_decrypt_call(compiler);
    }
    fail(compiler,method,"expected 'encrypt' or 'decrypt' after 'Cipher'");
    return 0;
}

/* SecureRandom.bytes(n) / SecureRandom.hex(n) -- single fixed argument,
 * mirrors parse_process_run_call's own one-argument shape. */
static uint16_t parse_secure_random_bytes_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'SecureRandom.bytes'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t count_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after SecureRandom.bytes arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SECURE_RANDOM_BYTES);
    emit_register(compiler,dest);
    emit_register(compiler,count_register);
    return dest;
}

static uint16_t parse_secure_random_hex_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'SecureRandom.hex'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t count_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after SecureRandom.hex arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SECURE_RANDOM_HEX);
    emit_register(compiler,dest);
    emit_register(compiler,count_register);
    return dest;
}

uint16_t parse_secure_random_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'bytes' or 'hex' after 'SecureRandom'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"bytes",method,false)) {
        advance_token(compiler);
        return parse_secure_random_bytes_call(compiler);
    }
    if(name_equals(compiler,"hex",method,false)) {
        advance_token(compiler);
        return parse_secure_random_hex_call(compiler);
    }
    fail(compiler,method,"expected 'bytes' or 'hex' after 'SecureRandom'");
    return 0;
}

static uint16_t parse_digest_call(Compiler *compiler,bool keyed,bool use_sha1) {
    const char *algorithm_name=use_sha1?"sha1":"sha256";
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after sha256");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t first=parse_expression(compiler);
    uint16_t second=0;
    if(keyed) {
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,"expected ',' after HMAC key");
            return 0;
        }
        advance_token(compiler);skip_newlines(compiler);
        second=parse_expression(compiler);
    }
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        (void)algorithm_name;
        fail(compiler,compiler->current.span,"expected ')' after digest arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    DiamondOpCode opcode;
    if(use_sha1)opcode=keyed?DIAMOND_OP_HMAC_SHA1:DIAMOND_OP_DIGEST_SHA1;
    else opcode=keyed?DIAMOND_OP_HMAC_SHA256:DIAMOND_OP_DIGEST_SHA256;
    emit_opcode(compiler,opcode);
    emit_register(compiler,dest);emit_register(compiler,first);
    if(keyed)emit_register(compiler,second);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

/* HMAC.verify(data, key, signature) -- three fixed arguments, mirrors
 * parse_bcrypt_verify_call's own two-argument comma chain with one more
 * link. Only reachable when `keyed` (i.e. only under 'HMAC.', not
 * 'Digest.') -- see parse_crypto_call below. */
static uint16_t parse_hmac_verify_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'HMAC.verify'");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t data_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after HMAC.verify data");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t key_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after HMAC.verify key");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t signature_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after HMAC.verify arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_HMAC_VERIFY);
    emit_register(compiler,dest);
    emit_register(compiler,data_register);
    emit_register(compiler,key_register);
    emit_register(compiler,signature_register);
    return dest;
}

uint16_t parse_crypto_call(Compiler *compiler,bool keyed) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'sha256' after crypto namespace");
        return 0;
    }
    if(keyed&&name_equals(compiler,"verify",compiler->current.span,false)) {
        advance_token(compiler);
        return parse_hmac_verify_call(compiler);
    }
    const bool is_sha256=name_equals(compiler,"sha256",compiler->current.span,false);
    const bool is_sha1=!is_sha256&&name_equals(compiler,"sha1",compiler->current.span,false);
    if(!is_sha256&&!is_sha1) {
        fail(compiler,compiler->current.span,keyed?
            "expected 'sha256', 'sha1', or 'verify' after 'HMAC'":
            "expected 'sha256' or 'sha1' after crypto namespace");
        return 0;
    }
    advance_token(compiler);
    return parse_digest_call(compiler,keyed,is_sha1);
}

const DiamondFunction *constructor_signature(const Compiler *compiler,
        int class_index) {
    while(class_index>=0&&(size_t)class_index<compiler->program->class_count) {
        const DiamondClass *class=compiler->program->classes[(size_t)class_index];
        for(size_t index=0;index<class->method_count;index++) {
            const DiamondMethod *method=&class->methods[index];
            if(strcmp(method->name,"initialize")!=0)continue;
            if(method->function_index>=compiler->program->function_count)
                return nullptr;
            return compiler->program->functions[method->function_index];
        }
        class_index=class->superclass==DIAMOND_NO_CLASS?-1:(int)class->superclass;
    }
    return nullptr;
}
