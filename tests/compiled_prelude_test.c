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

static void check_fixed_binding_rejection(DiamondProgram *program) {
    for(size_t i=0;i<program->class_count;i++) {
        DiamondClass *class=program->classes[i];
        if(strcmp(class->name,"IntBox")!=0)continue;
        DiamondBoundTypeMember *member=&class->fixed_type_bindings[0].nodes[0].members[0];
        const uint8_t saved=member->argument_node;
        member->argument_node=0;
        char *bytes=nullptr;size_t length=0;
        FILE *file=open_memstream(&bytes,&length);
        check(file!=nullptr,"invalid fixed-binding image stream opens");
        if(file!=nullptr) {
            check(diamond_program_write_compiled(program,file),"invalid fixed-binding image is written for validation");
            fclose(file);
            DiamondProgram *copy=alloc_program();diamond_program_init(copy);
            check(!diamond_program_read_compiled((const uint8_t *)bytes,length,copy),
                "malformed fixed superclass graph is rejected on restore");
            diamond_program_free(copy);free(copy);free(bytes);
        }
        member->argument_node=saved;
        return;
    }
    check(false,"fixed superclass found for invalid-image validation");
}

static void check_generic_class_round_trip(const DiamondProgram *template) {
    static const char source[]=
        "module ForwardTyped\n"
        "  def self.run() -> Int = ForwardTyped.identity[Int](value: 40) + ForwardTyped.identity[Int](*[2])\n"
        "  def self.identity[T](value: T) -> T = value\nend\n"
        "def singleton_reference[T]() = ForwardTyped.identity[T]\n"
        "def function_reference[T]() = later_identity[T]\n"
        "def later_identity[T](value: T) -> T = value\n"
        "class Box[T]\n"
        "  def initialize(value: T)\n    @value = value\n  end\n"
        "  def value() -> T = @value\n"
        "  def wrong() -> T = \"wrong\"\n"
        "  def echo[U](value: U) -> U = value\n"
        "  def reference[U]() = self.echo[U]\n"
        "end\nstruct Payload[T, E](value: T, error: E)\nend\n"
        "class IntBox < Box[Int]\nend\n"
        "class GrandIntBox < IntBox\nend\n"
        "class IntPayload < Payload[Int, String]\nend\n"
        "class CallbackBox < Box[Callable[[Int], Int]]\nend\n"
        "class TimeBox < Box[Time]\nend\n"
        "class ForwardBox[U] < Box[U]\nend\n"
        "class ArrayBox[U] < Box[Array[U]]\n  def first() -> U = @value[0]\nend\n"
        "class Producer[U] < Box[Callable[[], U]]\nend\n"
        "def answer() -> Int = 42\n"
        "class ForwardGrand[V] < ForwardBox[V]\nend\n"
        "class ForwardPair[A, B] < Payload[A, B]\nend\n"
        "class SwappedPair[A, B] < Payload[B, A]\nend\n"
        "class StringPayload[T] < Payload[T, String]\nend\n"
        "class RepeatedPayload[T] < Payload[T, T]\nend\n"
        "class OwnBox[T, U] < Box[T]\n"
        "  def initialize(value: T, tag: U)\n    super(value)\n    @tag = tag\n  end\n"
        "  def tag() -> U = @tag\nend\n"
        "class OwnFixed[T] < IntBox\n"
        "  def initialize(value: Int, tag: T)\n    super(value)\n    @tag = tag\n  end\n"
        "  def tag() -> T = @tag\nend\n"
        "class PlainBase\nend\n"
        "class OwnPlain[T] < PlainBase\n"
        "  def initialize(tag: T)\n    @tag = tag\n  end\n"
        "  def tag() -> T = @tag\nend\n"
        "struct RepeatedTagged[T, U](tag: U) < Payload[T, T]\nend\n"
        "struct TaggedPayload[T, U](tag: U) < Payload[T, String]\nend\n"
        "struct FixedPayload(tag: String) < Payload[Int, String]\nend\n"
        "struct FixedPayloadChild[T](child: T) < FixedPayload\nend\n"
        "struct PlainPayload(flag: Bool)\nend\n"
        "struct PlainPayloadChild[T](value: T) < PlainPayload\nend\n"
        "class TwicePair[C, D] < SwappedPair[D, C]\nend\n"
        "class FixedSwapped < SwappedPair[String, Int]\nend\n"
        "interface IntSource\n  def value() -> Int\nend\n"
        "interface IntEcho\n  def echo(value: Int) -> Int\nend\n"
        "interface MixedEcho\n  def both(left: Int, right: String) -> Int | String\nend\n"
        "class UnionEcho\n  def both[U](left: U, right: U) -> U = right\nend\n"
        "def read_mixed(value: MixedEcho) -> Int\n"
        "  result = value.both(42, \"abc\")\n"
        "  if result is String\n    result.length()\n  else\n    result\n  end\nend\n"
        "interface TimeSource\n  def value() -> Time\nend\n"
        "interface CallbackSource\n  def value() -> Callable[[Int], Int]\nend\n"
        "def increment(value: Int) -> Int = value + 1\n"
        "def read_callback(value: CallbackSource) -> Int = value.value()(41)\n"
        "def read_source(value: IntSource) -> Int = value.value()\n"
        "def read_echo(value: IntEcho) -> Int = value.echo(42)\n"
        "Box[Box[Int]].new(Box.new(42)).value().value()\n";
    DiamondProgram *program=alloc_program();
    DiamondDiagnostic diagnostic;
    const bool compiled=diamond_compile_incremental(source,program,template,&diagnostic);
    check(compiled,"generic class compiles against restored template");
    if(compiled) {
        check_fixed_binding_rejection(program);
        for(size_t i=0;i<program->class_count;i++) {
            DiamondClass *class=program->classes[i];
            if(strcmp(class->name,"TaggedPayload")!=0)continue;
            class->generated_struct=false;
            char *invalid=nullptr;size_t invalid_length=0;
            FILE *invalid_file=open_memstream(&invalid,&invalid_length);
            check(invalid_file!=nullptr,"invalid struct marker image stream opens");
            if(invalid_file!=nullptr) {
                check(diamond_program_write_compiled(program,invalid_file),"invalid struct marker image writes");
                fclose(invalid_file);
                DiamondProgram *rejected=alloc_program();diamond_program_init(rejected);
                check(diamond_program_read_compiled((const uint8_t *)invalid,invalid_length,rejected),
                    "child-only parameters also restore on ordinary classes");
                diamond_program_free(rejected);free(rejected);free(invalid);
            }
            class->generated_struct=true;break;
        }

        for(size_t i=0;i<program->class_count;i++) {
            DiamondClass *class=program->classes[i];
            if(strcmp(class->name,"StringPayload")!=0)continue;
            DiamondBoundTypeMember *fixed=&class->parent_type_bindings[1].nodes[0].members[0];
            const uint16_t saved=fixed->id;fixed->id=DIAMOND_TYPE_VARIABLE_BASE+1;
            char *invalid=nullptr;size_t invalid_length=0;
            FILE *invalid_file=open_memstream(&invalid,&invalid_length);
            check(invalid_file!=nullptr,"invalid mixed parent image stream opens");
            if(invalid_file!=nullptr) {
                check(diamond_program_write_compiled(program,invalid_file),"invalid mixed parent image writes");
                fclose(invalid_file);
                DiamondProgram *rejected=alloc_program();diamond_program_init(rejected);
                check(!diamond_program_read_compiled((const uint8_t *)invalid,invalid_length,rejected),
                    "second parent template rejects an undeclared child variable on restore");
                diamond_program_free(rejected);free(rejected);free(invalid);
            }
            fixed->id=saved;break;
        }

        for(size_t i=0;i<program->class_count;i++) {
            DiamondClass *class=program->classes[i];
            if(strcmp(class->name,"ArrayBox")!=0)continue;
            DiamondBoundTypeMember *root=&class->parent_type_bindings[0].nodes[0].members[0];
            const uint8_t saved=root->argument_node;root->argument_node=0;
            char *invalid=nullptr;size_t invalid_length=0;
            FILE *invalid_file=open_memstream(&invalid,&invalid_length);
            check(invalid_file!=nullptr,"invalid transformed image stream opens");
            if(invalid_file!=nullptr) {
                check(diamond_program_write_compiled(program,invalid_file),"invalid transformed image writes");
                fclose(invalid_file);
                DiamondProgram *rejected=alloc_program();diamond_program_init(rejected);
                check(!diamond_program_read_compiled((const uint8_t *)invalid,invalid_length,rejected),
                    "cyclic transformed superclass graph is rejected on restore");
                diamond_program_free(rejected);free(rejected);free(invalid);
            }
            root->argument_node=saved;break;
        }

        for(size_t i=0;i<program->class_count;i++) {
            DiamondClass *class=program->classes[i];
            if(strcmp(class->name,"ForwardBox")!=0)continue;
            class->swaps_parent_arguments=true;
            char *invalid=nullptr;size_t invalid_length=0;
            FILE *invalid_file=open_memstream(&invalid,&invalid_length);
            check(invalid_file!=nullptr,"invalid permutation image stream opens");
            if(invalid_file!=nullptr) {
                check(diamond_program_write_compiled(program,invalid_file),"invalid permutation image writes");
                fclose(invalid_file);
                DiamondProgram *rejected=alloc_program();diamond_program_init(rejected);
                check(!diamond_program_read_compiled((const uint8_t *)invalid,invalid_length,rejected),
                    "one-parameter class cannot restore a two-slot permutation");
                diamond_program_free(rejected);free(rejected);free(invalid);
            }
            class->swaps_parent_arguments=false;
            break;
        }
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
                const bool transformed=diamond_compile_incremental(
                    "ArrayBox.new([42]).first()\n",use,copy,&diagnostic);
                check(transformed,"restored transformed superclass infers child argument");
                if(transformed) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored transformed binding preserves child and parent views");
                }
                const bool nested_transform=diamond_compile_incremental(
                    "class NestedArrayBox[T] < ArrayBox[Array[T]]\nend\n"
                    "NestedArrayBox[Int].new([[43]]).value()[0][0]\n",use,copy,&diagnostic);
                check(nested_transform,"restored transformed parent can be specialized again");
                if(nested_transform) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"43")==0,
                        "restored composed transformed binding executes");
                }
                const bool producer=diamond_compile_incremental(
                    "Producer.new(answer).value()()\n",use,copy,&diagnostic);
                check(producer,"restored Callable template infers its return argument");
                if(producer) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored Callable template retains its signature");
                }
                const bool mixed_parent=diamond_compile_incremental(
                    "StringPayload.new(42, \"fixed\").value()\n",use,copy,&diagnostic);
                check(mixed_parent,"restored mixed parent infers its child argument");
                if(mixed_parent) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored mixed parent uses the ancestor's wider prefix");
                }
                const bool mixed_explicit=diamond_compile_incremental(
                    "StringPayload[Int].new(43, \"fixed\").error().length()\n",use,copy,&diagnostic);
                check(mixed_explicit,"restored mixed parent accepts explicit child specialization");
                if(mixed_explicit) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"5")==0,
                        "restored mixed parent retains its fixed second argument");
                }
                const bool cached_struct_child=diamond_compile_incremental(
                    "struct CachedChild[T, U](flag: Bool) < TaggedPayload[T, U]\nend\n"
                    "value = CachedChild.new(42, \"fixed\", false, true)\n"
                    "if value.tag()\n  0\nelse\n  42\nend\n",use,copy,&diagnostic);
                check(cached_struct_child,"restored generic struct accepts a new generic child");
                if(cached_struct_child) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored inherited constructor keeps child-only and fixed parent slots separate");
                }
                const bool cached_fixed_struct=diamond_compile_incremental(
                    "struct CachedFixed(tag: String) < Payload[Int, String]\nend\n"
                    "CachedFixed.new(42, \"fixed\", \"tag\").value()\n",use,copy,&diagnostic);
                check(cached_fixed_struct,"restored generic struct accepts a concrete struct child");
                if(cached_fixed_struct) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored fixed struct constructor specializes inherited fields");
                }
                const bool cached_plain_struct=diamond_compile_incremental(
                    "PlainPayloadChild.new(false, 42).value()\n",use,copy,&diagnostic);
                check(cached_plain_struct,"restored generic child retains a nongeneric struct parent");
                if(cached_plain_struct) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored child infers parameters solely from its new field");
                }
                const bool cached_fixed_parent=diamond_compile_incremental(
                    "value = FixedPayloadChild.new(42, \"fixed\", \"tag\", false)\n"
                    "if value.child()\n  0\nelse\n  value.value()\nend\n",use,copy,&diagnostic);
                check(cached_fixed_parent,"restored generic child retains a fixed struct parent");
                if(cached_fixed_parent) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored fixed parent bindings cannot replace the child's own parameter");
                }
                const bool restored_function=diamond_compile_incremental(
                    "ref = function_reference[Int]()\nref(42)\n",use,copy,&diagnostic);
                check(restored_function,"restored specialized function reference captures enclosing bindings");
                if(restored_function) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored forward reference wrapper retains its saved binding");
                }
                const bool restored_singleton=diamond_compile_incremental(
                    "ref = singleton_reference[Int]()\nref(42)\n",use,copy,&diagnostic);
                check(restored_singleton,"restored singleton reference captures enclosing bindings");
                if(restored_singleton) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored singleton wrapper uses its saved binding after factory returns");
                }
                const bool restored_reference=diamond_compile_incremental(
                    "ref = Box[String].new(\"outer\").reference[Int]()\nref(42)\n",use,copy,&diagnostic);
                check(restored_reference,"restored generic method builds a lexical bound reference");
                if(restored_reference) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored reference keeps class and method slots independent");
                }
                const bool restored_forward=diamond_compile_incremental(
                    "ForwardTyped.run()\n",use,copy,&diagnostic);
                check(restored_forward,"restored forward generic module call compiles");
                if(restored_forward) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored forward generic module call retains its explicit binding");
                }
                const bool restored_own=diamond_compile_incremental(
                    "OwnBox.new(40, 2).value() + OwnBox.new(40, 2).tag()\n",use,copy,&diagnostic);
                check(restored_own,"restored ordinary class infers a child-only parameter");
                if(restored_own) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored own constructor retains parent and child bindings");
                }
                const bool restored_own_fixed=diamond_compile_incremental(
                    "OwnFixed.new(40, 1).value() + OwnPlain.new(2).tag()\n",use,copy,&diagnostic);
                check(restored_own_fixed,"restored ordinary classes support fixed and plain parents");
                if(restored_own_fixed) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored fixed parent cannot replace the child's own binding");
                }
                const bool restored_repeated=diamond_compile_incremental(
                    "RepeatedPayload.new(42, \"other\").error().to_s().length()\n",use,copy,&diagnostic);
                check(restored_repeated,"restored repeated parent joins constructor witnesses");
                if(restored_repeated) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"5")==0,
                        "restored repeated parent keeps its shared union in both slots");
                }
                const bool restored_repeated_tag=diamond_compile_incremental(
                    "value = RepeatedTagged.new(42, \"other\", false)\n"
                    "if value.tag()\n  0\nelse\n  value.error().to_s().length()\nend\n",use,copy,&diagnostic);
                check(restored_repeated_tag,"restored repeated struct keeps a child-only parameter");
                if(restored_repeated_tag) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"5")==0,
                        "restored repeated mapping cannot masquerade as a permutation");
                }
                const bool swapped=diamond_compile_incremental(
                    "SwappedPair[String, Int].new(42, \"abc\").value()\n",use,copy,&diagnostic);
                check(swapped,"restored reordered superclass accepts explicit arguments");
                if(swapped) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored reordered binding executes in the parent view");
                }
                const bool swapped_inferred=diamond_compile_incremental(
                    "SwappedPair.new(42, \"abc\").error().length()\n",use,copy,&diagnostic);
                check(swapped_inferred,"restored reordered superclass infers child arguments");
                if(swapped_inferred) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"3")==0,
                        "restored inferred reordered binding retains the second parent slot");
                }
                const bool composed=diamond_compile_incremental(
                    "TwicePair[Int, String].new(42, \"abc\").value()\n",use,copy,&diagnostic);
                check(composed,"restored superclass permutations compose");
                if(composed) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored double permutation returns the original slot order");
                }
                const bool fixed_swapped=diamond_compile_incremental(
                    "FixedSwapped.new(42, \"abc\").value()\n",use,copy,&diagnostic);
                check(fixed_swapped,"restored fixed child keeps its reordered ancestor view");
                if(fixed_swapped) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored fixed reordered child executes");
                }
                const bool forwarded=diamond_compile_incremental(
                    "ForwardGrand[Int].new(42).value()\n",use,copy,&diagnostic);
                check(forwarded,"restored parameterized superclass accepts explicit arguments");
                if(forwarded) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored parameterized superclass keeps forwarded binding");
                }
                const bool forwarded_inferred=diamond_compile_incremental(
                    "read_source(ForwardBox.new(42))\n",use,copy,&diagnostic);
                check(forwarded_inferred,"restored parameterized superclass infers inherited constructor binding");
                if(forwarded_inferred) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored forwarded binding satisfies inherited interface");
                }
                const bool forwarded_pair=diamond_compile_incremental(
                    "ForwardPair.new(42, \"abc\").error().length()\n",use,copy,&diagnostic);
                check(forwarded_pair,"restored parameterized superclass forwards both arguments");
                if(forwarded_pair) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"3")==0,
                        "restored two-parameter forwarded binding executes");
                }
                const bool inherited=diamond_compile_incremental(
                    "GrandIntBox.new(42).value()\n",use,copy,&diagnostic);
                check(inherited,"restored fixed superclass accepts construction");
                if(inherited) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored fixed superclass retains inherited binding");
                }
                const bool inherited_interface=diamond_compile_incremental(
                    "read_source(IntBox.new(42))\n",use,copy,&diagnostic);
                check(inherited_interface,"restored fixed superclass satisfies inherited interface");
                if(inherited_interface) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored inherited interface dispatches with fixed arguments");
                }
                const bool inherited_pair=diamond_compile_incremental(
                    "IntPayload.new(42, \"abc\").value()\n",use,copy,&diagnostic);
                check(inherited_pair,"restored fixed superclass retains two arguments");
                if(inherited_pair) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored inherited struct reader uses the fixed binding");
                }
                const bool inherited_callback=diamond_compile_incremental(
                    "read_callback(CallbackBox.new(increment))\n",use,copy,&diagnostic);
                check(inherited_callback,"restored fixed superclass retains Callable arguments");
                if(inherited_callback) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored fixed Callable argument executes");
                }
                const bool inherited_native=diamond_compile_incremental(
                    "TimeBox.new(Time.now()).value().year()\n",use,copy,&diagnostic);
                check(inherited_native,"restored fixed superclass retains native arguments");
                if(inherited_native) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered),"restored fixed native argument executes");
                }
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
                const bool mixed=diamond_compile_incremental(
                    "read_mixed(UnionEcho.new())\n",use,copy,&diagnostic);
                check(mixed,"restored interface joins method input witnesses");
                if(mixed) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"3")==0,
                        "restored interface executes with a shared union binding");
                }
                const bool callback=diamond_compile_incremental(
                    "read_callback(Box[Callable[[Int], Int]].new(increment))\n",use,copy,&diagnostic);
                check(callback,"restored interface accepts Callable binding payload");
                if(callback) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered)&&strcmp(rendered,"42")==0,
                        "restored Callable binding retains parameter and return contracts");
                }
                const bool native=diamond_compile_incremental(
                    "def take_clock(value: TimeSource) -> Time = value.value()\n"
                    "take_clock(Box[Time].new(Time.now())).year()\n",use,copy,&diagnostic);
                check(native,"restored interface accepts native binding payload");
                if(native) {
                    char rendered[64];
                    check(run_and_get_string(use,rendered,sizeof rendered),
                        "restored native binding executes with its kind intact");
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
