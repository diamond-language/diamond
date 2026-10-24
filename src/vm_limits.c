#define _GNU_SOURCE

#include "vm_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#ifndef DIAMOND_NO_SQLITE
#include <sqlite3.h>
#endif
#ifndef DIAMOND_NO_POSTGRES
#include <libpq-fe.h>
#endif

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

void budget_register(DiamondVm *vm) {
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

void budget_unregister(DiamondVm *vm) {
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
