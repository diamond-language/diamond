#ifndef DIAMOND_RUN_SOURCE_H
#define DIAMOND_RUN_SOURCE_H

#include "compiler.h"

#include <stdbool.h>

/* Compiles and runs `source` exactly the way the `diamond` CLI's own
 * file/-e execution does: lib/core.di prepended, `require` resolution,
 * VM execution, every DIAMOND_*-env-var-driven tracing/stress knob
 * (DIAMOND_STRESS_GC, DIAMOND_QUICKEN, DIAMOND_REPEAT, the DIAMOND_TRACE_*
 * family, ...), matching stdout/stderr output byte-for-byte. `name` is
 * the display name used in diagnostics/stack traces (a file path, or
 * "-e"); `dump_bytecode` matches `--dump-bytecode`. `script_argc`/
 * `script_argv` are the program's own trailing command-line arguments
 * (whatever followed the script path or -e source on the real `diamond`
 * command line, or 0/nullptr for none) -- exposed to Diamond code as the
 * `ARGV` Array of Strings (see docs/syntax.md). Returns the same process
 * exit code the CLI itself would (0 success, 65 compile error, 70
 * runtime error, 74 I/O/OOM).
 *
 * Shared by src/main.c (the CLI, a thin wrapper around this) and
 * tests/run_cases.c (the batch test runner, which calls this in a loop
 * within one process instead of tests/run.sh spawning a fresh `diamond`
 * process per case) -- both stay behaviorally identical to each other,
 * and to every already-recorded `.expected` file under tests/cases/,
 * which was originally captured by running the CLI itself. */
int diamond_run_source(const char *name, const char *source, bool dump_bytecode,
    int script_argc, char *const *script_argv);

/* Same as diamond_run_source, except the caller supplies (and owns) the
 * DiamondProgram `program` gets compiled into, instead of a fresh one
 * being malloc'd and freed internally. diamond_compile always calls
 * diamond_program_init to reset it from scratch before compiling, so
 * one zero-initialized DiamondProgram can safely be reused across many calls;
 * each call releases and rebuilds its dynamically sized function storage. */
int diamond_run_source_with_program(const char *name, const char *source,
    bool dump_bytecode, DiamondProgram *program,
    int script_argc, char *const *script_argv);

/* Same as diamond_run_source_with_program, except `program` is compiled
 * via diamond_compile_incremental against `template` (an already-
 * compiled prelude program, see compiler.c's own doc comment on that
 * function) instead of the ordinary prelude-source-concatenated
 * diamond_compile -- skips re-lexing/re-parsing the prelude's own
 * source on every call, the dominant cost `DIAMOND_TRACE_STARTUP=1`
 * measures for an otherwise-trivial program (see docs/roadmap.md).
 * Meant for a caller that runs many independent programs in one
 * process against the same template, e.g. tests/run_cases.c's batch
 * corpus runner -- not the ordinary `diamond` CLI (src/main.c), which
 * only ever runs one program per process and has no template to
 * amortize a compile against. `source` must not redeclare any name
 * `template` already declares (see diamond_compile_incremental's own
 * comment); an ordinary Diamond program never has reason to name a
 * prelude function/class, so this is not a real practical constraint. */
int diamond_run_source_with_template(const char *name, const char *source,
    bool dump_bytecode, DiamondProgram *program, const DiamondProgram *template,
    int script_argc, char *const *script_argv);

#endif
