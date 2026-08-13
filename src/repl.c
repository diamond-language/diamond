#define _DEFAULT_SOURCE

#include "repl.h"
#include "compiler.h"
#include "loader.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static constexpr unsigned char DIAMOND_CORE_SOURCE[] = {
#embed "../lib/core.di" suffix(,)
    0
};
static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

/* Growable byte buffer, used both for the accumulated session source and
 * for a single line's worth of pending (not-yet-committed) input. Always
 * null-terminated; `length` excludes the terminator. */
typedef struct ReplBuffer {
    char *data;
    size_t length;
    size_t capacity;
} ReplBuffer;

static void buffer_init(ReplBuffer *buffer) {
    buffer->data = malloc(1);
    if (buffer->data != nullptr) buffer->data[0] = '\0';
    buffer->length = 0;
    buffer->capacity = buffer->data != nullptr ? 1 : 0;
}

static bool buffer_append(ReplBuffer *buffer, const char *text, size_t text_length) {
    if (text_length > SIZE_MAX - buffer->length - 1) return false;
    if (buffer->length + text_length + 1 > buffer->capacity) {
        size_t capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (capacity < buffer->length + text_length + 1) {
            if (capacity > SIZE_MAX / 2) return false;
            capacity *= 2;
        }
        char *grown = realloc(buffer->data, capacity);
        if (grown == nullptr) return false;
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, text, text_length);
    buffer->length += text_length;
    buffer->data[buffer->length] = '\0';
    return true;
}

static void buffer_free(ReplBuffer *buffer) {
    free(buffer->data);
    buffer->data = nullptr;
    buffer->length = 0;
    buffer->capacity = 0;
}

static void buffer_reset(ReplBuffer *buffer) {
    buffer->length = 0;
    if (buffer->data != nullptr) buffer->data[0] = '\0';
}

static size_t count_lines(const char *text) {
    size_t lines = 1;
    for (const char *cursor = text; *cursor != '\0'; cursor++)
        if (*cursor == '\n') lines++;
    return lines;
}

/* Attempts to compile `source` (the whole REPL session so far, plus the
 * newest pending input) exactly the way -e/file execution do: core.di
 * prepended, then a #line 1 reset so the diagnostic's own line/column
 * land relative to `source` itself, no offset math needed by the caller.
 * On failure, `*out_incomplete` distinguishes "ran out of input" (the
 * diagnostic landed on-or-past the last line `source` actually has, e.g.
 * "expected 'end' after if expression" at EOF) from a genuine syntax
 * error elsewhere -- the caller prompts for another line in the former
 * case, reports the error and discards the attempt in the latter. This
 * is a heuristic, not a real "unexpected EOF" signal from the parser
 * (compiler.c has no such distinct diagnostic kind); it covers the
 * common block-not-closed cases well but can't be perfect over every
 * possible trailing syntax error. */
static bool try_compile(const char *source, DiamondProgram *program,
                        bool *out_incomplete, char *error_buffer,
                        size_t error_buffer_size) {
    *out_incomplete = false;
    DiamondSourceBundle bundle;
    char load_error[768];
    if (!diamond_load_program("<repl>", source, &bundle, load_error, sizeof load_error)) {
        (void)snprintf(error_buffer, error_buffer_size, "%s", load_error);
        return false;
    }
    const size_t core_length = sizeof(DIAMOND_CORE_SOURCE) - 1;
    const size_t reset_length = sizeof(DIAMOND_USER_LINE_RESET) - 1;
    const size_t source_length = strlen(bundle.source);
    char *combined = malloc(core_length + reset_length + source_length + 1);
    if (combined == nullptr) {
        (void)snprintf(error_buffer, error_buffer_size, "out of memory");
        diamond_source_bundle_free(&bundle);
        return false;
    }
    memcpy(combined, DIAMOND_CORE_SOURCE, core_length);
    memcpy(combined + core_length, DIAMOND_USER_LINE_RESET, reset_length);
    memcpy(combined + core_length + reset_length, bundle.source, source_length + 1);

    DiamondDiagnostic diagnostic;
    const bool ok = diamond_compile(combined, program, &diagnostic);
    if (!ok) {
        const DiamondResolvedLocation resolved = diamond_resolve_diagnostic_location(
            "<repl>", combined, diagnostic, &bundle, core_length + reset_length);
        (void)snprintf(error_buffer, error_buffer_size, "%zu:%zu: error: %s",
                       resolved.line, resolved.column, diagnostic.message);
        *out_incomplete = strcmp(resolved.path, "<repl>") == 0 &&
            resolved.line >= count_lines(source);
    }
    free(combined);
    diamond_source_bundle_free(&bundle);
    return ok;
}

/* Runs a successfully compiled candidate, capturing everything the
 * program writes to stdout (puts/print, including replayed prior
 * output from earlier in the session -- see the caller) into `capture`
 * rather than the real terminal. Returns the top-level result value and
 * whether the run itself succeeded; on failure `*out_error` is set to a
 * static status/message string, not owned by the caller. */
static bool run_candidate(DiamondProgram *program, DiamondValue *out_result,
                          const char **out_error, char *error_buffer,
                          size_t error_buffer_size) {
    DiamondChunk chunk = diamond_program_chunk(program);
    chunk.name = "<repl>";
    DiamondVm vm;
    diamond_vm_init(&vm);
    const DiamondVmStatus status = diamond_vm_run(&vm, &chunk, out_result);
    if (status != DIAMOND_VM_OK) {
        const char *detail = diamond_vm_error(&vm);
        (void)snprintf(error_buffer, error_buffer_size, "runtime error: %s",
                       detail != nullptr ? detail : diamond_vm_status_name(status));
        *out_error = error_buffer;
        diamond_vm_free(&vm);
        return false;
    }
    diamond_vm_free(&vm);
    return true;
}

int diamond_repl_run(void) {
    printf("diamond REPL -- Ctrl-D to exit\n");

    FILE *capture = tmpfile();
    if (capture == nullptr) {
        fprintf(stderr, "diamond: cannot create temporary file for REPL output capture\n");
        return 74;
    }
    const int real_stdout_fd = dup(STDOUT_FILENO);
    if (real_stdout_fd < 0) {
        fprintf(stderr, "diamond: cannot duplicate stdout\n");
        fclose(capture);
        return 74;
    }
    FILE *real_stdout = fdopen(real_stdout_fd, "w");
    if (real_stdout == nullptr) {
        fprintf(stderr, "diamond: cannot open stdout duplicate\n");
        close(real_stdout_fd);
        fclose(capture);
        return 74;
    }

    ReplBuffer session;
    buffer_init(&session);
    ReplBuffer pending;
    buffer_init(&pending);
    ReplBuffer previous_output;
    buffer_init(&previous_output);
    char *line = nullptr;
    size_t line_capacity = 0;
    char error_message[512];

    bool eof = false;
    while (!eof) {
        fputs(">>> ", real_stdout);
        fflush(real_stdout);
        buffer_reset(&pending);
        bool have_candidate = false;
        DiamondProgram *program = nullptr;

        for (;;) {
            const ssize_t got = getline(&line, &line_capacity, stdin);
            if (got < 0) {
                eof = true;
                break;
            }
            if (!buffer_append(&pending, line, (size_t)got)) {
                fprintf(stderr, "diamond: out of memory reading input\n");
                eof = true;
                break;
            }

            ReplBuffer candidate_source;
            buffer_init(&candidate_source);
            if (!buffer_append(&candidate_source, session.data, session.length) ||
                !buffer_append(&candidate_source, pending.data, pending.length)) {
                fprintf(stderr, "diamond: out of memory reading input\n");
                buffer_free(&candidate_source);
                eof = true;
                break;
            }

            program = malloc(sizeof *program);
            if (program == nullptr) {
                fprintf(stderr, "diamond: out of memory allocating program\n");
                buffer_free(&candidate_source);
                eof = true;
                break;
            }
            bool incomplete = false;
            const bool ok = try_compile(candidate_source.data, program, &incomplete,
                                        error_message, sizeof error_message);
            buffer_free(&candidate_source);
            if (ok) {
                have_candidate = true;
                break;
            }
            if (incomplete) {
                free(program);
                program = nullptr;
                fputs("... ", real_stdout);
                fflush(real_stdout);
                continue;
            }
            fprintf(real_stdout, "%s\n", error_message);
            free(program);
            program = nullptr;
            break;
        }

        if (!have_candidate) continue;

        rewind(capture);
        if (ftruncate(fileno(capture), 0) != 0) {
            fprintf(stderr, "diamond: cannot reset output capture\n");
        }
        FILE *saved_stdout_stream = stdout;
        stdout = capture;

        DiamondValue result = DIAMOND_NIL;
        const char *run_error = nullptr;
        const bool ran_ok = run_candidate(program, &result, &run_error,
                                          error_message, sizeof error_message);

        fflush(capture);
        stdout = saved_stdout_stream;

        const long captured_length = ftell(capture);
        char *captured = nullptr;
        if (captured_length > 0) {
            captured = malloc((size_t)captured_length + 1);
            if (captured != nullptr) {
                rewind(capture);
                const size_t read_count = fread(captured, 1, (size_t)captured_length, capture);
                captured[read_count] = '\0';
            }
        }
        const char *new_output = captured != nullptr ? captured : "";
        const size_t new_output_length = strlen(new_output);
        if (new_output_length >= previous_output.length &&
            memcmp(new_output, previous_output.data, previous_output.length) == 0) {
            fputs(new_output + previous_output.length, real_stdout);
        } else {
            /* Prior output wasn't reproduced verbatim (non-deterministic
             * replay -- timing, I/O ordering, ...); fall back to showing
             * everything this run produced rather than guessing wrong. */
            fputs(new_output, real_stdout);
        }

        if (ran_ok) {
            diamond_value_fprint(real_stdout, result);
            fputc('\n', real_stdout);
            buffer_reset(&previous_output);
            buffer_append(&previous_output, new_output, new_output_length);
            /* Commit: session becomes session+pending for the next round,
             * so later input can see this round's definitions/variables. */
            ReplBuffer committed;
            buffer_init(&committed);
            buffer_append(&committed, session.data, session.length);
            buffer_append(&committed, pending.data, pending.length);
            buffer_free(&session);
            session = committed;
        } else {
            fprintf(real_stdout, "%s\n", run_error != nullptr ? run_error : "unknown error");
        }
        free(captured);
        free(program);
    }

    fputc('\n', real_stdout);
    free(line);
    buffer_free(&session);
    buffer_free(&pending);
    buffer_free(&previous_output);
    fclose(capture);
    fclose(real_stdout);
    return 0;
}
