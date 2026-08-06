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
    DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(source,&program,&diagnostic)) {
        fprintf(stderr,"compile failed: %s\n",diagnostic.message);return 1;
    }
    DiamondChunk chunk=diamond_program_chunk(&program);
    DiamondVm vm;diamond_vm_init(&vm);
    DiamondValue result=DIAMOND_NIL;
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_OK ||
        result.kind!=DIAMOND_VALUE_INT || result.as.integer!=3) return 2;
    DiamondClass *klass=nullptr;
    for(size_t index=0;index<program.class_count;index++)
        if(strcmp(program.classes[index].name,"Parent")==0)klass=&program.classes[index];
    if(klass==nullptr||klass->method_count<2)return 3;
    klass->methods[0].function_index=klass->methods[1].function_index;
    diamond_vm_invalidate_method_caches(&vm);
    if (diamond_vm_run(&vm,&chunk,&result)!=DIAMOND_VM_OK ||
        result.kind!=DIAMOND_VALUE_INT || result.as.integer!=4) return 4;
    diamond_vm_free(&vm);
    puts("api invalidation passed");
    return 0;
}
