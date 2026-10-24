#define _GNU_SOURCE

#include "vm_internal.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
