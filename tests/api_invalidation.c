/* See src/bignum.c's own identical comment: needed transitively for
 * vm.h's <ucontext.h> use, only under musl (docs/roadmap.md's
 * "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiler.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Exact allocations exercise dispatch at the boundary under ASan too:
 * embedding these in a larger buffer would hide a successor overread. */
static bool check_dispatch_ends(void) {
    static const struct {
        uint8_t code[11];
        size_t size;
        bool falls_off;
    } cases[] = {
        {{DIAMOND_OP_NIL,0,0},3,true},
        {{DIAMOND_OP_CONSTANT,0,0,0,0},5,true},
        {{DIAMOND_OP_JUMP,0,4,UINT8_MAX},4,true},
        {{DIAMOND_OP_JUMP_IF_FALSE,0,0,0,6,UINT8_MAX},6,true},
        {{DIAMOND_OP_JUMP_IF_TRUE,0,0,0,5},5,true},
        {{DIAMOND_OP_CONSTANT,0,0,0,0,DIAMOND_OP_JUMP_IF_TRUE,0,0,0,11,UINT8_MAX},11,true},
        {{DIAMOND_OP_CONSTANT,0,0,0,0,DIAMOND_OP_JUMP_IF_FALSE,0,0,0,10},10,true},
        {{DIAMOND_OP_JUMP,0,4},3,false},
        {{DIAMOND_OP_NIL,0},2,false},
        {{DIAMOND_OP_CONSTANT,0,0,0},4,false},
        {{DIAMOND_OP_NIL,0,0,UINT8_MAX},4,false},
    };
    const DiamondValue constant=DIAMOND_INT(42);
    for(size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        uint8_t *code=malloc(cases[i].size);
        if(code==nullptr)return false;
        memcpy(code,cases[i].code,cases[i].size);
        const DiamondChunk chunk={.name="dispatch end",.code=code,
            .code_count=cases[i].size,.register_count=1,
            .constants=&constant,.constant_count=1};
        DiamondVm vm;
        diamond_vm_init(&vm);
        DiamondValue result=DIAMOND_NIL;
        bool ok=diamond_vm_run(&vm,&chunk,&result)==DIAMOND_VM_INVALID_BYTECODE;
        DiamondFiber *fiber=diamond_fiber_new(&chunk);
        ok=ok&&fiber!=nullptr&&
            diamond_fiber_bind_vm(fiber,&vm)==DIAMOND_FIBER_OK&&
            diamond_fiber_prepare(fiber)==DIAMOND_FIBER_OK&&
            diamond_fiber_resume(fiber,DIAMOND_NIL)==DIAMOND_FIBER_OK;
        if(ok) {
            (void)diamond_fiber_run(fiber);
            ok=diamond_fiber_status(fiber)==(cases[i].falls_off?
                DIAMOND_VM_OK:DIAMOND_VM_INVALID_BYTECODE);
        }
        diamond_fiber_free(fiber);
        diamond_vm_free(&vm);
        free(code);
        if(!ok) {
            fprintf(stderr,"dispatch boundary case %zu failed\n",i);
            return false;
        }
    }
    return true;
}

/* Exercise both captured-store bytecodes. The source compiler materializes
 * capture cells and emits SET_CELL; an equivalent body using SET_CAPTURE
 * covers the direct form used by bytecode producers. Neither may replay its
 * swap when the subsequent return-type check fails. */
static bool check_captured_store_error(bool direct_capture) {
    static const char source[]=
        "def run()\n"
        " a = 1\n b = 2\n"
        " def swap(value) -> Int\n"
        "  previous = a\n  a = b\n  b = previous\n  value\n end\n"
        " begin\n  swap(nil)\n rescue error: TypeError\n  nil\n end\n"
        " [a, b]\nend\nrun()\n";
    static DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if(!diamond_compile(source,&program,&diagnostic)) {
        fprintf(stderr,"captured-store compile failed: %s\n",diagnostic.message);
        diamond_program_free(&program);
        return false;
    }
    DiamondFunction *swap=nullptr;
    for(size_t index=0;index<program.function_count;index++)
        if(strcmp(program.functions[index]->name,"swap")==0)swap=program.functions[index];
    if(swap==nullptr) {diamond_program_free(&program);return false;}
    if(direct_capture) {
        const uint16_t set=swap->return_type_set;
        const uint8_t code[]={
            DIAMOND_OP_GET_CAPTURE,0,1,0,0,
            DIAMOND_OP_GET_CAPTURE,0,2,0,1,
            DIAMOND_OP_SET_CAPTURE,0,0,0,2,
            DIAMOND_OP_SET_CAPTURE,0,1,0,1,
            DIAMOND_OP_CHECK_TYPE,0,0,(uint8_t)(set>>8),(uint8_t)set,
            DIAMOND_OP_RETURN,0,0,
        };
        if(sizeof code>swap->code_count) {diamond_program_free(&program);return false;}
        memcpy(swap->code,code,sizeof code);
        swap->code_count=sizeof code;
    }
    DiamondChunk chunk=diamond_program_chunk(&program);
    DiamondVm vm;
    diamond_vm_init(&vm);
#if (defined(__x86_64__) || defined(_M_X64)) && defined(__GLIBC__)
    vm.jit=true;
#endif
    vm.jit_threshold=1;
    vm.stress_gc=true;
    DiamondValue result=DIAMOND_NIL;
    const DiamondVmStatus status=diamond_vm_run(&vm,&chunk,&result);
    bool ok=status==DIAMOND_VM_OK&&result.kind==DIAMOND_VALUE_OBJECT&&
        result.as.object->kind==DIAMOND_OBJECT_ARRAY;
    if(ok) {
        const DiamondArray *array=(const DiamondArray *)result.as.object;
        ok=array->count==2&&array->values[0].kind==DIAMOND_VALUE_INT&&
            array->values[0].as.integer==2&&array->values[1].kind==DIAMOND_VALUE_INT&&
            array->values[1].as.integer==1;
    }
    if(vm.jit)ok=ok&&swap->jit_code!=nullptr&&vm.jit_bailouts==0;
    if(!ok)fprintf(stderr,"captured-store error test (%s) failed: %s %s\n",
        direct_capture?"SET_CAPTURE":"SET_CELL",diamond_vm_status_name(status),vm.error);
    diamond_vm_free(&vm);
    diamond_program_free(&program);
    return ok;
}

int main(void) {
    if(!check_dispatch_ends())return 20;
    const bool cell_ok=check_captured_store_error(false);
    const bool capture_ok=check_captured_store_error(true);
    if(!cell_ok||!capture_ok)return 21;
    static const char source[] =
        "class Parent\n"
        "  def value()\n"
        "    1\n"
        "  end\n"
        "  def replacement()\n"
        "    2\n"
        "  end\n"
        "end\n"
        "class Child < Parent\n"
        "end\n"
        "def read(object)\n"
        "  object.value()\n"
        "end\n"
        "def read_replacement(object)\n"
        "  object.replacement()\n"
        "end\n"
        "a = Child.new()\n"
        "read(a) + read_replacement(a)\n";
    static DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(source,&program,&diagnostic)) {
        fprintf(stderr,"compile failed: %s\n",diagnostic.message);return 1;
    }
    DiamondChunk chunk=diamond_program_chunk(&program);
    DiamondVm vm;diamond_vm_init(&vm);
    DiamondVm second_vm;diamond_vm_init(&second_vm);
    DiamondValue result=DIAMOND_NIL;
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_OK ||
        result.kind!=DIAMOND_VALUE_INT || result.as.integer!=3) return 2;
    DiamondValue second_result=DIAMOND_NIL;
    if (diamond_vm_run(&second_vm,&chunk,&second_result)!=DIAMOND_VM_OK ||
        second_result.kind!=DIAMOND_VALUE_INT || second_result.as.integer!=3)return 10;
    DiamondClass *klass=nullptr;
    for(size_t index=0;index<program.class_count;index++)
        if(strcmp(program.classes[index]->name,"Parent")==0)klass=program.classes[index];
    if(klass==nullptr||klass->method_count<2)return 3;
    const uint16_t original_value_function=klass->methods[0].function_index;
    DiamondClass *child=nullptr;
    for(size_t index=0;index<program.class_count;index++)
        if(strcmp(program.classes[index]->name,"Child")==0)child=program.classes[index];
    if(child==nullptr)return 7;
    klass->methods[0].function_index=klass->methods[1].function_index;
    diamond_vm_invalidate_method_caches(&vm);
    diamond_vm_invalidate_method_caches(&second_vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_OK ||
        result.kind!=DIAMOND_VALUE_INT || result.as.integer!=4) return 4;
    if (diamond_vm_run(&second_vm,&chunk,&second_result)!=DIAMOND_VM_OK ||
        second_result.kind!=DIAMOND_VALUE_INT || second_result.as.integer!=4)return 11;
    klass->methods[0].arity=1;
    klass->methods[0].required_arity=1;
    diamond_vm_invalidate_method_caches(&vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_ARITY_ERROR)return 5;
    klass->methods[0].arity=0;
    klass->methods[0].required_arity=0;
    klass->methods[1].is_private=true;
    diamond_vm_invalidate_method_caches(&vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_TYPE_ERROR)return 6;
    child->superclass=DIAMOND_NO_CLASS;
    diamond_vm_invalidate_method_caches(&vm);
    if(diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_NO_METHOD_ERROR)return 8;
    child->superclass=klass->index;
    klass->methods[0].function_index=original_value_function;
    klass->methods[1].is_private=false;
    diamond_vm_invalidate_method_caches(&vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_OK ||
        result.kind!=DIAMOND_VALUE_INT || result.as.integer!=3)return 9;
    diamond_vm_free(&vm);
    diamond_vm_free(&second_vm);
    const size_t generated_count=600;
    const size_t generated_capacity=generated_count*32+32;
    char *generated=malloc(generated_capacity);
    if(generated==nullptr)return 12;
    size_t generated_length=0;
    for(size_t index=0;index<generated_count;index++) {
        const int written=snprintf(generated+generated_length,
            generated_capacity-generated_length,"def f%zu()\n %zu\nend\n",
            index,index);
        if(written<0||(size_t)written>=generated_capacity-generated_length) {
            free(generated);return 13;
        }
        generated_length+=(size_t)written;
    }
    memcpy(generated+generated_length,"f599()\n",8);
    generated_length+=7;
    generated[generated_length]='\0';
    diamond_program_free(&program);
    if(!diamond_compile(generated,&program,&diagnostic)) {
        fprintf(stderr,"large compile failed: %s\n",diagnostic.message);
        free(generated);return 14;
    }
    free(generated);
    if(program.function_count!=generated_count)return 15;
    if(program.function_capacity<generated_count||
       program.function_capacity>=DIAMOND_MAX_FUNCTIONS)return 17;
    chunk=diamond_program_chunk(&program);
    DiamondVm large_vm;diamond_vm_init(&large_vm);
    if(diamond_vm_run(&large_vm,&chunk,&result)!=DIAMOND_VM_OK||
       result.kind!=DIAMOND_VALUE_INT||result.as.integer!=599)return 16;
    diamond_vm_free(&large_vm);
    diamond_program_free(&program);
    static const char diagnostic_source[]=
        "class Left\nend\nclass Right\nend\n"
        "def choose(value: Left | Right)\n"
        "  case value\n  when Left\n    1\n  end\nend\n";
    if(diamond_compile(diagnostic_source,&program,&diagnostic))return 18;
    DiamondDiagnostic copied_diagnostic=diagnostic;
    memset(diagnostic.message,'x',sizeof diagnostic.message-1);
    diagnostic.message[sizeof diagnostic.message-1]='\0';
    if(strstr(copied_diagnostic.message,"missing: Right")==nullptr)return 19;
    diamond_program_free(&program);
    puts("api invalidation passed");
    return 0;
}
