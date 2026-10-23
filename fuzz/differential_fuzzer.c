/* Differential execution fuzzer: runs one Diamond source twice in the same
 * process, once as the interpreter runs it and once with the JIT forced on at
 * the first call and minor-GC stress, and aborts if anything observable differs
 * (exit status, stdout, stderr). A program that compiles but behaves
 * differently under the JIT or under GC pressure is exactly the bug class that
 * has recurred in this project (interpreter and JIT each carry their own copy
 * of an opcode's semantics), and a hand-written corpus only reaches the
 * combinations someone thought of.
 *
 * The source is compiled and run through diamond_run_source_with_program, the
 * same path the CLI uses. The process runs sandboxed with instruction and
 * wall-clock budgets so no input touches the machine or hangs; an input that
 * hits a budget in either run is not compared (the JIT does not count
 * instructions the way the interpreter does). Inputs naming something
 * nondeterministic are skipped, because they would differ between any two
 * runs.
 *
 * Seed it with small programs from tests/cases (see docs/internal/fuzzing.md). */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiler.h"
#include "prelude.h"
#include "run_source.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const NONDETERMINISTIC[] = {
    "now", "rand", "Random", "Thread", "Process", "ENV", "gets", "sleep", "object_id",
    "Supervisor", "Channel", "IO.", "File", "Socket", "Server", "Signal", "Dir", "system",
    "exec", "spawn", "clock", "uptime", "pid", "hostname", "SecureRandom", "uuid", "Fiber",
    "debugger", "exit", "abort", "read", "write", "TLS", "Postgres", "MySQL", "SQLite3",
    "ProgramBuilder", "Time.",
};

static bool references_nondeterminism(const char *source) {
    for (size_t index = 0; index < sizeof NONDETERMINISTIC / sizeof NONDETERMINISTIC[0]; index++)
        if (strstr(source, NONDETERMINISTIC[index]) != nullptr) return true;
    return false;
}

typedef struct Capture {
    char *stdout_text;
    char *stderr_text;
    int status;
} Capture;

static char *slurp(int fd) {
    off_t length = lseek(fd, 0, SEEK_END);
    if (length < 0) length = 0;
    char *text = malloc((size_t)length + 1);
    if (text == nullptr) return nullptr;
    (void)lseek(fd, 0, SEEK_SET);
    size_t used = 0;
    while (used < (size_t)length) {
        const ssize_t got = read(fd, text + used, (size_t)length - used);
        if (got <= 0) break;
        used += (size_t)got;
    }
    text[used] = '\0';
    return text;
}

/* Runs `source` with stdout and stderr redirected to temporary files. */
static bool run_captured(const char *source, DiamondProgram *program,
        const DiamondProgram *prelude_template, Capture *out) {
    FILE *out_file = tmpfile();
    FILE *err_file = tmpfile();
    if (out_file == nullptr || err_file == nullptr) {
        if (out_file != nullptr) fclose(out_file);
        if (err_file != nullptr) fclose(err_file);
        return false;
    }
    const int out_fd = fileno(out_file);
    const int err_fd = fileno(err_file);
    fflush(stdout);
    fflush(stderr);
    const int saved_out = dup(STDOUT_FILENO);
    const int saved_err = dup(STDERR_FILENO);
    dup2(out_fd, STDOUT_FILENO);
    dup2(err_fd, STDERR_FILENO);
    diamond_program_free(program);
    out->status = diamond_run_source_with_template("fuzz.d6", source, DIAMOND_DUMP_NONE, program,
        prelude_template, 0, nullptr);
    fflush(stdout);
    fflush(stderr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    out->stdout_text = slurp(out_fd);
    out->stderr_text = slurp(err_fd);
    fclose(out_file);
    fclose(err_file);
    return out->stdout_text != nullptr && out->stderr_text != nullptr;
}

static void set_plain(void) {
    unsetenv("DIAMOND_JIT");
    unsetenv("DIAMOND_JIT_THRESHOLD");
    unsetenv("DIAMOND_STRESS_MINOR_GC");
}

static void set_stressed(void) {
    setenv("DIAMOND_JIT", "1", 1);
    setenv("DIAMOND_JIT_THRESHOLD", "1", 1);
    setenv("DIAMOND_STRESS_MINOR_GC", "1", 1);
}

static bool hit_budget(const Capture *capture) {
    return strstr(capture->stderr_text, "esource limit") != nullptr ||
           strstr(capture->stderr_text, "budget") != nullptr ||
           strstr(capture->stderr_text, "call stack overflow") != nullptr ||
           strstr(capture->stderr_text, "out of memory") != nullptr;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0 || size > 4096) return 0;
    char *source = malloc(size + 1);
    if (source == nullptr) return 0;
    memcpy(source, data, size);
    source[size] = '\0';
    if (memchr(source, '\0', size) != nullptr || references_nondeterminism(source)) {
        free(source);
        return 0;
    }
    static bool configured;
    if (!configured) {
        configured = true;
        setenv("DIAMOND_SANDBOX", "1", 1);
        setenv("DIAMOND_NO_CACHE", "1", 1);
        setenv("DIAMOND_MAX_INSTRUCTIONS", "3000000", 1);
        setenv("DIAMOND_MAX_WALL_MILLISECONDS", "3000", 1);
    }
    static DiamondProgram *program;
    if (program == nullptr) {
        program = calloc(1, sizeof *program);
        if (program == nullptr) { free(source); return 0; }
    }

    /* The prelude is compiled once and reused as the incremental-compile
     * template, as tests/run_cases.c does: re-lexing it per run dominated. */
    static DiamondProgram *prelude_template;
    if (prelude_template == nullptr) {
        prelude_template = calloc(1, sizeof *prelude_template);
        const size_t prelude_length = diamond_prelude_length(true);
        char *prelude_source = malloc(prelude_length + 1);
        if (prelude_template == nullptr || prelude_source == nullptr) abort();
        diamond_prelude_write(prelude_source, true);
        prelude_source[prelude_length] = '\0';
        DiamondDiagnostic diagnostic;
        if (!diamond_compile(prelude_source, prelude_template, &diagnostic)) abort();
        free(prelude_source);
    }

    Capture plain = {}, stressed = {};
    set_plain();
    const bool plain_ok = run_captured(source, program, prelude_template, &plain);
    set_stressed();
    const bool stressed_ok = run_captured(source, program, prelude_template, &stressed);
    set_plain();
    if (plain_ok && stressed_ok && plain.status != 65 && !hit_budget(&plain) &&
        !hit_budget(&stressed)) {
        const bool same = plain.status == stressed.status &&
            strcmp(plain.stdout_text, stressed.stdout_text) == 0 &&
            strcmp(plain.stderr_text, stressed.stderr_text) == 0;
        if (!same) {
            fprintf(stderr, "differential_fuzzer: interpreter and JIT/stress disagree\n"
                "--- source ---\n%s\n--- plain (exit %d) ---\n%s%s\n"
                "--- stressed (exit %d) ---\n%s%s\n",
                source, plain.status, plain.stdout_text, plain.stderr_text,
                stressed.status, stressed.stdout_text, stressed.stderr_text);
            abort();
        }
    }
    free(plain.stdout_text); free(plain.stderr_text);
    free(stressed.stdout_text); free(stressed.stderr_text);
    free(source);
    return 0;
}
