#define _DEFAULT_SOURCE

#include "compiler.h"
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    static DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (diamond_fiber_result(nullptr).kind != DIAMOND_VALUE_NIL ||
        diamond_fiber_status(nullptr) != DIAMOND_VM_INVALID_BYTECODE) return 1;
    if (!diamond_compile("1 + 2", &program, &diagnostic)) return 2;
    DiamondChunk chunk = diamond_program_chunk(&program);
    DiamondVm vm;
    diamond_vm_init(&vm);
    DiamondFiber *fiber = diamond_fiber_new(&chunk);
    if (fiber == nullptr || diamond_fiber_bind_vm(fiber, &vm) != DIAMOND_FIBER_OK ||
        diamond_fiber_prepare(fiber) != DIAMOND_FIBER_OK ||
        diamond_fiber_resume(fiber, DIAMOND_NIL) != DIAMOND_FIBER_OK ||
        diamond_fiber_run(fiber) != DIAMOND_FIBER_OK ||
        fiber->state != DIAMOND_FIBER_COMPLETED ||
        diamond_fiber_result(fiber).kind != DIAMOND_VALUE_INT ||
        diamond_fiber_result(fiber).as.integer != 3 ||
        diamond_fiber_status(fiber) != DIAMOND_VM_OK) return 3;
    if (diamond_fiber_run(fiber) != DIAMOND_FIBER_INVALID_STATE) return 4;
    diamond_fiber_free(fiber);
    diamond_vm_free(&vm);

    static DiamondProgram failing_program;
    if (!diamond_compile("1 / 0", &failing_program, &diagnostic)) return 5;
    DiamondChunk failing_chunk = diamond_program_chunk(&failing_program);
    DiamondVm failing_vm;
    diamond_vm_init(&failing_vm);
    DiamondFiber *failing = diamond_fiber_new(&failing_chunk);
    if (failing == nullptr || diamond_fiber_bind_vm(failing, &failing_vm) != DIAMOND_FIBER_OK ||
        diamond_fiber_prepare(failing) != DIAMOND_FIBER_OK ||
        diamond_fiber_resume(failing, DIAMOND_NIL) != DIAMOND_FIBER_OK ||
        diamond_fiber_run(failing) != DIAMOND_FIBER_OK ||
        failing->state != DIAMOND_FIBER_FAILED ||
        diamond_fiber_status(failing) != DIAMOND_VM_DIVISION_BY_ZERO) return 6;
    diamond_fiber_free(failing);
    diamond_vm_free(&failing_vm);

    static const uint8_t yield_code[]={DIAMOND_OP_YIELD,0,0};
    static const DiamondChunk yield_chunk={.name="yield",.code=yield_code,.code_count=3};
    DiamondVm yield_vm;diamond_vm_init(&yield_vm);
    DiamondFiber *yield_fiber=diamond_fiber_new(&yield_chunk);
    if(yield_fiber==nullptr||diamond_fiber_bind_vm(yield_fiber,&yield_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(yield_fiber)!=DIAMOND_FIBER_OK||
       yield_fiber->state!=DIAMOND_FIBER_SUSPENDED||
       diamond_fiber_status(yield_fiber)!=DIAMOND_VM_YIELDED)return 7;
    if(diamond_fiber_resume(yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(yield_fiber)!=DIAMOND_FIBER_OK||
       yield_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(yield_fiber)!=DIAMOND_VM_OK)return 8;
    diamond_fiber_free(yield_fiber);
    diamond_vm_free(&yield_vm);

    DiamondVm scheduled_vm;diamond_vm_init(&scheduled_vm);
    DiamondFiberQueue scheduled_queue;diamond_fiber_queue_init(&scheduled_queue);
    DiamondFiber *scheduled_fiber=diamond_fiber_new(&yield_chunk);
    if(scheduled_fiber==nullptr||diamond_fiber_bind_vm(scheduled_fiber,&scheduled_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(scheduled_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&scheduled_queue,scheduled_fiber))return 9;
    if(diamond_fiber_scheduler_run_once(&scheduled_queue)!=DIAMOND_FIBER_OK)return 10;
    if(scheduled_fiber->state!=DIAMOND_FIBER_RUNNABLE||
       diamond_fiber_queue_count(&scheduled_queue)!=1)return 11;
    if(diamond_fiber_scheduler_run_once(&scheduled_queue)!=DIAMOND_FIBER_OK||
       scheduled_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_queue_count(&scheduled_queue)!=0)return 12;
    diamond_fiber_queue_free(&scheduled_queue);diamond_fiber_free(scheduled_fiber);
    diamond_vm_free(&scheduled_vm);
    DiamondVm failed_sched_vm;diamond_vm_init(&failed_sched_vm);
    DiamondFiberQueue failed_sched_queue;diamond_fiber_queue_init(&failed_sched_queue);
    DiamondFiber *failed_sched=diamond_fiber_new(&failing_chunk);
    if(failed_sched==nullptr||diamond_fiber_bind_vm(failed_sched,&failed_sched_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(failed_sched)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&failed_sched_queue,failed_sched)||
       diamond_fiber_scheduler_run_once(&failed_sched_queue)!=DIAMOND_FIBER_OK||
       failed_sched->state!=DIAMOND_FIBER_FAILED||diamond_fiber_queue_count(&failed_sched_queue)!=0)return 13;
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
       diamond_fiber_resume(compiled_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(compiled_fiber)!=DIAMOND_FIBER_OK||
       compiled_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 16;
    if(diamond_fiber_result(compiled_fiber).kind!=DIAMOND_VALUE_NIL)return 71;
    diamond_fiber_free(compiled_fiber);diamond_vm_free(&compiled_vm);

    static DiamondProgram double_yield_program;DiamondDiagnostic double_yield_diagnostic;
    if(!diamond_compile("yield\nyield",&double_yield_program,&double_yield_diagnostic))return 17;
    DiamondChunk double_yield_chunk=diamond_program_chunk(&double_yield_program);
    size_t yield_count=0;
    for(size_t index=0;index<double_yield_chunk.code_count;index++)
        if(double_yield_chunk.code[index]==DIAMOND_OP_YIELD)yield_count++;
    if(yield_count!=2)return 18;
    DiamondVm double_vm;diamond_vm_init(&double_vm);
    DiamondFiber *double_fiber=diamond_fiber_new(&double_yield_chunk);
    if(double_fiber==nullptr||diamond_fiber_bind_vm(double_fiber,&double_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(double_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(double_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(double_fiber)!=DIAMOND_FIBER_OK||
       double_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 19;
    if(diamond_fiber_resume(double_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK)return 20;
    if(diamond_fiber_run(double_fiber)!=DIAMOND_FIBER_OK)return 21;
    if(double_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 22;
    if(diamond_fiber_status(double_fiber)!=DIAMOND_VM_YIELDED)return 23;
    if(diamond_fiber_resume(double_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(double_fiber)!=DIAMOND_FIBER_OK||
       double_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(double_fiber)!=DIAMOND_VM_OK)return 24;
    diamond_fiber_free(double_fiber);diamond_vm_free(&double_vm);

    DiamondFiberQueue empty_run_all_queue;diamond_fiber_queue_init(&empty_run_all_queue);
    if(diamond_fiber_scheduler_run_all(&empty_run_all_queue)!=DIAMOND_FIBER_OK||
       diamond_fiber_queue_count(&empty_run_all_queue)!=0)return 25;
    diamond_fiber_queue_free(&empty_run_all_queue);
    if(diamond_fiber_scheduler_run_all(nullptr)!=DIAMOND_FIBER_INVALID_STATE)return 26;

    DiamondVm run_all_single_vm;diamond_vm_init(&run_all_single_vm);
    DiamondFiberQueue run_all_single_queue;diamond_fiber_queue_init(&run_all_single_queue);
    DiamondFiber *run_all_single_fiber=diamond_fiber_new(&double_yield_chunk);
    if(run_all_single_fiber==nullptr||
       diamond_fiber_bind_vm(run_all_single_fiber,&run_all_single_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_all_single_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&run_all_single_queue,run_all_single_fiber))return 27;
    if(diamond_fiber_scheduler_run_all(&run_all_single_queue)!=DIAMOND_FIBER_OK||
       run_all_single_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(run_all_single_fiber)!=DIAMOND_VM_OK||
       diamond_fiber_queue_count(&run_all_single_queue)!=0)return 28;
    diamond_fiber_queue_free(&run_all_single_queue);
    diamond_fiber_free(run_all_single_fiber);diamond_vm_free(&run_all_single_vm);

    DiamondVm run_all_multi_vm_a;diamond_vm_init(&run_all_multi_vm_a);
    DiamondVm run_all_multi_vm_b;diamond_vm_init(&run_all_multi_vm_b);
    DiamondFiberQueue run_all_multi_queue;diamond_fiber_queue_init(&run_all_multi_queue);
    DiamondFiber *run_all_multi_a=diamond_fiber_new(&yield_chunk);
    DiamondFiber *run_all_multi_b=diamond_fiber_new(&double_yield_chunk);
    if(run_all_multi_a==nullptr||run_all_multi_b==nullptr||
       diamond_fiber_bind_vm(run_all_multi_a,&run_all_multi_vm_a)!=DIAMOND_FIBER_OK||
       diamond_fiber_bind_vm(run_all_multi_b,&run_all_multi_vm_b)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_all_multi_a)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_all_multi_b)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&run_all_multi_queue,run_all_multi_a)||
       !diamond_fiber_queue_push(&run_all_multi_queue,run_all_multi_b))return 29;
    if(diamond_fiber_scheduler_run_all(&run_all_multi_queue)!=DIAMOND_FIBER_OK||
       run_all_multi_a->state!=DIAMOND_FIBER_COMPLETED||
       run_all_multi_b->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_queue_count(&run_all_multi_queue)!=0)return 30;
    diamond_fiber_queue_free(&run_all_multi_queue);
    diamond_fiber_free(run_all_multi_a);diamond_fiber_free(run_all_multi_b);
    diamond_vm_free(&run_all_multi_vm_a);diamond_vm_free(&run_all_multi_vm_b);

    DiamondVm run_all_fail_vm;diamond_vm_init(&run_all_fail_vm);
    DiamondVm run_all_ok_vm;diamond_vm_init(&run_all_ok_vm);
    DiamondFiberQueue run_all_fail_queue;diamond_fiber_queue_init(&run_all_fail_queue);
    DiamondFiber *run_all_fail_fiber=diamond_fiber_new(&failing_chunk);
    DiamondFiber *run_all_ok_fiber=diamond_fiber_new(&yield_chunk);
    if(run_all_fail_fiber==nullptr||run_all_ok_fiber==nullptr||
       diamond_fiber_bind_vm(run_all_fail_fiber,&run_all_fail_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_bind_vm(run_all_ok_fiber,&run_all_ok_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_all_fail_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(run_all_ok_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&run_all_fail_queue,run_all_fail_fiber)||
       !diamond_fiber_queue_push(&run_all_fail_queue,run_all_ok_fiber))return 31;
    if(diamond_fiber_scheduler_run_all(&run_all_fail_queue)!=DIAMOND_FIBER_OK||
       run_all_fail_fiber->state!=DIAMOND_FIBER_FAILED||
       diamond_fiber_status(run_all_fail_fiber)!=DIAMOND_VM_DIVISION_BY_ZERO||
       run_all_ok_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_queue_count(&run_all_fail_queue)!=0)return 32;
    diamond_fiber_queue_free(&run_all_fail_queue);
    diamond_fiber_free(run_all_fail_fiber);diamond_fiber_free(run_all_ok_fiber);
    diamond_vm_free(&run_all_fail_vm);diamond_vm_free(&run_all_ok_vm);

    static const DiamondStringConstant gc_root_strings[]={{.chars="root-marked",.length=11}};
    static const uint8_t gc_root_code[]={DIAMOND_OP_STRING,0,0,DIAMOND_OP_YIELD,1,0,DIAMOND_OP_RETURN,0};
    static const DiamondChunk gc_root_chunk={.name="gc-root",.code=gc_root_code,
        .code_count=8,.strings=gc_root_strings,.string_count=1};
    DiamondVm gc_root_vm;diamond_vm_init(&gc_root_vm);
    DiamondFiberQueue gc_root_queue;diamond_fiber_queue_init(&gc_root_queue);
    DiamondFiber *gc_root_fiber=diamond_fiber_new(&gc_root_chunk);
    if(gc_root_fiber==nullptr||
       diamond_fiber_bind_vm(gc_root_fiber,&gc_root_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(gc_root_fiber)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&gc_root_queue,gc_root_fiber))return 33;
    diamond_vm_bind_fiber_queue(&gc_root_vm,&gc_root_queue);
    if(diamond_fiber_scheduler_run_once(&gc_root_queue)!=DIAMOND_FIBER_OK||
       gc_root_fiber->state!=DIAMOND_FIBER_RUNNABLE||
       diamond_fiber_status(gc_root_fiber)!=DIAMOND_VM_YIELDED)return 34;
    diamond_vm_collect(&gc_root_vm);
    if(diamond_fiber_scheduler_run_once(&gc_root_queue)!=DIAMOND_FIBER_OK||
       gc_root_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(gc_root_fiber)!=DIAMOND_VM_OK)return 35;
    DiamondValue gc_root_value=diamond_fiber_result(gc_root_fiber);
    if(gc_root_value.kind!=DIAMOND_VALUE_OBJECT)return 36;
    const DiamondString *gc_root_string=(const DiamondString *)gc_root_value.as.object;
    if(gc_root_string->length!=11||memcmp(gc_root_string->chars,"root-marked",11)!=0)return 37;
    diamond_fiber_queue_free(&gc_root_queue);diamond_fiber_free(gc_root_fiber);
    diamond_vm_free(&gc_root_vm);

    static const DiamondStringConstant unbound_gc_strings[]={
        {.chars="alpha",.length=5},{.chars="beta",.length=4}};
    static const uint8_t unbound_gc_code[]={
        DIAMOND_OP_STRING,0,0,DIAMOND_OP_YIELD,3,0,DIAMOND_OP_STRING,1,1,
        DIAMOND_OP_ARRAY,2,0,2,DIAMOND_OP_RETURN,2};
    static const DiamondChunk unbound_gc_chunk={.name="unbound-gc",.code=unbound_gc_code,
        .code_count=15,.strings=unbound_gc_strings,.string_count=2};
    DiamondVm unbound_gc_vm;diamond_vm_init(&unbound_gc_vm);
    unbound_gc_vm.stress_gc=true;
    DiamondFiber *unbound_gc_fiber=diamond_fiber_new(&unbound_gc_chunk);
    if(unbound_gc_fiber==nullptr||
       diamond_fiber_bind_vm(unbound_gc_fiber,&unbound_gc_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(unbound_gc_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(unbound_gc_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(unbound_gc_fiber)!=DIAMOND_FIBER_OK||
       unbound_gc_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 38;
    if(diamond_fiber_resume(unbound_gc_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(unbound_gc_fiber)!=DIAMOND_FIBER_OK||
       unbound_gc_fiber->state!=DIAMOND_FIBER_COMPLETED)return 39;
    DiamondValue unbound_result=diamond_fiber_result(unbound_gc_fiber);
    if(unbound_result.kind!=DIAMOND_VALUE_OBJECT)return 40;
    const DiamondArray *unbound_array=(const DiamondArray *)unbound_result.as.object;
    if(unbound_array->count!=2||
       unbound_array->values[0].kind!=DIAMOND_VALUE_OBJECT||
       unbound_array->values[1].kind!=DIAMOND_VALUE_OBJECT)return 41;
    const DiamondString *unbound_first_string=(const DiamondString *)unbound_array->values[0].as.object;
    const DiamondString *unbound_second_string=(const DiamondString *)unbound_array->values[1].as.object;
    if(unbound_first_string->length!=5||memcmp(unbound_first_string->chars,"alpha",5)!=0||
       unbound_second_string->length!=4||memcmp(unbound_second_string->chars,"beta",4)!=0)return 42;
    diamond_fiber_free(unbound_gc_fiber);diamond_vm_free(&unbound_gc_vm);

    static const DiamondStringConstant shared_gc_strings_a[]={{.chars="fiber-a-value",.length=13}};
    static const uint8_t shared_gc_code_a[]={DIAMOND_OP_STRING,0,0,DIAMOND_OP_YIELD,1,0,DIAMOND_OP_RETURN,0};
    static const DiamondChunk shared_gc_chunk_a={.name="shared-gc-a",.code=shared_gc_code_a,
        .code_count=8,.strings=shared_gc_strings_a,.string_count=1};
    static const DiamondStringConstant shared_gc_strings_b[]={{.chars="fiber-b-value",.length=13}};
    static const uint8_t shared_gc_code_b[]={DIAMOND_OP_STRING,0,0,DIAMOND_OP_YIELD,1,0,DIAMOND_OP_RETURN,0};
    static const DiamondChunk shared_gc_chunk_b={.name="shared-gc-b",.code=shared_gc_code_b,
        .code_count=8,.strings=shared_gc_strings_b,.string_count=1};
    DiamondVm shared_gc_vm;diamond_vm_init(&shared_gc_vm);
    shared_gc_vm.stress_gc=true;
    DiamondFiberQueue shared_gc_queue;diamond_fiber_queue_init(&shared_gc_queue);
    diamond_vm_bind_fiber_queue(&shared_gc_vm,&shared_gc_queue);
    DiamondFiber *shared_gc_a=diamond_fiber_new(&shared_gc_chunk_a);
    DiamondFiber *shared_gc_b=diamond_fiber_new(&shared_gc_chunk_b);
    if(shared_gc_a==nullptr||shared_gc_b==nullptr||
       diamond_fiber_bind_vm(shared_gc_a,&shared_gc_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_bind_vm(shared_gc_b,&shared_gc_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(shared_gc_a)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(shared_gc_b)!=DIAMOND_FIBER_OK||
       !diamond_fiber_queue_push(&shared_gc_queue,shared_gc_a)||
       !diamond_fiber_queue_push(&shared_gc_queue,shared_gc_b))return 43;
    if(diamond_fiber_scheduler_run_all(&shared_gc_queue)!=DIAMOND_FIBER_OK||
       shared_gc_a->state!=DIAMOND_FIBER_COMPLETED||
       shared_gc_b->state!=DIAMOND_FIBER_COMPLETED)return 44;
    DiamondValue shared_gc_a_value=diamond_fiber_result(shared_gc_a);
    DiamondValue shared_gc_b_value=diamond_fiber_result(shared_gc_b);
    if(shared_gc_a_value.kind!=DIAMOND_VALUE_OBJECT||shared_gc_b_value.kind!=DIAMOND_VALUE_OBJECT)return 45;
    const DiamondString *shared_gc_a_string=(const DiamondString *)shared_gc_a_value.as.object;
    const DiamondString *shared_gc_b_string=(const DiamondString *)shared_gc_b_value.as.object;
    if(shared_gc_a_string->length!=13||memcmp(shared_gc_a_string->chars,"fiber-a-value",13)!=0||
       shared_gc_b_string->length!=13||memcmp(shared_gc_b_string->chars,"fiber-b-value",13)!=0)return 46;
    diamond_fiber_queue_free(&shared_gc_queue);
    diamond_fiber_free(shared_gc_a);diamond_fiber_free(shared_gc_b);
    diamond_vm_free(&shared_gc_vm);

    static DiamondProgram nested_yield_program;DiamondDiagnostic nested_yield_diagnostic;
    if(!diamond_compile("def inner()\n yield\n 5\nend\ninner() + 100",
        &nested_yield_program,&nested_yield_diagnostic))return 47;
    DiamondChunk nested_yield_chunk=diamond_program_chunk(&nested_yield_program);
    DiamondVm nested_yield_vm;diamond_vm_init(&nested_yield_vm);
    DiamondFiber *nested_yield_fiber=diamond_fiber_new(&nested_yield_chunk);
    if(nested_yield_fiber==nullptr||
       diamond_fiber_bind_vm(nested_yield_fiber,&nested_yield_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(nested_yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(nested_yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(nested_yield_fiber)!=DIAMOND_FIBER_OK)return 54;
    if(nested_yield_fiber->state!=DIAMOND_FIBER_SUSPENDED||
       diamond_fiber_status(nested_yield_fiber)!=DIAMOND_VM_YIELDED)return 55;
    if(diamond_fiber_resume(nested_yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(nested_yield_fiber)!=DIAMOND_FIBER_OK||
       nested_yield_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(nested_yield_fiber)!=DIAMOND_VM_OK)return 56;
    DiamondValue nested_yield_result=diamond_fiber_result(nested_yield_fiber);
    if(nested_yield_result.kind!=DIAMOND_VALUE_INT||nested_yield_result.as.integer!=105)return 57;
    diamond_fiber_free(nested_yield_fiber);diamond_vm_free(&nested_yield_vm);

    DiamondVm nested_yield_direct_vm;diamond_vm_init(&nested_yield_direct_vm);
    DiamondValue nested_yield_direct_result=DIAMOND_NIL;
    if(diamond_vm_run(&nested_yield_direct_vm,&nested_yield_chunk,&nested_yield_direct_result)!=
       DIAMOND_VM_YIELD_WITHOUT_FIBER)return 48;
    const char *nested_yield_direct_error=diamond_vm_error(&nested_yield_direct_vm);
    if(nested_yield_direct_error==nullptr||
       strstr(nested_yield_direct_error,"yield outside a fiber")==nullptr)return 49;
    diamond_vm_free(&nested_yield_direct_vm);

    static DiamondProgram rescue_yield_program;DiamondDiagnostic rescue_yield_diagnostic;
    if(!diamond_compile("begin\n yield\n raise \"boom\"\nrescue error: String\n error\nend",
        &rescue_yield_program,&rescue_yield_diagnostic))return 58;
    DiamondChunk rescue_yield_chunk=diamond_program_chunk(&rescue_yield_program);
    DiamondVm rescue_yield_vm;diamond_vm_init(&rescue_yield_vm);
    DiamondFiber *rescue_yield_fiber=diamond_fiber_new(&rescue_yield_chunk);
    if(rescue_yield_fiber==nullptr||
       diamond_fiber_bind_vm(rescue_yield_fiber,&rescue_yield_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(rescue_yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(rescue_yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(rescue_yield_fiber)!=DIAMOND_FIBER_OK)return 59;
    if(rescue_yield_fiber->state!=DIAMOND_FIBER_SUSPENDED||
       diamond_fiber_status(rescue_yield_fiber)!=DIAMOND_VM_YIELDED)return 60;
    if(diamond_fiber_resume(rescue_yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(rescue_yield_fiber)!=DIAMOND_FIBER_OK||
       rescue_yield_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(rescue_yield_fiber)!=DIAMOND_VM_OK)return 61;
    DiamondValue rescue_yield_result=diamond_fiber_result(rescue_yield_fiber);
    if(rescue_yield_result.kind!=DIAMOND_VALUE_OBJECT)return 62;
    const DiamondString *rescue_yield_string=(const DiamondString *)rescue_yield_result.as.object;
    if(rescue_yield_string->length!=4||memcmp(rescue_yield_string->chars,"boom",4)!=0)return 63;
    diamond_fiber_free(rescue_yield_fiber);diamond_vm_free(&rescue_yield_vm);

    static DiamondProgram post_call_yield_program;DiamondDiagnostic post_call_yield_diagnostic;
    if(!diamond_compile("def helper()\n 1\nend\nhelper()\nyield",
        &post_call_yield_program,&post_call_yield_diagnostic))return 50;
    DiamondChunk post_call_yield_chunk=diamond_program_chunk(&post_call_yield_program);
    DiamondVm post_call_yield_vm;diamond_vm_init(&post_call_yield_vm);
    DiamondFiber *post_call_yield_fiber=diamond_fiber_new(&post_call_yield_chunk);
    if(post_call_yield_fiber==nullptr||
       diamond_fiber_bind_vm(post_call_yield_fiber,&post_call_yield_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(post_call_yield_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(post_call_yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(post_call_yield_fiber)!=DIAMOND_FIBER_OK)return 51;
    if(post_call_yield_fiber->state!=DIAMOND_FIBER_SUSPENDED||
       diamond_fiber_status(post_call_yield_fiber)!=DIAMOND_VM_YIELDED)return 52;
    if(diamond_fiber_resume(post_call_yield_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(post_call_yield_fiber)!=DIAMOND_FIBER_OK||
       post_call_yield_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(post_call_yield_fiber)!=DIAMOND_VM_OK)return 53;
    diamond_fiber_free(post_call_yield_fiber);diamond_vm_free(&post_call_yield_vm);

    static DiamondProgram fiber_depth_program;DiamondDiagnostic fiber_depth_diagnostic;
    if(!diamond_compile("def depth(n)\n if n <= 0\n  0\n else\n  depth(n - 1) + 1\n end\nend\ndepth(90)",
        &fiber_depth_program,&fiber_depth_diagnostic))return 64;
    DiamondChunk fiber_depth_chunk=diamond_program_chunk(&fiber_depth_program);
    DiamondVm fiber_depth_vm;diamond_vm_init(&fiber_depth_vm);
    DiamondFiber *fiber_depth_fiber=diamond_fiber_new(&fiber_depth_chunk);
    if(fiber_depth_fiber==nullptr||
       diamond_fiber_bind_vm(fiber_depth_fiber,&fiber_depth_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(fiber_depth_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(fiber_depth_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(fiber_depth_fiber)!=DIAMOND_FIBER_OK)return 65;
    if(fiber_depth_fiber->state!=DIAMOND_FIBER_COMPLETED||
       diamond_fiber_status(fiber_depth_fiber)!=DIAMOND_VM_OK)return 66;
    DiamondValue fiber_depth_result=diamond_fiber_result(fiber_depth_fiber);
    if(fiber_depth_result.kind!=DIAMOND_VALUE_INT||fiber_depth_result.as.integer!=90)return 67;
    diamond_fiber_free(fiber_depth_fiber);diamond_vm_free(&fiber_depth_vm);

    static DiamondProgram fiber_overflow_program;DiamondDiagnostic fiber_overflow_diagnostic;
    if(!diamond_compile("def depth(n)\n if n <= 0\n  0\n else\n  depth(n - 1) + 1\n end\nend\ndepth(5000)",
        &fiber_overflow_program,&fiber_overflow_diagnostic))return 68;
    DiamondChunk fiber_overflow_chunk=diamond_program_chunk(&fiber_overflow_program);
    DiamondVm fiber_overflow_vm;diamond_vm_init(&fiber_overflow_vm);
    DiamondFiber *fiber_overflow_fiber=diamond_fiber_new(&fiber_overflow_chunk);
    if(fiber_overflow_fiber==nullptr||
       diamond_fiber_bind_vm(fiber_overflow_fiber,&fiber_overflow_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(fiber_overflow_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(fiber_overflow_fiber, DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(fiber_overflow_fiber)!=DIAMOND_FIBER_OK)return 69;
    if(fiber_overflow_fiber->state!=DIAMOND_FIBER_FAILED||
       diamond_fiber_status(fiber_overflow_fiber)!=DIAMOND_VM_STACK_OVERFLOW)return 70;
    diamond_fiber_free(fiber_overflow_fiber);diamond_vm_free(&fiber_overflow_vm);

    DiamondVm sweep_vm;diamond_vm_init(&sweep_vm);
    DiamondFiber *sweep_fiber=diamond_fiber_new(nullptr);
    DiamondFiberHandle *sweep_handle=malloc(sizeof *sweep_handle);
    if(sweep_fiber==nullptr||sweep_handle==nullptr)return 71;
    *sweep_handle=(DiamondFiberHandle){.object={.next=sweep_vm.objects,.kind=DIAMOND_OBJECT_FIBER},
        .fiber=sweep_fiber};
    sweep_vm.objects=&sweep_handle->object;
    diamond_vm_collect(&sweep_vm);
    if(sweep_vm.objects!=nullptr)return 72;
    diamond_vm_free(&sweep_vm);

    static const DiamondStringConstant survive_strings[]={{.chars="fiber-survives-gc",.length=18}};
    static const uint8_t survive_code[]={DIAMOND_OP_STRING,0,0,DIAMOND_OP_YIELD,1,0,DIAMOND_OP_RETURN,0};
    static const DiamondChunk survive_chunk={.name="survive-gc",.code=survive_code,
        .code_count=8,.strings=survive_strings,.string_count=1};
    DiamondVm survive_vm;diamond_vm_init(&survive_vm);
    DiamondFiber *survive_fiber=diamond_fiber_new(&survive_chunk);
    DiamondFiberHandle *survive_handle=malloc(sizeof *survive_handle);
    if(survive_fiber==nullptr||survive_handle==nullptr||
       diamond_fiber_bind_vm(survive_fiber,&survive_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(survive_fiber)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(survive_fiber,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(survive_fiber)!=DIAMOND_FIBER_OK||
       survive_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 73;
    *survive_handle=(DiamondFiberHandle){.object={.next=survive_vm.objects,.kind=DIAMOND_OBJECT_FIBER},
        .fiber=survive_fiber};
    survive_vm.objects=&survive_handle->object;
    survive_vm.has_exception=true;
    survive_vm.exception=(DiamondValue){.kind=DIAMOND_VALUE_OBJECT,.as.object=&survive_handle->object};
    diamond_vm_collect(&survive_vm);
    survive_vm.has_exception=false;
    bool handle_survived=false;
    for(DiamondObject *object=survive_vm.objects;object!=nullptr;object=object->next)
        if(object==&survive_handle->object)handle_survived=true;
    if(!handle_survived)return 74;
    DiamondValue survive_result=diamond_fiber_result(survive_fiber);
    if(survive_result.kind!=DIAMOND_VALUE_OBJECT)return 75;
    const DiamondString *survive_string=(const DiamondString *)survive_result.as.object;
    if(survive_string->length!=18||memcmp(survive_string->chars,"fiber-survives-gc",18)!=0)return 76;
    diamond_fiber_free(survive_fiber);diamond_vm_free(&survive_vm);

    static const uint8_t stack_probe_code[]={DIAMOND_OP_RETURN,0};
    static const DiamondChunk stack_probe_chunk={.name="stack-probe",
        .code=stack_probe_code,.code_count=1};
    DiamondVm stack_probe_vm;diamond_vm_init(&stack_probe_vm);
    DiamondFiber *stack_probe_fiber=diamond_fiber_new(&stack_probe_chunk);
    DiamondFiberHandle *stack_probe_handle=malloc(sizeof *stack_probe_handle);
    if(stack_probe_fiber==nullptr||stack_probe_handle==nullptr||
       diamond_fiber_prepare(stack_probe_fiber)!=DIAMOND_FIBER_OK)return 77;
    void *stack_probe_addr=stack_probe_fiber->stack;
    const size_t stack_probe_page=(size_t)sysconf(_SC_PAGESIZE);
    unsigned char stack_probe_vec[1];
    if(mincore(stack_probe_addr,stack_probe_page,stack_probe_vec)!=0)return 78;
    *stack_probe_handle=(DiamondFiberHandle){
        .object={.next=stack_probe_vm.objects,.kind=DIAMOND_OBJECT_FIBER},
        .fiber=stack_probe_fiber};
    stack_probe_vm.objects=&stack_probe_handle->object;
    diamond_vm_collect(&stack_probe_vm);
    if(stack_probe_vm.objects!=nullptr)return 79;
    errno=0;
    if(mincore(stack_probe_addr,stack_probe_page,stack_probe_vec)!=-1||errno!=ENOMEM)return 80;
    diamond_vm_free(&stack_probe_vm);

    static const DiamondStringConstant resumer_hazard_strings[]={
        {.chars="resumer-only-value",.length=18}};
    static const uint8_t resumer_hazard_code[]={
        DIAMOND_OP_STRING,0,0,DIAMOND_OP_YIELD,1,0,DIAMOND_OP_RETURN,0};
    static const DiamondChunk resumer_hazard_chunk={.name="resumer-hazard-a",
        .code=resumer_hazard_code,.code_count=8,
        .strings=resumer_hazard_strings,.string_count=1};
    static const DiamondStringConstant resumer_hazard_child_strings[]={
        {.chars="child-value",.length=11}};
    static const uint8_t resumer_hazard_child_code[]={DIAMOND_OP_STRING,0,0,DIAMOND_OP_RETURN,0};
    static const DiamondChunk resumer_hazard_child_chunk={.name="resumer-hazard-b",
        .code=resumer_hazard_child_code,.code_count=5,
        .strings=resumer_hazard_child_strings,.string_count=1};
    DiamondVm resumer_hazard_vm;diamond_vm_init(&resumer_hazard_vm);
    DiamondFiber *resumer_hazard_a=diamond_fiber_new(&resumer_hazard_chunk);
    DiamondFiber *resumer_hazard_b=diamond_fiber_new(&resumer_hazard_child_chunk);
    if(resumer_hazard_a==nullptr||resumer_hazard_b==nullptr||
       diamond_fiber_bind_vm(resumer_hazard_a,&resumer_hazard_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_bind_vm(resumer_hazard_b,&resumer_hazard_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(resumer_hazard_a)!=DIAMOND_FIBER_OK||
       diamond_fiber_prepare(resumer_hazard_b)!=DIAMOND_FIBER_OK)return 81;
    if(diamond_fiber_resume(resumer_hazard_a,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(resumer_hazard_a)!=DIAMOND_FIBER_OK||
       resumer_hazard_a->state!=DIAMOND_FIBER_SUSPENDED)return 82;
    /* Simulate A being the live resumer of nested fiber B, exactly as
     * Phase D's .resume() dispatch will do from inside DIAMOND_OP_INVOKE:
     * A's own frame chain is "current" while B runs and allocates. */
    resumer_hazard_vm.stress_gc=true;
    resumer_hazard_vm.frames=resumer_hazard_a->native_frames;
    resumer_hazard_vm.running_fiber=resumer_hazard_a;
    if(diamond_fiber_resume(resumer_hazard_b,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(resumer_hazard_b)!=DIAMOND_FIBER_OK||
       resumer_hazard_b->state!=DIAMOND_FIBER_COMPLETED)return 83;
    resumer_hazard_vm.frames=nullptr;
    resumer_hazard_vm.running_fiber=nullptr;
    resumer_hazard_vm.stress_gc=false;
    if(diamond_fiber_resume(resumer_hazard_a,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(resumer_hazard_a)!=DIAMOND_FIBER_OK||
       resumer_hazard_a->state!=DIAMOND_FIBER_COMPLETED)return 84;
    DiamondValue resumer_hazard_result=diamond_fiber_result(resumer_hazard_a);
    if(resumer_hazard_result.kind!=DIAMOND_VALUE_OBJECT)return 85;
    const DiamondString *resumer_hazard_string=(const DiamondString *)resumer_hazard_result.as.object;
    if(resumer_hazard_string->length!=18||
       memcmp(resumer_hazard_string->chars,"resumer-only-value",18)!=0)return 86;
    diamond_fiber_free(resumer_hazard_a);diamond_fiber_free(resumer_hazard_b);
    diamond_vm_free(&resumer_hazard_vm);

    static DiamondProgram nested_fiber_new_program;DiamondDiagnostic nested_fiber_new_diagnostic;
    if(!diamond_compile(
        "def level3()\n"
        " def once()\n"
        "  x = yield(10)\n"
        "  x + 5\n"
        " end\n"
        " once\n"
        "end\n"
        "def level2()\n"
        " level3()\n"
        "end\n"
        "def level1()\n"
        " Fiber.new(level2())\n"
        "end\n"
        "level1()",
        &nested_fiber_new_program,&nested_fiber_new_diagnostic))return 87;
    DiamondChunk nested_fiber_new_chunk=diamond_program_chunk(&nested_fiber_new_program);
    DiamondVm nested_fiber_new_vm;diamond_vm_init(&nested_fiber_new_vm);
    DiamondValue nested_fiber_new_result=DIAMOND_NIL;
    /* level1/level2/level3's own call frames -- including whatever
     * transient stack-local `child` chunk was active at the FIBER_NEW
     * site -- are long gone by the time this returns, since Fiber.new
     * only constructs the fiber; nothing has resumed it yet. */
    if(diamond_vm_run(&nested_fiber_new_vm,&nested_fiber_new_chunk,
                       &nested_fiber_new_result)!=DIAMOND_VM_OK)return 88;
    if(nested_fiber_new_result.kind!=DIAMOND_VALUE_OBJECT||
       nested_fiber_new_result.as.object->kind!=DIAMOND_OBJECT_FIBER)return 89;
    DiamondFiberHandle *nested_fiber_new_handle=
        (DiamondFiberHandle *)nested_fiber_new_result.as.object;
    DiamondFiber *nested_fiber_new_fiber=nested_fiber_new_handle->fiber;
    if(diamond_fiber_bind_vm(nested_fiber_new_fiber,&nested_fiber_new_vm)!=DIAMOND_FIBER_OK||
       diamond_fiber_resume(nested_fiber_new_fiber,DIAMOND_NIL)!=DIAMOND_FIBER_OK||
       diamond_fiber_run(nested_fiber_new_fiber)!=DIAMOND_FIBER_OK||
       nested_fiber_new_fiber->state!=DIAMOND_FIBER_SUSPENDED)return 90;
    if(diamond_fiber_resume(nested_fiber_new_fiber,DIAMOND_INT(7))!=DIAMOND_FIBER_OK||
       diamond_fiber_run(nested_fiber_new_fiber)!=DIAMOND_FIBER_OK||
       nested_fiber_new_fiber->state!=DIAMOND_FIBER_COMPLETED)return 91;
    DiamondValue nested_fiber_new_final=diamond_fiber_result(nested_fiber_new_fiber);
    if(nested_fiber_new_final.kind!=DIAMOND_VALUE_INT||nested_fiber_new_final.as.integer!=12)return 92;
    diamond_fiber_free(nested_fiber_new_fiber);diamond_vm_free(&nested_fiber_new_vm);

    puts("fiber run passed");
    return 0;
}
