#include "compiler.h"
#include "vm.h"

#include <stdio.h>

int main(void) {
    DiamondProgram program;
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
        fiber->result.kind != DIAMOND_VALUE_INT || fiber->result.as.integer != 3) return 2;
    diamond_fiber_free(fiber);
    diamond_vm_free(&vm);
    puts("fiber run passed");
    return 0;
}
