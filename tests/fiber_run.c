#include "compiler.h"
#include "vm.h"

#include <stdio.h>

int main(void) {
    static DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile("1 + 2", &program, &diagnostic)) return 1;
    DiamondChunk chunk = diamond_program_chunk(&program);
    DiamondVm vm;
    diamond_vm_init(&vm);
    DiamondFiber *fiber = diamond_fiber_new(&chunk);
    if (fiber == nullptr || diamond_fiber_bind_vm(fiber, &vm) != DIAMOND_FIBER_OK ||
        diamond_fiber_prepare(fiber) != DIAMOND_FIBER_OK ||
        diamond_fiber_resume(fiber) != DIAMOND_FIBER_OK ||
        diamond_fiber_run(fiber) != DIAMOND_FIBER_OK ||
        fiber->state != DIAMOND_FIBER_COMPLETED ||
        diamond_fiber_result(fiber).kind != DIAMOND_VALUE_INT ||
        diamond_fiber_result(fiber).as.integer != 3 ||
        diamond_fiber_status(fiber) != DIAMOND_VM_OK) return 2;
    if (diamond_fiber_run(fiber) != DIAMOND_FIBER_INVALID_STATE) return 3;
    diamond_fiber_free(fiber);
    diamond_vm_free(&vm);

    static DiamondProgram failing_program;
    if (!diamond_compile("1 / 0", &failing_program, &diagnostic)) return 4;
    DiamondChunk failing_chunk = diamond_program_chunk(&failing_program);
    DiamondVm failing_vm;
    diamond_vm_init(&failing_vm);
    DiamondFiber *failing = diamond_fiber_new(&failing_chunk);
    if (failing == nullptr || diamond_fiber_bind_vm(failing, &failing_vm) != DIAMOND_FIBER_OK ||
        diamond_fiber_prepare(failing) != DIAMOND_FIBER_OK ||
        diamond_fiber_resume(failing) != DIAMOND_FIBER_OK ||
        diamond_fiber_run(failing) != DIAMOND_FIBER_OK ||
        failing->state != DIAMOND_FIBER_FAILED ||
        diamond_fiber_status(failing) != DIAMOND_VM_DIVISION_BY_ZERO) return 5;
    diamond_fiber_free(failing);
    diamond_vm_free(&failing_vm);
    puts("fiber run passed");
    return 0;
}
