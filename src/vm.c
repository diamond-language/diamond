#define _DEFAULT_SOURCE
/* _GNU_SOURCE (a superset of _DEFAULT_SOURCE): only for pthread_getattr_np,
 * used to learn a thread's own native stack bounds for the ASan
 * fiber-switch annotations below. */
#define _GNU_SOURCE

#include "vm.h"
#include "bignum.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdckdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#include <sqlite3.h>
#include <libpq-fe.h>
#include <mysql.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* Fibers hand-switch the C stack via swapcontext (see DIAMOND_FIBER_STACK_SIZE
 * and friends below), which ASan's stack-use-after-return instrumentation
 * doesn't know about on its own -- it associates each real stack address
 * range with at most one live "fake stack" region, and a manual switch to a
 * different memory region (a fiber's own mmapped stack) without telling it
 * looks the same as the previous occupant having returned. The official
 * fix is these paired annotation calls around every swapcontext, which are
 * a no-op unless built with ASan. See docs/threads.md. */
#if defined(__SANITIZE_ADDRESS__) || \
    (defined(__has_feature) && __has_feature(address_sanitizer))
#define DIAMOND_ASAN_FIBERS 1
#include <sanitizer/common_interface_defs.h>

/* A switch back out of a fiber (yield, or the final completing swap) needs
 * real bounds for whatever it's switching *to*, same as switching into a
 * fiber does -- an unknown/null destination isn't the harmless "just skip
 * detection there" the API docs suggest: real testing here (a deeply
 * recursive fiber resumed a second time, allocating under GC stress) showed
 * it still misattributes the *resuming* fiber's own still-live ancestor
 * frames as returned. Each OS thread's own native stack bounds are queried
 * once, lazily, and cached; a resume_target that belongs to another fiber
 * (nested Fiber.resume from within a fiber) instead uses that fiber's own
 * known mmap region. */
static thread_local void *diamond_native_stack_bottom;
static thread_local size_t diamond_native_stack_size;
static thread_local bool diamond_native_stack_known;

static void diamond_ensure_native_stack_bounds(void) {
    if (diamond_native_stack_known) return;
    diamond_native_stack_known = true;
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return;
    void *addr = nullptr; size_t size = 0;
    if (pthread_attr_getstack(&attr, &addr, &size) == 0) {
        diamond_native_stack_bottom = addr;
        diamond_native_stack_size = size;
    }
    pthread_attr_destroy(&attr);
}

/* Bounds of whatever fiber->resumer_fiber (if any) is resuming into, for
 * the "switching back out" annotation calls: another fiber's own known
 * mmap region when nested, otherwise this thread's own native stack. */
static void diamond_resume_target_bounds(const DiamondFiber *fiber,
        const void **bottom, size_t *size) {
    if (fiber->resumer_fiber != nullptr) {
        *bottom = fiber->resumer_fiber->context.uc_stack.ss_sp;
        *size = fiber->resumer_fiber->context.uc_stack.ss_size;
        return;
    }
    diamond_ensure_native_stack_bounds();
    *bottom = diamond_native_stack_bottom;
    *size = diamond_native_stack_size;
}
#endif

/* Each run_chunk activation unconditionally allocates DiamondValue
 * registers[256] (4KB), DiamondTypeBinding bindings[8] (~3.2KB), and
 * UnwindHandler handlers[16] (~0.5KB) as C-stack locals, regardless of the
 * called function's actual complexity. Measured against this machine's
 * default 8MB stack, real (non-instrumented) recursion segfaults around
 * depth ~210-220 in a debug (-O0) build and ~150-160 under AddressSanitizer's
 * redzone-inflated frames -- both well below the depth this counter used to
 * allow, so the guard never had a chance to trip before the native stack
 * actually overflowed.
 *
 * Recalibrated 100 -> 95 when adding native Time support: `-fstack-usage`
 * showed only ~240 bytes of *reported* growth from the new opcodes/
 * arithmetic-and-comparison fallback branches, but that was enough to
 * flip `depth(5000)` (tests/run.sh) from a clean guard trip to a real
 * ASan stack-overflow -- exactly the redzone-per-named-local amplification
 * the READ_SHORT incident just below already documents, not a byte-count
 * story. Empirically, every value from 91 through 99 passed cleanly
 * against the post-Time frame size (100 was the only one that didn't);
 * 95 was chosen to sit in the middle of that window rather than right
 * back at the wall, leaving margin both above `legacy_0091.di`'s own
 * `depth(90)` (which must keep succeeding -- this is a hard floor, not
 * just a preference) and below wherever the next opcode addition's own
 * growth lands. If a future change reopens this margin again, re-run the
 * same empirical sweep (rebuild at a range of candidate values under
 * `make sanitize`, check `depth(5000)` at each) rather than guessing.
 *
 * run_chunk's own `registers` array (below) stays a fixed
 * DIAMOND_INLINE_REGISTER_COUNT-wide C-stack array, at the same 256 this
 * comment's own measurement was taken against, rather than a VLA sized to
 * DIAMOND_REGISTER_COUNT (4096, see vm.h) -- a function whose
 * live_register_count exceeds it (rare -- see DIAMOND_REGISTER_COUNT's
 * own comment) heap-allocates instead, which doesn't consume C stack at
 * all and so can't affect this depth calibration regardless of how large
 * it gets. That register-count widening's first pass also nearly broke
 * this guard for a completely different reason, worth remembering: the
 * new READ_SHORT (below) originally declared its own `high_`/`low_`
 * uint8_t locals to assemble each 16-bit operand, and with ~90 opcodes
 * now reading 1-4 such operands apiece, that put several hundred extra
 * named locals into this one function -- at -O0, under ASan, each got
 * its own padded/redzoned stack slot, which dwarfed the cost of the
 * registers array itself (shrinking it 256->64 barely moved the
 * measured crash depth) and pushed the real crash below depth 90,
 * *under* this very call-depth guard, silently defeating it exactly
 * like an oversized array would have. Rewriting READ_SHORT to read
 * straight out of chunk->code[] into `target_` without any named
 * intermediate restored the original margin. */
enum { DIAMOND_MAX_CALL_DEPTH = 95 };
enum { DIAMOND_INLINE_REGISTER_COUNT = 256 };

typedef enum HandlerKind : uint8_t { HANDLER_RESCUE, HANDLER_ENSURE } HandlerKind;

typedef struct UnwindHandler {
    HandlerKind kind;
    size_t target;
    uint16_t destination;
    uint8_t type_count;
    uint8_t types[8];
    bool enabled;
} UnwindHandler;

typedef enum PendingKind : uint8_t {
    PENDING_NONE,
    PENDING_NORMAL,
    PENDING_RETURN,
    PENDING_EXCEPTION,
} PendingKind;

typedef struct PendingUnwind {
    PendingKind kind;
    DiamondValue value;
    size_t continuation;
} PendingUnwind;

typedef struct DiamondFrame {
    struct DiamondFrame *previous;
    DiamondValue *registers;
    PendingUnwind *pending;
    size_t register_count;
    /* Backtrace support (Exception#backtrace): the owning chunk plus a
     * pointer to run_chunk's own `instruction_offset` local, not a copied
     * value -- a suspended ancestor frame is a real, live C stack frame
     * blocked inside a nested call, so reading through the pointer always
     * reflects its current instruction (the call site) with no need to
     * sync a copy on every dispatch iteration. Valid for exactly the
     * frame's own lifetime, same as the frame struct itself. */
    const DiamondChunk *chunk;
    const size_t *instruction_offset;
} DiamondFrame;

/* Native backing struct for DiamondThreadHandle (object.h) -- see
 * docs/threads.md. `child_vm`/`child_program` are this thread's own,
 * fully independent heap/GC and a byte-for-byte memcpy clone of whatever
 * DiamondProgram was ambient at Thread.new time (see thread_new_helper),
 * never shared with the spawning thread's own program or any other
 * thread's clone. `function_index`/`args`/`arg_count` are captured at
 * spawn time so the OS-thread entry point (thread_entry_trampoline) has
 * everything it needs with no further reference back into the spawning
 * VM's own state. `result`/`raised` are only meaningful once `finished`
 * is true; `finished` is the only field the spawning thread may read
 * without holding `join_lock` (a plain atomic flag, set exactly once,
 * read-only afterward -- .alive?() polls it without blocking).
 * `join_lock` guards the joined/not-yet-joined transition so .join() is
 * safely callable more than once (pthread_join itself is not). */
typedef struct DiamondThread {
    pthread_t handle;
    /* True only once pthread_create actually succeeded for `handle` --
     * distinguishes "there's a real OS thread that must be pthread_join'd"
     * from the synchronous-stub path (Thread.new runs the callable inline,
     * no OS thread ever created) and from a failed pthread_create (also no
     * real thread to join). free_thread only calls pthread_join when this
     * is true. */
    bool spawned;
    DiamondVm *child_vm;
    DiamondProgram *child_program;
    uint16_t function_index;
    DiamondValue args[17];
    uint8_t arg_count;
    /* Three, mutually exclusive outcomes once `finished` is true:
     * (1) plain return -- `raised`/`internal_failure` both false, `result`
     *     is the returned value; (2) the target raised, uncaught -- only
     *     `raised` true, `result` is the actual Diamond exception instance
     *     to re-raise (still living in child_vm's own heap; .join() deep-
     *     copies it back, same as an ordinary return value); (3) the
     *     child hit an internal VM failure that was never a clean Diamond-
     *     level raise (stack overflow, invalid bytecode, ...) -- only
     *     `internal_failure` true, `result` unused, .join() reads
     *     child_vm->error directly (still alive at that point) to build a
     *     fresh ThreadError. */
    DiamondValue result;
    bool raised;
    bool internal_failure;
    atomic_bool finished;
    bool joined;
    pthread_mutex_t join_lock;
} DiamondThread;

static void mark_value(DiamondValue value);
static void mark_frame_chain(void *frames);
static bool gc_protect(DiamondVm *vm, DiamondValue value);
static void gc_unprotect(DiamondVm *vm, size_t saved_count);
static DiamondVmStatus run_chunk(const DiamondChunk *chunk, DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
                                 DiamondValue *result);
static bool hash_set(DiamondVm *vm,DiamondHash *hash,DiamondValue key,
                     DiamondValue value);
static bool array_push(DiamondVm *vm,DiamondArray *array,DiamondValue value);
static void free_adopted_programs(void *list);
static void free_thread(DiamondThread *thread);
static void populate_default_argv_env(DiamondVm *vm);

static void mark_object(DiamondObject *object) {
    if (object == nullptr || object->marked) return;
    object->marked = true;
    if (object->kind == DIAMOND_OBJECT_INSTANCE) {
        DiamondInstance *instance=(DiamondInstance *)object;
        for(size_t i=0;i<instance->field_count;i++) mark_value(instance->fields[i]);
    } else if(object->kind==DIAMOND_OBJECT_ARRAY) {
        DiamondArray *array=(DiamondArray *)object;
        for(size_t i=0;i<array->count;i++) mark_value(array->values[i]);
    } else if(object->kind==DIAMOND_OBJECT_HASH) {
        DiamondHash *hash=(DiamondHash *)object;
        for(size_t i=0;i<hash->count;i++) {
            mark_value(hash->entries[i].key);
            mark_value(hash->entries[i].value);
        }
    } else if(object->kind==DIAMOND_OBJECT_CLOSURE) {
        DiamondClosure *closure=(DiamondClosure *)object;
        for(size_t i=0;i<closure->capture_count;i++)mark_value(closure->captures[i]);
    } else if(object->kind==DIAMOND_OBJECT_CELL) {
        mark_value(((DiamondCell *)object)->value);
    } else if(object->kind==DIAMOND_OBJECT_FIBER) {
        DiamondFiber *fiber=((DiamondFiberHandle *)object)->fiber;
        if(fiber!=nullptr) {
            /* fiber->native_frames is only refreshed when this fiber
             * actually suspends or completes (see diamond_fiber_run) --
             * while it's DIAMOND_FIBER_RUNNING, that field is a stale
             * snapshot from its *previous* suspend, and may by now point at
             * C stack frames that have genuinely already returned (real
             * recursive run_chunk calls unwinding past where they were when
             * that snapshot was taken). A running fiber's actual live
             * frames are already covered elsewhere: diamond_vm_collect's
             * own mark_frame_chain(vm->frames) if it's the innermost
             * running fiber, or an ancestor's own resumer_frames snapshot
             * (also walked there) if it's a fiber blocked resuming a nested
             * child. Every *other* state's native_frames is a safe,
             * up-to-date-enough snapshot -- notably RUNNABLE too, which a
             * fiber scheduler (diamond_fiber_make_runnable) uses for a
             * fiber that just yielded and is waiting for its next turn, not
             * just DIAMOND_FIBER_SUSPENDED. */
            if(fiber->state!=DIAMOND_FIBER_RUNNING)
                mark_frame_chain(fiber->native_frames);
            mark_value(fiber->result);
            mark_value(fiber->resume_value);
            if(fiber->entry_closure!=nullptr)
                mark_object((DiamondObject *)fiber->entry_closure);
        }
    } else if(object->kind==DIAMOND_OBJECT_THREAD) {
        DiamondThread *thread=((DiamondThreadHandle *)object)->thread;
        /* Only once .join() has actually copied `result` into *this*
         * vm's own heap (thread->joined) does it need marking here --
         * before that it lives entirely in child_vm's own, separate heap
         * (this vm's GC has no business scanning another vm's memory),
         * and internal_failure means there's no result value at all (a
         * fresh ThreadError gets built and raised directly at join time
         * instead). See docs/threads.md. */
        if(thread!=nullptr&&thread->joined&&!thread->internal_failure)
            mark_value(thread->result);
    } else if(object->kind==DIAMOND_OBJECT_PROCESS_RESULT) {
        DiamondProcessResult *result=(DiamondProcessResult *)object;
        mark_value(result->stdout_value);
        mark_value(result->stderr_value);
    }
}

static void mark_value(DiamondValue value) {
    if (value.kind == DIAMOND_VALUE_OBJECT) mark_object(value.as.object);
}

static void mark_frame_chain(void *frames) {
    for (DiamondFrame *frame = frames; frame != nullptr;
         frame = frame->previous) {
        for (size_t index = 0; index < frame->register_count; index++) {
            mark_value(frame->registers[index]);
        }
        if(frame->pending!=nullptr && frame->pending->kind!=PENDING_NONE)
            mark_value(frame->pending->value);
    }
}

static void mark_fiber(const DiamondFiber *fiber) {
    if (fiber == nullptr) return;
    /* Same staleness hazard as mark_object's DIAMOND_OBJECT_FIBER case
     * above: native_frames is only untrustworthy while this exact fiber is
     * the one currently RUNNING. */
    if (fiber->state != DIAMOND_FIBER_RUNNING)
        mark_frame_chain(fiber->native_frames);
}

static void diamond_vm_collect_impl(DiamondVm *vm) {
    if(vm->has_exception)mark_value(vm->exception);
    mark_value(vm->argv_value);
    mark_value(vm->env_value);
    for(size_t index=0;index<vm->gc_protected_count;index++)
        mark_value(vm->gc_protected[index]);
    for(size_t index=0;index<DIAMOND_MAX_NAMESPACE_CONSTANTS;index++)
        if(vm->namespace_constant_initialized[index])
            mark_value(vm->namespace_constants[index]);
    /* Never allocated (see class_variables' own comment in vm.h) until
     * the first SET_CVAR -- nothing to mark if no class variable has
     * ever been written on this VM. Once allocated, no initialized
     * bitmap to gate individual slots on either: every never-assigned
     * slot is just DIAMOND_NIL (calloc's own zero-fill), and mark_value
     * on a nil is already a same-cost no-op check, so marking the full
     * flat array unconditionally costs nothing extra over tracking real
     * per-class counts here would. */
    if(vm->class_variables!=nullptr)
        for(size_t index=0;index<(size_t)DIAMOND_MAX_CLASSES*DIAMOND_MAX_FIELDS;index++)
            mark_value(vm->class_variables[index]);
    for(size_t index=0;index<DIAMOND_SIGNAL_COUNT;index++)
        mark_value(vm->trapped_signal_handlers[index]);
    mark_frame_chain(vm->frames);
    for (DiamondFiber *ancestor = vm->running_fiber; ancestor != nullptr;
         ancestor = ancestor->resumer_fiber)
        mark_frame_chain(ancestor->resumer_frames);
    if (vm->root_queue != nullptr)
        for (size_t index = 0; index < diamond_fiber_queue_count(vm->root_queue); index++)
            mark_fiber(diamond_fiber_queue_at(vm->root_queue, index));

    DiamondObject **object = &vm->objects;
    while (*object != nullptr) {
        if ((*object)->marked) {
            (*object)->marked = false;
            object = &(*object)->next;
            continue;
        }
        DiamondObject *unreached = *object;
        *object = unreached->next;
        size_t size=sizeof(DiamondObject);
        if(unreached->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)unreached;
            size=sizeof(DiamondString)+string->length+1;
        } else if(unreached->kind==DIAMOND_OBJECT_INSTANCE) {
            const DiamondInstance *instance=(const DiamondInstance *)unreached;
            size=sizeof(DiamondInstance)+instance->field_count*sizeof(DiamondValue);
        } else if(unreached->kind==DIAMOND_OBJECT_ARRAY) {
            DiamondArray *array=(DiamondArray *)unreached;
            size=sizeof(DiamondArray)+array->capacity*sizeof(DiamondValue);
            for(size_t index=0;index<array->constraint_count;index++)
                free(array->constraints[index].type_variable_bindings);
            free(array->values);
        } else if(unreached->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)unreached;
            size=sizeof(DiamondHash)+hash->capacity*sizeof(DiamondHashEntry)+
                hash->bucket_capacity*sizeof(size_t);
            for(size_t index=0;index<hash->constraint_count;index++)
                free(hash->constraints[index].type_variable_bindings);
            free(hash->entries);free(hash->buckets);
        } else if(unreached->kind==DIAMOND_OBJECT_CLOSURE) {
            size=sizeof(DiamondClosure);
        } else if(unreached->kind==DIAMOND_OBJECT_FIBER) {
            size=sizeof(DiamondFiberHandle);
            diamond_fiber_free(((DiamondFiberHandle *)unreached)->fiber);
        } else if(unreached->kind==DIAMOND_OBJECT_FILE) {
            size=sizeof(DiamondFileHandle);
            FILE *stream=((DiamondFileHandle *)unreached)->stream;
            if(stream!=nullptr)fclose(stream);
        } else if(unreached->kind==DIAMOND_OBJECT_LISTENER) {
            size=sizeof(DiamondListenerHandle);
            DiamondListenerHandle *listener=(DiamondListenerHandle *)unreached;
            if(listener->fd>=0)close(listener->fd);
            if(listener->tls_context!=nullptr)SSL_CTX_free(listener->tls_context);
        } else if(unreached->kind==DIAMOND_OBJECT_SOCKET) {
            size=sizeof(DiamondSocketHandle);
            const int fd=((DiamondSocketHandle *)unreached)->fd;
            if(fd>=0)close(fd);
        } else if(unreached->kind==DIAMOND_OBJECT_UDP_SOCKET) {
            size=sizeof(DiamondUdpSocketHandle);
            const int fd=((DiamondUdpSocketHandle *)unreached)->fd;
            if(fd>=0)close(fd);
        } else if(unreached->kind==DIAMOND_OBJECT_TLS_SOCKET) {
            size=sizeof(DiamondTlsSocketHandle);
            DiamondTlsSocketHandle *tls_handle=(DiamondTlsSocketHandle *)unreached;
            if(tls_handle->ssl!=nullptr) {
                SSL_shutdown(tls_handle->ssl);
                SSL_free(tls_handle->ssl);
            }
            if(tls_handle->fd>=0)close(tls_handle->fd);
        } else if(unreached->kind==DIAMOND_OBJECT_BIGNUM) {
            const DiamondBignum *bignum=(const DiamondBignum *)unreached;
            size=sizeof(DiamondBignum)+bignum->limb_count*sizeof(uint32_t);
        } else if(unreached->kind==DIAMOND_OBJECT_SYMBOL) {
            const DiamondSymbol *symbol=(const DiamondSymbol *)unreached;
            size=sizeof(DiamondSymbol)+symbol->length+1;
        } else if(unreached->kind==DIAMOND_OBJECT_REGEXP) {
            size=sizeof(DiamondRegexp);
            reginold_regex_free(((DiamondRegexp *)unreached)->handle);
        } else if(unreached->kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
            size=sizeof(DiamondProgramBuilder)+sizeof(DiamondProgram);
            DiamondProgramBuilder *builder=(DiamondProgramBuilder *)unreached;
            if(builder->source_bundle!=nullptr) {
                diamond_source_bundle_free(builder->source_bundle);
                free(builder->source_bundle);
            }
            diamond_program_free(builder->program);
            free(builder->program);
        } else if(unreached->kind==DIAMOND_OBJECT_THREAD) {
            size=sizeof(DiamondThreadHandle);
            free_thread(((DiamondThreadHandle *)unreached)->thread);
        } else if(unreached->kind==DIAMOND_OBJECT_SQLITE3) {
            size=sizeof(DiamondSqlite3Handle);
            sqlite3 *db=((DiamondSqlite3Handle *)unreached)->db;
            if(db!=nullptr)sqlite3_close(db);
        } else if(unreached->kind==DIAMOND_OBJECT_POSTGRES) {
            size=sizeof(DiamondPostgresHandle);
            PGconn *conn=((DiamondPostgresHandle *)unreached)->conn;
            if(conn!=nullptr)PQfinish(conn);
        } else if(unreached->kind==DIAMOND_OBJECT_MYSQL) {
            size=sizeof(DiamondMysqlHandle);
            MYSQL *conn=((DiamondMysqlHandle *)unreached)->conn;
            if(conn!=nullptr)mysql_close(conn);
        } else if(unreached->kind==DIAMOND_OBJECT_TIME) {
            /* No owned resource, no separate allocation -- free(unreached)
             * below is all that's needed; this branch exists only for
             * accurate bytes_allocated accounting. */
            size=sizeof(DiamondTime);
        } else if(unreached->kind==DIAMOND_OBJECT_PROCESS_RESULT) {
            /* Same as DIAMOND_OBJECT_TIME above: no owned OS resource by
             * this point (process_run_helper already closed both pipes
             * and reaped the child before ever returning), no separate
             * allocation -- just accounting. */
            size=sizeof(DiamondProcessResult);
        } else {
            size=sizeof(DiamondCell);
        }
        vm->bytes_allocated -= size;
        free(unreached);
    }
    vm->next_gc = vm->bytes_allocated < 1024
        ? 2048 : vm->bytes_allocated * 2;
}

/* Timing wrapper around diamond_vm_collect_impl -- see vm.h's own
 * comment on gc_collection_count/gc_total_seconds for why this exists
 * (DIAMOND_TRACE_GC, src/run_source.c). CLOCK_MONOTONIC, matching every
 * other wall-time measurement in this file (Time.monotonic(), the
 * TCP/TLS/UDP retry-loop deadline checks) -- immune to wall-clock
 * adjustments, which a long-running collection-heavy process is
 * exactly the kind of thing that could otherwise run across. */
void diamond_vm_collect(DiamondVm *vm) {
    struct timespec start={};
    clock_gettime(CLOCK_MONOTONIC,&start);
    diamond_vm_collect_impl(vm);
    struct timespec end={};
    clock_gettime(CLOCK_MONOTONIC,&end);
    vm->gc_collection_count++;
    vm->gc_total_seconds+=
        (double)(end.tv_sec-start.tv_sec)+
        (double)(end.tv_nsec-start.tv_nsec)/1e9;
}

void diamond_vm_init(DiamondVm *vm) {
    *vm = (DiamondVm){.next_gc = 2048};
    vm->quickening_threshold = 1;
    vm->monomorphic_threshold = 1;
    /* A write(2)/SSL_write to a TCP connection the peer has already reset
     * (not just cleanly closed) raises SIGPIPE, whose default disposition
     * is to kill the whole process outright -- surfaced by TLS in
     * particular: OpenSSL servers send a post-handshake NewSessionTicket
     * message automatically for TLS 1.3, and a peer that closes its own
     * socket without ever reading it (the common case for any short-
     * lived connection that errors out or disconnects right after the
     * handshake) leaves that data unread in the kernel receive buffer at
     * close time -- Linux's own trigger for sending RST instead of a
     * plain FIN. Every write-capable I/O path in this file already turns
     * a failed write into a catchable IOError via errno/SSL_get_error,
     * exactly the EPIPE this produces once the fatal signal is out of the
     * way -- so ignoring SIGPIPE here (once per VM, idempotent, matches
     * the universal practice for any long-running networked process) is
     * strictly a bug fix, not a behavior tradeoff: nothing in this VM
     * ever wanted "silently die" as its response to a broken connection. */
    signal(SIGPIPE, SIG_IGN);
    populate_default_argv_env(vm);
}

void diamond_vm_bind_fiber_queue(DiamondVm *vm, const DiamondFiberQueue *queue) {
    if(vm==nullptr)return;
    vm->root_queue=queue;
}

void diamond_vm_free(DiamondVm *vm) {
    DiamondObject *object = vm->objects;
    while (object != nullptr) {
        DiamondObject *next = object->next;
        if(object->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)object;
            for(size_t index=0;index<hash->constraint_count;index++)
                free(hash->constraints[index].type_variable_bindings);
            free(hash->entries);free(hash->buckets);
        } else if(object->kind==DIAMOND_OBJECT_ARRAY) {
            DiamondArray *array=(DiamondArray *)object;
            for(size_t index=0;index<array->constraint_count;index++)
                free(array->constraints[index].type_variable_bindings);
            free(array->values);
        } else if(object->kind==DIAMOND_OBJECT_FIBER) {
            diamond_fiber_free(((DiamondFiberHandle *)object)->fiber);
        } else if(object->kind==DIAMOND_OBJECT_FILE) {
            FILE *stream=((DiamondFileHandle *)object)->stream;
            if(stream!=nullptr)fclose(stream);
        } else if(object->kind==DIAMOND_OBJECT_LISTENER) {
            DiamondListenerHandle *listener=(DiamondListenerHandle *)object;
            if(listener->fd>=0)close(listener->fd);
            if(listener->tls_context!=nullptr)SSL_CTX_free(listener->tls_context);
        } else if(object->kind==DIAMOND_OBJECT_SOCKET) {
            const int fd=((DiamondSocketHandle *)object)->fd;
            if(fd>=0)close(fd);
        } else if(object->kind==DIAMOND_OBJECT_UDP_SOCKET) {
            const int fd=((DiamondUdpSocketHandle *)object)->fd;
            if(fd>=0)close(fd);
        } else if(object->kind==DIAMOND_OBJECT_TLS_SOCKET) {
            DiamondTlsSocketHandle *tls_handle=(DiamondTlsSocketHandle *)object;
            if(tls_handle->ssl!=nullptr) {
                SSL_shutdown(tls_handle->ssl);
                SSL_free(tls_handle->ssl);
            }
            if(tls_handle->fd>=0)close(tls_handle->fd);
        } else if(object->kind==DIAMOND_OBJECT_REGEXP) {
            reginold_regex_free(((DiamondRegexp *)object)->handle);
        } else if(object->kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
            DiamondProgramBuilder *builder=(DiamondProgramBuilder *)object;
            if(builder->source_bundle!=nullptr) {
                diamond_source_bundle_free(builder->source_bundle);
                free(builder->source_bundle);
            }
            diamond_program_free(builder->program);
            free(builder->program);
        } else if(object->kind==DIAMOND_OBJECT_THREAD) {
            free_thread(((DiamondThreadHandle *)object)->thread);
        } else if(object->kind==DIAMOND_OBJECT_SQLITE3) {
            sqlite3 *db=((DiamondSqlite3Handle *)object)->db;
            if(db!=nullptr)sqlite3_close(db);
        } else if(object->kind==DIAMOND_OBJECT_POSTGRES) {
            PGconn *conn=((DiamondPostgresHandle *)object)->conn;
            if(conn!=nullptr)PQfinish(conn);
        } else if(object->kind==DIAMOND_OBJECT_MYSQL) {
            MYSQL *conn=((DiamondMysqlHandle *)object)->conn;
            if(conn!=nullptr)mysql_close(conn);
        }
        free(object);
        object = next;
    }
    free_adopted_programs(vm->adopted_programs);
    free(vm->class_variables);
    free(vm->gc_protected);
    *vm = (DiamondVm){};
}

void diamond_vm_invalidate_method_caches(DiamondVm *vm) {
    memset(vm->method_caches,0,sizeof vm->method_caches);
    vm->inline_cache_hits=0;
    vm->inline_cache_misses=0;
    vm->monomorphic_dispatches=0;
    vm->method_cache_probes=0;
}

enum { DIAMOND_FIBER_STACK_SIZE = 8 * 1024 * 1024 };

static bool allocate_fiber_stack(DiamondFiber *fiber) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t total = DIAMOND_FIBER_STACK_SIZE + page;
    void *base = mmap(nullptr, total, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) return false;
    if (mprotect(base, page, PROT_NONE) != 0) {
        munmap(base, total);
        return false;
    }
    fiber->stack = base;
    fiber->stack_size = total;
    return true;
}

static void free_fiber_stack(DiamondFiber *fiber) {
    if (fiber->stack != nullptr) {
        munmap(fiber->stack, fiber->stack_size);
        fiber->stack = nullptr;
        fiber->stack_size = 0;
    }
}

/* thread_local (not a plain file-scope static): this is how diamond_fiber_run
 * smuggles the entering fiber's identity across swapcontext into
 * diamond_fiber_trampoline, which takes no arguments of its own -- one
 * process-wide pointer here would let two OS threads each running their own
 * fibers race writer/reader against each other the moment more than one
 * thread exists (see Thread, docs/threads.md). Each thread gets its own
 * slot, matching how every other piece of per-run state (DiamondVm itself)
 * is already scoped per-thread by construction. */
static thread_local DiamondFiber *diamond_fiber_entering;

static void diamond_fiber_trampoline(void) {
#ifdef DIAMOND_ASAN_FIBERS
    /* First activation of a fresh fiber stack (via makecontext) never
     * "returns" from a swapcontext call the way a resumed one does, so the
     * matching finish lives here instead, with no saved fake stack to
     * restore -- there's no prior history on a stack that's never run. */
    __sanitizer_finish_switch_fiber(nullptr, nullptr, nullptr);
#endif
    DiamondFiber *self = diamond_fiber_entering;
    if (self->entry_closure != nullptr) {
        const DiamondFunction *fn=
            self->program_tables.functions[self->entry_closure->function_index];
        DiamondChunk child={.name=fn->name,.code=fn->code,.lines=fn->lines,
          .columns=fn->columns,.code_count=fn->code_count,.constants=fn->constants,
          .constant_count=fn->constant_count,.strings=fn->strings,.string_count=fn->string_count,
          .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
          .functions=self->program_tables.functions,.function_count=self->program_tables.function_count,
          .classes=self->program_tables.classes,.class_count=self->program_tables.class_count,
          .interfaces=self->program_tables.interfaces,.interface_count=self->program_tables.interface_count,
          .parameter_type_sets=fn->parameter_type_sets,
          .type_variable_count=fn->type_variable_count,
          .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
          .register_count=fn->register_count};
        self->status=run_chunk(&child,self->vm,nullptr,0,0,self->entry_closure,&self->result);
    } else {
        self->status = run_chunk(self->chunk, self->vm, nullptr, 0, 0, nullptr,
                                 &self->result);
    }
#ifdef DIAMOND_ASAN_FIBERS
    /* This fiber is done for good (completed or failed) and will never be
     * resumed, so its own fake stack should be destroyed rather than saved
     * -- passing nullptr as the first argument is what tells ASan that, per
     * its documented fiber-switch contract -- but the destination we're
     * switching back to still needs real bounds (see
     * diamond_resume_target_bounds). */
    const void *exit_dest_bottom=nullptr;size_t exit_dest_size=0;
    diamond_resume_target_bounds(self,&exit_dest_bottom,&exit_dest_size);
    __sanitizer_start_switch_fiber(nullptr,exit_dest_bottom,exit_dest_size);
#endif
    swapcontext(&self->context, self->resume_target);
}

DiamondFiber *diamond_fiber_new(const DiamondChunk *chunk) {
    DiamondFiber *fiber=calloc(1,sizeof *fiber);
    if(fiber==nullptr)return nullptr;
    fiber->state=DIAMOND_FIBER_NEW;
    fiber->chunk=chunk;
    fiber->result=DIAMOND_NIL;
    fiber->status=DIAMOND_VM_OK;
    return fiber;
}

/* Builds a fiber that invokes a specific closure rather than running a whole
 * chunk from ip=0. Only the sub-tables that are provably stable for the
 * process's lifetime are copied by value -- never chunk->code/constants,
 * which belong to whatever transient stack-local chunk existed at the call
 * site -- so the fiber never retains a pointer that could dangle once that
 * call site returns, however deeply nested Fiber.new(...) was called from. */
static DiamondFiber *diamond_fiber_new_for_closure(
        const DiamondChunk *chunk, const DiamondClosure *closure) {
    DiamondFiber *fiber=diamond_fiber_new(nullptr);
    if(fiber==nullptr)return nullptr;
    fiber->entry_closure=closure;
    fiber->program_tables=(DiamondChunk){
        .functions=chunk->functions,.function_count=chunk->function_count,
        .classes=chunk->classes,.class_count=chunk->class_count,
        .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
    };
    return fiber;
}

void diamond_fiber_free(DiamondFiber *fiber) {
    if(fiber==nullptr)return;
    free_fiber_stack(fiber);
    free(fiber);
}

DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_NEW||
       (fiber->chunk==nullptr&&fiber->entry_closure==nullptr))
        return DIAMOND_FIBER_INVALID_STATE;
    if(!allocate_fiber_stack(fiber))return DIAMOND_FIBER_INVALID_STATE;
    if(getcontext(&fiber->context)!=0) {
        free_fiber_stack(fiber);
        return DIAMOND_FIBER_INVALID_STATE;
    }
    const size_t page=(size_t)sysconf(_SC_PAGESIZE);
    fiber->context.uc_stack.ss_sp=(char *)fiber->stack+page;
    fiber->context.uc_stack.ss_size=DIAMOND_FIBER_STACK_SIZE;
    fiber->context.uc_link=nullptr;
    makecontext(&fiber->context,diamond_fiber_trampoline,0);
    fiber->state=DIAMOND_FIBER_RUNNABLE;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *fiber, DiamondVm *vm) {
    if(fiber==nullptr||vm==nullptr||fiber->state==DIAMOND_FIBER_COMPLETED||
       fiber->state==DIAMOND_FIBER_FAILED)return DIAMOND_FIBER_INVALID_STATE;
    fiber->vm=vm;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_run(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING||fiber->vm==nullptr||
       (fiber->chunk==nullptr&&fiber->entry_closure==nullptr))
        return DIAMOND_FIBER_INVALID_STATE;
    DiamondVm *vm=fiber->vm;
    void *saved_frames=vm->frames;
    DiamondFiber *saved_running=vm->running_fiber;
    fiber->resumer_frames=saved_frames;
    fiber->resumer_fiber=saved_running;
    vm->frames=fiber->native_frames;
    vm->running_fiber=fiber;
    diamond_fiber_entering=fiber;
    ucontext_t caller_context;
    fiber->resume_target=&caller_context;
#ifdef DIAMOND_ASAN_FIBERS
    /* Entering the fiber's own stack (fresh via makecontext, or resuming one
     * parked at a prior yield) -- its bounds are always exactly known since
     * this VM owns the mmap. caller_fake_stack is a plain local: it's
     * produced right before this swapcontext and consumed right after it
     * returns, both in this same call, since re-entering this fiber always
     * comes back through here. */
    void *caller_fake_stack=nullptr;
    __sanitizer_start_switch_fiber(&caller_fake_stack,
        fiber->context.uc_stack.ss_sp,fiber->context.uc_stack.ss_size);
#endif
    swapcontext(&caller_context,&fiber->context);
#ifdef DIAMOND_ASAN_FIBERS
    __sanitizer_finish_switch_fiber(caller_fake_stack,nullptr,nullptr);
#endif
    fiber->native_frames=vm->frames;
    vm->frames=saved_frames;
    vm->running_fiber=saved_running;
    fiber->resumer_frames=nullptr;
    fiber->resumer_fiber=nullptr;
    fiber->state=fiber->status==DIAMOND_VM_OK?DIAMOND_FIBER_COMPLETED:
        (fiber->status==DIAMOND_VM_YIELDED?DIAMOND_FIBER_SUSPENDED:
         DIAMOND_FIBER_FAILED);
    return DIAMOND_FIBER_OK;
}

DiamondValue diamond_fiber_result(const DiamondFiber *fiber) {
    return fiber == nullptr ? DIAMOND_NIL : fiber->result;
}

DiamondVmStatus diamond_fiber_status(const DiamondFiber *fiber) {
    return fiber == nullptr ? DIAMOND_VM_INVALID_BYTECODE : fiber->status;
}

bool diamond_fiber_resumable(const DiamondFiber *fiber) {
    return fiber!=nullptr && (fiber->state==DIAMOND_FIBER_RUNNABLE||
        fiber->state==DIAMOND_FIBER_SUSPENDED);
}

const char *diamond_fiber_state_name(DiamondFiberState state) {
    switch(state) {
        case DIAMOND_FIBER_NEW:return "new";
        case DIAMOND_FIBER_RUNNABLE:return "runnable";
        case DIAMOND_FIBER_RUNNING:return "running";
        case DIAMOND_FIBER_SUSPENDED:return "suspended";
        case DIAMOND_FIBER_COMPLETED:return "completed";
        case DIAMOND_FIBER_FAILED:return "failed";
    }
    return "unknown";
}

DiamondFiberStatus diamond_fiber_make_runnable(DiamondFiber *fiber) {
    if(fiber==nullptr||(fiber->state!=DIAMOND_FIBER_NEW&&
       fiber->state!=DIAMOND_FIBER_SUSPENDED))return DIAMOND_FIBER_INVALID_STATE;
    fiber->state=DIAMOND_FIBER_RUNNABLE;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_begin(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNABLE)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->state=DIAMOND_FIBER_RUNNING;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_resume(DiamondFiber *fiber, DiamondValue value) {
    if(fiber==nullptr||(fiber->state!=DIAMOND_FIBER_RUNNABLE&&
       fiber->state!=DIAMOND_FIBER_SUSPENDED))return DIAMOND_FIBER_INVALID_STATE;
    fiber->resume_value=value;
    fiber->state=DIAMOND_FIBER_RUNNING;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_suspend(DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->state=DIAMOND_FIBER_SUSPENDED;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_yield(DiamondFiber *fiber) {
    return diamond_fiber_suspend(fiber);
}

DiamondFiberStatus diamond_fiber_complete(DiamondFiber *fiber, DiamondValue result) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->result=result;fiber->status=DIAMOND_VM_OK;
    fiber->state=DIAMOND_FIBER_COMPLETED;return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_fail(DiamondFiber *fiber, DiamondVmStatus status) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNING)
        return DIAMOND_FIBER_INVALID_STATE;
    fiber->status=status;fiber->state=DIAMOND_FIBER_FAILED;
    return DIAMOND_FIBER_OK;
}

void diamond_fiber_queue_init(DiamondFiberQueue *queue) {
    *queue=(DiamondFiberQueue){};
}

void diamond_fiber_queue_free(DiamondFiberQueue *queue) {
    free(queue->items);*queue=(DiamondFiberQueue){};
}

bool diamond_fiber_queue_push(DiamondFiberQueue *queue, DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_RUNNABLE)return false;
    if(queue->head>0&&queue->count==queue->capacity) {
        const size_t pending=queue->count-queue->head;
        memmove(queue->items,queue->items+queue->head,pending*sizeof *queue->items);
        queue->head=0;queue->count=pending;
    }
    if(queue->count==queue->capacity) {
        const size_t capacity=queue->capacity==0?8:queue->capacity*2;
        DiamondFiber **items=realloc(queue->items,capacity*sizeof *items);
        if(items==nullptr)return false;
        queue->items=items;queue->capacity=capacity;
    }
    queue->items[queue->count++]=fiber;return true;
}

DiamondFiber *diamond_fiber_queue_pop(DiamondFiberQueue *queue) {
    if(queue->head==queue->count)return nullptr;
    DiamondFiber *fiber=queue->items[queue->head++];
    if(queue->head==queue->count)queue->head=queue->count=0;
    if(fiber->state==DIAMOND_FIBER_RUNNABLE)fiber->state=DIAMOND_FIBER_RUNNING;
    return fiber;
}

size_t diamond_fiber_queue_count(const DiamondFiberQueue *queue) {
    return queue==nullptr?0:queue->count-queue->head;
}

DiamondFiber *diamond_fiber_queue_at(const DiamondFiberQueue *queue, size_t index) {
    if(queue==nullptr||index>=diamond_fiber_queue_count(queue))return nullptr;
    return queue->items[queue->head+index];
}

DiamondFiber *diamond_fiber_scheduler_step(DiamondFiberQueue *queue) {
    return diamond_fiber_queue_pop(queue);
}

bool diamond_fiber_scheduler_requeue(DiamondFiberQueue *queue, DiamondFiber *fiber) {
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_SUSPENDED)return false;
    fiber->state=DIAMOND_FIBER_RUNNABLE;
    if(!diamond_fiber_queue_push(queue,fiber)) {
        fiber->state=DIAMOND_FIBER_SUSPENDED;return false;
    }
    return true;
}

DiamondFiberStatus diamond_fiber_scheduler_run_once(DiamondFiberQueue *queue) {
    DiamondFiber *fiber=diamond_fiber_scheduler_step(queue);
    if(fiber==nullptr)return DIAMOND_FIBER_INVALID_STATE;
    if(diamond_fiber_run(fiber)!=DIAMOND_FIBER_OK)return DIAMOND_FIBER_INVALID_STATE;
    if(fiber->state==DIAMOND_FIBER_SUSPENDED&&
       !diamond_fiber_scheduler_requeue(queue,fiber))return DIAMOND_FIBER_INVALID_STATE;
    return DIAMOND_FIBER_OK;
}

DiamondFiberStatus diamond_fiber_scheduler_run_all(DiamondFiberQueue *queue) {
    if(queue==nullptr)return DIAMOND_FIBER_INVALID_STATE;
    while(diamond_fiber_queue_count(queue)>0) {
        if(diamond_fiber_scheduler_run_once(queue)!=DIAMOND_FIBER_OK)
            return DIAMOND_FIBER_INVALID_STATE;
    }
    return DIAMOND_FIBER_OK;
}

static DiamondString *allocate_string(DiamondVm *vm, const char *chars,
                                      size_t length) {
    if (vm->stress_gc || vm->bytes_allocated >= vm->next_gc) {
        diamond_vm_collect(vm);
    }
    DiamondString *string = malloc(sizeof(DiamondString) + length + 1);
    if (string == nullptr) return nullptr;
    string->object = (DiamondObject){
        .next = vm->objects,
        .kind = DIAMOND_OBJECT_STRING,
    };
    string->length = length;
    memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    vm->objects = &string->object;
    vm->bytes_allocated += sizeof(DiamondString) + length + 1;
    return string;
}

static DiamondSymbol *allocate_symbol(DiamondVm *vm, const char *chars,
                                      size_t length) {
    if (vm->stress_gc || vm->bytes_allocated >= vm->next_gc) {
        diamond_vm_collect(vm);
    }
    DiamondSymbol *symbol = malloc(sizeof(DiamondSymbol) + length + 1);
    if (symbol == nullptr) return nullptr;
    symbol->object = (DiamondObject){
        .next = vm->objects,
        .kind = DIAMOND_OBJECT_SYMBOL,
    };
    symbol->length = length;
    memcpy(symbol->chars, chars, length);
    symbol->chars[length] = '\0';
    vm->objects = &symbol->object;
    vm->bytes_allocated += sizeof(DiamondSymbol) + length + 1;
    return symbol;
}

static DiamondInstance *allocate_instance(DiamondVm *vm,const DiamondClass *class,
                                          const DiamondChunk *chunk) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    const size_t size=sizeof(DiamondInstance)+class->field_count*sizeof(DiamondValue);
    DiamondInstance *instance=malloc(size); if(instance==nullptr)return nullptr;
    instance->object=(DiamondObject){.next=vm->objects,.kind=DIAMOND_OBJECT_INSTANCE};
    instance->class=class; instance->shape=&class->shapes[0]; instance->owner=chunk;
    instance->field_count=class->field_count;
    for(size_t i=0;i<instance->field_count;i++) instance->fields[i]=DIAMOND_NIL;
    vm->objects=&instance->object; vm->bytes_allocated+=size; return instance;
}

static DiamondArray *allocate_array(DiamondVm *vm,const DiamondValue *values,
                                    size_t count) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    const size_t capacity=count;
    const size_t size=sizeof(DiamondArray)+capacity*sizeof(DiamondValue);
    DiamondArray *array=malloc(sizeof(DiamondArray)); if(array==nullptr)return nullptr;
    array->values=capacity==0?nullptr:malloc(capacity*sizeof(DiamondValue));
    if(capacity>0&&array->values==nullptr){free(array);return nullptr;}
    array->object=(DiamondObject){.next=vm->objects,.kind=DIAMOND_OBJECT_ARRAY};
    array->count=count;array->capacity=capacity;array->constraint_count=0;
    for(size_t i=0;i<count;i++) array->values[i]=values[i];
    vm->objects=&array->object;vm->bytes_allocated+=size;return array;
}

static DiamondHash *allocate_hash(DiamondVm *vm) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc) diamond_vm_collect(vm);
    DiamondHash *hash=malloc(sizeof(DiamondHash)); if(hash==nullptr)return nullptr;
    *hash=(DiamondHash){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_HASH}};
    vm->objects=&hash->object;vm->bytes_allocated+=sizeof(DiamondHash);return hash;
}

/* diamond_vm_init's own default for argv_value/env_value -- see that
 * field's comment in vm.h. Best-effort: an allocation failure partway
 * through (env_value in particular, since a real environment can have
 * dozens of entries) just stops early rather than failing VM
 * construction outright -- diamond_vm_init has no error return, and an
 * incomplete ENV is a far better failure mode than none at all when
 * memory is already this tight. */
static void populate_default_argv_env(DiamondVm *vm) {
    DiamondArray *argv=allocate_array(vm,nullptr,0);
    if(argv!=nullptr)vm->argv_value=DIAMOND_OBJECT(argv);
    DiamondHash *env=allocate_hash(vm);
    if(env==nullptr)return;
    vm->env_value=DIAMOND_OBJECT(env);
    for(char **entry=environ;entry!=nullptr&&*entry!=nullptr;entry++) {
        const char *equals=strchr(*entry,'=');
        if(equals==nullptr)continue;
        const size_t key_length=(size_t)(equals-*entry);
        DiamondString *key=allocate_string(vm,*entry,key_length);
        if(key==nullptr)return;
        /* key isn't stored into env (the only thing that would make it
         * GC-visible) until hash_set below -- the allocate_string call for
         * value, right here, can itself trigger a collection (same
         * bytes_allocated>=next_gc check every allocate_* helper makes),
         * which would free key out from under hash_set's own hash_value(key)
         * call. Confirmed via ASan as a real heap-use-after-free, not just
         * theoretical -- same "root the container first, populate
         * incrementally" lesson as regexp_scan_helper/copy_value_into_vm
         * (see docs/roadmap.md), just not yet applied to this newer site. */
        const size_t key_mark=vm->gc_protected_count;
        if(!gc_protect(vm,DIAMOND_OBJECT(key)))return;
        DiamondString *value=allocate_string(vm,equals+1,strlen(equals+1));
        if(value==nullptr){gc_unprotect(vm,key_mark);return;}
        const bool inserted=hash_set(vm,env,DIAMOND_OBJECT(key),DIAMOND_OBJECT(value));
        gc_unprotect(vm,key_mark);
        if(!inserted)return;
    }
}

/* Replaces the default empty ARGV (see populate_default_argv_env above)
 * with the real trailing command-line arguments a top-level script was
 * actually invoked with -- called once, by src/run_source.c, after
 * diamond_vm_init. Not used by a spawned Thread's child_vm or
 * ProgramBuilder#run's internal VM, which keep the empty default (see
 * argv_value's own comment in vm.h for why). Best-effort like
 * populate_default_argv_env: an allocation failure here just leaves
 * ARGV at whatever it already had (the empty default, or a partial
 * prefix), rather than a hard failure with no sensible status to report
 * through this void-returning function. */
void diamond_vm_set_argv(DiamondVm *vm, int argc, char *const *argv) {
    if(argc<=0||argv==nullptr)return;
    DiamondArray *array=allocate_array(vm,nullptr,0);
    if(array==nullptr)return;
    vm->argv_value=DIAMOND_OBJECT(array);
    for(int index=0;index<argc;index++) {
        DiamondString *piece=allocate_string(vm,argv[index],strlen(argv[index]));
        if(piece==nullptr)return;
        if(!array_push(vm,array,DIAMOND_OBJECT(piece)))return;
    }
}

static DiamondClosure *allocate_closure(DiamondVm *vm,uint16_t function_index,
                                        const DiamondValue *captures,size_t count) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondClosure *closure=malloc(sizeof(DiamondClosure));if(closure==nullptr)return nullptr;
    *closure=(DiamondClosure){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_CLOSURE},
      .function_index=function_index,.capture_count=(uint8_t)count};
    for(size_t i=0;i<count;i++)closure->captures[i]=captures[i];
    vm->objects=&closure->object;vm->bytes_allocated+=sizeof(DiamondClosure);return closure;
}

static DiamondCell *allocate_cell(DiamondVm *vm,DiamondValue value) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondCell *cell=malloc(sizeof(DiamondCell));if(cell==nullptr)return nullptr;
    *cell=(DiamondCell){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_CELL},.value=value};
    vm->objects=&cell->object;vm->bytes_allocated+=sizeof(DiamondCell);return cell;
}

static DiamondFiberHandle *allocate_fiber_handle(DiamondVm *vm,DiamondFiber *fiber) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondFiberHandle *handle=malloc(sizeof(DiamondFiberHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondFiberHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_FIBER},.fiber=fiber};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondFiberHandle);return handle;
}

/* A raw OS-thread limit, not a language-level tuning knob: real pthreads
 * are a genuinely limited, comparatively expensive OS resource (unlike
 * Fibers, cheap userspace stacks) and each one commits a cloned program --
 * 64 bounds resource use while still comfortably
 * covering the "a handful of coarse-grained parallel workers" use case
 * this primitive targets, and turns a runaway recursive Thread.new bug
 * into a prompt ThreadError instead of exhausting the host. Process-wide
 * (not per-DiamondVm) since threads spawned from unrelated VMs still
 * compete for the same real OS/memory resources. */
enum { DIAMOND_MAX_THREADS = 64 };
static atomic_size_t diamond_active_thread_count = 0;

/* Builds a fresh, independently-owned DiamondProgram whose
 * function records and classes[]/interfaces[] tables are a deep copy of
 * whatever program `chunk` is a view into -- see docs/threads.md. Used by
 * Thread.new so the spawned thread runs against its own program, never
 * the ambient one (sidesteps REDEFINE_METHOD racing another thread's own
 * dispatch entirely, rather than trying to synchronize it).
 * diamond_program_init handles every field DiamondChunk doesn't expose
 * (modules[]/namespace_constants[]/entry/entry_path) -- all purely
 * compile-time bookkeeping never read by run_chunk (confirmed by their
 * total absence from DiamondChunk itself), so leaving them at
 * diamond_program_init's own defaults is correct, not a gap. The three
 * fixed class/interface arrays are copied directly; independently allocated
 * function records keep thread-local method replacement isolated. Returns
 * nullptr only on allocation failure. */
static DiamondProgram *clone_program_from_chunk(const DiamondChunk *chunk) {
    DiamondProgram *clone=calloc(1,sizeof *clone);
    if(clone==nullptr)return nullptr;
    diamond_program_init(clone);
    for(size_t index=0;index<chunk->function_count;index++) {
        DiamondFunction *function=diamond_program_add_function(clone);
        if(function==nullptr) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        memcpy(function,chunk->functions[index],sizeof *function);
    }
    memcpy(clone->classes,chunk->classes,sizeof clone->classes);
    clone->class_count=chunk->class_count;
    memcpy(clone->interfaces,chunk->interfaces,sizeof clone->interfaces);
    clone->interface_count=chunk->interface_count;
    return clone;
}

/* pthread_create's entry point for a spawned Thread -- runs entirely
 * against `thread`'s own child_vm/child_program (see docs/threads.md),
 * never touching anything owned by the spawning thread, so this needs no
 * reference back into whatever chunk was ambient at Thread.new time (the
 * function to run is looked up in the *clone's* own functions[] table, by
 * the same function_index DIAMOND_OP_THREAD_NEW captured from the
 * closure). Populates the same finished/result/raised/internal_failure
 * contract DIAMOND_OP_THREAD_NEW's own synchronous stub previously did
 * inline -- .join() (run_chunk's INVOKE case) doesn't know or care
 * whether that contract was filled in synchronously or by a real OS
 * thread. Sets `finished` last and unconditionally, exactly once, since
 * that's the only field .alive?() reads without holding join_lock. */
static void *thread_entry_trampoline(void *argument) {
    DiamondThread *thread=(DiamondThread *)argument;
    const DiamondFunction *target_fn=
        thread->child_program->functions[thread->function_index];
    const DiamondChunk child_chunk={
        .name=target_fn->name,.code=target_fn->code,
        .lines=target_fn->lines,.columns=target_fn->columns,
        .code_count=target_fn->code_count,
        .constants=target_fn->constants,.constant_count=target_fn->constant_count,
        .strings=target_fn->strings,.string_count=target_fn->string_count,
        .type_sets=target_fn->type_sets,.type_set_count=target_fn->type_set_count,
        .functions=thread->child_program->functions,
        .function_count=thread->child_program->function_count,
        .classes=thread->child_program->classes,
        .class_count=thread->child_program->class_count,
        .interfaces=thread->child_program->interfaces,
        .interface_count=thread->child_program->interface_count,
        .parameter_type_sets=target_fn->parameter_type_sets,
        .type_variable_count=target_fn->type_variable_count,
        .parameter_offset=target_fn->owner_class==UINT8_MAX?0:1,
        .register_count=target_fn->register_count};
    DiamondValue run_result=DIAMOND_NIL;
    const DiamondVmStatus run_status=run_chunk(&child_chunk,thread->child_vm,
        thread->args,thread->arg_count,0,nullptr,&run_result);
    if(run_status==DIAMOND_VM_EXCEPTION) {
        thread->result=thread->child_vm->exception;thread->raised=true;
    } else if(run_status!=DIAMOND_VM_OK) {
        thread->internal_failure=true;
    } else {
        thread->result=run_result;
    }
    atomic_store(&thread->finished,true);
    return nullptr;
}

static DiamondThreadHandle *allocate_thread_handle(DiamondVm *vm,DiamondThread *thread) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondThreadHandle *handle=malloc(sizeof(DiamondThreadHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondThreadHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_THREAD},.thread=thread};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondThreadHandle);return handle;
}

/* Shared teardown for a DiamondThread, called from both diamond_vm_collect's
 * cycle-based sweep and diamond_vm_free's whole-VM teardown loop (mirroring
 * diamond_fiber_free's own role for DIAMOND_OBJECT_FIBER) -- guarantees no
 * real OS thread ever outlives its DiamondThreadHandle's GC lifetime: if a
 * spawned-but-never-.join()'d thread's handle becomes unreachable (or the
 * whole VM is shutting down), this blocks on pthread_join right here before
 * reclaiming the thread's own fully independent child_vm/child_program.
 * `thread` itself may be nullptr (mirrors diamond_fiber_free's own
 * nullptr-tolerance) so callers don't need their own guard. The sole
 * decrement matching DIAMOND_OP_THREAD_NEW's own increment of
 * diamond_active_thread_count also lives here -- this is the one place
 * every DiamondThread's lifecycle is guaranteed to pass through exactly
 * once, whether reaped after a normal .join() or cleaned up from a
 * spawn-time failure partway through construction. */
static void free_thread(DiamondThread *thread) {
    if(thread==nullptr)return;
    if(thread->spawned&&!thread->joined) {
        pthread_join(thread->handle,nullptr);
        thread->joined=true;
    }
    if(thread->child_vm!=nullptr) {
        diamond_vm_free(thread->child_vm);
        free(thread->child_vm);
    }
    diamond_program_free(thread->child_program);
    free(thread->child_program);
    pthread_mutex_destroy(&thread->join_lock);
    free(thread);
    atomic_fetch_sub(&diamond_active_thread_count,1);
}

static DiamondFileHandle *allocate_file_handle(DiamondVm *vm,FILE *stream) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondFileHandle *handle=malloc(sizeof(DiamondFileHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondFileHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_FILE},.stream=stream};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondFileHandle);return handle;
}

static DiamondListenerHandle *allocate_listener_handle(DiamondVm *vm,int fd,
        bool nonblocking) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondListenerHandle *handle=malloc(sizeof(DiamondListenerHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondListenerHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_LISTENER},
        .fd=fd,.nonblocking=nonblocking};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondListenerHandle);return handle;
}

static DiamondSocketHandle *allocate_socket_handle(DiamondVm *vm,int fd) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondSocketHandle *handle=malloc(sizeof(DiamondSocketHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondSocketHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_SOCKET},.fd=fd};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondSocketHandle);return handle;
}

static DiamondUdpSocketHandle *allocate_udp_socket_handle(DiamondVm *vm,int fd) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondUdpSocketHandle *handle=malloc(sizeof(DiamondUdpSocketHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondUdpSocketHandle){
        .object={.next=vm->objects,.kind=DIAMOND_OBJECT_UDP_SOCKET},.fd=fd};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondUdpSocketHandle);return handle;
}

static DiamondTlsSocketHandle *allocate_tls_socket_handle(DiamondVm *vm,SSL *ssl,int fd) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondTlsSocketHandle *handle=malloc(sizeof(DiamondTlsSocketHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondTlsSocketHandle){
        .object={.next=vm->objects,.kind=DIAMOND_OBJECT_TLS_SOCKET},.ssl=ssl,.fd=fd};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondTlsSocketHandle);return handle;
}

/* Formats the current head of OpenSSL's thread-local error queue (and
 * drains it -- ERR_error_string_n reads then implicitly leaves the queue
 * alone, so a real ERR_get_error() pop is needed first, or a stale error
 * from an earlier, unrelated failed call could be reported here instead
 * of the one that actually just happened). Used for every TLS failure
 * that isn't itself a plain errno/getaddrinfo-style failure. */
static void tls_format_error(char *buffer,size_t buffer_size) {
    const unsigned long code=ERR_get_error();
    if(code==0) {
        (void)snprintf(buffer,buffer_size,"unknown TLS error");
        return;
    }
    ERR_error_string_n(code,buffer,buffer_size);
}

/* Shared behind UDPSocket.bind(port)/UDPSocket.open(). bind_socket==true
 * (UDPSocket.bind) resolves and binds to a specific local port, same
 * getaddrinfo/AI_PASSIVE/try-each-candidate dance as tcp_listen_helper
 * below, minus the listen(2) call UDP has no equivalent of -- one socket
 * both sends and receives, so once bound there's nothing more to set up.
 * bind_socket==false (UDPSocket.open) just opens a plain AF_INET socket
 * with no local address, letting the OS assign an ephemeral port on
 * first use -- the common "client that doesn't care what port it sends
 * from" case. Deliberately AF_INET only (not AF_UNSPEC/getaddrinfo, which
 * needs a destination to resolve against and open() has none yet): an
 * unbound socket created this way can only reach IPv4 destinations from
 * a later .send(data, host, port) -- a real, documented scope cut, not
 * an oversight (see docs/io.md). */
static DiamondVmStatus udp_socket_helper(DiamondVm *vm,bool bind_socket,
        int64_t port,DiamondUdpSocketHandle **out_handle) {
    int fd=-1;
    if(bind_socket) {
        char port_text[32];
        (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
        struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_DGRAM,
            .ai_flags=AI_PASSIVE};
        struct addrinfo *results=nullptr;
        const int resolve_status=getaddrinfo(nullptr,port_text,&hints,&results);
        if(resolve_status!=0) {
            snprintf(vm->error,sizeof vm->error,"cannot bind UDP port %s: %s",
                     port_text,gai_strerror(resolve_status));
            return DIAMOND_VM_IO_ERROR;
        }
        int last_errno=0;
        for(struct addrinfo *candidate=results;candidate!=nullptr;
            candidate=candidate->ai_next) {
            const int candidate_fd=socket(candidate->ai_family,candidate->ai_socktype,
                                 candidate->ai_protocol);
            if(candidate_fd<0) {last_errno=errno;continue;}
            if(bind(candidate_fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
                fd=candidate_fd;break;
            }
            last_errno=errno;close(candidate_fd);
        }
        freeaddrinfo(results);
        if(fd<0) {
            snprintf(vm->error,sizeof vm->error,"cannot bind UDP port %s: %s",
                     port_text,strerror(last_errno));
            return DIAMOND_VM_IO_ERROR;
        }
    } else {
        fd=socket(AF_INET,SOCK_DGRAM,0);
        if(fd<0) {
            snprintf(vm->error,sizeof vm->error,"cannot open UDP socket: %s",strerror(errno));
            return DIAMOND_VM_IO_ERROR;
        }
    }
    DiamondUdpSocketHandle *handle=allocate_udp_socket_handle(vm,fd);
    if(handle==nullptr) {
        close(fd);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *out_handle=handle;
    return DIAMOND_VM_OK;
}

/* Signal.trap(name, handler) support. Deliberately a small, fixed set of
 * names rather than every signal POSIX knows about -- these three cover
 * "someone asked this process to stop" (INT: Ctrl+C, TERM: kill,
 * HUP: terminal/controlling-process hangup), the case a Diamond program
 * actually wants to react to (graceful server shutdown -- close
 * listening sockets, finish in-flight requests -- being the motivating
 * use case). SIGKILL/SIGSTOP can't be caught at the OS level regardless;
 * everything else (SIGSEGV, SIGCHLD, real-time signals, ...) is out of
 * scope for this first slice.
 *
 * File-scope (not DiamondVm fields) because signal delivery is a process-
 * wide OS concept, not a per-VM-instance one -- sigaction installs one
 * handler for the whole process regardless of which DiamondVm happens to
 * be running when it fires. The C handler itself only does what POSIX
 * guarantees is async-signal-safe: set a volatile sig_atomic_t and
 * return. Everything else -- resolving which Diamond closure to call,
 * actually calling it -- happens later, synchronously, from ordinary
 * (non-signal-handler) code that checks these flags at safe points: once
 * per bytecode instruction in run_chunk's own dispatch loop (see below),
 * and after EINTR from the handful of genuinely-blocking native calls
 * where waiting for the *next* bytecode instruction to run this check
 * could mean waiting arbitrarily long (TCPServer#accept, IO.poll,
 * UDPSocket#receive -- see their own opcode handlers). */
static const int diamond_signal_numbers[DIAMOND_SIGNAL_COUNT]={SIGINT,SIGTERM,SIGHUP};
static const char *const diamond_signal_names[DIAMOND_SIGNAL_COUNT]={"INT","TERM","HUP"};
static volatile sig_atomic_t diamond_signal_pending[DIAMOND_SIGNAL_COUNT]={0};
static volatile sig_atomic_t diamond_any_signal_pending=0;

static void diamond_signal_handler(int signal_number) {
    for(size_t index=0;index<DIAMOND_SIGNAL_COUNT;index++) {
        if(diamond_signal_numbers[index]==signal_number) {
            diamond_signal_pending[index]=1;
            diamond_any_signal_pending=1;
            return;
        }
    }
}

/* Invokes every currently-pending trapped signal's Diamond handler, in
 * signal-index order, synchronously -- via the same nested-run_chunk
 * mechanism DIAMOND_OP_CALL_CLOSURE itself uses, since a trapped handler
 * is an ordinary 0-arity Callable, not anything signal-specific at the
 * bytecode level. Clears each signal's own pending flag (and the
 * combined any-pending flag) before invoking its handler, not after --
 * a second delivery of the same signal *during* handler execution should
 * queue another invocation next time this runs, not be silently dropped
 * because the flag was still "pending" from the call already in
 * progress. Returns as soon as one handler's own status is non-OK
 * (an uncaught exception or worse from inside the handler itself) so the
 * caller can propagate it exactly like any other mid-dispatch failure;
 * remaining still-pending signals are simply handled on the next check
 * rather than lost. *any_invoked is purely informational for callers
 * that want to know whether anything actually happened (none of the
 * current call sites need it, but see docs/io.md before assuming it's
 * safe to drop -- future EINTR-retry call sites may want to distinguish
 * "a handler ran, retry" from "spurious EINTR, still retry" more
 * carefully than accept/poll/receive currently need to). */
static DiamondVmStatus dispatch_pending_signals(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,bool *any_invoked) {
    *any_invoked=false;
    if(!diamond_any_signal_pending)return DIAMOND_VM_OK;
    diamond_any_signal_pending=0;
    for(size_t index=0;index<DIAMOND_SIGNAL_COUNT;index++) {
        if(!diamond_signal_pending[index])continue;
        diamond_signal_pending[index]=0;
        if(vm->trapped_signal_handlers[index].kind!=DIAMOND_VALUE_OBJECT||
           vm->trapped_signal_handlers[index].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
            continue;
        *any_invoked=true;
        const DiamondClosure *handler=
            (const DiamondClosure *)vm->trapped_signal_handlers[index].as.object;
        if(handler->function_index>=chunk->function_count)continue;
        const DiamondFunction *fn=chunk->functions[handler->function_index];
        DiamondChunk child={.name=fn->name,.code=fn->code,.lines=fn->lines,
          .columns=fn->columns,.code_count=fn->code_count,.constants=fn->constants,
          .constant_count=fn->constant_count,.strings=fn->strings,.string_count=fn->string_count,
          .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
          .functions=chunk->functions,.function_count=chunk->function_count,
          .classes=chunk->classes,.class_count=chunk->class_count,
          .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
          .parameter_type_sets=fn->parameter_type_sets,
          .type_variable_count=fn->type_variable_count,
          .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
          .register_count=fn->register_count};
        DiamondValue ignored=DIAMOND_NIL;
        const DiamondVmStatus status=run_chunk(&child,vm,nullptr,0,depth+1,handler,&ignored);
        if(status!=DIAMOND_VM_OK)return status;
    }
    return DIAMOND_VM_OK;
}

/* Shared getaddrinfo/socket/connect dance behind TCPSocket.connect and
 * TLSSocket.connect -- both need a connected fd before doing anything
 * TLS-specific, so this is exactly the code TCP_CONNECT's own handler
 * used to have inline, unchanged, just callable from a second opcode
 * handler now too. No SA_RESTART-style signal-retry here, matching the
 * existing TCPSocket.connect scope cut documented in docs/io.md: a
 * signal arriving mid-connect makes the attempt fail rather than
 * transparently resuming, same as it always has. */
static DiamondVmStatus tcp_connect_helper(DiamondVm *vm,const DiamondString *host,
        int64_t port,int *out_fd) {
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM};
    struct addrinfo *results=nullptr;
    const int resolve_status=getaddrinfo(host->chars,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                 (int)host->length,host->chars,port_text,gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    int connected_fd=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        const int fd=socket(candidate->ai_family,candidate->ai_socktype,
                             candidate->ai_protocol);
        if(fd<0) {last_errno=errno;continue;}
        if(connect(fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
            connected_fd=fd;break;
        }
        last_errno=errno;close(fd);
    }
    freeaddrinfo(results);
    if(connected_fd<0) {
        snprintf(vm->error,sizeof vm->error,"cannot connect to '%.*s:%s': %s",
                 (int)host->length,host->chars,port_text,strerror(last_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    *out_fd=connected_fd;
    return DIAMOND_VM_OK;
}

/* Shared getaddrinfo/socket/bind/listen dance behind TCPServer.listen and
 * TCPServer.listen_nonblocking -- identical except for whether the
 * resulting fd is set O_NONBLOCK and which flavor of DiamondListenerHandle
 * comes out the other end. Follows the established helper-returns-status
 * convention (see stringify_value/regexp_new_helper) since VM_RETURN/
 * VM_PROPAGATE are only usable inside run_chunk's own dispatch loop.
 * `reuse_port` sets SO_REUSEPORT (opt-in, off by default -- unlike
 * SO_REUSEADDR below, which is unconditional and only affects rebinding
 * after close): lets more than one independent listening socket bind the
 * *same* port, with the kernel load-balancing new connections across them.
 * Exists for a multi-threaded server where each OS thread runs its own
 * independent accept loop against its own listener rather than sharing one
 * across a Thread boundary (Listener values can never cross one -- see
 * docs/threads.md) -- see packages/gremlin's own gremlin_worker. */
static DiamondVmStatus tcp_listen_helper(DiamondVm *vm,int64_t port,
        bool nonblocking,bool reuse_port,DiamondListenerHandle **out_handle) {
    char port_text[32];
    (void)snprintf(port_text,sizeof port_text,"%" PRId64,port);
    struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_STREAM,
        .ai_flags=AI_PASSIVE};
    struct addrinfo *results=nullptr;
    const int resolve_status=getaddrinfo(nullptr,port_text,&hints,&results);
    if(resolve_status!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                 port_text,gai_strerror(resolve_status));
        return DIAMOND_VM_IO_ERROR;
    }
    int listening_fd=-1;
    int last_errno=0;
    for(struct addrinfo *candidate=results;candidate!=nullptr;
        candidate=candidate->ai_next) {
        const int fd=socket(candidate->ai_family,candidate->ai_socktype,
                             candidate->ai_protocol);
        if(fd<0) {last_errno=errno;continue;}
        const int yes=1;
        (void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes);
        if(reuse_port)(void)setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&yes,sizeof yes);
        if(bind(fd,candidate->ai_addr,candidate->ai_addrlen)==0) {
            listening_fd=fd;break;
        }
        last_errno=errno;close(fd);
    }
    freeaddrinfo(results);
    if(listening_fd<0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                 port_text,strerror(last_errno));
        return DIAMOND_VM_IO_ERROR;
    }
    if(listen(listening_fd,16)!=0) {
        snprintf(vm->error,sizeof vm->error,"cannot listen on port %s: %s",
                 port_text,strerror(errno));
        close(listening_fd);
        return DIAMOND_VM_IO_ERROR;
    }
    if(nonblocking) {
        const int flags=fcntl(listening_fd,F_GETFL,0);
        if(flags<0||fcntl(listening_fd,F_SETFL,flags|O_NONBLOCK)<0) {
            snprintf(vm->error,sizeof vm->error,
                     "cannot listen on port %s: %s",port_text,strerror(errno));
            close(listening_fd);
            return DIAMOND_VM_IO_ERROR;
        }
    }
    DiamondListenerHandle *listener_handle=
        allocate_listener_handle(vm,listening_fd,nonblocking);
    if(listener_handle==nullptr) {
        close(listening_fd);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *out_handle=listener_handle;
    return DIAMOND_VM_OK;
}

/* Shared behind TLSServer.listen(port, cert_path, key_path): the same
 * plain-TCP listen tcp_listen_helper already does (a TLS listener is a
 * regular listening socket underneath -- the TLS part only starts once a
 * connection is actually accepted), plus loading the server's
 * certificate chain and private key into one SSL_CTX that every future
 * .accept() on this listener reuses (see DiamondListenerHandle's own
 * tls_context field comment for the lifetime story). Loaded once here
 * rather than per-connection specifically so a listener with a broken
 * cert/key pair fails loudly at TLSServer.listen time, not silently on
 * whichever connection happens to be first. */
static DiamondVmStatus tls_listen_helper(DiamondVm *vm,int64_t port,
        const char *cert_path,const char *key_path,DiamondListenerHandle **out_handle) {
    DiamondListenerHandle *listener_handle=nullptr;
    const DiamondVmStatus listen_status=tcp_listen_helper(vm,port,false,false,&listener_handle);
    if(listen_status!=DIAMOND_VM_OK)return listen_status;
    SSL_CTX *context=SSL_CTX_new(TLS_server_method());
    if(context==nullptr) {
        close(listener_handle->fd);listener_handle->fd=-1;
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,"cannot create TLS context: %s",detail);
        return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_CTX_use_certificate_chain_file(context,cert_path)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "cannot load TLS certificate '%s': %s",cert_path,detail);
        SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
        return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_CTX_use_PrivateKey_file(context,key_path,SSL_FILETYPE_PEM)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "cannot load TLS private key '%s': %s",key_path,detail);
        SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
        return DIAMOND_VM_IO_ERROR;
    }
    if(SSL_CTX_check_private_key(context)!=1) {
        char detail[256];tls_format_error(detail,sizeof detail);
        snprintf(vm->error,sizeof vm->error,
            "TLS certificate and private key do not match: %s",detail);
        SSL_CTX_free(context);close(listener_handle->fd);listener_handle->fd=-1;
        return DIAMOND_VM_IO_ERROR;
    }
    listener_handle->tls_context=context;
    *out_handle=listener_handle;
    return DIAMOND_VM_OK;
}

/* IO.poll accepts only the two object kinds that actually own a pollable
 * fd -- a TCPServer.listen_nonblocking listener (interesting for
 * readability: a pending connection) or one of its accepted Sockets
 * (interesting for either). A blocking TCPServer.listen listener/
 * TCPSocket.connect File is deliberately not accepted: poll()ing a
 * blocking-mode fd is meaningless here, since nothing in this VM ever
 * puts one in non-blocking mode, so it would always appear either always-
 * ready or never-ready depending on kernel buffering, never the genuine
 * signal IO.poll's caller needs. */
static DiamondVmStatus pollable_fd(DiamondVm *vm,DiamondValue value,int *out_fd) {
    if(value.kind!=DIAMOND_VALUE_OBJECT||
       (value.as.object->kind!=DIAMOND_OBJECT_LISTENER&&
        value.as.object->kind!=DIAMOND_OBJECT_SOCKET)) {
        snprintf(vm->error,sizeof vm->error,
            "IO.poll arguments must be nonblocking TCPServer listeners or their accepted Sockets");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int fd=value.as.object->kind==DIAMOND_OBJECT_LISTENER?
        ((DiamondListenerHandle *)value.as.object)->fd:
        ((DiamondSocketHandle *)value.as.object)->fd;
    if(fd<0) {
        snprintf(vm->error,sizeof vm->error,"cannot poll a closed listener/socket");
        return DIAMOND_VM_IO_ERROR;
    }
    *out_fd=fd;
    return DIAMOND_VM_OK;
}

/* Diamond's readables/writables are separate Arrays (see DIAMOND_OP_IO_POLL
 * below for why -- avoids needing Array#include?/object-identity checks on
 * the Diamond side), but the same fd can legitimately appear in both --
 * poll(2) itself keys purely by fd, so registering it twice would just
 * overwrite events instead of OR-ing them together. Returns false only
 * when max_fds is exhausted by a genuinely new fd. */
static bool poll_register_fd(struct pollfd *fds,nfds_t *fd_count,size_t max_fds,
        int fd,short want_events,size_t *out_slot) {
    for(size_t existing=0;existing<*fd_count;existing++) {
        if(fds[existing].fd==fd) {
            fds[existing].events=(short)(fds[existing].events|want_events);
            *out_slot=existing;
            return true;
        }
    }
    if(*fd_count>=max_fds)return false;
    fds[*fd_count]=(struct pollfd){.fd=fd,.events=want_events,.revents=0};
    *out_slot=*fd_count;
    (*fd_count)++;
    return true;
}

static DiamondRegexp *allocate_regexp_handle(DiamondVm *vm,reginold_regex *compiled) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondRegexp *regexp=malloc(sizeof(DiamondRegexp));
    if(regexp==nullptr)return nullptr;
    *regexp=(DiamondRegexp){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_REGEXP},
        .handle=compiled};
    vm->objects=&regexp->object;vm->bytes_allocated+=sizeof(DiamondRegexp);return regexp;
}

static DiamondSqlite3Handle *allocate_sqlite3_handle(DiamondVm *vm,sqlite3 *db) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondSqlite3Handle *handle=malloc(sizeof(DiamondSqlite3Handle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondSqlite3Handle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_SQLITE3},
        .db=db};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondSqlite3Handle);return handle;
}

static DiamondPostgresHandle *allocate_postgres_handle(DiamondVm *vm,PGconn *conn) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondPostgresHandle *handle=malloc(sizeof(DiamondPostgresHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondPostgresHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_POSTGRES},
        .conn=conn};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondPostgresHandle);return handle;
}

static DiamondMysqlHandle *allocate_mysql_handle(DiamondVm *vm,MYSQL *conn) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondMysqlHandle *handle=malloc(sizeof(DiamondMysqlHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondMysqlHandle){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_MYSQL},
        .conn=conn};
    vm->objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondMysqlHandle);return handle;
}

static DiamondTime *allocate_time(DiamondVm *vm,double epoch,bool utc) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondTime *time=malloc(sizeof(DiamondTime));
    if(time==nullptr)return nullptr;
    *time=(DiamondTime){.object={.next=vm->objects,.kind=DIAMOND_OBJECT_TIME},
        .epoch=epoch,.utc=utc};
    vm->objects=&time->object;vm->bytes_allocated+=sizeof(DiamondTime);return time;
}

/* Allocated (and rooted into the caller's dest register) before
 * process_run_helper does any of its own work, with stdout_value/
 * stderr_value still nil -- the same "root the container before filling
 * it in" ordering String#split's pieces array uses, since spawning and
 * draining a child process means several further allocations (the two
 * captured-output Strings) that can each trigger a GC. */
static DiamondProcessResult *allocate_process_result(DiamondVm *vm) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondProcessResult *result=malloc(sizeof(DiamondProcessResult));
    if(result==nullptr)return nullptr;
    *result=(DiamondProcessResult){
        .object={.next=vm->objects,.kind=DIAMOND_OBJECT_PROCESS_RESULT},
        .stdout_value=DIAMOND_NIL,.stderr_value=DIAMOND_NIL,.exit_code=0};
    vm->objects=&result->object;
    vm->bytes_allocated+=sizeof(DiamondProcessResult);
    return result;
}

/* Private backing type for DiamondVm.adopted_programs (see its own
 * comment, src/vm.h) -- a program a ProgramBuilder-returned Instance
 * still needs, kept alive for the rest of this vm's lifetime instead of
 * being freed with the temporary run_vm that built it. `chunk` is
 * computed once at adoption time and never moves again (the node itself
 * is heap-allocated and never relocated), so it's safe for any number of
 * DiamondInstance.owner fields to keep pointing at &node->chunk
 * indefinitely. */
typedef struct DiamondAdoptedProgram {
    DiamondProgram *program;
    DiamondChunk chunk;
    struct DiamondAdoptedProgram *next;
} DiamondAdoptedProgram;

/* Transfers ownership of `program` (previously a ProgramBuilder's own
 * `program`, about to otherwise be freed alongside it) to `vm`, and
 * returns a pointer to the persistent DiamondChunk view a copied
 * instance's `owner` field can safely reference forever. Returns nullptr
 * (leaving `program` unadopted, caller still responsible for it) only on
 * allocation failure. */
static const DiamondChunk *adopt_program(DiamondVm *vm,DiamondProgram *program) {
    DiamondAdoptedProgram *node=malloc(sizeof *node);
    if(node==nullptr)return nullptr;
    node->program=program;
    node->chunk=diamond_program_chunk(program);
    node->next=(DiamondAdoptedProgram *)vm->adopted_programs;
    vm->adopted_programs=node;
    return &node->chunk;
}

static void free_adopted_programs(void *list) {
    DiamondAdoptedProgram *node=(DiamondAdoptedProgram *)list;
    while(node!=nullptr) {
        DiamondAdoptedProgram *next=node->next;
        diamond_program_free(node->program);
        free(node->program);
        free(node);
        node=next;
    }
}

/* Account for the program container here; dynamically added function records
 * are accounted for by ProgramBuilder#declare_function. */
static DiamondProgramBuilder *allocate_program_builder(DiamondVm *vm) {
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    DiamondProgram *built=calloc(1,sizeof *built);
    if(built==nullptr)return nullptr;
    diamond_program_init(built);
    DiamondProgramBuilder *handle=malloc(sizeof(DiamondProgramBuilder));
    if(handle==nullptr){diamond_program_free(built);free(built);return nullptr;}
    *handle=(DiamondProgramBuilder){
        .object={.next=vm->objects,.kind=DIAMOND_OBJECT_PROGRAM_BUILDER},
        .program=built,.source_bundle=nullptr,.source_line=0,.source_column=0};
    vm->objects=&handle->object;
    vm->bytes_allocated+=sizeof(DiamondProgramBuilder)+sizeof(DiamondProgram);
    return handle;
}

/* function_index==-1 targets the program's entry function; 0..function_count-1
 * targets program->functions[index]. Returns nullptr on any other value. */
static DiamondFunction *program_builder_target(DiamondProgram *program,
                                                int64_t function_index) {
    if(function_index==-1) return &program->entry;
    if(function_index<0||(uint64_t)function_index>=program->function_count)
        return nullptr;
    return program->functions[function_index];
}

static DiamondClass *program_builder_class(DiamondProgram *program,
                                           int64_t class_index) {
    if(class_index<0||(uint64_t)class_index>=program->class_count)
        return nullptr;
    return &program->classes[class_index];
}

/* Recomputes shapes[0..field_count] for a class -- mirrors the loop
 * diamond_compile itself runs once, over every class, right after
 * compilation finishes (src/compiler.c). ProgramBuilder#declare_field
 * needs the same recomputation done incrementally, since a
 * ProgramBuilder-built program never goes through diamond_compile at
 * all. See docs/roadmap.md's self-hosting Phase 3 entry. */
static void program_builder_recompute_shapes(DiamondClass *class) {
    for(size_t field_count=0;field_count<=class->field_count;field_count++)
        class->shapes[field_count]=(DiamondShape){
            .class=class,.field_count=(uint8_t)field_count};
}

/* DIAMOND_OP_REGEXP_NEW's real body, factored out of run_chunk's own
 * switch statement deliberately, not just for readability: every local
 * variable declared anywhere in that switch contributes to run_chunk's
 * one stack frame regardless of which case actually runs (a -O0 build,
 * which this project's debug/sanitize builds both are, does not reuse
 * stack slots across sibling blocks), and DIAMOND_MAX_CALL_DEPTH is
 * calibrated against that frame's worst-case size to stay safe under
 * AddressSanitizer's redzone-inflated frames (see docs/design.md). A
 * reginold_error alone is ~112 bytes (its message buffer is
 * REGINOLD_ERROR_MSG_MAX=90); adding it and reginold_match's fields
 * directly into run_chunk's frame regressed depth(5000)-style recursion
 * into a genuine ASan stack-overflow crash before the depth counter ever
 * tripped -- caught by make test-sanitize, not by hand-testing, since the
 * regex feature itself worked perfectly right up until deep recursion
 * was exercised. Splitting this into its own function moves those locals
 * into a separate, transient frame that only exists while regex code is
 * actually running, not on every recursive run_chunk level. */
static DiamondVmStatus regexp_new_helper(DiamondVm *vm, const DiamondString *pattern,
        int64_t options, DiamondValue *result) {
    reginold_regex *compiled=nullptr;
    reginold_error compile_error={0};
    const reginold_status compile_status=reginold_compile(pattern->chars,
        pattern->length,(unsigned int)options,&compiled,&compile_error);
    if(compile_status!=REGINOLD_OK) {
        snprintf(vm->error,sizeof vm->error,"%.*s",
            (int)compile_error.message_len,compile_error.message);
        return DIAMOND_VM_REGEXP_ERROR;
    }
    DiamondRegexp *regexp=allocate_regexp_handle(vm,compiled);
    if(regexp==nullptr) {
        reginold_regex_free(compiled);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(regexp);
    return DIAMOND_VM_OK;
}

/* Regexp#match/#match? real body -- same stack-frame-isolation reasoning
 * as regexp_new_helper above (reginold_match's own fields would otherwise
 * land directly in run_chunk's frame too). */
/* Root through registers[dest] directly, not an out-param -- same real
 * bug, and same fix, as regexp_scan_helper above (see that function's
 * own comment for the full story: *result pointed into the caller's C
 * stack, never a real GC root, and the `groups` malloc'd buffer this
 * used to build had zero GC visibility of its own between one capture
 * group's String allocation and the next). */
static DiamondVmStatus regexp_match_helper(DiamondVm *vm, const DiamondRegexp *regexp,
        const DiamondString *subject, bool test_only,
        DiamondValue *registers, uint16_t dest) {
    if(test_only) {
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,0,nullptr);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        registers[dest]=DIAMOND_BOOL(search_status==REGINOLD_OK);
        return DIAMOND_VM_OK;
    }
    reginold_match match_result={0};
    const reginold_status search_status=reginold_search(regexp->handle,
        subject->chars,subject->length,0,&match_result);
    if(search_status==REGINOLD_ERROR) {
        snprintf(vm->error,sizeof vm->error,"regexp match failed");
        return DIAMOND_VM_REGEXP_ERROR;
    }
    if(search_status==REGINOLD_MISMATCH) {
        registers[dest]=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    DiamondArray *result_array=allocate_array(vm,nullptr,0);
    if(result_array==nullptr) {
        reginold_match_free(&match_result);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    registers[dest]=DIAMOND_OBJECT(result_array);
    const size_t group_count=1+match_result.capture_count;
    for(size_t index=0;index<group_count;index++) {
        const reginold_span span=index==0?match_result.overall:
            match_result.captures[index-1];
        DiamondValue group_value=DIAMOND_NIL;
        if(span.beg>=0&&span.end>=0) {
            DiamondString *group_string=allocate_string(vm,
                subject->chars+span.beg,(size_t)(span.end-span.beg));
            if(group_string==nullptr) {
                reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
            }
            group_value=DIAMOND_OBJECT(group_string);
        }
        if(!array_push(vm,result_array,group_value)) {
            reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    reginold_match_free(&match_result);
    return DIAMOND_VM_OK;
}

/* A growable byte buffer for regexp_replace_helper's own output, the only
 * place in this file that needs to build a string of unknown final length
 * incrementally rather than in one allocate_string call. */
typedef struct ByteBuffer {
    char *data;
    size_t length;
    size_t capacity;
} ByteBuffer;

static bool byte_buffer_append(ByteBuffer *buffer,const char *bytes,size_t count) {
    if(count==0)return true;
    if(count>SIZE_MAX-buffer->length)return false;
    if(buffer->length+count>buffer->capacity) {
        size_t capacity=buffer->capacity==0?256:buffer->capacity;
        while(capacity<buffer->length+count) {
            if(capacity>SIZE_MAX/2)return false;
            capacity*=2;
        }
        char *grown=realloc(buffer->data,capacity);
        if(grown==nullptr)return false;
        buffer->data=grown;buffer->capacity=capacity;
    }
    memcpy(buffer->data+buffer->length,bytes,count);
    buffer->length+=count;
    return true;
}

/* Expands a String#sub/String#gsub replacement into `out`, honoring Ruby's
 * backslash escapes: `\0`/`\&` is the whole match, `\1`-`\9` is that capture
 * group (empty if the group didn't participate, e.g. an unmatched `(x)?`),
 * `\\` is a literal backslash, and a backslash before anything else (or a
 * group number past the pattern's actual capture count) is dropped and the
 * following byte copied as-is -- no named (`\k<name>`) backreferences,
 * a scope cut nothing here exercises. */
static bool regexp_append_replacement(ByteBuffer *out,const DiamondString *subject,
        const DiamondString *replacement,const reginold_match *match) {
    size_t index=0;
    bool ok=true;
    while(ok&&index<replacement->length) {
        const char ch=replacement->chars[index];
        if(ch=='\\'&&index+1<replacement->length) {
            const char next=replacement->chars[index+1];
            if(next=='\\') {
                ok=byte_buffer_append(out,"\\",1);index+=2;continue;
            }
            if(next=='&'||next=='0') {
                const size_t begin=(size_t)match->overall.beg;
                const size_t end=(size_t)match->overall.end;
                ok=byte_buffer_append(out,subject->chars+begin,end-begin);
                index+=2;continue;
            }
            if(next>='1'&&next<='9') {
                const size_t group=(size_t)(next-'0');
                if(group<=match->capture_count) {
                    const reginold_span span=match->captures[group-1];
                    if(span.beg>=0&&span.end>=0)
                        ok=byte_buffer_append(out,subject->chars+span.beg,
                            (size_t)(span.end-span.beg));
                }
                index+=2;continue;
            }
            index++;continue;
        }
        ok=byte_buffer_append(out,&ch,1);index++;
    }
    return ok;
}

/* String#sub/String#gsub's shared body (replace_all toggles first-only vs
 * every match). Zero-length matches (a pattern that can match an empty
 * string, e.g. an empty pattern or "x zero-or-more-times") copy one
 * source byte forward after
 * inserting the replacement, the same way Ruby's own gsub avoids looping
 * forever on one -- without that, `search_status` would report the exact
 * same empty match at the exact same offset indefinitely. */
static DiamondVmStatus regexp_replace_helper(DiamondVm *vm,const DiamondRegexp *regexp,
        const DiamondString *subject,const DiamondString *replacement,
        bool replace_all,DiamondValue *result) {
    ByteBuffer output={0};
    size_t cursor=0;
    bool ok=true;
    while(ok&&cursor<=subject->length) {
        reginold_match match_result={0};
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,cursor,&match_result);
        if(search_status==REGINOLD_ERROR) {
            free(output.data);
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        if(search_status==REGINOLD_MISMATCH)break;
        const size_t match_begin=(size_t)match_result.overall.beg;
        const size_t match_end=(size_t)match_result.overall.end;
        ok=byte_buffer_append(&output,subject->chars+cursor,match_begin-cursor)&&
           regexp_append_replacement(&output,subject,replacement,&match_result);
        reginold_match_free(&match_result);
        if(match_end==match_begin) {
            if(match_end<subject->length)
                ok=ok&&byte_buffer_append(&output,subject->chars+match_end,1);
            cursor=match_end+1;
        } else {
            cursor=match_end;
        }
        if(!replace_all)break;
    }
    if(ok&&cursor<subject->length)
        ok=byte_buffer_append(&output,subject->chars+cursor,subject->length-cursor);
    if(!ok) {free(output.data);return DIAMOND_VM_OUT_OF_MEMORY;}
    DiamondString *replaced=allocate_string(vm,
        output.data!=nullptr?output.data:"",output.length);
    free(output.data);
    if(replaced==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *result=DIAMOND_OBJECT(replaced);
    return DIAMOND_VM_OK;
}

/* String#scan: every non-overlapping match, leftmost to rightmost, same
 * zero-length-match advance as regexp_replace_helper above. Each entry is
 * the whole matched String if the pattern has no capture groups, or an
 * Array of the capture groups (Nil for an unmatched optional group,
 * matching Regexp#match's own convention) if it does -- mirroring Ruby's
 * own #scan exactly. */
/* Root through registers[dest] directly, not an out-param -- a real bug
 * found while investigating an unrelated crash (a self-referential-
 * looking array from String#scan, reproduced with DIAMOND_STRESS_GC=1
 * even on code well before this session's own changes). The previous
 * shape wrote the result array into *result, a plain DiamondValue
 * sitting in the *caller's* C stack frame -- never a real GC root, so
 * every allocation after the first (each whole-match/capture-group
 * String, each per-match capture Array) risked a GC pass collecting the
 * result array, or an already-built capture Array, out from under this
 * function while it was still building it. The inner capture-group loop
 * had the same bug twice over: `groups`, a bare malloc'd C array, held
 * DiamondValues with zero GC visibility at all between allocating one
 * group String and the next. Fixed by rooting `matches` immediately via
 * the caller's own dest register (the same register the caller was
 * always going to assign it to anyway, just done at the start instead
 * of the end) and pushing each capture Array into it -- and each group
 * String into that capture Array -- the instant it exists, so nothing
 * is ever unreachable between one allocation and the next. */
static DiamondVmStatus regexp_scan_helper(DiamondVm *vm,const DiamondRegexp *regexp,
        const DiamondString *subject,DiamondValue *registers,uint16_t dest) {
    DiamondArray *matches=allocate_array(vm,nullptr,0);
    if(matches==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(matches);
    size_t cursor=0;
    while(cursor<=subject->length) {
        reginold_match match_result={0};
        const reginold_status search_status=reginold_search(regexp->handle,
            subject->chars,subject->length,cursor,&match_result);
        if(search_status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        if(search_status==REGINOLD_MISMATCH)break;
        const size_t match_begin=(size_t)match_result.overall.beg;
        const size_t match_end=(size_t)match_result.overall.end;
        if(match_result.capture_count==0) {
            DiamondString *whole=allocate_string(vm,
                subject->chars+match_begin,match_end-match_begin);
            if(whole==nullptr) {reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;}
            if(!array_push(vm,matches,DIAMOND_OBJECT(whole))) {
                reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
            }
        } else {
            DiamondArray *group_array=allocate_array(vm,nullptr,0);
            if(group_array==nullptr) {reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;}
            /* Pushed into the already-rooted `matches` before it has any
             * elements of its own, so it (and everything pushed into it
             * below) stays reachable transitively through matches for
             * the rest of this match's construction. */
            if(!array_push(vm,matches,DIAMOND_OBJECT(group_array))) {
                reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
            }
            for(size_t index=0;index<match_result.capture_count;index++) {
                const reginold_span span=match_result.captures[index];
                DiamondValue group_value=DIAMOND_NIL;
                if(span.beg>=0&&span.end>=0) {
                    DiamondString *group_string=allocate_string(vm,
                        subject->chars+span.beg,(size_t)(span.end-span.beg));
                    if(group_string==nullptr) {
                        reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
                    }
                    group_value=DIAMOND_OBJECT(group_string);
                }
                if(!array_push(vm,group_array,group_value)) {
                    reginold_match_free(&match_result);return DIAMOND_VM_OUT_OF_MEMORY;
                }
            }
        }
        reginold_match_free(&match_result);
        cursor=match_end==match_begin?match_end+1:match_end;
    }
    return DIAMOND_VM_OK;
}

/* String#tr's from/to specs: c1-c2 ranges and, for the from-spec only, a
 * leading ^ that negates the set. A backslash escapes the very next byte
 * (so \\, \^, and \- can appear as literal data instead of triggering
 * their special meaning) -- the same convention the caller already uses
 * to build both specs from ordinary Diamond string literals. `negate` is
 * only ever set true when interpret_negation is true and the spec is at
 * least two bytes starting with an unescaped '^'; the to-spec call passes
 * interpret_negation=false so a leading '^' there is just a literal byte,
 * matching Ruby's own tr. */
static DiamondVmStatus tr_expand_spec(const DiamondString *spec,
        bool interpret_negation,bool *negate,ByteBuffer *out) {
    size_t start=0;
    *negate=false;
    if(interpret_negation&&spec->length>1&&spec->chars[0]=='^') {
        *negate=true;start=1;
    }
    const size_t remaining=spec->length-start;
    char *literal=remaining>0?malloc(remaining):nullptr;
    bool *escaped=remaining>0?malloc(remaining):nullptr;
    if(remaining>0&&(literal==nullptr||escaped==nullptr)) {
        free(literal);free(escaped);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    size_t count=0;
    for(size_t index=start;index<spec->length;index++) {
        char ch=spec->chars[index];
        bool is_escaped=false;
        if(ch=='\\'&&index+1<spec->length) {
            index++;ch=spec->chars[index];is_escaped=true;
        }
        literal[count]=ch;escaped[count]=is_escaped;count++;
    }
    bool ok=true;
    size_t index=0;
    while(index<count&&ok) {
        if(!escaped[index]&&index+2<count&&
           !escaped[index+1]&&literal[index+1]=='-') {
            const unsigned char low=(unsigned char)literal[index];
            const unsigned char high=(unsigned char)literal[index+2];
            if(low<=high) {
                for(unsigned int code=low;code<=high&&ok;code++) {
                    const char byte=(char)(unsigned char)code;
                    ok=byte_buffer_append(out,&byte,1);
                }
                index+=3;continue;
            }
        }
        ok=byte_buffer_append(out,&literal[index],1);
        index++;
    }
    free(literal);free(escaped);
    if(!ok) {
        free(out->data);out->data=nullptr;out->length=0;out->capacity=0;
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    return DIAMOND_VM_OK;
}

/* Pushes `value` onto vm->gc_protected so diamond_vm_collect_impl's mark
 * phase treats it as a root, for a value under construction with no real
 * register to live in yet (see DiamondVm.gc_protected's own comment,
 * vm.h). Returns false on allocation failure, same convention as
 * array_push/hash_set, so callers can fold it into their own existing
 * failure path. Grows geometrically like array_push's own backing store;
 * this stack is expected to stay small (depth tracks the nesting depth of
 * whatever value is being copied, not its total element count, since
 * siblings are unprotected again before the next one is pushed -- see
 * copy_value_into_vm's call sites). */
static bool gc_protect(DiamondVm *vm, DiamondValue value) {
    if(vm->gc_protected_count>=vm->gc_protected_capacity) {
        const size_t capacity=
            vm->gc_protected_capacity==0?8:vm->gc_protected_capacity*2;
        DiamondValue *grown=
            realloc(vm->gc_protected,capacity*sizeof(DiamondValue));
        if(grown==nullptr)return false;
        vm->gc_protected=grown;vm->gc_protected_capacity=capacity;
    }
    vm->gc_protected[vm->gc_protected_count++]=value;
    return true;
}

/* Unwinds vm->gc_protected back to a mark saved from gc_protected_count
 * before a matching run of gc_protect calls -- strict LIFO, mirroring the
 * nested lifetime of copy_value_into_vm's own recursion. Never shrinks
 * the backing allocation, same as array/hash never shrinking on removal;
 * it's freed for real in diamond_vm_free. */
static void gc_unprotect(DiamondVm *vm, size_t saved_count) {
    vm->gc_protected_count=saved_count;
}

/* Deep-copies a DiamondValue rooted in some other VM's heap (typically
 * program_builder_run_helper's temporary run_vm, about to be freed) into
 * dest_vm's own heap, so the result stays valid once the source VM is
 * gone. Object kinds that wrap live VM/OS state rather than plain data
 * (Closure, Fiber, File, Listener, Regexp, ProgramBuilder) aren't safe to
 * hand across this boundary at all -- those return false and the caller
 * reports a TypeError. Composite kinds (Array, Hash, ...) recurse into
 * this same function per element, and deliberately drop the source's own
 * generic constraint metadata -- `constraints[]` holds pointers into the
 * *source* type-set/class tables, whose lifetime isn't tracked across
 * this boundary, so a copy comes back a plain, unconstrained collection
 * rather than trying to carry that metadata across intact.
 *
 * Instance is the one kind that isn't a plain value copy: its ->class
 * points into `source_program`'s own classes[] (superclass chain and
 * method function_index are indices into that *same* program, not
 * portable numbers), so the class itself can't simply be copied without
 * also copying every function/class it can reach -- effectively
 * relinking a whole program. Instead, on the first Instance actually
 * encountered during this copy, `source_program` is adopted into dest_vm
 * (see DiamondVm.adopted_programs, src/vm.h) so it survives for the rest
 * of dest_vm's lifetime, and every copied instance's `owner` field
 * (DiamondInstance, src/object.h) points at the adopted program's own
 * chunk -- dispatch sites read a receiver's own `owner` instead of
 * trusting the ambient chunk specifically so this works (see
 * invoke_operator_method/stringify_value/DIAMOND_OP_INVOKE above).
 * `*adopted_owner` caches that adoption across the whole recursive copy
 * (and across sibling instances in the same Array/Hash) so a single
 * result containing several instances only adopts `source_program` once,
 * not once per instance. Left nullptr (no adoption) for a result that
 * turns out not to contain any Instance at all -- adopting unconditionally
 * would leak the adopted program and all dynamically owned functions on
 * every ProgramBuilder#run call regardless of what it actually returned.
 *
 * `rebase_source_classes`/`rebase_dest_classes` are the alternative to
 * adoption, used by Thread (see docs/threads.md) instead of ProgramBuilder:
 * Thread.new clones the classes/functions/interfaces tables of whatever
 * program is ambient at spawn time (safe -- those tables are themselves
 * pointer-free) for the new thread to run against, rather than adopting a
 * genuinely foreign program. Because the clone's classes[] table has
 * byte-identical layout to the source table, an Instance's `->class`
 * pointer can simply be *rebased* by array-offset arithmetic --
 * `&rebase_dest_classes[source->class-rebase_source_classes]` -- with no
 * adoption and no DiamondVm.adopted_programs involvement; `copy->owner`
 * stays nullptr (the destination's own ambient chunk is already built from
 * the same cloned tables, so the ordinary owner-is-nullptr dispatch
 * fallback already resolves correctly). Both sides are plain `const
 * DiamondClass *` base pointers -- deliberately not `DiamondProgram *`,
 * since at both Thread.new (copying args into a freshly cloned program)
 * and .join() (copying the result back out) exactly one side of the copy
 * is a real, owned DiamondProgram and the other is only ever reachable as
 * a DiamondChunk view's own `.classes` pointer, never as a whole owned
 * program. Rebase mode is active whenever `rebase_dest_classes!=nullptr`;
 * passing it alongside a meaningfully-used `*adopted_owner` is not a
 * supported combination -- exactly one caller mode applies per call. */
static bool copy_value_into_vm(DiamondVm *dest_vm, DiamondValue value,
                               DiamondProgram *source_program,
                               const DiamondClass *rebase_source_classes,
                               const DiamondClass *rebase_dest_classes,
                               const DiamondChunk **adopted_owner,
                               DiamondValue *out) {
    if(value.kind!=DIAMOND_VALUE_OBJECT) {*out=value;return true;}
    switch(value.as.object->kind) {
        case DIAMOND_OBJECT_STRING: {
            const DiamondString *source=(const DiamondString *)value.as.object;
            DiamondString *copy=allocate_string(dest_vm,source->chars,source->length);
            if(copy==nullptr)return false;
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_SYMBOL: {
            const DiamondSymbol *source=(const DiamondSymbol *)value.as.object;
            DiamondSymbol *copy=allocate_symbol(dest_vm,source->chars,source->length);
            if(copy==nullptr)return false;
            *out=DIAMOND_OBJECT(copy);return true;
        }
        /* Array/Hash: the old implementation staged copied elements in a
         * bare malloc'd buffer (Array) or plain C locals (Hash) before
         * they had any GC root, so a collection triggered by copying one
         * element could free a sibling already copied moments earlier --
         * the same bug this session already found and fixed in
         * regexp_scan_helper/regexp_match_helper, just unreachable there
         * (both had a real destination register to root through
         * immediately). Fixed the same way: allocate the empty container
         * first, protect *it* via gc_protect (there's no destination
         * register here, unlike the regexp helpers), then populate it
         * incrementally via array_push/hash_set so each already-copied
         * element becomes reachable through the now-rooted container
         * before the next one is computed. array_push/hash_set only ever
         * grow their own backing storage via plain realloc, never trigger
         * a GC pass themselves, so there's no unrooted window between a
         * recursive copy_value_into_vm call returning and its result
         * being pushed. */
        case DIAMOND_OBJECT_ARRAY: {
            const DiamondArray *source=(const DiamondArray *)value.as.object;
            DiamondArray *copy=allocate_array(dest_vm,nullptr,0);
            if(copy==nullptr)return false;
            const size_t mark=dest_vm->gc_protected_count;
            if(!gc_protect(dest_vm,DIAMOND_OBJECT(copy)))return false;
            for(size_t index=0;index<source->count;index++) {
                DiamondValue element=DIAMOND_NIL;
                if(!copy_value_into_vm(dest_vm,source->values[index],
                                       source_program,rebase_source_classes,rebase_dest_classes,
                                       adopted_owner,&element)||
                   !array_push(dest_vm,copy,element)) {
                    gc_unprotect(dest_vm,mark);return false;
                }
            }
            gc_unprotect(dest_vm,mark);
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_HASH: {
            const DiamondHash *source=(const DiamondHash *)value.as.object;
            DiamondHash *copy=allocate_hash(dest_vm);
            if(copy==nullptr)return false;
            const size_t mark=dest_vm->gc_protected_count;
            if(!gc_protect(dest_vm,DIAMOND_OBJECT(copy)))return false;
            for(size_t index=0;index<source->count;index++) {
                DiamondValue key=DIAMOND_NIL,copied_value=DIAMOND_NIL;
                if(!copy_value_into_vm(dest_vm,source->entries[index].key,
                                       source_program,rebase_source_classes,rebase_dest_classes,
                                       adopted_owner,&key)) {
                    gc_unprotect(dest_vm,mark);return false;
                }
                /* `key` needs its own protection window: it's not
                 * reachable through `copy` yet (hash_set hasn't run), and
                 * copying the value below can itself trigger a GC. */
                const size_t key_mark=dest_vm->gc_protected_count;
                if(!gc_protect(dest_vm,key)) {
                    gc_unprotect(dest_vm,mark);return false;
                }
                if(!copy_value_into_vm(dest_vm,source->entries[index].value,
                                       source_program,rebase_source_classes,rebase_dest_classes,
                                       adopted_owner,&copied_value)) {
                    gc_unprotect(dest_vm,mark);return false;
                }
                gc_unprotect(dest_vm,key_mark);
                if(!hash_set(dest_vm,copy,key,copied_value)) {
                    gc_unprotect(dest_vm,mark);return false;
                }
            }
            gc_unprotect(dest_vm,mark);
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_INSTANCE: {
            const DiamondInstance *source=(const DiamondInstance *)value.as.object;
            DiamondInstance *copy;
            if(rebase_dest_classes!=nullptr) {
                const size_t offset=
                    (size_t)(source->class-rebase_source_classes);
                copy=allocate_instance(dest_vm,
                    &rebase_dest_classes[offset],nullptr);
            } else {
                if(*adopted_owner==nullptr) {
                    *adopted_owner=adopt_program(dest_vm,source_program);
                    if(*adopted_owner==nullptr)return false;
                }
                copy=allocate_instance(dest_vm,source->class,*adopted_owner);
            }
            if(copy==nullptr)return false;
            /* Same unrooted-window bug as Array/Hash above, fixed the
             * same way -- `copy` itself is the container to protect;
             * writing straight into copy->fields[index] already puts each
             * field where mark_object's DIAMOND_OBJECT_INSTANCE case will
             * find it, once `copy` is a root. */
            const size_t mark=dest_vm->gc_protected_count;
            if(!gc_protect(dest_vm,DIAMOND_OBJECT(copy)))return false;
            for(size_t index=0;index<source->field_count;index++) {
                if(!copy_value_into_vm(dest_vm,source->fields[index],
                                       source_program,rebase_source_classes,rebase_dest_classes,
                                       adopted_owner,&copy->fields[index])) {
                    gc_unprotect(dest_vm,mark);return false;
                }
            }
            /* allocate_instance always starts a fresh instance at
             * class->shapes[0] (nothing "materialized" yet, in the
             * gradual-field-initialization sense GET_IVAR's own inline
             * cache relies on -- see lookup_field_cached/DIAMOND_OP_
             * GET_IVAR) since it has no way to know how many fields this
             * particular caller is about to fill in. Writing `fields[]`
             * directly above (never going through the real SET_IVAR
             * opcode, which is what normally advances an instance's shape
             * one field at a time) leaves that shape stuck at 0 -- every
             * field would read back as a cache-materialized nil despite
             * genuinely holding a copied value, exactly the same class of
             * bug this session's DiamondInstance.owner fix targeted, just
             * one field over. Fixed by rebasing `source`'s own shape the
             * same way its class was rebased/adopted just above: shapes[]
             * is a fixed-size array *inside* DiamondClass (not separately
             * allocated), so the shape index within it is portable the
             * same way a class index is. */
            copy->shape=&copy->class->shapes[
                (size_t)(source->shape-source->class->shapes)];
            gc_unprotect(dest_vm,mark);
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_BIGNUM: {
            const DiamondBignum *source=(const DiamondBignum *)value.as.object;
            if(dest_vm->stress_gc||dest_vm->bytes_allocated>=dest_vm->next_gc)
                diamond_vm_collect(dest_vm);
            const size_t size=
                sizeof(DiamondBignum)+source->limb_count*sizeof(uint32_t);
            DiamondBignum *copy=malloc(size);
            if(copy==nullptr)return false;
            copy->object=(DiamondObject){.next=dest_vm->objects,
                .kind=DIAMOND_OBJECT_BIGNUM};
            copy->negative=source->negative;copy->limb_count=source->limb_count;
            memcpy(copy->limbs,source->limbs,source->limb_count*sizeof(uint32_t));
            dest_vm->objects=&copy->object;dest_vm->bytes_allocated+=size;
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_CLOSURE: {
            /* Only a *zero-capture* closure, and only in Thread's rebase
             * mode (rebase_dest_classes!=nullptr) -- a capturing closure
             * still holds live Cell/GC state tied to one specific heap,
             * categorically forbidden here exactly as Thread.new's own
             * primary-callable check already requires. In adopt mode
             * (ProgramBuilder#run) the source and destination programs are
             * genuinely different, so a bare function_index wouldn't mean
             * the same function in both -- unlike Thread, whose
             * child_program is a byte-for-byte clone of the whole
             * functions[] table (see clone_program_from_chunk), so a valid
             * index in one is the same function in the other with no
             * offset arithmetic needed at all (unlike the Instance case
             * just above, which rebases a pointer). See docs/threads.md. */
            const DiamondClosure *source=(const DiamondClosure *)value.as.object;
            if(source->capture_count!=0||rebase_dest_classes==nullptr)return false;
            DiamondClosure *copy=
                allocate_closure(dest_vm,source->function_index,nullptr,0);
            if(copy==nullptr)return false;
            *out=DIAMOND_OBJECT(copy);return true;
        }
        default: return false;
    }
}

/* ProgramBuilder#run's real body, factored out of run_chunk's own opcode
 * switch for the same stack-frame-isolation reason as regexp_new_helper
 * above -- but far more load-bearing here: a bare local DiamondVm is
 * ~50KB (method/field caches, rewritten_sites[DIAMOND_MAX_CODE], opcode
 * counters, namespace constants), not the ~100 bytes regexp_new_helper's
 * own locals needed. At -O0, every local anywhere in run_chunk's switch
 * contributes to its one shared stack frame regardless of which case
 * actually runs, so leaving `DiamondVm run_vm` inline in the INVOKE case
 * body would have added that ~50KB to *every* recursive run_chunk level
 * unconditionally -- confirmed by a real crash: depth(5000) (the existing
 * regression test for the DIAMOND_MAX_CALL_DEPTH guard) segfaulted before
 * that guard could trip, a worse version of the exact bug the Regexp
 * round already found and fixed this way (see docs/roadmap.md). Returns
 * DIAMOND_VM_PROGRAM_ERROR (not the constructed program's own status) for
 * a nonzero exit. A heap-object result is deep-copied into the caller's
 * own vm via copy_value_into_vm before run_vm is freed; kinds that
 * function can't safely copy (see its own comment) still report
 * DIAMOND_VM_TYPE_ERROR, a narrower version of this helper's original
 * scalar-only restriction. Takes `builder` itself (not just its
 * ->program) so that if the result actually contains an Instance,
 * ownership of `built` can be transferred into vm's adopted_programs
 * list (copy_value_into_vm's own adopt_program call) -- builder->program
 * is set to nullptr in that case so DIAMOND_OBJECT_PROGRAM_BUILDER's own
 * GC destructor (see diamond_vm_free) no longer frees it out from under
 * the copied instance still using it. */
static DiamondVmStatus program_builder_run_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, DiamondValue *result) {
    DiamondProgram *built=builder->program;
    const DiamondChunk built_chunk=diamond_program_chunk(built);
    /* #emit_byte/#patch_byte let Diamond code append raw bytes to a
     * function's code array with no idea what instruction it's building
     * -- unlike ordinary compiled bytecode, nothing here guarantees a
     * register operand stays within the function's own declared register
     * count, or that a jump target lands on a real instruction rather
     * than the middle of one. run_chunk (below, via diamond_vm_run) trusts
     * every register operand it reads with no bounds check of its own, so
     * an out-of-range one is a real out-of-bounds read/write on whatever
     * backs the callee's register array (the C stack for an ordinary-
     * sized function, a heap allocation past DIAMOND_INLINE_REGISTER_
     * COUNT) -- reachable from plain Diamond source, no native embedding
     * required. Reject it before it ever reaches run_chunk. */
    if(!diamond_verify_bytecode(&built_chunk)) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "run: constructed bytecode is invalid");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondVm run_vm;diamond_vm_init(&run_vm);
    DiamondValue run_result=DIAMOND_NIL;
    const DiamondVmStatus run_status=diamond_vm_run(&run_vm,&built_chunk,&run_result);
    if(run_status!=DIAMOND_VM_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",
            run_vm.error[0]!='\0'?run_vm.error:diamond_vm_status_name(run_status));
        diamond_vm_free(&run_vm);
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondValue copied_result=DIAMOND_NIL;
    const DiamondChunk *adopted_owner=nullptr;
    if(!copy_value_into_vm(vm,run_result,built,nullptr,nullptr,&adopted_owner,&copied_result)) {
        diamond_vm_free(&run_vm);
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
            "run does not support this result type");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(adopted_owner!=nullptr)builder->program=nullptr;
    diamond_vm_free(&run_vm);
    *result=copied_result;
    return DIAMOND_VM_OK;
}

/* All six ProgramBuilder instance methods, factored out of run_chunk's own
 * INVOKE case for the same stack-frame-isolation reason as
 * program_builder_run_helper above -- not because any *one* branch here has
 * a large local (they don't), but because at -O0 every local anywhere in
 * run_chunk's switch, across *every* branch, contributes to its one shared
 * stack frame regardless of which branch actually runs. Six method-name
 * flags plus each branch's own few pointers/integers added up to enough
 * that depth(5000) -- the existing regression test for the
 * DIAMOND_MAX_CALL_DEPTH guard -- segfaulted before that guard could trip,
 * even after program_builder_run_helper's extraction alone (confirmed by
 * testing that fix in isolation first). Takes `registers`/`base`/`argc`
 * directly rather than pre-extracted arguments, unlike
 * regexp_new_helper/regexp_match_helper, since six methods with different
 * arities would otherwise need six different call signatures. */
static DiamondVmStatus program_builder_invoke_helper(DiamondVm *vm,
        DiamondProgramBuilder *builder, const DiamondStringConstant *method_name,
        DiamondValue *registers, uint16_t base, uint8_t argc, size_t depth,
        DiamondValue *result) {
    DiamondProgram *built=builder->program;
    const bool declare_function_method=
        method_name->length==sizeof("declare_function")-1&&
        memcmp(method_name->chars,"declare_function",
            sizeof("declare_function")-1)==0;
    const bool emit_byte_method=
        method_name->length==sizeof("emit_byte")-1&&
        memcmp(method_name->chars,"emit_byte",sizeof("emit_byte")-1)==0;
    /* Needed for jump backpatching: compiler.c's own patch_jump (see
     * src/compiler.c) directly overwrites function->code[operand] after
     * the fact, once a forward jump's real target is known -- a single-pass
     * emitter can't know a forward target's offset before emitting the
     * jump itself. Phase 3's Diamond-language parser needs the same
     * capability for if/while/loop, so this mirrors patch_jump exactly
     * (overwrite an already-emitted byte, never append). See
     * docs/roadmap.md's self-hosting Phase 3 entry. */
    const bool patch_byte_method=
        method_name->length==sizeof("patch_byte")-1&&
        memcmp(method_name->chars,"patch_byte",sizeof("patch_byte")-1)==0;
    const bool add_constant_method=
        method_name->length==sizeof("add_constant")-1&&
        memcmp(method_name->chars,"add_constant",
            sizeof("add_constant")-1)==0;
    const bool add_string_method=
        method_name->length==sizeof("add_string")-1&&
        memcmp(method_name->chars,"add_string",sizeof("add_string")-1)==0;
    const bool set_register_count_method=
        method_name->length==sizeof("set_register_count")-1&&
        memcmp(method_name->chars,"set_register_count",
            sizeof("set_register_count")-1)==0;
    /* Phase 3 sub-phase 3 (classes): declare_class/declare_field/
     * declare_method mirror compile_class/field_index/compile_definition's
     * own class-registration side effects in compiler.c -- fields not
     * needed by any sub-phase before this one (Phase 1's own design note
     * flagged them as deferred until class-compiling logic actually
     * needed them). See docs/roadmap.md. */
    const bool declare_class_method=
        method_name->length==sizeof("declare_class")-1&&
        memcmp(method_name->chars,"declare_class",sizeof("declare_class")-1)==0;
    const bool declare_module_method=
        method_name->length==sizeof("declare_module")-1&&
        memcmp(method_name->chars,"declare_module",sizeof("declare_module")-1)==0;
    const bool declare_namespace_constant_method=
        method_name->length==sizeof("declare_namespace_constant")-1&&
        memcmp(method_name->chars,"declare_namespace_constant",
            sizeof("declare_namespace_constant")-1)==0;
    const bool declare_field_method=
        method_name->length==sizeof("declare_field")-1&&
        memcmp(method_name->chars,"declare_field",sizeof("declare_field")-1)==0;
    const bool declare_module_field_method=
        method_name->length==sizeof("declare_module_field")-1&&
        memcmp(method_name->chars,"declare_module_field",
            sizeof("declare_module_field")-1)==0;
    const bool declare_method_method=
        method_name->length==sizeof("declare_method")-1&&
        memcmp(method_name->chars,"declare_method",sizeof("declare_method")-1)==0;
    const bool declare_module_method_method=
        method_name->length==sizeof("declare_module_method")-1&&
        memcmp(method_name->chars,"declare_module_method",
            sizeof("declare_module_method")-1)==0;
    /* Declares a function directly as a class/module singleton method
     * (`def self.foo` syntax) -- distinct from export_module_method,
     * which instead re-exports an *already-declared* regular method
     * (the `module_function :name` syntax). Neither has a needs_receiver
     * counterpart here: diamond_compile's own module_singleton branch
     * (src/compiler.c's compile_definition) never sets it either, since
     * a directly-declared singleton never reserves register 0 for an
     * implicit self the way an ordinary method does. */
    const bool declare_class_singleton_method_method=
        method_name->length==sizeof("declare_class_singleton_method")-1&&
        memcmp(method_name->chars,"declare_class_singleton_method",
            sizeof("declare_class_singleton_method")-1)==0;
    const bool declare_module_singleton_method_method=
        method_name->length==sizeof("declare_module_singleton_method")-1&&
        memcmp(method_name->chars,"declare_module_singleton_method",
            sizeof("declare_module_singleton_method")-1)==0;
    /* A `def` nested directly inside a method body (never registered as
     * a named class method itself -- it stays a plain Closure value,
     * only ever installed via redefine_method) still needs owner_class
     * set so REDEFINE_METHOD's "callable must be a method of X" check
     * (which compares owner_class against the target class operand)
     * accepts it. declare_method/declare_module_method set this as a
     * side effect of registering a *named* method; this is the same
     * fix for a function that's deliberately never named. Mirrors
     * diamond_compile's own compile_definition, which sets
     * function->owner_class from current_class/current_module
     * unconditionally, independent of at_top_level. */
    const bool set_function_owner_class_method=
        method_name->length==sizeof("set_function_owner_class")-1&&
        memcmp(method_name->chars,"set_function_owner_class",
            sizeof("set_function_owner_class")-1)==0;
    const bool include_module_method=
        method_name->length==sizeof("include_module")-1&&
        memcmp(method_name->chars,"include_module",sizeof("include_module")-1)==0;
    const bool include_module_in_module_method=
        method_name->length==sizeof("include_module_in_module")-1&&
        memcmp(method_name->chars,"include_module_in_module",
            sizeof("include_module_in_module")-1)==0;
    const bool set_module_method_visibility_method=
        method_name->length==sizeof("set_module_method_visibility")-1&&
        memcmp(method_name->chars,"set_module_method_visibility",
            sizeof("set_module_method_visibility")-1)==0;
    /* Named `private`/`public` visibility targets (`private foo, bar`)
     * for a class -- the class-side counterpart to
     * set_module_method_visibility, retroactively flipping an
     * already-declared method's is_private flag by name. */
    const bool set_class_method_visibility_method=
        method_name->length==sizeof("set_class_method_visibility")-1&&
        memcmp(method_name->chars,"set_class_method_visibility",
            sizeof("set_class_method_visibility")-1)==0;
    /* `alias_method new_name, existing_name` (compiler.c's
     * compile_alias_method): copies an already-declared method's
     * DiamondMethod struct verbatim under a new name -- same
     * function_index/arity/required_arity/is_private, just re-registered
     * so a second name can call the identical implementation. The
     * caller (selfhost/parser.di) is expected to have already appended
     * "=" to either name string when aliasing a writer method, the same
     * way declare_method's own name argument already does. */
    const bool alias_class_method_method=
        method_name->length==sizeof("alias_class_method")-1&&
        memcmp(method_name->chars,"alias_class_method",
            sizeof("alias_class_method")-1)==0;
    const bool alias_module_method_method=
        method_name->length==sizeof("alias_module_method")-1&&
        memcmp(method_name->chars,"alias_module_method",
            sizeof("alias_module_method")-1)==0;
    const bool export_module_method_method=
        method_name->length==sizeof("export_module_method")-1&&
        memcmp(method_name->chars,"export_module_method",
            sizeof("export_module_method")-1)==0;
    const bool expand_source_method=
        method_name->length==sizeof("expand_source")-1&&
        memcmp(method_name->chars,"expand_source",sizeof("expand_source")-1)==0;
    const bool source_location_method=
        method_name->length==sizeof("source_location")-1&&
        memcmp(method_name->chars,"source_location",sizeof("source_location")-1)==0;
    const bool set_source_location_method=
        method_name->length==sizeof("set_source_location")-1&&
        memcmp(method_name->chars,"set_source_location",
            sizeof("set_source_location")-1)==0;
    /* Phase 3 sub-phase 4 (gradual typing): a scalar or union type set.
     * Nested Array[T]/Hash[K,V]/Callable/interfaces/generics remain
     * separate future extensions. See docs/roadmap.md. */
    const bool declare_type_set_method=
        method_name->length==sizeof("declare_type_set")-1&&
        memcmp(method_name->chars,"declare_type_set",
            sizeof("declare_type_set")-1)==0;
    const bool set_parameter_type_method=
        method_name->length==sizeof("set_parameter_type")-1&&
        memcmp(method_name->chars,"set_parameter_type",
            sizeof("set_parameter_type")-1)==0;
    const bool set_return_type_method=
        method_name->length==sizeof("set_return_type")-1&&
        memcmp(method_name->chars,"set_return_type",
            sizeof("set_return_type")-1)==0;
    const bool declare_interface_method=
        method_name->length==sizeof("declare_interface")-1&&
        memcmp(method_name->chars,"declare_interface",
            sizeof("declare_interface")-1)==0;
    const bool declare_interface_method_method=
        method_name->length==sizeof("declare_interface_method")-1&&
        memcmp(method_name->chars,"declare_interface_method",
            sizeof("declare_interface_method")-1)==0;
    const bool set_type_variables_method=
        method_name->length==sizeof("set_type_variables")-1&&
        memcmp(method_name->chars,"set_type_variables",
            sizeof("set_type_variables")-1)==0;
    const bool inherit_interface_method=
        method_name->length==sizeof("inherit_interface")-1&&
        memcmp(method_name->chars,"inherit_interface",
            sizeof("inherit_interface")-1)==0;
    const bool run_method=method_name->length==sizeof("run")-1&&
        memcmp(method_name->chars,"run",sizeof("run")-1)==0;
    if(!declare_function_method&&!emit_byte_method&&!patch_byte_method&&
       !add_constant_method&&!add_string_method&&
       !set_register_count_method&&!declare_class_method&&
       !declare_module_method&&!declare_namespace_constant_method&&
       !declare_field_method&&!declare_module_field_method&&!declare_method_method&&
       !declare_module_method_method&&
       !declare_class_singleton_method_method&&
       !declare_module_singleton_method_method&&!set_function_owner_class_method&&
       !include_module_method&&
       !include_module_in_module_method&&
       !set_module_method_visibility_method&&!set_class_method_visibility_method&&
       !alias_class_method_method&&!alias_module_method_method&&
       !export_module_method_method&&
       !expand_source_method&&!source_location_method&&!set_source_location_method&&
       !declare_type_set_method&&!set_parameter_type_method&&
       !set_return_type_method&&!declare_interface_method&&
       !declare_interface_method_method&&!set_type_variables_method&&
       !inherit_interface_method&&!run_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"ProgramBuilder");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(declare_function_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_function arguments must be (String, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *fname=
            (const DiamondString *)registers[base].as.object;
        const int64_t arity_value=registers[(size_t)base+1].as.integer;
        const int64_t required_value=registers[(size_t)base+2].as.integer;
        if(fname->length==0||fname->length>=DIAMOND_MAX_FUNCTION_NAME||
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_function has an invalid name or arity");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(built->function_count==DIAMOND_MAX_FUNCTIONS) {
            snprintf(vm->error,sizeof vm->error,"program has too many functions");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *function=diamond_program_add_function(built);
        if(function==nullptr) {
            snprintf(vm->error,sizeof vm->error,"program function allocation failed");
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        vm->bytes_allocated+=sizeof *function+sizeof function;
        *function=(DiamondFunction){};
        memcpy(function->name,fname->chars,fname->length);
        function->name[fname->length]='\0';
        function->owner_class=UINT8_MAX;
        function->arity=(uint8_t)arity_value;
        function->required_arity=(uint8_t)required_value;
        function->return_type_set=UINT8_MAX;
        for(size_t index=0;index<16;index++)
            function->parameter_type_sets[index]=UINT8_MAX;
        const int64_t new_index=(int64_t)built->function_count-1;
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(emit_byte_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#emit_byte arguments must be (Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t byte_value=registers[(size_t)base+1].as.integer;
        if(target==nullptr||byte_value<0||byte_value>UINT8_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "emit_byte has an invalid function index or byte value");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(target->code_count==DIAMOND_MAX_CODE) {
            snprintf(vm->error,sizeof vm->error,
                "function produces too much bytecode");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->code[target->code_count]=(uint8_t)byte_value;
        target->lines[target->code_count]=builder->source_line;
        target->columns[target->code_count]=builder->source_column;
        target->code_count++;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(patch_byte_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#patch_byte arguments must be (Int, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t offset_value=registers[(size_t)base+1].as.integer;
        const int64_t byte_value=registers[(size_t)base+2].as.integer;
        if(target==nullptr||offset_value<0||
           (uint64_t)offset_value>=target->code_count||
           byte_value<0||byte_value>UINT8_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "patch_byte has an invalid function index, offset, or byte value");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->code[offset_value]=(uint8_t)byte_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(add_constant_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_constant's function index must be an Int");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondValue value=registers[(size_t)base+1];
        if(value.kind==DIAMOND_VALUE_OBJECT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "add_constant only accepts Int, Float, Bool, or Nil");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        if(target==nullptr) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_constant has an invalid function index");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(target->constant_count==DIAMOND_MAX_CONSTANTS) {
            snprintf(vm->error,sizeof vm->error,
                "function has too many constants");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)target->constant_count;
        target->constants[target->constant_count++]=value;
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(add_string_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#add_string arguments must be (Int, String)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const DiamondString *text=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(target==nullptr||text->length>DIAMOND_MAX_STRING_LENGTH) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "add_string has an invalid function index or an oversized string");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(target->string_count==DIAMOND_MAX_STRING_CONSTANTS) {
            snprintf(vm->error,sizeof vm->error,
                "function has too many string constants");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondStringConstant *slot=&target->strings[target->string_count];
        memcpy(slot->chars,text->chars,text->length);
        slot->chars[text->length]='\0';
        slot->length=text->length;
        const int64_t new_index=(int64_t)target->string_count;
        target->string_count++;
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(set_register_count_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#set_register_count arguments must be (Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t count_value=registers[(size_t)base+1].as.integer;
        if(target==nullptr||count_value<0||count_value>UINT16_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_register_count has an invalid function index or count");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->register_count=(uint16_t)count_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_class_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_class arguments must be (String, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *cname=
            (const DiamondString *)registers[base].as.object;
        const int64_t superclass_index=registers[(size_t)base+1].as.integer;
        DiamondClass *parent=nullptr;
        if(superclass_index!=-1) {
            parent=program_builder_class(built,superclass_index);
            if(parent==nullptr) {
                snprintf(vm->error,sizeof vm->error,
                    "ProgramBuilder#declare_class has an invalid superclass index");
                return DIAMOND_VM_TYPE_ERROR;
            }
        }
        if(cname->length==0||cname->length>=DIAMOND_MAX_FUNCTION_NAME) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_class has an invalid name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(built->class_count==DIAMOND_MAX_CLASSES) {
            snprintf(vm->error,sizeof vm->error,"program has too many classes");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)built->class_count;
        DiamondClass *class=&built->classes[built->class_count++];
        *class=(DiamondClass){};
        memcpy(class->name,cname->chars,cname->length);
        class->name[cname->length]='\0';
        class->superclass=parent==nullptr?UINT8_MAX:(uint8_t)superclass_index;
        if(parent!=nullptr) {
            class->field_count=parent->field_count;
            memcpy(class->fields,parent->fields,
                parent->field_count*DIAMOND_MAX_FUNCTION_NAME);
        }
        program_builder_recompute_shapes(class);
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(declare_module_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_module argument must be String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *name=(const DiamondString *)registers[base].as.object;
        if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
           built->module_count==DIAMOND_MAX_MODULES) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_module has an invalid name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<built->module_count;index++)
            if(strlen(built->modules[index].name)==name->length&&
               memcmp(built->modules[index].name,name->chars,name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"module name is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        const int64_t index=(int64_t)built->module_count;
        DiamondModule *module=&built->modules[built->module_count++];
        *module=(DiamondModule){};
        memcpy(module->name,name->chars,name->length);
        module->name[name->length]='\0';
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
    }
    if(declare_namespace_constant_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_namespace_constant argument must be String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *name=(const DiamondString *)registers[base].as.object;
        if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
           built->namespace_constant_count==DIAMOND_MAX_NAMESPACE_CONSTANTS) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_namespace_constant has an invalid name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<built->namespace_constant_count;index++)
            if(strlen(built->namespace_constants[index])==name->length&&
               memcmp(built->namespace_constants[index],name->chars,name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"constant is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        const int64_t index=(int64_t)built->namespace_constant_count;
        memcpy(built->namespace_constants[built->namespace_constant_count],
            name->chars,name->length);
        built->namespace_constants[built->namespace_constant_count][name->length]='\0';
        built->namespace_constant_count++;
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
    }
    if(declare_field_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_field arguments must be (Int, String)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondClass *class=
            program_builder_class(built,registers[base].as.integer);
        const DiamondString *fname=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(class==nullptr||fname->length==0||
           fname->length>=DIAMOND_MAX_FUNCTION_NAME) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_field has an invalid class index or field name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<class->field_count;index++)
            if(strlen(class->fields[index])==fname->length&&
               memcmp(class->fields[index],fname->chars,fname->length)==0) {
                *result=DIAMOND_INT((int64_t)index);return DIAMOND_VM_OK;
            }
        if(class->field_count==DIAMOND_MAX_FIELDS) {
            snprintf(vm->error,sizeof vm->error,"class has too many fields");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)class->field_count;
        memcpy(class->fields[class->field_count],fname->chars,fname->length);
        class->fields[class->field_count][fname->length]='\0';
        class->field_count++;
        program_builder_recompute_shapes(class);
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(declare_module_field_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t module_index=registers[base].as.integer;
        const DiamondString *name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(module_index<0||(uint64_t)module_index>=built->module_count||
           name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondModule *module=&built->modules[(size_t)module_index];
        for(size_t index=0;index<module->field_count;index++)
            if(strlen(module->fields[index])==name->length&&
               memcmp(module->fields[index],name->chars,name->length)==0) {
                *result=DIAMOND_INT((int64_t)index);return DIAMOND_VM_OK;
            }
        if(module->field_count==DIAMOND_MAX_FIELDS)return DIAMOND_VM_TYPE_ERROR;
        const int64_t index=(int64_t)module->field_count;
        memcpy(module->fields[module->field_count],name->chars,name->length);
        module->fields[module->field_count][name->length]='\0';
        module->field_count++;
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
    }
    if(declare_method_method) {
        if(argc!=6)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+5].kind!=DIAMOND_VALUE_BOOL) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_method arguments must be "
                "(Int, String, Int, Int, Int, Bool)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondClass *class=
            program_builder_class(built,registers[base].as.integer);
        const DiamondString *mname=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const int64_t target_function=registers[(size_t)base+2].as.integer;
        const int64_t arity_value=registers[(size_t)base+3].as.integer;
        const int64_t required_value=registers[(size_t)base+4].as.integer;
        if(class==nullptr||mname->length==0||
           mname->length>=DIAMOND_MAX_FUNCTION_NAME||
           target_function<0||(uint64_t)target_function>=built->function_count||
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<class->method_count;index++)
            if(!class->methods[index].included&&
               strlen(class->methods[index].name)==mname->length&&
               memcmp(class->methods[index].name,mname->chars,mname->length)==0) {
                snprintf(vm->error,sizeof vm->error,"method is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(class->method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,"class has too many methods");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod *method=&class->methods[class->method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,mname->chars,mname->length);
        method->name[mname->length]='\0';
        method->function_index=(uint16_t)target_function;
        method->arity=(uint8_t)arity_value;
        method->required_arity=(uint8_t)required_value;
        method->is_private=registers[(size_t)base+5].as.boolean;
        /* declare_function always leaves owner_class at UINT8_MAX (not a
         * method) since it runs before the caller knows whether this
         * function will end up registered as one -- diamond_compile's own
         * compile_definition sets it inline instead, once current_class is
         * known. Matched here now that it's known: parameter_offset (see
         * run_chunk's INVOKE handler) derives from owner_class, and a
         * method whose owner_class is still UINT8_MAX gets parameter_offset
         * 0 instead of 1, which silently breaks the private-method
         * "explicit self receiver" bypass for every ProgramBuilder-built
         * class -- caught by the self-hosted parser's own private/public
         * support calling a private method via `self.foo()`, not by any
         * existing scalar-argument differential case. See docs/roadmap.md's
         * self-hosting Phase 3 follow-up entry. */
        built->functions[target_function]->owner_class=(uint8_t)registers[base].as.integer;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_module_method_method) {
        if(argc!=6)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+5].kind!=DIAMOND_VALUE_BOOL)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t module_index=registers[base].as.integer;
        const DiamondString *name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const int64_t function_index=registers[(size_t)base+2].as.integer;
        const int64_t arity=registers[(size_t)base+3].as.integer;
        const int64_t required=registers[(size_t)base+4].as.integer;
        if(module_index<0||(uint64_t)module_index>=built->module_count||
           name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
           function_index<0||(uint64_t)function_index>=built->function_count||
           arity<0||arity>UINT8_MAX||required<0||required>arity)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondModule *module=&built->modules[(size_t)module_index];
        for(size_t index=0;index<module->method_count;index++)
            if(!module->methods[index].included&&
               strlen(module->methods[index].name)==name->length&&
               memcmp(module->methods[index].name,name->chars,name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"method is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(module->method_count==DIAMOND_MAX_METHODS)return DIAMOND_VM_TYPE_ERROR;
        DiamondMethod *method=&module->methods[module->method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,name->chars,name->length);
        method->name[name->length]='\0';
        method->function_index=(uint16_t)function_index;
        method->arity=(uint8_t)arity;
        method->required_arity=(uint8_t)required;
        method->is_private=registers[(size_t)base+5].as.boolean;
        /* Same owner_class fix as declare_method just above, using
         * diamond_compile's own module-method sentinel (UINT8_MAX-1,
         * distinct from UINT8_MAX's "not a method at all" so the private-
         * bypass's parameter_offset==1 check still fires for module
         * methods too). See docs/roadmap.md's self-hosting Phase 3
         * follow-up entry. */
        built->functions[function_index]->owner_class=UINT8_MAX-1;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_class_singleton_method_method) {
        if(argc!=5)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_class_singleton_method arguments must be "
                "(Int, String, Int, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondClass *class=
            program_builder_class(built,registers[base].as.integer);
        const DiamondString *mname=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const int64_t target_function=registers[(size_t)base+2].as.integer;
        const int64_t arity_value=registers[(size_t)base+3].as.integer;
        const int64_t required_value=registers[(size_t)base+4].as.integer;
        if(class==nullptr||mname->length==0||
           mname->length>=DIAMOND_MAX_FUNCTION_NAME||
           target_function<0||(uint64_t)target_function>=built->function_count||
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_class_singleton_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<class->singleton_method_count;index++)
            if(strlen(class->singleton_methods[index].name)==mname->length&&
               memcmp(class->singleton_methods[index].name,mname->chars,
                      mname->length)==0) {
                snprintf(vm->error,sizeof vm->error,
                    "duplicate or excessive class singleton method");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(class->singleton_method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,
                "duplicate or excessive class singleton method");
            return DIAMOND_VM_TYPE_ERROR;
        }
        /* No needs_receiver/owner_class here, matching diamond_compile's
         * own module_singleton branch exactly: a directly-declared
         * singleton (`def self.foo`) never reserves register 0 for an
         * implicit self, unlike an ordinary method -- see declare_method
         * just above for the contrasting case that does. */
        DiamondMethod *method=
            &class->singleton_methods[class->singleton_method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,mname->chars,mname->length);
        method->name[mname->length]='\0';
        method->function_index=(uint16_t)target_function;
        method->arity=(uint8_t)arity_value;
        method->required_arity=(uint8_t)required_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_module_singleton_method_method) {
        if(argc!=5)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+3].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_module_singleton_method arguments must be "
                "(Int, String, Int, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t module_index=registers[base].as.integer;
        const DiamondString *mname=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const int64_t target_function=registers[(size_t)base+2].as.integer;
        const int64_t arity_value=registers[(size_t)base+3].as.integer;
        const int64_t required_value=registers[(size_t)base+4].as.integer;
        if(module_index<0||(uint64_t)module_index>=built->module_count||
           mname->length==0||mname->length>=DIAMOND_MAX_FUNCTION_NAME||
           target_function<0||(uint64_t)target_function>=built->function_count||
           arity_value<0||arity_value>UINT8_MAX||
           required_value<0||required_value>arity_value) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_module_singleton_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondModule *module=&built->modules[(size_t)module_index];
        for(size_t index=0;index<module->singleton_method_count;index++)
            if(strlen(module->singleton_methods[index].name)==mname->length&&
               memcmp(module->singleton_methods[index].name,mname->chars,
                      mname->length)==0) {
                snprintf(vm->error,sizeof vm->error,
                    "duplicate or excessive module singleton function");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(module->singleton_method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,
                "duplicate or excessive module singleton function");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod *method=
            &module->singleton_methods[module->singleton_method_count++];
        *method=(DiamondMethod){};
        memcpy(method->name,mname->chars,mname->length);
        method->name[mname->length]='\0';
        method->function_index=(uint16_t)target_function;
        method->arity=(uint8_t)arity_value;
        method->required_arity=(uint8_t)required_value;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(set_function_owner_class_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_function_owner_class arguments must be (Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t function_index=registers[base].as.integer;
        const int64_t owner_class=registers[(size_t)base+1].as.integer;
        if(function_index<0||(uint64_t)function_index>=built->function_count||
           owner_class<0||owner_class>UINT8_MAX) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_function_owner_class has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        built->functions[function_index]->owner_class=(uint8_t)owner_class;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(include_module_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondClass *class=program_builder_class(built,registers[base].as.integer);
        const int64_t module_index=registers[(size_t)base+1].as.integer;
        if(class==nullptr||module_index<0||
           (uint64_t)module_index>=built->module_count)return DIAMOND_VM_TYPE_ERROR;
        const DiamondModule *module=&built->modules[(size_t)module_index];
        if(class->field_count+module->field_count>DIAMOND_MAX_FIELDS||
           class->method_count+module->method_count>DIAMOND_MAX_METHODS)
            return DIAMOND_VM_TYPE_ERROR;
        for(size_t field=0;field<module->field_count;field++) {
            bool present=false;
            for(size_t existing=0;existing<class->field_count;existing++)
                if(strcmp(class->fields[existing],module->fields[field])==0)
                    present=true;
            if(!present) {
                (void)snprintf(class->fields[class->field_count++],
                    DIAMOND_MAX_FUNCTION_NAME,"%s",module->fields[field]);
            }
        }
        for(size_t method=0;method<module->method_count;method++) {
            class->methods[class->method_count]=module->methods[method];
            class->methods[class->method_count++].included=true;
        }
        program_builder_recompute_shapes(class);
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(include_module_in_module_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t target_index=registers[base].as.integer;
        const int64_t source_index=registers[(size_t)base+1].as.integer;
        if(target_index<0||source_index<0||target_index==source_index||
           (uint64_t)target_index>=built->module_count||
           (uint64_t)source_index>=built->module_count)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondModule *target=&built->modules[(size_t)target_index];
        const DiamondModule *source=&built->modules[(size_t)source_index];
        if(target->field_count+source->field_count>DIAMOND_MAX_FIELDS||
           target->method_count+source->method_count>DIAMOND_MAX_METHODS)
            return DIAMOND_VM_TYPE_ERROR;
        for(size_t field=0;field<source->field_count;field++) {
            bool present=false;
            for(size_t existing=0;existing<target->field_count;existing++)
                if(strcmp(target->fields[existing],source->fields[field])==0)
                    present=true;
            if(!present) {
                const size_t length=strlen(source->fields[field]);
                memcpy(target->fields[target->field_count],source->fields[field],length+1);
                target->field_count++;
            }
        }
        for(size_t method=0;method<source->method_count;method++) {
            target->methods[target->method_count]=source->methods[method];
            target->methods[target->method_count++].included=true;
        }
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(set_module_method_visibility_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_BOOL)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t module_index=registers[base].as.integer;
        const DiamondString *name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(module_index<0||(uint64_t)module_index>=built->module_count)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondModule *module=&built->modules[(size_t)module_index];
        DiamondMethod *found=nullptr;
        for(size_t index=0;index<module->method_count;index++)
            if(!module->methods[index].included&&
               strlen(module->methods[index].name)==name->length&&
               memcmp(module->methods[index].name,name->chars,name->length)==0)
                found=&module->methods[index];
        if(found==nullptr) {
            snprintf(vm->error,sizeof vm->error,"undefined method for visibility change");
            return DIAMOND_VM_TYPE_ERROR;
        }
        found->is_private=registers[(size_t)base+2].as.boolean;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(set_class_method_visibility_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_BOOL)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondClass *class=program_builder_class(built,registers[base].as.integer);
        const DiamondString *name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(class==nullptr)return DIAMOND_VM_TYPE_ERROR;
        DiamondMethod *found=nullptr;
        for(size_t index=0;index<class->method_count;index++)
            if(!class->methods[index].included&&
               strlen(class->methods[index].name)==name->length&&
               memcmp(class->methods[index].name,name->chars,name->length)==0)
                found=&class->methods[index];
        if(found==nullptr) {
            snprintf(vm->error,sizeof vm->error,"undefined method for visibility change");
            return DIAMOND_VM_TYPE_ERROR;
        }
        found->is_private=registers[(size_t)base+2].as.boolean;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(alias_class_method_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+2].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondClass *class=program_builder_class(built,registers[base].as.integer);
        const DiamondString *alias_name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const DiamondString *original_name=
            (const DiamondString *)registers[(size_t)base+2].as.object;
        if(class==nullptr||alias_name->length==0||
           alias_name->length>=DIAMOND_MAX_FUNCTION_NAME) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "alias_class_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod *source=nullptr;
        for(size_t index=class->method_count;index>0;index--)
            if(!class->methods[index-1].included&&
               strlen(class->methods[index-1].name)==original_name->length&&
               memcmp(class->methods[index-1].name,original_name->chars,
                      original_name->length)==0) {
                source=&class->methods[index-1];break;
            }
        if(source==nullptr) {
            snprintf(vm->error,sizeof vm->error,"alias source is not defined here");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<class->method_count;index++)
            if(!class->methods[index].included&&
               strlen(class->methods[index].name)==alias_name->length&&
               memcmp(class->methods[index].name,alias_name->chars,
                      alias_name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"alias name is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(class->method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,"too many methods");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod copied=*source;
        memcpy(copied.name,alias_name->chars,alias_name->length);
        copied.name[alias_name->length]='\0';copied.included=false;
        class->methods[class->method_count++]=copied;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(alias_module_method_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+2].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t module_index=registers[base].as.integer;
        const DiamondString *alias_name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const DiamondString *original_name=
            (const DiamondString *)registers[(size_t)base+2].as.object;
        if(module_index<0||(uint64_t)module_index>=built->module_count||
           alias_name->length==0||alias_name->length>=DIAMOND_MAX_FUNCTION_NAME) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "alias_module_method has invalid arguments");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondModule *module=&built->modules[(size_t)module_index];
        DiamondMethod *source=nullptr;
        for(size_t index=module->method_count;index>0;index--)
            if(!module->methods[index-1].included&&
               strlen(module->methods[index-1].name)==original_name->length&&
               memcmp(module->methods[index-1].name,original_name->chars,
                      original_name->length)==0) {
                source=&module->methods[index-1];break;
            }
        if(source==nullptr) {
            snprintf(vm->error,sizeof vm->error,"alias source is not defined here");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<module->method_count;index++)
            if(!module->methods[index].included&&
               strlen(module->methods[index].name)==alias_name->length&&
               memcmp(module->methods[index].name,alias_name->chars,
                      alias_name->length)==0) {
                snprintf(vm->error,sizeof vm->error,"alias name is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(module->method_count==DIAMOND_MAX_METHODS) {
            snprintf(vm->error,sizeof vm->error,"too many methods");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondMethod copied=*source;
        memcpy(copied.name,alias_name->chars,alias_name->length);
        copied.name[alias_name->length]='\0';copied.included=false;
        module->methods[module->method_count++]=copied;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(export_module_method_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t module_index=registers[base].as.integer;
        const DiamondString *name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        if(module_index<0||(uint64_t)module_index>=built->module_count)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondModule *module=&built->modules[(size_t)module_index];
        DiamondMethod *source=nullptr;
        for(size_t index=module->method_count;index>0;index--)
            if(!module->methods[index-1].included&&
               strlen(module->methods[index-1].name)==name->length&&
               memcmp(module->methods[index-1].name,name->chars,name->length)==0) {
                source=&module->methods[index-1];break;
            }
        if(source==nullptr) {
            snprintf(vm->error,sizeof vm->error,
                "module_function target is not defined here");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<module->singleton_method_count;index++)
            if(strcmp(module->singleton_methods[index].name,source->name)==0) {
                snprintf(vm->error,sizeof vm->error,
                    "module singleton function is already defined");
                return DIAMOND_VM_TYPE_ERROR;
            }
        if(module->singleton_method_count==DIAMOND_MAX_METHODS)
            return DIAMOND_VM_TYPE_ERROR;
        source->is_private=true;
        DiamondMethod exported=*source;
        exported.is_private=false;
        exported.needs_receiver=true;
        module->singleton_methods[module->singleton_method_count++]=exported;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(expand_source_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING)
            return DIAMOND_VM_TYPE_ERROR;
        const DiamondString *name=(const DiamondString *)registers[base].as.object;
        const DiamondString *source=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        char path[DIAMOND_MAX_SOURCE_PATH];
        if(name->length>=sizeof path)return DIAMOND_VM_TYPE_ERROR;
        memcpy(path,name->chars,name->length);path[name->length]='\0';
        /* entry_path (not entry.name's 64-byte function-name buffer) holds
         * the root chunk's display name -- see src/compiler.h. */
        memcpy(built->entry_path,path,name->length+1);
        char *source_text=malloc(source->length+1);
        if(source_text==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        memcpy(source_text,source->chars,source->length);source_text[source->length]='\0';
        DiamondSourceBundle *bundle=malloc(sizeof *bundle);char error[512];
        if(bundle==nullptr){free(source_text);return DIAMOND_VM_OUT_OF_MEMORY;}
        const bool loaded=diamond_load_program(path,source_text,bundle,error,sizeof error);
        free(source_text);
        if(!loaded) {
            free(bundle);
            snprintf(vm->error,sizeof vm->error,"%s",error);
            return DIAMOND_VM_IO_ERROR;
        }
        DiamondString *expanded=allocate_string(vm,bundle->source,strlen(bundle->source));
        if(expanded==nullptr) {
            diamond_source_bundle_free(bundle);free(bundle);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(builder->source_bundle!=nullptr) {
            diamond_source_bundle_free(builder->source_bundle);
            free(builder->source_bundle);
        }
        builder->source_bundle=bundle;
        *result=DIAMOND_OBJECT(expanded);return DIAMOND_VM_OK;
    }
    if(source_location_method) {
        if(argc!=3||registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t offset=registers[base].as.integer;
        const int64_t line=registers[(size_t)base+1].as.integer;
        const int64_t column=registers[(size_t)base+2].as.integer;
        if(offset<0||line<1||column<1||builder->source_bundle==nullptr)
            return DIAMOND_VM_TYPE_ERROR;
        const char *mapped_path="<expanded>";
        uint64_t mapped_line=(uint64_t)line;
        for(size_t index=0;index<builder->source_bundle->segment_count;index++) {
            const DiamondSourceSegment *segment=&builder->source_bundle->segments[index];
            const uint64_t source_offset=(uint64_t)offset;
            if(source_offset<segment->start||
               (source_offset>segment->end&&source_offset-segment->end>9))continue;
            mapped_path=segment->path;mapped_line=segment->original_line;
            size_t limit=source_offset<segment->end
                ?(size_t)source_offset:segment->end;
            if(limit==segment->end&&limit>segment->start&&
               builder->source_bundle->source[limit-1]=='\n')limit--;
            for(size_t cursor=segment->start;cursor<limit;cursor++)
                if(builder->source_bundle->source[cursor]=='\n')mapped_line++;
            break;
        }
        char location[DIAMOND_MAX_SOURCE_PATH+64];
        const int written=snprintf(location,sizeof location,"%s:%lld:%lld",
            mapped_path,(long long)mapped_line,(long long)column);
        if(written<0||(size_t)written>=sizeof location)return DIAMOND_VM_TYPE_ERROR;
        DiamondString *mapped=allocate_string(vm,location,(size_t)written);
        if(mapped==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *result=DIAMOND_OBJECT(mapped);return DIAMOND_VM_OK;
    }
    if(set_source_location_method) {
        if(argc!=2||registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t line=registers[base].as.integer;
        const int64_t column=registers[(size_t)base+1].as.integer;
        if(line<0||line>UINT32_MAX||column<0||column>UINT32_MAX)
            return DIAMOND_VM_TYPE_ERROR;
        builder->source_line=(uint32_t)line;
        builder->source_column=(uint32_t)column;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_type_set_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_type_set arguments must be (Int, Array)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const DiamondArray *type_ids=
            (const DiamondArray *)registers[(size_t)base+1].as.object;
        if(target==nullptr||type_ids->count==0||
           type_ids->count>DIAMOND_MAX_UNION_TYPES) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "declare_type_set has an invalid function index or type list");
            return DIAMOND_VM_TYPE_ERROR;
        }
        for(size_t index=0;index<type_ids->count;index++) {
            if(type_ids->values[index].kind!=DIAMOND_VALUE_OBJECT||
               type_ids->values[index].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                    "declare_type_set has an invalid function index or type list");
                return DIAMOND_VM_TYPE_ERROR;
            }
            const DiamondArray *descriptor=
                (const DiamondArray *)type_ids->values[index].as.object;
            if(descriptor->count!=6||
               descriptor->values[0].kind!=DIAMOND_VALUE_INT||
               descriptor->values[1].kind!=DIAMOND_VALUE_INT||
               descriptor->values[2].kind!=DIAMOND_VALUE_INT||
               descriptor->values[3].kind!=DIAMOND_VALUE_INT||
               descriptor->values[4].kind!=DIAMOND_VALUE_INT||
               descriptor->values[5].kind!=DIAMOND_VALUE_OBJECT||
               descriptor->values[5].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                    "declare_type_set has an invalid function index or type list");
                return DIAMOND_VM_TYPE_ERROR;
            }
            const int64_t type_id=descriptor->values[0].as.integer;
            const int64_t argument_set=descriptor->values[1].as.integer;
            const int64_t second_argument_set=descriptor->values[2].as.integer;
            const int64_t callable_arity=descriptor->values[3].as.integer;
            const int64_t callable_return=descriptor->values[4].as.integer;
            const DiamondArray *callable_parameters=
                (const DiamondArray *)descriptor->values[5].as.object;
            const bool primitive=type_id>=0&&type_id<DIAMOND_TYPE_CLASS_BASE;
            const bool class_type=type_id>=DIAMOND_TYPE_CLASS_BASE&&
                type_id<DIAMOND_TYPE_VARIABLE_BASE&&
                (uint64_t)(type_id-DIAMOND_TYPE_CLASS_BASE)<built->class_count;
            const bool interface_type=type_id>=DIAMOND_TYPE_INTERFACE_BASE&&
                (uint64_t)(type_id-DIAMOND_TYPE_INTERFACE_BASE)<
                    built->interface_count;
            const bool variable_type=type_id>=DIAMOND_TYPE_VARIABLE_BASE&&
                type_id<DIAMOND_TYPE_INTERFACE_BASE&&
                (uint64_t)(type_id-DIAMOND_TYPE_VARIABLE_BASE)<
                    target->type_variable_count;
            const bool valid_argument=argument_set==-1||
                (argument_set>=0&&(uint64_t)argument_set<target->type_set_count);
            const bool valid_second=second_argument_set==-1||
                (second_argument_set>=0&&
                 (uint64_t)second_argument_set<target->type_set_count);
            const bool valid_return=callable_return==-1||
                (callable_return>=0&&
                 (uint64_t)callable_return<target->type_set_count);
            bool valid_parameters=callable_parameters->count<=16;
            for(size_t parameter=0;parameter<callable_parameters->count;
                parameter++) {
                const DiamondValue value=callable_parameters->values[parameter];
                if(value.kind!=DIAMOND_VALUE_INT||value.as.integer<0||
                   (uint64_t)value.as.integer>=target->type_set_count)
                    valid_parameters=false;
            }
            const bool collection_arguments=
                (type_id==DIAMOND_TYPE_ARRAY&&valid_argument&&second_argument_set==-1)||
                (type_id==DIAMOND_TYPE_HASH&&valid_argument&&valid_second)||
                (argument_set==-1&&second_argument_set==-1);
            const bool callable_arguments=type_id!=DIAMOND_TYPE_CALLABLE?
                callable_arity==-1&&callable_return==-1&&
                    callable_parameters->count==0:
                callable_arity>=0&&callable_arity<=16&&valid_return&&
                    valid_parameters&&
                    (callable_parameters->count==0||
                     callable_parameters->count==(size_t)callable_arity);
            if(!(primitive||class_type||interface_type||variable_type)||
               !collection_arguments||
               !callable_arguments) {
                snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                    "declare_type_set has an invalid function index or type list");
                return DIAMOND_VM_TYPE_ERROR;
            }
        }
        if(target->type_set_count==DIAMOND_MAX_TYPE_SETS) {
            snprintf(vm->error,sizeof vm->error,
                "function has too many type annotations");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t new_index=(int64_t)target->type_set_count;
        DiamondTypeSet *set=&target->type_sets[target->type_set_count++];
        *set=(DiamondTypeSet){.count=(uint8_t)type_ids->count};
        for(size_t member=0;member<type_ids->count;member++) {
            const DiamondArray *descriptor=
                (const DiamondArray *)type_ids->values[member].as.object;
            const int64_t argument_set=descriptor->values[1].as.integer;
            const int64_t second_argument_set=descriptor->values[2].as.integer;
            const int64_t callable_arity=descriptor->values[3].as.integer;
            const int64_t callable_return=descriptor->values[4].as.integer;
            const DiamondArray *callable_parameters=
                (const DiamondArray *)descriptor->values[5].as.object;
            set->members[member]=(DiamondTypeMember){
                .id=(uint8_t)descriptor->values[0].as.integer,
                .argument_set=argument_set<0?UINT8_MAX:(uint8_t)argument_set,
                .second_argument_set=second_argument_set<0?UINT8_MAX:
                    (uint8_t)second_argument_set,
                .callable_arity=callable_arity<0?UINT8_MAX:(uint8_t)callable_arity,
                .callable_return_set=callable_return<0?UINT8_MAX:
                    (uint8_t)callable_return,
                .callable_parameters_typed=callable_parameters->count>0};
            for(size_t index=0;index<16;index++)
                set->members[member].callable_parameter_sets[index]=UINT8_MAX;
            for(size_t index=0;index<callable_parameters->count;index++)
                set->members[member].callable_parameter_sets[index]=
                    (uint8_t)callable_parameters->values[index].as.integer;
        }
        *result=DIAMOND_INT(new_index);return DIAMOND_VM_OK;
    }
    if(set_parameter_type_method) {
        if(argc!=3)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_parameter_type arguments must be (Int, Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t parameter=registers[(size_t)base+1].as.integer;
        const int64_t set=registers[(size_t)base+2].as.integer;
        if(target==nullptr||parameter<0||parameter>=target->arity||
           set<0||(uint64_t)set>=target->type_set_count) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#set_parameter_type has an invalid index");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->parameter_type_sets[(size_t)parameter]=(uint8_t)set;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(set_return_type_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
            snprintf(vm->error,sizeof vm->error,"ProgramBuilder#%s",
                "set_return_type arguments must be (Int, Int)");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const int64_t set=registers[(size_t)base+1].as.integer;
        if(target==nullptr||set<0||(uint64_t)set>=target->type_set_count) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#set_return_type has an invalid index");
            return DIAMOND_VM_TYPE_ERROR;
        }
        target->return_type_set=(uint8_t)set;
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(set_type_variables_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY)
            return DIAMOND_VM_TYPE_ERROR;
        DiamondFunction *target=
            program_builder_target(built,registers[base].as.integer);
        const DiamondArray *variables=
            (const DiamondArray *)registers[(size_t)base+1].as.object;
        if(target==nullptr||variables->count>8)return DIAMOND_VM_TYPE_ERROR;
        target->type_variable_count=(uint8_t)variables->count;
        for(size_t index=0;index<variables->count;index++) {
            if(variables->values[index].kind!=DIAMOND_VALUE_OBJECT||
               variables->values[index].as.object->kind!=DIAMOND_OBJECT_STRING)
                return DIAMOND_VM_TYPE_ERROR;
            const DiamondString *name=
                (const DiamondString *)variables->values[index].as.object;
            if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME)
                return DIAMOND_VM_TYPE_ERROR;
            memcpy(target->type_variables[index],name->chars,name->length);
            target->type_variables[index][name->length]='\0';
        }
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_interface_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_interface argument must be String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *name=(const DiamondString *)registers[base].as.object;
        if(name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
           built->interface_count==DIAMOND_MAX_INTERFACES) {
            snprintf(vm->error,sizeof vm->error,
                "ProgramBuilder#declare_interface has an invalid name");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const int64_t index=(int64_t)built->interface_count;
        DiamondInterface *interface=&built->interfaces[built->interface_count++];
        *interface=(DiamondInterface){.type_sets=built->entry.type_sets};
        memcpy(interface->name,name->chars,name->length);
        interface->name[name->length]='\0';
        *result=DIAMOND_INT(index);return DIAMOND_VM_OK;
    }
    if(inherit_interface_method) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t target_index=registers[base].as.integer;
        const int64_t base_index=registers[(size_t)base+1].as.integer;
        if(target_index<0||base_index<0||
           (uint64_t)target_index>=built->interface_count||
           (uint64_t)base_index>=built->interface_count||
           target_index==base_index)return DIAMOND_VM_TYPE_ERROR;
        DiamondInterface *target=&built->interfaces[(size_t)target_index];
        const DiamondInterface *source=&built->interfaces[(size_t)base_index];
        if(target->method_count+source->method_count>DIAMOND_MAX_METHODS)
            return DIAMOND_VM_TYPE_ERROR;
        for(size_t method=0;method<source->method_count;method++) {
            for(size_t existing=0;existing<target->method_count;existing++)
                if(strcmp(target->methods[existing].name,
                          source->methods[method].name)==0) {
                    snprintf(vm->error,sizeof vm->error,
                        "duplicate interface method");
                    return DIAMOND_VM_TYPE_ERROR;
                }
            target->methods[target->method_count++]=source->methods[method];
        }
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(declare_interface_method_method) {
        if(argc!=5)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT||
           registers[(size_t)base+3].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+3].as.object->kind!=DIAMOND_OBJECT_ARRAY||
           registers[(size_t)base+4].kind!=DIAMOND_VALUE_INT)
            return DIAMOND_VM_TYPE_ERROR;
        const int64_t interface_index=registers[base].as.integer;
        const DiamondString *name=
            (const DiamondString *)registers[(size_t)base+1].as.object;
        const int64_t arity=registers[(size_t)base+2].as.integer;
        const DiamondArray *sets=
            (const DiamondArray *)registers[(size_t)base+3].as.object;
        const int64_t return_set=registers[(size_t)base+4].as.integer;
        if(interface_index<0||(uint64_t)interface_index>=built->interface_count||
           name->length==0||name->length>=DIAMOND_MAX_FUNCTION_NAME||
           arity<0||arity>16||sets->count!=(size_t)arity||
           (return_set>=0&&(uint64_t)return_set>=built->entry.type_set_count))
            return DIAMOND_VM_TYPE_ERROR;
        DiamondInterface *interface=&built->interfaces[(size_t)interface_index];
        if(interface->method_count==DIAMOND_MAX_METHODS)return DIAMOND_VM_TYPE_ERROR;
        DiamondInterfaceMethod *method=&interface->methods[interface->method_count++];
        *method=(DiamondInterfaceMethod){.arity=(uint8_t)arity,
            .return_type_set=return_set<0?UINT8_MAX:(uint8_t)return_set};
        memcpy(method->name,name->chars,name->length);
        method->name[name->length]='\0';
        for(size_t index=0;index<16;index++)method->parameter_type_sets[index]=UINT8_MAX;
        for(size_t index=0;index<sets->count;index++) {
            if(sets->values[index].kind!=DIAMOND_VALUE_INT)return DIAMOND_VM_TYPE_ERROR;
            const int64_t set=sets->values[index].as.integer;
            if(set>=0&&(uint64_t)set>=built->entry.type_set_count)
                return DIAMOND_VM_TYPE_ERROR;
            method->parameter_type_sets[index]=set<0?UINT8_MAX:(uint8_t)set;
        }
        *result=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    /* run_method: the only remaining possibility once the combined
     * "no method matched" check above passed. */
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    /* Refuses to nest past a conservative depth rather than the full
     * DIAMOND_MAX_CALL_DEPTH: program_builder_run_helper's diamond_vm_run
     * starts a *fresh* run_chunk recursion (depth 0) on top of this
     * call's own C stack frame, which is itself already `depth` levels of
     * run_chunk deep -- so real C-stack usage is the *sum* of outer and
     * inner depth, not bounded by either guard alone. Capping outer depth
     * at 10 keeps that sum within the same call-depth budget already
     * verified safe under ASan (see the DIAMOND_MAX_CALL_DEPTH regression
     * entry in docs/roadmap.md) even if the inner program recurses to its
     * own full limit. */
    if(depth>=10) {
        snprintf(vm->error,sizeof vm->error,"ProgramBuilder#run nested too deeply");
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    return program_builder_run_helper(vm,builder,result);
}

static bool value_is_bignum(DiamondValue value) {
    return value.kind==DIAMOND_VALUE_OBJECT&&
        value.as.object->kind==DIAMOND_OBJECT_BIGNUM;
}

/* True for both representations an Int can have -- a plain inline
 * int64_t (kind==DIAMOND_VALUE_INT) or a promoted DiamondBignum. Used
 * everywhere "is this conceptually an Int" matters (type checks,
 * cross-type dispatch); arithmetic fast paths that specifically need
 * "is this a small int64_t I can compute on directly" still check
 * kind==DIAMOND_VALUE_INT alone, unchanged. */
static bool is_int_value(DiamondValue value) {
    return value.kind==DIAMOND_VALUE_INT||value_is_bignum(value);
}

static bool numeric_as_double(DiamondValue value, double *out) {
    if(value.kind==DIAMOND_VALUE_INT) {*out=(double)value.as.integer;return true;}
    if(value.kind==DIAMOND_VALUE_FLOAT) {*out=value.as.real;return true;}
    if(value_is_bignum(value)) {
        *out=diamond_bignum_to_double((const DiamondBignum *)value.as.object);
        return true;
    }
    return false;
}

static bool values_equal(DiamondValue left, DiamondValue right) {
    /* Bignum-aware equality ahead of everything else: a Float, however
     * large, is deliberately never treated as equal to a bignum (the
     * existing Int/Float cross-equality special-case below only
     * handles floats within int64 range, and that's left as-is rather
     * than extended -- see the bignum design notes). Two bignums, or a
     * bignum and a small Int, compare by value either way. */
    if(value_is_bignum(left)||value_is_bignum(right)) {
        if(left.kind==DIAMOND_VALUE_FLOAT||right.kind==DIAMOND_VALUE_FLOAT)return false;
        if(!is_int_value(left)||!is_int_value(right))return false;
        DiamondIntView left_view, right_view;
        diamond_int_view(left,&left_view);
        diamond_int_view(right,&right_view);
        return diamond_bignum_compare(left_view,right_view)==0;
    }
    /* Cross-type numeric equality (3 == 3.0) ahead of the kind guard,
     * per the auto-promotion design: Int widens to double for the
     * comparison. */
    if(left.kind==DIAMOND_VALUE_INT&&right.kind==DIAMOND_VALUE_FLOAT)
        return (double)left.as.integer==right.as.real;
    if(left.kind==DIAMOND_VALUE_FLOAT&&right.kind==DIAMOND_VALUE_INT)
        return left.as.real==(double)right.as.integer;
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
        case DIAMOND_VALUE_NIL:
            return true;
        case DIAMOND_VALUE_BOOL:
            return left.as.boolean == right.as.boolean;
        case DIAMOND_VALUE_INT:
            return left.as.integer == right.as.integer;
        case DIAMOND_VALUE_FLOAT:
            return left.as.real == right.as.real;
        case DIAMOND_VALUE_OBJECT: {
            if (left.as.object->kind != right.as.object->kind) return false;
            if(left.as.object->kind==DIAMOND_OBJECT_INSTANCE ||
               left.as.object->kind==DIAMOND_OBJECT_ARRAY ||
               left.as.object->kind==DIAMOND_OBJECT_HASH)
                return left.as.object==right.as.object;
            if(left.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *a=(const DiamondSymbol *)left.as.object;
                const DiamondSymbol *b=(const DiamondSymbol *)right.as.object;
                return a->length==b->length&&
                    memcmp(a->chars,b->chars,a->length)==0;
            }
            if(left.as.object->kind==DIAMOND_OBJECT_TIME) {
                /* By value (epoch), not identity -- two separately
                 * constructed Times at the same instant are `==`, same as
                 * Ruby, regardless of each one's own utc/local flag. */
                const DiamondTime *a=(const DiamondTime *)left.as.object;
                const DiamondTime *b=(const DiamondTime *)right.as.object;
                return a->epoch==b->epoch;
            }
            const DiamondString *a = (const DiamondString *)left.as.object;
            const DiamondString *b = (const DiamondString *)right.as.object;
            return a->length == b->length &&
                   memcmp(a->chars, b->chars, a->length) == 0;
        }
    }
    return false;
}

/* MurmurHash3's fmix64 finalizer (public domain) -- gives scalar/pointer
 * keys good bit distribution across all bits, not just the low bits a
 * power-of-two bucket mask would otherwise expose (a real risk for the
 * common case of small sequential Int keys). */
static uint64_t hash_mix64(uint64_t value) {
    value^=value>>33;value*=0xff51afd7ed558ccdULL;
    value^=value>>33;value*=0xc4ceb9fe1a85ec53ULL;
    value^=value>>33;
    return value;
}

/* FNV-1a over String content -- keys with equal bytes (values_equal's
 * String case, memcmp) must hash equal regardless of which String
 * object holds them. */
static uint64_t hash_bytes(const char *data,size_t length) {
    uint64_t hash=0xcbf29ce484222325ULL;
    for(size_t index=0;index<length;index++) {
        hash^=(unsigned char)data[index];
        hash*=0x100000001b3ULL;
    }
    return hash;
}

/* Must stay consistent with values_equal's exact equality semantics:
 * Int/Bool/Nil by value, String/Symbol by content, Array/Hash/Instance by
 * pointer identity. */
static uint64_t hash_value(DiamondValue value) {
    switch(value.kind) {
        case DIAMOND_VALUE_NIL:return hash_mix64(0);
        case DIAMOND_VALUE_BOOL:return hash_mix64(value.as.boolean?1:2);
        case DIAMOND_VALUE_INT:return hash_mix64((uint64_t)value.as.integer);
        case DIAMOND_VALUE_FLOAT: {
            const double real=value.as.real;
            /* A Float that's exactly equal to some Int64 (e.g. 3.0)
             * must hash the same way that Int64 does, since
             * values_equal treats 3 == 3.0 as true. Bounds-checked
             * before the int64 cast to avoid UB on an out-of-range or
             * non-finite double. */
            if(!isnan(real)&&!isinf(real)&&
               real>=-9223372036854775808.0&&real<9223372036854775808.0&&
               real==(double)(int64_t)real)
                return hash_mix64((uint64_t)(int64_t)real);
            uint64_t bits;
            memcpy(&bits,&real,sizeof bits);
            return hash_mix64(bits);
        }
        case DIAMOND_VALUE_OBJECT: {
            const DiamondObject *object=value.as.object;
            if(object->kind==DIAMOND_OBJECT_INSTANCE||
               object->kind==DIAMOND_OBJECT_ARRAY||
               object->kind==DIAMOND_OBJECT_HASH)
                return hash_mix64((uint64_t)(uintptr_t)object);
            /* No consistency requirement with the small-int hash path
             * above: the canonicalization invariant guarantees a
             * bignum and a small Int can never represent the same
             * value, so there's nothing for their hashes to need to
             * agree with. */
            if(object->kind==DIAMOND_OBJECT_BIGNUM)
                return diamond_bignum_hash((const DiamondBignum *)object);
            if(object->kind==DIAMOND_OBJECT_TIME) {
                /* Must agree with values_equal's by-epoch comparison --
                 * hashed the same bit-pattern way DIAMOND_VALUE_FLOAT is
                 * above, since epoch is itself just a double. */
                const double epoch=((const DiamondTime *)object)->epoch;
                uint64_t bits;
                memcpy(&bits,&epoch,sizeof bits);
                return hash_mix64(bits);
            }
            if(object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *symbol=(const DiamondSymbol *)object;
                return hash_bytes(symbol->chars,symbol->length);
            }
            const DiamondString *string=(const DiamondString *)object;
            return hash_bytes(string->chars,string->length);
        }
    }
    return 0;
}

/* Rebuilds only the bucket index table from entries[]'s already-cached
 * per-entry hash -- entries[] itself is never reordered, which is what
 * keeps insertion order (and "update doesn't move position") intact
 * across any number of rehashes. No tombstones: nothing ever deletes a
 * Hash entry, so an empty slot (SIZE_MAX) always safely ends a probe. */
static bool hash_rehash(DiamondVm *vm,DiamondHash *hash,size_t new_capacity) {
    size_t *buckets=malloc(new_capacity*sizeof(size_t));
    if(buckets==nullptr)return false;
    for(size_t index=0;index<new_capacity;index++)buckets[index]=SIZE_MAX;
    for(size_t index=0;index<hash->count;index++) {
        size_t slot=(size_t)(hash->entries[index].hash&(new_capacity-1));
        while(buckets[slot]!=SIZE_MAX)slot=(slot+1)&(new_capacity-1);
        buckets[slot]=index;
    }
    free(hash->buckets);
    if(hash->bucket_capacity>0)
        vm->bytes_allocated-=hash->bucket_capacity*sizeof(size_t);
    hash->buckets=buckets;hash->bucket_capacity=new_capacity;
    vm->bytes_allocated+=new_capacity*sizeof(size_t);
    return true;
}

static ptrdiff_t hash_find(const DiamondHash *hash,DiamondValue key) {
    if(hash->bucket_capacity==0)return -1;
    const uint64_t key_hash=hash_value(key);
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    for(size_t probe=0;probe<hash->bucket_capacity;probe++) {
        const size_t index=hash->buckets[slot];
        if(index==SIZE_MAX)return -1;
        if(hash->entries[index].hash==key_hash&&
           values_equal(hash->entries[index].key,key))
            return (ptrdiff_t)index;
        slot=(slot+1)&(hash->bucket_capacity-1);
    }
    return -1;
}

static bool hash_set(DiamondVm *vm,DiamondHash *hash,DiamondValue key,
                     DiamondValue value) {
    const ptrdiff_t existing=hash_find(hash,key);
    if(existing>=0) {hash->entries[(size_t)existing].value=value;return true;}
    if(hash->count==hash->capacity) {
        const size_t old_capacity=hash->capacity;
        const size_t capacity=old_capacity<8?8:old_capacity*2;
        DiamondHashEntry *entries=realloc(hash->entries,
            capacity*sizeof(DiamondHashEntry));
        if(entries==nullptr)return false;
        hash->entries=entries;hash->capacity=capacity;
        vm->bytes_allocated+=(capacity-old_capacity)*sizeof(DiamondHashEntry);
    }
    /* Load factor >= 0.75, checked with integer arithmetic. */
    if(hash->bucket_capacity==0||
       (hash->count+1)*4>=hash->bucket_capacity*3) {
        const size_t new_capacity=
            hash->bucket_capacity==0?8:hash->bucket_capacity*2;
        if(!hash_rehash(vm,hash,new_capacity))return false;
    }
    const uint64_t key_hash=hash_value(key);
    const size_t new_index=hash->count;
    hash->entries[hash->count++]=
        (DiamondHashEntry){.key=key,.value=value,.hash=key_hash};
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    while(hash->buckets[slot]!=SIZE_MAX)slot=(slot+1)&(hash->bucket_capacity-1);
    hash->buckets[slot]=new_index;
    return true;
}

static bool is_truthy(DiamondValue value) {
    return value.kind != DIAMOND_VALUE_NIL &&
           !(value.kind == DIAMOND_VALUE_BOOL && !value.as.boolean);
}

static const DiamondMethod *lookup_method(const DiamondChunk *chunk,
                                          const DiamondClass *class,
                                          const char *name, size_t length) {
    const DiamondClass *current = class;
    while (current != nullptr) {
        for(size_t index=current->method_count;index>0;index--) {
            const DiamondMethod *method=&current->methods[index-1];
            if (strlen(method->name) == length &&
                memcmp(method->name, name, length) == 0) {
                return method;
            }
        }
        current = current->superclass == UINT8_MAX
            ? nullptr : &chunk->classes[current->superclass];
    }
    return nullptr;
}

static const DiamondMethod *lookup_method_cached(
    DiamondVm *vm, const DiamondChunk *chunk, const uint8_t *site,
    const DiamondClass *class, const char *name, size_t length) {
    const size_t slot=((size_t)(uintptr_t)site>>2)%DIAMOND_INLINE_CACHE_COUNT;
    DiamondMethodCache *cache=&vm->method_caches[slot];
    if(cache->site!=site) {
        *cache=(DiamondMethodCache){.site=site};
    } else {
        if (cache->entry_count == 1 &&
            cache->hits >= vm->monomorphic_threshold &&
            cache->entries[0].receiver_class == class) {
            vm->inline_cache_hits++;
            vm->monomorphic_dispatches++;
            cache->hits++;
            return cache->entries[0].method;
        }
        for(size_t index=0;index<cache->entry_count;index++) {
            vm->method_cache_probes++;
            if(cache->entries[index].receiver_class!=class)continue;
            vm->inline_cache_hits++;
            cache->hits++;
            return cache->entries[index].method;
        }
    }
    vm->inline_cache_misses++;
    cache->misses++;
    const DiamondMethod *method=lookup_method(chunk,class,name,length);
    size_t entry=cache->entry_count;
    if(entry<DIAMOND_INLINE_CACHE_WIDTH) {
        cache->entry_count++;
    } else {
        entry=cache->next_replace;
        cache->next_replace=(uint8_t)((cache->next_replace+1)%DIAMOND_INLINE_CACHE_WIDTH);
    }
    cache->entries[entry]=(DiamondMethodCacheEntry){
        .receiver_class=class,.method=method};
    return method;
}

/* Operator overloading (see docs/syntax.md): dispatches to a user-class
 * method named "+"/"=="/"negate"/etc. when a builtin arithmetic/comparison
 * opcode's operand doesn't otherwise know how to combine with the other
 * one. `argument` is the other operand for a binary operator; nullptr for
 * a unary one (negate), meaning the receiver is the method's only
 * argument. `*found` tells the caller whether a method was located at
 * all -- when false, the caller falls through to its own existing
 * TypeError path unchanged, so this never changes behavior for a class
 * that doesn't define the operator. Uses lookup_method_cached (the same
 * cache INVOKE/INVOKE_MONO use, keyed off `site`) rather than a plain
 * lookup_method, so a hot operator-overload call site gets the same
 * monomorphic-class fast path any other polymorphic call site does, with
 * no new caching mechanism needed. Deliberately does not check
 * method->is_private: `a + b` is operator syntax, not an explicit-receiver
 * method call the way `a.plus(b)` would be -- matches how the to_s
 * dispatch in stringify_value below also ignores privacy. */
static DiamondVmStatus invoke_operator_method(DiamondVm *vm,
        const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondInstance *receiver,
        const char *name, size_t name_length, const DiamondValue *argument,
        DiamondValue *result, bool *found) {
    const DiamondChunk *owner=receiver->owner!=nullptr?receiver->owner:chunk;
    const DiamondMethod *method=lookup_method_cached(vm,owner,site,
        receiver->class,name,name_length);
    if(method==nullptr) {*found=false;return DIAMOND_VM_OK;}
    *found=true;
    /* method->arity/required_arity are stored receiver-exclusive (see
     * compile_definition's `function->arity-1` when registering a class
     * method), matching how the real INVOKE site checks its own `argc`
     * (also receiver-exclusive) against them -- only the explicit operand
     * counts here, not the receiver. */
    const size_t explicit_argument_count=argument==nullptr?0:1;
    if(explicit_argument_count<method->required_arity||
       explicit_argument_count>method->arity)
        return DIAMOND_VM_ARITY_ERROR;
    const size_t argument_count=argument==nullptr?1:2;
    DiamondValue args[2]={DIAMOND_OBJECT((DiamondObject *)receiver)};
    if(argument!=nullptr)args[1]=*argument;
    const DiamondFunction *fn=owner->functions[method->function_index];
    const DiamondChunk child={.name=fn->name,.code=fn->code,
      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
      .constants=fn->constants,.constant_count=fn->constant_count,
      .strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=owner->functions,.function_count=owner->function_count,
      .classes=owner->classes,.class_count=owner->class_count,
      .interfaces=owner->interfaces,.interface_count=owner->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
      .register_count=fn->register_count};
    return run_chunk(&child,vm,args,argument_count,depth+1,nullptr,result);
}

/* Shared fallback for `+` once the fast Int/Int path doesn't apply:
 * bignum promotion, mixed Int/Float promotion, String concatenation, and
 * an Instance's own `+` operator-overload method, in that exact order --
 * used by DIAMOND_OP_ADD's own case block (after its Int/Int fast path
 * finds the operands aren't both plain Int) and by ADD_INT's deopt
 * branch (which retargets the opcode back to ADD but, unlike a real
 * fresh dispatch of the now-generic instruction, has to run this same
 * fallback logic on the very instruction that just deopted, rather than
 * relying on ADD's own case block to run next). Previously hand-
 * duplicated at the second call site, and that copy was missing the
 * mixed-Int/Float branch entirely -- a real, previously-undiscovered
 * bug: once a `+` call site had been quickened to ADD_INT from earlier
 * Int+Int calls, a later Int+Float call at the same site raised a
 * spurious TypeError instead of promoting to Float, because only ADD's
 * own block had ever had the Float check. Confirmed with
 * DIAMOND_QUICKEN=1 DIAMOND_QUICKEN_THRESHOLD=1 before this fix. */
static DiamondVmStatus add_fallback(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        size_t instruction_offset,DiamondValue left_value,DiamondValue right_value,
        DiamondValue *out_result) {
    if (is_int_value(left_value) && is_int_value(right_value) &&
        (value_is_bignum(left_value) || value_is_bignum(right_value))) {
        DiamondIntView left_view, right_view;
        diamond_int_view(left_value,&left_view);
        diamond_int_view(right_value,&right_view);
        const DiamondValue bignum_result=diamond_bignum_add(vm,left_view,right_view);
        if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=bignum_result;
        return DIAMOND_VM_OK;
    }
    if ((left_value.kind==DIAMOND_VALUE_FLOAT||left_value.kind==DIAMOND_VALUE_INT) &&
        (right_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_INT) &&
        (left_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        /* Mixed Int/Float auto-promotes: the Int side widens to double
         * before the operation (user-confirmed design). */
        const double left_double=left_value.kind==DIAMOND_VALUE_FLOAT?
            left_value.as.real:(double)left_value.as.integer;
        const double right_double=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        *out_result=DIAMOND_FLOAT(left_double+right_double);
        return DIAMOND_VM_OK;
    }
    if (left_value.kind==DIAMOND_VALUE_OBJECT && right_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_STRING &&
        right_value.as.object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *left_string=(const DiamondString *)left_value.as.object;
        const DiamondString *right_string=(const DiamondString *)right_value.as.object;
        const size_t length=left_string->length+right_string->length;
        char *chars=malloc(length+1);
        if(chars==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        memcpy(chars,left_string->chars,left_string->length);
        memcpy(chars+left_string->length,right_string->chars,right_string->length);
        DiamondString *string=allocate_string(vm,chars,length);
        free(chars);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(string);
        return DIAMOND_VM_OK;
    }
    if (left_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        bool found=false;DiamondValue op_result=DIAMOND_NIL;
        const uint8_t *site=chunk->code+instruction_offset;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)left_value.as.object,"+",1,&right_value,
            &op_result,&found);
        if(found) {
            if(status!=DIAMOND_VM_OK)return status;
            *out_result=op_result;
            return DIAMOND_VM_OK;
        }
    }
    /* Time + Int|Float -> Time (offset forward, same utc flag as the
     * receiver); Time + Time is a TypeError, matching Ruby -- there's no
     * branch here for a Time right operand, so it simply falls through
     * to the type-error return below. */
    if (left_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_TIME &&
        (right_value.kind==DIAMOND_VALUE_INT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        const DiamondTime *left_time=(const DiamondTime *)left_value.as.object;
        const double offset=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        DiamondTime *result=allocate_time(vm,left_time->epoch+offset,left_time->utc);
        if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(result);
        return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR;
}

/* Time - Time -> Float seconds; Time - Int|Float -> Time (offset
 * backward, same utc flag). Factored out of run_chunk's own SUBTRACT/
 * MULTIPLY/DIVIDE case block for the same reason add_fallback already
 * is (see its own comment): every local declared anywhere in run_chunk's
 * switch adds to its one shared per-call stack frame, and run_chunk
 * recurses in C for every Diamond-level call -- confirmed the hard way
 * here too (an inline version of this logic reopened the exact
 * DIAMOND_MAX_CALL_DEPTH/ASan stack-overflow margin regression
 * add_fallback's comment already warns about, caught by tests/run.sh's
 * own depth(5000) case under `make sanitize`). Deliberately folds "not a
 * Time-involving mismatch" into the same DIAMOND_VM_TYPE_ERROR return as
 * a genuine error (rather than a separate `bool *matched`, the way
 * invoke_operator_method's own `bool *found` does) -- both cases want
 * exactly the same outcome here, and this keeps the run_chunk call site
 * down to one local instead of three, which is the whole point: even a
 * few bytes/variables matter at this margin (confirmed by trying the
 * three-local version first -- it still overflowed). Writes the result
 * straight into *out_result (the caller passes &registers[destination]
 * directly), so a successful call needs no separate temporary either. */
static DiamondVmStatus time_subtract_fallback(DiamondVm *vm,
        DiamondValue left_value,DiamondValue right_value,DiamondValue *out_result) {
    if (left_value.kind!=DIAMOND_VALUE_OBJECT||
        left_value.as.object->kind!=DIAMOND_OBJECT_TIME)
        return DIAMOND_VM_TYPE_ERROR;
    const DiamondTime *left_time=(const DiamondTime *)left_value.as.object;
    if (right_value.kind==DIAMOND_VALUE_OBJECT&&
        right_value.as.object->kind==DIAMOND_OBJECT_TIME) {
        const DiamondTime *right_time=(const DiamondTime *)right_value.as.object;
        *out_result=DIAMOND_FLOAT(left_time->epoch-right_time->epoch);
        return DIAMOND_VM_OK;
    }
    if (right_value.kind==DIAMOND_VALUE_INT||right_value.kind==DIAMOND_VALUE_FLOAT) {
        const double offset=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        DiamondTime *result=allocate_time(vm,left_time->epoch-offset,left_time->utc);
        if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(result);
        return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR;
}

/* Time vs Time only, matching Ruby -- no Time-vs-numeric ordering.
 * Compares epoch, ignoring utc/local. Same stack-frame-budget reasoning
 * as time_subtract_fallback above for why this is its own function
 * rather than inline in run_chunk's LESS/LESS_EQUAL/GREATER/GREATER_
 * EQUAL case block, and same reason it writes a ready-to-store
 * DiamondValue into *out_result rather than a bare bool -- the call site
 * needs zero locals of its own this way, just an `if` around the call. */
static bool time_comparison_fallback(DiamondValue left_value,DiamondValue right_value,
        DiamondOpCode opcode,DiamondValue *out_result) {
    if (left_value.kind!=DIAMOND_VALUE_OBJECT||
        left_value.as.object->kind!=DIAMOND_OBJECT_TIME||
        right_value.kind!=DIAMOND_VALUE_OBJECT||
        right_value.as.object->kind!=DIAMOND_OBJECT_TIME)
        return false;
    const double left_epoch=((const DiamondTime *)left_value.as.object)->epoch;
    const double right_epoch=((const DiamondTime *)right_value.as.object)->epoch;
    bool result=false;
    if(opcode==DIAMOND_OP_LESS)result=left_epoch<right_epoch;
    else if(opcode==DIAMOND_OP_LESS_EQUAL)result=left_epoch<=right_epoch;
    else if(opcode==DIAMOND_OP_GREATER)result=left_epoch>right_epoch;
    else result=left_epoch>=right_epoch;
    *out_result=DIAMOND_BOOL(result);
    return true;
}

/* Time.now()/Time.utc_now()'s shared body, and Time.at(epoch)'s --
 * pulled out of run_chunk's own TIME_NOW/TIME_AT cases for the same
 * stack-frame-budget reason as time_subtract_fallback/
 * time_comparison_fallback above (see time_subtract_fallback's own
 * comment for the full rationale, including the depth(5000)/ASan
 * regression this exact extraction fixes). */
static DiamondVmStatus time_now_helper(DiamondVm *vm,bool utc,DiamondValue *out_result) {
    struct timespec now={};
    clock_gettime(CLOCK_REALTIME,&now);
    const double epoch=(double)now.tv_sec+(double)now.tv_nsec/1e9;
    DiamondTime *time=allocate_time(vm,epoch,utc);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);
    return DIAMOND_VM_OK;
}

static DiamondVmStatus time_at_helper(DiamondVm *vm,DiamondValue epoch_value,
        DiamondValue *out_result) {
    if(epoch_value.kind!=DIAMOND_VALUE_INT&&epoch_value.kind!=DIAMOND_VALUE_FLOAT)
        return DIAMOND_VM_TYPE_ERROR;
    const double epoch=epoch_value.kind==DIAMOND_VALUE_FLOAT?
        epoch_value.as.real:(double)epoch_value.as.integer;
    if(isnan(epoch)||isinf(epoch))return DIAMOND_VM_TYPE_ERROR;
    DiamondTime *time=allocate_time(vm,epoch,false);
    if(time==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(time);
    return DIAMOND_VM_OK;
}

/* Mirrors find_function's two filters (compiler.c) exactly, operating on
 * the runtime DiamondChunk instead of the compile-time DiamondProgram:
 * excludes class/module methods (owner_class!=UINT8_MAX) and nested
 * def's, so a same-named local closure can never shadow a real
 * top-level prelude function. */
static const DiamondFunction *find_top_level_function(
        const DiamondChunk *chunk, const char *name, size_t length) {
    for (size_t index = chunk->function_count; index > 0; index--) {
        const DiamondFunction *candidate = chunk->functions[index - 1];
        if (candidate->owner_class != UINT8_MAX || candidate->nested) continue;
        if (strlen(candidate->name) == length &&
            memcmp(candidate->name, name, length) == 0) return candidate;
    }
    return nullptr;
}

static void record_rewritten_site(DiamondVm *vm, const uint8_t *site) {
    if (vm->rewritten_site_count >= DIAMOND_MAX_CODE)return;
    for (size_t index=0;index<vm->rewritten_site_count;index++)
        if (vm->rewritten_sites[index]==site)return;
    vm->rewritten_sites[vm->rewritten_site_count++]=site;
}

static DiamondFieldCacheEntry *lookup_field_cached(
    DiamondVm *vm, const uint8_t *site, const DiamondInstance *instance,
    uint8_t field, bool write) {
    const size_t slot=((size_t)(uintptr_t)site>>2)%DIAMOND_INLINE_CACHE_COUNT;
    DiamondFieldCache *cache=&vm->field_caches[slot];
    if(cache->site!=site) {
        *cache=(DiamondFieldCache){.site=site};
    } else {
        for(size_t index=0;index<cache->entry_count;index++) {
            if(cache->entries[index].input_shape!=instance->shape)continue;
            vm->field_cache_hits++;
            return &cache->entries[index];
        }
    }
    vm->field_cache_misses++;
    size_t entry=cache->entry_count;
    if(entry<DIAMOND_INLINE_CACHE_WIDTH) {
        cache->entry_count++;
    } else {
        entry=cache->next_replace;
        cache->next_replace=(uint8_t)((cache->next_replace+1)%DIAMOND_INLINE_CACHE_WIDTH);
    }
    const size_t needed=(size_t)field+1;
    cache->entries[entry]=(DiamondFieldCacheEntry){
        .input_shape=instance->shape,
        .output_shape=write && instance->shape->field_count<needed
            ? &instance->class->shapes[needed] : instance->shape,
        .materialized=(size_t)field<instance->shape->field_count};
    return &cache->entries[entry];
}

static int named_field_index(const DiamondInstance *instance,
                             const DiamondStringConstant *name) {
    for(size_t field=0;field<instance->class->field_count;field++)
        if(strlen(instance->class->fields[field])==name->length&&
           memcmp(instance->class->fields[field],name->chars,name->length)==0)
            return (int)field;
    return -1;
}

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index);

static bool value_matches_type(const DiamondChunk *chunk,DiamondValue value,
                               uint8_t type);

/* Every native method String/Array/Hash actually implement, for
 * structural-interface matching (see diamond_native_method_satisfies
 * below). Kept next to the real DIAMOND_OP_INVOKE dispatch further
 * down in this file so a future native-method addition is naturally
 * visible nearby. return_type is UINT8_MAX when a method's result
 * isn't one fixed scalar type (pop/key_at/value_at return the
 * container's stored element type; index_of returns Int | Nil) --
 * such a method can still satisfy an interface method with no return
 * annotation, just never one that requires a specific return type,
 * matching the same conservative convention already used for
 * user-class methods with an unannotated return. Enumerable-style
 * receiver methods (.each/.select/.count/.any?/.all?/.reduce/.map)
 * are deliberately not listed here -- they forward to ordinary prelude
 * Diamond functions rather than being native primitives, so an
 * interface already matches them the normal way, through a class's
 * own method table. */
typedef struct DiamondNativeMethod {
    uint8_t receiver_type;
    const char *name;
    uint8_t arity;
    uint8_t return_type;
} DiamondNativeMethod;

static const DiamondNativeMethod DIAMOND_NATIVE_METHODS[]={
    {DIAMOND_TYPE_STRING,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_ARRAY,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_HASH,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"repeat",1,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"ord",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"split",1,DIAMOND_TYPE_ARRAY},
    {DIAMOND_TYPE_STRING,"strip",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"reverse",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"downcase",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"upcase",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"to_i",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"to_f",0,DIAMOND_TYPE_FLOAT},
    {DIAMOND_TYPE_STRING,"index_of",1,UINT8_MAX},
    {DIAMOND_TYPE_STRING,"slice",2,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_ARRAY,"push",1,DIAMOND_TYPE_ARRAY},
    {DIAMOND_TYPE_ARRAY,"pop",0,UINT8_MAX},
    {DIAMOND_TYPE_HASH,"key_at",1,UINT8_MAX},
    {DIAMOND_TYPE_HASH,"value_at",1,UINT8_MAX},
};

bool diamond_native_method_satisfies(uint8_t receiver_type,const char *name,
                                     uint8_t arity,uint8_t *return_type) {
    for(size_t index=0;index<sizeof DIAMOND_NATIVE_METHODS/
        sizeof DIAMOND_NATIVE_METHODS[0];index++) {
        const DiamondNativeMethod *method=&DIAMOND_NATIVE_METHODS[index];
        if(method->receiver_type==receiver_type&&
           strcmp(method->name,name)==0&&method->arity==arity) {
            *return_type=method->return_type;
            return true;
        }
    }
    return false;
}

static bool value_matches_bound_node(const DiamondChunk *chunk,DiamondValue value,
                                     const DiamondTypeBinding *binding,
                                     uint8_t node_index) {
    if(node_index>=binding->node_count)return true;
    const DiamondBoundTypeNode *node=&binding->nodes[node_index];
    if(node->count==0)return true;
    for(size_t index=0;index<node->count;index++) {
        const DiamondBoundTypeMember member=node->members[index];
        if(!value_matches_type(chunk,value,member.id))continue;
        if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_node!=UINT8_MAX) {
            const DiamondArray *array=(const DiamondArray *)value.as.object;
            bool matches=true;
            for(size_t item=0;item<array->count&&matches;item++)
                matches=value_matches_bound_node(chunk,array->values[item],binding,
                                                 member.argument_node);
            if(matches)return true;
        } else if(member.id==DIAMOND_TYPE_HASH&&
                  member.argument_node!=UINT8_MAX&&
                  member.second_argument_node!=UINT8_MAX) {
            const DiamondHash *hash=(const DiamondHash *)value.as.object;
            bool matches=true;
            for(size_t item=0;item<hash->count&&matches;item++)
                matches=value_matches_bound_node(chunk,hash->entries[item].key,binding,
                                                 member.argument_node)&&
                    value_matches_bound_node(chunk,hash->entries[item].value,binding,
                                             member.second_argument_node);
            if(matches)return true;
        } else return true;
    }
    return false;
}

static bool value_matches_type(const DiamondChunk *chunk, DiamondValue value,
                               uint8_t type) {
    if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr)return true;
        return value_matches_bound_node(chunk,value,
            &chunk->type_variable_bindings[variable],0);
    }
    if(type==DIAMOND_TYPE_INT) return is_int_value(value);
    if(type==DIAMOND_TYPE_FLOAT) return value.kind==DIAMOND_VALUE_FLOAT;
    if(type==DIAMOND_TYPE_BOOL) return value.kind==DIAMOND_VALUE_BOOL;
    if(type==DIAMOND_TYPE_NIL) return value.kind==DIAMOND_VALUE_NIL;
    if(type==DIAMOND_TYPE_STRING) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_STRING;
    if(type==DIAMOND_TYPE_SYMBOL) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_SYMBOL;
    if(type==DIAMOND_TYPE_ARRAY) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_ARRAY;
    if(type==DIAMOND_TYPE_HASH) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_HASH;
    if(type==DIAMOND_TYPE_CALLABLE) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_CLOSURE;
    if(type==DIAMOND_TYPE_SIZED) {
        if(value.kind!=DIAMOND_VALUE_OBJECT)return false;
        if(value.as.object->kind==DIAMOND_OBJECT_STRING||
           value.as.object->kind==DIAMOND_OBJECT_ARRAY||
           value.as.object->kind==DIAMOND_OBJECT_HASH)return true;
        if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
        const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
        while(class!=nullptr) {
            for(size_t index=0;index<class->method_count;index++)
                if(strcmp(class->methods[index].name,"length")==0&&
                   class->methods[index].arity==0)return true;
            class=class->superclass==UINT8_MAX?nullptr:
                &chunk->classes[class->superclass];
        }
        return false;
    }
    if(type>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(type-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=chunk->interface_count||value.kind!=DIAMOND_VALUE_OBJECT)
            return false;
        const DiamondInterface *interface=&chunk->interfaces[interface_index];
        uint8_t builtin=UINT8_MAX;
        if(value.as.object->kind==DIAMOND_OBJECT_STRING)builtin=DIAMOND_TYPE_STRING;
        else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY)builtin=DIAMOND_TYPE_ARRAY;
        else if(value.as.object->kind==DIAMOND_OBJECT_HASH)builtin=DIAMOND_TYPE_HASH;
        if(builtin!=UINT8_MAX) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint8_t native_return=UINT8_MAX;
                if(!diamond_native_method_satisfies(builtin,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=UINT8_MAX) {
                    if(native_return==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,interface->type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;
            const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
            while(class!=nullptr&&!found) {
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=
                            &interface->methods[required];
                        const DiamondFunction *implementation=
                            chunk->functions[class->methods[method].function_index];
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint8_t required_set=wanted->parameter_type_sets[parameter];
                            const uint8_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==UINT8_MAX) {
                                if(actual_set!=UINT8_MAX)found=false;
                            } else if(actual_set!=UINT8_MAX&&
                                !runtime_set_satisfies(chunk,interface->type_sets,
                                    required_set,implementation->type_sets,
                                    actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,interface->type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                class=class->superclass==UINT8_MAX?nullptr:
                    &chunk->classes[class->superclass];
            }
            if(!found)return false;
        }
        return true;
    }
    const size_t class_index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
    if(class_index>=chunk->class_count || value.kind!=DIAMOND_VALUE_OBJECT ||
       value.as.object->kind!=DIAMOND_OBJECT_INSTANCE) return false;
    const DiamondClass *wanted=&chunk->classes[class_index];
    const DiamondClass *actual=((DiamondInstance *)value.as.object)->class;
    while(actual!=nullptr) {
        if(actual==wanted) return true;
        actual=actual->superclass==UINT8_MAX?nullptr:&chunk->classes[actual->superclass];
    }
    return false;
}

static bool value_matches_set(const DiamondChunk *chunk,DiamondValue value,
                              uint8_t set_index,bool attach);

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index);

static bool runtime_type_id_satisfies(const DiamondChunk *chunk,uint8_t known,
                                      uint8_t expected) {
    if(known==expected)return true;
    if(expected>=DIAMOND_TYPE_VARIABLE_BASE&&
       expected<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(expected-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr||
           chunk->type_variable_bindings[variable].node_count==0)return true;
        const DiamondBoundTypeNode *root=
            &chunk->type_variable_bindings[variable].nodes[0];
        for(size_t index=0;index<root->count;index++)
            if(runtime_type_id_satisfies(chunk,known,
               root->members[index].id))return true;
        return false;
    }
    if(known>=DIAMOND_TYPE_VARIABLE_BASE&&known<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(known-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr||
           chunk->type_variable_bindings[variable].node_count==0)return true;
        const DiamondBoundTypeNode *root=
            &chunk->type_variable_bindings[variable].nodes[0];
        for(size_t index=0;index<root->count;index++)
            if(!runtime_type_id_satisfies(chunk,root->members[index].id,expected))
                return false;
        return true;
    }
    if(expected==DIAMOND_TYPE_SIZED) {
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH)return true;
        if(known<DIAMOND_TYPE_CLASS_BASE)return false;
        size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
        while(index<chunk->class_count) {
            const DiamondClass *class=&chunk->classes[index];
            for(size_t method=0;method<class->method_count;method++)
                if(strcmp(class->methods[method].name,"length")==0&&
                   class->methods[method].arity==0)return true;
            if(class->superclass==UINT8_MAX)break;
            index=class->superclass;
        }
        return false;
    }
    if(expected>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(expected-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=chunk->interface_count)return false;
        const DiamondInterface *interface=&chunk->interfaces[interface_index];
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint8_t native_return=UINT8_MAX;
                if(!diamond_native_method_satisfies(known,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=UINT8_MAX) {
                    if(native_return==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,interface->type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(known<DIAMOND_TYPE_CLASS_BASE||known>=DIAMOND_TYPE_INTERFACE_BASE)
            return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
            while(index<chunk->class_count&&!found) {
                const DiamondClass *class=&chunk->classes[index];
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=&interface->methods[required];
                        const DiamondFunction *implementation=
                            chunk->functions[class->methods[method].function_index];
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint8_t required_set=wanted->parameter_type_sets[parameter];
                            const uint8_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==UINT8_MAX) {
                                if(actual_set!=UINT8_MAX)found=false;
                            } else if(actual_set!=UINT8_MAX&&
                                !runtime_set_satisfies(chunk,interface->type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,interface->type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                if(found||class->superclass==UINT8_MAX)break;
                index=class->superclass;
            }
            if(!found)return false;
        }
        return true;
    }
    if(known<DIAMOND_TYPE_CLASS_BASE||expected<DIAMOND_TYPE_CLASS_BASE)return false;
    size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
    const size_t wanted=(size_t)(expected-DIAMOND_TYPE_CLASS_BASE);
    while(index<chunk->class_count) {
        if(index==wanted)return true;
        const uint8_t parent=chunk->classes[index].superclass;
        if(parent==UINT8_MAX)break;
        index=parent;
    }
    return false;
}

static bool runtime_member_satisfies(const DiamondChunk *chunk,
                                     const DiamondTypeSet *known_sets,
                                     DiamondTypeMember known,
                                     const DiamondTypeSet *expected_sets,
                                     DiamondTypeMember expected) {
    if(!runtime_type_id_satisfies(chunk,known.id,expected.id))return false;
    if(expected.id==DIAMOND_TYPE_CALLABLE) {
        if(expected.callable_arity!=UINT8_MAX&&
           known.callable_arity!=expected.callable_arity)return false;
        if(expected.callable_parameters_typed)
            for(size_t parameter=0;parameter<expected.callable_arity;parameter++) {
                const uint8_t wanted=expected.callable_parameter_sets[parameter];
                const uint8_t actual=known.callable_parameter_sets[parameter];
                if(actual!=UINT8_MAX&&
                   !runtime_set_satisfies(chunk,expected_sets,wanted,
                                           known_sets,actual))return false;
            }
        return expected.callable_return_set==UINT8_MAX||
            (known.callable_return_set!=UINT8_MAX&&
             runtime_set_satisfies(chunk,known_sets,known.callable_return_set,
                                   expected_sets,expected.callable_return_set));
    }
    if(expected.argument_set==UINT8_MAX)return true;
    if(known.argument_set==UINT8_MAX||
       !runtime_set_satisfies(chunk,known_sets,known.argument_set,
                              expected_sets,expected.argument_set))return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=UINT8_MAX&&
        expected.second_argument_set!=UINT8_MAX&&
        runtime_set_satisfies(chunk,known_sets,known.second_argument_set,
                              expected_sets,expected.second_argument_set);
}

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint8_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint8_t expected_index) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=runtime_member_satisfies(chunk,known_sets,
                known->members[source],expected_sets,expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

static bool value_matches_member(const DiamondChunk *chunk,DiamondValue value,
                                 DiamondTypeMember member,bool attach) {
    if(!value_matches_type(chunk,value,member.id))return false;
    if(member.id==DIAMOND_TYPE_CALLABLE) {
        const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
        if(closure->function_index>=chunk->function_count)return false;
        const DiamondFunction *function=chunk->functions[closure->function_index];
        if(member.callable_arity!=UINT8_MAX&&
           (member.callable_arity<function->required_arity||
            member.callable_arity>function->arity))return false;
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                const uint8_t actual=function->parameter_type_sets[parameter];
                if(actual!=UINT8_MAX&&
                   !runtime_set_satisfies(chunk,chunk->type_sets,
                       member.callable_parameter_sets[parameter],
                       function->type_sets,actual))return false;
            }
        return member.callable_return_set==UINT8_MAX||
            ((size_t)member.callable_return_set<chunk->type_set_count&&
             function->return_type_set!=UINT8_MAX&&
             (size_t)function->return_type_set<function->type_set_count&&
             runtime_set_satisfies(chunk,function->type_sets,
                function->return_type_set,chunk->type_sets,
                member.callable_return_set));
    }
    if(member.argument_set==UINT8_MAX)return true;
    if((size_t)member.argument_set>=chunk->type_set_count)return false;
    if(member.id==DIAMOND_TYPE_ARRAY) {
        DiamondArray *array=(DiamondArray *)value.as.object;
        for(size_t index=0;index<array->count;index++)
            if(!value_matches_set(chunk,array->values[index],member.argument_set,false))
                return false;
        if(!attach)return true;
        for(size_t index=0;index<array->count;index++)
            if(!value_matches_set(chunk,array->values[index],member.argument_set,true))
                return false;
        for(size_t index=0;index<array->constraint_count;index++)
            if(array->constraints[index].type_sets==chunk->type_sets&&
               array->constraints[index].set_index==member.argument_set)return true;
        if(array->constraint_count==4)return false;
        array->constraints[array->constraint_count++]=(typeof(array->constraints[0])){
            .type_sets=chunk->type_sets,.type_set_count=chunk->type_set_count,
            .set_index=member.argument_set,.classes=chunk->classes,
            .class_count=chunk->class_count,.interfaces=chunk->interfaces,
            .interface_count=chunk->interface_count,
            .type_variable_count=chunk->type_variable_count};
        typeof(array->constraints[0]) *constraint=
            &array->constraints[array->constraint_count-1];
        if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr) {
            constraint->type_variable_bindings=malloc(
                chunk->type_variable_count*sizeof(DiamondTypeBinding));
            if(constraint->type_variable_bindings==nullptr) {
                array->constraint_count--;return false;
            }
            memcpy(constraint->type_variable_bindings,chunk->type_variable_bindings,
                   chunk->type_variable_count*sizeof(DiamondTypeBinding));
        }
        return true;
    }
    if(member.id!=DIAMOND_TYPE_HASH||member.second_argument_set==UINT8_MAX||
       (size_t)member.second_argument_set>=chunk->type_set_count)return false;
    DiamondHash *hash=(DiamondHash *)value.as.object;
    for(size_t index=0;index<hash->count;index++)
        if(!value_matches_set(chunk,hash->entries[index].key,member.argument_set,false)||
           !value_matches_set(chunk,hash->entries[index].value,
                              member.second_argument_set,false))return false;
    if(!attach)return true;
    for(size_t index=0;index<hash->count;index++)
        if(!value_matches_set(chunk,hash->entries[index].key,member.argument_set,true)||
           !value_matches_set(chunk,hash->entries[index].value,
                              member.second_argument_set,true))return false;
    for(size_t index=0;index<hash->constraint_count;index++)
        if(hash->constraints[index].type_sets==chunk->type_sets&&
           hash->constraints[index].key_set==member.argument_set&&
           hash->constraints[index].value_set==member.second_argument_set)return true;
    if(hash->constraint_count==4)return false;
    hash->constraints[hash->constraint_count++]=(typeof(hash->constraints[0])){
        .type_sets=chunk->type_sets,.type_set_count=chunk->type_set_count,
        .key_set=member.argument_set,.value_set=member.second_argument_set,
        .classes=chunk->classes,.class_count=chunk->class_count};
    hash->constraints[hash->constraint_count-1].interfaces=chunk->interfaces;
    hash->constraints[hash->constraint_count-1].interface_count=chunk->interface_count;
    hash->constraints[hash->constraint_count-1].type_variable_count=
        chunk->type_variable_count;
    if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr) {
        hash->constraints[hash->constraint_count-1].type_variable_bindings=malloc(
            chunk->type_variable_count*sizeof(DiamondTypeBinding));
        if(hash->constraints[hash->constraint_count-1].
           type_variable_bindings==nullptr) {
            hash->constraint_count--;return false;
        }
        memcpy(hash->constraints[hash->constraint_count-1].type_variable_bindings,
               chunk->type_variable_bindings,
               chunk->type_variable_count*sizeof(DiamondTypeBinding));
    }
    return true;
}

static bool value_matches_set(const DiamondChunk *chunk,DiamondValue value,
                              uint8_t set_index,bool attach) {
    if((size_t)set_index>=chunk->type_set_count)return false;
    const DiamondTypeSet *set=&chunk->type_sets[set_index];
    for(size_t index=0;index<set->count;index++)
        if(value_matches_member(chunk,value,set->members[index],attach))return true;
    return false;
}

static bool array_value_satisfies_constraints(DiamondArray *array,
                                               DiamondValue value) {
    for(size_t index=0;index<array->constraint_count;index++) {
        const typeof(array->constraints[0]) *constraint=&array->constraints[index];
        const DiamondChunk context={.type_sets=constraint->type_sets,
            .type_set_count=constraint->type_set_count,.classes=constraint->classes,
            .class_count=constraint->class_count,.interfaces=constraint->interfaces,
            .interface_count=constraint->interface_count,
            .type_variable_count=constraint->type_variable_count,
            .type_variable_bindings=constraint->type_variable_bindings};
        if(!value_matches_set(&context,value,constraint->set_index,true))return false;
    }
    return true;
}

static bool array_push(DiamondVm *vm,DiamondArray *array,DiamondValue value) {
    if(array->count==array->capacity) {
        if(array->capacity>SIZE_MAX/2/sizeof(DiamondValue))return false;
        const size_t old_capacity=array->capacity;
        const size_t capacity=old_capacity<8?8:old_capacity*2;
        DiamondValue *values=realloc(array->values,capacity*sizeof(DiamondValue));
        if(values==nullptr)return false;
        array->values=values;array->capacity=capacity;
        vm->bytes_allocated+=(capacity-old_capacity)*sizeof(DiamondValue);
    }
    array->values[array->count++]=value;return true;
}

static bool hash_entry_satisfies_constraints(DiamondHash *hash,
                                              DiamondValue key,
                                              DiamondValue value) {
    for(size_t index=0;index<hash->constraint_count;index++) {
        const typeof(hash->constraints[0]) *constraint=&hash->constraints[index];
        const DiamondChunk context={.type_sets=constraint->type_sets,
            .type_set_count=constraint->type_set_count,.classes=constraint->classes,
            .class_count=constraint->class_count,.interfaces=constraint->interfaces,
            .interface_count=constraint->interface_count,
            .type_variable_count=constraint->type_variable_count,
            .type_variable_bindings=constraint->type_variable_bindings};
        if(!value_matches_set(&context,key,constraint->key_set,true)||
           !value_matches_set(&context,value,constraint->value_set,true))return false;
    }
    return true;
}

static bool catch_exception(DiamondVm *vm,const DiamondChunk *chunk,
                            UnwindHandler *handlers,size_t *handler_count,
                            PendingUnwind *pending,DiamondValue *registers,
                            size_t *ip) {
    while(*handler_count>0) {
        (*handler_count)--;
        UnwindHandler *handler=&handlers[*handler_count];
        if(handler->kind==HANDLER_ENSURE) {
            *pending=(PendingUnwind){.kind=PENDING_EXCEPTION,
                                     .value=vm->exception};
            vm->has_exception=false;*ip=handler->target;return true;
        }
        if(!handler->enabled)continue;
        bool matches=handler->type_count==0;
        for(size_t i=0;i<handler->type_count&&!matches;i++)
            matches=value_matches_type(chunk,vm->exception,handler->types[i]);
        if(!matches)continue;
        *ip=handler->target;
        registers[handler->destination]=vm->exception;
        vm->has_exception=false;vm->error[0]='\0';return true;
    }
    return false;
}

static uint8_t exception_class_for_status(DiamondVmStatus status) {
    switch(status) {
        case DIAMOND_VM_TYPE_ERROR: return DIAMOND_CLASS_TYPE_ERROR;
        case DIAMOND_VM_INTEGER_OVERFLOW: return DIAMOND_CLASS_RANGE_ERROR;
        case DIAMOND_VM_DIVISION_BY_ZERO: return DIAMOND_CLASS_ZERO_DIVISION_ERROR;
        case DIAMOND_VM_ARITY_ERROR: return DIAMOND_CLASS_ARGUMENT_ERROR;
        case DIAMOND_VM_STACK_OVERFLOW: return DIAMOND_CLASS_SYSTEM_STACK_ERROR;
        case DIAMOND_VM_INDEX_ERROR: return DIAMOND_CLASS_INDEX_ERROR;
        case DIAMOND_VM_FIBER_NOT_RESUMABLE: return DIAMOND_CLASS_FIBER_ERROR;
        case DIAMOND_VM_YIELD_WITHOUT_FIBER: return DIAMOND_CLASS_FIBER_ERROR;
        case DIAMOND_VM_IO_ERROR: return DIAMOND_CLASS_IO_ERROR;
        case DIAMOND_VM_REGEXP_ERROR: return DIAMOND_CLASS_REGEXP_ERROR;
        case DIAMOND_VM_WOULD_BLOCK: return DIAMOND_CLASS_WOULD_BLOCK_ERROR;
        case DIAMOND_VM_PROGRAM_ERROR: return DIAMOND_CLASS_RUNTIME_ERROR;
        case DIAMOND_VM_THREAD_ERROR: return DIAMOND_CLASS_THREAD_ERROR;
        case DIAMOND_VM_SQLITE3_ERROR: return DIAMOND_CLASS_SQLITE3_ERROR;
        case DIAMOND_VM_POSTGRES_ERROR: return DIAMOND_CLASS_POSTGRES_ERROR;
        case DIAMOND_VM_MYSQL_ERROR: return DIAMOND_CLASS_MYSQL_ERROR;
        default: return UINT8_MAX;
    }
}

static bool catch_runtime_error(DiamondVm *vm,const DiamondChunk *chunk,
                                DiamondVmStatus status,UnwindHandler *handlers,
                                size_t *handler_count,PendingUnwind *pending,
                                DiamondValue *registers,size_t *ip) {
    const uint8_t class_index=exception_class_for_status(status);
    if(class_index==UINT8_MAX || (size_t)class_index>=chunk->class_count)return false;
    char message[sizeof vm->error];
    (void)snprintf(message,sizeof message,"%s",vm->error[0]!='\0'?vm->error:
                   diamond_vm_status_name(status));
    DiamondInstance *exception=allocate_instance(vm,&chunk->classes[class_index],nullptr);
    if(exception==nullptr)return false;
    vm->exception=DIAMOND_OBJECT(exception);vm->has_exception=true;
    DiamondString *text=allocate_string(vm,message,strlen(message));
    if(text==nullptr)return false;
    if(exception->field_count>0)exception->fields[0]=DIAMOND_OBJECT(text);
    (void)snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                   exception->class->name);
    return catch_exception(vm,chunk,handlers,handler_count,pending,registers,ip);
}

static const char *type_name(const DiamondChunk *chunk,uint8_t type) {
    const char *name="<invalid type>";
    if(type==DIAMOND_TYPE_INT) name="Int";
    else if(type==DIAMOND_TYPE_FLOAT) name="Float";
    else if(type==DIAMOND_TYPE_STRING) name="String";
    else if(type==DIAMOND_TYPE_SYMBOL) name="Symbol";
    else if(type==DIAMOND_TYPE_BOOL) name="Bool";
    else if(type==DIAMOND_TYPE_NIL) name="Nil";
    else if(type==DIAMOND_TYPE_ARRAY) name="Array";
    else if(type==DIAMOND_TYPE_HASH) name="Hash";
    else if(type==DIAMOND_TYPE_CALLABLE) name="Callable";
    else if(type==DIAMOND_TYPE_SIZED) name="Sized";
    else if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
        const DiamondTypeBinding *binding=chunk->type_variable_bindings;
        if(binding!=nullptr&&variable<chunk->type_variable_count&&
           binding[variable].node_count>0&&binding[variable].nodes[0].count>0)
            name=type_name(chunk,binding[variable].nodes[0].members[0].id);
        else name="TypeVariable";
    }
    else if(type>=DIAMOND_TYPE_INTERFACE_BASE&&
            (size_t)(type-DIAMOND_TYPE_INTERFACE_BASE)<chunk->interface_count)
        name=chunk->interfaces[type-DIAMOND_TYPE_INTERFACE_BASE].name;
    else {
        const size_t index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
        if(index<chunk->class_count) name=chunk->classes[index].name;
    }
    return name;
}

static void format_type_set_index(char *buffer,size_t capacity,
                                  const DiamondChunk *chunk,uint8_t set_index) {
    if((size_t)set_index>=chunk->type_set_count) {
        snprintf(buffer,capacity,"<invalid type set>");return;
    }
    const DiamondTypeSet *set=&chunk->type_sets[set_index];
    size_t used=0;buffer[0]='\0';
    for(size_t index=0;index<set->count && used<capacity;index++) {
        const int written=snprintf(buffer+used,capacity-used,"%s%s",
            index==0?"":" | ",type_name(chunk,set->members[index].id));
        if(written<0)return;
        used+=(size_t)written;
        if(set->members[index].argument_set!=UINT8_MAX&&used<capacity) {
            const int open=snprintf(buffer+used,capacity-used,"[");
            if(open<0)return;
            used+=(size_t)open;
            char nested[80];format_type_set_index(nested,sizeof nested,chunk,
                set->members[index].argument_set);
            char second[80]="";
            if(set->members[index].second_argument_set!=UINT8_MAX)
                format_type_set_index(second,sizeof second,chunk,
                    set->members[index].second_argument_set);
            const int close=snprintf(buffer+used,capacity-used,"%s%s%s]",nested,
                second[0]=='\0'?"":", ",second);
            if(close<0)return;
            used+=(size_t)close;
        } else if(set->members[index].id==DIAMOND_TYPE_CALLABLE&&
                  set->members[index].callable_arity!=UINT8_MAX&&used<capacity) {
            const DiamondTypeMember member=set->members[index];
            const int arity=snprintf(buffer+used,capacity-used,
                member.callable_parameters_typed?"[[":"[%u",member.callable_arity);
            if(arity<0)return;
            used+=(size_t)arity;
            if(member.callable_parameters_typed) {
                for(size_t parameter=0;parameter<member.callable_arity&&used<capacity;
                    parameter++) {
                    char parameter_type[80];
                    format_type_set_index(parameter_type,sizeof parameter_type,chunk,
                        member.callable_parameter_sets[parameter]);
                    const int result=snprintf(buffer+used,capacity-used,"%s%s",
                        parameter==0?"":", ",parameter_type);
                    if(result<0)return;
                    used+=(size_t)result;
                }
                if(used<capacity) {
                    const int close=snprintf(buffer+used,capacity-used,"]");
                    if(close<0)return;
                    used+=(size_t)close;
                }
            }
            if(member.callable_return_set!=UINT8_MAX&&used<capacity) {
                char returns[80];
                format_type_set_index(returns,sizeof returns,chunk,
                    member.callable_return_set);
                const int result=snprintf(buffer+used,capacity-used,", %s",returns);
                if(result<0)return;
                used+=(size_t)result;
            }
            if(used<capacity) {
                const int close=snprintf(buffer+used,capacity-used,"]");
                if(close<0)return;
                used+=(size_t)close;
            }
        }
    }
}

static void format_value_type(char *buffer, size_t capacity,
                              DiamondValue value) {
    const char *name="<unknown>";
    if(value.kind==DIAMOND_VALUE_NIL) name="Nil";
    else if(value.kind==DIAMOND_VALUE_BOOL) name="Bool";
    else if(value.kind==DIAMOND_VALUE_INT) name="Int";
    else if(value.kind==DIAMOND_VALUE_FLOAT) name="Float";
    else if(value.as.object->kind==DIAMOND_OBJECT_STRING) name="String";
    else if(value.as.object->kind==DIAMOND_OBJECT_SYMBOL) name="Symbol";
    else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY) name="Array";
    else if(value.as.object->kind==DIAMOND_OBJECT_HASH) name="Hash";
    else if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE) name="Callable";
    /* A promoted Int (see object.h's DiamondBignum) is still
     * conceptually an Int, not a distinct user-facing type -- must be
     * checked before the catch-all DiamondInstance branch below, or its
     * memory gets misread through an unrelated struct's layout. */
    else if(value.as.object->kind==DIAMOND_OBJECT_BIGNUM) name="Int";
    else {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        name=instance->class->name;
    }
    snprintf(buffer,capacity,"%s",name);
}

typedef struct StringBuilder {
    char *chars;
    size_t length;
    size_t capacity;
    const DiamondObject *active[32];
    size_t active_count;
} StringBuilder;

static bool builder_append(StringBuilder *builder,const char *chars,size_t length) {
    if(builder->length+length+1>builder->capacity) {
        size_t capacity=builder->capacity==0?64:builder->capacity;
        while(capacity<builder->length+length+1)capacity*=2;
        char *grown=realloc(builder->chars,capacity);
        if(grown==nullptr)return false;
        builder->chars=grown;builder->capacity=capacity;
    }
    memcpy(builder->chars+builder->length,chars,length);
    builder->length+=length;builder->chars[builder->length]='\0';return true;
}

/* Breaks a Time's epoch into calendar fields via gmtime_r/localtime_r
 * (chosen by ->utc) -- both are real, DST-aware, system-tzdata-backed
 * libc calls, so "supporting timezones" here is just calling the right
 * one, not hand-rolled timezone logic. Whole-second resolution, same as
 * Ruby/C convention (the fractional part only matters for #to_f). Fails
 * only for a genuinely out-of-range epoch (e.g. far enough in the future
 * to overflow time_t on a 32-bit platform) -- vanishingly unlikely on
 * any 64-bit system, but checked rather than left as UB. */
static bool time_struct_tm(const DiamondTime *target,struct tm *out) {
    const time_t seconds=(time_t)floor(target->epoch);
    return (target->utc?gmtime_r(&seconds,out):localtime_r(&seconds,out))!=nullptr;
}

/* Shared by #to_s and puts/string-interpolation's own stringify path
 * (see builder_format_value/stringify_value) -- one formatting
 * implementation, not two. */
static bool format_time_default(const DiamondTime *target,StringBuilder *builder) {
    struct tm parts;
    if(!time_struct_tm(target,&parts))return false;
    char buffer[64];
    const char *format=target->utc?
        "%Y-%m-%d %H:%M:%S UTC":"%Y-%m-%d %H:%M:%S %z";
    const size_t length=strftime(buffer,sizeof buffer,format,&parts);
    if(length==0)return false;
    return builder_append(builder,buffer,length);
}

/* Reads one line from stream into builder, growing across as many
 * underlying fgets calls as the line needs, then strips a trailing \n
 * and, if present, \r -- stripping happens on the accumulated line so
 * it's correct regardless of where an fgets chunk boundary falls
 * relative to the line ending. *saw_any is false only when zero bytes
 * were read before EOF. */
static DiamondVmStatus read_line(DiamondVm *vm,FILE *stream,StringBuilder *builder,
                                 bool *saw_any) {
    char chunk_buffer[256];
    *saw_any=false;
    errno=0;
    for(;;) {
        if(fgets(chunk_buffer,sizeof chunk_buffer,stream)==nullptr)break;
        *saw_any=true;
        const size_t piece_length=strlen(chunk_buffer);
        if(!builder_append(builder,chunk_buffer,piece_length))return DIAMOND_VM_OUT_OF_MEMORY;
        if(piece_length>0&&chunk_buffer[piece_length-1]=='\n')break;
    }
    if(ferror(stream)) {
        snprintf(vm->error,sizeof vm->error,"read error: %s",strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    if(builder->length>0&&builder->chars[builder->length-1]=='\n') {
        builder->length--;
        if(builder->length>0&&builder->chars[builder->length-1]=='\r')builder->length--;
        builder->chars[builder->length]='\0';
    }
    return DIAMOND_VM_OK;
}

/* SSL_read into a caller-supplied buffer, translating OpenSSL's own
 * error taxonomy into this VM's existing read-error/EOF conventions:
 * *out_eof true (with *out_read left 0) is the same "peer is done,
 * nothing more is ever coming" signal DIAMOND_OBJECT_SOCKET#read/
 * File#read already give a caller via a bare 0-byte-read/nil -- both a
 * clean TLS close_notify (SSL_ERROR_ZERO_RETURN) and the underlying TCP
 * connection just dropping without sending one (some peers do this) are
 * treated as EOF rather than an error, matching those two. No EINTR
 * retry here, same scope cut TCPSocket.connect and buffered File/stdin
 * reads already have (see docs/io.md's signals section) -- a signal
 * arriving mid-read makes the read fail rather than transparently
 * resuming. */
static DiamondVmStatus tls_read_chunk(DiamondVm *vm,SSL *ssl,void *buffer,size_t want,
        size_t *out_read,bool *out_eof) {
    *out_read=0;*out_eof=false;
    if(want==0)return DIAMOND_VM_OK;
    const int capped=want>(size_t)INT_MAX?INT_MAX:(int)want;
    ERR_clear_error();
    const int got=SSL_read(ssl,buffer,capped);
    if(got>0) {*out_read=(size_t)got;return DIAMOND_VM_OK;}
    const int ssl_error=SSL_get_error(ssl,got);
    if(ssl_error==SSL_ERROR_ZERO_RETURN||(ssl_error==SSL_ERROR_SYSCALL&&got==0)) {
        *out_eof=true;return DIAMOND_VM_OK;
    }
    char detail[256];
    if(ssl_error==SSL_ERROR_SYSCALL&&ERR_peek_error()==0)
        (void)snprintf(detail,sizeof detail,"%s",strerror(errno));
    else
        tls_format_error(detail,sizeof detail);
    snprintf(vm->error,sizeof vm->error,"TLS read error: %s",detail);
    return DIAMOND_VM_IO_ERROR;
}

/* Writes all of `length` bytes, looping over SSL_write as needed --
 * unlike DIAMOND_OBJECT_SOCKET#write (a non-blocking fd, where a partial
 * write is a normal outcome the caller retries), this fd is always
 * blocking, so a short SSL_write is only possible via a genuine error,
 * never "the buffer's full, try again later". */
static DiamondVmStatus tls_write_all(DiamondVm *vm,SSL *ssl,const char *data,size_t length) {
    size_t written=0;
    while(written<length) {
        const size_t remaining=length-written;
        const int want=remaining>(size_t)INT_MAX?INT_MAX:(int)remaining;
        ERR_clear_error();
        const int got=SSL_write(ssl,data+written,want);
        if(got<=0) {
            const int ssl_error=SSL_get_error(ssl,got);
            char detail[256];
            if(ssl_error==SSL_ERROR_SYSCALL&&ERR_peek_error()==0)
                (void)snprintf(detail,sizeof detail,"%s",strerror(errno));
            else
                tls_format_error(detail,sizeof detail);
            snprintf(vm->error,sizeof vm->error,"TLS write error: %s",detail);
            return DIAMOND_VM_IO_ERROR;
        }
        written+=(size_t)got;
    }
    return DIAMOND_VM_OK;
}

/* TLS analogue of read_line -- byte-at-a-time via tls_read_chunk rather
 * than fgets, since a raw SSL/fd has no libc stdio buffering underneath
 * it to lean on. Not as wasteful as it looks: SSL_read decrypts and
 * buffers a whole TLS record (up to 16KB) on the first call that needs
 * one, so a run of single-byte reads against an already-buffered record
 * costs one function call each, not one recv(2) each. Same trailing
 * \n/\r-stripping and *saw_any contract as read_line. */
static DiamondVmStatus tls_read_line(DiamondVm *vm,SSL *ssl,StringBuilder *builder,
                                     bool *saw_any) {
    *saw_any=false;
    for(;;) {
        char byte=0;size_t got=0;bool eof=false;
        const DiamondVmStatus status=tls_read_chunk(vm,ssl,&byte,1,&got,&eof);
        if(status!=DIAMOND_VM_OK)return status;
        if(eof||got==0)break;
        *saw_any=true;
        if(!builder_append(builder,&byte,1))return DIAMOND_VM_OUT_OF_MEMORY;
        if(byte=='\n')break;
    }
    if(builder->length>0&&builder->chars[builder->length-1]=='\n') {
        builder->length--;
        if(builder->length>0&&builder->chars[builder->length-1]=='\r')builder->length--;
        builder->chars[builder->length]='\0';
    }
    return DIAMOND_VM_OK;
}

static bool builder_format_value(StringBuilder *builder,DiamondValue value) {
    char scalar[96];int length=0;
    if(value.kind==DIAMOND_VALUE_NIL)return builder_append(builder,"nil",3);
    if(value.kind==DIAMOND_VALUE_BOOL)
        return builder_append(builder,value.as.boolean?"true":"false",
                              value.as.boolean?4:5);
    if(value.kind==DIAMOND_VALUE_INT) {
        length=snprintf(scalar,sizeof scalar,"%" PRId64,value.as.integer);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    if(value_is_bignum(value)) {
        /* Writes into a freshly-sized buffer rather than through the
         * fixed 96-byte `scalar` array above -- a bignum has no bound
         * on its digit count. */
        const DiamondBignum *bignum=(const DiamondBignum *)value.as.object;
        const size_t capacity=diamond_bignum_string_length(bignum);
        char *digits=malloc(capacity);
        if(digits==nullptr)return false;
        const size_t digit_length=diamond_bignum_to_string(bignum,digits,capacity);
        const bool ok=builder_append(builder,digits,digit_length);
        free(digits);
        return ok;
    }
    if(value.kind==DIAMOND_VALUE_FLOAT) {
        const double real=value.as.real;
        if(isnan(real))return builder_append(builder,"NaN",3);
        if(isinf(real))
            return real<0?builder_append(builder,"-Infinity",9):
                          builder_append(builder,"Infinity",8);
        /* Shortest decimal that round-trips exactly -- see value.c's
         * fprint_float for the full rationale (same algorithm,
         * duplicated per this codebase's existing Int/Float
         * formatting convention between the two call sites), including
         * probing the exponent first so %g doesn't jump to scientific
         * notation for round values like 10.0 just because the search
         * started at a low precision. */
        uint64_t real_bits;memcpy(&real_bits,&real,sizeof real_bits);
        char probe[32];
        snprintf(probe,sizeof probe,"%.0e",real);
        const char *exponent_marker=strchr(probe,'e');
        const int exponent=exponent_marker?atoi(exponent_marker+1):0;
        /* Only bump the starting precision when it can actually keep %g
         * in fixed-point mode (exponent 1..16) -- see value.c's
         * fprint_float for the full rationale (past that, %g would use
         * scientific notation at every precision anyway, so starting at
         * 1 costs nothing and finds a genuinely shorter form when one
         * exists). */
        const int start_precision=(exponent>=1&&exponent<=16)?exponent+1:1;
        for(int precision=start_precision;precision<=17;precision++) {
            length=snprintf(scalar,sizeof scalar,"%.*g",precision,real);
            if(length<0||(size_t)length>=sizeof scalar)continue;
            char *end=nullptr;
            const double parsed=strtod(scalar,&end);
            uint64_t parsed_bits;memcpy(&parsed_bits,&parsed,sizeof parsed_bits);
            if(end!=scalar&&parsed_bits==real_bits)break;
        }
        if(length<0||(size_t)length>=sizeof scalar)return false;
        bool has_marker=false;
        for(int index=0;index<length;index++)
            if(scalar[index]=='.'||scalar[index]=='e'||scalar[index]=='E') {
                has_marker=true;break;
            }
        if(!builder_append(builder,scalar,(size_t)length))return false;
        return has_marker||builder_append(builder,".0",2);
    }
    const DiamondObject *object=value.as.object;
    if(object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *string=(const DiamondString *)object;
        return builder_append(builder,string->chars,string->length);
    }
    if(object->kind==DIAMOND_OBJECT_SYMBOL) {
        /* Bare name, no leading ':' -- matches Ruby's to_s/puts/
         * interpolation convention (only inspect/p show the colon there,
         * and Diamond has no separate inspect mechanism), and keeps
         * to_sym(to_s(sym)) == sym a true round trip. */
        const DiamondSymbol *symbol=(const DiamondSymbol *)object;
        return builder_append(builder,symbol->chars,symbol->length);
    }
    for(size_t index=0;index<builder->active_count;index++)
        if(builder->active[index]==object)
            return builder_append(builder,
                object->kind==DIAMOND_OBJECT_HASH?"{...}":"[...]",5);
    if(object->kind==DIAMOND_OBJECT_ARRAY||object->kind==DIAMOND_OBJECT_HASH) {
        if(builder->active_count==32)return builder_append(builder,"...",3);
        builder->active[builder->active_count++]=object;
        const bool hash=object->kind==DIAMOND_OBJECT_HASH;
        if(!builder_append(builder,hash?"{":"[",1))return false;
        const size_t count=hash?((const DiamondHash *)object)->count:
                                ((const DiamondArray *)object)->count;
        for(size_t index=0;index<count;index++) {
            if(index>0&&!builder_append(builder,", ",2))return false;
            if(hash) {
                const DiamondHashEntry entry=((const DiamondHash *)object)->entries[index];
                if(!builder_format_value(builder,entry.key)||
                   !builder_append(builder,": ",2)||
                   !builder_format_value(builder,entry.value))return false;
            } else if(!builder_format_value(builder,
                ((const DiamondArray *)object)->values[index]))return false;
        }
        builder->active_count--;
        return builder_append(builder,hash?"}":"]",1);
    }
    if(object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)object;
        length=snprintf(scalar,sizeof scalar,"#<%s>",instance->class->name);
        return length>=0&&(size_t)length<sizeof scalar&&
            builder_append(builder,scalar,(size_t)length);
    }
    if(object->kind==DIAMOND_OBJECT_TIME)
        return format_time_default((const DiamondTime *)object,builder);
    return builder_append(builder,"#<Closure>",10);
}

static DiamondVmStatus stringify_value(DiamondVm *vm,const DiamondChunk *chunk,
                                        size_t depth,DiamondValue value,
                                        DiamondValue *out) {
    if(value.kind==DIAMOND_VALUE_OBJECT&&
       value.as.object->kind==DIAMOND_OBJECT_STRING) {
        *out=value;return DIAMOND_VM_OK;
    }
    if(value.kind==DIAMOND_VALUE_OBJECT&&
       value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:chunk;
        const DiamondMethod *method=lookup_method(owner,instance->class,
            "to_s",sizeof("to_s")-1);
        if(method!=nullptr) {
            if(method->required_arity>0)return DIAMOND_VM_ARITY_ERROR;
            const DiamondFunction *fn=owner->functions[method->function_index];
            const DiamondChunk child={.name=fn->name,.code=fn->code,
              .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
              .constants=fn->constants,.constant_count=fn->constant_count,
              .strings=fn->strings,.string_count=fn->string_count,
              .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
              .functions=owner->functions,.function_count=owner->function_count,
              .classes=owner->classes,.class_count=owner->class_count,
              .interfaces=owner->interfaces,.interface_count=owner->interface_count,
              .parameter_type_sets=fn->parameter_type_sets,
              .type_variable_count=fn->type_variable_count,
              .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
              .register_count=fn->register_count};
            DiamondValue converted=DIAMOND_NIL;
            DiamondVmStatus status=run_chunk(&child,vm,&value,1,
                                              depth+1,nullptr,&converted);
            if(status!=DIAMOND_VM_OK)return status;
            if(converted.kind!=DIAMOND_VALUE_OBJECT||
               converted.as.object->kind!=DIAMOND_OBJECT_STRING) {
                snprintf(vm->error,sizeof vm->error,"to_s must return String");
                return DIAMOND_VM_TYPE_ERROR;
            }
            *out=converted;return DIAMOND_VM_OK;
        }
    }
    StringBuilder builder={};
    if(!builder_format_value(&builder,value)) {
        free(builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondString *string=allocate_string(vm,builder.chars,builder.length);
    free(builder.chars);
    if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
}

/* Array#join's real body, factored out of run_chunk's own INVOKE case
 * for the same stack-frame-isolation reason as program_builder_run_
 * helper above -- StringBuilder's own `active[32]` pointer array alone
 * is real stack weight, and run_chunk is a single, deeply (self-)
 * recursive function whose safe recursion depth is a measured
 * constraint (see DIAMOND_MAX_CALL_DEPTH's own comment); a local this
 * size declared directly inside its switch gets baked into every
 * run_chunk call's stack frame, not just calls that actually reach
 * #join. stringify_value (not builder_format_value) matches
 * interpolation's own `"#{value}"` semantics exactly, including
 * calling a user-defined to_s override on an Instance -- the behavior
 * array_join's old Diamond-level `lib/core.di` loop had via its own
 * `"#{}"` interpolation, before it became this native, O(n) method
 * (see docs/roadmap.md's "Collections and Enumerable" entry). */
static DiamondVmStatus array_join_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const DiamondArray *array,const char *separator_chars,
        size_t separator_length,DiamondValue *out) {
    StringBuilder builder={};
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(size_t index=0;index<array->count;index++) {
        if(index>0&&!builder_append(&builder,separator_chars,separator_length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        DiamondValue piece=DIAMOND_NIL;
        status=stringify_value(vm,chunk,depth,array->values[index],&piece);
        if(status!=DIAMOND_VM_OK)break;
        const DiamondString *piece_string=(const DiamondString *)piece.as.object;
        if(!builder_append(&builder,piece_string->chars,piece_string->length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
    }
    if(status!=DIAMOND_VM_OK) {free(builder.chars);return status;}
    DiamondString *joined=allocate_string(vm,builder.chars?builder.chars:"",builder.length);
    free(builder.chars);
    if(joined==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(joined);
    return DIAMOND_VM_OK;
}

static uint8_t runtime_value_type(const DiamondChunk *chunk,DiamondValue value) {
    if(value.kind==DIAMOND_VALUE_NIL)return DIAMOND_TYPE_NIL;
    if(value.kind==DIAMOND_VALUE_BOOL)return DIAMOND_TYPE_BOOL;
    if(value.kind==DIAMOND_VALUE_INT)return DIAMOND_TYPE_INT;
    if(value.kind==DIAMOND_VALUE_FLOAT)return DIAMOND_TYPE_FLOAT;
    if(value.as.object->kind==DIAMOND_OBJECT_STRING)return DIAMOND_TYPE_STRING;
    if(value.as.object->kind==DIAMOND_OBJECT_ARRAY)return DIAMOND_TYPE_ARRAY;
    if(value.as.object->kind==DIAMOND_OBJECT_HASH)return DIAMOND_TYPE_HASH;
    if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE)return DIAMOND_TYPE_CALLABLE;
    const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
    for(size_t index=0;index<chunk->class_count;index++)
        if(&chunk->classes[index]==class)return (uint8_t)(DIAMOND_TYPE_CLASS_BASE+index);
    return UINT8_MAX;
}

static uint8_t binding_node(DiamondTypeBinding *binding) {
    if(binding->node_count==DIAMOND_BOUND_TYPE_NODES)return UINT8_MAX;
    return binding->node_count++;
}

static DiamondBoundTypeMember *binding_member(DiamondTypeBinding *binding,
                                               uint8_t node,uint8_t id) {
    if(node>=binding->node_count)return nullptr;
    DiamondBoundTypeNode *target=&binding->nodes[node];
    for(size_t index=0;index<target->count;index++)
        if(target->members[index].id==id)return &target->members[index];
    if(target->count==DIAMOND_BOUND_TYPE_MEMBERS)return nullptr;
    target->members[target->count]=(DiamondBoundTypeMember){.id=id,
        .argument_node=UINT8_MAX,.second_argument_node=UINT8_MAX};
    return &target->members[target->count++];
}

static void bind_value_graph(const DiamondChunk *chunk,DiamondTypeBinding *binding,
                             uint8_t node,DiamondValue value) {
    const uint8_t type=runtime_value_type(chunk,value);
    DiamondBoundTypeMember *member=binding_member(binding,node,type);
    if(member==nullptr)return;
    if(type==DIAMOND_TYPE_ARRAY) {
        if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
        if(member->argument_node==UINT8_MAX)return;
        const DiamondArray *array=(const DiamondArray *)value.as.object;
        for(size_t index=0;index<array->count;index++)
            bind_value_graph(chunk,binding,member->argument_node,array->values[index]);
    } else if(type==DIAMOND_TYPE_HASH) {
        if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
        if(member->second_argument_node==UINT8_MAX)
            member->second_argument_node=binding_node(binding);
        if(member->argument_node==UINT8_MAX||member->second_argument_node==UINT8_MAX)return;
        const DiamondHash *hash=(const DiamondHash *)value.as.object;
        for(size_t index=0;index<hash->count;index++) {
            bind_value_graph(chunk,binding,member->argument_node,hash->entries[index].key);
            bind_value_graph(chunk,binding,member->second_argument_node,
                             hash->entries[index].value);
        }
    }
}

static void bind_known_set(DiamondTypeBinding *binding,uint8_t node,
                           const DiamondTypeSet *sets,uint8_t set_index) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember known=set->members[index];
        DiamondBoundTypeMember *member=binding_member(binding,node,known.id);
        if(member==nullptr)continue;
        if(known.argument_set!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
            if(member->argument_node!=UINT8_MAX)
                bind_known_set(binding,member->argument_node,sets,known.argument_set);
        }
        if(known.second_argument_set!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(binding);
            if(member->second_argument_node!=UINT8_MAX)
                bind_known_set(binding,member->second_argument_node,sets,
                               known.second_argument_set);
        }
    }
}

static void bind_bound_node(DiamondTypeBinding *target,uint8_t target_node,
                            const DiamondTypeBinding *source,uint8_t source_node) {
    if(source_node>=source->node_count)return;
    const DiamondBoundTypeNode *node=&source->nodes[source_node];
    for(size_t index=0;index<node->count;index++) {
        const DiamondBoundTypeMember known=node->members[index];
        DiamondBoundTypeMember *member=
            binding_member(target,target_node,known.id);
        if(member==nullptr)continue;
        if(known.argument_node!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)
                member->argument_node=binding_node(target);
            if(member->argument_node!=UINT8_MAX)
                bind_bound_node(target,member->argument_node,source,
                                known.argument_node);
        }
        if(known.second_argument_node!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(target);
            if(member->second_argument_node!=UINT8_MAX)
                bind_bound_node(target,member->second_argument_node,source,
                                known.second_argument_node);
        }
    }
}

static void bind_context_set(DiamondTypeBinding *binding,uint8_t node,
    const DiamondChunk *context,const DiamondTypeSet *sets,uint8_t set_index) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember known=set->members[index];
        if(known.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           known.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(known.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable<context->type_variable_count&&
               context->type_variable_bindings!=nullptr)
                bind_bound_node(binding,node,
                    &context->type_variable_bindings[variable],0);
            continue;
        }
        DiamondBoundTypeMember *member=binding_member(binding,node,known.id);
        if(member==nullptr)continue;
        if(known.argument_set!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)
                member->argument_node=binding_node(binding);
            if(member->argument_node!=UINT8_MAX)
                bind_context_set(binding,member->argument_node,context,sets,
                                 known.argument_set);
        }
        if(known.second_argument_set!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(binding);
            if(member->second_argument_node!=UINT8_MAX)
                bind_context_set(binding,member->second_argument_node,context,sets,
                                 known.second_argument_set);
        }
    }
}

static void infer_from_context_set(const DiamondChunk *known_context,
    const DiamondTypeSet *known_sets,uint8_t known_index,
    const DiamondTypeSet *expected_sets,uint8_t expected_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t target=0;target<expected->count;target++) {
        const DiamondTypeMember wanted=expected->members[target];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=
                (uint8_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)
                (void)binding_node(&bindings[variable]);
            bind_context_set(&bindings[variable],0,known_context,known_sets,
                             known_index);
            continue;
        }
        for(size_t source=0;source<known->count;source++) {
            const DiamondTypeMember actual=known->members[source];
            if(actual.id!=wanted.id)continue;
            if(wanted.argument_set!=UINT8_MAX&&actual.argument_set!=UINT8_MAX)
                infer_from_context_set(known_context,known_sets,
                    actual.argument_set,expected_sets,wanted.argument_set,bindings);
            if(wanted.second_argument_set!=UINT8_MAX&&
               actual.second_argument_set!=UINT8_MAX)
                infer_from_context_set(known_context,known_sets,
                    actual.second_argument_set,expected_sets,
                    wanted.second_argument_set,bindings);
        }
    }
}

static void infer_from_known_set(const DiamondChunk *chunk,
    const DiamondTypeSet *known_sets,uint8_t known_index,
    const DiamondTypeSet *expected_sets,uint8_t expected_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t target=0;target<expected->count;target++) {
        const DiamondTypeMember wanted=expected->members[target];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=
                (uint8_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)
                (void)binding_node(&bindings[variable]);
            bind_known_set(&bindings[variable],0,known_sets,known_index);
        } else {
            for(size_t source=0;source<known->count;source++) {
                const DiamondTypeMember actual=known->members[source];
                if(actual.id!=wanted.id)continue;
                if(wanted.argument_set!=UINT8_MAX&&actual.argument_set!=UINT8_MAX)
                    infer_from_known_set(chunk,known_sets,actual.argument_set,
                        expected_sets,wanted.argument_set,bindings);
                if(wanted.second_argument_set!=UINT8_MAX&&
                   actual.second_argument_set!=UINT8_MAX)
                    infer_from_known_set(chunk,known_sets,actual.second_argument_set,
                        expected_sets,wanted.second_argument_set,bindings);
            }
        }
    }
    (void)chunk;
}

static void infer_from_value(const DiamondChunk *chunk,DiamondValue value,
    const DiamondTypeSet *sets,uint8_t set_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember member=set->members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=(uint8_t)(member.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)(void)binding_node(&bindings[variable]);
            bind_value_graph(chunk,&bindings[variable],0,value);continue;
        }
        if(!value_matches_type(chunk,value,member.id))continue;
        if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_set!=UINT8_MAX) {
            const DiamondArray *array=(const DiamondArray *)value.as.object;
            for(size_t constraint=0;constraint<array->constraint_count;constraint++) {
                const typeof(array->constraints[0]) *known=
                    &array->constraints[constraint];
                const DiamondChunk context={.type_sets=known->type_sets,
                    .type_set_count=known->type_set_count,.classes=known->classes,
                    .class_count=known->class_count,.interfaces=known->interfaces,
                    .interface_count=known->interface_count,
                    .type_variable_bindings=known->type_variable_bindings,
                    .type_variable_count=known->type_variable_count};
                infer_from_context_set(&context,known->type_sets,known->set_index,
                    sets,member.argument_set,bindings);
            }
            for(size_t item=0;item<array->count;item++)
                infer_from_value(chunk,array->values[item],sets,
                                 member.argument_set,bindings);
        } else if(member.id==DIAMOND_TYPE_HASH&&member.argument_set!=UINT8_MAX&&
                  member.second_argument_set!=UINT8_MAX) {
            const DiamondHash *hash=(const DiamondHash *)value.as.object;
            for(size_t constraint=0;constraint<hash->constraint_count;constraint++) {
                const typeof(hash->constraints[0]) *known=
                    &hash->constraints[constraint];
                const DiamondChunk context={.type_sets=known->type_sets,
                    .type_set_count=known->type_set_count,.classes=known->classes,
                    .class_count=known->class_count,.interfaces=known->interfaces,
                    .interface_count=known->interface_count,
                    .type_variable_bindings=known->type_variable_bindings,
                    .type_variable_count=known->type_variable_count};
                infer_from_context_set(&context,known->type_sets,known->key_set,
                    sets,member.argument_set,bindings);
                infer_from_context_set(&context,known->type_sets,known->value_set,
                    sets,member.second_argument_set,bindings);
            }
            for(size_t item=0;item<hash->count;item++) {
                infer_from_value(chunk,hash->entries[item].key,sets,
                                 member.argument_set,bindings);
                infer_from_value(chunk,hash->entries[item].value,sets,
                                 member.second_argument_set,bindings);
            }
        } else if(member.id==DIAMOND_TYPE_CALLABLE) {
            const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
            if(closure->function_index<chunk->function_count) {
                const DiamondFunction *function=chunk->functions[closure->function_index];
                if(member.callable_parameters_typed)
                    for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                        const uint8_t actual=function->parameter_type_sets[parameter];
                        if(actual!=UINT8_MAX)
                            infer_from_known_set(chunk,function->type_sets,actual,
                                sets,member.callable_parameter_sets[parameter],bindings);
                    }
                if(function->return_type_set!=UINT8_MAX)
                    if(member.callable_return_set!=UINT8_MAX)
                    infer_from_known_set(chunk,function->type_sets,
                        function->return_type_set,sets,member.callable_return_set,
                        bindings);
            }
        }
    }
}

/* Split out of run_chunk's own switch (unlike GET_IVAR/GET_NAMESPACE_
 * CONSTANT and friends, which stay inline) specifically to keep run_chunk's
 * own per-call C stack frame from growing: run_chunk recurses in C for
 * every ordinary Diamond function call (DIAMOND_OP_CALL below), and
 * DIAMOND_MAX_CALL_DEPTH's whole guarantee -- catching runaway Diamond-
 * level recursion with a clean error before the real C stack does --
 * depends on that per-frame size times DIAMOND_MAX_CALL_DEPTH staying
 * safely under the OS stack limit. Confirmed empirically while adding
 * this: even a few bytes of extra locals declared directly in run_chunk's
 * own switch shifted a stack-depth regression test (tests/cases/
 * program_builder_call_declared_function.di's sibling deep-recursion
 * case) from a clean "call stack overflow" to a real ASan-caught
 * stack-overflow segfault -- that margin is thinner than it looks.
 * Locals declared inside a called helper live on the *helper's* frame,
 * popped the moment it returns, so they never accumulate across
 * DIAMOND_MAX_CALL_DEPTH levels of recursion the way a case-local do. */
static DiamondVmStatus get_cvar_helper(DiamondVm *vm,
        const DiamondChunk *chunk, uint16_t class_index, uint16_t slot,
        DiamondValue *out) {
    if(class_index>=chunk->class_count||
       slot>=chunk->classes[class_index].class_variable_count)
        return DIAMOND_VM_INVALID_BYTECODE;
    /* A read before any write anywhere in this VM: no slot has ever been
     * allocated, so the value is definitionally the class variable
     * default (nil) -- allocating here just to immediately read back nil
     * would be pure waste, so skip it and return the default directly. */
    if(vm->class_variables==nullptr) {
        *out=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    *out=vm->class_variables[(size_t)class_index*DIAMOND_MAX_FIELDS+slot];
    return DIAMOND_VM_OK;
}

static DiamondVmStatus set_cvar_helper(DiamondVm *vm,const DiamondChunk *chunk,
        uint16_t class_index,uint16_t slot,DiamondValue value) {
    if(class_index>=chunk->class_count||
       slot>=chunk->classes[class_index].class_variable_count)
        return DIAMOND_VM_INVALID_BYTECODE;
    if(vm->class_variables==nullptr) {
        vm->class_variables=calloc((size_t)DIAMOND_MAX_CLASSES*DIAMOND_MAX_FIELDS,
            sizeof(DiamondValue));
        if(vm->class_variables==nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    }
    vm->class_variables[(size_t)class_index*DIAMOND_MAX_FIELDS+slot]=value;
    return DIAMOND_VM_OK;
}

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
static DiamondVmStatus call_closure_helper(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondFunction *fn,const DiamondClosure *called,
        const DiamondValue *registers,uint16_t base,uint8_t argc,size_t depth,
        DiamondValue *result) {
    const bool needs_self_slot=fn->owner_class!=UINT8_MAX;
    DiamondValue call_arguments[17];
    const DiamondValue *arguments=&registers[base];
    size_t argument_count=argc;
    if(needs_self_slot) {
        if(argc>16) return DIAMOND_VM_ARITY_ERROR;
        call_arguments[0]=DIAMOND_NIL;
        for(size_t index=0;index<argc;index++)
            call_arguments[index+1]=registers[(size_t)base+index];
        arguments=call_arguments;
        argument_count=(size_t)argc+1;
    }
    if(argument_count<fn->required_arity||argument_count>fn->arity)
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
      .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
      .register_count=fn->register_count};
    return run_chunk(&child,vm,arguments,argument_count,depth+1,called,result);
}

/* Bind an Array of Diamond values positionally (1-indexed, sqlite3's own
 * convention for '?' placeholders) to `stmt`. A parameter-count mismatch
 * is DIAMOND_VM_ARITY_ERROR (surfaces as ArgumentError, the same class
 * an ordinary wrong-argument-count call already gets) rather than
 * SQLite3Error, since it's a Diamond-level call-shape mistake, not
 * anything sqlite3 itself rejected; an unsupported Diamond value type is
 * DIAMOND_VM_TYPE_ERROR for the same reason. Only a genuine
 * sqlite3_bind_* failure (rare -- effectively just OOM) is
 * DIAMOND_VM_SQLITE3_ERROR. No Diamond allocation happens anywhere in
 * here, so there's no GC-rooting concern for `values`. */
static DiamondVmStatus sqlite3_bind_params_helper(DiamondVm *vm,sqlite3_stmt *stmt,
        const DiamondValue *values,size_t count) {
    const int expected=sqlite3_bind_parameter_count(stmt);
    if(count!=(size_t)expected) {
        snprintf(vm->error,sizeof vm->error,
            "SQLite3 statement expects %d bound parameter(s), got %zu",expected,count);
        return DIAMOND_VM_ARITY_ERROR;
    }
    for(size_t index=0;index<count;index++) {
        const DiamondValue value=values[index];
        const int position=(int)index+1;
        int rc=SQLITE_OK;
        if(value.kind==DIAMOND_VALUE_NIL) {
            rc=sqlite3_bind_null(stmt,position);
        } else if(value.kind==DIAMOND_VALUE_INT) {
            rc=sqlite3_bind_int64(stmt,position,value.as.integer);
        } else if(value.kind==DIAMOND_VALUE_FLOAT) {
            rc=sqlite3_bind_double(stmt,position,value.as.real);
        } else if(value.kind==DIAMOND_VALUE_BOOL) {
            rc=sqlite3_bind_int64(stmt,position,value.as.boolean?1:0);
        } else if(value.kind==DIAMOND_VALUE_OBJECT&&
                  value.as.object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)value.as.object;
            rc=sqlite3_bind_text(stmt,position,string->chars,
                (int)string->length,SQLITE_TRANSIENT);
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unsupported SQLite3 parameter type at position %d",position);
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(rc!=SQLITE_OK) {
            snprintf(vm->error,sizeof vm->error,"%s",
                sqlite3_errmsg(sqlite3_db_handle(stmt)));
            return DIAMOND_VM_SQLITE3_ERROR;
        }
    }
    return DIAMOND_VM_OK;
}

/* Shared prepare+single-statement-guard+bind step behind #execute and
 * #query. sqlite3_prepare_v2 only compiles the first statement up to a
 * ';' and leaves *pzTail pointing at whatever follows -- silently
 * ignoring a second statement chained after it would be a real
 * correctness trap, so anything left in the tail besides trailing
 * whitespace is rejected outright rather than dropped. On any failure
 * path the statement is finalized here (never left for the caller to
 * clean up), so a caller only ever needs to finalize the stmt it
 * actually got back on DIAMOND_VM_OK. */
static DiamondVmStatus sqlite3_prepare_helper(DiamondVm *vm,sqlite3 *db,
        const DiamondString *sql,const DiamondValue *param_values,size_t param_count,
        sqlite3_stmt **out_stmt) {
    sqlite3_stmt *stmt=nullptr;
    const char *tail=nullptr;
    const int rc=sqlite3_prepare_v2(db,sql->chars,(int)sql->length,&stmt,&tail);
    if(rc!=SQLITE_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",sqlite3_errmsg(db));
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    if(stmt==nullptr) {
        snprintf(vm->error,sizeof vm->error,"SQLite3: empty SQL statement");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    const char *cursor=tail;
    while(*cursor==' '||*cursor=='\t'||*cursor=='\n'||*cursor=='\r')cursor++;
    if(*cursor!='\0') {
        sqlite3_finalize(stmt);
        snprintf(vm->error,sizeof vm->error,
            "SQLite3#execute/#query only support one statement per call");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    const DiamondVmStatus bind_status=
        sqlite3_bind_params_helper(vm,stmt,param_values,param_count);
    if(bind_status!=DIAMOND_VM_OK) {
        sqlite3_finalize(stmt);
        return bind_status;
    }
    *out_stmt=stmt;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus sqlite3_execute_helper(DiamondVm *vm,DiamondSqlite3Handle *handle,
        const DiamondString *sql,const DiamondValue *param_values,size_t param_count,
        DiamondValue *result) {
    sqlite3_stmt *stmt=nullptr;
    const DiamondVmStatus prepare_status=
        sqlite3_prepare_helper(vm,handle->db,sql,param_values,param_count,&stmt);
    if(prepare_status!=DIAMOND_VM_OK)return prepare_status;
    int rc=sqlite3_step(stmt);
    while(rc==SQLITE_ROW)rc=sqlite3_step(stmt); /* discard any rows */
    if(rc!=SQLITE_DONE) {
        snprintf(vm->error,sizeof vm->error,"%s",sqlite3_errmsg(handle->db));
        sqlite3_finalize(stmt);
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    sqlite3_finalize(stmt);
    *result=DIAMOND_INT(sqlite3_changes(handle->db));
    return DIAMOND_VM_OK;
}

/* SQLITE_TEXT/SQLITE_BLOB both go through allocate_string -- a Diamond
 * String is already a raw byte buffer, not UTF-8-validated, so a blob's
 * raw bytes need no separate representation. A zero-length text/blob's
 * native pointer can be nullptr; allocate_string's memcpy(dest,NULL,0)
 * would be UB even though every real implementation treats it as a
 * no-op, so it's substituted with "" rather than relying on that. */
static bool sqlite3_column_value_helper(DiamondVm *vm,sqlite3_stmt *stmt,int column,
        DiamondValue *out) {
    switch(sqlite3_column_type(stmt,column)) {
        case SQLITE_INTEGER:
            *out=DIAMOND_INT(sqlite3_column_int64(stmt,column));return true;
        case SQLITE_FLOAT:
            *out=DIAMOND_FLOAT(sqlite3_column_double(stmt,column));return true;
        case SQLITE_NULL:
            *out=DIAMOND_NIL;return true;
        case SQLITE_TEXT: {
            const char *text=(const char *)sqlite3_column_text(stmt,column);
            const size_t length=(size_t)sqlite3_column_bytes(stmt,column);
            DiamondString *string=allocate_string(vm,text!=nullptr?text:"",length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);return true;
        }
        case SQLITE_BLOB: {
            const char *bytes=(const char *)sqlite3_column_blob(stmt,column);
            const size_t length=(size_t)sqlite3_column_bytes(stmt,column);
            DiamondString *string=allocate_string(vm,bytes!=nullptr?bytes:"",length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);return true;
        }
        default:
            *out=DIAMOND_NIL;return true;
    }
}

/* Unlike every other helper in this file (regexp_new_helper,
 * call_closure_helper, ...), `result` here MUST be a pointer into the
 * caller's live `registers` array (i.e. the caller passes
 * &registers[dest] directly, not an intermediate local later copied
 * in) -- this helper allocates repeatedly (one Hash per row, one String
 * per key and per Text/Blob column) while building the result, and only
 * an allocation already reachable from a genuine GC root survives a
 * collection triggered by a *later* allocation in that same sequence.
 * `*result` is written to hold the outer Array immediately, before any
 * further allocation, exactly like DIAMOND_OP_IO_POLL/UDPSocket#receive's
 * own Hash result (see their shared comment) -- and every row Hash is
 * pushed onto that already-rooted Array *before* its own columns are
 * filled in (array_push never itself allocates a Diamond object, only
 * reallocates the Array's own backing store via plain realloc, so this
 * costs nothing), so a row is reachable through the rooted Array for the
 * entire time its columns are being built up one allocation at a time. */
static DiamondVmStatus sqlite3_query_helper(DiamondVm *vm,DiamondSqlite3Handle *handle,
        const DiamondString *sql,const DiamondValue *param_values,size_t param_count,
        DiamondValue *result) {
    sqlite3_stmt *stmt=nullptr;
    const DiamondVmStatus prepare_status=
        sqlite3_prepare_helper(vm,handle->db,sql,param_values,param_count,&stmt);
    if(prepare_status!=DIAMOND_VM_OK)return prepare_status;
    DiamondArray *rows=allocate_array(vm,nullptr,0);
    if(rows==nullptr) {
        sqlite3_finalize(stmt);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(rows);
    const int column_count=sqlite3_column_count(stmt);
    for(;;) {
        const int rc=sqlite3_step(stmt);
        if(rc==SQLITE_DONE)break;
        if(rc!=SQLITE_ROW) {
            snprintf(vm->error,sizeof vm->error,"%s",sqlite3_errmsg(handle->db));
            sqlite3_finalize(stmt);
            return DIAMOND_VM_SQLITE3_ERROR;
        }
        DiamondHash *row=allocate_hash(vm);
        if(row==nullptr) {
            sqlite3_finalize(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(!array_push(vm,rows,DIAMOND_OBJECT(row))) {
            sqlite3_finalize(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        for(int column=0;column<column_count;column++) {
            const char *column_name=sqlite3_column_name(stmt,column);
            DiamondString *key=allocate_string(vm,column_name,strlen(column_name));
            if(key==nullptr) {
                sqlite3_finalize(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row,DIAMOND_OBJECT(key),DIAMOND_NIL)) {
                sqlite3_finalize(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            DiamondValue column_value=DIAMOND_NIL;
            if(!sqlite3_column_value_helper(vm,stmt,column,&column_value)) {
                sqlite3_finalize(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row,DIAMOND_OBJECT(key),column_value)) {
                sqlite3_finalize(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
        }
    }
    sqlite3_finalize(stmt);
    return DIAMOND_VM_OK;
}

/* All of #execute/#query/#last_insert_row_id/#close's method-name
 * comparison and argument marshaling, factored out of the INVOKE case
 * body for the same reason get_cvar_helper/call_closure_helper/
 * regexp_new_helper already are: every local declared anywhere in
 * run_chunk's own switch adds to its one shared per-call stack frame at
 * -O0 regardless of which case actually runs (run_chunk recurses in C
 * for every Diamond-level call), and this dispatch alone -- four method-
 * name bools, a handle pointer, a sql pointer, a params pointer/count --
 * was enough on its own to reopen the exact DIAMOND_MAX_CALL_DEPTH/ASan
 * margin regression documented elsewhere in this codebase (confirmed the
 * hard way: `depth(5000)` overflowed the real C stack under
 * -fsanitize=address before this was pulled out). `registers`/`base`/
 * `dest` are passed through unchanged, so sqlite3_query_helper's own
 * &registers[dest] GC-rooting still targets the real per-frame register
 * array either way -- moving this dispatch into its own function changes
 * nothing about which array that pointer refers to. */
static DiamondVmStatus sqlite3_dispatch_helper(DiamondVm *vm,DiamondSqlite3Handle *target_db,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool last_insert_row_id_method=method_name->length==18&&
        memcmp(method_name->chars,"last_insert_row_id",18)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!last_insert_row_id_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"SQLite3");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_db->db!=nullptr) {
            sqlite3_close(target_db->db);
            target_db->db=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_db->db==nullptr) {
        snprintf(vm->error,sizeof vm->error,"SQLite3 connection is closed");
        return DIAMOND_VM_SQLITE3_ERROR;
    }
    if(last_insert_row_id_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT(sqlite3_last_insert_rowid(target_db->db));
        return DIAMOND_VM_OK;
    }
    /* execute/query share the same argument shape: (sql) or (sql, params). */
    if(argc!=1&&argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"SQLite3#%.*s's sql argument must be a String",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *sql=(const DiamondString *)registers[base].as.object;
    const DiamondValue *param_values=nullptr;
    size_t param_count=0;
    if(argc==2) {
        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,
                "SQLite3#%.*s's params argument must be an Array",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *params=(const DiamondArray *)registers[(size_t)base+1].as.object;
        param_values=params->values;
        param_count=params->count;
    }
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=sqlite3_execute_helper(vm,
            target_db,sql,param_values,param_count,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: sqlite3_query_helper writes directly into
     * registers[dest] (a real GC root), not a local -- see its own
     * comment. */
    return sqlite3_query_helper(vm,target_db,sql,param_values,param_count,&registers[dest]);
}

/* Postgres OIDs for the column types this driver decodes into a native
 * Diamond type below. libpq-devel doesn't ship pg_type.h (that lives in
 * postgresql-server-devel, a separate package this driver doesn't
 * require) -- these are Postgres's own well-known, long-stable builtin
 * type OIDs, hardcoded directly rather than pulling in a whole extra
 * dependency for ten integer constants. Anything else (date/timestamp/
 * json/jsonb/uuid/bytea/arrays/...) decodes as the raw text libpq
 * already returns -- an explicit, documented scope cut, not silent data
 * loss (bytea in particular stays Postgres's default hex-text spelling,
 * not raw bytes). */
#define DIAMOND_PG_BOOLOID 16
#define DIAMOND_PG_INT8OID 20
#define DIAMOND_PG_INT2OID 21
#define DIAMOND_PG_INT4OID 23
#define DIAMOND_PG_FLOAT4OID 700
#define DIAMOND_PG_FLOAT8OID 701
#define DIAMOND_PG_NUMERICOID 1700

/* `?` -> `$1`/`$2`/... translation: SQLite3's own placeholder spelling is
 * kept at the Diamond level for API/Arel-adapter consistency even though
 * libpq's PQexecParams requires numbered placeholders. A `?` inside a
 * single-quoted string literal (`''` is the standard SQL escaped quote)
 * is left alone; nothing else is -- a literal `?` used outside a string
 * (Postgres's own JSONB "key exists" operator, for instance) isn't
 * distinguishable from a placeholder here and isn't supported through
 * the params-array call form in this first slice. Two-pass (count
 * placeholders, then allocate exactly and fill) rather than repeated
 * reallocation, the same one-allocation stance the rest of this codebase
 * already takes for string building. Returns nullptr only on OOM. */
static char *postgres_translate_placeholders_helper(const char *sql,size_t length,
        size_t *out_count) {
    size_t placeholder_count=0;
    bool in_string=false;
    for(size_t index=0;index<length;index++) {
        const char ch=sql[index];
        if(in_string) {
            if(ch=='\'') {
                if(index+1<length&&sql[index+1]=='\'')index++;
                else in_string=false;
            }
        } else if(ch=='\'') {
            in_string=true;
        } else if(ch=='?') {
            placeholder_count++;
        }
    }
    /* Each `?` (1 char) becomes `$` plus up to 10 digits -- far more
     * headroom than any realistic placeholder count needs, sized once. */
    const size_t max_extra_per_placeholder=10;
    char *out=malloc(length+placeholder_count*max_extra_per_placeholder+1);
    if(out==nullptr)return nullptr;
    size_t out_index=0,placeholder_number=0;
    in_string=false;
    for(size_t index=0;index<length;index++) {
        const char ch=sql[index];
        if(in_string) {
            out[out_index++]=ch;
            if(ch=='\'') {
                if(index+1<length&&sql[index+1]=='\'')out[out_index++]=sql[++index];
                else in_string=false;
            }
        } else if(ch=='\'') {
            in_string=true;out[out_index++]=ch;
        } else if(ch=='?') {
            placeholder_number++;
            out_index+=(size_t)snprintf(out+out_index,max_extra_per_placeholder+1,
                "$%zu",placeholder_number);
        } else {
            out[out_index++]=ch;
        }
    }
    out[out_index]='\0';
    *out_count=placeholder_count;
    return out;
}

static void postgres_free_params_helper(char **param_values,size_t count) {
    if(param_values==nullptr)return;
    for(size_t index=0;index<count;index++)free(param_values[index]);
    free(param_values);
}

/* Converts an Array of Diamond values into a NUL-terminated C string
 * array for PQexecParams's text-format parameter list (a Nil entry stays
 * a null pointer, PQexecParams's own spelling of SQL NULL); the caller
 * frees it via postgres_free_params_helper. Deliberately mirrors
 * sqlite3_bind_params_helper's exact same type-support boundary (Nil/
 * Int/Float/Bool/String only, no Array/Hash/Instance/bignum-promoted
 * Int) for consistency between the two drivers. */
static DiamondVmStatus postgres_bind_params_helper(DiamondVm *vm,
        const DiamondValue *values,size_t count,char ***out_values) {
    char **param_values=count==0?nullptr:calloc(count,sizeof(char *));
    if(count>0&&param_values==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<count;index++) {
        const DiamondValue value=values[index];
        if(value.kind==DIAMOND_VALUE_NIL)continue; /* stays nullptr = SQL NULL */
        char buffer[64];
        if(value.kind==DIAMOND_VALUE_INT) {
            snprintf(buffer,sizeof buffer,"%" PRId64,value.as.integer);
        } else if(value.kind==DIAMOND_VALUE_FLOAT) {
            if(isnan(value.as.real))snprintf(buffer,sizeof buffer,"NaN");
            else if(isinf(value.as.real))
                snprintf(buffer,sizeof buffer,"%s",value.as.real<0?"-Infinity":"Infinity");
            else snprintf(buffer,sizeof buffer,"%.17g",value.as.real);
        } else if(value.kind==DIAMOND_VALUE_BOOL) {
            snprintf(buffer,sizeof buffer,"%s",value.as.boolean?"true":"false");
        } else if(value.kind==DIAMOND_VALUE_OBJECT&&
                  value.as.object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)value.as.object;
            char *copy=malloc(string->length+1);
            if(copy==nullptr) {
                postgres_free_params_helper(param_values,count);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            memcpy(copy,string->chars,string->length);
            copy[string->length]='\0';
            param_values[index]=copy;
            continue;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unsupported PostgreSQL parameter type at position %zu",index+1);
            postgres_free_params_helper(param_values,count);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const size_t buffer_length=strlen(buffer);
        char *copy=malloc(buffer_length+1);
        if(copy==nullptr) {
            postgres_free_params_helper(param_values,count);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        memcpy(copy,buffer,buffer_length+1);
        param_values[index]=copy;
    }
    *out_values=param_values;
    return DIAMOND_VM_OK;
}

/* PQgetvalue's text-format output decoded to the matching native Diamond
 * type for the well-known OIDs above; anything else stays the raw String
 * libpq already returns. Only the String-allocation path can fail (OOM),
 * matching sqlite3_column_value_helper's own bool-return/out-param shape.
 * INT2/INT4/INT8OID are all guaranteed to fit int64_t by Postgres's own
 * type bounds, so unlike String#to_i there's no bignum-overflow case to
 * handle here; NUMERIC has no such bound but is deliberately decoded as
 * Float (lossy for values outside double precision), not Int. */
static bool postgres_decode_value_helper(DiamondVm *vm,PGresult *res,int row,int col,
        DiamondValue *out) {
    if(PQgetisnull(res,row,col)) {
        *out=DIAMOND_NIL;return true;
    }
    const char *text=PQgetvalue(res,row,col);
    switch(PQftype(res,col)) {
        case DIAMOND_PG_BOOLOID:
            *out=DIAMOND_BOOL(text[0]=='t');return true;
        case DIAMOND_PG_INT2OID:
        case DIAMOND_PG_INT4OID:
        case DIAMOND_PG_INT8OID:
            *out=DIAMOND_INT(strtoll(text,nullptr,10));return true;
        case DIAMOND_PG_FLOAT4OID:
        case DIAMOND_PG_FLOAT8OID:
        case DIAMOND_PG_NUMERICOID:
            *out=DIAMOND_FLOAT(strtod(text,nullptr));return true;
        default: {
            const size_t length=(size_t)PQgetlength(res,row,col);
            DiamondString *string=allocate_string(vm,text,length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);return true;
        }
    }
}

/* Shared translate+bind+exec step behind #execute/#query/the internal
 * SELECT lastval() #last_insert_row_id() runs. Unlike
 * sqlite3_prepare_helper, no manual multiple-statements guard is needed:
 * PQexecParams itself refuses more than one SQL command regardless of
 * parameter count, surfacing that as an ordinary error PGresult this
 * function already turns into DIAMOND_VM_POSTGRES_ERROR. */
static DiamondVmStatus postgres_exec_helper(DiamondVm *vm,DiamondPostgresHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,PGresult **out_res) {
    size_t placeholder_count=0;
    char *translated=postgres_translate_placeholders_helper(sql_chars,sql_length,
        &placeholder_count);
    if(translated==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(param_count!=placeholder_count) {
        free(translated);
        snprintf(vm->error,sizeof vm->error,
            "PostgreSQL statement expects %zu bound parameter(s), got %zu",
            placeholder_count,param_count);
        return DIAMOND_VM_ARITY_ERROR;
    }
    char **bound_values=nullptr;
    const DiamondVmStatus bind_status=
        postgres_bind_params_helper(vm,param_values,param_count,&bound_values);
    if(bind_status!=DIAMOND_VM_OK) {
        free(translated);
        return bind_status;
    }
    PGresult *res=PQexecParams(handle->conn,translated,(int)param_count,nullptr,
        (const char *const *)bound_values,nullptr,nullptr,0);
    free(translated);
    postgres_free_params_helper(bound_values,param_count);
    if(res==nullptr) {
        snprintf(vm->error,sizeof vm->error,"%s",PQerrorMessage(handle->conn));
        return DIAMOND_VM_POSTGRES_ERROR;
    }
    const ExecStatusType status=PQresultStatus(res);
    if(status!=PGRES_TUPLES_OK&&status!=PGRES_COMMAND_OK) {
        snprintf(vm->error,sizeof vm->error,"%s",PQresultErrorMessage(res));
        PQclear(res);
        return DIAMOND_VM_POSTGRES_ERROR;
    }
    *out_res=res;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus postgres_execute_helper(DiamondVm *vm,DiamondPostgresHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    PGresult *res=nullptr;
    const DiamondVmStatus status=
        postgres_exec_helper(vm,handle,sql_chars,sql_length,param_values,param_count,&res);
    if(status!=DIAMOND_VM_OK)return status;
    const char *affected=PQcmdTuples(res);
    *result=DIAMOND_INT(affected[0]=='\0'?0:strtoll(affected,nullptr,10));
    PQclear(res);
    return DIAMOND_VM_OK;
}

/* Same GC-rooting discipline as sqlite3_query_helper: `result` must be a
 * pointer into the caller's live registers array, `*result` is written
 * to hold the outer Array before any further allocation, and each row's
 * Hash is pushed onto that already-rooted Array (then each key set to
 * Nil first, rooting the key before decoding may itself allocate a
 * String) before its real column values are filled in one at a time. */
static DiamondVmStatus postgres_query_helper(DiamondVm *vm,DiamondPostgresHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    PGresult *res=nullptr;
    const DiamondVmStatus status=
        postgres_exec_helper(vm,handle,sql_chars,sql_length,param_values,param_count,&res);
    if(status!=DIAMOND_VM_OK)return status;
    DiamondArray *rows=allocate_array(vm,nullptr,0);
    if(rows==nullptr) {
        PQclear(res);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(rows);
    const int row_count=PQntuples(res);
    const int column_count=PQnfields(res);
    for(int row=0;row<row_count;row++) {
        DiamondHash *row_hash=allocate_hash(vm);
        if(row_hash==nullptr) {
            PQclear(res);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(!array_push(vm,rows,DIAMOND_OBJECT(row_hash))) {
            PQclear(res);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        for(int col=0;col<column_count;col++) {
            const char *column_name=PQfname(res,col);
            DiamondString *key=allocate_string(vm,column_name,strlen(column_name));
            if(key==nullptr) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),DIAMOND_NIL)) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            DiamondValue column_value=DIAMOND_NIL;
            if(!postgres_decode_value_helper(vm,res,row,col,&column_value)) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),column_value)) {
                PQclear(res);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
        }
    }
    PQclear(res);
    return DIAMOND_VM_OK;
}

/* #execute/#query/#last_insert_row_id/#close -- factored out of the
 * INVOKE case body for the same stack-frame reason sqlite3_dispatch_
 * helper's own comment explains. #last_insert_row_id runs `SELECT
 * lastval()` through postgres_query_helper and unwraps the single Int
 * cell; it fails with a real PostgreSQLError (lastval's own "not yet
 * defined in this session" condition) if no sequence has been used yet
 * on this connection, the same honest-failure spirit as everything else
 * this driver raises. */
static DiamondVmStatus postgres_dispatch_helper(DiamondVm *vm,DiamondPostgresHandle *target_db,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool last_insert_row_id_method=method_name->length==18&&
        memcmp(method_name->chars,"last_insert_row_id",18)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!last_insert_row_id_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"PostgreSQL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_db->conn!=nullptr) {
            PQfinish(target_db->conn);
            target_db->conn=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_db->conn==nullptr) {
        snprintf(vm->error,sizeof vm->error,"PostgreSQL connection is closed");
        return DIAMOND_VM_POSTGRES_ERROR;
    }
    if(last_insert_row_id_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        static const char lastval_sql[]="SELECT lastval()";
        DiamondValue rows=DIAMOND_NIL;
        const DiamondVmStatus query_status=postgres_query_helper(vm,target_db,
            lastval_sql,sizeof lastval_sql-1,nullptr,0,&rows);
        if(query_status!=DIAMOND_VM_OK)return query_status;
        const DiamondArray *row_array=(const DiamondArray *)rows.as.object;
        const DiamondHash *row=(const DiamondHash *)row_array->values[0].as.object;
        registers[dest]=row->entries[0].value;
        return DIAMOND_VM_OK;
    }
    /* execute/query share the same argument shape: (sql) or (sql, params). */
    if(argc!=1&&argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"PostgreSQL#%.*s's sql argument must be a String",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *sql=(const DiamondString *)registers[base].as.object;
    const DiamondValue *param_values=nullptr;
    size_t param_count=0;
    if(argc==2) {
        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,
                "PostgreSQL#%.*s's params argument must be an Array",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *params=(const DiamondArray *)registers[(size_t)base+1].as.object;
        param_values=params->values;
        param_count=params->count;
    }
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=postgres_execute_helper(vm,
            target_db,sql->chars,sql->length,param_values,param_count,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: postgres_query_helper writes directly into
     * registers[dest] (a real GC root), not a local -- see its own
     * comment. */
    return postgres_query_helper(vm,target_db,sql->chars,sql->length,param_values,
        param_count,&registers[dest]);
}

/* Fixed-width storage for a bound parameter's native value -- unlike
 * postgres_bind_params_helper (which must copy into freshly malloc'd,
 * NUL-terminated C strings for PQexecParams's text-format protocol),
 * MYSQL_BIND takes an explicit buffer_length for every type, so a String
 * parameter's buffer can point directly at the DiamondString's own
 * `chars` (safe here specifically because nothing between building this
 * array and mysql_stmt_execute consuming it can allocate and trigger a
 * GC pass -- the DiamondString stays reachable from the live `registers`
 * root throughout). Only Int/Float/Bool need an addressable copy of
 * their own, since DiamondValue's own layout isn't what MYSQL_BIND wants
 * pointed at. */
typedef union DiamondMysqlParamStorage {
    int64_t as_int64;
    double as_double;
    signed char as_tiny;
} DiamondMysqlParamStorage;

/* Builds a MYSQL_BIND array (plus the storage array backing its non-String
 * buffers) for mysql_stmt_bind_param -- mirrors postgres_bind_params_helper's
 * exact same type-support boundary (Nil/Int/Float/Bool/String only) for
 * consistency between the two drivers. Caller frees both arrays once
 * mysql_stmt_execute has consumed them. */
static DiamondVmStatus mysql_bind_params_helper(DiamondVm *vm,
        const DiamondValue *values,size_t count,MYSQL_BIND **out_binds,
        DiamondMysqlParamStorage **out_storage) {
    MYSQL_BIND *binds=count==0?nullptr:calloc(count,sizeof(MYSQL_BIND));
    DiamondMysqlParamStorage *storage=
        count==0?nullptr:calloc(count,sizeof(DiamondMysqlParamStorage));
    if(count>0&&(binds==nullptr||storage==nullptr)) {
        free(binds);free(storage);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    for(size_t index=0;index<count;index++) {
        const DiamondValue value=values[index];
        if(value.kind==DIAMOND_VALUE_NIL) {
            binds[index].buffer_type=MYSQL_TYPE_NULL;
        } else if(value.kind==DIAMOND_VALUE_INT) {
            storage[index].as_int64=value.as.integer;
            binds[index].buffer_type=MYSQL_TYPE_LONGLONG;
            binds[index].buffer=&storage[index].as_int64;
        } else if(value.kind==DIAMOND_VALUE_FLOAT) {
            storage[index].as_double=value.as.real;
            binds[index].buffer_type=MYSQL_TYPE_DOUBLE;
            binds[index].buffer=&storage[index].as_double;
        } else if(value.kind==DIAMOND_VALUE_BOOL) {
            storage[index].as_tiny=value.as.boolean?1:0;
            binds[index].buffer_type=MYSQL_TYPE_TINY;
            binds[index].buffer=&storage[index].as_tiny;
        } else if(value.kind==DIAMOND_VALUE_OBJECT&&
                  value.as.object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *string=(const DiamondString *)value.as.object;
            binds[index].buffer_type=MYSQL_TYPE_STRING;
            binds[index].buffer=(void *)string->chars;
            binds[index].buffer_length=(unsigned long)string->length;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unsupported MySQL parameter type at position %zu",index+1);
            free(binds);free(storage);
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    *out_binds=binds;
    *out_storage=storage;
    return DIAMOND_VM_OK;
}

/* PQftype's role for this driver: the field metadata type this column's
 * fetched string bytes should decode into. Every output column is bound
 * as MYSQL_TYPE_STRING (see mysql_query_helper) so the server itself
 * always hands back a text representation regardless of the column's real
 * wire type -- fields[col].type (captured before that rebinding) is the
 * only place the original type survives. Same documented scope cut as
 * postgres_decode_value_helper: anything not a recognized integer or
 * floating type stays the raw String already fetched (dates/times/blobs/
 * json included). MySQL has no native boolean type -- TINYINT(1) is only
 * a convention, indistinguishable at the protocol level from any other
 * TINYINT -- so unlike Postgres's real BOOLOID, every integer type here
 * decodes as Int, matching sqlite3_column_value_helper's same tradeoff. */
static bool mysql_decode_value_helper(DiamondVm *vm,const MYSQL_FIELD *field,
        const char *bytes,unsigned long length,DiamondValue *out) {
    switch(field->type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR: {
            char buffer[32];
            const size_t copy_length=length<sizeof buffer-1?length:sizeof buffer-1;
            memcpy(buffer,bytes,copy_length);buffer[copy_length]='\0';
            *out=DIAMOND_INT(strtoll(buffer,nullptr,10));
            return true;
        }
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL: {
            char buffer[64];
            const size_t copy_length=length<sizeof buffer-1?length:sizeof buffer-1;
            memcpy(buffer,bytes,copy_length);buffer[copy_length]='\0';
            *out=DIAMOND_FLOAT(strtod(buffer,nullptr));
            return true;
        }
        default: {
            DiamondString *string=allocate_string(vm,bytes,(size_t)length);
            if(string==nullptr)return false;
            *out=DIAMOND_OBJECT(string);
            return true;
        }
    }
}

/* Shared prepare+bind+execute step behind #execute/#query. Unlike
 * postgres_exec_helper, no `?` -> `$N` placeholder translation is needed
 * -- MySQL's own prepared-statement placeholder spelling already is `?`.
 * mysql_stmt_param_count(stmt) after a successful prepare gives an exact
 * placeholder count to validate the caller's params Array against, the
 * same arity check postgres_exec_helper derives from its own translation
 * pass. No manual multiple-statement guard is needed either: this
 * connection is never opened with CLIENT_MULTI_STATEMENTS (see
 * DIAMOND_OP_MYSQL_OPEN), so a semicolon-separated second statement is
 * simply a syntax error from mysql_stmt_prepare itself. */
static DiamondVmStatus mysql_exec_helper(DiamondVm *vm,DiamondMysqlHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,MYSQL_STMT **out_stmt) {
    MYSQL_STMT *stmt=mysql_stmt_init(handle->conn);
    if(stmt==nullptr) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_error(handle->conn));
        return DIAMOND_VM_MYSQL_ERROR;
    }
    if(mysql_stmt_prepare(stmt,sql_chars,(unsigned long)sql_length)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    const unsigned long placeholder_count=mysql_stmt_param_count(stmt);
    if((unsigned long)param_count!=placeholder_count) {
        snprintf(vm->error,sizeof vm->error,
            "MySQL statement expects %lu bound parameter(s), got %zu",
            placeholder_count,param_count);
        mysql_stmt_close(stmt);
        return DIAMOND_VM_ARITY_ERROR;
    }
    if(param_count>0) {
        MYSQL_BIND *binds=nullptr;
        DiamondMysqlParamStorage *storage=nullptr;
        const DiamondVmStatus bind_status=
            mysql_bind_params_helper(vm,param_values,param_count,&binds,&storage);
        if(bind_status!=DIAMOND_VM_OK) {
            mysql_stmt_close(stmt);
            return bind_status;
        }
        if(mysql_stmt_bind_param(stmt,binds)!=0) {
            snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
            free(binds);free(storage);
            mysql_stmt_close(stmt);
            return DIAMOND_VM_MYSQL_ERROR;
        }
        const int execute_result=mysql_stmt_execute(stmt);
        free(binds);free(storage);
        if(execute_result!=0) {
            snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
            mysql_stmt_close(stmt);
            return DIAMOND_VM_MYSQL_ERROR;
        }
    } else if(mysql_stmt_execute(stmt)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    *out_stmt=stmt;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus mysql_execute_helper(DiamondVm *vm,DiamondMysqlHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    MYSQL_STMT *stmt=nullptr;
    const DiamondVmStatus status=mysql_exec_helper(vm,handle,sql_chars,sql_length,
        param_values,param_count,&stmt);
    if(status!=DIAMOND_VM_OK)return status;
    *result=DIAMOND_INT((int64_t)mysql_stmt_affected_rows(stmt));
    mysql_stmt_close(stmt);
    return DIAMOND_VM_OK;
}

/* Same GC-rooting discipline as postgres_query_helper: `result` must be a
 * pointer into the caller's live registers array, `*result` is written to
 * hold the outer Array before any further allocation, and each row's Hash
 * is pushed onto that already-rooted Array (then each key set to Nil
 * first, rooting the key before decoding may itself allocate a String)
 * before its real column values are filled in one at a time.
 *
 * Unlike libpq's text protocol (PQgetvalue returns every column's value
 * directly, already materialized), the prepared-statement binary protocol
 * needs output buffers bound before fetching -- and this driver doesn't
 * know column widths ahead of time (these are ad hoc queries, not a fixed
 * schema). So every column is bound once as MYSQL_TYPE_STRING with a null
 * buffer/zero buffer_length purely to receive each row's true length via
 * mysql_stmt_fetch (which reports MYSQL_DATA_TRUNCATED, expected here,
 * not a real error) and its null flag; the actual bytes for a non-null
 * column are then pulled per cell via mysql_stmt_fetch_column into a
 * freshly sized buffer. This is the standard two-phase dynamic-length
 * fetch pattern for the MySQL C API's binary protocol. */
/* Fixed-width result types this driver gives a small inline buffer
 * directly in the one mysql_stmt_bind_result call, rather than the
 * null-buffer-probe-then-mysql_stmt_fetch_column two-phase dance query
 * helper below uses for genuinely unbounded types (STRING/VAR_STRING/
 * BLOB/dates/...). Both approaches were tried directly against a live
 * MariaDB server: the null-buffer probe reliably reports a truncated
 * column's true length for STRING-family columns, but *not* for these
 * fixed-width numeric ones -- `*length` came back wrong (observed: every
 * FLOAT/DOUBLE column decoded as 0.0 regardless of its real value) even
 * though the fetch itself reported MYSQL_DATA_TRUNCATED as expected. A
 * fixed buffer sidesteps that rather than depending on it: 128 bytes is
 * far more than any of these types' string form ever needs (MySQL's own
 * DECIMAL/NEWDECIMAL max precision is 65 digits, so ~68 characters worst
 * case including sign and point). */
static bool mysql_fixed_width_field_helper(enum enum_field_types type) {
    switch(type) {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_YEAR:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
            return true;
        default:
            return false;
    }
}

enum { DIAMOND_MYSQL_FIXED_FIELD_WIDTH = 128 };

static DiamondVmStatus mysql_query_helper(DiamondVm *vm,DiamondMysqlHandle *handle,
        const char *sql_chars,size_t sql_length,const DiamondValue *param_values,
        size_t param_count,DiamondValue *result) {
    MYSQL_STMT *stmt=nullptr;
    const DiamondVmStatus status=mysql_exec_helper(vm,handle,sql_chars,sql_length,
        param_values,param_count,&stmt);
    if(status!=DIAMOND_VM_OK)return status;
    MYSQL_RES *meta=mysql_stmt_result_metadata(stmt);
    if(meta==nullptr) {
        /* Not a resultset-producing statement (e.g. an UPDATE run through
         * #query) -- an empty Array, the same "no rows" shape a SELECT
         * matching nothing produces. */
        DiamondArray *rows=allocate_array(vm,nullptr,0);
        if(rows==nullptr) {mysql_stmt_close(stmt);return DIAMOND_VM_OUT_OF_MEMORY;}
        *result=DIAMOND_OBJECT(rows);
        mysql_stmt_close(stmt);
        return DIAMOND_VM_OK;
    }
    const unsigned int column_count=mysql_num_fields(meta);
    MYSQL_FIELD *fields=mysql_fetch_fields(meta);
    MYSQL_BIND *out_binds=calloc(column_count,sizeof(MYSQL_BIND));
    unsigned long *lengths=calloc(column_count,sizeof(unsigned long));
    my_bool *nulls=calloc(column_count,sizeof(my_bool));
    char *fixed_buffers=calloc(column_count,DIAMOND_MYSQL_FIXED_FIELD_WIDTH);
    if(out_binds==nullptr||lengths==nullptr||nulls==nullptr||fixed_buffers==nullptr) {
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    for(unsigned int col=0;col<column_count;col++) {
        out_binds[col].buffer_type=MYSQL_TYPE_STRING;
        out_binds[col].length=&lengths[col];
        out_binds[col].is_null=&nulls[col];
        if(mysql_fixed_width_field_helper(fields[col].type)) {
            out_binds[col].buffer=fixed_buffers+(size_t)col*DIAMOND_MYSQL_FIXED_FIELD_WIDTH;
            out_binds[col].buffer_length=DIAMOND_MYSQL_FIXED_FIELD_WIDTH;
        }
    }
    if(mysql_stmt_bind_result(stmt,out_binds)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    if(mysql_stmt_store_result(stmt)!=0) {
        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_MYSQL_ERROR;
    }
    DiamondArray *rows=allocate_array(vm,nullptr,0);
    if(rows==nullptr) {
        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
        mysql_free_result(meta);mysql_stmt_close(stmt);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    *result=DIAMOND_OBJECT(rows);
    for(;;) {
        const int fetch_status=mysql_stmt_fetch(stmt);
        if(fetch_status==MYSQL_NO_DATA)break;
        if(fetch_status!=0&&fetch_status!=MYSQL_DATA_TRUNCATED) {
            snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
            free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
            mysql_free_result(meta);mysql_stmt_close(stmt);
            return DIAMOND_VM_MYSQL_ERROR;
        }
        DiamondHash *row_hash=allocate_hash(vm);
        if(row_hash==nullptr) {
            free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
            mysql_free_result(meta);mysql_stmt_close(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        if(!array_push(vm,rows,DIAMOND_OBJECT(row_hash))) {
            free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
            mysql_free_result(meta);mysql_stmt_close(stmt);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
        for(unsigned int col=0;col<column_count;col++) {
            DiamondString *key=
                allocate_string(vm,fields[col].name,strlen(fields[col].name));
            if(key==nullptr) {
                free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                mysql_free_result(meta);mysql_stmt_close(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),DIAMOND_NIL)) {
                free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                mysql_free_result(meta);mysql_stmt_close(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
            DiamondValue column_value=DIAMOND_NIL;
            if(!nulls[col]) {
                const bool fixed_width=mysql_fixed_width_field_helper(fields[col].type);
                bool decoded;
                if(fixed_width) {
                    decoded=mysql_decode_value_helper(vm,&fields[col],
                        fixed_buffers+(size_t)col*DIAMOND_MYSQL_FIXED_FIELD_WIDTH,
                        lengths[col],&column_value);
                } else {
                    const unsigned long value_length=lengths[col];
                    char *buffer=malloc(value_length>0?value_length:1);
                    if(buffer==nullptr) {
                        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                        mysql_free_result(meta);mysql_stmt_close(stmt);
                        return DIAMOND_VM_OUT_OF_MEMORY;
                    }
                    MYSQL_BIND fetch_bind=(MYSQL_BIND){0};
                    fetch_bind.buffer_type=MYSQL_TYPE_STRING;
                    fetch_bind.buffer=buffer;
                    fetch_bind.buffer_length=value_length;
                    if(mysql_stmt_fetch_column(stmt,&fetch_bind,col,0)!=0) {
                        snprintf(vm->error,sizeof vm->error,"%s",mysql_stmt_error(stmt));
                        free(buffer);
                        free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                        mysql_free_result(meta);mysql_stmt_close(stmt);
                        return DIAMOND_VM_MYSQL_ERROR;
                    }
                    decoded=mysql_decode_value_helper(vm,&fields[col],buffer,
                        value_length,&column_value);
                    free(buffer);
                }
                if(!decoded) {
                    free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                    mysql_free_result(meta);mysql_stmt_close(stmt);
                    return DIAMOND_VM_OUT_OF_MEMORY;
                }
            }
            if(!hash_set(vm,row_hash,DIAMOND_OBJECT(key),column_value)) {
                free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
                mysql_free_result(meta);mysql_stmt_close(stmt);
                return DIAMOND_VM_OUT_OF_MEMORY;
            }
        }
    }
    free(out_binds);free(lengths);free(nulls);free(fixed_buffers);
    mysql_free_result(meta);
    mysql_stmt_close(stmt);
    return DIAMOND_VM_OK;
}

/* #execute/#query/#last_insert_row_id/#close -- factored out of the
 * INVOKE case body for the same stack-frame reason sqlite3_dispatch_
 * helper's own comment explains. #last_insert_row_id is a direct
 * mysql_insert_id(conn) call, simpler than PostgreSQL's own `SELECT
 * lastval()` round-trip -- MySQL's client library tracks the connection's
 * last AUTO_INCREMENT value itself, no separate query needed, and (unlike
 * lastval()) it has no "not yet defined this session" failure mode: an
 * unused connection just reads back 0. */
static DiamondVmStatus mysql_dispatch_helper(DiamondVm *vm,DiamondMysqlHandle *target_db,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    const bool execute_method=method_name->length==7&&
        memcmp(method_name->chars,"execute",7)==0;
    const bool query_method=method_name->length==5&&
        memcmp(method_name->chars,"query",5)==0;
    const bool last_insert_row_id_method=method_name->length==18&&
        memcmp(method_name->chars,"last_insert_row_id",18)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    if(!execute_method&&!query_method&&!last_insert_row_id_method&&!close_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"MySQL");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(target_db->conn!=nullptr) {
            mysql_close(target_db->conn);
            target_db->conn=nullptr;
        }
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(target_db->conn==nullptr) {
        snprintf(vm->error,sizeof vm->error,"MySQL connection is closed");
        return DIAMOND_VM_MYSQL_ERROR;
    }
    if(last_insert_row_id_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_INT((int64_t)mysql_insert_id(target_db->conn));
        return DIAMOND_VM_OK;
    }
    /* execute/query share the same argument shape: (sql) or (sql, params). */
    if(argc!=1&&argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,"MySQL#%.*s's sql argument must be a String",
            (int)method_name->length,method_name->chars);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *sql=(const DiamondString *)registers[base].as.object;
    const DiamondValue *param_values=nullptr;
    size_t param_count=0;
    if(argc==2) {
        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
            snprintf(vm->error,sizeof vm->error,
                "MySQL#%.*s's params argument must be an Array",
                (int)method_name->length,method_name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *params=(const DiamondArray *)registers[(size_t)base+1].as.object;
        param_values=params->values;
        param_count=params->count;
    }
    if(execute_method) {
        DiamondValue execute_result=DIAMOND_NIL;
        const DiamondVmStatus execute_status=mysql_execute_helper(vm,
            target_db,sql->chars,sql->length,param_values,param_count,&execute_result);
        if(execute_status!=DIAMOND_VM_OK)return execute_status;
        registers[dest]=execute_result;return DIAMOND_VM_OK;
    }
    /* query_method: mysql_query_helper writes directly into registers[dest]
     * (a real GC root), not a local -- see its own comment. */
    return mysql_query_helper(vm,target_db,sql->chars,sql->length,param_values,
        param_count,&registers[dest]);
}

/* #year/#month/#day/#hour/#min/#sec/#wday/#yday/#to_i/#to_f/#strftime/
 * #to_s/#utc/#localtime/#utc? -- factored out of the INVOKE case body
 * for the same stack-frame reason sqlite3_dispatch_helper's own comment
 * explains (immediately above). */
static DiamondVmStatus time_dispatch_helper(DiamondVm *vm,DiamondTime *target,
        const DiamondStringConstant *method_name,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    bool component=false;int component_value=0;
    struct tm parts={};
    const bool needs_parts=
        (method_name->length==4&&memcmp(method_name->chars,"year",4)==0)||
        (method_name->length==5&&memcmp(method_name->chars,"month",5)==0)||
        (method_name->length==3&&memcmp(method_name->chars,"day",3)==0)||
        (method_name->length==4&&memcmp(method_name->chars,"hour",4)==0)||
        (method_name->length==3&&memcmp(method_name->chars,"min",3)==0)||
        (method_name->length==3&&memcmp(method_name->chars,"sec",3)==0)||
        (method_name->length==4&&memcmp(method_name->chars,"wday",4)==0)||
        (method_name->length==4&&memcmp(method_name->chars,"yday",4)==0);
    if(needs_parts) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(!time_struct_tm(target,&parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        component=true;
        if(method_name->length==4&&memcmp(method_name->chars,"year",4)==0)
            component_value=parts.tm_year+1900;
        else if(method_name->length==5&&memcmp(method_name->chars,"month",5)==0)
            component_value=parts.tm_mon+1;
        else if(method_name->length==3&&memcmp(method_name->chars,"day",3)==0)
            component_value=parts.tm_mday;
        else if(method_name->length==4&&memcmp(method_name->chars,"hour",4)==0)
            component_value=parts.tm_hour;
        else if(method_name->length==3&&memcmp(method_name->chars,"min",3)==0)
            component_value=parts.tm_min;
        else if(method_name->length==3&&memcmp(method_name->chars,"sec",3)==0)
            component_value=parts.tm_sec;
        else if(method_name->length==4&&memcmp(method_name->chars,"wday",4)==0)
            component_value=parts.tm_wday;
        else component_value=parts.tm_yday+1;
    }
    if(component) {
        registers[dest]=DIAMOND_INT(component_value);
        return DIAMOND_VM_OK;
    }
    const bool to_i_method=method_name->length==4&&
        memcmp(method_name->chars,"to_i",4)==0;
    const bool to_f_method=method_name->length==4&&
        memcmp(method_name->chars,"to_f",4)==0;
    if(to_i_method||to_f_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=to_i_method?
            DIAMOND_INT((int64_t)target->epoch):DIAMOND_FLOAT(target->epoch);
        return DIAMOND_VM_OK;
    }
    const bool utc_p_method=method_name->length==4&&
        memcmp(method_name->chars,"utc?",4)==0;
    if(utc_p_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_BOOL(target->utc);
        return DIAMOND_VM_OK;
    }
    const bool utc_method=method_name->length==3&&
        memcmp(method_name->chars,"utc",3)==0;
    const bool localtime_method=method_name->length==9&&
        memcmp(method_name->chars,"localtime",9)==0;
    if(utc_method||localtime_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        DiamondTime *copy=allocate_time(vm,target->epoch,utc_method);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(copy);
        return DIAMOND_VM_OK;
    }
    const bool to_s_method=method_name->length==4&&
        memcmp(method_name->chars,"to_s",4)==0;
    if(to_s_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        StringBuilder builder={};
        if(!format_time_default(target,&builder)) {
            free(builder.chars);
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
        free(builder.chars);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);
        return DIAMOND_VM_OK;
    }
    const bool strftime_method=method_name->length==8&&
        memcmp(method_name->chars,"strftime",8)==0;
    if(strftime_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
           registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "Time#strftime argument must be a String");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *format=(const DiamondString *)registers[base].as.object;
        if(!time_struct_tm(target,&parts)) {
            snprintf(vm->error,sizeof vm->error,"Time value out of range");
            return DIAMOND_VM_TYPE_ERROR;
        }
        char stack_buffer[256];
        size_t length=strftime(stack_buffer,sizeof stack_buffer,format->chars,&parts);
        const char *result_chars=stack_buffer;
        char *heap_buffer=nullptr;
        if(length==0) {
            heap_buffer=malloc(4096);
            if(heap_buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            length=strftime(heap_buffer,4096,format->chars,&parts);
            result_chars=heap_buffer;
        }
        DiamondString *string=allocate_string(vm,result_chars,length);
        free(heap_buffer);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);
        return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"Time");
    return DIAMOND_VM_TYPE_ERROR;
}

/* String#format's real body -- kept out of run_chunk's own INVOKE case
 * for the same stack-frame-budget reason array_join_helper/
 * time_dispatch_helper already are (see array_join_helper's own
 * comment): the flag/width/precision parsing state below is real stack
 * weight this switch doesn't need baked into every call's frame.
 *
 * A %-directive's flags/width/precision are parsed and bounds-checked
 * here, then used to build a small, internally-constructed conversion
 * string (e.g. "%-05lld") handed to a real snprintf alongside exactly
 * one correctly-typed C argument -- never the caller's own format
 * string forwarded into printf-family varargs directly (that would be
 * a real format-string vulnerability, since a Diamond value's runtime
 * type has no relationship to whatever C conversion a matching printf
 * call site expects; every conversion here is chosen by this function
 * from a fixed, closed set, not from caller-controlled text).
 *
 * `args_value` is either the single value to format, or an Array of
 * them (`"%d-%s".format([1, "x"])`) -- matching Ruby's String#%,
 * without needing variadic/splat call support Diamond doesn't have. */
static DiamondVmStatus string_format_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const DiamondString *format,DiamondValue args_value,
        DiamondValue *out) {
    const DiamondValue *args=&args_value;
    size_t arg_count=1;
    if(args_value.kind==DIAMOND_VALUE_OBJECT&&
       args_value.as.object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *array=(const DiamondArray *)args_value.as.object;
        args=array->values;arg_count=array->count;
    }
    StringBuilder builder={};
    size_t arg_index=0;
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(size_t index=0;index<format->length;index++) {
        const char ch=format->chars[index];
        if(ch!='%') {
            if(!builder_append(&builder,&ch,1)){status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            continue;
        }
        index++;
        if(index>=format->length) {
            snprintf(vm->error,sizeof vm->error,
                "String#format: trailing '%%' with no directive");
            status=DIAMOND_VM_TYPE_ERROR;break;
        }
        if(format->chars[index]=='%') {
            if(!builder_append(&builder,"%",1)){status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            continue;
        }
        bool left_justify=false,zero_pad=false;
        while(index<format->length&&
              (format->chars[index]=='-'||format->chars[index]=='0')) {
            if(format->chars[index]=='-')left_justify=true;else zero_pad=true;
            index++;
        }
        int width=0;
        while(index<format->length&&isdigit((unsigned char)format->chars[index])) {
            width=width*10+(format->chars[index]-'0');index++;
        }
        int precision=-1;
        if(index<format->length&&format->chars[index]=='.') {
            index++;precision=0;
            while(index<format->length&&isdigit((unsigned char)format->chars[index])) {
                precision=precision*10+(format->chars[index]-'0');index++;
            }
        }
        if(index>=format->length) {
            snprintf(vm->error,sizeof vm->error,
                "String#format: incomplete directive at end of format string");
            status=DIAMOND_VM_TYPE_ERROR;break;
        }
        const char conversion=format->chars[index];
        if(arg_index>=arg_count) {
            snprintf(vm->error,sizeof vm->error,
                "String#format: too few arguments for format string");
            status=DIAMOND_VM_ARITY_ERROR;break;
        }
        const DiamondValue arg=args[arg_index++];
        char piece[512];
        int piece_length=-1;
        if(conversion=='d'||conversion=='i') {
            if(arg.kind!=DIAMOND_VALUE_INT&&arg.kind!=DIAMOND_VALUE_FLOAT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#format: %%%c needs an Int or Float argument",conversion);
                status=DIAMOND_VM_TYPE_ERROR;break;
            }
            const int64_t value=arg.kind==DIAMOND_VALUE_FLOAT?
                (int64_t)arg.as.real:arg.as.integer;
            char spec[16];
            snprintf(spec,sizeof spec,"%%%s%s%dlld",
                left_justify?"-":"",zero_pad?"0":"",width);
            piece_length=snprintf(piece,sizeof piece,spec,(long long)value);
        } else if(conversion=='f') {
            if(arg.kind!=DIAMOND_VALUE_INT&&arg.kind!=DIAMOND_VALUE_FLOAT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#format: %%f needs an Int or Float argument");
                status=DIAMOND_VM_TYPE_ERROR;break;
            }
            const double value=arg.kind==DIAMOND_VALUE_FLOAT?
                arg.as.real:(double)arg.as.integer;
            char spec[24];
            snprintf(spec,sizeof spec,"%%%s%s%d.%df",
                left_justify?"-":"",zero_pad?"0":"",width,precision<0?6:precision);
            piece_length=snprintf(piece,sizeof piece,spec,value);
        } else if(conversion=='x'||conversion=='X'||conversion=='o'||conversion=='b') {
            if(arg.kind!=DIAMOND_VALUE_INT) {
                snprintf(vm->error,sizeof vm->error,
                    "String#format: %%%c needs an Int argument",conversion);
                status=DIAMOND_VM_TYPE_ERROR;break;
            }
            if(conversion=='b') {
                char digits[65];size_t digit_count=0;
                uint64_t bits=(uint64_t)arg.as.integer;
                do {digits[digit_count++]=(char)('0'+(bits&1));bits>>=1;}
                while(bits!=0&&digit_count<sizeof digits);
                char reversed[65];
                for(size_t i=0;i<digit_count;i++)reversed[i]=digits[digit_count-1-i];
                reversed[digit_count]='\0';
                char spec[16];
                snprintf(spec,sizeof spec,"%%%s%s%ds",
                    left_justify?"-":"",zero_pad?"0":"",width);
                piece_length=snprintf(piece,sizeof piece,spec,reversed);
            } else {
                char spec[16];
                snprintf(spec,sizeof spec,"%%%s%s%d%c",
                    left_justify?"-":"",zero_pad?"0":"",width,conversion);
                piece_length=snprintf(piece,sizeof piece,spec,
                    (unsigned long long)arg.as.integer);
            }
        } else if(conversion=='s') {
            DiamondValue stringified=DIAMOND_NIL;
            status=stringify_value(vm,chunk,depth,arg,&stringified);
            if(status!=DIAMOND_VM_OK)break;
            const DiamondString *piece_string=
                (const DiamondString *)stringified.as.object;
            const size_t pad=width>0&&(size_t)width>piece_string->length?
                (size_t)width-piece_string->length:0;
            bool ok=true;
            if(pad>0&&!left_justify)
                for(size_t i=0;i<pad&&ok;i++)ok=builder_append(&builder," ",1);
            if(ok)ok=builder_append(&builder,piece_string->chars,piece_string->length);
            if(pad>0&&left_justify)
                for(size_t i=0;i<pad&&ok;i++)ok=builder_append(&builder," ",1);
            if(!ok){status=DIAMOND_VM_OUT_OF_MEMORY;break;}
            continue;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "String#format: unknown directive '%%%c'",conversion);
            status=DIAMOND_VM_TYPE_ERROR;break;
        }
        if(piece_length<0||(size_t)piece_length>=sizeof piece) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
        if(!builder_append(&builder,piece,(size_t)piece_length)) {
            status=DIAMOND_VM_OUT_OF_MEMORY;break;
        }
    }
    if(status!=DIAMOND_VM_OK) {free(builder.chars);return status;}
    DiamondString *formatted=allocate_string(vm,
        builder.chars?builder.chars:"",builder.length);
    free(builder.chars);
    if(formatted==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out=DIAMOND_OBJECT(formatted);
    return DIAMOND_VM_OK;
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
static void raise_capture_backtrace_helper(DiamondVm *vm,const DiamondChunk *chunk) {
    if(vm->exception.kind!=DIAMOND_VALUE_OBJECT||
       vm->exception.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return;
    DiamondInstance *raised=(DiamondInstance *)vm->exception.as.object;
    const DiamondChunk *owner=raised->owner!=nullptr?raised->owner:chunk;
    bool is_exception=false;const DiamondClass *ancestor=raised->class;
    while(ancestor!=nullptr) {
        if(ancestor==&owner->classes[DIAMOND_CLASS_EXCEPTION]){is_exception=true;break;}
        ancestor=ancestor->superclass==UINT8_MAX?nullptr:
            &owner->classes[ancestor->superclass];
    }
    if(!is_exception||raised->field_count<=2)return;
    DiamondArray *backtrace=allocate_array(vm,nullptr,0);
    if(backtrace==nullptr)return;
    /* Root immediately: vm->exception (already set, has_exception=true by
     * the caller) keeps `raised` alive, so assigning here makes
     * `backtrace` itself reachable before any allocation below can
     * trigger a GC -- same pattern String#split uses for its pieces. */
    raised->fields[2]=DIAMOND_OBJECT(backtrace);
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

enum { DIAMOND_PROCESS_MAX_ARGV = 65536 };

/* Process.run(argv): argv-array-only (never a shell string -- there is no
 * injection surface to guard against, by construction, matching the
 * design settled with the user before building this), blocking, full
 * stdout/stderr capture via a pipe pair and posix_spawnp. Child's stdin
 * is /dev/null (v1 deliberately has no way to feed it data -- see
 * docs/syntax.md). Reads both pipes with poll() rather than reading one
 * to EOF and then the other, specifically to avoid the classic deadlock
 * (child fills the stdout pipe buffer while blocked writing it, parent
 * is still blocked reading stderr, neither side ever makes progress).
 *
 * EINTR on poll()/read()/waitpid() just retries the syscall -- unlike
 * IO.poll's own EINTR handling, this does NOT run pending Signal.trap
 * handlers mid-wait (dispatch_pending_signals needs chunk/depth/ip from
 * run_chunk's own dispatch loop, which this helper -- deliberately kept
 * outside run_chunk's switch, per this file's stack-frame-budget
 * convention -- doesn't have). A signal trapped while a Process.run call
 * is blocked runs once the child exits and this call returns, not
 * immediately. Acceptable for a "minimal blocking capture" v1; a
 * non-blocking Process.spawn with a live handle (a real future feature,
 * not this one) would be the natural place to fix that. */
static DiamondVmStatus process_run_helper(DiamondVm *vm,
        DiamondArray *argv_array,DiamondProcessResult *result) {
    if(argv_array->count==0) {
        snprintf(vm->error,sizeof vm->error,"Process.run: argv must not be empty");
        return DIAMOND_VM_ARITY_ERROR;
    }
    if(argv_array->count>DIAMOND_PROCESS_MAX_ARGV) {
        snprintf(vm->error,sizeof vm->error,
            "Process.run: argv has too many elements (max %d)",
            DIAMOND_PROCESS_MAX_ARGV);
        return DIAMOND_VM_ARITY_ERROR;
    }
    for(size_t index=0;index<argv_array->count;index++) {
        const DiamondValue element=argv_array->values[index];
        if(element.kind!=DIAMOND_VALUE_OBJECT||
           element.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "Process.run: argv must be an Array of Strings");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *piece=(const DiamondString *)element.as.object;
        if(strlen(piece->chars)!=piece->length) {
            snprintf(vm->error,sizeof vm->error,
                "Process.run: argv strings must not contain a NUL byte");
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    /* Points directly at each DiamondString's own null-terminated buffer
     * -- no copying needed, argv_array stays reachable (still live in the
     * caller's own register) for this whole call, and posix_spawn/execve
     * never write through argv despite the non-const `char *const []`
     * signature (a C89-main-signature-compatibility artifact, not a real
     * mutation contract). */
    char **argv=malloc((argv_array->count+1)*sizeof(char *));
    if(argv==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    for(size_t index=0;index<argv_array->count;index++)
        argv[index]=((DiamondString *)argv_array->values[index].as.object)->chars;
    argv[argv_array->count]=nullptr;
    const char *command_name=argv[0];

    int stdout_pipe[2]={-1,-1};
    int stderr_pipe[2]={-1,-1};
    if(pipe(stdout_pipe)!=0||pipe(stderr_pipe)!=0) {
        const int saved_errno=errno;
        if(stdout_pipe[0]>=0)close(stdout_pipe[0]);
        if(stdout_pipe[1]>=0)close(stdout_pipe[1]);
        if(stderr_pipe[0]>=0)close(stderr_pipe[0]);
        if(stderr_pipe[1]>=0)close(stderr_pipe[1]);
        free(argv);
        snprintf(vm->error,sizeof vm->error,"Process.run: pipe: %s",
            strerror(saved_errno));
        return DIAMOND_VM_IO_ERROR;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    posix_spawn_file_actions_adddup2(&actions,stdout_pipe[1],STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,stderr_pipe[1],STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions,stdout_pipe[0]);
    posix_spawn_file_actions_addclose(&actions,stdout_pipe[1]);
    posix_spawn_file_actions_addclose(&actions,stderr_pipe[0]);
    posix_spawn_file_actions_addclose(&actions,stderr_pipe[1]);

    pid_t pid=0;
    const int spawn_status=posix_spawnp(&pid,command_name,&actions,
        nullptr,argv,environ);
    posix_spawn_file_actions_destroy(&actions);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    if(spawn_status!=0) {
        close(stdout_pipe[0]);close(stderr_pipe[0]);
        snprintf(vm->error,sizeof vm->error,"Process.run: %s: %s",
            command_name,strerror(spawn_status));
        free(argv);
        return DIAMOND_VM_IO_ERROR;
    }
    free(argv);

    StringBuilder stdout_builder={};
    StringBuilder stderr_builder={};
    bool stdout_open=true,stderr_open=true,out_of_memory=false;
    while((stdout_open||stderr_open)&&!out_of_memory) {
        struct pollfd fds[2];
        nfds_t fd_count=0;
        int stdout_slot=-1,stderr_slot=-1;
        if(stdout_open) {
            stdout_slot=(int)fd_count;
            fds[fd_count++]=(struct pollfd){.fd=stdout_pipe[0],.events=POLLIN};
        }
        if(stderr_open) {
            stderr_slot=(int)fd_count;
            fds[fd_count++]=(struct pollfd){.fd=stderr_pipe[0],.events=POLLIN};
        }
        errno=0;
        const int poll_result=poll(fds,fd_count,-1);
        if(poll_result<0) {
            if(errno==EINTR)continue;
            break;
        }
        char chunk_buffer[4096];
        if(stdout_slot>=0&&fds[stdout_slot].revents!=0) {
            const ssize_t bytes_read=read(stdout_pipe[0],chunk_buffer,sizeof chunk_buffer);
            if(bytes_read>0) {
                if(!builder_append(&stdout_builder,chunk_buffer,(size_t)bytes_read))
                    out_of_memory=true;
            } else if(bytes_read==0||errno!=EINTR) {
                close(stdout_pipe[0]);stdout_open=false;
            }
        }
        if(stderr_slot>=0&&fds[stderr_slot].revents!=0) {
            const ssize_t bytes_read=read(stderr_pipe[0],chunk_buffer,sizeof chunk_buffer);
            if(bytes_read>0) {
                if(!builder_append(&stderr_builder,chunk_buffer,(size_t)bytes_read))
                    out_of_memory=true;
            } else if(bytes_read==0||errno!=EINTR) {
                close(stderr_pipe[0]);stderr_open=false;
            }
        }
    }
    if(stdout_open)close(stdout_pipe[0]);
    if(stderr_open)close(stderr_pipe[0]);

    int wait_status=0;
    pid_t wait_result=0;
    do { wait_result=waitpid(pid,&wait_status,0); }
    while(wait_result<0&&errno==EINTR);

    if(out_of_memory) {
        free(stdout_builder.chars);free(stderr_builder.chars);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    int64_t exit_code=255;
    if(wait_result==pid) {
        if(WIFEXITED(wait_status))exit_code=WEXITSTATUS(wait_status);
        else if(WIFSIGNALED(wait_status))exit_code=128+WTERMSIG(wait_status);
    }
    /* result is already rooted (assigned to the caller's dest register
     * before this helper runs) -- see allocate_process_result's own
     * comment -- so each field write below is safe the instant it
     * happens, same as String#split rooting its array before pushing. */
    DiamondString *stdout_string=allocate_string(vm,
        stdout_builder.chars?stdout_builder.chars:"",stdout_builder.length);
    free(stdout_builder.chars);
    if(stdout_string==nullptr){free(stderr_builder.chars);return DIAMOND_VM_OUT_OF_MEMORY;}
    result->stdout_value=DIAMOND_OBJECT(stdout_string);
    DiamondString *stderr_string=allocate_string(vm,
        stderr_builder.chars?stderr_builder.chars:"",stderr_builder.length);
    free(stderr_builder.chars);
    if(stderr_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    result->stderr_value=DIAMOND_OBJECT(stderr_string);
    result->exit_code=exit_code;
    return DIAMOND_VM_OK;
}

static DiamondVmStatus process_result_dispatch_helper(DiamondVm *vm,
        DiamondProcessResult *target,const DiamondStringConstant *method_name,
        DiamondValue *registers,uint8_t argc,uint16_t dest) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    if(method_name->length==6&&memcmp(method_name->chars,"stdout",6)==0) {
        registers[dest]=target->stdout_value;return DIAMOND_VM_OK;
    }
    if(method_name->length==6&&memcmp(method_name->chars,"stderr",6)==0) {
        registers[dest]=target->stderr_value;return DIAMOND_VM_OK;
    }
    if(method_name->length==9&&memcmp(method_name->chars,"exit_code",9)==0) {
        registers[dest]=DIAMOND_INT(target->exit_code);return DIAMOND_VM_OK;
    }
    if(method_name->length==8&&memcmp(method_name->chars,"success?",8)==0) {
        registers[dest]=DIAMOND_BOOL(target->exit_code==0);return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for Process::Result",
        (int)method_name->length,method_name->chars);
    return DIAMOND_VM_TYPE_ERROR;
}

/* debugger()/breakpoint()'s runtime half -- see parse_debugger_call's own
 * comment in compiler.c for the compile-time half (name, register) pairs
 * come from. Prints "chunk:line:column" matching the exact format
 * RECORD_ERROR/raise_capture_backtrace_helper already use elsewhere in
 * this file, then each local's name and stringified value (unwrapping a
 * captured local's Cell box first -- BOX_LOCAL replaces a captured
 * local's own register contents with a DiamondCell wrapper in place, so
 * this checks the *runtime* value kind rather than trusting any
 * compile-time "captured" bookkeeping passed through), then blocks on
 * one line of stdin. Kept as its own helper (not inlined into
 * DIAMOND_OP_DEBUGGER's own case block) both for this file's usual
 * stack-frame-budget reasons and because it may recurse into run_chunk
 * itself once per local, through stringify_value calling a user-defined
 * to_s. */
static DiamondVmStatus debugger_helper(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,size_t instruction_offset,DiamondValue *registers,
        const uint8_t *name_indices,const uint16_t *local_registers,uint8_t local_count) {
    const char *frame_name=chunk->name!=nullptr?chunk->name:"<chunk>";
    const bool in_bounds=instruction_offset<chunk->code_count;
    const uint32_t line=in_bounds&&chunk->lines!=nullptr?
        chunk->lines[instruction_offset]:0;
    const uint32_t column=in_bounds&&chunk->columns!=nullptr?
        chunk->columns[instruction_offset]:0;
    fprintf(stdout,"--- paused at %s:%u:%u ---\n",frame_name,line,column);
    if(local_count>0) {
        fprintf(stdout,"locals:\n");
        for(size_t index=0;index<local_count;index++) {
            if((size_t)name_indices[index]>=chunk->string_count)continue;
            const DiamondStringConstant *name=&chunk->strings[name_indices[index]];
            DiamondValue value=registers[local_registers[index]];
            if(value.kind==DIAMOND_VALUE_OBJECT&&
               value.as.object->kind==DIAMOND_OBJECT_CELL)
                value=((DiamondCell *)value.as.object)->value;
            DiamondValue stringified=DIAMOND_NIL;
            const DiamondVmStatus status=
                stringify_value(vm,chunk,depth,value,&stringified);
            if(status!=DIAMOND_VM_OK)return status;
            const DiamondString *text=(const DiamondString *)stringified.as.object;
            fprintf(stdout,"  %.*s = %.*s\n",(int)name->length,name->chars,
                (int)text->length,text->chars);
        }
    }
    fprintf(stdout,"(press Enter to continue)\n");
    fflush(stdout);
    int character=0;
    while((character=getchar())!=EOF&&character!='\n') {}
    return DIAMOND_VM_OK;
}

static DiamondVmStatus run_chunk(const DiamondChunk *chunk,
                                 DiamondVm *vm,
                                 const DiamondValue *arguments,
                                 size_t argument_count, size_t depth,
                                 const DiamondClosure *closure,
                                 DiamondValue *result) {
    if (depth >= DIAMOND_MAX_CALL_DEPTH) {
        return DIAMOND_VM_STACK_OVERFLOW;
    }
    DiamondChunk execution;
    DiamondTypeBinding bindings[8]={};
    if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr)
        memcpy(bindings,chunk->type_variable_bindings,
               chunk->type_variable_count*sizeof(DiamondTypeBinding));
    if(chunk->type_variable_count>0&&chunk->parameter_type_sets!=nullptr&&
       chunk->type_variable_bindings==nullptr) {
        /* Only copy `*chunk` when this generic-function-with-unbound-
         * type-variable path is actually taken -- the common case
         * (type_variable_count==0, essentially every non-generic call)
         * never reads `execution`, so skip the 168-byte struct copy. */
        execution=*chunk;
        for(size_t parameter=0;
            parameter+chunk->parameter_offset<argument_count;parameter++) {
            const uint8_t set=chunk->parameter_type_sets[parameter];
            if(set!=UINT8_MAX&&set<chunk->type_set_count)
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
    if (argument_count > live_register_count) {
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
    for (size_t index = 0; index < argument_count; index++) {
        registers[index] = arguments[index];
    }
    PendingUnwind pending={};
    size_t ip = 0;
    size_t instruction_offset = 0;
    DiamondFrame frame = {
        .previous = vm->frames,
        .registers = registers,
        .pending = &pending,
        .register_count = live_register_count,
        .chunk = chunk,
        .instruction_offset = &instruction_offset,
    };
    vm->frames = &frame;
    UnwindHandler handlers[16];
    size_t handler_count=0;

    #define RECORD_ERROR(status_) do {                                      \
        if ((status_) != DIAMOND_VM_OK) {                                   \
            size_t used = strlen(vm->error);                                \
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
        const DiamondVmStatus return_status_=(status_);               \
        if(handler_count>0 && catch_runtime_error(vm,chunk,           \
           return_status_,handlers,&handler_count,&pending,registers,&ip))\
            goto dispatch_continue;                                  \
        RECORD_ERROR(return_status_);                                \
        vm->frames = frame.previous;                                 \
        free(heap_registers);                                        \
        return vm->has_exception?DIAMOND_VM_EXCEPTION:return_status_;\
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
        /* Cheap steady-state cost (one volatile read, almost always
         * false) for prompt signal handling in CPU-bound Diamond code
         * that never calls a blocking native function at all -- the
         * EINTR-based checks in accept/IO.poll/UDPSocket#receive below
         * cover the case where it's blocked in one of those instead. */
        if(diamond_any_signal_pending) {
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
                registers[destination]=DIAMOND_BOOL(index<argument_count);
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
                uint16_t destination=0,source=0;uint8_t newline=0;
                READ_SHORT(destination);READ_SHORT(source);READ_BYTE(newline);
                DiamondValue converted=DIAMOND_NIL;
                DiamondVmStatus status=stringify_value(vm,chunk,depth,
                    registers[source],&converted);
                VM_PROPAGATE(status);
                const DiamondString *text=(const DiamondString *)converted.as.object;
                fwrite(text->chars,1,text->length,stdout);
                if(newline!=0) {
                    fputc('\n',stdout);
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
                    fflush(stdout);
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
                if (opcode == DIAMOND_OP_ADD_INT &&
                    (registers[left].kind != DIAMOND_VALUE_INT ||
                     registers[right].kind != DIAMOND_VALUE_INT)) {
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)DIAMOND_OP_ADD;
                    vm->deoptimized_sites++;
                    /* Retargets the opcode but, unlike a real fresh
                     * dispatch of the now-generic ADD, has to run ADD's
                     * own fallback logic on this instruction directly --
                     * see add_fallback's own comment for why this is a
                     * shared helper rather than duplicated here (it used
                     * to be, and the duplicate silently lacked the mixed-
                     * Int/Float case). */
                    DiamondValue add_result=DIAMOND_NIL;
                    const DiamondVmStatus add_status=add_fallback(vm,chunk,depth,
                        instruction_offset,registers[left],registers[right],&add_result);
                    VM_PROPAGATE(add_status);
                    registers[destination]=add_result;
                    break;
                }
                /* SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT never had a deopt
                 * branch at all before bignums existed: the only way an
                 * already-observed-Int operand's kind could stop being
                 * DIAMOND_VALUE_INT was a genuine type violation, so
                 * falling straight to the type-error path below was
                 * correct. Once an Int can legitimately become a bignum
                 * mid-execution that's no longer true -- mirror ADD_INT's
                 * deopt (no string special-case needed here, since only
                 * ADD supports string concatenation). Retargets and falls
                 * through rather than returning, so the bignum check just
                 * below gets a chance at it. */
                if ((opcode==DIAMOND_OP_SUBTRACT_INT||
                     opcode==DIAMOND_OP_MULTIPLY_INT||
                     opcode==DIAMOND_OP_DIVIDE_INT) &&
                    (registers[left].kind!=DIAMOND_VALUE_INT||
                     registers[right].kind!=DIAMOND_VALUE_INT)) {
                    const DiamondOpCode generic=opcode==DIAMOND_OP_SUBTRACT_INT
                        ?DIAMOND_OP_SUBTRACT
                        :opcode==DIAMOND_OP_MULTIPLY_INT
                            ?DIAMOND_OP_MULTIPLY:DIAMOND_OP_DIVIDE;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)generic;
                    opcode=generic;
                    vm->deoptimized_sites++;
                }
                /* Scoped to the three generic (non-_INT) opcodes only - the
                 * _INT forms, once past the deopt check above, always have
                 * both operands confirmed DIAMOND_VALUE_INT by this point. */
                if ((opcode==DIAMOND_OP_SUBTRACT||opcode==DIAMOND_OP_MULTIPLY||
                     opcode==DIAMOND_OP_DIVIDE) &&
                    is_int_value(registers[left])&&is_int_value(registers[right])&&
                    (value_is_bignum(registers[left])||
                     value_is_bignum(registers[right]))) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    DiamondValue bignum_result;
                    if(opcode==DIAMOND_OP_SUBTRACT)
                        bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
                    else if(opcode==DIAMOND_OP_MULTIPLY)
                        bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
                    else {
                        DiamondIntView zero_view;
                        diamond_int_view_int64(0,&zero_view);
                        if(diamond_bignum_compare(right_view,zero_view)==0)
                            VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                        bignum_result=
                            diamond_bignum_divide_truncated(vm,left_view,right_view);
                    }
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                if ((opcode==DIAMOND_OP_SUBTRACT||opcode==DIAMOND_OP_MULTIPLY||
                     opcode==DIAMOND_OP_DIVIDE) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[left].kind==DIAMOND_VALUE_INT) &&
                    (registers[right].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_INT) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    const double left_real=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_real=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    double float_result=0;
                    if(opcode==DIAMOND_OP_SUBTRACT)float_result=left_real-right_real;
                    else if(opcode==DIAMOND_OP_MULTIPLY)float_result=left_real*right_real;
                    /* DIVIDE: IEEE-754 double/0.0 naturally yields
                     * +-Infinity/NaN, no UB and no check needed, unlike Int. */
                    else float_result=left_real/right_real;
                    registers[destination]=DIAMOND_FLOAT(float_result);
                    break;
                }
                if (registers[left].kind != DIAMOND_VALUE_INT ||
                    registers[right].kind != DIAMOND_VALUE_INT) {
                    /* Reached by SUBTRACT/MULTIPLY/DIVIDE (generic, or
                     * retargeted here from their _INT deopt above) with a
                     * non-Int left operand -- ADD_INT can't reach this
                     * point with a non-Int operand, since its own deopt
                     * branch above already handles (and returns for) that
                     * case, so it's never a candidate for "+" dispatch
                     * here. */
                    if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                        registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE &&
                        (opcode==DIAMOND_OP_SUBTRACT||opcode==DIAMOND_OP_MULTIPLY||
                         opcode==DIAMOND_OP_DIVIDE)) {
                        const char *name=opcode==DIAMOND_OP_SUBTRACT?"-":
                            opcode==DIAMOND_OP_MULTIPLY?"*":"/";
                        bool found=false;DiamondValue op_result=DIAMOND_NIL;
                        const uint8_t *site=chunk->code+instruction_offset;
                        const DiamondVmStatus status=invoke_operator_method(vm,chunk,
                            depth,site,(const DiamondInstance *)registers[left].as.object,
                            name,strlen(name),&registers[right],&op_result,&found);
                        if(found) {
                            VM_PROPAGATE(status);
                            registers[destination]=op_result;
                            break;
                        }
                    }
                    /* Time never supports * or /, so MULTIPLY/DIVIDE with
                     * a Time operand correctly fall through to the
                     * TypeError below (time_subtract_fallback itself
                     * doesn't check opcode -- gated here instead). */
                    if (opcode==DIAMOND_OP_SUBTRACT) {
                        const DiamondVmStatus time_status=time_subtract_fallback(vm,
                            registers[left],registers[right],&registers[destination]);
                        if(time_status!=DIAMOND_VM_TYPE_ERROR){VM_PROPAGATE(time_status);break;}
                    }
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t left_value = registers[left].as.integer;
                const int64_t right_value = registers[right].as.integer;
                int64_t result_value = 0;
                bool overflow = false;
                if (opcode == DIAMOND_OP_ADD_INT) {
                    overflow = ckd_add(&result_value, left_value, right_value);
                } else if (opcode == DIAMOND_OP_SUBTRACT_INT ||
                           opcode == DIAMOND_OP_SUBTRACT) {
                    overflow = ckd_sub(&result_value, left_value, right_value);
                } else if (opcode == DIAMOND_OP_MULTIPLY_INT ||
                           opcode == DIAMOND_OP_MULTIPLY) {
                    overflow = ckd_mul(&result_value, left_value, right_value);
                } else {
                    if (right_value == 0) {
                        VM_RETURN(DIAMOND_VM_DIVISION_BY_ZERO);
                    }
                    if (left_value == INT64_MIN && right_value == -1) {
                        /* -INT64_MIN doesn't fit int64_t; promote
                         * instead of erroring, matching every other
                         * overflow site here (negating left_value's
                         * bignum view flips its sign to positive,
                         * which is exactly -INT64_MIN = 2^63). */
                        DiamondIntView left_view;
                        diamond_int_view(registers[left],&left_view);
                        const DiamondValue bignum_result=
                            diamond_bignum_negate(vm,left_view);
                        if(bignum_result.kind==DIAMOND_VALUE_NIL)
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[destination]=bignum_result;
                        break;
                    }
                    result_value = left_value / right_value;
                }
                if (overflow) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    DiamondValue bignum_result;
                    if(opcode==DIAMOND_OP_ADD_INT)
                        bignum_result=diamond_bignum_add(vm,left_view,right_view);
                    else if(opcode==DIAMOND_OP_SUBTRACT_INT||opcode==DIAMOND_OP_SUBTRACT)
                        bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
                    else
                        bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=bignum_result;
                    break;
                }
                registers[destination] = DIAMOND_INT(result_value);
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
            case DIAMOND_OP_MODULO: {
                /* Floored modulo (result takes the divisor's sign),
                 * matching Ruby -- not C's truncating `%` (which takes
                 * the dividend's sign). User-overloadable like the other
                 * arithmetic operators (unlike `<<`): an Instance left
                 * operand tries a `%` method the same way ADD/SUBTRACT/
                 * etc. do. No bignum support -- kept simple like `<<`,
                 * a deliberate v1 scope cut (see docs/syntax.md); a
                 * bignum operand falls through to the Instance/TypeError
                 * path below like any other unsupported type would. */
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
                        "%",1,&registers[right],&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;break;
                    }
                }
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
                        "negate",6,nullptr,&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;
                        break;
                    }
                }
                if (registers[operand].kind != DIAMOND_VALUE_INT) {
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
                if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                    registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=chunk->code+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[left].as.object,
                        "==",2,&registers[right],&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        const bool overloaded_equal=is_truthy(op_result);
                        registers[destination]=DIAMOND_BOOL(
                            opcode==DIAMOND_OP_EQUAL?overloaded_equal:!overloaded_equal);
                        break;
                    }
                }
                const bool equal = values_equal(registers[left], registers[right]);
                registers[destination] = DIAMOND_BOOL(
                    opcode == DIAMOND_OP_EQUAL ? equal : !equal);
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
                /* Same reasoning as SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT:
                 * these four _INT comparisons never had a deopt branch
                 * before bignums existed (a non-Int operand was always a
                 * genuine type error). Mirror the arithmetic block's fix. */
                if ((opcode==DIAMOND_OP_LESS_INT||opcode==DIAMOND_OP_LESS_EQUAL_INT||
                     opcode==DIAMOND_OP_GREATER_INT||
                     opcode==DIAMOND_OP_GREATER_EQUAL_INT) &&
                    (registers[left].kind!=DIAMOND_VALUE_INT||
                     registers[right].kind!=DIAMOND_VALUE_INT)) {
                    const DiamondOpCode generic=opcode==DIAMOND_OP_LESS_INT
                        ?DIAMOND_OP_LESS
                        :opcode==DIAMOND_OP_LESS_EQUAL_INT
                            ?DIAMOND_OP_LESS_EQUAL
                            :opcode==DIAMOND_OP_GREATER_INT
                                ?DIAMOND_OP_GREATER:DIAMOND_OP_GREATER_EQUAL;
                    uint8_t *code=(uint8_t *)(void *)chunk->code;
                    code[instruction_offset]=(uint8_t)generic;
                    opcode=generic;
                    vm->deoptimized_sites++;
                }
                if ((opcode==DIAMOND_OP_LESS||opcode==DIAMOND_OP_LESS_EQUAL||
                     opcode==DIAMOND_OP_GREATER||opcode==DIAMOND_OP_GREATER_EQUAL) &&
                    is_int_value(registers[left])&&is_int_value(registers[right])&&
                    (value_is_bignum(registers[left])||
                     value_is_bignum(registers[right]))) {
                    DiamondIntView left_view, right_view;
                    diamond_int_view(registers[left],&left_view);
                    diamond_int_view(registers[right],&right_view);
                    const int comparison=diamond_bignum_compare(left_view,right_view);
                    bool bignum_comparison=false;
                    if(opcode==DIAMOND_OP_LESS)bignum_comparison=comparison<0;
                    else if(opcode==DIAMOND_OP_LESS_EQUAL)bignum_comparison=comparison<=0;
                    else if(opcode==DIAMOND_OP_GREATER)bignum_comparison=comparison>0;
                    else bignum_comparison=comparison>=0;
                    registers[destination]=DIAMOND_BOOL(bignum_comparison);
                    break;
                }
                /* Scoped to the four generic (non-_INT) opcodes only, same
                 * reasoning as the arithmetic block: the _INT forms, once
                 * past the deopt check above, always have both operands
                 * confirmed DIAMOND_VALUE_INT by this point. */
                if ((opcode==DIAMOND_OP_LESS||opcode==DIAMOND_OP_LESS_EQUAL||
                     opcode==DIAMOND_OP_GREATER||opcode==DIAMOND_OP_GREATER_EQUAL) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[left].kind==DIAMOND_VALUE_INT) &&
                    (registers[right].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_INT) &&
                    (registers[left].kind==DIAMOND_VALUE_FLOAT||
                     registers[right].kind==DIAMOND_VALUE_FLOAT)) {
                    const double left_real=registers[left].kind==DIAMOND_VALUE_FLOAT?
                        registers[left].as.real:(double)registers[left].as.integer;
                    const double right_real=registers[right].kind==DIAMOND_VALUE_FLOAT?
                        registers[right].as.real:(double)registers[right].as.integer;
                    bool float_comparison=false;
                    if(opcode==DIAMOND_OP_LESS)float_comparison=left_real<right_real;
                    else if(opcode==DIAMOND_OP_LESS_EQUAL)
                        float_comparison=left_real<=right_real;
                    else if(opcode==DIAMOND_OP_GREATER)
                        float_comparison=left_real>right_real;
                    else float_comparison=left_real>=right_real;
                    registers[destination]=DIAMOND_BOOL(float_comparison);
                    break;
                }
                if (registers[left].kind != DIAMOND_VALUE_INT ||
                    registers[right].kind != DIAMOND_VALUE_INT) {
                    /* By this point opcode is guaranteed one of the four
                     * generic (non-_INT) comparisons, same reasoning as
                     * EQUAL/NOT_EQUAL above. */
                    if (registers[left].kind==DIAMOND_VALUE_OBJECT &&
                        registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                        const char *name=opcode==DIAMOND_OP_LESS?"<":
                            opcode==DIAMOND_OP_LESS_EQUAL?"<=":
                            opcode==DIAMOND_OP_GREATER?">":">=";
                        bool found=false;DiamondValue op_result=DIAMOND_NIL;
                        const uint8_t *site=chunk->code+instruction_offset;
                        const DiamondVmStatus status=invoke_operator_method(vm,chunk,
                            depth,site,(const DiamondInstance *)registers[left].as.object,
                            name,strlen(name),&registers[right],&op_result,&found);
                        if(found) {
                            VM_PROPAGATE(status);
                            registers[destination]=DIAMOND_BOOL(is_truthy(op_result));
                            break;
                        }
                    }
                    if(time_comparison_fallback(registers[left],registers[right],
                            opcode,&registers[destination]))break;
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const int64_t a = registers[left].as.integer;
                const int64_t b = registers[right].as.integer;
                bool comparison = false;
                if (opcode == DIAMOND_OP_LESS_INT || opcode == DIAMOND_OP_LESS)
                    comparison = a < b;
                if (opcode == DIAMOND_OP_LESS_EQUAL_INT ||
                    opcode == DIAMOND_OP_LESS_EQUAL) comparison = a <= b;
                if (opcode == DIAMOND_OP_GREATER_INT ||
                    opcode == DIAMOND_OP_GREATER) comparison = a > b;
                if (opcode == DIAMOND_OP_GREATER_EQUAL_INT ||
                    opcode == DIAMOND_OP_GREATER_EQUAL) comparison = a >= b;
                registers[destination] = DIAMOND_BOOL(comparison);
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
             * own design doc) and no String/Time support -- String
             * doesn't support `<` either today, and Time keeps its own
             * working comparisons untouched, both explicit scope cuts. */
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
                   registers[left].as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    bool found=false;DiamondValue op_result=DIAMOND_NIL;
                    const uint8_t *site=chunk->code+instruction_offset;
                    const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,
                        site,(const DiamondInstance *)registers[left].as.object,
                        "<=>",3,&registers[right],&op_result,&found);
                    if(found) {
                        VM_PROPAGATE(status);
                        registers[destination]=op_result;break;
                    }
                }
                registers[destination]=DIAMOND_NIL;break;
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
                    call_argument_count > function->arity) {
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
                };
                DiamondValue call_result = DIAMOND_NIL;
                const DiamondVmStatus status = run_chunk(
                    &called_chunk, vm, &registers[argument_base],
                    call_argument_count, depth + 1, nullptr, &call_result);
                VM_PROPAGATE(status);
                registers[destination] = call_result;
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
                   call_argument_count>function->arity)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondTypeBinding explicit_bindings[8]={};
                for(size_t index=0;index<type_argument_count;index++) {
                    uint8_t set_index=0;READ_BYTE(set_index);
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
                    .register_count=function->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=run_chunk(&called_chunk,vm,
                    &registers[argument_base],call_argument_count,depth+1,nullptr,
                    &call_result);
                VM_PROPAGATE(status);
                registers[destination]=call_result;
                break;
            }
            case DIAMOND_OP_CLOSURE: {
                uint16_t dest=0;uint8_t count=0;uint16_t index=0;
                READ_SHORT(dest);READ_SHORT(index);READ_BYTE(count);
                if(index>=chunk->function_count||count>16)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondValue captures[16];
                for(size_t i=0;i<count;i++){uint16_t reg=0;READ_SHORT(reg);captures[i]=registers[reg];}
                DiamondClosure *created=allocate_closure(vm,index,captures,count);
                if(created==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
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
                ((DiamondCell *)captured.as.object)->value=registers[source];break;
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
                ((DiamondCell *)registers[cell_reg].as.object)->value=registers[source];break;
            }
            case DIAMOND_OP_CALL_CLOSURE: {
                uint16_t dest=0,callable=0,base=0;uint8_t argc=0;
                READ_SHORT(dest);READ_SHORT(callable);READ_SHORT(base);READ_BYTE(argc);
                if(registers[callable].kind!=DIAMOND_VALUE_OBJECT||
                   registers[callable].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondClosure *called=(DiamondClosure *)registers[callable].as.object;
                if(called->function_index>=chunk->function_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *fn=chunk->functions[called->function_index];
                DiamondValue call_result=DIAMOND_NIL;
                const DiamondVmStatus status=call_closure_helper(vm,chunk,fn,called,
                    registers,base,argc,depth,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;break;
            }
            case DIAMOND_OP_NEW: {
                uint16_t dest=0,base=0;uint8_t ci=0,argc=0;
                READ_SHORT(dest);READ_BYTE(ci);READ_SHORT(base);READ_BYTE(argc);
                if(argc>16) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                if((size_t)ci>=chunk->class_count) VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondClass *class=&chunk->classes[ci];
                DiamondInstance *instance=allocate_instance(vm,class,nullptr);
                if(instance==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[dest]=DIAMOND_OBJECT(instance);
                const DiamondMethod *init=lookup_method(
                    chunk,class,"initialize",sizeof("initialize")-1);
                if(init!=nullptr) {
                    if(argc<init->required_arity||argc>init->arity)
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue args[17];args[0]=registers[dest];
                    for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                    const DiamondFunction *fn=chunk->functions[init->function_index];
                    DiamondChunk child={.name=fn->name,.code=fn->code,
                      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                      .constants=fn->constants,.constant_count=fn->constant_count,
                      .strings=fn->strings,.string_count=fn->string_count,
                      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                      .functions=chunk->functions,.function_count=chunk->function_count,
                      .classes=chunk->classes,.class_count=chunk->class_count,
                      .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                      .parameter_type_sets=fn->parameter_type_sets,
                      .type_variable_count=fn->type_variable_count,
                      .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                      .register_count=fn->register_count};
                    DiamondValue ignored=DIAMOND_NIL;
                    DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,nullptr,&ignored);
                    VM_PROPAGATE(s);
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
                        if(argc>2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(argc>0)instance->fields[0]=registers[base];
                        if(argc>1)instance->fields[1]=registers[(size_t)base+1];
                    } else if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                }
                break;
            }
            case DIAMOND_OP_INVOKE:
            case DIAMOND_OP_INVOKE_MONO:
            case DIAMOND_OP_INVOKE_TYPED: {
                uint16_t dest=0,recv=0,base=0;uint8_t name=0,argc=0;
                READ_SHORT(dest);READ_SHORT(recv);READ_BYTE(name);READ_SHORT(base);READ_BYTE(argc);
                uint8_t type_argument_count=0,type_arguments[8];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED) {
                    READ_BYTE(type_argument_count);
                    if(type_argument_count>8)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    for(size_t index=0;index<type_argument_count;index++)
                        READ_BYTE(type_arguments[index]);
                }
                if(argc>16) VM_RETURN(DIAMOND_VM_ARITY_ERROR);
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
                    if(method_name->length==3&&
                       memcmp(method_name->chars,"tap",3)==0) {
                        if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                           registers[base].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        DiamondClosure *called=(DiamondClosure *)registers[base].as.object;
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
                        if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(registers[recv].kind!=DIAMOND_VALUE_OBJECT) {
                            registers[dest]=registers[recv];break;
                        }
                        const DiamondObjectKind dup_kind=registers[recv].as.object->kind;
                        if(dup_kind==DIAMOND_OBJECT_STRING||dup_kind==DIAMOND_OBJECT_SYMBOL) {
                            /* Both are immutable in this VM (every String/
                             * Symbol-producing operation returns a new
                             * object rather than mutating in place), so a
                             * distinct copy would be observably identical
                             * -- returning the same object is correct, not
                             * just an optimization. */
                            registers[dest]=registers[recv];break;
                        }
                        if(dup_kind==DIAMOND_OBJECT_ARRAY) {
                            const DiamondArray *source=
                                (const DiamondArray *)registers[recv].as.object;
                            DiamondArray *copy=allocate_array(vm,source->values,source->count);
                            if(copy==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            /* constraints[]/constraint_count deliberately NOT
                             * copied -- allocate_array already zero-inits
                             * constraint_count, and that array is a lazily
                             * populated match-result cache (vm.c's own
                             * type-check-against-annotation sites), not an
                             * authoritative type tag; the copy just starts
                             * with a cold cache, refilled the same way the
                             * original's was. */
                            registers[dest]=DIAMOND_OBJECT(copy);break;
                        }
                        const DiamondHash *source=
                            (const DiamondHash *)registers[recv].as.object;
                        DiamondHash *copy=allocate_hash(vm);
                        if(copy==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        for(size_t index=0;index<source->count;index++)
                            if(!hash_set(vm,copy,source->entries[index].key,
                                         source->entries[index].value))
                                VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(copy);break;
                    }
                }
                if(registers[recv].kind==DIAMOND_VALUE_INT) {
                    /* chr, the inverse of String#ord -- a single byte (0-255),
                     * matching every other String primitive in this VM
                     * staying byte- rather than codepoint-oriented. */
                    const bool chr_method=method_name->length==3&&
                        memcmp(method_name->chars,"chr",3)==0;
                    if(chr_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const int64_t code=registers[recv].as.integer;
                        if(code<0||code>255) {
                            snprintf(vm->error,sizeof vm->error,
                                "Int#chr argument must be between 0 and 255");
                            VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                        }
                        const char byte=(char)(unsigned char)code;
                        DiamondString *chr_string=allocate_string(vm,&byte,1);
                        if(chr_string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(chr_string);break;
                    }
                    /* times/upto/downto -- trivial Callable[1] consumers of
                     * block syntax, forwarded to ordinary prelude Diamond
                     * functions (lib/core.di) exactly like Array/Hash's own
                     * Enumerable methods below, rather than hand-rolled
                     * here. */
                    const char *target_name=nullptr;
                    if(method_name->length==5&&
                       memcmp(method_name->chars,"times",5)==0)
                        target_name="integer_times";
                    else if(method_name->length==4&&
                            memcmp(method_name->chars,"upto",4)==0)
                        target_name="integer_upto";
                    else if(method_name->length==6&&
                            memcmp(method_name->chars,"downto",6)==0)
                        target_name="integer_downto";
                    if(target_name==nullptr) {
                        snprintf(vm->error,sizeof vm->error,
                            "undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Int");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    const DiamondFunction *target=
                        find_top_level_function(chunk,target_name,strlen(target_name));
                    if(target==nullptr) {
                        snprintf(vm->error,sizeof vm->error,
                            "internal error: missing standard library function '%s'",
                            target_name);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    const size_t total_argc=(size_t)argc+1;
                    if(total_argc<target->required_arity||total_argc>target->arity)
                        VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue forward_args[17];
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
                      .parameter_offset=target->owner_class==UINT8_MAX?0:1,
                      .register_count=target->register_count};
                    DiamondValue call_result=DIAMOND_NIL;
                    const DiamondVmStatus status=run_chunk(&child,vm,forward_args,
                        total_argc,depth+1,nullptr,&call_result);
                    VM_PROPAGATE(status);
                    registers[dest]=call_result;break;
                }
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondObjectKind receiver_kind=registers[recv].as.object->kind;
                if(receiver_kind==DIAMOND_OBJECT_ARRAY||
                   receiver_kind==DIAMOND_OBJECT_HASH||
                   receiver_kind==DIAMOND_OBJECT_STRING) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const bool length_method=method_name->length==6&&
                        memcmp(method_name->chars,"length",6)==0;
                    if(length_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        size_t length=0;
                        if(receiver_kind==DIAMOND_OBJECT_ARRAY)
                            length=((DiamondArray *)registers[recv].as.object)->count;
                        else if(receiver_kind==DIAMOND_OBJECT_HASH)
                            length=((DiamondHash *)registers[recv].as.object)->count;
                        else length=((DiamondString *)registers[recv].as.object)->length;
                        registers[dest]=DIAMOND_INT((int64_t)length);break;
                    }
                    /* array_join_helper below does the real work -- kept out
                     * of this switch (like program_builder_run_helper is
                     * kept out of run_chunk's own INVOKE case) because its
                     * local StringBuilder alone (a 32-entry pointer array
                     * inside the struct) is real stack weight that would
                     * otherwise be baked into every run_chunk call's own
                     * frame, not just calls that actually reach #join --
                     * confirmed by an ASan stack-overflow regression this
                     * exact addition caused in legacy_0092.di's deep-
                     * recursion SystemStackError test before this was
                     * factored out. */
                    if(receiver_kind==DIAMOND_OBJECT_ARRAY) {
                        const bool join_method=method_name->length==4&&
                            memcmp(method_name->chars,"join",4)==0;
                        if(join_method) {
                            if(argc>1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            const char *separator_chars="";size_t separator_length=0;
                            if(argc==1) {
                                if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                                   registers[base].as.object->kind!=DIAMOND_OBJECT_STRING)
                                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                                const DiamondString *separator=
                                    (const DiamondString *)registers[base].as.object;
                                separator_chars=separator->chars;
                                separator_length=separator->length;
                            }
                            DiamondValue joined=DIAMOND_NIL;
                            const DiamondVmStatus join_status=array_join_helper(vm,chunk,depth,
                                (const DiamondArray *)registers[recv].as.object,
                                separator_chars,separator_length,&joined);
                            VM_PROPAGATE(join_status);
                            registers[dest]=joined;break;
                        }
                    }
                    if(receiver_kind==DIAMOND_OBJECT_ARRAY||receiver_kind==DIAMOND_OBJECT_HASH) {
                        const char *target_name=nullptr;
                        if(method_name->length==4&&memcmp(method_name->chars,"each",4)==0)
                            target_name=receiver_kind==DIAMOND_OBJECT_ARRAY?
                                "array_each":"hash_each";
                        else if(method_name->length==6&&
                                memcmp(method_name->chars,"select",6)==0)
                            target_name="enumerable_select";
                        else if(method_name->length==5&&
                                memcmp(method_name->chars,"count",5)==0)
                            target_name="enumerable_count";
                        else if(method_name->length==4&&
                                memcmp(method_name->chars,"any?",4)==0)
                            target_name="enumerable_any";
                        else if(method_name->length==4&&
                                memcmp(method_name->chars,"all?",4)==0)
                            target_name="enumerable_all";
                        else if(method_name->length==6&&
                                memcmp(method_name->chars,"reduce",6)==0)
                            target_name="enumerable_reduce";
                        else if(method_name->length==3&&
                                memcmp(method_name->chars,"map",3)==0)
                            target_name="enumerable_map";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==3&&
                                memcmp(method_name->chars,"sum",3)==0)
                            target_name="array_sum";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==6&&
                                memcmp(method_name->chars,"reject",6)==0)
                            target_name="array_reject";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"find",4)==0)
                            target_name="array_find";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==15&&
                                memcmp(method_name->chars,"each_with_index",15)==0)
                            target_name="array_each_with_index";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"sort",4)==0)
                            target_name="enumerable_sort";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==7&&
                                memcmp(method_name->chars,"sort_by",7)==0)
                            target_name="enumerable_sort_by";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==3&&
                                memcmp(method_name->chars,"min",3)==0)
                            target_name="enumerable_min";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==3&&
                                memcmp(method_name->chars,"max",3)==0)
                            target_name="enumerable_max";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==6&&
                                memcmp(method_name->chars,"min_by",6)==0)
                            target_name="array_min_by";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==6&&
                                memcmp(method_name->chars,"max_by",6)==0)
                            target_name="array_max_by";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"take",4)==0)
                            target_name="array_take";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"drop",4)==0)
                            target_name="array_drop";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==8&&
                                memcmp(method_name->chars,"flat_map",8)==0)
                            target_name="array_flat_map";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==9&&
                                memcmp(method_name->chars,"partition",9)==0)
                            target_name="array_partition";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==8&&
                                memcmp(method_name->chars,"group_by",8)==0)
                            target_name="array_group_by";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==3&&
                                memcmp(method_name->chars,"zip",3)==0)
                            target_name="array_zip";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==10&&
                                memcmp(method_name->chars,"each_slice",10)==0)
                            target_name="array_each_slice";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==9&&
                                memcmp(method_name->chars,"each_cons",9)==0)
                            target_name="array_each_cons";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==5&&
                                memcmp(method_name->chars,"tally",5)==0)
                            target_name="array_tally";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==5&&
                                memcmp(method_name->chars,"first",5)==0)
                            target_name="array_first";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==8&&
                                memcmp(method_name->chars,"first_or",8)==0)
                            target_name="array_first_or";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"last",4)==0)
                            target_name="array_last";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==7&&
                                memcmp(method_name->chars,"last_or",7)==0)
                            target_name="array_last_or";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==8&&
                                memcmp(method_name->chars,"include?",8)==0)
                            target_name="array_include";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==7&&
                                memcmp(method_name->chars,"reverse",7)==0)
                            target_name="array_reverse";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==6&&
                                memcmp(method_name->chars,"concat",6)==0)
                            target_name="array_concat";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==7&&
                                memcmp(method_name->chars,"compact",7)==0)
                            target_name="array_compact";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"uniq",4)==0)
                            target_name="array_uniq";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==7&&
                                memcmp(method_name->chars,"flatten",7)==0)
                            target_name="array_flatten";
                        else if(receiver_kind==DIAMOND_OBJECT_ARRAY&&
                                method_name->length==9&&
                                memcmp(method_name->chars,"delete_at",9)==0)
                            target_name="array_delete_at";
                        else if(method_name->length==6&&
                                memcmp(method_name->chars,"empty?",6)==0)
                            target_name=receiver_kind==DIAMOND_OBJECT_ARRAY?
                                "array_empty":"hash_empty";
                        else if(receiver_kind==DIAMOND_OBJECT_HASH&&
                                method_name->length==5&&
                                memcmp(method_name->chars,"fetch",5)==0)
                            target_name="hash_fetch";
                        else if(receiver_kind==DIAMOND_OBJECT_HASH&&
                                method_name->length==4&&
                                memcmp(method_name->chars,"keys",4)==0)
                            target_name="hash_keys";
                        else if(receiver_kind==DIAMOND_OBJECT_HASH&&
                                method_name->length==6&&
                                memcmp(method_name->chars,"values",6)==0)
                            target_name="hash_values";
                        else if(receiver_kind==DIAMOND_OBJECT_HASH&&
                                method_name->length==12&&
                                memcmp(method_name->chars,"include_key?",12)==0)
                            target_name="hash_include_key";
                        else if(receiver_kind==DIAMOND_OBJECT_HASH&&
                                method_name->length==10&&
                                memcmp(method_name->chars,"map_values",10)==0)
                            target_name="hash_map_values";
                        else if(receiver_kind==DIAMOND_OBJECT_HASH&&
                                method_name->length==5&&
                                memcmp(method_name->chars,"merge",5)==0)
                            target_name="hash_merge";
                        if(target_name!=nullptr) {
                            const DiamondFunction *target=
                                find_top_level_function(chunk,target_name,strlen(target_name));
                            if(target==nullptr) {
                                snprintf(vm->error,sizeof vm->error,
                                    "internal error: missing standard library function '%s'",
                                    target_name);
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const size_t total_argc=(size_t)argc+1;
                            if(total_argc<target->required_arity||total_argc>target->arity)
                                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondValue forward_args[17];
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
                              .parameter_offset=target->owner_class==UINT8_MAX?0:1,
                              .register_count=target->register_count};
                            DiamondValue call_result=DIAMOND_NIL;
                            const DiamondVmStatus status=run_chunk(&child,vm,forward_args,
                                total_argc,depth+1,nullptr,&call_result);
                            VM_PROPAGATE(status);
                            registers[dest]=call_result;break;
                        }
                    }
                    if(receiver_kind==DIAMOND_OBJECT_HASH) {
                        const bool key_method=method_name->length==6&&
                            memcmp(method_name->chars,"key_at",6)==0;
                        const bool value_method=method_name->length==8&&
                            memcmp(method_name->chars,"value_at",8)==0;
                        if(!key_method&&!value_method) {
                            snprintf(vm->error,sizeof vm->error,
                                "undefined method '%.*s' for %s",
                                (int)method_name->length,method_name->chars,"Hash");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(registers[base].kind!=DIAMOND_VALUE_INT)
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        DiamondHash *hash=(DiamondHash *)registers[recv].as.object;
                        const int64_t index=registers[base].as.integer;
                        if(index<0||(uint64_t)index>=hash->count) {
                            snprintf(vm->error,sizeof vm->error,
                                "index %" PRId64 " out of bounds for Hash of length %zu",
                                index,hash->count);
                            VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                        }
                        const DiamondHashEntry entry=hash->entries[(size_t)index];
                        registers[dest]=key_method?entry.key:entry.value;break;
                    }
                    if(receiver_kind==DIAMOND_OBJECT_STRING) {
                        const bool index_of_method=method_name->length==8&&
                            memcmp(method_name->chars,"index_of",8)==0;
                        const bool slice_method=method_name->length==5&&
                            memcmp(method_name->chars,"slice",5)==0;
                        const bool to_i_method=method_name->length==4&&
                            memcmp(method_name->chars,"to_i",4)==0;
                        const bool to_f_method=method_name->length==4&&
                            memcmp(method_name->chars,"to_f",4)==0;
                        const bool downcase_method=method_name->length==8&&
                            memcmp(method_name->chars,"downcase",8)==0;
                        const bool upcase_method=method_name->length==6&&
                            memcmp(method_name->chars,"upcase",6)==0;
                        const bool reverse_method=method_name->length==7&&
                            memcmp(method_name->chars,"reverse",7)==0;
                        const bool strip_method=method_name->length==5&&
                            memcmp(method_name->chars,"strip",5)==0;
                        const bool split_method=method_name->length==5&&
                            memcmp(method_name->chars,"split",5)==0;
                        const bool ord_method=method_name->length==3&&
                            memcmp(method_name->chars,"ord",3)==0;
                        const bool repeat_method=method_name->length==6&&
                            memcmp(method_name->chars,"repeat",6)==0;
                        const bool gsub_method=method_name->length==4&&
                            memcmp(method_name->chars,"gsub",4)==0;
                        const bool sub_method=method_name->length==3&&
                            memcmp(method_name->chars,"sub",3)==0;
                        const bool scan_method=method_name->length==4&&
                            memcmp(method_name->chars,"scan",4)==0;
                        const bool start_with_method=method_name->length==11&&
                            memcmp(method_name->chars,"start_with?",11)==0;
                        const bool end_with_method=method_name->length==9&&
                            memcmp(method_name->chars,"end_with?",9)==0;
                        const bool includes_method=method_name->length==8&&
                            memcmp(method_name->chars,"include?",8)==0;
                        const bool capitalize_method=method_name->length==10&&
                            memcmp(method_name->chars,"capitalize",10)==0;
                        const bool chars_method=method_name->length==5&&
                            memcmp(method_name->chars,"chars",5)==0;
                        const bool bytes_method=method_name->length==5&&
                            memcmp(method_name->chars,"bytes",5)==0;
                        const bool chomp_method=method_name->length==5&&
                            memcmp(method_name->chars,"chomp",5)==0;
                        const bool ljust_method=method_name->length==5&&
                            memcmp(method_name->chars,"ljust",5)==0;
                        const bool rjust_method=method_name->length==5&&
                            memcmp(method_name->chars,"rjust",5)==0;
                        const bool tr_method=method_name->length==2&&
                            memcmp(method_name->chars,"tr",2)==0;
                        const bool format_method=method_name->length==6&&
                            memcmp(method_name->chars,"format",6)==0;
                        const DiamondString *source=
                            (const DiamondString *)registers[recv].as.object;
                        if(gsub_method||sub_method) {
                            if(argc!=2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_REGEXP) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s pattern argument must be a Regexp",
                                    gsub_method?"gsub":"sub");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
                               registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s replacement argument must be a String",
                                    gsub_method?"gsub":"sub");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            DiamondValue replace_result=DIAMOND_NIL;
                            const DiamondVmStatus replace_status=regexp_replace_helper(vm,
                                (const DiamondRegexp *)registers[base].as.object,source,
                                (const DiamondString *)registers[(size_t)base+1].as.object,
                                gsub_method,&replace_result);
                            VM_PROPAGATE(replace_status);
                            registers[dest]=replace_result;break;
                        }
                        if(scan_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_REGEXP) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#scan argument must be a Regexp");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondVmStatus scan_status=regexp_scan_helper(vm,
                                (const DiamondRegexp *)registers[base].as.object,source,
                                registers,dest);
                            VM_PROPAGATE(scan_status);
                            break;
                        }
                        if(repeat_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_INT) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#repeat argument must be an Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const int64_t count=registers[base].as.integer;
                            if(count<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#repeat argument must be a non-negative Int");
                                VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                            }
                            size_t total_length=0;
                            if(ckd_mul(&total_length,source->length,(size_t)count)) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#repeat result is too large");
                                VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                            }
                            char *buffer=malloc(total_length+1);
                            if(buffer==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t copy=0;copy<(size_t)count;copy++)
                                memcpy(buffer+copy*source->length,source->chars,
                                       source->length);
                            DiamondString *repeated=
                                allocate_string(vm,buffer,total_length);
                            free(buffer);
                            if(repeated==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(repeated);break;
                        }
                        if(ord_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(source->length==0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "cannot take ord of an empty String");
                                VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                            }
                            registers[dest]=
                                DIAMOND_INT((unsigned char)source->chars[0]);
                            break;
                        }
                        if(split_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#split argument must be a String");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *separator=
                                (const DiamondString *)registers[base].as.object;
                            DiamondArray *pieces=allocate_array(vm,nullptr,0);
                            if(pieces==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            /* Root the result array in registers[dest] before any
                             * further allocation (each piece below) can trigger a
                             * GC collection - registers are the VM's root set. */
                            registers[dest]=DIAMOND_OBJECT(pieces);
                            if(separator->length==0) {
                                for(size_t index=0;index<source->length;index++) {
                                    DiamondString *piece=
                                        allocate_string(vm,source->chars+index,1);
                                    if(piece==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                    if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                }
                            } else {
                                size_t start=0,cursor=0;
                                while(cursor+separator->length<=source->length) {
                                    if(memcmp(source->chars+cursor,separator->chars,
                                              separator->length)==0) {
                                        DiamondString *piece=allocate_string(vm,
                                            source->chars+start,cursor-start);
                                        if(piece==nullptr)
                                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                        if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                        cursor+=separator->length;start=cursor;
                                    } else {
                                        cursor++;
                                    }
                                }
                                DiamondString *piece=allocate_string(vm,
                                    source->chars+start,source->length-start);
                                if(piece==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                            break;
                        }
                        if(strip_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            size_t start=0;
                            while(start<source->length&&
                                  isspace((unsigned char)source->chars[start]))start++;
                            size_t end=source->length;
                            while(end>start&&
                                  isspace((unsigned char)source->chars[end-1]))end--;
                            DiamondString *stripped=
                                allocate_string(vm,source->chars+start,end-start);
                            if(stripped==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(stripped);break;
                        }
                        if(reverse_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *reversed=
                                allocate_string(vm,source->chars,source->length);
                            if(reversed==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<reversed->length/2;index++) {
                                const char swap=reversed->chars[index];
                                reversed->chars[index]=
                                    reversed->chars[reversed->length-1-index];
                                reversed->chars[reversed->length-1-index]=swap;
                            }
                            registers[dest]=DIAMOND_OBJECT(reversed);break;
                        }
                        if(downcase_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *lowered=
                                allocate_string(vm,source->chars,source->length);
                            if(lowered==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<lowered->length;index++)
                                lowered->chars[index]=
                                    (char)tolower((unsigned char)lowered->chars[index]);
                            registers[dest]=DIAMOND_OBJECT(lowered);break;
                        }
                        if(upcase_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *raised=
                                allocate_string(vm,source->chars,source->length);
                            if(raised==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<raised->length;index++)
                                raised->chars[index]=
                                    (char)toupper((unsigned char)raised->chars[index]);
                            registers[dest]=DIAMOND_OBJECT(raised);break;
                        }
                        if(to_i_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            size_t position=0;bool negative=false;
                            if(position<source->length&&
                               (source->chars[position]=='-'||source->chars[position]=='+')) {
                                negative=source->chars[position]=='-';position++;
                            }
                            const size_t digit_start=position;
                            int64_t value=0;bool saw_digit=false;bool overflowed=false;
                            while(position<source->length&&
                                  source->chars[position]>='0'&&source->chars[position]<='9') {
                                saw_digit=true;
                                if(!overflowed) {
                                    int64_t widened=0;
                                    if(ckd_mul(&widened,value,(int64_t)10)||
                                       ckd_add(&value,widened,
                                               (int64_t)(source->chars[position]-'0')))
                                        overflowed=true;
                                }
                                position++;
                            }
                            if(!saw_digit) {
                                registers[dest]=DIAMOND_INT(0);
                                break;
                            }
                            if(overflowed) {
                                /* Wider than int64_t: promote instead of
                                 * raising, matching every other overflow
                                 * site now that Int auto-promotes. */
                                const DiamondValue bignum_result=
                                    diamond_bignum_from_decimal_digits(vm,
                                        source->chars+digit_start,
                                        position-digit_start,negative);
                                if(bignum_result.kind==DIAMOND_VALUE_NIL)
                                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                registers[dest]=bignum_result;
                                break;
                            }
                            registers[dest]=DIAMOND_INT(negative?-value:value);
                            break;
                        }
                        if(to_f_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            /* No leading-whitespace skip, matching to_i's
                             * convention -- strtod's own grammar would
                             * otherwise skip it. Overflow is allowed to
                             * become Infinity (unlike to_i, which must
                             * reject out-of-range values): Float already
                             * has a well-defined way to represent "too
                             * large", Int does not. */
                            double value=0.0;
                            if(source->length>0) {
                                const char first=source->chars[0];
                                if((first>='0'&&first<='9')||
                                   first=='+'||first=='-'||first=='.') {
                                    char *end=nullptr;
                                    const double parsed=strtod(source->chars,&end);
                                    if(end!=source->chars)value=parsed;
                                }
                            }
                            registers[dest]=DIAMOND_FLOAT(value);
                            break;
                        }
                        if(index_of_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#index_of argument must be a String");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *needle=
                                (const DiamondString *)registers[base].as.object;
                            registers[dest]=DIAMOND_NIL;
                            if(needle->length==0) {
                                registers[dest]=DIAMOND_INT(0);
                            } else if(needle->length<=source->length) {
                                for(size_t start=0;
                                    start+needle->length<=source->length;start++) {
                                    if(memcmp(source->chars+start,needle->chars,
                                              needle->length)==0) {
                                        registers[dest]=DIAMOND_INT((int64_t)start);break;
                                    }
                                }
                            }
                            break;
                        }
                        if(slice_method) {
                            if(argc!=2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_INT||
                               registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#slice arguments must be Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const int64_t start=registers[base].as.integer;
                            const int64_t requested_length=
                                registers[(size_t)base+1].as.integer;
                            if(start<0||(uint64_t)start>source->length||
                               requested_length<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "index %" PRId64 " out of bounds for String of length %zu",
                                    start,source->length);
                                VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                            }
                            const size_t available=source->length-(size_t)start;
                            const size_t take=(size_t)requested_length<available?
                                (size_t)requested_length:available;
                            DiamondString *sliced=
                                allocate_string(vm,source->chars+(size_t)start,take);
                            if(sliced==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(sliced);break;
                        }
                        if(start_with_method||end_with_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s argument must be a String",
                                    start_with_method?"start_with?":"end_with?");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *needle=
                                (const DiamondString *)registers[base].as.object;
                            const bool matches=needle->length<=source->length&&
                                memcmp(start_with_method?source->chars:
                                       source->chars+source->length-needle->length,
                                       needle->chars,needle->length)==0;
                            registers[dest]=DIAMOND_BOOL(matches);break;
                        }
                        if(includes_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#include? argument must be a String");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *needle=
                                (const DiamondString *)registers[base].as.object;
                            bool found=needle->length==0;
                            for(size_t start=0;
                                !found&&needle->length>0&&
                                start+needle->length<=source->length;start++) {
                                if(memcmp(source->chars+start,needle->chars,
                                          needle->length)==0)found=true;
                            }
                            registers[dest]=DIAMOND_BOOL(found);break;
                        }
                        if(capitalize_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondString *capitalized=
                                allocate_string(vm,source->chars,source->length);
                            if(capitalized==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            for(size_t index=0;index<capitalized->length;index++)
                                capitalized->chars[index]=
                                    (char)tolower((unsigned char)capitalized->chars[index]);
                            if(capitalized->length>0)
                                capitalized->chars[0]=
                                    (char)toupper((unsigned char)capitalized->chars[0]);
                            registers[dest]=DIAMOND_OBJECT(capitalized);break;
                        }
                        if(chars_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondArray *pieces=allocate_array(vm,nullptr,0);
                            if(pieces==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(pieces);
                            for(size_t index=0;index<source->length;index++) {
                                DiamondString *piece=
                                    allocate_string(vm,source->chars+index,1);
                                if(piece==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                if(!array_push(vm,pieces,DIAMOND_OBJECT(piece)))
                                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                            break;
                        }
                        if(bytes_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondArray *values=allocate_array(vm,nullptr,0);
                            if(values==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(values);
                            for(size_t index=0;index<source->length;index++) {
                                const DiamondValue byte_value=
                                    DIAMOND_INT((unsigned char)source->chars[index]);
                                if(!array_push(vm,values,byte_value))
                                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                            break;
                        }
                        if(chomp_method) {
                            if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            size_t end=source->length;
                            if(end>=2&&source->chars[end-2]=='\r'&&
                               source->chars[end-1]=='\n')
                                end-=2;
                            else if(end>=1&&(source->chars[end-1]=='\n'||
                                              source->chars[end-1]=='\r'))
                                end-=1;
                            DiamondString *chomped=
                                allocate_string(vm,source->chars,end);
                            if(chomped==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(chomped);break;
                        }
                        if(ljust_method||rjust_method) {
                            if(argc!=2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_INT) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s width argument must be an Int",
                                    ljust_method?"ljust":"rjust");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
                               registers[(size_t)base+1].as.object->kind!=
                                   DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s padding argument must be a String",
                                    ljust_method?"ljust":"rjust");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const int64_t width=registers[base].as.integer;
                            if(width<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s width argument must be a non-negative Int",
                                    ljust_method?"ljust":"rjust");
                                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            }
                            const DiamondString *pad=(const DiamondString *)
                                registers[(size_t)base+1].as.object;
                            if((uint64_t)width<=source->length) {
                                DiamondString *unchanged=
                                    allocate_string(vm,source->chars,source->length);
                                if(unchanged==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                                registers[dest]=DIAMOND_OBJECT(unchanged);break;
                            }
                            if(pad->length==0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#%s padding string must not be empty",
                                    ljust_method?"ljust":"rjust");
                                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            }
                            const size_t pad_needed=(size_t)width-source->length;
                            char *buffer=malloc((size_t)width+1);
                            if(buffer==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            if(ljust_method) {
                                memcpy(buffer,source->chars,source->length);
                                for(size_t index=0;index<pad_needed;index++)
                                    buffer[source->length+index]=
                                        pad->chars[index%pad->length];
                            } else {
                                for(size_t index=0;index<pad_needed;index++)
                                    buffer[index]=pad->chars[index%pad->length];
                                memcpy(buffer+pad_needed,source->chars,source->length);
                            }
                            DiamondString *justified=
                                allocate_string(vm,buffer,(size_t)width);
                            free(buffer);
                            if(justified==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(justified);break;
                        }
                        if(tr_method) {
                            if(argc!=2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                               registers[base].as.object->kind!=DIAMOND_OBJECT_STRING||
                               registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
                               registers[(size_t)base+1].as.object->kind!=
                                   DIAMOND_OBJECT_STRING) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#tr arguments must be Strings");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            const DiamondString *from_spec=(const DiamondString *)
                                registers[base].as.object;
                            const DiamondString *to_spec=(const DiamondString *)
                                registers[(size_t)base+1].as.object;
                            if(from_spec->length==0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "String#tr from-string must not be empty");
                                VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            }
                            bool from_negate=false;
                            ByteBuffer from_buffer={0};
                            DiamondVmStatus tr_status=
                                tr_expand_spec(from_spec,true,&from_negate,&from_buffer);
                            VM_PROPAGATE(tr_status);
                            bool to_negate_ignored=false;
                            ByteBuffer to_buffer={0};
                            tr_status=
                                tr_expand_spec(to_spec,false,&to_negate_ignored,&to_buffer);
                            if(tr_status!=DIAMOND_VM_OK) {
                                free(from_buffer.data);
                                VM_RETURN(tr_status);
                            }
                            bool member[256]={false};
                            for(size_t index=0;index<from_buffer.length;index++)
                                member[(unsigned char)from_buffer.data[index]]=true;
                            int map[256];
                            for(int code=0;code<256;code++)map[code]=-1;
                            if(!from_negate) {
                                for(size_t index=0;index<from_buffer.length;index++) {
                                    const unsigned char key=
                                        (unsigned char)from_buffer.data[index];
                                    const size_t to_index=index<to_buffer.length?
                                        index:to_buffer.length-1;
                                    map[key]=to_buffer.length==0?-2:
                                        (int)(unsigned char)to_buffer.data[to_index];
                                }
                            }
                            const int negate_replacement=to_buffer.length==0?-2:
                                (int)(unsigned char)to_buffer.data[to_buffer.length-1];
                            ByteBuffer result_buffer={0};
                            bool ok=true;
                            for(size_t index=0;index<source->length&&ok;index++) {
                                const unsigned char byte=
                                    (unsigned char)source->chars[index];
                                const int replacement=from_negate?
                                    (member[byte]?-1:negate_replacement):map[byte];
                                if(replacement==-1)
                                    ok=byte_buffer_append(&result_buffer,
                                        &source->chars[index],1);
                                else if(replacement!=-2) {
                                    const char byte_out=(char)(unsigned char)replacement;
                                    ok=byte_buffer_append(&result_buffer,&byte_out,1);
                                }
                            }
                            free(from_buffer.data);free(to_buffer.data);
                            if(!ok) {
                                free(result_buffer.data);
                                VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                            DiamondString *translated=allocate_string(vm,
                                result_buffer.data!=nullptr?result_buffer.data:"",
                                result_buffer.length);
                            free(result_buffer.data);
                            if(translated==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(translated);break;
                        }
                        if(format_method) {
                            if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                            DiamondValue formatted=DIAMOND_NIL;
                            const DiamondVmStatus format_status=string_format_helper(
                                vm,chunk,depth,source,registers[base],&formatted);
                            VM_PROPAGATE(format_status);
                            registers[dest]=formatted;break;
                        }
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"String");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(receiver_kind!=DIAMOND_OBJECT_ARRAY)
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondArray *array=(DiamondArray *)registers[recv].as.object;
                    const bool push_method=method_name->length==4&&
                        memcmp(method_name->chars,"push",4)==0;
                    const bool pop_method=method_name->length==3&&
                        memcmp(method_name->chars,"pop",3)==0;
                    if(push_method) {
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(!array_value_satisfies_constraints(array,registers[base])) {
                            snprintf(vm->error,sizeof vm->error,
                                     "array element violates its type annotation");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        if(!array_push(vm,array,registers[base]))
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=registers[recv];break;
                    }
                    if(pop_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        registers[dest]=array->count==0?DIAMOND_NIL:
                            array->values[--array->count];break;
                    }
                    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                        (int)method_name->length,method_name->chars,"Array");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(receiver_kind==DIAMOND_OBJECT_FIBER) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondFiber *target_fiber=
                        ((DiamondFiberHandle *)registers[recv].as.object)->fiber;
                    const bool resume_method=method_name->length==6&&
                        memcmp(method_name->chars,"resume",6)==0;
                    const bool status_method=method_name->length==6&&
                        memcmp(method_name->chars,"status",6)==0;
                    const bool alive_method=method_name->length==6&&
                        memcmp(method_name->chars,"alive?",6)==0;
                    if(status_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        const char *state_name=diamond_fiber_state_name(target_fiber->state);
                        DiamondString *string=allocate_string(vm,state_name,strlen(state_name));
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(alive_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        registers[dest]=DIAMOND_BOOL(
                            target_fiber->state!=DIAMOND_FIBER_COMPLETED&&
                            target_fiber->state!=DIAMOND_FIBER_FAILED);
                        break;
                    }
                    if(!resume_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Fiber");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc>1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    const DiamondValue resume_argument=argc==1?registers[base]:DIAMOND_NIL;
                    if(target_fiber->state!=DIAMOND_FIBER_RUNNABLE&&
                       target_fiber->state!=DIAMOND_FIBER_SUSPENDED) {
                        snprintf(vm->error,sizeof vm->error,
                                 "cannot resume a fiber that is not runnable or suspended");
                        VM_RETURN(DIAMOND_VM_FIBER_NOT_RESUMABLE);
                    }
                    diamond_fiber_resume(target_fiber,resume_argument);
                    diamond_fiber_run(target_fiber);
                    if(target_fiber->status!=DIAMOND_VM_OK&&
                       target_fiber->status!=DIAMOND_VM_YIELDED)
                        VM_PROPAGATE(target_fiber->status);
                    registers[dest]=target_fiber->result;break;
                }
                if(receiver_kind==DIAMOND_OBJECT_THREAD) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondThread *target_thread=
                        ((DiamondThreadHandle *)registers[recv].as.object)->thread;
                    const bool join_method=method_name->length==4&&
                        memcmp(method_name->chars,"join",4)==0;
                    const bool alive_method=method_name->length==6&&
                        memcmp(method_name->chars,"alive?",6)==0;
                    if(alive_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        registers[dest]=
                            DIAMOND_BOOL(!atomic_load(&target_thread->finished));
                        break;
                    }
                    if(!join_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Thread");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    /* Idempotent: pthread_join (once real threading lands)
                     * can only safely run once per thread, so the copy-
                     * back-and-cache work below only happens the first
                     * time -- a second .join() just re-reads the already-
                     * copied-into-*this*-vm's-heap result/re-raises the
                     * same cached exception, both now ordinary GC-rooted
                     * values (see mark_object's DIAMOND_OBJECT_THREAD
                     * branch above, gated on target_thread->joined for
                     * exactly this reason). */
                    pthread_mutex_lock(&target_thread->join_lock);
                    if(!target_thread->joined) {
                        if(target_thread->spawned)
                            pthread_join(target_thread->handle,nullptr);
                        if(!target_thread->internal_failure) {
                            DiamondValue copied=DIAMOND_NIL;
                            const bool copy_ok=copy_value_into_vm(vm,
                                target_thread->result,nullptr,
                                target_thread->child_program->classes,
                                chunk->classes,nullptr,&copied);
                            if(!copy_ok) {
                                pthread_mutex_unlock(&target_thread->join_lock);
                                snprintf(vm->error,sizeof vm->error,
                                    "Thread result does not support this type");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            target_thread->result=copied;
                        }
                        target_thread->joined=true;
                    }
                    pthread_mutex_unlock(&target_thread->join_lock);
                    if(target_thread->internal_failure) {
                        if((size_t)DIAMOND_CLASS_THREAD_ERROR>=chunk->class_count)
                            VM_RETURN(DIAMOND_VM_THREAD_ERROR);
                        DiamondInstance *thread_error=allocate_instance(vm,
                            &chunk->classes[DIAMOND_CLASS_THREAD_ERROR],nullptr);
                        if(thread_error==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        const char *message=
                            target_thread->child_vm->error[0]!='\0'?
                                target_thread->child_vm->error:"thread failed";
                        DiamondString *message_string=
                            allocate_string(vm,message,strlen(message));
                        if(message_string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        if(thread_error->field_count>0)
                            thread_error->fields[0]=DIAMOND_OBJECT(message_string);
                        vm->exception=DIAMOND_OBJECT(thread_error);
                        vm->has_exception=true;
                        if(catch_exception(vm,chunk,handlers,&handler_count,
                                           &pending,registers,&ip))break;
                        snprintf(vm->error,sizeof vm->error,
                            "uncaught exception: %s",thread_error->class->name);
                        VM_RETURN(DIAMOND_VM_EXCEPTION);
                    }
                    if(target_thread->raised) {
                        vm->exception=target_thread->result;
                        vm->has_exception=true;
                        if(catch_exception(vm,chunk,handlers,&handler_count,
                                           &pending,registers,&ip))break;
                        if(vm->exception.kind==DIAMOND_VALUE_OBJECT&&
                           vm->exception.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                            const DiamondInstance *raised_instance=
                                (const DiamondInstance *)vm->exception.as.object;
                            snprintf(vm->error,sizeof vm->error,
                                "uncaught exception: %s",raised_instance->class->name);
                        } else {
                            snprintf(vm->error,sizeof vm->error,"uncaught exception");
                        }
                        VM_RETURN(DIAMOND_VM_EXCEPTION);
                    }
                    registers[dest]=target_thread->result;break;
                }
                if(receiver_kind==DIAMOND_OBJECT_FILE) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondFileHandle *target_file=
                        (DiamondFileHandle *)registers[recv].as.object;
                    const bool read_method=method_name->length==4&&
                        memcmp(method_name->chars,"read",4)==0;
                    const bool gets_method=method_name->length==4&&
                        memcmp(method_name->chars,"gets",4)==0;
                    const bool write_method=method_name->length==5&&
                        memcmp(method_name->chars,"write",5)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!read_method&&!gets_method&&!write_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"File");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(close_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(target_file->stream!=nullptr) {
                            fclose(target_file->stream);
                            target_file->stream=nullptr;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(target_file->stream==nullptr) {
                        snprintf(vm->error,sizeof vm->error,"file is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    if(read_method) {
                        if(argc>1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        bool bounded=false;size_t limit=0;
                        if(argc==1) {
                            if(registers[base].kind!=DIAMOND_VALUE_INT||
                               registers[base].as.integer<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "File#read argument must be a non-negative Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            bounded=true;limit=(size_t)registers[base].as.integer;
                        }
                        StringBuilder builder={};
                        char chunk_buffer[4096];
                        size_t read_count=0;
                        errno=0;
                        while(!bounded||builder.length<limit) {
                            const size_t remaining=bounded?limit-builder.length:sizeof chunk_buffer;
                            const size_t want=remaining<sizeof chunk_buffer?
                                remaining:sizeof chunk_buffer;
                            read_count=fread(chunk_buffer,1,want,target_file->stream);
                            if(read_count==0)break;
                            if(!builder_append(&builder,chunk_buffer,read_count)) {
                                free(builder.chars);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                        }
                        if(ferror(target_file->stream)) {
                            free(builder.chars);
                            snprintf(vm->error,sizeof vm->error,"read error: %s",strerror(errno));
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                        free(builder.chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(gets_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        StringBuilder builder={};
                        bool saw_any=false;
                        DiamondVmStatus read_status=
                            read_line(vm,target_file->stream,&builder,&saw_any);
                        if(read_status!=DIAMOND_VM_OK) {
                            free(builder.chars);VM_RETURN(read_status);
                        }
                        if(!saw_any) {
                            free(builder.chars);
                            registers[dest]=DIAMOND_NIL;break;
                        }
                        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                        free(builder.chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue converted=DIAMOND_NIL;
                    DiamondVmStatus status=stringify_value(vm,chunk,depth,
                        registers[base],&converted);
                    VM_PROPAGATE(status);
                    const DiamondString *text=(const DiamondString *)converted.as.object;
                    errno=0;
                    const size_t written=fwrite(text->chars,1,text->length,target_file->stream);
                    if(written!=text->length||ferror(target_file->stream)) {
                        snprintf(vm->error,sizeof vm->error,"write error: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    registers[dest]=DIAMOND_NIL;break;
                }
                if(receiver_kind==DIAMOND_OBJECT_LISTENER) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondListenerHandle *listener=
                        (DiamondListenerHandle *)registers[recv].as.object;
                    const bool accept_method=method_name->length==6&&
                        memcmp(method_name->chars,"accept",6)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!accept_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Listener");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(close_method) {
                        if(listener->fd>=0) {
                            close(listener->fd);
                            listener->fd=-1;
                        }
                        if(listener->tls_context!=nullptr) {
                            SSL_CTX_free(listener->tls_context);
                            listener->tls_context=nullptr;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(listener->fd<0) {
                        snprintf(vm->error,sizeof vm->error,"listener is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    errno=0;
                    int client_fd=accept(listener->fd,nullptr,nullptr);
                    /* A blocking accept() can sit here indefinitely with
                     * nothing connecting -- exactly when a trapped signal
                     * needs to actually interrupt it (see
                     * diamond_signal_handler's own comment: no
                     * SA_RESTART, specifically so this EINTR happens)
                     * rather than waiting for a connection that may never
                     * arrive before the handler ever gets to run. Handles
                     * the pending signal(s), then transparently retries --
                     * a blocking listener's own .accept() semantics
                     * (blocks until a real connection or a real error)
                     * are unchanged from the caller's perspective. */
                    while(client_fd<0&&errno==EINTR) {
                        bool signal_invoked=false;
                        const DiamondVmStatus signal_status=
                            dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
                        VM_PROPAGATE_SIGNAL(signal_status);
                        errno=0;
                        client_fd=accept(listener->fd,nullptr,nullptr);
                    }
                    if(client_fd<0) {
                        if(listener->nonblocking&&(errno==EAGAIN||errno==EWOULDBLOCK)) {
                            registers[dest]=DIAMOND_NIL;break;
                        }
                        snprintf(vm->error,sizeof vm->error,"accept failed: %s",strerror(errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    if(listener->nonblocking) {
                        /* Unlike some other platforms, Linux's accept() never
                         * inherits O_NONBLOCK from the listening socket -- the
                         * accepted connection comes back blocking by default
                         * and must be set non-blocking explicitly, same as the
                         * listener itself was in tcp_listen_helper. */
                        const int flags=fcntl(client_fd,F_GETFL,0);
                        if(flags<0||fcntl(client_fd,F_SETFL,flags|O_NONBLOCK)<0) {
                            snprintf(vm->error,sizeof vm->error,
                                "accept failed: %s",strerror(errno));
                            close(client_fd);
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        DiamondSocketHandle *client_socket=
                            allocate_socket_handle(vm,client_fd);
                        if(client_socket==nullptr) {
                            close(client_fd);
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        }
                        registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                            .as.object=(DiamondObject *)client_socket};
                        break;
                    }
                    if(listener->tls_context!=nullptr) {
                        /* No EINTR-retry around SSL_accept itself, unlike
                         * the accept() above it -- the "server sits idle
                         * with nothing connecting" indefinite-wait case is
                         * accept()'s alone; once a connection exists, the
                         * handshake that follows is bounded (a couple of
                         * network round trips), the same category
                         * TCPSocket.connect's own connect() call is in, and
                         * gets the same documented scope cut (see
                         * tcp_connect_helper). */
                        SSL *ssl=SSL_new(listener->tls_context);
                        if(ssl==nullptr) {
                            close(client_fd);
                            char detail[256];tls_format_error(detail,sizeof detail);
                            snprintf(vm->error,sizeof vm->error,
                                "cannot create TLS session: %s",detail);
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        SSL_set_fd(ssl,client_fd);
                        ERR_clear_error();
                        if(SSL_accept(ssl)!=1) {
                            char detail[256];tls_format_error(detail,sizeof detail);
                            snprintf(vm->error,sizeof vm->error,
                                "TLS handshake failed: %s",detail);
                            SSL_free(ssl);close(client_fd);
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        DiamondTlsSocketHandle *client_tls=
                            allocate_tls_socket_handle(vm,ssl,client_fd);
                        if(client_tls==nullptr) {
                            SSL_free(ssl);close(client_fd);
                            VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        }
                        registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                            .as.object=(DiamondObject *)client_tls};
                        break;
                    }
                    FILE *client_stream=fdopen(client_fd,"r+");
                    if(client_stream==nullptr) {
                        close(client_fd);
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    DiamondFileHandle *client_handle=allocate_file_handle(vm,client_stream);
                    if(client_handle==nullptr) {
                        fclose(client_stream);
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                        .as.object=(DiamondObject *)client_handle};
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_SOCKET) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondSocketHandle *socket_handle=
                        (DiamondSocketHandle *)registers[recv].as.object;
                    const bool read_method=method_name->length==4&&
                        memcmp(method_name->chars,"read",4)==0;
                    const bool write_method=method_name->length==5&&
                        memcmp(method_name->chars,"write",5)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!read_method&&!write_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Socket");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(close_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(socket_handle->fd>=0) {
                            close(socket_handle->fd);
                            socket_handle->fd=-1;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(socket_handle->fd<0) {
                        snprintf(vm->error,sizeof vm->error,"socket is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    if(read_method) {
                        if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(registers[base].kind!=DIAMOND_VALUE_INT||
                           registers[base].as.integer<0) {
                            snprintf(vm->error,sizeof vm->error,
                                "Socket#read argument must be a non-negative Int");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        const size_t want=(size_t)registers[base].as.integer;
                        if(want==0) {
                            DiamondString *empty=allocate_string(vm,"",0);
                            if(empty==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            registers[dest]=DIAMOND_OBJECT(empty);break;
                        }
                        char *buffer=malloc(want);
                        if(buffer==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        errno=0;
                        const ssize_t read_count=read(socket_handle->fd,buffer,want);
                        if(read_count<0) {
                            const int saved_errno=errno;
                            free(buffer);
                            if(saved_errno==EAGAIN||saved_errno==EWOULDBLOCK) {
                                snprintf(vm->error,sizeof vm->error,"read would block");
                                VM_RETURN(DIAMOND_VM_WOULD_BLOCK);
                            }
                            snprintf(vm->error,sizeof vm->error,"read error: %s",
                                strerror(saved_errno));
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        if(read_count==0) {
                            /* Peer closed -- the same EOF-as-nil convention
                             * File#read already uses, distinct from
                             * WouldBlockError (nothing available *yet* vs.
                             * nothing ever coming again). */
                            free(buffer);
                            registers[dest]=DIAMOND_NIL;break;
                        }
                        DiamondString *string=allocate_string(vm,buffer,(size_t)read_count);
                        free(buffer);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue converted=DIAMOND_NIL;
                    DiamondVmStatus stringify_status=stringify_value(vm,chunk,depth,
                        registers[base],&converted);
                    VM_PROPAGATE(stringify_status);
                    const DiamondString *text=(const DiamondString *)converted.as.object;
                    errno=0;
                    const ssize_t written=write(socket_handle->fd,text->chars,text->length);
                    if(written<0) {
                        const int saved_errno=errno;
                        if(saved_errno==EAGAIN||saved_errno==EWOULDBLOCK) {
                            snprintf(vm->error,sizeof vm->error,"write would block");
                            VM_RETURN(DIAMOND_VM_WOULD_BLOCK);
                        }
                        snprintf(vm->error,sizeof vm->error,"write error: %s",
                            strerror(saved_errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    /* A partial write is a normal, expected outcome on a
                     * non-blocking socket (the send buffer filled up
                     * mid-write) -- unlike File#write, which either writes
                     * everything or raises, this returns the actual byte
                     * count written so the caller (packages/gremlin's
                     * NonblockingConnection#write) can retry the remainder. */
                    registers[dest]=DIAMOND_INT((int64_t)written);break;
                }
                if(receiver_kind==DIAMOND_OBJECT_UDP_SOCKET) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondUdpSocketHandle *udp_handle=
                        (DiamondUdpSocketHandle *)registers[recv].as.object;
                    const bool send_method=method_name->length==4&&
                        memcmp(method_name->chars,"send",4)==0;
                    const bool receive_method=method_name->length==7&&
                        memcmp(method_name->chars,"receive",7)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!send_method&&!receive_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"UDPSocket");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(close_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(udp_handle->fd>=0) {
                            close(udp_handle->fd);
                            udp_handle->fd=-1;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(udp_handle->fd<0) {
                        snprintf(vm->error,sizeof vm->error,"UDP socket is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    if(send_method) {
                        if(argc!=3)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        DiamondValue converted=DIAMOND_NIL;
                        DiamondVmStatus stringify_status=stringify_value(vm,chunk,depth,
                            registers[base],&converted);
                        VM_PROPAGATE(stringify_status);
                        const DiamondString *text=(const DiamondString *)converted.as.object;
                        if(registers[(size_t)base+1].kind!=DIAMOND_VALUE_OBJECT||
                           registers[(size_t)base+1].as.object->kind!=DIAMOND_OBJECT_STRING||
                           registers[(size_t)base+2].kind!=DIAMOND_VALUE_INT) {
                            snprintf(vm->error,sizeof vm->error,
                                "UDPSocket#send arguments must be (data, String host, Int port)");
                            VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                        }
                        const DiamondString *host=
                            (const DiamondString *)registers[(size_t)base+1].as.object;
                        char port_text[32];
                        (void)snprintf(port_text,sizeof port_text,"%" PRId64,
                                       registers[(size_t)base+2].as.integer);
                        struct addrinfo hints={.ai_family=AF_UNSPEC,.ai_socktype=SOCK_DGRAM};
                        struct addrinfo *results=nullptr;
                        const int resolve_status=
                            getaddrinfo(host->chars,port_text,&hints,&results);
                        if(resolve_status!=0) {
                            snprintf(vm->error,sizeof vm->error,"cannot resolve '%.*s:%s': %s",
                                     (int)host->length,host->chars,port_text,
                                     gai_strerror(resolve_status));
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        /* Tries each resolved candidate against this same
                         * existing socket until one succeeds -- host may
                         * resolve to both IPv4 and IPv6 addresses, and
                         * sendto fails outright on a family mismatch with
                         * whichever family this socket happened to be
                         * created with (see udp_socket_helper), so this is
                         * the sendto-time equivalent of TCPSocket.connect's
                         * own try-each-candidate resilience. */
                        ssize_t sent=-1;
                        int last_errno=0;
                        for(struct addrinfo *candidate=results;candidate!=nullptr;
                            candidate=candidate->ai_next) {
                            errno=0;
                            sent=sendto(udp_handle->fd,text->chars,text->length,0,
                                        candidate->ai_addr,candidate->ai_addrlen);
                            if(sent>=0)break;
                            last_errno=errno;
                        }
                        freeaddrinfo(results);
                        if(sent<0) {
                            snprintf(vm->error,sizeof vm->error,"send error: %s",
                                strerror(last_errno));
                            VM_RETURN(DIAMOND_VM_IO_ERROR);
                        }
                        registers[dest]=DIAMOND_INT((int64_t)sent);break;
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(registers[base].kind!=DIAMOND_VALUE_INT||
                       registers[base].as.integer<=0) {
                        snprintf(vm->error,sizeof vm->error,
                            "UDPSocket#receive argument must be a positive Int");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    const size_t want=(size_t)registers[base].as.integer;
                    char *buffer=malloc(want);
                    if(buffer==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    struct sockaddr_storage source_addr={0};
                    socklen_t source_addr_len=sizeof source_addr;
                    errno=0;
                    ssize_t received=recvfrom(udp_handle->fd,buffer,want,0,
                        (struct sockaddr *)&source_addr,&source_addr_len);
                    /* Same reasoning as blocking accept()/IO.poll above:
                     * a UDP server loop's own .receive() can block
                     * indefinitely with nothing arriving, exactly when a
                     * trapped signal needs to interrupt it promptly. */
                    while(received<0&&errno==EINTR) {
                        bool signal_invoked=false;
                        const DiamondVmStatus signal_status=
                            dispatch_pending_signals(vm,chunk,depth,&signal_invoked);
                        if(signal_status!=DIAMOND_VM_OK) {
                            free(buffer);
                            if(signal_status==DIAMOND_VM_EXCEPTION&&
                               catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                   registers,&ip))
                                goto dispatch_continue;
                            VM_RETURN(signal_status);
                        }
                        source_addr_len=sizeof source_addr;
                        errno=0;
                        received=recvfrom(udp_handle->fd,buffer,want,0,
                            (struct sockaddr *)&source_addr,&source_addr_len);
                    }
                    if(received<0) {
                        const int saved_errno=errno;
                        free(buffer);
                        snprintf(vm->error,sizeof vm->error,"receive error: %s",
                            strerror(saved_errno));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    char host_buffer[NI_MAXHOST];
                    char port_buffer[NI_MAXSERV];
                    const int name_status=getnameinfo((struct sockaddr *)&source_addr,
                        source_addr_len,host_buffer,sizeof host_buffer,
                        port_buffer,sizeof port_buffer,NI_NUMERICHOST|NI_NUMERICSERV);
                    if(name_status!=0) {
                        free(buffer);
                        snprintf(vm->error,sizeof vm->error,
                            "cannot resolve sender address: %s",gai_strerror(name_status));
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    const int64_t source_port=strtoll(port_buffer,nullptr,10);
                    /* Same GC-safety pattern as DIAMOND_OP_IO_POLL's own
                     * Hash result (see docs/io.md): root the Hash in
                     * registers[dest] immediately, then for any entry whose
                     * value needs its own further allocation, root the key
                     * first with a nil placeholder -- nil needs no
                     * protection -- before allocating the real value and
                     * overwriting it. "port" is a scalar Int with no
                     * allocation of its own, so its key+value can go in
                     * with one hash_set, no placeholder needed. */
                    DiamondHash *receive_result=allocate_hash(vm);
                    if(receive_result==nullptr) {
                        free(buffer);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    registers[dest]=DIAMOND_OBJECT(receive_result);
                    DiamondString *data_key=allocate_string(vm,"data",4);
                    if(data_key==nullptr) {
                        free(buffer);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(data_key),DIAMOND_NIL)) {
                        free(buffer);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    }
                    DiamondString *data_string=allocate_string(vm,buffer,(size_t)received);
                    free(buffer);
                    if(data_string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(data_key),
                            DIAMOND_OBJECT(data_string)))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    DiamondString *host_key=allocate_string(vm,"host",4);
                    if(host_key==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(host_key),DIAMOND_NIL))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    DiamondString *host_string=
                        allocate_string(vm,host_buffer,strlen(host_buffer));
                    if(host_string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(host_key),
                            DIAMOND_OBJECT(host_string)))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    DiamondString *port_key=allocate_string(vm,"port",4);
                    if(port_key==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    if(!hash_set(vm,receive_result,DIAMOND_OBJECT(port_key),
                            DIAMOND_INT(source_port)))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_TLS_SOCKET) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondTlsSocketHandle *tls_handle=
                        (DiamondTlsSocketHandle *)registers[recv].as.object;
                    const bool read_method=method_name->length==4&&
                        memcmp(method_name->chars,"read",4)==0;
                    const bool gets_method=method_name->length==4&&
                        memcmp(method_name->chars,"gets",4)==0;
                    const bool write_method=method_name->length==5&&
                        memcmp(method_name->chars,"write",5)==0;
                    const bool close_method=method_name->length==5&&
                        memcmp(method_name->chars,"close",5)==0;
                    if(!read_method&&!gets_method&&!write_method&&!close_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"TLSSocket");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(close_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        if(tls_handle->ssl!=nullptr) {
                            SSL_shutdown(tls_handle->ssl);
                            SSL_free(tls_handle->ssl);
                            tls_handle->ssl=nullptr;
                            if(tls_handle->fd>=0)close(tls_handle->fd);
                            tls_handle->fd=-1;
                        }
                        registers[dest]=DIAMOND_NIL;break;
                    }
                    if(tls_handle->ssl==nullptr) {
                        snprintf(vm->error,sizeof vm->error,"TLS socket is closed");
                        VM_RETURN(DIAMOND_VM_IO_ERROR);
                    }
                    if(read_method) {
                        if(argc>1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        bool bounded=false;size_t limit=0;
                        if(argc==1) {
                            if(registers[base].kind!=DIAMOND_VALUE_INT||
                               registers[base].as.integer<0) {
                                snprintf(vm->error,sizeof vm->error,
                                    "TLSSocket#read argument must be a non-negative Int");
                                VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                            }
                            bounded=true;limit=(size_t)registers[base].as.integer;
                        }
                        StringBuilder builder={};
                        char chunk_buffer[4096];
                        for(;;) {
                            if(bounded&&builder.length>=limit)break;
                            const size_t remaining=bounded?limit-builder.length:sizeof chunk_buffer;
                            const size_t want=remaining<sizeof chunk_buffer?
                                remaining:sizeof chunk_buffer;
                            size_t got=0;bool eof=false;
                            const DiamondVmStatus read_status=
                                tls_read_chunk(vm,tls_handle->ssl,chunk_buffer,want,&got,&eof);
                            if(read_status!=DIAMOND_VM_OK) {
                                free(builder.chars);VM_RETURN(read_status);
                            }
                            if(eof||got==0)break;
                            if(!builder_append(&builder,chunk_buffer,got)) {
                                free(builder.chars);VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                            }
                        }
                        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                        free(builder.chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(gets_method) {
                        if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        StringBuilder builder={};
                        bool saw_any=false;
                        const DiamondVmStatus read_status=
                            tls_read_line(vm,tls_handle->ssl,&builder,&saw_any);
                        if(read_status!=DIAMOND_VM_OK) {
                            free(builder.chars);VM_RETURN(read_status);
                        }
                        if(!saw_any) {
                            free(builder.chars);
                            registers[dest]=DIAMOND_NIL;break;
                        }
                        DiamondString *string=allocate_string(vm,builder.chars,builder.length);
                        free(builder.chars);
                        if(string==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                        registers[dest]=DIAMOND_OBJECT(string);break;
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondValue converted=DIAMOND_NIL;
                    DiamondVmStatus stringify_status=stringify_value(vm,chunk,depth,
                        registers[base],&converted);
                    VM_PROPAGATE(stringify_status);
                    const DiamondString *text=(const DiamondString *)converted.as.object;
                    const DiamondVmStatus write_status=
                        tls_write_all(vm,tls_handle->ssl,text->chars,text->length);
                    VM_PROPAGATE(write_status);
                    registers[dest]=DIAMOND_NIL;break;
                }
                if(receiver_kind==DIAMOND_OBJECT_REGEXP) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const bool match_method=method_name->length==5&&
                        memcmp(method_name->chars,"match",5)==0;
                    const bool match_p_method=method_name->length==6&&
                        memcmp(method_name->chars,"match?",6)==0;
                    if(!match_method&&!match_p_method) {
                        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
                            (int)method_name->length,method_name->chars,"Regexp");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                       registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                        snprintf(vm->error,sizeof vm->error,
                            "Regexp#%.*s argument must be a String",
                            (int)method_name->length,method_name->chars);
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    const DiamondVmStatus match_status=regexp_match_helper(vm,
                        (const DiamondRegexp *)registers[recv].as.object,
                        (const DiamondString *)registers[base].as.object,
                        match_p_method,registers,dest);
                    VM_PROPAGATE(match_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_SQLITE3) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondVmStatus dispatch_status=sqlite3_dispatch_helper(vm,
                        (DiamondSqlite3Handle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_POSTGRES) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondVmStatus dispatch_status=postgres_dispatch_helper(vm,
                        (DiamondPostgresHandle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_MYSQL) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondVmStatus dispatch_status=mysql_dispatch_helper(vm,
                        (DiamondMysqlHandle *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_TIME) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondVmStatus dispatch_status=time_dispatch_helper(vm,
                        (DiamondTime *)registers[recv].as.object,
                        method_name,registers,base,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROCESS_RESULT) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondVmStatus dispatch_status=process_result_dispatch_helper(vm,
                        (DiamondProcessResult *)registers[recv].as.object,
                        method_name,registers,argc,dest);
                    VM_PROPAGATE(dispatch_status);
                    break;
                }
                if(receiver_kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondValue invoke_result=DIAMOND_NIL;
                    const DiamondVmStatus invoke_status=program_builder_invoke_helper(vm,
                        (DiamondProgramBuilder *)registers[recv].as.object,method_name,
                        registers,base,argc,depth,&invoke_result);
                    VM_PROPAGATE(invoke_status);
                    registers[dest]=invoke_result;break;
                }
                if(receiver_kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:chunk;
                /* tap/dup/respond_to? -- same three universal methods the
                 * non-Instance branch above already handles, but gated on
                 * `lookup_method` coming back empty first: unlike a native
                 * type, a class CAN legitimately define its own `dup` (for
                 * real deep-copy semantics) or `tap`/`respond_to?`, and
                 * that user definition must win -- checked the same way
                 * Ruby's own method resolution order would put a class's
                 * own method ahead of an inherited Kernel one. */
                if(method_name->length==3&&memcmp(method_name->chars,"tap",3)==0&&
                   lookup_method(owner,instance->class,"tap",3)==nullptr) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                       registers[base].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    DiamondClosure *called=(DiamondClosure *)registers[base].as.object;
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
                if(method_name->length==3&&memcmp(method_name->chars,"dup",3)==0&&
                   lookup_method(owner,instance->class,"dup",3)==nullptr) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    DiamondInstance *copy=allocate_instance(vm,instance->class,instance->owner);
                    if(copy==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    for(size_t index=0;index<instance->field_count;index++)
                        copy->fields[index]=instance->fields[index];
                    /* allocate_instance always starts a fresh instance at
                     * shapes[0] (no fields considered materialized yet) --
                     * GET_IVAR treats a field as nil whenever its index
                     * isn't below shape->field_count, regardless of what's
                     * actually sitting in fields[] (lookup_field_cached's
                     * own `materialized` flag). Without also copying the
                     * source's current shape, every field on the copy read
                     * back as nil despite the values above being copied
                     * correctly -- confirmed directly, not assumed: the
                     * very first `.dup()` smoke test on a two-ivar class
                     * hit exactly this. shapes[] lives on the (shared)
                     * class, so aliasing the pointer is safe. */
                    copy->shape=instance->shape;
                    registers[dest]=DIAMOND_OBJECT(copy);break;
                }
                if(method_name->length==11&&
                   memcmp(method_name->chars,"respond_to?",11)==0&&
                   lookup_method(owner,instance->class,"respond_to?",11)==nullptr) {
                    if(type_argument_count!=0)VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    if(argc!=1)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                       registers[base].as.object->kind!=DIAMOND_OBJECT_SYMBOL)
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondSymbol *probe=(const DiamondSymbol *)registers[base].as.object;
                    const DiamondMethod *probed=lookup_method(owner,instance->class,
                        probe->chars,probe->length);
                    registers[dest]=DIAMOND_BOOL(probed!=nullptr&&!probed->is_private);
                    break;
                }
                bool exception_instance=false;
                const DiamondClass *ancestor=instance->class;
                while(ancestor!=nullptr) {
                    if(ancestor==&owner->classes[DIAMOND_CLASS_EXCEPTION]) {
                        exception_instance=true;break;
                    }
                    ancestor=ancestor->superclass==UINT8_MAX?nullptr:
                        &owner->classes[ancestor->superclass];
                }
                if(exception_instance&&method_name->length==7&&
                   memcmp(method_name->chars,"message",7)==0) {
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    registers[dest]=instance->field_count>0?
                        instance->fields[0]:DIAMOND_NIL;break;
                }
                if(exception_instance&&method_name->length==5&&
                   memcmp(method_name->chars,"cause",5)==0) {
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    registers[dest]=instance->field_count>1?
                        instance->fields[1]:DIAMOND_NIL;break;
                }
                if(exception_instance&&method_name->length==9&&
                   memcmp(method_name->chars,"backtrace",9)==0) {
                    if(argc!=0)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                    registers[dest]=instance->field_count>2?
                        instance->fields[2]:DIAMOND_NIL;break;
                }
                const uint8_t *site=chunk->code+instruction_offset;
                const size_t cache_slot=((size_t)(uintptr_t)site>>2)%
                    DIAMOND_INLINE_CACHE_COUNT;
                DiamondMethodCache *cache=&vm->method_caches[cache_slot];
                const DiamondMethod *method=nullptr;
                if ((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_MONO &&
                    cache->site==site && cache->entry_count==1 &&
                    cache->entries[0].receiver_class==instance->class) {
                    method=cache->entries[0].method;
                    vm->inline_cache_hits++;
                    vm->monomorphic_dispatches++;
                } else {
                    if ((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_MONO) {
                        uint8_t *code=(uint8_t *)(void *)chunk->code;
                        code[instruction_offset]=(uint8_t)DIAMOND_OP_INVOKE;
                    }
                    method=lookup_method_cached(vm,owner,site,instance->class,
                        method_name->chars,method_name->length);
                    if ((DiamondOpCode)instruction==DIAMOND_OP_INVOKE &&
                        cache->entry_count==1 &&
                        cache->hits>=vm->monomorphic_threshold) {
                        uint8_t *code=(uint8_t *)(void *)chunk->code;
                        code[instruction_offset]=(uint8_t)DIAMOND_OP_INVOKE_MONO;
                        record_rewritten_site(vm,site);
                        vm->direct_dispatch_rewrites++;
                    }
                }
                if(method==nullptr) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(method->is_private&&!(chunk->parameter_offset==1&&recv==0)) {
                    snprintf(vm->error,sizeof vm->error,
                        "private method '%.*s' called with an explicit receiver",
                        (int)method_name->length,method_name->chars);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(argc<method->required_arity||argc>method->arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17];args[0]=registers[recv];
                for(size_t i=0;i<argc;i++)args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=owner->functions[method->function_index];
                if((DiamondOpCode)instruction==DIAMOND_OP_INVOKE_TYPED&&
                   type_argument_count!=fn->type_variable_count)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondTypeBinding explicit_bindings[8]={};
                for(size_t index=0;index<type_argument_count;index++) {
                    if((size_t)type_arguments[index]>=chunk->type_set_count)
                        VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                    (void)binding_node(&explicit_bindings[index]);
                    bind_context_set(&explicit_bindings[index],0,chunk,
                        chunk->type_sets,type_arguments[index]);
                }
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                  .functions=owner->functions,.function_count=owner->function_count,
                  .classes=owner->classes,.class_count=owner->class_count,
                  .interfaces=owner->interfaces,.interface_count=owner->interface_count,
                  .parameter_type_sets=fn->parameter_type_sets,
                  .type_variable_count=fn->type_variable_count,
                  .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                  .type_variable_bindings=type_argument_count==0?nullptr:
                      explicit_bindings,
                  .register_count=fn->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus s=run_chunk(&child,vm,args,(size_t)argc+1,depth+1,nullptr,&call_result);
                VM_PROPAGATE(s);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_SUPER: {
                uint16_t dest=0,base=0;uint8_t owner_index=0,name=0,argc=0;
                READ_SHORT(dest); READ_BYTE(owner_index); READ_BYTE(name);
                READ_SHORT(base); READ_BYTE(argc);
                if(argc>16 || (size_t)owner_index>=chunk->class_count ||
                   (size_t)name>=chunk->string_count ||
                   registers[0].kind!=DIAMOND_VALUE_OBJECT ||
                   registers[0].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondClass *owner=&chunk->classes[owner_index];
                if(owner->superclass==UINT8_MAX) VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                const DiamondStringConstant *method_name=&chunk->strings[name];
                const DiamondMethod *method=lookup_method(chunk,
                    &chunk->classes[owner->superclass],method_name->chars,
                    method_name->length);
                if(method==nullptr) {
                    /* No user-defined method anywhere up the superclass
                     * chain -- if this is super(...) from an overridden
                     * initialize() reaching for the built-in Exception
                     * constructor (message/cause field assignment,
                     * otherwise synthesized inline by NEW's own
                     * exception_class branch for a class that never
                     * overrides initialize), apply that same behavior
                     * here instead of treating the built-in as missing. */
                    bool reaches_exception=false;
                    const DiamondClass *ancestor=&chunk->classes[owner->superclass];
                    while(ancestor!=nullptr) {
                        if(ancestor==&chunk->classes[DIAMOND_CLASS_EXCEPTION]) {
                            reaches_exception=true;break;
                        }
                        ancestor=ancestor->superclass==UINT8_MAX?nullptr:
                            &chunk->classes[ancestor->superclass];
                    }
                    if(reaches_exception&&method_name->length==10&&
                       memcmp(method_name->chars,"initialize",10)==0) {
                        if(argc>2)VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                        DiamondInstance *self=
                            (DiamondInstance *)registers[0].as.object;
                        if(argc>0&&self->field_count>0)
                            self->fields[0]=registers[base];
                        if(argc>1&&self->field_count>1)
                            self->fields[1]=registers[(size_t)base+1];
                        registers[dest]=DIAMOND_NIL;
                        break;
                    }
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(argc<method->required_arity||argc>method->arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                DiamondValue args[17]; args[0]=registers[0];
                for(size_t i=0;i<argc;i++) args[i+1]=registers[(size_t)base+i];
                const DiamondFunction *fn=chunk->functions[method->function_index];
                DiamondChunk child={.name=fn->name,.code=fn->code,
                  .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
                  .constants=fn->constants,.constant_count=fn->constant_count,
                  .strings=fn->strings,.string_count=fn->string_count,
                  .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
                  .functions=chunk->functions,.function_count=chunk->function_count,
                  .classes=chunk->classes,.class_count=chunk->class_count,
                  .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
                  .parameter_type_sets=fn->parameter_type_sets,
                  .type_variable_count=fn->type_variable_count,
                  .parameter_offset=fn->owner_class==UINT8_MAX?0:1,
                  .register_count=fn->register_count};
                DiamondValue call_result=DIAMOND_NIL;
                DiamondVmStatus status=run_chunk(&child,vm,args,(size_t)argc+1,
                                                  depth+1,nullptr,&call_result);
                VM_PROPAGATE(status);
                registers[dest]=call_result;
                break;
            }
            case DIAMOND_OP_GET_IVAR: {
                const uint8_t *site=&chunk->code[instruction_offset];
                uint16_t dest=0,recv=0,field_operand=0;
                READ_SHORT(dest);READ_SHORT(recv);READ_SHORT(field_operand);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                if(field_operand>=instance->field_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const uint8_t field=(uint8_t)field_operand;
                const DiamondFieldCacheEntry *cached=lookup_field_cached(
                    vm,site,instance,field,false);
                registers[dest]=cached->materialized
                    ? instance->fields[field] : DIAMOND_NIL;break;
            }
            case DIAMOND_OP_SET_IVAR: {
                const uint8_t *site=&chunk->code[instruction_offset];
                uint16_t recv=0,field_operand=0,source=0;
                READ_SHORT(recv);READ_SHORT(field_operand);READ_SHORT(source);
                if(registers[recv].kind!=DIAMOND_VALUE_OBJECT||registers[recv].as.object->kind!=DIAMOND_OBJECT_INSTANCE)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondInstance *instance=(DiamondInstance *)registers[recv].as.object;
                if(field_operand>=instance->field_count)VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const uint8_t field=(uint8_t)field_operand;
                const DiamondFieldCacheEntry *cached=lookup_field_cached(
                    vm,site,instance,field,true);
                if(instance->shape!=cached->output_shape) {
                    instance->shape=cached->output_shape;
                    vm->shape_transitions++;
                }
                instance->fields[field]=registers[source];break;
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
                    if(instance->shape!=cached->output_shape) {
                        instance->shape=cached->output_shape;
                        vm->shape_transitions++;
                    }
                    instance->fields[field]=registers[source];
                } else registers[first]=cached->materialized?
                    instance->fields[field]:DIAMOND_NIL;
                break;
            }
            case DIAMOND_OP_GET_NAMESPACE_CONSTANT: {
                uint16_t destination=0,index=0;READ_SHORT(destination);READ_SHORT(index);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS||
                   !vm->namespace_constant_initialized[index])
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                registers[destination]=vm->namespace_constants[index];break;
            }
            case DIAMOND_OP_SET_NAMESPACE_CONSTANT: {
                uint16_t index=0,source=0;READ_SHORT(index);READ_SHORT(source);
                if(index>=DIAMOND_MAX_NAMESPACE_CONSTANTS||
                   vm->namespace_constant_initialized[index])
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
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
                                                     (uint8_t)set_index,true);
                if(!matches) {
                    char expected[80]; char actual[80];
                    format_type_set_index(expected,sizeof expected,chunk,(uint8_t)set_index);
                    format_value_type(actual,sizeof actual,registers[source]);
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
                if(registers[array_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[array_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY) {
                    char actual[80];
                    format_value_type(actual,sizeof actual,registers[array_reg]);
                    snprintf(vm->error,sizeof vm->error,"expected Array, got %s",actual);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondArray *array=(const DiamondArray *)registers[array_reg].as.object;
                if(array->count!=expected) {
                    snprintf(vm->error,sizeof vm->error,
                        "destructuring assignment expected %u element(s), got %zu",
                        (unsigned)expected,array->count);
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
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
            case DIAMOND_OP_PROCESS_RUN: {
                uint16_t destination=0,argv_register=0;
                READ_SHORT(destination);READ_SHORT(argv_register);
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
            case DIAMOND_OP_DEBUGGER: {
                uint16_t destination=0;uint8_t local_count=0;
                READ_SHORT(destination);READ_BYTE(local_count);
                uint8_t name_indices[DIAMOND_MAX_LOCALS];
                uint16_t local_registers[DIAMOND_MAX_LOCALS];
                for(size_t index=0;index<local_count;index++) {
                    READ_BYTE(name_indices[index]);
                    READ_SHORT(local_registers[index]);
                }
                const DiamondVmStatus debugger_status=debugger_helper(vm,chunk,depth,
                    instruction_offset,registers,name_indices,local_registers,local_count);
                VM_PROPAGATE(debugger_status);
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
                registers[destination]=DIAMOND_BOOL(
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
                if(registers[receiver].kind!=DIAMOND_VALUE_OBJECT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_HASH) {
                    DiamondHash *hash=(DiamondHash *)registers[receiver].as.object;
                    const ptrdiff_t found=hash_find(hash,registers[index_register]);
                    registers[destination]=found<0 ? DIAMOND_NIL
                        : hash->entries[(size_t)found].value;
                    break;
                }
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_STRING) {
                    if(registers[index_register].kind!=DIAMOND_VALUE_INT)
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    const DiamondString *source=
                        (const DiamondString *)registers[receiver].as.object;
                    const int64_t index=registers[index_register].as.integer;
                    if(index<0 || (uint64_t)index>=source->length) {
                        snprintf(vm->error,sizeof vm->error,
                                 "index %" PRId64 " out of bounds for String of length %zu",
                                 index,source->length);
                        VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                    }
                    DiamondString *character=
                        allocate_string(vm,source->chars+(size_t)index,1);
                    if(character==nullptr)VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[destination]=DIAMOND_OBJECT(character);
                    break;
                }
                if(registers[receiver].as.object->kind!=DIAMOND_OBJECT_ARRAY ||
                   registers[index_register].kind!=DIAMOND_VALUE_INT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondArray *array=(DiamondArray *)registers[receiver].as.object;
                const int64_t index=registers[index_register].as.integer;
                if(index<0 || (uint64_t)index>=array->count) {
                    snprintf(vm->error,sizeof vm->error,
                             "index %" PRId64 " out of bounds for Array of length %zu",
                             index,array->count);
                    VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                }
                registers[destination]=array->values[(size_t)index];
                break;
            }
            case DIAMOND_OP_INDEX_SET: {
                uint16_t receiver=0,index_register=0,source=0;
                READ_SHORT(receiver);READ_SHORT(index_register);READ_SHORT(source);
                if(registers[receiver].kind!=DIAMOND_VALUE_OBJECT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_HASH) {
                    DiamondHash *hash=(DiamondHash *)registers[receiver].as.object;
                    if(!hash_entry_satisfies_constraints(hash,
                       registers[index_register],registers[source])) {
                        snprintf(vm->error,sizeof vm->error,
                                 "hash entry violates its type annotation");
                        VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                    }
                    if(!hash_set(vm,hash,registers[index_register],registers[source]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    break;
                }
                if(registers[receiver].as.object->kind==DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                             "String does not support element assignment");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                if(registers[receiver].as.object->kind!=DIAMOND_OBJECT_ARRAY ||
                   registers[index_register].kind!=DIAMOND_VALUE_INT)
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                DiamondArray *array=(DiamondArray *)registers[receiver].as.object;
                const int64_t index=registers[index_register].as.integer;
                if(index<0 || (uint64_t)index>=array->count) {
                    snprintf(vm->error,sizeof vm->error,
                             "index %" PRId64 " out of bounds for Array of length %zu",
                             index,array->count);
                    VM_RETURN(DIAMOND_VM_INDEX_ERROR);
                }
                if(!array_value_satisfies_constraints(array,registers[source])) {
                    snprintf(vm->error,sizeof vm->error,
                             "array element violates its type annotation");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                array->values[(size_t)index]=registers[source];
                break;
            }
            case DIAMOND_OP_HASH: {
                uint16_t destination=0,base=0,count=0;
                READ_SHORT(destination);READ_SHORT(base);READ_SHORT(count);
                if((size_t)base+(size_t)count*2>DIAMOND_REGISTER_COUNT)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                DiamondHash *hash=allocate_hash(vm);
                if(hash==nullptr) VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                registers[destination]=DIAMOND_OBJECT(hash);
                for(size_t i=0;i<count;i++) {
                    if(!hash_set(vm,hash,registers[(size_t)base+i*2],
                                 registers[(size_t)base+i*2+1]))
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
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
                    handler_count--;
                if(handler_count>0) {
                    const UnwindHandler handler=handlers[--handler_count];
                    pending=(PendingUnwind){.kind=PENDING_RETURN,
                                            .value=registers[source]};
                    ip=handler.target;break;
                }
                *result = registers[source];
                VM_RETURN(DIAMOND_VM_OK);
            }
            case DIAMOND_OP_RAISE: {
                uint16_t source=0;READ_SHORT(source);
                vm->exception=registers[source];vm->has_exception=true;
                raise_capture_backtrace_helper(vm,chunk);
                if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                   registers,&ip))break;
                if(vm->exception.kind==DIAMOND_VALUE_INT)
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %" PRId64,
                             vm->exception.as.integer);
                else if(vm->exception.kind==DIAMOND_VALUE_BOOL)
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                             vm->exception.as.boolean?"true":"false");
                else if(vm->exception.kind==DIAMOND_VALUE_NIL)
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: nil");
                else if(vm->exception.kind==DIAMOND_VALUE_FLOAT) {
                    StringBuilder message_builder={};
                    if(builder_format_value(&message_builder,vm->exception))
                        snprintf(vm->error,sizeof vm->error,"uncaught exception: %.*s",
                                 (int)message_builder.length,message_builder.chars);
                    else
                        snprintf(vm->error,sizeof vm->error,"uncaught exception: <float>");
                    free(message_builder.chars);
                } else if(vm->exception.as.object->kind==DIAMOND_OBJECT_STRING) {
                    const DiamondString *string=(const DiamondString *)vm->exception.as.object;
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %.*s",
                             (int)string->length,string->chars);
                } else if(vm->exception.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
                    const DiamondInstance *instance=(const DiamondInstance *)vm->exception.as.object;
                    snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                             instance->class->name);
                } else snprintf(vm->error,sizeof vm->error,"uncaught exception: object");
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
                const UnwindHandler handler=handlers[--handler_count];
                pending=(PendingUnwind){.kind=PENDING_NORMAL,
                                        .continuation=continuation};
                ip=handler.target;break;
            }
            case DIAMOND_OP_END_ENSURE: {
                const PendingUnwind resume=pending;pending=(PendingUnwind){};
                if(resume.kind==PENDING_NORMAL) {ip=resume.continuation;break;}
                if(resume.kind==PENDING_RETURN) {
                    while(handler_count>0 &&
                          handlers[handler_count-1].kind!=HANDLER_ENSURE)
                        handler_count--;
                    if(handler_count>0) {
                        const UnwindHandler handler=handlers[--handler_count];
                        pending=resume;ip=handler.target;break;
                    }
                    *result=resume.value;VM_RETURN(DIAMOND_VM_OK);
                }
                if(resume.kind==PENDING_EXCEPTION) {
                    vm->exception=resume.value;vm->has_exception=true;
                    if(catch_exception(vm,chunk,handlers,&handler_count,&pending,
                                       registers,&ip))break;
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
                if((size_t)replacement->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *new_function=chunk->functions[replacement->function_index];
                if(new_function->owner_class!=class_operand) {
                    snprintf(vm->error,sizeof vm->error,
                             "redefine_method callable must be a method of '%s'",class->name);
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const uint8_t new_arity=(uint8_t)(new_function->arity-1);
                const uint8_t new_required_arity=(uint8_t)(new_function->required_arity-1);
                if(new_arity!=target->arity||new_required_arity!=target->required_arity)
                    VM_RETURN(DIAMOND_VM_ARITY_ERROR);
                target->function_index=replacement->function_index;
                diamond_vm_invalidate_method_caches(vm);
                registers[dest]=DIAMOND_NIL;break;
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
                uint16_t dest=0,path_reg=0;
                READ_SHORT(dest);READ_SHORT(path_reg);
                if(registers[path_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[path_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,
                        "SQLite3.open argument must be a String path");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *path=(const DiamondString *)registers[path_reg].as.object;
                sqlite3 *db=nullptr;
                const int open_rc=sqlite3_open(path->chars,&db);
                if(open_rc!=SQLITE_OK) {
                    snprintf(vm->error,sizeof vm->error,"cannot open '%.*s': %s",
                             (int)path->length,path->chars,sqlite3_errmsg(db));
                    sqlite3_close(db);
                    VM_RETURN(DIAMOND_VM_SQLITE3_ERROR);
                }
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
                if((size_t)callable->function_index>=chunk->function_count)
                    VM_RETURN(DIAMOND_VM_INVALID_BYTECODE);
                const DiamondFunction *target_fn=chunk->functions[callable->function_index];
                if(argc<target_fn->required_arity||argc>target_fn->arity)
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
                        "Thread.new argument does not support this type");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                new_thread->spawned=pthread_create(&new_thread->handle,nullptr,
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
            case DIAMOND_OP_TCP_CONNECT: {
                uint16_t dest=0,host_reg=0,port_reg=0;
                READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(port_reg);
                if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "TCPSocket.connect arguments must be a String host and an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
                int connected_fd=-1;
                const DiamondVmStatus connect_status=tcp_connect_helper(vm,host,
                    registers[port_reg].as.integer,&connected_fd);
                VM_PROPAGATE(connect_status);
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
            case DIAMOND_OP_TLS_CONNECT: {
                uint16_t dest=0,host_reg=0,port_reg=0;
                READ_SHORT(dest);READ_SHORT(host_reg);READ_SHORT(port_reg);
                if(registers[host_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[host_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[port_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,
                             "TLSSocket.connect arguments must be a String host and an Int port");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *host=(const DiamondString *)registers[host_reg].as.object;
                int connected_fd=-1;
                const DiamondVmStatus connect_status=tcp_connect_helper(vm,host,
                    registers[port_reg].as.integer,&connected_fd);
                VM_PROPAGATE(connect_status);
                SSL_CTX *context=SSL_CTX_new(TLS_client_method());
                if(context==nullptr) {
                    close(connected_fd);
                    char detail[256];tls_format_error(detail,sizeof detail);
                    snprintf(vm->error,sizeof vm->error,"cannot create TLS context: %s",detail);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                /* Secure by default, no opt-out exposed: every
                 * TLSSocket.connect verifies the peer's certificate
                 * against the system trust store (SSL_CTX_set_default_
                 * verify_paths -- honors $SSL_CERT_FILE/$SSL_CERT_DIR,
                 * which is how tests/run.sh points this at a hermetic
                 * test CA rather than the real system store) and that the
                 * certificate is actually for `host` (SSL_set1_host
                 * below). A custom/pinned trust store is a real,
                 * documented scope cut (see docs/io.md), not an
                 * oversight -- there was no call site in this codebase
                 * yet that needed one. */
                SSL_CTX_set_verify(context,SSL_VERIFY_PEER,nullptr);
                if(SSL_CTX_set_default_verify_paths(context)!=1) {
                    char detail[256];tls_format_error(detail,sizeof detail);
                    snprintf(vm->error,sizeof vm->error,
                        "cannot load system TLS trust store: %s",detail);
                    SSL_CTX_free(context);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                SSL *ssl=SSL_new(context);
                SSL_CTX_free(context); /* ssl already holds its own reference */
                if(ssl==nullptr) {
                    close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                SSL_set_fd(ssl,connected_fd);
                /* SNI (which certificate a multi-tenant server presents)
                 * and the hostname check SSL_get_verify_result below
                 * relies on both need a null-terminated hostname --
                 * host->chars always is (see allocate_string). */
                SSL_set_tlsext_host_name(ssl,host->chars);
                if(SSL_set1_host(ssl,host->chars)!=1) {
                    SSL_free(ssl);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                ERR_clear_error();
                if(SSL_connect(ssl)!=1) {
                    char detail[256];tls_format_error(detail,sizeof detail);
                    snprintf(vm->error,sizeof vm->error,
                        "cannot connect to '%.*s' over TLS: %s",
                        (int)host->length,host->chars,detail);
                    SSL_free(ssl);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                const long verify_result=SSL_get_verify_result(ssl);
                if(verify_result!=X509_V_OK) {
                    snprintf(vm->error,sizeof vm->error,
                        "TLS certificate verification failed for '%.*s': %s",
                        (int)host->length,host->chars,
                        X509_verify_cert_error_string(verify_result));
                    SSL_free(ssl);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_IO_ERROR);
                }
                DiamondTlsSocketHandle *handle=
                    allocate_tls_socket_handle(vm,ssl,connected_fd);
                if(handle==nullptr) {
                    SSL_free(ssl);close(connected_fd);
                    VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                }
                registers[dest]=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,
                    .as.object=(DiamondObject *)handle};
                break;
            }
            case DIAMOND_OP_TLS_LISTEN: {
                uint16_t dest=0,port_reg=0,cert_reg=0,key_reg=0;
                READ_SHORT(dest);READ_SHORT(port_reg);READ_SHORT(cert_reg);READ_SHORT(key_reg);
                if(registers[port_reg].kind!=DIAMOND_VALUE_INT||
                   registers[cert_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[cert_reg].as.object->kind!=DIAMOND_OBJECT_STRING||
                   registers[key_reg].kind!=DIAMOND_VALUE_OBJECT||
                   registers[key_reg].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    snprintf(vm->error,sizeof vm->error,"TLSServer.listen arguments must be "
                        "an Int port and String certificate/key file paths");
                    VM_RETURN(DIAMOND_VM_TYPE_ERROR);
                }
                const DiamondString *cert_path=
                    (const DiamondString *)registers[cert_reg].as.object;
                const DiamondString *key_path=
                    (const DiamondString *)registers[key_reg].as.object;
                DiamondListenerHandle *listener_handle=nullptr;
                const DiamondVmStatus listen_status=tls_listen_helper(vm,
                    registers[port_reg].as.integer,cert_path->chars,key_path->chars,
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
                   registers[writable_reg].as.object->kind!=DIAMOND_OBJECT_ARRAY||
                   registers[timeout_reg].kind!=DIAMOND_VALUE_INT) {
                    snprintf(vm->error,sizeof vm->error,"IO.poll arguments must be an "
                        "Array of readables, an Array of writables, and an Int timeout");
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
                struct pollfd fds[DIAMOND_MAX_POLL_FDS];
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
                const double real=registers[source].as.real;
                if(isnan(real)||isinf(real)) {
                    snprintf(vm->error,sizeof vm->error,
                             "to_i argument must be a finite Float");
                    VM_RETURN(DIAMOND_VM_INTEGER_OVERFLOW);
                }
                if(real>=9223372036854775808.0||real<-9223372036854775808.0) {
                    /* Outside int64_t range: promote instead of raising,
                     * matching every other overflow site now that Int
                     * auto-promotes to a bignum. */
                    const DiamondValue bignum_result=diamond_bignum_from_double(vm,real);
                    if(bignum_result.kind==DIAMOND_VALUE_NIL)
                        VM_RETURN(DIAMOND_VM_OUT_OF_MEMORY);
                    registers[dest]=bignum_result;
                    break;
                }
                registers[dest]=DIAMOND_INT((int64_t)real);
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

DiamondVmStatus diamond_vm_run(DiamondVm *vm, const DiamondChunk *chunk,
                               DiamondValue *result) {
    for (size_t index=0;index<vm->rewritten_site_count;index++)
        ((uint8_t *)(void *)vm->rewritten_sites[index])[0]=(uint8_t)DIAMOND_OP_INVOKE;
    vm->rewritten_site_count=0;
    vm->error[0]='\0';
    vm->has_exception=false;
    diamond_vm_invalidate_method_caches(vm);
    memset(vm->field_caches,0,sizeof(vm->field_caches));
    vm->direct_dispatch_rewrites=0;
    vm->field_cache_hits=0;
    vm->field_cache_misses=0;
    vm->shape_transitions=0;
    vm->quickened_sites=0;
    vm->deoptimized_sites=0;
    return run_chunk(chunk, vm, nullptr, 0, 0, nullptr, result);
}

const char *diamond_vm_error(const DiamondVm *vm) {
    return vm->error[0]=='\0' ? nullptr : vm->error;
}

const char *diamond_vm_status_name(DiamondVmStatus status) {
    switch (status) {
        case DIAMOND_VM_OK:
            return "ok";
        case DIAMOND_VM_INVALID_BYTECODE:
            return "invalid bytecode";
        case DIAMOND_VM_TYPE_ERROR:
            return "type error";
        case DIAMOND_VM_INTEGER_OVERFLOW:
            return "integer overflow";
        case DIAMOND_VM_DIVISION_BY_ZERO:
            return "division by zero";
        case DIAMOND_VM_ARITY_ERROR:
            return "wrong number of arguments";
        case DIAMOND_VM_STACK_OVERFLOW:
            return "call stack overflow";
        case DIAMOND_VM_OUT_OF_MEMORY:
            return "out of memory";
        case DIAMOND_VM_INDEX_ERROR:
            return "array index out of bounds";
        case DIAMOND_VM_EXCEPTION:
            return "uncaught exception";
        case DIAMOND_VM_YIELDED:
            return "yielded";
        case DIAMOND_VM_YIELD_WITHOUT_FIBER:
            return "yield outside a fiber";
        case DIAMOND_VM_FIBER_NOT_RESUMABLE:
            return "fiber is not resumable";
        case DIAMOND_VM_IO_ERROR:
            return "I/O error";
        case DIAMOND_VM_REGEXP_ERROR:
            return "regexp error";
        case DIAMOND_VM_WOULD_BLOCK:
            return "would block";
        case DIAMOND_VM_PROGRAM_ERROR:
            return "constructed program failed";
        case DIAMOND_VM_THREAD_ERROR:
            return "thread error";
        case DIAMOND_VM_SQLITE3_ERROR:
            return "sqlite3 error";
        case DIAMOND_VM_POSTGRES_ERROR:
            return "postgres error";
        case DIAMOND_VM_MYSQL_ERROR:
            return "mysql error";
    }
    return "unknown VM status";
}
