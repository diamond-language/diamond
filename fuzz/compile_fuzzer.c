/* See src/bignum.c's own identical comment: needed transitively for
 * vm.h's <ucontext.h> use, only under musl (docs/roadmap.md's
 * "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
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
     * iterations would dominate runtime for no benefit. Reusing it needs
     * an explicit diamond_program_free before every compile, though --
     * unlike the fixed-size tables, the function table is independently
     * heap-allocated and diamond_program_init's memset would otherwise
     * leak the previous iteration's functions instead of freeing them. */
    static DiamondProgram *program=nullptr;
    if(program==nullptr) {
        program=calloc(1,sizeof *program);
        if(program==nullptr)return 0;
    }
    char *source=malloc(size+1);
    if(source==nullptr)return 0;
    memcpy(source,data,size);
    source[size]='\0';
    DiamondDiagnostic diagnostic;
    diamond_program_free(program);
    (void)diamond_compile(source,program,&diagnostic);
    free(source);
    return 0;
}
