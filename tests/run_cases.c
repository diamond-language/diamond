/* Runs every selected .di file under tests/cases through diamond_run_source
 * (src/run_source.c) *in this one process*, instead of tests/run.sh
 * spawning a fresh `diamond` process per file -- see docs/roadmap.md for
 * why that was the dominant cost of a full `make test` run (measured:
 * ~60s wall clock, 10s user / 46s sys -- the split itself pointing at
 * process-spawn overhead, not compiler/VM CPU work).
 *
 * This tool does the *running* only. Every actual pass/fail comparison
 * (exact match, substring, BRE `grep -q` patterns, last-line-only) stays
 * in tests/run.sh exactly as it already was -- reimplementing that
 * matching logic here risked subtly diverging from real grep/bash
 * semantics for no benefit, when the actual cost being eliminated is
 * process-spawning `diamond`, not the handful of `grep`/`cat` calls bash
 * already makes. For each case this writes, into `<output_dir>/`:
 *   <name>.stdout    -- captured stdout only
 *   <name>.combined  -- stdout bytes immediately followed by stderr
 *                        bytes (see run_one_case's own comment for why
 *                        this exactly matches a real `2>&1` merge here)
 *   <name>.exitcode  -- the process exit code diamond_run_source
 *                        returned, as plain text
 * tests/run.sh reads these back with bash's own `$(<file)` builtin
 * (zero subprocess cost) in place of spawning `diamond` per case. */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "prelude.h"
#include "run_source.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

enum { PATH_BUFFER_SIZE = 4096 };

/* snprintf wrapper that reports truncation as a genuine failure instead
 * of silently formatting a shortened path -- PATH_BUFFER_SIZE (4096)
 * comfortably covers any real cases_dir/output_dir/case name this tool
 * is ever invoked with, so truncation here would only mean something
 * is already very wrong; still cheap to catch outright here rather than
 * let a caller silently operate on the wrong path and fail somewhere
 * else with a much less obvious error (also what lets the compiler see
 * every one of these format calls as checked, not a potential
 * -Wformat-truncation). */
static bool format_path(char *buffer, size_t size, const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(buffer, size, format, args);
    va_end(args);
    return written >= 0 && (size_t)written < size;
}

/* Every DIAMOND_* environment variable diamond_run_source or the VM it
 * drives ever reads (src/run_source.c, src/vm.h) -- cleared before each
 * case so one case's own .env file, or anything already set in the
 * environment this tool itself was launched under, can never leak into
 * the next case. Real subprocess isolation (what tests/run.sh used to
 * get for free by spawning a fresh process per case) has to be
 * reproduced by hand now that every case shares one process. */
static const char *const DIAMOND_ENV_VARS[] = {
    "DIAMOND_STRESS_GC", "DIAMOND_QUICKEN", "DIAMOND_QUICKEN_THRESHOLD",
    "DIAMOND_IC_MONO_THRESHOLD", "DIAMOND_REPEAT", "DIAMOND_INVALIDATE_IC_EACH_RUN",
    "DIAMOND_TRACE_IC_EACH_RUN", "DIAMOND_TRACE_IC", "DIAMOND_TRACE_IC_SITES",
    "DIAMOND_TRACE_IC_FAST", "DIAMOND_TRACE_IC_PROBES", "DIAMOND_TRACE_IC_REWRITES",
    "DIAMOND_TRACE_IC_POLICY", "DIAMOND_TRACE_SHAPES", "DIAMOND_TRACE_FIELDS",
    "DIAMOND_TRACE_OPCODES", "DIAMOND_TRACE_QUICKEN", "DIAMOND_FORCE_REPL",
};
static constexpr size_t DIAMOND_ENV_VAR_COUNT =
    sizeof(DIAMOND_ENV_VARS) / sizeof(DIAMOND_ENV_VARS[0]);

static char *read_whole_file(const char *path, size_t *out_length) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return nullptr;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return nullptr; }
    const long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return nullptr; }
    char *buffer = malloc((size_t)length + 1);
    if (buffer == nullptr) { fclose(file); return nullptr; }
    const size_t read_count = fread(buffer, 1, (size_t)length, file);
    fclose(file);
    if (read_count != (size_t)length) { free(buffer); return nullptr; }
    buffer[read_count] = '\0';
    if (out_length != nullptr) *out_length = read_count;
    return buffer;
}

static bool write_whole_file(const char *path, const char *data, size_t length) {
    FILE *file = fopen(path, "wb");
    if (file == nullptr) return false;
    const bool wrote_all = fwrite(data, 1, length, file) == length;
    return fclose(file) == 0 && wrote_all;
}

static bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

/* Applies <base>.env's own KEY=VALUE lines (one override per line,
 * blank lines skipped) on top of the already-cleared baseline
 * environment -- mirrors what tests/run.sh's own `env "${case_env[@]}"
 * ...` used to do for a fresh subprocess. */
static bool apply_env_file(const char *env_path) {
    if (!file_exists(env_path)) return true;
    size_t length = 0;
    char *content = read_whole_file(env_path, &length);
    if (content == nullptr) {
        fprintf(stderr, "run_cases: cannot read %s\n", env_path);
        return false;
    }
    bool ok = true;
    char *line = content;
    while (ok && line < content + length) {
        char *newline = strchr(line, '\n');
        if (newline != nullptr) *newline = '\0';
        if (*line != '\0') {
            char *equals = strchr(line, '=');
            if (equals == nullptr) {
                fprintf(stderr, "run_cases: malformed line in %s: %s\n", env_path, line);
                ok = false;
            } else {
                *equals = '\0';
                if (setenv(line, equals + 1, 1) != 0) {
                    fprintf(stderr, "run_cases: setenv failed for %s\n", line);
                    ok = false;
                }
            }
        }
        if (newline == nullptr) break;
        line = newline + 1;
    }
    free(content);
    return ok;
}

/* <base>.flags: only ever "--dump-bytecode" across this repo today.
 * Anything else aborts loudly rather than silently ignoring a flag a
 * future test file might actually need honored. */
static bool read_flags_file(const char *flags_path, bool *out_dump_bytecode) {
    *out_dump_bytecode = false;
    if (!file_exists(flags_path)) return true;
    size_t length = 0;
    char *content = read_whole_file(flags_path, &length);
    if (content == nullptr) {
        fprintf(stderr, "run_cases: cannot read %s\n", flags_path);
        return false;
    }
    bool ok = true;
    char *line = content;
    while (ok && line < content + length) {
        char *newline = strchr(line, '\n');
        if (newline != nullptr) *newline = '\0';
        if (*line != '\0') {
            if (strcmp(line, "--dump-bytecode") == 0) {
                *out_dump_bytecode = true;
            } else {
                fprintf(stderr, "run_cases: unsupported flag in %s: %s\n", flags_path, line);
                ok = false;
            }
        }
        if (newline == nullptr) break;
        line = newline + 1;
    }
    free(content);
    return ok;
}

/* Redirects fd `target` (STDOUT_FILENO or STDERR_FILENO) to `path`,
 * returning a saved dup() of the original the caller passes back to
 * restore_fd once done, or -1 on failure. */
static int redirect_fd(int target, const char *path) {
    fflush(target == STDOUT_FILENO ? stdout : stderr);
    const int saved = dup(target);
    if (saved < 0) return -1;
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0) { close(saved); return -1; }
    const int dup_result = dup2(file, target);
    close(file);
    if (dup_result < 0) { close(saved); return -1; }
    return saved;
}

static void restore_fd(int target, int saved) {
    fflush(target == STDOUT_FILENO ? stdout : stderr);
    dup2(saved, target);
    close(saved);
}

/* One case: run <cases_dir>/<name>.di, capturing its output into
 * <output_dir>/<name>.stdout, .combined, and .exitcode.
 *
 * .combined is stdout bytes immediately followed by stderr bytes, not a
 * true fd-level merge -- but diamond_run_source (src/run_source.c) is
 * confirmed, by reading its own code, to always finish every stdout
 * write before writing anything to stderr, on both its success path
 * (all stdout happens before any DIAMOND_TRACE_*-gated stderr line) and
 * its failure paths (whatever stdout a program managed before crashing,
 * then exactly one stderr error message after). So "all of stdout, then
 * all of stderr" is provably identical to what a real `2>&1`-merged
 * capture would have produced for every case this tool runs, not just
 * an approximation.
 *
 * Returns false only on an I/O/setup failure in this *tool* -- a
 * failing/mismatching Diamond *test* is not this function's problem;
 * tests/run.sh still does every actual comparison, exactly as before,
 * just against these files instead of a fresh process's own output. */
static bool run_one_case(const char *cases_dir, const char *output_dir, const char *name) {
    char di_path[PATH_BUFFER_SIZE], env_path[PATH_BUFFER_SIZE], flags_path[PATH_BUFFER_SIZE];
    if (!format_path(di_path, sizeof di_path, "%s/%s.di", cases_dir, name) ||
        !format_path(env_path, sizeof env_path, "%s/%s.env", cases_dir, name) ||
        !format_path(flags_path, sizeof flags_path, "%s/%s.flags", cases_dir, name)) {
        fprintf(stderr, "run_cases: path too long for case %s\n", name);
        return false;
    }

    for (size_t index = 0; index < DIAMOND_ENV_VAR_COUNT; index++)
        unsetenv(DIAMOND_ENV_VARS[index]);
    if (!apply_env_file(env_path)) return false;
    bool dump_bytecode = false;
    if (!read_flags_file(flags_path, &dump_bytecode)) return false;

    size_t source_length = 0;
    char *source = read_whole_file(di_path, &source_length);
    if (source == nullptr) {
        fprintf(stderr, "run_cases: cannot read %s\n", di_path);
        return false;
    }

    /* The prelude, compiled exactly once for the whole run and reused
     * as a diamond_compile_incremental template by every case below --
     * see compiler.c's own doc comment on that function. Every case
     * gets the same JSON-inclusive prelude regardless of whether its
     * own source needs JSON: cheap (a handful of otherwise-unused
     * functions/one otherwise-unused class), and building two
     * templates (with/without JSON) to save that would need per-case
     * template selection for no measured benefit. This is the actual
     * point of this whole file's own top-of-file rationale (run every
     * case in one process instead of spawning a fresh `diamond`): the
     * corpus was already paying to re-lex/re-parse the ~24-36KB prelude
     * on all ~1285 cases before this, not just process-spawn overhead. */
    static DiamondProgram *prelude_template = nullptr;
    if (prelude_template == nullptr) {
        prelude_template = calloc(1,sizeof *prelude_template);
        if (prelude_template == nullptr) {
            fprintf(stderr, "run_cases: out of memory allocating prelude template\n");
            free(source);
            return false;
        }
        const size_t prelude_length = diamond_prelude_length(true);
        char *prelude_source = malloc(prelude_length + 1);
        if (prelude_source == nullptr) {
            fprintf(stderr, "run_cases: out of memory building prelude template source\n");
            free(source);
            return false;
        }
        diamond_prelude_write(prelude_source, true);
        prelude_source[prelude_length] = '\0';
        DiamondDiagnostic template_diagnostic;
        const bool template_compiled =
            diamond_compile(prelude_source, prelude_template, &template_diagnostic);
        free(prelude_source);
        if (!template_compiled) {
            fprintf(stderr, "run_cases: prelude template failed to compile: %s\n",
                template_diagnostic.message);
            free(source);
            return false;
        }
    }

    /* One zero-initialized DiamondProgram reused for every case. Compilation
     * releases and rebuilds its dynamically sized function storage, exercising
     * the same ownership path repeatedly without reallocating the container. */
    static DiamondProgram *program = nullptr;
    if (program == nullptr) {
        program = calloc(1,sizeof *program);
        if (program == nullptr) {
            fprintf(stderr, "run_cases: out of memory allocating program\n");
            free(source);
            return false;
        }
    }

    char stdout_path[PATH_BUFFER_SIZE], stderr_path[PATH_BUFFER_SIZE];
    if (!format_path(stdout_path, sizeof stdout_path, "%s/%s.stdout", output_dir, name) ||
        !format_path(stderr_path, sizeof stderr_path, "%s/%s.stderr", output_dir, name)) {
        fprintf(stderr, "run_cases: path too long for case %s\n", name);
        free(source);
        return false;
    }

    const int saved_stdout = redirect_fd(STDOUT_FILENO, stdout_path);
    const int saved_stderr = redirect_fd(STDERR_FILENO, stderr_path);
    if (saved_stdout < 0 || saved_stderr < 0) {
        if (saved_stdout >= 0) restore_fd(STDOUT_FILENO, saved_stdout);
        if (saved_stderr >= 0) restore_fd(STDERR_FILENO, saved_stderr);
        fprintf(stderr, "run_cases: cannot redirect output for %s\n", name);
        free(source);
        return false;
    }

    const int exit_code = diamond_run_source_with_template(
        di_path, source, dump_bytecode, program, prelude_template, 0, nullptr);
    free(source);

    restore_fd(STDOUT_FILENO, saved_stdout);
    restore_fd(STDERR_FILENO, saved_stderr);

    size_t stdout_length = 0, stderr_length = 0;
    char *stdout_content = read_whole_file(stdout_path, &stdout_length);
    char *stderr_content = read_whole_file(stderr_path, &stderr_length);
    bool ok = stdout_content != nullptr && stderr_content != nullptr;
    if (ok) {
        char *combined = malloc(stdout_length + stderr_length);
        ok = combined != nullptr;
        if (ok) {
            memcpy(combined, stdout_content, stdout_length);
            memcpy(combined + stdout_length, stderr_content, stderr_length);
            char combined_path[PATH_BUFFER_SIZE];
            ok = format_path(combined_path, sizeof combined_path, "%s/%s.combined", output_dir, name) &&
                write_whole_file(combined_path, combined, stdout_length + stderr_length);
        }
        free(combined);
    }
    free(stdout_content);
    free(stderr_content);
    if (!ok) {
        fprintf(stderr, "run_cases: cannot capture output for %s\n", name);
        return false;
    }

    char exitcode_path[PATH_BUFFER_SIZE], exitcode_text[16];
    const int written = snprintf(exitcode_text, sizeof exitcode_text, "%d", exit_code);
    if (written < 0 || (size_t)written >= sizeof exitcode_text ||
        !format_path(exitcode_path, sizeof exitcode_path, "%s/%s.exitcode", output_dir, name) ||
        !write_whole_file(exitcode_path, exitcode_text, (size_t)written)) {
        fprintf(stderr, "run_cases: cannot write exit code for %s\n", name);
        return false;
    }
    return true;
}

/* True when `name`'s sibling .expected/.expected_error/
 * .expected_contains/.expected_lastline files -- the only four kinds
 * tests/run.sh's file-based-case loop knows how to compare against --
 * exist. A `.di` file with none of the four (the older inline -e/env-
 * var assertions elsewhere in tests/run.sh are driven a different way
 * entirely) is silently skipped, matching that loop's own existing
 * behavior exactly. */
static bool should_run_case(const char *cases_dir, const char *name) {
    static const char *const suffixes[] = {
        ".expected", ".expected_error", ".expected_contains", ".expected_lastline",
    };
    for (size_t index = 0; index < sizeof(suffixes) / sizeof(suffixes[0]); index++) {
        char path[PATH_BUFFER_SIZE];
        if (format_path(path, sizeof path, "%s/%s%s", cases_dir, name, suffixes[index]) &&
            file_exists(path)) return true;
    }
    char source_path[PATH_BUFFER_SIZE];
    if (!format_path(source_path, sizeof source_path, "%s/%s.di", cases_dir, name)) return false;
    char *source=read_whole_file(source_path,nullptr);
    if(source==nullptr)return false;
    const bool uses_exit_status=strstr(source,"suite.run!()")!=nullptr;
    free(source);
    if(uses_exit_status)return true;
    return false;
}

int main(int argc, char **argv) {
    /* DIAMOND_MAX_CALL_DEPTH (src/vm.c) is tuned against an ordinary
     * 8MB main-thread stack -- deep Diamond-level recursion (see
     * legacy_0092.di's own SystemStackError regression case) is expected
     * to hit that guard well before run_chunk's own C recursion could
     * exhaust it. This tool runs every case *in this one process* rather
     * than spawning a fresh `diamond` per case (see this file's own
     * top-of-file comment) specifically for speed -- but that means
     * run_one_case's own locals (four PATH_BUFFER_SIZE-sized path
     * buffers per case, on top of everything else) sit underneath
     * run_chunk's entire recursive call chain for every case, a
     * fixed cost `diamond`'s own plain CLI entry point (src/main.c)
     * never pays. Confirmed empirically that this margin is real: an
     * unoptimized+ASan build of this tool can overflow the actual C
     * stack while a standalone `diamond -e` running the exact same
     * recursive program still hits DIAMOND_MAX_CALL_DEPTH's clean guard
     * first, for the sole reason that this process's baseline stack
     * usage before recursion even starts is larger. Rather than shrink
     * DIAMOND_MAX_CALL_DEPTH itself (a real behavior change for every
     * build, including release, to compensate for a debug-build-only
     * test-harness margin) or force every future opcode addition to
     * fight for bytes in an already-thin margin, raise this process's
     * own stack ceiling once, up front -- Linux grows the main thread's
     * stack lazily on page fault against the *current* rlimit, so this
     * takes effect immediately with no re-exec needed. Best-effort: if
     * the platform or its limits.conf refuses the raise, fall through
     * and run at the default 8MB exactly as before. */
    struct rlimit stack_limit;
    if(getrlimit(RLIMIT_STACK,&stack_limit)==0) {
        constexpr rlim_t desired=64u*1024*1024;
        if(stack_limit.rlim_cur<desired&&
           (stack_limit.rlim_max==RLIM_INFINITY||stack_limit.rlim_max>=desired)) {
            stack_limit.rlim_cur=desired;
            (void)setrlimit(RLIMIT_STACK,&stack_limit);
        }
    }

    const char *cases_dir = argc > 1 ? argv[1] : "tests/cases";
    const char *output_dir = argc > 2 ? argv[2] : "build/case_output";

    if (mkdir(output_dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "run_cases: cannot create %s: %s\n", output_dir, strerror(errno));
        return 1;
    }

    char pattern[PATH_BUFFER_SIZE];
    snprintf(pattern, sizeof pattern, "%s/*.di", cases_dir);
    glob_t matches = {};
    if (glob(pattern, 0, nullptr, &matches) != 0) {
        fprintf(stderr, "run_cases: no *.di files found under %s\n", cases_dir);
        return 1;
    }

    size_t run_count = 0, skipped_count = 0;
    bool ok = true;
    for (size_t index = 0; index < matches.gl_pathc && ok; index++) {
        const char *di_path = matches.gl_pathv[index];
        const char *base = strrchr(di_path, '/');
        base = base != nullptr ? base + 1 : di_path;
        const size_t base_length = strlen(base) - 3; /* strip trailing ".di" */
        char name[PATH_BUFFER_SIZE];
        if (base_length >= sizeof name) {
            fprintf(stderr, "run_cases: case name too long: %s\n", base);
            ok = false;
            break;
        }
        memcpy(name, base, base_length);
        name[base_length] = '\0';

        if (!should_run_case(cases_dir, name)) { skipped_count++; continue; }
        ok = run_one_case(cases_dir, output_dir, name);
        if (ok) run_count++;
    }
    globfree(&matches);
    if (!ok) return 1;
    fprintf(stderr, "run_cases: ran %zu case(s), skipped %zu (no expectation or run! marker)\n",
            run_count, skipped_count);
    return 0;
}
