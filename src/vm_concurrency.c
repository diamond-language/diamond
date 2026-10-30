#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "vm_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

atomic_size_t diamond_active_thread_count = 0;

/* OS threads currently running Diamond code: Thread.new workers and
 * supervised children, counted from just before pthread_create until their
 * entry function returns. Unlike diamond_active_thread_count, a finished
 * but not yet reaped thread doesn't count. See diamond_vm_running_threads. */
static atomic_size_t diamond_running_thread_count = 0;

size_t diamond_vm_running_threads(void) {
    return atomic_load(&diamond_running_thread_count);
}

/* Off unless the process owner turns it on: an embedder running many
 * programs in one process (tests/run_cases.c) must not be exited by one of
 * them. */
static atomic_bool diamond_exit_on_threaded_failure = false;

void diamond_vm_set_exit_on_threaded_failure(bool enabled) {
    atomic_store(&diamond_exit_on_threaded_failure,enabled);
}

void diamond_vm_exit_if_threads_running(int status) {
    if(!atomic_load(&diamond_exit_on_threaded_failure))return;
    if(atomic_load(&diamond_running_thread_count)==0)return;
    fflush(stdout);
    fflush(stderr);
    exit(status);
}

/* run_chunk's deliberately fixed-size interpreter frame is large (see the
 * native-recursion-limit comment near the top of this file), so libc's
 * pthread default is part of the VM's effective call-depth contract unless
 * Diamond overrides it. glibc happens to inherit an 8 MiB default, while
 * musl commonly supplies only about 128 KiB and Darwin 512 KiB: ordinary
 * Thread.new code can exhaust the former, and the language-level recursion
 * guard cannot fire before the latter's native stack is already gone.
 *
 * Keep language threads aligned with Diamond fibers and the stack size used
 * to calibrate DIAMOND_MAX_CALL_DEPTH. Small native worker threads such as
 * Tensor#matmul do not run the interpreter and intentionally keep their
 * platform defaults. */
enum { DIAMOND_VM_THREAD_STACK_SIZE = 8 * 1024 * 1024 };

thread_local size_t diamond_thread_call_depth_limit = DIAMOND_MAX_CALL_DEPTH;

void diamond_set_thread_stack_size(size_t stack_bytes) {
    if (stack_bytes < DIAMOND_VM_THREAD_STACK_SIZE)
        stack_bytes = DIAMOND_VM_THREAD_STACK_SIZE;
    diamond_thread_call_depth_limit = (size_t)DIAMOND_MAX_CALL_DEPTH *
        (stack_bytes / DIAMOND_VM_THREAD_STACK_SIZE);
}

int create_vm_thread(pthread_t *handle,
        void *(*entry)(void *), void *argument) {
    pthread_attr_t attributes;
    int status=pthread_attr_init(&attributes);
    if(status!=0)return status;
    status=pthread_attr_setstacksize(&attributes,DIAMOND_VM_THREAD_STACK_SIZE);
    if(status==0) {
        /* Counted before the thread can possibly finish, and uncounted by
         * the entry function itself on its way out. */
        atomic_fetch_add(&diamond_running_thread_count,1);
        status=pthread_create(handle,&attributes,entry,argument);
        if(status!=0)atomic_fetch_sub(&diamond_running_thread_count,1);
    }
    pthread_attr_destroy(&attributes);
    return status;
}

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
 * diamond_program_init's own defaults is correct, not a gap. Class and
 * interface slots are copied one by one; independently allocated
 * function records keep thread-local method replacement isolated. Returns
 * nullptr only on allocation failure. */
static size_t interface_type_end(size_t end,uint16_t index) {
    return index==DIAMOND_NO_TYPE_SET||index<end?end:(size_t)index+1;
}

DiamondProgram *clone_program_from_chunk(const DiamondChunk *chunk) {
    DiamondProgram *clone=calloc(1,sizeof *clone);
    if(clone==nullptr)return nullptr;
    /* _fresh, not diamond_program_init: `clone` is freshly calloc'd right
     * above (already all-zero) and never reused across calls -- a new
     * Thread.new spawn always gets its own brand-new clone -- so
     * diamond_program_init's own memset would just re-zero memory
     * calloc already zeroed. See its own comment (src/compiler.c). */
    diamond_program_init_fresh(clone);
    const DiamondTypeSet *entry_types=chunk->type_sets;
    size_t entry_type_count=chunk->type_set_count;
    const DiamondTypeSet *interface_types=nullptr;
    size_t interface_end=0;
    for(size_t index=0;index<chunk->interface_count;index++) {
        const DiamondInterface *interface=chunk->interfaces[index];
        size_t needed=0;
        for(size_t method=0;method<interface->method_count;method++) {
            const DiamondInterfaceMethod *signature=&interface->methods[method];
            needed=interface_type_end(needed,signature->return_type_set);
            for(size_t parameter=0;parameter<signature->arity;parameter++)
                needed=interface_type_end(needed,signature->parameter_type_sets[parameter]);
        }
        if(needed==0)continue;
        if(interface->type_sets==nullptr||
           (interface_types!=nullptr&&interface_types!=interface->type_sets)) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        interface_types=interface->type_sets;
        if(needed>interface_end)interface_end=needed;
    }
    if(interface_types!=nullptr) {
        /* A function's chunk has its own type table. Interface indexes still
         * name the program entry table: copy its closed referenced prefix. */
        for(size_t index=0;index<interface_end;index++)
            for(size_t member=0;member<interface_types[index].count;member++) {
                const DiamondTypeMember type=interface_types[index].members[member];
                interface_end=interface_type_end(interface_end,type.argument_set);
                interface_end=interface_type_end(interface_end,type.second_argument_set);
                interface_end=interface_type_end(interface_end,type.callable_return_set);
                if(type.callable_parameters_typed)
                    for(size_t parameter=0;parameter<type.callable_arity;parameter++)
                        interface_end=interface_type_end(interface_end,type.callable_parameter_sets[parameter]);
            }
        entry_types=interface_types;entry_type_count=interface_end;
    }
    if(entry_type_count>0) {
        if(!diamond_function_reserve_type_sets(&clone->entry,
                entry_type_count)) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        memcpy(clone->entry.type_sets,entry_types,
            entry_type_count*sizeof *clone->entry.type_sets);
        clone->entry.type_set_count=entry_type_count;
    }
    for(size_t index=0;index<chunk->function_count;index++) {
        DiamondFunction *function=diamond_program_add_function(clone);
        if(function==nullptr) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        if(!diamond_function_copy(function,chunk->functions[index])) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
    }
    for(size_t index=0;index<chunk->class_count;index++) {
        DiamondClass *copy=diamond_program_class_slot(clone,index);
        if(copy==nullptr) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        if(!diamond_class_copy(copy,chunk->classes[index])) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        copy->index=(uint16_t)index;
    }
    clone->class_count=chunk->class_count;
    for(size_t index=0;index<chunk->interface_count;index++) {
        DiamondInterface *copy=diamond_program_interface_slot(clone,index);
        if(copy==nullptr) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        if(!diamond_interface_copy(copy,chunk->interfaces[index])) {
            diamond_program_free(clone);free(clone);return nullptr;
        }
        copy->type_sets=clone->entry.type_sets;
    }
    clone->interface_count=chunk->interface_count;
    clone->range_class_index=chunk->range_class_index;
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
void *thread_entry_trampoline(void *argument) {
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
        .class_type_variable=target_fn->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(target_fn->owner_class+1u),
        .parameter_offset=target_fn->owner_class==DIAMOND_OWNER_NONE?0:1,
        .register_count=target_fn->register_count,
        .has_variadic=target_fn->has_variadic,
        .range_class_index=thread->child_program->range_class_index};
    /* This trampoline calls run_chunk directly rather than through
     * diamond_vm_run (this is a spawned Thread's own dedicated entry
     * point, on its own OS thread, never diamond_vm_run's top-level
     * caller), so it has to seed range_class_index onto child_vm itself
     * here -- diamond_vm_run's own copy of this same assignment never
     * runs for a Thread. See DiamondVm's own comment on this field.
     * root_chunk needs the same seeding, for the same reason -- &child_chunk
     * is safe to store there despite being a local: root_chunk is only
     * ever read while run_chunk(&child_chunk,...) below is still on the
     * stack, the same lifetime child_chunk itself already relies on. */
    thread->child_vm->range_class_index=thread->child_program->range_class_index;
    thread->child_vm->root_chunk=&child_chunk;
    DiamondValue run_result=DIAMOND_NIL;
    const DiamondClosure *entry=thread->args[thread->arg_count].kind==DIAMOND_VALUE_OBJECT?
        (const DiamondClosure *)thread->args[thread->arg_count].as.object:nullptr;
    const DiamondVmStatus run_status=run_chunk(&child_chunk,thread->child_vm,
        thread->args,thread->arg_count,0,entry,&run_result);
    if(run_status==DIAMOND_VM_EXCEPTION) {
        thread->result=thread->child_vm->exception;thread->raised=true;
    } else if(run_status!=DIAMOND_VM_OK) {
        thread->internal_failure=true;
    } else {
        thread->result=run_result;
    }
    budget_release_finished(thread->child_vm);
    atomic_store(&thread->finished,true);
    atomic_fetch_sub(&diamond_running_thread_count,1);
    return nullptr;
}

/* The synthetic DiamondChunk a supervised child runs, exactly the shape
 * thread_entry_trampoline's own local `child_chunk` already uses --
 * program_template never changes across restarts, so it is built once
 * per child, not per attempt. */
static DiamondChunk supervisor_child_chunk(const DiamondSupervisorChild *child) {
    const DiamondFunction *target_fn=
        child->program_template->functions[child->function_index];
    return (DiamondChunk){
        .name=target_fn->name,.code=target_fn->code,
        .lines=target_fn->lines,.columns=target_fn->columns,
        .code_count=target_fn->code_count,
        .constants=target_fn->constants,.constant_count=target_fn->constant_count,
        .strings=target_fn->strings,.string_count=target_fn->string_count,
        .type_sets=target_fn->type_sets,.type_set_count=target_fn->type_set_count,
        .functions=child->program_template->functions,
        .function_count=child->program_template->function_count,
        .classes=child->program_template->classes,
        .class_count=child->program_template->class_count,
        .interfaces=child->program_template->interfaces,
        .interface_count=child->program_template->interface_count,
        .parameter_type_sets=target_fn->parameter_type_sets,
        .type_variable_count=target_fn->type_variable_count,
        .type_variable_bindings=child->args[child->arg_count].kind==DIAMOND_VALUE_OBJECT?
            ((const DiamondClosure *)child->args[child->arg_count].as.object)->type_bindings:nullptr,
        .class_type_variable=target_fn->class_type_variable,
        .binding_owner_plus_one=(uint16_t)(target_fn->owner_class+1u),
        .parameter_offset=target_fn->owner_class==DIAMOND_OWNER_NONE?0:1,
        .register_count=target_fn->register_count,
        .has_variadic=target_fn->has_variadic,
        .range_class_index=child->program_template->range_class_index};
}

/* Sets up one attempt's freshly allocated run_vm and copies the child's
 * constants and arguments into it. Returns false when an argument could
 * not be copied. Called without the supervisor's lock. */
static bool prepare_supervised_attempt(DiamondSupervisorChild *child,
        const DiamondChunk *child_chunk,DiamondVm *run_vm,DiamondValue *run_args) {
    DiamondSupervisor *supervisor=child->supervisor;
    diamond_vm_init(run_vm);
    diamond_vm_configure_jit_from_env(run_vm);
    run_vm->range_class_index=child->program_template->range_class_index;
    run_vm->root_chunk=child_chunk;
    /* The interrupt check costs every dispatched instruction, so it is
     * armed up front only where a sibling's crash may need it.
     * Supervisor#stop(timeout_ms) arms it on demand through running_vm. */
    run_vm->interrupt_flag=&child->interrupt;
    if(supervisor->strategy!=DIAMOND_SUPERVISOR_ONE_FOR_ONE||
       supervisor->max_restarts>=0)
        run_vm->resource_limits_active=true;
    /* Same defensive gc_protect-as-produced pattern DIAMOND_OP_THREAD_
     * NEW's own argument copy loop uses, for the identical reason: a
     * later argument's copy_value_into_vm call can itself trigger a
     * collection on run_vm before anything roots an earlier argument
     * already copied into it this iteration. */
    bool copy_failed=false;
    run_vm->spawned_thread=true;
    copy_namespace_constants_into_vm(child->args_vm,run_vm,
        child->program_template->classes,child->program_template->classes);
    const size_t args_mark=run_vm->gc_protected_count;
    for(uint8_t index=0;index<child->arg_count;index++) {
        if(!copy_value_into_vm(run_vm,child->args[index],nullptr,
                child->program_template->classes,
                child->program_template->classes,nullptr,
                &run_args[index])||
           !gc_protect(run_vm,run_args[index])) {
            copy_failed=true;break;
        }
    }
    gc_unprotect(run_vm,args_mark);
    return !copy_failed;
}

/* Records why a failed attempt ended in child->last_error, and counts a
 * restart where one follows. Called with the supervisor's lock held. */
static void record_attempt_failure(DiamondSupervisorChild *child,DiamondVm *run_vm,
        DiamondVmStatus status) {
    DiamondSupervisor *supervisor=child->supervisor;
    const bool interrupted_by_sibling=status==DIAMOND_VM_INTERRUPTED;
    if(interrupted_by_sibling&&supervisor->failed) {
        /* The supervisor gave up: this is a shutdown, not a restart. */
        snprintf(child->last_error,sizeof child->last_error,
            "stopped: the supervisor's restart intensity was exceeded");
    } else if(interrupted_by_sibling&&supervisor->stop_forced) {
        /* Supervisor#stop(timeout_ms) ran out of patience. */
        snprintf(child->last_error,sizeof child->last_error,
            "stopped: Supervisor#stop timed out");
    } else if(interrupted_by_sibling) {
        /* Not this child's own crash: a sibling's failure restarted
         * it, and last_error says so. No restart delay below, since
         * this attempt wasn't failing on its own. */
        child->restart_count++;
        snprintf(child->last_error,sizeof child->last_error,
            "restarted: a sibling crashed");
    } else if(status==DIAMOND_VM_EXCEPTION) {
        child->restart_count++;
        format_uncaught_exception_message(run_vm,run_vm->exception,false);
        snprintf(child->last_error,sizeof child->last_error,"%s",run_vm->error);
    } else {
        child->restart_count++;
        const char *message=run_vm->error[0]!='\0'?run_vm->error:
            diamond_vm_status_name(status);
        snprintf(child->last_error,sizeof child->last_error,"%s",message);
    }
}

/* Counts the child's own crash at `now_ns` against the restart intensity
 * (more than max_restarts crashes within within_ms), and gives up -- and
 * interrupts every other running child -- when it is exceeded. Called
 * with the supervisor's lock held. */
static void count_crash_against_intensity(DiamondSupervisorChild *child,int64_t now_ns) {
    DiamondSupervisor *supervisor=child->supervisor;
    if(supervisor->max_restarts<0||supervisor->failed||
       atomic_load(&supervisor->stop_requested))
        return;
    /* Drop crashes that left the window, then count this one. */
    const int64_t window_ns=supervisor->within_ms*1000000;
    size_t kept=0;
    for(size_t index=0;index<supervisor->crash_time_count;index++)
        if(now_ns-supervisor->crash_times_ns[index]<=window_ns)
            supervisor->crash_times_ns[kept++]=
                supervisor->crash_times_ns[index];
    supervisor->crash_time_count=kept;
    supervisor->crash_times_ns[supervisor->crash_time_count++]=now_ns;
    if((int64_t)supervisor->crash_time_count>supervisor->max_restarts) {
        supervisor->failed=true;
        /* This crash is not followed by a restart. */
        child->restart_count--;
        snprintf(supervisor->failure_message,
            sizeof supervisor->failure_message,
            "supervisor gave up: more than %" PRId64 " crashes within "
            "%" PRId64 " ms (last error: %s)",
            supervisor->max_restarts,supervisor->within_ms,
            child->last_error);
        atomic_store(&supervisor->stop_requested,true);
        for(size_t other=0;other<supervisor->child_count;other++)
            if(other!=child->index&&!supervisor->children[other].done)
                supervisor_interrupt_child(&supervisor->children[other]);
    }
}

/* The child's own crash: the exponential backoff before its restart, after
 * counting the crash against the restart intensity. Called with the
 * supervisor's lock held. */
static int64_t crash_restart_delay_ms(DiamondSupervisorChild *child,
        int64_t attempt_started_ns) {
    DiamondSupervisor *supervisor=child->supervisor;
    int64_t restart_delay_ms=supervisor->backoff_ms;
    const int64_t now_ns=monotonic_nanoseconds();
    /* An attempt that outlived the longest pause was healthy: the
     * crash that ended it starts a new run of consecutive crashes. */
    if((now_ns-attempt_started_ns)/1000000>supervisor->max_backoff_ms)
        child->consecutive_crashes=0;
    child->consecutive_crashes++;
    for(size_t doubling=1;doubling<child->consecutive_crashes&&
            restart_delay_ms<supervisor->max_backoff_ms;doubling++)
        restart_delay_ms*=2;
    if(restart_delay_ms>supervisor->max_backoff_ms)
        restart_delay_ms=supervisor->max_backoff_ms;
    count_crash_against_intensity(child,now_ns);
    return restart_delay_ms;
}

/* This child's own crash: take down the siblings its strategy
 * covers. Already-finished children (clean return) stay
 * finished, the same as for a plain crash. Called with the supervisor's
 * lock held. */
static void interrupt_strategy_siblings(DiamondSupervisorChild *child) {
    DiamondSupervisor *supervisor=child->supervisor;
    const size_t first=supervisor->strategy==DIAMOND_SUPERVISOR_ONE_FOR_ALL?0:
        child->index+1;
    for(size_t other=first;other<supervisor->child_count;other++) {
        if(other==child->index||supervisor->children[other].done)continue;
        supervisor_interrupt_child(&supervisor->children[other]);
    }
}

/* Flips alive?() to false for a child that is about to stop for good. */
static void mark_supervised_child_done(DiamondSupervisorChild *child) {
    pthread_mutex_lock(&child->supervisor->lock);
    child->done=true;
    pthread_mutex_unlock(&child->supervisor->lock);
}

/* pthread_create's entry point for one supervised child -- unlike
 * thread_entry_trampoline (a single run, one OS thread per Thread.new
 * call), this loops for the child's *entire* supervised lifetime: a
 * fresh DiamondVm/heap per attempt, freed at the end of every attempt
 * whether it crashed or not, exactly the isolated-heap-per-run guarantee
 * an ordinary Thread already gives a single spawn. A clean, non-raising
 * return ends the loop for good (v1 restarts on crash only, never on a
 * normal return -- see docs/threads.md); an uncaught exception or
 * internal VM failure records the reason, bumps restart_count, and
 * loops again after the restart policy's backoff (see
 * crash_restart_delay_ms) unless stop_requested has been set meanwhile.
 * Every lock and unlock of supervisor->lock is in this function; the
 * helpers it calls either run unlocked or say they need it held. */
static void *supervisor_child_entry_trampoline(void *argument) {
    DiamondSupervisorChild *child=(DiamondSupervisorChild *)argument;
    DiamondSupervisor *supervisor=child->supervisor;
    const DiamondChunk child_chunk=supervisor_child_chunk(child);
    for(;;) {
        /* A child added after the supervisor gave up (or racing with it)
         * must not start: nothing would ever interrupt it. */
        pthread_mutex_lock(&supervisor->lock);
        const bool supervisor_failed=supervisor->failed;
        if(supervisor_failed) {
            snprintf(child->last_error,sizeof child->last_error,
                "stopped: the supervisor's restart intensity was exceeded");
            child->done=true;
        } else {
            /* Cleared here, under the same lock a giving-up sibling holds
             * while it sets `failed` and raises every other child's flag. A
             * reset after unlocking could erase that interrupt (the attempt's
             * VM setup below takes long enough for a sibling to give up in
             * between) and leave this child running forever. */
            atomic_store(&child->interrupt,false);
        }
        pthread_mutex_unlock(&supervisor->lock);
        if(supervisor_failed)break;
        DiamondVm *run_vm=malloc(sizeof *run_vm);
        if(run_vm==nullptr) {
            /* System-level OOM allocating this attempt's own VM -- give up
             * for good rather than spinning trying the same allocation
             * again; still needs to flip alive?() to false and explain
             * why, exactly like any other terminal outcome below. */
            pthread_mutex_lock(&supervisor->lock);
            snprintf(child->last_error,sizeof child->last_error,
                "out of memory allocating supervised run state");
            child->done=true;
            pthread_mutex_unlock(&supervisor->lock);
            break;
        }
        DiamondValue run_args[DIAMOND_MAX_ARGUMENTS]={};
        const bool copy_failed=!prepare_supervised_attempt(child,&child_chunk,run_vm,run_args);
        DiamondValue run_result=DIAMOND_NIL;
        pthread_mutex_lock(&supervisor->lock);
        child->running_vm=run_vm;
        if(supervisor->stop_forced)
            atomic_store(&run_vm->resource_limits_active,true);
        pthread_mutex_unlock(&supervisor->lock);
        const int64_t attempt_started_ns=monotonic_nanoseconds();
        const DiamondVmStatus status=copy_failed?DIAMOND_VM_OUT_OF_MEMORY:
            run_chunk(&child_chunk,run_vm,run_args,child->arg_count,0,
                      nullptr,&run_result);
        pthread_mutex_lock(&supervisor->lock);
        child->running_vm=nullptr;
        if(status==DIAMOND_VM_OK) {
            child->done=true;
            pthread_mutex_unlock(&supervisor->lock);
            diamond_vm_free(run_vm);free(run_vm);
            break;
        }
        const bool interrupted_by_sibling=status==DIAMOND_VM_INTERRUPTED;
        record_attempt_failure(child,run_vm,status);
        int64_t restart_delay_ms=supervisor->backoff_ms;
        if(!interrupted_by_sibling)
            restart_delay_ms=crash_restart_delay_ms(child,attempt_started_ns);
        if(!interrupted_by_sibling&&
           !supervisor->failed&&
           supervisor->strategy!=DIAMOND_SUPERVISOR_ONE_FOR_ONE&&
           !atomic_load(&supervisor->stop_requested))
            interrupt_strategy_siblings(child);
        pthread_mutex_unlock(&supervisor->lock);
        diamond_vm_free(run_vm);free(run_vm);
        if(atomic_load(&supervisor->stop_requested)) {
            /* stop() (or free_supervisor_reference) already blocks
             * joining this very thread once it returns, but alive?()
             * must also flip to false right away -- otherwise a caller
             * that checks alive?() right after stop() returns would
             * still see a stale "true" for a child that stopped here
             * rather than via a clean return. */
            mark_supervised_child_done(child);
            break;
        }
        if(interrupted_by_sibling)continue;
        struct timespec delay={.tv_sec=(time_t)(restart_delay_ms/1000),
            .tv_nsec=(long)(restart_delay_ms%1000)*1000000L};
        nanosleep(&delay,nullptr);
        /* stop() may have arrived during the pause: no further attempt. */
        if(atomic_load(&supervisor->stop_requested)) {
            mark_supervised_child_done(child);
            break;
        }
    }
    atomic_fetch_sub(&diamond_active_thread_count,1);
    atomic_fetch_sub(&diamond_running_thread_count,1);
    return nullptr;
}

/* Shared by Supervisor#stop, Supervisor#join, and free_supervisor_
 * reference -- all three "join every child" call sites, any combination
 * of which may run against the same Supervisor over its lifetime.
 * pthread_join on an already-joined thread is undefined behavior (POSIX),
 * so each child's own `joined` flag (checked and set under `lock`) makes
 * the real pthread_join call idempotent regardless of how many times or
 * which of the three callers reach a given child first. Deliberately
 * does not touch stop_requested/stopped -- callers that need to actually
 * stop future restarts (Supervisor#stop, free_supervisor_reference) set
 * those themselves before calling this; Supervisor#join does not, so it
 * blocks only on children that finish on their own. */
void supervisor_join_all_children(DiamondSupervisor *supervisor) {
    for(size_t index=0;index<supervisor->child_count;index++) {
        DiamondSupervisorChild *child=&supervisor->children[index];
        pthread_mutex_lock(&supervisor->lock);
        const bool already_joined=child->joined;
        child->joined=true;
        pthread_mutex_unlock(&supervisor->lock);
        if(!already_joined)pthread_join(child->handle,nullptr);
    }
}

DiamondVmStatus fiber_invoke_helper(DiamondVm *vm,
                                           DiamondValue *registers, uint16_t recv,
                                           uint16_t base, uint8_t argc, uint16_t dest,
                                           const DiamondStringConstant *method_name) {
    DiamondFiber *target_fiber=
        ((DiamondFiberHandle *)registers[recv].as.object)->fiber;
    const bool resume_method=method_name->length==6&&
        memcmp(method_name->chars,"resume",6)==0;
    const bool status_method=method_name->length==6&&
        memcmp(method_name->chars,"status",6)==0;
    const bool alive_method=method_name->length==6&&
        memcmp(method_name->chars,"alive?",6)==0;
    if(status_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const char *state_name=diamond_fiber_state_name(target_fiber->state);
        DiamondString *string=allocate_string(vm,state_name,strlen(state_name));
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=DIAMOND_OBJECT(string);return DIAMOND_VM_OK;
    }
    if(alive_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=DIAMOND_BOOL(
            target_fiber->state!=DIAMOND_FIBER_COMPLETED&&
            target_fiber->state!=DIAMOND_FIBER_FAILED);
        return DIAMOND_VM_OK;
    }
    if(!resume_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Fiber");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(argc>1)return DIAMOND_VM_ARITY_ERROR;
    const DiamondValue resume_argument=argc==1?registers[base]:DIAMOND_NIL;
    if(target_fiber->state!=DIAMOND_FIBER_RUNNABLE&&
       target_fiber->state!=DIAMOND_FIBER_SUSPENDED) {
        snprintf(vm->error,sizeof vm->error,
                 "cannot resume a fiber that is not runnable or suspended");
        return DIAMOND_VM_FIBER_NOT_RESUMABLE;
    }
    diamond_fiber_resume(target_fiber,resume_argument);
    diamond_fiber_run(target_fiber);
    /* target_fiber->result/resume_value just mutated above,
     * but those live in the DiamondFiber payload struct, not
     * a DiamondObject header of their own -- the write
     * barrier has to reach back to the owning
     * DiamondFiberHandle (registers[recv]'s own object) for
     * a promoted fiber to stay correctly remembered. See
     * docs/gc-generational-design.md's "sharpest risk". */
    if(!gc_write_barrier(vm,registers[recv].as.object))
        return DIAMOND_VM_OUT_OF_MEMORY;
    if(target_fiber->status!=DIAMOND_VM_OK&&
       target_fiber->status!=DIAMOND_VM_YIELDED)
        return target_fiber->status;
    registers[dest]=target_fiber->result;return DIAMOND_VM_OK;
}

DiamondVmStatus thread_invoke_helper(DiamondVm *vm,
                                            const DiamondChunk *chunk, DiamondValue *registers,
                                            uint16_t recv, uint8_t argc, uint16_t dest,
                                            const DiamondStringConstant *method_name) {
    DiamondThread *target_thread=
        ((DiamondThreadHandle *)registers[recv].as.object)->thread;
    const bool join_method=method_name->length==4&&
        memcmp(method_name->chars,"join",4)==0;
    const bool alive_method=method_name->length==6&&
        memcmp(method_name->chars,"alive?",6)==0;
    if(alive_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        registers[dest]=
            DIAMOND_BOOL(!atomic_load(&target_thread->finished));
        return DIAMOND_VM_OK;
    }
    if(!join_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Thread");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
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
        if(target_thread->spawned&&vm->interrupt_flag!=nullptr) {
            /* pthread_join cannot be interrupted: wait for the thread to
             * finish in short naps, then join a thread that has already
             * returned. An interrupted join leaves the thread unjoined. */
            while(!atomic_load(&target_thread->finished)) {
                if(vm_interrupt_requested(vm)) {
                    pthread_mutex_unlock(&target_thread->join_lock);
                    return DIAMOND_VM_INTERRUPTED;
                }
                const struct timespec nap={.tv_sec=0,
                    .tv_nsec=(DIAMOND_INTERRUPT_SLICE_MS/4)*1000000L};
                nanosleep(&nap,nullptr);
            }
        }
        if(target_thread->spawned)
            pthread_join(target_thread->handle,nullptr);
        if(!target_thread->internal_failure) {
            DiamondValue copied=DIAMOND_NIL;
            const bool copy_ok=copy_value_into_vm(vm,
                target_thread->result,nullptr,
                target_thread->child_program->classes,
                chunk->classes,nullptr,&copied);
            if(!copy_ok) {
                /* The OS thread was already pthread_join'd
                 * above, so this join is spent: leaving
                 * `joined` false made a later join (or
                 * free_thread) join it a second time, which
                 * is undefined behavior (a crash on musl).
                 * Record it as a failed join instead; the
                 * ThreadError path below raises the message
                 * and every later join re-raises it. */
                snprintf(target_thread->child_vm->error,
                    sizeof target_thread->child_vm->error,
                    "%s %s","Thread result",copy_failure_reason());
                target_thread->internal_failure=true;
            } else {
                target_thread->result=copied;
                /* target_thread->result lives in the
                 * DiamondThread payload struct, not a
                 * DiamondObject header of its own -- the
                 * barrier has to reach back to the owning
                 * DiamondThreadHandle (registers[recv]'s own
                 * object) for a promoted handle to stay
                 * correctly remembered. */
                if(!gc_write_barrier(vm,registers[recv].as.object)) {
                    pthread_mutex_unlock(&target_thread->join_lock);
                    return DIAMOND_VM_OUT_OF_MEMORY;
                }
            }
        }
        target_thread->joined=true;
    }
    pthread_mutex_unlock(&target_thread->join_lock);
    if(target_thread->internal_failure) {
        if((size_t)DIAMOND_CLASS_THREAD_ERROR>=chunk->class_count)
            return DIAMOND_VM_THREAD_ERROR;
        DiamondInstance *thread_error=allocate_instance(vm,
            chunk->classes[DIAMOND_CLASS_THREAD_ERROR],nullptr);
        if(thread_error==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        /* Root immediately, before the allocate_string call
         * below can itself trigger a collection -- same
         * pattern raise_capture_backtrace_helper's own
         * comment documents: thread_error must be
         * reachable via vm->exception before any further
         * allocation, or a GC in between would sweep it as
         * unreferenced garbage. */
        vm->exception=DIAMOND_OBJECT(thread_error);
        vm->has_exception=true;
        const char *message=
            target_thread->child_vm->error[0]!='\0'?
                target_thread->child_vm->error:"thread failed";
        DiamondString *message_string=
            allocate_string(vm,message,strlen(message));
        if(message_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(thread_error->field_count>0) {
            thread_error->fields[0]=DIAMOND_OBJECT(message_string);
            /* allocate_string above can itself have
             * triggered a minor collection that promoted
             * thread_error (now reachable via
             * vm->exception) before this assignment ran --
             * this raw field write bypasses
             * DIAMOND_OP_SET_IVAR entirely, so it needs its
             * own barrier call. */
            if(!gc_write_barrier(vm,(DiamondObject *)thread_error))
                return DIAMOND_VM_OUT_OF_MEMORY;
        }
        return DIAMOND_VM_EXCEPTION;
    }
    if(target_thread->raised) {
        vm->exception=target_thread->result;
        vm->has_exception=true;
        return DIAMOND_VM_EXCEPTION;
    }
    registers[dest]=target_thread->result;return DIAMOND_VM_OK;
}

/* Channel#send and #try_send: copies the value into the channel's private program and queues
 * it, waiting for room (and, for a rendezvous channel, for a receiver to take it) unless trying.
 * Part of channel_invoke_helper. */
static DiamondVmStatus channel_send_helper(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondChannel *target_channel,DiamondValue value_to_send,bool try_send_method) {
    pthread_mutex_lock(&target_channel->lock);
    while((target_channel->count==target_channel->capacity||
           (try_send_method&&target_channel->rendezvous&&
            target_channel->receivers_waiting==0))&&
          !target_channel->closed) {
        if(try_send_method) {
            pthread_mutex_unlock(&target_channel->lock);
            snprintf(vm->error,sizeof vm->error,
                "channel send would block");
            return DIAMOND_VM_WOULD_BLOCK;
        }
        const DiamondVmStatus wait_status=channel_wait_bounded(vm,
            &target_channel->not_full,&target_channel->lock);
        if(wait_status!=DIAMOND_VM_OK) {
            pthread_mutex_unlock(&target_channel->lock);
            return wait_status;
        }
    }
    if(target_channel->closed) {
        pthread_mutex_unlock(&target_channel->lock);
        snprintf(vm->error,sizeof vm->error,"channel is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    /* Rebase from this sender's own ambient classes into
     * the channel's own private (permanent, never-run)
     * program -- identical call shape to DIAMOND_OP_
     * THREAD_NEW's own argument copy above, just into a
     * standing home instead of a freshly spawned child.
     * extra_root_count kept current across the mutation
     * below so a collection triggered by this very copy
     * (on private_vm, still holding `lock`) can find
     * every already-queued value. */
    DiamondValue copied=DIAMOND_NIL;
    target_channel->private_vm->extra_root_count=
        target_channel->count;
    const bool copy_ok=copy_value_into_vm(
        target_channel->private_vm,value_to_send,nullptr,
        chunk->classes,target_channel->private_program->classes,
        nullptr,&copied);
    if(!copy_ok) {
        pthread_mutex_unlock(&target_channel->lock);
        snprintf(vm->error,sizeof vm->error,
            "%s %s","Channel#send argument",copy_failure_reason());
        return DIAMOND_VM_TYPE_ERROR;
    }
    target_channel->queue[target_channel->count++]=copied;
    const uint64_t sequence=++target_channel->sent;
    target_channel->private_vm->extra_root_count=
        target_channel->count;
    notify_channel_waiters(target_channel);
    pthread_cond_signal(&target_channel->not_empty);
    /* A handoff is complete only when a receiver has taken the value. The
     * single slot means the value is still the queue's only entry until
     * then, so giving up (closed, cancelled, out of budget) withdraws it. */
    DiamondVmStatus handoff_status=DIAMOND_VM_OK;
    while(target_channel->rendezvous&&!try_send_method&&
          target_channel->taken<sequence&&!target_channel->closed&&
          handoff_status==DIAMOND_VM_OK)
        handoff_status=channel_wait_bounded(vm,
            &target_channel->not_full,&target_channel->lock);
    if(target_channel->rendezvous&&!try_send_method&&target_channel->taken<sequence) {
        target_channel->count=0;
        target_channel->sent--;
        target_channel->private_vm->extra_root_count=0;
        pthread_cond_broadcast(&target_channel->not_full);
        notify_channel_waiters(target_channel);
        pthread_mutex_unlock(&target_channel->lock);
        if(handoff_status!=DIAMOND_VM_OK)return handoff_status;
        snprintf(vm->error,sizeof vm->error,"channel is closed");
        return DIAMOND_VM_IO_ERROR;
    }
    pthread_mutex_unlock(&target_channel->lock);
    return DIAMOND_VM_OK;
}

/* Channel#receive and #try_receive: takes the oldest queued value, copied into `vm`, waiting for
 * one unless trying; nil once the channel is closed and drained. Part of channel_invoke_helper. */
static DiamondVmStatus channel_receive_helper(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondChannel *target_channel,bool try_receive_method,DiamondValue *result_out) {
    pthread_mutex_lock(&target_channel->lock);
    while(target_channel->count==0&&!target_channel->closed) {
        if(try_receive_method) {
            pthread_mutex_unlock(&target_channel->lock);
            snprintf(vm->error,sizeof vm->error,
                "channel receive would block");
            return DIAMOND_VM_WOULD_BLOCK;
        }
        target_channel->receivers_waiting++;
        if(target_channel->rendezvous)notify_channel_waiters(target_channel);
        const DiamondVmStatus wait_status=channel_wait_bounded(vm,
            &target_channel->not_empty,&target_channel->lock);
        target_channel->receivers_waiting--;
        if(wait_status!=DIAMOND_VM_OK) {
            pthread_mutex_unlock(&target_channel->lock);
            return wait_status;
        }
    }
    if(target_channel->count==0) {
        /* Closed and drained: nil means "nothing ever
         * again," distinct from WouldBlockError's "nothing
         * right now" above -- the identical EOF-as-nil-vs-
         * WouldBlockError distinction File#read/Socket#read
         * already draw (see their own comments). */
        pthread_mutex_unlock(&target_channel->lock);
        *result_out=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    /* Rebase the other direction: out of the channel's own
     * private program, into this receiver's own ambient
     * classes -- identical call shape to Thread#join's own
     * result copy above. Allocates on `vm` (this receiver's
     * own real VM), never on private_vm, so private_vm's
     * own extra_root_count needs no update for this call --
     * only for the queue-mutation just below. */
    DiamondValue copied=DIAMOND_NIL;
    const bool copy_ok=copy_value_into_vm(vm,
        target_channel->queue[0],nullptr,
        target_channel->private_program->classes,chunk->classes,
        nullptr,&copied);
    if(!copy_ok) {
        pthread_mutex_unlock(&target_channel->lock);
        snprintf(vm->error,sizeof vm->error,
            "%s %s","Channel#receive result",copy_failure_reason());
        return DIAMOND_VM_TYPE_ERROR;
    }
    memmove(target_channel->queue,target_channel->queue+1,
        (target_channel->count-1)*sizeof *target_channel->queue);
    target_channel->count--;
    target_channel->taken++;
    target_channel->private_vm->extra_root_count=
        target_channel->count;
    notify_channel_waiters(target_channel);
    if(target_channel->rendezvous)pthread_cond_broadcast(&target_channel->not_full);
    else pthread_cond_signal(&target_channel->not_full);
    pthread_mutex_unlock(&target_channel->lock);
    *result_out=copied;return DIAMOND_VM_OK;
}

DiamondVmStatus channel_invoke_helper(DiamondVm *vm,
                                             const DiamondChunk *chunk, DiamondValue *registers,
                                             uint16_t recv, uint16_t base, uint8_t argc,
                                             uint16_t dest,
                                             const DiamondStringConstant *method_name) {
    DiamondChannel *target_channel=
        ((DiamondChannelHandle *)registers[recv].as.object)->channel;
    const bool wait_readable=method_name->length==13&&
        memcmp(method_name->chars,"wait_readable",13)==0;
    const bool wait_writable=method_name->length==13&&
        memcmp(method_name->chars,"wait_writable",13)==0;
    if(wait_readable||wait_writable) {
        if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
        struct pollfd wait_fd[1];
        const DiamondVmStatus wait_status=cancellable_wait_helper(vm,target_channel,
            wait_writable,registers[base],registers[base+1],wait_fd,0,nullptr);
        if(wait_status!=DIAMOND_VM_OK)return wait_status;
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    const bool send_method=method_name->length==4&&
        memcmp(method_name->chars,"send",4)==0;
    const bool receive_method=method_name->length==7&&
        memcmp(method_name->chars,"receive",7)==0;
    const bool try_send_method=method_name->length==8&&
        memcmp(method_name->chars,"try_send",8)==0;
    const bool try_receive_method=method_name->length==11&&
        memcmp(method_name->chars,"try_receive",11)==0;
    const bool close_method=method_name->length==5&&
        memcmp(method_name->chars,"close",5)==0;
    const bool closed_method=method_name->length==7&&
        memcmp(method_name->chars,"closed?",7)==0;
    const bool size_method=method_name->length==4&&
        memcmp(method_name->chars,"size",4)==0;
    if(!send_method&&!receive_method&&!try_send_method&&
       !try_receive_method&&!close_method&&!closed_method&&!size_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Channel");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(close_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        pthread_mutex_lock(&target_channel->lock);
        /* Idempotent (docs/threads.md), matching Ruby's own
         * Thread::Queue#close -- a second close() is a no-op,
         * not an error. */
        if(!target_channel->closed) {
            target_channel->closed=true;
            notify_channel_waiters(target_channel);
            pthread_cond_broadcast(&target_channel->not_empty);
            pthread_cond_broadcast(&target_channel->not_full);
        }
        pthread_mutex_unlock(&target_channel->lock);
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(closed_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        pthread_mutex_lock(&target_channel->lock);
        const bool is_closed=target_channel->closed;
        pthread_mutex_unlock(&target_channel->lock);
        registers[dest]=DIAMOND_BOOL(is_closed);return DIAMOND_VM_OK;
    }
    if(size_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        pthread_mutex_lock(&target_channel->lock);
        const size_t current_size=target_channel->rendezvous?0:target_channel->count;
        pthread_mutex_unlock(&target_channel->lock);
        registers[dest]=DIAMOND_INT((int64_t)current_size);return DIAMOND_VM_OK;
    }
    if(send_method||try_send_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        const DiamondVmStatus send_status=channel_send_helper(vm,chunk,target_channel,registers[base],
            try_send_method);
        if(send_status!=DIAMOND_VM_OK)return send_status;
        registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    /* Only receive_method/try_receive_method left, per the
     * exhaustive unknown-method check above -- same "fall
     * through to whichever's left" shape Socket#read/#write
     * already uses just below. */
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    return channel_receive_helper(vm,chunk,target_channel,try_receive_method,&registers[dest]);
}

/* Supervisor#restart_policy(options): intensity and backoff, settable only
 * before the first add_child. */
static DiamondVmStatus supervisor_restart_policy_method(DiamondVm *vm,
        DiamondSupervisor *target_supervisor,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    const DiamondValue given=registers[base];
    if(given.kind!=DIAMOND_VALUE_OBJECT||
       given.as.object->kind!=DIAMOND_OBJECT_HASH) {
        snprintf(vm->error,sizeof vm->error,
            "Supervisor#restart_policy expects a Hash of options");
        return DIAMOND_VM_TYPE_ERROR;
    }
    int64_t max_restarts=-1,within_ms=5000,backoff_ms=20,max_backoff_ms=-1;
    bool max_backoff_given=false;
    const DiamondHash *options=(const DiamondHash *)given.as.object;
    for(size_t entry=0;entry<options->count;entry++) {
        const DiamondValue key=options->entries[entry].key;
        const DiamondValue value=options->entries[entry].value;
        if(key.kind!=DIAMOND_VALUE_OBJECT||
           key.as.object->kind!=DIAMOND_OBJECT_STRING) {
            snprintf(vm->error,sizeof vm->error,
                "Supervisor#restart_policy option keys must be Strings");
            return DIAMOND_VM_TYPE_ERROR;
        }
        const DiamondString *name=(const DiamondString *)key.as.object;
        int64_t *target=nullptr;
        int64_t minimum=0,maximum=INT32_MAX;
        if(name->length==12&&memcmp(name->chars,"max_restarts",12)==0) {
            target=&max_restarts;maximum=DIAMOND_MAX_SUPERVISOR_RESTARTS;
        } else if(name->length==9&&memcmp(name->chars,"within_ms",9)==0) {
            target=&within_ms;minimum=1;
        } else if(name->length==10&&memcmp(name->chars,"backoff_ms",10)==0) {
            target=&backoff_ms;minimum=1;
        } else if(name->length==14&&
                  memcmp(name->chars,"max_backoff_ms",14)==0) {
            target=&max_backoff_ms;minimum=1;max_backoff_given=true;
        } else {
            snprintf(vm->error,sizeof vm->error,
                "unknown Supervisor#restart_policy option '%.*s' (expected "
                "max_restarts, within_ms, backoff_ms or max_backoff_ms)",
                (int)name->length,name->chars);
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(value.kind!=DIAMOND_VALUE_INT||value.as.integer<minimum||
           value.as.integer>maximum) {
            snprintf(vm->error,sizeof vm->error,
                "Supervisor#restart_policy '%.*s' must be an Int from %" PRId64
                " to %" PRId64,(int)name->length,name->chars,minimum,maximum);
            return DIAMOND_VM_TYPE_ERROR;
        }
        *target=value.as.integer;
    }
    if(!max_backoff_given)max_backoff_ms=backoff_ms;
    if(max_backoff_ms<backoff_ms) {
        snprintf(vm->error,sizeof vm->error,
            "Supervisor#restart_policy max_backoff_ms must be at least backoff_ms");
        return DIAMOND_VM_TYPE_ERROR;
    }
    pthread_mutex_lock(&target_supervisor->lock);
    if(target_supervisor->child_count!=0) {
        pthread_mutex_unlock(&target_supervisor->lock);
        snprintf(vm->error,sizeof vm->error,
            "Supervisor#restart_policy must be set before the first add_child");
        return DIAMOND_VM_SUPERVISOR_ERROR;
    }
    target_supervisor->max_restarts=max_restarts;
    target_supervisor->within_ms=within_ms;
    target_supervisor->backoff_ms=backoff_ms;
    target_supervisor->max_backoff_ms=max_backoff_ms;
    pthread_mutex_unlock(&target_supervisor->lock);
    registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;
}

/* Supervisor#restart_count/alive?/last_error(index): one child's state. */
static DiamondVmStatus supervisor_child_query_method(DiamondVm *vm,
        DiamondSupervisor *target_supervisor,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,
        bool restart_count_method,bool alive_method) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,
            "Supervisor child index must be an Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t requested_index=registers[base].as.integer;
    pthread_mutex_lock(&target_supervisor->lock);
    if(requested_index<0||
       (size_t)requested_index>=target_supervisor->child_count) {
        pthread_mutex_unlock(&target_supervisor->lock);
        snprintf(vm->error,sizeof vm->error,
            "Supervisor child index %" PRId64 " out of range",
            requested_index);
        return DIAMOND_VM_INDEX_ERROR;
    }
    DiamondSupervisorChild *target_child=
        &target_supervisor->children[(size_t)requested_index];
    if(restart_count_method) {
        const size_t current_restarts=target_child->restart_count;
        pthread_mutex_unlock(&target_supervisor->lock);
        registers[dest]=DIAMOND_INT((int64_t)current_restarts);return DIAMOND_VM_OK;
    }
    if(alive_method) {
        const bool still_alive=!target_child->done;
        pthread_mutex_unlock(&target_supervisor->lock);
        registers[dest]=DIAMOND_BOOL(still_alive);return DIAMOND_VM_OK;
    }
    /* last_error_method: nil until the first crash. Copy
     * the message out before unlocking rather than
     * allocating (a potential GC on `vm`, unrelated to
     * target_supervisor) while still holding the lock. */
    char last_error_copy[sizeof target_child->last_error];
    const bool has_error=target_child->last_error[0]!='\0';
    if(has_error)
        snprintf(last_error_copy,sizeof last_error_copy,
            "%s",target_child->last_error);
    pthread_mutex_unlock(&target_supervisor->lock);
    if(!has_error) {registers[dest]=DIAMOND_NIL;return DIAMOND_VM_OK;}
    DiamondString *message=allocate_string(vm,last_error_copy,
        strlen(last_error_copy));
    if(message==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(message);return DIAMOND_VM_OK;
}

/* Supervisor#stop([timeout_ms]) and #join. */
static DiamondVmStatus supervisor_stop_or_join_method(DiamondVm *vm,
        DiamondSupervisor *target_supervisor,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest,
        bool stop_method,bool join_method) {
    int64_t stop_timeout_ms=-1;
    if(join_method&&argc!=0)return DIAMOND_VM_ARITY_ERROR;
    if(argc>1)return DIAMOND_VM_ARITY_ERROR;
    if(argc==1) {
        const DiamondValue given=registers[base];
        if(given.kind!=DIAMOND_VALUE_INT||given.as.integer<0||
           given.as.integer>INT32_MAX) {
            snprintf(vm->error,sizeof vm->error,
                "Supervisor#stop timeout_ms must be an Int from 0 to %d",
                INT32_MAX);
            return DIAMOND_VM_TYPE_ERROR;
        }
        stop_timeout_ms=given.as.integer;
    }
    if(stop_method) {
        pthread_mutex_lock(&target_supervisor->lock);
        target_supervisor->stopped=true;
        pthread_mutex_unlock(&target_supervisor->lock);
        atomic_store(&target_supervisor->stop_requested,true);
    }
    bool forced=false;
    if(stop_timeout_ms>=0) {
        /* Wait for the current attempts to finish on their own for up to
         * the timeout, then interrupt whatever is still running. The
         * interruption is the sibling-restart one: it lands at the next
         * instruction, so a worker parked in a blocking native call
         * still holds the join below until that call returns. */
        const int64_t deadline_ns=monotonic_nanoseconds()+
            stop_timeout_ms*1000000;
        for(;;) {
            pthread_mutex_lock(&target_supervisor->lock);
            bool all_done=true;
            for(size_t index=0;index<target_supervisor->child_count;index++)
                if(!target_supervisor->children[index].done)all_done=false;
            pthread_mutex_unlock(&target_supervisor->lock);
            if(all_done||monotonic_nanoseconds()>=deadline_ns)break;
            const struct timespec nap={.tv_sec=0,.tv_nsec=1000000L};
            nanosleep(&nap,nullptr);
        }
        pthread_mutex_lock(&target_supervisor->lock);
        for(size_t index=0;index<target_supervisor->child_count;index++) {
            if(target_supervisor->children[index].done)continue;
            forced=true;
            target_supervisor->stop_forced=true;
            supervisor_interrupt_child(&target_supervisor->children[index]);
        }
        pthread_mutex_unlock(&target_supervisor->lock);
    }
    /* Without a timeout there is no cancellation: a child mid-crash-loop
     * still finishes its *current* attempt before noticing
     * stop_requested. join() blocks the same way but without ever
     * setting stop_requested, so it only returns once every child
     * finishes on its own (see docs/threads.md). */
    supervisor_join_all_children(target_supervisor);
    if(join_method) {
        /* A supervisor that gave up escalates here, to whoever waits on
         * it. stop() is cleanup and never raises. */
        pthread_mutex_lock(&target_supervisor->lock);
        const bool gave_up=target_supervisor->failed;
        if(gave_up)
            snprintf(vm->error,sizeof vm->error,"%.1000s",
                target_supervisor->failure_message);
        pthread_mutex_unlock(&target_supervisor->lock);
        if(gave_up)return DIAMOND_VM_SUPERVISOR_ERROR;
    }
    /* stop(timeout_ms) answers whether it had to interrupt anything. */
    registers[dest]=stop_method&&stop_timeout_ms>=0?DIAMOND_BOOL(!forced):DIAMOND_NIL;
    return DIAMOND_VM_OK;
}

/* Supervisor#add_child(callable, args...). Mirrors DIAMOND_OP_THREAD_NEW's
 * own body almost exactly (see its own comments) -- the real differences
 * are storing into a fixed children[] slot instead of a fresh handle,
 * cloning program_template once for the child's entire restart lifetime
 * rather than per spawn, and copying args into args_vm (GC storage only,
 * mirrors Channel's private_vm) instead of directly into a to-be-run
 * child_vm. */
static DiamondVmStatus supervisor_add_child_method(DiamondVm *vm,const DiamondChunk *chunk,
        DiamondSupervisor *target_supervisor,DiamondValue *registers,uint16_t base,
        uint8_t argc,uint16_t dest) {
    if(argc<1)return DIAMOND_VM_ARITY_ERROR;
    pthread_mutex_lock(&target_supervisor->lock);
    if(target_supervisor->stopped) {
        pthread_mutex_unlock(&target_supervisor->lock);
        snprintf(vm->error,sizeof vm->error,
            "Supervisor#add_child called after stop()");
        return DIAMOND_VM_SUPERVISOR_ERROR;
    }
    if(target_supervisor->failed) {
        snprintf(vm->error,sizeof vm->error,"Supervisor#add_child called after the "
            "supervisor gave up: %.900s",target_supervisor->failure_message);
        pthread_mutex_unlock(&target_supervisor->lock);
        return DIAMOND_VM_SUPERVISOR_ERROR;
    }
    if(target_supervisor->child_count>=DIAMOND_MAX_SUPERVISOR_CHILDREN) {
        pthread_mutex_unlock(&target_supervisor->lock);
        snprintf(vm->error,sizeof vm->error,
            "Supervisor has reached its maximum of %d children",
            DIAMOND_MAX_SUPERVISOR_CHILDREN);
        return DIAMOND_VM_SUPERVISOR_ERROR;
    }
    const size_t new_index=target_supervisor->child_count;
    pthread_mutex_unlock(&target_supervisor->lock);
    if(atomic_load(&diamond_active_thread_count)>=diamond_live_thread_limit()) {
        snprintf(vm->error,sizeof vm->error,
            "too many concurrently active threads");
        return DIAMOND_VM_THREAD_ERROR;
    }
    if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
       registers[base].as.object->kind!=DIAMOND_OBJECT_CLOSURE) {
        snprintf(vm->error,sizeof vm->error,
            "Supervisor.add_child's first argument must be a Callable value");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondClosure *callable=
        (const DiamondClosure *)registers[base].as.object;
    if(callable->capture_count!=0) {
        snprintf(vm->error,sizeof vm->error,
            "Supervisor.add_child's callable must not capture any local state");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(callable->foreign_chunk!=nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "a compile_method callable can only be passed to define_method");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if((size_t)callable->function_index>=chunk->function_count)
        return DIAMOND_VM_INVALID_BYTECODE;
    const DiamondFunction *target_fn=
        chunk->functions[callable->function_index];
    const uint8_t forwarded_argc=(uint8_t)(argc-1);
    if(forwarded_argc<target_fn->required_arity||
       (forwarded_argc>target_fn->arity&&!target_fn->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    DiamondProgram *program_template=clone_program_from_chunk(chunk);
    if(program_template==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondVm *args_vm=malloc(sizeof *args_vm);
    if(args_vm==nullptr) {
        diamond_program_free(program_template);free(program_template);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    diamond_vm_init(args_vm);
    /* The child's constants, as of now: args_vm lives as long as the child,
     * and every attempt copies from it (the adding thread's own heap is not
     * safe to read from the child thread). Copied before the arguments for
     * the same GC-rooting reason as in Thread.new. */
    args_vm->spawned_thread=true;
    copy_namespace_constants_into_vm(vm,args_vm,chunk->classes,
        program_template->classes);
    DiamondSupervisorChild *new_child=
        &target_supervisor->children[new_index];
    new_child->supervisor=target_supervisor;
    new_child->index=new_index;
    atomic_init(&new_child->interrupt,false);
    new_child->program_template=program_template;
    new_child->args_vm=args_vm;
    new_child->function_index=callable->function_index;
    new_child->arg_count=forwarded_argc;
    bool copy_failed=false;
    const size_t args_mark=args_vm->gc_protected_count;
    for(uint8_t index=0;index<forwarded_argc;index++) {
        if(!copy_value_into_vm(args_vm,
                registers[(size_t)base+1+index],nullptr,
                chunk->classes,program_template->classes,
                nullptr,&new_child->args[index])||
           !gc_protect(args_vm,new_child->args[index])) {
            copy_failed=true;break;
        }
    }
    new_child->args[forwarded_argc]=DIAMOND_NIL;
    if(!copy_failed&&callable->type_binding_count>0&&
       !copy_value_into_vm(args_vm,registers[base],nullptr,
            chunk->classes,program_template->classes,nullptr,&new_child->args[forwarded_argc]))
        copy_failed=true;
    gc_unprotect(args_vm,args_mark);
    if(copy_failed) {
        diamond_vm_free(args_vm);free(args_vm);
        diamond_program_free(program_template);free(program_template);
        /* memset, not a `(DiamondSupervisorChild){}`
         * compound literal -- see DIAMOND_OP_SUPERVISOR_
         * NEW's own comment on why that matters even for
         * a single child-sized (not full Supervisor-sized)
         * temporary inside this same recursive run_chunk. */
        memset(new_child,0,sizeof *new_child);
        snprintf(vm->error,sizeof vm->error,
            "Supervisor.add_child argument %s",copy_failure_reason());
        return DIAMOND_VM_TYPE_ERROR;
    }
    args_vm->extra_roots=new_child->args;
    args_vm->extra_root_count=(size_t)forwarded_argc+1;
    if(!diamond_reserve_thread_slot()) {
        diamond_vm_free(args_vm);free(args_vm);
        diamond_program_free(program_template);free(program_template);
        memset(new_child,0,sizeof *new_child);
        snprintf(vm->error,sizeof vm->error,"too many concurrently active threads");
        return DIAMOND_VM_THREAD_ERROR;
    }
    /* The child is counted *before* its thread starts, not
     * after: a started child runs immediately, and a sibling
     * that crashes while this thread is still between
     * create_vm_thread and the store below walks
     * `other < child_count` to decide whom its strategy
     * restarts -- publishing the count afterwards let it miss
     * a child that was already running, which then never
     * restarted (a real, rare hang once a 2-CPU CI runner
     * descheduled this thread right here). A child counted
     * but not yet started is harmless: its own attempt clears
     * `interrupt` at start, since starting is the restart. If
     * the thread can't be created the count is rolled back
     * (only this owning thread adds children, so nothing else
     * has seen the slot as real). */
    pthread_mutex_lock(&target_supervisor->lock);
    target_supervisor->child_count=new_index+1;
    pthread_mutex_unlock(&target_supervisor->lock);
    if(create_vm_thread(&new_child->handle,
            supervisor_child_entry_trampoline,new_child)!=0) {
        pthread_mutex_lock(&target_supervisor->lock);
        target_supervisor->child_count=new_index;
        pthread_mutex_unlock(&target_supervisor->lock);
        atomic_fetch_sub(&diamond_active_thread_count,1);
        diamond_vm_free(args_vm);free(args_vm);
        diamond_program_free(program_template);free(program_template);
        memset(new_child,0,sizeof *new_child);
        snprintf(vm->error,sizeof vm->error,"failed to create thread");
        return DIAMOND_VM_THREAD_ERROR;
    }
    registers[dest]=DIAMOND_INT((int64_t)new_index);return DIAMOND_VM_OK;
}

DiamondVmStatus supervisor_invoke_helper(DiamondVm *vm,
                                                const DiamondChunk *chunk,
                                                DiamondValue *registers, uint16_t recv,
                                                uint16_t base, uint8_t argc, uint16_t dest,
                                                const DiamondStringConstant *method_name) {
    DiamondSupervisor *target_supervisor=
        ((DiamondSupervisorHandle *)registers[recv].as.object)->supervisor;
    const bool add_child_method=method_name->length==9&&
        memcmp(method_name->chars,"add_child",9)==0;
    const bool stop_method=method_name->length==4&&
        memcmp(method_name->chars,"stop",4)==0;
    const bool join_method=method_name->length==4&&
        memcmp(method_name->chars,"join",4)==0;
    const bool child_count_method=method_name->length==11&&
        memcmp(method_name->chars,"child_count",11)==0;
    const bool restart_count_method=method_name->length==13&&
        memcmp(method_name->chars,"restart_count",13)==0;
    const bool last_error_method=method_name->length==10&&
        memcmp(method_name->chars,"last_error",10)==0;
    const bool alive_method=method_name->length==6&&
        memcmp(method_name->chars,"alive?",6)==0;
    const bool restart_policy_method=method_name->length==14&&
        memcmp(method_name->chars,"restart_policy",14)==0;
    const bool failed_method=method_name->length==7&&
        memcmp(method_name->chars,"failed?",7)==0;
    if(!add_child_method&&!stop_method&&!join_method&&
       !child_count_method&&!restart_count_method&&
       !last_error_method&&!alive_method&&
       !restart_policy_method&&!failed_method) {
        snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Supervisor");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(failed_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        pthread_mutex_lock(&target_supervisor->lock);
        const bool gave_up=target_supervisor->failed;
        pthread_mutex_unlock(&target_supervisor->lock);
        registers[dest]=DIAMOND_BOOL(gave_up);return DIAMOND_VM_OK;
    }
    if(restart_policy_method) {
        return supervisor_restart_policy_method(vm,target_supervisor,registers,base,argc,dest);
    }
    if(child_count_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        pthread_mutex_lock(&target_supervisor->lock);
        const size_t current_count=target_supervisor->child_count;
        pthread_mutex_unlock(&target_supervisor->lock);
        registers[dest]=DIAMOND_INT((int64_t)current_count);return DIAMOND_VM_OK;
    }
    if(restart_count_method||last_error_method||alive_method) {
        return supervisor_child_query_method(vm,target_supervisor,registers,base,argc,dest,
            restart_count_method,alive_method);
    }
    if(stop_method||join_method) {
        return supervisor_stop_or_join_method(vm,target_supervisor,registers,base,argc,dest,
            stop_method,join_method);
    }
    /* add_child_method, per the exhaustive unknown-method check above. */
    return supervisor_add_child_method(vm,chunk,target_supervisor,registers,base,argc,dest);
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
