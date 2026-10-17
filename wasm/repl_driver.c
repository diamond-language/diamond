/* The browser REPL's native half (docs/internal/wasm.md). The page keeps the
 * session text; each evaluation hands the whole candidate source to
 * diamond_wasm_eval, which compiles it against a prelude template built once
 * and runs it in a fresh VM, exactly as the terminal REPL replays its session
 * (src/repl.c). Output goes to stdout, the result value follows a 0x1E record
 * separator so the page can tell program output from the value, and a compile
 * or runtime error goes to stderr. */
#include "compiler.h"
#include "lexer.h"
#include "prelude.h"
#include "repl.h"
#include "value.h"
#include "vm.h"

#include <stdint.h>
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

/* Syntax highlighting for the page. The lexer is the language's own, so the colours
 * cannot drift from what the compiler accepts. Returns a pointer to
 * [count, (class, byte_start, byte_length) * count] as uint32_t, valid until the next call.
 * Whitespace and comments are not tokens; the page treats any gap starting with '#' as a
 * comment. Classes match HIGHLIGHT_CLASSES in wasm/web/diamond-wasm.js. */
enum { HL_PLAIN, HL_KEYWORD, HL_STRING, HL_NUMBER, HL_FUNCTION, HL_CONSTANT, HL_IVAR, HL_SYMBOL, HL_REGEX };

static uint32_t *highlight_buffer;
static size_t highlight_capacity;

static uint32_t highlight_class(DiamondTokenKind kind, DiamondTokenKind previous, char first) {
    if (kind >= DIAMOND_TOKEN_IF && kind <= DIAMOND_TOKEN_IS) return HL_KEYWORD;
    switch (kind) {
    case DIAMOND_TOKEN_STRING: return HL_STRING;
    case DIAMOND_TOKEN_INTEGER:
    case DIAMOND_TOKEN_FLOAT: return HL_NUMBER;
    case DIAMOND_TOKEN_SYMBOL: return HL_SYMBOL;
    case DIAMOND_TOKEN_REGEX: return HL_REGEX;
    case DIAMOND_TOKEN_INSTANCE_VARIABLE:
    case DIAMOND_TOKEN_CLASS_VARIABLE: return HL_IVAR;
    case DIAMOND_TOKEN_IDENTIFIER:
        if (previous == DIAMOND_TOKEN_DEF) return HL_FUNCTION;
        return first >= 'A' && first <= 'Z' ? HL_CONSTANT : HL_PLAIN;
    default: return HL_PLAIN;
    }
}

WASM_EXPORT("diamond_wasm_tokens") uint32_t *diamond_wasm_tokens(const char *source) {
    size_t count = 0;
    DiamondLexer lexer;
    diamond_lexer_init(&lexer, source);
    DiamondTokenKind previous = DIAMOND_TOKEN_EOF;
    for (;;) {
        const DiamondToken token = diamond_lexer_next(&lexer);
        if (token.kind == DIAMOND_TOKEN_EOF) break;
        if (token.span.length == 0) break;
        if (token.kind != DIAMOND_TOKEN_NEWLINE) {
            if ((count + 1) * 3 + 1 > highlight_capacity) {
                const size_t capacity = highlight_capacity == 0 ? 256 : highlight_capacity * 2;
                uint32_t *grown = realloc(highlight_buffer, capacity * sizeof *grown);
                if (grown == nullptr) break;
                highlight_buffer = grown;
                highlight_capacity = capacity;
            }
            uint32_t *entry = highlight_buffer + 1 + count * 3;
            entry[0] = highlight_class(token.kind, previous, source[token.span.start]);
            entry[1] = (uint32_t)token.span.start;
            entry[2] = (uint32_t)token.span.length;
            count++;
        }
        previous = token.kind;
        if (token.kind == DIAMOND_TOKEN_ERROR) break;
    }
    if (highlight_buffer == nullptr) {
        highlight_buffer = malloc(256 * sizeof *highlight_buffer);
        highlight_capacity = 256;
        if (highlight_buffer == nullptr) return nullptr;
    }
    highlight_buffer[0] = (uint32_t)count;
    return highlight_buffer;
}

/* Completion for the page's prompt: the same engine as the terminal REPL's Tab key
 * (lsp/repl_completion.c). `cursor` is a byte offset into `line`. Returns the candidates, each
 * followed by '\n', as one NUL-terminated string valid until the next call (empty when there
 * are none, nullptr when out of memory). The module compiles the whole session plus the line
 * to answer, so a call costs about as much as one evaluation. */
static char *completion_buffer;

WASM_EXPORT("diamond_wasm_complete") const char *diamond_wasm_complete(
        const char *session, const char *pending, const char *line, size_t cursor) {
    char **labels = nullptr;
    size_t count = 0;
    if (!repl_compute_completions(session, strlen(session), pending, strlen(pending),
            line, cursor, &labels, &count)) return nullptr;
    size_t length = 1;
    for (size_t index = 0; index < count; index++) length += strlen(labels[index]) + 1;
    char *grown = realloc(completion_buffer, length);
    if (grown == nullptr) { repl_free_completions(labels, count); return nullptr; }
    completion_buffer = grown;
    char *cursor_out = completion_buffer;
    for (size_t index = 0; index < count; index++) {
        const size_t label_length = strlen(labels[index]);
        memcpy(cursor_out, labels[index], label_length);
        cursor_out += label_length;
        *cursor_out++ = '\n';
    }
    *cursor_out = '\0';
    repl_free_completions(labels, count);
    return completion_buffer;
}
