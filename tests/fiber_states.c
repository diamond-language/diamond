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
    if(diamond_fiber_resume(fiber)!=DIAMOND_FIBER_OK)return 7;
    if(diamond_fiber_suspend(fiber)!=DIAMOND_FIBER_OK)return 8;
    if(diamond_fiber_state_name(fiber->state)==nullptr)return 9;
    DiamondFiberFrame frame={.chunk=nullptr,.instruction=12,.depth=2},popped={};
    if(!diamond_fiber_push_frame(fiber,frame)||!diamond_fiber_update_instruction(fiber,13)||
       !diamond_fiber_update_depth(fiber,3)||
       !diamond_fiber_pop_frame(fiber,&popped)||
       popped.instruction!=13||popped.depth!=3)return 16;
    diamond_fiber_free(fiber);
    DiamondChunk empty_chunk={};
    fiber=diamond_fiber_new(&empty_chunk);
    if(fiber==nullptr||diamond_fiber_prepare(fiber)!=DIAMOND_FIBER_OK||
       fiber->state!=DIAMOND_FIBER_RUNNABLE||
       diamond_fiber_current_frame(fiber)==nullptr||
       diamond_fiber_current_frame(fiber)->instruction!=0||
       diamond_fiber_update_instruction(fiber,1))return 17;
    DiamondValue register_value=DIAMOND_NIL;
    if(diamond_fiber_register_count()!=DIAMOND_REGISTER_COUNT)return 20;
    if(diamond_fiber_get_register(nullptr,0,&register_value)||
       diamond_fiber_get_register(fiber,0,nullptr)||
       diamond_fiber_set_register(nullptr,0,DIAMOND_NIL))return 19;
    if(!diamond_fiber_get_register(fiber,0,&register_value)||
       register_value.kind!=DIAMOND_VALUE_NIL||
       !diamond_fiber_set_register(fiber,0,DIAMOND_INT(42))||
       !diamond_fiber_get_register(fiber,0,&register_value)||
       register_value.kind!=DIAMOND_VALUE_INT||register_value.as.integer!=42||
       diamond_fiber_set_register(fiber,DIAMOND_REGISTER_COUNT,DIAMOND_NIL)||
       diamond_fiber_get_register(fiber,DIAMOND_REGISTER_COUNT,&register_value))return 18;
    DiamondFiberExecutionContext context={};
    if(!diamond_fiber_capture_context(fiber,&context)||
       context.status!=DIAMOND_VM_OK||
       !diamond_fiber_set_register(fiber,0,DIAMOND_INT(7))||
       !diamond_fiber_restore_context(fiber,&context)||
       !diamond_fiber_get_register(fiber,0,&register_value)||
       register_value.as.integer!=42||diamond_fiber_restore_context(fiber,nullptr))return 21;
    context.instruction=2;
    context.chunk=&empty_chunk;
    if(diamond_fiber_restore_context(fiber,&context))return 22;
    context.instruction=0;
    context.status=DIAMOND_VM_EXCEPTION;
    if(diamond_fiber_restore_context(fiber,&context))return 24;
    context.status=DIAMOND_VM_OK;
    context.instruction=0;
    if(!diamond_fiber_context_terminal(&context))return 25;
    DiamondVm vm;diamond_vm_init(&vm);
    DiamondValue vm_result=DIAMOND_NIL;
    if(diamond_vm_run_context(&vm,nullptr,&vm_result)!=DIAMOND_VM_INVALID_BYTECODE||
       diamond_vm_run_context(&vm,&context,&vm_result)!=DIAMOND_VM_INVALID_BYTECODE) return 23;
    context.chunk=&empty_chunk;context.depth=1;context.status=DIAMOND_VM_OK;
    if(diamond_vm_run_context(&vm,&context,&vm_result)!=DIAMOND_VM_INVALID_BYTECODE)return 26;
    diamond_vm_free(&vm);
    diamond_fiber_free(fiber);
    DiamondFiberQueue queue;diamond_fiber_queue_init(&queue);
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
    puts("fiber states passed");
    return 0;
}
