/* Standalone verification for diamond_compile_incremental (src/compiler.c):
 * compiles the prelude once into a template DiamondProgram, then compiles
 * several small user programs against it without ever re-parsing the
 * prelude's own source, checking each scenario the mechanism has to get
 * right against what an ordinary diamond_compile(prelude+user, ...) would
 * have produced. Not wired into `make test` yet -- a standalone probe
 * while the feature is still being verified.
 *
 * Every DiamondProgram here is heap-allocated (calloc), never a local --
 * it's ~14MB (confirmed directly: `sizeof(DiamondProgram)`), and this
 * file declares several of them across main()'s various scenarios; at
 * -O0, with no stack-slot coalescing across non-overlapping lexical
 * scopes, that's the exact same class of stack-overflow risk this
 * project's own Makefile already documents for run_chunk's registers
 * (see CFLAGS_DEBUG's own comment) -- confirmed the hard way here too,
 * as a segfault at main()'s very first statement before this fix. */

/* See src/bignum.c's own identical comment: needed transitively for
 * vm.h's <ucontext.h> use, only under musl (docs/roadmap.md's
 * "Portability"). */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
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

static char *build_prelude_source(bool include_json, size_t *out_length) {
    const size_t length = diamond_prelude_length(include_json);
    char *buffer = malloc(length + 1);
    if (buffer == nullptr) { fprintf(stderr, "out of memory\n"); exit(1); }
    diamond_prelude_write(buffer, include_json);
    buffer[length] = '\0';
    if (out_length != nullptr) *out_length = length;
    return buffer;
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
    /* Every scenario below produces an Int -- a minimal ad-hoc render,
     * not diamond_value_print (stdout-only, no string-capture form). */
    if (result.kind == DIAMOND_VALUE_INT) {
        snprintf(out, out_size, "%lld", (long long)result.as.integer);
    } else {
        snprintf(out, out_size, "<non-Int value kind %d>", (int)result.kind);
    }
    diamond_vm_free(&vm);
    return true;
}

int main(void) {
    size_t prelude_length = 0;
    char *prelude_source = build_prelude_source(true, &prelude_length);

    DiamondProgram *template_program = alloc_program();
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(prelude_source, template_program, &diagnostic)) {
        fprintf(stderr, "FAIL: prelude-alone compile: %s\n", diagnostic.message);
        return 1;
    }
    fprintf(stderr, "ok: prelude-alone compile (%zu bytes, %zu functions, %zu classes)\n",
            prelude_length, template_program->function_count, template_program->class_count);

    /* One scratch program, reused (freed between uses) across scenarios --
     * exactly the pattern tests/run_cases.c and the REPL already use for
     * the same 14MB-struct reason. */
    DiamondProgram *program = alloc_program();

    /* Scenario 1: calling a prelude top-level function and using a
     * prelude-defined class/module (Range via Enumerable's `to_a`/sum)
     * from source that never itself mentions the prelude's own text. */
    {
        static const char source[] = "mod(7, 3) + (1..3).to_a().sum()\n";
        const bool compiled = diamond_compile_incremental(
            source, program, template_program, &diagnostic);
        check(compiled, "scenario 1: incremental compile of a prelude-using program");
        if (compiled) {
            char rendered[64];
            const bool ran = run_and_get_string(program, rendered, sizeof rendered);
            check(ran && strcmp(rendered, "7") == 0,
                "scenario 1: mod(7,3)=1 + (1..3).to_a().sum()=6 == 7");
        } else {
            fprintf(stderr, "  diagnostic: %s\n", diagnostic.message);
        }
    }

    /* Scenario 2: redefining a prelude top-level function must still be
     * a compile error, not a silent shadow/duplicate -- matching what
     * concatenated prelude+user text already rejects today. */
    {
        static const char source[] = "def mod(a, b)\n  -1\nend\nmod(7, 3)\n";
        const bool compiled = diamond_compile_incremental(
            source, program, template_program, &diagnostic);
        check(!compiled, "scenario 2: redefining a prelude function is rejected");
    }

    /* Scenario 3: two independent, sequential incremental compiles reusing
     * the exact same template (what a batch test runner does) -- confirms
     * no leftover per-call state (next_function_claim et al.) corrupts
     * the second call. */
    {
        static const char source_a[] = "mod(10, 4)\n";
        check(diamond_compile_incremental(source_a, program, template_program, &diagnostic),
            "scenario 3a: first sequential incremental compile");
        char rendered_a[64];
        run_and_get_string(program, rendered_a, sizeof rendered_a);
        check(strcmp(rendered_a, "2") == 0, "scenario 3a: mod(10,4) == 2");

        static const char source_b[] =
            "def double(x)\n  x * 2\nend\ndouble(mod(10, 4))\n";
        check(diamond_compile_incremental(source_b, program, template_program, &diagnostic),
            "scenario 3b: second sequential incremental compile, own new function");
        char rendered_b[64];
        run_and_get_string(program, rendered_b, sizeof rendered_b);
        check(strcmp(rendered_b, "4") == 0,
            "scenario 3b: double(mod(10,4)) == 4 (own function + prelude function)");
    }

    /* Scenario 4: a syntax error in user-only source reports a
     * plausible, non-prelude-skewed line/column -- there's no prelude
     * text prepended for a real offset to need correcting against. */
    {
        static const char source[] = "1 +\n";
        const bool compiled = diamond_compile_incremental(
            source, program, template_program, &diagnostic);
        check(!compiled, "scenario 4: syntax error still rejected");
        /* line 2, not 1: confirmed identical against the existing,
         * unmodified diamond_compile("1 +\n", ...) -- the EOF token
         * after the trailing newline is genuinely on the next line,
         * nothing to do with any prelude-offset skew. */
        check(diagnostic.span.line == 2 && diagnostic.span.column == 1,
            "scenario 4: diagnostic position matches plain diamond_compile exactly");
    }

    /* Scenario 5: a forward reference within the user source itself
     * (calling a function declared later in the same incremental
     * source) still resolves -- confirms the discovery pass still does
     * its job over the user-only text, not just over the template. */
    {
        static const char source[] =
            "def caller()\n  callee()\nend\ndef callee()\n  42\nend\ncaller()\n";
        const bool compiled = diamond_compile_incremental(
            source, program, template_program, &diagnostic);
        check(compiled, "scenario 5: forward reference within user source compiles");
        if (compiled) {
            char rendered[64];
            run_and_get_string(program, rendered, sizeof rendered);
            check(strcmp(rendered, "42") == 0, "scenario 5: caller() -> callee() == 42");
        } else {
            fprintf(stderr, "  diagnostic: %s\n", diagnostic.message);
        }
    }

    /* Scenario 6: cross-check against the existing, unmodified
     * diamond_compile path -- the exact same user program, compiled the
     * old way (prelude + "#line 1" + user text concatenated) and the
     * new way (template + incremental), must produce the same result. */
    {
        static const char user_source[] = "(1..5).to_a().select() do |n|\n  n % 2 == 0\nend.sum()\n";

        const size_t reset_length = strlen("\n#line 1\n");
        char *combined = malloc(prelude_length + reset_length + strlen(user_source) + 1);
        size_t offset = diamond_prelude_write(combined, true);
        memcpy(combined + offset, "\n#line 1\n", reset_length); offset += reset_length;
        memcpy(combined + offset, user_source, strlen(user_source) + 1);
        const bool old_compiled = diamond_compile(combined, program, &diagnostic);
        check(old_compiled, "scenario 6: old concatenated-compile path still compiles");
        char old_rendered[64] = {0};
        if (old_compiled) run_and_get_string(program, old_rendered, sizeof old_rendered);
        free(combined);

        const bool new_compiled = diamond_compile_incremental(
            user_source, program, template_program, &diagnostic);
        check(new_compiled, "scenario 6: new incremental-compile path compiles");
        char new_rendered[64] = {0};
        if (new_compiled) run_and_get_string(program, new_rendered, sizeof new_rendered);

        check(old_compiled && new_compiled && strcmp(old_rendered, new_rendered) == 0,
            "scenario 6: both paths produce the same result");
        fprintf(stderr, "  old=%s new=%s\n", old_rendered, new_rendered);
    }

    diamond_program_free(program);
    free(program);
    diamond_program_free(template_program);
    free(template_program);
    free(prelude_source);

    if (failures > 0) {
        fprintf(stderr, "\n%d failure(s)\n", failures);
        return 1;
    }
    fprintf(stderr, "\nall checks passed\n");
    return 0;
}
