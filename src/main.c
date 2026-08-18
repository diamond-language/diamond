#define _DEFAULT_SOURCE

#include "repl.h"
#include "run_source.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr char DIAMOND_VERSION[] = "0.1.0-dev";

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        fprintf(stderr, "diamond: cannot open '%s': %s\n", path, strerror(errno));
        return nullptr;
    }
    /* open(2)/fopen(3) succeed on a directory on Linux -- it's read(2)
     * that's meant to fail with EISDIR, but that's a filesystem-driver
     * behavior, not a POSIX guarantee: some drivers (seen in practice on
     * overlayfs, common for container build directories) return a
     * plain short/empty read instead of setting errno at all, which
     * previously surfaced as a misleading "unexpected end of file"
     * instead of "Is a directory". Checking the file type explicitly
     * up front makes this deterministic across filesystems, rather than
     * depending on how a given driver's read(2) happens to behave. */
    struct stat file_status;
    if (fstat(fileno(file), &file_status) == 0 && S_ISDIR(file_status.st_mode)) {
        fprintf(stderr, "diamond: cannot read '%s': %s\n", path, strerror(EISDIR));
        fclose(file);
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "diamond: cannot seek '%s': %s\n", path,
                strerror(errno));
        fclose(file);
        return nullptr;
    }
    const long length = ftell(file);
    if (length < 0) {
        fprintf(stderr, "diamond: cannot determine size of '%s': %s\n", path,
                strerror(errno));
        fclose(file);
        return nullptr;
    }
    if ((unsigned long long)length >= SIZE_MAX) {
        fprintf(stderr, "diamond: file '%s' is too large\n", path);
        fclose(file);
        return nullptr;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "diamond: cannot read '%s': %s\n", path,
                strerror(errno));
        fclose(file);
        return nullptr;
    }

    char *source = malloc((size_t)length + 1);
    if (source == nullptr) {
        fprintf(stderr, "diamond: out of memory reading '%s'\n", path);
        fclose(file);
        return nullptr;
    }
    const size_t read_count = fread(source, 1, (size_t)length, file);
    if (read_count != (size_t)length) {
        fprintf(stderr, "diamond: cannot read '%s': %s\n", path,
                ferror(file) ? strerror(errno) : "unexpected end of file");
        free(source);
        fclose(file);
        return nullptr;
    }
    source[read_count] = '\0';
    fclose(file);
    return source;
}

static void print_usage(void) {
    fputs("usage: diamond [-e CODE | FILE | --version] [ARGS...]\n"
          "       diamond --dump-bytecode [-e CODE | FILE] [ARGS...]\n"
          "       diamond                          (starts a REPL if stdin is a terminal)\n"
          "ARGS, if given, are the script's own arguments -- see ARGV in docs/syntax.md.\n",
          stderr);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("diamond %s\n", DIAMOND_VERSION);
        return 0;
    }
    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        return diamond_run_source("-e", argv[2], false, argc - 3, argv + 3);
    }
    if (argc >= 4 && strcmp(argv[1], "--dump-bytecode") == 0 &&
        strcmp(argv[2], "-e") == 0) {
        return diamond_run_source("-e", argv[3], true, argc - 4, argv + 4);
    }
    const bool dump_file = argc >= 2 &&
        strcmp(argv[1], "--dump-bytecode") == 0;
    if ((argc >= 2 && !dump_file) || (dump_file && argc >= 3)) {
        const char *path = dump_file ? argv[2] : argv[1];
        char *source = read_file(path);
        if (source == nullptr) {
            return 74;
        }
        const int arg_start = dump_file ? 3 : 2;
        const int status = diamond_run_source(path, source, dump_file,
            argc - arg_start, argv + arg_start);
        free(source);
        return status;
    }
    /* DIAMOND_FORCE_REPL exists purely for tests/repl_test.sh: a bash
     * coproc drives the REPL over pipes, not a real pty, so isatty()
     * alone would never launch it there. */
    if (argc == 1 && (isatty(STDIN_FILENO) || getenv("DIAMOND_FORCE_REPL") != nullptr)) {
        return diamond_repl_run();
    }

    print_usage();
    return 64;
}
