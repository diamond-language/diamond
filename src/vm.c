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

/* Called with channel->lock held. Writes never block; a full pipe already
 * contains a wakeup. Waiters unlink under this same lock before closing their
 * descriptors, so no writer can retain a stale pointer or write to a closed
 * reader. No user code, allocation, or second channel lock is involved. */
void notify_channel_waiters(DiamondChannel *channel) {
    for(ChannelWaitLink *waiter=channel->waiters;waiter!=nullptr;waiter=waiter->next) {
        const char byte=1;
        ssize_t written;
        do {written=write(waiter->write_fd,&byte,1);} while(written<0&&errno==EINTR);
    }
}

typedef struct ChannelWaitRegistration {
    DiamondChannel *channel;
    ChannelWaitLink link;
} ChannelWaitRegistration;

/* A readiness wait does not consume data or reserve capacity. The caller
 * retries its operation after waking. Registration and readiness checks share
 * each channel's lock: a transition either precedes the check or writes to
 * the already-registered pipe. This closes the check/sleep lost-wakeup gap.
 * Receiver/argument registers keep all handles alive for this entire call.
 * fds has room for one extra descriptor after fd_count. target is NULL for
 * IO.poll, which waits on its supplied fds plus cancellation channels.
 * select_drained is non-NULL only for Channel.select and
 * Channel.select_send: the Array's channels are then candidates rather than
 * cancellation sources, ready when they hold a value (or, with `writable`,
 * have room) or close, except entries the caller already saw closed and drained
 * (select_drained[i]) -- those can never become ready again and would
 * otherwise spin the wait. */
DiamondVmStatus cancellable_wait_helper(DiamondVm *vm,DiamondChannel *target,
        bool writable,DiamondValue cancellations,DiamondValue deadline_value,
        struct pollfd *fds,nfds_t fd_count,const bool *select_drained) {
    if(cancellations.kind!=DIAMOND_VALUE_OBJECT ||
       cancellations.as.object->kind!=DIAMOND_OBJECT_ARRAY) {
        snprintf(vm->error,sizeof vm->error,"channel wait cancellations must be an Array of Channels");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondArray *array=(const DiamondArray *)cancellations.as.object;
    for(size_t i=0;i<array->count;i++) {
        if(array->values[i].kind!=DIAMOND_VALUE_OBJECT ||
           array->values[i].as.object->kind!=DIAMOND_OBJECT_CHANNEL) {
            snprintf(vm->error,sizeof vm->error,"channel wait cancellations must be an Array of Channels");
            return DIAMOND_VM_TYPE_ERROR;
        }
    }
    const bool timed=deadline_value.kind!=DIAMOND_VALUE_NIL;
    double deadline=0;
    if(timed) {
        if(deadline_value.kind!=DIAMOND_VALUE_FLOAT || !isfinite(deadline_value.as.real)) {
            snprintf(vm->error,sizeof vm->error,"channel wait deadline must be a finite monotonic Float or nil");
            return DIAMOND_VM_TYPE_ERROR;
        }
        deadline=deadline_value.as.real;
    }
    if(array->count>=SIZE_MAX/sizeof(ChannelWaitRegistration)-1)
        return DIAMOND_VM_OUT_OF_MEMORY;
    const size_t target_count=target==nullptr?0:1;
    const size_t count=array->count+target_count;
    ChannelWaitRegistration *registrations=calloc(count==0?1:count,sizeof *registrations);
    if(registrations==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    int descriptors[2];
    if(pipe(descriptors)!=0) {
        free(registrations);
        snprintf(vm->error,sizeof vm->error,"channel wait pipe: %s",strerror(errno));
        return DIAMOND_VM_IO_ERROR;
    }
    for(size_t i=0;i<2;i++) {
        if(fcntl(descriptors[i],F_SETFD,FD_CLOEXEC)<0 ||
           fcntl(descriptors[i],F_SETFL,O_NONBLOCK)<0) {
            const int error=errno;
            close(descriptors[0]);close(descriptors[1]);free(registrations);
            snprintf(vm->error,sizeof vm->error,"channel wait pipe flags: %s",strerror(error));
            return DIAMOND_VM_IO_ERROR;
        }
    }
    fds[fd_count]=(struct pollfd){.fd=descriptors[0],.events=POLLIN};
    bool ready=false;
    for(size_t i=0;i<count;i++) {
        DiamondChannel *channel=i<target_count?target:
            ((DiamondChannelHandle *)array->values[i-target_count].as.object)->channel;
        ChannelWaitRegistration *registration=&registrations[i];
        registration->channel=channel;
        registration->link.write_fd=descriptors[1];
        pthread_mutex_lock(&channel->lock);
        registration->link.next=channel->waiters;
        channel->waiters=&registration->link;
        if(select_drained!=nullptr)
            ready=ready||(writable?channel->count<channel->capacity&&!channel->closed&&
                    (!channel->rendezvous||channel->receivers_waiting>0):
                    channel->count>0) ||
                (channel->closed&&!select_drained[i-target_count]);
        else
            ready=ready||channel->closed ||
                (i<target_count&&(writable?channel->count<channel->capacity&&
                    (!channel->rendezvous||channel->receivers_waiting>0):channel->count>0));
        pthread_mutex_unlock(&channel->lock);
    }
    /* Trapped signals are dispatched at VM instruction boundaries. Only
     * VMs that installed handlers need a bounded return to that dispatcher;
     * ordinary worker waits have no periodic timeout. EINTR also returns
     * immediately, after unlinking, so handlers can safely close channels. */
    bool signal_aware=false;
    for(size_t i=0;i<DIAMOND_SIGNAL_COUNT;i++)
        if(vm->trapped_signal_handlers[i].kind==DIAMOND_VALUE_OBJECT)
            signal_aware=true;
    int wait_error=0;
    DiamondVmStatus budget_status=DIAMOND_VM_OK;
    while(!ready) {
        int milliseconds=-1;
        if(timed) {
            struct timespec now;
            if(clock_gettime(CLOCK_MONOTONIC,&now)!=0) {wait_error=errno;break;}
            const double remaining=deadline-(double)now.tv_sec-(double)now.tv_nsec/1e9;
            if(remaining<=0)break;
            const double rounded=ceil(remaining*1000);
            milliseconds=rounded>INT_MAX?INT_MAX:(int)rounded;
        }
        if(signal_aware&&(milliseconds<0||milliseconds>10))milliseconds=10;
        bool budget_bounded=false;
        milliseconds=bound_wait_by_wall_budget(vm,milliseconds,&budget_bounded);
        /* A supervised attempt wakes every slice to look at its interrupt
         * flag; the loop recomputes the deadline and waits again. */
        if(vm->interrupt_flag!=nullptr) {
            if(vm_interrupt_requested(vm)) {budget_status=DIAMOND_VM_INTERRUPTED;break;}
            if(milliseconds<0||milliseconds>DIAMOND_INTERRUPT_SLICE_MS)
                milliseconds=DIAMOND_INTERRUPT_SLICE_MS;
        }
        const int result=poll(fds,fd_count+1,milliseconds);
        if(result==0&&budget_bounded) {
            budget_status=wall_budget_after_wait(vm);
            if(budget_status!=DIAMOND_VM_OK)break;
        }
        if(result>0) {
            if(fds[fd_count].revents&(POLLERR|POLLHUP|POLLNVAL))wait_error=EIO;
            break;
        }
        if(result<0) {
            if(errno!=EINTR)wait_error=errno;
            for(nfds_t i=0;i<fd_count;i++)fds[i].revents=0;
            break;
        }
        if(signal_aware)break;
        /* Timeout: recompute against the absolute monotonic deadline.
         * Long deadlines may need more than one INT_MAX-millisecond wait. */
    }
    for(size_t i=0;i<count;i++) {
        ChannelWaitRegistration *registration=&registrations[i];
        pthread_mutex_lock(&registration->channel->lock);
        ChannelWaitLink **link=&registration->channel->waiters;
        while(*link!=&registration->link)link=&(*link)->next;
        *link=registration->link.next;
        pthread_mutex_unlock(&registration->channel->lock);
    }
    close(descriptors[0]);close(descriptors[1]);free(registrations);
    if(wait_error!=0) {
        snprintf(vm->error,sizeof vm->error,"channel wait: %s",strerror(wait_error));
        return DIAMOND_VM_IO_ERROR;
    }
    return budget_status;
}

void gc_unprotect(DiamondVm *vm, size_t saved_count);
static void free_adopted_programs(void *list);
static void populate_default_argv_env(DiamondVm *vm);


/* The three DIAMOND_MAX_* budgets bound the whole process, not each VM: a
 * program that spawns threads, supervised children or ProgramBuilder runs would
 * otherwise get a full allowance for every one of them. Every VM with a
 * budget configured registers here. The first registration after the last
 * budgeted VM was freed starts a fresh allowance (instruction total, byte
 * total, wall-clock origin), so successive programs in one process (the test
 * runner, an embedder) do not inherit each other's use. */
static atomic_size_t budget_live_vms;
static atomic_size_t budget_instructions;
static atomic_ptrdiff_t budget_bytes;
static atomic_int_least64_t budget_start_ns;

int64_t monotonic_nanoseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (int64_t)now.tv_sec*1000000000LL+(int64_t)now.tv_nsec;
}

/* The single collection-trigger check every allocate_* helper in this
 * file makes before actually allocating -- originally factored out of
 * 23 previously-duplicated inline copies (confirmed by direct grep, not
 * assumed) so a generational collector's own cheaper, more-frequent
 * minor-GC threshold would have exactly one place to be added, instead
 * of needing to touch all 23 call sites again -- and now that place.
 * Minor checked first (cheaper, fires far more often); major checked
 * unconditionally after, in case a lot of the nursery just got promoted
 * and total live bytes are already past next_gc too.
 *
 * Also the one place a configured DIAMOND_MAX_MEMORY_BYTES budget
 * (docs/sandbox.md's own "Resource limits" section) is enforced -- exactly
 * this comment's own "exactly one place to be added" reasoning, extended
 * to a second kind of limit. Returns false (checked after both collection
 * passes above already ran, giving them a real chance to free memory
 * first) once live bytes are still over budget; every one of this
 * function's own callers already has a `return nullptr`/`return false`
 * OOM path one line below its own `malloc`/`calloc` check, and reuses that
 * exact path here rather than a new error shape -- see this status's own
 * comment in vm.h for why this deliberately reuses DIAMOND_VM_OUT_OF_
 * MEMORY rather than a new status.
 *
 * The budget fires in two steps. The first time bytes_allocated exceeds
 * max_memory_bytes it sets memory_limit_tripped, raises max_memory_bytes by the
 * grace allowance and returns false: catch_runtime_error's own path to report
 * this as a real, rescuable ResourceLimitError itself calls allocate_instance/
 * allocate_string, which call straight back into this function, so the check
 * cannot stay armed at the same level or the exception could never be built,
 * let alone rescued. Exceeding the raised ceiling too sets memory_hard_stopped
 * and returns false again, and that OOM is deliberately not mapped to any
 * exception class (exception_class_for_status), so nothing can rescue it. */
bool maybe_collect(DiamondVm *vm) {
    vm->memory_recheck=false;
    if(vm->stress_minor_gc||
       vm->bytes_allocated-vm->bytes_allocated_at_last_minor_gc>=vm->minor_gc_threshold_bytes)
        diamond_vm_collect_minor(vm);
    if(vm->stress_gc||vm->bytes_allocated>=vm->next_gc)diamond_vm_collect(vm);
    if(vm->max_memory_bytes!=0) {
        /* Publish this VM's change since the last call into the process-wide
         * total, and judge the budget against the total. */
        const ptrdiff_t delta=(ptrdiff_t)vm->bytes_allocated-vm->memory_published;
        const ptrdiff_t total=atomic_fetch_add(&budget_bytes,delta)+delta;
        vm->memory_published=(ptrdiff_t)vm->bytes_allocated;
        if(total<=0||(size_t)total<=vm->max_memory_bytes)return true;
        if(!vm->memory_limit_tripped) {
            /* First overrun: the catchable ResourceLimitError. The ceiling
             * moves up by the grace allowance so the exception can be built
             * and a rescue clause can run. */
            vm->memory_limit_tripped=true;
            const size_t default_grace=vm->max_memory_bytes/4>DIAMOND_MEMORY_GRACE_MINIMUM_BYTES?
                vm->max_memory_bytes/4:(size_t)DIAMOND_MEMORY_GRACE_MINIMUM_BYTES;
            const size_t grace=vm->grace_memory_bytes!=0?vm->grace_memory_bytes:default_grace;
            vm->max_memory_bytes=vm->max_memory_bytes>SIZE_MAX-grace?SIZE_MAX:
                vm->max_memory_bytes+grace;
            return false;
        }
        /* Past the grace ceiling too: stop for good. */
        vm->memory_hard_stopped=true;
        snprintf(vm->error,sizeof vm->error,
            "memory budget exhausted: the program kept allocating after "
            "its ResourceLimitError");
        return false;
    }
    return true;
}

/* The cap on live spawned threads: DIAMOND_MAX_THREADS (the process-wide
 * hard cap) unless DIAMOND_MAX_LIVE_THREADS asks for fewer. Read on each
 * Thread.new/add_child like DIAMOND_SANDBOX, so every VM agrees without
 * propagation. An unparseable, zero or too-large value keeps the hard cap;
 * a limit can only tighten it. */
size_t diamond_live_thread_limit(void) {
    const char *env=getenv("DIAMOND_MAX_LIVE_THREADS");
    if(env==nullptr||env[0]=='\0')return DIAMOND_MAX_THREADS;
    char *end=nullptr;
    const unsigned long long parsed=strtoull(env,&end,10);
    if(end==env||*end!='\0'||parsed==0||parsed>DIAMOND_MAX_THREADS)
        return DIAMOND_MAX_THREADS;
    return (size_t)parsed;
}

/* Takes one slot of the live-thread budget, or fails when none is left. The count is claimed with a compare-and-swap so that threads creating
 * children at the same moment cannot all pass the check and overshoot the cap, and it is taken *before* the thread starts: a thread that ends
 * immediately decrements the count, and the parent adding afterwards let it wrap below zero, which made every other creation fail. */
bool diamond_reserve_thread_slot(void) {
    size_t current=atomic_load(&diamond_active_thread_count);
    const size_t limit=diamond_live_thread_limit();
    do {
        if(current>=limit)return false;
    } while(!atomic_compare_exchange_weak(&diamond_active_thread_count,&current,current+1));
    return true;
}

/* A configured instruction or wall-clock budget has just been exceeded:
 * run_chunk is about to return the catchable DIAMOND_VM_RESOURCE_LIMIT_ERROR.
 * Disarm both soft budgets (re-tripping on the very next instruction would
 * make the rescue clause that handles the error impossible to run) and arm
 * the hard limits that bound the grace allowance instead, each only if its
 * own budget was configured. The wall-clock limit is measured from the
 * budget's deadline, not from this moment, so a program that rescues late is
 * not given extra time; the instruction limit counts this VM's own further
 * instructions, since the shared total is no longer being added to. */
void resource_limit_trip(DiamondVm *vm) {
    if(vm->max_instructions!=0)
        vm->hard_stop_instruction_count=vm->instructions_executed+
            (vm->grace_instructions!=0?vm->grace_instructions:(size_t)DIAMOND_INSTRUCTION_GRACE);
    if(vm->max_wall_nanoseconds!=0) {
        const int64_t now_ns=monotonic_nanoseconds();
        const int64_t deadline=vm->start_time_ns+vm->max_wall_nanoseconds;
        vm->hard_stop_time_ns=(now_ns>deadline?now_ns:deadline)+
            (vm->grace_wall_nanoseconds!=0?vm->grace_wall_nanoseconds:
                (int64_t)DIAMOND_WALL_GRACE_MILLISECONDS*1000000LL);
    }
    vm->max_instructions=0;
    vm->max_wall_nanoseconds=0;
    vm->resource_grace_active=true;
    /* resource_limits_active stays true: the grace checks run in the same
     * per-instruction block. */
}

/* The grace allowance ran out. Returns the status for run_chunk to return;
 * no exception class maps to it, so nothing can rescue it. */
DiamondVmStatus resource_hard_stop(DiamondVm *vm,const char *budget) {
    vm->hard_stop_instruction_count=0;
    vm->hard_stop_time_ns=0;
    snprintf(vm->error,sizeof vm->error,
        "%s budget exhausted: the program kept running after its ResourceLimitError",
        budget);
    return DIAMOND_VM_RESOURCE_EXHAUSTED;
}

/* The absolute CLOCK_MONOTONIC time at which the wall-clock budget next stops
 * this VM, or 0 when none is configured: the soft budget's deadline while it is
 * armed, the end of the grace allowance once it has tripped. */
static int64_t wall_budget_limit_ns(const DiamondVm *vm) {
    if(vm->max_wall_nanoseconds!=0)return vm->start_time_ns+vm->max_wall_nanoseconds;
    if(vm->resource_grace_active&&vm->hard_stop_time_ns!=0)return vm->hard_stop_time_ns;
    return 0;
}

/* A blocking wait (IO.poll, a channel wait) sleeps in the kernel, where the
 * per-instruction budget check cannot run, so an unbounded or long wait would
 * outlive DIAMOND_MAX_WALL_MILLISECONDS entirely. Clamps `timeout_ms` (-1 for
 * none) to just past the budget's remaining time and sets *bounded when it did,
 * so the caller can tell a budget expiry from an ordinary timeout. */
int bound_wait_by_wall_budget(const DiamondVm *vm,int timeout_ms,bool *bounded) {
    *bounded=false;
    const int64_t limit=wall_budget_limit_ns(vm);
    if(limit==0)return timeout_ms;
    const int64_t remaining_ns=limit-monotonic_nanoseconds();
    const int64_t remaining_ms=remaining_ns<=0?0:(remaining_ns+999999)/1000000;
    const int64_t slice=remaining_ms+1;
    if(timeout_ms>=0&&timeout_ms<=slice)return timeout_ms;
    *bounded=true;
    return slice>INT_MAX?INT_MAX:(int)slice;
}

/* Called after a wait that bound_wait_by_wall_budget clamped returned with
 * nothing ready. DIAMOND_VM_OK if the budget has not actually run out yet (the
 * caller treats it as the timeout it is); otherwise the same status the
 * per-instruction check would have produced. */
DiamondVmStatus wall_budget_after_wait(DiamondVm *vm) {
    const int64_t limit=wall_budget_limit_ns(vm);
    if(limit==0||monotonic_nanoseconds()<=limit)return DIAMOND_VM_OK;
    if(vm->max_wall_nanoseconds!=0) {
        resource_limit_trip(vm);
        return DIAMOND_VM_RESOURCE_LIMIT_ERROR;
    }
    return resource_hard_stop(vm,"wall-clock");
}

DiamondVmStatus wait_fd_interruptible(DiamondVm *vm,int fd,short events) {
    if(vm->interrupt_flag==nullptr)return DIAMOND_VM_OK;
    for(;;) {
        if(vm_interrupt_requested(vm))return DIAMOND_VM_INTERRUPTED;
        struct pollfd interest={.fd=fd,.events=events};
        if(poll(&interest,1,DIAMOND_INTERRUPT_SLICE_MS)!=0)return DIAMOND_VM_OK;
    }
}

bool vm_blocking_call_begin(DiamondVm *vm,int kind,void *handle) {
    if(vm->interrupt_flag==nullptr)return true;
    pthread_mutex_lock(&vm->blocking_call_lock);
    /* The interrupting thread sets the flag before it takes this lock, so
     * either this sees the flag or that thread sees the registration. */
    const bool interrupted=vm_interrupt_requested(vm);
    if(!interrupted) {
        vm->blocking_call_kind=kind;
        vm->blocking_call_handle=handle;
    }
    pthread_mutex_unlock(&vm->blocking_call_lock);
    return !interrupted;
}

void vm_blocking_call_end(DiamondVm *vm) {
    if(vm->interrupt_flag==nullptr)return;
    pthread_mutex_lock(&vm->blocking_call_lock);
    vm->blocking_call_kind=0;
    vm->blocking_call_handle=nullptr;
    pthread_mutex_unlock(&vm->blocking_call_lock);
}

void vm_blocking_call_interrupt(DiamondVm *vm) {
    pthread_mutex_lock(&vm->blocking_call_lock);
    if(vm->blocking_call_handle!=nullptr) {
        if(vm->blocking_call_kind==DIAMOND_BLOCKING_SQLITE) {
#ifndef DIAMOND_NO_SQLITE
            sqlite3_interrupt((sqlite3 *)vm->blocking_call_handle);
#endif
        } else if(vm->blocking_call_kind==DIAMOND_BLOCKING_POSTGRES) {
#ifndef DIAMOND_NO_POSTGRES
            char ignored[256];
            (void)PQcancel((PGcancel *)vm->blocking_call_handle,ignored,sizeof ignored);
#endif
        } else if(vm->blocking_call_kind==DIAMOND_BLOCKING_MYSQL)
            (void)shutdown((int)((intptr_t)vm->blocking_call_handle-1),SHUT_RDWR);
    }
    pthread_mutex_unlock(&vm->blocking_call_lock);
}

/* Stops a supervised child's running attempt: sets its interrupt flag, arms the
 * per-instruction check and cancels a database call it is blocked in. Called
 * with the supervisor lock held (it keeps running_vm valid). */
void supervisor_interrupt_child(DiamondSupervisorChild *child) {
    if(child->running_vm!=nullptr)
        atomic_store(&child->running_vm->resource_limits_active,true);
    atomic_store(&child->interrupt,true);
    if(child->running_vm!=nullptr)
        vm_blocking_call_interrupt(child->running_vm);
}

/* Stops a thread started by an interrupted VM: arms its interrupt flag, the
 * per-instruction check and any database call it is blocked in. Has an effect
 * only on a thread whose child VM carries the flag (see THREAD_NEW). */
void thread_interrupt(DiamondThread *thread) {
    if(thread==nullptr||!thread->spawned||thread->joined)return;
    if(thread->child_vm==nullptr||thread->child_vm->interrupt_flag!=&thread->interrupt)
        return;
    atomic_store(&thread->child_vm->resource_limits_active,true);
    atomic_store(&thread->interrupt,true);
    vm_blocking_call_interrupt(thread->child_vm);
}

int poll_interruptible(DiamondVm *vm,struct pollfd *fds,nfds_t count,
        int timeout_ms,DiamondVmStatus *status) {
    *status=DIAMOND_VM_OK;
    if(vm->interrupt_flag==nullptr)return poll(fds,count,timeout_ms);
    const int64_t deadline_ns=timeout_ms<0?0:
        monotonic_nanoseconds()+(int64_t)timeout_ms*1000000;
    for(;;) {
        if(vm_interrupt_requested(vm)) {
            *status=DIAMOND_VM_INTERRUPTED;
            return 0;
        }
        int slice=DIAMOND_INTERRUPT_SLICE_MS;
        if(timeout_ms>=0) {
            const int64_t remaining_ns=deadline_ns-monotonic_nanoseconds();
            if(remaining_ns<=0)return poll(fds,count,0);
            const int64_t remaining_ms=(remaining_ns+999999)/1000000;
            if(remaining_ms<slice)slice=(int)remaining_ms;
        }
        const int result=poll(fds,count,slice);
        if(result!=0)return result;
    }
}

/* pthread_cond_wait, bounded by the wall-clock budget (see
 * bound_wait_by_wall_budget). `lock` is held on entry and on return. Returns
 * DIAMOND_VM_OK when the caller should re-test its condition, or the budget's
 * status when the budget ran out while waiting. The channels' condition
 * variables use the default CLOCK_REALTIME. */
DiamondVmStatus channel_wait_bounded(DiamondVm *vm,pthread_cond_t *condition,
        pthread_mutex_t *lock) {
    bool bounded=false;
    int milliseconds=bound_wait_by_wall_budget(vm,-1,&bounded);
    if(vm->interrupt_flag!=nullptr) {
        /* Sleep in slices so a set interrupt flag is seen between them. */
        if(vm_interrupt_requested(vm))return DIAMOND_VM_INTERRUPTED;
        if(!bounded||milliseconds>DIAMOND_INTERRUPT_SLICE_MS)
            milliseconds=DIAMOND_INTERRUPT_SLICE_MS;
        bounded=true;
    }
    if(!bounded) {
        pthread_cond_wait(condition,lock);
        return DIAMOND_VM_OK;
    }
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME,&deadline);
    deadline.tv_sec+=milliseconds/1000;
    deadline.tv_nsec+=(long)(milliseconds%1000)*1000000L;
    if(deadline.tv_nsec>=1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec-=1000000000L;
    }
    if(pthread_cond_timedwait(condition,lock,&deadline)==ETIMEDOUT) {
        if(vm_interrupt_requested(vm))return DIAMOND_VM_INTERRUPTED;
        return wall_budget_after_wait(vm);
    }
    return DIAMOND_VM_OK;
}

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

static void budget_register(DiamondVm *vm) {
    if(atomic_fetch_add(&budget_live_vms,1)==0) {
        atomic_store(&budget_instructions,0);
        atomic_store(&budget_bytes,0);
        atomic_store(&budget_start_ns,0);
    }
    vm->budgets_registered=true;
    if(vm->max_wall_nanoseconds!=0) {
        int_least64_t expected=0;
        atomic_compare_exchange_strong(&budget_start_ns,&expected,monotonic_nanoseconds());
        vm->start_time_ns=atomic_load(&budget_start_ns);
    }
}

/* A thread that has run to completion keeps its heap until its handle is
 * collected (join still reads the result out of it), but nothing runs in it
 * any more: add the instructions it has not flushed to the total, and stop
 * counting its heap against the memory budget, so a parent that is about to
 * copy the result into its own heap is not refused for memory that is only
 * waiting to be reclaimed. */
void budget_release_finished(DiamondVm *vm) {
    if(!vm->budgets_registered)return;
    atomic_fetch_add_explicit(&budget_instructions,
        vm->instructions_executed-vm->instructions_flushed,memory_order_relaxed);
    vm->instructions_flushed=vm->instructions_executed;
    atomic_fetch_sub(&budget_bytes,vm->memory_published);
    vm->memory_published=0;
    vm->max_memory_bytes=0;
}

static void budget_unregister(DiamondVm *vm) {
    if(!vm->budgets_registered)return;
    atomic_fetch_add_explicit(&budget_instructions,
        vm->instructions_executed-vm->instructions_flushed,memory_order_relaxed);
    atomic_fetch_sub(&budget_bytes,vm->memory_published);
    atomic_fetch_sub(&budget_live_vms,1);
}

/* Adds this VM's instructions since its last flush to the process-wide total
 * and reports whether the total has passed max_instructions. run_chunk calls
 * it every DIAMOND_RESOURCE_LIMIT_CLOCK_CHECK_MASK + 1 instructions a
 * budgeted VM executes, so a VM is told about an exhausted budget at its next
 * such point: promptly if it is still doing real work, never if it is only
 * waiting to join a thread that used the allowance up. A VM that finishes
 * before its next flush adds its remainder when it is freed, so a swarm of
 * short-lived threads cannot slip under the count. */
bool instruction_budget_exceeded(DiamondVm *vm) {
    const size_t pending=vm->instructions_executed-vm->instructions_flushed;
    vm->instructions_flushed=vm->instructions_executed;
    const size_t total=atomic_fetch_add_explicit(&budget_instructions,pending,
        memory_order_relaxed)+pending;
    return total>vm->max_instructions;
}

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
    const size_t size=sizeof(DiamondInstance)+class->field_count*sizeof(DiamondValue);
    DiamondInstance *instance=malloc(size); if(instance==nullptr)return nullptr;
    instance->object=(DiamondObject){.next=vm->young_objects,.kind=DIAMOND_OBJECT_INSTANCE};
    instance->class=class; instance->shape=&class->shapes[0]; instance->owner=chunk;
    instance->field_count=class->field_count;
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
static DiamondArray *allocate_array_uninitialized(DiamondVm *vm,size_t count) {
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
 * guarantees is async-signal-safe: update a lock-free atomic bitmask and
 * return. Everything else -- resolving which Diamond closure to call,
 * actually calling it -- happens later, synchronously, from ordinary
 * (non-signal-handler) code that checks these flags at safe points: once
 * per bytecode instruction in run_chunk's own dispatch loop (see below),
 * and after EINTR from the handful of genuinely-blocking native calls
 * where waiting for the *next* bytecode instruction to run this check
 * could mean waiting arbitrarily long (TCPServer#accept, IO.poll,
 * UDPSocket#receive -- see their own opcode handlers). */
const int diamond_signal_numbers[DIAMOND_SIGNAL_COUNT]={SIGINT,SIGTERM,SIGHUP};
const char *const diamond_signal_names[DIAMOND_SIGNAL_COUNT]={"INT","TERM","HUP"};
/* Signal handlers must never enter an atomic library lock. All supported
 * targets provide lock-free unsigned int atomics. A single mask also avoids
 * losing a delivery between clearing a summary flag and its per-signal flag. */
static_assert(ATOMIC_INT_LOCK_FREE==2,"signal delivery requires lock-free int atomics");
atomic_uint diamond_pending_signals=0;

void diamond_signal_handler(int signal_number) {
    for(size_t index=0;index<DIAMOND_SIGNAL_COUNT;index++) {
        if(diamond_signal_numbers[index]==signal_number) {
            atomic_fetch_or_explicit(&diamond_pending_signals,1u<<index,memory_order_relaxed);
            return;
        }
    }
}

/* Invokes every currently-pending trapped signal's Diamond handler, in
 * signal-index order, synchronously -- via the same nested-run_chunk
 * mechanism DIAMOND_OP_CALL_CLOSURE itself uses, since a trapped handler
 * is an ordinary 0-arity Callable, not anything signal-specific at the
 * bytecode level. Claims each signal's pending bit before invoking its handler, not after --
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
DiamondVmStatus dispatch_pending_signals(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,bool *any_invoked) {
    *any_invoked=false;
    if(!atomic_load_explicit(&diamond_pending_signals,memory_order_relaxed))return DIAMOND_VM_OK;
    for(size_t index=0;index<DIAMOND_SIGNAL_COUNT;index++) {
        /* A VM without this handler must leave the delivery for its owner.
         * Multiple eligible VMs atomically claim a coalesced delivery once. */
        if(vm->trapped_signal_handlers[index].kind!=DIAMOND_VALUE_OBJECT||
           vm->trapped_signal_handlers[index].as.object->kind!=DIAMOND_OBJECT_CLOSURE)
            continue;
        const unsigned bit=1u<<index;
        if(!(atomic_fetch_and_explicit(&diamond_pending_signals,~bit,memory_order_relaxed)&bit))
            continue;
        *any_invoked=true;
        const DiamondClosure *handler=
            (const DiamondClosure *)vm->trapped_signal_handlers[index].as.object;
        /* See DIAMOND_OP_CALL_CLOSURE's own comment -- a compile_method
         * result is scoped to define_method, not general Callable use. */
        if(handler->foreign_chunk!=nullptr)continue;
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
          .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
          .register_count=fn->register_count};
        DiamondValue ignored=DIAMOND_NIL;
        const DiamondVmStatus status=run_chunk(&child,vm,nullptr,0,depth+1,handler,&ignored);
        if(status!=DIAMOND_VM_OK)return status;
    }
    return DIAMOND_VM_OK;
}

/* Readiness is meaningful for TLS only while its handshake is pending.
 * Completed TLS sockets retain their blocking application I/O contract. */
DiamondVmStatus pollable_fd(DiamondVm *vm,DiamondValue value,int *out_fd) {
    if(value.kind!=DIAMOND_VALUE_OBJECT||
       (value.as.object->kind!=DIAMOND_OBJECT_LISTENER&&
        value.as.object->kind!=DIAMOND_OBJECT_SOCKET&&
        value.as.object->kind!=DIAMOND_OBJECT_UDP_SOCKET&&
        value.as.object->kind!=DIAMOND_OBJECT_TLS_SOCKET&&
        value.as.object->kind!=DIAMOND_OBJECT_PROCESS_STREAM)) {
        snprintf(vm->error,sizeof vm->error,"IO.poll arguments must be nonblocking "
            "TCPServer listeners, their accepted Sockets, UDPSockets, or a Process.spawn stream");
        return DIAMOND_VM_TYPE_ERROR;
    }
    int fd=-1;
    const char *closed_message="cannot poll a closed listener/socket";
    if(value.as.object->kind==DIAMOND_OBJECT_LISTENER)
        fd=((DiamondListenerHandle *)value.as.object)->fd;
    else if(value.as.object->kind==DIAMOND_OBJECT_SOCKET)
        fd=((DiamondSocketHandle *)value.as.object)->fd;
    else if(value.as.object->kind==DIAMOND_OBJECT_TLS_SOCKET) {
        DiamondTlsSocketHandle *tls=(DiamondTlsSocketHandle *)value.as.object;
        if(!tls->handshake_pending) {
            snprintf(vm->error,sizeof vm->error,"IO.poll only supports TLS sockets during their handshake");
            return DIAMOND_VM_TYPE_ERROR;
        }
        fd=tls->fd;
    } else if(value.as.object->kind==DIAMOND_OBJECT_UDP_SOCKET) {
        fd=((DiamondUdpSocketHandle *)value.as.object)->fd;
        closed_message="cannot poll a closed UDP socket";
    } else {
        fd=((DiamondProcessStream *)value.as.object)->fd;
        closed_message="cannot poll a closed process stream";
    }
    if(fd<0) {
        snprintf(vm->error,sizeof vm->error,"%s",closed_message);
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
bool poll_register_fd(struct pollfd *fds,nfds_t *fd_count,size_t max_fds,
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

/* ClassName.compile_method(name, params, body_source) -- see
 * docs/design.md's "Runtime method synthesis" section for the full
 * design. Validates every identifier that gets spliced into the
 * synthesized source itself (name, each param); body_source is not
 * restricted, since letting it be arbitrary Diamond source is the whole
 * point -- same trust level as Ruby's class_eval/define_method, not
 * something to sandbox against here. */
static bool is_valid_identifier(const char *chars,size_t length) {
    if(length==0)return false;
    if(!(isalpha((unsigned char)chars[0])||chars[0]=='_'))return false;
    for(size_t index=1;index<length;index++)
        if(!(isalnum((unsigned char)chars[index])||chars[index]=='_'))return false;
    return true;
}

/* A method name as `def` accepts one: an identifier, optionally ending in
 * one `?` (predicate), `!`, or `=` (writer, as `obj.name = value` calls). */
static bool is_valid_method_name(const char *chars,size_t length) {
    if(length>1&&(chars[length-1]=='?'||chars[length-1]=='!'||chars[length-1]=='='))
        length--;
    return is_valid_identifier(chars,length);
}

bool grow_buffer_append(GrowBuffer *buffer,const char *text,size_t text_length) {
    size_t needed;
    if(ckd_add(&needed,buffer->length,text_length)||ckd_add(&needed,needed,1))return false;
    if(needed>buffer->capacity) {
        size_t new_capacity=buffer->capacity==0?256:buffer->capacity;
        while(new_capacity<needed)
            if(ckd_mul(&new_capacity,new_capacity,2))return false;
        char *new_data=realloc(buffer->data,new_capacity);
        if(new_data==nullptr)return false;
        buffer->data=new_data;buffer->capacity=new_capacity;
    }
    memcpy(buffer->data+buffer->length,text,text_length);
    buffer->length+=text_length;
    buffer->data[buffer->length]='\0';
    return true;
}

/* Synthesizes "class <Name>\n  attr_accessor <fields>\n  def <method_name>
 * (<params>)\n<body_source>\n  end\nend\n" -- the attr_accessor line
 * (skipped when the class has no fields) re-declares target_class's own
 * existing field names, in order, using the compiler's completely
 * unmodified @ivar auto-declare-on-first-reference machinery to seed an
 * identically-shaped field table in the throwaway program compiled
 * below, rather than adding any new compiler entry point for this. */
/* `bound_values_hash` is nullptr-safe (an empty bound_values Hash still
 * has a live, zero-count DiamondHash object -- this file never passes
 * nullptr here, but treating it as "no bound values" defensively costs
 * nothing). Each key becomes an extra trailing parameter name after
 * params_array's own, in the same order compile_method_helper iterates
 * the same hash separately to collect the *values* -- the two orders
 * must match, so both walk `hash->entries[0..count)` directly rather
 * than one of them going through some other iteration order. */
static bool build_compiled_method_source(GrowBuffer *buffer,const DiamondClass *target_class,
        const DiamondString *name_string,const DiamondArray *params_array,
        const DiamondString *body_string,const DiamondHash *bound_values_hash,
        DiamondVm *vm,bool *validation_failed) {
    *validation_failed=false;
    if(!is_valid_method_name(name_string->chars,name_string->length)) {
        snprintf(vm->error,sizeof vm->error,"compile_method name must be a valid method name");
        *validation_failed=true;return false;
    }
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"class "))return false;
    const size_t class_name_length=strlen(target_class->name);
    if(!grow_buffer_append(buffer,target_class->name,class_name_length))return false;
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\n"))return false;
    if(target_class->field_count>0) {
        if(!GROW_BUFFER_APPEND_LITERAL(buffer,"  attr_accessor "))return false;
        for(size_t index=0;index<target_class->field_count;index++) {
            if(index>0&&!GROW_BUFFER_APPEND_LITERAL(buffer,", "))return false;
            const size_t field_length=strlen(target_class->fields[index]);
            if(!grow_buffer_append(buffer,target_class->fields[index],field_length))return false;
        }
        if(!GROW_BUFFER_APPEND_LITERAL(buffer,"\n"))return false;
    }
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"  def "))return false;
    if(!grow_buffer_append(buffer,name_string->chars,name_string->length))return false;
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,"("))return false;
    bool any_param=false;
    for(size_t index=0;index<params_array->count;index++) {
        const DiamondValue param=params_array->values[index];
        if(param.kind!=DIAMOND_VALUE_OBJECT||param.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,"compile_method params must be an Array of Strings");
            *validation_failed=true;return false;
        }
        const DiamondString *param_string=(const DiamondString *)param.as.object;
        if(!is_valid_identifier(param_string->chars,param_string->length)) {
            snprintf(vm->error,sizeof vm->error,
                "compile_method param names must be valid identifiers");
            *validation_failed=true;return false;
        }
        if(any_param&&!GROW_BUFFER_APPEND_LITERAL(buffer,", "))return false;
        if(!grow_buffer_append(buffer,param_string->chars,param_string->length))return false;
        any_param=true;
    }
    for(size_t index=0;index<bound_values_hash->count;index++) {
        const DiamondValue key=bound_values_hash->entries[index].key;
        if(key.kind!=DIAMOND_VALUE_OBJECT||key.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "compile_method bound_values keys must be Strings");
            *validation_failed=true;return false;
        }
        const DiamondString *key_string=(const DiamondString *)key.as.object;
        if(!is_valid_identifier(key_string->chars,key_string->length)) {
            snprintf(vm->error,sizeof vm->error,
                "compile_method bound_values keys must be valid identifiers");
            *validation_failed=true;return false;
        }
        if(any_param&&!GROW_BUFFER_APPEND_LITERAL(buffer,", "))return false;
        if(!grow_buffer_append(buffer,key_string->chars,key_string->length))return false;
        any_param=true;
    }
    if(!GROW_BUFFER_APPEND_LITERAL(buffer,")\n"))return false;
    if(!grow_buffer_append(buffer,body_string->chars,body_string->length))return false;
    return GROW_BUFFER_APPEND_LITERAL(buffer,"\n  end\nend\n");
}

/* Finds the DiamondClass this compile compiled (by name -- the prelude
 * already declares many classes of its own, so the synthesized class
 * is not reliably at any fixed index) and validates its field_count
 * exactly matches target_class's own -- the one check that actually
 * proves body_source didn't reference/auto-declare any field beyond
 * the ones build_compiled_method_source seeded via attr_accessor, i.e.
 * that every @field offset the compiled bytecode assumes is valid
 * against target_class's real instances. */
static const DiamondClass *find_matching_synthetic_class(const DiamondChunk *synthetic_chunk,
        const DiamondClass *target_class,DiamondVm *vm) {
    const size_t class_name_length=strlen(target_class->name);
    for(size_t index=0;index<synthetic_chunk->class_count;index++) {
        const DiamondClass *candidate=synthetic_chunk->classes[index];
        if(strlen(candidate->name)==class_name_length&&
           memcmp(candidate->name,target_class->name,class_name_length)==0) {
            if(candidate->field_count!=target_class->field_count) {
                snprintf(vm->error,sizeof vm->error,
                    "compile_method body references a field '%s' does not have",
                    target_class->name);
                return nullptr;
            }
            return candidate;
        }
    }
    snprintf(vm->error,sizeof vm->error,
        "compile_method: internal error locating compiled class '%s'",target_class->name);
    return nullptr;
}

DiamondVmStatus compile_method_helper(DiamondVm *vm,const DiamondClass *target_class,
        const DiamondString *name_string,const DiamondArray *params_array,
        const DiamondString *body_string,const DiamondHash *bound_values_hash,
        DiamondValue *out_result) {
    if(name_string->length==0||name_string->length>=DIAMOND_MAX_FUNCTION_NAME) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method name must be 1 to %d characters",DIAMOND_MAX_FUNCTION_NAME-1);
        return DIAMOND_VM_ARITY_ERROR;
    }
    if(bound_values_hash->count>DIAMOND_MAX_BOUND_VALUES) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method bound_values must have at most %d entries",
            DIAMOND_MAX_BOUND_VALUES);
        return DIAMOND_VM_ARITY_ERROR;
    }
    GrowBuffer source={0};
    bool validation_failed=false;
    if(!build_compiled_method_source(&source,target_class,name_string,params_array,
            body_string,bound_values_hash,vm,&validation_failed)) {
        free(source.data);
        if(validation_failed)return DIAMOND_VM_ARITY_ERROR;
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    const bool include_json=diamond_prelude_needs_json(source.data);
    const size_t prelude_length=diamond_prelude_length(include_json);
    char *combined=malloc(prelude_length+source.length+1);
    if(combined==nullptr) {
        free(source.data);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    const size_t offset=diamond_prelude_write(combined,include_json);
    memcpy(combined+offset,source.data,source.length+1);
    free(source.data);
    DiamondProgram *program=calloc(1,sizeof *program);
    if(program==nullptr) {
        free(combined);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    diamond_program_init(program);
    DiamondDiagnostic diagnostic;
    const bool compiled=diamond_compile(combined,program,&diagnostic);
    free(combined);
    if(!compiled) {
        snprintf(vm->error,sizeof vm->error,"compile_method: %.1007s",
            diagnostic.message);
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondChunk synthetic_chunk=diamond_program_chunk(program);
    const DiamondClass *synthetic_class=
        find_matching_synthetic_class(&synthetic_chunk,target_class,vm);
    if(synthetic_class==nullptr) {
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondMethod *synthetic_method=nullptr;
    for(size_t index=0;index<synthetic_class->method_count;index++)
        if(strlen(synthetic_class->methods[index].name)==name_string->length&&
           memcmp(synthetic_class->methods[index].name,name_string->chars,
                  name_string->length)==0)
            synthetic_method=&synthetic_class->methods[index];
    if(synthetic_method==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "compile_method: internal error locating compiled method '%.*s'",
            (int)name_string->length,name_string->chars);
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const uint16_t function_index=synthetic_method->function_index;
    /* Collected in the same order build_compiled_method_source walked
     * bound_values_hash->entries to emit their names as trailing
     * parameters -- see that function's own comment on why both must
     * agree. */
    DiamondValue bound_values[DIAMOND_MAX_BOUND_VALUES];
    const uint8_t bound_value_count=(uint8_t)bound_values_hash->count;
    for(uint8_t index=0;index<bound_value_count;index++)
        bound_values[index]=bound_values_hash->entries[index].value;
    DiamondAdoptedProgram *adopted=
        adopt_program(vm,program,bound_values,bound_value_count);
    if(adopted==nullptr) {
        diamond_program_free(program);
        free(program);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    DiamondClosure *closure=allocate_closure(vm,function_index,nullptr,0);
    if(closure==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    closure->foreign_chunk=&adopted->chunk;
    closure->intended_class=target_class;
    closure->bound_values=adopted->bound_values;
    closure->bound_value_count=bound_value_count;
    *out_result=DIAMOND_OBJECT(closure);
    return DIAMOND_VM_OK;
}

/* Whether `methods` already holds a method defined directly on the class or
 * module (not copied in by an earlier include) under `name`. An include skips
 * such names so a class's own method always beats an included module's, as in
 * the compiler's own `include` handling (has_own_method_named there). */
bool methods_have_own_named(const DiamondMethod *methods,size_t count,
        const char *name) {
    for(size_t index=0;index<count;index++)
        if(!methods[index].included&&strcmp(methods[index].name,name)==0)
            return true;
    return false;
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
DiamondVmStatus invoke_operator_method(DiamondVm *vm,
        const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondInstance *receiver,
        const char *name, size_t name_length, const DiamondValue *arguments,
        size_t explicit_argument_count, DiamondValue *result, bool *found) {
    /* No longer needed for the owner fallback below (vm->root_chunk
     * replaced it, see that field's own comment) -- kept as a parameter
     * so every call site doesn't need updating for what's otherwise an
     * internal implementation detail. */
    (void)chunk;
    const DiamondChunk *owner=receiver->owner!=nullptr?receiver->owner:vm->root_chunk;
    const DiamondMethod *method=lookup_method_cached(vm,owner,site,
        receiver->class,name,name_length,nullptr);
    if(method==nullptr) {*found=false;return DIAMOND_VM_OK;}
    *found=true;
    /* method->arity/required_arity are stored receiver-exclusive (see
     * compile_definition's `function->arity-1` when registering a class
     * method), matching how the real INVOKE site checks its own `argc`
     * (also receiver-exclusive) against them -- only the explicit
     * operands count here, not the receiver. */
    if(explicit_argument_count<method->required_arity||
       (explicit_argument_count>method->arity && !method->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    /* Sized for the receiver (1) + every operator this dispatches for
     * today (at most 2 explicit arguments, "[]="'s index+value) + a
     * ClassName.compile_method-installed method's own bound values. */
    DiamondValue args[1+2+DIAMOND_MAX_BOUND_VALUES]={DIAMOND_OBJECT((DiamondObject *)receiver)};
    for(size_t i=0;i<explicit_argument_count;i++)args[1+i]=arguments[i];
    size_t argument_count=1+explicit_argument_count;
    for(size_t i=0;i<method->bound_value_count;i++)
        args[argument_count+i]=method->bound_values[i];
    argument_count+=method->bound_value_count;
    /* See DIAMOND_OP_INVOKE_TYPED's own comment on function_chunk -- a
     * ClassName.compile_method-installed operator method's bytecode
     * lives in its own chunk, not `owner`. */
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
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
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    return run_chunk(&child,vm,args,argument_count,depth+1,nullptr,result);
}

/* JIT trampoline for DIAMOND_OP_EQUAL/NOT_EQUAL's general case -- Phase 2e
 * fix, extracted verbatim from that case's own real tail (src/vm.c's
 * EQUAL/NOT_EQUAL case, right after this file's own INT fast path, which
 * the JIT's own compile_equal_op already replicates directly in generated
 * code and never routes through here). The Phase 2d version of this
 * trampoline (diamond_jit_values_equal) called values_equal() directly for
 * every non-fast-path case, silently skipping the "==" override check
 * below for a DIAMOND_OBJECT_INSTANCE operand -- a real, shipped
 * correctness bug (a class defining `def ==` would get identity
 * comparison instead of its own override when compared via JIT'd EQUAL).
 * This version checks the override first, exactly like the interpreter's
 * own case does, and only falls back to values_equal when no override is
 * found -- matching that case's own comment ("two instances of a class
 * with no '==' compare by identity exactly as before this feature
 * existed"). Unlike the old version, this can genuinely invoke arbitrary
 * interpreted code (the override method), so it's status-bearing, not
 * infallible -- compile_equal_op sets jc->has_called = true accordingly. */
DiamondVmStatus diamond_jit_equal_general(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, bool negate, DiamondValue *out) {
    if (left->kind == DIAMOND_VALUE_OBJECT &&
        left->as.object->kind == DIAMOND_OBJECT_INSTANCE) {
        bool found = false;
        DiamondValue op_result = DIAMOND_NIL;
        const DiamondVmStatus status = invoke_operator_method(vm, chunk, depth, site,
            (const DiamondInstance *)left->as.object, "==", 2, right, 1, &op_result, &found);
        if (found) {
            if (status != DIAMOND_VM_OK) return status;
            const bool overloaded_equal = is_truthy(op_result);
            *out = DIAMOND_BOOL(negate ? !overloaded_equal : overloaded_equal);
            return DIAMOND_VM_OK;
        }
    }
    const bool equal = values_equal(*left, *right);
    *out = DIAMOND_BOOL(negate ? !equal : equal);
    return DIAMOND_VM_OK;
}

static bool case_numeric_compare(DiamondValue left,DiamondValue right,int *comparison) {
    if(is_int_value(left)&&is_int_value(right)) {
        DiamondIntView left_view,right_view;
        diamond_int_view(left,&left_view);diamond_int_view(right,&right_view);
        *comparison=diamond_bignum_compare(left_view,right_view);return true;
    }
    return false;
}

DiamondVmStatus case_match_value(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const uint8_t *site,DiamondValue pattern,DiamondValue subject,
        bool *matched) {
    *matched=false;
    if(pattern.kind==DIAMOND_VALUE_CLASS) {
        if(pattern.as.class_index>=chunk->class_count||
           subject.kind!=DIAMOND_VALUE_OBJECT||
           subject.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return DIAMOND_VM_OK;
        const DiamondInstance *instance=(const DiamondInstance *)subject.as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(owner==nullptr||owner->classes!=chunk->classes)return DIAMOND_VM_OK;
        const DiamondClass *target=chunk->classes[pattern.as.class_index];
        const DiamondClass *current=instance->class;
        while(current!=nullptr) {
            if(current==target) {*matched=true;break;}
            current=current->superclass==DIAMOND_NO_CLASS?nullptr:
                owner->classes[current->superclass];
        }
        return DIAMOND_VM_OK;
    }
    if(pattern.kind==DIAMOND_VALUE_OBJECT&&
       pattern.as.object->kind==DIAMOND_OBJECT_REGEXP) {
        if(subject.kind!=DIAMOND_VALUE_OBJECT||
           subject.as.object->kind!=DIAMOND_OBJECT_STRING)return DIAMOND_VM_OK;
        const reginold_status status=reginold_search(
            ((const DiamondRegexp *)pattern.as.object)->handle,
            ((const DiamondString *)subject.as.object)->chars,
            ((const DiamondString *)subject.as.object)->length,0,nullptr);
        if(status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        *matched=status==REGINOLD_OK;return DIAMOND_VM_OK;
    }
    if(vm->range_class_index!=DIAMOND_NO_CLASS&&pattern.kind==DIAMOND_VALUE_OBJECT&&
       pattern.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *range=(const DiamondInstance *)pattern.as.object;
        if(range->class==chunk->classes[vm->range_class_index]&&range->field_count>=3) {
            int from_start=0,from_end=0;
            if(range->fields[1].kind==DIAMOND_VALUE_NIL) {
                /* Endless: `when 5..` matches anything from 5 up. */
                if(!case_numeric_compare(subject,range->fields[0],&from_start))return DIAMOND_VM_OK;
                *matched=from_start>=0;return DIAMOND_VM_OK;
            }
            if(!case_numeric_compare(subject,range->fields[0],&from_start)||
               !case_numeric_compare(subject,range->fields[1],&from_end))return DIAMOND_VM_OK;
            const bool exclusive=range->fields[2].kind==DIAMOND_VALUE_BOOL&&
                range->fields[2].as.boolean;
            *matched=from_start>=0&&(exclusive?from_end<0:from_end<=0);
            return DIAMOND_VM_OK;
        }
    }
    if(pattern.kind==DIAMOND_VALUE_OBJECT&&
       pattern.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        bool found=false;DiamondValue result=DIAMOND_NIL;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)pattern.as.object,"==",2,&subject,1,
            &result,&found);
        if(found) {if(status!=DIAMOND_VM_OK)return status;*matched=is_truthy(result);return status;}
    }
    *matched=values_equal(pattern,subject);return DIAMOND_VM_OK;
}

/* ClassName#method_missing(name, args) -- called when ordinary instance
 * method dispatch (the main DIAMOND_OP_INVOKE-family site) fails to find
 * `attempted_name` on the receiver's own class. Mirrors
 * invoke_operator_method's own found-or-not shape directly (*found stays
 * false, *result untouched, status DIAMOND_VM_OK, when the class has no
 * method_missing of its own either) so the caller raises the default
 * NoMethodError instead -- there is no risk of this recursing into
 * itself: reaching this function at all already means lookup_method_cached
 * failed to find `attempted_name`, and the lookup_method call here is for
 * the fixed literal name "method_missing", a different lookup entirely.
 * `call_args` points into the caller's own live register file (the
 * original call's explicit arguments, receiver excluded) -- already part
 * of the current frame's GC roots via mark_frame_chain, same as every
 * other dispatch site's own args[] construction. name_symbol is not: it's
 * a brand-new allocation with no register or container to live in until
 * args[] is built below, so it's gc_protect'd across the allocate_array
 * call that follows it (confirmed as a real bug under
 * DIAMOND_STRESS_GC=1, not just theoretical -- same "root it before the
 * next allocation" lesson as populate_default_argv_env's own key_mark).
 *
 * Deliberately scoped to this one dispatch site (ordinary instance method
 * calls) for a first version -- not operator overloading
 * (invoke_operator_method), not #to_s (stringify_value), not super, not
 * self.-singleton dispatch. Each of those already has its own sensible
 * fallback (native operator semantics, a default object representation,
 * a real "no such superclass method" error, a separate method table) that
 * silently redirecting through method_missing would be more likely to
 * surprise than help; a class wanting custom behavior there defines the
 * specific method directly. See docs/design.md. */
DiamondVmStatus method_missing_helper(DiamondVm *vm,const DiamondChunk *owner,
        const DiamondInstance *receiver,const char *attempted_name,size_t attempted_length,
        const DiamondValue *call_args,size_t call_arg_count,size_t depth,
        DiamondValue *result,bool *found) {
    static const char missing_name[]="method_missing";
    const DiamondMethod *method=lookup_method(owner,receiver->class,
        missing_name,sizeof(missing_name)-1);
    if(method==nullptr) {*found=false;return DIAMOND_VM_OK;}
    *found=true;
    if(2<method->required_arity||2>method->arity)
        return DIAMOND_VM_ARITY_ERROR;
    DiamondSymbol *name_symbol=allocate_symbol(vm,attempted_name,attempted_length);
    if(name_symbol==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const size_t name_mark=vm->gc_protected_count;
    if(!gc_protect(vm,DIAMOND_OBJECT(name_symbol)))return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondArray *args_array=allocate_array(vm,call_args,call_arg_count);
    gc_unprotect(vm,name_mark);
    if(args_array==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondValue args[3]={DIAMOND_OBJECT((DiamondObject *)receiver),
        DIAMOND_OBJECT(name_symbol),DIAMOND_OBJECT(args_array)};
    /* See DIAMOND_OP_INVOKE_TYPED's own comment on function_chunk. */
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
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
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    return run_chunk(&child,vm,args,3,depth+1,nullptr,result);
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
/* Array - & | * and String * are written in the prelude: `left OP right`
 * with an Array or String on the left calls diamond_array_op_WORD(left,
 * right) or diamond_string_op_WORD(left, right) when that function exists. Returns false,
 * with nothing called, when it doesn't. */
bool operator_prelude_fallback(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,DiamondValue left,DiamondValue right,const char *word,
        DiamondValue *out,DiamondVmStatus *status) {
    if(left.kind!=DIAMOND_VALUE_OBJECT)return false;
    const char *prefix=left.as.object->kind==DIAMOND_OBJECT_ARRAY?"diamond_array_op_":
        left.as.object->kind==DIAMOND_OBJECT_STRING?"diamond_string_op_":nullptr;
    if(prefix==nullptr)return false;
    char bridge[DIAMOND_MAX_FUNCTION_NAME];
    const int written=snprintf(bridge,sizeof bridge,"%s%s",prefix,word);
    if(written<=0||(size_t)written>=sizeof bridge)return false;
    const DiamondFunction *target=find_top_level_function(chunk,bridge,(size_t)written);
    if(target==nullptr)return false;
    const DiamondValue arguments[2]={left,right};
    *status=forward_to_top_level_helper(vm,chunk,target,arguments,0,1,1,depth,out);
    return true;
}

DiamondVmStatus add_fallback(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
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
    /* Array + Array: a new Array with left's elements, then right's.
     * Writes both halves straight into the new array's own storage with
     * allocate_array_uninitialized (see its own comment) instead of
     * building a throwaway buffer and letting allocate_array copy that a
     * second time -- this used to copy every element of both arrays
     * twice. */
    if (left_value.kind==DIAMOND_VALUE_OBJECT && right_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_ARRAY &&
        right_value.as.object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *left_array=(const DiamondArray *)left_value.as.object;
        const DiamondArray *right_array=(const DiamondArray *)right_value.as.object;
        const size_t count=left_array->count+right_array->count;
        DiamondArray *joined=allocate_array_uninitialized(vm,count);
        if(joined==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(left_array->count>0)
            memcpy(joined->values,left_array->values,
                   left_array->count*sizeof *joined->values);
        if(right_array->count>0)
            memcpy(joined->values+left_array->count,right_array->values,
                   right_array->count*sizeof *joined->values);
        *out_result=DIAMOND_OBJECT(joined);
        return DIAMOND_VM_OK;
    }
    if (left_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        bool found=false;DiamondValue op_result=DIAMOND_NIL;
        const uint8_t *site=chunk->code+instruction_offset;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)left_value.as.object,"+",1,&right_value,1,
            &op_result,&found);
        if(found) {
            if(status!=DIAMOND_VM_OK)return status;
            *out_result=op_result;
            return DIAMOND_VM_OK;
        }
    }
    /* Time + Int|Float -> Time (offset forward, same timezone mode as the
     * receiver); Time + Time is a TypeError, matching Ruby -- there's no
     * branch here for a Time right operand, so it simply falls through
     * to the type-error return below. */
    if (left_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_TIME &&
        (right_value.kind==DIAMOND_VALUE_INT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        const DiamondTime *left_time=(const DiamondTime *)left_value.as.object;
        const double offset=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        DiamondTime *result=allocate_time(vm,left_time->epoch+offset,
            left_time->zone_mode,left_time->utc_offset);
        if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(result);
        return DIAMOND_VM_OK;
    }
    format_operator_type_error(vm,left_value,right_value,"+");
    return DIAMOND_VM_TYPE_ERROR;
}

/* Time vs Time only, matching Ruby -- no Time-vs-numeric ordering.
 * Compares epoch, ignoring each display timezone. Same stack-frame-budget reasoning
 * as time_subtract_fallback above for why this is its own function
 * rather than inline in run_chunk's LESS/LESS_EQUAL/GREATER/GREATER_
 * EQUAL case block, and same reason it writes a ready-to-store
 * DiamondValue into *out_result rather than a bare bool -- the call site
 * needs zero locals of its own this way, just an `if` around the call. */
/* Plain byte-lexicographic ordering (memcmp on the shared prefix, then
 * shorter-is-less on a tie) -- the same ordering C's own strcmp/memcmp
 * and Ruby's String#<=> use. Diamond's String is a raw byte buffer,
 * not UTF-8-validated (see e.g. examples/transformer/lib/tokenizer.d6's
 * own comment on why that property gets relied on elsewhere), so this
 * is a byte comparison, not a locale-aware or codepoint-aware one --
 * matches every other place in this codebase that already treats
 * String as bytes rather than text. */
int diamond_string_compare(const DiamondString *a,const DiamondString *b) {
    const size_t min_length=a->length<b->length?a->length:b->length;
    const int result=min_length==0?0:memcmp(a->chars,b->chars,min_length);
    if(result!=0)return result;
    if(a->length<b->length)return -1;
    if(a->length>b->length)return 1;
    return 0;
}

/* Phase 3 (docs/internal/jit-design.md). Shared slow path for ADD_INT/
 * SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT and their generic (non-_INT)
 * counterparts -- covers everything the fast, purely-native path in both
 * run_chunk's own case and the JIT (src/jit.c's compile_binary_int_op)
 * can't handle inline: a non-plain-int operand (deopts an _INT opcode
 * back to its generic form first, exactly like this always did, then
 * dispatches through the same fallback logic every generic ADD/SUBTRACT/
 * MULTIPLY/DIVIDE already used) and genuine Int-Int overflow or DIVIDE's
 * own zero/INT64_MIN edge cases, which promote to Diamond's real bignum
 * representation rather than erroring. `opcode` may be any of the seven
 * opcodes this covers (generic or _INT); only ever called once the
 * caller has already ruled out the fast, non-overflowing, both-plain-Int
 * path. Reuses add_fallback for ADD's own generic dispatch (String
 * concat, Instance `+`, Time offset) rather than duplicating it -- the
 * same anti-duplication discipline established in Phase 2e after the
 * EQUAL/INDEX_GET trampolines shipped with their own, subtly incomplete
 * copies of logic that already lived elsewhere. */
DiamondVmStatus int_arith_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, size_t instruction_offset, DiamondOpCode opcode,
        DiamondValue left_value, DiamondValue right_value, DiamondValue *out_result) {
    const bool kind_mismatch=left_value.kind!=DIAMOND_VALUE_INT||
        right_value.kind!=DIAMOND_VALUE_INT;
    DiamondOpCode generic=opcode;
    if(opcode==DIAMOND_OP_ADD_INT)generic=DIAMOND_OP_ADD;
    else if(opcode==DIAMOND_OP_SUBTRACT_INT)generic=DIAMOND_OP_SUBTRACT;
    else if(opcode==DIAMOND_OP_MULTIPLY_INT)generic=DIAMOND_OP_MULTIPLY;
    else if(opcode==DIAMOND_OP_DIVIDE_INT)generic=DIAMOND_OP_DIVIDE;
    if(kind_mismatch&&generic!=opcode) {
        uint8_t *code=(uint8_t *)(void *)chunk->code;
        code[instruction_offset]=(uint8_t)generic;
        vm->deoptimized_sites++;
    }
    if(kind_mismatch) {
        if(generic==DIAMOND_OP_ADD)
            return add_fallback(vm,chunk,depth,instruction_offset,left_value,right_value,out_result);
        if(is_int_value(left_value)&&is_int_value(right_value)&&
           (value_is_bignum(left_value)||value_is_bignum(right_value))) {
            DiamondIntView left_view, right_view;
            diamond_int_view(left_value,&left_view);
            diamond_int_view(right_value,&right_view);
            DiamondValue bignum_result;
            if(generic==DIAMOND_OP_SUBTRACT)
                bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
            else if(generic==DIAMOND_OP_MULTIPLY)
                bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
            else {
                DiamondIntView zero_view;
                diamond_int_view_int64(0,&zero_view);
                if(diamond_bignum_compare(right_view,zero_view)==0)
                    return DIAMOND_VM_DIVISION_BY_ZERO;
                bignum_result=diamond_bignum_divide_truncated(vm,left_view,right_view);
            }
            if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
            *out_result=bignum_result;
            return DIAMOND_VM_OK;
        }
        if((left_value.kind==DIAMOND_VALUE_FLOAT||left_value.kind==DIAMOND_VALUE_INT)&&
           (right_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_INT)&&
           (left_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
            const double left_real=left_value.kind==DIAMOND_VALUE_FLOAT?
                left_value.as.real:(double)left_value.as.integer;
            const double right_real=right_value.kind==DIAMOND_VALUE_FLOAT?
                right_value.as.real:(double)right_value.as.integer;
            double float_result=0;
            if(generic==DIAMOND_OP_SUBTRACT)float_result=left_real-right_real;
            else if(generic==DIAMOND_OP_MULTIPLY)float_result=left_real*right_real;
            else float_result=left_real/right_real;
            *out_result=DIAMOND_FLOAT(float_result);
            return DIAMOND_VM_OK;
        }
        const char *name=generic==DIAMOND_OP_SUBTRACT?"-":
            generic==DIAMOND_OP_MULTIPLY?"*":"/";
        if(left_value.kind==DIAMOND_VALUE_OBJECT&&
           left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
            bool found=false;DiamondValue op_result=DIAMOND_NIL;
            const uint8_t *site=chunk->code+instruction_offset;
            const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
                (const DiamondInstance *)left_value.as.object,name,strlen(name),
                &right_value,1,&op_result,&found);
            if(found) {
                if(status!=DIAMOND_VM_OK)return status;
                *out_result=op_result;
                return DIAMOND_VM_OK;
            }
        }
        if(generic==DIAMOND_OP_SUBTRACT) {
            const DiamondVmStatus time_status=
                time_subtract_fallback(vm,left_value,right_value,out_result);
            if(time_status!=DIAMOND_VM_TYPE_ERROR)return time_status;
        }
        if(generic==DIAMOND_OP_SUBTRACT||generic==DIAMOND_OP_MULTIPLY) {
            DiamondVmStatus prelude_status=DIAMOND_VM_OK;
            if(operator_prelude_fallback(vm,chunk,depth,left_value,right_value,
                   generic==DIAMOND_OP_SUBTRACT?"minus":"times",out_result,&prelude_status))
                return prelude_status;
        }
        format_operator_type_error(vm,left_value,right_value,name);
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* Both operands confirmed plain (non-bignum) Int -- reached only for
     * a genuine overflow (ADD/SUBTRACT/MULTIPLY) or DIVIDE's own zero/
     * INT64_MIN edge cases; the caller already ruled out the fast,
     * non-overflowing native path before calling here. */
    const int64_t left_int=left_value.as.integer;
    const int64_t right_int=right_value.as.integer;
    if(generic==DIAMOND_OP_DIVIDE) {
        if(right_int==0)return DIAMOND_VM_DIVISION_BY_ZERO;
        if(left_int==INT64_MIN&&right_int==-1) {
            /* -INT64_MIN doesn't fit int64_t; promote instead of
             * erroring, matching every other overflow site here. */
            DiamondIntView left_view;
            diamond_int_view(left_value,&left_view);
            const DiamondValue bignum_result=diamond_bignum_negate(vm,left_view);
            if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
            *out_result=bignum_result;
            return DIAMOND_VM_OK;
        }
        *out_result=DIAMOND_INT(left_int/right_int);
        return DIAMOND_VM_OK;
    }
    DiamondIntView left_view, right_view;
    diamond_int_view(left_value,&left_view);
    diamond_int_view(right_value,&right_view);
    DiamondValue bignum_result;
    if(generic==DIAMOND_OP_ADD)bignum_result=diamond_bignum_add(vm,left_view,right_view);
    else if(generic==DIAMOND_OP_SUBTRACT)bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
    else bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
    if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=bignum_result;
    return DIAMOND_VM_OK;
}

/* Phase 3. Shared slow path for LESS/LESS_EQUAL/GREATER/GREATER_EQUAL and
 * their _INT forms -- mirrors int_arith_slow's own shape exactly, except
 * comparisons never overflow, so the only reason this is ever called at
 * all is a non-plain-int operand (which, for an _INT form, always deopts
 * the opcode back to its generic counterpart first, matching this
 * always did before this extraction). */
DiamondVmStatus compare_int_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, size_t instruction_offset, DiamondOpCode opcode,
        DiamondValue left_value, DiamondValue right_value, DiamondValue *out_result) {
    DiamondOpCode generic=opcode;
    if(opcode==DIAMOND_OP_LESS_INT)generic=DIAMOND_OP_LESS;
    else if(opcode==DIAMOND_OP_LESS_EQUAL_INT)generic=DIAMOND_OP_LESS_EQUAL;
    else if(opcode==DIAMOND_OP_GREATER_INT)generic=DIAMOND_OP_GREATER;
    else if(opcode==DIAMOND_OP_GREATER_EQUAL_INT)generic=DIAMOND_OP_GREATER_EQUAL;
    if(generic!=opcode) {
        uint8_t *code=(uint8_t *)(void *)chunk->code;
        code[instruction_offset]=(uint8_t)generic;
        vm->deoptimized_sites++;
    }
    if(is_int_value(left_value)&&is_int_value(right_value)&&
       (value_is_bignum(left_value)||value_is_bignum(right_value))) {
        DiamondIntView left_view, right_view;
        diamond_int_view(left_value,&left_view);
        diamond_int_view(right_value,&right_view);
        const int comparison=diamond_bignum_compare(left_view,right_view);
        bool bignum_comparison=false;
        if(generic==DIAMOND_OP_LESS)bignum_comparison=comparison<0;
        else if(generic==DIAMOND_OP_LESS_EQUAL)bignum_comparison=comparison<=0;
        else if(generic==DIAMOND_OP_GREATER)bignum_comparison=comparison>0;
        else bignum_comparison=comparison>=0;
        *out_result=DIAMOND_BOOL(bignum_comparison);
        return DIAMOND_VM_OK;
    }
    if((left_value.kind==DIAMOND_VALUE_FLOAT||left_value.kind==DIAMOND_VALUE_INT)&&
       (right_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_INT)&&
       (left_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        const double left_real=left_value.kind==DIAMOND_VALUE_FLOAT?
            left_value.as.real:(double)left_value.as.integer;
        const double right_real=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        bool float_comparison=false;
        if(generic==DIAMOND_OP_LESS)float_comparison=left_real<right_real;
        else if(generic==DIAMOND_OP_LESS_EQUAL)float_comparison=left_real<=right_real;
        else if(generic==DIAMOND_OP_GREATER)float_comparison=left_real>right_real;
        else float_comparison=left_real>=right_real;
        *out_result=DIAMOND_BOOL(float_comparison);
        return DIAMOND_VM_OK;
    }
    if(left_value.kind==DIAMOND_VALUE_OBJECT&&left_value.as.object->kind==DIAMOND_OBJECT_STRING&&
       right_value.kind==DIAMOND_VALUE_OBJECT&&right_value.as.object->kind==DIAMOND_OBJECT_STRING) {
        const int comparison=diamond_string_compare(
            (const DiamondString *)left_value.as.object,(const DiamondString *)right_value.as.object);
        bool string_comparison=false;
        if(generic==DIAMOND_OP_LESS)string_comparison=comparison<0;
        else if(generic==DIAMOND_OP_LESS_EQUAL)string_comparison=comparison<=0;
        else if(generic==DIAMOND_OP_GREATER)string_comparison=comparison>0;
        else string_comparison=comparison>=0;
        *out_result=DIAMOND_BOOL(string_comparison);
        return DIAMOND_VM_OK;
    }
    if(left_value.kind==DIAMOND_VALUE_OBJECT&&left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const char *name=generic==DIAMOND_OP_LESS?"<":
            generic==DIAMOND_OP_LESS_EQUAL?"<=":
            generic==DIAMOND_OP_GREATER?">":">=";
        bool found=false;DiamondValue op_result=DIAMOND_NIL;
        const uint8_t *site=chunk->code+instruction_offset;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)left_value.as.object,name,strlen(name),
            &right_value,1,&op_result,&found);
        if(found) {
            if(status!=DIAMOND_VM_OK)return status;
            *out_result=DIAMOND_BOOL(is_truthy(op_result));
            return DIAMOND_VM_OK;
        }
    }
    if(time_comparison_fallback(left_value,right_value,generic,out_result))
        return DIAMOND_VM_OK;
    {
        const char *name=generic==DIAMOND_OP_LESS?"<":
            generic==DIAMOND_OP_LESS_EQUAL?"<=":
            generic==DIAMOND_OP_GREATER?">":">=";
        format_operator_type_error(vm,left_value,right_value,name);
    }
    return DIAMOND_VM_TYPE_ERROR;
}

/* Phase 3. JIT trampolines wrapping int_arith_slow/compare_int_slow above
 * for compile_binary_int_op (src/jit.c) -- see that function's own
 * comment for why compiling ADD_INT/SUBTRACT_INT/MULTIPLY_INT/
 * DIVIDE_INT/LESS_INT no longer needs jc->has_called to already be false.
 * `site` is this occurrence's own bytecode address (jc->function->code +
 * instruction_start), the same per-occurrence convention every other
 * overload-checking trampoline here already uses; converted back to an
 * offset via pointer subtraction since the shared helpers key off
 * chunk->code + instruction_offset the same way run_chunk's own case
 * does. */
DiamondVmStatus diamond_jit_arith_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, DiamondOpCode opcode, DiamondValue *out) {
    return int_arith_slow(vm,chunk,depth,(size_t)(site-chunk->code),opcode,*left,*right,out);
}
DiamondVmStatus diamond_jit_compare_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, DiamondOpCode opcode, DiamondValue *out) {
    return compare_int_slow(vm,chunk,depth,(size_t)(site-chunk->code),opcode,*left,*right,out);
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


bool array_value_satisfies_constraints(DiamondArray *array,
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

/* Recognizes an INDEX_GET/SET operand that's a Range instance (lib/
 * core.d6, fields @start/@end/@exclusive at slots 0/1/2 in declaration
 * order -- confirmed directly against field_index's own create-on-
 * first-miss slot assignment) rather than a plain Int -- there's no
 * native VM value kind for Range at all, it's an ordinary user-space
 * class, so this is the one place that needs to reach into a prelude-
 * defined class's own instance fields.
 *
 * Reads the resolved class index from `vm->range_class_index`, not
 * `chunk->range_class_index` -- confirmed directly (a real bug caught
 * before shipping) that a nested function/closure call constructs its
 * *own* fresh DiamondChunk view at the call site (many places in this
 * file), none of which propagate a program-wide field like this one, so
 * reading it off whatever chunk happens to be ambient at a given opcode
 * would silently stop working inside any nested def. DiamondVm doesn't
 * have that problem: it's seeded once, at the true top of execution
 * (diamond_vm_run, and separately at every place that starts a VM
 * without going through it -- ProgramBuilder#run's own child VM goes
 * through diamond_vm_run so needs nothing extra; a spawned Thread's
 * trampoline calls run_chunk directly and seeds it there instead -- see
 * DiamondVm's own comment on this field). `chunk->classes` itself is
 * still used for the actual pointer-equality target, since the
 * underlying classes[] array is the same memory across every such view
 * in the common (single-program) case -- only *which slot* to look at
 * needed moving, not the class table itself.
 *
 * Returns -1 if `index_value` isn't recognizably a Range for this VM
 * (including an instance whose *own* class actually lives in a
 * different, ProgramBuilder-adopted chunk than `chunk` itself -- a
 * deliberate scope cut, not a crash) -- caller falls through to its
 * existing plain-Int handling, unchanged. Returns 0 if it is a Range
 * but its own @start is out of bounds for `array_count` (vm->error
 * already set, matching this opcode's own existing bounds-error
 * message/format) -- caller should VM_RETURN(DIAMOND_VM_INDEX_ERROR)
 * immediately. Returns 1 with `*out_start`/`*out_length` set
 * otherwise -- length already clamped to whatever's actually
 * available past `start`, matching String#slice's own established
 * convention (docs/design.md): an over-long range is not an error,
 * only an out-of-bounds start is. */
static int resolve_array_range(DiamondVm *vm,const DiamondChunk *chunk,
        const char *container,DiamondValue index_value,size_t array_count,
        size_t *out_start,size_t *out_length) {
    if(vm->range_class_index==DIAMOND_NO_CLASS)return -1;
    if(index_value.kind!=DIAMOND_VALUE_OBJECT||
       index_value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)
        return -1;
    const DiamondInstance *instance=(const DiamondInstance *)index_value.as.object;
    if(instance->class!=chunk->classes[vm->range_class_index])return -1;
    int64_t start=instance->fields[0].as.integer;
    /* An endless range (`a[1..]`) has a nil end: it runs to the last element. */
    const bool endless=instance->fields[1].kind!=DIAMOND_VALUE_INT;
    int64_t end=endless?-1:instance->fields[1].as.integer;
    const bool exclusive=!endless&&instance->fields[2].as.boolean;
    /* A negative bound counts from the end, as in Ruby: a[1..-1] is all but the first. */
    const int64_t original_start=start;
    if(start<0&&array_count<=(uint64_t)INT64_MAX)start+=(int64_t)array_count;
    if(end<0&&array_count<=(uint64_t)INT64_MAX)end+=(int64_t)array_count;
    if(start<0||(uint64_t)start>array_count) {
        snprintf(vm->error,sizeof vm->error,
                 "index %" PRId64 " out of bounds for %s of length %zu",
                 original_start,container,array_count);
        return 0;
    }
    const size_t available=array_count-(size_t)start;
    /* Subtract only ordered, nonnegative bounds, then clamp before narrowing
     * to size_t. Avoid both INT64_MIN-1 and INT64_MAX+1. */
    const uint64_t requested=end<start?0:
        (uint64_t)end-(uint64_t)start+(exclusive?0u:1u);
    *out_start=(size_t)start;
    *out_length=requested<available?(size_t)requested:available;
    return 1;
}

bool array_push(DiamondVm *vm,DiamondArray *array,DiamondValue value) {
    if(array->count==array->capacity) {
        if(array->capacity>SIZE_MAX/2/sizeof(DiamondValue))return false;
        const size_t old_capacity=array->capacity;
        const size_t capacity=old_capacity<8?8:old_capacity*2;
        DiamondValue *values=realloc(array->values,capacity*sizeof(DiamondValue));
        if(values==nullptr)return false;
        array->values=values;array->capacity=capacity;
        vm->bytes_allocated+=(capacity-old_capacity)*sizeof(DiamondValue);
        note_storage_growth(vm);
    }
    const size_t new_index=array->count;
    array->values[array->count++]=value;
    return gc_write_barrier_index(vm,(DiamondObject *)array,new_index);
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

/* JIT trampoline for DIAMOND_OP_INDEX_GET -- Phase 2e, extracted verbatim
 * from that case's own real body (below) so the interpreter and the JIT
 * share one implementation. Replaces Phase 2b's diamond_jit_hash_get,
 * which only ever handled a Hash receiver, returning DIAMOND_VM_TYPE_ERROR
 * for everything else -- accidentally safe before Phase 2d (every bailout
 * retried via full interpretation, which correctly checks the `[]`
 * override below), but a real latent correctness bug once a function's
 * bailout target can be "propagate" (jc->has_called already true from an
 * earlier call): a JIT'd INDEX_GET on an Instance with a real `[]`
 * override, reached after such a call, would have incorrectly propagated
 * TYPE_ERROR instead of invoking the override. This version handles
 * Hash/String/Array/Instance-overload exactly like the real opcode, and
 * -- because the Instance branch can genuinely invoke arbitrary code --
 * is compiled with jc->has_called = true unconditionally, the same
 * conservative, compile-time-only choice diamond_jit_equal_general's own
 * compile_equal_op already makes. */
DiamondVmStatus diamond_jit_index_get(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        const DiamondValue *index, DiamondValue *out) {
    if (receiver->kind != DIAMOND_VALUE_OBJECT) {
        char actual[80];
        diamond_format_value_type(actual, sizeof actual, *receiver);
        snprintf(vm->error, sizeof vm->error, "undefined method '[]' for %s", actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if (receiver->as.object->kind == DIAMOND_OBJECT_HASH) {
        DiamondHash *hash = (DiamondHash *)receiver->as.object;
        const ptrdiff_t found = hash_find(hash, *index);
        *out = found < 0 ? DIAMOND_NIL : hash->entries[(size_t)found].value;
        return DIAMOND_VM_OK;
    }
    if (receiver->as.object->kind == DIAMOND_OBJECT_STRING) {
        {
            const DiamondString *text = (const DiamondString *)receiver->as.object;
            size_t slice_start = 0, slice_length = 0;
            const int slice_result = resolve_array_range(vm, chunk, "String", *index, text->length,
                &slice_start, &slice_length);
            if (slice_result == 0) return DIAMOND_VM_INDEX_ERROR;
            if (slice_result == 1) {
                DiamondString *piece = allocate_string(vm, text->chars + slice_start, slice_length);
                if (piece == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
                *out = DIAMOND_OBJECT(piece);
                return DIAMOND_VM_OK;
            }
        }
        if (index->kind != DIAMOND_VALUE_INT) {
            char actual[80];
            diamond_format_value_type(actual, sizeof actual, *index);
            snprintf(vm->error, sizeof vm->error, "String#[] index must be an Int or Range, got %s", actual);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *source = (const DiamondString *)receiver->as.object;
        /* A negative index counts from the end: s[-1] is the last byte. */
        int64_t char_index = index->as.integer;
        if (char_index < 0) char_index += (int64_t)source->length;
        if (char_index < 0 || (uint64_t)char_index >= source->length) {
            snprintf(vm->error, sizeof vm->error,
                     "index %" PRId64 " out of bounds for String of length %zu",
                     index->as.integer, source->length);
            return DIAMOND_VM_INDEX_ERROR;
        }
        DiamondString *character = allocate_string(vm, source->chars + (size_t)char_index, 1);
        if (character == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
        *out = DIAMOND_OBJECT(character);
        return DIAMOND_VM_OK;
    }
    if (receiver->as.object->kind == DIAMOND_OBJECT_INSTANCE) {
        bool found = false;
        DiamondValue op_result = DIAMOND_NIL;
        const DiamondVmStatus status = invoke_operator_method(vm, chunk, depth, site,
            (const DiamondInstance *)receiver->as.object, "[]", 2, index, 1, &op_result, &found);
        if (found) {
            if (status != DIAMOND_VM_OK) return status;
            *out = op_result;
            return DIAMOND_VM_OK;
        }
    }
    if (receiver->as.object->kind != DIAMOND_OBJECT_ARRAY) {
        char actual[80];
        diamond_format_value_type(actual, sizeof actual, *receiver);
        snprintf(vm->error, sizeof vm->error, "undefined method '[]' for %s", actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondArray *array = (DiamondArray *)receiver->as.object;
    size_t range_start = 0, range_length = 0;
    const int range_result = resolve_array_range(vm, chunk, "Array", *index, array->count,
        &range_start, &range_length);
    if (range_result == 0) return DIAMOND_VM_INDEX_ERROR;
    if (range_result == 1) {
        DiamondArray *sliced = allocate_array(vm, &array->values[range_start], range_length);
        if (sliced == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
        *out = DIAMOND_OBJECT(sliced);
        return DIAMOND_VM_OK;
    }
    if (index->kind != DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual, sizeof actual, *index);
        snprintf(vm->error, sizeof vm->error, "Array#[] index must be an Int or Range, got %s", actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* A negative index counts from the end: xs[-1] is the last element. */
    int64_t array_index = index->as.integer;
    if (array_index < 0) array_index += (int64_t)array->count;
    if (array_index < 0 || (uint64_t)array_index >= array->count) {
        snprintf(vm->error, sizeof vm->error,
                 "index %" PRId64 " out of bounds for Array of length %zu",
                 index->as.integer, array->count);
        return DIAMOND_VM_INDEX_ERROR;
    }
    *out = array->values[(size_t)array_index];
    return DIAMOND_VM_OK;
}

/* DIAMOND_OP_INDEX_GET_STRING (see its comment in vm.h; shared by the interpreter and the JIT).
 * A Hash receiver is by far the common case and is searched by the constant's bytes. Any
 * other receiver gets the key String INDEX_GET would have been handed; that String is
 * gc_protect'ed across diamond_jit_index_get, whose Instance `[]` path can allocate and
 * collect before the argument is copied anywhere the collector scans. */
DiamondVmStatus diamond_jit_index_get_string(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        uint16_t string_index, DiamondValue *out) {
    if (string_index >= chunk->string_count) return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondStringConstant *constant = &chunk->strings[string_index];
    if (receiver->kind == DIAMOND_VALUE_OBJECT &&
        receiver->as.object->kind == DIAMOND_OBJECT_HASH) {
        const DiamondHash *hash = (const DiamondHash *)receiver->as.object;
        const ptrdiff_t found = hash_find_string_bytes(hash, constant->chars, constant->length);
        *out = found < 0 ? DIAMOND_NIL : hash->entries[(size_t)found].value;
        return DIAMOND_VM_OK;
    }
    DiamondString *key = allocate_string(vm, constant->chars, constant->length);
    if (key == nullptr) return DIAMOND_VM_OUT_OF_MEMORY;
    const DiamondValue index = DIAMOND_OBJECT(key);
    const size_t mark = vm->gc_protected_count;
    if (!gc_protect(vm, index)) return DIAMOND_VM_OUT_OF_MEMORY;
    const DiamondVmStatus status = diamond_jit_index_get(vm, chunk, depth, site, receiver, &index, out);
    gc_unprotect(vm, mark);
    return status;
}

/* JIT trampoline for DIAMOND_OP_INDEX_SET -- Phase 2e, extracted verbatim
 * from that case's own real body (below), new in this phase (INDEX_SET
 * was entirely unsupported before). Handles Hash/String/Instance-
 * overload/Array exactly like the real opcode; the Instance branch can
 * genuinely invoke arbitrary code, so this is compiled with jc->has_called
 * = true unconditionally, same as diamond_jit_index_get/diamond_jit_
 * equal_general. No `out` parameter -- INDEX_SET never writes a
 * destination register (see the real case's own comment: `x[i] = v`
 * already evaluates to `v` itself, computed before this opcode runs). */
DiamondVmStatus diamond_jit_index_set(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *receiver,
        const DiamondValue *index, const DiamondValue *source) {
    if (receiver->kind != DIAMOND_VALUE_OBJECT) {
        char actual[80];
        diamond_format_value_type(actual, sizeof actual, *receiver);
        snprintf(vm->error, sizeof vm->error, "undefined method '[]=' for %s", actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if (receiver->as.object->kind == DIAMOND_OBJECT_HASH) {
        DiamondHash *hash = (DiamondHash *)receiver->as.object;
        if (hash->object.frozen) return DIAMOND_VM_FROZEN_ERROR;
        if (!hash_entry_satisfies_constraints(hash, *index, *source)) {
            snprintf(vm->error, sizeof vm->error, "hash entry violates its type annotation");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if (!hash_set(vm, hash, *index, *source)) return DIAMOND_VM_OUT_OF_MEMORY;
        return DIAMOND_VM_OK;
    }
    if (receiver->as.object->kind == DIAMOND_OBJECT_STRING) {
        snprintf(vm->error, sizeof vm->error, "String does not support element assignment");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if (receiver->as.object->kind == DIAMOND_OBJECT_INSTANCE) {
        bool found = false;
        DiamondValue op_result = DIAMOND_NIL;
        const DiamondValue setter_arguments[2] = {*index, *source};
        const DiamondVmStatus status = invoke_operator_method(vm, chunk, depth, site,
            (const DiamondInstance *)receiver->as.object, "[]=", 3, setter_arguments, 2,
            &op_result, &found);
        if (found) return status;
    }
    if (receiver->as.object->kind != DIAMOND_OBJECT_ARRAY) {
        char actual[80];
        diamond_format_value_type(actual, sizeof actual, *receiver);
        snprintf(vm->error, sizeof vm->error, "undefined method '[]=' for %s", actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondArray *array = (DiamondArray *)receiver->as.object;
    if (array->object.frozen) return DIAMOND_VM_FROZEN_ERROR;
    size_t range_start = 0, range_length = 0;
    const int range_result = resolve_array_range(vm, chunk, "Array", *index, array->count,
        &range_start, &range_length);
    if (range_result == 0) return DIAMOND_VM_INDEX_ERROR;
    if (range_result == 1) {
        if (source->kind != DIAMOND_VALUE_OBJECT ||
            source->as.object->kind != DIAMOND_OBJECT_ARRAY ||
            ((DiamondArray *)source->as.object)->count != range_length) {
            snprintf(vm->error, sizeof vm->error,
                     "range assignment requires a replacement Array of exactly %zu element(s)",
                     range_length);
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondArray *replacement = (const DiamondArray *)source->as.object;
        for (size_t i = 0; i < range_length; i++) {
            if (!array_value_satisfies_constraints(array, replacement->values[i])) {
                snprintf(vm->error, sizeof vm->error, "array element violates its type annotation");
                return DIAMOND_VM_TYPE_ERROR;
            }
        }
        for (size_t i = 0; i < range_length; i++)
            array->values[range_start + i] = replacement->values[i];
        if (!gc_write_barrier_range(vm, (DiamondObject *)array, range_start, range_length))
            return DIAMOND_VM_OUT_OF_MEMORY;
        return DIAMOND_VM_OK;
    }
    if (index->kind != DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual, sizeof actual, *index);
        snprintf(vm->error, sizeof vm->error, "Array#[]= index must be an Int or Range, got %s", actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* A negative index counts from the end: xs[-1] is the last element. */
    int64_t array_index = index->as.integer;
    if (array_index < 0) array_index += (int64_t)array->count;
    if (array_index < 0 || (uint64_t)array_index >= array->count) {
        snprintf(vm->error, sizeof vm->error,
                 "index %" PRId64 " out of bounds for Array of length %zu",
                 index->as.integer, array->count);
        return DIAMOND_VM_INDEX_ERROR;
    }
    if (!array_value_satisfies_constraints(array, *source)) {
        snprintf(vm->error, sizeof vm->error, "array element violates its type annotation");
        return DIAMOND_VM_TYPE_ERROR;
    }
    array->values[(size_t)array_index] = *source;
    if (!gc_write_barrier_index(vm, (DiamondObject *)array, (size_t)array_index))
        return DIAMOND_VM_OUT_OF_MEMORY;
    return DIAMOND_VM_OK;
}

bool catch_exception(DiamondVm *vm,const DiamondChunk *chunk,
                            UnwindHandler *handlers,size_t *handler_count,
                            PendingUnwind *pending,DiamondValue *registers,
                            size_t *ip) {
    while(*handler_count>0) {
        UnwindHandler *handler=&handlers[*handler_count-1];
        if(handler->kind==HANDLER_ENSURE) {
            *ip=enter_ensure(handler,pending,(PendingUnwind){
                .kind=PENDING_EXCEPTION,.value=vm->exception});
            vm->has_exception=false;return true;
        }
        const UnwindHandler popped=pop_unwind_handler(handlers,handler_count,pending);
        if(popped.kind==HANDLER_ACTIVE_ENSURE)continue;
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

/* DIAMOND_VM_OUT_OF_MEMORY deliberately has no case below (falls to
 * `default: return UINT8_MAX`, meaning it can never match any rescue
 * clause) -- a genuine host allocator failure is treated as unrecoverable
 * by design: the collector has already tried and failed to free anything,
 * and running more Diamond code to handle it (which itself needs to
 * allocate, starting with the exception instance this function's own
 * caller constructs) risks cascading rather than actually recovering.
 * The one exception (so to speak): vm->memory_limit_tripped is set only
 * by maybe_collect's own configured-budget check (src/vm.c, "Resource
 * limits" section), never by a real malloc/calloc/realloc failure --
 * when it's set, THIS specific OOM is known to be an artificial, self-
 * imposed cap with plenty of real host memory still available, not a
 * genuine crisis, so it's surfaced as the same catchable ResourceLimitError
 * instruction-count/wall-clock budgets already use, rather than silently
 * reusing the uncatchable path a real OOM deliberately takes. */
static uint16_t exception_class_for_status(const DiamondVm *vm,DiamondVmStatus status) {
    if(status==DIAMOND_VM_OUT_OF_MEMORY)
        return vm->memory_limit_tripped&&!vm->memory_hard_stopped?
            DIAMOND_CLASS_RESOURCE_LIMIT_ERROR:DIAMOND_NO_CLASS;
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
        case DIAMOND_VM_CONSTANT_ERROR: return DIAMOND_CLASS_RUNTIME_ERROR;
        case DIAMOND_VM_THREAD_ERROR: return DIAMOND_CLASS_THREAD_ERROR;
        case DIAMOND_VM_SQLITE3_ERROR: return DIAMOND_CLASS_SQLITE3_ERROR;
        case DIAMOND_VM_POSTGRES_ERROR: return DIAMOND_CLASS_POSTGRES_ERROR;
        case DIAMOND_VM_MYSQL_ERROR: return DIAMOND_CLASS_MYSQL_ERROR;
        case DIAMOND_VM_NO_METHOD_ERROR: return DIAMOND_CLASS_NO_METHOD_ERROR;
        case DIAMOND_VM_JSON_ERROR: return DIAMOND_CLASS_JSON_ERROR;
        case DIAMOND_VM_SUPERVISOR_ERROR: return DIAMOND_CLASS_SUPERVISOR_ERROR;
        case DIAMOND_VM_SANDBOX_ERROR: return DIAMOND_CLASS_SANDBOX_ERROR;
        case DIAMOND_VM_RESOURCE_LIMIT_ERROR: return DIAMOND_CLASS_RESOURCE_LIMIT_ERROR;
        case DIAMOND_VM_FROZEN_ERROR: return DIAMOND_CLASS_FROZEN_ERROR;
        default: return DIAMOND_NO_CLASS;
    }
}

bool catch_runtime_error(DiamondVm *vm,const DiamondChunk *chunk,
                                DiamondVmStatus status,UnwindHandler *handlers,
                                size_t *handler_count,PendingUnwind *pending,
                                DiamondValue *registers,size_t *ip) {
    const uint16_t class_index=exception_class_for_status(vm,status);
    if(class_index==DIAMOND_NO_CLASS || (size_t)class_index>=chunk->class_count)return false;
    char message[sizeof vm->error];
    (void)snprintf(message,sizeof message,"%s",vm->error[0]!='\0'?vm->error:
                   diamond_vm_status_name(status));
    /* vm->error collects an "\n  at name:line:col" line from every frame
     * the failure unwound through (RECORD_ERROR), for the uncaught-error
     * report. The exception's message is just the first part, the same
     * text a raised Exception.new(message) carries. */
    char *location=strstr(message,"\n  at ");
    if(location!=nullptr)*location='\0';
    DiamondInstance *exception=allocate_instance(vm,chunk->classes[class_index],nullptr);
    if(exception==nullptr)return false;
    vm->exception=DIAMOND_OBJECT(exception);vm->has_exception=true;
    DiamondString *text=allocate_string(vm,message,strlen(message));
    if(text==nullptr)return false;
    if(exception->field_count>0) {
        exception->fields[0]=DIAMOND_OBJECT(text);
        /* allocate_string above can itself have triggered a minor
         * collection that promoted `exception` (reachable via
         * vm->exception, set just above) before this assignment ran --
         * this raw field write bypasses DIAMOND_OP_SET_IVAR entirely, so
         * it needs its own barrier call rather than relying on the
         * opcode-level one. */
        if(!gc_write_barrier(vm,(DiamondObject *)exception))return false;
    }
    /* Its backtrace, like a raised exception's: the frames the failure
     * already unwound through (their "at" lines, innermost first), then
     * the frames still live, starting with this rescuing one. */
    DiamondArray *backtrace=install_backtrace(vm,exception);
    if(backtrace!=nullptr) {
        const char *line=location!=nullptr?strstr(vm->error,"\n  at "):nullptr;
        while(line!=nullptr) {
            line+=6;
            const char *end=strchr(line,'\n');
            const size_t length=end!=nullptr?(size_t)(end-line):strlen(line);
            DiamondString *entry=allocate_string(vm,line,length);
            if(entry==nullptr||!array_push(vm,backtrace,DIAMOND_OBJECT(entry)))break;
            line=end!=nullptr&&strncmp(end,"\n  at ",6)==0?end:nullptr;
        }
        backtrace_append_live_frames(vm,backtrace);
    }
    /* Only fill in a generic header when vm->error is still empty (no
     * RECORD_ERROR has run yet for this failure, e.g. the origin frame
     * itself is the one with a handler) -- if it already holds the
     * detailed message + origin "at file:line:col" that RECORD_ERROR
     * wrote at the true raise site, leave it alone. Overwriting it
     * unconditionally here used to discard that detail the moment an
     * exception got caught by an *ensure* handler (which re-raises
     * rather than truly handling it, see DIAMOND_OP_END_ENSURE): the
     * final uncaught-exception message would then start over from
     * wherever the ensure block re-raised, several frames away from
     * the real cause, even though this exact detail survives intact on
     * the exception instance's own .message field above. Leaving
     * vm->error as-is here lets RECORD_ERROR keep appending later "at
     * ..." frames onto the *original* message instead of a fresh one. */
    if(vm->error[0]=='\0')
        (void)snprintf(vm->error,sizeof vm->error,"uncaught exception: %s",
                       exception->class->name);
    return catch_exception(vm,chunk,handlers,handler_count,pending,registers,ip);
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

/* Phase 4 (docs/internal/jit-design.md). Shared implementation for
 * DIAMOND_OP_INVOKE's `dup`/`freeze`/`frozen?` pseudo-methods --
 * receiver-kind-agnostic native operations checked before any per-type
 * or Instance method dispatch (see that opcode's own comment on
 * interception order), extracted so the interpreter and the JIT
 * (src/jit.c's compile_body, own DIAMOND_OP_INVOKE case) share one
 * implementation rather than two independently maintained copies. None
 * of the three can ever invoke arbitrary user code (no operator-override
 * equivalent for any of them, unlike EQUAL/INDEX_GET), so compiling one
 * of these never needs jc->has_called = true. `dup_defined` (Array/Hash/
 * String/Symbol, or any non-Object primitive) is folded directly into
 * each function rather than left to a caller to check first: a receiver
 * that doesn't qualify (an Instance, or any other Object kind --
 * Time/Regexp/File/...) returns a plain nonzero DiamondVmStatus, safe to
 * treat as "not handled here, fall back" by any caller that only ever
 * reaches these before jc->has_called could be true (see compile_body's
 * own comment for why that's guaranteed). */
/* Copies `source`'s entries and bucket table into the freshly allocated, still-empty
 * `copy` without hashing or probing anything: each entry already carries its hash, and a
 * copy with identical entry indices and bucket_capacity can reuse the bucket table
 * byte for byte. `copy` is young (allocate_hash just made it and nothing has collected
 * since), so no write barrier or dirty card is needed. entries is sized to exactly
 * `count`; hash_set grows it from there on the first insert. The typed-container
 * constraints are deliberately not copied, as before (docs/internal/jit-design.md's
 * Phase 4). */
static bool hash_clone_contents(DiamondVm *vm,DiamondHash *copy,const DiamondHash *source) {
    if(source->count==0)return true;
    DiamondHashEntry *entries=malloc(source->count*sizeof(DiamondHashEntry));
    if(entries==nullptr)return false;
    size_t *buckets=malloc(source->bucket_capacity*sizeof(size_t));
    if(buckets==nullptr){free(entries);return false;}
    memcpy(entries,source->entries,source->count*sizeof(DiamondHashEntry));
    memcpy(buckets,source->buckets,source->bucket_capacity*sizeof(size_t));
    copy->entries=entries;copy->capacity=source->count;copy->count=source->count;
    copy->buckets=buckets;copy->bucket_capacity=source->bucket_capacity;
    vm->bytes_allocated+=source->count*sizeof(DiamondHashEntry)+
        source->bucket_capacity*sizeof(size_t);
    note_storage_growth(vm);
    return true;
}

DiamondVmStatus diamond_jit_dup(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    if(receiver->kind!=DIAMOND_VALUE_OBJECT) {*out=*receiver;return DIAMOND_VM_OK;}
    const DiamondObjectKind dup_kind=receiver->as.object->kind;
    if(dup_kind==DIAMOND_OBJECT_STRING||dup_kind==DIAMOND_OBJECT_SYMBOL) {
        /* Both are immutable in this VM (every String/Symbol-producing
         * operation returns a new object rather than mutating in place),
         * so a distinct copy would be observably identical -- returning
         * the same object is correct, not just an optimization. */
        *out=*receiver;return DIAMOND_VM_OK;
    }
    if(dup_kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *source=(const DiamondArray *)receiver->as.object;
        DiamondArray *copy=allocate_array(vm,source->values,source->count);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        /* constraints[]/constraint_count deliberately NOT copied -- see
         * this same rationale preserved from the interpreter's own former
         * inline copy of this logic (docs/internal/jit-design.md's
         * Phase 4). */
        *out=DIAMOND_OBJECT(copy);return DIAMOND_VM_OK;
    }
    if(dup_kind==DIAMOND_OBJECT_HASH) {
        const DiamondHash *source=(const DiamondHash *)receiver->as.object;
        DiamondHash *copy=allocate_hash(vm);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!hash_clone_contents(vm,copy,source))return DIAMOND_VM_OUT_OF_MEMORY;
        *out=DIAMOND_OBJECT(copy);return DIAMOND_VM_OK;
    }
    if(dup_kind==DIAMOND_OBJECT_INSTANCE) {
        /* Phase 6: an Instance's own `dup`/`freeze`/`frozen?` (src/vm.c's
         * real INVOKE case) is only ever intercepted here when its class
         * does NOT define its own same-named method -- lookup_method is a
         * pure, deterministic table lookup, never arbitrary code, so
         * folding that same check into this trampoline doesn't need
         * jc->has_called (already true unconditionally for other reasons)
         * to mean anything new. A class that *does* override dup/freeze/
         * frozen? still correctly falls back to full interpretation (the
         * "not dup_defined" return below), which then reaches real
         * Instance method dispatch -- this trampoline never attempts
         * that itself. */
        const DiamondInstance *instance=(const DiamondInstance *)receiver->as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(lookup_method(owner,instance->class,"dup",3)!=nullptr)
            return DIAMOND_VM_TYPE_ERROR; /* real override -- caller must fall back */
        DiamondInstance *copy=allocate_instance(vm,instance->class,instance->owner);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        for(size_t index=0;index<instance->field_count;index++)
            copy->fields[index]=instance->fields[index];
        copy->shape=instance->shape;
        *out=DIAMOND_OBJECT(copy);return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR; /* not dup_defined -- caller must fall back */
}

DiamondVmStatus diamond_jit_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    (void)vm; /* unused -- kept for a calling convention uniform with diamond_jit_dup */
    if(receiver->kind!=DIAMOND_VALUE_OBJECT) {*out=*receiver;return DIAMOND_VM_OK;}
    const DiamondObjectKind freeze_kind=receiver->as.object->kind;
    if(freeze_kind==DIAMOND_OBJECT_STRING||freeze_kind==DIAMOND_OBJECT_SYMBOL) {
        *out=*receiver;return DIAMOND_VM_OK;
    }
    if(freeze_kind==DIAMOND_OBJECT_ARRAY||freeze_kind==DIAMOND_OBJECT_HASH) {
        receiver->as.object->frozen=true;
        *out=*receiver;return DIAMOND_VM_OK;
    }
    if(freeze_kind==DIAMOND_OBJECT_INSTANCE) {
        /* Phase 6: see diamond_jit_dup's own comment on why folding the
         * lookup_method check in here is safe. */
        const DiamondInstance *instance=(const DiamondInstance *)receiver->as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(lookup_method(owner,instance->class,"freeze",6)!=nullptr)
            return DIAMOND_VM_TYPE_ERROR; /* real override -- caller must fall back */
        receiver->as.object->frozen=true;
        *out=*receiver;return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR; /* not dup_defined -- caller must fall back */
}

/* `deep_freeze`: freezes the receiver and, transitively, every Array
 * element, Hash key/value, and Instance field reachable from it. Native
 * and shallow-by-design siblings (`freeze`) stay as they are; this is a
 * separate universal method so a class's own `freeze` is never bypassed
 * for the receiver itself, but nested instances are frozen directly,
 * without calling any user-defined `freeze` on them -- a deep freeze is a
 * guarantee about the graph, not a request to run arbitrary user code
 * mid-walk. Cycles are handled with an explicit visited set (pointer-keyed,
 * open addressing) and an explicit work stack, so neither a cyclic graph
 * nor a very deep one can recurse the C stack. Only Array/Hash/Instance
 * carry a frozen flag; every other object kind is skipped (immutable
 * String/Symbol, or native resources with no guarded mutation). Never
 * allocates on the VM heap, so no GC can run mid-walk. */
typedef struct DeepFreezeWalk {
    const DiamondObject **visited;
    size_t visited_capacity;
    size_t visited_count;
    DiamondObject **stack;
    size_t stack_count;
    size_t stack_capacity;
} DeepFreezeWalk;

static size_t deep_freeze_slot(const DiamondObject *object,size_t capacity) {
    return (size_t)(((uintptr_t)object>>4)*(uintptr_t)11400714819323198485ull)&(capacity-1);
}

/* Returns 1 if newly queued, 0 if already seen or not freezable, -1 on OOM. */
static int deep_freeze_visit(DeepFreezeWalk *walk,DiamondValue value) {
    if(value.kind!=DIAMOND_VALUE_OBJECT||value.as.object==nullptr)return 0;
    DiamondObject *object=value.as.object;
    if(object->kind!=DIAMOND_OBJECT_ARRAY&&object->kind!=DIAMOND_OBJECT_HASH&&
       object->kind!=DIAMOND_OBJECT_INSTANCE)return 0;
    if(walk->visited_count*2>=walk->visited_capacity) {
        const size_t new_capacity=walk->visited_capacity==0?64:walk->visited_capacity*2;
        const DiamondObject **grown=calloc(new_capacity,sizeof *grown);
        if(grown==nullptr)return -1;
        for(size_t index=0;index<walk->visited_capacity;index++) {
            const DiamondObject *entry=walk->visited[index];
            if(entry==nullptr)continue;
            size_t slot=deep_freeze_slot(entry,new_capacity);
            while(grown[slot]!=nullptr)slot=(slot+1)&(new_capacity-1);
            grown[slot]=entry;
        }
        free(walk->visited);
        walk->visited=grown;walk->visited_capacity=new_capacity;
    }
    size_t slot=deep_freeze_slot(object,walk->visited_capacity);
    while(walk->visited[slot]!=nullptr) {
        if(walk->visited[slot]==object)return 0;
        slot=(slot+1)&(walk->visited_capacity-1);
    }
    walk->visited[slot]=object;walk->visited_count++;
    if(walk->stack_count==walk->stack_capacity) {
        const size_t new_capacity=walk->stack_capacity==0?64:walk->stack_capacity*2;
        DiamondObject **grown=realloc(walk->stack,new_capacity*sizeof *grown);
        if(grown==nullptr)return -1;
        walk->stack=grown;walk->stack_capacity=new_capacity;
    }
    walk->stack[walk->stack_count++]=object;
    return 1;
}

DiamondVmStatus diamond_deep_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    (void)vm;
    DeepFreezeWalk walk={};
    DiamondVmStatus status=DIAMOND_VM_OK;
    if(deep_freeze_visit(&walk,*receiver)<0)status=DIAMOND_VM_OUT_OF_MEMORY;
    while(status==DIAMOND_VM_OK&&walk.stack_count>0) {
        DiamondObject *object=walk.stack[--walk.stack_count];
        object->frozen=true;
        if(object->kind==DIAMOND_OBJECT_ARRAY) {
            const DiamondArray *array=(const DiamondArray *)object;
            for(size_t index=0;index<array->count&&status==DIAMOND_VM_OK;index++)
                if(deep_freeze_visit(&walk,array->values[index])<0)
                    status=DIAMOND_VM_OUT_OF_MEMORY;
        } else if(object->kind==DIAMOND_OBJECT_HASH) {
            const DiamondHash *hash=(const DiamondHash *)object;
            for(size_t index=0;index<hash->count&&status==DIAMOND_VM_OK;index++)
                if(deep_freeze_visit(&walk,hash->entries[index].key)<0||
                   deep_freeze_visit(&walk,hash->entries[index].value)<0)
                    status=DIAMOND_VM_OUT_OF_MEMORY;
        } else {
            const DiamondInstance *instance=(const DiamondInstance *)object;
            for(size_t index=0;index<instance->field_count&&status==DIAMOND_VM_OK;index++)
                if(deep_freeze_visit(&walk,instance->fields[index])<0)
                    status=DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    free(walk.visited);free(walk.stack);
    if(status==DIAMOND_VM_OK)*out=*receiver;
    return status;
}

DiamondVmStatus diamond_jit_frozen(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    if(receiver->kind!=DIAMOND_VALUE_OBJECT) {*out=DIAMOND_BOOL(true);return DIAMOND_VM_OK;}
    const DiamondObjectKind kind=receiver->as.object->kind;
    if(kind==DIAMOND_OBJECT_STRING||kind==DIAMOND_OBJECT_SYMBOL) {
        *out=DIAMOND_BOOL(true);return DIAMOND_VM_OK;
    }
    if(kind==DIAMOND_OBJECT_ARRAY||kind==DIAMOND_OBJECT_HASH) {
        *out=DIAMOND_BOOL(receiver->as.object->frozen);return DIAMOND_VM_OK;
    }
    if(kind==DIAMOND_OBJECT_INSTANCE) {
        /* Phase 6: see diamond_jit_dup's own comment on why folding the
         * lookup_method check in here is safe. */
        const DiamondInstance *instance=(const DiamondInstance *)receiver->as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(lookup_method(owner,instance->class,"frozen?",7)!=nullptr)
            return DIAMOND_VM_TYPE_ERROR; /* real override -- caller must fall back */
        *out=DIAMOND_BOOL(receiver->as.object->frozen);return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR; /* not dup_defined -- caller must fall back */
}

/* A bare "wrong number of arguments" doesn't say which call. When the
 * failing instruction is a method call, name the method, the receiver's
 * type, and how many arguments it got. Returns the message length, or 0
 * when the instruction isn't a method call. */
size_t describe_invoke_arity_error(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondValue *registers,size_t offset) {
    if(offset+10>chunk->code_count)return 0;
    const DiamondOpCode opcode=(DiamondOpCode)chunk->code[offset];
    if(opcode!=DIAMOND_OP_INVOKE&&opcode!=DIAMOND_OP_INVOKE_MONO&&
       opcode!=DIAMOND_OP_INVOKE_TYPED)return 0;
    const uint16_t recv=(uint16_t)((chunk->code[offset+3]<<8)|chunk->code[offset+4]);
    const uint16_t name=(uint16_t)((chunk->code[offset+5]<<8)|chunk->code[offset+6]);
    const uint8_t argc=chunk->code[offset+9];
    if(name>=chunk->string_count)return 0;
    const DiamondStringConstant *method_name=&chunk->strings[name];
    char receiver[80];
    diamond_format_value_type(receiver,sizeof receiver,registers[recv]);
    const int written=snprintf(vm->error,sizeof vm->error,
        "wrong number of arguments for '%.*s' on %s (given %u)",
        (int)method_name->length,method_name->chars,receiver,argc);
    return written<0?0:strlen(vm->error);
}

/* Is `opcode` a call whose first operand is its destination register and
 * which reads every operand before invoking anything? A block's break
 * lands in exactly such an instruction: the one that passed the block. */
static bool nonlocal_break_can_land(uint8_t opcode) {
    switch((DiamondOpCode)opcode) {
        case DIAMOND_OP_CALL: case DIAMOND_OP_CALL_TYPED: case DIAMOND_OP_CALL_CLOSURE:
        case DIAMOND_OP_NEW: case DIAMOND_OP_INVOKE: case DIAMOND_OP_INVOKE_MONO:
        case DIAMOND_OP_INVOKE_TYPED: case DIAMOND_OP_SUPER:
        case DIAMOND_OP_INVOKE_SELF_METHOD: case DIAMOND_OP_CALL_SPREAD:
        case DIAMOND_OP_INVOKE_SPREAD: case DIAMOND_OP_CALL_CLOSURE_SPREAD:
        case DIAMOND_OP_NEW_SPREAD: case DIAMOND_OP_CALL_SINGLETON_SPREAD:
        case DIAMOND_OP_CALL_TYPED_SPREAD: case DIAMOND_OP_INVOKE_TYPED_SPREAD:
        case DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD: case DIAMOND_OP_CALL_KEYWORD_SPREAD:
        case DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD: case DIAMOND_OP_INVOKE_KEYWORDS:
        case DIAMOND_OP_INVOKE_TYPED_KEYWORDS: case DIAMOND_OP_CALL_CLOSURE_KEYWORDS:
        case DIAMOND_OP_NEW_KEYWORDS: case DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
        case DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS:
            return true;
        default:
            return false;
    }
}

static void clear_nonlocal_exit(DiamondVm *vm) {
    vm->nonlocal_value=DIAMOND_NIL;
    vm->nonlocal_target=0;
}

/* A DIAMOND_VM_NONLOCAL_EXIT reaching `frame` on its way out. The target
 * frame either lands a break -- the call instruction it's inside receives
 * the value and execution continues after it, since every call opcode has
 * read all its operands before invoking anything -- or returns the value
 * as `return` would, running ensure handlers first. Any other frame runs
 * its own ensure handlers and passes the exit on. */
NonlocalOutcome nonlocal_exit_arrives(DiamondVm *vm,const DiamondChunk *chunk,
        const DiamondFrame *frame,DiamondValue *registers,UnwindHandler *handlers,
        size_t *handler_count,PendingUnwind *pending,size_t *ip,
        size_t instruction_offset,DiamondValue *result) {
    const bool here=vm->nonlocal_target==frame->serial;
    if(here&&vm->nonlocal_is_break) {
        const uint8_t opcode=chunk->code[instruction_offset];
        if(!nonlocal_break_can_land(opcode)||instruction_offset+3>chunk->code_count) {
            snprintf(vm->error,sizeof vm->error,
                "break from a block that wasn't passed to a method call here");
            return NONLOCAL_INVALID;
        }
        const uint16_t destination=(uint16_t)(
            ((unsigned)chunk->code[instruction_offset+1]<<8)|
            chunk->code[instruction_offset+2]);
        if(destination>=frame->register_count) {
            snprintf(vm->error,sizeof vm->error,"invalid break destination");
            return NONLOCAL_INVALID;
        }
        registers[destination]=vm->nonlocal_value;
        clear_nonlocal_exit(vm);
        return NONLOCAL_CONTINUE;
    }
    while(*handler_count>0&&handlers[*handler_count-1].kind!=HANDLER_ENSURE)
        (void)pop_unwind_handler(handlers,handler_count,pending);
    if(*handler_count>0) {
        const PendingUnwind next=here?
            (PendingUnwind){.kind=PENDING_RETURN,.value=vm->nonlocal_value}:
            (PendingUnwind){.kind=PENDING_NONLOCAL,.value=vm->nonlocal_value,
                .nonlocal_target=vm->nonlocal_target,
                .nonlocal_is_break=vm->nonlocal_is_break};
        *ip=enter_ensure(&handlers[*handler_count-1],pending,next);
        if(here) {clear_nonlocal_exit(vm);}
        return NONLOCAL_CONTINUE;
    }
    if(here) {
        *result=vm->nonlocal_value;
        clear_nonlocal_exit(vm);
        return NONLOCAL_RETURNED;
    }
    if(frame->previous==nullptr) {
        clear_nonlocal_exit(vm);
        snprintf(vm->error,sizeof vm->error,vm->nonlocal_is_break?
            "break from a block outside the call it was passed to":
            "return from a block outside the method it was written in");
        return NONLOCAL_INVALID;
    }
    return NONLOCAL_PASS_ON;
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
