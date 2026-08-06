#include "vm.h"

#include <stdio.h>

int main(void) {
    DiamondFiber *fiber=diamond_fiber_new(nullptr);
    if(fiber==nullptr||fiber->state!=DIAMOND_FIBER_NEW)return 1;
    if(diamond_fiber_begin(fiber)!=DIAMOND_FIBER_INVALID_STATE)return 2;
    if(diamond_fiber_make_runnable(fiber)!=DIAMOND_FIBER_OK)return 3;
    if(diamond_fiber_begin(fiber)!=DIAMOND_FIBER_OK)return 4;
    if(diamond_fiber_suspend(fiber)!=DIAMOND_FIBER_OK)return 5;
    if(diamond_fiber_make_runnable(fiber)!=DIAMOND_FIBER_OK)return 6;
    if(diamond_fiber_begin(fiber)!=DIAMOND_FIBER_OK)return 7;
    if(diamond_fiber_suspend(fiber)!=DIAMOND_FIBER_OK)return 8;
    if(diamond_fiber_state_name(fiber->state)==nullptr)return 9;
    diamond_fiber_free(fiber);
    DiamondFiberQueue queue;diamond_fiber_queue_init(&queue);
    DiamondFiber *first=diamond_fiber_new(nullptr),*second=diamond_fiber_new(nullptr);
    if(first==nullptr||second==nullptr||
       diamond_fiber_make_runnable(first)!=DIAMOND_FIBER_OK||
       diamond_fiber_make_runnable(second)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&queue,first)||
       !diamond_fiber_queue_push(&queue,second)||
       diamond_fiber_queue_pop(&queue)!=first||
       diamond_fiber_queue_pop(&queue)!=second||
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
