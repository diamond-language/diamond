/* See src/bignum.c's own identical comment: needed for vm.h's own
 * <ucontext.h> use, only under musl (docs/roadmap.md's "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#include "vm.h"

#include <stdio.h>

int main(void) {
    DiamondFiber *fiber=diamond_fiber_new(nullptr);
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_NEW)return 1;
    if(diamond_fiber_begin(fiber)!=DIAMOND_FIBER_INVALID_STATE)return 2;
    if(diamond_fiber_make_runnable(fiber)!=DIAMOND_FIBER_OK)return 3;
    if(diamond_fiber_begin(fiber)!=DIAMOND_FIBER_OK)return 4;
    if(diamond_fiber_yield(fiber)!=DIAMOND_FIBER_OK)return 5;
    if(diamond_fiber_make_runnable(fiber)!=DIAMOND_FIBER_OK)return 6;
    if(diamond_fiber_resume(fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK)return 7;
    if(diamond_fiber_suspend(fiber)!=DIAMOND_FIBER_OK)return 8;
    if(diamond_fiber_state_name(fiber->state)==nullptr)return 9;
    diamond_fiber_free(fiber);
    DiamondChunk empty_chunk={};
    fiber=diamond_fiber_new(&empty_chunk);
    if(fiber==nullptr||diamond_fiber_prepare(fiber)!=DIAMOND_FIBER_OK||
       fiber->state!=DIAMOND_FIBER_RUNNABLE||
       fiber->stack==nullptr)return 17;
    diamond_fiber_free(fiber);
    DiamondFiberQueue queue;diamond_fiber_queue_init(&queue);
    if(diamond_fiber_scheduler_run_once(&queue)!=DIAMOND_FIBER_INVALID_STATE)return 27;
    DiamondFiber *first=diamond_fiber_new(nullptr),*second=diamond_fiber_new(nullptr);
    if(first==nullptr||second==nullptr||
       diamond_fiber_make_runnable(first)!=DIAMOND_FIBER_OK||
       diamond_fiber_make_runnable(second)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&queue,first)||
       !diamond_fiber_queue_push(&queue,second)||
       diamond_fiber_queue_count(&queue)!=2||
       diamond_fiber_queue_at(&queue,0)!=first||
       diamond_fiber_scheduler_step(&queue)!=first||first->state!=DIAMOND_FIBER_RUNNING||
       diamond_fiber_yield(first)!=DIAMOND_FIBER_OK||
       !diamond_fiber_scheduler_requeue(&queue,first)||
       diamond_fiber_scheduler_step(&queue)!=second||
       diamond_fiber_scheduler_step(&queue)!=first||
       diamond_fiber_queue_pop(&queue)!=nullptr)return 12;
    diamond_fiber_queue_free(&queue);diamond_fiber_free(first);diamond_fiber_free(second);
    diamond_fiber_queue_init(&queue);
    DiamondFiber *batch[12]={};
    for(size_t index=0;index<12;index++) {
        batch[index]=diamond_fiber_new(nullptr);
        if(batch[index]==nullptr||diamond_fiber_make_runnable(batch[index])!=DIAMOND_FIBER_OK||
           !diamond_fiber_queue_push(&queue,batch[index]))return 13;
    }
    for(size_t index=0;index<4;index++) {
        if(diamond_fiber_queue_pop(&queue)!=batch[index])return 14;
        diamond_fiber_free(batch[index]);
    }
    for(size_t index=0;index<4;index++) {
        batch[index]=diamond_fiber_new(nullptr);
        if(batch[index]==nullptr||diamond_fiber_make_runnable(batch[index])!=DIAMOND_FIBER_OK||
           !diamond_fiber_queue_push(&queue,batch[index]))return 15;
    }
    diamond_fiber_queue_free(&queue);
    for(size_t index=0;index<12;index++)diamond_fiber_free(batch[index]);
    fiber=diamond_fiber_new(nullptr);
    if(fiber==nullptr||diamond_fiber_make_runnable(fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_complete(fiber,DIAMOND_INT(42))!=DIAMOND_FIBER_OK||
       fiber->state!=DIAMOND_FIBER_COMPLETED||fiber->result.as.integer!=42)return 10;
    diamond_fiber_free(fiber);
    fiber=diamond_fiber_new(nullptr);
    if(fiber==nullptr||diamond_fiber_make_runnable(fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_fail(fiber,DIAMOND_VM_TYPE_ERROR)!=DIAMOND_FIBER_OK||
       fiber->state!=DIAMOND_FIBER_FAILED)return 11;
    diamond_fiber_free(fiber);

    DiamondFiber *resumable_fiber=diamond_fiber_new(nullptr);
    if(resumable_fiber==nullptr||diamond_fiber_resumable(nullptr)||
       diamond_fiber_resumable(resumable_fiber))return 28;
    if(diamond_fiber_make_runnable(resumable_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_resumable(resumable_fiber))return 29;
    if(diamond_fiber_begin(resumable_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resumable(resumable_fiber))return 30;
    if(diamond_fiber_suspend(resumable_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_resumable(resumable_fiber))return 31;
    if(diamond_fiber_resume(resumable_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_resumable(resumable_fiber))return 32;
    if(diamond_fiber_complete(resumable_fiber,DIAMOND_INT(1))!=DIAMOND_FIBER_OK||
       diamond_fiber_resumable(resumable_fiber))return 33;
    diamond_fiber_free(resumable_fiber);

    DiamondFiber *resumable_failed=diamond_fiber_new(nullptr);
    if(resumable_failed==nullptr||diamond_fiber_make_runnable(resumable_failed)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(resumable_failed)!=DIAMOND_FIBER_OK||
       diamond_fiber_fail(resumable_failed,DIAMOND_VM_TYPE_ERROR)!=DIAMOND_FIBER_OK||
       diamond_fiber_resumable(resumable_failed))return 34;
    diamond_fiber_free(resumable_failed);

    DiamondFiber *resume_new=diamond_fiber_new(nullptr);
    if(resume_new==nullptr||diamond_fiber_resume(resume_new, DIAMOND_NIL)!=DIAMOND_FIBER_INVALID_STATE)return 35;
    diamond_fiber_free(resume_new);

    DiamondFiber *resume_running=diamond_fiber_new(nullptr);
    if(resume_running==nullptr||diamond_fiber_make_runnable(resume_running)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(resume_running)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(resume_running, DIAMOND_NIL)!=DIAMOND_FIBER_INVALID_STATE)return 36;
    diamond_fiber_free(resume_running);

    DiamondFiber *resume_completed=diamond_fiber_new(nullptr);
    if(resume_completed==nullptr||diamond_fiber_make_runnable(resume_completed)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(resume_completed)!=DIAMOND_FIBER_OK||
       diamond_fiber_complete(resume_completed,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(resume_completed, DIAMOND_NIL)!=DIAMOND_FIBER_INVALID_STATE)return 37;
    diamond_fiber_free(resume_completed);

    DiamondFiber *resume_failed=diamond_fiber_new(nullptr);
    if(resume_failed==nullptr||diamond_fiber_make_runnable(resume_failed)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(resume_failed)!=DIAMOND_FIBER_OK||
       diamond_fiber_fail(resume_failed,DIAMOND_VM_TYPE_ERROR)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(resume_failed, DIAMOND_NIL)!=DIAMOND_FIBER_INVALID_STATE)return 38;
    diamond_fiber_free(resume_failed);

    DiamondChunk run_guard_chunk={};
    DiamondVm run_guard_vm;diamond_vm_init(&run_guard_vm);

    DiamondFiber *run_new=diamond_fiber_new(&run_guard_chunk);
    if(run_new==nullptr||diamond_fiber_bind_vm(run_new,&run_guard_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(run_new)!=DIAMOND_FIBER_INVALID_STATE)return 39;
    diamond_fiber_free(run_new);

    DiamondFiber *run_runnable=diamond_fiber_new(&run_guard_chunk);
    if(run_runnable==nullptr||diamond_fiber_bind_vm(run_runnable,&run_guard_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_runnable)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(run_runnable)!=DIAMOND_FIBER_INVALID_STATE)return 40;
    diamond_fiber_free(run_runnable);

    DiamondFiber *run_suspended=diamond_fiber_new(&run_guard_chunk);
    if(run_suspended==nullptr||diamond_fiber_bind_vm(run_suspended,&run_guard_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_suspended)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(run_suspended, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_suspend(run_suspended)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(run_suspended)!=DIAMOND_FIBER_INVALID_STATE)return 41;
    diamond_fiber_free(run_suspended);
    diamond_vm_free(&run_guard_vm);

    DiamondFiber *begin_running=diamond_fiber_new(nullptr);
    if(begin_running==nullptr||diamond_fiber_make_runnable(begin_running)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_running)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_running)!=DIAMOND_FIBER_INVALID_STATE)return 42;
    diamond_fiber_free(begin_running);

    DiamondFiber *begin_suspended=diamond_fiber_new(nullptr);
    if(begin_suspended==nullptr||diamond_fiber_make_runnable(begin_suspended)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_suspended)!=DIAMOND_FIBER_OK||
       diamond_fiber_suspend(begin_suspended)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_suspended)!=DIAMOND_FIBER_INVALID_STATE)return 43;
    diamond_fiber_free(begin_suspended);

    DiamondFiber *begin_completed=diamond_fiber_new(nullptr);
    if(begin_completed==nullptr||diamond_fiber_make_runnable(begin_completed)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_completed)!=DIAMOND_FIBER_OK||
       diamond_fiber_complete(begin_completed,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_completed)!=DIAMOND_FIBER_INVALID_STATE)return 44;
    diamond_fiber_free(begin_completed);

    DiamondFiber *begin_failed=diamond_fiber_new(nullptr);
    if(begin_failed==nullptr||diamond_fiber_make_runnable(begin_failed)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_failed)!=DIAMOND_FIBER_OK||
       diamond_fiber_fail(begin_failed,DIAMOND_VM_TYPE_ERROR)!=DIAMOND_FIBER_OK||
       diamond_fiber_begin(begin_failed)!=DIAMOND_FIBER_INVALID_STATE)return 45;
    diamond_fiber_free(begin_failed);

    puts("fiber states passed");
    return 0;
}
