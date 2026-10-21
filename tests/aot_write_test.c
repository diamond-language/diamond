/* Exercise the CLI's private AOT writers with deterministic stream failures. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiled_prelude.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool fail_write;
static bool fail_close;
static int close_calls;

static int test_fclose(FILE *file) {
    close_calls++;
    const int result = fclose(file);
    if (fail_close) { errno = ENOSPC; return EOF; }
    return result;
}

static int test_fprintf(FILE *file, const char *format, ...) {
    if (file != stderr && fail_write) { errno = ENOSPC; return -1; }
    va_list args;
    va_start(args, format);
    const int result = vfprintf(file, format, args);
    va_end(args);
    return result;
}

static bool test_write_compiled(const DiamondProgram *program, FILE *file) {
    (void)program;
    return !fail_write && fputs("compiled", file) >= 0;
}

static int test_repl_run(void) { return 0; }

#define main diamond_cli_main
#define fclose test_fclose
#define fprintf test_fprintf
#define diamond_program_write_compiled test_write_compiled
#define diamond_repl_run test_repl_run
/* libc may normalize this to 1; main.c supplies its own feature-test value. */
#undef _DEFAULT_SOURCE
#include "../src/main.c"
#undef main
#undef fclose
#undef fprintf
#undef diamond_program_write_compiled
#undef diamond_repl_run

int main(void) {
    /* Test all write/close outcomes, including failure during the final flush. */
    for (int write_failure = 0; write_failure < 2; write_failure++) {
        for (int close_failure = 0; close_failure < 2; close_failure++) {
            fail_write = write_failure != 0;
            fail_close = close_failure != 0;
            const bool expected = !fail_write && !fail_close;
            char path[4096];
            close_calls = 0;
            assert(write_compiled_program_to_temp(nullptr, path, sizeof path) == expected);
            assert(close_calls == 1);
            assert((access(path, F_OK) == 0) == expected);
            if (expected) {
                FILE *file = fopen(path, "r");
                assert(file != nullptr);
                char contents[16] = {0};
                assert(fread(contents, 1, sizeof contents, file) == 8);
                assert(strcmp(contents, "compiled") == 0);
                assert(fclose(file) == 0);
                assert(unlink(path) == 0);
            }

            char embed[] = "/tmp/diamond-aot-write-test-XXXXXX";
            const int fd = mkstemp(embed);
            assert(fd >= 0);
            assert(close(fd) == 0);
            close_calls = 0;
            assert(write_embed_data_file("program.bin", embed) == expected);
            assert(close_calls == 1);
            assert(unlink(embed) == 0);
        }
    }
    puts("AOT write failure tests passed");
}
