#define _XOPEN_SOURCE 700
#include "compiler.h"
#include "semver.h"
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
    /* Exactly one of (ref, ref_is_commit) or (uses_version, version_text)
     * is meaningful, set at manifest-parse time by parse_dependencies --
     * see docs/roadmap.md's "Real semver dependency resolution". */
    char ref[FACET_MAX_REF];
    bool ref_is_commit;
    bool uses_version;
    char version_text[FACET_MAX_REF];
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
    /* The literal tag/branch/commit an exact-ref dependency asked for,
     * or the tag a version-constrained one resolved to (same as
     * `version` below in that case) -- "" only ever for a lockfile-
     * driven install, which never repopulates this (see parse_lockfile). */
    char ref[FACET_MAX_REF];
    char commit[FACET_MAX_COMMIT];
    /* "" unless this was resolved via a `version` constraint (as
     * opposed to an exact tag/branch/commit) -- the resolved tag text,
     * kept separate from `ref` so a later conflict check can tell "this
     * came from a range" apart from "this came from an exact ref" at a
     * glance, and so the lockfile can record it for transparency. */
    char version[FACET_MAX_REF];
    char required_by[FACET_MAX_NAME];
} FacetResolved;

typedef struct FacetResolution {
    FacetResolved packages[FACET_MAX_DEPENDENCIES];
    size_t count;
} FacetResolution;

/* A cut name seen with a `version` constraint from at least one
 * requester, not yet resolved to one concrete tag -- see
 * resolve_full_graph's own comment for why this can't just resolve
 * immediately the way an exact-ref dependency does. */
typedef struct FacetPendingSemver {
    char name[FACET_MAX_NAME];
    char git[FACET_MAX_URL];
    SemverConstraint constraint;
    /* The requester whose constraint is currently reflected in
     * `constraint` -- if several requesters have contributed via
     * intersection, this is just the most recent one, kept for error
     * messages rather than tracking the full history. */
    char first_requester[FACET_MAX_NAME];
} FacetPendingSemver;

typedef struct FacetPendingTable {
    FacetPendingSemver entries[FACET_MAX_DEPENDENCIES];
    size_t count;
} FacetPendingTable;

/* A cloned dependency's own diamond.cut, still needing its own
 * dependencies walked -- see resolve_full_graph's own comment. */
typedef struct FacetWorkItem {
    char path[FACET_MAX_PATH];
    char cut_name[FACET_MAX_NAME];
} FacetWorkItem;

typedef struct FacetWorkQueue {
    FacetWorkItem items[FACET_MAX_DEPENDENCIES];
    size_t count;
} FacetWorkQueue;

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
    const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
    FacetWorkQueue *queue, const char *scratch_root, char *error, size_t error_size);

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

/* Lists every tag on `url`'s remote whose name parses as a semver
 * version (with or without a leading v/V -- see tools/semver.h),
 * writing each matching tag's own original name (not normalized) into
 * tags[0..*out_count). This is the *entire* "version database" a
 * version-constrained dependency ever consults -- there is no registry
 * (docs/roadmap.md), so a repository's own tags are the only source of
 * "what versions exist." `--refs` excludes the `^{}` peeled-commit
 * duplicate entry an annotated tag would otherwise also produce. */
static bool git_list_semver_tags(const char *url, char tags[][FACET_MAX_REF],
        size_t capacity, size_t *out_count, char *error, size_t error_size) {
    char *const argv[] = {(char *)"git", (char *)"ls-remote", (char *)"--tags",
        (char *)"--refs", (char *)"--", (char *)url, nullptr};
    /* Heap, not stack -- generous enough that a real repository's tag
     * list (even a large one) fits comfortably; ls-remote lines are
     * short and fixed-width-ish, so this is not a tight bound. */
    const size_t output_capacity = 1u << 21;
    char *output = malloc(output_capacity);
    if (output == nullptr) {
        (void)snprintf(error, error_size, "out of memory listing tags for '%s'", url);
        return false;
    }
    char context[FACET_MAX_PATH];
    (void)snprintf(context, sizeof context, "listing tags for '%s'", url);
    if (!run_git_capture(argv, context, output, output_capacity, error, error_size)) {
        free(output);
        return false;
    }
    *out_count = 0;
    static const char prefix[] = "refs/tags/";
    char *line_cursor = output;
    while (*line_cursor != '\0') {
        char *line_end = strchr(line_cursor, '\n');
        if (line_end != nullptr) *line_end = '\0';
        char *tab = strchr(line_cursor, '\t');
        if (tab != nullptr) {
            const char *ref_name = tab + 1;
            if (strncmp(ref_name, prefix, sizeof prefix - 1) == 0) {
                const char *tag_name = ref_name + (sizeof prefix - 1);
                Semver probe;
                if (semver_parse(tag_name, &probe) && *out_count < capacity) {
                    (void)snprintf(tags[*out_count], FACET_MAX_REF, "%s", tag_name);
                    (*out_count)++;
                }
            }
        }
        if (line_end == nullptr) break;
        line_cursor = line_end + 1;
    }
    free(output);
    return true;
}

/* Picks the highest tag in tags[0..tag_count) that satisfies
 * `constraint` -- deterministic, no backtracking (docs/roadmap.md's
 * own "no real backtracking needed for a first version"). Returns
 * false if none satisfy it. */
static bool pick_best_matching_tag(char tags[][FACET_MAX_REF], size_t tag_count,
        const SemverConstraint *constraint, char *out_tag, size_t out_tag_size) {
    bool found = false;
    Semver best_version = {0};
    size_t best_index = 0;
    for (size_t index = 0; index < tag_count; index++) {
        Semver candidate;
        if (!semver_parse(tags[index], &candidate)) continue;
        if (!semver_satisfies(&candidate, constraint)) continue;
        if (!found || semver_compare(&candidate, &best_version) > 0) {
            found = true;
            best_version = candidate;
            best_index = index;
        }
    }
    if (!found) return false;
    (void)snprintf(out_tag, out_tag_size, "%s", tags[best_index]);
    return true;
}

/* --- manifest/lockfile reading: both are just Diamond Hash literals,
 * compiled and run standalone exactly the way src/loader.c's
 * validate_cut_manifest already evaluates a cut's own diamond.cut -
 * duplicated here rather than shared, since this needs a different
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
    /* calloc, not malloc: diamond_compile_impl's own first act is
     * diamond_program_free(program) (src/compiler.c), so it can be
     * called again to recompile an already-populated program -- that's
     * only safe when `program` starts out already valid (compiler.h's
     * own diamond_program_init doc comment: "Zeroes *program..."), not
     * freshly malloc'd garbage. Every other real caller gets this for
     * free (a `static`/global DiamondProgram is zero-initialized by C
     * itself, or it's already been through a prior diamond_program_
     * init/diamond_compile call) -- this is the one heap-allocated,
     * first-use case, so it has to ask for the zero-fill explicitly.
     * Found via a from-scratch ASan/UBSan build of facet (never done
     * before this file existed) crashing (SEGV freeing garbage
     * pointers, UBSan flagging a garbage `bool`) on the very first
     * diamond_compile call, reproduced identically on facet.c before
     * this commit's own changes -- pre-existing, unrelated to this
     * commit's actual feature work. diamond_vm_init has no equivalent
     * issue: it unconditionally overwrites every field via a struct-
     * literal assignment rather than reading anything first, so `vm`
     * staying a plain malloc is fine. */
    owner->program = calloc(1, sizeof *owner->program);
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
    /* diamond_program_free releases the program's own internal dynamic
     * arrays (bytecode, constants, strings, type sets, and every
     * compiled function's own copies of those) -- a second pre-existing
     * bug found by the same from-scratch ASan/UBSan build that caught
     * the calloc issue above: this only ever freed the outer
     * `*owner->program` block itself, leaking everything diamond_compile
     * allocated inside it on every single manifest/lockfile read.
     * Harmless in practice (facet is a short-lived CLI process; the OS
     * reclaims everything at exit either way), but a real, fixable leak
     * caught by the same investigation, not a new risk introduced by it. */
    diamond_program_free(owner->program);
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
        const bool has_version = hash_find_string(spec, "version", dependency->version_text,
            sizeof dependency->version_text);
        if (has_version) {
            ref_keys++;
            if (!is_safe_field(dependency->version_text)) {
                (void)snprintf(error, error_size,
                    "dependency '%s' has an invalid 'version' constraint",
                    dependency->name);
                return false;
            }
            SemverConstraint probe;
            if (!semver_constraint_parse(dependency->version_text, &probe)) {
                (void)snprintf(error, error_size,
                    "dependency '%s' has an invalid 'version' constraint '%s'",
                    dependency->name, dependency->version_text);
                return false;
            }
        }
        if (ref_keys != 1 || (!has_version && !is_safe_field(dependency->ref))) {
            (void)snprintf(error, error_size,
                "dependency '%s' must specify exactly one of tag/branch/commit/version",
                dependency->name);
            return false;
        }
        dependency->uses_version = has_version;
        dependency->ref_is_commit = !has_version && ref_is_commit;
        if (has_version) dependency->ref[0] = '\0';
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
        /* Purely informational if present -- `facet install` from a
         * lockfile never re-resolves, so nothing here reads it back;
         * `facet update` always re-resolves from diamond.cut instead of
         * consulting the old lockfile at all (see run_install_or_update). */
        resolved->version[0] = '\0';
        (void)hash_find_string(entry, "version", resolved->version, sizeof resolved->version);
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
        if (resolved->version[0] != '\0') {
            fprintf(file, "\"%s\": {\"git\": \"%s\", \"commit\": \"%s\", \"version\": \"%s\"}, ",
                    resolved->name, resolved->git, resolved->commit, resolved->version);
        } else {
            fprintf(file, "\"%s\": {\"git\": \"%s\", \"commit\": \"%s\"}, ",
                    resolved->name, resolved->git, resolved->commit);
        }
    }
    fputs("}\n", file);
    const bool ok = fclose(file) == 0;
    if (!ok) {
        (void)snprintf(error, error_size, "cannot write '%s': %s", path,
                       strerror(errno));
    }
    return ok;
}

/* --- dependency resolution ---
 *
 * An exact-ref dependency (tag/branch/commit) resolves exactly as
 * before: clone it immediately, and if it has its own diamond.cut,
 * queue that manifest's own dependencies for the same treatment. A
 * name seen twice with the same (git, ref) is idempotent (also what
 * makes a genuine cycle terminate safely, with no separate cycle-
 * detection code: the second time a cycle reaches an already-resolved
 * name, it just stops). A name seen twice with different (git, ref) is
 * a hard conflict -- the language's flat single-namespace compilation
 * model (see docs/packages.md) means there is no such thing as "both
 * versions," so this can never be silently resolved.
 *
 * A `version`-constrained dependency can't resolve immediately the
 * same way: there is no registry, so the only way to discover a cut's
 * *own* dependencies is to clone some concrete tag of it first -- but
 * which tag to clone depends on every requester's constraint, and
 * other requesters may not be discovered until *later* in the walk
 * (including from manifests this same resolution hasn't cloned yet).
 * So a version-constrained name is instead parked in a "pending" table,
 * accumulating the intersection of every constraint seen for it, and
 * only actually resolved (tags listed, best match picked, cloned) once
 * the exact-ref-reachable part of the graph runs out of new work --
 * at which point resolving one pending name can itself discover more
 * exact-ref work (queued) or tighten/conflict with other still-pending
 * names, so resolve_full_graph alternates between draining the queue
 * and resolving one pending name until both are empty. Mixing an
 * exact ref and a version constraint for the same name is a hard
 * error in both directions (see handle_exact_dependency/
 * handle_version_dependency) -- "no principled way to compare an
 * arbitrary commit against a range's intent" (docs/roadmap.md) --
 * except when the side resolved first happens to already satisfy the
 * other's own constraint, in which case there is nothing to reconcile. */

static bool find_resolved(FacetResolution *resolution, const char *name,
        FacetResolved **out) {
    for (size_t index = 0; index < resolution->count; index++) {
        if (strcmp(resolution->packages[index].name, name) == 0) {
            *out = &resolution->packages[index];
            return true;
        }
    }
    return false;
}

static bool find_pending(FacetPendingTable *pending, const char *name,
        FacetPendingSemver **out) {
    for (size_t index = 0; index < pending->count; index++) {
        if (strcmp(pending->entries[index].name, name) == 0) {
            *out = &pending->entries[index];
            return true;
        }
    }
    return false;
}

static bool work_queue_push(FacetWorkQueue *queue, const char *path,
        const char *cut_name, char *error, size_t error_size) {
    if (queue->count == FACET_MAX_DEPENDENCIES) {
        (void)snprintf(error, error_size, "too many pending manifests (max %d)",
                       FACET_MAX_DEPENDENCIES);
        return false;
    }
    (void)snprintf(queue->items[queue->count].path, sizeof queue->items[queue->count].path,
                   "%s", path);
    (void)snprintf(queue->items[queue->count].cut_name,
                   sizeof queue->items[queue->count].cut_name, "%s", cut_name);
    queue->count++;
    return true;
}

static bool handle_exact_dependency(const FacetDependency *dependency,
        const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
        FacetWorkQueue *queue, const char *scratch_root, char *error, size_t error_size) {
    FacetResolved *existing;
    if (find_resolved(resolution, dependency->name, &existing)) {
        if (existing->version[0] != '\0') {
            (void)snprintf(error, error_size,
                "conflicting dependency '%s': '%s' wants an exact %s@%s, but it "
                "was already resolved to version %s via a semver constraint "
                "from '%s' -- mixing an exact ref and a version constraint for "
                "the same dependency is not supported",
                dependency->name, required_by, dependency->git, dependency->ref,
                existing->version, existing->required_by);
            return false;
        }
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
    FacetPendingSemver *pending_existing;
    if (find_pending(pending, dependency->name, &pending_existing)) {
        char range_text[128];
        (void)semver_constraint_format(&pending_existing->constraint, range_text,
                                       sizeof range_text);
        (void)snprintf(error, error_size,
            "conflicting dependency '%s': '%s' wants an exact %s@%s, but '%s' "
            "already constrained it to version %s -- mixing an exact ref and a "
            "version constraint for the same dependency is not supported",
            dependency->name, required_by, dependency->git, dependency->ref,
            pending_existing->first_requester, range_text);
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
    resolved->version[0] = '\0';
    (void)snprintf(resolved->required_by, sizeof resolved->required_by, "%s",
                   required_by);

    char nested_manifest_path[FACET_MAX_PATH];
    written = snprintf(nested_manifest_path, sizeof nested_manifest_path,
                       "%s/diamond.cut", scratch_path);
    if (written < 0 || (size_t)written >= sizeof nested_manifest_path) {
        (void)snprintf(error, error_size, "path too long while resolving '%s'",
                       dependency->name);
        return false;
    }
    if (file_exists(nested_manifest_path)) {
        return work_queue_push(queue, nested_manifest_path, dependency->name, error, error_size);
    }
    return true;
}

static bool handle_version_dependency(const FacetDependency *dependency,
        const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
        char *error, size_t error_size) {
    SemverConstraint constraint;
    if (!semver_constraint_parse(dependency->version_text, &constraint)) {
        /* Already validated at manifest-parse time (parse_dependencies) --
         * unreachable in practice, but fail clearly rather than silently
         * misbehaving if that ever drifts. */
        (void)snprintf(error, error_size,
            "dependency '%s' has an invalid version constraint '%s'",
            dependency->name, dependency->version_text);
        return false;
    }
    FacetResolved *existing;
    if (find_resolved(resolution, dependency->name, &existing)) {
        if (existing->version[0] == '\0') {
            (void)snprintf(error, error_size,
                "conflicting dependency '%s': '%s' wants version %s, but it was "
                "already resolved to an exact %s@%s (required by '%s') -- "
                "mixing an exact ref and a version constraint for the same "
                "dependency is not supported",
                dependency->name, required_by, dependency->version_text,
                existing->git, existing->ref, existing->required_by);
            return false;
        }
        Semver resolved_version;
        if (semver_parse(existing->version, &resolved_version) &&
            semver_satisfies(&resolved_version, &constraint)) {
            return true; /* already-resolved version also satisfies this one */
        }
        (void)snprintf(error, error_size,
            "conflicting dependency '%s': '%s' wants version %s, but it was "
            "already resolved to version %s (required by '%s') before this "
            "constraint was seen -- re-resolving an already-cloned dependency "
            "is not supported yet",
            dependency->name, required_by, dependency->version_text,
            existing->version, existing->required_by);
        return false;
    }
    FacetPendingSemver *pending_existing;
    if (find_pending(pending, dependency->name, &pending_existing)) {
        if (strcmp(pending_existing->git, dependency->git) != 0) {
            (void)snprintf(error, error_size,
                "conflicting dependency '%s': '%s' and '%s' point at different "
                "git repositories ('%s' vs '%s')",
                dependency->name, required_by, pending_existing->first_requester,
                dependency->git, pending_existing->git);
            return false;
        }
        SemverConstraint intersected;
        if (!semver_constraint_intersect(&pending_existing->constraint, &constraint,
                                         &intersected)) {
            char existing_text[128], new_text[128];
            (void)semver_constraint_format(&pending_existing->constraint, existing_text,
                                           sizeof existing_text);
            (void)semver_constraint_format(&constraint, new_text, sizeof new_text);
            (void)snprintf(error, error_size,
                "conflicting dependency '%s': '%s' wants %s, but '%s' wants %s "
                "-- no version can satisfy both",
                dependency->name, pending_existing->first_requester, existing_text,
                required_by, new_text);
            return false;
        }
        pending_existing->constraint = intersected;
        (void)snprintf(pending_existing->first_requester, sizeof pending_existing->first_requester,
                       "%s", required_by);
        return true;
    }
    if (pending->count == FACET_MAX_DEPENDENCIES) {
        (void)snprintf(error, error_size,
                       "too many pending dependencies (max %d)", FACET_MAX_DEPENDENCIES);
        return false;
    }
    FacetPendingSemver *new_entry = &pending->entries[pending->count++];
    (void)snprintf(new_entry->name, sizeof new_entry->name, "%s", dependency->name);
    (void)snprintf(new_entry->git, sizeof new_entry->git, "%s", dependency->git);
    new_entry->constraint = constraint;
    (void)snprintf(new_entry->first_requester, sizeof new_entry->first_requester, "%s", required_by);
    return true;
}

static bool resolve_manifest_dependencies(const FacetManifest *manifest,
    const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
    FacetWorkQueue *queue, const char *scratch_root, char *error, size_t error_size) {
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        const FacetDependency *dependency = &manifest->dependencies[index];
        if (dependency->uses_version) {
            if (!handle_version_dependency(dependency, required_by, resolution, pending,
                                           error, error_size)) {
                return false;
            }
        } else if (!handle_exact_dependency(dependency, required_by, resolution, pending,
                                            queue, scratch_root, error, error_size)) {
            return false;
        }
    }
    return true;
}

static bool process_work_queue(FacetWorkQueue *queue, FacetResolution *resolution,
        FacetPendingTable *pending, const char *scratch_root, char *error, size_t error_size) {
    while (queue->count > 0) {
        FacetWorkItem item = queue->items[0];
        memmove(&queue->items[0], &queue->items[1],
               (queue->count - 1) * sizeof queue->items[0]);
        queue->count--;
        FacetManifest nested;
        if (!parse_manifest(item.path, &nested, error, error_size)) return false;
        if (strcmp(nested.name, item.cut_name) != 0) {
            (void)snprintf(error, error_size, "'%s' declares name '%s', expected '%s'",
                           item.path, nested.name, item.cut_name);
            return false;
        }
        if (!resolve_manifest_dependencies(&nested, item.cut_name, resolution, pending,
                                           queue, scratch_root, error, error_size)) {
            return false;
        }
    }
    return true;
}

/* Resolves the single pending name at the front of the table: lists
 * its repository's own semver tags, picks the highest one satisfying
 * the (already fully intersected, as far as the graph explored so far
 * shows) accumulated constraint, clones it, and queues its own
 * diamond.cut if it has one. MAX_TAGS is a soft cap (a repository with
 * more published semver tags than that has its oldest-discovered ones
 * silently dropped from consideration) -- generous enough that this is
 * not a realistic concern for any real package. */
static bool resolve_one_pending(FacetPendingTable *pending, FacetResolution *resolution,
        FacetWorkQueue *queue, const char *scratch_root, char *error, size_t error_size) {
    FacetPendingSemver entry = pending->entries[0];
    memmove(&pending->entries[0], &pending->entries[1],
           (pending->count - 1) * sizeof pending->entries[0]);
    pending->count--;

    enum { MAX_TAGS = 4096 };
    char (*tags)[FACET_MAX_REF] = malloc((size_t)MAX_TAGS * sizeof *tags);
    if (tags == nullptr) {
        (void)snprintf(error, error_size, "out of memory listing tags for '%s'", entry.name);
        return false;
    }
    size_t tag_count = 0;
    bool ok = git_list_semver_tags(entry.git, tags, MAX_TAGS, &tag_count, error, error_size);
    char best_tag[FACET_MAX_REF] = {0};
    if (ok) {
        if (!pick_best_matching_tag(tags, tag_count, &entry.constraint, best_tag,
                                    sizeof best_tag)) {
            char range_text[128];
            (void)semver_constraint_format(&entry.constraint, range_text, sizeof range_text);
            (void)snprintf(error, error_size,
                "no tag on '%s' satisfies %s (required by '%s') for dependency '%s'",
                entry.git, range_text, entry.first_requester, entry.name);
            ok = false;
        }
    }
    free(tags);
    if (!ok) return false;

    if (resolution->count == FACET_MAX_DEPENDENCIES) {
        (void)snprintf(error, error_size,
                       "too many resolved dependencies (max %d)", FACET_MAX_DEPENDENCIES);
        return false;
    }
    char scratch_path[FACET_MAX_PATH];
    int written = snprintf(scratch_path, sizeof scratch_path, "%s/%s", scratch_root, entry.name);
    if (written < 0 || (size_t)written >= sizeof scratch_path) {
        (void)snprintf(error, error_size, "path too long while resolving '%s'", entry.name);
        return false;
    }
    if (!git_clone(entry.git, best_tag, scratch_path, error, error_size)) return false;
    char commit[FACET_MAX_COMMIT];
    if (!git_rev_parse_head(scratch_path, commit, sizeof commit, error, error_size)) return false;

    FacetResolved *resolved = &resolution->packages[resolution->count++];
    (void)snprintf(resolved->name, sizeof resolved->name, "%s", entry.name);
    (void)snprintf(resolved->git, sizeof resolved->git, "%s", entry.git);
    (void)snprintf(resolved->ref, sizeof resolved->ref, "%s", best_tag);
    (void)snprintf(resolved->commit, sizeof resolved->commit, "%s", commit);
    (void)snprintf(resolved->version, sizeof resolved->version, "%s", best_tag);
    (void)snprintf(resolved->required_by, sizeof resolved->required_by, "%s", entry.first_requester);

    char nested_manifest_path[FACET_MAX_PATH];
    written = snprintf(nested_manifest_path, sizeof nested_manifest_path,
                       "%s/diamond.cut", scratch_path);
    if (written < 0 || (size_t)written >= sizeof nested_manifest_path) {
        (void)snprintf(error, error_size, "path too long while resolving '%s'", entry.name);
        return false;
    }
    if (file_exists(nested_manifest_path)) {
        return work_queue_push(queue, nested_manifest_path, entry.name, error, error_size);
    }
    return true;
}

static bool resolve_full_graph(const FacetManifest *manifest, FacetResolution *resolution,
        const char *scratch_root, char *error, size_t error_size) {
    FacetPendingTable pending = {0};
    FacetWorkQueue queue = {0};
    if (!resolve_manifest_dependencies(manifest, manifest->name, resolution, &pending,
                                       &queue, scratch_root, error, error_size)) {
        return false;
    }
    if (!process_work_queue(&queue, resolution, &pending, scratch_root, error, error_size)) {
        return false;
    }
    while (pending.count > 0) {
        if (!resolve_one_pending(&pending, resolution, &queue, scratch_root, error, error_size)) {
            return false;
        }
        if (!process_work_queue(&queue, resolution, &pending, scratch_root, error, error_size)) {
            return false;
        }
    }
    return true;
}

/* --- install: move each resolved package's checkout into
 * cuts/<name>, stripping .git first - facet.lock, not a
 * live repo sitting inside cuts/, is the source of truth
 * for "what commit." The scratch root lives inside cuts/
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
        (void)snprintf(final_path, sizeof final_path, "cuts/%s",
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
    if (!file_exists("diamond.cut")) {
        fprintf(stderr, "facet: no diamond.cut found in the current directory\n");
        return 66;
    }
    const char *scratch_root = "cuts/.facet-tmp";
    if (!ensure_directory("cuts") || !ensure_directory(scratch_root)) {
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
        ok = parse_manifest("diamond.cut", &manifest, error, sizeof error) &&
             resolve_full_graph(&manifest, &resolution, scratch_root, error, sizeof error) &&
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
