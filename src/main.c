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

static constexpr char DIAMOND_VERSION[] = "0.8.0";

static char *absolute_path(const char *path);

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
    char *absolute_tmp_dir = absolute_path(tmp_dir);
    if (absolute_tmp_dir == nullptr) {
        fprintf(stderr, "diamond: cannot resolve temporary directory\n");
        return false;
    }
    const int written = snprintf(path_out, path_out_capacity,
        "%s/diamond-aot-XXXXXX", absolute_tmp_dir);
    free(absolute_tmp_dir);
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

static char *make_assignment(const char *name,const char *value) {
    const size_t name_length=strlen(name),value_length=strlen(value);
    if(name_length>SIZE_MAX-value_length-2)return nullptr;
    char *assignment=malloc(name_length+value_length+2);
    if(assignment==nullptr)return nullptr;
    memcpy(assignment,name,name_length);
    assignment[name_length]='=';
    memcpy(assignment+name_length+1,value,value_length+1);
    return assignment;
}

static char *absolute_path(const char *path) {
    if (path[0] == '/') return strdup(path);
    char *cwd = getcwd(nullptr, 0);
    if (cwd == nullptr) return nullptr;
    const size_t length = strlen(cwd) + 1 + strlen(path) + 1;
    char *result = malloc(length);
    if (result != nullptr) snprintf(result, length, "%s/%s", cwd, path);
    free(cwd);
    return result;
}

/* Resolve the binary itself, including when invoked as `diamond` via PATH
 * or through a symlink. /proc/self/exe also handles a custom argv[0] on
 * Linux; the argv/PATH path works on other POSIX systems. */
static char *executable_path(const char *argv0) {
#if defined(__linux__)
    char proc_path[4096];
    const ssize_t count = readlink("/proc/self/exe", proc_path,
        sizeof proc_path - 1);
    if (count > 0 && (size_t)count < sizeof proc_path - 1) {
        proc_path[count] = '\0';
        return realpath(proc_path, nullptr);
    }
#endif
    if (strchr(argv0, '/') != nullptr) return realpath(argv0, nullptr);
    const char *path = getenv("PATH");
    if (path == nullptr) return nullptr;
    char *copy = strdup(path);
    if (copy == nullptr) return nullptr;
    char *entry = copy;
    char *resolved = nullptr;
    while (entry != nullptr) {
        char *separator = strchr(entry, ':');
        if (separator != nullptr) *separator = '\0';
        const char *directory = entry[0] == '\0' ? "." : entry;
        const size_t length = strlen(directory) + 1 + strlen(argv0) + 1;
        char *candidate = malloc(length);
        if (candidate == nullptr) break;
        snprintf(candidate, length, "%s/%s", directory, argv0);
        if (access(candidate, X_OK) == 0) resolved = realpath(candidate, nullptr);
        free(candidate);
        if (resolved != nullptr || separator == nullptr) break;
        entry = separator + 1;
    }
    free(copy);
    return resolved;
}

static bool has_build_files(const char *directory) {
    static const char *const files[] = {
        "Makefile", "src/main.c", "tools/aot_runtime_main.c"};
    for (size_t index = 0; index < sizeof files / sizeof files[0]; index++) {
        const size_t length = strlen(directory) + 1 + strlen(files[index]) + 1;
        char *path = malloc(length);
        if (path == nullptr) return false;
        snprintf(path, length, "%s/%s", directory, files[index]);
        const bool exists = access(path, F_OK) == 0;
        free(path);
        if (!exists) return false;
    }
    return true;
}

static char *diamond_repo_root(const char *argv0) {
    char *path = executable_path(argv0);
    if (path == nullptr) return nullptr;
    char *slash = strrchr(path, '/');
    if (slash == nullptr) { free(path); return nullptr; }
    if (slash == path) slash[1] = '\0';
    else *slash = '\0';
    while (!has_build_files(path)) {
        slash = strrchr(path, '/');
        if (slash == nullptr || slash == path) {
            free(path);
            return nullptr;
        }
        *slash = '\0';
    }
    return path;
}

/* Runs `make -C REPO aot-build AOT_EMBED=... AOT_OUTPUT=... [CC=...]` (the
 * Makefile target added alongside diamond-lsp/diamond-dap's own) via
 * fork/execvp -- not system(3), so make receives these assignments as
 * distinct arguments. The Makefile
 * lives alongside the executable's source checkout; the app and output
 * paths still belong to the caller's working directory. */
static int run_make_aot_build(const char *embed_path, const char *output_path,
        const char *cc, const char *argv0) {
    char *repo_root = diamond_repo_root(argv0);
    char *absolute_embed = absolute_path(embed_path);
    char *absolute_output = absolute_path(output_path);
    if (repo_root == nullptr || absolute_embed == nullptr || absolute_output == nullptr) {
        fprintf(stderr, "diamond: cannot locate Diamond build files or resolve build paths\n");
        free(repo_root);free(absolute_embed);free(absolute_output);
        return 74;
    }
    char *embed_arg=make_assignment("AOT_EMBED",absolute_embed);
    char *output_arg=make_assignment("AOT_OUTPUT",absolute_output);
    char *cc_arg=cc!=nullptr?make_assignment("CC",cc):nullptr;
    if(embed_arg==nullptr||output_arg==nullptr||(cc!=nullptr&&cc_arg==nullptr)) {
        fprintf(stderr,"diamond: out of memory\n");
        free(embed_arg);free(output_arg);free(cc_arg);
        free(repo_root);free(absolute_embed);free(absolute_output);return 74;
    }
    char *args[10];
    size_t arg_count = 0;
    args[arg_count++] = (char *)"make";
    args[arg_count++] = (char *)"-C";
    args[arg_count++] = repo_root;
    args[arg_count++] = (char *)"aot-build";
    args[arg_count++] = embed_arg;
    args[arg_count++] = output_arg;
    if (cc != nullptr) {
        args[arg_count++] = cc_arg;
    }
    args[arg_count] = nullptr;

    const pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "diamond: fork failed: %s\n", strerror(errno));
        free(embed_arg);free(output_arg);free(cc_arg);
        free(repo_root);free(absolute_embed);free(absolute_output);
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
        free(embed_arg);free(output_arg);free(cc_arg);
        free(repo_root);free(absolute_embed);free(absolute_output);
        return 74;
    }
    free(embed_arg);free(output_arg);free(cc_arg);
    free(repo_root);free(absolute_embed);free(absolute_output);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "diamond: build failed\n");
        return 74;
    }
    return 0;
}

/* Installed layout: <prefix>/bin/diamond with its prebuilt AOT kit (`make
 * aot-kit`/`make install`) at <prefix>/lib/diamond/aot. $DIAMOND_AOT_KIT
 * overrides the location. Returns nullptr when there is no kit, so a source
 * checkout keeps using its Makefile. */
static char *find_aot_kit(const char *argv0) {
    const char *override = getenv("DIAMOND_AOT_KIT");
    if (override != nullptr && override[0] != '\0') return absolute_path(override);
    char *exe = executable_path(argv0);
    if (exe == nullptr) return nullptr;
    char *slash = strrchr(exe, '/');
    if (slash != nullptr) *slash = '\0';
    const size_t length = strlen(exe) + sizeof "/../lib/diamond/aot/libdiamond-aot.a";
    char *kit = malloc(length);
    if (kit == nullptr) { free(exe); return nullptr; }
    snprintf(kit, length, "%s/../lib/diamond/aot/libdiamond-aot.a", exe);
    free(exe);
    char *resolved = realpath(kit, nullptr);
    free(kit);
    if (resolved == nullptr) return nullptr;
    *strrchr(resolved, '/') = '\0';
    return resolved;
}

static char *kit_path(const char *kit, const char *name) {
    const size_t length = strlen(kit) + 1 + strlen(name) + 1;
    char *path = malloc(length);
    if (path != nullptr) snprintf(path, length, "%s/%s", kit, name);
    return path;
}

/* Kit metadata files hold one argument per line; blank lines are ignored.
 * Appends each line to `args` (capacity `capacity`), returning false if the
 * file is unreadable or there is no room. The strings live in `*storage`. */
static bool append_kit_lines(const char *kit, const char *name, char **args,
        size_t *count, size_t capacity, char **storage) {
    char *path = kit_path(kit, name);
    char *text = path != nullptr ? read_file(path) : nullptr;
    free(path);
    if (text == nullptr) return false;
    *storage = text;
    for (char *line = strtok(text, "\n"); line != nullptr; line = strtok(nullptr, "\n")) {
        if (line[0] == '\0') continue;
        if (*count + 1 >= capacity) return false;
        args[(*count)++] = line;
    }
    return true;
}

static int wait_for_child(pid_t child, const char *tool) {
    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        fprintf(stderr, "diamond: waitpid failed: %s\n", strerror(errno));
        return 74;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "diamond: %s failed\n", tool);
        return 74;
    }
    return 0;
}

/* Links the generated embed-data file against a prebuilt kit with a single
 * compiler invocation: no make, runtime sources, or headers are needed. */
static int run_kit_aot_build(const char *kit, const char *embed_path,
        const char *output_path, const char *cc_override) {
    char *version_path = kit_path(kit, "version");
    char *version = version_path != nullptr ? read_file(version_path) : nullptr;
    free(version_path);
    if (version == nullptr) {
        fprintf(stderr, "diamond: AOT kit '%s' is incomplete\n", kit);
        return 74;
    }
    version[strcspn(version, "\n")] = '\0';
    if (strcmp(version, DIAMOND_VERSION) != 0) {
        fprintf(stderr, "diamond: AOT kit '%s' is for Diamond %s, not %s\n",
                kit, version, DIAMOND_VERSION);
        free(version);
        return 74;
    }
    free(version);

    enum { MAX_ARGS = 256 };
    char *args[MAX_ARGS];
    size_t count = 0;
    char *cc_text = nullptr, *compile_text = nullptr, *link_text = nullptr;
    char *runtime = kit_path(kit, "libdiamond-aot.a");
    char *reginold = kit_path(kit, "libreginold.a");
    int result = 74;
    if (cc_override != nullptr) {
        args[count++] = (char *)cc_override;
    } else if (!append_kit_lines(kit, "cc", args, &count, 2, &cc_text) || count != 1) {
        fprintf(stderr, "diamond: AOT kit '%s' does not name a compiler\n", kit);
        goto done;
    }
    if (runtime == nullptr || reginold == nullptr ||
        !append_kit_lines(kit, "compile.args", args, &count, MAX_ARGS, &compile_text) ||
        count + 3 >= MAX_ARGS) {
        fprintf(stderr, "diamond: AOT kit '%s' is incomplete\n", kit);
        goto done;
    }
    args[count++] = (char *)embed_path;
    args[count++] = runtime;
    args[count++] = reginold;
    if (!append_kit_lines(kit, "link.args", args, &count, MAX_ARGS - 2, &link_text)) {
        fprintf(stderr, "diamond: AOT kit '%s' is incomplete\n", kit);
        goto done;
    }
    args[count++] = (char *)"-o";
    args[count++] = (char *)output_path;
    args[count] = nullptr;

    const pid_t child = fork();
    if (child < 0) {
        fprintf(stderr, "diamond: fork failed: %s\n", strerror(errno));
        goto done;
    }
    if (child == 0) {
        execvp(args[0], args);
        fprintf(stderr, "diamond: cannot exec '%s': %s\n", args[0], strerror(errno));
        _exit(127);
    }
    result = wait_for_child(child, "link");
done:
    free(cc_text);
    free(compile_text);
    free(link_text);
    free(runtime);
    free(reginold);
    return result;
}

/* `diamond build SOURCE [-o OUTPUT] [--cc=COMPILER]` -- compiles SOURCE
 * exactly the way ordinary execution would (diamond_compile_source,
 * src/run_source.h), serializes the result, generates a temp embed-data
 * file for it, and links that plus tools/aot_runtime_main.c into a
 * standalone executable via the Makefile's own aot-build target. See
 * docs/deployment.md for the full contract and its current limitations
 * (still dynamically links whatever `diamond` itself does). */
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

    char *kit = find_aot_kit(argv[0]);
    const int status = kit != nullptr
        ? run_kit_aot_build(kit, embed_path, output_path, cc)
        : run_make_aot_build(embed_path, output_path, cc, argv[0]);
    free(kit);
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
          "  --sandbox           deny filesystem/network/subprocess access --\n"
          "                      see docs/sandbox.md\n"
          "  -h, --help          display this help and exit\n"
          "  -v, --version       display version information and exit\n"
          "\n"
          "'diamond build' compiles SOURCE into a standalone executable (default\n"
          "output: SOURCE's own basename with its extension stripped) -- see\n"
          "docs/deployment.md. The Diamond build checkout must be available.\n"
          "\n"
          "ARGS after FILE or CODE are available to the program through ARGV.\n",
          stream);
}

int main(int argc, char **argv) {
    /* A thin convenience over DIAMOND_SANDBOX=1 (see docs/sandbox.md) --
     * shifts argv[2..] down into argv[1..] (argv[0], the program name,
     * stays put) so every branch below sees exactly the shape it already
     * expects, with `--sandbox` itself gone. Deliberately only recognized
     * as the very first argument -- simplest to reason about, and every
     * existing form (`-e`, `--dump-bytecode`, `build`, a plain file) can
     * still follow it unmodified. */
    if (argc >= 2 && strcmp(argv[1], "--sandbox") == 0) {
        setenv("DIAMOND_SANDBOX", "1", 1);
        for (int index = 1; index < argc - 1; index++) argv[index] = argv[index + 1];
        argc--;
    }
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
