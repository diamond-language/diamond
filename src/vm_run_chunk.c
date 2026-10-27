#include "vm_internal.h"
#include "bignum.h"
#include "compiler.h"
#include "jit.h"
#include "tls_crypto.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef DIAMOND_NO_SQLITE
#include <sqlite3.h>
#endif
#ifndef DIAMOND_NO_POSTGRES
#include <libpq-fe.h>
#endif
#ifndef DIAMOND_NO_MYSQL
#include <mysql.h>
#endif

extern char **environ;


/* Array#push and #length are the two collection calls hot loops make, and
 * the call into collection_invoke_helper costs more than they do: its frame
 * saves every callee-saved register and spills nine arguments. This answers
 * them at the call site, with the same checks the helper makes in the same
 * order: #length comes before any extension lookup there, and #push honours
 * a user-defined Array#push exactly as the helper does. Anything unusual (an
 * arity mismatch, a frozen array, a type constraint, a cold extension cache)
 * is left for the helper, which then produces the error or fills the cache.
 * Returns true when it handled the call, with the status in *status. */
static inline bool collection_invoke_fast(DiamondVm *vm,DiamondValue *registers,
        uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,
        const DiamondStringConstant *method_name,
        DiamondObjectKind receiver_kind,DiamondVmStatus *status) {
    if(method_name->length==6&&argc==0&&
       memcmp(method_name->chars,"length",6)==0) {
        size_t length;
        if(receiver_kind==DIAMOND_OBJECT_ARRAY)
            length=((DiamondArray *)registers[recv].as.object)->count;
        else if(receiver_kind==DIAMOND_OBJECT_HASH)
            length=((DiamondHash *)registers[recv].as.object)->count;
        else length=((DiamondString *)registers[recv].as.object)->length;
        registers[dest]=DIAMOND_INT((int64_t)length);
        *status=DIAMOND_VM_OK;return true;
    }
    if(receiver_kind==DIAMOND_OBJECT_ARRAY&&method_name->length==4&&argc==1&&
       memcmp(method_name->chars,"push",4)==0) {
        DiamondArray *array=(DiamondArray *)registers[recv].as.object;
        const DiamondFunction *extension=nullptr;
        if(array->object.frozen||
           !cached_extension_lookup(vm,(const uint8_t *)(const void *)method_name,
               (uint8_t)receiver_kind,&extension)||extension!=nullptr||
           !array_value_satisfies_constraints(array,registers[base]))
            return false;
        if(!array_push(vm,array,registers[base]))
            *status=DIAMOND_VM_OUT_OF_MEMORY;
        else {
            registers[dest]=registers[recv];
            *status=DIAMOND_VM_OK;
        }
        return true;
    }
    return false;
}


DiamondVmStatus run_chunk(const DiamondChunk *chunk,
                                 DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
                                 DiamondValue *result) {
    if (depth >= diamond_call_depth_limit(vm)) {
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    DiamondChunk execution;
    /* Only the first chunk->type_variable_count entries are ever read:
     * copied from the chunk's own bindings just below, or zeroed for
     * inference in the unbound-generic branch after it. Zero-initializing
     * all eight here cost about 3 KB of memset at the top of every call,
     * generic or not. */
    DiamondTypeBinding bindings[8];
    if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr)
        memcpy(bindings,chunk->type_variable_bindings,
               chunk->type_variable_count*sizeof(DiamondTypeBinding));
    if(chunk->type_variable_count>0&&chunk->parameter_type_sets!=nullptr&&
       chunk->type_variable_bindings==nullptr) {
        memset(bindings,0,chunk->type_variable_count*sizeof(DiamondTypeBinding));
        /* Only copy `*chunk` when this generic-function-with-unbound-
         * type-variable path is actually taken -- the common case
         * (type_variable_count==0, essentially every non-generic call)
         * never reads `execution`, so skip the 168-byte struct copy. */
        execution=*chunk;
        /* parameter_type_sets holds one entry per declared parameter, at most
         * DIAMOND_MAX_DECLARED_PARAMETERS. The argument count is not bounded by
         * that (a spread call can supply any number, and the arity check comes
         * after this loop), so bound the index by the array, not the call. */
        for(size_t parameter=0;
            parameter<DIAMOND_MAX_DECLARED_PARAMETERS&&
            parameter+chunk->parameter_offset<argument_count;parameter++) {
            if(arguments[parameter+chunk->parameter_offset].kind==
               DIAMOND_VALUE_UNDEFINED)continue;
            const uint16_t set=chunk->parameter_type_sets[parameter];
            if(set!=DIAMOND_NO_TYPE_SET&&set<chunk->type_set_count)
                infer_from_value(chunk,arguments[parameter+chunk->parameter_offset],
                                 chunk->type_sets,set,bindings);
        }
        execution.type_variable_bindings=bindings;
        chunk=&execution;
    }
    if (argument_count > DIAMOND_REGISTER_COUNT) {
        return DIAMOND_VM_ARITY_ERROR;
    }
    /* Only registers ever allocated by this function body (the compiler's
     * next_register high-water mark, chunk->register_count) need zeroing --
     * allocate_register() never recycles a slot within one function body,
     * so bytecode can never reference a register past this bound.
     * register_count==0 means an unset field -- every compiler-generated
     * function has at least one register for its return value, so 0 only
     * happens for hand-authored DiamondChunk literals (e.g. tests driving
     * the C API directly) that predate this field; fall back to the full
     * width rather than silently under-zeroing/under-scanning those. */
    size_t live_register_count =
        chunk->register_count == 0 ? DIAMOND_REGISTER_COUNT : chunk->register_count;
    /* A variadic callee is allowed to receive more arguments than it has
     * registers for -- the excess never lands in an individual register
     * at all, only DIAMOND_OP_COLLECT_VARIADIC's own Array (built from
     * `arguments` directly, below, not from `registers[]`). Every other
     * callee still requires argument_count <= live_register_count, same
     * as before. */
    if (argument_count > live_register_count && !chunk->has_variadic) {
        return DIAMOND_VM_ARITY_ERROR;
    }
    /* A fixed DIAMOND_INLINE_REGISTER_COUNT-wide C-stack array covers
     * every function that fits in it (the overwhelming majority --
     * DIAMOND_REGISTER_COUNT, 4096, is a compile-time ceiling / bytecode
     * operand range, not a typical per-call need, see its own comment in
     * src/vm.h) at exactly the stack cost the DIAMOND_MAX_CALL_DEPTH
     * comment above was measured against. Only a function whose
     * live_register_count actually exceeds that inline width heap-
     * allocates instead -- see DIAMOND_INLINE_REGISTER_COUNT's own
     * comment above for why this isn't a VLA. */
    DiamondValue inline_registers[DIAMOND_INLINE_REGISTER_COUNT];
    DiamondValue *heap_registers = nullptr;
    DiamondValue *registers = inline_registers;
    if (live_register_count > DIAMOND_INLINE_REGISTER_COUNT) {
        heap_registers = malloc(live_register_count * sizeof(DiamondValue));
        if (heap_registers == nullptr) {
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        registers = heap_registers;
    }
    /* Slots `registers` can hold: a cross-function TAIL_CALL switches to a
     * callee with a different register count and grows the heap array only
     * when that count exceeds this. */
    size_t register_capacity = live_register_count > DIAMOND_INLINE_REGISTER_COUNT
        ? live_register_count : DIAMOND_INLINE_REGISTER_COUNT;
    memset(registers, 0, live_register_count * sizeof(DiamondValue));
    /* Bounded at live_register_count, not argument_count -- a variadic
     * call (see the has_variadic check above) can pass more arguments
     * than this callee has registers for; DIAMOND_OP_COLLECT_VARIADIC
     * recovers the un-copied tail directly from `arguments` itself
     * (still in scope for the rest of this function), not from
     * `registers[]`. For every non-variadic callee, argument_count is
     * already <= live_register_count by the check above, so this bound
     * changes nothing there. */
    const size_t copied_argument_count=
        argument_count<live_register_count?argument_count:live_register_count;
    for (size_t index = 0; index < copied_argument_count; index++) {
        if(arguments[index].kind!=DIAMOND_VALUE_UNDEFINED)
            registers[index] = arguments[index];
    }
    /* Argument list staged by an in-place DIAMOND_OP_TAIL_CALL; freed on exit. */
    DiamondValue *tail_buffer = nullptr;
    size_t tail_buffer_capacity = 0;
    PendingUnwind pending={};
    UnwindHandler handlers[16];
    size_t handler_count=0;
    /* ip is never address-taken, so the compiler can keep it in a register
     * across the dispatch loop. Helpers that move it (catch_exception, the
     * outlined_* bodies, ...) take &ip_slot instead; IP_CALL syncs the two
     * around each such call. */
    size_t ip = 0;
    /* The decode macros read these instead of chunk->code/code_count, so the
     * dispatch loop needs no register for `chunk`. A cross-function
     * TAIL_CALL reloads them when it switches chunks. */
    const uint8_t *bytecode = chunk->code;
    size_t bytecode_count = chunk->code_count;
    size_t ip_slot = 0;
    size_t instruction_offset = 0;
    const uint64_t frame_serial=++vm->frame_serial;
    DiamondFrame frame = {
        .previous = vm->frames,
        .registers = registers,
        .pending = &pending,
        .handlers = handlers,
        .handler_count = &handler_count,
        .register_count = live_register_count,
        .chunk = chunk,
        .instruction_offset = &instruction_offset,
        .serial = frame_serial,
        .home = closure!=nullptr&&closure->is_block&&closure->return_target!=0?
            closure->return_target:frame_serial,
    };
    vm->frames = &frame;

    #define RECORD_ERROR(status_) do {                                      \
        if ((status_) != DIAMOND_VM_OK) {                                   \
            size_t used = strlen(vm->error);                                \
            if (used == 0 && (status_) == DIAMOND_VM_ARITY_ERROR)           \
                used = describe_invoke_arity_error(vm, chunk, registers,    \
                                                   instruction_offset);     \
            if (used == 0) {                                                \
                used = (size_t)snprintf(vm->error, sizeof(vm->error), "%s", \
                                        diamond_vm_status_name(status_));    \
            }                                                               \
            diamond_record_error_line(vm, chunk, used, instruction_offset);  \
        }                                                                   \
    } while (false)

/* A bool-returning helper call that takes &ip_slot: copy ip in, copy it back
 * whatever the result. */
#define IP_CALL(call_) \
    ((ip_slot=ip,(call_))?(ip=ip_slot,true):(ip=ip_slot,false))

#define VM_RETURN(status_)                                           \
    do {                                                             \
        DiamondVmStatus return_status_=(status_);                     \
        if(return_status_==DIAMOND_VM_NONLOCAL_EXIT) {                \
            ip_slot=ip;                                              \
            const NonlocalOutcome outcome_=nonlocal_exit_arrives(vm,  \
                    chunk,&frame,registers,handlers,&handler_count,   \
                    &pending,&ip_slot,instruction_offset,result);     \
            ip=ip_slot;                                              \
            switch(outcome_) {                                       \
                case NONLOCAL_CONTINUE: goto dispatch_continue;       \
                case NONLOCAL_RETURNED: return_status_=DIAMOND_VM_OK;  \
                    break;                                           \
                case NONLOCAL_PASS_ON: break;                         \
                case NONLOCAL_INVALID:                                \
                    return_status_=DIAMOND_VM_TYPE_ERROR; break;      \
            }                                                        \
            if(return_status_!=DIAMOND_VM_TYPE_ERROR) {               \
                vm->frames = frame.previous;                         \
                free(heap_registers);                                \
                free(tail_buffer);                                   \
                return return_status_;                               \
            }                                                        \
        }                                                            \
        if(handler_count>0 && IP_CALL(catch_runtime_error(vm,chunk,   \
           return_status_,handlers,&handler_count,&pending,registers, \
           &ip_slot)))                                               \
            goto dispatch_continue;                                  \
        RECORD_ERROR(return_status_);                                \
        vm->frames = frame.previous;                                 \
        free(heap_registers);                                        \
        free(tail_buffer);                                           \
        return vm->has_exception?DIAMOND_VM_EXCEPTION:return_status_;\
    } while (false)

/* Every native/builtin pseudo-method (tap, dup, respond_to?, public_send,
 * Regexp#match,
 * ProgramBuilder's own invoke path, etc.) that doesn't declare any type
 * variables rejects an explicit generic argument list the same way -- a
 * real, if unusual, user mistake (`x.dup[Int]()`) rather than a bytecode-
 * integrity concern, unlike the analogous check against a real
 * DiamondFunction's type_variable_count (compile_call already rejects a
 * mismatched count there at compile time, so that check is defense-in-
 * depth only and doesn't need a message). `method_name_` must be a
 * `const DiamondStringConstant *` already in scope. */
#define VM_REJECT_TYPE_ARGUMENTS(method_name_)                            \
    do {                                                                  \
        snprintf(vm->error,sizeof vm->error,                              \
            "'%.*s' does not accept generic type arguments",              \
            (int)(method_name_)->length,(method_name_)->chars);           \
        VM_RETURN(DIAMOND_VM_TYPE_ERROR);                                 \
    } while (false)

/* One line, at the top of every opcode case that opens a real filesystem/
 * network/subprocess resource (File.open, TCPSocket.connect, Process.run,
 * ...), before any side effect -- see docs/sandbox.md for the full deny
 * list and why this checks getenv directly rather than a cached global or
 * a per-DiamondVm field: every execution path (the top-level program, a
 * spawned Thread's child_vm, a Supervisor child's per-attempt run_vm, a
 * ProgramBuilder#run's own run_vm) reads the same real process
 * environment, so nothing needs to propagate a flag from a parent VM to a
 * child one -- the one propagation mistake that would actually matter for
 * a security feature like this. `capability_name_` is a plain string
 * literal (a Diamond-facing name, e.g. "File.open"), not a dynamic value.
 *
 * `category_` (docs/sandbox.md's own "Per-capability granularity" section)
 * is one of "filesystem"/"network"/"database"/"subprocess", also always a
 * plain string literal -- sandbox_category_allowed (just above this
 * function) checks it against DIAMOND_SANDBOX_ALLOW, an opt-in allow-list
 * consulted only once DIAMOND_SANDBOX is already denying everything.
 * Deliberately an allow-list, not a deny-list: an unrecognized or
 * misspelled category name in DIAMOND_SANDBOX_ALLOW simply never matches,
 * leaving that capability denied (fails safe) -- the equivalent mistake in
 * a deny-list design would silently fail open instead. */
#define VM_SANDBOX_GUARD(capability_name_, category_)                     \
    do {                                                                  \
        if (getenv("DIAMOND_SANDBOX") != nullptr &&                      \
            !sandbox_category_allowed(category_)) {                       \
            snprintf(vm->error,sizeof vm->error,                          \
                "sandbox denies %s",(capability_name_));                  \
            VM_RETURN(DIAMOND_VM_SANDBOX_ERROR);                          \
        }                                                                 \
    } while (false)

#define VM_PROPAGATE(status_)                                      \
    if ((status_) != DIAMOND_VM_OK) {                              \
        if ((status_) == DIAMOND_VM_EXCEPTION &&                  \
            IP_CALL(catch_exception(vm,chunk,handlers,&handler_count,&pending,registers,&ip_slot))) {\
            break;                                                  \
        }                                                           \
        VM_RETURN(status_);                                         \
    }

/* Identical to VM_PROPAGATE except for `goto dispatch_continue` where
 * VM_PROPAGATE uses `break` -- needed anywhere this dispatch_pending_
 * signals result is checked from *inside* a retry loop nested within a
 * case block (TCPServer#accept, IO.poll, UDPSocket#receive all wrap
 * their own blocking syscall in a `while`/`for` to survive EINTR), where
 * a bare `break` would exit that inner loop rather than the switch,
 * leaving execution to fall through into code that assumes the syscall
 * actually completed. `goto` doesn't have that ambiguity -- it always
 * reaches the real dispatch_continue label regardless of how many loops
 * currently enclose the call site, which is also exactly why it's safe
 * to use for the main dispatch loop's own top-of-loop check (see below),
 * a point that isn't inside the switch at all yet. */
#define VM_PROPAGATE_SIGNAL(status_)                                \
    if ((status_) != DIAMOND_VM_OK) {                              \
        if ((status_) == DIAMOND_VM_EXCEPTION &&                  \
            IP_CALL(catch_exception(vm,chunk,handlers,&handler_count,&pending,registers,&ip_slot))) {\
            goto dispatch_continue;                                \
        }                                                           \
        VM_RETURN(status_);                                         \
    }

#define READ_BYTE(target_)                   \
    do {                                     \
        if (ip >= bytecode_count) {          \
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE); \
        }                                    \
        (target_) = bytecode[ip++];          \
    } while (false)

    /* Big-endian, matching the existing JUMP-target 16-bit operand
     * convention -- function indices (CALL/CALL_TYPED/CLOSURE) address the
     * dynamically growing function table beyond one-byte range. */
#define READ_SHORT(target_)                  \
    do {                                     \
        if (ip + 1 >= bytecode_count) {      \
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE); \
        }                                    \
        (target_) = (uint16_t)(((unsigned)bytecode[ip] << 8) | bytecode[ip + 1]); \
        ip += 2;                             \
    } while (false)

    /* One bounds check for all of an instruction's fixed operand bytes, then
     * plain reads through a local pointer. Every READ_BYTE/READ_SHORT checks
     * the bound and advances ip on its own; the hot opcodes decode this way
     * instead. */
#define READ_OPERANDS(width_)                              \
    if (ip + (width_) > bytecode_count) {                  \
        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);            \
    }                                                      \
    const uint8_t *const operands = bytecode + ip;         \
    ip += (width_)
#define OPERAND_BYTE(offset_) (operands[(offset_)])
#define OPERAND_SHORT(offset_) \
    ((uint16_t)(((unsigned)operands[(offset_)] << 8) | operands[(offset_) + 1]))

    while (ip < bytecode_count) {
        /* Cheap steady-state cost (one relaxed atomic read, almost always
         * false) for prompt signal handling in CPU-bound Diamond code
         * that never calls a blocking native function at all -- the
         * EINTR-based checks in accept/IO.poll/UDPSocket#receive below
         * cover the case where it's blocked in one of those instead. */
        if(atomic_load_explicit(&diamond_pending_signals,memory_order_relaxed)) {
            bool signal_invoked=false;
            const DiamondVmStatus signal_status=
                dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
            VM_PROPAGATE_SIGNAL(signal_status);
        }
        instruction_offset = ip;
        uint8_t instruction = 0;
        READ_BYTE(instruction);
        /* Both per-VM checks below are hinted false: the hint lets the compiler
         * move their bodies out of the hot loop, which measured 2-19% faster on
         * GCC and up to 15% on clang. Folding the two flags into one cut two
         * instructions per dispatch but was slower on GCC, so they stay apart. */
        if (__builtin_expect(vm->count_opcodes, 0) && instruction < DIAMOND_OP_COUNT)
            vm->opcode_counts[instruction]++;
        /* Cheap steady-state cost (one boolean read, false unless a
         * DIAMOND_MAX_* budget is configured) for docs/sandbox.md's own
         * "Resource limits" -- same shape as diamond_pending_signals's own
         * check just above. The instruction-count comparison itself is cheap
         * enough to run every dispatch when active; the wall-clock check is
         * additionally masked (see DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK's
         * own comment) since clock_gettime is the genuinely non-trivial part.
         *
         * A budget fires in two steps. The first time one is exceeded
         * resource_limit_trip disarms both soft budgets *before* VM_RETURN
         * (instructions_executed only ever grows and elapsed time only ever
         * increases, so leaving either armed would re-trip on the very next
         * instruction -- including every instruction a matching `rescue`/
         * `ensure` body needs, so a program that correctly catches
         * ResourceLimitError could never finish handling it) and arms the
         * hard limits instead. Past those, the program has had its grace
         * allowance: resource_hard_stop returns DIAMOND_VM_RESOURCE_EXHAUSTED,
         * which no rescue clause can match. Without the second step a program
         * that rescued the error would run unbounded. */
        if (__builtin_expect(atomic_load_explicit(&vm->resource_limits_active,
                                                  memory_order_relaxed), 0)) {
            vm->instructions_executed++;
            if (vm->interrupt_flag != nullptr &&
                atomic_load_explicit(vm->interrupt_flag, memory_order_relaxed)) {
                VM_RETURN(DIAMOND_VM_INTERRUPTED);
            }
            if (vm->resource_grace_active) {
                if (vm->hard_stop_instruction_count != 0 &&
                    vm->instructions_executed > vm->hard_stop_instruction_count) {
                    VM_RETURN(resource_hard_stop(vm, "instruction"));
                }
                if (vm->hard_stop_time_ns != 0 &&
                    (vm->instructions_executed & DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK) == 0) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    const int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
                    if (now_ns > vm->hard_stop_time_ns) {
                        VM_RETURN(resource_hard_stop(vm, "wall-clock"));
                    }
                }
            } else {
                if (vm->max_instructions != 0 &&
                    (vm->instructions_executed & DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK) == 0 &&
                    instruction_budget_exceeded(vm)) {
                    resource_limit_trip(vm);
                    VM_RETURN(DIAMOND_VM_RESOURCE_LIMIT_ERROR);
                }
                if (vm->max_wall_nanoseconds != 0 &&
                    (vm->instructions_executed & DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK) == 0) {
                    struct timespec now;
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    const int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
                    if (now_ns - vm->start_time_ns > vm->max_wall_nanoseconds) {
                        resource_limit_trip(vm);
                        VM_RETURN(DIAMOND_VM_RESOURCE_LIMIT_ERROR);
                    }
                }
            }
            if (vm->memory_recheck) {
                vm->memory_recheck = false;
                if (!maybe_collect(vm)) {
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
            }
        }

        switch ((DiamondOpCode)instruction) {
            case DIAMOND_OP_CONSTANT: {
                uint16_t destination = 0;
                uint16_t constant = 0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); constant=OPERAND_SHORT(2);
                if ((size_t)constant >= chunk->constant_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[destination] = chunk->constants[constant];
                break;
            }
            case DIAMOND_OP_STRING: {
                uint16_t destination = 0;
                uint16_t string_index = 0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); string_index=OPERAND_SHORT(2);
                if ((size_t)string_index >= chunk->string_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondStringConstant *constant =
                    &chunk->strings[string_index];
                DiamondString *string = allocate_string(
                    vm, constant->chars, constant->length);
                if (string == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination] = DIAMOND_OBJECT(string);
                break;
            }
            case DIAMOND_OP_SYMBOL: {
                uint16_t destination = 0;
                uint16_t string_index = 0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); string_index=OPERAND_SHORT(2);
                if ((size_t)string_index >= chunk->string_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondStringConstant *constant =
                    &chunk->strings[string_index];
                DiamondSymbol *symbol = allocate_symbol(
                    vm, constant->chars, constant->length);
                if (symbol == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination] = DIAMOND_OBJECT(symbol);
                break;
            }
            case DIAMOND_OP_NIL: {
                uint16_t destination = 0;
                READ_SHORT(destination);
                registers[destination] = DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_BOOL: {
                uint16_t destination = 0;
                uint16_t boolean = 0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); boolean=OPERAND_SHORT(2);
                registers[destination] = DIAMOND_BOOL(boolean != 0);
                break;
            }
            case DIAMOND_OP_ARGUMENT_PROVIDED: {
                uint16_t destination=0,index=0;
                READ_SHORT(destination);READ_SHORT(index);
                registers[destination]=DIAMOND_BOOL(index<argument_count&&
                    arguments[index].kind!=DIAMOND_VALUE_UNDEFINED);
                break;
            }
            case DIAMOND_OP_COLLECT_VARIADIC: {
                uint16_t destination=0,fixed_count=0,preserved_count=0;
                READ_SHORT(destination);READ_SHORT(fixed_count);
                READ_SHORT(preserved_count);
                /* Reads the call's original `arguments`/`argument_count`
                 * (run_chunk's own parameters, still in scope here) --
                 * not `registers[]`, which only ever receives up to
                 * min(argument_count, live_register_count) copied values
                 * now (see run_chunk's own bounds fix above). */
                if(preserved_count>1||
                   (size_t)destination+preserved_count>=chunk->register_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                size_t preserved=0;
                if(preserved_count==1&&argument_count>fixed_count) {
                    const DiamondValue candidate=arguments[argument_count-1];
                    if(candidate.kind==DIAMOND_VALUE_OBJECT&&
                       candidate.as.object->kind==DIAMOND_OBJECT_CLOSURE&&
                       ((DiamondClosure *)candidate.as.object)->is_block)
                        preserved=1;
                }
                const size_t available=argument_count-preserved;
                const size_t trailing=available>fixed_count?
                    available-fixed_count:0;
                /* `arguments` is null for a call with none (the entry function,
                 * or hand-assembled bytecode); forming &arguments[n] from it is
                 * undefined even when nothing is copied. */
                DiamondArray *variadic_array=
                    allocate_array(vm,trailing>0?&arguments[fixed_count]:nullptr,trailing);
                if(variadic_array==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(variadic_array);
                if(preserved_count==1)
                    registers[(size_t)destination+1]=preserved==1?
                        arguments[available]:DIAMOND_NIL;
                /* Arguments past the variadic (and block) slot were copied
                 * into registers the body treats as never-written temporaries
                 * -- a bare `nil` operand reads one. Only parameter setup runs
                 * before this instruction, and a spill means every fixed
                 * argument was passed, so no default fallback has read them. */
                const size_t spilled=argument_count<live_register_count?
                    argument_count:live_register_count;
                for(size_t index=(size_t)destination+preserved_count+1;
                    index<spilled;index++)
                    registers[index]=DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_TO_STRING: {
                uint16_t destination=0,source=0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); source=OPERAND_SHORT(2);
                DiamondValue converted=DIAMOND_NIL;
                DiamondVmStatus status=stringify_value(vm,chunk,depth,
                    registers[source],&converted);
                VM_PROPAGATE(status);
                registers[destination]=converted;break;
            }
            case DIAMOND_OP_PRINT: {
                uint16_t destination=0,source=0;uint8_t flags=0;
                READ_SHORT(destination);READ_SHORT(source);READ_BYTE(flags);
                const bool newline=(flags&DIAMOND_PRINT_NEWLINE)!=0;
                /* warn() writes to stderr; print/puts to stdout. */
                FILE *out=(flags&DIAMOND_PRINT_STDERR)!=0?stderr:stdout;
                DiamondValue converted=DIAMOND_NIL;
                DiamondVmStatus status=stringify_value(vm,chunk,depth,
                    registers[source],&converted);
                VM_PROPAGATE(status);
                const DiamondString *text=(const DiamondString *)converted.as.object;
                errno=0;
                const size_t written=fwrite(text->chars,1,text->length,out);
                if(written!=text->length||ferror(out)) {
                    snprintf(vm->error,sizeof vm->error,"write error: %s",strerror(errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                if(newline) {
                    errno=0;
                    if(fputc('\n',out)==EOF||ferror(out)) {
                        snprintf(vm->error,sizeof vm->error,"write error: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    /* stdout is fully buffered (not line-buffered) once
                     * it isn't a terminal -- redirected to a file, a
                     * pipe, whatever a test harness or `> log` capture
                     * uses. Without an explicit flush, puts("ready")
                     * right before blocking in a native call (accept(),
                     * IO.poll, a receive loop -- exactly the shape every
                     * readiness-signaling test in tests/run.sh and the
                     * packages test scripts uses) could sit in the
                     * buffer indefinitely: nothing forces a flush until
                     * the buffer fills or the process exits, and a
                     * process blocked waiting for someone to *see* its
                     * own "ready" line is exactly the case that never
                     * reaches either. plain print() (no trailing
                     * newline, used to build up a line incrementally)
                     * stays fully buffered -- this only fires for the
                     * puts-style, newline-terminated case, matching
                     * ordinary line-buffered-on-a-terminal behavior
                     * unconditionally rather than only when isatty(). */
                    errno=0;
                    if(fflush(out)==EOF) {
                        snprintf(vm->error,sizeof vm->error,"write error: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                }
                registers[destination]=DIAMOND_NIL;break;
            }
            case DIAMOND_OP_GETS: {
                uint16_t destination=0;
                READ_SHORT(destination);
                StringBuilder builder={};
                bool saw_any=false;
                DiamondVmStatus read_status=read_line(vm,stdin,&builder,&saw_any);
                if(read_status!=DIAMOND_VM_OK) {
                    free(builder.chars);VM_RETURN(read_status);
                }
                if(!saw_any) {
                    free(builder.chars);
                    registers[destination]=DIAMOND_NIL;break;
                }
                DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                free(builder.chars);
                if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(string);break;
            }
            case DIAMOND_OP_MOVE: {
                uint16_t destination = 0;
                uint16_t source = 0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); source=OPERAND_SHORT(2);
                registers[destination] = registers[source];
                break;
            }
            case DIAMOND_OP_ADD: {
                uint16_t destination = 0;
                uint16_t left = 0;
                uint16_t right = 0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                if (registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT) {
                    if (vm->quickening &&
                        ++vm->quickening_observations >= vm->quickening_threshold) {
                        uint8_t *code=(uint8_t *)(void *)bytecode;
                        code[instruction_offset]=(uint8_t)DIAMOND_OP_ADD_INT;
                        vm->quickened_sites++;
                    }
                    int64_t sum = 0;
                    if (ckd_add(&sum, registers[left].as.integer,
                                registers[right].as.integer)) {
                        DiamondIntView left_view, right_view;
                        diamond_int_view(registers[left],&left_view);
                        diamond_int_view(registers[right],&right_view);
                        const DiamondValue bignum_result=
                            diamond_bignum_add(vm,left_view,right_view);
                        if(bignum_result.kind==DIAMOND_VALUE_NIL)
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=bignum_result;
                        break;
                    }
                    registers[destination] = DIAMOND_INT(sum);
                    break;
                }
                DiamondValue add_result=DIAMOND_NIL;
                const DiamondVmStatus add_status=add_fallback(vm,chunk,depth,
                    instruction_offset,registers[left],registers[right],&add_result);
                VM_PROPAGATE(add_status);
                registers[destination]=add_result;
                break;
            }
            case DIAMOND_OP_SUBTRACT:
            case DIAMOND_OP_MULTIPLY:
            case DIAMOND_OP_DIVIDE:
            case DIAMOND_OP_ADD_INT:
            case DIAMOND_OP_SUBTRACT_INT:
            case DIAMOND_OP_MULTIPLY_INT:
            case DIAMOND_OP_DIVIDE_INT: {
                DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint16_t destination = 0;
                uint16_t left = 0;
                uint16_t right = 0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                if (vm->quickening &&
                    (opcode == DIAMOND_OP_SUBTRACT ||
                     opcode == DIAMOND_OP_MULTIPLY ||
                     opcode == DIAMOND_OP_DIVIDE) &&
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_SUBTRACT
                        ? DIAMOND_OP_SUBTRACT_INT
                        : opcode == DIAMOND_OP_MULTIPLY
                            ? DIAMOND_OP_MULTIPLY_INT : DIAMOND_OP_DIVIDE_INT;
                    uint8_t *code=(uint8_t *)(void *)bytecode;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                /* Fast path: one of the seven opcodes above with both
                 * operands already confirmed plain (non-bignum) Int and
                 * no overflow/zero/INT64_MIN edge case -- everything
                 * else (a mismatched or bignum operand, or a genuine
                 * overflow/div-zero/INT64_MIN case) goes through the
                 * shared slow path below (Phase 3, docs/internal/
                 * jit-design.md), shared with the JIT's own compile_
                 * binary_int_op (src/jit.c) so the two can never drift
                 * apart -- the same standing rule Phase 2e already
                 * established for EQUAL/INDEX_GET/SET. */
                if ((opcode==DIAMOND_OP_ADD_INT||opcode==DIAMOND_OP_SUBTRACT_INT||
                     opcode==DIAMOND_OP_MULTIPLY_INT||opcode==DIAMOND_OP_DIVIDE_INT)&&
                    registers[left].kind==DIAMOND_VALUE_INT&&
                    registers[right].kind==DIAMOND_VALUE_INT) {
                    const int64_t left_value=registers[left].as.integer;
                    const int64_t right_value=registers[right].as.integer;
                    int64_t result_value=0;
                    bool overflow=false;
                    if(opcode==DIAMOND_OP_ADD_INT)
                        overflow=ckd_add(&result_value,left_value,right_value);
                    else if(opcode==DIAMOND_OP_SUBTRACT_INT)
                        overflow=ckd_sub(&result_value,left_value,right_value);
                    else if(opcode==DIAMOND_OP_MULTIPLY_INT)
                        overflow=ckd_mul(&result_value,left_value,right_value);
                    if(opcode!=DIAMOND_OP_DIVIDE_INT&&!overflow) {
                        registers[destination]=DIAMOND_INT(result_value);
                        break;
                    }
                    if(opcode==DIAMOND_OP_DIVIDE_INT&&right_value!=0&&
                       !(left_value==INT64_MIN&&right_value==-1)) {
                        registers[destination]=DIAMOND_INT(left_value/right_value);
                        break;
                    }
                }
                DiamondValue slow_result=DIAMOND_NIL;
                const DiamondVmStatus slow_status=int_arith_slow(vm,chunk,depth,
                    instruction_offset,opcode,registers[left],registers[right],&slow_result);
                VM_PROPAGATE(slow_status);
                registers[destination]=slow_result;
                break;
            }
            case DIAMOND_OP_SHIFT_LEFT: {
                /* Int << Int: bitwise left shift. Array << value: push and
                 * return the array itself (Ruby's append idiom). Not a
                 * user-overloadable operator (see docs/syntax.md) -- no
                 * invoke_operator_method dispatch, unlike the arithmetic
                 * operators, since it's native-only on these two types by
                 * design. No quickening/bignum-shift support: kept
                 * deliberately simple, unlike ADD/SUBTRACT/MULTIPLY/DIVIDE,
                 * since `<<` is rarely a hot-loop operator the way
                 * arithmetic is. */
                uint16_t destination=0,left=0,right=0;
                READ_SHORT(destination);READ_SHORT(left);READ_SHORT(right);
                if(is_int_value(registers[left])&&is_int_value(registers[right])) {
                    if(value_is_bignum(registers[right])||registers[right].as.integer<0||
                       registers[right].as.integer>DIAMOND_MAX_SHIFT_BITS) {
                        snprintf(vm->error,sizeof vm->error,
                            "shift amount must be between 0 and %d",
                            (int)DIAMOND_MAX_SHIFT_BITS);
                        VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                    }
                    const int64_t shift_amount=registers[right].as.integer;
                    if(registers[left].kind==DIAMOND_VALUE_INT&&shift_amount<64) {
                        const int64_t left_value=registers[left].as.integer;
                        const int64_t result_value=(int64_t)
                            ((uint64_t)left_value<<(unsigned)shift_amount);
                        if((result_value>>(unsigned)shift_amount)==left_value) {
                            registers[destination]=DIAMOND_INT(result_value);
                            break;
                        }
                    }
                    /* Overflowed 64 bits, or already a bignum: the result
                     * promotes like every other Int operation. */
                    DiamondIntView left_view;
                    diamond_int_view(registers[left],&left_view);
                    const DiamondValue shifted=diamond_bignum_shift_left(vm,
                        left_view,(uint64_t)shift_amount);
                    if(shifted.kind==DIAMOND_VALUE_NIL)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=shifted;
                    break;
                }
                if(registers[left].kind==DIAMOND_VALUE_OBJECT&&
                   registers[left].as.object->kind==DIAMOND_OBJECT_ARRAY) {
                    DiamondArray *array=(DiamondArray *)registers[left].as.object;
                    if(!array_value_satisfies_constraints(array,registers[right])) {
                        snprintf(vm->error,sizeof vm->error,
                                 "array element violates its type annotation");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(!array_push(vm,array,registers[right]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=registers[left];break;
                }
                snprintf(vm->error,sizeof vm->error,
                    "'<<' expects an Int shift amount or a value to push onto an Array");
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
            case DIAMOND_OP_SHIFT_RIGHT: {
                /* Int only (a bignum on either side is handled below, not
                 * user-overloadable, no quickening; see SHIFT_LEFT). Arithmetic (sign-extending),
                 * matching Ruby's own Integer#>>, and well-defined for a
                 * negative left-hand side under C23 (this project's own
                 * -std=c23) -- not the "implementation-defined" territory
                 * an older C standard would put a signed right-shift in. */
                uint16_t destination=0,left=0,right=0;
                READ_SHORT(destination);READ_SHORT(left);READ_SHORT(right);
                if(!is_int_value(registers[left])||!is_int_value(registers[right])) {
                    format_operator_type_error(vm,registers[left],registers[right],">>");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(value_is_bignum(registers[right])||registers[right].as.integer<0) {
                    snprintf(vm->error,sizeof vm->error,
                        "shift amount must not be negative");
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                const int64_t shift_amount=registers[right].as.integer;
                if(registers[left].kind==DIAMOND_VALUE_INT) {
                    /* Arithmetic shift; one past the width keeps only the sign. */
                    const int64_t left_value=registers[left].as.integer;
                    registers[destination]=DIAMOND_INT(shift_amount>=64?
                        (left_value<0?-1:0):left_value>>(unsigned)shift_amount);
                    break;
                }
                DiamondIntView left_view;
                diamond_int_view(registers[left],&left_view);
                const DiamondValue shifted=diamond_bignum_shift_right(vm,left_view,
                    (uint64_t)shift_amount);
                if(shifted.kind==DIAMOND_VALUE_NIL)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=shifted;
                break;
            }
            case DIAMOND_OP_BITWISE_AND:
            case DIAMOND_OP_BITWISE_OR:
            case DIAMOND_OP_BITWISE_XOR: {
                /* Int only, same scope cut as SHIFT_LEFT/SHIFT_RIGHT
                 * above. All three share one case body -- the only
                 * difference between them is which C operator runs.
                 * `bitwise_opcode` is this local case body's own copy of
                 * the switch's controlling value (the switch itself
                 * dispatches on a bare `instruction`, not a variable
                 * named `opcode` -- unlike DIAMOND_OP_EQUAL's own case,
                 * which does declare a local `opcode`, this one didn't
                 * need to until now). */
                const DiamondOpCode bitwise_opcode=(DiamondOpCode)instruction;
                uint16_t destination=0,left=0,right=0;
                READ_SHORT(destination);READ_SHORT(left);READ_SHORT(right);
                if(registers[left].kind!=DIAMOND_VALUE_INT||
                   registers[right].kind!=DIAMOND_VALUE_INT) {
                    if(is_int_value(registers[left])&&is_int_value(registers[right])) {
                        DiamondIntView left_view,right_view;
                        diamond_int_view(registers[left],&left_view);
                        diamond_int_view(registers[right],&right_view);
                        const DiamondValue combined=diamond_bignum_bitwise(vm,
                            bitwise_opcode==DIAMOND_OP_BITWISE_AND?DIAMOND_BIGNUM_BITWISE_AND:
                            bitwise_opcode==DIAMOND_OP_BITWISE_OR?DIAMOND_BIGNUM_BITWISE_OR:
                            DIAMOND_BIGNUM_BITWISE_XOR,left_view,right_view);
                        if(combined.kind==DIAMOND_VALUE_NIL)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=combined;break;
                    }
                    const char *name=bitwise_opcode==DIAMOND_OP_BITWISE_AND?"&":
                        bitwise_opcode==DIAMOND_OP_BITWISE_OR?"|":"^";
                    DiamondValue prelude_result=DIAMOND_NIL;
                    DiamondVmStatus prelude_status=DIAMOND_VM_OK;
                    if(bitwise_opcode!=DIAMOND_OP_BITWISE_XOR&&
                       operator_prelude_fallback(vm,chunk,depth,registers[left],
                           registers[right],bitwise_opcode==DIAMOND_OP_BITWISE_AND?
                           "and":"or",&prelude_result,&prelude_status)) {
                        VM_PROPAGATE(prelude_status);
                        registers[destination]=prelude_result;break;
                    }
                    format_operator_type_error(vm,registers[left],registers[right],name);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t a=registers[left].as.integer;
                const int64_t b=registers[right].as.integer;
                const int64_t bitwise_result=bitwise_opcode==DIAMOND_OP_BITWISE_AND?(a&b):
                    bitwise_opcode==DIAMOND_OP_BITWISE_OR?(a|b):(a^b);
                registers[destination]=DIAMOND_INT(bitwise_result);
                break;
            }
            case DIAMOND_OP_CLASS_NAME: {
                uint16_t destination=0,source=0;
                READ_SHORT(destination);READ_SHORT(source);
                char name[80];
                diamond_format_value_type(name,sizeof name,registers[source]);
                DiamondString *class_name=allocate_string(vm,name,strlen(name));
                if(class_name==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(class_name);
                break;
            }
            case DIAMOND_OP_MODULO: {
                /* Floored modulo (result takes the divisor's sign),
                 * matching Ruby -- not C's truncating `%` (which takes
                 * the dividend's sign). User-overloadable like the other
                 * arithmetic operators (unlike `<<`): an Instance left
                 * operand tries a `%` method the same way ADD/SUBTRACT/
                 * etc. do. Arbitrary-precision Ints are supported through
                 * diamond_bignum_modulo_floored (the remainder falls out
                 * of the same limb division `/` uses). */
                uint16_t destination=0,left=0,right=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                if(registers[left].kind==DIAMOND_VALUE_INT&&
                   registers[right].kind==DIAMOND_VALUE_INT&&
                   !value_is_bignum(registers[left])&&
                   !value_is_bignum(registers[right])) {
                    const int64_t left_value=registers[left].as.integer;
                    const int64_t right_value=registers[right].as.integer;
                    if(right_value==0)VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                    int64_t remainder=0;
                    if(left_value==INT64_MIN&&right_value==-1) {
                        /* INT64_MIN / -1 overflows int64_t (traps on some
                         * platforms) -- but mathematically -1 divides
                         * everything evenly, so the true remainder is
                         * always 0 regardless, no division needed. */
                        remainder=0;
                    } else {
                        remainder=left_value%right_value;
                        if(remainder!=0&&((remainder<0)!=(right_value<0)))
                            remainder+=right_value;
                    }
                    registers[destination]=DIAMOND_INT(remainder);
                    break;
                }
                if(is_int_value(registers[left])&&is_int_value(registers[right])) {
                    /* Reaching here with two Ints means at least one is a
                     * bignum (the plain-int64 case broke out above). */
                    DiamondIntView left_view, right_view, zero_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    diamond_int_view_int64(0,&zero_view);
                    if(diamond_bignum_compare(right_view,zero_view)==0)
                        VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                    const DiamondValue modulus=
                        diamond_bignum_modulo_floored(vm,left_view,right_view);
                    if(modulus.kind==DIAMOND_VALUE_NIL)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=modulus;break;
                }
                if((registers[left].kind==DIAMOND_VALUE_FLOAT||
                    registers[left].kind==DIAMOND_VALUE_INT)&&
                   (registers[right].kind==DIAMOND_VALUE_FLOAT||
                    registers[right].kind==DIAMOND_VALUE_INT)&&
                   (registers[left].kind==DIAMOND_VALUE_FLOAT||
                    registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    const double left_real=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_real=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    double remainder=fmod(left_real,right_real);
                    if(remainder!=0&&((remainder<0)!=(right_real<0)))
                        remainder+=right_real;
                    registers[destination]=DIAMOND_FLOAT(remainder);
                    break;
                }
                if(registers[left].kind==DIAMOND_VALUE_OBJECT&&
                   registers[left].as.object->kind==DIAMOND_OBJECT_STRING) {
                    const DiamondVmStatus percent_status=string_percent_helper(vm,chunk,
                        depth,registers,left,right,destination);
                    VM_PROPAGATE(percent_status);
                    break;
                }
                if(registers[left].kind==DIAMOND_VALUE_OBJECT&&
                   registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=bytecode+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[left].as.object,
                        "%",1,&registers[right],1,&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;break;
                    }
                }
                format_operator_type_error(vm,registers[left],registers[right],"%");
                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
            }
            case DIAMOND_OP_NEGATE: {
                uint16_t destination = 0;
                uint16_t operand = 0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); operand=OPERAND_SHORT(2);
                if (registers[operand].kind == DIAMOND_VALUE_FLOAT) {
                    registers[destination] =
                        DIAMOND_FLOAT(-registers[operand].as.real);
                    break;
                }
                if (value_is_bignum(registers[operand])) {
                    DiamondIntView operand_view;
                    diamond_int_view(registers[operand],&operand_view);
                    const DiamondValue bignum_result=
                        diamond_bignum_negate(vm,operand_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                if (registers[operand].kind==DIAMOND_VALUE_OBJECT &&
                    registers[operand].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=bytecode+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[operand].as.object,
                        "negate",6,nullptr,0,&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;
                        break;
                    }
                }
                if (registers[operand].kind != DIAMOND_VALUE_INT) {
                    char actual[80];
                    diamond_format_value_type(actual,sizeof actual,registers[operand]);
                    snprintf(vm->error,sizeof vm->error,
                        "undefined method 'negate' for %s",actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                int64_t result_value = 0;
                if (ckd_sub(&result_value, 0, registers[operand].as.integer)) {
                    DiamondIntView operand_view;
                    diamond_int_view(registers[operand],&operand_view);
                    const DiamondValue bignum_result=
                        diamond_bignum_negate(vm,operand_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                registers[destination] = DIAMOND_INT(result_value);
                break;
            }
            case DIAMOND_OP_EQUAL:
            case DIAMOND_OP_NOT_EQUAL:
            case DIAMOND_OP_EQUAL_INT:
            case DIAMOND_OP_NOT_EQUAL_INT: {
                DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint16_t destination = 0;
                uint16_t left = 0;
                uint16_t right = 0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                const bool integer_operands =
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT;
                if (vm->quickening && integer_operands &&
                    (opcode == DIAMOND_OP_EQUAL || opcode == DIAMOND_OP_NOT_EQUAL) &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_EQUAL
                        ? DIAMOND_OP_EQUAL_INT : DIAMOND_OP_NOT_EQUAL_INT;
                    uint8_t *code=(uint8_t *)(void *)bytecode;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                if ((opcode == DIAMOND_OP_EQUAL_INT ||
                     opcode == DIAMOND_OP_NOT_EQUAL_INT) && !integer_operands) {
                    uint8_t *code=(uint8_t *)(void *)bytecode;
                    code[instruction_offset]=(uint8_t)(opcode == DIAMOND_OP_EQUAL_INT
                        ? DIAMOND_OP_EQUAL : DIAMOND_OP_NOT_EQUAL);
                    vm->deoptimized_sites++;
                    opcode=(DiamondOpCode)code[instruction_offset];
                }
                if ((opcode == DIAMOND_OP_EQUAL_INT ||
                     opcode == DIAMOND_OP_NOT_EQUAL_INT) && integer_operands) {
                    const bool equal = registers[left].as.integer ==
                        registers[right].as.integer;
                    registers[destination] = DIAMOND_BOOL(
                        opcode == DIAMOND_OP_EQUAL_INT ? equal : !equal);
                    break;
                }
                /* By this point opcode is guaranteed plain EQUAL/NOT_EQUAL
                 * (never the _INT forms -- either integer_operands was
                 * true and the fast path above already broke out, or the
                 * deopt just above already retargeted). No override found
                 * falls through to values_equal unchanged, so two
                 * instances of a class with no "==" compare by identity
                 * exactly as before this feature existed. */
                {
                    const uint8_t *site=bytecode+instruction_offset;
                    DiamondValue general_result=DIAMOND_NIL;
                    const DiamondVmStatus status=diamond_jit_equal_general(vm,chunk,depth,
                        site,&registers[left],&registers[right],
                        opcode==DIAMOND_OP_NOT_EQUAL,&general_result);
                    VM_PROPAGATE(status);
                    registers[destination]=general_result;
                }
                break;
            }
            case DIAMOND_OP_LESS:
            case DIAMOND_OP_LESS_EQUAL:
            case DIAMOND_OP_GREATER:
            case DIAMOND_OP_GREATER_EQUAL:
            case DIAMOND_OP_LESS_INT:
            case DIAMOND_OP_LESS_EQUAL_INT:
            case DIAMOND_OP_GREATER_INT:
            case DIAMOND_OP_GREATER_EQUAL_INT: {
                DiamondOpCode opcode = (DiamondOpCode)instruction;
                uint16_t destination = 0;
                uint16_t left = 0;
                uint16_t right = 0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                if (vm->quickening &&
                    (opcode == DIAMOND_OP_LESS ||
                     opcode == DIAMOND_OP_LESS_EQUAL ||
                     opcode == DIAMOND_OP_GREATER ||
                     opcode == DIAMOND_OP_GREATER_EQUAL) &&
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_LESS
                        ? DIAMOND_OP_LESS_INT
                        : opcode == DIAMOND_OP_LESS_EQUAL
                            ? DIAMOND_OP_LESS_EQUAL_INT
                            : opcode == DIAMOND_OP_GREATER
                                ? DIAMOND_OP_GREATER_INT
                                : DIAMOND_OP_GREATER_EQUAL_INT;
                    uint8_t *code=(uint8_t *)(void *)bytecode;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                /* Fast path: both operands already confirmed plain
                 * (non-bignum) Int, generic or _INT form alike -- a
                 * comparison never overflows, so a mismatched or bignum
                 * operand is the only reason to fall through to the
                 * shared slow path below (Phase 3, docs/internal/
                 * jit-design.md), shared with the JIT's own compile_
                 * binary_int_op (src/jit.c). */
                if ((opcode==DIAMOND_OP_LESS_INT||opcode==DIAMOND_OP_LESS_EQUAL_INT||
                     opcode==DIAMOND_OP_GREATER_INT||opcode==DIAMOND_OP_GREATER_EQUAL_INT||
                     opcode==DIAMOND_OP_LESS||opcode==DIAMOND_OP_LESS_EQUAL||
                     opcode==DIAMOND_OP_GREATER||opcode==DIAMOND_OP_GREATER_EQUAL)&&
                    registers[left].kind==DIAMOND_VALUE_INT&&
                    registers[right].kind==DIAMOND_VALUE_INT) {
                    const int64_t a=registers[left].as.integer;
                    const int64_t b=registers[right].as.integer;
                    bool comparison=false;
                    if(opcode==DIAMOND_OP_LESS_INT||opcode==DIAMOND_OP_LESS)comparison=a<b;
                    else if(opcode==DIAMOND_OP_LESS_EQUAL_INT||opcode==DIAMOND_OP_LESS_EQUAL)
                        comparison=a<=b;
                    else if(opcode==DIAMOND_OP_GREATER_INT||opcode==DIAMOND_OP_GREATER)
                        comparison=a>b;
                    else comparison=a>=b;
                    registers[destination]=DIAMOND_BOOL(comparison);
                    break;
                }
                DiamondValue slow_result=DIAMOND_NIL;
                const DiamondVmStatus slow_status=compare_int_slow(vm,chunk,depth,
                    instruction_offset,opcode,registers[left],registers[right],&slow_result);
                VM_PROPAGATE(slow_status);
                registers[destination]=slow_result;
                break;
            }
            /* `<=>` -- unlike LESS/GREATER/EQUAL above, the result is an
             * Int (-1/0/1) or Nil, never a Bool, and an unorderable pair
             * (no `<=>` method found, an incomparable native type, or
             * either Float operand is NaN) is Nil rather than a raised
             * TypeError -- matching Ruby's own `<=>` contract, which is
             * why Comparable's own derived `<` (`(self <=> other) < 0`)
             * still ends up raising for a genuinely incomparable pair, on
             * the next comparison rather than a bespoke error path here.
             * Deliberately no _INT quickening variant (see this feature's
             * own design doc). String now supports both `<=>` and
             * LESS/LESS_EQUAL/GREATER/GREATER_EQUAL (diamond_string_compare,
             * byte-lexicographic) -- this comment used to note that gap as
             * a deliberate scope cut; it wasn't sustainable (see e.g.
             * packages/active_record, packages/graphql, and examples/
             * transformer's own workarounds sorting by an extracted Int
             * key instead). Time keeps its own separate working
             * comparisons untouched. */
            case DIAMOND_OP_COMPARE: {
                uint16_t destination=0,left=0,right=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                if(is_int_value(registers[left])&&is_int_value(registers[right])) {
                    if(value_is_bignum(registers[left])||value_is_bignum(registers[right])) {
                        DiamondIntView left_view,right_view;
                        diamond_int_view(registers[left],&left_view);
                        diamond_int_view(registers[right],&right_view);
                        const int comparison=diamond_bignum_compare(left_view,right_view);
                        registers[destination]=
                            DIAMOND_INT(comparison<0?-1:comparison>0?1:0);
                        break;
                    }
                    const int64_t a=registers[left].as.integer;
                    const int64_t b=registers[right].as.integer;
                    registers[destination]=DIAMOND_INT(a<b?-1:(a>b?1:0));
                    break;
                }
                if((registers[left].kind==DIAMOND_VALUE_FLOAT||
                    registers[left].kind==DIAMOND_VALUE_INT)&&
                   (registers[right].kind==DIAMOND_VALUE_FLOAT||
                    registers[right].kind==DIAMOND_VALUE_INT)&&
                   (registers[left].kind==DIAMOND_VALUE_FLOAT||
                    registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    const double left_real=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_real=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    if(isnan(left_real)||isnan(right_real)) {
                        registers[destination]=DIAMOND_NIL;break;
                    }
                    registers[destination]=DIAMOND_INT(
                        left_real<right_real?-1:(left_real>right_real?1:0));
                    break;
                }
                if(registers[left].kind==DIAMOND_VALUE_OBJECT&&
                   registers[left].as.object->kind==DIAMOND_OBJECT_STRING&&
                   registers[right].kind==DIAMOND_VALUE_OBJECT&&
                   registers[right].as.object->kind==DIAMOND_OBJECT_STRING) {
                    const int comparison=diamond_string_compare(
                        (const DiamondString *)registers[left].as.object,
                        (const DiamondString *)registers[right].as.object);
                    registers[destination]=DIAMOND_INT(comparison<0?-1:(comparison>0?1:0));
                    break;
                }
                if(registers[left].kind==DIAMOND_VALUE_OBJECT&&
                   registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=bytecode+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[left].as.object,
                        "<=>",3,&registers[right],1,&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;break;
                    }
                }
                registers[destination]=DIAMOND_NIL;break;
            }
            case DIAMOND_OP_CASE_MATCH: {
                uint16_t destination=0,pattern=0,subject=0;
                READ_SHORT(destination);READ_SHORT(pattern);READ_SHORT(subject);
                bool matched=false;
                const DiamondVmStatus status=case_match_value(vm,chunk,depth,
                    bytecode+instruction_offset,registers[pattern],
                    registers[subject],&matched);
                VM_PROPAGATE(status);
                registers[destination]=DIAMOND_BOOL(matched);break;
            }
            case DIAMOND_OP_JUMP: {
                uint8_t high = 0;
                uint8_t low = 0;
                READ_OPERANDS(2);
                high=OPERAND_BYTE(0); low=OPERAND_BYTE(1);
                const size_t target = ((size_t)high << 8) | low;
                if (target > bytecode_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                ip = target;
                break;
            }
            case DIAMOND_OP_JUMP_IF_FALSE: {
                uint16_t condition = 0;
                uint8_t high = 0;
                uint8_t low = 0;
                READ_OPERANDS(4);
                condition=OPERAND_SHORT(0); high=OPERAND_BYTE(2); low=OPERAND_BYTE(3);
                const size_t target = ((size_t)high << 8) | low;
                if (target > bytecode_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                if (!is_truthy(registers[condition])) {
                    ip = target;
                }
                break;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: {
                uint16_t condition=0;uint8_t high=0,low=0;
                READ_OPERANDS(4);
                condition=OPERAND_SHORT(0); high=OPERAND_BYTE(2); low=OPERAND_BYTE(3);
                const size_t target=((size_t)high<<8)|low;
                if(target>bytecode_count) VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(is_truthy(registers[condition])) ip=target;
                break;
            }
            case DIAMOND_OP_CALL: {
                uint16_t destination = 0;
                uint16_t function_index = 0;
                uint16_t argument_base = 0;
                uint8_t call_argument_count = 0;
                READ_OPERANDS(7);
                destination=OPERAND_SHORT(0);
                function_index=OPERAND_SHORT(2);
                argument_base=OPERAND_SHORT(4);
                call_argument_count=OPERAND_BYTE(6);
                if ((size_t)function_index >= chunk->function_count ||
                    (size_t)argument_base + call_argument_count >
                        DIAMOND_REGISTER_COUNT) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondFunction *function =
                    chunk->functions[function_index];
                if (call_argument_count < function->required_arity||
                    (call_argument_count > function->arity &&
                     !function->has_variadic)) {
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                const DiamondChunk called_chunk =
                    plain_call_chunk(function, chunk);
                DiamondValue call_result = DIAMOND_NIL;
                const DiamondVmStatus status = jit_call_or_interpret(vm, function,
                    &called_chunk, &registers[argument_base], call_argument_count,
                    depth, nullptr, &call_result);
                VM_PROPAGATE(status);
                registers[destination] = call_result;
                break;
            }
            case DIAMOND_OP_TAIL_CALL: {
                /* Produced only by the compiler rewriting a plain CALL in
                 * tail position (maybe_rewrite_self_tail_call and
                 * rewrite_cyclic_tail_calls, src/compiler.c), whose result
                 * was already checked to be the RETURN's value. Either the
                 * function's own call to itself, which reuses the chunk and
                 * registers as they are, or a call to a different function
                 * on a cycle of such calls (mutual recursion), which swaps
                 * in that function's chunk and register count below.
                 * Bounds/arity were enforced when the CALL compiled; the
                 * checks here are the same defensive ones CALL's case makes,
                 * in case this was hand-built (ProgramBuilder exposes raw
                 * opcode numbers). */
                uint16_t destination = 0, function_index = 0, argument_base = 0;
                uint8_t call_argument_count = 0;
                READ_OPERANDS(7);
                destination=OPERAND_SHORT(0);
                function_index=OPERAND_SHORT(2);
                argument_base=OPERAND_SHORT(4);
                call_argument_count=OPERAND_BYTE(6);
                if ((size_t)argument_base + call_argument_count >
                        DIAMOND_REGISTER_COUNT) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                const DiamondFunction *tail_target =
                    (size_t)function_index < chunk->function_count
                        ? chunk->functions[function_index] : nullptr;
                const bool cross_function =
                    tail_target != nullptr && tail_target->code != bytecode;
                size_t next_register_count = live_register_count;
                DiamondChunk next_chunk;
                if (cross_function) {
                    if (call_argument_count < tail_target->required_arity ||
                        (call_argument_count > tail_target->arity &&
                         !tail_target->has_variadic)) {
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    }
                    next_chunk = plain_call_chunk(tail_target, chunk);
                    next_register_count = next_chunk.register_count == 0
                        ? DIAMOND_REGISTER_COUNT : next_chunk.register_count;
                    if (closure != nullptr || handler_count > 0 ||
                        (call_argument_count > next_register_count &&
                         !tail_target->has_variadic)) {
                        /* A block's frame, a live handler, or an argument
                         * list the callee's registers cannot hold: an
                         * ordinary call, which the RETURN right after it
                         * completes. */
                        DiamondValue call_result = DIAMOND_NIL;
                        const DiamondVmStatus status = jit_call_or_interpret(
                            vm, tail_target, &next_chunk,
                            &registers[argument_base], call_argument_count,
                            depth, nullptr, &call_result);
                        VM_PROPAGATE(status);
                        registers[destination] = call_result;
                        break;
                    }
                } else if (call_argument_count > DIAMOND_MAX_DECLARED_PARAMETERS &&
                           !chunk->has_variadic) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                /* The next iteration's argument list replaces this one's:
                 * ARGUMENT_PROVIDED (a default parameter left out of the
                 * tail call), COLLECT_VARIADIC (overflow arguments) and
                 * generic inference all read `arguments`/`argument_count`.
                 * Staged into a frame-owned heap buffer, not the register
                 * file (cleared just below) and not the C stack (a
                 * variadic call may pass up to 255); the frame lists it as
                 * extra GC roots because overflow arguments live nowhere
                 * else. Allocation failure happens before any state
                 * changes. */
                if ((size_t)call_argument_count > tail_buffer_capacity) {
                    DiamondValue *grown = realloc(tail_buffer,
                        (size_t)call_argument_count * sizeof(DiamondValue));
                    if (grown == nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    tail_buffer = grown;
                    tail_buffer_capacity = call_argument_count;
                    frame.extra_roots = tail_buffer;
                }
                for (size_t index = 0; index < call_argument_count; index++)
                    tail_buffer[index] = registers[argument_base + index];
                frame.extra_root_count = call_argument_count;
                if (cross_function) {
                    /* Last fallible step, before any state changes. */
                    if (next_register_count > register_capacity) {
                        DiamondValue *grown_registers =
                            malloc(next_register_count * sizeof(DiamondValue));
                        if (grown_registers == nullptr)
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        free(heap_registers);
                        heap_registers = grown_registers;
                        registers = grown_registers;
                        register_capacity = next_register_count;
                        frame.registers = registers;
                    }
                    live_register_count = next_register_count;
                    frame.register_count = live_register_count;
                    execution = next_chunk;
                    chunk = &execution;
                    frame.chunk = chunk;
                    bytecode = chunk->code;
                    bytecode_count = chunk->code_count;
                }
                arguments = tail_buffer;
                argument_count = call_argument_count;
                memset(registers, 0, live_register_count * sizeof(DiamondValue));
                /* The same copy a fresh call makes at entry. */
                const size_t copied = (size_t)call_argument_count < live_register_count
                    ? (size_t)call_argument_count : live_register_count;
                for (size_t index = 0; index < copied; index++)
                    if (tail_buffer[index].kind != DIAMOND_VALUE_UNDEFINED)
                        registers[index] = tail_buffer[index];
                if (chunk->type_variable_count > 0 && chunk->parameter_type_sets != nullptr) {
                    /* Bindings are derived from the arguments of every call, as at
                     * entry, so the next iteration may bind different types. */
                    if (chunk != &execution) execution = *chunk;
                    memset(bindings, 0, chunk->type_variable_count * sizeof(DiamondTypeBinding));
                    for (size_t parameter = 0;
                         parameter < DIAMOND_MAX_DECLARED_PARAMETERS &&
                         parameter + chunk->parameter_offset < argument_count;
                         parameter++) {
                        if (arguments[parameter + chunk->parameter_offset].kind ==
                            DIAMOND_VALUE_UNDEFINED) continue;
                        const uint16_t set = chunk->parameter_type_sets[parameter];
                        if (set != DIAMOND_NO_TYPE_SET && set < chunk->type_set_count)
                            infer_from_value(chunk,
                                arguments[parameter + chunk->parameter_offset],
                                chunk->type_sets, set, bindings);
                    }
                    execution.type_variable_bindings = bindings;
                    chunk = &execution;
                    frame.chunk = chunk;
                }
                ip = 0;
                break;
            }
            case DIAMOND_OP_CALL_TYPED: {
                uint16_t destination=0,argument_base=0;
                uint16_t function_index=0;
                uint8_t call_argument_count=0,type_argument_count=0;
                READ_OPERANDS(8);
                destination=OPERAND_SHORT(0);
                function_index=OPERAND_SHORT(2);
                argument_base=OPERAND_SHORT(4);
                call_argument_count=OPERAND_BYTE(6);
                type_argument_count=OPERAND_BYTE(7);
                if((size_t)function_index>=chunk->function_count||
                   (size_t)argument_base+call_argument_count>DIAMOND_REGISTER_COUNT||
                   type_argument_count>8)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *function=chunk->functions[function_index];
                if(type_argument_count!=function->type_variable_count||
                   call_argument_count<function->required_arity||
                   (call_argument_count>function->arity && !function->has_variadic))
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                /* Only the first type_argument_count entries are ever read (below,
                 * and through .type_variable_bindings); zeroing all eight cost about
                 * 3 KB of memset on every call, typed or not. */
                DiamondTypeBinding explicit_bindings[8];
                for(size_t index=0;index<type_argument_count;index++) {
                    explicit_bindings[index]=(DiamondTypeBinding){};
                    uint16_t set_index=0;READ_SHORT(set_index);
                    if((size_t)set_index>=chunk->type_set_count)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                                     chunk->type_sets,set_index);
                }
                const DiamondChunk called_chunk={.name=function->name,
                    .code=function->code,.lines=function->lines,
                    .columns=function->columns,.code_count=function->code_count,
                    .constants=function->constants,
                    .constant_count=function->constant_count,
                    .strings=function->strings,.string_count=function->string_count,
                    .type_sets=function->type_sets,
                    .type_set_count=function->type_set_count,
                    .functions=chunk->functions,.function_count=chunk->function_count,
                    .classes=chunk->classes,.class_count=chunk->class_count,
                    .interfaces=chunk->interfaces,
                    .interface_count=chunk->interface_count,
                    .parameter_type_sets=function->parameter_type_sets,
                    .type_variable_count=function->type_variable_count,
                    .parameter_offset=function->owner_class==DIAMOND_OWNER_NONE?0:1,
                    .type_variable_bindings=explicit_bindings,
                    .register_count=function->register_count,
                    .has_variadic=function->has_variadic};
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&called_chunk,vm,
                    &registers[argument_base],call_argument_count,depth+1,nullptr,
                    &call_result);
                VM_PROPAGATE(status);
                registers[destination]=call_result;
                break;
            }
            case DIAMOND_OP_CALL_SPREAD:
            case DIAMOND_OP_CALL_TYPED_SPREAD:
            case DIAMOND_OP_CALL_KEYWORD_SPREAD:
            case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_call_spread(vm,chunk,registers,depth,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_BUILD_SPREAD_ARGS: {
                uint16_t destination=0,prefix_base=0,spread_register=0,
                    suffix_base=0;
                uint8_t prefix_count=0,suffix_count=0;
                READ_SHORT(destination);READ_SHORT(prefix_base);
                READ_BYTE(prefix_count);READ_SHORT(spread_register);
                READ_SHORT(suffix_base);READ_BYTE(suffix_count);
                const bool optional_block=(suffix_count&0x80u)!=0;
                suffix_count&=0x7fu;
                if(optional_block&&suffix_count==0)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if((size_t)prefix_base+prefix_count>DIAMOND_REGISTER_COUNT||
                   (size_t)suffix_base+suffix_count>DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *spread=(const DiamondArray *)
                    registers[spread_register].as.object;
                DiamondArray *combined=allocate_array(vm,nullptr,0);
                if(combined==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(combined);
                for(size_t index=0;index<prefix_count;index++)
                    if(!array_push(vm,combined,
                            registers[(size_t)prefix_base+index]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                for(size_t index=0;index<spread->count;index++)
                    if(!array_push(vm,combined,spread->values[index]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                for(size_t index=0;index<suffix_count;index++) {
                    if(optional_block&&index+1==suffix_count&&
                       registers[(size_t)suffix_base+index].kind==
                           DIAMOND_VALUE_NIL)
                        continue;
                    if(!array_push(vm,combined,
                            registers[(size_t)suffix_base+index]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                break;
            }
            case DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
            case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_call_singleton_keywords(vm,chunk,registers,depth,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_CALL_SINGLETON_SPREAD:
            case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_call_singleton_spread(vm,chunk,registers,depth,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_CLOSURE: {
                uint16_t dest=0;uint8_t count=0;uint16_t index=0;
                READ_OPERANDS(5);
                dest=OPERAND_SHORT(0); index=OPERAND_SHORT(2); count=OPERAND_BYTE(4);
                /* count is a uint8_t, so DIAMOND_MAX_CAPTURES (255) bounds it; this only rejects bytecode
                 * that names a function that does not exist. */
                if(index>=chunk->function_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondClosure *created=allocate_closure(vm,index,nullptr,count);
                if(created==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                for(size_t i=0;i<count;i++){uint16_t reg=0;READ_SHORT(reg);created->captures[i]=registers[reg];}
                created->is_block=strcmp(chunk->functions[index]->name,"<block>")==0;
                if(created->is_block) {
                    created->break_target=frame.serial;
                    created->return_target=frame.home;
                    created->break_call_offset=
                        chunk->functions[index]->block_call_offset;
                }
                registers[dest]=DIAMOND_OBJECT(created);break;
            }
            case DIAMOND_OP_GET_CAPTURE: {
                uint16_t dest=0,index=0;READ_OPERANDS(4);
                dest=OPERAND_SHORT(0); index=OPERAND_SHORT(2);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=((DiamondCell *)captured.as.object)->value;break;
            }
            case DIAMOND_OP_GET_CAPTURE_CELL: {
                uint16_t dest=0,index=0;READ_OPERANDS(4);
                dest=OPERAND_SHORT(0); index=OPERAND_SHORT(2);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=captured;break;
            }
            case DIAMOND_OP_SET_CAPTURE: {
                uint16_t index=0,source=0;READ_OPERANDS(4);
                index=OPERAND_SHORT(0); source=OPERAND_SHORT(2);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ((DiamondCell *)captured.as.object)->value=registers[source];
                if(!gc_write_barrier(vm,captured.as.object))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                break;
            }
            case DIAMOND_OP_BOX_LOCAL: {
                uint16_t reg=0;READ_SHORT(reg);
                if(registers[reg].kind==DIAMOND_VALUE_OBJECT&&
                   registers[reg].as.object->kind==DIAMOND_OBJECT_CELL)break;
                DiamondCell *cell=allocate_cell(vm,registers[reg]);
                if(cell==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[reg]=DIAMOND_OBJECT(cell);break;
            }
            case DIAMOND_OP_GET_CELL: {
                uint16_t dest=0,cell_reg=0;READ_OPERANDS(4);
                dest=OPERAND_SHORT(0); cell_reg=OPERAND_SHORT(2);
                if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=((DiamondCell *)registers[cell_reg].as.object)->value;break;
            }
            case DIAMOND_OP_SET_CELL: {
                uint16_t cell_reg=0,source=0;READ_OPERANDS(4);
                cell_reg=OPERAND_SHORT(0); source=OPERAND_SHORT(2);
                if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ((DiamondCell *)registers[cell_reg].as.object)->value=registers[source];
                if(!gc_write_barrier(vm,registers[cell_reg].as.object))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                break;
            }
            case DIAMOND_OP_CALL_CLOSURE: {
                uint16_t dest=0,callable=0,base=0;uint8_t argc=0;
                READ_OPERANDS(7);
                dest=OPERAND_SHORT(0);
                callable=OPERAND_SHORT(2);
                base=OPERAND_SHORT(4);
                argc=OPERAND_BYTE(6);
                if(registers[callable].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    char actual[80];
                    diamond_format_value_type(actual,sizeof actual,registers[callable]);
                    snprintf(vm->error,sizeof vm->error,
                        "undefined method 'call' for %s",actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondClosure *called=(DiamondClosure *)registers[callable].as.object;
                /* A ClassName.compile_method result is scoped to being
                 * passed to define_method (see DIAMOND_OP_DEFINE_METHOD) --
                 * its function_index is relative to its own foreign_chunk,
                 * not the ambient one call_closure_helper below assumes,
                 * so calling it directly is rejected rather than silently
                 * running the wrong bytecode. */
                if(called->foreign_chunk!=nullptr) {
                    snprintf(vm->error,sizeof vm->error,
                        "a compile_method callable can only be passed to define_method");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(called->function_index>=chunk->function_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *fn=chunk->functions[called->function_index];
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=call_closure_helper(vm,chunk,fn,called,
                    registers,base,argc,depth,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;break;
            }
            case DIAMOND_OP_CALL_CLOSURE_KEYWORDS: {
                uint16_t dest=0,callable=0,positional_register=0;
                uint8_t keyword_count=0;uint16_t keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];
                uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
                READ_SHORT(dest);READ_SHORT(callable);READ_SHORT(positional_register);
                READ_BYTE(keyword_count);
                const bool has_block=(keyword_count&0x80u)!=0;
                keyword_count&=0x7fu;
                if((keyword_count==0&&!has_block)||keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                for(size_t index=0;index<keyword_count;index++) {
                    READ_SHORT(keyword_names[index]);READ_SHORT(keyword_registers[index]);
                }
                uint16_t block_register=0;
                if(has_block)READ_SHORT(block_register);
                if(has_block&&block_register>=DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[callable].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread call receiver must be a Callable");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[positional_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[positional_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondClosure *called=(DiamondClosure *)registers[callable].as.object;
                if(called->foreign_chunk!=nullptr) {
                    snprintf(vm->error,sizeof vm->error,
                        "a compile_method callable can only be passed to define_method");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(called->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *fn=chunk->functions[called->function_index];
                DiamondValue *merged=nullptr;size_t merged_count=0;
                const DiamondVmStatus merge_status=merge_keyword_arguments(vm,chunk,
                    fn,(const DiamondArray *)registers[positional_register].as.object,
                    keyword_names,keyword_registers,keyword_count,registers,
                    fn->arity,has_block?&registers[block_register]:nullptr,
                    &merged,&merged_count);
                VM_PROPAGATE(merge_status);
                DiamondArray merged_array={.count=merged_count,.values=merged};
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=call_closure_spread_helper(vm,chunk,
                    fn,called,&merged_array,depth,&call_result);
                free(merged);VM_PROPAGATE(status);registers[dest]=call_result;break;
            }
            case DIAMOND_OP_CALL_CLOSURE_SPREAD: {
                uint16_t dest=0,callable=0,spread_register=0;
                READ_SHORT(dest);READ_SHORT(callable);READ_SHORT(spread_register);
                if(registers[callable].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread call receiver must be a Callable");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondClosure *called=(DiamondClosure *)registers[callable].as.object;
                if(called->foreign_chunk!=nullptr) {
                    snprintf(vm->error,sizeof vm->error,
                        "a compile_method callable can only be passed to define_method");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(called->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *fn=chunk->functions[called->function_index];
                const DiamondArray *spread=(const DiamondArray *)
                    registers[spread_register].as.object;
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=call_closure_spread_helper(vm,chunk,
                    fn,called,spread,depth,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;break;
            }
            case DIAMOND_OP_NEW: {
                uint16_t dest=0,base=0;uint16_t ci=0;uint8_t argc=0;
                READ_OPERANDS(7);
                dest=OPERAND_SHORT(0);
                ci=OPERAND_SHORT(2);
                base=OPERAND_SHORT(4);
                argc=OPERAND_BYTE(6);
                const DiamondVmStatus s=diamond_jit_new_instance(vm,chunk,
                    registers,dest,ci,base,argc,depth);
                VM_PROPAGATE(s);
                break;
            }
            case DIAMOND_OP_NEW_KEYWORDS: {
                uint16_t destination=0,positional_register=0;
                uint16_t class_index=0;uint8_t keyword_count=0;uint16_t keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];
                uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
                READ_SHORT(destination);READ_SHORT(class_index);
                READ_SHORT(positional_register);READ_BYTE(keyword_count);
                const bool has_block=(keyword_count&0x80u)!=0;
                keyword_count&=0x7fu;
                if((size_t)class_index>=chunk->class_count||
                   (keyword_count==0&&!has_block)||
                   keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS||
                   registers[positional_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[positional_register].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                for(size_t index=0;index<keyword_count;index++) {
                    READ_SHORT(keyword_names[index]);READ_SHORT(keyword_registers[index]);
                }
                uint16_t block_register=0;
                if(has_block)READ_SHORT(block_register);
                if(has_block&&block_register>=DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondClass *class=chunk->classes[class_index];
                const DiamondMethod *initialize=lookup_method(chunk,class,
                    "initialize",sizeof("initialize")-1);
                if(initialize==nullptr) {snprintf(vm->error,sizeof vm->error,
                    "no parameter with this name");VM_RETURN(DIAMOND_VM_ARITY_ERROR);}
                const DiamondChunk *function_chunk=initialize->source_chunk!=nullptr?
                    initialize->source_chunk:chunk;
                const DiamondFunction *fn=
                    function_chunk->functions[initialize->function_index];
                DiamondValue *merged=nullptr;size_t merged_count=0;
                const DiamondVmStatus merge_status=merge_keyword_arguments(vm,chunk,
                    fn,(const DiamondArray *)registers[positional_register].as.object,
                    keyword_names,keyword_registers,keyword_count,registers,
                    initialize->arity,has_block?&registers[block_register]:nullptr,
                    &merged,&merged_count);
                VM_PROPAGATE(merge_status);
                DiamondArray *merged_array=allocate_array(vm,merged,merged_count);
                free(merged);if(merged_array==nullptr)
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                const size_t protected_count=vm->gc_protected_count;
                if(!gc_protect(vm,DIAMOND_OBJECT(merged_array)))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                uint8_t code[10]={DIAMOND_OP_NEW_SPREAD,0,1,
                    (uint8_t)(class_index>>8),(uint8_t)(class_index&UINT8_MAX),0,0,
                    DIAMOND_OP_RETURN,0,1};uint32_t locations[10]={0};
                DiamondChunk synthetic=*chunk;synthetic.name="<keyword new>";
                synthetic.code=code;synthetic.lines=locations;
                synthetic.columns=locations;synthetic.code_count=10;
                synthetic.register_count=2;
                const DiamondValue argument=DIAMOND_OBJECT(merged_array);
                DiamondValue constructor_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&synthetic,vm,&argument,1,
                    depth+1,nullptr,&constructor_result);
                gc_unprotect(vm,protected_count);VM_PROPAGATE(status);
                registers[destination]=constructor_result;break;
            }
            case DIAMOND_OP_NEW_SPREAD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_new_spread(vm,chunk,registers,depth,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_INVOKE_KEYWORDS:
            case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_invoke_keywords(vm,chunk,registers,depth,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_INVOKE_SPREAD:
            case DIAMOND_OP_INVOKE_TYPED_SPREAD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_invoke_spread(vm,chunk,registers,depth,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_INVOKE:
            case DIAMOND_OP_INVOKE_MONO:
            case DIAMOND_OP_INVOKE_TYPED: {
                READ_OPERANDS(9);
                const uint16_t dest=OPERAND_SHORT(0);
                const uint16_t recv=OPERAND_SHORT(2);
                const uint16_t name=OPERAND_SHORT(4);
                const uint16_t base=OPERAND_SHORT(6);
                const uint8_t argc=OPERAND_BYTE(8);
                uint8_t type_argument_count=0;uint16_t type_arguments[8];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED) {
                    READ_BYTE(type_argument_count);
                    if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_argument_count;index++)
                        READ_SHORT(type_arguments[index]);
                }
                if((size_t)name>=chunk->string_count) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
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
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        DiamondValue sent=DIAMOND_NIL;
                        const DiamondVmStatus send_status=public_send_helper(vm,chunk,
                            registers[recv],&registers[base],argc,depth,&sent);
                        VM_PROPAGATE(send_status);
                        registers[dest]=sent;break;
                    }
                    if(method_name->length==3&&
                       memcmp(method_name->chars,"tap",3)==0) {
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        const DiamondVmStatus tap_status=outlined_invoke_tap(vm,chunk,registers,recv,base,argc,dest,depth);
                        VM_PROPAGATE(tap_status);
                        break;
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
                        VM_PROPAGATE(shuffle_status);
                        break;
                    }
                    /* nil?() on any built-in value, as in Ruby. */
                    if(method_name->length==4&&memcmp(method_name->chars,"nil?",4)==0&&
                       argc==0&&type_argument_count==0&&
                       (registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                        registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)) {
                        registers[dest]=DIAMOND_BOOL(registers[recv].kind==DIAMOND_VALUE_NIL);
                        break;
                    }
                    /* Callable#arity: how many arguments it declares (its
                     * required ones, for a callable with optional or rest
                     * parameters -- no Ruby-style negative encoding). */
                    if(method_name->length==5&&memcmp(method_name->chars,"arity",5)==0&&
                       argc==0&&type_argument_count==0&&
                       registers[recv].kind==DIAMOND_VALUE_OBJECT&&
                       registers[recv].as.object->kind==DIAMOND_OBJECT_CLOSURE) {
                        const DiamondVmStatus arity_status=outlined_callable_arity(chunk,registers,recv,dest);
                        VM_PROPAGATE(arity_status);
                        break;
                    }
                    /* to_s() on any built-in value: the same text string
                     * interpolation produces. (Int, Float, and Time also
                     * have their own to_s further down; this matches them.) */
                    if(method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0&&
                       argc==0&&(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                        registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)) {
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        DiamondValue text=DIAMOND_NIL;
                        const DiamondVmStatus to_s_status=
                            stringify_value(vm,chunk,depth,registers[recv],&text);
                        VM_PROPAGATE(to_s_status);
                        registers[dest]=text;break;
                    }
                    /* inspect() on any built-in value: like to_s(), but Strings
                     * are quoted and Symbols keep their colon, so the text shows
                     * what the value is rather than how it prints. */
                    if(method_name->length==7&&memcmp(method_name->chars,"inspect",7)==0&&
                       argc==0&&(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                        registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)) {
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        DiamondValue text=DIAMOND_NIL;
                        const DiamondVmStatus inspect_status=
                            inspect_value(vm,chunk,depth,registers[recv],&text);
                        VM_PROPAGATE(inspect_status);
                        registers[dest]=text;break;
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
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const DiamondVmStatus dup_status=
                            diamond_jit_dup(vm,&registers[recv],&registers[dest]);
                        VM_PROPAGATE(dup_status);break;
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
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const DiamondVmStatus freeze_status=
                            diamond_jit_freeze(vm,&registers[recv],&registers[dest]);
                        VM_PROPAGATE(freeze_status);break;
                    }
                    /* deep_freeze: same receiver set as freeze (no-op on a
                     * primitive/String/Symbol); Array/Hash get the real,
                     * transitive walk. Not JIT-compiled -- jit.c bails on
                     * any method name it doesn't list, which is correct. */
                    if(dup_defined&&method_name->length==11&&
                       memcmp(method_name->chars,"deep_freeze",11)==0) {
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const DiamondVmStatus deep_status=
                            diamond_deep_freeze(vm,&registers[recv],&registers[dest]);
                        VM_PROPAGATE(deep_status);break;
                    }
                    if(dup_defined&&method_name->length==7&&
                       memcmp(method_name->chars,"frozen?",7)==0) {
                        if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const DiamondVmStatus frozen_status=
                            diamond_jit_frozen(vm,&registers[recv],&registers[dest]);
                        VM_PROPAGATE(frozen_status);break;
                    }
                }
                /* Int#to_s(base) on a promoted Int: digits in base 2..36. */
                if(value_is_bignum(registers[recv])&&argc==1&&
                   method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0) {
                    const DiamondVmStatus radix_status=outlined_bignum_to_s_base(vm,registers,recv,base,dest);
                    VM_PROPAGATE(radix_status);
                    break;
                }
                /* An Int past 64 bits is a bignum object; give it the same
                 * conversions as a small Int. */
                if(value_is_bignum(registers[recv])&&argc==0&&
                   ((method_name->length==4&&(memcmp(method_name->chars,"to_s",4)==0||
                     memcmp(method_name->chars,"to_i",4)==0||
                     memcmp(method_name->chars,"to_f",4)==0))||
                    (method_name->length==3&&memcmp(method_name->chars,"abs",3)==0))) {
                    const DiamondVmStatus convert_status=outlined_bignum_convert(vm,registers,recv,dest,method_name);
                    VM_PROPAGATE(convert_status);
                    break;
                }
                if(registers[recv].kind==DIAMOND_VALUE_INT||
                   registers[recv].kind==DIAMOND_VALUE_FLOAT) {
                    const DiamondVmStatus dispatch_status=numeric_invoke_helper(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
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
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondObjectKind receiver_kind=registers[recv].as.object->kind;
                if(receiver_kind==DIAMOND_OBJECT_ARRAY||
                   receiver_kind==DIAMOND_OBJECT_HASH||
                   receiver_kind==DIAMOND_OBJECT_STRING) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    DiamondVmStatus dispatch_status;
                    if(!collection_invoke_fast(vm,registers,recv,base,argc,dest,method_name,receiver_kind,&dispatch_status))
                        dispatch_status=collection_invoke_helper(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_FIBER) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=fiber_invoke_helper(
                        vm,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_THREAD) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=thread_invoke_helper(
                        vm,chunk,registers,recv,argc,dest,method_name);
                    if(dispatch_status==DIAMOND_VM_EXCEPTION) {
                        if(IP_CALL(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                               registers,&ip_slot)))
                            break;
                        set_uncaught_exception_error(vm);
                        VM_RETURN(dispatch_status);
                    }
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind!=DIAMOND_OBJECT_INSTANCE) {
                    bool native_handled=false;
                    const DiamondVmStatus native_status=outlined_native_object_invoke(vm,chunk,depth,registers,recv,base,
                        argc,dest,method_name,type_argument_count,receiver_kind,&native_handled);
                    VM_PROPAGATE(native_status);
                    if(native_handled)break;
                }
                /* A thin wrapper around diamond_jit_invoke_instance -- the
                 * same function src/jit.c's own compile_invoke_self calls
                 * for the recv==0 self-dispatch case, mirroring DIAMOND_OP_
                 * SUPER's own treatment just below and GET_IVAR/SET_IVAR's
                 * own treatment further down. `receiver_kind` (computed
                 * further up this case for the native-type checks above)
                 * isn't reused here -- the extracted function does its own
                 * unconditional registers[recv] kind check first, correct
                 * regardless of whether a primitive receiver fell through
                 * every earlier native-type block above without matching
                 * any of them. */
                const uint8_t *site=bytecode+instruction_offset;
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=diamond_jit_invoke_instance(vm,chunk,
                    site,(DiamondOpCode)instruction,registers,recv,name,base,argc,
                    type_argument_count,type_arguments,depth,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_SUPER: {
                uint16_t dest=0,base=0,name=0;uint16_t owner_index=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(owner_index);READ_SHORT(name);
                READ_SHORT(base); READ_BYTE(argc);
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=diamond_jit_super_call(vm,chunk,
                    owner_index,name,registers,base,argc,depth,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_GET_IVAR: {
                /* A thin wrapper around diamond_jit_get_ivar -- the same
                 * function src/jit.c's own ivar-read codegen calls --
                 * mirroring DIAMOND_OP_SET_IVAR's own treatment just below.
                 * As there, the field_operand bounds check happens here,
                 * before narrowing to the trampoline's own uint8_t, since a
                 * hand-built (ProgramBuilder) field_operand wider than 255
                 * must be rejected as invalid rather than silently
                 * truncated into some other, smaller, valid-looking field
                 * index. */
                const uint8_t *site=&bytecode[instruction_offset];
                uint16_t dest=0,recv=0,field_operand=0;
                READ_OPERANDS(6);
                dest=OPERAND_SHORT(0); recv=OPERAND_SHORT(2); field_operand=OPERAND_SHORT(4);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(field_operand>=
                   ((DiamondInstance *)registers[recv].as.object)->field_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondVmStatus get_ivar_status=diamond_jit_get_ivar(vm,site,
                    &registers[recv],(uint8_t)field_operand,&registers[dest]);
                VM_PROPAGATE(get_ivar_status);
                break;
            }
            case DIAMOND_OP_SET_IVAR: {
                /* A thin wrapper around diamond_jit_set_ivar -- the same
                 * function src/jit.c's own ivar-write codegen calls --
                 * rather than a second, independent copy of its real
                 * logic. This case used to duplicate that whole body
                 * verbatim (shape-transition tracking, the actual field
                 * write, the GC write barrier); freeze checking is what
                 * made the drift a real, user-visible bug rather than
                 * just untidy: adding it to only one of the two copies
                 * would have made freezing silently not apply to ivar
                 * writes in JIT-compiled methods while still applying
                 * under plain interpretation. Only the two checks that
                 * must happen *before* narrowing field_operand to the
                 * trampoline's own uint8_t stay here -- a hand-built
                 * (ProgramBuilder) field_operand wider than 255 must be
                 * rejected as invalid before truncating it, not silently
                 * wrap into some other, smaller, valid-looking field
                 * index. */
                const uint8_t *site=&bytecode[instruction_offset];
                uint16_t recv=0,field_operand=0,source=0;
                READ_OPERANDS(6);
                recv=OPERAND_SHORT(0); field_operand=OPERAND_SHORT(2); source=OPERAND_SHORT(4);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(field_operand>=
                   ((DiamondInstance *)registers[recv].as.object)->field_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondVmStatus set_ivar_status=diamond_jit_set_ivar(vm,site,
                    &registers[recv],(uint8_t)field_operand,&registers[source]);
                VM_PROPAGATE(set_ivar_status);
                break;
            }
            case DIAMOND_OP_GET_IVAR_NAME:
            case DIAMOND_OP_SET_IVAR_NAME: {
                const uint8_t *site=&bytecode[instruction_offset];
                uint16_t first=0,receiver=0,name=0;
                READ_SHORT(first);READ_SHORT(receiver);READ_SHORT(name);
                const bool write=(DiamondOpCode)instruction==DIAMOND_OP_SET_IVAR_NAME;
                const uint16_t recv=write?first:receiver;
                const uint16_t source=write?name:0;
                const uint16_t string_index=write?receiver:name;
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE||
                   (size_t)string_index>=chunk->string_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const int resolved=named_field_index(instance,
                    &chunk->strings[string_index]);
                if(resolved<0||(size_t)resolved>=instance->field_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const uint8_t field=(uint8_t)resolved;
                DiamondFieldCacheEntry *cached=lookup_field_cached(
                    vm,site,instance,field,write);
                if(write) {
                    if(instance->object.frozen)VM_RETURN(DIAMOND_VM_FROZEN_ERROR);
                    if(instance->shape!=cached->output_shape) {
                        instance->shape=cached->output_shape;
                        vm->shape_transitions++;
                    }
                    instance->fields[field]=registers[source];
                    if(!gc_write_barrier(vm,(DiamondObject *)instance))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                } else registers[first]=cached->materialized?
                    instance->fields[field]:DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_GET_NAMESPACE_CONSTANT: {
                uint16_t destination=0,index=0;READ_SHORT(destination);READ_SHORT(index);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(index>=vm->namespace_constant_capacity||
                   !vm->namespace_constant_initialized[index]) {
                    snprintf(vm->error,sizeof vm->error,vm->spawned_thread?
                        "uninitialized constant (in a spawned thread: its value "
                        "cannot cross a thread boundary, or it was defined after "
                        "the thread started)":"uninitialized constant");
                    VM_RETURN(DIAMOND_VM_CONSTANT_ERROR);
                }
                registers[destination]=vm->namespace_constants[index];break;
            }
            case DIAMOND_OP_SET_NAMESPACE_CONSTANT: {
                uint16_t index=0,source=0;READ_SHORT(index);READ_SHORT(source);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(!vm_reserve_namespace_constants(vm,(size_t)index+1))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                if(vm->namespace_constant_initialized[index]) {
                    snprintf(vm->error,sizeof vm->error,"constant is already initialized");
                    VM_RETURN(DIAMOND_VM_CONSTANT_ERROR);
                }
                vm->namespace_constants[index]=registers[source];
                vm->namespace_constant_initialized[index]=true;break;
            }
            /* Ordinary mutable storage, unlike namespace constants above --
             * no initialized bitmap, defaults to nil (DiamondVm's own
             * zero-init, DIAMOND_VALUE_NIL == 0) until first assigned,
             * exactly like an Instance's own fields. class_index/slot are
             * both compile-time constants (class_variable_index resolves
             * them once per name, see compiler.c), so the only reason
             * either could ever be out of range here is malformed
             * bytecode -- same defensive bounds-check convention
             * DIAMOND_OP_NEW's own class index uses. */
            case DIAMOND_OP_GET_CVAR: {
                uint16_t destination=0,class_index=0,slot=0;
                READ_SHORT(destination);READ_SHORT(class_index);READ_SHORT(slot);
                const DiamondVmStatus status=
                    get_cvar_helper(vm,chunk,class_index,slot,&registers[destination]);
                VM_PROPAGATE(status);break;
            }
            case DIAMOND_OP_SET_CVAR: {
                uint16_t class_index=0,slot=0,source=0;
                READ_SHORT(class_index);READ_SHORT(slot);READ_SHORT(source);
                const DiamondVmStatus status=
                    set_cvar_helper(vm,chunk,class_index,slot,registers[source]);
                VM_PROPAGATE(status);break;
            }
            case DIAMOND_OP_CHECK_TYPE: {
                uint16_t source=0,set_index=0; READ_OPERANDS(4);
                source=OPERAND_SHORT(0); set_index=OPERAND_SHORT(2);
                if(set_index>=chunk->type_set_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const bool matches=value_matches_set(chunk,registers[source],
                    set_index,true);
                if(!matches) {
                    char expected[256]; char actual[256];
                    format_type_set_index(expected,sizeof expected,chunk,set_index);
                    format_value_type_detailed(actual,sizeof actual,chunk,registers[source]);
                    snprintf(vm->error,sizeof vm->error,"expected %s, got %s",
                             expected,actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                break;
            }
            /* Multi-value destructuring assignment (`a, b = expr`,
             * src/compiler.c's compile_multi_assignment) always emits a
             * DIAMOND_OP_CHECK_TYPE against an Array-only type set
             * immediately before this opcode, so ordinary compiler-emitted
             * bytecode never reaches here with anything but a genuine
             * Array in registers[array_reg] -- but this handler cannot
             * itself trust that pairing: ProgramBuilder-constructed
             * bytecode (#emit_byte/#patch_byte, src/object.h's
             * DiamondProgramBuilder comment) can emit this opcode with no
             * preceding CHECK_TYPE at all, and previously did dereference
             * registers[array_reg].as.object unconditionally -- a real
             * type-confusion crash (SEGV reading through a non-Array
             * object, or an uninitialized union read for a non-object
             * DiamondValue entirely) found by fuzz/execute_fuzzer.c, the
             * exact class of bug diamond_verify_bytecode's register-bounds
             * checking was built for but doesn't cover (a value's runtime
             * *type* isn't something a bytecode-level walk can know
             * statically). Checked directly here now, the same way every
             * other opcode that assumes a specific object kind already
             * does (NEW's class-index bound, SQLite3's handle-kind check,
             * etc.) -- reuses the existing ArityError/DIAMOND_VM_ARITY_
             * ERROR status (the same one the SQLite3 driver's own bound-
             * parameter-count check reuses) rather than a new exception
             * class, for the same "wrong count of things" shape. */
            case DIAMOND_OP_CHECK_DESTRUCTURE_COUNT: {
                uint16_t array_reg=0,expected=0;
                READ_SHORT(array_reg); READ_SHORT(expected);
                const bool minimum=(expected&0x8000u)!=0;
                expected&=0x7fffu;
                if(registers[array_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[array_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    char actual[80];
                    diamond_format_value_type(actual,sizeof actual,registers[array_reg]);
                    snprintf(vm->error,sizeof vm->error,"expected Array, got %s",actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *array=(const DiamondArray *)registers[array_reg].as.object;
                if((minimum&&array->count<expected)||(!minimum&&array->count!=expected)) {
                    snprintf(vm->error,sizeof vm->error,
                        minimum?"destructuring assignment expected at least %u element(s), got %zu":
                        "destructuring assignment expected %u element(s), got %zu",
                        (unsigned)expected,array->count);
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                break;
            }
            case DIAMOND_OP_CASE_ARRAY_SHAPE: {
                uint16_t destination=0,array_reg=0,expected=0;
                READ_SHORT(destination);READ_SHORT(array_reg);READ_SHORT(expected);
                const bool minimum=(expected&0x8000u)!=0;
                expected&=0x7fffu;
                bool matches=false;
                if(registers[array_reg].kind==DIAMOND_VALUE_OBJECT&&
                   registers[array_reg].as.object->kind==DIAMOND_OBJECT_ARRAY) {
                    const DiamondArray *array=
                        (const DiamondArray *)registers[array_reg].as.object;
                    matches=minimum?array->count>=expected:array->count==expected;
                }
                registers[destination]=DIAMOND_BOOL(matches);break;
            }
            case DIAMOND_OP_ARRAY_REST: {
                uint16_t destination=0,array_reg=0,start=0;
                READ_SHORT(destination);READ_SHORT(array_reg);READ_SHORT(start);
                if(registers[array_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[array_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondArray *source=
                    (const DiamondArray *)registers[array_reg].as.object;
                if(start>source->count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondValue *rest_values=start==source->count?
                    nullptr:source->values+start;
                DiamondArray *rest=allocate_array(vm,rest_values,source->count-start);
                if(rest==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(rest);break;
            }
            case DIAMOND_OP_CASE_HASH_SHAPE: {
                uint16_t destination=0,source=0;
                READ_SHORT(destination);READ_SHORT(source);
                registers[destination]=DIAMOND_BOOL(
                    registers[source].kind==DIAMOND_VALUE_OBJECT&&
                    registers[source].as.object->kind==DIAMOND_OBJECT_HASH);
                break;
            }
            case DIAMOND_OP_CASE_HASH_HAS: {
                uint16_t destination=0,source=0,key=0;
                READ_SHORT(destination);READ_SHORT(source);READ_SHORT(key);
                bool present=false;
                if(registers[source].kind==DIAMOND_VALUE_OBJECT&&
                   registers[source].as.object->kind==DIAMOND_OBJECT_HASH)
                    present=hash_find((DiamondHash *)registers[source].as.object,
                        registers[key])>=0;
                registers[destination]=DIAMOND_BOOL(present);break;
            }
            case DIAMOND_OP_HASH_REST: {
                uint16_t destination=0,source_reg=0,excluded_reg=0;
                READ_SHORT(destination);READ_SHORT(source_reg);READ_SHORT(excluded_reg);
                if(registers[source_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[source_reg].as.object->kind!=DIAMOND_OBJECT_HASH||
                   registers[excluded_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[excluded_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondHash *source=
                    (const DiamondHash *)registers[source_reg].as.object;
                const DiamondArray *excluded=
                    (const DiamondArray *)registers[excluded_reg].as.object;
                DiamondHash *rest=allocate_hash(vm);
                if(rest==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(rest);
                for(size_t entry=0;entry<source->count;entry++) {
                    bool omit=false;
                    for(size_t key=0;key<excluded->count;key++)
                        if(values_equal(source->entries[entry].key,
                                        excluded->values[key])) {omit=true;break;}
                    if(!omit&&!hash_set(vm,rest,source->entries[entry].key,
                        source->entries[entry].value))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                break;
            }
            case DIAMOND_OP_ARRAY_MIDDLE: {
                uint16_t destination=0,source_reg=0,bounds=0;
                READ_SHORT(destination);READ_SHORT(source_reg);READ_SHORT(bounds);
                if(registers[source_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[source_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondArray *source=
                    (const DiamondArray *)registers[source_reg].as.object;
                const size_t prefix=(size_t)(bounds>>8);
                const size_t suffix=(size_t)(bounds&0xffu);
                if(prefix+suffix>source->count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const size_t count=source->count-prefix-suffix;
                const DiamondValue *values=count==0?nullptr:source->values+prefix;
                DiamondArray *middle=allocate_array(vm,values,count);
                if(middle==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(middle);break;
            }
            case DIAMOND_OP_ARRAY_SUFFIX: {
                uint16_t destination=0,source_reg=0,reverse=0;
                READ_SHORT(destination);READ_SHORT(source_reg);READ_SHORT(reverse);
                if(registers[source_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[source_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondArray *source=
                    (const DiamondArray *)registers[source_reg].as.object;
                if(reverse==0||reverse>source->count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[destination]=source->values[source->count-reverse];break;
            }
            case DIAMOND_OP_CHECK_HASH_KEY: {
                uint16_t source_reg=0,key_reg=0;
                READ_SHORT(source_reg);READ_SHORT(key_reg);
                if(registers[source_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[source_reg].as.object->kind!=DIAMOND_OBJECT_HASH)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(hash_find((DiamondHash *)registers[source_reg].as.object,
                             registers[key_reg])<0) {
                    snprintf(vm->error,sizeof vm->error,
                        "destructuring assignment missing required Hash key");
                    VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                }
                break;
            }
            /* A duration-only clock: seconds since some unspecified,
             * process-local reference point (CLOCK_MONOTONIC), never
             * meaningful as a calendar timestamp or across processes --
             * only the difference between two readings means anything.
             * Diamond has no wall-clock/calendar Time type at all yet
             * (see docs/threads.md); this is deliberately just enough to
             * measure an elapsed duration (e.g. a request-timing rack
             * middleware), not a step toward one. */
            case DIAMOND_OP_TIME_MONOTONIC: {
                uint16_t destination=0;
                READ_SHORT(destination);
                struct timespec now={};
                clock_gettime(CLOCK_MONOTONIC,&now);
                registers[destination]=
                    DIAMOND_FLOAT((double)now.tv_sec+(double)now.tv_nsec/1e9);
                break;
            }
            case DIAMOND_OP_TIME_NOW: {
                /* VM_PROPAGATE's argument is expanded multiple times (see
                 * its own definition) -- a function call passed directly
                 * would run time_now_helper up to 3x on any non-OK status,
                 * double-allocating. Stored in a local first, same as
                 * every other VM_PROPAGATE call site in this switch. */
                uint16_t destination=0;uint8_t utc_flag=0;
                READ_SHORT(destination);READ_BYTE(utc_flag);
                const DiamondVmStatus time_status=
                    time_now_helper(vm,utc_flag!=0,&registers[destination]);
                VM_PROPAGATE(time_status);
                break;
            }
            case DIAMOND_OP_TIME_AT: {
                uint16_t destination=0,epoch_register=0;
                READ_SHORT(destination);READ_SHORT(epoch_register);
                const DiamondVmStatus time_status=
                    time_at_helper(vm,registers[epoch_register],&registers[destination]);
                VM_PROPAGATE(time_status);
                break;
            }
            case DIAMOND_OP_TIME_PARSE: {
                uint16_t destination=0,string_register=0;
                READ_SHORT(destination);READ_SHORT(string_register);
                const DiamondVmStatus time_status=time_parse_helper(vm,
                    registers[string_register],&registers[destination]);
                VM_PROPAGATE(time_status);
                break;
            }
            case DIAMOND_OP_TIME_BUILD: {
                uint16_t destination=0,base_register=0;uint8_t mode=0;
                READ_SHORT(destination);READ_SHORT(base_register);READ_BYTE(mode);
                const DiamondVmStatus time_status=time_build_helper(vm,
                    &registers[base_register],mode,&registers[destination]);
                VM_PROPAGATE(time_status);
                break;
            }
            case DIAMOND_OP_TENSOR_ZEROS: {
                uint16_t dest=0,rows_reg=0,cols_reg=0;
                READ_SHORT(dest);READ_SHORT(rows_reg);READ_SHORT(cols_reg);
                if(registers[rows_reg].kind!=DIAMOND_VALUE_INT||
                   registers[cols_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                        "Tensor.zeros's rows/cols arguments must be Ints");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t rows=registers[rows_reg].as.integer;
                const int64_t cols=registers[cols_reg].as.integer;
                if(rows<=0||cols<=0) {
                    snprintf(vm->error,sizeof vm->error,
                        "Tensor.zeros's rows/cols arguments must be positive");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondTensor *tensor=allocate_tensor(vm,(size_t)rows,(size_t)cols);
                if(tensor==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(tensor);
                break;
            }
            case DIAMOND_OP_TENSOR_FROM_ARRAY: {
                uint16_t dest=0,array_reg=0;
                READ_SHORT(dest);READ_SHORT(array_reg);
                if(registers[array_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[array_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "Tensor.from_array's argument must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondVmStatus from_array_status=tensor_from_array_helper(vm,
                    (const DiamondArray *)registers[array_reg].as.object,&registers[dest]);
                VM_PROPAGATE(from_array_status);
                break;
            }
            case DIAMOND_OP_TENSOR_RANDOM: {
                uint16_t dest=0,rows_reg=0,cols_reg=0,seed_reg=0;
                READ_SHORT(dest);READ_SHORT(rows_reg);READ_SHORT(cols_reg);READ_SHORT(seed_reg);
                if(registers[rows_reg].kind!=DIAMOND_VALUE_INT||
                   registers[cols_reg].kind!=DIAMOND_VALUE_INT||
                   registers[seed_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                        "Tensor.random's rows/cols/seed arguments must be Ints");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t rows=registers[rows_reg].as.integer;
                const int64_t cols=registers[cols_reg].as.integer;
                if(rows<=0||cols<=0) {
                    snprintf(vm->error,sizeof vm->error,
                        "Tensor.random's rows/cols arguments must be positive");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondTensor *tensor=allocate_tensor(vm,(size_t)rows,(size_t)cols);
                if(tensor==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(tensor);
                tensor_random_helper(tensor,registers[seed_reg].as.integer);
                break;
            }
            case DIAMOND_OP_PROCESS_RUN:
            case DIAMOND_OP_PROCESS_RUN_OPTIONS: {
                uint16_t destination=0,argv_register=0,options_register=UINT16_MAX;
                READ_SHORT(destination);READ_SHORT(argv_register);
                if(instruction==DIAMOND_OP_PROCESS_RUN_OPTIONS)READ_SHORT(options_register);
                VM_SANDBOX_GUARD("Process.run", "subprocess");
                if(registers[argv_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[argv_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "Process.run expects an Array of Strings");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondProcessResult *process_result=allocate_process_result(vm);
                if(process_result==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                /* Rooted immediately, before process_run_helper's own
                 * further allocations -- see allocate_process_result's
                 * comment. */
                registers[destination]=DIAMOND_OBJECT(process_result);
                const DiamondVmStatus run_status=process_run_helper(vm,
                    (DiamondArray *)registers[argv_register].as.object,process_result,
                    options_register==UINT16_MAX?DIAMOND_NIL:registers[options_register]);
                VM_PROPAGATE(run_status);
                break;
            }
            case DIAMOND_OP_PROCESS_SPAWN:
            case DIAMOND_OP_PROCESS_SPAWN_OPTIONS: {
                uint16_t destination=0,argv_register=0,options_register=UINT16_MAX;
                READ_SHORT(destination);READ_SHORT(argv_register);
                if(instruction==DIAMOND_OP_PROCESS_SPAWN_OPTIONS)READ_SHORT(options_register);
                VM_SANDBOX_GUARD("Process.spawn", "subprocess");
                if(registers[argv_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[argv_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "Process.spawn expects an Array of Strings");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondProcessHandle *process_handle=allocate_process_handle(vm,0);
                if(process_handle==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                /* Rooted immediately, before process_spawn_helper's own
                 * further allocations -- see allocate_process_handle's
                 * comment. */
                registers[destination]=DIAMOND_OBJECT(process_handle);
                const DiamondVmStatus spawn_status=process_spawn_helper(vm,
                    (DiamondArray *)registers[argv_register].as.object,process_handle,
                    options_register==UINT16_MAX?DIAMOND_NIL:registers[options_register]);
                VM_PROPAGATE(spawn_status);
                break;
            }
            case DIAMOND_OP_BCRYPT_HASH: {
                uint16_t destination=0,password_register=0,cost_register=0;
                READ_SHORT(destination);READ_SHORT(password_register);
                READ_SHORT(cost_register);
                const DiamondVmStatus hash_status=bcrypt_hash_helper(vm,
                    registers[password_register],registers[cost_register],
                    &registers[destination]);
                VM_PROPAGATE(hash_status);
                break;
            }
            case DIAMOND_OP_BCRYPT_VERIFY: {
                uint16_t destination=0,password_register=0,digest_register=0;
                READ_SHORT(destination);READ_SHORT(password_register);
                READ_SHORT(digest_register);
                const DiamondVmStatus verify_status=bcrypt_verify_helper(vm,
                    registers[password_register],registers[digest_register],
                    &registers[destination]);
                VM_PROPAGATE(verify_status);
                break;
            }
            case DIAMOND_OP_SECURE_RANDOM_BYTES: {
                uint16_t destination=0,count_register=0;
                READ_SHORT(destination);READ_SHORT(count_register);
                const DiamondVmStatus bytes_status=secure_random_bytes_helper(vm,
                    registers[count_register],&registers[destination]);
                VM_PROPAGATE(bytes_status);
                break;
            }
            case DIAMOND_OP_SECURE_RANDOM_HEX: {
                uint16_t destination=0,count_register=0;
                READ_SHORT(destination);READ_SHORT(count_register);
                const DiamondVmStatus hex_status=secure_random_hex_helper(vm,
                    registers[count_register],&registers[destination]);
                VM_PROPAGATE(hex_status);
                break;
            }
            case DIAMOND_OP_DIGEST_SHA256: {
                uint16_t destination=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);
                const DiamondVmStatus digest_status=sha256_hex_helper(vm,
                    DIAMOND_NIL,registers[data_register],false,&registers[destination]);
                VM_PROPAGATE(digest_status);break;
            }
            case DIAMOND_OP_HMAC_SHA256: {
                uint16_t destination=0,key_register=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(key_register);READ_SHORT(data_register);
                const DiamondVmStatus hmac_status=sha256_hex_helper(vm,
                    registers[key_register],registers[data_register],true,
                    &registers[destination]);
                VM_PROPAGATE(hmac_status);break;
            }
            case DIAMOND_OP_HMAC_VERIFY: {
                uint16_t destination=0,data_register=0,key_register=0,signature_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);
                READ_SHORT(key_register);READ_SHORT(signature_register);
                const DiamondVmStatus verify_status=hmac_verify_helper(vm,
                    registers[data_register],registers[key_register],
                    registers[signature_register],&registers[destination]);
                VM_PROPAGATE(verify_status);break;
            }
            case DIAMOND_OP_DIGEST_SHA1: {
                uint16_t destination=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);
                const DiamondVmStatus digest_status=sha1_hex_helper(vm,
                    DIAMOND_NIL,registers[data_register],false,&registers[destination]);
                VM_PROPAGATE(digest_status);break;
            }
            case DIAMOND_OP_HMAC_SHA1: {
                uint16_t destination=0,key_register=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(key_register);READ_SHORT(data_register);
                const DiamondVmStatus hmac_status=sha1_hex_helper(vm,
                    registers[key_register],registers[data_register],true,
                    &registers[destination]);
                VM_PROPAGATE(hmac_status);break;
            }
            case DIAMOND_OP_CIPHER_ENCRYPT: {
                uint16_t destination=0,key_register=0,plaintext_register=0;
                READ_SHORT(destination);READ_SHORT(key_register);
                READ_SHORT(plaintext_register);
                const DiamondVmStatus encrypt_status=aes_gcm_encrypt_helper(vm,
                    registers[key_register],registers[plaintext_register],
                    &registers[destination]);
                VM_PROPAGATE(encrypt_status);break;
            }
            case DIAMOND_OP_CIPHER_DECRYPT: {
                uint16_t destination=0,key_register=0,blob_register=0;
                READ_SHORT(destination);READ_SHORT(key_register);READ_SHORT(blob_register);
                const DiamondVmStatus decrypt_status=aes_gcm_decrypt_helper(vm,
                    registers[key_register],registers[blob_register],
                    &registers[destination]);
                VM_PROPAGATE(decrypt_status);break;
            }
            case DIAMOND_OP_GZIP_COMPRESS: {
                uint16_t destination=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);
                const DiamondVmStatus compress_status=gzip_compress_helper(vm,
                    registers[data_register],&registers[destination]);
                VM_PROPAGATE(compress_status);break;
            }
            case DIAMOND_OP_GZIP_DECOMPRESS: {
                uint16_t destination=0,data_register=0,max_size_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);READ_SHORT(max_size_register);
                const DiamondVmStatus decompress_status=gzip_decompress_helper(vm,
                    registers[data_register],registers[max_size_register],
                    &registers[destination]);
                VM_PROPAGATE(decompress_status);break;
            }
            case DIAMOND_OP_BASE64_ENCODE: {
                uint16_t destination=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);
                const DiamondVmStatus encode_status=base64_encode_helper(vm,
                    registers[data_register],&registers[destination]);
                VM_PROPAGATE(encode_status);break;
            }
            case DIAMOND_OP_BASE64_DECODE: {
                uint16_t destination=0,data_register=0;
                READ_SHORT(destination);READ_SHORT(data_register);
                const DiamondVmStatus decode_status=base64_decode_helper(vm,
                    registers[data_register],&registers[destination]);
                VM_PROPAGATE(decode_status);break;
            }
            case DIAMOND_OP_EXIT: {
                uint16_t code_register=0;
                READ_SHORT(code_register);
                const DiamondVmStatus exit_status=exit_helper(vm,registers[code_register]);
                /* exit_helper only returns at all on a validation
                 * failure -- a valid code calls libc exit() and this
                 * line is never reached. */
                VM_PROPAGATE(exit_status);
                break;
            }
            case DIAMOND_OP_DEBUGGER: {
                uint16_t destination=0;uint8_t local_count=0;
                READ_SHORT(destination);READ_BYTE(local_count);
                uint16_t name_indices[DIAMOND_MAX_LOCALS];
                uint16_t local_registers[DIAMOND_MAX_LOCALS];
                for(size_t index=0;index<local_count;index++) {
                    READ_SHORT(name_indices[index]);
                    READ_SHORT(local_registers[index]);
                }
                const DiamondVmStatus debugger_status=debugger_helper(vm,chunk,depth,
                    instruction_offset,SIZE_MAX,registers,name_indices,local_registers,local_count,
                    "breakpoint");
                VM_PROPAGATE(debugger_status);
                registers[destination]=DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_BREAKPOINT_CHECK: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_breakpoint_check(vm,chunk,registers,depth,instruction_offset,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_ARGV: {
                uint16_t destination=0;READ_SHORT(destination);
                registers[destination]=vm->argv_value;break;
            }
            case DIAMOND_OP_ENV: {
                uint16_t destination=0;READ_SHORT(destination);
                registers[destination]=vm->env_value;break;
            }
            case DIAMOND_OP_IS_TYPE: {
                uint16_t destination=0,source=0,type=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); source=OPERAND_SHORT(2); type=OPERAND_SHORT(4);
                registers[destination]=DIAMOND_BOOL(diamond_type_operand_is_native(type)?
                    value_is_native_kind(registers[source],
                        (uint8_t)(type-DIAMOND_NATIVE_TYPE_OPERAND_BASE)):
                    value_matches_type(chunk,registers[source],type));
                break;
            }
            case DIAMOND_OP_SAME_CLASS: {
                uint16_t destination=0,left=0,right=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); left=OPERAND_SHORT(2); right=OPERAND_SHORT(4);
                registers[destination]=DIAMOND_BOOL(
                    diamond_values_same_class(registers[left],registers[right]));
                break;
            }
            case DIAMOND_OP_ARRAY: {
                uint16_t destination=0,base=0,count=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); base=OPERAND_SHORT(2); count=OPERAND_SHORT(4);
                const DiamondVmStatus status=diamond_jit_new_array(vm,registers,
                    base,count,&registers[destination]);
                VM_PROPAGATE(status);
                break;
            }
            case DIAMOND_OP_INDEX_GET: {
                uint16_t destination=0,receiver=0,index_register=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); receiver=OPERAND_SHORT(2); index_register=OPERAND_SHORT(4);
                const uint8_t *site=bytecode+instruction_offset;
                DiamondValue indexed=DIAMOND_NIL;
                const DiamondVmStatus status=diamond_jit_index_get(vm,chunk,depth,site,
                    &registers[receiver],&registers[index_register],&indexed);
                VM_PROPAGATE(status);
                registers[destination]=indexed;
                break;
            }
            case DIAMOND_OP_INDEX_GET_STRING: {
                uint16_t destination=0,receiver=0,string_index=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); receiver=OPERAND_SHORT(2); string_index=OPERAND_SHORT(4);
                const uint8_t *site=bytecode+instruction_offset;
                DiamondValue indexed=DIAMOND_NIL;
                const DiamondVmStatus status=diamond_jit_index_get_string(vm,chunk,depth,site,
                    &registers[receiver],string_index,&indexed);
                VM_PROPAGATE(status);
                registers[destination]=indexed;
                break;
            }
            case DIAMOND_OP_INDEX_SET: {
                uint16_t receiver=0,index_register=0,source=0;
                READ_OPERANDS(6);
                receiver=OPERAND_SHORT(0); index_register=OPERAND_SHORT(2); source=OPERAND_SHORT(4);
                const uint8_t *site=bytecode+instruction_offset;
                const DiamondVmStatus status=diamond_jit_index_set(vm,chunk,depth,site,
                    &registers[receiver],&registers[index_register],&registers[source]);
                VM_PROPAGATE(status);
                break;
            }
            case DIAMOND_OP_HASH: {
                uint16_t destination=0,base=0,count=0;
                READ_OPERANDS(6);
                destination=OPERAND_SHORT(0); base=OPERAND_SHORT(2); count=OPERAND_SHORT(4);
                DiamondValue built=DIAMOND_NIL;
                const DiamondVmStatus hash_status=
                    diamond_jit_new_hash(vm,registers,base,count,&built);
                VM_PROPAGATE(hash_status);
                registers[destination]=built;
                break;
            }
            case DIAMOND_OP_NOT: {
                uint16_t destination=0,source=0;
                READ_OPERANDS(4);
                destination=OPERAND_SHORT(0); source=OPERAND_SHORT(2);
                registers[destination]=DIAMOND_BOOL(!is_truthy(registers[source]));
                break;
            }
            case DIAMOND_OP_RETURN: {
                uint16_t source = 0;
                READ_SHORT(source);
                while(handler_count>0 &&
                      handlers[handler_count-1].kind!=HANDLER_ENSURE)
                    (void)pop_unwind_handler(handlers,&handler_count,&pending);
                if(handler_count>0) {
                    ip=enter_ensure(&handlers[handler_count-1],&pending,
                        (PendingUnwind){.kind=PENDING_RETURN,.value=registers[source]});
                    break;
                }
                *result = registers[source];
                VM_RETURN(DIAMOND_VM_OK);
            }
            case DIAMOND_OP_RAISE: {
                uint16_t source=0;READ_SHORT(source);
                vm->exception=registers[source];vm->has_exception=true;
                const bool first_raise=raise_capture_backtrace_helper(vm,chunk);
                if(IP_CALL(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                   registers,&ip_slot)))break;
                /* Only fill in a message when vm->error is still empty --
                 * see catch_runtime_error's own identical guard: a raise
                 * of a value that a *previous*, now-unwound attempt at
                 * this same statement already recorded a detail for
                 * (there is no such case today, since a DIAMOND_OP_RAISE
                 * that reaches here always does so on its first and only
                 * attempt) should never overwrite that detail. Kept for
                 * the same reason format_uncaught_exception_message's own
                 * other call site (DIAMOND_OP_END_ENSURE) needs it: this
                 * check is what makes the two callers safe to share one
                 * helper without each needing to reason about the other's
                 * invariants. */
                if(vm->error[0]=='\0')
                    format_uncaught_exception_message(vm,vm->exception,!first_raise);
                VM_RETURN(DIAMOND_VM_EXCEPTION);
            }
            case DIAMOND_OP_PUSH_RESCUE: {
                uint16_t destination=0;uint8_t type_count=0,types[8],high=0,low=0;
                READ_SHORT(destination);READ_BYTE(type_count);
                for(size_t i=0;i<8;i++)READ_BYTE(types[i]);
                READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                const bool enabled=(type_count&0x80)==0;
                type_count&=0x7f;
                if(handler_count==16||type_count>8||target>bytecode_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                UnwindHandler *handler=&handlers[handler_count++];
                *handler=(UnwindHandler){.kind=HANDLER_RESCUE,.target=target,
                    .destination=destination,.type_count=type_count,.enabled=enabled};
                for(size_t i=0;i<type_count;i++)handler->types[i]=types[i];
                break;
            }
            case DIAMOND_OP_POP_RESCUE:
                if(handler_count==0||handlers[handler_count-1].kind!=HANDLER_RESCUE)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                handler_count--;break;
            case DIAMOND_OP_PUSH_ENSURE: {
                uint8_t high=0,low=0;READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                if(handler_count==16||target>bytecode_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                handlers[handler_count++]=(UnwindHandler){
                    .kind=HANDLER_ENSURE,.target=target,.enabled=true};
                break;
            }
            case DIAMOND_OP_UNWIND_JUMP: {
                uint8_t levels=0,high=0,low=0;READ_BYTE(levels);READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                if(target>bytecode_count||
                   !IP_CALL(unwind_for_jump(handlers,&handler_count,&pending,levels,target,&ip_slot)))
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                break;
            }
            case DIAMOND_OP_RUN_ENSURE: {
                uint8_t high=0,low=0;READ_BYTE(high);READ_BYTE(low);
                const size_t continuation=((size_t)high<<8)|low;
                if(handler_count==0||continuation>bytecode_count||
                   handlers[handler_count-1].kind!=HANDLER_ENSURE)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ip=enter_ensure(&handlers[handler_count-1],&pending,
                    (PendingUnwind){.kind=PENDING_NORMAL,.continuation=continuation});
                break;
            }
            case DIAMOND_OP_BLOCK_RETURN:
            case DIAMOND_OP_BLOCK_BREAK: {
                uint16_t source=0;READ_SHORT(source);
                const bool is_break=bytecode[instruction_offset]==DIAMOND_OP_BLOCK_BREAK;
                const uint64_t target=closure==nullptr?0:
                    (is_break?closure->break_target:closure->return_target);
                if(target==0) {
                    /* A block copied into another VM (Thread.new) has no
                     * frame to go back to there. */
                    snprintf(vm->error,sizeof vm->error,is_break?
                        "break from a block outside the call it was passed to":
                        "return from a block outside the method it was written in");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* Check the target is still live on this stack (and, for a
                 * break, still inside the call it was passed to) before
                 * unwinding anything, so a stale target is an ordinary
                 * rescuable error raised right here. */
                const DiamondFrame *callee=nullptr;
                for(const DiamondFrame *walk=&frame;walk!=nullptr;walk=walk->previous) {
                    if(walk->previous!=nullptr&&walk->previous->serial==target) {
                        callee=walk;break;
                    }
                    if(walk->serial==target) {callee=walk;break;}
                }
                /* A break lands only in the very call the block was
                 * written for, not a later one it was handed to. */
                const bool live=callee!=nullptr&&(is_break?
                    callee->serial!=target&&closure->break_call_offset!=0&&
                    *callee->previous->instruction_offset==
                        closure->break_call_offset:true);
                if(!live) {
                    snprintf(vm->error,sizeof vm->error,is_break?
                        "break from a block outside the call it was passed to":
                        "return from a block outside the method it was written in");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                vm->nonlocal_value=registers[source];
                vm->nonlocal_target=target;
                vm->nonlocal_is_break=is_break;
                VM_RETURN(DIAMOND_VM_NONLOCAL_EXIT);
            }
            case DIAMOND_OP_END_ENSURE: {
                if(handler_count==0 ||
                   handlers[handler_count-1].kind!=HANDLER_ACTIVE_ENSURE)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const PendingUnwind resume=pending;
                (void)pop_unwind_handler(handlers,&handler_count,&pending);
                if(resume.kind==PENDING_NORMAL) {ip=resume.continuation;break;}
                if(resume.kind==PENDING_JUMP) {
                    if(!IP_CALL(unwind_for_jump(handlers,&handler_count,&pending,
                            (size_t)resume.nonlocal_target,resume.continuation,&ip_slot)))
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    break;
                }
                if(resume.kind==PENDING_RETURN) {
                    while(handler_count>0 &&
                          handlers[handler_count-1].kind!=HANDLER_ENSURE)
                        (void)pop_unwind_handler(handlers,&handler_count,&pending);
                    if(handler_count>0) {
                        ip=enter_ensure(&handlers[handler_count-1],&pending,resume);break;
                    }
                    *result=resume.value;VM_RETURN(DIAMOND_VM_OK);
                }
                if(resume.kind==PENDING_NONLOCAL) {
                    vm->nonlocal_value=resume.value;
                    vm->nonlocal_target=resume.nonlocal_target;
                    vm->nonlocal_is_break=resume.nonlocal_is_break;
                    VM_RETURN(DIAMOND_VM_NONLOCAL_EXIT);
                }
                if(resume.kind==PENDING_EXCEPTION) {
                    vm->exception=resume.value;vm->has_exception=true;
                    if(IP_CALL(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                       registers,&ip_slot)))break;
                    /* Only fill in a message when vm->error is still
                     * empty -- see catch_runtime_error's own identical
                     * guard. A status-code-originated exception (e.g.
                     * division by zero) that passed through this same
                     * ensure boundary already had its detailed message +
                     * true-origin "at ..." frame written by
                     * catch_runtime_error before ever reaching here;
                     * overwriting it unconditionally would discard that
                     * accumulated backtrace and replace it with a fresh,
                     * frame-less message starting from wherever this
                     * ensure happens to sit -- exactly the bug
                     * catch_runtime_error's own guard exists to avoid,
                     * reintroduced here the first time this call was
                     * added. A plain `raise`-originated value never has
                     * this problem (DIAMOND_OP_RAISE's own redirect-to-
                     * ensure path never touches vm->error in the first
                     * place), so this guard only ever changes behavior
                     * for the status-code-originated case. */
                    if(vm->error[0]=='\0')
                        format_uncaught_exception_message(vm,vm->exception,true);
                    VM_RETURN(DIAMOND_VM_EXCEPTION);
                }
                VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
            }
            case DIAMOND_OP_YIELD: {
                uint16_t dest=0,source=0;
                READ_SHORT(dest);READ_SHORT(source);
                if(vm->running_fiber==nullptr) VM_RETURN(DIAMOND_VM_YIELD_WITHOUT_FIBER);
                vm->running_fiber->status=DIAMOND_VM_YIELDED;
                vm->running_fiber->result=registers[source];
#ifdef DIAMOND_ASAN_FIBERS
                /* Suspending, not leaving for good: this fiber's own stack
                 * (the one this frame lives on) must be preserved rather
                 * than destroyed. yield_fake_stack is a plain local: this
                 * swapcontext call is where a later resume physically
                 * continues, since this whole C frame lives on the fiber's
                 * own parked stack memory in the meantime. */
                void *yield_fake_stack=nullptr;
                const void *yield_dest_bottom=nullptr;size_t yield_dest_size=0;
                diamond_resume_target_bounds(vm->running_fiber,
                    &yield_dest_bottom,&yield_dest_size);
                __sanitizer_start_switch_fiber(&yield_fake_stack,
                    yield_dest_bottom,yield_dest_size);
#endif
                swapcontext(&vm->running_fiber->context,vm->running_fiber->resume_target);
#ifdef DIAMOND_ASAN_FIBERS
                __sanitizer_finish_switch_fiber(yield_fake_stack,nullptr,nullptr);
#endif
                registers[dest]=vm->running_fiber->resume_value;
                break;
            }
            case DIAMOND_OP_REDEFINE_METHOD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_redefine_method(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            /* redefine_method's add-a-new-slot counterpart. Same four
             * checks (String name, Callable value, zero captures,
             * owner_class matches), minus the arity-must-match check --
             * there is no existing method to match arity against, so the
             * new slot just takes the callable's own arity, the same way
             * a `def` compiled directly into this class would. The fixed-
             * size `methods[]` array already has DIAMOND_MAX_METHODS
             * headroom regardless of how many compile-time methods a
             * class declared, so "adding" a method is just populating the
             * next unused slot and incrementing method_count -- no
             * reallocation, no heap object, nothing for the GC to know
             * about. Rejects a name that already exists (directing the
             * caller to redefine_method instead) rather than silently
             * repointing it, keeping the two operations' contracts
             * separate and predictable. */
            case DIAMOND_OP_DEFINE_METHOD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_define_method(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_COMPILE_METHOD: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_compile_method(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_LOAD_CLASS: {
                uint16_t dest=0;uint16_t class_operand=0;
                READ_SHORT(dest);READ_SHORT(class_operand);
                if((size_t)class_operand>=chunk->class_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=DIAMOND_CLASS(class_operand);break;
            }
            /* `self.method_name(...)` inside a class-owned singleton
             * method body -- the runtime half of parse_self_class_method_
             * call's emission. registers[0] (self) is expected to already
             * hold a DIAMOND_VALUE_CLASS (guaranteed by the compiler only
             * ever emitting this opcode inside a class-owned singleton
             * method, where compile_definition reserves and populates
             * that slot), resolved by name via lookup_singleton_method
             * (lookup_method's exact algorithm, over singleton_methods[]
             * instead) starting from self's *actual* class_index and
             * walking its superclass chain -- not the literal class this
             * calling function happens to be lexically defined in, which
             * is the entire point: an inherited method reaches whichever
             * subclass actually received the original external call. */
            case DIAMOND_OP_INVOKE_SELF_METHOD: {
                uint16_t dest=0,base=0,name_index=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(name_index);READ_SHORT(base);READ_BYTE(argc);
                if(registers[0].kind!=DIAMOND_VALUE_CLASS)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const uint16_t class_operand=registers[0].as.class_index;
                if((size_t)class_operand>=chunk->class_count||
                   (size_t)name_index>=chunk->string_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondClass *class=chunk->classes[class_operand];
                const DiamondStringConstant *method_name=&chunk->strings[name_index];
                const DiamondMethod *method=lookup_singleton_method(chunk,class,
                    method_name->chars,method_name->length);
                /* self.new(...): an instance of whichever class self is, so
                 * an inherited factory method builds the subclass it was
                 * called on. Unless the class defines its own `new`. */
                if(method==nullptr&&method_name->length==3&&
                   memcmp(method_name->chars,"new",3)==0) {
                    if(class->sealed) {
                        snprintf(vm->error,sizeof vm->error,
                            "cannot instantiate sealed class %s directly -- "
                            "use one of its subclasses",class->name);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    const DiamondVmStatus new_status=diamond_jit_new_instance(vm,chunk,
                        registers,dest,class_operand,base,argc,depth);
                    VM_PROPAGATE(new_status);
                    break;
                }
                /* self.name() / self.to_s(): the class's own name, unless
                 * the class defines a singleton of that name itself. */
                if(method==nullptr&&argc==0&&
                   ((method_name->length==4&&memcmp(method_name->chars,"name",4)==0)||
                    (method_name->length==4&&memcmp(method_name->chars,"to_s",4)==0))) {
                    DiamondString *class_name=allocate_string(vm,class->name,strlen(class->name));
                    if(class_name==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[dest]=DIAMOND_OBJECT(class_name);break;
                }
                if(method==nullptr) {
                    snprintf(vm->error,sizeof vm->error,
                        "undefined class singleton method '%.*s' for %s",
                        (int)method_name->length,method_name->chars,class->name);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(argc<method->required_arity||
                   (argc>method->arity && !method->has_variadic))
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if((size_t)base+argc>DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if((size_t)method->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus self_call_status=invoke_resolved_method_helper(
                    vm,chunk,method,registers[0],registers,base,argc,false,0,
                    nullptr,chunk,depth,&call_result);
                VM_PROPAGATE(self_call_status);
                registers[dest]=call_result;break;
            }
            case DIAMOND_OP_FIBER_NEW: {
                uint16_t dest=0,callable_reg=0;
                READ_SHORT(dest);READ_SHORT(callable_reg);
                if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,"Fiber.new argument must be a Callable value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondClosure *callable=(const DiamondClosure *)registers[callable_reg].as.object;
                /* See DIAMOND_OP_CALL_CLOSURE's own comment. */
                if(callable->foreign_chunk!=nullptr) {
                    snprintf(vm->error,sizeof vm->error,
                        "a compile_method callable can only be passed to define_method");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if((size_t)callable->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *target_fn=chunk->functions[callable->function_index];
                if(target_fn->arity!=0) {
                    snprintf(vm->error,sizeof vm->error,"Fiber.new callable must take no arguments");
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                DiamondFiber *new_fiber=diamond_fiber_new_for_closure(chunk,callable);
                if(new_fiber==nullptr||diamond_fiber_bind_vm(new_fiber,vm)!=DIAMOND_FIBER_OK||
                   diamond_fiber_prepare(new_fiber)!=DIAMOND_FIBER_OK) {
                    diamond_fiber_free(new_fiber);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                DiamondFiberHandle *handle=allocate_fiber_handle(vm,new_fiber);
                if(handle==nullptr) {
                    diamond_fiber_free(new_fiber);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_FILE_OPEN: {
                uint16_t dest=0,path_reg=0,mode_reg=0;
                READ_SHORT(dest);READ_SHORT(path_reg);READ_SHORT(mode_reg);
                VM_SANDBOX_GUARD("File.open", "filesystem");
                if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[mode_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[mode_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"File.open arguments must be String values");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
                const DiamondString *mode=(const DiamondString *)registers[mode_reg].as.object;
                errno=0;
                FILE *stream=fopen(path->chars,mode->chars);
                if(stream==nullptr) {
                    snprintf(vm->error,sizeof vm->error,"cannot open '%.*s': %s",
                             (int)path->length,path->chars,strerror(errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                diamond_set_cloexec(fileno(stream));
                DiamondFileHandle *handle=allocate_file_handle(vm,stream);
                if(handle==nullptr) {
                    fclose(stream);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_FILE_DELETE: {
                uint16_t dest=0,path_reg=0;
                READ_SHORT(dest);READ_SHORT(path_reg);
                VM_SANDBOX_GUARD("File.delete", "filesystem");
                if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"File.delete argument must be a String value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
                errno=0;
                if(remove(path->chars)!=0) {
                    if(errno==ENOENT) {
                        registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_BOOL,.as.boolean=false};
                        break;
                    }
                    snprintf(vm->error,sizeof vm->error,"cannot delete '%.*s': %s",
                             (int)path->length,path->chars,strerror(errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_BOOL,.as.boolean=true};
                break;
            }
            case DIAMOND_OP_DIR_ENTRIES: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_dir_entries(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_FILE_JOIN: {
                uint16_t dest=0,base=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(base);READ_BYTE(argc);
                DiamondValue join_result=DIAMOND_NIL;
                const DiamondVmStatus join_status=file_path_join_helper(vm,
                    &registers[base],argc,&join_result);
                VM_PROPAGATE(join_status);
                registers[dest]=join_result;
                break;
            }
            case DIAMOND_OP_FILE_PATH: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_file_path(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_REGEXP_NEW: {
                uint16_t dest=0,pattern_reg=0,options_reg=0;
                READ_SHORT(dest);READ_SHORT(pattern_reg);READ_SHORT(options_reg);
                if(registers[pattern_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[pattern_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[options_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                        "Regexp.new arguments must be a String pattern and an Int options");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondValue new_result=DIAMOND_NIL;
                const DiamondVmStatus new_status=regexp_new_helper(vm,
                    (const DiamondString *)registers[pattern_reg].as.object,
                    registers[options_reg].as.integer,&new_result);
                VM_PROPAGATE(new_status);
                registers[dest]=new_result;
                break;
            }
            case DIAMOND_OP_SQLITE3_OPEN: {
                uint16_t dest=0,path_reg=0,mode_reg=0;
                READ_SHORT(dest);READ_SHORT(path_reg);READ_SHORT(mode_reg);
                VM_SANDBOX_GUARD("SQLite3.open", "database");
#ifdef DIAMOND_NO_SQLITE
                (void)dest;(void)path_reg;(void)mode_reg;
                snprintf(vm->error,sizeof vm->error,
                    "SQLite3 support is not compiled into this build");
                VM_RETURN(DIAMOND_VM_SQLITE3_ERROR);
#else
                if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "SQLite3.open argument must be a String path");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
                sqlite3 *db=nullptr;
                const DiamondVmStatus open_status=
                    sqlite3_open_helper(vm,path,registers[mode_reg],&db);
                VM_PROPAGATE(open_status);
                DiamondSqlite3Handle *handle=allocate_sqlite3_handle(vm,db);
                if(handle==nullptr) {
                    sqlite3_close(db);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
#endif
                break;
            }
            case DIAMOND_OP_POSTGRES_OPEN: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_postgres_open(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_MYSQL_OPEN: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_mysql_open(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_PROGRAM_BUILDER_NEW: {
                uint16_t dest=0;
                READ_SHORT(dest);
                DiamondProgramBuilder *handle=allocate_program_builder(vm);
                if(handle==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_THREAD_NEW: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_thread_new(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_CHANNEL_NEW: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_channel_new(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_CHANNEL_SELECT: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_channel_select(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_CHANNEL_SELECT_SEND: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_channel_select_send(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_SUPERVISOR_NEW: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_supervisor_new(vm,chunk,registers,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_DNS_RESOLVE: {
                uint16_t dest=0,host_reg=0,channels_reg=0,deadline_reg=0;
                READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(channels_reg);READ_SHORT(deadline_reg);
                VM_SANDBOX_GUARD("DNS.resolve", "network");
                const size_t root_count=vm->gc_protected_count;
                if(!gc_protect(vm,registers[host_reg])||!gc_protect(vm,registers[channels_reg])) {
                    gc_unprotect(vm,root_count);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                const DiamondVmStatus status=dns_resolve_helper(vm,chunk,depth,
                    registers[host_reg],registers[channels_reg],registers[deadline_reg],&registers[dest]);
                gc_unprotect(vm,root_count);
                VM_PROPAGATE(status);
                break;
            }
            case DIAMOND_OP_TCP_CONNECT_NONBLOCK: {
                uint16_t dest=0,address_reg=0,port_reg=0;
                READ_SHORT(dest);READ_SHORT(address_reg);READ_SHORT(port_reg);
                VM_SANDBOX_GUARD("TCPSocket.connect_nonblocking", "network");
                if(registers[address_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[address_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                        "connect_nonblocking arguments must be a String address and an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondSocketHandle *handle=nullptr;
                const DiamondVmStatus status=tcp_connect_nonblocking_helper(vm,
                    (const DiamondString *)registers[address_reg].as.object,
                    registers[port_reg].as.integer,&handle);
                VM_PROPAGATE(status);
                registers[dest]=DIAMOND_OBJECT(handle);
                break;
            }
            case DIAMOND_OP_TCP_CONNECT: {
                uint16_t dest=0,host_reg=0,port_reg=0,options_reg=0;
                READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(port_reg);
                READ_SHORT(options_reg);
                VM_SANDBOX_GUARD("TCPSocket.connect", "network");
                if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "TCPSocket.connect arguments must be a String host and an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondSocketConnectOptions options;
                const DiamondVmStatus options_status=parse_socket_connect_options_helper(
                    vm,registers[options_reg],false,&options);
                VM_PROPAGATE(options_status);
                const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
                int connected_fd=-1;
                const DiamondVmStatus connect_status=tcp_connect_helper(vm,host,
                    registers[port_reg].as.integer,options.connect_timeout_ms,
                    options.recv_buffer_bytes,options.send_buffer_bytes,&connected_fd);
                VM_PROPAGATE(connect_status);
                const DiamondVmStatus timeout_status=apply_socket_timeouts_helper(vm,
                    connected_fd,options.read_timeout_ms,options.write_timeout_ms);
                if(timeout_status!=DIAMOND_VM_OK) {close(connected_fd);VM_RETURN(timeout_status);}
                FILE *stream=fdopen(connected_fd,"r+");
                if(stream==nullptr) {
                    close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                DiamondFileHandle *handle=allocate_file_handle(vm,stream);
                if(handle==nullptr) {
                    fclose(stream);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_TCP_LISTEN:
            case DIAMOND_OP_TCP_LISTEN_NONBLOCK: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_tcp_listen_nonblock(vm,chunk,registers,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_UDP_BIND: {
                uint16_t dest=0,port_reg=0,host_reg=0;
                READ_SHORT(dest);READ_SHORT(port_reg);READ_SHORT(host_reg);
                VM_SANDBOX_GUARD("UDPSocket.bind", "network");
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "UDPSocket.bind argument must be an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *udp_host=nullptr;
                if(registers[host_reg].kind!=DIAMOND_VALUE_NIL) {
                    if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                       registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                        snprintf(vm->error,sizeof vm->error,
                                 "UDPSocket.bind host: must be a String");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    udp_host=(const DiamondString *)registers[host_reg].as.object;
                }
                DiamondUdpSocketHandle *udp_handle=nullptr;
                const DiamondVmStatus udp_status=udp_socket_helper(vm,true,
                    registers[port_reg].as.integer,udp_host,&udp_handle);
                VM_PROPAGATE(udp_status);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)udp_handle};
                break;
            }
            case DIAMOND_OP_UDP_OPEN: {
                uint16_t dest=0;
                READ_SHORT(dest);
                VM_SANDBOX_GUARD("UDPSocket.open", "network");
                DiamondUdpSocketHandle *udp_handle=nullptr;
                const DiamondVmStatus udp_status=udp_socket_helper(vm,false,0,nullptr,&udp_handle);
                VM_PROPAGATE(udp_status);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)udp_handle};
                break;
            }
            case DIAMOND_OP_SIGNAL_TRAP: {
                VM_SANDBOX_GUARD("Signal.trap", "signals");
                uint16_t dest=0,name_reg=0,handler_reg=0;
                READ_SHORT(dest);READ_SHORT(name_reg);READ_SHORT(handler_reg);
                if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"Signal.trap name must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[handler_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[handler_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,"Signal.trap handler must be a Callable");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *signal_name=
                    (const DiamondString *)registers[name_reg].as.object;
                size_t signal_index=DIAMOND_SIGNAL_COUNT;
                for(size_t candidate=0;candidate<DIAMOND_SIGNAL_COUNT;candidate++) {
                    const size_t candidate_length=strlen(diamond_signal_names[candidate]);
                    if(signal_name->length==candidate_length&&
                       memcmp(signal_name->chars,diamond_signal_names[candidate],
                              candidate_length)==0) {
                        signal_index=candidate;break;
                    }
                }
                if(signal_index==DIAMOND_SIGNAL_COUNT) {
                    snprintf(vm->error,sizeof vm->error,"Signal.trap: unrecognized signal "
                        "name '%.*s' (supported: INT, TERM, HUP)",
                        (int)signal_name->length,signal_name->chars);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                struct sigaction action={0};
                action.sa_handler=diamond_signal_handler;
                sigemptyset(&action.sa_mask);
                /* Deliberately no SA_RESTART: a trapped signal arriving
                 * while blocked in accept()/poll()/recvfrom() needs that
                 * call to actually return EINTR so the handler can run
                 * promptly (see the accept/IO.poll/UDPSocket#receive
                 * opcode handlers) rather than the kernel silently
                 * resuming the blocking call as if nothing happened,
                 * which is what SA_RESTART would do -- and is exactly
                 * wrong for the motivating use case (a signal arriving
                 * while a server sits idle in accept()/poll() with
                 * nothing connecting). */
                action.sa_flags=0;
                if(sigaction(diamond_signal_numbers[signal_index],&action,nullptr)!=0) {
                    snprintf(vm->error,sizeof vm->error,"cannot trap signal: %s",
                        strerror(errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                vm->trapped_signal_handlers[signal_index]=registers[handler_reg];
                registers[dest]=DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_TLS_START_HANDSHAKE:
            case DIAMOND_OP_TLS_CONNECT: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_tls_connect(vm,chunk,registers,instruction,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_TLS_LISTEN: {
                uint16_t dest=0,port_reg=0,cert_reg=0,key_reg=0,options_reg=0;
                READ_SHORT(dest);READ_SHORT(port_reg);READ_SHORT(cert_reg);READ_SHORT(key_reg);
                READ_SHORT(options_reg);
                VM_SANDBOX_GUARD("TLSServer.listen", "network");
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT||
                   registers[cert_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cert_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[key_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[key_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"TLSServer.listen arguments must be "
                        "an Int port and String certificate/key file paths");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *alpn_protocols=nullptr;
                const DiamondString *client_ca=nullptr;
                const DiamondVmStatus options_status=parse_tls_listen_options_helper(
                    vm,registers[options_reg],&alpn_protocols,&client_ca);
                VM_PROPAGATE(options_status);
                const DiamondString *cert_path=
                    (const DiamondString *)registers[cert_reg].as.object;
                const DiamondString *key_path=
                    (const DiamondString *)registers[key_reg].as.object;
                DiamondListenerHandle *listener_handle=nullptr;
                const DiamondVmStatus listen_status=tls_listen_helper(vm,
                    registers[port_reg].as.integer,cert_path->chars,key_path->chars,
                    alpn_protocols,client_ca==nullptr?nullptr:client_ca->chars,
                    &listener_handle);
                VM_PROPAGATE(listen_status);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)listener_handle};
                break;
            }
            case DIAMOND_OP_IO_POLL: {
                ip_slot=ip;const DiamondVmStatus helper_status=outlined_io_poll(vm,chunk,registers,depth,&ip_slot);ip=ip_slot;
                VM_PROPAGATE(helper_status);
                break;
            }
            case DIAMOND_OP_CHR: {
                uint16_t dest=0,source=0;
                READ_SHORT(dest);READ_SHORT(source);
                if(registers[source].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,"chr argument must be an Int");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t code=registers[source].as.integer;
                if(code<0||code>255) {
                    snprintf(vm->error,sizeof vm->error,
                             "chr argument must be between 0 and 255");
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                const char byte=(char)code;
                DiamondString *string=allocate_string(vm,&byte,1);
                if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(string);break;
            }
            case DIAMOND_OP_TO_FLOAT: {
                uint16_t dest=0,source=0;
                READ_SHORT(dest);READ_SHORT(source);
                double as_double=0.0;
                if(!is_int_value(registers[source])||
                   !numeric_as_double(registers[source],&as_double)) {
                    snprintf(vm->error,sizeof vm->error,"to_f argument must be an Int");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                registers[dest]=DIAMOND_FLOAT(as_double);
                break;
            }
            case DIAMOND_OP_TO_INT: {
                uint16_t dest=0,source=0;
                READ_SHORT(dest);READ_SHORT(source);
                if(registers[source].kind!=DIAMOND_VALUE_FLOAT) {
                    snprintf(vm->error,sizeof vm->error,"to_i argument must be a Float");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondValue converted=DIAMOND_NIL;
                VM_PROPAGATE(float_to_int(vm,trunc(registers[source].as.real),"to_i",
                                          &converted));
                registers[dest]=converted;
                break;
            }
            case DIAMOND_OP_TO_SYMBOL: {
                uint16_t dest=0,source=0;
                READ_SHORT(dest);READ_SHORT(source);
                if(registers[source].kind!=DIAMOND_VALUE_OBJECT||
                   registers[source].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"to_sym argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *source_string=
                    (const DiamondString *)registers[source].as.object;
                DiamondSymbol *symbol=allocate_symbol(vm,source_string->chars,
                    source_string->length);
                if(symbol==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(symbol);
                break;
            }
            case DIAMOND_OP_JSON_STRINGIFY:
            case DIAMOND_OP_JSON_STRINGIFY_OPTIONS: {
                uint16_t dest=0,source=0,options=UINT16_MAX;
                READ_SHORT(dest);READ_SHORT(source);
                if(instruction==DIAMOND_OP_JSON_STRINGIFY_OPTIONS)READ_SHORT(options);
                DiamondValue json_text=DIAMOND_NIL;
                const DiamondVmStatus json_status=json_stringify_document(
                    vm,chunk,depth,registers[source],
                    options==UINT16_MAX?DIAMOND_NIL:registers[options],&json_text);
                VM_PROPAGATE(json_status);
                registers[dest]=json_text;
                break;
            }
            case DIAMOND_OP_MATH_UNARY: {
                uint16_t dest=0,source=0;uint8_t function_id=0;
                READ_SHORT(dest);READ_SHORT(source);READ_BYTE(function_id);
                double operand=0.0;
                if(!numeric_as_double(registers[source],&operand)) {
                    snprintf(vm->error,sizeof vm->error,
                             "math function argument must be an Int or Float");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                double math_result=0.0;
                switch((DiamondMathFunction)function_id) {
                    case DIAMOND_MATH_SQRT: math_result=sqrt(operand); break;
                    case DIAMOND_MATH_SIN: math_result=sin(operand); break;
                    case DIAMOND_MATH_COS: math_result=cos(operand); break;
                    case DIAMOND_MATH_TAN: math_result=tan(operand); break;
                    case DIAMOND_MATH_EXP: math_result=exp(operand); break;
                    case DIAMOND_MATH_LOG: math_result=log(operand); break;
                    case DIAMOND_MATH_TANH: math_result=tanh(operand); break;
                    default: VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[dest]=DIAMOND_FLOAT(math_result);
                break;
            }
            case DIAMOND_OP_MATH_BINARY: {
                uint16_t dest=0,left=0,right=0;uint8_t function_id=0;
                READ_SHORT(dest);READ_SHORT(left);READ_SHORT(right);READ_BYTE(function_id);
                double left_value=0.0,right_value=0.0;
                if(!numeric_as_double(registers[left],&left_value)||
                   !numeric_as_double(registers[right],&right_value)) {
                    snprintf(vm->error,sizeof vm->error,
                             "math function argument must be an Int or Float");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                double math_result=0.0;
                switch((DiamondMathFunction)function_id) {
                    case DIAMOND_MATH_POW: math_result=pow(left_value,right_value); break;
                    default: VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[dest]=DIAMOND_FLOAT(math_result);
                break;
            }
            default:
                VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
        }
dispatch_continue:
        continue;
    }

#undef READ_BYTE
#undef READ_SHORT
#undef READ_OPERANDS
#undef OPERAND_BYTE
#undef OPERAND_SHORT
#undef VM_RETURN
#undef VM_PROPAGATE
#undef VM_PROPAGATE_SIGNAL
#undef RECORD_ERROR

    if(depth==0&&vm->running_fiber!=nullptr&&ip>=bytecode_count) {
        *result=DIAMOND_NIL;
        vm->frames=frame.previous;
        free(heap_registers);
        free(tail_buffer);
        return DIAMOND_VM_OK;
    }
    vm->frames = frame.previous;
    free(heap_registers);
    free(tail_buffer);
    return DIAMOND_VM_INVALID_BYTECODE;
}
