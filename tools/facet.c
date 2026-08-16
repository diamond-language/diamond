#define _XOPEN_SOURCE 700
#include "compiler.h"
#include "vm.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum {
    FACET_MAX_NAME = 64,
    FACET_MAX_URL = 512,
    FACET_MAX_REF = 128,
    FACET_MAX_PATH = 4096,
    FACET_MAX_COMMIT = 80,
    FACET_MAX_DEPENDENCIES = 64,
};

typedef struct FacetDependency {
    char name[FACET_MAX_NAME];
    char git[FACET_MAX_URL];
    char ref[FACET_MAX_REF];
    bool ref_is_commit;
} FacetDependency;

typedef struct FacetManifest {
    char name[FACET_MAX_NAME];
    char version[FACET_MAX_NAME];
    bool has_version;
    FacetDependency dependencies[FACET_MAX_DEPENDENCIES];
    size_t dependency_count;
} FacetManifest;

typedef struct FacetResolved {
    char name[FACET_MAX_NAME];
    char git[FACET_MAX_URL];
    char ref[FACET_MAX_REF];
    char commit[FACET_MAX_COMMIT];
    char required_by[FACET_MAX_NAME];
} FacetResolved;

typedef struct FacetResolution {
    FacetResolved packages[FACET_MAX_DEPENDENCIES];
    size_t count;
} FacetResolution;

/* DiamondProgram/DiamondVm are heap-allocated everywhere below, never
 * stack-declared: sizeof(DiamondProgram) is over 3MB (see the same note
 * in src/loader.c's validate_package_manifest, the pattern this file's
 * manifest/lockfile reading mirrors). */
typedef struct FacetProgram {
    DiamondProgram *program;
    DiamondVm *vm;
    bool vm_initialized;
} FacetProgram;

static bool parse_manifest(const char *path, FacetManifest *manifest,
                           char *error, size_t error_size);
static bool resolve_manifest_dependencies(const FacetManifest *manifest,
    const char *required_by, FacetResolution *resolution,
    const char *scratch_root, char *error, size_t error_size);

static bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static bool is_safe_field(const char *text) {
    for (const char *cursor = text; *cursor != '\0'; cursor++)
        if (*cursor == '"' || *cursor == '\\' || (unsigned char)*cursor < 0x20)
            return false;
    return true;
}

static char *read_whole_file(const char *path, char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        (void)snprintf(error, error_size, "cannot open '%s': %s", path,
                       strerror(errno));
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)snprintf(error, error_size, "cannot seek '%s': %s", path,
                       strerror(errno));
        fclose(file);
        return nullptr;
    }
    const long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        (void)snprintf(error, error_size, "cannot read '%s'", path);
        fclose(file);
        return nullptr;
    }
    char *source = malloc((size_t)size + 1);
    if (source == nullptr) {
        (void)snprintf(error, error_size, "out of memory reading '%s'", path);
        fclose(file);
        return nullptr;
    }
    if (fread(source, 1, (size_t)size, file) != (size_t)size) {
        (void)snprintf(error, error_size, "cannot read '%s'", path);
        free(source);
        fclose(file);
        return nullptr;
    }
    source[size] = '\0';
    fclose(file);
    return source;
}

static bool ensure_directory(const char *path) {
    char buffer[FACET_MAX_PATH];
    const int written = snprintf(buffer, sizeof buffer, "%s", path);
    if (written < 0 || (size_t)written >= sizeof buffer) return false;
    const size_t length = (size_t)written;
    for (size_t index = 1; index < length; index++) {
        if (buffer[index] != '/') continue;
        buffer[index] = '\0';
        if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;
        buffer[index] = '/';
    }
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) return false;
    return true;
}

static int remove_entry(const char *path, const struct stat *info, int flag,
                        struct FTW *walker) {
    (void)info; (void)flag; (void)walker;
    return remove(path);
}

static bool remove_directory_recursive(const char *path) {
    if (!file_exists(path)) return true;
    return nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0;
}

/* --- git subprocess handling: execvp with an argv array, never a shell,
 * so a URL/ref pulled from a manifest can never be interpreted as shell
 * syntax. --- */

static bool run_git(char *const argv[], const char *context, char *error,
                    size_t error_size) {
    const pid_t pid = fork();
    if (pid < 0) {
        (void)snprintf(error, error_size, "fork failed while %s: %s", context,
                       strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp("git", argv);
        fprintf(stderr, "facet: cannot execute 'git': %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        (void)snprintf(error, error_size, "waitpid failed while %s: %s",
                       context, strerror(errno));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void)snprintf(error, error_size, "git failed while %s", context);
        return false;
    }
    return true;
}

static bool run_git_capture(char *const argv[], const char *context, char *out,
                            size_t out_size, char *error, size_t error_size) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        (void)snprintf(error, error_size, "pipe failed while %s: %s", context,
                       strerror(errno));
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        (void)snprintf(error, error_size, "fork failed while %s: %s", context,
                       strerror(errno));
        close(pipe_fds[0]); close(pipe_fds[1]);
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], STDOUT_FILENO);
        close(pipe_fds[1]);
        execvp("git", argv);
        fprintf(stderr, "facet: cannot execute 'git': %s\n", strerror(errno));
        _exit(127);
    }
    close(pipe_fds[1]);
    size_t length = 0;
    while (length + 1 < out_size) {
        const ssize_t read_count = read(pipe_fds[0], out + length,
                                        out_size - length - 1);
        if (read_count <= 0) break;
        length += (size_t)read_count;
    }
    out[length] = '\0';
    char drain[256];
    while (read(pipe_fds[0], drain, sizeof drain) > 0) { }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    while (length > 0 && (out[length - 1] == '\n' || out[length - 1] == '\r'))
        out[--length] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        (void)snprintf(error, error_size, "git failed while %s", context);
        return false;
    }
    return true;
}

/* Always --no-single-branch: a plain single-branch clone only fetches
 * the default branch's history, so checking out a tag/branch/commit
 * that isn't on that branch would otherwise fail unpredictably. Costs
 * a bit more clone bandwidth in exchange for tag/branch/commit all
 * working uniformly through the same two commands. */
/* `url`/`ref` come straight from a dependency's manifest/lockfile --
 * untrusted input crossing exactly the boundary a package manager exists
 * to cross safely. execvp (never a shell) already rules out shell
 * metacharacter injection (see the comment above run_git), but git
 * itself parses any argument starting with `-` as an option regardless
 * of position, so a manifest url/ref like "--upload-pack=..." would
 * otherwise be parsed as a git flag rather than a literal repository/
 * revision string -- a well-known argument-injection class that reaches
 * arbitrary command execution through git's own hook/pack mechanisms.
 * `clone`'s positional args (url, destination) come after an ordinary
 * `--`; `checkout`'s single positional <ref> needs `--end-of-options`
 * instead (git >= 2.24) -- checkout's own `--` means "no more revisions,
 * everything after is a pathspec", which would silently reinterpret a
 * plain branch/tag/commit ref as a file path to restore rather than
 * something to check out. */
static bool git_clone(const char *url, const char *ref, const char *destination,
                      char *error, size_t error_size) {
    (void)remove_directory_recursive(destination);
    char *const clone_argv[] = {(char *)"git", (char *)"clone", (char *)"--quiet",
        (char *)"--no-single-branch", (char *)"--", (char *)url,
        (char *)destination, nullptr};
    char context[FACET_MAX_PATH];
    (void)snprintf(context, sizeof context, "cloning '%s'", url);
    if (!run_git(clone_argv, context, error, error_size)) return false;
    char *const checkout_argv[] = {(char *)"git", (char *)"-C",
        (char *)destination, (char *)"checkout", (char *)"--quiet",
        (char *)"--end-of-options", (char *)ref, nullptr};
    (void)snprintf(context, sizeof context, "checking out '%s' in '%s'", ref, url);
    return run_git(checkout_argv, context, error, error_size);
}

static bool git_rev_parse_head(const char *repository, char *out, size_t out_size,
                               char *error, size_t error_size) {
    char *const argv[] = {(char *)"git", (char *)"-C", (char *)repository,
        (char *)"rev-parse", (char *)"HEAD", nullptr};
    return run_git_capture(argv, "resolving the checked-out commit", out,
                           out_size, error, error_size);
}

/* --- manifest/lockfile reading: both are just Diamond Hash literals,
 * compiled and run standalone exactly the way src/loader.c's
 * validate_package_manifest already evaluates package.di - duplicated
 * here rather than shared, since this needs a different
 * error-reporting shape (stderr + exit code, not a Loader error
 * buffer) and facet.lock has no counterpart in the runtime at all. --- */

static bool hash_find(const DiamondHash *hash, const char *key, DiamondValue *out) {
    const size_t key_length = strlen(key);
    for (size_t index = 0; index < hash->count; index++) {
        const DiamondValue candidate = hash->entries[index].key;
        if (candidate.kind != DIAMOND_VALUE_OBJECT ||
            candidate.as.object->kind != DIAMOND_OBJECT_STRING) continue;
        const DiamondString *string = (const DiamondString *)candidate.as.object;
        if (string->length == key_length &&
            memcmp(string->chars, key, key_length) == 0) {
            *out = hash->entries[index].value;
            return true;
        }
    }
    return false;
}

static bool hash_find_string(const DiamondHash *hash, const char *key, char *out,
                             size_t out_size) {
    DiamondValue value;
    if (!hash_find(hash, key, &value)) return false;
    if (value.kind != DIAMOND_VALUE_OBJECT ||
        value.as.object->kind != DIAMOND_OBJECT_STRING) return false;
    const DiamondString *string = (const DiamondString *)value.as.object;
    if (string->length >= out_size) return false;
    memcpy(out, string->chars, string->length);
    out[string->length] = '\0';
    return true;
}

static bool facet_run_hash(const char *path, FacetProgram *owner,
                           const DiamondHash **out_hash, char *error,
                           size_t error_size) {
    owner->program = malloc(sizeof *owner->program);
    owner->vm = malloc(sizeof *owner->vm);
    owner->vm_initialized = false;
    if (owner->program == nullptr || owner->vm == nullptr) {
        (void)snprintf(error, error_size, "out of memory reading '%s'", path);
        return false;
    }
    char *source = read_whole_file(path, error, error_size);
    if (source == nullptr) return false;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(source, owner->program, &diagnostic)) {
        (void)snprintf(error, error_size, "'%s' failed to compile at line %zu: %s",
                       path, diagnostic.span.line, diagnostic.message);
        free(source);
        return false;
    }
    free(source);
    DiamondChunk chunk = diamond_program_chunk(owner->program);
    chunk.name = path;
    diamond_vm_init(owner->vm);
    owner->vm_initialized = true;
    DiamondValue result = DIAMOND_NIL;
    const DiamondVmStatus status = diamond_vm_run(owner->vm, &chunk, &result);
    if (status != DIAMOND_VM_OK) {
        const char *detail = diamond_vm_error(owner->vm);
        (void)snprintf(error, error_size, "'%s' failed: %s", path,
                       detail != nullptr ? detail : diamond_vm_status_name(status));
        return false;
    }
    if (result.kind != DIAMOND_VALUE_OBJECT ||
        result.as.object->kind != DIAMOND_OBJECT_HASH) {
        (void)snprintf(error, error_size, "'%s' must evaluate to a Hash", path);
        return false;
    }
    *out_hash = (const DiamondHash *)result.as.object;
    return true;
}

static void facet_program_free(FacetProgram *owner) {
    if (owner->vm_initialized) diamond_vm_free(owner->vm);
    free(owner->program);
    free(owner->vm);
}

static bool parse_dependencies(const DiamondHash *dependencies,
                               FacetManifest *manifest, char *error,
                               size_t error_size) {
    for (size_t index = 0; index < dependencies->count; index++) {
        const DiamondValue key = dependencies->entries[index].key;
        const DiamondValue value = dependencies->entries[index].value;
        if (key.kind != DIAMOND_VALUE_OBJECT ||
            key.as.object->kind != DIAMOND_OBJECT_STRING) {
            (void)snprintf(error, error_size,
                           "dependency name must be a String");
            return false;
        }
        const DiamondString *name_string = (const DiamondString *)key.as.object;
        if (value.kind != DIAMOND_VALUE_OBJECT ||
            value.as.object->kind != DIAMOND_OBJECT_HASH) {
            (void)snprintf(error, error_size,
                           "dependency spec for '%.*s' must be a Hash",
                           (int)name_string->length, name_string->chars);
            return false;
        }
        if (manifest->dependency_count == FACET_MAX_DEPENDENCIES) {
            (void)snprintf(error, error_size,
                           "too many dependencies (max %d)",
                           FACET_MAX_DEPENDENCIES);
            return false;
        }
        FacetDependency *dependency =
            &manifest->dependencies[manifest->dependency_count];
        if (name_string->length >= sizeof dependency->name) {
            (void)snprintf(error, error_size, "dependency name is too long");
            return false;
        }
        memcpy(dependency->name, name_string->chars, name_string->length);
        dependency->name[name_string->length] = '\0';
        if (!is_safe_field(dependency->name)) {
            (void)snprintf(error, error_size,
                           "dependency name '%s' contains an invalid character",
                           dependency->name);
            return false;
        }
        const DiamondHash *spec = (const DiamondHash *)value.as.object;
        if (!hash_find_string(spec, "git", dependency->git, sizeof dependency->git) ||
            !is_safe_field(dependency->git)) {
            (void)snprintf(error, error_size,
                           "dependency '%s' is missing a valid String 'git' key",
                           dependency->name);
            return false;
        }
        int ref_keys = 0;
        bool ref_is_commit = false;
        if (hash_find_string(spec, "tag", dependency->ref, sizeof dependency->ref)) {
            ref_keys++;
        }
        if (hash_find_string(spec, "branch", dependency->ref, sizeof dependency->ref)) {
            ref_keys++;
        }
        if (hash_find_string(spec, "commit", dependency->ref, sizeof dependency->ref)) {
            ref_keys++;
            ref_is_commit = true;
        }
        if (ref_keys != 1 || !is_safe_field(dependency->ref)) {
            (void)snprintf(error, error_size,
                "dependency '%s' must specify exactly one of tag/branch/commit",
                dependency->name);
            return false;
        }
        dependency->ref_is_commit = ref_is_commit;
        manifest->dependency_count++;
    }
    return true;
}

static bool parse_manifest(const char *path, FacetManifest *manifest,
                           char *error, size_t error_size) {
    memset(manifest, 0, sizeof *manifest);
    FacetProgram owner = {};
    const DiamondHash *hash = nullptr;
    bool ok = facet_run_hash(path, &owner, &hash, error, error_size);
    if (ok && (!hash_find_string(hash, "name", manifest->name,
                                 sizeof manifest->name) ||
              !is_safe_field(manifest->name))) {
        (void)snprintf(error, error_size,
                       "'%s' must have a valid String 'name' key", path);
        ok = false;
    }
    if (ok) {
        manifest->has_version = hash_find_string(hash, "version",
            manifest->version, sizeof manifest->version);
    }
    DiamondValue dependencies_value;
    if (ok && hash_find(hash, "dependencies", &dependencies_value)) {
        if (dependencies_value.kind != DIAMOND_VALUE_OBJECT ||
            dependencies_value.as.object->kind != DIAMOND_OBJECT_HASH) {
            (void)snprintf(error, error_size,
                           "'%s' key 'dependencies' must be a Hash", path);
            ok = false;
        } else if (!parse_dependencies(
                (const DiamondHash *)dependencies_value.as.object, manifest,
                error, error_size)) {
            ok = false;
        }
    }
    facet_program_free(&owner);
    return ok;
}

static bool parse_lockfile(const char *path, FacetResolution *resolution,
                           char *error, size_t error_size) {
    resolution->count = 0;
    FacetProgram owner = {};
    const DiamondHash *hash = nullptr;
    bool ok = facet_run_hash(path, &owner, &hash, error, error_size);
    for (size_t index = 0; ok && index < hash->count; index++) {
        const DiamondValue key = hash->entries[index].key;
        const DiamondValue value = hash->entries[index].value;
        if (key.kind != DIAMOND_VALUE_OBJECT ||
            key.as.object->kind != DIAMOND_OBJECT_STRING ||
            value.kind != DIAMOND_VALUE_OBJECT ||
            value.as.object->kind != DIAMOND_OBJECT_HASH) {
            (void)snprintf(error, error_size, "'%s' has a malformed entry", path);
            ok = false;
            break;
        }
        if (resolution->count == FACET_MAX_DEPENDENCIES) {
            (void)snprintf(error, error_size, "'%s' has too many entries", path);
            ok = false;
            break;
        }
        FacetResolved *resolved = &resolution->packages[resolution->count];
        const DiamondString *name_string = (const DiamondString *)key.as.object;
        if (name_string->length >= sizeof resolved->name) {
            (void)snprintf(error, error_size, "package name in '%s' is too long",
                           path);
            ok = false;
            break;
        }
        memcpy(resolved->name, name_string->chars, name_string->length);
        resolved->name[name_string->length] = '\0';
        const DiamondHash *entry = (const DiamondHash *)value.as.object;
        if (!hash_find_string(entry, "git", resolved->git, sizeof resolved->git) ||
            !hash_find_string(entry, "commit", resolved->commit,
                              sizeof resolved->commit) ||
            !is_safe_field(resolved->name) || !is_safe_field(resolved->git) ||
            !is_safe_field(resolved->commit)) {
            (void)snprintf(error, error_size,
                "'%s' entry '%s' must have valid String 'git' and 'commit' keys",
                path, resolved->name);
            ok = false;
            break;
        }
        resolved->ref[0] = '\0';
        resolved->required_by[0] = '\0';
        resolution->count++;
    }
    facet_program_free(&owner);
    return ok;
}

static bool write_lockfile(const char *path, const FacetResolution *resolution,
                           char *error, size_t error_size) {
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        (void)snprintf(error, error_size, "cannot write '%s': %s", path,
                       strerror(errno));
        return false;
    }
    /* Single-line: Diamond's Hash-literal parser doesn't accept a
     * newline between "{" and its first entry (a pre-existing parser
     * limitation, not new to this feature), so a pretty-printed
     * multi-line lockfile would fail to parse back on the very next
     * `facet install`. */
    fputs("{", file);
    for (size_t index = 0; index < resolution->count; index++) {
        const FacetResolved *resolved = &resolution->packages[index];
        fprintf(file, "\"%s\": {\"git\": \"%s\", \"commit\": \"%s\"}, ",
                resolved->name, resolved->git, resolved->commit);
    }
    fputs("}\n", file);
    const bool ok = fclose(file) == 0;
    if (!ok) {
        (void)snprintf(error, error_size, "cannot write '%s': %s", path,
                       strerror(errno));
    }
    return ok;
}

/* --- dependency resolution: one recursive walk, since there's no
 * registry to consult metadata from - discovering a dependency's own
 * dependencies requires a clone of it first. A name seen twice with the
 * same (git, ref) is idempotent (also what makes a genuine cycle
 * terminate safely, with no separate cycle-detection code: the second
 * time a cycle reaches an already-resolved name, it just stops). A name
 * seen twice with different (git, ref) is a hard conflict - the
 * language's flat single-namespace compilation model (see
 * docs/packages.md) means there is no such thing as "both versions,"
 * so this can never be silently resolved. --- */

static bool resolve_dependency(const FacetDependency *dependency,
    const char *required_by, FacetResolution *resolution,
    const char *scratch_root, char *error, size_t error_size) {
    for (size_t index = 0; index < resolution->count; index++) {
        FacetResolved *existing = &resolution->packages[index];
        if (strcmp(existing->name, dependency->name) != 0) continue;
        if (strcmp(existing->git, dependency->git) == 0 &&
            strcmp(existing->ref, dependency->ref) == 0) {
            return true;
        }
        (void)snprintf(error, error_size,
            "conflicting dependency '%s': '%s' wants %s@%s, but '%s' wants %s@%s",
            dependency->name, required_by, dependency->git, dependency->ref,
            existing->required_by, existing->git, existing->ref);
        return false;
    }
    if (resolution->count == FACET_MAX_DEPENDENCIES) {
        (void)snprintf(error, error_size,
                       "too many resolved dependencies (max %d)",
                       FACET_MAX_DEPENDENCIES);
        return false;
    }
    char scratch_path[FACET_MAX_PATH];
    int written = snprintf(scratch_path, sizeof scratch_path, "%s/%s",
                           scratch_root, dependency->name);
    if (written < 0 || (size_t)written >= sizeof scratch_path) {
        (void)snprintf(error, error_size, "path too long while resolving '%s'",
                       dependency->name);
        return false;
    }
    if (!git_clone(dependency->git, dependency->ref, scratch_path, error,
                   error_size)) {
        return false;
    }
    char commit[FACET_MAX_COMMIT];
    if (!git_rev_parse_head(scratch_path, commit, sizeof commit, error,
                            error_size)) {
        return false;
    }

    FacetResolved *resolved = &resolution->packages[resolution->count++];
    (void)snprintf(resolved->name, sizeof resolved->name, "%s", dependency->name);
    (void)snprintf(resolved->git, sizeof resolved->git, "%s", dependency->git);
    (void)snprintf(resolved->ref, sizeof resolved->ref, "%s", dependency->ref);
    (void)snprintf(resolved->commit, sizeof resolved->commit, "%s", commit);
    (void)snprintf(resolved->required_by, sizeof resolved->required_by, "%s",
                   required_by);

    char nested_manifest_path[FACET_MAX_PATH];
    written = snprintf(nested_manifest_path, sizeof nested_manifest_path,
                       "%s/package.di", scratch_path);
    if (written < 0 || (size_t)written >= sizeof nested_manifest_path) {
        (void)snprintf(error, error_size, "path too long while resolving '%s'",
                       dependency->name);
        return false;
    }
    if (file_exists(nested_manifest_path)) {
        FacetManifest nested;
        if (!parse_manifest(nested_manifest_path, &nested, error, error_size)) {
            return false;
        }
        if (strcmp(nested.name, dependency->name) != 0) {
            (void)snprintf(error, error_size,
                           "'%s' declares name '%s', expected '%s'",
                           nested_manifest_path, nested.name, dependency->name);
            return false;
        }
        if (!resolve_manifest_dependencies(&nested, dependency->name, resolution,
                                           scratch_root, error, error_size)) {
            return false;
        }
    }
    return true;
}

static bool resolve_manifest_dependencies(const FacetManifest *manifest,
    const char *required_by, FacetResolution *resolution,
    const char *scratch_root, char *error, size_t error_size) {
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        if (!resolve_dependency(&manifest->dependencies[index], required_by,
                                resolution, scratch_root, error, error_size)) {
            return false;
        }
    }
    return true;
}

/* --- install: move each resolved package's checkout into
 * diamond_packages/<name>, stripping .git first - facet.lock, not a
 * live repo sitting inside diamond_packages/, is the source of truth
 * for "what commit." The scratch root lives inside diamond_packages/
 * itself so this rename() is always same-filesystem. --- */

static bool strip_git_directory(const char *package_path) {
    char git_path[FACET_MAX_PATH];
    const int written = snprintf(git_path, sizeof git_path, "%s/.git",
                                 package_path);
    if (written < 0 || (size_t)written >= sizeof git_path) return false;
    return remove_directory_recursive(git_path);
}

static bool install_resolution(const FacetResolution *resolution,
                               const char *scratch_root, char *error,
                               size_t error_size) {
    for (size_t index = 0; index < resolution->count; index++) {
        const FacetResolved *resolved = &resolution->packages[index];
        char scratch_path[FACET_MAX_PATH];
        char final_path[FACET_MAX_PATH];
        (void)snprintf(scratch_path, sizeof scratch_path, "%s/%s", scratch_root,
                       resolved->name);
        (void)snprintf(final_path, sizeof final_path, "diamond_packages/%s",
                       resolved->name);
        if (!file_exists(scratch_path)) {
            /* facet.lock-driven install: resolution didn't clone anything
             * (it skipped the walk entirely), so clone the pinned commit now. */
            if (!git_clone(resolved->git, resolved->commit, scratch_path, error,
                           error_size)) {
                return false;
            }
        }
        if (!strip_git_directory(scratch_path)) {
            (void)snprintf(error, error_size, "cannot remove '.git' from '%s'",
                           scratch_path);
            return false;
        }
        (void)remove_directory_recursive(final_path);
        if (rename(scratch_path, final_path) != 0) {
            (void)snprintf(error, error_size, "cannot install '%s': %s",
                           resolved->name, strerror(errno));
            return false;
        }
        printf("facet: installed %s (%s)\n", resolved->name, resolved->commit);
    }
    return true;
}

static int run_install_or_update(bool force_resolve) {
    char error[512];
    if (!file_exists("package.di")) {
        fprintf(stderr, "facet: no package.di found in the current directory\n");
        return 66;
    }
    const char *scratch_root = "diamond_packages/.facet-tmp";
    if (!ensure_directory("diamond_packages") || !ensure_directory(scratch_root)) {
        fprintf(stderr, "facet: cannot create '%s': %s\n", scratch_root,
                strerror(errno));
        return 74;
    }
    FacetResolution resolution = {0};
    bool ok;
    if (!force_resolve && file_exists("facet.lock")) {
        ok = parse_lockfile("facet.lock", &resolution, error, sizeof error);
    } else {
        FacetManifest manifest;
        ok = parse_manifest("package.di", &manifest, error, sizeof error) &&
             resolve_manifest_dependencies(&manifest, manifest.name, &resolution,
                                           scratch_root, error, sizeof error) &&
             write_lockfile("facet.lock", &resolution, error, sizeof error);
    }
    if (ok) ok = install_resolution(&resolution, scratch_root, error, sizeof error);
    (void)remove_directory_recursive(scratch_root);
    if (!ok) {
        fprintf(stderr, "facet: %s\n", error);
        return 70;
    }
    printf("facet: %zu package(s) installed\n", resolution.count);
    return 0;
}

static void print_usage(void) {
    fputs("usage: facet install\n       facet update\n", stderr);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "install") == 0) {
        return run_install_or_update(false);
    }
    if (argc == 2 && strcmp(argv[1], "update") == 0) {
        return run_install_or_update(true);
    }
    print_usage();
    return 64;
}
