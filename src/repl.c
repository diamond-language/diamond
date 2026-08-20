#define _DEFAULT_SOURCE

#include "repl.h"
#include "compiler.h"
#include "loader.h"
#include "value.h"
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static constexpr unsigned char DIAMOND_CORE_SOURCE[] = {
#embed "../lib/core.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_STRING_BUILDER_SOURCE[] = {
#embed "../lib/core/string_builder.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_NUMERIC_SOURCE[] = {
#embed "../lib/core/numeric.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_JSON_CODEC_SOURCE[] = {
#embed "../lib/core/json_codec.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_JSON_SOURCE[] = {
#embed "../lib/core/json.di" suffix(,)
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

/* Persisted across sessions at $HOME/.diamond_history (one entry per
 * line, matching bash_history's own plain-text convention -- each entry
 * here is already guaranteed newline-free, since it's exactly one
 * physical line submitted at the prompt). `file` stays open for the
 * whole interactive session and every new entry is written+flushed
 * immediately (not just buffered and written at exit), so history
 * survives a `kill -9` or crash, not only a clean Ctrl-D exit. Unbounded
 * growth (no HISTSIZE-style cap) is a known simplification, not a
 * deliberate stance -- see docs/repl.md. */
typedef struct ReplHistory {
    char **entries;
    size_t count;
    size_t capacity;
    FILE *file;
} ReplHistory;

static char *history_file_path(void) {
    const char *home = getenv("HOME");
    if (home == nullptr || home[0] == '\0') return nullptr;
    static constexpr char suffix[] = "/.diamond_history";
    const size_t home_length = strlen(home);
    char *path = malloc(home_length + sizeof suffix);
    if (path == nullptr) return nullptr;
    memcpy(path, home, home_length);
    memcpy(path + home_length, suffix, sizeof suffix);
    return path;
}

/* Loads existing entries (if any) and opens the file for append so new
 * entries in this session join them. A missing/unreadable/unwritable
 * history file is not fatal -- `history->file` just stays nullptr and
 * every history_add below silently skips persistence, in-memory
 * navigation still works for the rest of the session. */
static void history_init(ReplHistory *history) {
    *history = (ReplHistory){};
    char *path = history_file_path();
    if (path == nullptr) return;
    FILE *existing = fopen(path, "r");
    if (existing != nullptr) {
        char *line = nullptr;
        size_t line_capacity = 0;
        ssize_t got;
        while ((got = getline(&line, &line_capacity, existing)) >= 0) {
            while (got > 0 && (line[got - 1] == '\n' || line[got - 1] == '\r')) got--;
            if (got == 0) continue;
            if (history->count == history->capacity) {
                const size_t capacity = history->capacity == 0 ? 64 : history->capacity * 2;
                char **grown = realloc(history->entries, capacity * sizeof *grown);
                if (grown == nullptr) break;
                history->entries = grown;
                history->capacity = capacity;
            }
            char *entry = malloc((size_t)got + 1);
            if (entry == nullptr) break;
            memcpy(entry, line, (size_t)got);
            entry[got] = '\0';
            history->entries[history->count++] = entry;
        }
        free(line);
        fclose(existing);
    }
    history->file = fopen(path, "a");
    free(path);
}

/* Skips an empty/whitespace-only line (nothing worth navigating back to)
 * and an exact repeat of the immediately preceding entry (matching
 * bash's own default `ignoredups`-like behavior -- pressing Up repeatedly
 * after re-running the same command shouldn't require stepping through
 * every identical copy). */
static void history_add(ReplHistory *history, const char *line) {
    bool blank = true;
    for (const char *cursor = line; *cursor != '\0'; cursor++)
        if (*cursor != ' ' && *cursor != '\t') { blank = false; break; }
    if (blank) return;
    if (history->count > 0 && strcmp(history->entries[history->count - 1], line) == 0) return;
    if (history->count == history->capacity) {
        const size_t capacity = history->capacity == 0 ? 64 : history->capacity * 2;
        char **grown = realloc(history->entries, capacity * sizeof *grown);
        if (grown == nullptr) return;
        history->entries = grown;
        history->capacity = capacity;
    }
    char *entry = malloc(strlen(line) + 1);
    if (entry == nullptr) return;
    strcpy(entry, line);
    history->entries[history->count++] = entry;
    if (history->file != nullptr) {
        fprintf(history->file, "%s\n", line);
        fflush(history->file);
    }
}

static void history_free(ReplHistory *history) {
    for (size_t index = 0; index < history->count; index++) free(history->entries[index]);
    free(history->entries);
    if (history->file != nullptr) fclose(history->file);
    *history = (ReplHistory){};
}

typedef enum ReplLineResult {
    REPL_LINE_OK,
    REPL_LINE_EOF,
    REPL_LINE_INTERRUPTED,
} ReplLineResult;

/* Redraws the entire current line from scratch: return to column 0,
 * clear to end of line, print prompt+buffer, then reposition the cursor.
 * Simplest correct approach for a from-scratch line editor (as opposed
 * to diffing against the previously drawn state and emitting minimal
 * updates) -- some extra bytes written per keystroke, never a wrong
 * on-screen result. Doesn't account for the buffer wrapping past the
 * terminal's own width (the cursor-repositioning escape below moves
 * within the current terminal row only): a known, acceptable limitation
 * for a REPL where the common "long input" case is already handled by
 * separate physical lines via multi-line continuation, not one very long
 * single line. */
static void redraw_line(FILE *out, const char *prompt, const char *buffer, size_t cursor) {
    fputs("\r\x1b[K", out);
    fputs(prompt, out);
    fputs(buffer, out);
    const size_t length = strlen(buffer);
    if (cursor < length) fprintf(out, "\x1b[%zuD", length - cursor);
    fflush(out);
}

static bool read_raw_byte(unsigned char *out) {
    for (;;) {
        const ssize_t got = read(STDIN_FILENO, out, 1);
        if (got == 1) return true;
        if (got < 0 && errno == EINTR) continue;
        return false;
    }
}

/* Reads one line with basic in-place editing: printable character
 * insertion at the cursor, Backspace, Left/Right/Home/End/Delete, and
 * Up/Down for history navigation -- while composing a fresh line, the
 * first Up stashes whatever was already typed so Down can restore it
 * after navigating back past the newest history entry, matching
 * bash/readline's own convention. Ctrl-C discards the in-progress line
 * (REPL_LINE_INTERRUPTED; the caller decides what "discard" means for
 * anything accumulated across earlier lines of a multi-line block).
 * Ctrl-D exits only on an empty line (REPL_LINE_EOF), matching common
 * shell convention -- on a non-empty line it's a no-op, not a forced
 * submit or a deletion. A raw standalone Escape keypress (no following
 * CSI bytes) blocks waiting for the next byte rather than resolving
 * immediately, a known limitation of not implementing read-with-timeout
 * disambiguation here -- see docs/repl.md. */
static ReplLineResult read_line_interactive(FILE *out, ReplHistory *history,
        const char *prompt, ReplBuffer *out_line) {
    char *buffer = malloc(1);
    if (buffer == nullptr) return REPL_LINE_EOF;
    buffer[0] = '\0';
    size_t length = 0;
    size_t capacity = 1;
    size_t cursor = 0;
    size_t history_position = history->count;
    char *stashed_live = nullptr;

    fputs(prompt, out);
    fflush(out);

    ReplLineResult result = REPL_LINE_OK;
    for (;;) {
        unsigned char byte = 0;
        if (!read_raw_byte(&byte)) { result = REPL_LINE_EOF; break; }

        if (byte == '\r' || byte == '\n') {
            fputs("\r\n", out);
            fflush(out);
            break;
        }
        if (byte == 0x03) { /* Ctrl-C */
            fputs("^C\r\n", out);
            fflush(out);
            result = REPL_LINE_INTERRUPTED;
            break;
        }
        if (byte == 0x04) { /* Ctrl-D */
            if (length == 0) { result = REPL_LINE_EOF; break; }
            continue;
        }
        if (byte == 0x7f || byte == 0x08) { /* Backspace */
            if (cursor > 0) {
                memmove(buffer + cursor - 1, buffer + cursor, length - cursor);
                cursor--; length--;
                buffer[length] = '\0';
                redraw_line(out, prompt, buffer, cursor);
            }
            continue;
        }
        if (byte == 0x1b) { /* ESC -- CSI sequence expected */
            unsigned char next = 0;
            if (!read_raw_byte(&next)) { result = REPL_LINE_EOF; break; }
            if (next != '[') continue;
            unsigned char params[16];
            size_t param_count = 0;
            unsigned char final_byte = 0;
            for (;;) {
                if (!read_raw_byte(&next)) { final_byte = 0; break; }
                if (next >= 0x40 && next <= 0x7e) { final_byte = next; break; }
                if (param_count < sizeof params) params[param_count++] = next;
            }
            if (final_byte == 'A' || final_byte == 'B') { /* Up / Down */
                if (final_byte == 'A' && history_position > 0) {
                    if (history_position == history->count) {
                        free(stashed_live);
                        stashed_live = malloc(length + 1);
                        if (stashed_live != nullptr) memcpy(stashed_live, buffer, length + 1);
                    }
                    history_position--;
                } else if (final_byte == 'B' && history_position < history->count) {
                    history_position++;
                } else {
                    continue;
                }
                const char *replacement = history_position == history->count
                    ? (stashed_live != nullptr ? stashed_live : "")
                    : history->entries[history_position];
                const size_t replacement_length = strlen(replacement);
                if (replacement_length + 1 > capacity) {
                    char *grown = realloc(buffer, replacement_length + 1);
                    if (grown == nullptr) continue;
                    buffer = grown;
                    capacity = replacement_length + 1;
                }
                memcpy(buffer, replacement, replacement_length + 1);
                length = replacement_length;
                cursor = length;
                redraw_line(out, prompt, buffer, cursor);
                continue;
            }
            if (final_byte == 'C' && cursor < length) { /* Right */
                cursor++;
                redraw_line(out, prompt, buffer, cursor);
                continue;
            }
            if (final_byte == 'D' && cursor > 0) { /* Left */
                cursor--;
                redraw_line(out, prompt, buffer, cursor);
                continue;
            }
            if (final_byte == 'H' || (final_byte == '~' && param_count == 1 && params[0] == '1')) {
                cursor = 0; /* Home */
                redraw_line(out, prompt, buffer, cursor);
                continue;
            }
            if (final_byte == 'F' || (final_byte == '~' && param_count == 1 && params[0] == '4')) {
                cursor = length; /* End */
                redraw_line(out, prompt, buffer, cursor);
                continue;
            }
            if (final_byte == '~' && param_count == 1 && params[0] == '3') { /* Delete */
                if (cursor < length) {
                    memmove(buffer + cursor, buffer + cursor + 1, length - cursor - 1);
                    length--;
                    buffer[length] = '\0';
                    redraw_line(out, prompt, buffer, cursor);
                }
                continue;
            }
            continue; /* recognized-CSI-shape but unhandled: already swallowed above */
        }
        if (byte < 0x20) continue; /* any other control byte: ignore */

        if (length + 2 > capacity) {
            const size_t grown_capacity = capacity < 64 ? 64 : capacity * 2;
            char *grown = realloc(buffer, grown_capacity);
            if (grown == nullptr) continue;
            buffer = grown;
            capacity = grown_capacity;
        }
        memmove(buffer + cursor + 1, buffer + cursor, length - cursor);
        buffer[cursor] = (char)byte;
        cursor++; length++;
        buffer[length] = '\0';
        redraw_line(out, prompt, buffer, cursor);
    }

    if (result == REPL_LINE_OK) {
        history_add(history, buffer);
        buffer_append(out_line, buffer, length);
        buffer_append(out_line, "\n", 1);
    }
    free(buffer);
    free(stashed_live);
    return result;
}

/* True if `line` (a single physical line, trailing '\n' and all) is
 * nothing but "exit" or "quit", modulo surrounding whitespace -- the two
 * names every reasonably popular REPL treats as "leave now" regardless
 * of what the host language actually calls its own exit builtin (Diamond
 * has none at all). Case-sensitive, matching the exact spelling every
 * such REPL actually recognizes; a program that happens to have a local
 * variable or function named exactly `exit`/`quit` is vanishingly
 * unlikely and, if it ever happens, is only shadowed at the REPL prompt
 * itself, not in the language. */
static bool is_bare_exit_command(const char *line) {
    size_t start = 0;
    while (line[start] == ' ' || line[start] == '\t') start++;
    size_t end = strlen(line);
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                            line[end - 1] == '\r' || line[end - 1] == '\n'))
        end--;
    const size_t length = end - start;
    return (length == 4 && memcmp(line + start, "exit", 4) == 0) ||
           (length == 4 && memcmp(line + start, "quit", 4) == 0);
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
    const size_t string_builder_length =
        sizeof(DIAMOND_CORE_STRING_BUILDER_SOURCE) - 1;
    const size_t numeric_length = sizeof(DIAMOND_CORE_NUMERIC_SOURCE) - 1;
    const size_t json_codec_length =
        sizeof(DIAMOND_CORE_JSON_CODEC_SOURCE) - 1;
    const size_t json_length = sizeof(DIAMOND_CORE_JSON_SOURCE) - 1;
    const size_t reset_length = sizeof(DIAMOND_USER_LINE_RESET) - 1;
    const size_t source_length = strlen(bundle.source);
    char *combined = malloc(core_length + string_builder_length + numeric_length +
                            json_codec_length + json_length +
                            reset_length + source_length + 1);
    if (combined == nullptr) {
        (void)snprintf(error_buffer, error_buffer_size, "out of memory");
        diamond_source_bundle_free(&bundle);
        return false;
    }
    memcpy(combined, DIAMOND_CORE_NUMERIC_SOURCE, numeric_length);
    memcpy(combined + numeric_length, DIAMOND_CORE_SOURCE, core_length);
    memcpy(combined + numeric_length + core_length,
           DIAMOND_CORE_STRING_BUILDER_SOURCE, string_builder_length);
        memcpy(combined + numeric_length + core_length + string_builder_length,
            DIAMOND_CORE_JSON_CODEC_SOURCE, json_codec_length);
        memcpy(combined + numeric_length + core_length + string_builder_length +
            json_codec_length, DIAMOND_CORE_JSON_SOURCE, json_length);
        memcpy(combined + numeric_length + core_length + string_builder_length +
            json_codec_length + json_length,
            DIAMOND_USER_LINE_RESET, reset_length);
        memcpy(combined + numeric_length + core_length + string_builder_length +
            json_codec_length + json_length + reset_length,
            bundle.source, source_length + 1);

        DiamondDiagnostic diagnostic;
    const bool ok = diamond_compile(combined, program, &diagnostic);
    if (!ok) {
        const DiamondResolvedLocation resolved = diamond_resolve_diagnostic_location(
            "<repl>", combined, diagnostic, &bundle,
            core_length + string_builder_length + numeric_length +
            json_codec_length + json_length +
            reset_length);
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
                          DiamondVm *out_vm,
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
    /* The result may point into this VM's managed heap. Keep the VM alive
     * until the caller has printed the result and committed the candidate. */
    *out_vm = vm;
    return true;
}

static bool repl_pending_is_declaration(const char *source) {
    while (*source == ' ' || *source == '\t' || *source == '\r' || *source == '\n')
        source++;
    return strncmp(source, "def ", 4) == 0 || strncmp(source, "class ", 6) == 0 ||
        strncmp(source, "module ", 7) == 0 || strncmp(source, "interface ", 10) == 0;
}

static bool append_repl_assignment(ReplBuffer *destination, const ReplBuffer *pending) {
    const char *source = pending->data;
    while (*source == ' ' || *source == '\t') source++;
    const char *name = source;
    while ((*source >= 'a' && *source <= 'z') || (*source >= 'A' && *source <= 'Z') ||
           (*source >= '0' && *source <= '9') || *source == '_') source++;
    if (source == name) return false;
    const char *equals = source;
    while (*equals == ' ' || *equals == '\t') equals++;
    if (*equals != '=' || equals[1] == '=') return false;
    if (!buffer_append(destination, pending->data, pending->length) ||
        !buffer_append(destination, "_ = ", 4) ||
        !buffer_append(destination, name, (size_t)(source - name)) ||
        !buffer_append(destination, "\n", 1)) return false;
    return true;
}

static bool append_repl_pending(ReplBuffer *destination, const ReplBuffer *pending) {
    if (repl_pending_is_declaration(pending->data)) {
        return buffer_append(destination, pending->data, pending->length);
    }
    if (append_repl_assignment(destination, pending)) return true;
    size_t expression_length = pending->length;
    while (expression_length > 0 &&
           (pending->data[expression_length - 1] == '\n' ||
            pending->data[expression_length - 1] == '\r')) expression_length--;
    if (!buffer_append(destination, "_ = (", 5) ||
        !buffer_append(destination, pending->data, expression_length) ||
        !buffer_append(destination, ")\n", 2)) return false;
    return true;
}

int diamond_repl_run(void) {
    printf("diamond REPL -- Ctrl-D, exit, or quit to leave\n");

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
    buffer_append(&session, "_ = nil\n", 8);
    ReplBuffer pending;
    buffer_init(&pending);
    ReplBuffer previous_output;
    buffer_init(&previous_output);
    char *line = nullptr;
    size_t line_capacity = 0;
    char error_message[512];

    /* Raw-mode line editing (history navigation, in-place cursor
     * movement/backspace, Ctrl-C aborting the current input instead of
     * killing the process) only makes sense against a real terminal --
     * tests/repl_test.sh deliberately drives this over a bash coproc
     * (a pipe, not a pty) via DIAMOND_FORCE_REPL, so falling back to the
     * exact prior getline()-based behavior whenever stdin isn't a tty
     * keeps that suite passing unchanged, not just working around it. */
    const bool interactive = isatty(STDIN_FILENO) != 0;
    struct termios original_termios;
    ReplHistory history = {};
    if (interactive) {
        history_init(&history);
        if (tcgetattr(STDIN_FILENO, &original_termios) == 0) {
            struct termios raw = original_termios;
            raw.c_lflag &= (unsigned)~(ECHO | ICANON | ISIG);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
    }

    bool eof = false;
    while (!eof) {
        buffer_reset(&pending);
        bool have_candidate = false;
        DiamondProgram *program = nullptr;
        const char *prompt = ">>> ";

        for (;;) {
            ReplLineResult line_result;
            ReplBuffer line_buffer;
            buffer_init(&line_buffer);
            if (interactive) {
                line_result = read_line_interactive(real_stdout, &history, prompt, &line_buffer);
            } else {
                fputs(prompt, real_stdout);
                fflush(real_stdout);
                const ssize_t got = getline(&line, &line_capacity, stdin);
                if (got < 0) {
                    line_result = REPL_LINE_EOF;
                } else {
                    line_result = REPL_LINE_OK;
                    if (!buffer_append(&line_buffer, line, (size_t)got)) {
                        fprintf(stderr, "diamond: out of memory reading input\n");
                        line_result = REPL_LINE_EOF;
                    }
                }
            }
            if (line_result == REPL_LINE_EOF) {
                buffer_free(&line_buffer);
                eof = true;
                break;
            }
            if (line_result == REPL_LINE_INTERRUPTED) {
                buffer_free(&line_buffer);
                break;
            }
            /* `exit`/`quit`, bare, as the first line of a fresh
             * statement (pending is still empty) -- irb/pry/python-REPL
             * convention for "leave the REPL", distinct from Ctrl-D and
             * worth having since not every user reaches for Ctrl-D by
             * instinct. Deliberately only recognized here, not as a
             * real language builtin: neither name means anything to
             * ordinary Diamond code, and gating on "still the first
             * line of pending" avoids misfiring mid-continuation (e.g.
             * a string literal or comment that happens to contain the
             * word on its own line). */
            if (pending.length == 0 && is_bare_exit_command(line_buffer.data)) {
                buffer_free(&line_buffer);
                eof = true;
                break;
            }

            const bool appended = buffer_append(&pending, line_buffer.data, line_buffer.length);
            buffer_free(&line_buffer);
            if (!appended) {
                fprintf(stderr, "diamond: out of memory reading input\n");
                eof = true;
                break;
            }

            ReplBuffer candidate_source;
            buffer_init(&candidate_source);
            if (!buffer_append(&candidate_source, session.data, session.length) ||
                !append_repl_pending(&candidate_source, &pending)) {
                fprintf(stderr, "diamond: out of memory reading input\n");
                buffer_free(&candidate_source);
                eof = true;
                break;
            }

            program = calloc(1,sizeof *program);
            if (program == nullptr) {
                fprintf(stderr, "diamond: out of memory allocating program\n");
                buffer_free(&candidate_source);
                eof = true;
                break;
            }
            program->allow_top_level_redefinition = true;
            bool incomplete = false;
            const bool ok = try_compile(candidate_source.data, program, &incomplete,
                                        error_message, sizeof error_message);
            buffer_free(&candidate_source);
            if (ok) {
                have_candidate = true;
                break;
            }
            if (incomplete) {
                diamond_program_free(program);
                free(program);
                program = nullptr;
                prompt = "... ";
                continue;
            }
            if (!repl_pending_is_declaration(pending.data)) {
                diamond_program_free(program);
                free(program);
                program = calloc(1, sizeof *program);
                bool raw_incomplete = false;
                if (program != nullptr) {
                    ReplBuffer raw_source;
                    buffer_init(&raw_source);
                    const bool raw_appended =
                        buffer_append(&raw_source, session.data, session.length) &&
                        buffer_append(&raw_source, pending.data, pending.length);
                    const bool raw_ok = raw_appended && try_compile(
                        raw_source.data, program, &raw_incomplete, error_message,
                        sizeof error_message);
                    buffer_free(&raw_source);
                    if (!raw_ok && raw_incomplete) {
                        diamond_program_free(program);
                        free(program);
                        program = nullptr;
                        prompt = "... ";
                        continue;
                    }
                    if (raw_ok) {
                        diamond_program_free(program);
                    }
                }
                if (program == nullptr) {
                    fprintf(stderr, "diamond: out of memory allocating program\n");
                    eof = true;
                    break;
                }
                diamond_program_free(program);
                free(program);
                program = calloc(1, sizeof *program);
                if (program == nullptr) {
                    fprintf(stderr, "diamond: out of memory allocating program\n");
                    eof = true;
                    break;
                }
                ReplBuffer wrapped_source;
                buffer_init(&wrapped_source);
                buffer_append(&wrapped_source, session.data, session.length);
                append_repl_pending(&wrapped_source, &pending);
                try_compile(wrapped_source.data, program, &incomplete,
                            error_message, sizeof error_message);
                buffer_free(&wrapped_source);
            }
            fprintf(real_stdout, "%s\n", error_message);
            diamond_program_free(program);
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
        DiamondVm result_vm;
        const char *run_error = nullptr;
        const bool ran_ok = run_candidate(program, &result, &result_vm,
                          &run_error,
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
            append_repl_pending(&committed, &pending);
            buffer_free(&session);
            session = committed;
        } else {
            fprintf(real_stdout, "%s\n", run_error != nullptr ? run_error : "unknown error");
        }
        if (ran_ok) diamond_vm_free(&result_vm);
        free(captured);
        diamond_program_free(program);
        free(program);
    }

    fputc('\n', real_stdout);
    if (interactive) {
        tcsetattr(STDIN_FILENO, TCSANOW, &original_termios);
        history_free(&history);
    }
    free(line);
    buffer_free(&session);
    buffer_free(&pending);
    buffer_free(&previous_output);
    fclose(capture);
    fclose(real_stdout);
    return 0;
}
