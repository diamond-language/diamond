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
    check(strcmp(restored->classes[0].name, original->classes[0].name) == 0,
        "restored classes[0].name matches original");

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
