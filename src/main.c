#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiled_prelude.h"
#include "compiler.h"
#include "repl.h"
#include "run_source.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr char DIAMOND_VERSION[] = "0.4.0";

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

/* `diamond build app.di` with no `-o` defaults the output to `app.di`'s
 * own basename, extension stripped ("app.di" -> "app"; no extension at
 * all if the source has none). Caller owns the returned buffer. */
static char *derive_output_path(const char *source_path) {
    const char *slash = strrchr(source_path, '/');
    const char *base = slash != nullptr ? slash + 1 : source_path;
    const char *dot = strrchr(base, '.');
    const size_t length = dot != nullptr && dot != base ?
        (size_t)(dot - base) : strlen(base);
    char *output = malloc(length + 1);
    if (output == nullptr) return nullptr;
    memcpy(output, base, length);
    output[length] = '\0';
    return output;
}

/* Serializes `program` (diamond_program_write_compiled, src/compiled_
 * prelude.h -- the same function tools/gen_compiled_prelude.c already
 * uses for the prelude itself) to a fresh temp file, writing its path
 * into `path_out`. The caller unlinks it once the generated embed-data
 * file (below) has been compiled past #embed-ing it. */
static bool write_compiled_program_to_temp(const DiamondProgram *program,
        char *path_out, size_t path_out_capacity) {
    const char *tmp_dir = getenv("TMPDIR");
    if (tmp_dir == nullptr || tmp_dir[0] == '\0') tmp_dir = "/tmp";
    const int written = snprintf(path_out, path_out_capacity,
        "%s/diamond-aot-XXXXXX", tmp_dir);
    if (written < 0 || (size_t)written >= path_out_capacity) {
        fprintf(stderr, "diamond: temporary directory path is too long\n");
        return false;
    }
    const int fd = mkstemp(path_out);
    if (fd < 0) {
        fprintf(stderr, "diamond: cannot create temporary file: %s\n", strerror(errno));
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (file == nullptr) {
        fprintf(stderr, "diamond: cannot open temporary file: %s\n", strerror(errno));
        close(fd);
        unlink(path_out);
        return false;
    }
    const bool ok = diamond_program_write_compiled(program, file);
    fclose(file);
    if (!ok) {
        fprintf(stderr, "diamond: failed to serialize compiled program\n");
        unlink(path_out);
    }
    return ok;
}

/* Generates a `.c` file at `embed_path` that #embeds `bin_path` and
 * exposes it via diamond_aot_program_data()/_size() -- the exact
 * accessor-function shape src/compiled_prelude_data.c already uses for
 * the prelude's own embedded bytecode, just generated per `diamond
 * build` invocation instead of once at Diamond's own build time.
 * tools/aot_runtime_main.c declares and calls these two functions. */
static bool write_embed_data_file(const char *bin_path, const char *embed_path) {
    FILE *file = fopen(embed_path, "w");
    if (file == nullptr) {
        fprintf(stderr, "diamond: cannot create '%s': %s\n", embed_path, strerror(errno));
        return false;
    }
    const int written = fprintf(file,
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "static const uint8_t DIAMOND_AOT_PROGRAM_BYTES[] = {\n"
        "#embed \"%s\"\n"
        "};\n"
        "const uint8_t *diamond_aot_program_data(void) "
            "{ return DIAMOND_AOT_PROGRAM_BYTES; }\n"
        "size_t diamond_aot_program_size(void) "
            "{ return sizeof(DIAMOND_AOT_PROGRAM_BYTES); }\n",
        bin_path);
    const bool ok = written >= 0 && fclose(file) == 0;
    if (!ok) fprintf(stderr, "diamond: error writing '%s'\n", embed_path);
    return ok;
}

/* Runs `make aot-build AOT_EMBED=... AOT_OUTPUT=... [CC=...]` (the
 * Makefile target added alongside diamond-lsp/diamond-dap's own) via
 * fork/execvp -- not system(3), so none of these paths ever pass
 * through a shell, sidestepping shell-quoting entirely. Requires a
 * `./Makefile` in the current directory, the same "run from the repo
 * root" assumption `make dap`/`make lsp` already make -- there is no
 * separate "install Diamond" story yet for this to build against
 * instead (see docs/roadmap.md's own note on this). */
static int run_make_aot_build(const char *embed_path, const char *output_path,
        const char *cc) {
    if (access("Makefile", F_OK) != 0) {
        fprintf(stderr,
            "diamond: 'diamond build' must be run from the Diamond repository root\n");
        return 74;
    }
    char embed_arg[4096], output_arg[4096], cc_arg[512];
    (void)snprintf(embed_arg, sizeof embed_arg, "AOT_EMBED=%s", embed_path);
    (void)snprintf(output_arg, sizeof output_arg, "AOT_OUTPUT=%s", output_path);
    char *args[8];
    size_t arg_count = 0;
    args[arg_count++] = (char *)"make";
    args[arg_count++] = (char *)"aot-build";
    args[arg_count++] = embed_arg;
    args[arg_count++] = output_arg;
    if (cc != nullptr) {
        (void)snprintf(cc_arg, sizeof cc_arg, "CC=%s", cc);
        args[arg_count++] = cc_arg;
    }
    args[arg_count] = nullptr;

    const pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "diamond: fork failed: %s\n", strerror(errno));
        return 74;
    }
    if (child == 0) {
        execvp("make", args);
        fprintf(stderr, "diamond: cannot exec 'make': %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        fprintf(stderr, "diamond: waitpid failed: %s\n", strerror(errno));
        return 74;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "diamond: build failed\n");
        return 74;
    }
    return 0;
}

/* `diamond build SOURCE [-o OUTPUT] [--cc=COMPILER]` -- compiles SOURCE
 * exactly the way ordinary execution would (diamond_compile_source,
 * src/run_source.h), serializes the result, generates a temp embed-data
 * file for it, and links that plus tools/aot_runtime_main.c into a
 * standalone executable via the Makefile's own aot-build target. See
 * docs/deployment.md for the full contract and its current limitations
 * (still dynamically links whatever `diamond` itself does; must run from
 * the repo root). */
static int handle_build_command(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: diamond build SOURCE [-o OUTPUT] [--cc=COMPILER]\n");
        return 64;
    }
    const char *source_path = argv[2];
    const char *output_path = nullptr;
    const char *cc = nullptr;
    for (int index = 3; index < argc; index++) {
        if (strcmp(argv[index], "-o") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else if (strncmp(argv[index], "--cc=", 5) == 0) {
            cc = argv[index] + 5;
        } else {
            fprintf(stderr, "diamond: unrecognized build option '%s'\n", argv[index]);
            return 64;
        }
    }
    char *derived_output = nullptr;
    if (output_path == nullptr) {
        derived_output = derive_output_path(source_path);
        if (derived_output == nullptr) {
            fprintf(stderr, "diamond: out of memory\n");
            return 74;
        }
        output_path = derived_output;
    }

    char *source = read_file(source_path);
    if (source == nullptr) {
        free(derived_output);
        return 74;
    }
    DiamondProgram *program = calloc(1, sizeof *program);
    if (program == nullptr) {
        fprintf(stderr, "diamond: out of memory\n");
        free(source);
        free(derived_output);
        return 74;
    }
    /* No diamond_program_init call here -- diamond_compile_source (like
     * diamond_compile/diamond_compile_incremental themselves) always
     * re-initializes `program` from scratch before compiling, and it's
     * already freshly calloc'd (all-zero), the exact same pattern
     * diamond_run_source's own top-level caller relies on. */
    const bool compiled = diamond_compile_source(source_path, source, program);
    free(source);
    if (!compiled) {
        diamond_program_free(program);
        free(program);
        free(derived_output);
        return 65;
    }

    char bin_path[4096];
    const bool wrote_bin =
        write_compiled_program_to_temp(program, bin_path, sizeof bin_path);
    diamond_program_free(program);
    free(program);
    if (!wrote_bin) {
        free(derived_output);
        return 74;
    }

    char embed_path[4104];
    (void)snprintf(embed_path, sizeof embed_path, "%s.c", bin_path);
    if (!write_embed_data_file(bin_path, embed_path)) {
        unlink(bin_path);
        free(derived_output);
        return 74;
    }

    const int status = run_make_aot_build(embed_path, output_path, cc);
    unlink(bin_path);
    unlink(embed_path);
    if (status == 0) printf("diamond: built '%s'\n", output_path);
    free(derived_output);
    return status;
}

static void print_usage(FILE *stream) {
    fputs("Usage: diamond [OPTIONS] [FILE [ARGS...]]\n"
          "       diamond -e CODE [ARGS...]\n"
          "       diamond --dump-bytecode [-e CODE | FILE] [ARGS...]\n"
          "       diamond build SOURCE [-o OUTPUT] [--cc=COMPILER]\n"
          "\n"
          "Run a Diamond program, evaluate source, or start the REPL when no\n"
          "arguments are given and standard input is a terminal.\n"
          "\n"
          "Options:\n"
          "  -e CODE             evaluate CODE\n"
          "  --dump-bytecode     print bytecode before running\n"
          "  -h, --help          display this help and exit\n"
          "  -v, --version       display version information and exit\n"
          "\n"
          "'diamond build' compiles SOURCE into a standalone executable (default\n"
          "output: SOURCE's own basename with its extension stripped) -- see\n"
          "docs/deployment.md. Must be run from the Diamond repository root.\n"
          "\n"
          "ARGS after FILE or CODE are available to the program through ARGV.\n",
          stream);
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "-v") == 0 ||
                      strcmp(argv[1], "--version") == 0)) {
        printf("diamond %s\n", DIAMOND_VERSION);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "build") == 0) {
        return handle_build_command(argc, argv);
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

    print_usage(stderr);
    return 64;
}
