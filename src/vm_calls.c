#define _GNU_SOURCE

#include "vm_internal.h"
#include "jit.h"
#include "bignum.h"

#include <errno.h>
#include <inttypes.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DIAMOND_OP_CALL_CLOSURE's own version of DIAMOND_OP_NEW/INVOKE's
 * self-goes-in-arguments[0] convention. A function compiled with
 * owner_class set reserves register 0 for self and folds an implicit
 * +1 into its own arity/required_arity (compile_definition's
 * nested_in_singleton_method handling, src/compiler.c) -- deliberate,
 * for redefine_method's patch-factory idiom (a closure meant to become
 * a real instance method later needs the same register/arity shape one
 * already has). But that flag also fires for *any* closure nested
 * directly inside a `def self.x` method, not just ones destined for
 * redefine_method, and an ordinary closure call (`doubler(v)`, no
 * receiver at all) has no self value to supply the way NEW/INVOKE
 * always do -- confirmed the hard way: calling such a closure directly
 * raised a spurious "wrong number of arguments" for every arity,
 * because nothing was compensating for that +1 on this call path.
 * Synthesizing an unread nil placeholder at argument position 0 exactly
 * matches the calling convention the function body was compiled to
 * expect (its own register 0 is never actually read in this case --
 * self access for a plain closure like this goes through a captured
 * cell instead, not the register-0 convention a real method gets from
 * INVOKE -- so what's in register 0 doesn't matter, only that
 * something occupies it so the real parameters land where the compiled
 * body expects them).
 *
 * Kept out of run_chunk's own switch, not inlined the way this call
 * used to be, for the same reason get_cvar_helper/set_cvar_helper are
 * (see their own comment): the 17-DiamondValue buffer this needs would
 * cost real margin against DIAMOND_MAX_CALL_DEPTH's stack-depth guard
 * if it lived directly in a run_chunk case, and CALL_CLOSURE is itself
 * on run_chunk's own recursive call path. */
DiamondVmStatus block_call_prepare(const DiamondChunk *chunk,
        const DiamondFunction *fn,const DiamondClosure *called,uint8_t argc,
        DiamondBlockCall *call) {
    if(diamond_block_ignores_arguments(called,fn))argc=0;
    const bool needs_self_slot=fn->owner_class!=DIAMOND_OWNER_NONE;
    /* A `closure` (compiler.c's UINT8_MAX-2 owner_class sentinel) also
     * needs the phantom-slot-0 padding below -- it reserves register 0
     * for its own materialized captured self, so real arguments still
     * need to land starting at register 1, exactly like a genuine
     * method's implicit receiver. But unlike a genuine method, nothing
     * external ever supplies that slot's value, so fn->arity/
     * required_arity were never inflated to account for it (see
     * compiler.c's owner_class comment) -- the arity-bounds check below
     * must compare against the real, un-inflated argc, not the padded
     * argument_count used only to place real arguments correctly. */
    const bool self_via_capture=fn->owner_class==DIAMOND_OWNER_CAPTURED_SELF;
    const size_t argument_count=needs_self_slot?(size_t)argc+1:argc;
    const size_t arity_check_count=self_via_capture?(size_t)argc:argument_count;
    if(arity_check_count<fn->required_arity||
       (arity_check_count>fn->arity && !fn->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    call->child=function_call_chunk(chunk,fn,&call->child_storage);
    call->called=called;
    call->function=fn;
    call->argc=argc;
    call->self_slot=needs_self_slot;
    return DIAMOND_VM_OK;
}

/* A block with a self slot: the arguments shift up one behind a nil
 * placeholder. Kept out of block_call_run so the buffer does not give the
 * common call a 4 KB frame and saved registers. */
__attribute__((noinline))
static DiamondVmStatus block_call_run_self_slot(DiamondVm *vm,const DiamondBlockCall *call,
        const DiamondValue *arguments,size_t depth,DiamondValue *result) {
    /* No argc>DIAMOND_MAX_ARGUMENTS guard needed here: argc is a plain
     * uint8_t (the wire format's own argument-count width), so it can
     * never exceed DIAMOND_MAX_ARGUMENTS (255) in the first place --
     * call_arguments[DIAMOND_MAX_ARGUMENTS+1] below always has room. */
    DiamondValue call_arguments[DIAMOND_MAX_ARGUMENTS+1];
    call_arguments[0]=DIAMOND_NIL;
    for(size_t index=0;index<call->argc;index++)
        call_arguments[index+1]=arguments[index];
    /* A closure that materializes `self` from its captures can run as
     * native code. A real class or module owner stays interpreted: the JIT
     * treats register 0 of such a function as the receiver, which the nil
     * placeholder here is not. */
    if(call->function->owner_class==DIAMOND_OWNER_CAPTURED_SELF)
        return jit_call_or_interpret(vm,call->function,call->child,call_arguments,
            (size_t)call->argc+1,depth,call->called,result);
    return run_chunk(call->child,vm,call_arguments,(size_t)call->argc+1,
        depth+1,call->called,result);
}

DiamondVmStatus call_closure_helper(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondFunction *fn,const DiamondClosure *called,
        const DiamondValue *registers,uint16_t base,uint8_t argc,size_t depth,
        DiamondValue *result) {
    DiamondBlockCall call;
    const DiamondVmStatus prepared=block_call_prepare(chunk,fn,called,argc,&call);
    if(prepared!=DIAMOND_VM_OK)return prepared;
    return block_call_run(vm,&call,&registers[base],depth,result);
}

/* DIAMOND_OP_CALL_CLOSURE: registers[callable] called with argc arguments
 * from registers[base]. Shared by run_chunk and compiled code
 * (diamond_jit_call_closure), so both raise the same errors. */
DiamondVmStatus call_closure_value(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,uint16_t callable,uint16_t base,uint8_t argc,
        size_t depth,DiamondValue *result) {
    if(registers[callable].kind!=DIAMOND_VALUE_OBJECT||
       registers[callable].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,registers[callable]);
        snprintf(vm->error,sizeof vm->error,"undefined method 'call' for %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondClosure *called=(const DiamondClosure *)registers[callable].as.object;
    /* A ClassName.compile_method result is scoped to being passed to
     * define_method (see DIAMOND_OP_DEFINE_METHOD) -- its function_index is
     * relative to its own foreign_chunk, not the ambient one
     * call_closure_helper assumes, so calling it directly is rejected rather
     * than silently running the wrong bytecode. */
    if(called->foreign_chunk!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "a compile_method callable can only be passed to define_method");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(called->function_index>=chunk->function_count)return DIAMOND_VM_INVALID_BYTECODE;
    return call_closure_helper(vm,chunk,chunk->functions[called->function_index],called,
        registers,base,argc,depth,result);
}

/* Shared by DIAMOND_OP_INVOKE's Int-primitive (times/upto/downto) and
 * Array/Hash-collection-extension forwarding arms: both resolve a native
 * receiver method to an ordinary prelude/user top-level function and call
 * it with the receiver prepended as its own first argument -- identical
 * shape, just a different `target` lookup beforehand. Factored out (like
 * call_closure_helper just above) so the DIAMOND_MAX_ARGUMENTS+1-sized
 * forwarding buffer doesn't live directly in one of run_chunk's own case
 * blocks. */
DiamondVmStatus forward_to_top_level_helper(DiamondVm *vm,
        const DiamondChunk *chunk,const DiamondFunction *target,
        const DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,
        size_t depth,DiamondValue *result) {
    const size_t total_argc=(size_t)argc+1;
    if(total_argc<target->required_arity||
       (total_argc>target->arity&&!target->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    DiamondValue forward_args[DIAMOND_MAX_ARGUMENTS+1];
    forward_args[0]=registers[recv];
    for(size_t i=0;i<argc;i++)
        forward_args[i+1]=registers[(size_t)base+i];
    const DiamondChunk child={.name=target->name,.code=target->code,
      .lines=target->lines,.columns=target->columns,
      .code_count=target->code_count,
      .constants=target->constants,.constant_count=target->constant_count,
      .strings=target->strings,.string_count=target->string_count,
      .type_sets=target->type_sets,.type_set_count=target->type_set_count,
      .functions=chunk->functions,.function_count=chunk->function_count,
      .classes=chunk->classes,.class_count=chunk->class_count,
      .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
      .parameter_type_sets=target->parameter_type_sets,
      .type_variable_count=target->type_variable_count,
      .class_type_variable=target->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(target->owner_class+1u),
      .parameter_offset=target->owner_class==DIAMOND_OWNER_NONE?0:1,
      .entry_check=&target->entry_check,.register_count=target->register_count};
    return run_chunk(&child,vm,forward_args,total_argc,depth+1,nullptr,result);
}

/* Runs `function`'s compiled code with `arguments` copied into a fresh
 * register file. Shared by jit_call_core and diamond_jit_call_self, so the
 * two enter native code identically. Returns DIAMOND_VM_OK (*result
 * written), DIAMOND_JIT_TAIL (a staged cross-function tail call, for
 * jit_tail_loop), DIAMOND_JIT_RETRY (counted as a bailout: the caller
 * interprets the call instead, which is always correct), or a real status
 * to propagate as-is. The no_sanitize attribute is explained at
 * jit_call_core below. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static inline uint8_t jit_run_native_in(DiamondVm *vm, const DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret, const DiamondValue *arguments,
        size_t argument_count, size_t depth, const DiamondClosure *closure,
        DiamondValue *result, DiamondValue *jit_registers, size_t jit_register_count) {
    memset(jit_registers, 0, jit_register_count * sizeof(DiamondValue));
    const size_t copy_count = argument_count < jit_register_count
        ? argument_count : jit_register_count;
    for (size_t index = 0; index < copy_count; index++) jit_registers[index] = arguments[index];
    DiamondValue jit_result = DIAMOND_NIL;
    /* ISO C has no portable object-pointer-to-function-pointer
     * conversion, but POSIX explicitly requires it to work on any
     * platform with dlsym (the same cast dlsym's own callers
     * need) -- unavoidable for calling into mmap'd JIT code, not
     * a real portability gap on any platform this VM targets. */
    #if defined(__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wpedantic"
    #endif
    const DiamondJitFn jit_fn = (DiamondJitFn)function->jit_code;
    #if defined(__GNUC__)
    #pragma GCC diagnostic pop
    #endif
    /* Phase 2d: DiamondJitFn's own return convention is now 3-way,
     * not a plain bool -- see jit.h's own comment. DIAMOND_VM_OK
     * (0) means success (*result written); DIAMOND_JIT_RETRY means
     * the existing "discard this attempt, fall back to run_chunk"
     * behavior every phase before this one relied on exclusively;
     * anything else is a real DiamondVmStatus (most notably
     * DIAMOND_VM_EXCEPTION from a SUPER call that itself raised)
     * that must be returned exactly as-is -- NOT retried, since a
     * real, already-executed side effect (the SUPER call) may be
     * why this status exists at all, and re-running the whole
     * function from scratch would invoke it a second time. */
    const uint8_t jit_status = jit_fn(vm, jit_registers, &jit_result,
            argument_count, chunk_to_interpret, depth + 1, closure);
    if (jit_status == DIAMOND_VM_OK) {
        value_copy_from_callee(result, &jit_result);
        return DIAMOND_VM_OK;
    }
    if (jit_status == DIAMOND_JIT_TAIL) return DIAMOND_JIT_TAIL;
    if (jit_status != DIAMOND_JIT_RETRY) {
        vm->jit_hard_propagations++;
        return (DiamondVmStatus)jit_status;
    }
    vm->jit_bailouts++;
    return DIAMOND_JIT_RETRY;
}

/* A function with more registers than jit_run_native's stack buffer holds
 * gets its register file from the heap, as run_chunk does; the compiled
 * code roots the registers through its own frame while it runs and frees
 * them on its way out (uses_heap_registers in src/jit.c, which must agree
 * with the test in jit_run_native). The JIT never compiles a function whose
 * register_count is 0 (a hand-built chunk's "unset"), so the common case
 * costs one compare, no more than the zero test it replaces. */
__attribute__((noinline, cold))
static DiamondValue *jit_heap_registers(const DiamondFunction *function) {
    return malloc((size_t)function->register_count * sizeof(DiamondValue));
}

#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static inline uint8_t jit_run_native(DiamondVm *vm, const DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret, const DiamondValue *arguments,
        size_t argument_count, size_t depth, const DiamondClosure *closure,
        DiamondValue *result) {
    DiamondValue stack_registers[DIAMOND_JIT_MAX_REGISTERS];
    DiamondValue *jit_registers = stack_registers;
    const size_t jit_register_count = function->register_count;
    if (__builtin_expect(jit_register_count > DIAMOND_JIT_MAX_REGISTERS, 0)) {
        jit_registers = jit_heap_registers(function);
        if (jit_registers == nullptr) return DIAMOND_JIT_RETRY;
    }
    return jit_run_native_in(vm, function, chunk_to_interpret, arguments,
        argument_count, depth, closure, result, jit_registers, jit_register_count);
}

/* A generic function's compiled prologue (src/jit.c): run_chunk's own
 * bind_generic_entry over the arguments in `registers`, into `state` in the
 * function's frame reservation. Returns the chunk the body runs in, or
 * nullptr to retry the call in the interpreter, which binds the same way
 * and reports any failure itself. The registers hold only as many arguments
 * as the function has registers, so a call passing more retries too. */
const DiamondChunk *diamond_jit_bind_generic(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *registers, size_t argument_count,
        const DiamondClosure *closure, DiamondJitGenericState *state) {
    const size_t register_count = chunk->register_count == 0
        ? DIAMOND_JIT_MAX_REGISTERS : chunk->register_count;
    if (argument_count > register_count) return nullptr;
    DiamondVmStatus status = DIAMOND_VM_OK;
    return bind_generic_entry(chunk, vm, registers, argument_count, closure,
        &state->execution, state->bindings, &status);
}

/* Phase 2b baseline JIT (docs/internal/jit-design.md) -- shared by every
 * call site that invokes a DiamondFunction with an already-assembled
 * arguments buffer: DIAMOND_OP_CALL below, and invoke_resolved_method_
 * helper just below this (which DIAMOND_OP_NEW/SUPER/INVOKE_TYPED's
 * ordinary instance dispatch all route through) -- neither goes through
 * the other, so both need their own tier-up/dispatch check rather than
 * sharing one call site. Tries the compiled path first if the function is
 * warm enough to have been compiled, falling back to run_chunk for
 * anything not (yet) compiled or that bails at runtime. `function` came
 * from a chunk's own functions[] table, declared const the same way a
 * quickened opcode's own `chunk->code` is -- this is the same "logically
 * mutable cache metadata behind a const pointer" pattern already used
 * throughout this file for self-modifying bytecode, not a new one. */
/* Compiles `function` (to run in `chunk_to_interpret`), or marks it
 * ineligible when the JIT turns it down. */
__attribute__((noinline, cold))
static void jit_compile_function(DiamondVm *vm, DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret) {
    size_t jit_code_size = 0;
    size_t self_index = SIZE_MAX;
    for (size_t index = 0; index < chunk_to_interpret->function_count; index++)
        if (chunk_to_interpret->functions[index] == function) {
            self_index = index;
            break;
        }
    void *compiled = diamond_jit_try_compile(function,
        chunk_to_interpret->functions, chunk_to_interpret->function_count,
        self_index, &jit_code_size);
    if (compiled != nullptr) {
        function->jit_code = compiled;
        function->jit_code_size = jit_code_size;
        vm->jit_compiled_functions++;
    } else {
        function->jit_ineligible = true;
    }
}

/* Clang's UBSan `function` check assumes every indirect-call target was
 * emitted by Clang and probes its private 8-byte type signature immediately
 * before the target address. A Diamond JIT target is the first byte of an
 * mmap'd executable region and deliberately has no compiler-owned prefix, so
 * that probe reads the unmapped guard page at jit_code-8 and segfaults before
 * the generated function can run. The JIT ABI is declared by DiamondJitFn and
 * emitted in one place (jit.c); exempt only this dispatcher's indirect call
 * from that inapplicable check while retaining ASan and every other UBSan
 * check throughout the function and its C trampolines. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static uint8_t jit_call_core(DiamondVm *vm, const DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret, const DiamondValue *arguments,
        size_t argument_count, size_t depth, const DiamondClosure *closure,
        DiamondValue *result) {
    /* Phase 2d: this is the ONE place both the compiled and interpreted
     * dispatch paths funnel through, so it's also the one place that can
     * enforce DIAMOND_MAX_CALL_DEPTH uniformly. Before Phase 2d, a
     * compiled function could never itself make a further call, so a
     * missing check here was harmless (run_chunk's own entry check caught
     * every call that ever reached it). Now that a compiled function can
     * invoke SUPER, which can reach an arbitrarily deep chain of further
     * calls, a sequence of hops that happen to all be compiled would
     * otherwise never touch run_chunk's own check at all -- a real latent
     * gap, not a theoretical one, that this closes for every call site
     * uniformly (not just the ones this phase adds). */
    if (depth + 1 >= diamond_call_depth_limit(vm)) {
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    /* Native code is never interrupted and executes none of the per-
     * instruction checks, so a VM with an instruction, wall-clock or memory
     * budget (or one already past a tripped budget, in its grace allowance)
     * stays on the interpreter, where the budget is enforced. A memory budget
     * counts because a loop that only grows one array allocates nothing new:
     * the interpreter notices through memory_recheck between instructions,
     * and native code has no such point. */
    if (vm->jit && !function->jit_ineligible && vm->max_instructions == 0 &&
        vm->max_wall_nanoseconds == 0 && vm->max_memory_bytes == 0 &&
        !vm->resource_grace_active) {
        DiamondFunction *mutable_function = (DiamondFunction *)function;
        if (mutable_function->jit_code == nullptr) {
            mutable_function->jit_call_count++;
            /* A function with a loop gets compiled by its first call: a
             * loop in a function called once (a script's main routine, a
             * benchmark's run()) would otherwise never reach the threshold
             * and run interpreted to completion, since there is no
             * on-stack replacement. Native loops poll the interrupt flag
             * of a supervised attempt (emit_interrupt_poll in src/jit.c). */
            const bool loop_on_first_call =
                mutable_function->jit_call_count == 1 &&
                diamond_jit_function_has_loop(function);
            if (loop_on_first_call ||
                mutable_function->jit_call_count >= vm->jit_threshold)
                jit_compile_function(vm, mutable_function, chunk_to_interpret);
        }
        if (mutable_function->jit_code != nullptr) {
            const uint8_t jit_status = jit_run_native(vm, function, chunk_to_interpret,
                arguments, argument_count, depth, closure, result);
            /* A retry falls through to the ordinary interpreted call below --
             * always fully correct regardless of why the compiled attempt
             * bailed. */
            if (jit_status != DIAMOND_JIT_RETRY) return jit_status;
        }
    }
    return (uint8_t)run_chunk(chunk_to_interpret, vm, arguments, argument_count, depth + 1, closure, result);
}

/* Runs the cross-function tail calls a compiled function staged, one callee
 * after another at the same depth: each hop returns to this loop instead of
 * nesting, so a cycle of tail calls uses constant native stack. Kept out of
 * jit_call_or_interpret so that function's frame stays small. */
__attribute__((noinline))
static DiamondVmStatus jit_tail_loop(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, DiamondValue *result) {
    uint8_t status = DIAMOND_JIT_TAIL;
    DiamondValue arguments[DIAMOND_MAX_DECLARED_PARAMETERS];
    DiamondChunk next;
    while (status == DIAMOND_JIT_TAIL) {
        const DiamondFunction *function = vm->jit_tail_function;
        const size_t count = vm->jit_tail_count;
        for (size_t index = 0; index < count; index++)
            arguments[index] = vm->jit_tail_args[index];
        next = plain_call_chunk(function, chunk);
        status = jit_call_core(vm, function, &next, arguments, count, depth,
            nullptr, result);
        chunk = &next;
    }
    return (DiamondVmStatus)status;
}

/* A call with the JIT on: native code if the callee is compiled, the interpreter otherwise, and the
 * cross-function tail calls native code staged. jit_call_or_interpret (vm_internal.h) handles the
 * JIT-off case inline. */
DiamondVmStatus jit_call_with_jit(DiamondVm *vm, const DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret, const DiamondValue *arguments,
        size_t argument_count, size_t depth, const DiamondClosure *closure,
        DiamondValue *result) {
    const uint8_t status = jit_call_core(vm, function, chunk_to_interpret,
        arguments, argument_count, depth, closure, result);
    if (status == DIAMOND_JIT_TAIL)
        return jit_tail_loop(vm, chunk_to_interpret, depth, result);
    return (DiamondVmStatus)status;
}

/* A compiled block, entered without jit_call_core's tier-up bookkeeping:
 * block_call_run checked everything jit_call_core would. Out of line so
 * the native register file stays out of block_call_run's frame, which
 * the interpreter's block calls share. */
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
__attribute__((noinline))
static DiamondVmStatus block_call_native(DiamondVm *vm,const DiamondBlockCall *call,
        const DiamondValue *arguments,size_t depth,DiamondValue *result) {
    const uint8_t status=jit_run_native(vm,call->function,call->child,arguments,
        call->argc,depth,call->called,result);
    if(status==DIAMOND_JIT_TAIL) return jit_tail_loop(vm,call->child,depth,result);
    if(status!=DIAMOND_JIT_RETRY) return (DiamondVmStatus)status;
    return run_chunk(call->child,vm,arguments,call->argc,depth+1,call->called,result);
}

/* A block's every call lands here (the C loops behind each/map/times and
 * CALL_CLOSURE), so a compiled block goes straight to its native code when
 * nothing jit_call_core checks could stop it: the depth is under the limit
 * (jit_depth_limit starts at 0, which sends the first call the long way;
 * that call fills it in, on the thread running the VM) and the VM has no
 * instruction, wall-clock or memory budget. */
DiamondVmStatus block_call_run(DiamondVm *vm,const DiamondBlockCall *call,
        const DiamondValue *arguments,size_t depth,DiamondValue *result) {
    /* Native each/map/times loops may call a compiled block forever without
     * returning to run_chunk, whose entry normally observes cancellation. */
    if(vm_interrupt_requested(vm))return DIAMOND_VM_INTERRUPTED;
    if(call->self_slot) return block_call_run_self_slot(vm,call,arguments,depth,result);
    if(vm->jit && call->function->jit_code!=nullptr && depth+1<vm->jit_depth_limit &&
       vm->max_instructions==0 && vm->max_wall_nanoseconds==0 && vm->max_memory_bytes==0 &&
       !vm->resource_grace_active)
        return block_call_native(vm,call,arguments,depth,result);
    if(vm->jit_depth_limit==0) vm->jit_depth_limit=diamond_call_depth_limit(vm);
    return jit_call_or_interpret(vm,call->function,call->child,arguments,
        call->argc,depth,call->called,result);
}

/* Shared by DIAMOND_OP_NEW's #initialize call, DIAMOND_OP_INVOKE/_MONO/
 * _TYPED's ordinary Instance method dispatch, and DIAMOND_OP_SUPER: all
 * three build the same self-plus-arguments-plus-bound-values buffer and
 * call an already-resolved `method`, differing only in how they got there
 * (constructor lookup, cache/vtable dispatch, superclass lookup) and
 * whether explicit generic type arguments are possible (only real
 * `DIAMOND_OP_INVOKE_TYPED` calls; `typed` false skips that entirely, and
 * `type_argument_count`/`type_arguments`/`caller_chunk` are then unused).
 * `self_value` is the receiver placed at args[0]; the caller keeps
 * whatever separate write it needs to make with its own return value (NEW
 * discards this call's own result -- `initialize`'s return isn't the
 * constructed instance -- rather than routing it through here). Factored
 * out (like call_closure_helper/forward_to_top_level_helper above) so the
 * DIAMOND_MAX_ARGUMENTS+1-sized buffer doesn't live directly in any of
 * run_chunk's own case blocks, times three. */
static inline uint16_t getter_operand(const uint8_t *code) {
    return (uint16_t)(((unsigned)code[0] << 8) | code[1]);
}

/* Whether a getter's return type or a setter's parameter type plainly
 * accepts `value`, for the call shortcuts below: true only where
 * value_matches_set is sure to say yes too, false meaning "make the call".
 * Beyond value_plainly_matches_set it settles user-class members, which a
 * class-typed field (`-> Node | Nil`) needs: an exact instance of the class
 * matches, and nil, a Bool, an Int or a Float never does, so the walk can
 * go on to the next member. Anything else at a class member (a subclass
 * instance, a native object) is left to the full matcher, and so is every
 * generic or otherwise non-plain member. `classes` is the table the
 * function's own type ids index, its chunk's. */
__attribute__((always_inline))
static inline bool shortcut_type_matches(const DiamondChunk *function_chunk,
        const DiamondTypeSet *set,DiamondValue value) {
    /* The commonest field types, before the general walk. */
    if(set->count!=0&&set->members[0].argument_set==DIAMOND_NO_TYPE_SET&&
       set->members[0].second_argument_set==DIAMOND_NO_TYPE_SET&&
       ((set->members[0].id==DIAMOND_TYPE_INT&&value.kind==DIAMOND_VALUE_INT)||
        (set->members[0].id==DIAMOND_TYPE_FLOAT&&value.kind==DIAMOND_VALUE_FLOAT)))
        return true;
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember *member=&set->members[index];
        if(member->argument_set!=DIAMOND_NO_TYPE_SET||
           member->second_argument_set!=DIAMOND_NO_TYPE_SET)return false;
        const int plain=diamond_plain_type_match(value,member->id);
        if(plain==1)return true;
        if(plain==0)continue;
        if(member->id<DIAMOND_TYPE_CLASS_BASE||member->id>=DIAMOND_TYPE_VARIABLE_BASE)
            return false;
        if(value.kind==DIAMOND_VALUE_NIL||value.kind==DIAMOND_VALUE_BOOL||
           value.kind==DIAMOND_VALUE_INT||value.kind==DIAMOND_VALUE_FLOAT)
            continue;
        const size_t class_index=(size_t)(member->id-DIAMOND_TYPE_CLASS_BASE);
        if(value.kind!=DIAMOND_VALUE_OBJECT||
           value.as.object->kind!=DIAMOND_OBJECT_INSTANCE||
           class_index>=function_chunk->class_count||
           ((const DiamondInstance *)value.as.object)->class!=
               function_chunk->classes[class_index])
            return false;
        return true;
    }
    return false;
}

/* Recognizes a trivial accessor body (DiamondAccessor). A getter reads one
 * instance variable and returns it (`def x() = @x`, optionally with a
 * declared return type):
 *
 *     GET_IVAR  rD, r0, field          7 bytes
 *     CHECK_TYPE rD, set               5 bytes, only with a return type
 *     RETURN    rD                     3 bytes
 *
 * and takes self alone without an arity error. A setter stores its one
 * argument and returns it (`attr_accessor`'s `x=`, or `def x=(v) = @x = v`):
 *
 *     CHECK_TYPE r1, set               5 bytes, only with a parameter type
 *     SET_IVAR  r0, field, r1          7 bytes
 *     RETURN    r1                     3 bytes
 *
 * and takes exactly self plus one argument. diamond_vm_run stores the
 * answer in fn->accessor; a function it has not filled (a debugger run, or
 * a compile_method chunk) is decoded on each call by function_accessor. */
DiamondAccessor diamond_decode_accessor(const DiamondFunction *fn) {
    const DiamondAccessor none={.kind=DIAMOND_ACCESSOR_NONE,.set=DIAMOND_NO_TYPE_SET};
    if(fn->code_count!=10&&fn->code_count!=15)return none;
    const uint8_t *code=fn->code;
    const bool typed=fn->code_count==15;
    if(code[0]==DIAMOND_OP_GET_IVAR) {
        const uint16_t dest=getter_operand(code+1);
        const uint16_t field=getter_operand(code+5);
        if(getter_operand(code+3)!=0||field>UINT8_MAX)return none;
        size_t at=7;
        uint16_t set=DIAMOND_NO_TYPE_SET;
        if(typed) {
            if(code[7]!=DIAMOND_OP_CHECK_TYPE||getter_operand(code+8)!=dest)return none;
            set=getter_operand(code+10);
            if((size_t)set>=fn->type_set_count)return none;
            at=12;
        }
        if(code[at]!=DIAMOND_OP_RETURN||getter_operand(code+at+1)!=dest)return none;
        /* The call passes one argument, self. */
        if(fn->required_arity>1||(fn->arity<1&&!fn->has_variadic))return none;
        return (DiamondAccessor){.kind=DIAMOND_ACCESSOR_GETTER,.field=(uint8_t)field,.set=set};
    }
    if(fn->arity!=2||fn->required_arity!=2||fn->has_variadic)return none;
    size_t at=0;
    uint16_t set=DIAMOND_NO_TYPE_SET;
    if(typed) {
        if(code[0]!=DIAMOND_OP_CHECK_TYPE||getter_operand(code+1)!=1)return none;
        set=getter_operand(code+3);
        if((size_t)set>=fn->type_set_count)return none;
        at=5;
    }
    if(code[at]!=DIAMOND_OP_SET_IVAR||getter_operand(code+at+1)!=0||
       getter_operand(code+at+5)!=1)return none;
    const uint16_t field=getter_operand(code+at+3);
    if(field>UINT8_MAX)return none;
    if(code[at+7]!=DIAMOND_OP_RETURN||getter_operand(code+at+8)!=1)return none;
    return (DiamondAccessor){.kind=DIAMOND_ACCESSOR_SETTER,.field=(uint8_t)field,.set=set};
}

static inline DiamondAccessor function_accessor(const DiamondFunction *fn) {
    return fn->entry_check.computed?fn->accessor:diamond_decode_accessor(fn);
}

/* Recognizes a trivial constructor body: an `initialize` that checks its
 * parameters' declared types and copies them into fields, in any order,
 * then returns,
 *
 *     CHECK_TYPE rP, set               5 bytes, P a parameter (1..arity-1)
 *     SET_IVAR  r0, field, rP          7 bytes, at least one
 *     RETURN    rX                     3 bytes
 *
 * taking exactly its declared arguments (no defaults, no splat).
 * diamond_vm_run stores the answer in fn->trivial_constructor. */
bool diamond_decode_constructor(const DiamondFunction *fn) {
    if(fn->has_variadic||fn->arity<2||fn->required_arity!=fn->arity)return false;
    const uint8_t *code=fn->code;
    bool stored=false;
    size_t at=0;
    while(at<fn->code_count) {
        if(code[at]==DIAMOND_OP_CHECK_TYPE) {
            if(at+5>fn->code_count)return false;
            const uint16_t parameter=getter_operand(code+at+1);
            if(parameter<1||parameter>=fn->arity||
               (size_t)getter_operand(code+at+3)>=fn->type_set_count)return false;
            at+=5;
        } else if(code[at]==DIAMOND_OP_SET_IVAR) {
            if(at+7>fn->code_count)return false;
            const uint16_t parameter=getter_operand(code+at+5);
            if(getter_operand(code+at+1)!=0||getter_operand(code+at+3)>UINT8_MAX||
               parameter<1||parameter>=fn->arity)return false;
            stored=true;
            at+=7;
        } else {
            return code[at]==DIAMOND_OP_RETURN&&at+3==fn->code_count&&stored;
        }
    }
    return false;
}

/* `Class.new(...)` with a trivial constructor (diamond_decode_constructor),
 * answered without a frame: every parameter check first, then the stores
 * through diamond_jit_set_ivar as the body makes them. A check that fails
 * or needs the full type matcher, or a field past the instance's, returns
 * false before anything is stored, and the caller runs `initialize` as
 * usual, so errors, messages and backtraces are the ordinary ones.
 * `arguments` are the call's explicit arguments, the body's r1 onwards.
 * Otherwise true, with *status the stores' outcome. */
static bool trivial_constructor_result(DiamondVm *vm,const DiamondChunk *function_chunk,
        const DiamondFunction *fn,DiamondValue self_value,const DiamondValue *arguments,
        DiamondVmStatus *status) {
    const DiamondInstance *instance=(const DiamondInstance *)self_value.as.object;
    const uint8_t *code=fn->code;
    size_t at=0;
    while(code[at]!=DIAMOND_OP_RETURN) {
        if(code[at]==DIAMOND_OP_CHECK_TYPE) {
            const uint16_t parameter=getter_operand(code+at+1);
            if(!shortcut_type_matches(function_chunk,
                   &fn->type_sets[getter_operand(code+at+3)],arguments[parameter-1]))
                return false;
            at+=5;
        } else {
            if(getter_operand(code+at+3)>=instance->field_count)return false;
            at+=7;
        }
    }
    for(at=0;code[at]!=DIAMOND_OP_RETURN;) {
        if(code[at]==DIAMOND_OP_SET_IVAR) {
            const DiamondVmStatus stored=diamond_jit_set_ivar(vm,nullptr,&self_value,
                (uint8_t)getter_operand(code+at+3),&arguments[getter_operand(code+at+5)-1]);
            if(stored!=DIAMOND_VM_OK) {
                *status=stored;
                return true;
            }
            at+=7;
        } else {
            at+=5;
        }
    }
    *status=DIAMOND_VM_OK;
    return true;
}

/* A getter call answered without a frame: the field read the body would
 * make. Every case where the body could fail or behave otherwise returns
 * false and the method is called as usual, so errors, messages and
 * backtraces are the ordinary ones: a receiver that isn't an instance, a
 * field past the instance's, or a value the declared type rejects (or that
 * needs the full type matcher). The caller skips this with a debugger
 * attached, which can stop inside the body. */
static bool trivial_getter_result(const DiamondChunk *function_chunk,
        const DiamondFunction *fn,DiamondAccessor accessor,
        DiamondValue self_value,DiamondValue *result) {
    if(self_value.kind!=DIAMOND_VALUE_OBJECT||
       self_value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)
        return false;
    const DiamondInstance *instance=(const DiamondInstance *)self_value.as.object;
    if(accessor.field>=instance->field_count)return false;
    const DiamondValue value=instance_field_value(instance,accessor.field);
    if(accessor.set!=DIAMOND_NO_TYPE_SET&&
       !shortcut_type_matches(function_chunk,&fn->type_sets[accessor.set],value))
        return false;
    *result=value;
    return true;
}

/* The setter twin of trivial_getter_result, storing through
 * diamond_jit_set_ivar as the body would. Whenever the body could fail or
 * behave otherwise -- an argument the parameter type rejects (or that needs
 * the full matcher), a frozen receiver, a field past the instance's -- it
 * returns false before writing anything and the method is called as usual.
 * A failed write barrier (out of memory) after the store also falls back:
 * the call stores the same value again and reports the error itself. */
static bool trivial_setter_result(DiamondVm *vm,const DiamondChunk *function_chunk,
        const DiamondFunction *fn,DiamondAccessor accessor,
        DiamondValue self_value,DiamondValue argument,DiamondValue *result) {
    if(self_value.kind!=DIAMOND_VALUE_OBJECT||
       self_value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)
        return false;
    const DiamondInstance *instance=(const DiamondInstance *)self_value.as.object;
    if(instance->object.frozen||accessor.field>=instance->field_count)return false;
    if(accessor.set!=DIAMOND_NO_TYPE_SET&&
       !shortcut_type_matches(function_chunk,&fn->type_sets[accessor.set],argument))
        return false;
    if(diamond_jit_set_ivar(vm,nullptr,&self_value,accessor.field,&argument)!=DIAMOND_VM_OK)
        return false;
    *result=argument;
    return true;
}

/* Range#to_a for a bounded Int range, filled here instead of by the prelude's
 * push loop (lib/core.d6), which costs a push call per element. Only an exact
 * Range (a subclass may override to_a) with Int bounds and a Bool `exclusive`
 * qualifies; Range's own methods cannot be redefined. Everything else (an
 * endless range, which raises there, an inclusive end at the largest Int,
 * where the prelude's `@end + 1` becomes a big integer, or a range of more
 * than 2^26 values, whose allocation is left to the prelude's own growth)
 * returns false and the prelude method runs. Returns true with *status set
 * when it answered. */
static bool native_range_to_a(DiamondVm *vm,const DiamondChunk *owner_chunk,
        const DiamondMethod *method,DiamondValue self_value,
        DiamondValue *result,DiamondVmStatus *status) {
    /* The class test comes first: it is one comparison, and every other
     * method call with no arguments passes through here. */
    if(vm->range_class_index==DIAMOND_NO_CLASS||
       (size_t)vm->range_class_index>=owner_chunk->class_count||
       self_value.kind!=DIAMOND_VALUE_OBJECT||
       self_value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)
        return false;
    const DiamondInstance *range=(const DiamondInstance *)self_value.as.object;
    if(range->class!=owner_chunk->classes[vm->range_class_index]||
       strcmp(method->name,"to_a")!=0||range->field_count<3||
       range->fields[0].kind!=DIAMOND_VALUE_INT||range->fields[1].kind!=DIAMOND_VALUE_INT||
       range->fields[2].kind!=DIAMOND_VALUE_BOOL)
        return false;
    const int64_t start=range->fields[0].as.integer;
    int64_t stop=range->fields[1].as.integer;
    if(!range->fields[2].as.boolean&&ckd_add(&stop,stop,1))return false;
    uint64_t count=0;
    if(stop>start)count=(uint64_t)stop-(uint64_t)start;
    if(count>(UINT64_C(1)<<26))return false;
    DiamondArray *values=allocate_array_uninitialized(vm,(size_t)count);
    if(values==nullptr) {*status=DIAMOND_VM_OUT_OF_MEMORY;return true;}
    for(size_t index=0;index<(size_t)count;index++)
        values->values[index]=DIAMOND_INT(start+(int64_t)index);
    *result=DIAMOND_OBJECT(values);
    *status=DIAMOND_VM_OK;
    return true;
}

__attribute__((noinline))
static DiamondVmStatus invoke_resolved_method_general(DiamondVm *vm,
        const DiamondChunk *owner_chunk,const DiamondMethod *method,
        DiamondValue self_value,const DiamondValue *registers,uint16_t base,
        uint8_t argc,bool typed,uint8_t type_argument_count,
        const uint16_t *type_arguments,const DiamondChunk *caller_chunk,
        size_t depth,DiamondValue *result) {
    /* bound_value_count is set from an arbitrary compile_method() call's
     * own bound-values Hash size (src/vm.c's compile_method_helper), with
     * no independent cap of its own -- unlike argc, which is already
     * type-bounded to DIAMOND_MAX_ARGUMENTS by being a plain uint8_t, this
     * needs its own explicit guard against the shared buffer below. */
    if((size_t)argc+1+method->bound_value_count>(size_t)DIAMOND_MAX_ARGUMENTS+1)
        return DIAMOND_VM_ARITY_ERROR;
    DiamondValue args[DIAMOND_MAX_ARGUMENTS+1];args[0]=self_value;
    for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
    /* Trailing parameters compile_method's own synthesized source appended
     * -- see DiamondMethod.bound_values's own comment (src/vm.h). Not
     * supplied by the caller, so not part of argc above. */
    for(size_t i=0;i<method->bound_value_count;i++)
        args[argc+1+i]=method->bound_values[i];
    const size_t total_args=(size_t)argc+1+method->bound_value_count;
    /* A ClassName.compile_method-installed method's bytecode (and the
     * class/function/interface tables it references by index) lives in
     * its own chunk, not `owner_chunk` -- see DiamondMethod.source_chunk's
     * own comment (src/vm.h). */
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner_chunk;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    const size_t class_offset=fn->class_type_variable;
    if(typed&&(fn->type_variable_count>8||fn->type_variable_count<class_offset||
       type_argument_count!=fn->type_variable_count-class_offset)) {
        if(fn->class_type_variable&&fn->type_variable_count==fn->class_type_variable)
            snprintf(vm->error,sizeof vm->error,
                "generic class type arguments are fixed by construction");
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* Only the first type_argument_count entries are ever read (below,
     * and through .type_variable_bindings); zeroing all eight cost about
     * 3 KB of memset on every call, typed or not. */
    /* Ordinary calls must not reserve eight full binding graphs on every
     * recursive frame. Typed calls need exactly the declared slots. */
    DiamondTypeBinding explicit_bindings[typed&&fn->type_variable_count>0?fn->type_variable_count:1];
    if(typed&&class_offset!=0)explicit_bindings[0]=(DiamondTypeBinding){};
    for(size_t index=0;index<type_argument_count;index++) {
        DiamondTypeBinding *binding=&explicit_bindings[index+class_offset];
        *binding=(DiamondTypeBinding){};
        if((size_t)type_arguments[index]>=caller_chunk->type_set_count)
            return DIAMOND_VM_INVALID_BYTECODE;
        (void)binding_node(binding);
        bind_context_set(binding,0,caller_chunk,
            caller_chunk->type_sets,type_arguments[index]);
    }
    DiamondChunk child_storage;
    const DiamondChunk *child;
    if(type_argument_count==0) {
        child=function_call_chunk(function_chunk,fn,&child_storage);
    } else {
        child_storage=plain_call_chunk(fn,function_chunk);
        child_storage.type_variable_bindings=explicit_bindings;
        child=&child_storage;
    }
    return jit_call_or_interpret(vm,fn,child,args,total_args,depth,nullptr,result);
}

/* Methods with at most this many arguments, called without type arguments or
 * compile_method bound values, take invoke_resolved_method_plain. */
enum { DIAMOND_PLAIN_METHOD_ARGUMENTS = 7 };

/* invoke_resolved_method_general for the common call, with the same chunk and
 * arguments it would build (no explicit bindings, so type_variable_bindings
 * stays nullptr, as it does there for an untyped call). Kept apart so this
 * frame holds eight argument slots rather than the general one's 256-slot
 * buffer and binding array, which every call used to pay for on entry. */
__attribute__((always_inline))
static inline DiamondVmStatus invoke_resolved_method_plain(DiamondVm *vm,
        const DiamondChunk *function_chunk,const DiamondFunction *fn,
        DiamondValue self_value,const DiamondValue *registers,uint16_t base,
        uint8_t argc,size_t depth,DiamondValue *result) {
    DiamondValue args[DIAMOND_PLAIN_METHOD_ARGUMENTS+1];
    args[0]=self_value;
    /* At most seven values: copied one by one, since GCC would otherwise turn
     * the loop into a memcpy call that costs more than the copy. The empty asm
     * is what stops it. */
    for(size_t i=0;i<argc;i++) {
        args[i+1]=registers[(size_t)base+i];
        __asm__ volatile("" : : "r"(args) : "memory");
    }
    DiamondChunk scratch;
    const DiamondChunk *child=function_call_chunk(function_chunk,fn,&scratch);
    return jit_call_or_interpret(vm,fn,child,args,(size_t)argc+1,depth,nullptr,result);
}

/* Calls `method` on `self_value` with the caller's registers[base..base+argc)
 * as its arguments. A getter, a bounded Int Range#to_a, and every call with no
 * type arguments, no bound values and few arguments are answered before
 * invoke_resolved_method_general's large frame is ever set up. */
__attribute__((always_inline)) static inline DiamondVmStatus
invoke_resolved_method_dispatch(DiamondVm *vm,
        const DiamondChunk *owner_chunk,const DiamondMethod *method,
        DiamondValue self_value,const DiamondValue *registers,uint16_t base,
        uint8_t argc,bool typed,uint8_t type_argument_count,
        const uint16_t *type_arguments,const DiamondChunk *caller_chunk,
        size_t depth,DiamondValue *result) {
    if(typed||method->bound_value_count!=0||argc>DIAMOND_PLAIN_METHOD_ARGUMENTS)
        return invoke_resolved_method_general(vm,owner_chunk,method,self_value,
            registers,base,argc,typed,type_argument_count,type_arguments,
            caller_chunk,depth,result);
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner_chunk;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    /* Both shortcut bodies are 10 or 15 bytes; checking that here keeps
     * every other method from paying for the calls. */
    const bool shortcut_sized=fn->code_count==10||fn->code_count==15;
    if(argc==0&&vm->debug_fd<0) {
        if(shortcut_sized) {
            const DiamondAccessor accessor=function_accessor(fn);
            if(accessor.kind==DIAMOND_ACCESSOR_GETTER&&
               trivial_getter_result(function_chunk,fn,accessor,self_value,result))
                return DIAMOND_VM_OK;
        }
        if(method->source_chunk==nullptr) {
            DiamondVmStatus range_status;
            if(native_range_to_a(vm,owner_chunk,method,self_value,result,&range_status))
                return range_status;
        }
    } else if(argc==1&&shortcut_sized&&vm->debug_fd<0) {
        const DiamondAccessor accessor=function_accessor(fn);
        if(accessor.kind==DIAMOND_ACCESSOR_SETTER&&
           trivial_setter_result(vm,function_chunk,fn,accessor,self_value,registers[base],result))
            return DIAMOND_VM_OK;
    }
    return invoke_resolved_method_plain(vm,function_chunk,fn,self_value,
        registers,base,argc,depth,result);
}

DiamondVmStatus invoke_resolved_method_helper(DiamondVm *vm,
        const DiamondChunk *owner_chunk,const DiamondMethod *method,
        DiamondValue self_value,const DiamondValue *registers,uint16_t base,
        uint8_t argc,bool typed,uint8_t type_argument_count,
        const uint16_t *type_arguments,const DiamondChunk *caller_chunk,
        size_t depth,DiamondValue *result) {
    return invoke_resolved_method_dispatch(vm,owner_chunk,method,self_value,
        registers,base,argc,typed,type_argument_count,type_arguments,
        caller_chunk,depth,result);
}

/* diamond_mono_invoke's full call, in its own frame so an accessor answered
 * there does not set up this one's argument and chunk storage. */
__attribute__((noinline))
static DiamondVmStatus mono_invoke_call(DiamondVm *vm,const DiamondChunk *owner,
        const DiamondMethod *method,const DiamondValue *registers,uint16_t recv,
        uint16_t base,uint8_t argc,const DiamondChunk *chunk,size_t depth,
        DiamondValue *result) {
    return invoke_resolved_method_dispatch(vm,owner,method,registers[recv],
        registers,base,argc,false,0,nullptr,chunk,depth,result);
}

/* Fills a monomorphic cache entry's shortcut the first time
 * diamond_mono_invoke gets past its method checks with it: GETTER or
 * SETTER when the method is an attr accessor this site calls with the
 * accessor's own argument count, NONE for anything else. The caller has
 * already ruled out a private or protected method, a wrong arity and the
 * exception field names, all fixed for one site and method. */
/* DIAMOND_SHORTCUT_CHECK_CLASS for a type set that is one plain user class,
 * DIAMOND_SHORTCUT_CHECK_CLASS_OR_NIL for that class and Nil in either
 * order, with the class stored in entry->shortcut_class; anything else
 * stays DIAMOND_SHORTCUT_CHECK_FULL. */
static uint8_t classify_class_check(DiamondMethodCacheEntry *entry,
        const DiamondChunk *function_chunk,const DiamondTypeSet *set) {
    if(set->count==0||set->count>2)return DIAMOND_SHORTCUT_CHECK_FULL;
    const DiamondClass *class=nullptr;
    bool nil=false;
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember *member=&set->members[index];
        if(member->argument_set!=DIAMOND_NO_TYPE_SET||
           member->second_argument_set!=DIAMOND_NO_TYPE_SET)
            return DIAMOND_SHORTCUT_CHECK_FULL;
        if(member->id==DIAMOND_TYPE_NIL&&!nil) {
            nil=true;
            continue;
        }
        if(class!=nullptr||member->id<DIAMOND_TYPE_CLASS_BASE||
           member->id>=DIAMOND_TYPE_VARIABLE_BASE)
            return DIAMOND_SHORTCUT_CHECK_FULL;
        const size_t class_index=(size_t)(member->id-DIAMOND_TYPE_CLASS_BASE);
        if(class_index>=function_chunk->class_count||
           function_chunk->classes[class_index]==nullptr)
            return DIAMOND_SHORTCUT_CHECK_FULL;
        class=function_chunk->classes[class_index];
    }
    if(class==nullptr)return DIAMOND_SHORTCUT_CHECK_FULL;
    entry->shortcut_class=class;
    return nil?DIAMOND_SHORTCUT_CHECK_CLASS_OR_NIL:DIAMOND_SHORTCUT_CHECK_CLASS;
}

__attribute__((noinline,cold))
static void classify_mono_shortcut(DiamondMethodCacheEntry *entry,
        const DiamondChunk *owner,const DiamondMethod *method,uint8_t argc) {
    entry->shortcut=DIAMOND_SHORTCUT_NONE;
    if(argc>1||method->bound_value_count!=0)return;
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    if(fn->code_count!=10&&fn->code_count!=15)return;
    const DiamondAccessor accessor=function_accessor(fn);
    if(accessor.kind!=(argc==0?DIAMOND_ACCESSOR_GETTER:DIAMOND_ACCESSOR_SETTER))return;
    uint8_t check=DIAMOND_SHORTCUT_CHECK_NONE;
    if(accessor.set!=DIAMOND_NO_TYPE_SET) {
        const DiamondTypeSet *set=&fn->type_sets[accessor.set];
        const bool lone=set->count==1&&
            set->members[0].argument_set==DIAMOND_NO_TYPE_SET&&
            set->members[0].second_argument_set==DIAMOND_NO_TYPE_SET;
        check=lone&&set->members[0].id==DIAMOND_TYPE_FLOAT?DIAMOND_SHORTCUT_CHECK_FLOAT:
            lone&&set->members[0].id==DIAMOND_TYPE_INT?DIAMOND_SHORTCUT_CHECK_INT:
            DIAMOND_SHORTCUT_CHECK_FULL;
        if(check==DIAMOND_SHORTCUT_CHECK_FULL)
            check=classify_class_check(entry,function_chunk,set);
    }
    entry->shortcut_field=accessor.field;
    entry->shortcut_check=check;
    entry->shortcut=argc==0?DIAMOND_SHORTCUT_GETTER:DIAMOND_SHORTCUT_SETTER;
}

/* DIAMOND_SHORTCUT_CHECK_FULL: the accessor's declared type is more than a
 * lone Int or Float (`Node | Nil`, a class, a String), so the value goes
 * through shortcut_type_matches on the method's own type set, exactly as
 * trivial_getter_result and trivial_setter_result judge it. */
__attribute__((noinline))
static bool shortcut_full_check(const DiamondVm *vm,const DiamondInstance *instance,
        const DiamondMethod *method,DiamondValue value) {
    const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    const DiamondAccessor accessor=function_accessor(fn);
    return accessor.set!=DIAMOND_NO_TYPE_SET&&
        shortcut_type_matches(function_chunk,&fn->type_sets[accessor.set],value);
}

/* Whether a classified accessor's value passes the check its declared type
 * asks for (DiamondMethodCacheEntry.shortcut_check). */
__attribute__((always_inline))
static inline bool shortcut_value_passes(const DiamondVm *vm,
        const DiamondMethodCacheEntry *entry,const DiamondInstance *instance,
        DiamondValue value) {
    switch(entry->shortcut_check) {
        case DIAMOND_SHORTCUT_CHECK_NONE: return true;
        case DIAMOND_SHORTCUT_CHECK_FLOAT: return value.kind==DIAMOND_VALUE_FLOAT;
        case DIAMOND_SHORTCUT_CHECK_INT: return value.kind==DIAMOND_VALUE_INT;
        case DIAMOND_SHORTCUT_CHECK_CLASS_OR_NIL:
            if(value.kind==DIAMOND_VALUE_NIL)return true;
            [[fallthrough]];
        case DIAMOND_SHORTCUT_CHECK_CLASS:
            /* An exact instance passes; a subclass instance or anything
             * else is judged by the full walk, which may still accept it. */
            if(value.kind==DIAMOND_VALUE_OBJECT&&
               value.as.object->kind==DIAMOND_OBJECT_INSTANCE&&
               ((const DiamondInstance *)value.as.object)->class==entry->shortcut_class)
                return true;
            return shortcut_full_check(vm,instance,entry->method,value);
        default: return shortcut_full_check(vm,instance,entry->method,value);
    }
}

/* run_chunk's INVOKE_MONO on an Instance whose site cache holds the
 * receiver's class: the whole call, in one cache lookup and without
 * diamond_jit_invoke_instance's frame. Everything that path checks before
 * reaching the cached method holds here: the cache only holds a method
 * lookup_method found on this class, so none of the universal methods it
 * tests by name can apply. A private or protected method, a wrong arity and
 * the exception-field names, which that path intercepts even when a class
 * defines them, are left to it: DIAMOND_VM_NOT_CACHED, having changed
 * nothing. Otherwise the call's status, with *result set on success.
 * `site` is the INVOKE_MONO instruction, whose operands run_chunk has already
 * bounds-checked; they are read from it here so that every argument of this
 * call travels in a register. */
DiamondVmStatus diamond_mono_invoke(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,const uint8_t *site,size_t depth,
        DiamondValue *result) {
    const uint16_t recv=(uint16_t)(((unsigned)site[3]<<8)|site[4]);
    const uint16_t name=(uint16_t)(((unsigned)site[5]<<8)|site[6]);
    const uint16_t base=(uint16_t)(((unsigned)site[7]<<8)|site[8]);
    const uint8_t argc=site[9];
    const DiamondStringConstant *method_name=&chunk->strings[name];
    const DiamondInstance *instance=(const DiamondInstance *)registers[recv].as.object;
    const size_t slot=diamond_cache_find(site,vm->method_caches,
        sizeof vm->method_caches[0]);
    if(slot==SIZE_MAX||vm->method_caches[slot].entry_count!=1||
       vm->method_caches[slot].entries[0].receiver_class!=instance->class)
        return DIAMOND_VM_NOT_CACHED;
    DiamondMethodCacheEntry *entry=&vm->method_caches[slot].entries[0];
    /* A getter or setter this entry has already classified: everything
     * below would come to the same answer for this site and method, so
     * only the per-value checks trivial_getter_result and
     * trivial_setter_result make are left. A value that fails them takes
     * the ordinary call further down, as it would have before. */
    if(entry->shortcut>=DIAMOND_SHORTCUT_GETTER&&vm->debug_fd<0) {
        const uint8_t field=entry->shortcut_field;
        if(entry->shortcut==DIAMOND_SHORTCUT_GETTER) {
            if(field<instance->field_count) {
                const DiamondValue value=instance_field_value(instance,field);
                if(shortcut_value_passes(vm,entry,instance,value)) {
                    vm->inline_cache_hits++;
                    vm->monomorphic_dispatches++;
                    *result=value;
                    return DIAMOND_VM_OK;
                }
            }
        } else {
            const DiamondValue argument=registers[base];
            if(!instance->object.frozen&&field<instance->field_count&&
               shortcut_value_passes(vm,entry,instance,argument)) {
                DiamondValue self_value=registers[recv];
                if(diamond_jit_set_ivar(vm,nullptr,&self_value,field,&argument)==DIAMOND_VM_OK) {
                    vm->inline_cache_hits++;
                    vm->monomorphic_dispatches++;
                    *result=argument;
                    return DIAMOND_VM_OK;
                }
            }
        }
    }
    const DiamondMethod *method=entry->method;
    if(method==nullptr||method->is_private||method->is_protected||
       argc<method->required_arity||(argc>method->arity&&!method->has_variadic))
        return DIAMOND_VM_NOT_CACHED;
    if((method_name->length==7&&memcmp(method_name->chars,"message",7)==0)||
       (method_name->length==5&&memcmp(method_name->chars,"cause",5)==0)||
       (method_name->length==9&&memcmp(method_name->chars,"backtrace",9)==0))
        return DIAMOND_VM_NOT_CACHED;
    vm->inline_cache_hits++;
    vm->monomorphic_dispatches++;
    const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
    if(entry->shortcut==DIAMOND_SHORTCUT_UNKNOWN)
        classify_mono_shortcut(entry,owner,method,argc);
    /* An attr getter or setter is answered here, in this small frame; most
     * methods are not one, so the size test comes first. */
    if(argc<=1&&method->bound_value_count==0&&vm->debug_fd<0) {
        const DiamondChunk *function_chunk=
            method->source_chunk!=nullptr?method->source_chunk:owner;
        const DiamondFunction *fn=function_chunk->functions[method->function_index];
        if(fn->code_count==10||fn->code_count==15) {
            const DiamondAccessor accessor=function_accessor(fn);
            if(accessor.kind==(argc==0?DIAMOND_ACCESSOR_GETTER:DIAMOND_ACCESSOR_SETTER)&&
               (argc==0?trivial_getter_result(function_chunk,fn,accessor,registers[recv],result):
                trivial_setter_result(vm,function_chunk,fn,accessor,registers[recv],
                    registers[base],result)))
                return DIAMOND_VM_OK;
        }
    }
    return mono_invoke_call(vm,owner,method,registers,recv,base,argc,chunk,depth,result);
}

/* JIT trampoline for a plain DIAMOND_OP_CALL: the same validation and the same
 * callee chunk (plain_call_chunk) as the interpreter's case, dispatched through
 * jit_call_or_interpret so a compiled callee runs as native code. A real call
 * with real side effects, so jit.c treats it like SUPER: every bail from the
 * moment it compiles onward propagates the exact status instead of retrying. */
DiamondVmStatus diamond_jit_call_function(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t function_index, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out) {
    if ((size_t)function_index >= chunk->function_count ||
        (size_t)base + argc > DIAMOND_REGISTER_COUNT)
        return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFunction *function = chunk->functions[function_index];
    if (argc < function->required_arity ||
        (argc > function->arity && !function->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    /* A native call to another function (compile_call_native) checks the
     * depth against this, 0 until a call comes through here. */
    if (vm->jit_depth_limit == 0) vm->jit_depth_limit = diamond_call_depth_limit(vm);
    DiamondChunk called_storage;
    const DiamondChunk *called_chunk = plain_call_chunk_for(function, chunk, &called_storage);
    return jit_call_or_interpret(vm, function, called_chunk, &registers[base],
        argc, depth, nullptr, out);
}

/* A compiled function's plain CALL of itself (recursion). jit.c emits this
 * instead of diamond_jit_call_function only once it has checked, at compile
 * time, that the index is the function's own and the argument count fits
 * its arity and the register file. What is left to do at run time is what
 * jit_call_core would find unchanged: the caller is running this function's
 * native code, so the VM has the JIT on and no instruction or wall-clock
 * budget (either keeps a VM on the interpreter from the start). That leaves
 * the depth limit, then straight into the native code, with every other
 * outcome handed to the same paths jit_call_or_interpret uses. */
DiamondVmStatus diamond_jit_call_self(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t function_index, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out) {
    /* The index was this function's own in the table it was compiled
     * against; anything else (another table, not compiled) takes the
     * general path. */
    const DiamondFunction *function = (size_t)function_index < chunk->function_count
        ? chunk->functions[function_index] : nullptr;
    if (function == nullptr || function->code != chunk->code || function->jit_code == nullptr)
        return diamond_jit_call_function(vm, chunk, registers, function_index, base, argc, depth, out);
    /* The native self-call's limit (compile_self_call_native), filled in
     * here on this thread the first time it comes through. */
    vm->jit_depth_limit = diamond_call_depth_limit(vm);
    if (depth + 1 >= vm->jit_depth_limit) return DIAMOND_VM_STACK_OVERFLOW;
    DiamondChunk called_storage;
    const DiamondChunk *called_chunk = plain_call_chunk_for(function, chunk, &called_storage);
    const uint8_t status = jit_run_native(vm, function, called_chunk, &registers[base], argc,
        depth, nullptr, out);
    if (status == DIAMOND_JIT_TAIL) return jit_tail_loop(vm, called_chunk, depth, out);
    if (status == DIAMOND_JIT_RETRY)
        return run_chunk(called_chunk, vm, &registers[base], argc, depth + 1, nullptr, out);
    return (DiamondVmStatus)status;
}


/* What follows a direct native self-call (compile_call_function in jit.c
 * calls its own entry without a C round trip) whose status was not
 * DIAMOND_VM_OK: exactly what jit_run_native and diamond_jit_call_self do
 * with that status, counters included. `arguments` are the caller's
 * registers the call was made with, unchanged, so a retry interprets the
 * call from them. */
DiamondVmStatus diamond_jit_self_call_finish(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *arguments, uint8_t argc, size_t depth, DiamondValue *out,
        uint8_t status) {
    if (status == DIAMOND_JIT_TAIL) return jit_tail_loop(vm, chunk, depth, out);
    if (status == DIAMOND_JIT_RETRY) {
        vm->jit_bailouts++;
        return run_chunk(chunk, vm, arguments, argc, depth + 1, nullptr, out);
    }
    vm->jit_hard_propagations++;
    return (DiamondVmStatus)status;
}
/* JIT trampoline for DIAMOND_OP_TAIL_CALL to a function other than the
 * caller. Returns DIAMOND_JIT_TAIL after staging the callee and its
 * arguments when the interpreter's own in-place switch would apply (a target
 * whose registers hold the arguments: jit_tail_loop's copy of them is not a
 * GC root, so a variadic target's overflow could not wait there); otherwise it is
 * an ordinary call whose result lands in `out` (the compiled code then
 * returns it) -- the same fallback the interpreter makes. */
uint8_t diamond_jit_tail_call_function(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t function_index, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out) {
    if ((size_t)function_index >= chunk->function_count ||
        (size_t)base + argc > DIAMOND_REGISTER_COUNT)
        return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFunction *function = chunk->functions[function_index];
    if (argc < function->required_arity ||
        (argc > function->arity && !function->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    const size_t callee_registers = function->register_count == 0
        ? DIAMOND_REGISTER_COUNT : function->register_count;
    /* A generic function's call to itself is staged too: run_chunk's in-place
     * self tail call infers its type variables again from the new arguments,
     * which jit_tail_loop's fresh entry does (diamond_jit_bind_generic). A
     * variadic function's is too: compiled code has no in-place loop for one
     * (compile_self_tail_call), and the interpreter collects its rest afresh. */
    if ((function->code != chunk->code || function->type_variable_count > 0 ||
         function->has_variadic) &&
        argc <= DIAMOND_MAX_DECLARED_PARAMETERS && argc <= callee_registers) {
        vm->jit_tail_function = function;
        vm->jit_tail_count = argc;
        for (size_t index = 0; index < argc; index++)
            vm->jit_tail_args[index] = registers[(size_t)base + index];
        return DIAMOND_JIT_TAIL;
    }
    const DiamondChunk called_chunk = plain_call_chunk(function, chunk);
    return (uint8_t)jit_call_or_interpret(vm, function, &called_chunk,
        &registers[base], argc, depth, nullptr, out);
}

/* JIT trampoline for DIAMOND_OP_SUPER -- extracted from that case's own
 * body (below) so the interpreter and the JIT share one implementation
 * rather than risking the two drifting apart, exactly like DIAMOND_OP_
 * HASH's own diamond_jit_new_hash just above. Unlike every other
 * trampoline in this file, this one's own failure can be a *real,
 * already-happened* outcome -- an uncaught exception genuinely raised
 * somewhere inside the superclass method this calls, surfacing here as
 * DIAMOND_VM_EXCEPTION exactly the way invoke_resolved_method_helper's
 * own return already works for the interpreter -- not just an internal
 * condition safe to retry from scratch. jit.c's own compiler is written
 * to treat this trampoline's failure specially because of that (see
 * jc->has_called in jit.c): every bail site from the moment this compiles
 * onward propagates its exact returned status directly rather than
 * discarding the whole compiled attempt and re-running the function,
 * which -- now that a real call with real side effects can have already
 * happened -- would risk invoking this same super() call a second time. */
DiamondVmStatus diamond_jit_super_call(DiamondVm *vm, const DiamondChunk *chunk,
        uint16_t owner_index, uint16_t name, DiamondValue *registers, uint16_t base,
        uint8_t argc, size_t depth, DiamondValue *out) {
    if ((size_t)owner_index >= chunk->class_count ||
        (size_t)name >= chunk->string_count ||
        registers[0].kind != DIAMOND_VALUE_OBJECT ||
        registers[0].as.object->kind != DIAMOND_OBJECT_INSTANCE)
        return DIAMOND_VM_TYPE_ERROR;
    const DiamondClass *owner = chunk->classes[owner_index];
    if (owner->superclass == DIAMOND_NO_CLASS) return DIAMOND_VM_TYPE_ERROR;
    const DiamondStringConstant *method_name = &chunk->strings[name];
    const DiamondMethod *method = lookup_method(chunk,
        chunk->classes[owner->superclass], method_name->chars, method_name->length);
    if (method == nullptr) {
        /* No user-defined method anywhere up the superclass chain -- if
         * this is super(...) from an overridden initialize() reaching for
         * the built-in Exception constructor, apply that same behavior
         * here instead of treating the built-in as missing. */
        bool reaches_exception = false;
        const DiamondClass *ancestor = chunk->classes[owner->superclass];
        while (ancestor != nullptr) {
            if (ancestor == chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
                reaches_exception = true;
                break;
            }
            ancestor = ancestor->superclass == DIAMOND_NO_CLASS ? nullptr :
                chunk->classes[ancestor->superclass];
        }
        if (reaches_exception && method_name->length == 10 &&
            memcmp(method_name->chars, "initialize", 10) == 0) {
            if (argc > 2) return DIAMOND_VM_ARITY_ERROR;
            DiamondInstance *self = (DiamondInstance *)registers[0].as.object;
            if (argc > 0 && self->field_count > 0) self->fields[0] = registers[base];
            if (argc > 1 && self->field_count > 1) self->fields[1] = registers[(size_t)base + 1];
            if (!gc_write_barrier(vm, (DiamondObject *)self)) return DIAMOND_VM_OUT_OF_MEMORY;
            *out = DIAMOND_NIL;
            return DIAMOND_VM_OK;
        }
        return DIAMOND_VM_TYPE_ERROR;
    }
    if (argc < method->required_arity || (argc > method->arity && !method->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    return invoke_resolved_method_helper(vm, chunk, method, registers[0], registers, base, argc,
        false, 0, nullptr, chunk, depth, out);
}

/* The error for generic type arguments given to a method that takes none. */
static DiamondVmStatus reject_type_arguments(DiamondVm *vm,const DiamondStringConstant *method_name) {
    snprintf(vm->error,sizeof vm->error,
        "'%.*s' does not accept generic type arguments",
        (int)method_name->length,method_name->chars);
    return DIAMOND_VM_TYPE_ERROR;
}

/* Universal `tap(block)` on an instance: calls block with the receiver and
 * returns the receiver. */
__attribute__((noinline))
static DiamondVmStatus jit_instance_tap(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,size_t depth,
        DiamondValue *out) {
    if(argc!=1) return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondClosure *called=(DiamondClosure *)registers[base].as.object;
    /* See DIAMOND_OP_CALL_CLOSURE's own comment. */
    if(called->foreign_chunk!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "a compile_method callable can only be passed to define_method");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(called->function_index>=chunk->function_count)
        return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFunction *fn=chunk->functions[called->function_index];
    DiamondValue tap_argument[1]={registers[recv]};
    DiamondValue tap_result=DIAMOND_NIL;
    const DiamondVmStatus tap_status=call_closure_helper(vm,chunk,fn,
        called,tap_argument,0,1,depth,&tap_result);
    if(tap_status!=DIAMOND_VM_OK) return tap_status;
    *out=registers[recv];return DIAMOND_VM_OK;
}

/* No method by that name: method_missing if the class defines it, otherwise
 * the undefined-method error. */
__attribute__((noinline))
static DiamondVmStatus jit_instance_method_missing(DiamondVm *vm,const DiamondChunk *owner,
        DiamondInstance *instance,const DiamondStringConstant *method_name,
        DiamondValue *registers,uint16_t base,uint8_t argc,size_t depth,DiamondValue *out) {
    DiamondValue missing_result=DIAMOND_NIL;
    bool missing_found=false;
    const DiamondVmStatus missing_status=method_missing_helper(vm,owner,
        instance,method_name->chars,method_name->length,
        &registers[base],argc,depth,&missing_result,&missing_found);
    if(missing_found) {
        if(missing_status!=DIAMOND_VM_OK) return missing_status;
        *out=missing_result;return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,
        "undefined method '%.*s' for an instance of %s",
        (int)method_name->length,method_name->chars,instance->class->name);
    diamond_append_method_suggestion(vm,owner,DIAMOND_TYPE_NONE,nullptr,0,
        instance->class,method_name->chars,method_name->length);
    return DIAMOND_VM_NO_METHOD_ERROR;
}

/* A private method may only be called on an implicit self receiver; a
 * protected one only from inside its declaring class's hierarchy. */
__attribute__((noinline))
static DiamondVmStatus check_instance_method_visibility(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondChunk *owner,const DiamondInstance *instance,const DiamondMethod *method,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t recv) {
    if(method->is_private&&!(chunk->parameter_offset==1&&recv==0)) {
        snprintf(vm->error,sizeof vm->error,
            "private method '%.*s' called with an explicit receiver",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(method->is_protected) {
        const DiamondClass *declaring=method_declaring_class(
            owner,instance->class,method);
        const bool has_method_self=chunk->parameter_offset==1&&
            registers[0].kind==DIAMOND_VALUE_OBJECT&&
            registers[0].as.object->kind==DIAMOND_OBJECT_INSTANCE;
        const DiamondClass *caller_class=has_method_self?
            ((DiamondInstance *)registers[0].as.object)->class:nullptr;
        if(declaring==nullptr||caller_class==nullptr||
           !class_is_a(owner,caller_class,declaring)) {
            snprintf(vm->error,sizeof vm->error,
                "protected method '%.*s' called outside its class hierarchy",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    return DIAMOND_VM_OK;
}

/* JIT trampoline for DIAMOND_OP_INVOKE/INVOKE_MONO/INVOKE_TYPED dispatch on
 * an Instance receiver. The interpreter and JIT share this implementation;
 * recv remains a real register parameter rather than an implicit self.
 * `site` identifies the bytecode instruction for monomorphic quickening, as
 * in diamond_jit_get_ivar. Helpers propagate statuses explicitly because
 * VM_RETURN/VM_PROPAGATE only apply inside run_chunk. */
/* Whether `class` (or an ancestor) defines `name`: diamond_jit_invoke_instance's
 * universal-method checks (tap, to_s, dup, respond_to?, ...), each reached
 * only once the call's name matched. Out of line: inlined there, the method
 * walk measured as register spills on that function's hot path. */
__attribute__((noinline))
static bool class_defines(const DiamondChunk *owner, const DiamondClass *class,
        const char *name, size_t length) {
    return lookup_method(owner, class, name, length) != nullptr;
}

DiamondVmStatus diamond_jit_invoke_instance(DiamondVm *vm, const DiamondChunk *chunk,
        const uint8_t *site, DiamondOpCode instruction, DiamondValue *registers,
        uint16_t recv, uint16_t name, uint16_t base, uint8_t argc,
        uint8_t type_argument_count, const uint16_t *type_arguments,
        size_t depth, DiamondValue *out) {
    if ((size_t)name>=chunk->string_count) return DIAMOND_VM_TYPE_ERROR;
    const DiamondStringConstant *method_name=&chunk->strings[name];
    if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
       registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,registers[recv]);
        snprintf(vm->error,sizeof vm->error,
            "undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
    const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
    const DiamondMethod *method=nullptr;
    /* Warm monomorphic site: the cache only ever holds a method that
     * lookup_method found on this class, so the universal-method checks
     * below (each gated on lookup_method coming back empty) cannot apply.
     * The exception-field names are intercepted even when a class defines
     * them, so those keep the full path. */
    if(instruction==DIAMOND_OP_INVOKE_MONO&&
       !(method_name->length==7&&memcmp(method_name->chars,"message",7)==0)&&
       !(method_name->length==5&&memcmp(method_name->chars,"cause",5)==0)&&
       !(method_name->length==9&&memcmp(method_name->chars,"backtrace",9)==0)) {
        const size_t slot=diamond_cache_find(site,vm->method_caches,
            sizeof vm->method_caches[0]);
        if(slot!=SIZE_MAX&&vm->method_caches[slot].entry_count==1&&
           vm->method_caches[slot].entries[0].receiver_class==instance->class) {
            method=vm->method_caches[slot].entries[0].method;
            vm->inline_cache_hits++;
            vm->monomorphic_dispatches++;
            if(method!=nullptr)goto resolved;
        }
    }
    /* tap/dup/freeze/frozen?/respond_to?/public_send -- universal methods,
     * gated on lookup_method coming back empty first: a class CAN
     * legitimately define its own dup/tap/respond_to?, and that user
     * definition must win, mirroring run_chunk's own non-Instance-receiver
     * interception just above this case (dup_defined) for why interception
     * order matters here but not there. */
    if(method_name->length==3&&memcmp(method_name->chars,"tap",3)==0&&
       !class_defines(owner,instance->class,"tap",3)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        return jit_instance_tap(vm,chunk,registers,recv,base,argc,depth,out);
    }
    if(method_name->length==4&&memcmp(method_name->chars,"nil?",4)==0&&
       argc==0&&!class_defines(owner,instance->class,"nil?",4)) {
        *out=DIAMOND_BOOL(false);return DIAMOND_VM_OK;
    }
    /* Every object answers to_s(), with the text interpolation gives it,
     * unless its class defines its own. */
    if(method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0&&
       !class_defines(owner,instance->class,"to_s",4)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return stringify_value(vm,chunk,depth,registers[recv],out);
    }
    /* Every object answers inspect(): its fields, unless its class defines its
     * own inspect (any arity), which then dispatches normally. */
    if(method_name->length==7&&memcmp(method_name->chars,"inspect",7)==0&&
       !class_defines(owner,instance->class,"inspect",7)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return inspect_value(vm,chunk,depth,registers[recv],out);
    }
    if(method_name->length==3&&memcmp(method_name->chars,"dup",3)==0&&
       !class_defines(owner,instance->class,"dup",3)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_jit_dup(vm,&registers[recv],out);
    }
    if(method_name->length==6&&memcmp(method_name->chars,"freeze",6)==0&&
       !class_defines(owner,instance->class,"freeze",6)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_jit_freeze(vm,&registers[recv],out);
    }
    if(method_name->length==11&&memcmp(method_name->chars,"deep_freeze",11)==0&&
       !class_defines(owner,instance->class,"deep_freeze",11)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_deep_freeze(vm,&registers[recv],out);
    }
    if(method_name->length==7&&memcmp(method_name->chars,"frozen?",7)==0&&
       !class_defines(owner,instance->class,"frozen?",7)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_jit_frozen(vm,&registers[recv],out);
    }
    if(method_name->length==11&&
       memcmp(method_name->chars,"respond_to?",11)==0&&
       !class_defines(owner,instance->class,"respond_to?",11)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=1) return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_SYMBOL)
            return DIAMOND_VM_TYPE_ERROR;
        const DiamondSymbol *probe=(const DiamondSymbol *)registers[base].as.object;
        const DiamondMethod *probed=lookup_method(owner,instance->class,
            probe->chars,probe->length);
        *out=DIAMOND_BOOL(probed!=nullptr&&!probed->is_private);
        return DIAMOND_VM_OK;
    }
    if(method_name->length==11&&
       memcmp(method_name->chars,"public_send",11)==0&&
       !class_defines(owner,instance->class,"public_send",11)) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        DiamondValue sent=DIAMOND_NIL;
        const DiamondVmStatus send_status=public_send_helper(vm,chunk,
            registers[recv],&registers[base],argc,depth,&sent);
        if(send_status!=DIAMOND_VM_OK) return send_status;
        *out=sent;return DIAMOND_VM_OK;
    }
    const bool exception_instance=instance_is_exception(vm,instance);
    if(exception_instance&&method_name->length==7&&
       memcmp(method_name->chars,"message",7)==0) {
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        *out=instance->field_count>0?instance->fields[0]:DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    if(exception_instance&&method_name->length==5&&
       memcmp(method_name->chars,"cause",5)==0) {
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        *out=instance->field_count>1?instance->fields[1]:DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    if(exception_instance&&method_name->length==9&&
       memcmp(method_name->chars,"backtrace",9)==0) {
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        *out=instance->field_count>2?instance->fields[2]:DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    const size_t cache_slot=diamond_cache_find(site,vm->method_caches,
        sizeof vm->method_caches[0]);
    const DiamondMethodCache *cache=cache_slot==SIZE_MAX?nullptr:
        &vm->method_caches[cache_slot];
    if (instruction==DIAMOND_OP_INVOKE_MONO &&
        cache!=nullptr && cache->entry_count==1 &&
        cache->entries[0].receiver_class==instance->class) {
        method=cache->entries[0].method;
        vm->inline_cache_hits++;
        vm->monomorphic_dispatches++;
    } else {
        if (instruction==DIAMOND_OP_INVOKE_MONO) {
            *(uint8_t *)(void *)site=(uint8_t)DIAMOND_OP_INVOKE;
        }
        method=lookup_method_cached(vm,owner,site,instance->class,
            method_name->chars,method_name->length,&cache);
        if (instruction==DIAMOND_OP_INVOKE &&
            cache->entry_count==1 &&
            cache->hits>=vm->monomorphic_threshold) {
            if(record_rewritten_site(vm,site)) {
                *(uint8_t *)(void *)site=(uint8_t)DIAMOND_OP_INVOKE_MONO;
                vm->direct_dispatch_rewrites++;
            }
        }
    }
    if(method==nullptr)
        return jit_instance_method_missing(vm,owner,instance,method_name,registers,base,argc,
            depth,out);
resolved:
    if(method->is_private||method->is_protected) {
        const DiamondVmStatus visibility=check_instance_method_visibility(vm,chunk,owner,
            instance,method,method_name,registers,recv);
        if(visibility!=DIAMOND_VM_OK)return visibility;
    }
    if(argc<method->required_arity||
       (argc>method->arity && !method->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    return invoke_resolved_method_dispatch(vm,owner,
        method,registers[recv],registers,base,argc,
        instruction==DIAMOND_OP_INVOKE_TYPED,
        type_argument_count,type_arguments,chunk,depth,out);
}

#if defined(__x86_64__)
static_assert(sizeof(DiamondJitCallEntry) == 32, "jit.c indexes call entries by 32");
static_assert(offsetof(DiamondJitCallEntry, code) == 8, "jit.c reads code at 8");
static_assert(offsetof(DiamondJitCallEntry, chunk) == 16, "jit.c reads chunk at 16");
static_assert(offsetof(DiamondJitCallEntry, frame_bytes) == 24, "jit.c reads frame_bytes at 24");
static_assert(offsetof(DiamondJitCallEntry, accessor) == 28, "jit.c reads accessor at 28");
static_assert(offsetof(DiamondJitCallEntry, accessor_field) == 29, "jit.c reads the field at 29");
static_assert(offsetof(DiamondJitCallEntry, accessor_value_kind) == 30, "jit.c reads the kind at 30");
static_assert(sizeof(DiamondJitCallSite) == 136, "jit.c indexes call sites by 136");
#endif

/* The site's table entry, growing vm->jit_call_sites to hold it (new sites
 * zeroed: no classes, no failed fills). nullptr when out of memory. */
static DiamondJitCallSite *jit_call_site(DiamondVm *vm, uint32_t site_id) {
    if (site_id < vm->jit_call_site_count) return &vm->jit_call_sites[site_id];
    size_t count = vm->jit_call_site_count < 64 ? 64 : vm->jit_call_site_count * 2;
    while (count <= site_id) count *= 2;
    DiamondJitCallSite *grown = realloc(vm->jit_call_sites, count * sizeof *grown);
    if (grown == nullptr) return nullptr;
    memset(grown + vm->jit_call_site_count, 0,
        (count - vm->jit_call_site_count) * sizeof *grown);
    vm->jit_call_sites = grown;
    vm->jit_call_site_count = count;
    return &grown[site_id];
}

/* For a call site's fill: whether `fn` (not compiled yet) is compiled now.
 * An accessor or a trivial constructor is answered by a shortcut without a
 * call, so it never warms up through jit_call_core: it is counted and
 * compiled here instead. Anything else warms up as it runs, so *later says
 * to try again; it is false when the JIT turned `fn` down. */
static bool jit_compile_shortcut_target(DiamondVm *vm, const DiamondFunction *fn,
        bool *later) {
    *later = true;
    if (fn->code_count != 10 && fn->code_count != 15 && !fn->trivial_constructor)
        return false;
    DiamondFunction *mutable_fn = (DiamondFunction *)fn;
    if (++mutable_fn->jit_call_count < vm->jit_threshold) return false;
    jit_compile_function(vm, mutable_fn, &fn->call_chunk);
    if (fn->jit_code != nullptr) return true;
    *later = false;
    return false;
}

/* Marks a call entry whose method is a trivial accessor the call site can
 * answer itself (DiamondJitCallEntry): one invoke_resolved_method_dispatch
 * would hand to trivial_getter_result or trivial_setter_result, with no
 * declared type or one naming only Int or Float, which the site checks by
 * the value's kind as shortcut_type_matches does. */
static void jit_call_entry_accessor(const DiamondVm *vm, const DiamondFunction *fn,
        uint8_t argc, DiamondJitCallEntry *entry) {
    if (vm->debug_fd >= 0 || (fn->code_count != 10 && fn->code_count != 15)) return;
    const DiamondAccessor accessor = function_accessor(fn);
    if (accessor.kind != (argc == 0 ? DIAMOND_ACCESSOR_GETTER : DIAMOND_ACCESSOR_SETTER)) return;
    uint8_t value_kind = DIAMOND_JIT_ANY_VALUE_KIND;
    if (accessor.set != DIAMOND_NO_TYPE_SET) {
        const DiamondTypeSet *set = &fn->type_sets[accessor.set];
        if (set->count != 1 || set->members[0].argument_set != DIAMOND_NO_TYPE_SET ||
            set->members[0].second_argument_set != DIAMOND_NO_TYPE_SET)
            return;
        if (set->members[0].id == DIAMOND_TYPE_INT) value_kind = DIAMOND_VALUE_INT;
        else if (set->members[0].id == DIAMOND_TYPE_FLOAT) value_kind = DIAMOND_VALUE_FLOAT;
        else return;
    }
    entry->accessor = accessor.kind;
    entry->accessor_field = accessor.field;
    entry->accessor_value_kind = value_kind;
}

/* Whether diamond_jit_invoke_instance, given an Instance of `class` and this
 * site's name and argument count, ends in jit_call_or_interpret(fn,
 * &fn->call_chunk, [receiver, arguments...]) with fn compiled: then the
 * compiled code can make that call itself. Writes the entry on success.
 * Returns false for a reason that will not go away (the caller counts it),
 * and sets *later when the method could qualify once it is compiled. */
static bool jit_call_entry_for(DiamondVm *vm, const DiamondChunk *owner,
        const DiamondClass *class, const DiamondStringConstant *name, uint8_t argc,
        DiamondJitCallEntry *entry, bool *later) {
    *later = false;
    /* An exception's message, cause and backtrace are answered from its
     * fields even when its class defines them. */
    if ((name->length == 7 && memcmp(name->chars, "message", 7) == 0) ||
        (name->length == 5 && memcmp(name->chars, "cause", 5) == 0) ||
        (name->length == 9 && memcmp(name->chars, "backtrace", 9) == 0))
        return false;
    /* A class that does not define the method gets the universal ones
     * (dup, to_s, ...) or method_missing: C answers those. */
    const DiamondMethod *method = lookup_method(owner, class, name->chars, name->length);
    if (method == nullptr || method->is_private || method->is_protected ||
        method->has_variadic || method->bound_value_count != 0 ||
        argc < method->required_arity || argc > method->arity ||
        argc > DIAMOND_PLAIN_METHOD_ARGUMENTS)
        return false;
    const DiamondChunk *function_chunk =
        method->source_chunk != nullptr ? method->source_chunk : owner;
    if (method->function_index >= function_chunk->function_count) return false;
    const DiamondFunction *fn = function_chunk->functions[method->function_index];
    if (fn == nullptr || fn->call_chunk.functions != function_chunk->functions ||
        fn->call_chunk.entry_check != &fn->entry_check ||
        fn->register_count == 0 || fn->register_count > DIAMOND_JIT_MAX_REGISTERS ||
        fn->register_count < (size_t)argc + 1 || fn->jit_ineligible)
        return false;
    /* Range#to_a is answered natively for Int bounds. */
    if (argc == 0 && method->source_chunk == nullptr &&
        vm->range_class_index != DIAMOND_NO_CLASS &&
        (size_t)vm->range_class_index < owner->class_count &&
        class == owner->classes[vm->range_class_index])
        return false;
    if (fn->jit_code == nullptr && !jit_compile_shortcut_target(vm, fn, later)) return false;
    *entry = (DiamondJitCallEntry){
        .receiver_class = class,
        .code = fn->jit_code,
        .chunk = &fn->call_chunk,
        .frame_bytes = (uint32_t)(fn->register_count * sizeof(DiamondValue)),
        .accessor = DIAMOND_ACCESSOR_NONE,
    };
    jit_call_entry_accessor(vm, fn, argc, entry);
    return true;
}

void diamond_jit_call_site_fill(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *receiver, const uint8_t *site, uint32_t site_id) {
    if (vm->jit_depth_limit == 0) vm->jit_depth_limit = diamond_call_depth_limit(vm);
    DiamondJitCallSite *call_site = jit_call_site(vm, site_id);
    if (call_site == nullptr || call_site->failed_fills >= DIAMOND_JIT_CALL_FILL_TRIES) return;
    const uint16_t name = (uint16_t)(((unsigned)site[5] << 8) | site[6]);
    const uint8_t argc = site[9];
    if (receiver->kind != DIAMOND_VALUE_OBJECT ||
        receiver->as.object->kind != DIAMOND_OBJECT_INSTANCE ||
        (size_t)name >= chunk->string_count) {
        call_site->failed_fills++;
        return;
    }
    const DiamondInstance *instance = (const DiamondInstance *)receiver->as.object;
    const DiamondChunk *owner = instance->owner != nullptr ? instance->owner : vm->root_chunk;
    size_t way = 0;
    while (way < DIAMOND_JIT_CALL_WAYS &&
           call_site->entries[way].receiver_class != nullptr &&
           call_site->entries[way].receiver_class != instance->class)
        way++;
    DiamondJitCallEntry entry;
    bool later = false;
    if (way == DIAMOND_JIT_CALL_WAYS ||
        !jit_call_entry_for(vm, owner, instance->class, &chunk->strings[name], argc,
            &entry, &later)) {
        if (!later) call_site->failed_fills++;
        return;
    }
    call_site->entries[way] = entry;
}

/* Whether diamond_jit_new_instance, for `class_index` and `argc`, allocates
 * an Instance of a plain class and then either stops (no `initialize`, no
 * arguments) or ends in jit_call_or_interpret(fn, &fn->call_chunk,
 * [instance, arguments...]) with fn compiled; writes the entry (code nullptr
 * for no `initialize`). Returns false and sets *later as
 * jit_call_entry_for does. */
static bool jit_new_entry_for(DiamondVm *vm, const DiamondChunk *chunk,
        uint16_t class_index, uint8_t argc, DiamondJitCallEntry *entry, bool *later) {
    *later = false;
    if ((size_t)class_index >= chunk->class_count) return false;
    const DiamondClass *class = chunk->classes[class_index];
    if (class == nullptr || class->type_parameter[0] != '\0') return false;
    const DiamondMethod *init = lookup_method(chunk, class, "initialize",
        sizeof("initialize") - 1);
    if (init == nullptr) {
        /* An exception class takes up to two arguments into its fields. */
        for (const DiamondClass *ancestor = class; ancestor != nullptr;
             ancestor = ancestor->superclass == DIAMOND_NO_CLASS ? nullptr
                 : chunk->classes[ancestor->superclass])
            if (ancestor == chunk->classes[DIAMOND_CLASS_EXCEPTION]) return false;
        if (argc != 0) return false;
        *entry = (DiamondJitCallEntry){.receiver_class = class};
        return true;
    }
    if (init->has_variadic || init->bound_value_count != 0 ||
        argc < init->required_arity || argc > init->arity ||
        argc > DIAMOND_PLAIN_METHOD_ARGUMENTS)
        return false;
    const DiamondChunk *function_chunk =
        init->source_chunk != nullptr ? init->source_chunk : chunk;
    if (init->function_index >= function_chunk->function_count) return false;
    const DiamondFunction *fn = function_chunk->functions[init->function_index];
    if (fn == nullptr || fn->call_chunk.functions != function_chunk->functions ||
        fn->call_chunk.entry_check != &fn->entry_check ||
        fn->register_count == 0 || fn->register_count > DIAMOND_JIT_MAX_REGISTERS ||
        fn->register_count < (size_t)argc + 1 || fn->jit_ineligible)
        return false;
    if (fn->jit_code == nullptr && !jit_compile_shortcut_target(vm, fn, later)) return false;
    *entry = (DiamondJitCallEntry){
        .receiver_class = class,
        .code = fn->jit_code,
        .chunk = &fn->call_chunk,
        .frame_bytes = (uint32_t)(fn->register_count * sizeof(DiamondValue)),
    };
    return true;
}

void diamond_jit_new_site_fill(DiamondVm *vm, const DiamondChunk *chunk,
        uint32_t site_id, uint16_t class_index, uint8_t argc) {
    if (vm->jit_depth_limit == 0) vm->jit_depth_limit = diamond_call_depth_limit(vm);
    DiamondJitCallSite *call_site = jit_call_site(vm, site_id);
    if (call_site == nullptr || call_site->failed_fills >= DIAMOND_JIT_CALL_FILL_TRIES) return;
    DiamondJitCallEntry entry;
    bool later = false;
    if (!jit_new_entry_for(vm, chunk, class_index, argc, &entry, &later)) {
        if (!later) call_site->failed_fills++;
        return;
    }
    call_site->entries[0] = entry;
}

void diamond_jit_equal_site_fill(DiamondVm *vm, uint32_t site_id, const DiamondValue *left) {
    DiamondJitCallSite *call_site = jit_call_site(vm, site_id);
    if (call_site == nullptr || call_site->failed_fills >= DIAMOND_JIT_CALL_FILL_TRIES) return;
    if (left->kind != DIAMOND_VALUE_OBJECT || left->as.object->kind != DIAMOND_OBJECT_INSTANCE) {
        call_site->failed_fills++;
        return;
    }
    const DiamondInstance *instance = (const DiamondInstance *)left->as.object;
    const DiamondChunk *owner = instance->owner != nullptr ? instance->owner : vm->root_chunk;
    size_t way = 0;
    while (way < DIAMOND_JIT_CALL_WAYS &&
           call_site->entries[way].receiver_class != nullptr &&
           call_site->entries[way].receiver_class != instance->class)
        way++;
    /* invoke_operator_method's lookup: a class that answers `==` runs it. */
    if (way == DIAMOND_JIT_CALL_WAYS ||
        lookup_method(owner, instance->class, "==", 2) != nullptr) {
        call_site->failed_fills++;
        return;
    }
    call_site->entries[way] = (DiamondJitCallEntry){.receiver_class = instance->class};
}

DiamondInstance *diamond_jit_new_object(DiamondVm *vm, const DiamondClass *class) {
    return allocate_instance(vm, class, nullptr);
}

DiamondVmStatus diamond_jit_method_call_finish(DiamondVm *vm, const DiamondChunk *chunk,
        const DiamondValue *registers, uint64_t operands, size_t depth, DiamondValue *out,
        uint8_t status) {
    if (status == DIAMOND_JIT_TAIL) return jit_tail_loop(vm, chunk, depth, out);
    if (status == DIAMOND_JIT_RETRY) {
        vm->jit_bailouts++;
        const uint16_t recv = (uint16_t)operands;
        const uint16_t base = (uint16_t)(operands >> 16);
        const uint8_t argc = (uint8_t)(operands >> 32);
        DiamondValue args[DIAMOND_PLAIN_METHOD_ARGUMENTS + 1];
        args[0] = registers[recv];
        for (size_t i = 0; i < argc; i++) args[i + 1] = registers[(size_t)base + i];
        return run_chunk(chunk, vm, args, (size_t)argc + 1, depth + 1, nullptr, out);
    }
    vm->jit_hard_propagations++;
    return (DiamondVmStatus)status;
}

/* What a native loop's block call is passed, rebuilt for a retry through
 * the interpreter: [nil self slot,] then one or two arguments. */
static size_t jit_loop_arguments(const DiamondJitLoop *loop, DiamondValue *arguments) {
    size_t count = 0;
    if (loop->self_slot) arguments[count++] = DIAMOND_NIL;
    switch ((DiamondJitLoopKind)loop->kind) {
    case DIAMOND_JIT_LOOP_EACH_WITH_INDEX:
        arguments[count++] = loop->current;
        arguments[count++] = DIAMOND_INT((int64_t)loop->index);
        break;
    case DIAMOND_JIT_LOOP_REDUCE:
        arguments[count++] = loop->result;
        arguments[count++] = loop->current;
        break;
    default:
        arguments[count++] = loop->current;
        break;
    }
    return count;
}

/* Whether the loop holds protected values that every way out releases. */
static bool jit_loop_protects(const DiamondJitLoop *loop) {
    return loop->kind == DIAMOND_JIT_LOOP_MAP || loop->kind == DIAMOND_JIT_LOOP_SELECT ||
           loop->kind == DIAMOND_JIT_LOOP_REDUCE;
}

DiamondVmStatus diamond_jit_loop_step(DiamondVm *vm, DiamondJitLoop *loop, uint8_t status,
        size_t depth) {
    DiamondVmStatus finished = (DiamondVmStatus)status;
    if (status == DIAMOND_JIT_TAIL) {
        finished = jit_tail_loop(vm, loop->child, depth, &loop->result);
    } else if (status == DIAMOND_JIT_RETRY) {
        vm->jit_bailouts++;
        DiamondValue arguments[3];
        const size_t count = jit_loop_arguments(loop, arguments);
        finished = run_chunk(loop->child, vm, arguments, count, depth + 1, loop->closure,
            &loop->result);
    } else if (status != DIAMOND_VM_OK) {
        vm->jit_hard_propagations++;
    }
    if (finished == DIAMOND_VM_OK) {
        if (loop->kind == DIAMOND_JIT_LOOP_MAP) {
            if (!array_push(vm, loop->collected, loop->result))
                finished = DIAMOND_VM_OUT_OF_MEMORY;
        } else if (loop->kind == DIAMOND_JIT_LOOP_SELECT) {
            if (is_truthy(loop->result) && !array_push(vm, loop->collected, loop->current))
                finished = DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    if (finished != DIAMOND_VM_OK) {
        if (jit_loop_protects(loop)) gc_unprotect(vm, loop->gc_mark);
        return finished;
    }
    return DIAMOND_VM_OK;
}

void diamond_jit_loop_end(DiamondVm *vm, DiamondJitLoop *loop, DiamondValue *out) {
    if (jit_loop_protects(loop)) gc_unprotect(vm, loop->gc_mark);
    switch ((DiamondJitLoopKind)loop->kind) {
    case DIAMOND_JIT_LOOP_MAP:
    case DIAMOND_JIT_LOOP_SELECT:
        *out = DIAMOND_OBJECT(loop->collected);
        break;
    case DIAMOND_JIT_LOOP_REDUCE:
        *out = loop->result;
        break;
    case DIAMOND_JIT_LOOP_TIMES:
        *out = DIAMOND_INT(loop->limit);
        break;
    default:
        *out = DIAMOND_OBJECT((DiamondArray *)loop->array);
        break;
    }
}

DiamondVmStatus diamond_jit_native_call(DiamondVm *vm,
        const DiamondValue *receiver,const DiamondValue *argument,
        DiamondJitNativeReadOp operation,DiamondValue *out) {
    const bool string_operation=operation==DIAMOND_JIT_NATIVE_STRING_LENGTH||
        operation==DIAMOND_JIT_NATIVE_STRING_INDEX_OF||
        operation==DIAMOND_JIT_NATIVE_STRING_ORD||
        operation==DIAMOND_JIT_NATIVE_STRING_SLICE;
    const bool array_operation=operation==DIAMOND_JIT_NATIVE_ARRAY_LENGTH;
    if(receiver->kind!=DIAMOND_VALUE_OBJECT||
       receiver->as.object->kind!=(string_operation?DIAMOND_OBJECT_STRING:
           (array_operation?DIAMOND_OBJECT_ARRAY:DIAMOND_OBJECT_HASH)))
        return DIAMOND_VM_TYPE_ERROR;
    if(string_operation) {
        const DiamondString *source=(const DiamondString *)receiver->as.object;
        if(operation==DIAMOND_JIT_NATIVE_STRING_LENGTH) {
            *out=DIAMOND_INT((int64_t)source->length);return DIAMOND_VM_OK;
        }
        if(operation==DIAMOND_JIT_NATIVE_STRING_ORD) {
            if(source->length==0) {
                snprintf(vm->error,sizeof vm->error,
                    "cannot take ord of an empty String");
                return DIAMOND_VM_INDEX_ERROR;
            }
            *out=DIAMOND_INT((unsigned char)source->chars[0]);
            return DIAMOND_VM_OK;
        }
        if(operation==DIAMOND_JIT_NATIVE_STRING_SLICE) {
            if(argument[0].kind!=DIAMOND_VALUE_INT||
               argument[1].kind!=DIAMOND_VALUE_INT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#slice arguments must be Int");
                return DIAMOND_VM_TYPE_ERROR;
            }
            const int64_t start=argument[0].as.integer;
            const int64_t requested_length=argument[1].as.integer;
            if(start<0||(uint64_t)start>source->length||requested_length<0) {
                snprintf(vm->error,sizeof vm->error,
                    "index %" PRId64 " out of bounds for String of length %zu",
                    start,source->length);
                return DIAMOND_VM_INDEX_ERROR;
            }
            const size_t available=source->length-(size_t)start;
            const size_t length=(uint64_t)requested_length>available?
                available:(size_t)requested_length;
            DiamondString *slice=allocate_string(vm,source->chars+(size_t)start,length);
            if(slice==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            *out=DIAMOND_OBJECT(slice);return DIAMOND_VM_OK;
        }
        if(argument->kind!=DIAMOND_VALUE_OBJECT||
           argument->as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "String#index_of argument must be a String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *needle=(const DiamondString *)argument->as.object;
        *out=DIAMOND_NIL;
        if(needle->length==0) {*out=DIAMOND_INT(0);return DIAMOND_VM_OK;}
        if(needle->length<=source->length)
            for(size_t start=0;start+needle->length<=source->length;start++)
                if(memcmp(source->chars+start,needle->chars,needle->length)==0) {
                    *out=DIAMOND_INT((int64_t)start);break;
                }
        return DIAMOND_VM_OK;
    }
    if(operation==DIAMOND_JIT_NATIVE_ARRAY_LENGTH) {
        *out=DIAMOND_INT((int64_t)((const DiamondArray *)receiver->as.object)->count);
        return DIAMOND_VM_OK;
    }
    const DiamondHash *hash=(const DiamondHash *)receiver->as.object;
    if(operation==DIAMOND_JIT_NATIVE_HASH_LENGTH) {
        *out=DIAMOND_INT((int64_t)hash->count);
        return DIAMOND_VM_OK;
    }
    if(argument->kind!=DIAMOND_VALUE_INT)return DIAMOND_VM_TYPE_ERROR;
    const int64_t index=argument->as.integer;
    if(index<0||(uint64_t)index>=hash->count) {
        snprintf(vm->error,sizeof vm->error,
            "index %" PRId64 " out of bounds for Hash of length %zu",
            index,hash->count);
        return DIAMOND_VM_INDEX_ERROR;
    }
    const DiamondHashEntry entry=hash->entries[(size_t)index];
    *out=operation==DIAMOND_JIT_NATIVE_HASH_KEY_AT?entry.key:entry.value;
    return DIAMOND_VM_OK;
}

static bool type_set_mentions_variable(const DiamondTypeSet *sets,uint16_t set,
        size_t variable,size_t depth) {
    if(set==DIAMOND_NO_TYPE_SET||depth>=DIAMOND_BOUND_TYPE_NODES)return false;
    for(size_t index=0;index<sets[set].count;index++) {
        const DiamondTypeMember member=sets[set].members[index];
        if(member.id==DIAMOND_TYPE_VARIABLE_BASE+variable||
           type_set_mentions_variable(sets,member.argument_set,variable,depth+1)||
           type_set_mentions_variable(sets,member.second_argument_set,variable,depth+1)||
           (member.id==DIAMOND_TYPE_CALLABLE&&
            type_set_mentions_variable(sets,member.callable_return_set,variable,depth+1)))
            return true;
    }
    return false;
}

/* Whether an argument leaves a class variable open rather than failing to say
 * anything about it: a callable with no declared return can't show what it
 * will produce, and a generic instance may itself have been left open. */
static bool argument_leaves_variable_open(const DiamondChunk *chunk,DiamondValue value,
        const DiamondTypeSet *sets,uint16_t set,size_t variable) {
    for(size_t index=0;index<sets[set].count;index++) {
        const DiamondTypeMember member=sets[set].members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE||!value_matches_type(chunk,value,member.id))continue;
        if(member.id==DIAMOND_TYPE_CALLABLE) {
            const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
            if(type_set_mentions_variable(sets,member.callable_return_set,variable,0)&&
               (closure->foreign_chunk!=nullptr||closure->function_index>=chunk->function_count||
                chunk->functions[closure->function_index]->return_type_set==DIAMOND_NO_TYPE_SET))
                return true;
        } else if(member.id>=DIAMOND_TYPE_CLASS_BASE&&member.id<DIAMOND_TYPE_CLASS_LIMIT) {
            const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
            DiamondTypeBinding scratch[2];
            const DiamondTypeBinding *projected=instance_bindings_as(chunk,instance,
                member.id-DIAMOND_TYPE_CLASS_BASE,scratch);
            if(projected==nullptr)continue;
            if((projected[0].node_count==0&&
                type_set_mentions_variable(sets,member.argument_set,variable,0))||
               (member.second_argument_set!=DIAMOND_NO_TYPE_SET&&projected[1].node_count==0&&
                type_set_mentions_variable(sets,member.second_argument_set,variable,0)))
                return true;
        }
    }
    return false;
}

static bool constructor_leaves_variable_open(const DiamondChunk *chunk,const DiamondFunction *fn,
        const DiamondClass *class,const DiamondValue *arguments,size_t count,size_t variable) {
    /* A transformed parent (`Sub[T] < Base[Array[T]]`) has no view of an open
     * variable, so its inherited methods could not be entered. */
    for(const DiamondClass *ancestor=class;ancestor!=nullptr;
        ancestor=ancestor->superclass<chunk->class_count?chunk->classes[ancestor->superclass]:nullptr)
        if(ancestor->transforms_parent_arguments)return false;
    for(size_t parameter=0;parameter<count&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++) {
        const uint16_t set=fn->parameter_type_sets[parameter];
        if(arguments[parameter].kind!=DIAMOND_VALUE_UNDEFINED&&
           set!=DIAMOND_NO_TYPE_SET&&set<fn->type_set_count&&
           argument_leaves_variable_open(chunk,arguments[parameter],fn->type_sets,set,variable))
            return true;
    }
    return false;
}

/* Keep the inference scratch graph out of ordinary constructor frames. */
__attribute__((noinline))
DiamondVmStatus bind_instance_constructor_type(DiamondVm *vm,
        const DiamondChunk *chunk,const DiamondFunction *fn,
        const DiamondValue *arguments,size_t count,DiamondInstance *instance) {
    if(instance->class->fixed_type_arguments)return DIAMOND_VM_OK;
    if(!fn->class_type_variable||fn->class_type_variable>fn->type_variable_count||
       fn->type_variable_count>8||
       fn->class_type_variable>2)
        return DIAMOND_VM_TYPE_ERROR;
    /* Only the function's own variables are read, as at a generic call's entry. */
    DiamondTypeBinding bindings[8];
    memset(bindings,0,fn->type_variable_count*sizeof bindings[0]);
    for(size_t parameter=0;parameter<count&&
        parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++) {
        if(arguments[parameter].kind==DIAMOND_VALUE_UNDEFINED)continue;
        const uint16_t set=fn->parameter_type_sets[parameter];
        if(set!=DIAMOND_NO_TYPE_SET&&set<fn->type_set_count)
            infer_from_value(chunk,arguments[parameter],fn->type_sets,set,bindings);
    }
    const bool own_constructor=instance->class->index==fn->owner_class;
    if(!own_constructor&&!diamond_seed_fixed_parent_bindings(chunk->classes,chunk->class_count,
        instance->class->index,fn->owner_class,bindings))return DIAMOND_VM_TYPE_ERROR;
    for(size_t variable=0;variable<fn->class_type_variable;variable++)
    if(bindings[variable].invalid||bindings[variable].node_count==0||bindings[variable].nodes[0].count==0) {
        if(!bindings[variable].invalid&&
           constructor_leaves_variable_open(chunk,fn,instance->class,arguments,count,variable)) {
            bindings[variable]=(DiamondTypeBinding){};
            continue;
        }
        snprintf(vm->error,sizeof vm->error,
            "cannot infer generic class type parameter from constructor arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(own_constructor) {
        for(size_t variable=0;variable<fn->class_type_variable;variable++)
            instance->type_binding[variable]=bindings[variable];
        return DIAMOND_VM_OK;
    }
    DiamondTypeBinding actual[2]={};
    if(!diamond_map_class_bindings(chunk->classes,chunk->class_count,instance->class->index,
        fn->owner_class,bindings,actual,true)) {
        snprintf(vm->error,sizeof vm->error,
            "cannot infer generic class type parameter from constructor arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    for(size_t variable=0;variable<DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class);variable++)
        instance->type_binding[variable]=actual[variable];
    return DIAMOND_VM_OK;
}

/* Phase 10 (docs/internal/jit-design.md): the interpreter's own plain
 * DIAMOND_OP_NEW case, extracted verbatim -- allocate, run `initialize` if
 * the class defines one (arity-checked exactly as the interpreter does),
 * else the Exception-subclass two-field fallback, else a bare argc==0
 * requirement. Writes registers[dest] directly rather than through a
 * separate `out` pointer (unlike diamond_jit_invoke_instance) since dest
 * is already a real index into the same registers array both the
 * interpreter and the JIT's own JIT_REGISTERS_BASE share.
 * DIAMOND_OP_NEW_KEYWORDS/DIAMOND_OP_NEW_SPREAD (synthetic-chunk re-entry
 * into run_chunk, src/vm.c below) are deliberately NOT covered here --
 * compile_body has no case for either, so a function containing one still
 * bails whole, same as any other unsupported construct. */
DiamondVmStatus diamond_jit_new_instance(DiamondVm *vm, const DiamondChunk *chunk,
        DiamondValue *registers, uint16_t dest, uint16_t class_index,
        uint16_t base, uint8_t argc, size_t depth) {
    if((size_t)class_index>=chunk->class_count) return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondClass *class=chunk->classes[class_index];
    DiamondInstance *instance=allocate_instance(vm,class,nullptr);
    if(instance==nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(instance);
    const DiamondMethod *init=lookup_method(
        chunk,class,"initialize",sizeof("initialize")-1);
    if(instance->type_binding!=nullptr&&init==nullptr&&!class->fixed_type_arguments)return DIAMOND_VM_TYPE_ERROR;
    if(init!=nullptr) {
        if(argc<init->required_arity||
           (argc>init->arity && !init->has_variadic))
            return DIAMOND_VM_ARITY_ERROR;
        if(instance->type_binding!=nullptr) {
            const DiamondChunk *owner=init->source_chunk!=nullptr?init->source_chunk:chunk;
            const DiamondFunction *fn=owner->functions[init->function_index];
            const DiamondVmStatus bound=bind_instance_constructor_type(vm,owner,fn,
                &registers[base],argc,instance);
            if(bound!=DIAMOND_VM_OK)return bound;
        } else if(init->bound_value_count==0&&vm->debug_fd<0) {
            /* An `initialize` that only copies its arguments into fields
             * stores them here, without a frame. */
            const DiamondChunk *owner=init->source_chunk!=nullptr?init->source_chunk:chunk;
            const DiamondFunction *fn=owner->functions[init->function_index];
            DiamondVmStatus stored;
            if(fn->trivial_constructor&&fn->entry_check.computed&&
               (size_t)argc+1==fn->arity&&
               trivial_constructor_result(vm,owner,fn,registers[dest],
                   &registers[base],&stored))
                return stored;
        }
        DiamondValue ignored=DIAMOND_NIL;
        return invoke_resolved_method_helper(vm,chunk,
            init,registers[dest],registers,base,argc,false,0,nullptr,
            chunk,depth,&ignored);
    }
    bool exception_class=false;const DiamondClass *ancestor=class;
    while(ancestor!=nullptr) {
        if(ancestor==chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
            exception_class=true;break;
        }
        ancestor=ancestor->superclass==DIAMOND_NO_CLASS?nullptr:
            chunk->classes[ancestor->superclass];
    }
    if(exception_class) {
        if(argc>2) return DIAMOND_VM_ARITY_ERROR;
        if(argc>0)instance->fields[0]=registers[base];
        if(argc>1)instance->fields[1]=registers[(size_t)base+1];
    } else if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
    return DIAMOND_VM_OK;
}

DiamondVmStatus call_closure_spread_helper(DiamondVm *vm,
        const DiamondChunk *chunk,const DiamondFunction *fn,
        const DiamondClosure *called,const DiamondArray *spread,size_t depth,
        DiamondValue *result) {
    const bool needs_self_slot=fn->owner_class!=DIAMOND_OWNER_NONE;
    const bool self_via_capture=fn->owner_class==DIAMOND_OWNER_CAPTURED_SELF;
    const size_t arity_count=self_via_capture?spread->count:
        spread->count+(needs_self_slot?1:0);
    if(arity_count<fn->required_arity||
       (arity_count>fn->arity&&!fn->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    const DiamondValue *arguments=spread->values;
    size_t argument_count=spread->count;
    DiamondValue *padded=nullptr;
    if(needs_self_slot) {
        if(spread->count>DIAMOND_REGISTER_COUNT-1)
            return DIAMOND_VM_ARITY_ERROR;
        padded=malloc((spread->count+1)*sizeof *padded);
        if(padded==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        padded[0]=DIAMOND_NIL;
        for(size_t index=0;index<spread->count;index++)
            padded[index+1]=spread->values[index];
        arguments=padded;argument_count++;
    }
    DiamondChunk child_storage;
    const DiamondChunk *child=function_call_chunk(chunk,fn,&child_storage);
    const DiamondVmStatus status=run_chunk(child,vm,arguments,argument_count,
        depth+1,called,result);
    free(padded);return status;
}

/* Snapshots the live call-stack chain (vm->frames) into a backtrace Array
 * of "chunk:line:column" Strings, attached to the raised Exception
 * instance's own hidden `backtrace` field (its 3rd reserved field, after
 * message/cause -- see diamond_program_init). Called unconditionally at
 * DIAMOND_OP_RAISE, not lazily from Exception#backtrace itself: by the
 * time a rescue clause later reads #backtrace, the deeper frames that
 * were live at the raise site are long gone from vm->frames, so the only
 * point that can see them is the raise itself. Best-effort -- an
 * allocation failure here just truncates the backtrace early rather than
 * failing the raise. */
/* Appends one "chunk:line:column" String per live frame (innermost first).
 * Best-effort: stops early on allocation failure. */
void backtrace_append_live_frames(DiamondVm *vm,DiamondArray *backtrace) {
    for(const DiamondFrame *frame=vm->frames;frame!=nullptr;frame=frame->previous) {
        if(frame->chunk==nullptr||frame->instruction_offset==nullptr)continue;
        const char *name=frame->chunk->name!=nullptr?frame->chunk->name:"<chunk>";
        const size_t offset=*frame->instruction_offset;
        const bool in_bounds=offset<frame->chunk->code_count;
        const uint32_t line=in_bounds&&frame->chunk->lines!=nullptr?
            frame->chunk->lines[offset]:0;
        const uint32_t column=in_bounds&&frame->chunk->columns!=nullptr?
            frame->chunk->columns[offset]:0;
        /* "name:line:column", capped at 255 bytes as snprintf into this
         * buffer used to be; built by hand since every raise pays for it. */
        char text[256+48];
        size_t length=strnlen(name,255);
        memcpy(text,name,length);
        text[length++]=':';
        length+=format_int_decimal(text+length,line);
        text[length++]=':';
        length+=format_int_decimal(text+length,column);
        if(length>255)length=255;
        DiamondString *entry=allocate_string(vm,text,length);
        if(entry==nullptr)return;
        if(!array_push(vm,backtrace,DIAMOND_OBJECT(entry)))return;
    }
}

/* Installs a fresh, empty backtrace Array on `raised` (rooted through
 * vm->exception, which the caller has already set) and returns it, or
 * nullptr when `raised` has no backtrace field or allocation fails. */
DiamondArray *install_backtrace(DiamondVm *vm,DiamondInstance *raised) {
    if(!instance_is_exception(vm,raised)||raised->field_count<=2)return nullptr;
    DiamondArray *backtrace=allocate_array(vm,nullptr,0);
    if(backtrace==nullptr)return nullptr;
    raised->fields[2]=DIAMOND_OBJECT(backtrace);
    /* allocate_array above can itself have triggered a minor collection
     * that promoted `raised` (reachable via vm->exception) before this
     * assignment ran -- this raw field write bypasses DIAMOND_OP_SET_IVAR
     * entirely, so it needs its own barrier call rather than relying on
     * the opcode-level one. */
    if(!gc_write_barrier(vm,(DiamondObject *)raised))return nullptr;
    return backtrace;
}

/* Snapshots the live call-stack chain (vm->frames) into a backtrace Array
 * of "chunk:line:column" Strings, attached to the raised Exception
 * instance's own hidden `backtrace` field (its 3rd reserved field, after
 * message/cause -- see diamond_program_init). Called at DIAMOND_OP_RAISE,
 * not lazily from Exception#backtrace itself: by the time a rescue clause
 * later reads #backtrace, the deeper frames that were live at the raise
 * site are long gone from vm->frames, so the only point that can see them
 * is the raise itself. Only the first raise records one: re-raising an
 * exception -- explicitly, or when no rescue clause matched -- keeps where
 * it came from, as in Ruby. Returns whether this raise was the first. */
bool raise_capture_backtrace_helper(DiamondVm *vm,const DiamondChunk *chunk) {
    /* See invoke_operator_method's own comment on this same pattern. */
    (void)chunk;
    if(vm->exception.kind!=DIAMOND_VALUE_OBJECT||
       vm->exception.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return true;
    DiamondInstance *raised=(DiamondInstance *)vm->exception.as.object;
    if(raised->field_count>2&&raised->fields[2].kind==DIAMOND_VALUE_OBJECT&&
       raised->fields[2].as.object->kind==DIAMOND_OBJECT_ARRAY)return false;
    DiamondArray *backtrace=install_backtrace(vm,raised);
    if(backtrace!=nullptr)backtrace_append_live_frames(vm,backtrace);
    return true;
}

/* Stable public names for every parameter-bearing native receiver method.
 * Arity/default enforcement deliberately remains in the existing INVOKE
 * branches; this table only maps keyword names to their positional slots. */
static const NativeKeywordSignature native_keyword_signatures[]={
    {"tap",{"callback"},1},{"times",{"callback"},1},
    {"upto",{"limit","callback"},2},{"downto",{"limit","callback"},2},
    {"join",{"separator"},1},{"each",{"callback"},1},
    {"select",{"callback"},1},{"count",{"callback"},1},
    {"any?",{"callback"},1},{"all?",{"callback"},1},
    {"reduce",{"initial","callback"},2},{"map",{"callback"},1},
    {"reject",{"callback"},1},{"find",{"callback"},1},
    {"each_with_index",{"callback"},1},{"sort_by",{"callback"},1},
    {"min_by",{"callback"},1},{"max_by",{"callback"},1},
    {"take",{"n"},1},{"drop",{"n"},1},{"flat_map",{"callback"},1},
    {"partition",{"callback"},1},{"group_by",{"callback"},1},
    {"zip",{"other"},1},{"each_slice",{"size"},1},
    {"each_cons",{"size"},1},{"first_or",{"fallback"},1},
    {"last_or",{"fallback"},1},{"include?",{"needle"},1},
    {"concat",{"other"},1},{"delete_at",{"index"},1},
    {"fetch",{"key","fallback"},2},{"include_key?",{"needle"},1},
    {"map_values",{"callback"},1},{"merge",{"other"},1},
    {"key_at",{"index"},1},{"value_at",{"index"},1},{"delete",{"key"},1},
    {"index_of",{"needle"},1},{"slice",{"start","length"},2},
    {"split",{"separator"},1},{"repeat",{"count"},1},
    {"gsub",{"pattern","replacement"},2},{"sub",{"pattern","replacement"},2},
    {"scan",{"pattern"},1},{"getbyte",{"index"},1},{"start_with?",{"prefix"},1},
    {"end_with?",{"suffix"},1},{"ljust",{"width","padding"},2},
    {"rjust",{"width","padding"},2},{"tr",{"from","to"},2},
    {"format",{"values"},1},{"push",{"value"},1},
    {"resume",{"value"},1},{"read",{"length"},1},
    {"write",{"value"},1},{"send",{"value","host","port"},3},
    {"receive",{"length"},1},{"match",{"value"},1},
    {"match?",{"value"},1},{"respond_to?",{"name"},1}
};

const NativeKeywordSignature *native_keyword_signature(
        const DiamondStringConstant *method) {
    for(size_t index=0;index<sizeof native_keyword_signatures/
            sizeof native_keyword_signatures[0];index++) {
        const NativeKeywordSignature *signature=&native_keyword_signatures[index];
        if(strlen(signature->method)==method->length&&
           memcmp(signature->method,method->chars,method->length)==0)
            return signature;
    }
    return nullptr;
}

/* `receiver.public_send(name, *arguments)` re-enters the ordinary dynamic
 * INVOKE_SPREAD path with `name` as that synthetic call site's method-name
 * constant. Keeping the actual dispatch there means native receivers, user
 * classes, inheritance, variadics, runtime-installed methods, and
 * method_missing retain one implementation. The synthetic chunk explicitly
 * has no method self (`parameter_offset=0`): public_send must never inherit
 * the caller's private/protected access, even when called from inside the
 * target's own class hierarchy. */
DiamondVmStatus public_send_helper(DiamondVm *vm,
        const DiamondChunk *chunk,DiamondValue receiver,
        const DiamondValue *arguments,size_t argument_count,size_t depth,
        DiamondValue *result) {
    if(argument_count==0)return DIAMOND_VM_ARITY_ERROR;
    const DiamondValue name_value=arguments[0];
    if(name_value.kind!=DIAMOND_VALUE_OBJECT||
       (name_value.as.object->kind!=DIAMOND_OBJECT_SYMBOL&&
        name_value.as.object->kind!=DIAMOND_OBJECT_STRING)) {
        snprintf(vm->error,sizeof vm->error,
            "public_send method name must be a Symbol or String");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const bool symbol_name=name_value.as.object->kind==DIAMOND_OBJECT_SYMBOL;
    const size_t name_length=symbol_name?
        ((const DiamondSymbol *)name_value.as.object)->length:
        ((const DiamondString *)name_value.as.object)->length;
    const char *name_chars=symbol_name?
        ((const DiamondSymbol *)name_value.as.object)->chars:
        ((const DiamondString *)name_value.as.object)->chars;
    if(name_length>DIAMOND_MAX_STRING_LENGTH) {
        snprintf(vm->error,sizeof vm->error,
            "public_send method name is too long (maximum %d)",DIAMOND_MAX_STRING_LENGTH);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondArray *forwarded=allocate_array(vm,arguments+1,argument_count-1);
    if(forwarded==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const size_t protected_count=vm->gc_protected_count;
    if(!gc_protect(vm,DIAMOND_OBJECT(forwarded)))
        return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondStringConstant *dynamic_name=malloc(sizeof *dynamic_name+name_length+1);
    if(dynamic_name==nullptr) {
        gc_unprotect(vm,protected_count);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    dynamic_name->chars=(char *)(dynamic_name+1);
    memcpy(dynamic_name->chars,name_chars,name_length);
    dynamic_name->chars[name_length]='\0';dynamic_name->length=name_length;
    uint8_t code[12]={DIAMOND_OP_INVOKE_SPREAD,
        0,2,0,0,0,0,0,1,DIAMOND_OP_RETURN,0,2};
    uint32_t locations[12]={0};
    DiamondChunk synthetic=*chunk;synthetic.class_type_variable=false;
    synthetic.name="<public_send>";
    synthetic.code=code;synthetic.lines=locations;synthetic.columns=locations;
    synthetic.code_count=sizeof code;synthetic.register_count=3;
    synthetic.strings=dynamic_name;synthetic.string_count=1;
    synthetic.parameter_offset=0;
    DiamondValue call_arguments[2]={receiver,DIAMOND_OBJECT(forwarded)};
    const DiamondVmStatus status=run_chunk(&synthetic,vm,call_arguments,2,
        depth+1,nullptr,result);
    static const char synthetic_frame[]="\n  at <public_send>:0:0";
    const size_t error_length=strlen(vm->error);
    const size_t frame_length=sizeof synthetic_frame-1;
    if(status!=DIAMOND_VM_OK&&error_length>=frame_length&&
       memcmp(vm->error+error_length-frame_length,synthetic_frame,
              frame_length)==0)
        vm->error[error_length-frame_length]='\0';
    free(dynamic_name);
    gc_unprotect(vm,protected_count);
    return status;
}

DiamondVmStatus merge_native_keyword_arguments(DiamondVm *vm,
        const DiamondChunk *caller,const NativeKeywordSignature *signature,
        const DiamondArray *positional,const uint16_t *keyword_names,
        const uint16_t *keyword_registers,size_t keyword_count,
        const DiamondValue *registers,DiamondValue **merged,size_t *merged_count) {
    if(positional->count>signature->parameter_count)return DIAMOND_VM_ARITY_ERROR;
    bool filled[3]={0};size_t slots[16];size_t count=positional->count;
    for(size_t index=0;index<count;index++)filled[index]=true;
    for(size_t keyword=0;keyword<keyword_count;keyword++) {
        if((size_t)keyword_names[keyword]>=caller->string_count)
            return DIAMOND_VM_INVALID_BYTECODE;
        const DiamondStringConstant *name=&caller->strings[keyword_names[keyword]];
        size_t slot=SIZE_MAX;
        for(size_t index=0;index<signature->parameter_count;index++)
            if(strlen(signature->parameters[index])==name->length&&
               memcmp(signature->parameters[index],name->chars,name->length)==0) {
                slot=index;break;
            }
        if(slot==SIZE_MAX) {snprintf(vm->error,sizeof vm->error,
            "no parameter with this name");return DIAMOND_VM_ARITY_ERROR;}
        if(filled[slot]) {snprintf(vm->error,sizeof vm->error,
            "multiple values for the same argument");return DIAMOND_VM_ARITY_ERROR;}
        slots[keyword]=slot;filled[slot]=true;if(slot+1>count)count=slot+1;
    }
    for(size_t index=0;index<count;index++)if(!filled[index]) {
        snprintf(vm->error,sizeof vm->error,"missing argument");
        return DIAMOND_VM_ARITY_ERROR;
    }
    DiamondValue *values=malloc(count*sizeof *values);
    if(values==nullptr&&count>0)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<positional->count;index++)values[index]=positional->values[index];
    for(size_t keyword=0;keyword<keyword_count;keyword++)
        values[slots[keyword]]=registers[keyword_registers[keyword]];
    *merged=values;*merged_count=count;return DIAMOND_VM_OK;
}

DiamondVmStatus merge_keyword_arguments(DiamondVm *vm,
        const DiamondChunk *caller,const DiamondFunction *function,
        const DiamondArray *positional,const uint16_t *keyword_names,
        const uint16_t *keyword_registers,size_t keyword_count,
        const DiamondValue *registers,size_t public_arity,
        const DiamondValue *block,
        DiamondValue **merged,size_t *merged_count) {
    /* positional->count could exceed DIAMOND_MAX_DECLARED_PARAMETERS for a
     * variadic target reached via a large spread combined with keyword
     * overrides -- narrower than DIAMOND_MAX_ARGUMENTS's own general
     * ceiling in that one specific combination, a deliberate scope cut
     * (see docs/design.md) rather than an oversight: keyword names only
     * ever resolve against a *declared* parameter slot, so widening this
     * further would need a differently-shaped fix, not just a bigger
     * buffer. */
    if(positional->count>DIAMOND_MAX_DECLARED_PARAMETERS||
       public_arity>DIAMOND_MAX_DECLARED_PARAMETERS)
        return DIAMOND_VM_ARITY_ERROR;
    bool filled[DIAMOND_MAX_DECLARED_PARAMETERS]={0};
    size_t count=positional->count;
    for(size_t index=0;index<count;index++)filled[index]=true;
    size_t slots[DIAMOND_MAX_DECLARED_PARAMETERS];
    for(size_t keyword=0;keyword<keyword_count;keyword++) {
        if((size_t)keyword_names[keyword]>=caller->string_count)
            return DIAMOND_VM_INVALID_BYTECODE;
        const DiamondStringConstant *name=&caller->strings[keyword_names[keyword]];
        size_t slot=SIZE_MAX;
        for(size_t index=0;index<public_arity;index++)
            if(strlen(function->parameter_names[index])==name->length&&
               memcmp(function->parameter_names[index],name->chars,name->length)==0) {
                slot=index;break;
            }
        if(slot==SIZE_MAX) {snprintf(vm->error,sizeof vm->error,
            "no parameter with this name");return DIAMOND_VM_ARITY_ERROR;}
        if(filled[slot]) {snprintf(vm->error,sizeof vm->error,
            "multiple values for the same argument");return DIAMOND_VM_ARITY_ERROR;}
        slots[keyword]=slot;filled[slot]=true;if(slot+1>count)count=slot+1;
    }
    const size_t block_slot=public_arity==0?SIZE_MAX:public_arity-1;
    if(block!=nullptr) {
        if(block_slot==SIZE_MAX||filled[block_slot]) {
            snprintf(vm->error,sizeof vm->error,
                "multiple values for the same argument");
            return DIAMOND_VM_ARITY_ERROR;
        }
        filled[block_slot]=true;
        if(block_slot+1>count)count=block_slot+1;
    }
    const size_t parameter_offset=function->owner_class==DIAMOND_OWNER_NONE?0:1;
    const size_t required_public=function->required_arity>parameter_offset?
        function->required_arity-parameter_offset:0;
    for(size_t index=0;index<count;index++)if(!filled[index]&&
        index<required_public) {
        snprintf(vm->error,sizeof vm->error,"missing argument");
        return DIAMOND_VM_ARITY_ERROR;
    }
    DiamondValue *values=malloc(count*sizeof *values);
    if(values==nullptr&&count>0)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<count;index++)values[index]=DIAMOND_UNDEFINED;
    for(size_t index=0;index<positional->count;index++)
        values[index]=positional->values[index];
    for(size_t keyword=0;keyword<keyword_count;keyword++)
        values[slots[keyword]]=registers[keyword_registers[keyword]];
    if(block!=nullptr)values[block_slot]=*block;
    *merged=values;*merged_count=count;return DIAMOND_VM_OK;
}

/* DIAMOND_OP_INVOKE/INVOKE_MONO/INVOKE_TYPED for every receiver once
 * run_chunk's inline fast paths (a warm monomorphic Instance site, and
 * Array/Hash/String #length and #push) have not handled the call. Shared by
 * the interpreter and by compiled code (diamond_jit_invoke_any), so the two
 * dispatch in the same order and report the same errors. `site` is the
 * instruction, whose operands (and INVOKE_TYPED's type arguments) the caller
 * has already bounds-checked; they are read from it here, as
 * diamond_mono_invoke does, so every argument travels in a register. Writes
 * registers[dest] on success. An exception Thread#join re-raises comes back
 * as DIAMOND_VM_EXCEPTION, and the caller decides whether it was caught. */
DiamondVmStatus diamond_invoke_general(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,const uint8_t *site,size_t depth) {
    const DiamondOpCode instruction=(DiamondOpCode)site[0];
    const uint16_t dest=(uint16_t)(((unsigned)site[1]<<8)|site[2]);
    const uint16_t recv=(uint16_t)(((unsigned)site[3]<<8)|site[4]);
    const uint16_t name=(uint16_t)(((unsigned)site[5]<<8)|site[6]);
    const uint16_t base=(uint16_t)(((unsigned)site[7]<<8)|site[8]);
    const uint8_t argc=site[9];
    uint8_t type_argument_count=0;
    uint16_t type_arguments[8];
    if(instruction==DIAMOND_OP_INVOKE_TYPED) {
        type_argument_count=site[10];
        if(type_argument_count>8) return DIAMOND_VM_INVALID_BYTECODE;
        for(size_t index=0;index<type_argument_count;index++)
            type_arguments[index]=(uint16_t)(((unsigned)site[11+2*index]<<8)|
                site[12+2*index]);
    }
    if((size_t)name>=chunk->string_count) return DIAMOND_VM_TYPE_ERROR;
    const DiamondStringConstant *method_name=&chunk->strings[name];
    /* tap/dup -- universal for every native receiver kind (a
     * primitive, String, Symbol, Array, or Hash can't ever
     * define its own method to shadow these, unlike an
     * Instance, which gets its own version of both checks
     * further down, gated on the class NOT already defining a
     * same-named method of its own -- see that comment for
     * why interception order matters there but not here). */
    if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
       registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE) {
        if(method_name->length==11&&
           memcmp(method_name->chars,"public_send",11)==0) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            DiamondValue sent=DIAMOND_NIL;
            const DiamondVmStatus send_status=public_send_helper(vm,chunk,
                registers[recv],&registers[base],argc,depth,&sent);
            if(send_status!=DIAMOND_VM_OK)return send_status;
            registers[dest]=sent;return DIAMOND_VM_OK;
        }
        if(method_name->length==3&&
           memcmp(method_name->chars,"tap",3)==0) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            const DiamondVmStatus tap_status=outlined_invoke_tap(vm,chunk,registers,recv,base,argc,dest,depth);
            return tap_status;
        }
        /* Array#shuffle and #sample: uniform choices from
         * OpenSSL's RAND_bytes (as SecureRandom uses), with
         * rejection sampling so no index is favoured. */
        if(registers[recv].kind==DIAMOND_VALUE_OBJECT&&
           registers[recv].as.object->kind==DIAMOND_OBJECT_ARRAY&&
           argc==0&&type_argument_count==0&&
           ((method_name->length==7&&memcmp(method_name->chars,"shuffle",7)==0)||
            (method_name->length==6&&memcmp(method_name->chars,"sample",6)==0))) {
            const DiamondVmStatus shuffle_status=outlined_array_shuffle_sample(vm,registers,recv,dest,method_name);
            return shuffle_status;
        }
        /* nil?() on any built-in value, as in Ruby. */
        if(method_name->length==4&&memcmp(method_name->chars,"nil?",4)==0&&
           argc==0&&type_argument_count==0&&
           (registers[recv].kind!=DIAMOND_VALUE_OBJECT||
            registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)) {
            registers[dest]=DIAMOND_BOOL(registers[recv].kind==DIAMOND_VALUE_NIL);
            return DIAMOND_VM_OK;
        }
        /* Callable#arity: how many arguments it declares (its
         * required ones, for a callable with optional or rest
         * parameters -- no Ruby-style negative encoding). */
        if(method_name->length==5&&memcmp(method_name->chars,"arity",5)==0&&
           argc==0&&type_argument_count==0&&
           registers[recv].kind==DIAMOND_VALUE_OBJECT&&
           registers[recv].as.object->kind==DIAMOND_OBJECT_CLOSURE) {
            const DiamondVmStatus arity_status=outlined_callable_arity(chunk,registers,recv,dest);
            return arity_status;
        }
        /* to_s() on any built-in value: the same text string
         * interpolation produces. (Int, Float, and Time also
         * have their own to_s further down; this matches them.) */
        if(method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0&&
           argc==0&&(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
            registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            DiamondValue text=DIAMOND_NIL;
            const DiamondVmStatus to_s_status=
                stringify_value(vm,chunk,depth,registers[recv],&text);
            if(to_s_status!=DIAMOND_VM_OK)return to_s_status;
            registers[dest]=text;return DIAMOND_VM_OK;
        }
        /* inspect() on any built-in value: like to_s(), but Strings
         * are quoted and Symbols keep their colon, so the text shows
         * what the value is rather than how it prints. */
        if(method_name->length==7&&memcmp(method_name->chars,"inspect",7)==0&&
           argc==0&&(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
            registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            DiamondValue text=DIAMOND_NIL;
            const DiamondVmStatus inspect_status=
                inspect_value(vm,chunk,depth,registers[recv],&text);
            if(inspect_status!=DIAMOND_VM_OK)return inspect_status;
            registers[dest]=text;return DIAMOND_VM_OK;
        }
        /* dup only where a real (or trivially self-returning)
         * shallow copy is well-defined -- everything else
         * (Regexp/Time/File/Socket/...) falls through to its
         * own per-type block below and gets that type's own
         * accurate "undefined method 'dup' for X" instead of a
         * generic one here. */
        const bool dup_defined=registers[recv].kind!=DIAMOND_VALUE_OBJECT||
            registers[recv].as.object->kind==DIAMOND_OBJECT_STRING||
            registers[recv].as.object->kind==DIAMOND_OBJECT_SYMBOL||
            registers[recv].as.object->kind==DIAMOND_OBJECT_ARRAY||
            registers[recv].as.object->kind==DIAMOND_OBJECT_HASH;
        if(dup_defined&&method_name->length==3&&
           memcmp(method_name->chars,"dup",3)==0) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            const DiamondVmStatus dup_status=
                diamond_jit_dup(vm,&registers[recv],&registers[dest]);
            return dup_status;
        }
        /* freeze/frozen? -- defined for exactly the same
         * receiver set dup_defined already names. For a
         * primitive or an already-immutable String/Symbol,
         * mirrors dup's own "already immutable, return self/
         * true unchanged" precedent (see that block's own
         * comment) rather than raising "undefined method" --
         * freeze() is a harmless no-op, frozen?() is always
         * true. Array/Hash get the real, effectful check:
         * every native mutation they support (push, pop,
         * `[]=`, and everything built from those -- see
         * docs/classes-and-modules.md's "freeze / frozen?"
         * section) tests object.frozen before proceeding. */
        if(dup_defined&&method_name->length==6&&
           memcmp(method_name->chars,"freeze",6)==0) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            const DiamondVmStatus freeze_status=
                diamond_jit_freeze(vm,&registers[recv],&registers[dest]);
            return freeze_status;
        }
        /* deep_freeze: same receiver set as freeze (no-op on a
         * primitive/String/Symbol); Array/Hash get the real,
         * transitive walk. Not JIT-compiled -- jit.c bails on
         * any method name it doesn't list, which is correct. */
        if(dup_defined&&method_name->length==11&&
           memcmp(method_name->chars,"deep_freeze",11)==0) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            const DiamondVmStatus deep_status=
                diamond_deep_freeze(vm,&registers[recv],&registers[dest]);
            return deep_status;
        }
        if(dup_defined&&method_name->length==7&&
           memcmp(method_name->chars,"frozen?",7)==0) {
            if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            const DiamondVmStatus frozen_status=
                diamond_jit_frozen(vm,&registers[recv],&registers[dest]);
            return frozen_status;
        }
    }
    /* Int#to_s(base) on a promoted Int: digits in base 2..36. */
    if(value_is_bignum(registers[recv])&&argc==1&&
       method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0) {
        const DiamondVmStatus radix_status=outlined_bignum_to_s_base(vm,registers,recv,base,dest);
        return radix_status;
    }
    /* An Int past 64 bits is a bignum object; give it the same
     * conversions as a small Int. */
    if(value_is_bignum(registers[recv])&&argc==0&&
       ((method_name->length==4&&(memcmp(method_name->chars,"to_s",4)==0||
         memcmp(method_name->chars,"to_i",4)==0||
         memcmp(method_name->chars,"to_f",4)==0))||
        (method_name->length==3&&memcmp(method_name->chars,"abs",3)==0))) {
        const DiamondVmStatus convert_status=outlined_bignum_convert(vm,registers,recv,dest,method_name);
        return convert_status;
    }
    if(registers[recv].kind==DIAMOND_VALUE_INT||
       registers[recv].kind==DIAMOND_VALUE_FLOAT) {
        const DiamondVmStatus dispatch_status=numeric_invoke_helper(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
        return dispatch_status;
    }
    if(registers[recv].kind!=DIAMOND_VALUE_OBJECT) {
        /* Every native-type "no such method" site above (and
         * the DIAMOND_OBJECT_INSTANCE case further down) names
         * the method and the receiver's type; this catch-all
         * for a primitive receiver (nil, Bool, or an Int/Float
         * whose method name didn't match the numeric-method
         * dispatch just above) previously fell through silently
         * to a bare DIAMOND_VM_TYPE_ERROR with no message, so
         * `nil.foo()` (and, notably, `self.foo(...)` called
         * from inside a module_function method invoked via its
         * qualified form -- self's slot is always nil there,
         * see emit_singleton_call's own comment, src/compiler.c
         * -- deliberately not given a `self` story) just said
         * "runtime error: type error" with no further detail. */
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,registers[recv]);
        snprintf(vm->error,sizeof vm->error,
            "undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondObjectKind receiver_kind=registers[recv].as.object->kind;
    if(receiver_kind==DIAMOND_OBJECT_ARRAY||
       receiver_kind==DIAMOND_OBJECT_HASH||
       receiver_kind==DIAMOND_OBJECT_STRING) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        return collection_invoke_helper(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
    }
    if(receiver_kind==DIAMOND_OBJECT_FIBER) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        const DiamondVmStatus dispatch_status=fiber_invoke_helper(
            vm,registers,recv,base,argc,dest,method_name);
        return dispatch_status;
    }
    if(receiver_kind==DIAMOND_OBJECT_THREAD) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        return thread_invoke_helper(vm,chunk,registers,recv,argc,dest,method_name);
    }
    if(receiver_kind!=DIAMOND_OBJECT_INSTANCE) {
        bool native_handled=false;
        const DiamondVmStatus native_status=outlined_native_object_invoke(vm,chunk,depth,registers,recv,base,
            argc,dest,method_name,type_argument_count,receiver_kind,&native_handled);
        if(native_status!=DIAMOND_VM_OK||native_handled)return native_status;
    }
    /* Everything else is an Instance (or a receiver with no such method,
     * which diamond_jit_invoke_instance reports). */
    DiamondValue call_result=DIAMOND_NIL;
    const DiamondVmStatus status=diamond_jit_invoke_instance(vm,chunk,
        site,instruction,registers,recv,name,base,argc,
        type_argument_count,type_arguments,depth,&call_result);
    if(status!=DIAMOND_VM_OK)return status;
    registers[dest]=call_result;
    return DIAMOND_VM_OK;
}

/* Compiled code's DIAMOND_OP_INVOKE/INVOKE_MONO on a receiver whose type it
 * could not prove: the interpreter's whole dispatch (diamond_mono_invoke for
 * a warm site, then diamond_invoke_general). A compiled function has no
 * exception handlers, so a Thread#join exception is uncaught here and gets
 * the error text run_chunk gives it in that case. `site` is the instruction,
 * read as diamond_invoke_general reads it. */
DiamondVmStatus diamond_jit_invoke_any(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,const uint8_t *site,size_t depth) {
    const DiamondOpCode instruction=(DiamondOpCode)site[0];
    const uint16_t dest=(uint16_t)(((unsigned)site[1]<<8)|site[2]);
    const uint16_t recv=(uint16_t)(((unsigned)site[3]<<8)|site[4]);
    const uint16_t name=(uint16_t)(((unsigned)site[5]<<8)|site[6]);
    const uint16_t base=(uint16_t)(((unsigned)site[7]<<8)|site[8]);
    const uint8_t argc=site[9];
    const bool instance=registers[recv].kind==DIAMOND_VALUE_OBJECT&&
        registers[recv].as.object->kind==DIAMOND_OBJECT_INSTANCE;
    if(instruction==DIAMOND_OP_INVOKE_MONO&&instance) {
        DiamondValue mono_result;
        const DiamondVmStatus mono_status=diamond_mono_invoke(vm,chunk,
            registers,site,depth,&mono_result);
        if(mono_status==DIAMOND_VM_OK) {
            value_copy_from_callee(&registers[dest],&mono_result);
            return DIAMOND_VM_OK;
        }
        if(mono_status!=DIAMOND_VM_NOT_CACHED) return mono_status;
    }
    /* As in run_chunk: an Instance passes every check before the method
     * call in diamond_invoke_general. */
    if(instance) {
        DiamondValue call_result=DIAMOND_NIL;
        const DiamondVmStatus status=diamond_jit_invoke_instance(vm,chunk,site,
            instruction,registers,recv,name,base,argc,0,nullptr,depth,&call_result);
        if(status!=DIAMOND_VM_OK) return status;
        registers[dest]=call_result;
        return DIAMOND_VM_OK;
    }
    const bool thread=registers[recv].kind==DIAMOND_VALUE_OBJECT&&
        registers[recv].as.object->kind==DIAMOND_OBJECT_THREAD;
    const DiamondVmStatus status=diamond_invoke_general(vm,chunk,registers,site,depth);
    if(status==DIAMOND_VM_EXCEPTION&&thread) set_uncaught_exception_error(vm);
    return status;
}

/* Compiled code's DIAMOND_OP_INVOKE_TYPED (`recv.method[Type]()`), read from
 * the instruction at `site`: run_chunk's dispatch for it, with the explicit
 * type arguments that follow the operands passed to an Instance's method.
 * Separate from diamond_jit_invoke_any so untyped calls don't pay for the
 * decode. jit.c checked the trailer's bounds when it compiled the site. */
DiamondVmStatus diamond_jit_invoke_typed(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,const uint8_t *site,size_t depth) {
    const uint16_t dest=(uint16_t)(((unsigned)site[1]<<8)|site[2]);
    const uint16_t recv=(uint16_t)(((unsigned)site[3]<<8)|site[4]);
    const uint16_t name=(uint16_t)(((unsigned)site[5]<<8)|site[6]);
    const uint16_t base=(uint16_t)(((unsigned)site[7]<<8)|site[8]);
    const uint8_t argc=site[9];
    const uint8_t type_argument_count=site[10];
    if(type_argument_count>8)return DIAMOND_VM_INVALID_BYTECODE;
    uint16_t type_arguments[8];
    for(size_t index=0;index<type_argument_count;index++)
        type_arguments[index]=(uint16_t)(((unsigned)site[11+2*index]<<8)|site[12+2*index]);
    if(registers[recv].kind==DIAMOND_VALUE_OBJECT&&
       registers[recv].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        DiamondValue call_result=DIAMOND_NIL;
        const DiamondVmStatus status=diamond_jit_invoke_instance(vm,chunk,site,
            DIAMOND_OP_INVOKE_TYPED,registers,recv,name,base,argc,type_argument_count,
            type_arguments,depth,&call_result);
        if(status!=DIAMOND_VM_OK) return status;
        registers[dest]=call_result;
        return DIAMOND_VM_OK;
    }
    const bool thread=registers[recv].kind==DIAMOND_VALUE_OBJECT&&
        registers[recv].as.object->kind==DIAMOND_OBJECT_THREAD;
    const DiamondVmStatus status=diamond_invoke_general(vm,chunk,registers,site,depth);
    if(status==DIAMOND_VM_EXCEPTION&&thread) set_uncaught_exception_error(vm);
    return status;
}

/* Compiled code's DIAMOND_OP_CALL_CLOSURE, read from the instruction at
 * `site`: run_chunk's call_closure_value. Writes registers[dest]. */
DiamondVmStatus diamond_jit_call_closure(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,const uint8_t *site,size_t depth) {
    const uint16_t dest=(uint16_t)(((unsigned)site[1]<<8)|site[2]);
    const uint16_t callable=(uint16_t)(((unsigned)site[3]<<8)|site[4]);
    const uint16_t base=(uint16_t)(((unsigned)site[5]<<8)|site[6]);
    DiamondValue call_result=DIAMOND_NIL;
    const DiamondVmStatus status=call_closure_value(vm,chunk,registers,callable,base,
        site[7],depth,&call_result);
    if(status!=DIAMOND_VM_OK)return status;
    value_copy_from_callee(&registers[dest],&call_result);
    return DIAMOND_VM_OK;
}

/* Compiled code's DIAMOND_OP_TO_STRING for a value that is not already a
 * String (native code copies a String itself): run_chunk's stringify_value.
 * Writes registers[dest]. */
DiamondVmStatus diamond_jit_to_string(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,uint16_t dest,uint16_t source,size_t depth) {
    DiamondValue converted=DIAMOND_NIL;
    const DiamondVmStatus status=stringify_value(vm,chunk,depth,registers[source],&converted);
    if(status!=DIAMOND_VM_OK)return status;
    value_copy_from_callee(&registers[dest],&converted);
    return DIAMOND_VM_OK;
}

/* Compiled code's DIAMOND_OP_CLOSURE. The compiling function always pushes
 * a frame for it (vm->frames is that frame). The prologue leaves its
 * serial 0, so the first closure gives it one here, sparing every other
 * compiled call the increment. Its home is run_chunk's: the frame itself,
 * or for a do-block body (`closure`, the one it was entered with) the
 * method the block was written in, where a `return` inside a nested block
 * lands. */
DiamondVmStatus diamond_jit_closure(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,const uint8_t *site,const DiamondClosure *closure) {
    const uint16_t dest=(uint16_t)(((unsigned)site[1]<<8)|site[2]);
    DiamondFrame *frame=(DiamondFrame *)vm->frames;
    if(frame->serial==0) {
        frame->serial=++vm->frame_serial;
        frame->home=closure!=nullptr&&closure->is_block&&closure->return_target!=0?
            closure->return_target:frame->serial;
    }
    DiamondValue created=DIAMOND_NIL;
    const DiamondVmStatus status=diamond_make_closure(vm,chunk,registers,site,
        frame->serial,frame->home,&created);
    if(status!=DIAMOND_VM_OK) return status;
    registers[dest]=created;
    return DIAMOND_VM_OK;
}

/* Compiled code's DIAMOND_OP_GET_NAMESPACE_CONSTANT. Neither allocates nor
 * runs user code. */
DiamondVmStatus diamond_jit_get_namespace_constant(DiamondVm *vm,DiamondValue *registers,
        uint16_t dest,uint16_t index) {
    return diamond_read_namespace_constant(vm,index,&registers[dest]);
}

/* A block body's captures: run_chunk's GET_CAPTURE, GET_CAPTURE_CELL and
 * SET_CAPTURE, checks included. */
static inline DiamondCell *jit_capture_cell(const DiamondClosure *closure,uint16_t index) {
    if(closure==nullptr||index>=closure->capture_count) return nullptr;
    const DiamondValue captured=closure->captures[index];
    if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
        return nullptr;
    return (DiamondCell *)captured.as.object;
}

DiamondVmStatus diamond_jit_get_capture(const DiamondClosure *closure,
        DiamondValue *registers,uint16_t dest,uint16_t index,bool cell) {
    DiamondCell *found=jit_capture_cell(closure,index);
    if(found==nullptr) return DIAMOND_VM_INVALID_BYTECODE;
    registers[dest]=cell?DIAMOND_OBJECT(found):found->value;
    return DIAMOND_VM_OK;
}

DiamondVmStatus diamond_jit_set_capture(DiamondVm *vm,const DiamondClosure *closure,
        DiamondValue *registers,uint16_t index,uint16_t source) {
    DiamondCell *found=jit_capture_cell(closure,index);
    if(found==nullptr) return DIAMOND_VM_INVALID_BYTECODE;
    found->value=registers[source];
    if(!gc_write_barrier(vm,&found->object)) return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}

/* Compiled code's DIAMOND_OP_BOX_LOCAL, GET_CELL and SET_CELL, with
 * run_chunk's checks. Only BOX_LOCAL allocates; only SET_CELL can fail
 * other than on malformed bytecode (its write barrier). */
DiamondVmStatus diamond_jit_box_local(DiamondVm *vm,DiamondValue *registers,
        uint16_t reg) {
    if(registers[reg].kind==DIAMOND_VALUE_OBJECT&&
       registers[reg].as.object->kind==DIAMOND_OBJECT_CELL) return DIAMOND_VM_OK;
    DiamondCell *cell=allocate_cell(vm,registers[reg]);
    if(cell==nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    registers[reg]=DIAMOND_OBJECT(cell);
    return DIAMOND_VM_OK;
}

DiamondVmStatus diamond_jit_get_cell(DiamondValue *registers,uint16_t dest,
        uint16_t cell_reg) {
    if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)
        return DIAMOND_VM_INVALID_BYTECODE;
    registers[dest]=((DiamondCell *)registers[cell_reg].as.object)->value;
    return DIAMOND_VM_OK;
}

DiamondVmStatus diamond_jit_set_cell(DiamondVm *vm,DiamondValue *registers,
        uint16_t cell_reg,uint16_t source) {
    if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
       registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)
        return DIAMOND_VM_INVALID_BYTECODE;
    ((DiamondCell *)registers[cell_reg].as.object)->value=registers[source];
    if(!gc_write_barrier(vm,registers[cell_reg].as.object))
        return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}

/* Compiled code's way into an opcode whose interpreter body lives outside
 * run_chunk (declared in src/jit.h). The bodies decode their own operands,
 * so this only dispatches; jit.c's opcode_runs_outlined is the matching
 * list, and decode_instruction_operands knows each one's operands and the
 * register it writes. */
static uint16_t jit_outlined_short(const DiamondChunk *chunk,size_t offset) {
    return (uint16_t)(((unsigned)chunk->code[offset]<<8)|chunk->code[offset+1]);
}

DiamondVmStatus diamond_jit_outlined(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondValue *registers,size_t depth,size_t offset) {
    size_t ip=offset+1;
    switch((DiamondOpCode)chunk->code[offset]) {
        case DIAMOND_OP_PRINT: return outlined_print(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SYMBOL: return outlined_symbol(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CLASS_NAME: return outlined_class_name(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_NEGATE: return outlined_negate(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CHECK_DESTRUCTURE_COUNT:
            return outlined_check_destructure_count(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_INVOKE_SELF_METHOD:
            return outlined_invoke_self_method(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CALL_TYPED: return outlined_call_typed(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CALL_SPREAD: case DIAMOND_OP_CALL_TYPED_SPREAD:
        case DIAMOND_OP_CALL_KEYWORD_SPREAD: case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD:
            return outlined_call_spread(vm,chunk,registers,depth,chunk->code[offset],&ip);
        case DIAMOND_OP_CALL_SINGLETON_KEYWORDS: case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS:
            return outlined_call_singleton_keywords(vm,chunk,registers,depth,
                chunk->code[offset],&ip);
        case DIAMOND_OP_CALL_SINGLETON_SPREAD: case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD:
            return outlined_call_singleton_spread(vm,chunk,registers,depth,
                chunk->code[offset],&ip);
        case DIAMOND_OP_NEW_SPREAD: return outlined_new_spread(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_NEW_TYPED:
            return outlined_new_keywords(vm,chunk,registers,depth,chunk->code[offset],&ip);
        case DIAMOND_OP_INVOKE_KEYWORDS: case DIAMOND_OP_INVOKE_TYPED_KEYWORDS:
            return outlined_invoke_keywords(vm,chunk,registers,depth,chunk->code[offset],&ip);
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD:
            return outlined_invoke_spread(vm,chunk,registers,depth,chunk->code[offset],&ip);
        case DIAMOND_OP_CALL_CLOSURE_KEYWORDS:
            return outlined_call_closure_keywords(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CALL_CLOSURE_SPREAD:
            return outlined_call_closure_spread(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_BUILD_SPREAD_ARGS:
            return outlined_build_spread_args(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_JSON_STRINGIFY:
        case DIAMOND_OP_JSON_STRINGIFY_OPTIONS:
            return outlined_json_stringify(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TO_SYMBOL:
            return outlined_to_symbol(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TO_INT:
            return outlined_to_int(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TO_FLOAT:
            return outlined_to_float(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CHR:
            return outlined_chr(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TLS_LISTEN:
            return outlined_tls_listen(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SIGNAL_TRAP:
            return outlined_signal_trap(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_UDP_OPEN:
            return outlined_udp_open(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_UDP_BIND:
            return outlined_udp_bind(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TCP_CONNECT:
            return outlined_tcp_connect(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TCP_CONNECT_NONBLOCK:
            return outlined_tcp_connect_nonblock(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_DNS_RESOLVE:
            return outlined_dns_resolve(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_PROGRAM_BUILDER_NEW:
            return outlined_program_builder_new(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SQLITE3_OPEN:
            return outlined_sqlite3_open(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_REGEXP_NEW:
            return outlined_regexp_new(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_FILE_JOIN:
            return outlined_file_join(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_FILE_DELETE:
            return outlined_file_delete(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_FILE_OPEN:
            return outlined_file_open(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_FIBER_NEW:
            return outlined_fiber_new(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_YIELD:
            return outlined_yield(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_ENV:
            return outlined_env(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_ARGV:
            return outlined_argv(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_EXIT:
            return outlined_exit(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_BASE64_DECODE:
            return outlined_base64_decode(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_BASE64_ENCODE:
            return outlined_base64_encode(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_GZIP_DECOMPRESS:
            return outlined_gzip_decompress(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_GZIP_COMPRESS:
            return outlined_gzip_compress(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CIPHER_DECRYPT:
            return outlined_cipher_decrypt(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CIPHER_ENCRYPT:
            return outlined_cipher_encrypt(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_HMAC_SHA1:
            return outlined_hmac_sha1(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_DIGEST_SHA1:
            return outlined_digest_sha1(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_HMAC_VERIFY:
            return outlined_hmac_verify(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_HMAC_SHA256:
            return outlined_hmac_sha256(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_DIGEST_SHA256:
            return outlined_digest_sha256(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SECURE_RANDOM_HEX:
            return outlined_secure_random_hex(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SECURE_RANDOM_BYTES:
            return outlined_secure_random_bytes(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_BCRYPT_VERIFY:
            return outlined_bcrypt_verify(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_BCRYPT_HASH:
            return outlined_bcrypt_hash(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_PROCESS_SPAWN:
        case DIAMOND_OP_PROCESS_SPAWN_OPTIONS:
            return outlined_process_spawn(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_PROCESS_RUN:
        case DIAMOND_OP_PROCESS_RUN_OPTIONS:
            return outlined_process_run(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TENSOR_RANDOM:
            return outlined_tensor_random(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TENSOR_FROM_ARRAY:
            return outlined_tensor_from_array(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TENSOR_ZEROS:
            return outlined_tensor_zeros(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TIME_BUILD:
            return outlined_time_build(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TIME_PARSE:
            return outlined_time_parse(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TIME_AT:
            return outlined_time_at(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TIME_NOW:
            return outlined_time_now(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_TIME_MONOTONIC:
            return outlined_time_monotonic(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CHECK_HASH_KEY:
            return outlined_check_hash_key(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_ARRAY_SUFFIX:
            return outlined_array_suffix(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_ARRAY_MIDDLE:
            return outlined_array_middle(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_HASH_REST:
            return outlined_hash_rest(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CASE_HASH_HAS:
            return outlined_case_hash_has(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CASE_HASH_SHAPE:
            return outlined_case_hash_shape(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_ARRAY_REST:
            return outlined_array_rest(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CASE_ARRAY_SHAPE:
            return outlined_case_array_shape(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SET_NAMESPACE_CONSTANT:
            return outlined_set_namespace_constant(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_GET_IVAR_NAME:
        case DIAMOND_OP_SET_IVAR_NAME:
            return outlined_get_ivar_name(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_CASE_MATCH:
            return outlined_case_match(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_COMPARE:
            return outlined_compare(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_BITWISE_AND:
        case DIAMOND_OP_BITWISE_OR:
        case DIAMOND_OP_BITWISE_XOR:
            return outlined_bitwise_and(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SHIFT_RIGHT:
            return outlined_shift_right(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_SHIFT_LEFT:
            return outlined_shift_left(vm,chunk,registers,depth,&ip);
        case DIAMOND_OP_GETS:
            return outlined_gets(vm,chunk,registers,depth,&ip);
        /* Bodies run_chunk already called out of line. */
        case DIAMOND_OP_THREAD_NEW: return outlined_thread_new(vm,chunk,registers,&ip);
        case DIAMOND_OP_CHANNEL_NEW: return outlined_channel_new(vm,chunk,registers,&ip);
        case DIAMOND_OP_CHANNEL_SELECT: return outlined_channel_select(vm,chunk,registers,&ip);
        case DIAMOND_OP_CHANNEL_SELECT_SEND:
            return outlined_channel_select_send(vm,chunk,registers,&ip);
        case DIAMOND_OP_SUPERVISOR_NEW: return outlined_supervisor_new(vm,chunk,registers,&ip);
        case DIAMOND_OP_DIR_ENTRIES: return outlined_dir_entries(vm,chunk,registers,&ip);
        case DIAMOND_OP_SEMVER: return outlined_semver(vm,chunk,registers,&ip);
        case DIAMOND_OP_FILE_PATH: return outlined_file_path(vm,chunk,registers,&ip);
        case DIAMOND_OP_POSTGRES_OPEN: return outlined_postgres_open(vm,chunk,registers,&ip);
        case DIAMOND_OP_MYSQL_OPEN: return outlined_mysql_open(vm,chunk,registers,&ip);
        case DIAMOND_OP_TCP_LISTEN: case DIAMOND_OP_TCP_LISTEN_NONBLOCK:
            return outlined_tcp_listen_nonblock(vm,chunk,registers,chunk->code[offset],&ip);
        case DIAMOND_OP_TLS_CONNECT: case DIAMOND_OP_TLS_START_HANDSHAKE:
            return outlined_tls_connect(vm,chunk,registers,chunk->code[offset],&ip);
        case DIAMOND_OP_IO_POLL: return outlined_io_poll(vm,chunk,registers,depth,&ip);
        /* Each invalidates the method caches compiled code reads too. */
        case DIAMOND_OP_DEFINE_METHOD: return outlined_define_method(vm,chunk,registers,&ip);
        case DIAMOND_OP_REDEFINE_METHOD: return outlined_redefine_method(vm,chunk,registers,&ip);
        case DIAMOND_OP_COMPILE_METHOD: return outlined_compile_method(vm,chunk,registers,&ip);
        /* The JIT checked these operands' extent when it compiled them. */
        case DIAMOND_OP_GET_CVAR:
            return get_cvar_helper(vm,chunk,jit_outlined_short(chunk,offset+3),
                jit_outlined_short(chunk,offset+5),
                &registers[jit_outlined_short(chunk,offset+1)]);
        case DIAMOND_OP_SET_CVAR:
            return set_cvar_helper(vm,chunk,jit_outlined_short(chunk,offset+1),
                jit_outlined_short(chunk,offset+3),
                registers[jit_outlined_short(chunk,offset+5)]);
        case DIAMOND_OP_CHECK_SELF_RETURN:
            return check_self_return(vm,chunk,registers[jit_outlined_short(chunk,offset+1)],
                jit_outlined_short(chunk,offset+3),
                registers[jit_outlined_short(chunk,offset+5)]);
        default:
            return DIAMOND_VM_INVALID_BYTECODE;
    }
}
