#include "compiler.h"
#include "vm.h"

#include <stdio.h>

int main(void) {
    static DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (diamond_fiber_result(nullptr).kind != DIAMOND_VALUE_NIL ||
        diamond_fiber_status(nullptr) != DIAMOND_VM_INVALID_BYTECODE) return 6;
    if (!diamond_compile("1 + 2", &program, &diagnostic)) return 1;
    DiamondChunk chunk = diamond_program_chunk(&program);
    DiamondFiberFrame checkpoint;
    DiamondFiberExecutionContext context;
    DiamondValue context_result=DIAMOND_NIL;
    DiamondVm vm;
    diamond_vm_init(&vm);
    context=(DiamondFiberExecutionContext){.chunk=&chunk};
    context.registers[7]=DIAMOND_INT(99);
    if(diamond_vm_run_context(&vm,&context,&context_result)!=DIAMOND_VM_OK||
       context_result.kind!=DIAMOND_VALUE_INT||context_result.as.integer!=3||
       context.instruction!=chunk.code_count||context.status!=DIAMOND_VM_OK||
       context.registers[7].kind!=DIAMOND_VALUE_INT||context.registers[7].as.integer!=99)return 7;
    if(!diamond_fiber_context_terminal(&context)||diamond_fiber_context_terminal(nullptr))return 9;
    DiamondFiber *fiber = diamond_fiber_new(&chunk);
    if (fiber == nullptr || diamond_fiber_bind_vm(fiber, &vm) != DIAMOND_FIBER_OK ||
        diamond_fiber_prepare(fiber) != DIAMOND_FIBER_OK ||
        !diamond_fiber_set_register(fiber, 7, DIAMOND_INT(99)) ||
        diamond_fiber_resume(fiber) != DIAMOND_FIBER_OK ||
        diamond_fiber_run(fiber) != DIAMOND_FIBER_OK ||
        fiber->state != DIAMOND_FIBER_COMPLETED ||
        diamond_fiber_result(fiber).kind != DIAMOND_VALUE_INT ||
        diamond_fiber_result(fiber).as.integer != 3 ||
        diamond_fiber_status(fiber) != DIAMOND_VM_OK ||
        diamond_fiber_current_frame(fiber) == nullptr ||
        diamond_fiber_current_frame(fiber)->instruction != chunk.code_count ||
        !diamond_fiber_checkpoint(fiber, &checkpoint) ||
        checkpoint.instruction != chunk.code_count ||
        checkpoint.registers[7].kind != DIAMOND_VALUE_INT ||
        checkpoint.registers[7].as.integer != 99 ||
        !diamond_fiber_capture_context(fiber, &context) ||
        context.instruction != chunk.code_count ||
        diamond_fiber_restore_context(fiber, &context) ||
        diamond_fiber_checkpoint(fiber, nullptr)) return 2;
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
    DiamondFiberExecutionContext failing_context={.chunk=&failing_chunk};
    DiamondValue failing_result=DIAMOND_NIL;
    if(diamond_vm_run_context(&failing_vm,&failing_context,&failing_result)!=
       DIAMOND_VM_DIVISION_BY_ZERO||failing_context.instruction>failing_chunk.code_count||
       failing_context.status!=DIAMOND_VM_DIVISION_BY_ZERO)return 8;
    if(!diamond_fiber_context_terminal(&failing_context))return 10;
    diamond_fiber_free(failing);
    diamond_vm_free(&failing_vm);
    puts("fiber run passed");
    return 0;
}
