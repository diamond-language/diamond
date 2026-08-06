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
    puts("fiber states passed");
    return 0;
}
