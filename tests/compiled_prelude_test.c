/* Standalone verification for compiled_prelude.c's write/read round-trip:
 * compile the prelude the ordinary way, serialize it, deserialize it into
 * a fresh DiamondProgram, then confirm that deserialized program works
 * exactly like the original as a diamond_compile_incremental template.
 * Not wired into `make test` yet -- a standalone probe while the feature
 * is still being verified.
 *
 * Every DiamondProgram here is heap-allocated -- see
 * tests/incremental_compile_test.c's own comment on why (~14MB struct,
 * several of them declared across one function's scopes would risk a
 * real stack overflow at -O0). */

/* open_memstream (POSIX.1-2008) is hidden by glibc's stdio.h under a
 * strict -std=c23 with no feature-test macro set (same fix
 * fuzz/execute_fuzzer.c already needed). */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiled_prelude.h"
#include "compiler.h"
#include "disassemble.h"
#include "prelude.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(bool condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", description);
        failures++;
    } else {
        fprintf(stderr, "ok: %s\n", description);
    }
}

static DiamondProgram *alloc_program(void) {
    DiamondProgram *program = calloc(1, sizeof *program);
    if (program == nullptr) { fprintf(stderr, "out of memory\n"); exit(1); }
    return program;
}

static bool run_and_get_string(DiamondProgram *program, char *out, size_t out_size) {
    DiamondChunk chunk = diamond_program_chunk(program);
    DiamondVm vm;
    diamond_vm_init(&vm);
    DiamondValue result = DIAMOND_NIL;
    const DiamondVmStatus status = diamond_vm_run(&vm, &chunk, &result);
    if (status != DIAMOND_VM_OK) {
        snprintf(out, out_size, "<runtime error: %s>", diamond_vm_status_name(status));
        diamond_vm_free(&vm);
        return false;
    }
    if (result.kind == DIAMOND_VALUE_INT) {
        snprintf(out, out_size, "%lld", (long long)result.as.integer);
    } else {
        snprintf(out, out_size, "<non-Int value kind %d>", (int)result.kind);
    }
    diamond_vm_free(&vm);
    return true;
}

static void check_specialized_constructor_bytecode(const DiamondProgram *program) {
    size_t class_index=0;
    while(class_index<program->class_count&&strcmp(program->classes[class_index]->name,"Box")!=0)
        class_index++;
    check(class_index<program->class_count,"generic constructor class found");
    if(class_index>=program->class_count)return;
    uint8_t code[]={DIAMOND_OP_NEW_TYPED,0,1,(uint8_t)(class_index>>8),
        (uint8_t)(class_index&UINT8_MAX),0,0,0,0,0,DIAMOND_OP_RETURN,0,1};
    const DiamondTypeSet set={.count=1,.members={{.id=DIAMOND_TYPE_INT,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}}};
    DiamondChunk chunk=diamond_program_chunk(program);
    chunk.code=code;chunk.code_count=sizeof code;chunk.register_count=2;
    chunk.type_sets=&set;chunk.type_set_count=1;
    check(diamond_verify_bytecode(&chunk),"explicit constructor bytecode verifies");
    code[5]=UINT8_MAX;code[6]=UINT8_MAX;
    check(!diamond_verify_bytecode(&chunk),"invalid constructor type-set index rejected");
    code[5]=0;code[6]=0;chunk.code_count=9;
    check(!diamond_verify_bytecode(&chunk),"truncated constructor operands rejected");
    chunk.code_count=sizeof code;code[9]=0x7f;
    check(!diamond_verify_bytecode(&chunk),"oversized constructor keyword trailer rejected");
    code[9]=0;code[3]=0;code[4]=0;
    check(!diamond_verify_bytecode(&chunk),"non-generic class specialization rejected");
}

static void check_generic_class_round_trip(const DiamondProgram *template) {
    static const char source[]=
        "class Box[T]\n"
        "  def initialize(value: T)\n    @value = value\n  end\n"
        "  def value() -> T = @value\n"
        "  def wrong() -> T = \"wrong\"\n"
        "  def echo[U](value: U) -> U = value\n"
        "end\nstruct Payload[T, E](value: T, error: E)\nend\n"
        "interface IntSource\n  def value() -> Int\nend\n"
        "interface IntEcho\n  def echo(value: Int) -> Int\nend\n"
        "def read_source(value: IntSource) -> Int = value.value()\n"
        "def read_echo(value: IntEcho) -> Int = value.echo(42)\n"
        "Box[Box[Int]].new(Box.new(42)).value().value()\n";
    DiamondProgram *program=alloc_program();
    DiamondDiagnostic diagnostic;
    const bool compiled=diamond_compile_incremental(source,program,template,&diagnostic);
    check(compiled,"generic class compiles against restored template");
    if(compiled) {
        char *bytes=nullptr;size_t length=0;
        FILE *file=open_memstream(&bytes,&length);
        check(file!=nullptr,"generic class serialization stream opens");
        if(file!=nullptr) {
            check(diamond_program_write_compiled(program,file),"generic class serializes");
            fclose(file);
            DiamondProgram *copy=alloc_program();diamond_program_init(copy);
            const bool read=diamond_program_read_compiled((const uint8_t *)bytes,length,copy);
            check(read,"generic class deserializes");
            if(read) {
                check_specialized_constructor_bytecode(copy);
                char rendered_constructor[64];
                check(run_and_get_string(copy,rendered_constructor,sizeof rendered_constructor)&&
                    strcmp(rendered_constructor,"42")==0,
                    "serialized explicit generic constructor executes");
                DiamondProgram *use=alloc_program();
                const bool good=diamond_compile_incremental("Box.new(42).value()\n",
                    use,copy,&diagnostic);
                check(good,"restored generic class accepts new construction");
                if(good) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&
                        strcmp(rendered,"42")==0,"restored generic class retains binding");
                }
                const bool nested=diamond_compile_incremental(
                    "Box.new(Box.new(42)).value().value()\n",use,copy,&diagnostic);
                check(nested,"restored generic class infers nested bindings");
                if(nested) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored nested binding survives both readers");
                }
                const bool conforming=diamond_compile_incremental(
                    "read_source(Box.new(42))\n",use,copy,&diagnostic);
                check(conforming,"restored generic instance satisfies interface");
                if(!conforming)fprintf(stderr,"interface compile diagnostic: %s\n",diagnostic.message);
                if(conforming) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored interface uses receiver binding");
                }
                const bool rejected=diamond_compile_incremental(
                    "read_source(Box.new(\"wrong\"))\n",use,copy,&diagnostic);
                check(!rejected,"restored interface rejects incompatible receiver binding");
                const bool echo=diamond_compile_incremental(
                    "read_echo(Box.new(\"receiver\"))\n",use,copy,&diagnostic);
                check(echo,"restored interface specializes method-owned variable");
                if(echo) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored interface isolates method witness from receiver");
                }
                const bool record=diamond_compile_incremental(
                    "Payload[Int, String].new(42, \"abc\").error().length()\n",use,copy,&diagnostic);
                check(record,"restored generic struct accepts construction");
                if(record) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"3")==0,
                        "restored generic struct reader retains its binding");
                }
                const bool method=diamond_compile_incremental(
                    "Box[Int].new(42).echo[String](\"abc\").length()\n",use,copy,&diagnostic);
                check(method,"restored generic class accepts method-owned type argument");
                if(method) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"3")==0,
                        "restored method keeps class and method bindings separate");
                }
                const bool bad=diamond_compile_incremental("Box.new(42).wrong()\n",
                    use,copy,&diagnostic);
                check(bad,"restored generic class wrong return remains a runtime check");
                if(bad) {
                    DiamondChunk chunk=diamond_program_chunk(use);
                    DiamondVm vm;diamond_vm_init(&vm);DiamondValue result=DIAMOND_NIL;
                    check(diamond_vm_run(&vm,&chunk,&result)==DIAMOND_VM_TYPE_ERROR,
                        "restored generic class rejects wrong return");
                    diamond_vm_free(&vm);
                }
                diamond_program_free(use);free(use);
            }
            diamond_program_free(copy);free(copy);free(bytes);
        }
    }
    diamond_program_free(program);free(program);
}

int main(void) {
    const size_t prelude_length = diamond_prelude_length(true);
    char *prelude_source = malloc(prelude_length + 1);
    diamond_prelude_write(prelude_source, true);
    prelude_source[prelude_length] = '\0';

    DiamondProgram *original = alloc_program();
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(prelude_source, original, &diagnostic)) {
        fprintf(stderr, "FAIL: prelude compile: %s\n", diagnostic.message);
        return 1;
    }
    free(prelude_source);
    fprintf(stderr, "ok: original prelude compile (%zu functions, %zu classes)\n",
        original->function_count, original->class_count);

    char *buffer = nullptr;
    size_t buffer_size = 0;
    FILE *memfile = open_memstream(&buffer, &buffer_size);
    check(memfile != nullptr, "open_memstream succeeded");
    check(diamond_program_write_compiled(original, memfile), "write_compiled succeeded");
    fclose(memfile);
    fprintf(stderr, "  serialized size: %zu bytes\n", buffer_size);

    DiamondProgram *restored = alloc_program();
    diamond_program_init(restored);
    check(diamond_program_read_compiled((const uint8_t *)buffer, buffer_size, restored),
        "read_compiled succeeded");
    check(restored->function_count == original->function_count,
        "restored function_count matches original");
    check(restored->class_count == original->class_count,
        "restored class_count matches original");
    check(restored->interface_count == original->interface_count,
        "restored interface_count matches original");
    check(restored->module_count == original->module_count,
        "restored module_count matches original");
    check(restored->range_class_index == original->range_class_index,
        "restored range_class_index matches original");
    check(strcmp(restored->classes[0]->name, original->classes[0]->name) == 0,
        "restored classes[0]->name matches original");

    /* The real test: use `restored` (built purely from a byte buffer,
     * never itself passed through diamond_compile) as a
     * diamond_compile_incremental template, exactly the role
     * tests/run_cases.c's own live-compiled template plays today. */
    DiamondProgram *program = alloc_program();
    static const char user_source[] =
        "mod(7, 3) + (1..3).to_a().sum() + \"ok\".length()\n";
    const bool compiled = diamond_compile_incremental(
        user_source, program, restored, &diagnostic);
    check(compiled, "incremental compile against a deserialized template succeeds");
    if (compiled) {
        char rendered[64];
        const bool ran = run_and_get_string(program, rendered, sizeof rendered);
        /* mod(7,3)=1, (1..3).to_a().sum()=6, "ok".length()=2 -> 9 */
        check(ran && strcmp(rendered, "9") == 0,
            "result matches (1 + 6 + 2 == 9)");
    } else {
        fprintf(stderr, "  diagnostic: %s\n", diagnostic.message);
    }

    /* Redefining a prelude function must still be rejected against the
     * deserialized template too, exactly as it is against a live one. */
    static const char redefine_source[] = "def mod(a, b)\n  -1\nend\nmod(7, 3)\n";
    const bool redefine_compiled = diamond_compile_incremental(
        redefine_source, program, restored, &diagnostic);
    check(!redefine_compiled, "redefining a prelude function is rejected against the deserialized template");

    check_generic_class_round_trip(restored);
    diamond_program_free(program);
    free(program);
    diamond_program_free(restored);
    free(restored);
    free(buffer);
    diamond_program_free(original);
    free(original);

    if (failures > 0) {
        fprintf(stderr, "\n%d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "\nall checks passed\n");
    return 0;
}
