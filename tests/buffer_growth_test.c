/* Include the two implementations to exercise private growth paths with
 * synthetic near-SIZE_MAX metadata, without adding production test APIs. */
#include "../src/jit.c"
#include "../src/loader.c"
#include "vm_internal.h"

#include <assert.h>

static void test_jit_growth(void) {
    JitBuffer buffer={};
    for(size_t i=0;i<600;i++)emit_u8(&buffer,(uint8_t)i);
    assert(!buffer.failed&&buffer.length==600&&buffer.capacity>=600);
    for(size_t i=0;i<600;i++)assert(buffer.code[i]==(uint8_t)i);
    free(buffer.code);

    buffer=(JitBuffer){.length=SIZE_MAX,.capacity=SIZE_MAX};
    jit_buf_ensure(&buffer,1);
    assert(buffer.failed&&buffer.code==nullptr&&buffer.length==SIZE_MAX);
    buffer=(JitBuffer){.length=SIZE_MAX/2+1,.capacity=SIZE_MAX/2+1};
    jit_buf_ensure(&buffer,1);
    assert(buffer.failed&&buffer.code==nullptr&&buffer.capacity==SIZE_MAX/2+1);

    JitCompiler compiler={};
    for(size_t i=0;i<40;i++)record_global_patch(&compiler,i,i+1);
    assert(!compiler.bailed&&compiler.patch_count==40);
    for(size_t i=0;i<40;i++)assert(compiler.patches[i].bytecode_target==i+1);
    free(compiler.patches);
    const size_t capacities[]={SIZE_MAX/2+1,SIZE_MAX/sizeof(JitPatch)/2+1};
    for(size_t i=0;i<sizeof capacities/sizeof *capacities;i++) {
        compiler=(JitCompiler){.patch_capacity=capacities[i],.patch_count=capacities[i]};
        record_global_patch(&compiler,0,0);
        assert(compiler.bailed&&compiler.patches==nullptr);
        assert(compiler.patch_count==capacities[i]&&compiler.patch_capacity==capacities[i]);
    }
}

static void test_loader_growth(void) {
    static Loader loader;
    PathRewrite rewrite={};
    for(size_t i=0;i<5000;i++) {
        assert(append(&loader,"x",1));
        rewrite_append(&rewrite,"x",1);
    }
    assert(loader.length==5000&&!rewrite.failed&&rewrite.length==5000);
    assert(loader.buffer[5000]=='\0'&&rewrite.text[5000]=='\0');
    assert(memcmp(loader.buffer,rewrite.text,5001)==0);
    free(loader.buffer);
    free(rewrite.text);
    const size_t lengths[]={SIZE_MAX,SIZE_MAX-1,SIZE_MAX/2+1};
    for(size_t i=0;i<sizeof lengths/sizeof *lengths;i++) {
        memset(&loader,0,sizeof loader);
        loader.length=loader.capacity=lengths[i];
        assert(!append(&loader,"x",1));
        assert(loader.buffer==nullptr&&loader.length==lengths[i]&&loader.capacity==lengths[i]);
        rewrite=(PathRewrite){.length=lengths[i],.capacity=lengths[i]};
        rewrite_append(&rewrite,"x",1);
        assert(rewrite.failed&&rewrite.text==nullptr);
        assert(rewrite.length==lengths[i]&&rewrite.capacity==lengths[i]);
    }
}

static void test_vm_growth(void) {
    GrowBuffer grow={};
    StringBuilder builder={};
    ByteBuffer bytes={};
    for(size_t i=0;i<600;i++) {
        assert(grow_buffer_append(&grow,"x",1));
        assert(builder_append(&builder,"x",1));
        assert(byte_buffer_append(&bytes,"x",1));
    }
    assert(grow.length==600&&builder.length==600&&bytes.length==600);
    assert(grow.data[600]=='\0'&&builder.chars[600]=='\0');
    assert(memcmp(grow.data,builder.chars,601)==0&&memcmp(grow.data,bytes.data,600)==0);
    free(grow.data);free(builder.chars);free(bytes.data);
    const size_t lengths[]={SIZE_MAX,SIZE_MAX-1,SIZE_MAX/2+1};
    for(size_t i=0;i<sizeof lengths/sizeof *lengths;i++) {
        grow=(GrowBuffer){.length=lengths[i],.capacity=lengths[i]};
        builder=(StringBuilder){.length=lengths[i],.capacity=lengths[i]};
        assert(!grow_buffer_append(&grow,"x",1));
        assert(!builder_append(&builder,"x",1));
        assert(grow.data==nullptr&&grow.length==lengths[i]&&grow.capacity==lengths[i]);
        assert(builder.chars==nullptr&&builder.length==lengths[i]&&builder.capacity==lengths[i]);
    }
    bytes=(ByteBuffer){.length=SIZE_MAX,.capacity=SIZE_MAX};
    assert(!byte_buffer_append(&bytes,"x",1));
    assert(byte_buffer_append(&bytes,nullptr,0));
    assert(bytes.data==nullptr&&bytes.length==SIZE_MAX);
    bytes=(ByteBuffer){.length=SIZE_MAX/2+1,.capacity=SIZE_MAX/2+1};
    assert(!byte_buffer_append(&bytes,"x",1));
    assert(bytes.data==nullptr&&bytes.capacity==SIZE_MAX/2+1);

    static DiamondVm vm;
    diamond_vm_init(&vm);
    for(size_t i=0;i<20;i++)assert(gc_protect(&vm,DIAMOND_INT((int64_t)i)));
    for(size_t i=0;i<20;i++)assert(vm.gc_protected[i].as.integer==(int64_t)i);
    free(vm.gc_protected);
    const size_t capacities[]={SIZE_MAX/2+1,SIZE_MAX/sizeof(DiamondValue)/2+1};
    for(size_t i=0;i<sizeof capacities/sizeof *capacities;i++) {
        vm.gc_protected=nullptr;
        vm.gc_protected_capacity=vm.gc_protected_count=capacities[i];
        assert(!gc_protect(&vm,DIAMOND_NIL));
        assert(vm.gc_protected==nullptr&&vm.gc_protected_count==capacities[i]);
        assert(vm.gc_protected_capacity==capacities[i]);
    }
    vm.gc_protected_count=vm.gc_protected_capacity=0;
    assert(allocate_tensor(&vm,SIZE_MAX,2)==nullptr);
    assert(allocate_tensor(&vm,SIZE_MAX/sizeof(double)+1,1)==nullptr);
    assert(allocate_tensor(&vm,SIZE_MAX/sizeof(double),1)==nullptr);
    DiamondTensor *tensor=allocate_tensor(&vm,2,3);
    assert(tensor!=nullptr&&tensor->rows==2&&tensor->cols==3);
    for(size_t i=0;i<6;i++)assert(tensor->data[i]==0);
    diamond_vm_free(&vm);
}

int main(void) {
    test_jit_growth();
    test_loader_growth();
    test_vm_growth();
    puts("buffer_growth_test: ok");
}
