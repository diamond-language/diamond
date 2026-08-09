#include "compiler.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <string.h>

int main(void) {
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
        if(strcmp(program.classes[index].name,"Parent")==0)klass=&program.classes[index];
    if(klass==nullptr||klass->method_count<2)return 3;
    const uint8_t original_value_function=klass->methods[0].function_index;
    DiamondClass *child=nullptr;
    for(size_t index=0;index<program.class_count;index++)
        if(strcmp(program.classes[index].name,"Child")==0)child=&program.classes[index];
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
    child->superclass=UINT8_MAX;
    diamond_vm_invalidate_method_caches(&vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_TYPE_ERROR)return 8;
    child->superclass=(uint8_t)(klass-program.classes);
    klass->methods[0].function_index=original_value_function;
    klass->methods[1].is_private=false;
    diamond_vm_invalidate_method_caches(&vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_OK ||
        result.kind!=DIAMOND_VALUE_INT || result.as.integer!=3)return 9;
    diamond_vm_free(&vm);
    diamond_vm_free(&second_vm);
    puts("api invalidation passed");
    return 0;
}
