#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"
#include "tls_crypto.h"

#include <errno.h>
#include <inttypes.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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
#ifndef DIAMOND_NO_ZLIB
#include <zlib.h>
#endif

/* Card size for the generational GC's Array/Hash write barrier -- see
 * mark_card_dirty's own comment (below, near gc_write_barrier) for why
 * only these two kinds need index-granularity remembering. Declared
 * this early because mark_remembered_set (just below) already needs it
 * to size its own card-index arithmetic. */
enum { DIAMOND_GC_CARD_SIZE = 64 };

/* The per-kind "walk this object's own direct children" switch,
 * deliberately factored out of mark_object below (which adds the
 * marked/dead-end guards around it) so mark_remembered_set can call
 * this directly for a remembered root -- see that function's own
 * comment on why it must bypass mark_object's guards specifically for
 * a remembered object's own first level, without duplicating this
 * per-kind logic a second time (every native object kind added over
 * many sessions already has to be listed once here; a second copy
 * would just be one more place for a future kind to be missed from). */
static void mark_object_children(DiamondObject *object, bool minor) {
    if (object->kind == DIAMOND_OBJECT_INSTANCE) {
        DiamondInstance *instance=(DiamondInstance *)object;
        for(size_t i=0;i<instance->field_count;i++) mark_value(instance->fields[i],minor);
    } else if(object->kind==DIAMOND_OBJECT_ARRAY) {
        DiamondArray *array=(DiamondArray *)object;
        for(size_t i=0;i<array->count;i++) mark_value(array->values[i],minor);
    } else if(object->kind==DIAMOND_OBJECT_HASH) {
        DiamondHash *hash=(DiamondHash *)object;
        for(size_t i=0;i<hash->count;i++) {
            mark_value(hash->entries[i].key,minor);
            mark_value(hash->entries[i].value,minor);
        }
    } else if(object->kind==DIAMOND_OBJECT_CLOSURE) {
        DiamondClosure *closure=(DiamondClosure *)object;
        for(size_t i=0;i<closure->capture_count;i++)mark_value(closure->captures[i],minor);
    } else if(object->kind==DIAMOND_OBJECT_CELL) {
        mark_value(((DiamondCell *)object)->value,minor);
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
                mark_frame_chain(fiber->native_frames,minor);
            mark_value(fiber->result,minor);
            mark_value(fiber->resume_value,minor);
            if(fiber->entry_closure!=nullptr)
                mark_object((DiamondObject *)fiber->entry_closure,minor);
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
            mark_value(thread->result,minor);
    } else if(object->kind==DIAMOND_OBJECT_PROCESS_RESULT) {
        DiamondProcessResult *result=(DiamondProcessResult *)object;
        mark_value(result->stdout_value,minor);
        mark_value(result->stderr_value,minor);
    } else if(object->kind==DIAMOND_OBJECT_PROCESS_HANDLE) {
        DiamondProcessHandle *handle=(DiamondProcessHandle *)object;
        mark_value(handle->stdout_stream,minor);
        mark_value(handle->stderr_stream,minor);
        mark_value(handle->stdin_stream,minor);
    }
}

/* Marking uses an explicit work stack instead of recursing, so its depth
 * is bounded by heap memory, not by the C stack: a chain of N nested
 * Arrays (or a long linked list of Instances) used to recurse N levels
 * through mark_object -> mark_object_children -> mark_value, and a
 * collection running with a deep enough structure live (a few tens of
 * thousands of levels on a debug build) overflowed the C stack and
 * crashed the process. See the GC issue this fixes in docs/gc-
 * generational-design.md's own notes.
 *
 * thread_local, not per-VM: the mark_* functions take no VM, every Thread
 * runs its own VM's collections on its own OS thread concurrently, and a
 * collection never yields mid-mark (so Fibers sharing one OS thread can't
 * interleave two marks either). mark_roots frees the buffer when its
 * collection ends, so nothing outlives a collection -- no leak when a
 * Thread exits, and no per-mark_object malloc/free churn.
 *
 * `draining` makes the outermost mark_object call the only loop: a nested
 * call (from mark_object_children via mark_value) just marks and pushes.
 * mark_object_children itself is still the single place that knows each
 * object kind's children; this stack deliberately holds every newly
 * marked object (bar Strings/Symbols, which can never have children)
 * rather than duplicating that per-kind list in a "has children"
 * predicate that a future kind could be missed from.
 *
 * If growing the stack fails (out of memory), fall back to the old direct
 * recursion for that object so marking stays correct, just not
 * stack-bounded, exactly as before. */
typedef struct MarkStack {
    DiamondObject **items;
    size_t count;
    size_t capacity;
    bool draining;
} MarkStack;

static thread_local MarkStack diamond_mark_stack;

static void mark_stack_release(void) {
    free(diamond_mark_stack.items);
    diamond_mark_stack=(MarkStack){};
}

void mark_object(DiamondObject *object, bool minor) {
    if (object == nullptr) return;
    /* A minor collection only traces into the young generation -- an
     * old object reached *during trace* (as opposed to being a
     * remembered-set root, which reaches mark_object_children directly
     * instead of coming through here -- see mark_remembered_set) is a
     * dead end: whatever young objects it points to are already
     * covered by it being a member of the remembered set (or it has
     * none, if nothing was written to it since promotion), so
     * recursing into it here would just redo that same work with no
     * correctness need. See docs/design.md's "Generational garbage
     * collection" section. */
    if (minor && object->old) return;
    if (object->marked) return;
    object->marked = true;
    if (object->kind == DIAMOND_OBJECT_STRING ||
        object->kind == DIAMOND_OBJECT_SYMBOL) return;
    MarkStack *stack = &diamond_mark_stack;
    if (stack->count == stack->capacity) {
        const size_t capacity = stack->capacity == 0 ? 256 : stack->capacity * 2;
        DiamondObject **grown = realloc(stack->items, capacity * sizeof *grown);
        if (grown == nullptr) {
            mark_object_children(object, minor);
            return;
        }
        stack->items = grown;
        stack->capacity = capacity;
    }
    stack->items[stack->count++] = object;
    if (stack->draining) return;
    stack->draining = true;
    while (stack->count > 0)
        mark_object_children(stack->items[--stack->count], minor);
    stack->draining = false;
}

void mark_value(DiamondValue value, bool minor) {
    if (value.kind == DIAMOND_VALUE_OBJECT) mark_object(value.as.object, minor);
}

void mark_frame_chain(void *frames, bool minor) {
    for (DiamondFrame *frame = frames; frame != nullptr;
         frame = frame->previous) {
        for (size_t index = 0; index < frame->register_count; index++) {
            mark_value(frame->registers[index], minor);
        }
        for (size_t index = 0; index < frame->extra_root_count; index++)
            mark_value(frame->extra_roots[index], minor);
        if(frame->pending!=nullptr && frame->pending->kind!=PENDING_NONE)
            mark_value(frame->pending->value, minor);
        if(frame->handlers!=nullptr && frame->handler_count!=nullptr)
            for(size_t i=0;i<*frame->handler_count;i++)
                if(frame->handlers[i].kind==HANDLER_ACTIVE_ENSURE)
                    mark_value(frame->handlers[i].saved_pending.value,minor);
    }
}

static void mark_fiber(const DiamondFiber *fiber, bool minor) {
    if (fiber == nullptr) return;
    /* Same staleness hazard as mark_object's DIAMOND_OBJECT_FIBER case
     * above: native_frames is only untrustworthy while this exact fiber is
     * the one currently RUNNING. */
    if (fiber->state != DIAMOND_FIBER_RUNNING)
        mark_frame_chain(fiber->native_frames, minor);
}

/* Every entry in vm->remembered_set (see that field's own comment in
 * vm.h) is an *old* object -- calls mark_object_children directly
 * rather than mark_object, deliberately bypassing both of that
 * function's guards for exactly this one call: the "dead end at an
 * old object" rule exists to stop recursion reaching an old object
 * *transitively* during trace, which must not also block scanning a
 * remembered root's own children (the entire reason it's in the
 * remembered set); and the "already marked" rule is meaningless here
 * since a minor collection never sets `marked` on an old object at
 * all (mark_object's own guard returns before ever reaching that
 * line), so it would always read false anyway.
 *
 * Array/Hash are special-cased to scan only their dirty cards (see
 * mark_card_dirty/gc_write_barrier_index above) instead of every
 * element -- this is the actual Phase 4 fix: the first generational
 * attempt remembered these two kinds at whole-container granularity,
 * which meant re-walking a large, mostly-stable container on *every*
 * minor collection it survived. A card is cleared once scanned (a
 * later write re-dirties it for the next pass); dirty_cards==nullptr
 * means no index write has happened since promotion (or since the last
 * full scan created it), so there's nothing to walk. Falls back to a
 * full mark_object_children scan if dirty_cards is somehow null on an
 * object already in the remembered set (should never happen, since
 * only gc_write_barrier_index/_range ever call remember() on these two
 * kinds, and both always mark a card immediately after -- but a missed
 * scan here would silently free a live young value, so this defaults
 * to the safe side rather than skipping). */
static void mark_remembered_set(DiamondVm *vm) {
    for (size_t index = 0; index < vm->remembered_count; index++) {
        DiamondObject *object = vm->remembered_set[index];
        if (object->kind == DIAMOND_OBJECT_ARRAY) {
            DiamondArray *array = (DiamondArray *)object;
            if (array->dirty_cards == nullptr) {
                mark_object_children(object, true);
                continue;
            }
            const size_t card_count =
                (array->capacity + DIAMOND_GC_CARD_SIZE - 1) / DIAMOND_GC_CARD_SIZE;
            for (size_t card = 0;
                 card < card_count && card < array->dirty_card_capacity; card++) {
                if (!array->dirty_cards[card]) continue;
                const size_t start = card * DIAMOND_GC_CARD_SIZE;
                const size_t end = start + DIAMOND_GC_CARD_SIZE < array->count ?
                    start + DIAMOND_GC_CARD_SIZE : array->count;
                for (size_t i = start; i < end; i++) mark_value(array->values[i], true);
                array->dirty_cards[card] = 0;
            }
        } else if (object->kind == DIAMOND_OBJECT_HASH) {
            DiamondHash *hash = (DiamondHash *)object;
            if (hash->dirty_cards == nullptr) {
                mark_object_children(object, true);
                continue;
            }
            const size_t card_count =
                (hash->capacity + DIAMOND_GC_CARD_SIZE - 1) / DIAMOND_GC_CARD_SIZE;
            for (size_t card = 0;
                 card < card_count && card < hash->dirty_card_capacity; card++) {
                if (!hash->dirty_cards[card]) continue;
                const size_t start = card * DIAMOND_GC_CARD_SIZE;
                const size_t end = start + DIAMOND_GC_CARD_SIZE < hash->count ?
                    start + DIAMOND_GC_CARD_SIZE : hash->count;
                for (size_t i = start; i < end; i++) {
                    mark_value(hash->entries[i].key, true);
                    mark_value(hash->entries[i].value, true);
                }
                hash->dirty_cards[card] = 0;
            }
        } else {
            mark_object_children(object, true);
        }
    }
}

/* Defined near DiamondAdoptedProgram itself (this function is used
 * before that type is declared) -- marks every compile_method
 * bound_value kept alive in vm->adopted_programs, the one place this
 * whole feature needs the collector's help (everything else it added --
 * DiamondClosure.foreign_chunk, DiamondMethod.source_chunk -- is a
 * non-owning reference to something else's already-covered lifetime).
 * A no-op for a ProgramBuilder-only adoption (bound_value_count==0). */
void mark_adopted_programs(void *list, bool minor);

/* The complete root set, minus the remembered set -- shared by both
 * major (minor=false) and minor (minor=true) collection, since a
 * minor collection's own roots are exactly this same walk plus every
 * remembered-set entry (mark_remembered_set, appended at the end when
 * minor is true). Every mark_* call below threads `minor` through so
 * mark_object's own generational dead-end guard applies consistently
 * regardless of which collection triggered this walk. */
static void mark_roots(DiamondVm *vm, bool minor) {
    if(vm->has_exception)mark_value(vm->exception,minor);
    mark_value(vm->nonlocal_value,minor);
    mark_adopted_programs(vm->adopted_programs,minor);
    mark_value(vm->argv_value,minor);
    mark_value(vm->env_value,minor);
    for(size_t index=0;index<vm->gc_protected_count;index++)
        mark_value(vm->gc_protected[index],minor);
    for(size_t index=0;index<vm->namespace_constant_capacity;index++)
        if(vm->namespace_constant_initialized[index])
            mark_value(vm->namespace_constants[index],minor);
    /* Never allocated (see class_variables' own comment in vm.h) until
     * the first SET_CVAR -- nothing to mark if no class variable has
     * ever been written on this VM. Once allocated, no initialized
     * bitmap to gate individual slots on either: every never-assigned
     * slot is just DIAMOND_NIL (calloc's own zero-fill), and mark_value
     * on a nil is already a same-cost no-op check, so marking the full
     * flat array unconditionally costs nothing extra over tracking real
     * per-class counts here would. */
    if(vm->class_variables!=nullptr)
        for(size_t index=0;index<vm->class_variable_extent;index++)
            mark_value(vm->class_variables[index],minor);
    for(size_t index=0;index<DIAMOND_SIGNAL_COUNT;index++)
        mark_value(vm->trapped_signal_handlers[index],minor);
    mark_frame_chain(vm->frames,minor);
    for (DiamondFiber *ancestor = vm->running_fiber; ancestor != nullptr;
         ancestor = ancestor->resumer_fiber)
        mark_frame_chain(ancestor->resumer_frames,minor);
    if (vm->root_queue != nullptr)
        for (size_t index = 0; index < diamond_fiber_queue_count(vm->root_queue); index++)
            mark_fiber(diamond_fiber_queue_at(vm->root_queue, index),minor);
    for(size_t index=0;index<vm->extra_root_count;index++)
        mark_value(vm->extra_roots[index],minor);
    if (minor) mark_remembered_set(vm);
    mark_stack_release();
}

/* Sweeps *list_head, freeing anything left unmarked exactly as the
 * single-generation collector always did (the per-kind cleanup switch
 * below is unchanged from before the generational split -- factored
 * out here so major collection, over both young_objects and
 * old_objects, and minor collection, over young_objects alone, share
 * one copy instead of drifting two independent ones as new native
 * object kinds keep getting added). A survivor (still marked) either
 * stays in *list_head with its mark bit reset (promote_to==nullptr --
 * major collection: it's already exactly where it belongs, in whichever
 * generation it was already in), or is spliced onto *promote_to instead
 * with `old` set true (promote_to!=nullptr -- minor collection
 * promoting a young survivor: unlink from young_objects, relink onto
 * old_objects, no copy -- see docs/design.md's "Generational garbage
 * collection" section on why this is safe and needs no age counter for
 * a non-moving collector). */
static void sweep_list(DiamondVm *vm, DiamondObject **list_head,
        DiamondObject **promote_to) {
    DiamondObject **object = list_head;
    while (*object != nullptr) {
        if ((*object)->marked) {
            (*object)->marked = false;
            if (promote_to != nullptr) {
                DiamondObject *survivor = *object;
                *object = survivor->next;
                survivor->old = true;
                survivor->next = *promote_to;
                *promote_to = survivor;
                continue;
            }
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
            size=sizeof(DiamondInstance)+instance->field_count*sizeof(DiamondValue)+
                (instance->type_binding!=nullptr?sizeof(DiamondTypeBinding):0);
        } else if(unreached->kind==DIAMOND_OBJECT_ARRAY) {
            DiamondArray *array=(DiamondArray *)unreached;
            size=sizeof(DiamondArray)+array->capacity*sizeof(DiamondValue);
            for(size_t index=0;index<array->constraint_count;index++)
                free(array->constraints[index].type_variable_bindings);
            free(array->values);free(array->dirty_cards);
        } else if(unreached->kind==DIAMOND_OBJECT_HASH) {
            DiamondHash *hash=(DiamondHash *)unreached;
            size=sizeof(DiamondHash)+hash->capacity*sizeof(DiamondHashEntry)+
                hash->bucket_capacity*sizeof(size_t);
            for(size_t index=0;index<hash->constraint_count;index++)
                free(hash->constraints[index].type_variable_bindings);
            free(hash->entries);free(hash->buckets);free(hash->dirty_cards);
        } else if(unreached->kind==DIAMOND_OBJECT_CLOSURE) {
            size=sizeof(DiamondClosure)+
                ((DiamondClosure *)unreached)->capture_count*sizeof(DiamondValue);
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
            free(listener->alpn_protocols);
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
                if(!tls_handle->handshake_pending)SSL_shutdown(tls_handle->ssl);
                SSL_free(tls_handle->ssl);
            }
            if(tls_handle->fd>=0)close(tls_handle->fd);
            if(tls_handle->received_session!=nullptr)
                SSL_SESSION_free(tls_handle->received_session);
        } else if(unreached->kind==DIAMOND_OBJECT_BIGNUM) {
            const DiamondBignum *bignum=(const DiamondBignum *)unreached;
            size=sizeof(DiamondBignum)+bignum->limb_count*sizeof(uint32_t);
        } else if(unreached->kind==DIAMOND_OBJECT_SYMBOL) {
            const DiamondSymbol *symbol=(const DiamondSymbol *)unreached;
            size=sizeof(DiamondSymbol)+symbol->length+1;
        } else if(unreached->kind==DIAMOND_OBJECT_REGEXP) {
            size=sizeof(DiamondRegexp);
            reginold_regex_free(((DiamondRegexp *)unreached)->handle);
            free(((DiamondRegexp *)unreached)->source);
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
        } else if(unreached->kind==DIAMOND_OBJECT_CHANNEL) {
            size=sizeof(DiamondChannelHandle);
            free_channel_reference(((DiamondChannelHandle *)unreached)->channel);
        } else if(unreached->kind==DIAMOND_OBJECT_SUPERVISOR) {
            size=sizeof(DiamondSupervisorHandle);
            free_supervisor_reference(
                ((DiamondSupervisorHandle *)unreached)->supervisor);
        } else if(unreached->kind==DIAMOND_OBJECT_SQLITE3) {
            size=sizeof(DiamondSqlite3Handle);
            sqlite3 *db=((DiamondSqlite3Handle *)unreached)->db;
            /* _v2, not plain sqlite3_close: a Statement prepared from this
             * connection (SQLite3#prepare) can outlive the connection's own
             * Diamond-level reachability -- an independent GC object with no
             * back-reference forcing sweep order between the two. Plain
             * sqlite3_close fails (SQLITE_BUSY, unclosed) with any
             * unfinalized statement still outstanding, and this call site
             * never checked that return code, so it would silently leak the
             * real OS-level connection. _v2 never fails: it closes
             * immediately if nothing is outstanding, or defers the actual
             * close until every associated statement is itself finalized
             * (already guaranteed by the SQLITE3_STATEMENT branch just
             * below, and by Statement#close) -- sqlite3's own documented
             * idiom for exactly this ownership shape. */
#ifndef DIAMOND_NO_SQLITE
            if(db!=nullptr)sqlite3_close_v2(db);
#else
            (void)db;
#endif
        } else if(unreached->kind==DIAMOND_OBJECT_SQLITE3_STATEMENT) {
            size=sizeof(DiamondSqlite3StatementHandle);
            sqlite3_stmt *stmt=((DiamondSqlite3StatementHandle *)unreached)->stmt;
#ifndef DIAMOND_NO_SQLITE
            if(stmt!=nullptr)sqlite3_finalize(stmt);
#else
            (void)stmt;
#endif
        } else if(unreached->kind==DIAMOND_OBJECT_POSTGRES) {
            size=sizeof(DiamondPostgresHandle);
            PGconn *conn=((DiamondPostgresHandle *)unreached)->conn;
#ifndef DIAMOND_NO_POSTGRES
            if(conn!=nullptr)PQfinish(conn);
#else
            (void)conn;
#endif
        } else if(unreached->kind==DIAMOND_OBJECT_MYSQL) {
            size=sizeof(DiamondMysqlHandle);
#ifndef DIAMOND_NO_MYSQL
            MYSQL *conn=((DiamondMysqlHandle *)unreached)->conn;
            if(conn!=nullptr)mysql_close(conn);
#endif
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
        } else if(unreached->kind==DIAMOND_OBJECT_PROCESS_STREAM) {
            size=sizeof(DiamondProcessStream);
            const int fd=((DiamondProcessStream *)unreached)->fd;
            if(fd>=0)close(fd);
        } else if(unreached->kind==DIAMOND_OBJECT_PROCESS_HANDLE) {
            size=sizeof(DiamondProcessHandle);
            /* Non-blocking reap only -- a handle the caller dropped
             * without ever calling #wait must not stall this GC pass on
             * some unrelated, possibly long-lived child. If the child
             * hasn't exited yet, it's simply left running, detached from
             * any Diamond-level handle from this point on (same as a
             * shell backgrounding a job and then exiting) -- not
             * reachable for #wait/#kill any more, but not killed either.
             * If it already exited, this reaps it so it doesn't sit
             * around as a zombie forever just because nothing ever
             * called #wait/#running? on its own handle. */
            DiamondProcessHandle *handle=(DiamondProcessHandle *)unreached;
            if(!handle->reaped) {
                int reap_status=0;
                pid_t reap_result=0;
                do {reap_result=waitpid(handle->pid,&reap_status,WNOHANG);}
                while(reap_result<0&&errno==EINTR);
            }
        } else if(unreached->kind==DIAMOND_OBJECT_TENSOR) {
            /* No owned OS resource, no separate allocation -- payload is
             * the struct's own flexible array member, one malloc, freed
             * by the generic free(unreached) below. This branch exists
             * only for accurate bytes_allocated accounting, same as
             * DIAMOND_OBJECT_TIME's own. */
            const DiamondTensor *tensor=(const DiamondTensor *)unreached;
            size=sizeof(DiamondTensor)+tensor->rows*tensor->cols*sizeof(double);
        } else {
            size=sizeof(DiamondCell);
        }
        vm->bytes_allocated -= size;
        free(unreached);
    }
}

/* Adds `owner` to vm->remembered_set if it isn't already there -- see
 * DiamondObject.remembered's own comment for why the caller
 * (gc_write_barrier) only ever calls this once per object's lifetime as
 * an old object. Grows like gc_protected (the closest existing
 * precedent in this file) -- doubling capacity, never shrinks. Returns
 * false only on allocation failure. */
static bool remember(DiamondVm *vm, DiamondObject *owner) {
    if (vm->remembered_count == vm->remembered_capacity) {
        const size_t grown_capacity =
            vm->remembered_capacity == 0 ? 16 : vm->remembered_capacity * 2;
        DiamondObject **grown =
            realloc(vm->remembered_set, grown_capacity * sizeof *grown);
        if (grown == nullptr) return false;
        vm->remembered_set = grown;
        vm->remembered_capacity = grown_capacity;
    }
    vm->remembered_set[vm->remembered_count++] = owner;
    return true;
}

/* The write barrier -- called at every mutation site that stores a
 * value into an already-allocated container/object (see docs/gc-
 * generational-design.md's own audited-sites list; re-verified fresh
 * against current code rather than trusted, since that document's own
 * line numbers are already stale -- src/vm.c's own call sites are the
 * up-to-date authority). Only old->young writes matter: a minor
 * collection's ordinary root walk already sees anything reachable
 * *from* a young object directly (young_objects is swept every minor
 * pass regardless), so this only needs to record the one case that
 * walk wouldn't otherwise catch -- an *old* object gaining a pointer to
 * a young one. Unconditional on the stored value's own generation (or
 * even whether it's an object at all) -- a cheaper branch than checking,
 * over-remembers old->old writes and non-object values, an accepted
 * tradeoff matching the retained design's own reasoning. Returns false
 * only when growing the remembered set failed (remember's own return
 * value) -- callers propagate that as DIAMOND_VM_OUT_OF_MEMORY exactly
 * like any other allocation failure at the same call site, rather than
 * risk silently under-recording a live old->young edge. */
bool gc_write_barrier(DiamondVm *vm, DiamondObject *owner) {
    if (owner->old && !owner->remembered) {
        if (!remember(vm, owner)) return false;
        owner->remembered = true;
    }
    return true;
}

/* Marks the card containing `index` dirty on an old Array or Hash,
 * lazily allocating (or growing) the card table to cover the object's
 * current capacity if needed -- most old Array/Hash objects are never
 * index-written again after promotion, so this stays unallocated in the
 * common case rather than being sized up front for every promotion.
 * Only ever called (via gc_write_barrier_index/_range below) after the
 * caller has already confirmed `owner` is old, so it doesn't re-check
 * that itself. Deliberately not counted in vm->bytes_allocated (unlike
 * values/entries/buckets, which every realloc site already tracks) --
 * a handful of cards is negligible next to the container it shadows
 * (~625 bytes for a 40,000-entry Hash), not worth a second accounting
 * path for. Returns false only on allocation failure. */
/* DiamondArray and DiamondHash lay out dirty_cards/dirty_card_capacity at
 * different offsets (object.h) -- fine at runtime, since owner->kind
 * always picks the matching branch below, but when this function gets
 * inlined at -O3 into a call site where the concrete object is
 * statically known to be one specific (smaller) allocation size, e.g.
 * diamond_vm_set_argv's own Array-only construction, GCC's -Warray-bounds
 * still evaluates the *other*, provably-unreachable-there branch's
 * hash->dirty_cards/hash->capacity access against that size and reports
 * a false positive. Confirmed false: the two branches are never
 * conflated at runtime, only during this particular inlined analysis. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
static bool mark_card_dirty(DiamondObject *owner, size_t index) {
    uint8_t **dirty_cards;size_t *dirty_card_capacity,capacity;
    if(owner->kind==DIAMOND_OBJECT_ARRAY) {
        DiamondArray *array=(DiamondArray *)owner;
        dirty_cards=&array->dirty_cards;
        dirty_card_capacity=&array->dirty_card_capacity;
        capacity=array->capacity;
    } else {
        DiamondHash *hash=(DiamondHash *)owner;
        dirty_cards=&hash->dirty_cards;
        dirty_card_capacity=&hash->dirty_card_capacity;
        capacity=hash->capacity;
    }
    const size_t needed_cards=
        (capacity+DIAMOND_GC_CARD_SIZE-1)/DIAMOND_GC_CARD_SIZE;
    if(*dirty_cards==nullptr) {
        const size_t initial=needed_cards==0?1:needed_cards;
        uint8_t *fresh=calloc(initial,1);
        if(fresh==nullptr)return false;
        *dirty_cards=fresh;*dirty_card_capacity=initial;
    } else if(needed_cards>*dirty_card_capacity) {
        uint8_t *grown=realloc(*dirty_cards,needed_cards);
        if(grown==nullptr)return false;
        memset(grown+*dirty_card_capacity,0,needed_cards-*dirty_card_capacity);
        *dirty_cards=grown;*dirty_card_capacity=needed_cards;
    }
    (*dirty_cards)[index/DIAMOND_GC_CARD_SIZE]=1;
    return true;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* gc_write_barrier's own Array/Hash-specific sibling: same old-object/
 * remembered-set bookkeeping, plus marking the one card the write
 * actually touched dirty (see mark_card_dirty above) so
 * mark_remembered_set (below) can scan just that card on the next minor
 * collection instead of the whole container. */
bool gc_write_barrier_index(DiamondVm *vm, DiamondObject *owner, size_t index) {
    if (!owner->old) return true;
    if (!owner->remembered) {
        if (!remember(vm, owner)) return false;
        owner->remembered = true;
    }
    return mark_card_dirty(owner, index);
}

/* gc_write_barrier_index's range-write sibling (DIAMOND_OP_INDEX_SET's
 * Array range-assignment branch) -- dirties every card touched by
 * [start, start+count). */
bool gc_write_barrier_range(DiamondVm *vm, DiamondObject *owner,
        size_t start, size_t count) {
    if (!owner->old) return true;
    if (!owner->remembered) {
        if (!remember(vm, owner)) return false;
        owner->remembered = true;
    }
    if (count == 0) return true;
    const size_t first_card = start / DIAMOND_GC_CARD_SIZE;
    const size_t last_card = (start + count - 1) / DIAMOND_GC_CARD_SIZE;
    for (size_t card = first_card; card <= last_card; card++)
        if (!mark_card_dirty(owner, card * DIAMOND_GC_CARD_SIZE)) return false;
    return true;
}

static void diamond_vm_collect_impl(DiamondVm *vm) {
    mark_roots(vm, false);
    /* Filter the remembered set by `marked` *before* sweeping below --
     * an entry whose object didn't get marked by the full pass above is
     * genuinely unreachable and about to be freed by the sweep; keeping
     * it here would leave vm->remembered_set holding a dangling pointer
     * once that happens. Confirmed as a real use-after-free during the
     * first implementation attempt at this design, not just a
     * theoretical concern -- see docs/gc-generational-design.md. A
     * surviving old object's own `remembered` bit needs no change here:
     * whatever young object it still points to was necessarily also
     * marked by this same full, unrestricted major pass, so it survives
     * the sweep below too. */
    size_t kept_remembered = 0;
    for (size_t index = 0; index < vm->remembered_count; index++)
        if (vm->remembered_set[index]->marked)
            vm->remembered_set[kept_remembered++] = vm->remembered_set[index];
    vm->remembered_count = kept_remembered;
    sweep_list(vm, &vm->young_objects, nullptr);
    sweep_list(vm, &vm->old_objects, nullptr);
    vm->next_gc = vm->bytes_allocated < 1024
        ? 2048 : vm->bytes_allocated * 2;
    vm->bytes_allocated_at_last_minor_gc = vm->bytes_allocated;
}

/* The minor collector: traces only into the young generation (roots
 * plus the remembered set -- mark_roots(vm,true) covers both), sweeps
 * only vm->young_objects, and promotes every survivor straight onto
 * vm->old_objects (sweep_list's own promote_to parameter) rather than
 * leaving it in place. Deliberately does *not* touch vm->remembered_set
 * at all -- a minor collection never discovers a *dead* old object
 * (old_objects is never swept here), so there is nothing to filter;
 * only a major collection's full liveness pass over the old generation
 * can determine that, which is exactly why the remembered-set filtering
 * lives in diamond_vm_collect_impl instead. */
static void diamond_vm_collect_minor_impl(DiamondVm *vm) {
    mark_roots(vm, true);
    sweep_list(vm, &vm->young_objects, &vm->old_objects);
    vm->bytes_allocated_at_last_minor_gc = vm->bytes_allocated;
}

/* Timing wrapper around diamond_vm_collect_impl (the major collector)
 * -- see vm.h's own comment on gc_major_collection_count/
 * gc_major_total_seconds for why this exists (DIAMOND_TRACE_GC,
 * src/run_source.c). CLOCK_MONOTONIC, matching every other wall-time
 * measurement in this file (Time.monotonic(), the TCP/TLS/UDP retry-
 * loop deadline checks) -- immune to wall-clock adjustments, which a
 * long-running collection-heavy process is exactly the kind of thing
 * that could otherwise run across. */
void diamond_vm_collect(DiamondVm *vm) {
    struct timespec start={};
    clock_gettime(CLOCK_MONOTONIC,&start);
    diamond_vm_collect_impl(vm);
    struct timespec end={};
    clock_gettime(CLOCK_MONOTONIC,&end);
    vm->gc_major_collection_count++;
    vm->gc_major_total_seconds+=
        (double)(end.tv_sec-start.tv_sec)+
        (double)(end.tv_nsec-start.tv_nsec)/1e9;
}

/* Same timing wrapper, over diamond_vm_collect_minor_impl instead --
 * see vm.h's own comment on gc_minor_collection_count/
 * gc_minor_total_seconds. Public (not static), matching
 * diamond_vm_collect's own visibility, so a test can force a
 * deterministic minor collection directly the same way several existing
 * tests already call diamond_vm_collect (tests/fiber_run.c). */
void diamond_vm_collect_minor(DiamondVm *vm) {
    struct timespec start={};
    clock_gettime(CLOCK_MONOTONIC,&start);
    diamond_vm_collect_minor_impl(vm);
    struct timespec end={};
    clock_gettime(CLOCK_MONOTONIC,&end);
    vm->gc_minor_collection_count++;
    vm->gc_minor_total_seconds+=
        (double)(end.tv_sec-start.tv_sec)+
        (double)(end.tv_nsec-start.tv_nsec)/1e9;
}
