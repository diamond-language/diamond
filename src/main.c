#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiled_prelude.h"
#include "compiler.h"
#include "repl.h"
#include "run_source.h"
#include "vm.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static constexpr char DIAMOND_VERSION[] = "0.18.0";

/* Appended to --version output. The default `make` target is an unoptimized
 * debug build (so are `make sanitize`/`make tsan`), which runs Diamond code
 * roughly 10x slower than `make release`; saying so up front keeps anyone
 * benchmarking a debug binary from concluding the language is that slow. */
#ifdef DIAMOND_DEBUG
static constexpr char DIAMOND_BUILD_NOTE[] =
    " (debug build: unoptimized, much slower than `make release`)";
#else
static constexpr char DIAMOND_BUILD_NOTE[] = "";
#endif

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

/* `diamond build app.d6` with no `-o` defaults the output to `app.d6`'s
 * own basename, extension stripped ("app.d6" -> "app"; no extension at
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
    bool ok = diamond_program_write_compiled(program, file);
    if (fclose(file) != 0) ok = false;
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
    const int closed = fclose(file);
    const bool ok = written >= 0 && closed == 0;
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

/* A target name becomes a directory name, so only the characters a target
 * triple uses are accepted. */
static bool target_name_is_valid(const char *target) {
    if (target[0] == '\0') return false;
    for (const char *c = target; *c != '\0'; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '-' || *c == '_' || *c == '.'))
            return false;
    return target[0] != '.';
}

static bool kit_has_runtime(const char *directory) {
    const size_t length = strlen(directory) + sizeof "/libdiamond-aot.a";
    char *path = malloc(length);
    if (path == nullptr) return false;
    snprintf(path, length, "%s/libdiamond-aot.a", directory);
    const bool exists = access(path, R_OK) == 0;
    free(path);
    return exists;
}

/* The cross kit for `diamond build --target=TARGET` (`make aot-kit-cross`):
 * <kit>/targets/TARGET under the installed or $DIAMOND_AOT_KIT kit, or
 * build/aot-targets/TARGET in a source checkout. $DIAMOND_AOT_KIT may also
 * name a cross kit directly. Returns nullptr when none exists. */
static char *find_target_kit(const char *argv0, const char *target) {
    char *host_kit = find_aot_kit(argv0);
    char *found = nullptr;
    if (host_kit != nullptr) {
        const size_t length = strlen(host_kit) + sizeof "/targets/" + strlen(target);
        char *candidate = malloc(length);
        if (candidate != nullptr) {
            snprintf(candidate, length, "%s/targets/%s", host_kit, target);
            if (kit_has_runtime(candidate)) found = candidate; else free(candidate);
        }
        if (found == nullptr) {
            char *recorded_path = kit_path(host_kit, "target");
            char *recorded = recorded_path != nullptr ? read_file(recorded_path) : nullptr;
            free(recorded_path);
            if (recorded != nullptr) {
                recorded[strcspn(recorded, "\n")] = '\0';
                if (strcmp(recorded, target) == 0) found = strdup(host_kit);
                free(recorded);
            }
        }
        free(host_kit);
        if (found != nullptr) return found;
    }
    char *repo_root = diamond_repo_root(argv0);
    if (repo_root == nullptr) return nullptr;
    const size_t length = strlen(repo_root) + sizeof "/build/aot-targets/" + strlen(target);
    char *candidate = malloc(length);
    if (candidate != nullptr) {
        snprintf(candidate, length, "%s/build/aot-targets/%s", repo_root, target);
        if (kit_has_runtime(candidate)) found = candidate; else free(candidate);
    }
    free(repo_root);
    return found;
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
        const char *output_path, const char *cc_override, const char *target, bool lto) {
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
    if (target != nullptr) {
        char *target_path = kit_path(kit, "target");
        char *recorded = target_path != nullptr ? read_file(target_path) : nullptr;
        free(target_path);
        if (recorded != nullptr) recorded[strcspn(recorded, "\n")] = '\0';
        const bool matches = recorded != nullptr && strcmp(recorded, target) == 0;
        if (!matches) {
            fprintf(stderr, "diamond: AOT kit '%s' is for target '%s', not '%s'\n",
                    kit, recorded != nullptr ? recorded : "this machine", target);
            free(recorded);
            return 74;
        }
        free(recorded);
    }

    enum { MAX_ARGS = 256 };
    char *args[MAX_ARGS];
    size_t count = 0;
    char *cc_text = nullptr, *compile_text = nullptr, *link_text = nullptr;
    /* --lto links the kit's second runtime (fat LTO objects at -O3, `make aot-kit AOT_LTO=1`),
     * compiled with its own flags; the compiler then optimises runtime and program together. */
    char *runtime = kit_path(kit, lto ? "libdiamond-aot-lto.a" : "libdiamond-aot.a");
    char *reginold = kit_path(kit, "libreginold.a");
    int result = 74;
    if (cc_override != nullptr) {
        args[count++] = (char *)cc_override;
    } else if (!append_kit_lines(kit, "cc", args, &count, 2, &cc_text) || count != 1) {
        fprintf(stderr, "diamond: AOT kit '%s' does not name a compiler\n", kit);
        goto done;
    }
    if (lto && (runtime == nullptr || access(runtime, R_OK) != 0)) {
        fprintf(stderr, "diamond: AOT kit '%s' has no LTO runtime (build the kit with "
            "`make aot-kit AOT_LTO=1`, see docs/deployment.md)\n", kit);
        goto done;
    }
    if (runtime == nullptr || reginold == nullptr ||
        !append_kit_lines(kit, lto ? "compile-lto.args" : "compile.args", args, &count,
            MAX_ARGS, &compile_text) ||
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
        fprintf(stderr, "usage: diamond build SOURCE [-o OUTPUT] [--cc=COMPILER] [--target=TRIPLE] [--lto]\n");
        return 64;
    }
    const char *source_path = argv[2];
    const char *output_path = nullptr;
    const char *cc = nullptr;
    const char *target = nullptr;
    bool lto = false;
    for (int index = 3; index < argc; index++) {
        if (strcmp(argv[index], "-o") == 0 && index + 1 < argc) {
            output_path = argv[++index];
        } else if (strcmp(argv[index], "--lto") == 0) {
            lto = true;
        } else if (strncmp(argv[index], "--cc=", 5) == 0) {
            cc = argv[index] + 5;
        } else if (strncmp(argv[index], "--target=", 9) == 0) {
            target = argv[index] + 9;
            if (!target_name_is_valid(target)) {
                fprintf(stderr, "diamond: invalid build target '%s'\n", target);
                return 64;
            }
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

    char *kit = target != nullptr ? find_target_kit(argv[0], target)
                                  : find_aot_kit(argv[0]);
    int status;
    if (target != nullptr && kit == nullptr) {
        fprintf(stderr,
            "diamond: no AOT kit for target '%s' (build one with `make aot-kit-cross "
            "TARGET_TRIPLE=%s SYSROOT=...`, see docs/deployment.md)\n", target, target);
        status = 74;
    } else if (lto && kit == nullptr) {
        fprintf(stderr, "diamond: --lto needs an installed AOT kit built with "
            "`make aot-kit AOT_LTO=1` (see docs/deployment.md)\n");
        status = 74;
    } else {
        status = kit != nullptr
            ? run_kit_aot_build(kit, embed_path, output_path, cc, target, lto)
            : run_make_aot_build(embed_path, output_path, cc, argv[0]);
    }
    free(kit);
    unlink(bin_path);
    unlink(embed_path);
    if (status == 0) {
        if (target != nullptr) printf("diamond: built '%s' for %s\n", output_path, target);
        else printf("diamond: built '%s'\n", output_path);
    }
    free(derived_output);
    return status;
}

/* `diamond cache clear [--dry-run] [PATH...]`: removes bytecode caches
 * (docs/caching.md). A PATH that is a file names that script's own cache; a
 * directory is searched recursively for `.d6c` and legacy `.dic` files; with no PATH it is the
 * current directory. Only files that start with a cache's magic bytes are
 * removed, and symbolic links are never followed. */
typedef struct CacheClearTotals {
    size_t files;
    uint64_t bytes;
    bool dry_run;
    bool failed;
} CacheClearTotals;

static void cache_clear_file(const char *path, CacheClearTotals *totals) {
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode)) return;
    if (!diamond_cache_file_is_cache(path)) return;
    if (!totals->dry_run && unlink(path) != 0) {
        fprintf(stderr, "diamond: cannot remove %s: %s\n", path, strerror(errno));
        totals->failed = true;
        return;
    }
    printf("%s %s\n", totals->dry_run ? "would remove" : "removed", path);
    totals->files++;
    totals->bytes += (uint64_t)info.st_size;
}

static void cache_clear_directory(const char *directory, CacheClearTotals *totals) {
    DIR *handle = opendir(directory);
    if (handle == nullptr) {
        fprintf(stderr, "diamond: cannot read %s: %s\n", directory, strerror(errno));
        totals->failed = true;
        return;
    }
    const struct dirent *entry;
    while ((entry = readdir(handle)) != nullptr) {
        const char *name = entry->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, ".git") == 0)
            continue;
        const size_t directory_length = strlen(directory);
        const size_t name_length = strlen(name);
        char *path = malloc(directory_length + name_length + 2);
        if (path == nullptr) continue;
        memcpy(path, directory, directory_length);
        path[directory_length] = '/';
        memcpy(path + directory_length + 1, name, name_length + 1);
        struct stat info;
        if (lstat(path, &info) == 0) {
            if (S_ISDIR(info.st_mode)) {
                cache_clear_directory(path, totals);
            } else if (name_length > 4 &&
                       (strcmp(name + name_length - 4, ".d6c") == 0 ||
                        strcmp(name + name_length - 4, ".dic") == 0)) {
                cache_clear_file(path, totals);
            }
        }
        free(path);
    }
    closedir(handle);
}

/* `diamond cache warm SOURCE...`: compiles each script and writes its cache
 * without running it. */
static int handle_cache_warm(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: diamond cache warm SOURCE...\n");
        return 64;
    }
    int worst = 0;
    for (int index = 3; index < argc; index++) {
        char *source = read_file(argv[index]);
        if (source == nullptr) {
            worst = worst > 74 ? worst : 74;
            continue;
        }
        const int status = diamond_warm_cache(argv[index], source);
        free(source);
        if (status > worst) worst = status;
    }
    return worst;
}

static int handle_cache_command(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[2], "warm") == 0) return handle_cache_warm(argc, argv);
    if (argc < 3 || strcmp(argv[2], "clear") != 0) {
        fprintf(stderr, "usage: diamond cache clear [--dry-run] [PATH...]\n"
                        "       diamond cache warm SOURCE...\n");
        return 64;
    }
    CacheClearTotals totals = {0};
    int first_path = 3;
    if (first_path < argc && strcmp(argv[first_path], "--dry-run") == 0) {
        totals.dry_run = true;
        first_path++;
    }
    if (first_path >= argc) {
        cache_clear_directory(".", &totals);
    }
    for (int index = first_path; index < argc; index++) {
        struct stat info;
        if (stat(argv[index], &info) != 0) {
            fprintf(stderr, "diamond: cannot read %s: %s\n", argv[index], strerror(errno));
            totals.failed = true;
        } else if (S_ISDIR(info.st_mode)) {
            cache_clear_directory(argv[index], &totals);
        } else {
            char *path = diamond_cache_path_for(argv[index]);
            if (path != nullptr) {
                cache_clear_file(path, &totals);
                free(path);
            }
        }
    }
    const bool megabytes = totals.bytes >= 1000000;
    printf("%s %zu cache file%s (%.1f %s)\n", totals.dry_run ? "would remove" : "removed",
           totals.files, totals.files == 1 ? "" : "s",
           (double)totals.bytes / (megabytes ? 1e6 : 1e3), megabytes ? "MB" : "KB");
    return totals.failed ? 1 : 0;
}

static void print_usage(FILE *stream) {
    fputs("Usage: diamond [OPTIONS] [FILE [ARGS...]]\n"
          "       diamond -e CODE [ARGS...]\n"
          "       diamond --dump-bytecode[=user|all] [-e CODE | FILE] [ARGS...]\n"
          "       diamond build SOURCE [-o OUTPUT] [--cc=COMPILER]\n"
          "       diamond cache clear [--dry-run] [PATH...]\n"
          "       diamond cache warm SOURCE...\n"
          "\n"
          "Run a Diamond program, evaluate source, or start the REPL when no\n"
          "arguments are given and standard input is a terminal.\n"
          "\n"
          "Options:\n"
          "  -e CODE             evaluate CODE\n"
          "  --dump-bytecode     print all bytecode before running (same as =all)\n"
          "  --dump-bytecode=user  print application/import bytecode without the preamble\n"
          "  --sandbox           deny filesystem/network/subprocess access --\n"
          "                      see docs/sandbox.md\n"
          "  -h, --help          display this help and exit\n"
          "  -v, --version       display version information and exit\n"
          "\n"
          "'diamond build' compiles SOURCE into a standalone executable (default\n"
          "output: SOURCE's own basename with its extension stripped) -- see\n"
          "docs/deployment.md. The Diamond build checkout must be available.\n"
          "\n"
          "'diamond cache clear' removes the bytecode caches (.d6c files) next to\n"
          "scripts: those of the given files, every one under the given\n"
          "directories, or under the current directory. 'diamond cache warm'\n"
          "compiles scripts and writes their caches without running them, for\n"
          "image builds -- see docs/caching.md.\n"
          "\n"
          "ARGS after FILE or CODE are available to the program through ARGV.\n",
          stream);
}

static int diamond_main(int argc, char **argv) {
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
        printf("diamond %s%s\n", DIAMOND_VERSION, DIAMOND_BUILD_NOTE);
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
    if (argc >= 2 && strcmp(argv[1], "cache") == 0) {
        return handle_cache_command(argc, argv);
    }
    /* This process runs one program, so a failed program with threads
     * still running may exit instead of waiting on them (see vm.h). */
    diamond_vm_set_exit_on_threaded_failure(true);
    if (argc >= 3 && strcmp(argv[1], "-e") == 0) {
        return diamond_run_source("-e", argv[2], false, argc - 3, argv + 3);
    }
    DiamondBytecodeDump dump_mode=DIAMOND_DUMP_NONE;
    if(argc>=2) {
        if(strcmp(argv[1],"--dump-bytecode")==0||strcmp(argv[1],"--dump-bytecode=all")==0)
            dump_mode=DIAMOND_DUMP_ALL;
        else if(strcmp(argv[1],"--dump-bytecode=user")==0)
            dump_mode=DIAMOND_DUMP_USER;
        else if(strncmp(argv[1],"--dump-bytecode=",16)==0) {
            print_usage(stderr);
            return 64;
        }
    }
    if (argc >= 4 && dump_mode && strcmp(argv[2], "-e") == 0) {
        return diamond_run_source("-e", argv[3], dump_mode, argc - 4, argv + 4);
    }
    const bool dump_file = dump_mode != DIAMOND_DUMP_NONE;
    if ((argc >= 2 && !dump_file) || (dump_file && argc >= 3)) {
        const char *path = dump_file ? argv[2] : argv[1];
        char *source = read_file(path);
        if (source == nullptr) {
            return 74;
        }
        const int arg_start = dump_file ? 3 : 2;
        const int status = diamond_run_source(path, source, dump_mode,
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

/* The interpreter recurses natively once per Diamond call, so how deep a
 * program may recurse is bounded by its thread's stack, and the process's
 * main thread has only the OS default (usually 8 MiB). The program therefore
 * runs on a thread with a much larger stack (address space only; pages are
 * committed as recursion actually uses them), and the call-depth limit
 * scales with it (diamond_set_thread_stack_size). DIAMOND_MAIN_STACK_MB
 * overrides the size in MiB; 0 runs on the main thread. If the thread cannot
 * be created the program runs on the main thread, exactly as before. */
typedef struct MainThreadStart {
    int argc;
    char **argv;
    size_t stack_bytes;
    sigset_t signal_mask;
    int result;
} MainThreadStart;

static void *main_thread_entry(void *argument) {
    MainThreadStart *start = argument;
    /* Signals the launching thread blocked so they would land here. */
    pthread_sigmask(SIG_SETMASK, &start->signal_mask, nullptr);
    diamond_set_thread_stack_size(start->stack_bytes);
    start->result = diamond_main(start->argc, start->argv);
    return nullptr;
}

int main(int argc, char **argv) {
    size_t megabytes = 512;
    const char *override = getenv("DIAMOND_MAIN_STACK_MB");
    if (override != nullptr && override[0] != '\0') {
        char *end = nullptr;
        const unsigned long parsed = strtoul(override, &end, 10);
        if (*end == '\0') megabytes = (size_t)parsed;
    }
    for (; megabytes > 8; megabytes /= 2) {
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) != 0) break;
        MainThreadStart start = {.argc = argc, .argv = argv,
            .stack_bytes = megabytes * 1024 * 1024};
        pthread_t thread;
        sigset_t all;
        sigfillset(&all);
        bool created = pthread_attr_setstacksize(&attributes, start.stack_bytes) == 0;
        if (created) {
            /* Blocked here, restored in the new thread: the new thread
             * inherits this mask, and this one then keeps every signal
             * blocked so process-directed ones are delivered to the thread
             * running the program (and interrupt its blocking calls). */
            pthread_sigmask(SIG_SETMASK, &all, &start.signal_mask);
            created = pthread_create(&thread, &attributes, main_thread_entry, &start) == 0;
            if (!created) pthread_sigmask(SIG_SETMASK, &start.signal_mask, nullptr);
        }
        pthread_attr_destroy(&attributes);
        if (created) {
            pthread_join(thread, nullptr);
            return start.result;
        }
    }
    return diamond_main(argc, argv);
}
