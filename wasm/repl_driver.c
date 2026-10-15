/* The browser REPL's native half (docs/internal/wasm.md). The page keeps the
 * session text; each evaluation hands the whole candidate source to
 * diamond_wasm_eval, which compiles it against a prelude template built once
 * and runs it in a fresh VM, exactly as the terminal REPL replays its session
 * (src/repl.c). Output goes to stdout, the result value follows a 0x1E record
 * separator so the page can tell program output from the value, and a compile
 * or runtime error goes to stderr. */
#include "compiler.h"
#include "prelude.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WASM_EXPORT(name) __attribute__((export_name(name)))

WASM_EXPORT("diamond_wasm_alloc") void *diamond_wasm_alloc(size_t size) { return malloc(size); }
WASM_EXPORT("diamond_wasm_free") void diamond_wasm_free(void *pointer) { free(pointer); }

static DiamondProgram *prelude_template;
static DiamondProgram *program;

static bool ensure_state(void) {
    if (prelude_template != nullptr) return true;
    prelude_template = calloc(1, sizeof *prelude_template);
    program = calloc(1, sizeof *program);
    if (prelude_template == nullptr || program == nullptr) return false;
    const size_t length = diamond_prelude_length(true);
    char *source = malloc(length + 1);
    if (source == nullptr) return false;
    diamond_prelude_write(source, true);
    source[length] = '\0';
    DiamondDiagnostic diagnostic;
    const bool ok = diamond_compile(source, prelude_template, &diagnostic);
    free(source);
    return ok;
}

/* 0 on success, 65 compile error, 70 runtime error, 74 out of memory. */
WASM_EXPORT("diamond_wasm_eval") int diamond_wasm_eval(const char *source) {
    if (!ensure_state()) return 74;
    diamond_program_free(program);
    program->allow_top_level_redefinition = true;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile_incremental(source, program, prelude_template, &diagnostic)) {
        fprintf(stderr, "%u:%u: error: %s\n", (unsigned)diagnostic.span.line,
                (unsigned)diagnostic.span.column, diagnostic.message);
        fflush(stderr);
        return 65;
    }
    DiamondChunk chunk = diamond_program_chunk(program);
    chunk.name = "<repl>";
    DiamondVm vm;
    diamond_vm_init(&vm);
    DiamondValue result = DIAMOND_NIL;
    const DiamondVmStatus status = diamond_vm_run(&vm, &chunk, &result);
    int exit_code = 0;
    if (status != DIAMOND_VM_OK) {
        const char *detail = diamond_vm_error(&vm);
        fprintf(stderr, "runtime error: %s\n",
                detail != nullptr ? detail : diamond_vm_status_name(status));
        exit_code = 70;
    } else {
        fputc(0x1e, stdout);
        diamond_value_fprint(stdout, result);
        fputc('\n', stdout);
    }
    diamond_vm_free(&vm);
    fflush(stdout);
    fflush(stderr);
    return exit_code;
}
