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

    static const uint8_t yield_code[]={DIAMOND_OP_YIELD};
    static const DiamondChunk yield_chunk={.name="yield",.code=yield_code,.code_count=1};
    DiamondFiberExecutionContext yield_context={.chunk=&yield_chunk};
    DiamondVm yield_vm;diamond_vm_init(&yield_vm);
    DiamondValue yield_result=DIAMOND_NIL;
    if(diamond_vm_run_context(&yield_vm,&yield_context,&yield_result)!=DIAMOND_VM_YIELDED||
       yield_context.instruction!=1||yield_context.status!=DIAMOND_VM_YIELDED)return 11;
    if(diamond_vm_run_context(&yield_vm,&yield_context,&yield_result)!=DIAMOND_VM_OK||
       yield_context.instruction!=yield_chunk.code_count||
       yield_context.status!=DIAMOND_VM_OK)return 17;
    DiamondFiber *yield_fiber=diamond_fiber_new(&yield_chunk);
    if(yield_fiber==nullptr||diamond_fiber_bind_vm(yield_fiber,&yield_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(yield_fiber)!=DIAMOND_FIBER_OK||
       yield_fiber->state!=DIAMOND_FIBER_SUSPENDED||
       diamond_fiber_status(yield_fiber)!=DIAMOND_VM_YIELDED)return 12;
    if(diamond_fiber_resume(yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(yield_fiber)!=DIAMOND_FIBER_OK||
       yield_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(yield_fiber)!=DIAMOND_VM_OK)return 13;
    diamond_fiber_free(yield_fiber);
    diamond_vm_free(&yield_vm);

    DiamondVm scheduled_vm;diamond_vm_init(&scheduled_vm);
    DiamondFiberQueue scheduled_queue;diamond_fiber_queue_init(&scheduled_queue);
    DiamondFiber *scheduled_fiber=diamond_fiber_new(&yield_chunk);
    if(scheduled_fiber==nullptr||diamond_fiber_bind_vm(scheduled_fiber,&scheduled_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(scheduled_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&scheduled_queue,scheduled_fiber))return 27;
    if(diamond_fiber_scheduler_run_once(&scheduled_queue)!=DIAMOND_FIBER_OK)return 28;
    if(scheduled_fiber->state!=DIAMOND_FIBER_RUNNABLE||
       diamond_fiber_queue_count(&scheduled_queue)!=1)return 29;
    if(diamond_fiber_scheduler_run_once(&scheduled_queue)!=DIAMOND_FIBER_OK||
       scheduled_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_queue_count(&scheduled_queue)!=0)return 30;
    diamond_fiber_queue_free(&scheduled_queue);diamond_fiber_free(scheduled_fiber);
    diamond_vm_free(&scheduled_vm);
    DiamondVm failed_sched_vm;diamond_vm_init(&failed_sched_vm);
    DiamondFiberQueue failed_sched_queue;diamond_fiber_queue_init(&failed_sched_queue);
    DiamondFiber *failed_sched=diamond_fiber_new(&failing_chunk);
    if(failed_sched==nullptr||diamond_fiber_bind_vm(failed_sched,&failed_sched_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(failed_sched)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&failed_sched_queue,failed_sched)||
       diamond_fiber_scheduler_run_once(&failed_sched_queue)!=DIAMOND_FIBER_OK||
       failed_sched->state!=DIAMOND_FIBER_FAILED||diamond_fiber_queue_count(&failed_sched_queue)!=0)return 31;
    diamond_fiber_queue_free(&failed_sched_queue);diamond_fiber_free(failed_sched);diamond_vm_free(&failed_sched_vm);

    static DiamondProgram yield_program;DiamondDiagnostic yield_diagnostic;
    if(!diamond_compile("yield",&yield_program,&yield_diagnostic))return 14;
    DiamondChunk compiled_yield=diamond_program_chunk(&yield_program);
    bool found_yield=false;
    for(size_t index=0;index<compiled_yield.code_count;index++)
        if(compiled_yield.code[index]==DIAMOND_OP_YIELD)found_yield=true;
    if(!found_yield)return 15;
    DiamondVm compiled_vm;diamond_vm_init(&compiled_vm);
    DiamondFiber *compiled_fiber=diamond_fiber_new(&compiled_yield);
    if(compiled_fiber==nullptr||diamond_fiber_bind_vm(compiled_fiber,&compiled_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(compiled_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(compiled_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(compiled_fiber)!=DIAMOND_FIBER_OK||
       compiled_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 16;
    diamond_fiber_free(compiled_fiber);diamond_vm_free(&compiled_vm);

    static DiamondProgram double_yield_program;DiamondDiagnostic double_yield_diagnostic;
    if(!diamond_compile("yield\nyield",&double_yield_program,&double_yield_diagnostic))return 18;
    DiamondChunk double_yield_chunk=diamond_program_chunk(&double_yield_program);
    size_t yield_count=0;
    for(size_t index=0;index<double_yield_chunk.code_count;index++)
        if(double_yield_chunk.code[index]==DIAMOND_OP_YIELD)yield_count++;
    if(yield_count!=2)return 19;
    DiamondVm double_vm;diamond_vm_init(&double_vm);
    DiamondFiber *double_fiber=diamond_fiber_new(&double_yield_chunk);
    if(double_fiber==nullptr||diamond_fiber_bind_vm(double_fiber,&double_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(double_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(double_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(double_fiber)!=DIAMOND_FIBER_OK||
       double_fiber->state!=DIAMOND_FIBER_SUSPENDED||
       diamond_fiber_current_frame(double_fiber)==nullptr||
       diamond_fiber_current_frame(double_fiber)->instruction==0||
       diamond_fiber_current_frame(double_fiber)->instruction>=double_yield_chunk.code_count)return 20;
    const size_t first_double_instruction=diamond_fiber_current_frame(double_fiber)->instruction;
    if(diamond_fiber_resume(double_fiber)!=DIAMOND_FIBER_OK)return 21;
    if(diamond_fiber_run(double_fiber)!=DIAMOND_FIBER_OK)return 22;
    if(double_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 23;
    if(diamond_fiber_status(double_fiber)!=DIAMOND_VM_YIELDED)return 24;
    if(diamond_fiber_current_frame(double_fiber)->instruction<=first_double_instruction)return 26;
    if(diamond_fiber_resume(double_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(double_fiber)!=DIAMOND_FIBER_OK||
       double_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(double_fiber)!=DIAMOND_VM_OK)return 25;
    diamond_fiber_free(double_fiber);diamond_vm_free(&double_vm);

    DiamondFiberQueue empty_run_all_queue;diamond_fiber_queue_init(&empty_run_all_queue);
    if(diamond_fiber_scheduler_run_all(&empty_run_all_queue)!=DIAMOND_FIBER_OK||
       diamond_fiber_queue_count(&empty_run_all_queue)!=0)return 32;
    diamond_fiber_queue_free(&empty_run_all_queue);
    if(diamond_fiber_scheduler_run_all(nullptr)!=DIAMOND_FIBER_INVALID_STATE)return 33;

    DiamondVm run_all_single_vm;diamond_vm_init(&run_all_single_vm);
    DiamondFiberQueue run_all_single_queue;diamond_fiber_queue_init(&run_all_single_queue);
    DiamondFiber *run_all_single_fiber=diamond_fiber_new(&double_yield_chunk);
    if(run_all_single_fiber==nullptr||
       diamond_fiber_bind_vm(run_all_single_fiber,&run_all_single_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_all_single_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&run_all_single_queue,run_all_single_fiber))return 34;
    if(diamond_fiber_scheduler_run_all(&run_all_single_queue)!=DIAMOND_FIBER_OK||
       run_all_single_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(run_all_single_fiber)!=DIAMOND_VM_OK||
       diamond_fiber_queue_count(&run_all_single_queue)!=0)return 35;
    diamond_fiber_queue_free(&run_all_single_queue);
    diamond_fiber_free(run_all_single_fiber);diamond_vm_free(&run_all_single_vm);

    puts("fiber run passed");
    return 0;
}
