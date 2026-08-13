#include "compiler.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Fuzzes diamond_compile directly against raw source bytes -- lexer,
 * parser, and bytecode emission, the exact surface a `.di` file exposes
 * to arbitrary input. Deliberately compile-only, never diamond_vm_run:
 * Diamond has real File/TCPSocket/Regexp bridges (see docs/io.md), so
 * actually executing an arbitrary fuzzer-mutated program isn't safe
 * without sandboxing/resource limits this harness doesn't attempt --
 * see docs/fuzzing.md for the full reasoning and what a future
 * execution-fuzzing harness would need first.
 *
 * Deliberately doesn't prepend lib/core.di the way src/main.c's
 * run_source does for a real CLI invocation: core.di's own bytes never
 * change between iterations, so prepending it would only add fixed
 * per-iteration overhead without exercising any additional code path --
 * it's already covered by the ordinary test suite on every change. */
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* DiamondProgram is tens of MB (see src/compiler.h's own comment on
     * the struct) -- heap-allocated once and reused across the whole
     * fuzzing run for the same reason lsp/diagnostics.c does: a fresh
     * malloc of that size on every one of potentially millions of
     * iterations would dominate runtime for no benefit, and
     * diamond_compile always re-initializes it from scratch via
     * diamond_program_init before compiling. */
    static DiamondProgram *program=nullptr;
    if(program==nullptr) {
        program=malloc(sizeof *program);
        if(program==nullptr)return 0;
    }
    char *source=malloc(size+1);
    if(source==nullptr)return 0;
    memcpy(source,data,size);
    source[size]='\0';
    DiamondDiagnostic diagnostic;
    (void)diamond_compile(source,program,&diagnostic);
    free(source);
    return 0;
}
