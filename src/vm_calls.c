#define _GNU_SOURCE

#include "vm_internal.h"
#include "jit.h"
#include "bignum.h"

#include <errno.h>
#include <inttypes.h>
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
DiamondVmStatus call_closure_helper(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondFunction *fn,const DiamondClosure *called,
        const DiamondValue *registers,uint16_t base,uint8_t argc,size_t depth,
        DiamondValue *result) {
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
    /* No argc>DIAMOND_MAX_ARGUMENTS guard needed here: argc is a plain
     * uint8_t (the wire format's own argument-count width), so it can
     * never exceed DIAMOND_MAX_ARGUMENTS (255) in the first place --
     * call_arguments[DIAMOND_MAX_ARGUMENTS+1] below always has room. */
    DiamondValue call_arguments[DIAMOND_MAX_ARGUMENTS+1];
    const DiamondValue *arguments=&registers[base];
    size_t argument_count=argc;
    if(needs_self_slot) {
        call_arguments[0]=DIAMOND_NIL;
        for(size_t index=0;index<argc;index++)
            call_arguments[index+1]=registers[(size_t)base+index];
        arguments=call_arguments;
        argument_count=(size_t)argc+1;
    }
    const size_t arity_check_count=self_via_capture?(size_t)argc:argument_count;
    if(arity_check_count<fn->required_arity||
       (arity_check_count>fn->arity && !fn->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    const DiamondChunk child={.name=fn->name,.code=fn->code,.lines=fn->lines,
      .columns=fn->columns,.code_count=fn->code_count,.constants=fn->constants,
      .constant_count=fn->constant_count,.strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=chunk->functions,.function_count=chunk->function_count,
      .classes=chunk->classes,.class_count=chunk->class_count,
      .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .class_type_variable=fn->class_type_variable,
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    return run_chunk(&child,vm,arguments,argument_count,depth+1,called,result);
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
      .parameter_offset=target->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=target->register_count};
    return run_chunk(&child,vm,forward_args,total_argc,depth+1,nullptr,result);
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
     * instruction checks, so a VM with an instruction or wall-clock budget
     * (or one already past a tripped budget, in its grace allowance) stays
     * on the interpreter, where the budget is enforced. */
    if (vm->jit && !function->jit_ineligible && vm->max_instructions == 0 &&
        vm->max_wall_nanoseconds == 0 && !vm->resource_grace_active) {
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
                mutable_function->jit_call_count >= vm->jit_threshold) {
                size_t jit_code_size = 0;
                size_t self_index = SIZE_MAX;
                for (size_t index = 0; index < chunk_to_interpret->function_count; index++)
                    if (chunk_to_interpret->functions[index] == function) {
                        self_index = index;
                        break;
                    }
                void *compiled = diamond_jit_try_compile(function, self_index,
                    &jit_code_size);
                if (compiled != nullptr) {
                    mutable_function->jit_code = compiled;
                    mutable_function->jit_code_size = jit_code_size;
                    vm->jit_compiled_functions++;
                } else {
                    mutable_function->jit_ineligible = true;
                }
            }
        }
        if (mutable_function->jit_code != nullptr) {
            DiamondValue jit_registers[DIAMOND_JIT_MAX_REGISTERS];
            const size_t jit_register_count = function->register_count == 0
                ? DIAMOND_JIT_MAX_REGISTERS : function->register_count;
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
            const DiamondJitFn jit_fn = (DiamondJitFn)mutable_function->jit_code;
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
                    argument_count, chunk_to_interpret, depth + 1);
            if (jit_status == DIAMOND_VM_OK) {
                *result = jit_result;
                return DIAMOND_VM_OK;
            }
            if (jit_status == DIAMOND_JIT_TAIL) return DIAMOND_JIT_TAIL;
            if (jit_status != DIAMOND_JIT_RETRY) {
                vm->jit_hard_propagations++;
                return (DiamondVmStatus)jit_status;
            }
            vm->jit_bailouts++;
            /* falls through to the ordinary interpreted call below --
             * always fully correct regardless of why the compiled
             * attempt bailed. */
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

DiamondVmStatus jit_call_or_interpret(DiamondVm *vm, const DiamondFunction *function,
        const DiamondChunk *chunk_to_interpret, const DiamondValue *arguments,
        size_t argument_count, size_t depth, const DiamondClosure *closure,
        DiamondValue *result) {
    const uint8_t status = jit_call_core(vm, function, chunk_to_interpret,
        arguments, argument_count, depth, closure, result);
    if (status == DIAMOND_JIT_TAIL)
        return jit_tail_loop(vm, chunk_to_interpret, depth, result);
    return (DiamondVmStatus)status;
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
DiamondVmStatus invoke_resolved_method_helper(DiamondVm *vm,
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
    DiamondTypeBinding explicit_bindings[8];
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
    const DiamondChunk child={.name=fn->name,.code=fn->code,
      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
      .constants=fn->constants,.constant_count=fn->constant_count,
      .strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=function_chunk->functions,.function_count=function_chunk->function_count,
      .classes=function_chunk->classes,.class_count=function_chunk->class_count,
      .interfaces=function_chunk->interfaces,.interface_count=function_chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .class_type_variable=fn->class_type_variable,
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .type_variable_bindings=type_argument_count==0?nullptr:explicit_bindings,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    return jit_call_or_interpret(vm,fn,&child,args,total_args,depth,nullptr,result);
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
    const DiamondChunk called_chunk = plain_call_chunk(function, chunk);
    return jit_call_or_interpret(vm, function, &called_chunk, &registers[base],
        argc, depth, nullptr, out);
}

/* JIT trampoline for DIAMOND_OP_TAIL_CALL to a function other than the
 * caller. Returns DIAMOND_JIT_TAIL after staging the callee and its
 * arguments when the interpreter's own in-place switch would apply (a plain,
 * non-variadic target whose registers hold the arguments); otherwise it is
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
    if (function->code != chunk->code && !function->has_variadic &&
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
    /* tap/dup/freeze/frozen?/respond_to?/public_send -- universal methods,
     * gated on lookup_method coming back empty first: a class CAN
     * legitimately define its own dup/tap/respond_to?, and that user
     * definition must win, mirroring run_chunk's own non-Instance-receiver
     * interception just above this case (dup_defined) for why interception
     * order matters here but not there. */
    if(method_name->length==3&&memcmp(method_name->chars,"tap",3)==0&&
       lookup_method(owner,instance->class,"tap",3)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        return jit_instance_tap(vm,chunk,registers,recv,base,argc,depth,out);
    }
    if(method_name->length==4&&memcmp(method_name->chars,"nil?",4)==0&&
       argc==0&&lookup_method(owner,instance->class,"nil?",4)==nullptr) {
        *out=DIAMOND_BOOL(false);return DIAMOND_VM_OK;
    }
    /* Every object answers to_s(), with the text interpolation gives it,
     * unless its class defines its own. */
    if(method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0&&
       lookup_method(owner,instance->class,"to_s",4)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return stringify_value(vm,chunk,depth,registers[recv],out);
    }
    /* Every object answers inspect(): its fields, unless its class defines its
     * own inspect (any arity), which then dispatches normally. */
    if(method_name->length==7&&memcmp(method_name->chars,"inspect",7)==0&&
       lookup_method(owner,instance->class,"inspect",7)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return inspect_value(vm,chunk,depth,registers[recv],out);
    }
    if(method_name->length==3&&memcmp(method_name->chars,"dup",3)==0&&
       lookup_method(owner,instance->class,"dup",3)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_jit_dup(vm,&registers[recv],out);
    }
    if(method_name->length==6&&memcmp(method_name->chars,"freeze",6)==0&&
       lookup_method(owner,instance->class,"freeze",6)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_jit_freeze(vm,&registers[recv],out);
    }
    if(method_name->length==11&&memcmp(method_name->chars,"deep_freeze",11)==0&&
       lookup_method(owner,instance->class,"deep_freeze",11)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_deep_freeze(vm,&registers[recv],out);
    }
    if(method_name->length==7&&memcmp(method_name->chars,"frozen?",7)==0&&
       lookup_method(owner,instance->class,"frozen?",7)==nullptr) {
        if(type_argument_count!=0)return reject_type_arguments(vm,method_name);
        if(argc!=0) return DIAMOND_VM_ARITY_ERROR;
        return diamond_jit_frozen(vm,&registers[recv],out);
    }
    if(method_name->length==11&&
       memcmp(method_name->chars,"respond_to?",11)==0&&
       lookup_method(owner,instance->class,"respond_to?",11)==nullptr) {
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
       lookup_method(owner,instance->class,"public_send",11)==nullptr) {
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
    const DiamondMethod *method=nullptr;
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
    if(method->is_private||method->is_protected) {
        const DiamondVmStatus visibility=check_instance_method_visibility(vm,chunk,owner,
            instance,method,method_name,registers,recv);
        if(visibility!=DIAMOND_VM_OK)return visibility;
    }
    if(argc<method->required_arity||
       (argc>method->arity && !method->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    return invoke_resolved_method_helper(vm,owner,
        method,registers[recv],registers,base,argc,
        instruction==DIAMOND_OP_INVOKE_TYPED,
        type_argument_count,type_arguments,chunk,depth,out);
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

/* Keep the inference scratch graph out of ordinary constructor frames. */
__attribute__((noinline))
DiamondVmStatus bind_instance_constructor_type(DiamondVm *vm,
        const DiamondChunk *chunk,const DiamondFunction *fn,
        const DiamondValue *arguments,size_t count,DiamondInstance *instance) {
    if(!fn->class_type_variable||fn->class_type_variable>fn->type_variable_count||
       fn->type_variable_count>8||
       fn->class_type_variable!=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class))
        return DIAMOND_VM_TYPE_ERROR;
    DiamondTypeBinding bindings[8]={};
    for(size_t parameter=0;parameter<count&&
        parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++) {
        if(arguments[parameter].kind==DIAMOND_VALUE_UNDEFINED)continue;
        const uint16_t set=fn->parameter_type_sets[parameter];
        if(set!=DIAMOND_NO_TYPE_SET&&set<fn->type_set_count)
            infer_from_value(chunk,arguments[parameter],fn->type_sets,set,bindings);
    }
    for(size_t variable=0;variable<DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class);variable++)
    if(bindings[variable].node_count==0||bindings[variable].nodes[0].count==0) {
        snprintf(vm->error,sizeof vm->error,
            "cannot infer generic class type parameter from constructor arguments");
        return DIAMOND_VM_TYPE_ERROR;
    }
    memcpy(instance->type_binding,bindings,DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class)*sizeof(DiamondTypeBinding));
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
    if(instance->type_binding!=nullptr&&init==nullptr)return DIAMOND_VM_TYPE_ERROR;
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
    const DiamondChunk child={.name=fn->name,.code=fn->code,.lines=fn->lines,
      .columns=fn->columns,.code_count=fn->code_count,.constants=fn->constants,
      .constant_count=fn->constant_count,.strings=fn->strings,
      .string_count=fn->string_count,.type_sets=fn->type_sets,
      .type_set_count=fn->type_set_count,.functions=chunk->functions,
      .function_count=chunk->function_count,.classes=chunk->classes,
      .class_count=chunk->class_count,.interfaces=chunk->interfaces,
      .interface_count=chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .class_type_variable=fn->class_type_variable,
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    const DiamondVmStatus status=run_chunk(&child,vm,arguments,argument_count,
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
        char text[256];
        const int written=snprintf(text,sizeof text,"%s:%u:%u",name,line,column);
        if(written<0)continue;
        const size_t length=(size_t)written<sizeof text?(size_t)written:sizeof text-1;
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
    DiamondStringConstant *dynamic_name=malloc(sizeof *dynamic_name);
    if(dynamic_name==nullptr) {
        gc_unprotect(vm,protected_count);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
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
