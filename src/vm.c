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
#include <arpa/inet.h>
#include "tls_crypto.h"
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <spawn.h>
#ifndef DIAMOND_NO_SQLITE
#include <sqlite3.h>
#endif
#ifndef DIAMOND_NO_POSTGRES
#include <libpq-fe.h>
#endif
#ifndef DIAMOND_NO_MYSQL
#include <mysql.h>
#endif
#ifndef DIAMOND_NO_ZLIB
#include <zlib.h>
#endif
#include <netinet/in.h>
#include <arpa/inet.h>
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
void diamond_resume_target_bounds(const DiamondFiber *fiber,
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

/* JIT trampolines for Phase 2c (docs/internal/jit-design.md) -- DiamondFrame
 * is private to this file, so jit.c can never construct or link one
 * directly; it only ever reserves opaque bytes on its own native stack and
 * calls these to manage them, the same "risky logic stays in real C, not
 * hand-rolled machine code" pattern already used for SET_IVAR/INDEX_GET/
 * CHECK_TYPE. diamond_jit_frame_size is called once per *compile* (not per
 * generated call) so jit.c learns the exact byte count to reserve without
 * a hardcoded constant that could silently drift out of sync with this
 * struct's own layout -- unlike DIAMOND_JIT_MAX_REGISTERS (a bare enum,
 * nothing to query), sizeof() on a real struct is exactly this available
 * here. The extra trailing size_t past the frame itself is instruction_
 * offset's own backing storage: that field is a *pointer* to a live value
 * for the frame's whole lifetime (see its own struct comment above), not
 * a copy, and a JIT'd function has no `ip`-like local of its own to point
 * at -- this reserves one, initialized to 0 (a synthetic placeholder;
 * this JIT compiles no begin/rescue and nothing it calls through these
 * trampolines can itself raise, so no code path today ever reads it back
 * for a real backtrace, but a future one might). */
size_t diamond_jit_frame_size(void) {
    return sizeof(DiamondFrame);
}

/* `offset_slot` is the prologue's pad slot, just above the frame: the frame's
 * instruction_offset points at it, and every trampoline call from the body
 * stores the current bytecode offset there (emit_body_call in jit.c), so a
 * backtrace and an error trace line both read the call being made. */
void diamond_jit_frame_push(void *frame_storage, DiamondVm *vm,
        DiamondValue *registers, size_t register_count, const DiamondChunk *chunk,
        size_t *offset_slot) {
    DiamondFrame *frame = (DiamondFrame *)frame_storage;
    *offset_slot = 0;
    *frame = (DiamondFrame){
        .previous = vm->frames,
        .registers = registers,
        .pending = nullptr,
        .register_count = register_count,
        .chunk = chunk,
        .instruction_offset = offset_slot,
    };
    vm->frames = frame;
}

void diamond_jit_tail_reset(DiamondValue *registers, size_t base, size_t argc,
        size_t live) {
    memmove(registers, registers + base, argc * sizeof *registers);
    memset(registers + argc, 0, (live - argc) * sizeof *registers);
}

void diamond_jit_frame_pop(DiamondVm *vm) {
    DiamondFrame *frame = (DiamondFrame *)vm->frames;
    vm->frames = frame->previous;
}

/* Appends "\n  at name:line:col" for the instruction at `offset` of `chunk`
 * to vm->error. Shared by run_chunk's RECORD_ERROR and the JIT's propagate
 * exits so interpreted and compiled traces cannot drift apart. `used` is the
 * current strlen(vm->error), already non-zero. */
DIAMOND_INTERNAL void diamond_record_error_line(DiamondVm *vm, const DiamondChunk *chunk,
        size_t used, size_t offset) {
    const char *name = chunk->name != nullptr ? chunk->name : "<chunk>";
    const bool in_bounds = offset < chunk->code_count;
    const uint32_t line = in_bounds && chunk->lines != nullptr ? chunk->lines[offset] : 0;
    const uint32_t column = in_bounds && chunk->columns != nullptr ? chunk->columns[offset] : 0;
    if (used < sizeof vm->error)
        (void)snprintf(vm->error + used, sizeof vm->error - used,
            "\n  at %s:%u:%u", name, line, column);
}

/* A compiled function propagating a failing `status`: records its trace line
 * as run_chunk's RECORD_ERROR does for an interpreted frame. */
static void jit_record_error_line(DiamondVm *vm, const DiamondChunk *chunk,
        size_t offset, uint8_t status) {
    if (status == DIAMOND_VM_OK || status == DIAMOND_VM_NONLOCAL_EXIT || chunk == nullptr)
        return;
    size_t used = strlen(vm->error);
    if (used == 0)
        used = (size_t)snprintf(vm->error, sizeof vm->error, "%s",
            diamond_vm_status_name((DiamondVmStatus)status));
    diamond_record_error_line(vm, chunk, used, offset);
}

/* Propagate exit of a function that pushed a frame: record, then pop. */
void diamond_jit_frame_pop_propagating(DiamondVm *vm, uint8_t status) {
    DiamondFrame *frame = (DiamondFrame *)vm->frames;
    if (frame->chunk != nullptr && frame->instruction_offset != nullptr)
        jit_record_error_line(vm, frame->chunk, *frame->instruction_offset, status);
    vm->frames = frame->previous;
}

/* Propagate exit of a function with no frame (nothing to pop). */
void diamond_jit_record_propagating(DiamondVm *vm, uint8_t status,
        const DiamondChunk *chunk, size_t offset) {
    jit_record_error_line(vm, chunk, offset, status);
}


void gc_unprotect(DiamondVm *vm, size_t saved_count);
static void free_adopted_programs(void *list);
static void populate_default_argv_env(DiamondVm *vm);



/* mysql_init() -- called per-connection from MySQL.open's dispatch --
 * implicitly runs mysql_library_init() the first time it's ever called
 * in the process if nothing already called it explicitly. That implicit
 * path is documented by the MySQL/MariaDB client library as unsafe under
 * concurrent first use: two threads racing their first MySQL.open at once
 * can corrupt the client library's own one-time setup. Diamond threads are
 * independent OS threads with isolated heaps (see docs/threads.md) that can
 * each reach MySQL.open before any other thread has, so an explicit,
 * pthread_once-guarded call here -- once per process, from every VM's own
 * init, the same "once per VM, idempotent" shape as the SIGPIPE handling
 * below -- removes the race instead of relying on the implicit path. */
static pthread_once_t mysql_library_init_once = PTHREAD_ONCE_INIT;

void diamond_vm_init(DiamondVm *vm) {
    *vm = (DiamondVm){.next_gc = 2048,.range_class_index=DIAMOND_NO_CLASS,
        .minor_gc_threshold_bytes = 1048576,.debug_fd=-1,
        .blocking_call_lock = PTHREAD_MUTEX_INITIALIZER};
    vm->quickening_threshold = 1;
    vm->monomorphic_threshold = 1;
    /* Much higher than quickening_threshold's 1: an opcode rewrite is a
     * single in-place byte write, while a JIT compile allocates and fills
     * a whole executable-memory buffer -- worth doing only for a function
     * that's actually going to be called a lot, not the first handful of
     * calls. Unmeasured beyond "clearly should not be 1"; see
     * docs/internal/jit-design.md. */
    vm->jit_threshold = 50;
    /* See DiamondVm.debug_fd's own comment (src/vm.h): read once here,
     * not per-pause. An unparseable or negative value is treated the
     * same as unset -- debugger_helper's ordinary stdout/stdin path --
     * rather than failing VM init over a malformed env var. */
    const char *debug_fd_env=getenv("DIAMOND_DEBUG_FD");
    if(debug_fd_env!=nullptr) {
        const long parsed=strtol(debug_fd_env,nullptr,10);
        if(parsed>=0&&parsed<=INT_MAX)vm->debug_fd=(int)parsed;
    }
    /* Initial breakpoint positions (see DiamondVm.debug_active_lines's own
     * comment above): the initial armed set, read directly here rather
     * than threaded in as a parameter, matching debug_fd's own
     * independent-per-VM env read just above. A live `setBreakpoints`
     * command (docs/debugging.md) freely replaces this later -- this is
     * only ever the *starting* point, and may legitimately be empty
     * (comma-separated, same convention as every other DIAMOND_*-env-var
     * list elsewhere in this codebase; malformed entries are skipped
     * rather than failing VM init, matching debug_fd's own tolerance). */
    const char *debug_breakpoints_env=getenv("DIAMOND_DEBUG_BREAKPOINT_OFFSETS");
    vm->debug_breakpoints_are_offsets=debug_breakpoints_env!=nullptr;
    if(debug_breakpoints_env==nullptr)
        debug_breakpoints_env=getenv("DIAMOND_DEBUG_BREAKPOINTS");
    if(debug_breakpoints_env!=nullptr) {
        const char *cursor=debug_breakpoints_env;
        while(*cursor!='\0'&&vm->debug_active_line_count<DIAMOND_MAX_ACTIVE_BREAKPOINTS) {
            while(*cursor==' '||*cursor==',')cursor++;
            if(*cursor=='\0')break;
            char *end=nullptr;
            const unsigned long long parsed=strtoull(cursor,&end,10);
            if(end==cursor)break;
            vm->debug_active_lines[vm->debug_active_line_count++]=(size_t)parsed;
            cursor=end;
        }
    }
    /* Resource limits (docs/sandbox.md's own "Resource limits" section) --
     * DiamondVm.max_instructions/max_wall_nanoseconds/max_memory_bytes's
     * own comment (src/vm.h) explains why this is read once per VM here
     * rather than a process-wide cache: every VM (this one, a spawned
     * Thread's child_vm, a Supervisor child's run_vm, ProgramBuilder#run's
     * own run_vm) independently reads the same real process environment
     * at its own init, matching DIAMOND_SANDBOX's own propagation-free
     * design. An unparseable, zero, or negative value is treated the same
     * as unset (no limit), matching debug_fd's own convention just above. */
    const char *max_instructions_env=getenv("DIAMOND_MAX_INSTRUCTIONS");
    if(max_instructions_env!=nullptr&&max_instructions_env[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(max_instructions_env,&end,10);
        if(end!=max_instructions_env&&*end=='\0'&&parsed>0&&parsed<=SIZE_MAX)
            vm->max_instructions=(size_t)parsed;
    }
    const char *max_wall_env=getenv("DIAMOND_MAX_WALL_MILLISECONDS");
    if(max_wall_env!=nullptr&&max_wall_env[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(max_wall_env,&end,10);
        if(end!=max_wall_env&&*end=='\0'&&parsed>0&&
           parsed<=(unsigned long long)INT64_MAX/1000000ULL)
            vm->max_wall_nanoseconds=(int64_t)parsed*1000000LL;
    }
    const char *max_memory_env=getenv("DIAMOND_MAX_MEMORY_BYTES");
    if(max_memory_env!=nullptr&&max_memory_env[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(max_memory_env,&end,10);
        if(end!=max_memory_env&&*end=='\0'&&parsed>0&&parsed<=SIZE_MAX)
            vm->max_memory_bytes=(size_t)parsed;
    }
    /* The grace allowance after a budget trips; same parsing rules, and
     * unset or invalid keeps the built-in default (docs/sandbox.md). */
    const char *grace_instructions_env=getenv("DIAMOND_GRACE_INSTRUCTIONS");
    if(grace_instructions_env!=nullptr&&grace_instructions_env[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(grace_instructions_env,&end,10);
        if(end!=grace_instructions_env&&*end=='\0'&&parsed>0&&parsed<=SIZE_MAX)
            vm->grace_instructions=(size_t)parsed;
    }
    const char *grace_wall_env=getenv("DIAMOND_GRACE_WALL_MILLISECONDS");
    if(grace_wall_env!=nullptr&&grace_wall_env[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(grace_wall_env,&end,10);
        if(end!=grace_wall_env&&*end=='\0'&&parsed>0&&
           parsed<=(unsigned long long)INT64_MAX/1000000ULL)
            vm->grace_wall_nanoseconds=(int64_t)parsed*1000000LL;
    }
    const char *grace_memory_env=getenv("DIAMOND_GRACE_MEMORY_BYTES");
    if(grace_memory_env!=nullptr&&grace_memory_env[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(grace_memory_env,&end,10);
        if(end!=grace_memory_env&&*end=='\0'&&parsed>0&&parsed<=SIZE_MAX)
            vm->grace_memory_bytes=(size_t)parsed;
    }
    vm->resource_limits_active=vm->max_instructions!=0||vm->max_wall_nanoseconds!=0||
        vm->max_memory_bytes!=0;
    if(vm->resource_limits_active)budget_register(vm);
    pthread_once(&mysql_library_init_once,mysql_library_init_once_fn);
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

void diamond_vm_configure_jit_from_env(DiamondVm *vm) {
    /* The native backend has only been validated on glibc x86-64. */
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__GLIBC__)
    vm->jit = getenv("DIAMOND_JIT") != nullptr;
#else
    vm->jit = false;
#endif
    const char *threshold = getenv("DIAMOND_JIT_THRESHOLD");
    if (threshold != nullptr && threshold[0] != '\0') {
        char *end = nullptr;
        const unsigned long long parsed = strtoull(threshold, &end, 10);
        if (end != threshold && *end == '\0' && parsed > 0 &&
            parsed <= SIZE_MAX) {
            vm->jit_threshold = (size_t)parsed;
        }
    }
}

void diamond_vm_bind_fiber_queue(DiamondVm *vm, const DiamondFiberQueue *queue) {
    if(vm==nullptr)return;
    vm->root_queue=queue;
}

static void free_object_list(const DiamondVm *vm,DiamondObject *object) {
    while (object != nullptr) {
        DiamondObject *next = object->next;
        if(object->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)object;
            for(size_t index=0;index<hash->constraint_count;index++)
                free(hash->constraints[index].type_variable_bindings);
            free(hash->entries);free(hash->buckets);free(hash->dirty_cards);
        } else if(object->kind==DIAMOND_OBJECT_ARRAY) {
            DiamondArray *array=(DiamondArray *)object;
            for(size_t index=0;index<array->constraint_count;index++)
                free(array->constraints[index].type_variable_bindings);
            free(array->values);free(array->dirty_cards);
        } else if(object->kind==DIAMOND_OBJECT_FIBER) {
            diamond_fiber_free(((DiamondFiberHandle *)object)->fiber);
        } else if(object->kind==DIAMOND_OBJECT_FILE) {
            FILE *stream=((DiamondFileHandle *)object)->stream;
            if(stream!=nullptr)fclose(stream);
        } else if(object->kind==DIAMOND_OBJECT_LISTENER) {
            DiamondListenerHandle *listener=(DiamondListenerHandle *)object;
            if(listener->fd>=0)close(listener->fd);
            if(listener->tls_context!=nullptr)SSL_CTX_free(listener->tls_context);
            free(listener->alpn_protocols);
        } else if(object->kind==DIAMOND_OBJECT_SOCKET) {
            const int fd=((DiamondSocketHandle *)object)->fd;
            if(fd>=0)close(fd);
        } else if(object->kind==DIAMOND_OBJECT_UDP_SOCKET) {
            const int fd=((DiamondUdpSocketHandle *)object)->fd;
            if(fd>=0)close(fd);
        } else if(object->kind==DIAMOND_OBJECT_TLS_SOCKET) {
            DiamondTlsSocketHandle *tls_handle=(DiamondTlsSocketHandle *)object;
            if(tls_handle->ssl!=nullptr) {
                if(!tls_handle->handshake_pending)SSL_shutdown(tls_handle->ssl);
                SSL_free(tls_handle->ssl);
            }
            if(tls_handle->fd>=0)close(tls_handle->fd);
            if(tls_handle->received_session!=nullptr)
                SSL_SESSION_free(tls_handle->received_session);
        } else if(object->kind==DIAMOND_OBJECT_PROCESS_STREAM) {
            const int fd=((DiamondProcessStream *)object)->fd;
            if(fd>=0)close(fd);
        } else if(object->kind==DIAMOND_OBJECT_PROCESS_HANDLE) {
            /* Same non-blocking reap as the GC sweep: an exited child is collected, a running one is left. */
            DiamondProcessHandle *handle=(DiamondProcessHandle *)object;
            if(!handle->reaped) {
                pid_t reap_result=0;
                do {reap_result=waitpid(handle->pid,nullptr,WNOHANG);}
                while(reap_result<0&&errno==EINTR);
            }
        } else if(object->kind==DIAMOND_OBJECT_REGEXP) {
            reginold_regex_free(((DiamondRegexp *)object)->handle);
            free(((DiamondRegexp *)object)->source);
        } else if(object->kind==DIAMOND_OBJECT_PROGRAM_BUILDER) {
            DiamondProgramBuilder *builder=(DiamondProgramBuilder *)object;
            if(builder->source_bundle!=nullptr) {
                diamond_source_bundle_free(builder->source_bundle);
                free(builder->source_bundle);
            }
            diamond_program_free(builder->program);
            free(builder->program);
        } else if(object->kind==DIAMOND_OBJECT_THREAD) {
            /* An interrupted VM (a supervised attempt stopped from outside)
             * stops the threads it started instead of waiting for them. */
            if(vm_interrupt_requested(vm))
                thread_interrupt(((DiamondThreadHandle *)object)->thread);
            free_thread(((DiamondThreadHandle *)object)->thread);
        } else if(object->kind==DIAMOND_OBJECT_CHANNEL) {
            free_channel_reference(((DiamondChannelHandle *)object)->channel);
        } else if(object->kind==DIAMOND_OBJECT_SUPERVISOR) {
            free_supervisor_reference(
                ((DiamondSupervisorHandle *)object)->supervisor);
        } else if(object->kind==DIAMOND_OBJECT_SQLITE3) {
            sqlite3 *db=((DiamondSqlite3Handle *)object)->db;
            /* _v2 -- see the matching branch in diamond_vm_collect_impl
             * above for why. */
#ifndef DIAMOND_NO_SQLITE
            if(db!=nullptr)sqlite3_close_v2(db);
#else
            (void)db;
#endif
        } else if(object->kind==DIAMOND_OBJECT_SQLITE3_STATEMENT) {
            sqlite3_stmt *stmt=((DiamondSqlite3StatementHandle *)object)->stmt;
#ifndef DIAMOND_NO_SQLITE
            if(stmt!=nullptr)sqlite3_finalize(stmt);
#else
            (void)stmt;
#endif
        } else if(object->kind==DIAMOND_OBJECT_POSTGRES) {
            PGconn *conn=((DiamondPostgresHandle *)object)->conn;
#ifndef DIAMOND_NO_POSTGRES
            if(conn!=nullptr)PQfinish(conn);
#else
            (void)conn;
#endif
        } else if(object->kind==DIAMOND_OBJECT_MYSQL) {
#ifndef DIAMOND_NO_MYSQL
            MYSQL *conn=((DiamondMysqlHandle *)object)->conn;
            if(conn!=nullptr)mysql_close(conn);
#endif
        }
        free(object);
        object = next;
    }
}

void diamond_vm_free(DiamondVm *vm) {
    budget_unregister(vm);
    free_object_list(vm,vm->young_objects);
    free_object_list(vm,vm->old_objects);
    free_adopted_programs(vm->adopted_programs);
    free(vm->class_variables);
    free(vm->namespace_constants);
    free(vm->namespace_constant_initialized);
    free(vm->gc_protected);
    free(vm->rewritten_sites);
    free(vm->remembered_set);
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
          .class_type_variable=fn->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(fn->owner_class+1u),
          .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
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
DiamondFiber *diamond_fiber_new_for_closure(
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

DiamondString *allocate_string(DiamondVm *vm, const char *chars,
                                      size_t length) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondString *string = malloc(sizeof(DiamondString) + length + 1);
    if (string == nullptr) return nullptr;
    string->object = (DiamondObject){
        .next = vm->young_objects,
        .kind = DIAMOND_OBJECT_STRING,
    };
    string->length = length;
    /* `chars` can be nullptr here with length 0 -- an empty StringBuilder
     * (File#read at EOF, an empty gets() line, ...) never allocates its
     * `chars` buffer at all. memcpy's first two arguments are declared
     * never-null regardless of the length passed, so calling it with a
     * null source is undefined behavior even when it would copy zero
     * bytes (confirmed live under UBSan: "null pointer passed as
     * argument 2, which is declared to never be null"). */
    if (length > 0) memcpy(string->chars, chars, length);
    string->chars[length] = '\0';
    vm->young_objects = &string->object;
    vm->bytes_allocated += sizeof(DiamondString) + length + 1;
    return string;
}

/* JIT trampoline for DIAMOND_OP_STRING -- see jit.h's own comment. Mirrors
 * that opcode's own interpreter case exactly (src/vm.c's dispatch loop):
 * resolve the string constant, allocate via allocate_string (which calls
 * maybe_collect unconditionally -- the actual reason this whole trampoline
 * needs the caller to have already published a DiamondFrame, unlike
 * SET_IVAR/INDEX_GET/CHECK_TYPE). */
DiamondVmStatus diamond_jit_new_string(DiamondVm *vm, const DiamondChunk *chunk,
        uint16_t string_index, DiamondValue *out) {
    if (string_index >= chunk->string_count) return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondStringConstant *constant = &chunk->strings[string_index];
    DiamondString *string = allocate_string(vm, constant->chars, constant->length);
    if (string == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    *out = DIAMOND_OBJECT(string);
    return DIAMOND_VM_OK;
}

DiamondSymbol *allocate_symbol(DiamondVm *vm, const char *chars,
                                      size_t length) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondSymbol *symbol = malloc(sizeof(DiamondSymbol) + length + 1);
    if (symbol == nullptr) return nullptr;
    symbol->object = (DiamondObject){
        .next = vm->young_objects,
        .kind = DIAMOND_OBJECT_SYMBOL,
    };
    symbol->length = length;
    /* See allocate_string's own comment just above: a null `chars` with
     * length 0 is well-defined for every current caller (a String/Symbol
     * object's own `chars` is never null, even empty), but the same
     * memcpy-is-UB-on-null rule applies here too, so guard it identically
     * rather than relying on that staying true. */
    if (length > 0) memcpy(symbol->chars, chars, length);
    symbol->chars[length] = '\0';
    vm->young_objects = &symbol->object;
    vm->bytes_allocated += sizeof(DiamondSymbol) + length + 1;
    return symbol;
}

DiamondInstance *allocate_instance(DiamondVm *vm,const DiamondClass *class,
                                          const DiamondChunk *chunk) {
    if (!maybe_collect(vm)) return nullptr;
    const bool generic=class->type_parameter[0]!='\0';
    const size_t size=sizeof(DiamondInstance)+class->field_count*sizeof(DiamondValue)+
        DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)*sizeof(DiamondTypeBinding);
    DiamondInstance *instance=malloc(size); if(instance==nullptr)return nullptr;
    instance->object=(DiamondObject){.next=vm->young_objects,.kind=DIAMOND_OBJECT_INSTANCE};
    instance->class=class; instance->shape=&class->shapes[0]; instance->owner=chunk;
    instance->field_count=class->field_count;
    instance->type_binding=generic?
        (DiamondTypeBinding *)(instance->fields+instance->field_count):nullptr;
    if(generic)memset(instance->type_binding,0,DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)*sizeof(DiamondTypeBinding));
    if(class->fixed_type_arguments)
        memcpy(instance->type_binding,class->fixed_type_bindings,
            DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)*sizeof(DiamondTypeBinding));
    for(size_t i=0;i<instance->field_count;i++) instance->fields[i]=DIAMOND_NIL;
    vm->young_objects=&instance->object; vm->bytes_allocated+=size; return instance;
}

/* Same allocation as allocate_array below, minus populating `values` --
 * for a caller that already has (or is about to build) exactly `count`
 * DiamondValues and would otherwise have to stage them in a throwaway
 * buffer first just to hand them to allocate_array for a second copy
 * (add_fallback's Array+Array, below, used to do exactly that). Safe to
 * defer reading any *other* array's contents until after this returns,
 * the same way allocate_array's own copy loop already runs after its
 * maybe_collect: DiamondArray objects are never moved by GC, so a source
 * still reachable from a live register (this VM has no other kind of
 * temporary) stays exactly where it was regardless of what maybe_collect
 * did meanwhile. */
DiamondArray *allocate_array_uninitialized(DiamondVm *vm,size_t count) {
    if (!maybe_collect(vm)) return nullptr;
    const size_t capacity=count;
    const size_t size=sizeof(DiamondArray)+capacity*sizeof(DiamondValue);
    DiamondArray *array=malloc(sizeof(DiamondArray)); if(array==nullptr)return nullptr;
    array->values=capacity==0?nullptr:malloc(capacity*sizeof(DiamondValue));
    if(capacity>0&&array->values==nullptr){free(array);return nullptr;}
    array->object=(DiamondObject){.next=vm->young_objects,.kind=DIAMOND_OBJECT_ARRAY};
    array->count=count;array->capacity=capacity;array->constraint_count=0;
    array->dirty_cards=nullptr;array->dirty_card_capacity=0;
    vm->young_objects=&array->object;vm->bytes_allocated+=size;return array;
}

DiamondArray *allocate_array(DiamondVm *vm,const DiamondValue *values,
                                    size_t count) {
    DiamondArray *array=allocate_array_uninitialized(vm,count);
    if(array==nullptr)return nullptr;
    if(count>0)memcpy(array->values,values,count*sizeof *values);
    return array;
}

DiamondHash *allocate_hash(DiamondVm *vm) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondHash *hash=malloc(sizeof(DiamondHash)); if(hash==nullptr)return nullptr;
    *hash=(DiamondHash){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_HASH}};
    vm->young_objects=&hash->object;vm->bytes_allocated+=sizeof(DiamondHash);return hash;
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
         * incrementally" lesson as regexp_scan_helper/copy_value_into_vm, just not yet applied to this newer site. */
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

DiamondFiberHandle *allocate_fiber_handle(DiamondVm *vm,DiamondFiber *fiber) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondFiberHandle *handle=malloc(sizeof(DiamondFiberHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondFiberHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_FIBER},.fiber=fiber};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondFiberHandle);return handle;
}

DiamondThreadHandle *allocate_thread_handle(DiamondVm *vm,DiamondThread *thread) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondThreadHandle *handle=malloc(sizeof(DiamondThreadHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondThreadHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_THREAD},.thread=thread};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondThreadHandle);return handle;
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
void free_thread(DiamondThread *thread) {
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

DiamondChannelHandle *allocate_channel_handle(DiamondVm *vm,DiamondChannel *channel) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondChannelHandle *handle=malloc(sizeof(DiamondChannelHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondChannelHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_CHANNEL},
        .channel=channel};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondChannelHandle);
    return handle;
}

/* Mirrors free_thread's own role, called from the same two sweep paths
 * (diamond_vm_collect's cycle-based sweep, diamond_vm_free's whole-VM
 * teardown) for a DIAMOND_OBJECT_CHANNEL handle -- except a DiamondChannel
 * is refcounted (see its own comment) rather than owned one-to-one by a
 * single handle, so this only actually tears anything down once the
 * *last* referencing handle, across however many VM heaps ever held one,
 * is swept. Safe by construction at that point: reachability is exactly
 * what the GC just proved false for every one of those handles, so no
 * thread can be mid-call (send/receive/close/...) against a channel whose
 * last reference is about to disappear. `channel` may be nullptr (mirrors
 * free_thread's own tolerance) so callers don't need their own guard. */
void free_channel_reference(DiamondChannel *channel) {
    if(channel==nullptr)return;
    if(atomic_fetch_sub(&channel->refcount,1)!=1)return;
    pthread_mutex_destroy(&channel->lock);
    pthread_cond_destroy(&channel->not_empty);
    pthread_cond_destroy(&channel->not_full);
    if(channel->private_vm!=nullptr) {
        diamond_vm_free(channel->private_vm);
        free(channel->private_vm);
    }
    diamond_program_free(channel->private_program);
    free(channel->private_program);
    free(channel->queue);
    free(channel);
}

DiamondSupervisorHandle *allocate_supervisor_handle(DiamondVm *vm,
        DiamondSupervisor *supervisor) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondSupervisorHandle *handle=malloc(sizeof(DiamondSupervisorHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondSupervisorHandle){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_SUPERVISOR},
        .supervisor=supervisor};
    vm->young_objects=&handle->object;
    vm->bytes_allocated+=sizeof(DiamondSupervisorHandle);
    return handle;
}

/* Shared teardown for a DiamondSupervisor -- mirrors free_channel_
 * reference's own role/refcount discipline (only the last referencing
 * handle's sweep actually tears anything down), but additionally has to
 * *stop* every child first: sets stop_requested (same flag Supervisor#
 * stop sets) and blocks joining every child's OS thread (supervisor_
 * join_all_children above, exactly what stop() itself does) before
 * reclaiming any child's program_template/args_vm. A child mid-crash-
 * loop still finishes its *current* attempt before noticing the flag --
 * there is no cancellation anywhere in Diamond's concurrency model, same
 * as free_thread's own block-join reasoning for an abandoned Thread. */
void free_supervisor_reference(DiamondSupervisor *supervisor) {
    if(supervisor==nullptr)return;
    if(atomic_fetch_sub(&supervisor->refcount,1)!=1)return;
    atomic_store(&supervisor->stop_requested,true);
    supervisor_join_all_children(supervisor);
    for(size_t index=0;index<supervisor->child_count;index++) {
        DiamondSupervisorChild *child=&supervisor->children[index];
        if(child->args_vm!=nullptr) {
            diamond_vm_free(child->args_vm);
            free(child->args_vm);
        }
        diamond_program_free(child->program_template);
        free(child->program_template);
    }
    pthread_mutex_destroy(&supervisor->lock);
    free(supervisor);
}

DiamondFileHandle *allocate_file_handle(DiamondVm *vm,FILE *stream) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondFileHandle *handle=malloc(sizeof(DiamondFileHandle));if(handle==nullptr)return nullptr;
    *handle=(DiamondFileHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_FILE},.stream=stream};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondFileHandle);return handle;
}

DiamondListenerHandle *allocate_listener_handle(DiamondVm *vm,int fd,
        bool nonblocking) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondListenerHandle *handle=malloc(sizeof(DiamondListenerHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondListenerHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_LISTENER},
        .fd=fd,.nonblocking=nonblocking};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondListenerHandle);return handle;
}

DiamondSocketHandle *allocate_socket_handle(DiamondVm *vm,int fd) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondSocketHandle *handle=malloc(sizeof(DiamondSocketHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondSocketHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_SOCKET},.fd=fd};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondSocketHandle);return handle;
}

DiamondUdpSocketHandle *allocate_udp_socket_handle(DiamondVm *vm,int fd) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondUdpSocketHandle *handle=malloc(sizeof(DiamondUdpSocketHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondUdpSocketHandle){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_UDP_SOCKET},.fd=fd};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondUdpSocketHandle);return handle;
}

DiamondTlsSocketHandle *allocate_tls_socket_handle(DiamondVm *vm,SSL *ssl,int fd) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondTlsSocketHandle *handle=malloc(sizeof(DiamondTlsSocketHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondTlsSocketHandle){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_TLS_SOCKET},.ssl=ssl,.fd=fd};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondTlsSocketHandle);return handle;
}


DiamondSqlite3Handle *allocate_sqlite3_handle(DiamondVm *vm,sqlite3 *db) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondSqlite3Handle *handle=malloc(sizeof(DiamondSqlite3Handle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondSqlite3Handle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_SQLITE3},
        .db=db};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondSqlite3Handle);return handle;
}

DiamondSqlite3StatementHandle *allocate_sqlite3_statement_handle(
        DiamondVm *vm,sqlite3_stmt *stmt) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondSqlite3StatementHandle *handle=malloc(sizeof(DiamondSqlite3StatementHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondSqlite3StatementHandle){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_SQLITE3_STATEMENT},.stmt=stmt};
    vm->young_objects=&handle->object;
    vm->bytes_allocated+=sizeof(DiamondSqlite3StatementHandle);
    return handle;
}

DiamondPostgresHandle *allocate_postgres_handle(DiamondVm *vm,PGconn *conn) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondPostgresHandle *handle=malloc(sizeof(DiamondPostgresHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondPostgresHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_POSTGRES},
        .conn=conn};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondPostgresHandle);return handle;
}

DiamondMysqlHandle *allocate_mysql_handle(DiamondVm *vm,MYSQL *conn) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondMysqlHandle *handle=malloc(sizeof(DiamondMysqlHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondMysqlHandle){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_MYSQL},
        .conn=conn};
    vm->young_objects=&handle->object;vm->bytes_allocated+=sizeof(DiamondMysqlHandle);return handle;
}

DiamondTime *allocate_time(DiamondVm *vm,double epoch,uint8_t zone_mode,
        int32_t utc_offset) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondTime *time=malloc(sizeof(DiamondTime));
    if(time==nullptr)return nullptr;
    *time=(DiamondTime){.object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_TIME},
        .epoch=epoch,.utc_offset=utc_offset,.zone_mode=zone_mode};
    vm->young_objects=&time->object;vm->bytes_allocated+=sizeof(DiamondTime);return time;
}

/* Allocated (and rooted into the caller's dest register) before
 * process_run_helper does any of its own work, with stdout_value/
 * stderr_value still nil -- the same "root the container before filling
 * it in" ordering String#split's pieces array uses, since spawning and
 * draining a child process means several further allocations (the two
 * captured-output Strings) that can each trigger a GC. */
DiamondProcessResult *allocate_process_result(DiamondVm *vm) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondProcessResult *result=malloc(sizeof(DiamondProcessResult));
    if(result==nullptr)return nullptr;
    *result=(DiamondProcessResult){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_PROCESS_RESULT},
        .stdout_value=DIAMOND_NIL,.stderr_value=DIAMOND_NIL,.exit_code=0};
    vm->young_objects=&result->object;
    vm->bytes_allocated+=sizeof(DiamondProcessResult);
    return result;
}

DiamondProcessStream *allocate_process_stream(DiamondVm *vm,int fd) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondProcessStream *stream=malloc(sizeof(DiamondProcessStream));
    if(stream==nullptr)return nullptr;
    *stream=(DiamondProcessStream){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_PROCESS_STREAM},.fd=fd};
    vm->young_objects=&stream->object;
    vm->bytes_allocated+=sizeof(DiamondProcessStream);
    return stream;
}

/* Rooted into the caller's dest register (see DIAMOND_OP_PROCESS_SPAWN)
 * before either stream is allocated -- same "root the container first"
 * ordering allocate_process_result's own comment explains, since
 * allocating each DiamondProcessStream can itself trigger a GC. Both
 * stream fields start nil; process_spawn_helper fills them in with
 * explicit gc_write_barrier calls once each stream is actually
 * allocated, for the same reason process_run_helper's stdout_value/
 * stderr_value writes need them. */
DiamondProcessHandle *allocate_process_handle(DiamondVm *vm,pid_t pid) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondProcessHandle *handle=malloc(sizeof(DiamondProcessHandle));
    if(handle==nullptr)return nullptr;
    *handle=(DiamondProcessHandle){
        .object={.next=vm->young_objects,.kind=DIAMOND_OBJECT_PROCESS_HANDLE},
        .pid=pid,.stdout_stream=DIAMOND_NIL,.stderr_stream=DIAMOND_NIL,
        .stdin_stream=DIAMOND_NIL,
        .reaped=false,.exit_code=0};
    vm->young_objects=&handle->object;
    vm->bytes_allocated+=sizeof(DiamondProcessHandle);
    return handle;
}

/* Transfers ownership of `program` (previously a ProgramBuilder's own
 * `program`, about to otherwise be freed alongside it) to `vm`, and
 * returns the persistent node a copied instance's `owner` field (via
 * &result->chunk) or a compile_method closure's `bound_values` field
 * (via result->bound_values) can safely reference forever. Returns
 * nullptr (leaving `program` unadopted, caller still responsible for it)
 * only on allocation failure. `bound_values`/`bound_value_count` may be
 * nullptr/0 (ProgramBuilder's own adoption never has any). */
DiamondAdoptedProgram *adopt_program(DiamondVm *vm,DiamondProgram *program,
        const DiamondValue *bound_values,uint8_t bound_value_count) {
    DiamondAdoptedProgram *node=malloc(sizeof *node);
    if(node==nullptr)return nullptr;
    node->program=program;
    node->chunk=diamond_program_chunk(program);
    node->bound_value_count=bound_value_count;
    for(uint8_t index=0;index<bound_value_count;index++)
        node->bound_values[index]=bound_values[index];
    node->next=(DiamondAdoptedProgram *)vm->adopted_programs;
    vm->adopted_programs=node;
    return node;
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

void mark_adopted_programs(void *list, bool minor) {
    const DiamondAdoptedProgram *node=(const DiamondAdoptedProgram *)list;
    while(node!=nullptr) {
        for(uint8_t index=0;index<node->bound_value_count;index++)
            mark_value(node->bound_values[index],minor);
        node=node->next;
    }
}


bool byte_buffer_append(ByteBuffer *buffer,const char *bytes,size_t count) {
    if(count==0)return true;
    size_t needed;
    if(ckd_add(&needed,buffer->length,count))return false;
    if(needed>buffer->capacity) {
        size_t capacity=buffer->capacity==0?256:buffer->capacity;
        while(capacity<needed)
            if(ckd_mul(&capacity,capacity,2))return false;
        char *grown=realloc(buffer->data,capacity);
        if(grown==nullptr)return false;
        buffer->data=grown;buffer->capacity=capacity;
    }
    memcpy(buffer->data+buffer->length,bytes,count);
    buffer->length+=count;
    return true;
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
bool gc_protect(DiamondVm *vm, DiamondValue value) {
    if(vm->gc_protected_count>=vm->gc_protected_capacity) {
        size_t capacity=8,bytes;
        if((vm->gc_protected_capacity!=0&&ckd_mul(&capacity,vm->gc_protected_capacity,2))||
            ckd_mul(&bytes,capacity,sizeof *vm->gc_protected))return false;
        DiamondValue *grown=
            realloc(vm->gc_protected,bytes);
        if(grown==nullptr)return false;
        vm->gc_protected=grown;vm->gc_protected_capacity=capacity;
    }
    vm->gc_protected[vm->gc_protected_count++]=value;
    return true;
}


/* Shared ARRAY implementation. allocate_array can collect before copying
 * elements, so its source registers must already be published as GC roots. */
DiamondVmStatus diamond_jit_new_array(DiamondVm *vm, DiamondValue *registers,
        uint16_t base, uint16_t count, DiamondValue *out) {
    if ((size_t)base + count > DIAMOND_REGISTER_COUNT)
        return DIAMOND_VM_INVALID_BYTECODE;
    DiamondArray *array = allocate_array(vm, &registers[base], count);
    if (array == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    *out = DIAMOND_OBJECT(array);
    return DIAMOND_VM_OK;
}

/* JIT trampoline for DIAMOND_OP_HASH -- extracted from that case's own
 * body (below) so the interpreter and the JIT share one implementation
 * rather than risking the two drifting apart. Allocates via
 * allocate_hash, which (like allocate_string) calls maybe_collect
 * unconditionally -- a real GC safepoint, so any JIT'd function compiling
 * this opcode must already have published a DiamondFrame. `count` is a
 * compile-time-known immediate (baked into the bytecode by the compiler,
 * exactly like DIAMOND_OP_STRING's own string_index), so the whole
 * key/value-pair loop lives here in C rather than needing to be unrolled
 * into generated machine code. */
DiamondVmStatus diamond_jit_new_hash(DiamondVm *vm, DiamondValue *registers,
        uint16_t base, uint16_t count, DiamondValue *out) {
    if ((size_t)base + (size_t)count * 2 > DIAMOND_REGISTER_COUNT)
        return DIAMOND_VM_INVALID_BYTECODE;
    DiamondHash *hash = allocate_hash(vm);
    if (hash == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    *out = DIAMOND_OBJECT(hash);
    for (size_t i = 0; i < count; i++) {
        if (!hash_set(vm, hash, registers[(size_t)base + i * 2],
                      registers[(size_t)base + i * 2 + 1]))
            return DIAMOND_VM_OUT_OF_MEMORY;
    }
    return DIAMOND_VM_OK;
}

const DiamondMethod *lookup_method(const DiamondChunk *chunk,
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
        current = current->superclass == DIAMOND_NO_CLASS
            ? nullptr : chunk->classes[current->superclass];
    }
    return nullptr;
}

const DiamondMethod *lookup_method_cached(
    DiamondVm *vm, const DiamondChunk *chunk, const uint8_t *site,
    const DiamondClass *class, const char *name, size_t length,
    const DiamondMethodCache **cache_out) {
    const size_t slot=diamond_cache_slot(site,vm->method_caches,
        sizeof vm->method_caches[0],&vm->cache_victim_cursor);
    DiamondMethodCache *cache=&vm->method_caches[slot];
    if(cache_out!=nullptr)*cache_out=cache;
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


#ifndef DIAMOND_NO_ZLIB
/* Gzip.compress(data) -- gzip-wrapped deflate via zlib (already linked
 * for the outbound HTTP client's Accept-Encoding support), growing an
 * output buffer geometrically since a compressed size can't be predicted
 * exactly up front. `windowBits=15+16` requests a gzip (not raw
 * zlib-wrapped) header/trailer, matching what every real HTTP server
 * expects behind Content-Encoding: gzip. */
DiamondVmStatus gzip_compress_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,data_value);
        snprintf(vm->error,sizeof vm->error,
            "Gzip.compress data must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *data=(const DiamondString *)data_value.as.object;
    z_stream stream={0};
    if(deflateInit2(&stream,Z_DEFAULT_COMPRESSION,Z_DEFLATED,15+16,8,
            Z_DEFAULT_STRATEGY)!=Z_OK) {
        snprintf(vm->error,sizeof vm->error,"cannot initialize gzip compression");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    stream.next_in=(Bytef *)data->chars;
    stream.avail_in=(uInt)(data->length>UINT_MAX?UINT_MAX:data->length);
    size_t capacity=data->length/2+64;
    unsigned char *output=malloc(capacity);
    if(output==nullptr) {deflateEnd(&stream);return DIAMOND_VM_OUT_OF_MEMORY;}
    size_t produced=0;
    int deflate_status=Z_OK;
    while(deflate_status!=Z_STREAM_END) {
        if(produced==capacity) {
            const size_t grown=capacity*2;
            unsigned char *bigger=realloc(output,grown);
            if(bigger==nullptr) {free(output);deflateEnd(&stream);return DIAMOND_VM_OUT_OF_MEMORY;}
            output=bigger;capacity=grown;
        }
        stream.next_out=output+produced;
        stream.avail_out=(uInt)(capacity-produced>UINT_MAX?UINT_MAX:capacity-produced);
        const uInt before=stream.avail_out;
        deflate_status=deflate(&stream,Z_FINISH);
        produced+=before-stream.avail_out;
        if(deflate_status==Z_STREAM_ERROR) {
            free(output);deflateEnd(&stream);
            snprintf(vm->error,sizeof vm->error,"gzip compression failed");
            return DIAMOND_VM_PROGRAM_ERROR;
        }
    }
    deflateEnd(&stream);
    DiamondString *result=allocate_string(vm,(const char *)output,produced);
    free(output);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* Gzip.decompress(data, max_size) -- the inverse, accepting either a
 * gzip- or zlib-wrapped stream (`windowBits=15+32` asks zlib to detect
 * either header automatically -- covers both real-world spellings of
 * HTTP's Content-Encoding: gzip/deflate, since some servers send
 * "deflate" as a zlib-wrapped stream despite the RFC's ambiguity about
 * which). `max_size` is a required cap on the *decompressed* output,
 * checked incrementally as output grows rather than after the fact --
 * decompressing a small, attacker-controlled input into an unbounded
 * output (a "zip bomb") is a real risk for anything that automatically
 * decompresses a network response, so this never fully materializes an
 * over-cap buffer in the first place. The caller decides what "too big"
 * means (the outbound HTTP client passes its own http_max_body_size()) --
 * there's no single sensible default this native primitive could pick
 * on its own. */
DiamondVmStatus gzip_decompress_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue max_size_value,DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,data_value);
        snprintf(vm->error,sizeof vm->error,
            "Gzip.decompress data must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(max_size_value.kind!=DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,max_size_value);
        snprintf(vm->error,sizeof vm->error,
            "Gzip.decompress max_size must be an Int, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(max_size_value.as.integer<0) {
        snprintf(vm->error,sizeof vm->error,"Gzip.decompress max_size must not be negative");
        return DIAMOND_VM_ARITY_ERROR;
    }
    const size_t max_size=(size_t)max_size_value.as.integer;
    const DiamondString *data=(const DiamondString *)data_value.as.object;
    z_stream stream={0};
    if(inflateInit2(&stream,15+32)!=Z_OK) {
        snprintf(vm->error,sizeof vm->error,"cannot initialize gzip decompression");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    stream.next_in=(Bytef *)data->chars;
    stream.avail_in=(uInt)(data->length>UINT_MAX?UINT_MAX:data->length);
    size_t capacity=data->length*3+64;
    if(capacity>max_size&&max_size>0)capacity=max_size;
    unsigned char *output=malloc(capacity>0?capacity:1);
    if(output==nullptr) {inflateEnd(&stream);return DIAMOND_VM_OUT_OF_MEMORY;}
    size_t produced=0;
    int inflate_status=Z_OK;
    while(inflate_status!=Z_STREAM_END) {
        if(produced==capacity) {
            if(produced>=max_size) {
                free(output);inflateEnd(&stream);
                snprintf(vm->error,sizeof vm->error,
                    "decompressed data exceeds maximum size of %zu bytes",max_size);
                return DIAMOND_VM_IO_ERROR;
            }
            size_t grown=capacity==0?64:capacity*2;
            if(grown>max_size)grown=max_size;
            unsigned char *bigger=realloc(output,grown);
            if(bigger==nullptr) {free(output);inflateEnd(&stream);return DIAMOND_VM_OUT_OF_MEMORY;}
            output=bigger;capacity=grown;
        }
        stream.next_out=output+produced;
        stream.avail_out=(uInt)(capacity-produced>UINT_MAX?UINT_MAX:capacity-produced);
        const uInt before=stream.avail_out;
        inflate_status=inflate(&stream,Z_NO_FLUSH);
        produced+=before-stream.avail_out;
        if(inflate_status==Z_NEED_DICT||inflate_status==Z_DATA_ERROR||
           inflate_status==Z_MEM_ERROR||inflate_status==Z_STREAM_ERROR) {
            char detail[256];
            (void)snprintf(detail,sizeof detail,"%s",
                stream.msg!=nullptr?stream.msg:"corrupt or truncated gzip stream");
            free(output);inflateEnd(&stream);
            snprintf(vm->error,sizeof vm->error,"gzip decompression failed: %s",detail);
            return DIAMOND_VM_IO_ERROR;
        }
        if(inflate_status!=Z_STREAM_END&&stream.avail_in==0&&stream.avail_out!=0) {
            /* No more input, decoder didn't ask for more output room, and
             * it isn't reporting the stream as finished -- a truncated
             * stream, not a bug in the grow loop above. */
            free(output);inflateEnd(&stream);
            snprintf(vm->error,sizeof vm->error,
                "gzip decompression failed: truncated stream");
            return DIAMOND_VM_IO_ERROR;
        }
    }
    inflateEnd(&stream);
    DiamondString *result=allocate_string(vm,(const char *)output,produced);
    free(output);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}
#else
DiamondVmStatus gzip_compress_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out) {
    (void)data_value;(void)out;
    snprintf(vm->error,sizeof vm->error,"Gzip is not compiled into this build");
    return DIAMOND_VM_TYPE_ERROR;
}

DiamondVmStatus gzip_decompress_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue max_size_value,DiamondValue *out) {
    (void)data_value;(void)max_size_value;(void)out;
    snprintf(vm->error,sizeof vm->error,"Gzip is not compiled into this build");
    return DIAMOND_VM_TYPE_ERROR;
}
#endif

/* Resolves DIAMOND_CLASS_FROM_SELF to the class in register 0 (`self` in a
 * class singleton method), leaving a literal class operand unchanged.
 * False, with vm->error set, when self isn't a class. */
bool resolve_class_operand(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,const char *operation,uint16_t *class_operand) {
    if(*class_operand!=DIAMOND_CLASS_FROM_SELF)return true;
    if(chunk->parameter_offset!=1||registers[0].kind!=DIAMOND_VALUE_CLASS) {
        snprintf(vm->error,sizeof vm->error,
            "self.%s works only in a class's own def self. method",operation);
        return false;
    }
    *class_operand=registers[0].as.class_index;
    return true;
}

/* exit(code = 0) -- validation only returns; a valid code calls libc
 * exit() directly and never returns at all. This is a hard, immediate,
 * whole-process exit (like Ruby's Kernel#exit!, not Kernel#exit): no
 * `ensure` block anywhere on the call stack runs, and every other
 * Thread.new-spawned OS thread stops too, since they all share this one
 * process (docs/threads.md). Deliberately not the softer,
 * exception-based/`ensure`-respecting semantic real Ruby `exit` has --
 * that would need a new rescuable-but-uncatchable-by-default exception
 * class and real unwind-and-check-ensure plumbing at every call site on
 * the stack, out of scope for this first version. stdout/stderr are
 * flushed first since libc exit() only flushes streams it owns, and a
 * DiamondVm's own stdout writes go through ordinary buffered fputs/puts
 * (see DIAMOND_OP_PRINT) that could otherwise still be sitting
 * unflushed in the C library's buffer. */
DiamondVmStatus exit_helper(DiamondVm *vm,DiamondValue code_value) {
    if(code_value.kind!=DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,code_value);
        snprintf(vm->error,sizeof vm->error,
            "exit code must be an Int, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t code=code_value.as.integer;
    if(code<0||code>255) {
        (void)snprintf(vm->error,sizeof vm->error,
            "exit code must be between 0 and 255");
        return DIAMOND_VM_ARITY_ERROR;
    }
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
}

/* Mirrors find_function's two filters (compiler.c) exactly, operating on
 * the runtime DiamondChunk instead of the compile-time DiamondProgram:
 * excludes class/module methods (owner_class!=DIAMOND_OWNER_NONE) and nested
 * def's, so a same-named local closure can never shadow a real
 * top-level prelude function. */
const DiamondFunction *find_top_level_function(
        const DiamondChunk *chunk, const char *name, size_t length) {
    /* A function name is NUL-terminated inside its DIAMOND_MAX_FUNCTION_NAME
     * array, so nothing that long can match. */
    if (length >= DIAMOND_MAX_FUNCTION_NAME) return nullptr;
    for (size_t index = chunk->function_count; index > 0; index--) {
        const DiamondFunction *candidate = chunk->functions[index - 1];
        if (candidate->owner_class != DIAMOND_OWNER_NONE || candidate->nested) continue;
        /* The extension protocol probes this table two or three times per
         * call on a native collection, and the table holds every prelude
         * function: reject on the first and last character (both inside the
         * zero-filled name array) before paying for strlen on each one. A
         * real match always agrees on both, so this never skips one. */
        if (length > 0 && (candidate->name[0] != name[0] ||
                           candidate->name[length - 1] != name[length - 1])) continue;
        if (strlen(candidate->name) == length &&
            memcmp(candidate->name, name, length) == 0) return candidate;
    }
    return nullptr;
}

/* Source-level extension protocol for native collections. `items.foo(x)`
 * probes array_foo(items,x), then enumerable_foo(items,x); Hash uses the
 * corresponding hash_ prefix. A predicate `items.foo?(x)` probes the exact
 * name first (array_foo?), then falls back to the bridge name without the
 * '?' (array_foo). Native methods handled directly by the VM still win first. */
static const DiamondFunction *find_collection_extension_exact(
        const DiamondChunk *chunk,DiamondObjectKind kind,
        const char *name,size_t length) {
    char bridge[DIAMOND_MAX_FUNCTION_NAME];
    const char *prefix=kind==DIAMOND_OBJECT_ARRAY?"array_":"hash_";
    int written=snprintf(bridge,sizeof bridge,"%s%.*s",prefix,(int)length,name);
    if(written>0&&(size_t)written<sizeof bridge) {
        const DiamondFunction *specific=find_top_level_function(
            chunk,bridge,(size_t)written);
        if(specific!=nullptr)return specific;
    }
    /* The prelude's own Ruby-compatible methods live under diamond_, so
     * they never collide with a program's own array_x/hash_x functions --
     * which, being probed first, can still extend or replace them. */
    written=snprintf(bridge,sizeof bridge,"diamond_%s%.*s",prefix,(int)length,name);
    if(written>0&&(size_t)written<sizeof bridge) {
        const DiamondFunction *builtin=find_top_level_function(
            chunk,bridge,(size_t)written);
        if(builtin!=nullptr)return builtin;
    }
    if(kind==DIAMOND_OBJECT_HASH) {
        const bool shared=
            (length==4&&memcmp(name,"lazy",4)==0)||
            (length==6&&memcmp(name,"select",6)==0)||
            (length==5&&memcmp(name,"count",5)==0)||
            (length==3&&memcmp(name,"any",3)==0)||
            (length==3&&memcmp(name,"all",3)==0)||
            (length==6&&memcmp(name,"reduce",6)==0)||
            (length==3&&memcmp(name,"map",3)==0);
        if(!shared)return nullptr;
    }
    written=snprintf(bridge,sizeof bridge,"enumerable_%.*s",(int)length,name);
    if(written<=0||(size_t)written>=sizeof bridge)return nullptr;
    return find_top_level_function(chunk,bridge,(size_t)written);
}

const DiamondFunction *find_collection_extension(
        const DiamondChunk *chunk,DiamondObjectKind kind,
        const char *name,size_t length) {
    const DiamondFunction *found=find_collection_extension_exact(chunk,kind,name,length);
    const size_t stem=diamond_method_stem_length(name,length);
    if(found==nullptr&&stem!=length)
        found=find_collection_extension_exact(chunk,kind,name,stem);
    return found;
}

/* The same extension protocol for String, Int, and Float: `"s".foo(x)`
 * probes string_foo(s, x); `5.foo(x)` probes integer_foo, then
 * numeric_foo; a Float probes float_foo, then numeric_foo -- each first
 * as a program's own function, then as the prelude's diamond_ version.
 * `**` maps to the name `pow`, since a function name can't contain
 * symbols. */
static const DiamondFunction *find_value_extension_exact(const DiamondChunk *chunk,
        const char *const *prefixes,size_t prefix_count,const char *name,size_t length) {
    for(size_t pass=0;pass<2;pass++)
        for(size_t index=0;index<prefix_count;index++) {
            char bridge[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(bridge,sizeof bridge,"%s%s%.*s",
                pass==0?"":"diamond_",prefixes[index],(int)length,name);
            if(written<=0||(size_t)written>=sizeof bridge)continue;
            const DiamondFunction *found=find_top_level_function(chunk,bridge,(size_t)written);
            if(found!=nullptr)return found;
        }
    return nullptr;
}

/* A predicate `"s".foo?(x)` probes the exact name (string_foo?) first, then
 * falls back to the name without the '?' (string_foo). */
const DiamondFunction *find_value_extension(const DiamondChunk *chunk,
        const char *const *prefixes,size_t prefix_count,const char *name,size_t length) {
    if(length==2&&memcmp(name,"**",2)==0) {name="pow";length=3;}
    const DiamondFunction *found=find_value_extension_exact(chunk,prefixes,prefix_count,name,length);
    const size_t stem=diamond_method_stem_length(name,length);
    if(found==nullptr&&stem!=length)
        found=find_value_extension_exact(chunk,prefixes,prefix_count,name,stem);
    return found;
}

/* Edit distance between two method names, counting a swap of adjacent
 * letters as one edit, or a value above `limit` once it is certain to exceed
 * it. Names are under DIAMOND_MAX_FUNCTION_NAME bytes. */
static size_t method_name_distance(const char *a,size_t a_length,
        const char *b,size_t b_length,size_t limit) {
    if(a_length>=DIAMOND_MAX_FUNCTION_NAME||b_length>=DIAMOND_MAX_FUNCTION_NAME)
        return limit+1;
    if((a_length>b_length?a_length-b_length:b_length-a_length)>limit)return limit+1;
    size_t rows[3][DIAMOND_MAX_FUNCTION_NAME+1];
    size_t *before=rows[0],*previous=rows[1],*current=rows[2];
    for(size_t j=0;j<=b_length;j++)previous[j]=j;
    for(size_t i=1;i<=a_length;i++) {
        current[0]=i;
        size_t row_min=current[0];
        for(size_t j=1;j<=b_length;j++) {
            const size_t substitute=previous[j-1]+(a[i-1]==b[j-1]?0:1);
            const size_t remove=previous[j]+1,insert=current[j-1]+1;
            size_t best=substitute<remove?substitute:remove;
            if(insert<best)best=insert;
            if(i>1&&j>1&&a[i-1]==b[j-2]&&a[i-2]==b[j-1]&&before[j-2]+1<best)
                best=before[j-2]+1;
            current[j]=best;
            if(best<row_min)row_min=best;
        }
        if(row_min>limit)return limit+1;
        size_t *recycled=before;
        before=previous;previous=current;current=recycled;
    }
    return previous[b_length];
}

typedef struct MethodSuggestion {
    const char *name;
    size_t length;
    size_t distance;
} MethodSuggestion;

static void consider_method_name(MethodSuggestion *best,const char *name,size_t length,
        const char *wanted,size_t wanted_length) {
    if(length==wanted_length&&memcmp(name,wanted,length)==0)return;
    const size_t distance=method_name_distance(wanted,wanted_length,name,length,
        best->distance-1);
    if(distance<best->distance) {
        best->name=name;best->length=length;best->distance=distance;
    }
}

/* Appends " -- did you mean 'x'?" to vm->error when a method the receiver does
 * have is one or two edits from `name`: the built-in names for `receiver_type`
 * (DIAMOND_TYPE_NONE for none), the program's and the prelude's extension
 * functions under `prefixes` (string_, array_, ...), and the methods of `class`
 * and its superclasses when it is not nullptr. */
void diamond_append_method_suggestion(DiamondVm *vm,const DiamondChunk *chunk,
        uint16_t receiver_type,const char *const *prefixes,size_t prefix_count,
        const DiamondClass *class,const char *name,size_t length) {
    if(length<3)return;
    MethodSuggestion best={.distance=(length<=5?1:2)+1};
    const char *const *natives=nullptr;
    const size_t native_count=receiver_type==DIAMOND_TYPE_NONE?0:
        diamond_native_member_names(receiver_type,&natives);
    for(size_t index=0;index<native_count;index++)
        consider_method_name(&best,natives[index],strlen(natives[index]),name,length);
    for(size_t index=0;prefix_count>0&&index<chunk->function_count;index++) {
        const DiamondFunction *function=chunk->functions[index];
        if(function->owner_class!=DIAMOND_OWNER_NONE||function->nested)continue;
        const char *candidate=function->name;
        if(strncmp(candidate,"diamond_",8)==0)candidate+=8;
        for(size_t prefix=0;prefix<prefix_count;prefix++) {
            const size_t prefix_length=strlen(prefixes[prefix]);
            if(strncmp(candidate,prefixes[prefix],prefix_length)!=0)continue;
            consider_method_name(&best,candidate+prefix_length,
                strlen(candidate+prefix_length),name,length);
        }
    }
    for(const DiamondClass *current=class;current!=nullptr;
        current=current->superclass==DIAMOND_NO_CLASS?nullptr:
            chunk->classes[current->superclass])
        for(size_t index=0;index<current->method_count;index++)
            consider_method_name(&best,current->methods[index].name,
                strlen(current->methods[index].name),name,length);
    if(best.name==nullptr)return;
    const size_t used=strlen(vm->error);
    if(used<sizeof vm->error)
        snprintf(vm->error+used,sizeof vm->error-used," -- did you mean '%.*s'?",
            (int)best.length,best.name);
}

void store_extension_lookup(DiamondVm *vm,const uint8_t *site,
        uint8_t kind,const DiamondFunction *function) {
    const size_t slot=diamond_cache_slot(site,vm->extension_caches,
        sizeof vm->extension_caches[0],&vm->cache_victim_cursor);
    DiamondExtensionCache *cache=&vm->extension_caches[slot];
    if(cache->site!=site) *cache=(DiamondExtensionCache){.site=site};
    for(size_t index=0;index<cache->entry_count;index++)
        if(cache->entries[index].kind==kind) {
            cache->entries[index]=(DiamondExtensionCacheEntry){
                .kind=kind,.found=true,.function=function};
            return;
        }
    size_t entry=cache->entry_count;
    if(entry<DIAMOND_INLINE_CACHE_WIDTH) {
        cache->entry_count++;
    } else {
        entry=cache->next_replace;
        cache->next_replace=(uint8_t)((cache->next_replace+1)%DIAMOND_INLINE_CACHE_WIDTH);
    }
    cache->entries[entry]=(DiamondExtensionCacheEntry){
        .kind=kind,.found=true,.function=function};
}

bool record_rewritten_site(DiamondVm *vm, const uint8_t *site) {
    for (size_t index=0;index<vm->rewritten_site_count;index++)
        if (vm->rewritten_sites[index]==site)return true;
    if(vm->rewritten_site_count==vm->rewritten_site_capacity) {
        if(vm->rewritten_site_count==DIAMOND_MAX_CODE)return false;
        size_t capacity=vm->rewritten_site_capacity==0?64:
            vm->rewritten_site_capacity*2;
        if(capacity>DIAMOND_MAX_CODE)capacity=DIAMOND_MAX_CODE;
        const uint8_t **sites=realloc(vm->rewritten_sites,
            capacity*sizeof *sites);
        if(sites==nullptr)return false;
        vm->rewritten_sites=sites;vm->rewritten_site_capacity=capacity;
    }
    vm->rewritten_sites[vm->rewritten_site_count++]=site;
    return true;
}

DiamondFieldCacheEntry *lookup_field_cached(
    DiamondVm *vm, const uint8_t *site, const DiamondInstance *instance,
    uint8_t field, bool write) {
    const size_t slot=diamond_cache_slot(site,vm->field_caches,
        sizeof vm->field_caches[0],&vm->cache_victim_cursor);
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

/* JIT trampoline for DIAMOND_OP_SET_IVAR -- see jit.h's own comment for why
 * this exists as a real C function generated code calls into rather than a
 * hand-rolled native field write: shape transitions and the GC write
 * barrier both need to stay exactly correct, and this is a direct copy of
 * the interpreter's own DIAMOND_OP_SET_IVAR case (src/vm.c's opcode
 * dispatch) with `site` threaded in from the caller instead of read off
 * `chunk`/`instruction_offset`, since generated code has neither -- see
 * this function's own caller in jit.c for what it passes instead (the
 * function's own bytecode offset at JIT-compile time, giving the same
 * per-occurrence cache-key stability an interpreted execution would have
 * gotten from `&chunk->code[instruction_offset]`). */
DiamondVmStatus diamond_jit_set_ivar(DiamondVm *vm, const uint8_t *site,
        const DiamondValue *receiver, uint8_t field, const DiamondValue *value) {
    if (receiver->kind != DIAMOND_VALUE_OBJECT ||
        receiver->as.object->kind != DIAMOND_OBJECT_INSTANCE) {
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondInstance *instance = (DiamondInstance *)receiver->as.object;
    if (instance->object.frozen) return DIAMOND_VM_FROZEN_ERROR;
    if (field >= instance->field_count) return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFieldCacheEntry *cached = lookup_field_cached(vm, site, instance, field, true);
    if (instance->shape != cached->output_shape) {
        instance->shape = cached->output_shape;
        vm->shape_transitions++;
    }
    instance->fields[field] = *value;
    if (!gc_write_barrier(vm, (DiamondObject *)instance)) return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}

/* JIT trampoline for DIAMOND_OP_GET_IVAR -- see diamond_jit_set_ivar's own
 * comment just above for why this exists as a real C function rather than
 * hand-rolled generated code: the field cache lookup is the same shared
 * machinery either way. A direct copy of the interpreter's own
 * DIAMOND_OP_GET_IVAR case (src/vm.c's opcode dispatch), with `site`
 * threaded in from the caller (generated code has no `chunk`/
 * instruction_offset to compute it from) exactly like diamond_jit_set_ivar.
 * Unlike SET_IVAR, this can never allocate or invoke user code (no operator
 * overload exists for plain field access), so its caller in jit.c needs
 * neither a DiamondFrame nor jc->has_called. */
DiamondVmStatus diamond_jit_get_ivar(DiamondVm *vm, const uint8_t *site,
        const DiamondValue *receiver, uint8_t field, DiamondValue *out) {
    if (receiver->kind != DIAMOND_VALUE_OBJECT ||
        receiver->as.object->kind != DIAMOND_OBJECT_INSTANCE) {
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondInstance *instance = (const DiamondInstance *)receiver->as.object;
    if (field >= instance->field_count) return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFieldCacheEntry *cached = lookup_field_cached(vm, site, instance, field, false);
    *out = cached->materialized ? instance->fields[field] : DIAMOND_NIL;
    return DIAMOND_VM_OK;
}





bool builder_append(StringBuilder *builder,const char *chars,size_t length) {
    size_t needed;
    if(ckd_add(&needed,builder->length,length)||ckd_add(&needed,needed,1))return false;
    if(needed>builder->capacity) {
        size_t capacity=builder->capacity==0?64:builder->capacity;
        while(capacity<needed)
            if(ckd_mul(&capacity,capacity,2))return false;
        char *grown=realloc(builder->chars,capacity);
        if(grown==nullptr)return false;
        builder->chars=grown;builder->capacity=capacity;
    }
    memcpy(builder->chars+builder->length,chars,length);
    builder->length+=length;builder->chars[builder->length]='\0';return true;
}





/* Publish complete bytes without replacing an existing directory entry.
 * The containing directory must be trusted and already exist. A failed final
 * directory sync can leave a complete published file: never remove that file
 * on failure, since another process may already have observed it. */



/* Sandbox per-capability granularity (docs/sandbox.md's own "Per-capability
 * granularity" section) -- VM_SANDBOX_GUARD's own second argument. Reads
 * DIAMOND_SANDBOX_ALLOW fresh on every call, exactly like VM_SANDBOX_GUARD
 * itself reads DIAMOND_SANDBOX fresh every time (see that macro's own
 * comment for why: every execution path -- top-level, a spawned Thread's
 * child_vm, a Supervisor child's run_vm, ProgramBuilder#run's own run_vm --
 * independently reads the same real process environment, so nothing needs
 * to propagate from a parent VM to a child one). `category` is always a
 * plain string literal from a VM_SANDBOX_GUARD call site, never a dynamic
 * value. Matches whole, comma-separated tokens only (surrounding spaces
 * trimmed) -- never a substring match, so a future longer category name
 * can never accidentally match a shorter one contained within it. Absent
 * or empty DIAMOND_SANDBOX_ALLOW (the common case: DIAMOND_SANDBOX=1 alone,
 * meaning "deny everything") returns false for every category, unchanged
 * from sandbox mode's original all-or-nothing behavior. */
bool sandbox_category_allowed(const char *category) {
    const char *allow = getenv("DIAMOND_SANDBOX_ALLOW");
    if (allow == nullptr) return false;
    const size_t category_length = strlen(category);
    const char *cursor = allow;
    while (*cursor != '\0') {
        while (*cursor == ' ' || *cursor == ',') cursor++;
        const char *token_start = cursor;
        while (*cursor != '\0' && *cursor != ',') cursor++;
        const char *token_end = cursor;
        while (token_end > token_start && token_end[-1] == ' ') token_end--;
        const size_t token_length = (size_t)(token_end - token_start);
        if (token_length == category_length &&
            memcmp(token_start, category, category_length) == 0)
            return true;
    }
    return false;
}




/* The vm->error text for an exception that nothing caught, built from
 * vm->exception. Thread#join leaves it to its call site so that a caught
 * exception does not overwrite vm->error. */
void set_uncaught_exception_error(DiamondVm *vm) {
    if(vm->exception.kind==DIAMOND_VALUE_OBJECT&&
       vm->exception.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *raised_instance=
            (const DiamondInstance *)vm->exception.as.object;
        snprintf(vm->error,sizeof vm->error,
            "uncaught exception: %s",raised_instance->class->name);
    } else {
        snprintf(vm->error,sizeof vm->error,"uncaught exception");
    }
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
    /* Never strictly needed to invalidate for correctness (see
     * DiamondExtensionCache's own comment, src/vm.h -- its resolution
     * never changes after compilation), but cleared here for the same
     * reason field_caches is: a fresh diamond_vm_run (a DIAMOND_REPEAT
     * rerun, the REPL, ...) starts with predictable, empty caches rather
     * than ones a previous run happened to warm. */
    memset(vm->extension_caches,0,sizeof(vm->extension_caches));
    vm->direct_dispatch_rewrites=0;
    vm->field_cache_hits=0;
    vm->field_cache_misses=0;
    vm->shape_transitions=0;
    vm->quickened_sites=0;
    vm->deoptimized_sites=0;
    vm->range_class_index=chunk->range_class_index;
    vm->root_chunk=chunk;
    return run_chunk(chunk, vm, nullptr, 0, 0, nullptr, result);
}

const char *diamond_vm_error(const DiamondVm *vm) {
    return vm->error[0]=='\0' ? nullptr : vm->error;
}

const char *diamond_vm_status_name(DiamondVmStatus status) {
    switch (status) {
        case DIAMOND_VM_NONLOCAL_EXIT:
            return "break or return from a block that can't reach its target";
        case DIAMOND_VM_OK:
            return "ok";
        case DIAMOND_VM_INVALID_BYTECODE:
            return "invalid bytecode";
        case DIAMOND_VM_CONSTANT_ERROR:
            return "constant initialization error";
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
        case DIAMOND_VM_NO_METHOD_ERROR:
            return "undefined method";
        case DIAMOND_VM_JSON_ERROR:
            return "json error";
        case DIAMOND_VM_SUPERVISOR_ERROR:
            return "supervisor error";
        case DIAMOND_VM_SANDBOX_ERROR:
            return "sandbox error";
        case DIAMOND_VM_RESOURCE_LIMIT_ERROR:
            return "resource limit exceeded";
        case DIAMOND_VM_FROZEN_ERROR:
            return "frozen object cannot be modified";
        case DIAMOND_VM_INTERRUPTED:
            return "interrupted by supervisor";
        case DIAMOND_VM_RESOURCE_EXHAUSTED:
            return "resource budget exhausted";
    }
    return "unknown VM status";
}
