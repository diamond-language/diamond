#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE): only for pthread_getattr_np,
 * used to learn a thread's own native stack bounds for the ASan
 * fiber-switch annotations below. */
#define _GNU_SOURCE

#include "vm.h"
#include "jit.h"
#include "bignum.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"
#include "vm_internal.h"

/* <crypt.h> exists on glibc (libxcrypt) and musl (see BCrypt.hash's own
 * comment below for what musl's version lacks), declaring crypt_r/
 * struct crypt_data/CRYPT_GENSALT_* -- but not on FreeBSD, which declares
 * plain crypt()/crypt_r() directly in <unistd.h> (already included below)
 * instead, with no separate header at all. __has_include, not an
 * __APPLE__/__FreeBSD__-style OS check (see docs/portability.md's own
 * "What hasn't been found" on why this codebase avoids those): this is a
 * feature test, and the same reasoning applies wherever else a libc omits
 * this header. */
#if __has_include(<crypt.h>)
#include <crypt.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sqlite3.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <zlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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
    if (depth >= DIAMOND_MAX_CALL_DEPTH) {
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
    const size_t live_register_count =
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
    PendingUnwind pending={};
    UnwindHandler handlers[16];
    size_t handler_count=0;
    size_t ip = 0;
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
            const char *frame_name = chunk->name != nullptr ? chunk->name    \
                                                              : "<chunk>"; \
            const uint32_t line = chunk->lines != nullptr                    \
                ? chunk->lines[instruction_offset] : 0;                      \
            const uint32_t column = chunk->columns != nullptr                \
                ? chunk->columns[instruction_offset] : 0;                    \
            if (used < sizeof(vm->error)) {                                  \
                (void)snprintf(vm->error + used, sizeof(vm->error) - used,   \
                               "\n  at %s:%u:%u", frame_name, line, column);  \
            }                                                               \
        }                                                                   \
    } while (false)

#define VM_RETURN(status_)                                           \
    do {                                                             \
        DiamondVmStatus return_status_=(status_);                     \
        if(return_status_==DIAMOND_VM_NONLOCAL_EXIT) {                \
            switch(nonlocal_exit_arrives(vm,chunk,&frame,registers,   \
                    handlers,&handler_count,&pending,&ip,             \
                    instruction_offset,result)) {                     \
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
                return return_status_;                               \
            }                                                        \
        }                                                            \
        if(handler_count>0 && catch_runtime_error(vm,chunk,           \
           return_status_,handlers,&handler_count,&pending,registers,&ip))\
            goto dispatch_continue;                                  \
        RECORD_ERROR(return_status_);                                \
        vm->frames = frame.previous;                                 \
        free(heap_registers);                                        \
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
            catch_exception(vm,chunk,handlers,&handler_count,&pending,registers,&ip)) {\
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
            catch_exception(vm,chunk,handlers,&handler_count,&pending,registers,&ip)) {\
            goto dispatch_continue;                                \
        }                                                           \
        VM_RETURN(status_);                                         \
    }

#define READ_BYTE(target_)                   \
    do {                                     \
        if (ip >= chunk->code_count) {       \
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE); \
        }                                    \
        (target_) = chunk->code[ip++];       \
    } while (false)

    /* Big-endian, matching the existing JUMP-target 16-bit operand
     * convention -- function indices (CALL/CALL_TYPED/CLOSURE) address the
     * dynamically growing function table beyond one-byte range. */
#define READ_SHORT(target_)                  \
    do {                                     \
        if (ip + 1 >= chunk->code_count) {   \
            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE); \
        }                                    \
        (target_) = (uint16_t)(((unsigned)chunk->code[ip] << 8) | chunk->code[ip + 1]); \
        ip += 2;                             \
    } while (false)

    while (ip < chunk->code_count) {
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
        if (instruction < DIAMOND_OP_COUNT)
            vm->opcode_counts[instruction]++;
        /* Cheap steady-state cost (one boolean read, false unless either
         * DIAMOND_MAX_INSTRUCTIONS or DIAMOND_MAX_WALL_MILLISECONDS is
         * configured) for docs/sandbox.md's own "Resource limits" --
         * same shape as diamond_pending_signals's own check just above.
         * The instruction-count comparison itself is cheap enough to run
         * every dispatch when active; the wall-clock check is additionally
         * masked (see DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK's own
         * comment) since clock_gettime is the genuinely non-trivial part.
         *
         * Both branches clear resource_limits_active (and the two budgets
         * themselves) *before* VM_RETURN -- unlike DIAMOND_MAX_CALL_DEPTH,
         * whose own `depth` parameter naturally shrinks as the call stack
         * unwinds (so a rescue clause in a shallower, already-returned-to
         * frame never re-trips it), instructions_executed only ever grows
         * and elapsed wall-clock time only ever increases: leaving either
         * budget "armed" after it first fires would re-trip this exact
         * check on the *very next* instruction dispatched -- including
         * every instruction needed to run a matching `rescue`/`ensure`
         * clause's own body -- so a program that correctly catches
         * ResourceLimitError could still never finish handling it. Once
         * either budget has genuinely been exceeded once, the VM has
         * already committed to reporting that outcome; letting the
         * program's own exception handling run to a normal conclusion
         * afterward (with no further limit interference) is the whole
         * point of it being a catchable exception rather than an abrupt
         * kill. */
        if (vm->resource_limits_active) {
            vm->instructions_executed++;
            if (vm->interrupt_flag != nullptr &&
                atomic_load_explicit(vm->interrupt_flag, memory_order_relaxed)) {
                VM_RETURN(DIAMOND_VM_INTERRUPTED);
            }
            if (vm->max_instructions != 0 &&
                vm->instructions_executed > vm->max_instructions) {
                vm->max_instructions = 0;
                vm->max_wall_nanoseconds = 0;
                vm->resource_limits_active = vm->interrupt_flag != nullptr;
                VM_RETURN(DIAMOND_VM_RESOURCE_LIMIT_ERROR);
            }
            if (vm->max_wall_nanoseconds != 0 &&
                (vm->instructions_executed & DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK) == 0) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                const int64_t now_ns = (int64_t)now.tv_sec * 1000000000LL + (int64_t)now.tv_nsec;
                if (now_ns - vm->start_time_ns > vm->max_wall_nanoseconds) {
                    vm->max_instructions = 0;
                    vm->max_wall_nanoseconds = 0;
                    vm->resource_limits_active = vm->interrupt_flag != nullptr;
                    VM_RETURN(DIAMOND_VM_RESOURCE_LIMIT_ERROR);
                }
            }
        }

        switch ((DiamondOpCode)instruction) {
            case DIAMOND_OP_CONSTANT: {
                uint16_t destination = 0;
                uint16_t constant = 0;
                READ_SHORT(destination);
                READ_SHORT(constant);
                if ((size_t)constant >= chunk->constant_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                registers[destination] = chunk->constants[constant];
                break;
            }
            case DIAMOND_OP_STRING: {
                uint16_t destination = 0;
                uint16_t string_index = 0;
                READ_SHORT(destination);
                READ_SHORT(string_index);
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
                READ_SHORT(destination);
                READ_SHORT(string_index);
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
                READ_SHORT(destination);
                READ_SHORT(boolean);
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
                break;
            }
            case DIAMOND_OP_TO_STRING: {
                uint16_t destination=0,source=0;
                READ_SHORT(destination);READ_SHORT(source);
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
                READ_SHORT(destination);
                READ_SHORT(source);
                registers[destination] = registers[source];
                break;
            }
            case DIAMOND_OP_ADD: {
                uint16_t destination = 0;
                uint16_t left = 0;
                uint16_t right = 0;
                READ_SHORT(destination);
                READ_SHORT(left);
                READ_SHORT(right);
                if (registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT) {
                    if (vm->quickening &&
                        ++vm->quickening_observations >= vm->quickening_threshold) {
                        uint8_t *code=(uint8_t *)(void *)chunk->code;
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
                READ_SHORT(destination);
                READ_SHORT(left);
                READ_SHORT(right);
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
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
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
                if(registers[left].kind==DIAMOND_VALUE_INT&&
                   registers[right].kind==DIAMOND_VALUE_INT&&
                   !value_is_bignum(registers[left])&&
                   !value_is_bignum(registers[right])) {
                    const int64_t shift_amount=registers[right].as.integer;
                    if(shift_amount<0||shift_amount>=64) {
                        snprintf(vm->error,sizeof vm->error,
                            "shift amount must be between 0 and 63");
                        VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                    }
                    const int64_t left_value=registers[left].as.integer;
                    const int64_t result_value=(int64_t)
                        ((uint64_t)left_value<<(unsigned)shift_amount);
                    registers[destination]=DIAMOND_INT(result_value);
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
                /* Int only -- same scope cut as DIAMOND_OP_SHIFT_LEFT
                 * (no bignum, not user-overloadable, no quickening; see
                 * that case's own comment). Arithmetic (sign-extending),
                 * matching Ruby's own Integer#>>, and well-defined for a
                 * negative left-hand side under C23 (this project's own
                 * -std=c23) -- not the "implementation-defined" territory
                 * an older C standard would put a signed right-shift in. */
                uint16_t destination=0,left=0,right=0;
                READ_SHORT(destination);READ_SHORT(left);READ_SHORT(right);
                if(registers[left].kind!=DIAMOND_VALUE_INT||
                   registers[right].kind!=DIAMOND_VALUE_INT||
                   value_is_bignum(registers[left])||value_is_bignum(registers[right])) {
                    format_operator_type_error(vm,registers[left],registers[right],">>");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t shift_amount=registers[right].as.integer;
                if(shift_amount<0||shift_amount>=64) {
                    snprintf(vm->error,sizeof vm->error,
                        "shift amount must be between 0 and 63");
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                registers[destination]=
                    DIAMOND_INT(registers[left].as.integer>>(unsigned)shift_amount);
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
                   registers[right].kind!=DIAMOND_VALUE_INT||
                   value_is_bignum(registers[left])||value_is_bignum(registers[right])) {
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
                READ_SHORT(destination);READ_SHORT(left);READ_SHORT(right);
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
                   registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=chunk->code+instruction_offset;
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
                READ_SHORT(destination);
                READ_SHORT(operand);
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
                    const uint8_t *site=chunk->code+instruction_offset;
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
                READ_SHORT(destination);
                READ_SHORT(left);
                READ_SHORT(right);
                const bool integer_operands =
                    registers[left].kind == DIAMOND_VALUE_INT &&
                    registers[right].kind == DIAMOND_VALUE_INT;
                if (vm->quickening && integer_operands &&
                    (opcode == DIAMOND_OP_EQUAL || opcode == DIAMOND_OP_NOT_EQUAL) &&
                    ++vm->quickening_observations >= vm->quickening_threshold) {
                    const DiamondOpCode specialized = opcode == DIAMOND_OP_EQUAL
                        ? DIAMOND_OP_EQUAL_INT : DIAMOND_OP_NOT_EQUAL_INT;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)specialized;
                    opcode=specialized;
                    vm->quickened_sites++;
                }
                if ((opcode == DIAMOND_OP_EQUAL_INT ||
                     opcode == DIAMOND_OP_NOT_EQUAL_INT) && !integer_operands) {
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
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
                    const uint8_t *site=chunk->code+instruction_offset;
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
                READ_SHORT(destination);
                READ_SHORT(left);
                READ_SHORT(right);
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
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
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
                READ_SHORT(destination);READ_SHORT(left);READ_SHORT(right);
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
                    const uint8_t *site=chunk->code+instruction_offset;
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
                    chunk->code+instruction_offset,registers[pattern],
                    registers[subject],&matched);
                VM_PROPAGATE(status);
                registers[destination]=DIAMOND_BOOL(matched);break;
            }
            case DIAMOND_OP_JUMP: {
                uint8_t high = 0;
                uint8_t low = 0;
                READ_BYTE(high);
                READ_BYTE(low);
                const size_t target = ((size_t)high << 8) | low;
                if (target > chunk->code_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                ip = target;
                break;
            }
            case DIAMOND_OP_JUMP_IF_FALSE: {
                uint16_t condition = 0;
                uint8_t high = 0;
                uint8_t low = 0;
                READ_SHORT(condition);
                READ_BYTE(high);
                READ_BYTE(low);
                const size_t target = ((size_t)high << 8) | low;
                if (target > chunk->code_count) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                if (!is_truthy(registers[condition])) {
                    ip = target;
                }
                break;
            }
            case DIAMOND_OP_JUMP_IF_TRUE: {
                uint16_t condition=0;uint8_t high=0,low=0;
                READ_SHORT(condition);READ_BYTE(high);READ_BYTE(low);
                const size_t target=((size_t)high<<8)|low;
                if(target>chunk->code_count) VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(is_truthy(registers[condition])) ip=target;
                break;
            }
            case DIAMOND_OP_CALL: {
                uint16_t destination = 0;
                uint16_t function_index = 0;
                uint16_t argument_base = 0;
                uint8_t call_argument_count = 0;
                READ_SHORT(destination);
                READ_SHORT(function_index);
                READ_SHORT(argument_base);
                READ_BYTE(call_argument_count);
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
                const DiamondChunk called_chunk = {
                    .name = function->name,
                    .code = function->code,
                    .lines = function->lines,
                    .columns = function->columns,
                    .code_count = function->code_count,
                    .constants = function->constants,
                    .constant_count = function->constant_count,
                    .strings = function->strings,
                    .string_count = function->string_count,
                    .type_sets = function->type_sets,
                    .type_set_count = function->type_set_count,
                    .functions = chunk->functions,
                    .function_count = chunk->function_count,
                    .classes = chunk->classes,
                    .class_count = chunk->class_count,
                    .interfaces=chunk->interfaces,
                    .interface_count=chunk->interface_count,
                    .parameter_type_sets=function->parameter_type_sets,
                    .type_variable_count=function->type_variable_count,
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
                    .register_count=function->register_count,
                    .has_variadic=function->has_variadic,
                };
                DiamondValue call_result = DIAMOND_NIL;
                const DiamondVmStatus status = jit_call_or_interpret(vm, function,
                    &called_chunk, &registers[argument_base], call_argument_count,
                    depth, nullptr, &call_result);
                VM_PROPAGATE(status);
                registers[destination] = call_result;
                break;
            }
            case DIAMOND_OP_TAIL_CALL: {
                /* Only ever produced by the compiler rewriting an
                 * already-validated self-recursive CALL in tail position
                 * (see maybe_rewrite_self_tail_call, src/compiler.c) --
                 * `function_index`/`destination` are intentionally unread:
                 * the target is always *this* function (chunk/registers/
                 * frame/depth all stay exactly as they are, since a self-
                 * call can never need a different one of any of them),
                 * and a value that would have been returned here was
                 * already checked byte-for-byte to be the immediately
                 * preceding CALL's own destination, needing no separate
                 * confirmation at run time. Bounds/arity were already
                 * enforced when that CALL itself first compiled; the only
                 * new check needed here is argument_base/count staying in
                 * range, the same defensive validation CALL's own case
                 * applies, in case this was ever hand-built directly
                 * (ProgramBuilder exposes raw opcode numbers). */
                uint16_t destination = 0, function_index = 0, argument_base = 0;
                uint8_t call_argument_count = 0;
                READ_SHORT(destination); /* unused, see above */
                READ_SHORT(function_index); /* unused, see above */
                READ_SHORT(argument_base);
                READ_BYTE(call_argument_count);
                (void)destination;
                (void)function_index;
                if ((size_t)argument_base + call_argument_count >
                        DIAMOND_REGISTER_COUNT ||
                    call_argument_count > DIAMOND_MAX_DECLARED_PARAMETERS) {
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                /* Captured into a small local buffer *before* zeroing
                 * registers[] below -- argument_base is ordinary bytecode
                 * addressing the same register file being cleared, unlike
                 * a real CALL's own `arguments` (always a genuinely
                 * separate C array, the caller's own registers one C
                 * stack frame up). */
                DiamondValue tail_arguments[DIAMOND_MAX_DECLARED_PARAMETERS];
                for (size_t index = 0; index < call_argument_count; index++)
                    tail_arguments[index] = registers[argument_base + index];
                memset(registers, 0, live_register_count * sizeof(DiamondValue));
                const size_t copied = (size_t)call_argument_count < live_register_count
                    ? (size_t)call_argument_count : live_register_count;
                for (size_t index = 0; index < copied; index++)
                    registers[index] = tail_arguments[index];
                ip = 0;
                break;
            }
            case DIAMOND_OP_CALL_TYPED: {
                uint16_t destination=0,argument_base=0;
                uint16_t function_index=0;
                uint8_t call_argument_count=0,type_argument_count=0;
                READ_SHORT(destination);READ_SHORT(function_index);
                READ_SHORT(argument_base);READ_BYTE(call_argument_count);
                READ_BYTE(type_argument_count);
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
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
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
                uint16_t destination=0,function_index=0,array_register=0;
                READ_SHORT(destination);READ_SHORT(function_index);
                READ_SHORT(array_register);
                const bool has_keywords=(DiamondOpCode)instruction==
                    DIAMOND_OP_CALL_KEYWORD_SPREAD||
                    (DiamondOpCode)instruction==
                    DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD;
                uint8_t keyword_count=0,keyword_slots[DIAMOND_MAX_DECLARED_PARAMETERS];
                uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
                if(has_keywords) {
                    READ_BYTE(keyword_count);
                    if(keyword_count==0||keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<keyword_count;index++) {
                        READ_BYTE(keyword_slots[index]);
                        READ_SHORT(keyword_registers[index]);
                    }
                }
                uint8_t type_argument_count=0;uint16_t type_arguments[8];
                if((DiamondOpCode)instruction==DIAMOND_OP_CALL_TYPED_SPREAD||
                   (DiamondOpCode)instruction==
                    DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD) {
                    READ_BYTE(type_argument_count);
                    if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_argument_count;index++)
                        READ_SHORT(type_arguments[index]);
                }
                if((size_t)function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[array_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[array_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *spread=
                    (const DiamondArray *)registers[array_register].as.object;
                const DiamondFunction *function=chunk->functions[function_index];
                for(size_t index=0;index<keyword_count;index++)
                    if(keyword_slots[index]>=function->arity||
                       keyword_registers[index]>=DIAMOND_REGISTER_COUNT)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(type_argument_count>0&&
                   type_argument_count!=function->type_variable_count)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                /* Only the first type_argument_count entries are ever read (below,
                 * and through .type_variable_bindings); zeroing all eight cost about
                 * 3 KB of memset on every call, typed or not. */
                DiamondTypeBinding explicit_bindings[8];
                for(size_t index=0;index<type_argument_count;index++) {
                    explicit_bindings[index]=(DiamondTypeBinding){};
                    if((size_t)type_arguments[index]>=chunk->type_set_count)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                        chunk->type_sets,type_arguments[index]);
                }
                const DiamondValue *call_arguments=spread->values;
                size_t call_argument_count=spread->count;
                DiamondValue *merged_arguments=nullptr;
                if(has_keywords) {
                    bool filled[DIAMOND_MAX_DECLARED_PARAMETERS]={0};
                    if(spread->count>DIAMOND_MAX_DECLARED_PARAMETERS)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    for(size_t index=0;index<spread->count;index++)filled[index]=true;
                    for(size_t index=0;index<keyword_count;index++) {
                        const size_t slot=keyword_slots[index];
                        if(filled[slot]) {
                            snprintf(vm->error,sizeof vm->error,
                                "multiple values for the same argument");
                            VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        }
                        filled[slot]=true;
                        if(slot+1>call_argument_count)call_argument_count=slot+1;
                    }
                    for(size_t index=0;index<call_argument_count;index++)
                        if(!filled[index]) {
                            snprintf(vm->error,sizeof vm->error,"missing argument");
                            VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        }
                    merged_arguments=malloc(call_argument_count*
                        sizeof *merged_arguments);
                    if(merged_arguments==nullptr)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    for(size_t index=0;index<spread->count;index++)
                        merged_arguments[index]=spread->values[index];
                    for(size_t index=0;index<keyword_count;index++)
                        merged_arguments[keyword_slots[index]]=
                            registers[keyword_registers[index]];
                    call_arguments=merged_arguments;
                }
                if(call_argument_count<function->required_arity||
                   (call_argument_count>function->arity&&!function->has_variadic)) {
                    free(merged_arguments);
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                const DiamondChunk called_chunk={
                    .name=function->name,.code=function->code,
                    .lines=function->lines,.columns=function->columns,
                    .code_count=function->code_count,
                    .constants=function->constants,
                    .constant_count=function->constant_count,
                    .strings=function->strings,.string_count=function->string_count,
                    .type_sets=function->type_sets,
                    .type_set_count=function->type_set_count,
                    .functions=chunk->functions,.function_count=chunk->function_count,
                    .classes=chunk->classes,.class_count=chunk->class_count,
                    .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                    .parameter_type_sets=function->parameter_type_sets,
                    .type_variable_count=function->type_variable_count,
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
                    .type_variable_bindings=type_argument_count==0?nullptr:
                        explicit_bindings,
                    .register_count=function->register_count,
                    .has_variadic=function->has_variadic,
                };
                DiamondValue spread_result=DIAMOND_NIL;
                /* spread->values is already a plain contiguous
                 * DiamondValue* -- an Array's normal in-memory shape --
                 * so it's passed straight to run_chunk as `arguments`,
                 * no copy needed, the same way an ordinary call already
                 * passes a live pointer into its own caller's registers
                 * (&registers[argument_base] above) into a nested
                 * run_chunk frame. `spread` itself stays reachable
                 * throughout via registers[array_register], the same
                 * live root any other in-use register already is. */
                const DiamondVmStatus spread_status=run_chunk(&called_chunk,vm,
                    call_arguments,call_argument_count,depth+1,nullptr,
                    &spread_result);
                free(merged_arguments);
                VM_PROPAGATE(spread_status);
                registers[destination]=spread_result;
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
                uint16_t destination=0,function_index=0,positional_register=0;
                uint8_t class_index=0,needs_receiver=0,keyword_count=0;
                uint16_t keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS],type_arguments[8];uint8_t type_count=0;
                uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
                READ_SHORT(destination);READ_SHORT(function_index);
                READ_SHORT(positional_register);READ_BYTE(class_index);
                READ_BYTE(needs_receiver);READ_BYTE(keyword_count);
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
                if((DiamondOpCode)instruction==
                        DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS) {
                    READ_BYTE(type_count);if(type_count>8)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_count;index++)READ_SHORT(type_arguments[index]);
                }
                if((size_t)function_index>=chunk->function_count||needs_receiver>1||
                   (class_index!=UINT8_MAX&&(size_t)class_index>=chunk->class_count)||
                   registers[positional_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[positional_register].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *function=chunk->functions[function_index];
                if(function->arity<(uint8_t)needs_receiver||
                   (type_count>0&&type_count!=function->type_variable_count))
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondValue *merged=nullptr;size_t merged_count=0;
                DiamondVmStatus status=merge_keyword_arguments(vm,chunk,function,
                    (const DiamondArray *)registers[positional_register].as.object,
                    keyword_names,keyword_registers,keyword_count,registers,
                    function->arity-needs_receiver,
                    has_block?&registers[block_register]:nullptr,
                    &merged,&merged_count);
                VM_PROPAGATE(status);
                const size_t total=merged_count+needs_receiver;
                if(total<function->required_arity||
                   (total>function->arity&&!function->has_variadic)) {
                    free(merged);VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                DiamondValue *singleton_arguments=malloc(total*
                    sizeof *singleton_arguments);
                if(singleton_arguments==nullptr&&total>0) {free(merged);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);}
                if(needs_receiver)singleton_arguments[0]=class_index==UINT8_MAX?
                    DIAMOND_NIL:DIAMOND_CLASS(class_index);
                for(size_t index=0;index<merged_count;index++)
                    singleton_arguments[index+needs_receiver]=merged[index];
                free(merged);
                DiamondTypeBinding singleton_bindings[8];
                for(size_t index=0;index<type_count;index++) {
                    singleton_bindings[index]=(DiamondTypeBinding){};
                    if((size_t)type_arguments[index]>=chunk->type_set_count) {
                        free(singleton_arguments);VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);}
                    (void)binding_node(&singleton_bindings[index]);
                    bind_context_set(&singleton_bindings[index],0,chunk,chunk->type_sets,
                        type_arguments[index]);
                }
                DiamondChunk child={.name=function->name,.code=function->code,
                    .lines=function->lines,.columns=function->columns,
                    .code_count=function->code_count,.constants=function->constants,
                    .constant_count=function->constant_count,.strings=function->strings,
                    .string_count=function->string_count,.type_sets=function->type_sets,
                    .type_set_count=function->type_set_count,.functions=chunk->functions,
                    .function_count=chunk->function_count,.classes=chunk->classes,
                    .class_count=chunk->class_count,.interfaces=chunk->interfaces,
                    .interface_count=chunk->interface_count,
                    .parameter_type_sets=function->parameter_type_sets,
                    .type_variable_count=function->type_variable_count,
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
                    .type_variable_bindings=type_count==0?nullptr:singleton_bindings,
                    .register_count=function->register_count,
                    .has_variadic=function->has_variadic};
                DiamondValue singleton_result=DIAMOND_NIL;
                status=run_chunk(&child,vm,singleton_arguments,total,depth+1,
                    nullptr,&singleton_result);
                free(singleton_arguments);VM_PROPAGATE(status);
                registers[destination]=singleton_result;break;
            }
            case DIAMOND_OP_CALL_SINGLETON_SPREAD:
            case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: {
                uint16_t destination=0,function_index=0,spread_register=0;
                uint8_t class_index=0,needs_receiver=0;
                READ_SHORT(destination);READ_SHORT(function_index);
                READ_SHORT(spread_register);READ_BYTE(class_index);
                READ_BYTE(needs_receiver);
                uint8_t type_argument_count=0;uint16_t type_arguments[8];
                if((DiamondOpCode)instruction==
                        DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD) {
                    READ_BYTE(type_argument_count);
                    if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_argument_count;index++)
                        READ_SHORT(type_arguments[index]);
                }
                if((size_t)function_index>=chunk->function_count||
                   needs_receiver>1||
                   (class_index!=UINT8_MAX&&(size_t)class_index>=chunk->class_count))
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *spread=(const DiamondArray *)
                    registers[spread_register].as.object;
                const DiamondFunction *function=chunk->functions[function_index];
                if(type_argument_count>0&&
                   type_argument_count!=function->type_variable_count)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                /* Only the first type_argument_count entries are ever read (below,
                 * and through .type_variable_bindings); zeroing all eight cost about
                 * 3 KB of memset on every call, typed or not. */
                DiamondTypeBinding explicit_bindings[8];
                for(size_t index=0;index<type_argument_count;index++) {
                    explicit_bindings[index]=(DiamondTypeBinding){};
                    if((size_t)type_arguments[index]>=chunk->type_set_count)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                        chunk->type_sets,type_arguments[index]);
                }
                const size_t spread_argument_count=
                    spread->count+(needs_receiver?1:0);
                if(spread_argument_count<function->required_arity||
                   (spread_argument_count>function->arity&&!function->has_variadic))
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                const DiamondValue *spread_arguments=spread->values;
                DiamondValue *padded=nullptr;
                if(needs_receiver) {
                    if(spread->count>DIAMOND_REGISTER_COUNT-1)
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    padded=malloc(spread_argument_count*sizeof *padded);
                    if(padded==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    padded[0]=class_index==UINT8_MAX?DIAMOND_NIL:
                        DIAMOND_CLASS(class_index);
                    for(size_t index=0;index<spread->count;index++)
                        padded[index+1]=spread->values[index];
                    spread_arguments=padded;
                }
                const DiamondChunk called_chunk={
                    .name=function->name,.code=function->code,
                    .lines=function->lines,.columns=function->columns,
                    .code_count=function->code_count,
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
                    .parameter_offset=function->owner_class==UINT8_MAX?0:1,
                    .type_variable_bindings=type_argument_count==0?nullptr:
                        explicit_bindings,
                    .register_count=function->register_count,
                    .has_variadic=function->has_variadic};
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&called_chunk,vm,
                    spread_arguments,spread_argument_count,depth+1,nullptr,
                    &call_result);
                free(padded);VM_PROPAGATE(status);
                registers[destination]=call_result;break;
            }
            case DIAMOND_OP_CLOSURE: {
                uint16_t dest=0;uint8_t count=0;uint16_t index=0;
                READ_SHORT(dest);READ_SHORT(index);READ_BYTE(count);
                /* This check is load-bearing, unlike a same-shaped one
                 * might look for DIAMOND_MAX_ARGUMENTS elsewhere: count
                 * is a uint8_t (0-255) but DIAMOND_MAX_CAPTURES is only
                 * 32 (object.h -- DiamondClosure's own captures field is
                 * a real per-instance runtime cost, not a one-off
                 * compile-time buffer), so count genuinely can exceed it.
                 * A first pass of the capture-cap fix this constant is
                 * part of dropped this check by (wrongly, at the time)
                 * assuming it -- see object.h's own DIAMOND_MAX_CAPTURES
                 * comment for exactly what that let happen. */
                if(index>=chunk->function_count||count>DIAMOND_MAX_CAPTURES)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captures[DIAMOND_MAX_CAPTURES];
                for(size_t i=0;i<count;i++){uint16_t reg=0;READ_SHORT(reg);captures[i]=registers[reg];}
                DiamondClosure *created=allocate_closure(vm,index,captures,count);
                if(created==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
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
                uint16_t dest=0,index=0;READ_SHORT(dest);READ_SHORT(index);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=((DiamondCell *)captured.as.object)->value;break;
            }
            case DIAMOND_OP_GET_CAPTURE_CELL: {
                uint16_t dest=0,index=0;READ_SHORT(dest);READ_SHORT(index);
                if(closure==nullptr||index>=closure->capture_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captured=closure->captures[index];
                if(captured.kind!=DIAMOND_VALUE_OBJECT||captured.as.object->kind!=DIAMOND_OBJECT_CELL)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=captured;break;
            }
            case DIAMOND_OP_SET_CAPTURE: {
                uint16_t index=0,source=0;READ_SHORT(index);READ_SHORT(source);
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
                uint16_t dest=0,cell_reg=0;READ_SHORT(dest);READ_SHORT(cell_reg);
                if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[dest]=((DiamondCell *)registers[cell_reg].as.object)->value;break;
            }
            case DIAMOND_OP_SET_CELL: {
                uint16_t cell_reg=0,source=0;READ_SHORT(cell_reg);READ_SHORT(source);
                if(registers[cell_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cell_reg].as.object->kind!=DIAMOND_OBJECT_CELL)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ((DiamondCell *)registers[cell_reg].as.object)->value=registers[source];
                if(!gc_write_barrier(vm,registers[cell_reg].as.object))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                break;
            }
            case DIAMOND_OP_CALL_CLOSURE: {
                uint16_t dest=0,callable=0,base=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(callable);READ_SHORT(base);READ_BYTE(argc);
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
                uint16_t dest=0,base=0;uint8_t ci=0,argc=0;
                READ_SHORT(dest);READ_BYTE(ci);READ_SHORT(base);READ_BYTE(argc);
                const DiamondVmStatus s=diamond_jit_new_instance(vm,chunk,
                    registers,dest,ci,base,argc,depth);
                VM_PROPAGATE(s);
                break;
            }
            case DIAMOND_OP_NEW_KEYWORDS: {
                uint16_t destination=0,positional_register=0;
                uint8_t class_index=0,keyword_count=0;uint16_t keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];
                uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
                READ_SHORT(destination);READ_BYTE(class_index);
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
                const DiamondClass *class=&chunk->classes[class_index];
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
                uint8_t code[9]={DIAMOND_OP_NEW_SPREAD,0,1,class_index,0,0,
                    DIAMOND_OP_RETURN,0,1};uint32_t locations[9]={0};
                DiamondChunk synthetic=*chunk;synthetic.name="<keyword new>";
                synthetic.code=code;synthetic.lines=locations;
                synthetic.columns=locations;synthetic.code_count=9;
                synthetic.register_count=2;
                const DiamondValue argument=DIAMOND_OBJECT(merged_array);
                DiamondValue constructor_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&synthetic,vm,&argument,1,
                    depth+1,nullptr,&constructor_result);
                gc_unprotect(vm,protected_count);VM_PROPAGATE(status);
                registers[destination]=constructor_result;break;
            }
            case DIAMOND_OP_NEW_SPREAD: {
                uint16_t dest=0,spread_register=0;uint8_t class_index=0;
                READ_SHORT(dest);READ_BYTE(class_index);READ_SHORT(spread_register);
                if((size_t)class_index>=chunk->class_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *spread=(const DiamondArray *)
                    registers[spread_register].as.object;
                const DiamondClass *class=&chunk->classes[class_index];
                DiamondInstance *instance=allocate_instance(vm,class,nullptr);
                if(instance==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(instance);
                const DiamondMethod *initialize=lookup_method(chunk,class,
                    "initialize",sizeof("initialize")-1);
                if(initialize!=nullptr) {
                    if(spread->count<initialize->required_arity||
                       (spread->count>initialize->arity&&
                        !initialize->has_variadic))
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(spread->count>(size_t)DIAMOND_REGISTER_COUNT-1-
                            initialize->bound_value_count)
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    const size_t total=spread->count+1+
                        initialize->bound_value_count;
                    DiamondValue *args=malloc(total*sizeof *args);
                    if(args==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    args[0]=registers[dest];
                    for(size_t index=0;index<spread->count;index++)
                        args[index+1]=spread->values[index];
                    for(size_t index=0;index<initialize->bound_value_count;index++)
                        args[spread->count+1+index]=initialize->bound_values[index];
                    const DiamondChunk *function_chunk=initialize->source_chunk!=nullptr?
                        initialize->source_chunk:chunk;
                    const DiamondFunction *fn=
                        function_chunk->functions[initialize->function_index];
                    DiamondChunk child={.name=fn->name,.code=fn->code,
                      .lines=fn->lines,.columns=fn->columns,
                      .code_count=fn->code_count,.constants=fn->constants,
                      .constant_count=fn->constant_count,.strings=fn->strings,
                      .string_count=fn->string_count,.type_sets=fn->type_sets,
                      .type_set_count=fn->type_set_count,
                      .functions=function_chunk->functions,
                      .function_count=function_chunk->function_count,
                      .classes=function_chunk->classes,
                      .class_count=function_chunk->class_count,
                      .interfaces=function_chunk->interfaces,
                      .interface_count=function_chunk->interface_count,
                      .parameter_type_sets=fn->parameter_type_sets,
                      .type_variable_count=fn->type_variable_count,
                      .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                      .register_count=fn->register_count,
                      .has_variadic=fn->has_variadic};
                    DiamondValue ignored=DIAMOND_NIL;
                    const DiamondVmStatus status=run_chunk(&child,vm,args,total,
                        depth+1,nullptr,&ignored);
                    free(args);VM_PROPAGATE(status);
                } else {
                    bool exception_class=false;const DiamondClass *ancestor=class;
                    while(ancestor!=nullptr) {
                        if(ancestor==&chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
                            exception_class=true;break;
                        }
                        ancestor=ancestor->superclass==UINT8_MAX?nullptr:
                            &chunk->classes[ancestor->superclass];
                    }
                    if(exception_class) {
                        if(spread->count>2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(spread->count>0)instance->fields[0]=spread->values[0];
                        if(spread->count>1)instance->fields[1]=spread->values[1];
                    } else if(spread->count!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                break;
            }
            case DIAMOND_OP_INVOKE_KEYWORDS:
            case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: {
                uint16_t dest=0,recv=0,positional_register=0;
                uint16_t method_name_index=0,keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];uint8_t keyword_count=0;
                uint16_t keyword_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
                READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(method_name_index);
                READ_SHORT(positional_register);READ_BYTE(keyword_count);
                const bool has_block=(keyword_count&0x80u)!=0;
                keyword_count&=0x7fu;
                if((keyword_count==0&&!has_block)||keyword_count>DIAMOND_MAX_DECLARED_PARAMETERS||
                   (size_t)method_name_index>=chunk->string_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                for(size_t index=0;index<keyword_count;index++) {
                    READ_SHORT(keyword_names[index]);READ_SHORT(keyword_registers[index]);
                }
                uint16_t block_register=0;
                if(has_block)READ_SHORT(block_register);
                if(has_block&&block_register>=DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                uint8_t type_count=0;uint16_t type_arguments[8];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED_KEYWORDS) {
                    READ_BYTE(type_count);if(type_count>8)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_count;index++)READ_SHORT(type_arguments[index]);
                }
                if(registers[positional_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[positional_register].as.object->kind!=DIAMOND_OBJECT_ARRAY)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE) {
                    const DiamondStringConstant *native_name=
                        &chunk->strings[method_name_index];
                    if(type_count!=0)VM_REJECT_TYPE_ARGUMENTS(native_name);
                    if(has_block) {
                        snprintf(vm->error,sizeof vm->error,
                            "native keyword method cannot take a block");
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    }
                    const NativeKeywordSignature *signature=
                        native_keyword_signature(native_name);
                    if(signature==nullptr) {snprintf(vm->error,sizeof vm->error,
                        "no parameter with this name");
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);}
                    DiamondValue *native_merged=nullptr;size_t native_count=0;
                    const DiamondVmStatus native_merge=merge_native_keyword_arguments(vm,
                        chunk,signature,(const DiamondArray *)
                        registers[positional_register].as.object,keyword_names,
                        keyword_registers,keyword_count,registers,&native_merged,
                        &native_count);
                    VM_PROPAGATE(native_merge);
                    DiamondArray *native_array=allocate_array(vm,native_merged,native_count);
                    free(native_merged);if(native_array==nullptr)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    const size_t native_protected=vm->gc_protected_count;
                    if(!gc_protect(vm,DIAMOND_OBJECT(native_array)))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    uint8_t native_code[12]={DIAMOND_OP_INVOKE_SPREAD,
                        0,2,0,0,(uint8_t)(method_name_index>>8),
                        (uint8_t)method_name_index,0,1,
                        DIAMOND_OP_RETURN,0,2};
                    uint32_t native_locations[12]={0};
                    DiamondChunk native_chunk=*chunk;
                    native_chunk.name="<native keyword invoke>";
                    native_chunk.code=native_code;
                    native_chunk.lines=native_locations;
                    native_chunk.columns=native_locations;
                    native_chunk.code_count=12;native_chunk.register_count=3;
                    DiamondValue native_arguments[2]={registers[recv],
                        DIAMOND_OBJECT(native_array)};
                    DiamondValue native_result=DIAMOND_NIL;
                    const DiamondVmStatus native_status=run_chunk(&native_chunk,vm,
                        native_arguments,2,depth+1,nullptr,&native_result);
                    gc_unprotect(vm,native_protected);VM_PROPAGATE(native_status);
                    registers[dest]=native_result;break;
                }
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const DiamondChunk *owner=instance->owner!=nullptr?
                    instance->owner:vm->root_chunk;
                const DiamondStringConstant *method_name=
                    &chunk->strings[method_name_index];
                const DiamondMethod *method=lookup_method(owner,instance->class,
                    method_name->chars,method_name->length);
                if(method==nullptr) {
                    /* Unlike the plain INVOKE opcodes, a keyword call has no
                     * method_missing fallback: method_missing's own
                     * `(name, args)` contract has no established shape for
                     * keyword arguments, so this stays a hard error. */
                    snprintf(vm->error,sizeof vm->error,
                        "undefined method '%.*s' for an instance of %s",
                        (int)method_name->length,method_name->chars,
                        instance->class->name);
                    VM_RETURN(DIAMOND_VM_NO_METHOD_ERROR);
                }
                const DiamondChunk *function_chunk=method->source_chunk!=nullptr?
                    method->source_chunk:owner;
                const DiamondFunction *fn=
                    function_chunk->functions[method->function_index];
                DiamondValue *merged=nullptr;size_t merged_count=0;
                const DiamondVmStatus merge_status=merge_keyword_arguments(vm,chunk,
                    fn,(const DiamondArray *)registers[positional_register].as.object,
                    keyword_names,keyword_registers,keyword_count,registers,
                    method->arity,has_block?&registers[block_register]:nullptr,
                    &merged,&merged_count);
                VM_PROPAGATE(merge_status);
                DiamondArray *merged_array=allocate_array(vm,merged,merged_count);
                free(merged);if(merged_array==nullptr)
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                const size_t protected_count=vm->gc_protected_count;
                if(!gc_protect(vm,DIAMOND_OBJECT(merged_array)))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                uint8_t code[32]={0};size_t code_count=0;
#define KEYWORD_SHORT(value) do {code[code_count++]=(uint8_t)((value)>>8); \
    code[code_count++]=(uint8_t)(value);} while(false)
                code[code_count++]=(uint8_t)(type_count==0?
                    DIAMOND_OP_INVOKE_SPREAD:DIAMOND_OP_INVOKE_TYPED_SPREAD);
                KEYWORD_SHORT(2);KEYWORD_SHORT(0);KEYWORD_SHORT(method_name_index);
                KEYWORD_SHORT(1);
                if(type_count>0) {code[code_count++]=type_count;
                    for(size_t index=0;index<type_count;index++)
                        KEYWORD_SHORT(type_arguments[index]);}
                code[code_count++]=DIAMOND_OP_RETURN;KEYWORD_SHORT(2);
#undef KEYWORD_SHORT
                uint32_t locations[32]={0};
                DiamondChunk synthetic=*chunk;synthetic.name="<keyword invoke>";
                synthetic.code=code;synthetic.lines=locations;
                synthetic.columns=locations;synthetic.code_count=code_count;
                synthetic.register_count=3;
                DiamondValue synthetic_arguments[2]={registers[recv],
                    DIAMOND_OBJECT(merged_array)};DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&synthetic,vm,
                    synthetic_arguments,2,depth+1,nullptr,&call_result);
                gc_unprotect(vm,protected_count);VM_PROPAGATE(status);
                registers[dest]=call_result;break;
            }
            case DIAMOND_OP_INVOKE_SPREAD:
            case DIAMOND_OP_INVOKE_TYPED_SPREAD: {
                uint16_t dest=0,recv=0,spread_register=0,name=0;
                READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(name);
                READ_SHORT(spread_register);
                uint8_t type_argument_count=0;uint16_t type_arguments[8];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED_SPREAD) {
                    READ_BYTE(type_argument_count);
                    if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_argument_count;index++)
                        READ_SHORT(type_arguments[index]);
                }
                if((size_t)name>=chunk->string_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[spread_register].kind!=DIAMOND_VALUE_OBJECT||
                   registers[spread_register].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "spread argument (*expr) must be an Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *spread=(const DiamondArray *)
                    registers[spread_register].as.object;
                const DiamondStringConstant *method_name=&chunk->strings[name];
                const bool instance_receiver=registers[recv].kind==DIAMOND_VALUE_OBJECT&&
                    registers[recv].as.object->kind==DIAMOND_OBJECT_INSTANCE;
                bool user_public_send=false;
                if(instance_receiver) {
                    const DiamondInstance *candidate=
                        (const DiamondInstance *)registers[recv].as.object;
                    const DiamondChunk *candidate_owner=candidate->owner!=nullptr?
                        candidate->owner:vm->root_chunk;
                    user_public_send=lookup_method(candidate_owner,candidate->class,
                        "public_send",11)!=nullptr;
                }
                if(method_name->length==11&&
                   memcmp(method_name->chars,"public_send",11)==0&&
                   !user_public_send) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    DiamondValue sent=DIAMOND_NIL;
                    const DiamondVmStatus send_status=public_send_helper(vm,chunk,
                        registers[recv],spread->values,spread->count,depth,&sent);
                    VM_PROPAGATE(send_status);
                    registers[dest]=sent;break;
                }
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||
                   registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE) {
                    /* Native receivers already share one deliberately ordered
                     * INVOKE matrix below (universal methods, built-ins, then
                     * collection extensions). Re-enter that matrix through a
                     * verifier-safe synthetic frame instead of cloning its
                     * many receiver branches here. The ordinary INVOKE format
                     * has an 8-bit count and currently caps calls at 16 args. */
                    if(spread->count>16)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue *native_arguments=malloc((spread->count+1)*
                        sizeof *native_arguments);
                    if(native_arguments==nullptr)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    native_arguments[0]=registers[recv];
                    for(size_t index=0;index<spread->count;index++)
                        native_arguments[index+1]=spread->values[index];
                    uint8_t synthetic_code[32]={0};size_t code_index=0;
                    const uint16_t destination=(uint16_t)(spread->count+1);
#define SYNTHETIC_SHORT(value) do { \
    synthetic_code[code_index++]=(uint8_t)((value)>>8); \
    synthetic_code[code_index++]=(uint8_t)(value); \
} while(false)
                    synthetic_code[code_index++]=(uint8_t)(
                        type_argument_count==0?DIAMOND_OP_INVOKE:
                        DIAMOND_OP_INVOKE_TYPED);
                    SYNTHETIC_SHORT(destination);
                    SYNTHETIC_SHORT(0);
                    SYNTHETIC_SHORT(name);
                    SYNTHETIC_SHORT(1);
                    synthetic_code[code_index++]=(uint8_t)spread->count;
                    if(type_argument_count>0) {
                        synthetic_code[code_index++]=type_argument_count;
                        for(size_t index=0;index<type_argument_count;index++)
                            SYNTHETIC_SHORT(type_arguments[index]);
                    }
                    synthetic_code[code_index++]=DIAMOND_OP_RETURN;
                    SYNTHETIC_SHORT(destination);
#undef SYNTHETIC_SHORT
                    uint32_t synthetic_lines[32]={0};
                    uint32_t synthetic_columns[32]={0};
                    DiamondChunk synthetic={.name="<native spread>",
                        .code=synthetic_code,.lines=synthetic_lines,
                        .columns=synthetic_columns,.code_count=code_index,
                        .constants=chunk->constants,
                        .constant_count=chunk->constant_count,
                        .strings=chunk->strings,.string_count=chunk->string_count,
                        .type_sets=chunk->type_sets,
                        .type_set_count=chunk->type_set_count,
                        .functions=chunk->functions,
                        .function_count=chunk->function_count,
                        .classes=chunk->classes,.class_count=chunk->class_count,
                        .interfaces=chunk->interfaces,
                        .interface_count=chunk->interface_count,
                        .modules=chunk->modules,.module_count=chunk->module_count,
                        .type_variable_bindings=chunk->type_variable_bindings,
                        .register_count=(uint16_t)(spread->count+2),
                        .range_class_index=chunk->range_class_index};
                    DiamondValue native_result=DIAMOND_NIL;
                    const DiamondVmStatus native_status=run_chunk(&synthetic,vm,
                        native_arguments,spread->count+1,depth+1,nullptr,
                        &native_result);
                    free(native_arguments);VM_PROPAGATE(native_status);
                    registers[dest]=native_result;break;
                }
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const DiamondChunk *owner=
                    instance->owner!=nullptr?instance->owner:vm->root_chunk;
                const DiamondMethod *method=lookup_method(owner,instance->class,
                    method_name->chars,method_name->length);
                if(method==nullptr) {
                    DiamondValue missing_result=DIAMOND_NIL;bool missing_found=false;
                    const DiamondVmStatus missing_status=method_missing_helper(vm,owner,
                        instance,method_name->chars,method_name->length,
                        spread->values,spread->count,depth,&missing_result,&missing_found);
                    if(missing_found) {
                        VM_PROPAGATE(missing_status);
                        registers[dest]=missing_result;break;
                    }
                    snprintf(vm->error,sizeof vm->error,
                        "undefined method '%.*s' for an instance of %s",
                        (int)method_name->length,method_name->chars,
                        instance->class->name);
                    VM_RETURN(DIAMOND_VM_NO_METHOD_ERROR);
                }
                if(method->is_private&&!(chunk->parameter_offset==1&&recv==0)) {
                    snprintf(vm->error,sizeof vm->error,
                        "private method '%.*s' called with an explicit receiver",
                        (int)method_name->length,method_name->chars);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
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
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                }
                if(spread->count<method->required_arity||
                   (spread->count>method->arity&&!method->has_variadic))
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if(spread->count>(size_t)DIAMOND_REGISTER_COUNT-1-
                        method->bound_value_count)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                const size_t total_args=spread->count+1+method->bound_value_count;
                DiamondValue *args=malloc(total_args*sizeof *args);
                if(args==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                args[0]=registers[recv];
                for(size_t index=0;index<spread->count;index++)
                    args[index+1]=spread->values[index];
                for(size_t index=0;index<method->bound_value_count;index++)
                    args[spread->count+1+index]=method->bound_values[index];
                const DiamondChunk *function_chunk=
                    method->source_chunk!=nullptr?method->source_chunk:owner;
                const DiamondFunction *fn=
                    function_chunk->functions[method->function_index];
                if(type_argument_count>0&&
                   type_argument_count!=fn->type_variable_count) {
                    free(args);VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* Only the first type_argument_count entries are ever read (below,
                 * and through .type_variable_bindings); zeroing all eight cost about
                 * 3 KB of memset on every call, typed or not. */
                DiamondTypeBinding explicit_bindings[8];
                for(size_t index=0;index<type_argument_count;index++) {
                    explicit_bindings[index]=(DiamondTypeBinding){};
                    if((size_t)type_arguments[index]>=chunk->type_set_count) {
                        free(args);VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    }
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                        chunk->type_sets,type_arguments[index]);
                }
                DiamondChunk child={.name=fn->name,.code=fn->code,
                    .lines=fn->lines,.columns=fn->columns,
                    .code_count=fn->code_count,.constants=fn->constants,
                    .constant_count=fn->constant_count,.strings=fn->strings,
                    .string_count=fn->string_count,.type_sets=fn->type_sets,
                    .type_set_count=fn->type_set_count,
                    .functions=function_chunk->functions,
                    .function_count=function_chunk->function_count,
                    .classes=function_chunk->classes,
                    .class_count=function_chunk->class_count,
                    .interfaces=function_chunk->interfaces,
                    .interface_count=function_chunk->interface_count,
                    .parameter_type_sets=fn->parameter_type_sets,
                    .type_variable_count=fn->type_variable_count,
                    .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                    .type_variable_bindings=type_argument_count==0?nullptr:
                        explicit_bindings,
                    .register_count=fn->register_count,
                    .has_variadic=fn->has_variadic};
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus spread_status=run_chunk(&child,vm,args,
                    total_args,depth+1,nullptr,&call_result);
                free(args);
                VM_PROPAGATE(spread_status);
                registers[dest]=call_result;break;
            }
            case DIAMOND_OP_INVOKE:
            case DIAMOND_OP_INVOKE_MONO:
            case DIAMOND_OP_INVOKE_TYPED: {
                uint16_t dest=0,recv=0,base=0,name=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(name);READ_SHORT(base);READ_BYTE(argc);
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
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                           registers[base].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        DiamondClosure *called=(DiamondClosure *)registers[base].as.object;
                        /* See DIAMOND_OP_CALL_CLOSURE's own comment. */
                        if(called->foreign_chunk!=nullptr) {
                            snprintf(vm->error,sizeof vm->error,
                                "a compile_method callable can only be passed to define_method");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        if(called->function_index>=chunk->function_count)
                            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                        const DiamondFunction *fn=chunk->functions[called->function_index];
                        DiamondValue tap_argument[1]={registers[recv]};
                        DiamondValue tap_result=DIAMOND_NIL;
                        const DiamondVmStatus tap_status=call_closure_helper(vm,chunk,fn,
                            called,tap_argument,0,1,depth,&tap_result);
                        VM_PROPAGATE(tap_status);
                        registers[dest]=registers[recv];break;
                    }
                    /* Array#shuffle and #sample: uniform choices from
                     * OpenSSL's RAND_bytes (as SecureRandom uses), with
                     * rejection sampling so no index is favoured. */
                    if(registers[recv].kind==DIAMOND_VALUE_OBJECT&&
                       registers[recv].as.object->kind==DIAMOND_OBJECT_ARRAY&&
                       argc==0&&type_argument_count==0&&
                       ((method_name->length==7&&memcmp(method_name->chars,"shuffle",7)==0)||
                        (method_name->length==6&&memcmp(method_name->chars,"sample",6)==0))) {
                        const DiamondArray *source=(const DiamondArray *)registers[recv].as.object;
                        const bool sample=method_name->length==6;
                        if(sample&&source->count==0) {registers[dest]=DIAMOND_NIL;break;}
                        DiamondArray *shuffled=sample?nullptr:
                            allocate_array(vm,source->values,source->count);
                        if(!sample&&shuffled==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        const size_t rounds=sample?1:(shuffled->count>0?shuffled->count-1:0);
                        for(size_t round=0;round<rounds;round++) {
                            const uint64_t bound=sample?(uint64_t)source->count:
                                (uint64_t)(shuffled->count-round);
                            const uint64_t limit=UINT64_MAX-UINT64_MAX%bound;
                            uint64_t draw=0;
                            do {
                                if(RAND_bytes((unsigned char *)&draw,sizeof draw)!=1) {
                                    snprintf(vm->error,sizeof vm->error,
                                        "Array#%s: RAND_bytes failed",sample?"sample":"shuffle");
                                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                                }
                            } while(draw>=limit);
                            const size_t pick=(size_t)(draw%bound);
                            if(sample) {registers[dest]=source->values[pick];break;}
                            const size_t last=shuffled->count-1-round;
                            const DiamondValue held=shuffled->values[last];
                            shuffled->values[last]=shuffled->values[pick];
                            shuffled->values[pick]=held;
                        }
                        if(!sample)registers[dest]=DIAMOND_OBJECT(shuffled);
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
                        const DiamondClosure *callable=
                            (const DiamondClosure *)registers[recv].as.object;
                        const DiamondChunk *owner=callable->foreign_chunk!=nullptr?
                            callable->foreign_chunk:chunk;
                        if(callable->function_index>=owner->function_count)
                            VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                        const DiamondFunction *fn=owner->functions[callable->function_index];
                        const uint8_t declared=fn->has_variadic?fn->required_arity:fn->arity;
                        registers[dest]=DIAMOND_INT((int64_t)declared-
                            (int64_t)diamond_function_self_offset(fn));
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
                /* An Int past 64 bits is a bignum object; give it the same
                 * conversions as a small Int. */
                if(value_is_bignum(registers[recv])&&argc==0&&
                   ((method_name->length==4&&(memcmp(method_name->chars,"to_s",4)==0||
                     memcmp(method_name->chars,"to_i",4)==0||
                     memcmp(method_name->chars,"to_f",4)==0))||
                    (method_name->length==3&&memcmp(method_name->chars,"abs",3)==0))) {
                    DiamondIntView view;
                    diamond_int_view(registers[recv],&view);
                    if(method_name->chars[0]=='a') {
                        if(view.negative) {
                            const DiamondValue positive=diamond_bignum_negate(vm,view);
                            if(positive.kind==DIAMOND_VALUE_NIL)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=positive;
                        } else registers[dest]=registers[recv];
                    } else if(method_name->chars[3]=='i') {
                        registers[dest]=registers[recv];
                    } else if(method_name->chars[3]=='f') {
                        registers[dest]=DIAMOND_FLOAT(diamond_bignum_to_double(
                            (const DiamondBignum *)registers[recv].as.object));
                    } else {
                        StringBuilder text={};
                        if(!builder_format_value(&text,registers[recv])) {
                            free(text.chars);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        }
                        DiamondString *formatted=allocate_string(vm,text.chars,text.length);
                        free(text.chars);
                        if(formatted==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(formatted);
                    }
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
                        if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                               registers,&ip))
                            break;
                        set_uncaught_exception_error(vm);
                        VM_RETURN(dispatch_status);
                    }
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_CHANNEL) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=channel_invoke_helper(
                        vm,chunk,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_SUPERVISOR) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=supervisor_invoke_helper(
                        vm,chunk,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_FILE) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=file_invoke_helper(
                        vm,chunk,depth,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_LISTENER) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=listener_invoke_helper(
                        vm,chunk,depth,registers,recv,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_SOCKET) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=socket_invoke_helper(
                        vm,chunk,depth,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_UDP_SOCKET) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=udp_socket_invoke_helper(
                        vm,chunk,depth,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_TLS_SOCKET) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=tls_socket_invoke_helper(
                        vm,chunk,depth,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_REGEXP) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=regexp_invoke_helper(
                        vm,registers,recv,base,argc,dest,method_name);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_SQLITE3) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=sqlite3_dispatch_helper(vm,
                        (DiamondSqlite3Handle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_TENSOR) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=tensor_dispatch_helper(vm,
                        (DiamondTensor *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_SQLITE3_STATEMENT) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=sqlite3_statement_dispatch_helper(vm,
                        (DiamondSqlite3StatementHandle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_POSTGRES) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=postgres_dispatch_helper(vm,
                        (DiamondPostgresHandle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_MYSQL) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=mysql_dispatch_helper(vm,
                        (DiamondMysqlHandle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_TIME) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=time_dispatch_helper(vm,
                        (DiamondTime *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROCESS_RESULT) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=process_result_dispatch_helper(vm,
                        (DiamondProcessResult *)registers[recv].as.object,
                        method_name,registers,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROCESS_HANDLE) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=process_handle_dispatch_helper(vm,
                        (DiamondProcessHandle *)registers[recv].as.object,
                        method_name,registers,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROCESS_STREAM) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    const DiamondVmStatus dispatch_status=process_stream_dispatch_helper(vm,
                        (DiamondProcessStream *)registers[recv].as.object,
                        method_name,registers,argc,base,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
                    if(type_argument_count!=0)VM_REJECT_TYPE_ARGUMENTS(method_name);
                    DiamondValue invoke_result=DIAMOND_NIL;
                    const DiamondVmStatus invoke_status=program_builder_invoke_helper(vm,
                        (DiamondProgramBuilder *)registers[recv].as.object,method_name,
                        registers,base,argc,depth,&invoke_result);
                    VM_PROPAGATE(invoke_status);
                    registers[dest]=invoke_result;break;
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
                const uint8_t *site=chunk->code+instruction_offset;
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=diamond_jit_invoke_instance(vm,chunk,
                    site,(DiamondOpCode)instruction,registers,recv,name,base,argc,
                    type_argument_count,type_arguments,depth,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_SUPER: {
                uint16_t dest=0,base=0,name=0;uint8_t owner_index=0,argc=0;
                READ_SHORT(dest);READ_BYTE(owner_index);READ_SHORT(name);
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
                const uint8_t *site=&chunk->code[instruction_offset];
                uint16_t dest=0,recv=0,field_operand=0;
                READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(field_operand);
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
                const uint8_t *site=&chunk->code[instruction_offset];
                uint16_t recv=0,field_operand=0,source=0;
                READ_SHORT(recv);READ_SHORT(field_operand);READ_SHORT(source);
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
                const uint8_t *site=&chunk->code[instruction_offset];
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
                if(!vm->namespace_constant_initialized[index]) {
                    snprintf(vm->error,sizeof vm->error,"uninitialized constant");
                    VM_RETURN(DIAMOND_VM_CONSTANT_ERROR);
                }
                registers[destination]=vm->namespace_constants[index];break;
            }
            case DIAMOND_OP_SET_NAMESPACE_CONSTANT: {
                uint16_t index=0,source=0;READ_SHORT(index);READ_SHORT(source);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
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
                uint16_t source=0,set_index=0; READ_SHORT(source); READ_SHORT(set_index);
                if(set_index>=chunk->type_set_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const bool matches=value_matches_set(chunk,registers[source],
                    set_index,true);
                if(!matches) {
                    char expected[256]; char actual[256];
                    format_type_set_index(expected,sizeof expected,chunk,set_index);
                    format_value_type_detailed(actual,sizeof actual,registers[source]);
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
            case DIAMOND_OP_PROCESS_RUN: {
                uint16_t destination=0,argv_register=0;
                READ_SHORT(destination);READ_SHORT(argv_register);
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
                    (DiamondArray *)registers[argv_register].as.object,process_result);
                VM_PROPAGATE(run_status);
                break;
            }
            case DIAMOND_OP_PROCESS_SPAWN: {
                uint16_t destination=0,argv_register=0;
                READ_SHORT(destination);READ_SHORT(argv_register);
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
                    (DiamondArray *)registers[argv_register].as.object,process_handle);
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
                uint16_t destination=0;uint8_t local_count=0;
                READ_SHORT(destination);READ_BYTE(local_count);
                uint64_t source_line_offset=0;
                for(size_t index=0;index<8;index++) {
                    uint8_t next=0;READ_BYTE(next);
                    source_line_offset=(source_line_offset<<8)|next;
                }
                uint16_t name_indices[DIAMOND_MAX_LOCALS];
                uint16_t local_registers[DIAMOND_MAX_LOCALS];
                for(size_t index=0;index<local_count;index++) {
                    READ_SHORT(name_indices[index]);
                    READ_SHORT(local_registers[index]);
                }
                /* Non-blocking: a live `setBreakpoints` sent while this
                 * program is running (not currently paused at any
                 * breakpoint) has nobody else reading vm->debug_fd to
                 * receive it -- see DiamondVm.debug_active_lines's own
                 * comment (src/vm.h) and docs/debugging.md's "live
                 * breakpoints" section for why this has to happen right
                 * here, at every statement, rather than via a background
                 * thread. Applies every currently-buffered command
                 * (there could be more than one queued up) before
                 * deciding whether *this* statement's own line is armed. */
                if(vm->debug_fd>=0) {
                    struct pollfd poll_fd={.fd=vm->debug_fd,.events=POLLIN};
                    while(poll(&poll_fd,1,0)>0&&(poll_fd.revents&POLLIN)!=0) {
                        DiamondDebugCommandKind kind=DIAMOND_DEBUG_COMMAND_NONE;
                        size_t lines[DIAMOND_MAX_ACTIVE_BREAKPOINTS];size_t line_count=0;
                        bool are_offsets=false;
                        if(!debug_pipe_read_command(vm->debug_fd,&kind,lines,&line_count,
                                DIAMOND_MAX_ACTIVE_BREAKPOINTS,&are_offsets))
                            break;
                        if(kind==DIAMOND_DEBUG_COMMAND_SET_BREAKPOINTS) {
                            memcpy(vm->debug_active_lines,lines,line_count*sizeof lines[0]);
                            vm->debug_active_line_count=line_count;
                            vm->debug_breakpoints_are_offsets=are_offsets;
                        }
                        poll_fd.revents=0;
                    }
                }
                const bool in_bounds=instruction_offset<chunk->code_count;
                const uint32_t statement_line=in_bounds&&chunk->lines!=nullptr?
                    chunk->lines[instruction_offset]:0;
                bool armed=false;
                for(size_t index=0;index<vm->debug_active_line_count;index++)
                    if(vm->debug_active_lines[index]==(vm->debug_breakpoints_are_offsets?
                            (size_t)source_line_offset:(size_t)statement_line)) { armed=true; break; }
                /* Real stepping (docs/debugging.md's own "Stepping"
                 * section): an armed breakpoint line always wins/pauses
                 * regardless (checked above, unconditionally) -- this is
                 * only a fallback for when that check didn't already
                 * decide to pause. See DiamondVm.debug_step_mode's own
                 * comment (src/vm.h) for the full semantics; `depth` is
                 * this exact run_chunk invocation's own already-tracked
                 * recursion depth, needing no new state to read. */
                bool stepped=false;
                if(!armed&&vm->debug_step_mode!=DIAMOND_STEP_NONE) {
                    switch(vm->debug_step_mode) {
                        case DIAMOND_STEP_IN: stepped=true; break;
                        case DIAMOND_STEP_OVER:
                            stepped=depth<=vm->debug_step_target_depth; break;
                        case DIAMOND_STEP_OUT:
                            stepped=depth<vm->debug_step_target_depth; break;
                        default: break;
                    }
                }
                if(armed||stepped) {
                    vm->debug_step_mode=DIAMOND_STEP_NONE;
                    const DiamondVmStatus debugger_status=debugger_helper(vm,chunk,depth,
                        instruction_offset,(size_t)source_line_offset,registers,name_indices,local_registers,local_count,
                        armed?"breakpoint":"step");
                    VM_PROPAGATE(debugger_status);
                }
                registers[destination]=DIAMOND_NIL;
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
                READ_SHORT(destination);READ_SHORT(source);READ_SHORT(type);
                registers[destination]=DIAMOND_BOOL(type>=DIAMOND_NATIVE_TYPE_OPERAND_BASE?
                    value_is_native_kind(registers[source],
                        (uint8_t)(type-DIAMOND_NATIVE_TYPE_OPERAND_BASE)):
                    value_matches_type(chunk,registers[source],(uint8_t)type));
                break;
            }
            case DIAMOND_OP_ARRAY: {
                uint16_t destination=0,base=0,count=0;
                READ_SHORT(destination);READ_SHORT(base);READ_SHORT(count);
                if((size_t)base+count>DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondArray *array=allocate_array(vm,&registers[base],count);
                if(array==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(array);
                break;
            }
            case DIAMOND_OP_INDEX_GET: {
                uint16_t destination=0,receiver=0,index_register=0;
                READ_SHORT(destination);READ_SHORT(receiver);READ_SHORT(index_register);
                const uint8_t *site=chunk->code+instruction_offset;
                DiamondValue indexed=DIAMOND_NIL;
                const DiamondVmStatus status=diamond_jit_index_get(vm,chunk,depth,site,
                    &registers[receiver],&registers[index_register],&indexed);
                VM_PROPAGATE(status);
                registers[destination]=indexed;
                break;
            }
            case DIAMOND_OP_INDEX_SET: {
                uint16_t receiver=0,index_register=0,source=0;
                READ_SHORT(receiver);READ_SHORT(index_register);READ_SHORT(source);
                const uint8_t *site=chunk->code+instruction_offset;
                const DiamondVmStatus status=diamond_jit_index_set(vm,chunk,depth,site,
                    &registers[receiver],&registers[index_register],&registers[source]);
                VM_PROPAGATE(status);
                break;
            }
            case DIAMOND_OP_HASH: {
                uint16_t destination=0,base=0,count=0;
                READ_SHORT(destination);READ_SHORT(base);READ_SHORT(count);
                DiamondValue built=DIAMOND_NIL;
                const DiamondVmStatus hash_status=
                    diamond_jit_new_hash(vm,registers,base,count,&built);
                VM_PROPAGATE(hash_status);
                registers[destination]=built;
                break;
            }
            case DIAMOND_OP_NOT: {
                uint16_t destination=0,source=0;
                READ_SHORT(destination);READ_SHORT(source);
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
                if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                   registers,&ip))break;
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
                if(handler_count==16||type_count>8||target>chunk->code_count)
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
                if(handler_count==16||target>chunk->code_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                handlers[handler_count++]=(UnwindHandler){
                    .kind=HANDLER_ENSURE,.target=target,.enabled=true};
                break;
            }
            case DIAMOND_OP_RUN_ENSURE: {
                uint8_t high=0,low=0;READ_BYTE(high);READ_BYTE(low);
                const size_t continuation=((size_t)high<<8)|low;
                if(handler_count==0||continuation>chunk->code_count||
                   handlers[handler_count-1].kind!=HANDLER_ENSURE)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                ip=enter_ensure(&handlers[handler_count-1],&pending,
                    (PendingUnwind){.kind=PENDING_NORMAL,.continuation=continuation});
                break;
            }
            case DIAMOND_OP_BLOCK_RETURN:
            case DIAMOND_OP_BLOCK_BREAK: {
                uint16_t source=0;READ_SHORT(source);
                const bool is_break=chunk->code[instruction_offset]==DIAMOND_OP_BLOCK_BREAK;
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
                    if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                       registers,&ip))break;
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
                uint16_t dest=0,name_reg=0,callable_reg=0;uint8_t class_operand=0;
                READ_SHORT(dest);READ_BYTE(class_operand);READ_SHORT(name_reg);READ_SHORT(callable_reg);
                if(!resolve_class_operand(vm,chunk,registers,"redefine_method",&class_operand))
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"redefine_method name must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,"redefine_method callable must be a Callable value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
                DiamondClosure *replacement=(DiamondClosure *)registers[callable_reg].as.object;
                DiamondClass *class=(DiamondClass *)(void *)&chunk->classes[class_operand];
                DiamondMethod *target=nullptr;
                for(size_t index=0;index<class->method_count;index++)
                    if(strlen(class->methods[index].name)==name_string->length&&
                       memcmp(class->methods[index].name,name_string->chars,name_string->length)==0) {
                        target=&class->methods[index];break;
                    }
                if(target==nullptr) {
                    snprintf(vm->error,sizeof vm->error,"class '%s' has no method '%.*s' to redefine",
                             class->name,(int)name_string->length,name_string->chars);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(replacement->capture_count!=0) {
                    snprintf(vm->error,sizeof vm->error,
                             "redefine_method callable must not capture any variables");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* A compile_method result carries its own chunk and bound
                 * values and was compiled for one specific class; it's
                 * installed the same way define_method installs one (see
                 * that handler's comment), just into the existing slot. */
                const DiamondChunk *function_chunk=
                    replacement->foreign_chunk!=nullptr?replacement->foreign_chunk:chunk;
                if((size_t)replacement->function_index>=function_chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *new_function=
                    function_chunk->functions[replacement->function_index];
                if(replacement->foreign_chunk!=nullptr) {
                    if(replacement->intended_class!=class) {
                        snprintf(vm->error,sizeof vm->error,
                            "redefine_method callable was compiled for a different class than '%s'",
                            class->name);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                } else if(new_function->owner_class!=class_operand) {
                    snprintf(vm->error,sizeof vm->error,
                             "redefine_method callable must be a method of '%s'",class->name);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const uint8_t new_arity=
                    (uint8_t)(new_function->arity-1-replacement->bound_value_count);
                const uint8_t new_required_arity=
                    (uint8_t)(new_function->required_arity-1-replacement->bound_value_count);
                if(new_arity!=target->arity||new_required_arity!=target->required_arity||
                   new_function->has_variadic!=target->has_variadic)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                target->function_index=replacement->function_index;
                target->source_chunk=replacement->foreign_chunk;
                target->bound_values=replacement->bound_values;
                target->bound_value_count=replacement->bound_value_count;
                diamond_vm_invalidate_method_caches(vm);
                registers[dest]=DIAMOND_NIL;break;
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
                uint16_t dest=0,name_reg=0,callable_reg=0;uint8_t class_operand=0;
                READ_SHORT(dest);READ_BYTE(class_operand);READ_SHORT(name_reg);READ_SHORT(callable_reg);
                if(!resolve_class_operand(vm,chunk,registers,"define_method",&class_operand))
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"define_method name must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,"define_method callable must be a Callable value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
                if(name_string->length==0||name_string->length>=DIAMOND_MAX_FUNCTION_NAME) {
                    snprintf(vm->error,sizeof vm->error,
                        "define_method name must be 1 to %d characters",
                        DIAMOND_MAX_FUNCTION_NAME-1);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondClosure *replacement=(DiamondClosure *)registers[callable_reg].as.object;
                DiamondClass *class=(DiamondClass *)(void *)&chunk->classes[class_operand];
                for(size_t index=0;index<class->method_count;index++)
                    if(strlen(class->methods[index].name)==name_string->length&&
                       memcmp(class->methods[index].name,name_string->chars,
                              name_string->length)==0) {
                        snprintf(vm->error,sizeof vm->error,
                            "class '%s' already has a method '%.*s' -- use redefine_method instead",
                            class->name,(int)name_string->length,name_string->chars);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                if(class->method_count>=DIAMOND_MAX_METHODS) {
                    snprintf(vm->error,sizeof vm->error,
                        "class '%s' already has the maximum number of methods (%d)",
                        class->name,DIAMOND_MAX_METHODS);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(replacement->capture_count!=0) {
                    snprintf(vm->error,sizeof vm->error,
                             "define_method callable must not capture any variables");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* A ClassName.compile_method result carries its own chunk
                 * (function_index is meaningless against the ambient
                 * `chunk` for one of those) and was already validated
                 * against a specific class's field layout at compile
                 * time -- intended_class, compared by pointer identity,
                 * catches installing it onto a *different* class than
                 * the one it was actually compiled for. An ordinary
                 * closure (foreign_chunk==nullptr) keeps the original
                 * same-chunk, same-owner_class checks unchanged. */
                const DiamondChunk *function_chunk=
                    replacement->foreign_chunk!=nullptr?replacement->foreign_chunk:chunk;
                if((size_t)replacement->function_index>=function_chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *new_function=
                    function_chunk->functions[replacement->function_index];
                if(replacement->foreign_chunk!=nullptr) {
                    if(replacement->intended_class!=class) {
                        snprintf(vm->error,sizeof vm->error,
                            "define_method callable was compiled for a different class than '%s'",
                            class->name);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                } else if(new_function->owner_class!=class_operand) {
                    snprintf(vm->error,sizeof vm->error,
                             "define_method callable must be a method of '%s'",class->name);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondMethod *added=&class->methods[class->method_count];
                *added=(DiamondMethod){0};
                memcpy(added->name,name_string->chars,name_string->length);
                added->name[name_string->length]='\0';
                added->function_index=replacement->function_index;
                added->source_chunk=replacement->foreign_chunk;
                /* bound_values are trailing parameters compile_method's own
                 * synthesized source already appended (see
                 * build_compiled_method_source) -- a caller of the
                 * installed method never supplies them, so they're
                 * excluded from the arity a caller is checked against;
                 * dispatch appends them itself, see the
                 * DIAMOND_OP_INVOKE_TYPED-family sites that read
                 * method->bound_values. */
                added->bound_values=replacement->bound_values;
                added->bound_value_count=replacement->bound_value_count;
                added->arity=(uint8_t)(new_function->arity-1-replacement->bound_value_count);
                added->required_arity=
                    (uint8_t)(new_function->required_arity-1-replacement->bound_value_count);
                added->has_variadic=new_function->has_variadic;
                class->method_count++;
                diamond_vm_invalidate_method_caches(vm);
                registers[dest]=DIAMOND_NIL;break;
            }
            case DIAMOND_OP_COMPILE_METHOD: {
                uint16_t dest=0,name_reg=0,params_reg=0,body_reg=0,bound_values_reg=0;
                uint8_t class_operand=0;
                READ_SHORT(dest);READ_BYTE(class_operand);READ_SHORT(name_reg);
                READ_SHORT(params_reg);READ_SHORT(body_reg);READ_SHORT(bound_values_reg);
                if(!resolve_class_operand(vm,chunk,registers,"compile_method",&class_operand))
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if((size_t)class_operand>=chunk->class_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                if(registers[name_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[name_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"compile_method name must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[params_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[params_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,
                        "compile_method params must be an Array of Strings");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[body_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[body_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"compile_method body_source must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[bound_values_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[bound_values_reg].as.object->kind!=DIAMOND_OBJECT_HASH) {
                    snprintf(vm->error,sizeof vm->error,"compile_method bound_values must be a Hash");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *name_string=(const DiamondString *)registers[name_reg].as.object;
                const DiamondArray *params_array=(const DiamondArray *)registers[params_reg].as.object;
                const DiamondString *body_string=(const DiamondString *)registers[body_reg].as.object;
                const DiamondHash *bound_values_hash=
                    (const DiamondHash *)registers[bound_values_reg].as.object;
                const DiamondClass *target_class=&chunk->classes[class_operand];
                const DiamondVmStatus compile_status=compile_method_helper(vm,target_class,
                    name_string,params_array,body_string,bound_values_hash,&registers[dest]);
                VM_PROPAGATE(compile_status);
                break;
            }
            case DIAMOND_OP_LOAD_CLASS: {
                uint16_t dest=0;uint8_t class_operand=0;
                READ_SHORT(dest);READ_BYTE(class_operand);
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
                const uint8_t class_operand=registers[0].as.class_index;
                if((size_t)class_operand>=chunk->class_count||
                   (size_t)name_index>=chunk->string_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondClass *class=&chunk->classes[class_operand];
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
                uint16_t dest=0,path_reg=0;
                READ_SHORT(dest);READ_SHORT(path_reg);
                VM_SANDBOX_GUARD("Dir.entries", "filesystem");
                if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"Dir.entries argument must be a String value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
                errno=0;
                DIR *directory=opendir(path->chars);
                if(directory==nullptr) {
                    snprintf(vm->error,sizeof vm->error,"cannot open directory '%.*s': %s",
                             (int)path->length,path->chars,strerror(errno));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                /* Rooted immediately -- entries is pushed onto as it
                 * grows, same "root the outer Array first, every element
                 * is reachable through it before the next one's own
                 * allocation" discipline tensor_to_a_helper's own comment
                 * (earlier in this file) explains in full. */
                DiamondArray *entries=allocate_array(vm,nullptr,0);
                if(entries==nullptr) {
                    closedir(directory);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=DIAMOND_OBJECT(entries);
                /* readdir's own error contract: a nullptr return means
                 * either "no more entries" (errno left unchanged) or a
                 * genuine read error (errno set) -- indistinguishable
                 * without resetting errno immediately before every call
                 * and checking it only once the loop actually stops. */
                errno=0;
                struct dirent *entry=readdir(directory);
                while(entry!=nullptr) {
                    if(strcmp(entry->d_name,".")!=0&&strcmp(entry->d_name,"..")!=0) {
                        DiamondString *name=
                            allocate_string(vm,entry->d_name,strlen(entry->d_name));
                        if(name==nullptr) {
                            closedir(directory);
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        }
                        if(!array_push(vm,entries,DIAMOND_OBJECT(name))) {
                            closedir(directory);
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        }
                    }
                    errno=0;
                    entry=readdir(directory);
                }
                if(errno!=0) {
                    snprintf(vm->error,sizeof vm->error,"error reading directory '%.*s': %s",
                             (int)path->length,path->chars,strerror(errno));
                    closedir(directory);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                closedir(directory);
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
                uint16_t dest=0,arg1=0,arg2=0;uint8_t selector=0;
                READ_SHORT(dest);READ_SHORT(arg1);READ_SHORT(arg2);READ_BYTE(selector);
                if(registers[arg1].kind!=DIAMOND_VALUE_OBJECT||
                   registers[arg1].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"File path argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[arg1].as.object;
                const bool has_second=registers[arg2].kind!=DIAMOND_VALUE_NIL;
                if(has_second&&(registers[arg2].kind!=DIAMOND_VALUE_OBJECT||
                                registers[arg2].as.object->kind!=DIAMOND_OBJECT_STRING)) {
                    snprintf(vm->error,sizeof vm->error,"File path argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *second=has_second?
                    (const DiamondString *)registers[arg2].as.object:nullptr;
                DiamondValue path_result=DIAMOND_NIL;
                DiamondVmStatus path_status=DIAMOND_VM_OK;
                switch((DiamondFilePathFunction)selector) {
                    case DIAMOND_FILE_PATH_SYNC:
                        VM_SANDBOX_GUARD("File.sync", "filesystem");
                        path_status=file_sync_helper(vm,path);
                        path_result=registers[arg1];
                        break;
                    case DIAMOND_FILE_PATH_PUBLISH:
                        VM_SANDBOX_GUARD("File.publish", "filesystem");
                        path_status=file_publish_helper(vm,path,second);
                        path_result=registers[arg1];
                        break;
                    case DIAMOND_FILE_PATH_RENAME:
                        VM_SANDBOX_GUARD("File.rename", "filesystem");
                        path_status=file_rename_helper(vm,path,second);
                        path_result=registers[arg2];
                        break;
                    case DIAMOND_FILE_PATH_DIRNAME:
                        path_status=file_path_dirname_helper(vm,path,&path_result);break;
                    case DIAMOND_FILE_PATH_BASENAME:
                        path_status=file_path_basename_helper(vm,path,second,&path_result);break;
                    case DIAMOND_FILE_PATH_EXTNAME:
                        path_status=file_path_extname_helper(vm,path,&path_result);break;
                    case DIAMOND_FILE_PATH_ABSOLUTE:
                        path_result=DIAMOND_BOOL(path->length>0&&path->chars[0]=='/');break;
                    case DIAMOND_FILE_PATH_EXPAND:
                        VM_SANDBOX_GUARD("File.expand_path", "filesystem");
                        path_status=file_path_expand_helper(vm,path,second,&path_result);break;
                    case DIAMOND_FILE_PATH_EXIST: {
                        VM_SANDBOX_GUARD("File.exist?", "filesystem");
                        struct stat path_stat;
                        path_result=DIAMOND_BOOL(stat(path->chars,&path_stat)==0);
                        break;
                    }
                    case DIAMOND_FILE_PATH_READ:
                        VM_SANDBOX_GUARD("File.read", "filesystem");
                        path_status=file_read_helper(vm,path,&path_result);break;
                    case DIAMOND_FILE_PATH_WRITE:
                        VM_SANDBOX_GUARD("File.write", "filesystem");
                        path_status=file_write_helper(vm,path,second,&path_result);break;
                    case DIAMOND_FILE_PATH_DIRECTORY: {
                        VM_SANDBOX_GUARD("File.directory?", "filesystem");
                        struct stat path_stat;
                        const bool is_directory=
                            stat(path->chars,&path_stat)==0&&S_ISDIR(path_stat.st_mode);
                        path_result=DIAMOND_BOOL(is_directory);
                        break;
                    }
                    default: VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                }
                VM_PROPAGATE(path_status);
                registers[dest]=path_result;
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
                break;
            }
            case DIAMOND_OP_POSTGRES_OPEN: {
                uint16_t dest=0,conninfo_reg=0;
                READ_SHORT(dest);READ_SHORT(conninfo_reg);
                VM_SANDBOX_GUARD("PostgreSQL.open", "database");
                if(registers[conninfo_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[conninfo_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "PostgreSQL.open argument must be a String conninfo");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *conninfo=
                    (const DiamondString *)registers[conninfo_reg].as.object;
                PGconn *conn=PQconnectdb(conninfo->chars);
                /* Unlike sqlite3_open, a failed PQconnectdb still returns a
                 * non-null conn whose PQerrorMessage must be read before
                 * PQfinish-ing it -- conn is only ever null on the client's
                 * own OOM, a separate case PQerrorMessage(nullptr) can't
                 * describe. */
                if(conn==nullptr) {
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                if(PQstatus(conn)!=CONNECTION_OK) {
                    snprintf(vm->error,sizeof vm->error,"%s",PQerrorMessage(conn));
                    PQfinish(conn);
                    VM_RETURN(DIAMOND_VM_POSTGRES_ERROR);
                }
                DiamondPostgresHandle *handle=allocate_postgres_handle(vm,conn);
                if(handle==nullptr) {
                    PQfinish(conn);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_MYSQL_OPEN: {
                uint16_t dest=0,host_reg=0,user_reg=0,password_reg=0,database_reg=0,
                    port_reg=0;
                READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(user_reg);
                READ_SHORT(password_reg);READ_SHORT(database_reg);READ_SHORT(port_reg);
                VM_SANDBOX_GUARD("MySQL.open", "database");
                if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "MySQL.open's host argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[user_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[user_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "MySQL.open's user argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[password_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[password_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "MySQL.open's password argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[database_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[database_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "MySQL.open's database argument must be a String");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                        "MySQL.open's port argument must be an Int");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
                const DiamondString *user=(const DiamondString *)registers[user_reg].as.object;
                const DiamondString *password=
                    (const DiamondString *)registers[password_reg].as.object;
                const DiamondString *database=
                    (const DiamondString *)registers[database_reg].as.object;
                const int64_t port=registers[port_reg].as.integer;
                if(port<0||port>UINT16_MAX) {
                    snprintf(vm->error,sizeof vm->error,
                        "MySQL.open's port argument out of range");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* mysql_real_connect's const char* arguments assume C
                 * strings, unlike MYSQL_BIND's explicit buffer_length --
                 * DiamondString->chars isn't guaranteed NUL-terminated (a
                 * flexible array member sized by ->length only), so each
                 * needs its own NUL-terminated copy here. */
                char *host_copy=malloc(host->length+1);
                char *user_copy=malloc(user->length+1);
                char *password_copy=malloc(password->length+1);
                char *database_copy=malloc(database->length+1);
                if(host_copy==nullptr||user_copy==nullptr||password_copy==nullptr||
                   database_copy==nullptr) {
                    free(host_copy);free(user_copy);free(password_copy);free(database_copy);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                memcpy(host_copy,host->chars,host->length);host_copy[host->length]='\0';
                memcpy(user_copy,user->chars,user->length);user_copy[user->length]='\0';
                memcpy(password_copy,password->chars,password->length);
                password_copy[password->length]='\0';
                memcpy(database_copy,database->chars,database->length);
                database_copy[database->length]='\0';
                MYSQL *conn=mysql_init(nullptr);
                if(conn==nullptr) {
                    free(host_copy);free(user_copy);free(password_copy);free(database_copy);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                /* Same requirement as libpq's PQconnectdb: a failed
                 * mysql_real_connect returns nullptr but leaves `conn`
                 * itself (allocated by mysql_init above) still owned by
                 * the caller -- mysql_close(conn), not mysql_close(connected)
                 * (nullptr), is what actually frees it and must run on
                 * this path too. */
                MYSQL *connected=mysql_real_connect(conn,host_copy,user_copy,password_copy,
                    database_copy,(unsigned int)port,nullptr,0);
                free(host_copy);free(user_copy);free(password_copy);free(database_copy);
                if(connected==nullptr) {
                    snprintf(vm->error,sizeof vm->error,"%s",mysql_error(conn));
                    mysql_close(conn);
                    VM_RETURN(DIAMOND_VM_MYSQL_ERROR);
                }
                DiamondMysqlHandle *handle=allocate_mysql_handle(vm,connected);
                if(handle==nullptr) {
                    mysql_close(connected);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
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
                uint16_t dest=0,callable_reg=0,base=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(callable_reg);READ_SHORT(base);READ_BYTE(argc);
                if(registers[callable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable_reg].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
                    snprintf(vm->error,sizeof vm->error,
                        "Thread.new's first argument must be a Callable value");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondClosure *callable=
                    (const DiamondClosure *)registers[callable_reg].as.object;
                if(callable->capture_count!=0) {
                    snprintf(vm->error,sizeof vm->error,
                        "Thread.new's callable must not capture any local state");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* See DIAMOND_OP_CALL_CLOSURE's own comment -- doubly true
                 * here, since clone_program_from_chunk below clones only
                 * the ambient chunk, never a compile_method result's own
                 * foreign one. */
                if(callable->foreign_chunk!=nullptr) {
                    snprintf(vm->error,sizeof vm->error,
                        "a compile_method callable can only be passed to define_method");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if((size_t)callable->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *target_fn=chunk->functions[callable->function_index];
                if(argc<target_fn->required_arity||
                   (argc>target_fn->arity && !target_fn->has_variadic))
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if(atomic_load(&diamond_active_thread_count)>=DIAMOND_MAX_THREADS) {
                    snprintf(vm->error,sizeof vm->error,
                        "too many concurrently active threads");
                    VM_RETURN(DIAMOND_VM_THREAD_ERROR);
                }
                DiamondProgram *child_program=clone_program_from_chunk(chunk);
                if(child_program==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                DiamondVm *child_vm=malloc(sizeof *child_vm);
                if(child_vm==nullptr) {
                    diamond_program_free(child_program);free(child_program);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                diamond_vm_init(child_vm);
                DiamondThread *new_thread=malloc(sizeof *new_thread);
                if(new_thread==nullptr) {
                    diamond_vm_free(child_vm);free(child_vm);
                    diamond_program_free(child_program);free(child_program);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                *new_thread=(DiamondThread){.child_vm=child_vm,
                    .child_program=child_program,
                    .function_index=callable->function_index,.arg_count=argc};
                pthread_mutex_init(&new_thread->join_lock,nullptr);
                /* From here on, `new_thread` is a fully valid DiamondThread
                 * (free_thread works correctly on it regardless of whether
                 * the arg copy below actually finishes), so every
                 * remaining failure path in this case reuses free_thread
                 * as its single cleanup rather than hand-rolling another
                 * teardown sequence -- also where diamond_active_thread_
                 * count's matching increment belongs: exactly the window
                 * where a future free_thread call is guaranteed to
                 * decrement it back out again. */
                atomic_fetch_add(&diamond_active_thread_count,1);
                bool copy_failed=false;
                /* new_thread->args[] is a plain struct field, not scanned
                 * by child_vm's GC until thread_entry_trampoline's own
                 * run_chunk call copies it into a real frame -- so an
                 * already-copied earlier argument is just as unrooted here
                 * as the Array/Hash/Instance cases inside
                 * copy_value_into_vm itself were before this fix. Same
                 * remedy: protect each arg on child_vm's own gc_protected
                 * stack as it's produced, and only unwind once every
                 * argument is safely copied -- nothing else allocates on
                 * child_vm between this loop finishing and the child
                 * thread's first run_chunk frame taking over as the real
                 * root. */
                const size_t args_mark=child_vm->gc_protected_count;
                for(size_t index=0;index<argc;index++) {
                    if(!copy_value_into_vm(child_vm,registers[(size_t)base+index],
                            nullptr,chunk->classes,child_program->classes,
                            nullptr,&new_thread->args[index])||
                       !gc_protect(child_vm,new_thread->args[index])) {
                        copy_failed=true;break;
                    }
                }
                gc_unprotect(child_vm,args_mark);
                if(copy_failed) {
                    free_thread(new_thread);
                    snprintf(vm->error,sizeof vm->error,
                        "%s %s","Thread.new argument",copy_failure_reason());
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                new_thread->spawned=create_vm_thread(&new_thread->handle,
                    thread_entry_trampoline,new_thread)==0;
                if(!new_thread->spawned) {
                    free_thread(new_thread);
                    snprintf(vm->error,sizeof vm->error,"failed to create thread");
                    VM_RETURN(DIAMOND_VM_THREAD_ERROR);
                }
                DiamondThreadHandle *thread_handle=
                    allocate_thread_handle(vm,new_thread);
                if(thread_handle==nullptr) {
                    free_thread(new_thread);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)thread_handle};
                break;
            }
            case DIAMOND_OP_CHANNEL_NEW: {
                uint16_t dest=0,capacity_reg=0;
                READ_SHORT(dest);READ_SHORT(capacity_reg);
                if(registers[capacity_reg].kind!=DIAMOND_VALUE_INT||
                   registers[capacity_reg].as.integer<1||
                   registers[capacity_reg].as.integer>1000000) {
                    snprintf(vm->error,sizeof vm->error,
                        "Channel.new's argument must be an Int capacity between "
                        "1 and 1,000,000");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const size_t capacity=(size_t)registers[capacity_reg].as.integer;
                /* Same clone_program_from_chunk call DIAMOND_OP_THREAD_NEW
                 * above already uses -- see DiamondChannel's own comment for
                 * why the channel needs its own permanent copy of these
                 * tables rather than borrowing the ambient chunk's. */
                DiamondProgram *private_program=clone_program_from_chunk(chunk);
                if(private_program==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                DiamondVm *private_vm=malloc(sizeof *private_vm);
                if(private_vm==nullptr) {
                    diamond_program_free(private_program);free(private_program);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                diamond_vm_init(private_vm);
                DiamondValue *queue=calloc(capacity,sizeof *queue);
                if(queue==nullptr) {
                    diamond_vm_free(private_vm);free(private_vm);
                    diamond_program_free(private_program);free(private_program);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                DiamondChannel *new_channel=malloc(sizeof *new_channel);
                if(new_channel==nullptr) {
                    free(queue);
                    diamond_vm_free(private_vm);free(private_vm);
                    diamond_program_free(private_program);free(private_program);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                *new_channel=(DiamondChannel){.private_vm=private_vm,
                    .private_program=private_program,.queue=queue,
                    .capacity=capacity};
                atomic_init(&new_channel->refcount,1);
                pthread_mutex_init(&new_channel->lock,nullptr);
                pthread_cond_init(&new_channel->not_empty,nullptr);
                pthread_cond_init(&new_channel->not_full,nullptr);
                /* See DiamondVm.extra_roots' own comment: private_vm never
                 * runs bytecode of its own, so its only root set is
                 * whatever's actually queued -- extra_root_count starts at
                 * 0 (nothing queued yet) and is kept current by send/
                 * receive, both of which already hold `lock` before
                 * touching private_vm at all. */
                private_vm->extra_roots=queue;
                DiamondChannelHandle *new_handle=
                    allocate_channel_handle(vm,new_channel);
                if(new_handle==nullptr) {
                    free_channel_reference(new_channel);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)new_handle};
                break;
            }
            case DIAMOND_OP_CHANNEL_SELECT: {
                /* Channel.select(channels, deadline) -- see docs/threads.md.
                 * Receives from the first channel (in array order) that holds
                 * a value, returning [index, value]; blocks until one does.
                 * nil means nothing will arrive: the deadline passed, or
                 * every channel is closed and drained. Built from the same
                 * take-one-value step as Channel#receive plus the readiness
                 * wait wait_readable already uses, so a value is only ever
                 * consumed under its own channel's lock. */
                uint16_t dest=0,channels_reg=0,deadline_reg=0;
                READ_SHORT(dest);READ_SHORT(channels_reg);READ_SHORT(deadline_reg);
                const DiamondValue channels_value=registers[channels_reg];
                const DiamondValue deadline_value=registers[deadline_reg];
                bool valid_channels=channels_value.kind==DIAMOND_VALUE_OBJECT&&
                    channels_value.as.object->kind==DIAMOND_OBJECT_ARRAY;
                const DiamondArray *candidates=valid_channels?
                    (const DiamondArray *)channels_value.as.object:nullptr;
                for(size_t i=0;valid_channels&&i<candidates->count;i++)
                    valid_channels=candidates->values[i].kind==DIAMOND_VALUE_OBJECT&&
                        candidates->values[i].as.object->kind==DIAMOND_OBJECT_CHANNEL;
                if(!valid_channels||candidates->count==0) {
                    snprintf(vm->error,sizeof vm->error,
                        "Channel.select's first argument must be a non-empty Array of Channels");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const bool select_timed=deadline_value.kind!=DIAMOND_VALUE_NIL;
                if(select_timed&&(deadline_value.kind!=DIAMOND_VALUE_FLOAT||
                                  !isfinite(deadline_value.as.real))) {
                    snprintf(vm->error,sizeof vm->error,
                        "Channel.select's deadline must be a finite monotonic Float or nil");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                bool *drained=calloc(candidates->count,sizeof *drained);
                if(drained==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                DiamondValue selected=DIAMOND_NIL;
                bool found=false;
                while(!found) {
                    size_t open_count=0;
                    for(size_t i=0;i<candidates->count&&!found;i++) {
                        DiamondChannel *candidate=
                            ((DiamondChannelHandle *)candidates->values[i].as.object)->channel;
                        pthread_mutex_lock(&candidate->lock);
                        if(candidate->count==0) {
                            if(candidate->closed)drained[i]=true;
                            else open_count++;
                            pthread_mutex_unlock(&candidate->lock);
                            continue;
                        }
                        DiamondValue copied=DIAMOND_NIL;
                        if(!copy_value_into_vm(vm,candidate->queue[0],nullptr,
                               candidate->private_program->classes,chunk->classes,
                               nullptr,&copied)) {
                            pthread_mutex_unlock(&candidate->lock);
                            free(drained);
                            snprintf(vm->error,sizeof vm->error,
                                "%s %s","Channel.select result",copy_failure_reason());
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        memmove(candidate->queue,candidate->queue+1,
                            (candidate->count-1)*sizeof *candidate->queue);
                        candidate->count--;
                        candidate->private_vm->extra_root_count=candidate->count;
                        notify_channel_waiters(candidate);
                        pthread_cond_signal(&candidate->not_full);
                        pthread_mutex_unlock(&candidate->lock);
                        /* Root the value in dest before allocating the result
                         * pair, which may trigger a collection. */
                        registers[dest]=copied;
                        const DiamondValue pair_values[2]={DIAMOND_INT((int64_t)i),copied};
                        DiamondArray *pair=allocate_array(vm,pair_values,2);
                        if(pair==nullptr) {
                            free(drained);
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        }
                        selected=DIAMOND_OBJECT(pair);
                        found=true;
                    }
                    if(found)break;
                    /* Closed and drained everywhere: nothing can ever arrive. */
                    if(open_count==0)break;
                    if(select_timed) {
                        struct timespec now;
                        if(clock_gettime(CLOCK_MONOTONIC,&now)!=0 ||
                           deadline_value.as.real-(double)now.tv_sec-(double)now.tv_nsec/1e9<=0)
                            break;
                    }
                    struct pollfd wait_fd[1];
                    const DiamondVmStatus select_status=cancellable_wait_helper(vm,nullptr,
                        false,channels_value,deadline_value,wait_fd,0,drained);
                    if(select_status!=DIAMOND_VM_OK) {
                        free(drained);
                        VM_RETURN(select_status);
                    }
                }
                free(drained);
                registers[dest]=selected;
                break;
            }
            case DIAMOND_OP_SUPERVISOR_NEW: {
                /* One optional operand: the restart strategy symbol (nil
                 * register when Supervisor.new() is called with none).
                 * Restart delay/child cap remain fixed constants, see
                 * DIAMOND_MAX_SUPERVISOR_CHILDREN's own comment. */
                uint16_t dest=0,strategy_reg=0;
                READ_SHORT(dest);READ_SHORT(strategy_reg);
                /* calloc, never a `(DiamondSupervisor){}` compound literal --
                 * see DiamondSupervisor's own comment: with children[]'s
                 * DIAMOND_MAX_SUPERVISOR_CHILDREN*DIAMOND_MAX_ARGUMENTS-sized
                 * footprint, a compound literal here would be a large
                 * automatic-storage temporary that an unoptimized build
                 * allocates unconditionally on run_chunk's OWN stack frame
                 * (regardless of which opcode case actually runs), inflating
                 * every recursive run_chunk call enough to blow the C stack
                 * long before DIAMOND_MAX_CALL_DEPTH's own counter check
                 * could catch it -- exactly the DiamondProgram calloc
                 * convention this codebase already uses for its own large
                 * fixed structs, for the identical reason. */
                DiamondSupervisorStrategy strategy=DIAMOND_SUPERVISOR_ONE_FOR_ONE;
                if(registers[strategy_reg].kind!=DIAMOND_VALUE_NIL) {
                    const DiamondValue given=registers[strategy_reg];
                    const DiamondSymbol *name=given.kind==DIAMOND_VALUE_OBJECT&&
                        given.as.object->kind==DIAMOND_OBJECT_SYMBOL?
                        (const DiamondSymbol *)given.as.object:nullptr;
                    if(name!=nullptr&&name->length==11&&
                       memcmp(name->chars,"one_for_one",11)==0)
                        strategy=DIAMOND_SUPERVISOR_ONE_FOR_ONE;
                    else if(name!=nullptr&&name->length==11&&
                       memcmp(name->chars,"one_for_all",11)==0)
                        strategy=DIAMOND_SUPERVISOR_ONE_FOR_ALL;
                    else if(name!=nullptr&&name->length==12&&
                       memcmp(name->chars,"rest_for_one",12)==0)
                        strategy=DIAMOND_SUPERVISOR_REST_FOR_ONE;
                    else {
                        snprintf(vm->error,sizeof vm->error,
                            "Supervisor.new's strategy must be :one_for_one, "
                            ":one_for_all, or :rest_for_one");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                }
                DiamondSupervisor *new_supervisor=calloc(1,sizeof *new_supervisor);
                if(new_supervisor==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                new_supervisor->strategy=strategy;
                atomic_init(&new_supervisor->refcount,1);
                atomic_init(&new_supervisor->stop_requested,false);
                pthread_mutex_init(&new_supervisor->lock,nullptr);
                DiamondSupervisorHandle *new_handle=
                    allocate_supervisor_handle(vm,new_supervisor);
                if(new_handle==nullptr) {
                    free_supervisor_reference(new_supervisor);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)new_handle};
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
                    registers[port_reg].as.integer,options.connect_timeout_ms,&connected_fd);
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
                uint16_t dest=0,port_reg=0,reuse_port_reg=0;
                READ_SHORT(dest);READ_SHORT(port_reg);READ_SHORT(reuse_port_reg);
                VM_SANDBOX_GUARD("TCPServer.listen", "network");
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "TCPServer.listen argument must be an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[reuse_port_reg].kind!=DIAMOND_VALUE_BOOL) {
                    snprintf(vm->error,sizeof vm->error,
                             "TCPServer.listen's reuse_port option must be a Bool");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondListenerHandle *listener_handle=nullptr;
                const DiamondVmStatus listen_status=tcp_listen_helper(vm,
                    registers[port_reg].as.integer,
                    instruction==DIAMOND_OP_TCP_LISTEN_NONBLOCK,
                    registers[reuse_port_reg].as.boolean,&listener_handle);
                VM_PROPAGATE(listen_status);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)listener_handle};
                break;
            }
            case DIAMOND_OP_UDP_BIND: {
                uint16_t dest=0,port_reg=0;
                READ_SHORT(dest);READ_SHORT(port_reg);
                VM_SANDBOX_GUARD("UDPSocket.bind", "network");
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "UDPSocket.bind argument must be an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondUdpSocketHandle *udp_handle=nullptr;
                const DiamondVmStatus udp_status=udp_socket_helper(vm,true,
                    registers[port_reg].as.integer,&udp_handle);
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
                const DiamondVmStatus udp_status=udp_socket_helper(vm,false,0,&udp_handle);
                VM_PROPAGATE(udp_status);
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)udp_handle};
                break;
            }
            case DIAMOND_OP_SIGNAL_TRAP: {
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
                const bool start_handshake=instruction==DIAMOND_OP_TLS_START_HANDSHAKE;
                uint16_t dest=0,host_reg=0,port_reg=0,options_reg=0;
                READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(port_reg);
                READ_SHORT(options_reg);
                VM_SANDBOX_GUARD("TLSSocket.connect", "network");
                DiamondSocketHandle *tcp=nullptr;
                if(start_handshake) {
                    if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                       registers[host_reg].as.object->kind!=DIAMOND_OBJECT_SOCKET) {
                        snprintf(vm->error,sizeof vm->error,"start_handshake expects a connected Socket");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    tcp=(DiamondSocketHandle *)registers[host_reg].as.object;
                    if(tcp->fd<0||tcp->connecting) {
                        snprintf(vm->error,sizeof vm->error,"start_handshake requires a completed TCP connection");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    host_reg=port_reg;
                }
                if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   (!start_handshake&&registers[port_reg].kind!=DIAMOND_VALUE_INT)) {
                    snprintf(vm->error,sizeof vm->error,
                             "TLSSocket.connect arguments must be a String host and an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                DiamondSocketConnectOptions options;
                const DiamondVmStatus options_status=parse_socket_connect_options_helper(
                    vm,registers[options_reg],true,&options);
                VM_PROPAGATE(options_status);
                if(start_handshake&&options.connect_timeout_ms>=0) {
                    snprintf(vm->error,sizeof vm->error,"start_handshake uses caller readiness/deadlines, not connect_timeout_ms");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* Checked before ever touching the network: an incomplete
                 * cert/key pair is a caller mistake regardless of whether
                 * the connection attempt would otherwise succeed. */
                if((options.cert!=nullptr)!=(options.key!=nullptr)) {
                    snprintf(vm->error,sizeof vm->error,
                        "TLSSocket.connect: 'cert' and 'key' must be given together");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* Same "cheap input, check before the network" reasoning
                 * as the cert/key pairing check just above -- the actual
                 * wire-format encoding still happens later, once a TLS
                 * context exists (see tls_encode_alpn_protocols below). */
                if(options.alpn!=nullptr) {
                    const DiamondVmStatus alpn_validate_status=
                        tls_validate_alpn_protocols(vm,"TLSSocket.connect",options.alpn);
                    VM_PROPAGATE(alpn_validate_status);
                }
                const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
                int connected_fd=-1;
                if(host->length==0||memchr(host->chars,'\0',host->length)!=nullptr) {
                    snprintf(vm->error,sizeof vm->error,"TLS hostname must be nonempty and contain no NUL");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(start_handshake) {
                    connected_fd=tcp->fd;tcp->fd=-1; /* ownership moves exactly once */
                } else {
                    const DiamondVmStatus connect_status=tcp_connect_helper(vm,host,
                        registers[port_reg].as.integer,options.connect_timeout_ms,&connected_fd);
                    VM_PROPAGATE(connect_status);
                }
                const DiamondVmStatus timeout_status=apply_socket_timeouts_helper(vm,
                    connected_fd,options.read_timeout_ms,options.write_timeout_ms);
                if(timeout_status!=DIAMOND_VM_OK) {close(connected_fd);VM_RETURN(timeout_status);}
                SSL_CTX *context=SSL_CTX_new(TLS_client_method());
                if(context==nullptr) {
                    close(connected_fd);
                    char detail[256];tls_format_error(detail,sizeof detail);
                    snprintf(vm->error,sizeof vm->error,"cannot create TLS context: %s",detail);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                /* Secure by default, no opt-out exposed: every
                 * TLSSocket.connect verifies the peer's certificate
                 * (SSL_VERIFY_PEER) and that the certificate is actually
                 * for `host` (SSL_set1_host below) -- there is still no
                 * `verify: false`-style escape hatch. The trust anchor
                 * itself, though, is now a real option: `ca_file`/
                 * `ca_path` load a caller-supplied CA bundle/directory
                 * (SSL_CTX_load_verify_locations) instead of the system
                 * trust store, for talking to an internal CA or a
                 * hermetic test server. Neither given still means the
                 * original, unchanged default (SSL_CTX_set_default_
                 * verify_paths, honoring $SSL_CERT_FILE/$SSL_CERT_DIR). */
                SSL_CTX_set_verify(context,SSL_VERIFY_PEER,nullptr);
                if(options.ca_file!=nullptr||options.ca_path!=nullptr) {
                    if(SSL_CTX_load_verify_locations(context,
                            options.ca_file!=nullptr?options.ca_file->chars:nullptr,
                            options.ca_path!=nullptr?options.ca_path->chars:nullptr)!=1) {
                        char detail[256];tls_format_error(detail,sizeof detail);
                        snprintf(vm->error,sizeof vm->error,
                            "cannot load custom TLS trust store: %s",detail);
                        SSL_CTX_free(context);close(connected_fd);
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                } else if(SSL_CTX_set_default_verify_paths(context)!=1) {
                    char detail[256];tls_format_error(detail,sizeof detail);
                    snprintf(vm->error,sizeof vm->error,
                        "cannot load system TLS trust store: %s",detail);
                    SSL_CTX_free(context);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                /* Mutual TLS: a client certificate/key pair presented to
                 * a server that requests one (RECOMMENDED: only offered
                 * when the server asks, per normal TLS negotiation --
                 * this does not force client-cert auth on a server that
                 * doesn't want it). Pairing already validated above. */
                if(options.cert!=nullptr) {
                    if(SSL_CTX_use_certificate_chain_file(context,options.cert->chars)!=1||
                       SSL_CTX_use_PrivateKey_file(context,options.key->chars,
                           SSL_FILETYPE_PEM)!=1||
                       SSL_CTX_check_private_key(context)!=1) {
                        char detail[256];tls_format_error(detail,sizeof detail);
                        snprintf(vm->error,sizeof vm->error,
                            "cannot load TLS client certificate/key: %s",detail);
                        SSL_CTX_free(context);close(connected_fd);
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                }
                /* ALPN protocol offer, encoded and handed to the context
                 * before SSL_new -- see tls_encode_alpn_protocols. */
                if(options.alpn!=nullptr) {
                    unsigned char *alpn_encoded=nullptr;unsigned int alpn_encoded_length=0;
                    const DiamondVmStatus alpn_status=tls_encode_alpn_protocols(vm,
                        "TLSSocket.connect",options.alpn,&alpn_encoded,&alpn_encoded_length);
                    if(alpn_status!=DIAMOND_VM_OK) {
                        SSL_CTX_free(context);close(connected_fd);
                        VM_RETURN(alpn_status);
                    }
                    /* SSL_CTX_set_alpn_protos copies the buffer internally
                     * -- freed right after the call regardless of outcome,
                     * matching every OpenSSL client example's own usage. */
                    const int alpn_set_status=
                        SSL_CTX_set_alpn_protos(context,alpn_encoded,alpn_encoded_length);
                    free(alpn_encoded);
                    if(alpn_set_status!=0) {
                        char detail[256];tls_format_error(detail,sizeof detail);
                        snprintf(vm->error,sizeof vm->error,
                            "cannot set ALPN protocols: %s",detail);
                        SSL_CTX_free(context);close(connected_fd);
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                }
                /* Client-side session caching, needed for
                 * tls_new_session_callback to ever fire at all -- the
                 * default mode is server-side caching only, even on a
                 * context created with TLS_client_method(). Registered
                 * unconditionally (not just when `session` was passed in)
                 * since #session should work for capturing a *new*
                 * session to reuse on a future connection too, not only
                 * for resuming one given here. */
                SSL_CTX_set_session_cache_mode(context,SSL_SESS_CACHE_CLIENT);
                SSL_CTX_sess_set_new_cb(context,tls_new_session_callback);
                SSL *ssl=SSL_new(context);
                SSL_CTX_free(context); /* ssl already holds its own reference */
                if(ssl==nullptr) {
                    close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                DiamondTlsSocketHandle *handle=
                    allocate_tls_socket_handle(vm,ssl,connected_fd);
                if(handle==nullptr) {
                    SSL_free(ssl);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                /* Rooted immediately, and app_data wired up before
                 * anything that might trigger tls_new_session_callback --
                 * a TLS <=1.2 session is commonly established
                 * synchronously inside SSL_connect itself, not only via a
                 * later TLS 1.3 post-handshake ticket, so the correlation
                 * needs to already work before SSL_connect is even
                 * called. Every failure path below this point
                 * deliberately does NOT free ssl/close connected_fd
                 * directly any more -- `handle` already owns both, and
                 * letting the ordinary sweep-time cleanup (or an explicit
                 * #close) do it once, later, avoids a double free through
                 * this now-reachable object's own fields. */
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                handle->handshake_pending=start_handshake;
                SSL_set_app_data(ssl,handle);
                if(SSL_set_fd(ssl,connected_fd)!=1) {
                    tls_abort_handshake(handle);VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                /* SNI (which certificate a multi-tenant server presents)
                 * and the hostname check SSL_get_verify_result below
                 * relies on both need a null-terminated hostname --
                 * host->chars always is (see allocate_string). */
                struct in6_addr numeric_host;
                const bool is_ip=inet_pton(AF_INET,host->chars,&numeric_host)==1||
                    inet_pton(AF_INET6,host->chars,&numeric_host)==1;
                const int identity_status=is_ip?
                    X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl),host->chars):
                    SSL_set1_host(ssl,host->chars);
                if(identity_status!=1||(!is_ip&&SSL_set_tlsext_host_name(ssl,host->chars)!=1)) {
                    tls_abort_handshake(handle);VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                /* Session resumption is always best-effort: a `session`
                 * blob that fails to parse (corrupt, or from an
                 * incompatible OpenSSL build/rotated ticket key) is
                 * silently ignored rather than an error -- TLS itself
                 * transparently falls back to a full handshake when
                 * resumption doesn't happen, exactly the same outcome as
                 * if this option had never been given at all.
                 * #session_reused? tells the caller which actually
                 * happened. */
                if(options.session!=nullptr) {
                    const unsigned char *session_cursor=
                        (const unsigned char *)options.session->chars;
                    SSL_SESSION *resume_session=d2i_SSL_SESSION(nullptr,&session_cursor,
                        (long)options.session->length);
                    if(resume_session!=nullptr) {
                        SSL_set_session(ssl,resume_session);
                        SSL_SESSION_free(resume_session); /* SSL_set_session took its own ref */
                    }
                }
                if(start_handshake)break;
                ERR_clear_error();
                if(SSL_connect(ssl)!=1) {
                    char detail[256];tls_format_error(detail,sizeof detail);
                    snprintf(vm->error,sizeof vm->error,
                        "cannot connect to '%.*s' over TLS: %s",
                        (int)host->length,host->chars,detail);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                const long verify_result=SSL_get_verify_result(ssl);
                if(verify_result!=X509_V_OK) {
                    snprintf(vm->error,sizeof vm->error,
                        "TLS certificate verification failed for '%.*s': %s",
                        (int)host->length,host->chars,
                        X509_verify_cert_error_string(verify_result));
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
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
                uint16_t dest=0,readable_reg=0,writable_reg=0,timeout_reg=0;
                READ_SHORT(dest);READ_SHORT(readable_reg);READ_SHORT(writable_reg);
                READ_SHORT(timeout_reg);
                if(registers[readable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[readable_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY||
                   registers[writable_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[writable_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    snprintf(vm->error,sizeof vm->error,"IO.poll readables and writables must be Arrays");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *readable_array=
                    (const DiamondArray *)registers[readable_reg].as.object;
                const DiamondArray *writable_array=
                    (const DiamondArray *)registers[writable_reg].as.object;
                enum { DIAMOND_MAX_POLL_FDS = 256 };
                if(readable_array->count>DIAMOND_MAX_POLL_FDS||
                   writable_array->count>DIAMOND_MAX_POLL_FDS) {
                    snprintf(vm->error,sizeof vm->error,
                        "IO.poll supports at most %d fds per list",DIAMOND_MAX_POLL_FDS);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* Reserve a separate slot for the cancellation notification pipe. */
                struct pollfd fds[DIAMOND_MAX_POLL_FDS+1];
                nfds_t fd_count=0;
                size_t read_slot[DIAMOND_MAX_POLL_FDS];
                size_t write_slot[DIAMOND_MAX_POLL_FDS];
                for(size_t index=0;index<readable_array->count;index++) {
                    int fd=-1;
                    const DiamondVmStatus fd_status=
                        pollable_fd(vm,readable_array->values[index],&fd);
                    VM_PROPAGATE(fd_status);
                    if(!poll_register_fd(fds,&fd_count,DIAMOND_MAX_POLL_FDS,fd,
                            POLLIN,&read_slot[index])) {
                        snprintf(vm->error,sizeof vm->error,
                            "IO.poll supports at most %d distinct fds",DIAMOND_MAX_POLL_FDS);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                }
                for(size_t index=0;index<writable_array->count;index++) {
                    int fd=-1;
                    const DiamondVmStatus fd_status=
                        pollable_fd(vm,writable_array->values[index],&fd);
                    VM_PROPAGATE(fd_status);
                    if(!poll_register_fd(fds,&fd_count,DIAMOND_MAX_POLL_FDS,fd,
                            POLLOUT,&write_slot[index])) {
                        snprintf(vm->error,sizeof vm->error,
                            "IO.poll supports at most %d distinct fds",DIAMOND_MAX_POLL_FDS);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                }
                const DiamondValue timeout=registers[timeout_reg];
                if(timeout.kind==DIAMOND_VALUE_OBJECT&&
                   timeout.as.object->kind==DIAMOND_OBJECT_HASH) {
                    const DiamondHash *options=(const DiamondHash *)timeout.as.object;
                    DiamondValue cancellations=DIAMOND_NIL,deadline=DIAMOND_NIL;
                    for(size_t i=0;i<options->count;i++) {
                        const DiamondValue key=options->entries[i].key;
                        if(key.kind!=DIAMOND_VALUE_OBJECT||key.as.object->kind!=DIAMOND_OBJECT_STRING) {
                            snprintf(vm->error,sizeof vm->error,"IO.poll option keys must be Strings");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        const DiamondString *name=(const DiamondString *)key.as.object;
                        if(name->length==13&&memcmp(name->chars,"cancellations",13)==0)
                            cancellations=options->entries[i].value;
                        else if(name->length==8&&memcmp(name->chars,"deadline",8)==0)
                            deadline=options->entries[i].value;
                        else {
                            snprintf(vm->error,sizeof vm->error,"unknown IO.poll option");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                    }
                    /* Unlink before returning to the VM's signal dispatcher:
                     * handlers can close sockets or cancellation sources. */
                    VM_PROPAGATE(cancellable_wait_helper(vm,nullptr,false,
                        cancellations,deadline,fds,fd_count,nullptr));
                } else if(timeout.kind==DIAMOND_VALUE_INT) {
                    const int64_t timeout_value=registers[timeout_reg].as.integer;
                    const int timeout_ms=timeout_value<0?-1:
                        (timeout_value>INT_MAX?INT_MAX:(int)timeout_value);
                    int poll_result=0;
                    errno=0;
                    poll_result=poll(fds,fd_count,timeout_ms);
                    /* Unlike the plain EINTR-retries-unconditionally loop
                     * this replaced, a signal actually gets handled here
                     * before retrying -- gremlin_serve's own event loop calls
                     * IO.poll with timeout_ms=-1 (block until something's
                     * ready), so blindly retrying on every EINTR would mean a
                     * trapped signal arriving while a gremlin server sits
                     * idle would never actually run its handler until some
                     * connection activity happened to wake the poll() up
                     * first -- exactly backwards for "let me shut this server
                     * down gracefully on Ctrl+C." */
                    while(poll_result<0&&errno==EINTR) {
                        bool signal_invoked=false;
                        const DiamondVmStatus signal_status=
                            dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
                        VM_PROPAGATE_SIGNAL(signal_status);
                        errno=0;
                        poll_result=poll(fds,fd_count,timeout_ms);
                    }
                    if(poll_result<0) {
                        snprintf(vm->error,sizeof vm->error,"poll failed: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                } else {
                    snprintf(vm->error,sizeof vm->error,"IO.poll timeout must be an Int or cancellation options Hash");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                /* POLLHUP/POLLERR/POLLNVAL count toward *both* readiness
                 * directions: a peer that closed its end, or a socket that
                 * hit a genuine error, is exactly the condition a caller's
                 * next .read()/.write() needs to be woken up to observe
                 * (EOF as nil, or the error surfacing as IOError) rather
                 * than sitting forever waiting for a POLLIN/POLLOUT that a
                 * dead connection will never produce. */
                DiamondValue readable_results[DIAMOND_MAX_POLL_FDS];
                for(size_t index=0;index<readable_array->count;index++) {
                    const short revents=fds[read_slot[index]].revents;
                    readable_results[index]=
                        DIAMOND_BOOL((revents&(POLLIN|POLLHUP|POLLERR|POLLNVAL))!=0);
                }
                DiamondValue writable_results[DIAMOND_MAX_POLL_FDS];
                for(size_t index=0;index<writable_array->count;index++) {
                    const short revents=fds[write_slot[index]].revents;
                    writable_results[index]=
                        DIAMOND_BOOL((revents&(POLLOUT|POLLHUP|POLLERR|POLLNVAL))!=0);
                }
                /* Every allocate_* call below can trigger a collection, and
                 * this VM's GC only marks from rooted locations (registers,
                 * the exception slot, frame chains) -- a value sitting in a
                 * plain C local between two allocate_* calls is invisible to
                 * it and would be swept out from under this function
                 * (confirmed the hard way: a heap-use-after-free in
                 * hash_find, caught by `make test-sanitize` under
                 * DIAMOND_STRESS_GC=1, from an earlier version of this code
                 * that allocated all four pieces before touching
                 * registers[dest] at all). So: root the hash in
                 * registers[dest] immediately, then for each entry, root its
                 * key first with a DIAMOND_NIL placeholder value -- nil
                 * needs no GC protection, so this is always safe -- before
                 * allocating the real value and overwriting the placeholder
                 * (hash_set already updates an existing key in place).
                 * Nothing is ever more than one allocation away from being
                 * reachable through registers[dest]. */
                DiamondHash *poll_result_hash=allocate_hash(vm);
                if(poll_result_hash==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(poll_result_hash);
                DiamondString *readable_key=allocate_string(vm,"readable",8);
                if(readable_key==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(readable_key),DIAMOND_NIL))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                DiamondArray *readable_result_array=
                    allocate_array(vm,readable_results,readable_array->count);
                if(readable_result_array==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(readable_key),
                        DIAMOND_OBJECT(readable_result_array)))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                DiamondString *writable_key=allocate_string(vm,"writable",8);
                if(writable_key==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(writable_key),DIAMOND_NIL))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                DiamondArray *writable_result_array=
                    allocate_array(vm,writable_results,writable_array->count);
                if(writable_result_array==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                if(!hash_set(vm,poll_result_hash,DIAMOND_OBJECT(writable_key),
                        DIAMOND_OBJECT(writable_result_array)))
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
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
            case DIAMOND_OP_JSON_STRINGIFY: {
                uint16_t dest=0,source=0;
                READ_SHORT(dest);READ_SHORT(source);
                DiamondValue json_text=DIAMOND_NIL;
                const DiamondVmStatus json_status=json_stringify_document(
                    vm,chunk,depth,registers[source],&json_text);
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
#undef VM_RETURN
#undef VM_PROPAGATE
#undef VM_PROPAGATE_SIGNAL
#undef RECORD_ERROR

    if(depth==0&&vm->running_fiber!=nullptr&&ip>=chunk->code_count) {
        *result=DIAMOND_NIL;
        vm->frames=frame.previous;
        free(heap_registers);
        return DIAMOND_VM_OK;
    }
    vm->frames = frame.previous;
    free(heap_registers);
    return DIAMOND_VM_INVALID_BYTECODE;
}
