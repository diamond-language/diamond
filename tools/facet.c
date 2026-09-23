#define _XOPEN_SOURCE 700
#include "semver.h"
#include "manifest_literal.h"

#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdint.h>

enum {
    FACET_MAX_NAME = 64,
    FACET_MAX_URL = 512,
    FACET_MAX_REF = 128,
    FACET_MAX_PATH = 4096,
    FACET_MAX_COMMIT = 80,
    FACET_MAX_DEPENDENCIES = 64,
    FACET_MAX_ARCHIVE_SIZE = 56 * 1024 * 1024,
};

typedef enum FacetSource {
    FACET_SOURCE_GIT,
    FACET_SOURCE_REGISTRY,
} FacetSource;

/* Which literal manifest key a dependency's ref came from -- needed (not
 * just for resolution, which treats tag/branch/commit identically via
 * `git checkout <ref>`) so `write_manifest` can round-trip an existing
 * dependency's spec exactly, rather than guessing "tag" for every
 * non-commit ref and silently turning a tracked `branch` dependency into
 * a pinned `tag` one on the next `facet add`. */
typedef enum FacetRefKind {
    FACET_REF_TAG,
    FACET_REF_BRANCH,
    FACET_REF_COMMIT,
} FacetRefKind;

typedef struct FacetDependency {
    char name[FACET_MAX_NAME];
    FacetSource source;
    char git[FACET_MAX_URL];
    char registry[FACET_MAX_URL];
    /* Exactly one of (ref, ref_kind) or (uses_version, version_text)
     * is meaningful, set at manifest-parse time by parse_dependencies --
     * see docs/roadmap.md's "Real semver dependency resolution". */
    char ref[FACET_MAX_REF];
    FacetRefKind ref_kind;
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
    FacetSource source;
    char git[FACET_MAX_URL];
    char registry[FACET_MAX_URL];
    char sha256[65];
    uint64_t size;
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
    FacetSource source;
    char git[FACET_MAX_URL];
    char registry[FACET_MAX_URL];
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

/* Real backtracking (docs/roadmap.md's own former "not attempted yet"
 * gap): the AND of every version constraint ever seen for this name,
 * across every resolve_full_graph *attempt* so far -- unlike
 * FacetPendingSemver.constraint above, this is never cleared when the
 * name resolves, and it survives a full graph-walk restart (see
 * resolve_full_graph's own comment). Seeding a name's very first
 * pending entry in a later attempt from this accumulated history is
 * what lets that attempt resolve it correctly the first time, instead
 * of repeating the same premature choice that forced the restart.
 * `most_recent_requester` mirrors FacetPendingSemver's own identical
 * simplification (error messages only, not full provenance). */
typedef struct FacetConstraintRecord {
    char name[FACET_MAX_NAME];
    FacetSource source;
    char git[FACET_MAX_URL];
    char registry[FACET_MAX_URL];
    SemverConstraint accumulated;
    char most_recent_requester[FACET_MAX_NAME];
} FacetConstraintRecord;

typedef struct FacetConstraintHistory {
    FacetConstraintRecord entries[FACET_MAX_DEPENDENCIES];
    size_t count;
} FacetConstraintHistory;

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

static bool parse_manifest(const char *path, FacetManifest *manifest,
                           char *error, size_t error_size);
static bool publishable_name(const char *name);
static int cmd_verify(int argc, char **argv);
static void tar_header(unsigned char header[512], const char *relative,
                       unsigned mode, unsigned long long size);
static bool tar_number(const unsigned char *field, size_t width,
                       unsigned long long *out);
static bool all_zero(const unsigned char *bytes, size_t count);
static bool valid_archive_path(const char *path);
static bool digest_file(const char *path, unsigned char digest[32],
                        char *error, size_t error_size);
static bool expected_digest(const char *text, const unsigned char actual[32]);
static bool run_curl_download(const char *url, const char *output, uint64_t max_size,
                              char *error, size_t error_size);
static bool resolve_manifest_dependencies(const FacetManifest *manifest,
    const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
    FacetWorkQueue *queue, const char *scratch_root, FacetConstraintHistory *history,
    bool *needs_restart, char *error, size_t error_size);

static bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

static bool valid_sha256(const char *text) {
    if (strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

static bool valid_registry_url(const char *url) {
    static const char prefix[] = "https://";
    if (strncmp(url, prefix, sizeof prefix - 1) != 0 ||
        url[sizeof prefix - 1] == '\0' || strchr(url, '?') != NULL ||
        strchr(url, '#') != NULL || strchr(url + sizeof prefix - 1, '@') != NULL)
        return false;
    const char *authority = url + sizeof prefix - 1;
    const char *slash = strchr(authority, '/');
    size_t authority_length = slash == NULL ? strlen(authority) : (size_t)(slash - authority);
    if (authority_length == 0 || (slash != NULL && slash[1] == '\0')) return false;
    for (const char *cursor = url; *cursor != '\0'; cursor++)
        if ((unsigned char)*cursor <= 0x20 || (unsigned char)*cursor >= 0x7f) return false;
    return true;
}

static bool canonical_registry_version(const char *text) {
    if (text[0] == 'v' || text[0] == 'V' || strchr(text, '+') != NULL) return false;
    Semver parsed;
    char formatted[FACET_MAX_REF];
    return semver_parse(text, &parsed) && semver_format(&parsed, formatted, sizeof formatted) &&
           strcmp(text, formatted) == 0;
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
    if (memchr(source, '\0', (size_t)size) != nullptr) {
        (void)snprintf(error, error_size, "'%s' contains a NUL byte", path);
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

/* Read manifest and lockfile metadata without compiling or executing it. */
static DiamondManifestValue *facet_read_hash(const char *path, char *error,
                                             size_t error_size) {
    char *source = read_whole_file(path, error, error_size);
    if (source == NULL) return NULL;
    char detail[160];
    DiamondManifestValue *hash = diamond_manifest_parse(source, detail, sizeof detail);
    free(source);
    if (hash == NULL)
        (void)snprintf(error, error_size, "'%s' must be data only: %s", path, detail);
    return hash;
}

static bool parse_dependencies(const DiamondManifestValue *dependencies,
                               FacetManifest *manifest, char *error,
                               size_t error_size) {
    for (const DiamondManifestValue *value = dependencies->children;
         value != NULL; value = value->next) {
        const char *name_string = value->key;
        if (value->kind != DIAMOND_MANIFEST_HASH) {
            (void)snprintf(error, error_size,
                           "dependency spec for '%s' must be a Hash", name_string);
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
        if (strlen(name_string) >= sizeof dependency->name) {
            (void)snprintf(error, error_size, "dependency name is too long");
            return false;
        }
        memcpy(dependency->name, name_string, strlen(name_string));
        dependency->name[strlen(name_string)] = '\0';
        if (!is_safe_field(dependency->name)) {
            (void)snprintf(error, error_size,
                           "dependency name '%s' contains an invalid character",
                           dependency->name);
            return false;
        }
        const DiamondManifestValue *spec = value;
        const bool has_git = diamond_manifest_get_string(spec, "git", dependency->git,
                                                          sizeof dependency->git);
        const bool has_registry = diamond_manifest_get_string(spec, "registry",
            dependency->registry, sizeof dependency->registry);
        if (has_git == has_registry) {
            (void)snprintf(error, error_size,
                           "dependency '%s' must specify exactly one of git or registry",
                           dependency->name);
            return false;
        }
        dependency->source = has_registry ? FACET_SOURCE_REGISTRY : FACET_SOURCE_GIT;
        if ((has_git && !is_safe_field(dependency->git)) ||
            (has_registry && !valid_registry_url(dependency->registry))) {
            (void)snprintf(error, error_size, "dependency '%s' has an invalid source",
                           dependency->name);
            return false;
        }
        int ref_keys = 0;
        FacetRefKind ref_kind = FACET_REF_TAG;
        if (diamond_manifest_get_string(spec, "tag", dependency->ref, sizeof dependency->ref)) {
            ref_keys++;
            ref_kind = FACET_REF_TAG;
        }
        if (diamond_manifest_get_string(spec, "branch", dependency->ref, sizeof dependency->ref)) {
            ref_keys++;
            ref_kind = FACET_REF_BRANCH;
        }
        if (diamond_manifest_get_string(spec, "commit", dependency->ref, sizeof dependency->ref)) {
            ref_keys++;
            ref_kind = FACET_REF_COMMIT;
        }
        const bool has_version = diamond_manifest_get_string(spec, "version", dependency->version_text,
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
        for (const DiamondManifestValue *field = spec->children;
             field != NULL; field = field->next) {
            bool allowed = strcmp(field->key, has_registry ? "registry" : "git") == 0 ||
                strcmp(field->key, "version") == 0 ||
                (!has_registry && (strcmp(field->key, "tag") == 0 ||
                                    strcmp(field->key, "branch") == 0 ||
                                    strcmp(field->key, "commit") == 0));
            if (!allowed) {
                (void)snprintf(error, error_size,
                    "dependency '%s' has unknown or mixed-source key '%s'",
                    dependency->name, field->key);
                return false;
            }
        }
        if (has_registry && (!has_version || ref_keys != 1)) {
            (void)snprintf(error, error_size,
                "registry dependency '%s' must specify exactly one version constraint",
                dependency->name);
            return false;
        }
        if (ref_keys != 1 || (!has_version && !is_safe_field(dependency->ref))) {
            (void)snprintf(error, error_size,
                "dependency '%s' must specify exactly one of tag/branch/commit/version",
                dependency->name);
            return false;
        }
        dependency->uses_version = has_version;
        dependency->ref_kind = ref_kind;
        if (has_version) dependency->ref[0] = '\0';
        manifest->dependency_count++;
    }
    return true;
}

static bool parse_manifest(const char *path, FacetManifest *manifest,
                           char *error, size_t error_size) {
    memset(manifest, 0, sizeof *manifest);
    DiamondManifestValue *hash = facet_read_hash(path, error, error_size);
    if (hash == NULL) return false;
    bool ok = true;
    if (!diamond_manifest_get_string(hash, "name", manifest->name,
                                     sizeof manifest->name) ||
        !is_safe_field(manifest->name)) {
        (void)snprintf(error, error_size,
                       "'%s' must have a valid String 'name' key", path);
        ok = false;
    }
    if (ok)
        manifest->has_version = diamond_manifest_get_string(hash, "version",
            manifest->version, sizeof manifest->version);
    const DiamondManifestValue *dependencies = diamond_manifest_get(hash, "dependencies");
    if (ok && dependencies != NULL) {
        if (dependencies->kind != DIAMOND_MANIFEST_HASH) {
            (void)snprintf(error, error_size,
                           "'%s' key 'dependencies' must be a Hash", path);
            ok = false;
        } else if (!parse_dependencies(dependencies, manifest, error, error_size)) {
            ok = false;
        }
    }
    diamond_manifest_free(hash);
    return ok;
}

static bool parse_lockfile(const char *path, FacetResolution *resolution,
                           char *error, size_t error_size) {
    resolution->count = 0;
    DiamondManifestValue *hash = facet_read_hash(path, error, error_size);
    if (hash == NULL) return false;
    bool ok = true;
    for (const DiamondManifestValue *entry = hash->children;
         ok && entry != NULL; entry = entry->next) {
        if (entry->kind != DIAMOND_MANIFEST_HASH) {
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
        const char *name_string = entry->key;
        if (strlen(name_string) >= sizeof resolved->name) {
            (void)snprintf(error, error_size, "package name in '%s' is too long", path);
            ok = false;
            break;
        }
        strcpy(resolved->name, name_string);
        const DiamondManifestValue *source = diamond_manifest_get(entry, "source");
        if (source != NULL && source->kind != DIAMOND_MANIFEST_STRING) {
            (void)snprintf(error, error_size, "'%s' entry '%s' has invalid source",
                           path, resolved->name);
            ok = false;
            break;
        }
        const bool registry = source != NULL && strcmp(source->string, "registry") == 0;
        if (source != NULL && !registry && strcmp(source->string, "git") != 0) {
            (void)snprintf(error, error_size, "'%s' entry '%s' has unsupported source",
                           path, resolved->name);
            ok = false;
            break;
        }
        resolved->source = registry ? FACET_SOURCE_REGISTRY : FACET_SOURCE_GIT;
        for (const DiamondManifestValue *field = entry->children;
             field != NULL; field = field->next) {
            const bool allowed = strcmp(field->key, "source") == 0 ||
                strcmp(field->key, "version") == 0 ||
                (!registry && (strcmp(field->key, "git") == 0 ||
                               strcmp(field->key, "commit") == 0)) ||
                (registry && (strcmp(field->key, "registry") == 0 ||
                              strcmp(field->key, "sha256") == 0 ||
                              strcmp(field->key, "size") == 0));
            if (!allowed) {
                (void)snprintf(error, error_size,
                    "'%s' entry '%s' has unknown key '%s'",
                    path, resolved->name, field->key);
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (registry) {
            if (!diamond_manifest_get_string(entry, "registry", resolved->registry,
                                              sizeof resolved->registry) ||
                !diamond_manifest_get_string(entry, "version", resolved->version,
                                              sizeof resolved->version) ||
                !diamond_manifest_get_string(entry, "sha256", resolved->sha256,
                                              sizeof resolved->sha256) ||
                !diamond_manifest_get_u64(entry, "size", &resolved->size) ||
                !publishable_name(resolved->name) ||
                !valid_registry_url(resolved->registry) ||
                !canonical_registry_version(resolved->version) ||
                !valid_sha256(resolved->sha256) || resolved->size == 0 ||
                resolved->size > FACET_MAX_ARCHIVE_SIZE) {
                (void)snprintf(error, error_size,
                    "'%s' entry '%s' has invalid registry fields", path, resolved->name);
                ok = false;
            }
            if (ok) resolution->count++;
            continue;
        }
        if (!diamond_manifest_get_string(entry, "git", resolved->git,
                                         sizeof resolved->git) ||
            !diamond_manifest_get_string(entry, "commit", resolved->commit,
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
        resolved->version[0] = '\0';
        const DiamondManifestValue *version = diamond_manifest_get(entry, "version");
        if (version != NULL &&
            (!diamond_manifest_get_string(entry, "version", resolved->version,
                                          sizeof resolved->version) ||
             !is_safe_field(resolved->version))) {
            (void)snprintf(error, error_size,
                "'%s' entry '%s' has invalid version", path, resolved->name);
            ok = false;
            break;
        }
        resolution->count++;
    }
    diamond_manifest_free(hash);
    return ok;
}

static bool write_lockfile(const char *path, const FacetResolution *resolution,
                           char *error, size_t error_size) {
    char temporary[FACET_MAX_PATH];
    if (snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path) >=
        (int)sizeof temporary) {
        (void)snprintf(error, error_size, "lockfile path is too long");
        return false;
    }
    int fd = mkstemp(temporary);
    FILE *file = fd < 0 ? NULL : fdopen(fd, "wb");
    if (file == nullptr) {
        if (fd >= 0) close(fd);
        (void)remove(temporary);
        (void)snprintf(error, error_size, "cannot create '%s': %s", path,
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
        if (resolved->source == FACET_SOURCE_REGISTRY) {
            fprintf(file, "\"%s\": {\"source\": \"registry\", \"registry\": \"%s\", \"version\": \"%s\", \"sha256\": \"%s\", \"size\": %llu}, ",
                    resolved->name, resolved->registry, resolved->version,
                    resolved->sha256, (unsigned long long)resolved->size);
        } else if (resolved->version[0] != '\0') {
            fprintf(file, "\"%s\": {\"source\": \"git\", \"git\": \"%s\", \"commit\": \"%s\", \"version\": \"%s\"}, ",
                    resolved->name, resolved->git, resolved->commit, resolved->version);
        } else {
            fprintf(file, "\"%s\": {\"source\": \"git\", \"git\": \"%s\", \"commit\": \"%s\"}, ",
                    resolved->name, resolved->git, resolved->commit);
        }
    }
    fputs("}\n", file);
    bool ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if (fclose(file) != 0) ok = false;
    if (ok && rename(temporary, path) != 0) ok = false;
    if (!ok) {
        int saved_errno = errno;
        (void)remove(temporary);
        (void)snprintf(error, error_size, "cannot write '%s': %s", path,
                       strerror(saved_errno));
    }
    return ok;
}

/* Writes `manifest` back out to `path` in a fixed, pretty-printed
 * canonical form -- used by `facet init`/`facet add` (write_lockfile's
 * own single-line style is deliberately not reused here: unlike
 * facet.lock, `diamond.cut` is meant to be hand-read, per docs/
 * packages.md's own "A manifest can be pretty-printed" note, which this
 * mirrors: a newline right after `{`, right after each `,`, and right
 * before `}`). This necessarily rewrites the *entire* file, not just the
 * dependency being added -- there is no Hash-literal-aware text editor
 * here, so any hand-added comment or unusual formatting in an existing
 * diamond.cut does not survive a `facet add`. Each individual dependency
 * spec (the `{"git": ..., "tag": ...}` part) stays on one line -- short
 * enough that breaking it up further would hurt readability rather than
 * help it. */
static bool write_manifest(const char *path, const FacetManifest *manifest,
                           char *error, size_t error_size) {
    FILE *file = fopen(path, "wb");
    if (file == nullptr) {
        (void)snprintf(error, error_size, "cannot write '%s': %s", path,
                       strerror(errno));
        return false;
    }
    fprintf(file, "{\n  \"name\": \"%s\"", manifest->name);
    if (manifest->has_version) {
        fprintf(file, ",\n  \"version\": \"%s\"", manifest->version);
    }
    if (manifest->dependency_count > 0) {
        fputs(",\n  \"dependencies\": {\n", file);
        for (size_t index = 0; index < manifest->dependency_count; index++) {
            const FacetDependency *dependency = &manifest->dependencies[index];
            fprintf(file, "    \"%s\": {\"%s\": \"%s\", ", dependency->name,
                    dependency->source == FACET_SOURCE_REGISTRY ? "registry" : "git",
                    dependency->source == FACET_SOURCE_REGISTRY ? dependency->registry :
                                                                  dependency->git);
            if (dependency->uses_version) {
                fprintf(file, "\"version\": \"%s\"}", dependency->version_text);
            } else {
                const char *ref_key = dependency->ref_kind == FACET_REF_BRANCH
                    ? "branch"
                    : dependency->ref_kind == FACET_REF_COMMIT ? "commit" : "tag";
                fprintf(file, "\"%s\": \"%s\"}", ref_key, dependency->ref);
            }
            fputs(index + 1 < manifest->dependency_count ? ",\n" : "\n", file);
        }
        fputs("  }\n", file);
    } else {
        fputs("\n", file);
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

/* Finds this name's constraint-history record, creating a fresh
 * (unconstrained-until-first-merge) one on first sighting -- mirrors
 * find_resolved/find_pending's own linear-scan-by-name shape. `git` is
 * only used to populate a freshly created record (a repeat sighting
 * already has it; a git-URL mismatch for the same name is handle_
 * version_dependency's own existing pending-table check to make, not
 * this function's). */
static bool find_or_create_constraint_record(FacetConstraintHistory *history,
        const FacetDependency *dependency, FacetConstraintRecord **out,
        char *error, size_t error_size) {
    for (size_t index = 0; index < history->count; index++) {
        if (strcmp(history->entries[index].name, dependency->name) == 0) {
            const bool same_source = history->entries[index].source == dependency->source &&
                (dependency->source == FACET_SOURCE_GIT
                    ? strcmp(history->entries[index].git, dependency->git) == 0
                    : strcmp(history->entries[index].registry, dependency->registry) == 0);
            if (!same_source) {
                (void)snprintf(error, error_size,
                    "conflicting sources for dependency '%s'", dependency->name);
                return false;
            }
            *out = &history->entries[index];
            return true;
        }
    }
    if (history->count == FACET_MAX_DEPENDENCIES) {
        (void)snprintf(error, error_size,
                       "too many distinct version-constrained dependencies (max %d)",
                       FACET_MAX_DEPENDENCIES);
        return false;
    }
    FacetConstraintRecord *record = &history->entries[history->count++];
    (void)snprintf(record->name, sizeof record->name, "%s", dependency->name);
    record->source = dependency->source;
    (void)snprintf(record->git, sizeof record->git, "%s", dependency->git);
    (void)snprintf(record->registry, sizeof record->registry, "%s", dependency->registry);
    *out = record;
    return true;
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

/* `history`/`needs_restart` are the real-backtracking mechanism
 * (docs/roadmap.md's own former "not attempted yet" gap; see resolve_
 * full_graph's own comment for the retry loop this feeds): every
 * version constraint ever seen for a name, across every graph-walk
 * attempt, is folded into `history` *before* any of the resolved/
 * pending branching below -- accumulating first means the one
 * genuinely unrecoverable case (two constraints whose ranges provably
 * never overlap, checked by semver_constraint_intersect's own pure
 * range math, independent of which tags actually exist) is caught
 * uniformly, regardless of whether this name happens to be already
 * resolved, already pending, or brand new. When a *resolved* name's
 * chosen version stops satisfying the newly-widened accumulated
 * constraint, that range math has already proven some version could
 * satisfy everyone -- just not the one already picked -- so this sets
 * `*needs_restart` instead of failing outright: the caller wipes the
 * scratch state and tries the whole graph again, this time with this
 * name's own pending entry seeded from the full accumulated history
 * from the very start (see the brand-new-pending-entry branch below),
 * so it resolves correctly the first time that attempt reaches it. */
static bool handle_version_dependency(const FacetDependency *dependency,
        const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
        FacetConstraintHistory *history, bool *needs_restart,
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
    FacetConstraintRecord *record;
    if (!find_or_create_constraint_record(history, dependency, &record, error, error_size)) {
        return false;
    }
    SemverConstraint merged;
    if (!semver_constraint_intersect(&record->accumulated, &constraint, &merged)) {
        char existing_text[128], new_text[128];
        (void)semver_constraint_format(&record->accumulated, existing_text,
                                       sizeof existing_text);
        (void)semver_constraint_format(&constraint, new_text, sizeof new_text);
        (void)snprintf(error, error_size,
            "conflicting dependency '%s': '%s' wants %s, but '%s' wants %s "
            "-- no version can satisfy both",
            dependency->name, record->most_recent_requester, existing_text,
            required_by, new_text);
        return false;
    }
    record->accumulated = merged;
    (void)snprintf(record->most_recent_requester, sizeof record->most_recent_requester,
                   "%s", required_by);

    FacetResolved *existing;
    if (find_resolved(resolution, dependency->name, &existing)) {
        const bool same_source = existing->source == dependency->source &&
            (dependency->source == FACET_SOURCE_GIT
                ? strcmp(existing->git, dependency->git) == 0
                : strcmp(existing->registry, dependency->registry) == 0);
        if (!same_source) {
            (void)snprintf(error, error_size,
                "conflicting sources for dependency '%s'", dependency->name);
            return false;
        }
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
            semver_satisfies(&resolved_version, &record->accumulated)) {
            return true; /* already-resolved version also satisfies this one */
        }
        *needs_restart = true;
        return false;
    }
    FacetPendingSemver *pending_existing;
    if (find_pending(pending, dependency->name, &pending_existing)) {
        const bool same_source = pending_existing->source == dependency->source &&
            (dependency->source == FACET_SOURCE_GIT
                ? strcmp(pending_existing->git, dependency->git) == 0
                : strcmp(pending_existing->registry, dependency->registry) == 0);
        if (!same_source) {
            (void)snprintf(error, error_size,
                "conflicting dependency '%s': '%s' and '%s' point at different "
                "git repositories ('%s' vs '%s')",
                dependency->name, required_by, pending_existing->first_requester,
                dependency->git, pending_existing->git);
            return false;
        }
        /* Already merged into `record->accumulated` above -- no separate
         * intersection needed here. */
        pending_existing->constraint = record->accumulated;
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
    new_entry->source = dependency->source;
    (void)snprintf(new_entry->git, sizeof new_entry->git, "%s", dependency->git);
    (void)snprintf(new_entry->registry, sizeof new_entry->registry, "%s",
                   dependency->registry);
    /* Seeded from the full cross-attempt history, not just this one
     * sighting's own constraint -- see this function's own top comment. */
    new_entry->constraint = record->accumulated;
    (void)snprintf(new_entry->first_requester, sizeof new_entry->first_requester, "%s", required_by);
    return true;
}

static bool resolve_manifest_dependencies(const FacetManifest *manifest,
    const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
    FacetWorkQueue *queue, const char *scratch_root, FacetConstraintHistory *history,
    bool *needs_restart, char *error, size_t error_size) {
    for (size_t index = 0; index < manifest->dependency_count; index++) {
        const FacetDependency *dependency = &manifest->dependencies[index];
        if (dependency->uses_version) {
            if (!handle_version_dependency(dependency, required_by, resolution, pending,
                                           history, needs_restart, error, error_size)) {
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
        FacetPendingTable *pending, const char *scratch_root, FacetConstraintHistory *history,
        bool *needs_restart, char *error, size_t error_size) {
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
                                           queue, scratch_root, history, needs_restart,
                                           error, error_size)) {
            return false;
        }
    }
    return true;
}

static bool only_keys(const DiamondManifestValue *hash, const char *const keys[],
                      size_t key_count, char *error, size_t error_size) {
    for (const DiamondManifestValue *field = hash->children;
         field != NULL; field = field->next) {
        bool known = false;
        for (size_t index = 0; index < key_count; index++)
            if (strcmp(field->key, keys[index]) == 0) known = true;
        if (!known) {
            (void)snprintf(error, error_size, "registry metadata has unknown key '%s'",
                           field->key);
            return false;
        }
    }
    return true;
}

static DiamondManifestValue *fetch_registry_hash(const char *url, const char *path,
        char *error, size_t error_size) {
    (void)remove(path);
    if (!run_curl_download(url, path, 1024 * 1024, error, error_size)) return NULL;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size > 1024 * 1024) {
        (void)remove(path);
        (void)snprintf(error, error_size, "registry metadata has an invalid size");
        return NULL;
    }
    DiamondManifestValue *hash = facet_read_hash(path, error, error_size);
    (void)remove(path);
    return hash;
}

static bool registry_best_version(const FacetPendingSemver *entry,
        const char *scratch_root, char *out, size_t out_size,
        char *error, size_t error_size) {
    char url[FACET_MAX_PATH], path[FACET_MAX_PATH];
    if (snprintf(url, sizeof url, "%s/v1/cuts/%s/versions", entry->registry,
                 entry->name) >= (int)sizeof url ||
        snprintf(path, sizeof path, "%s/.index-%s.json", scratch_root,
                 entry->name) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "registry metadata path is too long");
        return false;
    }
    DiamondManifestValue *root = fetch_registry_hash(url, path, error, error_size);
    if (root == NULL) return false;
    const char *root_keys[] = {"protocol", "versions"};
    uint64_t protocol = 0;
    const DiamondManifestValue *versions = diamond_manifest_get(root, "versions");
    bool ok = diamond_manifest_get_u64(root, "protocol", &protocol) && protocol == 1 &&
        versions != NULL && versions->kind == DIAMOND_MANIFEST_ARRAY &&
        only_keys(root, root_keys, 2, error, error_size);
    bool found = false;
    Semver best = {0};
    if (ok) {
        const char *record_keys[] = {"version", "yanked"};
        for (const DiamondManifestValue *record = versions->children;
             record != NULL; record = record->next) {
            char version[FACET_MAX_REF];
            bool yanked;
            Semver candidate;
            if (record->kind != DIAMOND_MANIFEST_HASH ||
                !only_keys(record, record_keys, 2, error, error_size) ||
                !diamond_manifest_get_string(record, "version", version, sizeof version) ||
                !diamond_manifest_get_bool(record, "yanked", &yanked) ||
                !canonical_registry_version(version) ||
                !semver_parse(version, &candidate)) {
                (void)snprintf(error, error_size,
                               "registry returned an invalid version record for '%s'",
                               entry->name);
                ok = false;
                break;
            }
            if (!yanked && semver_satisfies(&candidate, &entry->constraint) &&
                (!found || semver_compare(&candidate, &best) > 0)) {
                found = true;
                best = candidate;
                (void)snprintf(out, out_size, "%s", version);
            }
        }
    }
    diamond_manifest_free(root);
    if (ok && !found) {
        char range[128];
        (void)semver_constraint_format(&entry->constraint, range, sizeof range);
        (void)snprintf(error, error_size,
            "no registry version of '%s' satisfies %s", entry->name, range);
        ok = false;
    }
    return ok;
}

static bool registry_release(const FacetPendingSemver *entry, const char *version,
        const char *scratch_root, FacetResolved *resolved, FacetManifest *nested,
        char *error, size_t error_size) {
    char url[FACET_MAX_PATH], path[FACET_MAX_PATH];
    if (snprintf(url, sizeof url, "%s/v1/cuts/%s/versions/%s", entry->registry,
                 entry->name, version) >= (int)sizeof url ||
        snprintf(path, sizeof path, "%s/.release-%s.json", scratch_root,
                 entry->name) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "registry release path is too long");
        return false;
    }
    DiamondManifestValue *root = fetch_registry_hash(url, path, error, error_size);
    if (root == NULL) return false;
    const char *root_keys[] = {"protocol", "name", "version", "dependencies",
                               "yanked", "archive"};
    uint64_t protocol = 0;
    char name[FACET_MAX_NAME], returned_version[FACET_MAX_REF];
    bool yanked = true;
    const DiamondManifestValue *dependencies = diamond_manifest_get(root, "dependencies");
    const DiamondManifestValue *archive = diamond_manifest_get(root, "archive");
    bool ok = only_keys(root, root_keys, 6, error, error_size) &&
        diamond_manifest_get_u64(root, "protocol", &protocol) && protocol == 1 &&
        diamond_manifest_get_string(root, "name", name, sizeof name) &&
        diamond_manifest_get_string(root, "version", returned_version,
                                    sizeof returned_version) &&
        diamond_manifest_get_bool(root, "yanked", &yanked) && !yanked &&
        strcmp(name, entry->name) == 0 && strcmp(returned_version, version) == 0 &&
        dependencies != NULL && dependencies->kind == DIAMOND_MANIFEST_HASH &&
        archive != NULL && archive->kind == DIAMOND_MANIFEST_HASH;
    const char *archive_keys[] = {"path", "sha256", "size"};
    char archive_path[FACET_MAX_PATH];
    if (ok) {
        ok = only_keys(archive, archive_keys, 3, error, error_size) &&
            diamond_manifest_get_string(archive, "path", archive_path,
                                        sizeof archive_path) &&
            diamond_manifest_get_string(archive, "sha256", resolved->sha256,
                                        sizeof resolved->sha256) &&
            diamond_manifest_get_u64(archive, "size", &resolved->size) &&
            valid_sha256(resolved->sha256) && resolved->size > 0 &&
            resolved->size <= FACET_MAX_ARCHIVE_SIZE;
        char expected_path[96];
        (void)snprintf(expected_path, sizeof expected_path,
                       "/v1/blobs/sha256/%s", resolved->sha256);
        if (ok && strcmp(archive_path, expected_path) != 0) ok = false;
    }
    memset(nested, 0, sizeof *nested);
    if (ok) {
        (void)snprintf(nested->name, sizeof nested->name, "%s", entry->name);
        for (const DiamondManifestValue *dependency = dependencies->children;
             dependency != NULL; dependency = dependency->next) {
            if (nested->dependency_count == FACET_MAX_DEPENDENCIES ||
                !publishable_name(dependency->key) ||
                dependency->kind != DIAMOND_MANIFEST_STRING ||
                strlen(dependency->string) >= FACET_MAX_REF) {
                ok = false;
                break;
            }
            FacetDependency *item = &nested->dependencies[nested->dependency_count++];
            (void)snprintf(item->name, sizeof item->name, "%s", dependency->key);
            item->source = FACET_SOURCE_REGISTRY;
            (void)snprintf(item->registry, sizeof item->registry, "%s", entry->registry);
            (void)snprintf(item->version_text, sizeof item->version_text, "%s",
                           dependency->string);
            SemverConstraint probe;
            if (!semver_constraint_parse(item->version_text, &probe)) {
                ok = false;
                break;
            }
            item->uses_version = true;
        }
    }
    if (ok) {
        resolved->source = FACET_SOURCE_REGISTRY;
        (void)snprintf(resolved->name, sizeof resolved->name, "%s", entry->name);
        (void)snprintf(resolved->registry, sizeof resolved->registry, "%s",
                       entry->registry);
        (void)snprintf(resolved->version, sizeof resolved->version, "%s", version);
        (void)snprintf(resolved->required_by, sizeof resolved->required_by, "%s",
                       entry->first_requester);
    } else if (error[0] == '\0') {
        (void)snprintf(error, error_size, "registry returned an invalid release for '%s'",
                       entry->name);
    }
    diamond_manifest_free(root);
    return ok;
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
        FacetWorkQueue *queue, const char *scratch_root, FacetConstraintHistory *history,
        bool *needs_restart, char *error, size_t error_size) {
    FacetPendingSemver entry = pending->entries[0];
    memmove(&pending->entries[0], &pending->entries[1],
           (pending->count - 1) * sizeof pending->entries[0]);
    pending->count--;

    if (entry.source == FACET_SOURCE_REGISTRY) {
        char best_version[FACET_MAX_REF];
        if (!registry_best_version(&entry, scratch_root, best_version,
                                   sizeof best_version, error, error_size)) return false;
        if (resolution->count == FACET_MAX_DEPENDENCIES) {
            (void)snprintf(error, error_size,
                           "too many resolved dependencies (max %d)",
                           FACET_MAX_DEPENDENCIES);
            return false;
        }
        FacetResolved *resolved = &resolution->packages[resolution->count];
        FacetManifest nested;
        if (!registry_release(&entry, best_version, scratch_root, resolved, &nested,
                              error, error_size)) return false;
        resolution->count++;
        return resolve_manifest_dependencies(&nested, entry.name, resolution, pending,
            queue, scratch_root, history, needs_restart, error, error_size);
    }

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

/* One graph-walk attempt -- see resolve_full_graph's own comment below
 * for the retry loop this feeds and what `history`/`needs_restart` are
 * for. Otherwise unchanged from before real backtracking existed:
 * classify the root manifest's own dependencies, fully drain whatever
 * that queues before ever resolving a pending name (so exact-ref work
 * never waits on a version-constrained sibling), then alternate
 * resolving exactly one pending name and re-draining the queue until
 * both are empty. */
static bool attempt_resolve_graph(const FacetManifest *manifest, FacetResolution *resolution,
        const char *scratch_root, FacetConstraintHistory *history, bool *needs_restart,
        char *error, size_t error_size) {
    FacetPendingTable pending = {0};
    FacetWorkQueue queue = {0};
    if (!resolve_manifest_dependencies(manifest, manifest->name, resolution, &pending,
                                       &queue, scratch_root, history, needs_restart,
                                       error, error_size)) {
        return false;
    }
    if (!process_work_queue(&queue, resolution, &pending, scratch_root, history,
                            needs_restart, error, error_size)) {
        return false;
    }
    while (pending.count > 0) {
        if (!resolve_one_pending(&pending, resolution, &queue, scratch_root, history,
                                 needs_restart, error, error_size)) {
            return false;
        }
        if (!process_work_queue(&queue, resolution, &pending, scratch_root, history,
                                needs_restart, error, error_size)) {
            return false;
        }
    }
    return true;
}

/* Real backtracking (docs/roadmap.md's own former "not attempted yet"
 * gap): attempt_resolve_graph's only way to signal "an already-resolved
 * name's chosen version stopped satisfying a later-discovered
 * constraint, but some version could still satisfy everyone" is
 * `needs_restart` (see handle_version_dependency's own comment for
 * exactly when that fires and why it's provably not a dead end). On
 * that signal, wipe the ephemeral scratch clones and try the whole
 * graph again -- `history` (never reset, unlike `resolution`/the
 * pending table/queue, which start fresh every attempt) is what makes
 * the next attempt actually converge instead of repeating the same
 * premature choice: by the time a name is first seen in a later
 * attempt, `history` already reflects every constraint discovered
 * about it in every earlier attempt, however late.
 *
 * The `FACET_MAX_DEPENDENCIES` attempt bound is a real, provable limit,
 * not an arbitrary guess: each restart is caused by discovering a
 * genuinely new (name, conflicting-constraint) fact that didn't exist
 * in `history` before that exact attempt, and there are at most that
 * many version-constrained dependency edges in the whole graph -- so
 * exhausting the bound means a resolver bug (a restart loop that isn't
 * actually converging), not a hard-to-satisfy manifest; those still
 * fail fast, on the very first attempt, via the range-intersection
 * check in handle_version_dependency itself. */
static bool resolve_full_graph(const FacetManifest *manifest, FacetResolution *resolution,
        const char *scratch_root, char *error, size_t error_size) {
    FacetConstraintHistory history = {0};
    for (size_t attempt = 0; attempt < FACET_MAX_DEPENDENCIES; attempt++) {
        *resolution = (FacetResolution){0};
        if (attempt > 0) {
            (void)remove_directory_recursive(scratch_root);
            if (!ensure_directory(scratch_root)) {
                (void)snprintf(error, error_size,
                    "cannot recreate '%s' while retrying dependency resolution: %s",
                    scratch_root, strerror(errno));
                return false;
            }
        }
        bool needs_restart = false;
        if (attempt_resolve_graph(manifest, resolution, scratch_root, &history,
                                  &needs_restart, error, error_size)) {
            return true;
        }
        if (!needs_restart) return false; /* a real error; `error` is already set */
    }
    (void)snprintf(error, error_size,
        "dependency resolution did not converge after %d attempts (internal error)",
        FACET_MAX_DEPENDENCIES);
    return false;
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

static bool run_curl_download(const char *url, const char *output, uint64_t max_size,
                              char *error, size_t error_size) {
    char size_text[32];
    (void)snprintf(size_text, sizeof size_text, "%llu",
                   (unsigned long long)max_size);
    char *const argv[] = {(char *)"curl", (char *)"--fail", (char *)"--silent",
        (char *)"--show-error", (char *)"--proto", (char *)"=https",
        (char *)"--connect-timeout", (char *)"10", (char *)"--max-time",
        (char *)"60", (char *)"--retry", (char *)"2", (char *)"--retry-delay",
        (char *)"1", (char *)"--max-filesize", size_text, (char *)"--output",
        (char *)output, (char *)"--", (char *)url, nullptr};
    const pid_t pid = fork();
    if (pid < 0) {
        (void)snprintf(error, error_size, "cannot start curl: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp("curl", argv);
        fprintf(stderr, "facet: cannot execute 'curl': %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        (void)snprintf(error, error_size, "cannot download '%s'", url);
        return false;
    }
    return true;
}

static bool read_hashed(FILE *file, EVP_MD_CTX *digest, void *buffer, size_t size) {
    return fread(buffer, 1, size, file) == size &&
           EVP_DigestUpdate(digest, buffer, size) == 1;
}

static bool extract_verified_archive(const char *archive_path, const char *destination,
                                     const char *expected_sha256,
                                     char *error, size_t error_size) {
    FILE *archive = fopen(archive_path, "rb");
    if (archive == NULL || !ensure_directory(destination)) {
        if (archive != NULL) fclose(archive);
        (void)snprintf(error, error_size, "cannot prepare archive extraction");
        return false;
    }
    struct stat before, after;
    EVP_MD_CTX *digest_context = EVP_MD_CTX_new();
    bool ok = digest_context != NULL &&
              EVP_DigestInit_ex(digest_context, EVP_sha256(), NULL) == 1 &&
              fstat(fileno(archive), &before) == 0 && S_ISREG(before.st_mode);
    bool ended = false;
    size_t file_count = 0;
    unsigned long long total_bytes = 0;
    while (ok && !ended) {
        unsigned char header[512];
        if (!read_hashed(archive, digest_context, header, sizeof header)) {
            ok = false;
            break;
        }
        if (all_zero(header, sizeof header)) {
            unsigned char second[512];
            ok = read_hashed(archive, digest_context, second, sizeof second) &&
                 all_zero(second, sizeof second) && fgetc(archive) == EOF &&
                 !ferror(archive);
            ended = true;
            break;
        }
        const unsigned char *end = memchr(header, '\0', 100);
        unsigned long long mode, size;
        char relative[101];
        if (end == NULL || end == header || !tar_number(header + 100, 8, &mode) ||
            !tar_number(header + 124, 12, &size) ||
            (mode != 0644 && mode != 0755) || size > 10ULL * 1024 * 1024 ||
            file_count == 4096 || total_bytes + size > 50ULL * 1024 * 1024) {
            ok = false;
            break;
        }
        file_count++;
        total_bytes += size;
        size_t length = (size_t)(end - header);
        memcpy(relative, header, length);
        relative[length] = '\0';
        unsigned char canonical[512];
        tar_header(canonical, relative, (unsigned)mode, size);
        if (!valid_archive_path(relative) || memcmp(header, canonical, sizeof header) != 0) {
            ok = false;
            break;
        }
        char output[FACET_MAX_PATH];
        if (snprintf(output, sizeof output, "%s/%s", destination, relative) >=
            (int)sizeof output) {
            ok = false;
            break;
        }
        char parent[FACET_MAX_PATH];
        (void)snprintf(parent, sizeof parent, "%s", output);
        char *slash = strrchr(parent, '/');
        if (slash == NULL) { ok = false; break; }
        *slash = '\0';
        if (!ensure_directory(parent)) { ok = false; break; }
        int fd = open(output, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                      (mode_t)mode);
        if (fd < 0) { ok = false; break; }
        unsigned char buffer[8192];
        unsigned long long remaining = size;
        while (remaining > 0) {
            size_t amount = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
            if (!read_hashed(archive, digest_context, buffer, amount)) {
                ok = false;
                break;
            }
            size_t written = 0;
            while (written < amount) {
                ssize_t count = write(fd, buffer + written, amount - written);
                if (count <= 0) { ok = false; break; }
                written += (size_t)count;
            }
            if (!ok) break;
            remaining -= amount;
        }
        if (close(fd) != 0) ok = false;
        unsigned char padding[512];
        size_t pad = (size_t)((512 - size % 512) % 512);
        if (ok && pad > 0 && (!read_hashed(archive, digest_context, padding, pad) ||
                              !all_zero(padding, pad))) ok = false;
    }
    unsigned char extracted_digest[32];
    unsigned digest_length = 0;
    if (!ok || EVP_DigestFinal_ex(digest_context, extracted_digest, &digest_length) != 1 ||
        digest_length != 32 || !expected_digest(expected_sha256, extracted_digest) ||
        fstat(fileno(archive), &after) != 0 || before.st_size != after.st_size ||
        before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) ok = false;
    EVP_MD_CTX_free(digest_context);
    if (fclose(archive) != 0) ok = false;
    if (!ok) {
        (void)remove_directory_recursive(destination);
        (void)snprintf(error, error_size, "cannot extract verified archive");
    }
    return ok;
}

static bool prepare_registry_package(const FacetResolved *resolved,
        const char *scratch_root, char *error, size_t error_size) {
    char url[FACET_MAX_PATH], archive[FACET_MAX_PATH], destination[FACET_MAX_PATH];
    if (snprintf(url, sizeof url, "%s/v1/blobs/sha256/%s", resolved->registry,
                 resolved->sha256) >= (int)sizeof url ||
        snprintf(archive, sizeof archive, "%s/.%s.tar", scratch_root,
                 resolved->name) >= (int)sizeof archive ||
        snprintf(destination, sizeof destination, "%s/%s", scratch_root,
                 resolved->name) >= (int)sizeof destination) {
        (void)snprintf(error, error_size, "registry path is too long for '%s'",
                       resolved->name);
        return false;
    }
    (void)remove(archive);
    if (!run_curl_download(url, archive, resolved->size, error, error_size)) {
        (void)remove(archive);
        return false;
    }
    struct stat info;
    unsigned char digest[32];
    if (stat(archive, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size != resolved->size ||
        !digest_file(archive, digest, error, error_size) ||
        !expected_digest(resolved->sha256, digest)) {
        (void)remove(archive);
        (void)snprintf(error, error_size,
            "downloaded archive for '%s' does not match its lock record", resolved->name);
        return false;
    }
    char *verify_argv[] = {(char *)"facet", (char *)"verify", archive,
                           (char *)"--sha256", (char *)resolved->sha256, nullptr};
    (void)remove_directory_recursive(destination);
    if (cmd_verify(5, verify_argv) != 0 ||
        !extract_verified_archive(archive, destination, resolved->sha256,
                                  error, error_size)) {
        (void)remove(archive);
        if (error[0] == '\0')
            (void)snprintf(error, error_size, "archive verification failed for '%s'",
                           resolved->name);
        return false;
    }
    (void)remove(archive);
    char manifest_path[FACET_MAX_PATH];
    FacetManifest manifest;
    if (snprintf(manifest_path, sizeof manifest_path, "%s/diamond.cut", destination) >=
            (int)sizeof manifest_path ||
        !parse_manifest(manifest_path, &manifest, error, error_size) ||
        strcmp(manifest.name, resolved->name) != 0 || !manifest.has_version ||
        strcmp(manifest.version, resolved->version) != 0) {
        (void)remove_directory_recursive(destination);
        (void)snprintf(error, error_size,
            "archive identity does not match lock record for '%s'", resolved->name);
        return false;
    }
    return true;
}

static bool install_resolution(const FacetResolution *resolution,
                               const char *scratch_root, char *error,
                               size_t error_size) {
    bool had_previous[FACET_MAX_DEPENDENCIES] = {0};
    for (size_t index = 0; index < resolution->count; index++) {
        const FacetResolved *resolved = &resolution->packages[index];
        char scratch_path[FACET_MAX_PATH];
        (void)snprintf(scratch_path, sizeof scratch_path, "%s/%s", scratch_root,
                       resolved->name);
        if (!file_exists(scratch_path)) {
            /* facet.lock-driven install: resolution didn't clone anything
             * (it skipped the walk entirely), so clone the pinned commit now. */
            if (resolved->source != FACET_SOURCE_GIT) {
                (void)snprintf(error, error_size,
                               "registry archive for '%s' was not staged",
                               resolved->name);
                return false;
            }
            if (!git_clone(resolved->git, resolved->commit, scratch_path, error,
                           error_size)) {
                return false;
            }
        }
        if (resolved->source == FACET_SOURCE_GIT && !strip_git_directory(scratch_path)) {
            (void)snprintf(error, error_size, "cannot remove '.git' from '%s'",
                           scratch_path);
            return false;
        }
    }
    size_t backed_up = 0;
    for (; backed_up < resolution->count; backed_up++) {
        const FacetResolved *resolved = &resolution->packages[backed_up];
        char final_path[FACET_MAX_PATH], backup_path[FACET_MAX_PATH];
        (void)snprintf(final_path, sizeof final_path, "cuts/%s", resolved->name);
        (void)snprintf(backup_path, sizeof backup_path, "cuts/.facet-old-%s",
                       resolved->name);
        if (!remove_directory_recursive(backup_path)) {
            (void)snprintf(error, error_size, "cannot clear install backup for '%s'",
                           resolved->name);
            break;
        }
        if (file_exists(final_path)) {
            if (rename(final_path, backup_path) != 0) {
                (void)snprintf(error, error_size, "cannot stage installed '%s': %s",
                               resolved->name, strerror(errno));
                break;
            }
            had_previous[backed_up] = true;
        }
    }
    if (backed_up != resolution->count) {
        for (size_t index = 0; index < backed_up; index++) {
            if (!had_previous[index]) continue;
            char final_path[FACET_MAX_PATH], backup_path[FACET_MAX_PATH];
            (void)snprintf(final_path, sizeof final_path, "cuts/%s",
                           resolution->packages[index].name);
            (void)snprintf(backup_path, sizeof backup_path, "cuts/.facet-old-%s",
                           resolution->packages[index].name);
            (void)rename(backup_path, final_path);
        }
        return false;
    }
    size_t installed = 0;
    for (; installed < resolution->count; installed++) {
        const FacetResolved *resolved = &resolution->packages[installed];
        char scratch_path[FACET_MAX_PATH], final_path[FACET_MAX_PATH];
        (void)snprintf(scratch_path, sizeof scratch_path, "%s/%s", scratch_root,
                       resolved->name);
        (void)snprintf(final_path, sizeof final_path, "cuts/%s", resolved->name);
        if (rename(scratch_path, final_path) != 0) {
            (void)snprintf(error, error_size, "cannot install '%s': %s",
                           resolved->name, strerror(errno));
            break;
        }
    }
    if (installed != resolution->count) {
        for (size_t index = 0; index < installed; index++) {
            char final_path[FACET_MAX_PATH];
            (void)snprintf(final_path, sizeof final_path, "cuts/%s",
                           resolution->packages[index].name);
            (void)remove_directory_recursive(final_path);
        }
        for (size_t index = 0; index < resolution->count; index++) {
            if (!had_previous[index]) continue;
            char final_path[FACET_MAX_PATH], backup_path[FACET_MAX_PATH];
            (void)snprintf(final_path, sizeof final_path, "cuts/%s",
                           resolution->packages[index].name);
            (void)snprintf(backup_path, sizeof backup_path, "cuts/.facet-old-%s",
                           resolution->packages[index].name);
            (void)rename(backup_path, final_path);
        }
        return false;
    }
    for (size_t index = 0; index < resolution->count; index++) {
        const FacetResolved *resolved = &resolution->packages[index];
        if (had_previous[index]) {
            char backup_path[FACET_MAX_PATH];
            (void)snprintf(backup_path, sizeof backup_path, "cuts/.facet-old-%s",
                           resolved->name);
            if (!remove_directory_recursive(backup_path))
                fprintf(stderr, "facet: warning: cannot remove install backup for '%s'\n",
                        resolved->name);
        }
        printf("facet: installed %s (%s)\n", resolved->name,
               resolved->source == FACET_SOURCE_REGISTRY ? resolved->version : resolved->commit);
    }
    return true;
}

static int run_install_or_update(bool force_resolve) {
    char error[512] = {0};
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
    for (size_t index = 0; ok && index < resolution.count; index++) {
        if (resolution.packages[index].source == FACET_SOURCE_REGISTRY)
            ok = prepare_registry_package(&resolution.packages[index], scratch_root,
                                          error, sizeof error);
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

/* Derives a default `facet init` name from the current directory's own
 * basename -- e.g. running it inside `~/projects/greeter` defaults to
 * "greeter", the same convention `cargo init`/`npm init -y` already use.
 * Returns false only if the working directory itself can't be read
 * (never for "/" specifically -- see the fallback below). */
static bool basename_of_cwd(char *out, size_t out_size) {
    char cwd[FACET_MAX_PATH];
    if (getcwd(cwd, sizeof cwd) == nullptr) return false;
    const char *slash = strrchr(cwd, '/');
    const char *base = slash != nullptr ? slash + 1 : cwd;
    if (base[0] == '\0') base = "cut"; /* cwd was "/" itself */
    return snprintf(out, out_size, "%s", base) < (int)out_size;
}

static int cmd_init(int argc, char **argv) {
    if (argc > 3) {
        fputs("usage: facet init [name]\n", stderr);
        return 64;
    }
    if (file_exists("diamond.cut")) {
        fprintf(stderr,
                "facet: 'diamond.cut' already exists in the current directory\n");
        return 65;
    }
    FacetManifest manifest = {0};
    if (argc == 3) {
        if (strlen(argv[2]) >= sizeof manifest.name) {
            fprintf(stderr, "facet: name '%s' is too long\n", argv[2]);
            return 64;
        }
        (void)snprintf(manifest.name, sizeof manifest.name, "%s", argv[2]);
    } else if (!basename_of_cwd(manifest.name, sizeof manifest.name)) {
        fprintf(stderr,
                "facet: cannot determine a default name from the current "
                "directory; pass one explicitly (facet init <name>)\n");
        return 74;
    }
    if (manifest.name[0] == '\0' || !is_safe_field(manifest.name)) {
        fprintf(stderr, "facet: '%s' is not a valid cut name\n", manifest.name);
        return 64;
    }
    char error[512];
    if (!write_manifest("diamond.cut", &manifest, error, sizeof error)) {
        fprintf(stderr, "facet: %s\n", error);
        return 70;
    }
    printf("facet: wrote diamond.cut (name: %s)\n", manifest.name);
    return 0;
}

static int cmd_add(int argc, char **argv) {
    if (argc < 4) {
        fputs("usage: facet add <name> (--git <url> "
              "(--tag <ref> | --branch <ref> | --commit <ref> | --version <constraint>) "
              "| --registry <url> --version <constraint>)\n",
              stderr);
        return 64;
    }
    const char *name = argv[2];
    const char *git = nullptr;
    const char *registry = nullptr;
    const char *tag = nullptr, *branch = nullptr, *commit = nullptr, *version = nullptr;
    for (int index = 3; index < argc; index++) {
        const char *flag = argv[index];
        const char **slot = strcmp(flag, "--git") == 0 ? &git
            : strcmp(flag, "--registry") == 0 ? &registry
            : strcmp(flag, "--tag") == 0 ? &tag
            : strcmp(flag, "--branch") == 0 ? &branch
            : strcmp(flag, "--commit") == 0 ? &commit
            : strcmp(flag, "--version") == 0 ? &version
            : nullptr;
        if (slot == nullptr) {
            fprintf(stderr, "facet: unrecognized option '%s'\n", flag);
            return 64;
        }
        if (index + 1 >= argc) {
            fprintf(stderr, "facet: '%s' requires a value\n", flag);
            return 64;
        }
        *slot = argv[++index];
    }
    if ((git == nullptr) == (registry == nullptr)) {
        fprintf(stderr, "facet: specify exactly one of --git or --registry\n");
        return 64;
    }
    const int ref_count = (tag != nullptr) + (branch != nullptr) +
        (commit != nullptr) + (version != nullptr);
    if (ref_count != 1) {
        fprintf(stderr,
                "facet: specify exactly one of --tag, --branch, --commit, --version\n");
        return 64;
    }
    if (registry != nullptr && version == nullptr) {
        fprintf(stderr, "facet: --registry requires --version\n");
        return 64;
    }
    if (!file_exists("diamond.cut")) {
        fprintf(stderr,
                "facet: no diamond.cut found in the current directory -- "
                "run 'facet init' first\n");
        return 66;
    }
    char error[512];
    FacetManifest manifest;
    if (!parse_manifest("diamond.cut", &manifest, error, sizeof error)) {
        fprintf(stderr, "facet: %s\n", error);
        return 70;
    }
    if (name[0] == '\0' || strlen(name) >= FACET_MAX_NAME || !is_safe_field(name)) {
        fprintf(stderr, "facet: '%s' is not a valid dependency name\n", name);
        return 64;
    }
    for (size_t index = 0; index < manifest.dependency_count; index++) {
        if (strcmp(manifest.dependencies[index].name, name) == 0) {
            fprintf(stderr,
                    "facet: dependency '%s' already exists in diamond.cut -- "
                    "edit it directly, or remove it first\n", name);
            return 65;
        }
    }
    if (git != nullptr && (git[0] == '\0' || !is_safe_field(git))) {
        fprintf(stderr, "facet: '%s' is not a valid git URL\n", git);
        return 64;
    }
    if (registry != nullptr && !valid_registry_url(registry)) {
        fprintf(stderr, "facet: '%s' is not a valid registry URL\n", registry);
        return 64;
    }
    if (manifest.dependency_count == FACET_MAX_DEPENDENCIES) {
        fprintf(stderr, "facet: too many dependencies (max %d)\n",
                FACET_MAX_DEPENDENCIES);
        return 65;
    }
    FacetDependency *dependency = &manifest.dependencies[manifest.dependency_count];
    memset(dependency, 0, sizeof *dependency);
    (void)snprintf(dependency->name, sizeof dependency->name, "%s", name);
    dependency->source = registry != nullptr ? FACET_SOURCE_REGISTRY : FACET_SOURCE_GIT;
    if (registry != nullptr)
        (void)snprintf(dependency->registry, sizeof dependency->registry, "%s", registry);
    else
        (void)snprintf(dependency->git, sizeof dependency->git, "%s", git);
    if (version != nullptr) {
        if (version[0] == '\0' || strlen(version) >= sizeof dependency->version_text ||
            !is_safe_field(version)) {
            fprintf(stderr, "facet: '%s' is not a valid version constraint\n", version);
            return 64;
        }
        SemverConstraint probe;
        if (!semver_constraint_parse(version, &probe)) {
            fprintf(stderr, "facet: '%s' is not a valid version constraint\n", version);
            return 64;
        }
        dependency->uses_version = true;
        (void)snprintf(dependency->version_text, sizeof dependency->version_text, "%s",
                        version);
    } else {
        const char *ref = tag != nullptr ? tag : branch != nullptr ? branch : commit;
        if (ref[0] == '\0' || strlen(ref) >= sizeof dependency->ref || !is_safe_field(ref)) {
            fprintf(stderr, "facet: '%s' is not a valid ref\n", ref);
            return 64;
        }
        dependency->ref_kind = tag != nullptr ? FACET_REF_TAG
            : branch != nullptr ? FACET_REF_BRANCH : FACET_REF_COMMIT;
        (void)snprintf(dependency->ref, sizeof dependency->ref, "%s", ref);
    }
    manifest.dependency_count++;
    if (!write_manifest("diamond.cut", &manifest, error, sizeof error)) {
        fprintf(stderr, "facet: %s\n", error);
        return 70;
    }
    printf("facet: added '%s' to diamond.cut -- run 'facet update' to fetch it\n", name);
    return 0;
}

static bool publishable_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length >= FACET_MAX_NAME ||
        name[0] < 'a' || name[0] > 'z') return false;
    for (size_t i = 1; i < length; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return strcmp(name, "diamond") != 0 && strcmp(name, "facet") != 0 &&
           strcmp(name, "cuts") != 0;
}

static bool regular_file(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISREG(info.st_mode);
}

typedef struct FacetFileList {
    char **paths;
    size_t count;
    size_t capacity;
    unsigned long long total_bytes;
    size_t examined_entries;
} FacetFileList;

static void free_file_list(FacetFileList *list) {
    for (size_t i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
}

static bool same_folded_path(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        unsigned char left = (unsigned char)*a, right = (unsigned char)*b;
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32);
        if (left != right) return false;
    }
    return *a == *b;
}

static bool sensitive_name(const char *name) {
    size_t length = strlen(name);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e || c == '\\' || c == ':') return true;
    }
    if (name[0] == '.' || strcmp(name, "id_rsa") == 0 ||
        strcmp(name, "id_ed25519") == 0 || strcmp(name, "credentials") == 0 ||
        strcmp(name, "secrets") == 0) return true;
    const char *suffixes[] = {".pem", ".key", ".p12", ".pfx", ".dic"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        size_t suffix_length = strlen(suffixes[i]);
        if (length >= suffix_length &&
            strcmp(name + length - suffix_length, suffixes[i]) == 0) return true;
    }
    return false;
}

static bool add_file(FacetFileList *list, const char *root, const char *relative,
                     char *error, size_t error_size) {
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path too long: %s", relative);
        return false;
    }
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        (void)snprintf(error, error_size, "unsafe file type or hardlink: %s", relative);
        return false;
    }
    if (info.st_size < 0 || (unsigned long long)info.st_size > 10ULL * 1024 * 1024 ||
        list->total_bytes + (unsigned long long)info.st_size > 50ULL * 1024 * 1024) {
        (void)snprintf(error, error_size, "file size limit exceeded: %s", relative);
        return false;
    }
    if (list->count == 4096) {
        (void)snprintf(error, error_size, "too many package files (max 4096)");
        return false;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (same_folded_path(list->paths[i], relative)) {
            (void)snprintf(error, error_size, "case-insensitive path collision: %s", relative);
            return false;
        }
    }
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        char **grown = realloc(list->paths, new_capacity * sizeof *grown);
        if (grown == NULL) {
            (void)snprintf(error, error_size, "out of memory collecting files");
            return false;
        }
        list->paths = grown;
        list->capacity = new_capacity;
    }
    list->paths[list->count] = strdup(relative);
    if (list->paths[list->count] == NULL) {
        (void)snprintf(error, error_size, "out of memory collecting files");
        return false;
    }
    list->count++;
    list->total_bytes += (unsigned long long)info.st_size;
    return true;
}

static bool scan_runtime_tree(FacetFileList *list, const char *root,
                              const char *relative, unsigned depth,
                              char *error, size_t error_size) {
    if (depth > 32) {
        (void)snprintf(error, error_size, "package tree exceeds 32 levels");
        return false;
    }
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path too long: %s", relative);
        return false;
    }
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        (void)snprintf(error, error_size, "unsafe directory: %s", relative);
        return false;
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        (void)snprintf(error, error_size, "cannot read directory: %s", relative);
        return false;
    }
    bool ok = true;
    char **siblings = NULL;
    size_t sibling_count = 0;
    size_t sibling_capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (++list->examined_entries > 8192) {
            (void)snprintf(error, error_size, "too many package entries (max 8192)");
            ok = false;
            break;
        }
        for (size_t i = 0; i < sibling_count; i++) {
            if (same_folded_path(siblings[i], entry->d_name)) {
                (void)snprintf(error, error_size,
                               "case-insensitive path collision: %s/%s",
                               relative, entry->d_name);
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (sibling_count == sibling_capacity) {
            size_t new_capacity = sibling_capacity == 0 ? 8 : sibling_capacity * 2;
            char **grown = realloc(siblings, new_capacity * sizeof *grown);
            if (grown == NULL) {
                (void)snprintf(error, error_size, "out of memory collecting paths");
                ok = false;
                break;
            }
            siblings = grown;
            sibling_capacity = new_capacity;
        }
        siblings[sibling_count] = strdup(entry->d_name);
        if (siblings[sibling_count] == NULL) {
            (void)snprintf(error, error_size, "out of memory collecting paths");
            ok = false;
            break;
        }
        sibling_count++;
        if (sensitive_name(entry->d_name)) {
            (void)snprintf(error, error_size, "excluded or sensitive path in runtime tree: %s/%s",
                           relative, entry->d_name);
            ok = false;
            break;
        }
        char child[FACET_MAX_PATH];
        if (snprintf(child, sizeof child, "%s/%s", relative, entry->d_name) >=
            (int)sizeof child || snprintf(path, sizeof path, "%s/%s", root, child) >=
            (int)sizeof path || lstat(path, &info) != 0) {
            (void)snprintf(error, error_size, "cannot inspect package path");
            ok = false;
            break;
        }
        if (S_ISDIR(info.st_mode))
            ok = scan_runtime_tree(list, root, child, depth + 1, error, error_size);
        else
            ok = add_file(list, root, child, error, error_size);
        if (!ok) break;
    }
    closedir(directory);
    for (size_t i = 0; i < sibling_count; i++) free(siblings[i]);
    free(siblings);
    return ok;
}

static int compare_file_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void tar_octal(char *field, size_t width, unsigned long long number) {
    (void)snprintf(field, width, "%0*llo", (int)width - 1, number);
}

static void tar_header(unsigned char header[512], const char *relative,
                       unsigned mode, unsigned long long size) {
    memset(header, 0, 512);
    memcpy(header, relative, strlen(relative));
    tar_octal((char *)header + 100, 8, mode);
    tar_octal((char *)header + 108, 8, 0);
    tar_octal((char *)header + 116, 8, 0);
    tar_octal((char *)header + 124, 12, size);
    tar_octal((char *)header + 136, 12, 0);
    memset(header + 148, ' ', 8);
    header[156] = '0';
    memcpy(header + 257, "ustar", 5);
    memcpy(header + 263, "00", 2);
    unsigned checksum = 0;
    for (size_t i = 0; i < 512; i++) checksum += header[i];
    (void)snprintf((char *)header + 148, 7, "%06o", checksum);
    header[155] = ' ';
}

static bool write_tar_member(FILE *archive, const char *root, const char *relative,
                             char *error, size_t error_size) {
    if (strlen(relative) > 99) {
        (void)snprintf(error, error_size, "ustar path exceeds 99 bytes: %s", relative);
        return false;
    }
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path too long: %s", relative);
        return false;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot open package file: %s", relative);
        return false;
    }
    struct stat before, after;
    bool ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) &&
              before.st_nlink == 1 && before.st_size >= 0 &&
              before.st_size <= 10 * 1024 * 1024;
    if (!ok) {
        (void)snprintf(error, error_size, "package file changed or is unsafe: %s", relative);
        close(fd);
        return false;
    }
    unsigned char header[512];
    tar_header(header, relative, (before.st_mode & 0111) != 0 ? 0755 : 0644,
               (unsigned long long)before.st_size);
    if (fwrite(header, 1, sizeof header, archive) != sizeof header) ok = false;
    unsigned char buffer[8192];
    unsigned long long remaining = (unsigned long long)before.st_size;
    while (ok && remaining > 0) {
        size_t amount = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
        ssize_t got = read(fd, buffer, amount);
        if (got <= 0 || fwrite(buffer, 1, (size_t)got, archive) != (size_t)got) {
            ok = false;
            break;
        }
        remaining -= (unsigned long long)got;
    }
    size_t padding = (size_t)((512 - ((unsigned long long)before.st_size % 512)) % 512);
    if (ok && padding > 0) {
        unsigned char zeros[512] = {0};
        if (fwrite(zeros, 1, padding, archive) != padding) ok = false;
    }
    if (fstat(fd, &after) != 0 || after.st_size != before.st_size ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_mode != before.st_mode ||
        after.st_mtim.tv_sec != before.st_mtim.tv_sec ||
        after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
        after.st_ctim.tv_sec != before.st_ctim.tv_sec ||
        after.st_ctim.tv_nsec != before.st_ctim.tv_nsec)
        ok = false;
    close(fd);
    if (!ok) (void)snprintf(error, error_size, "package file changed while packing: %s", relative);
    return ok;
}

static bool digest_stream(FILE *file, unsigned char digest[32],
                          char *error, size_t error_size) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool ok = context != NULL &&
              EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    unsigned char buffer[8192];
    while (ok) {
        size_t got = fread(buffer, 1, sizeof buffer, file);
        if (got > 0 && EVP_DigestUpdate(context, buffer, got) != 1) ok = false;
        if (got < sizeof buffer) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    unsigned length = 0;
    if (ok && (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32))
        ok = false;
    EVP_MD_CTX_free(context);
    if (!ok) (void)snprintf(error, error_size, "cannot hash archive");
    return ok;
}

static bool digest_file(const char *path, unsigned char digest[32],
                        char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(error, error_size, "cannot open archive: %s", strerror(errno));
        return false;
    }
    bool ok = digest_stream(file, digest, error, error_size);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool pack_tar(const char *root, const FacetFileList *files,
                     const char *output_arg, char *error, size_t error_size) {
    error[0] = '\0';
    char output_copy[FACET_MAX_PATH];
    if (snprintf(output_copy, sizeof output_copy, "%s", output_arg) >=
        (int)sizeof output_copy) {
        (void)snprintf(error, error_size, "output path too long");
        return false;
    }
    char *slash = strrchr(output_copy, '/');
    const char *base = slash == NULL ? output_copy : slash + 1;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        (void)snprintf(error, error_size, "invalid output filename");
        return false;
    }
    const char *parent = ".";
    if (slash != NULL) {
        *slash = '\0';
        parent = output_copy[0] == '\0' ? "/" : output_copy;
    }
    char *directory = realpath(parent, NULL);
    if (directory == NULL) {
        (void)snprintf(error, error_size, "cannot resolve output directory");
        return false;
    }
    bool inside_root = strncmp(directory, root, strlen(root)) == 0 &&
        (directory[strlen(root)] == '/' || directory[strlen(root)] == '\0');
    if (inside_root) {
        (void)snprintf(error, error_size, "archive output must be outside the cut directory");
        free(directory);
        return false;
    }
    char output[FACET_MAX_PATH], temporary[FACET_MAX_PATH];
    bool paths_ok = snprintf(output, sizeof output, "%s/%s", directory, base) <
                    (int)sizeof output &&
                    snprintf(temporary, sizeof temporary, "%s/.facet-pack-XXXXXX", directory) <
                    (int)sizeof temporary;
    free(directory);
    if (!paths_ok) {
        (void)snprintf(error, error_size, "output path too long");
        return false;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot create temporary archive: %s", strerror(errno));
        return false;
    }
    FILE *archive = fdopen(fd, "wb");
    if (archive == NULL) {
        close(fd);
        unlink(temporary);
        (void)snprintf(error, error_size, "cannot open temporary archive");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < files->count; i++)
        ok = write_tar_member(archive, root, files->paths[i], error, error_size);
    unsigned char zeros[1024] = {0};
    if (ok && fwrite(zeros, 1, sizeof zeros, archive) != sizeof zeros) ok = false;
    if (fclose(archive) != 0) ok = false;
    unsigned char digest[32];
    if (ok) ok = digest_file(temporary, digest, error, error_size);
    if (ok && link(temporary, output) != 0) {
        (void)snprintf(error, error_size, "cannot create '%s': %s", output, strerror(errno));
        ok = false;
    }
    unlink(temporary);
    if (!ok) {
        if (error[0] == '\0')
            (void)snprintf(error, error_size, "cannot write archive");
        return false;
    }
    printf("facet: packed %zu files to %s\nsha256: ", files->count, output);
    for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
    putchar('\n');
    return true;
}

static bool tar_number(const unsigned char *field, size_t width,
                       unsigned long long *out) {
    if (field[width - 1] != '\0') return false;
    unsigned long long value = 0;
    for (size_t i = 0; i + 1 < width; i++) {
        if (field[i] < '0' || field[i] > '7') return false;
        value = value * 8 + (unsigned long long)(field[i] - '0');
    }
    *out = value;
    return true;
}

static bool all_zero(const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) if (bytes[i] != 0) return false;
    return true;
}

static bool valid_archive_path(const char *path) {
    if (strcmp(path, "diamond.cut") == 0 || strcmp(path, "README.md") == 0 ||
        strcmp(path, "LICENSE") == 0) return true;
    const char *prefixes[] = {"lib/", "bin/", "assets/"};
    bool allowed = false;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0) allowed = true;
    if (!allowed) return false;
    const char *component = path;
    for (const char *cursor = path;; cursor++) {
        unsigned char c = (unsigned char)*cursor;
        if (c == '/' || c == '\0') {
            size_t length = (size_t)(cursor - component);
            if (length == 0 || (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.'))
                return false;
            char name[101];
            if (length >= sizeof name) return false;
            memcpy(name, component, length);
            name[length] = '\0';
            if (sensitive_name(name)) return false;
            if (c == '\0') return true;
            component = cursor + 1;
        } else if (c < 0x20 || c > 0x7e || c == '\\' || c == ':') {
            return false;
        }
    }
}

static bool archive_path_conflict(const char *a, const char *b) {
    for (;;) {
        const char *a_end = strchr(a, '/');
        const char *b_end = strchr(b, '/');
        size_t a_length = a_end == NULL ? strlen(a) : (size_t)(a_end - a);
        size_t b_length = b_end == NULL ? strlen(b) : (size_t)(b_end - b);
        if (a_length != b_length) return false;
        bool equal_folded = true;
        for (size_t i = 0; i < a_length; i++) {
            unsigned char left = (unsigned char)a[i], right = (unsigned char)b[i];
            if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32);
            if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32);
            if (left != right) { equal_folded = false; break; }
        }
        if (!equal_folded) return false;
        if (memcmp(a, b, a_length) != 0) return true;
        if (a_end == NULL || b_end == NULL) return a_end != b_end;
        a = a_end + 1;
        b = b_end + 1;
    }
}

static bool expected_digest(const char *text, const unsigned char actual[32]) {
    if (strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        unsigned char digits[2] = {(unsigned char)text[i * 2],
                                   (unsigned char)text[i * 2 + 1]};
        unsigned byte = 0;
        for (size_t j = 0; j < 2; j++) {
            unsigned char c = digits[j];
            if (c >= '0' && c <= '9') byte = byte * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') byte = byte * 16 + (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') byte = byte * 16 + (unsigned)(c - 'A' + 10);
            else return false;
        }
        if (byte != actual[i]) return false;
    }
    return true;
}

/* Catch literal imports that would be missing from a standalone artifact.
 * This is a preflight for statically visible imports, not a full Diamond
 * parser: quoted imports at the start of a source line are the common form. */
static bool import_escapes_cut(const char *source_path, const char *target) {
    if (target[0] == '/') return true;
    int depth = 0;
    for (const char *p = source_path; *p != '\0'; p++)
        if (*p == '/') depth++;
    const char *part = target;
    while (*part != '\0') {
        const char *end = strchr(part, '/');
        size_t length = end == NULL ? strlen(part) : (size_t)(end - part);
        if (length == 2 && part[0] == '.' && part[1] == '.') {
            if (--depth < 0) return true;
        } else if (length != 0 && !(length == 1 && part[0] == '.')) {
            depth++;
        }
        if (end == NULL) break;
        part = end + 1;
    }
    return false;
}

static bool runtime_source_path(const char *relative) {
    size_t length = strlen(relative);
    return (strncmp(relative, "lib/", 4) == 0 ||
            strncmp(relative, "bin/", 4) == 0) &&
           length >= 3 && strcmp(relative + length - 3, ".di") == 0;
}

static bool audit_source_imports(char *source, const char *relative,
                                 const DiamondManifestValue *dependencies,
                                 char *error, size_t error_size) {
        for (char *line = source; *line != '\0';) {
            char *end = strchr(line, '\n');
            if (end != NULL) *end = '\0';
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            bool cut = strncmp(p, "require_cut", 11) == 0 &&
                       (p[11] == ' ' || p[11] == '\t');
            bool local = !cut && strncmp(p, "require", 7) == 0 &&
                         (p[7] == ' ' || p[7] == '\t');
            if (cut || local) {
                p += cut ? 11 : 7;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    char *value = ++p;
                    while (*p != '\0' && *p != '"') p++;
                    if (*p == '"') {
                        *p = '\0';
                        if (cut && (dependencies == NULL ||
                            diamond_manifest_get(dependencies, value) == NULL)) {
                            (void)snprintf(error, error_size,
                                "%s imports undeclared cut '%s'", relative, value);
                            return false;
                        }
                        if (local && import_escapes_cut(relative, value)) {
                            (void)snprintf(error, error_size,
                                "%s imports outside the cut: %s", relative, value);
                            return false;
                        }
                    }
                }
            }
            if (end == NULL) break;
            line = end + 1;
        }
    return true;
}

static bool audit_cut_imports(const char *root, const FacetFileList *files,
                              const DiamondManifestValue *dependencies,
                              char *error, size_t error_size) {
    for (size_t i = 0; i < files->count; i++) {
        const char *relative = files->paths[i];
        if (!runtime_source_path(relative)) continue;
        char path[FACET_MAX_PATH];
        if (snprintf(path, sizeof path, "%s/%s", root, relative) >=
            (int)sizeof path) {
            (void)snprintf(error, error_size, "runtime source path is too long");
            return false;
        }
        char *source = read_whole_file(path, error, error_size);
        if (source == NULL) return false;
        bool ok = audit_source_imports(source, relative, dependencies, error, error_size);
        free(source);
        if (!ok) return false;
    }
    return true;
}

static int cmd_verify(int argc, char **argv) {
    if (argc != 3 && !(argc == 5 && strcmp(argv[3], "--sha256") == 0)) {
        fputs("usage: facet verify <archive.tar> [--sha256 <digest>]\n", stderr);
        return 64;
    }
    char error[512];
    unsigned char digest[32];
    FILE *archive = fopen(argv[2], "rb");
    if (archive == NULL) {
        fprintf(stderr, "facet: cannot open archive: %s\n", strerror(errno));
        return 66;
    }
    struct stat archive_before, archive_after;
    if (fstat(fileno(archive), &archive_before) != 0 ||
        !S_ISREG(archive_before.st_mode) || archive_before.st_size < 0 ||
        archive_before.st_size > 56LL * 1024 * 1024 ||
        !digest_stream(archive, digest, error, sizeof error) ||
        fseek(archive, 0, SEEK_SET) != 0) {
        fclose(archive);
        fputs("facet: cannot hash archive\n", stderr);
        return 66;
    }
    if (argc == 5 && !expected_digest(argv[4], digest)) {
        fclose(archive);
        fputs("facet: archive SHA-256 does not match expected digest\n", stderr);
        return 65;
    }
    FacetFileList files = {0};
    char **runtime_sources = calloc(4096, sizeof *runtime_sources);
    if (runtime_sources == NULL) {
        fclose(archive);
        fputs("facet: out of memory reading archive\n", stderr);
        return 65;
    }
    char *manifest_source = NULL;
    unsigned long long total_bytes = 0;
    bool ok = true, ended = false;
    while (ok && !ended) {
        unsigned char header[512];
        if (fread(header, 1, sizeof header, archive) != sizeof header) {
            (void)snprintf(error, sizeof error, "truncated archive header");
            ok = false;
            break;
        }
        if (all_zero(header, sizeof header)) {
            unsigned char second[512];
            if (fread(second, 1, sizeof second, archive) != sizeof second ||
                !all_zero(second, sizeof second) || fgetc(archive) != EOF ||
                ferror(archive)) {
                (void)snprintf(error, sizeof error, "invalid archive ending");
                ok = false;
            }
            ended = true;
            break;
        }
        const unsigned char *end = memchr(header, '\0', 100);
        unsigned long long mode, size;
        if (end == NULL || end == header ||
            !tar_number(header + 100, 8, &mode) ||
            !tar_number(header + 124, 12, &size) ||
            (mode != 0644 && mode != 0755) || size > 10ULL * 1024 * 1024 ||
            files.count == 4096 || total_bytes + size > 50ULL * 1024 * 1024) {
            (void)snprintf(error, sizeof error, "invalid archive entry header or limits");
            ok = false;
            break;
        }
        char path[101];
        size_t path_length = (size_t)(end - header);
        memcpy(path, header, path_length);
        path[path_length] = '\0';
        unsigned char canonical[512];
        tar_header(canonical, path, (unsigned)mode, size);
        if (memcmp(header, canonical, sizeof header) != 0 ||
            !valid_archive_path(path) ||
            (files.count > 0 && strcmp(files.paths[files.count - 1], path) >= 0)) {
            (void)snprintf(error, sizeof error, "noncanonical or unsafe archive entry: %s", path);
            ok = false;
            break;
        }
        for (size_t i = 0; i < files.count; i++) {
            if (archive_path_conflict(files.paths[i], path)) {
                (void)snprintf(error, sizeof error,
                               "case-insensitive archive path collision: %s", path);
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (files.count == files.capacity) {
            size_t capacity = files.capacity == 0 ? 16 : files.capacity * 2;
            char **grown = realloc(files.paths, capacity * sizeof *grown);
            if (grown == NULL) {
                (void)snprintf(error, sizeof error, "out of memory reading archive");
                ok = false;
                break;
            }
            files.paths = grown;
            files.capacity = capacity;
        }
        files.paths[files.count] = strdup(path);
        if (files.paths[files.count] == NULL) {
            (void)snprintf(error, sizeof error, "out of memory reading archive");
            ok = false;
            break;
        }
        files.count++;
        total_bytes += size;
        if (strcmp(path, "diamond.cut") == 0) {
            if (size > 1024 * 1024) {
                (void)snprintf(error, sizeof error, "manifest exceeds 1 MiB");
                ok = false;
                break;
            }
            manifest_source = malloc((size_t)size + 1);
            if (manifest_source == NULL ||
                fread(manifest_source, 1, (size_t)size, archive) != (size_t)size) {
                (void)snprintf(error, sizeof error, "truncated archive manifest");
                ok = false;
                break;
            }
            manifest_source[size] = '\0';
            if (memchr(manifest_source, '\0', (size_t)size) != NULL) {
                (void)snprintf(error, sizeof error, "NUL in archive manifest");
                ok = false;
                break;
            }
        } else if (runtime_source_path(path)) {
            char *source = malloc((size_t)size + 1);
            if (source == NULL || fread(source, 1, (size_t)size, archive) != (size_t)size) {
                free(source);
                (void)snprintf(error, sizeof error, "truncated archive source: %s", path);
                ok = false;
                break;
            }
            source[size] = '\0';
            if (memchr(source, '\0', (size_t)size) != NULL) {
                free(source);
                (void)snprintf(error, sizeof error, "NUL in archive source: %s", path);
                ok = false;
                break;
            }
            runtime_sources[files.count - 1] = source;
        } else {
            unsigned char buffer[8192];
            unsigned long long remaining = size;
            while (remaining > 0) {
                size_t amount = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
                if (fread(buffer, 1, amount, archive) != amount) {
                    (void)snprintf(error, sizeof error, "truncated archive entry: %s", path);
                    ok = false;
                    break;
                }
                remaining -= amount;
            }
            if (!ok) break;
        }
        unsigned char padding[512];
        size_t pad = (size_t)((512 - size % 512) % 512);
        if (pad > 0 && (fread(padding, 1, pad, archive) != pad ||
                        !all_zero(padding, pad))) {
            (void)snprintf(error, sizeof error, "invalid archive padding: %s", path);
            ok = false;
        }
    }
    if (fstat(fileno(archive), &archive_after) != 0 ||
        archive_before.st_size != archive_after.st_size ||
        archive_before.st_mtim.tv_sec != archive_after.st_mtim.tv_sec ||
        archive_before.st_mtim.tv_nsec != archive_after.st_mtim.tv_nsec ||
        archive_before.st_ctim.tv_sec != archive_after.st_ctim.tv_sec ||
        archive_before.st_ctim.tv_nsec != archive_after.st_ctim.tv_nsec) {
        (void)snprintf(error, sizeof error, "archive changed while verifying");
        ok = false;
    }
    fclose(archive);
    DiamondManifestValue *manifest = NULL;
    if (ok && manifest_source != NULL) {
        char detail[160];
        manifest = diamond_manifest_parse(manifest_source, detail, sizeof detail);
        if (manifest == NULL) {
            (void)snprintf(error, sizeof error, "invalid archive manifest: %s", detail);
            ok = false;
        }
    }
    if (ok && manifest == NULL) {
        (void)snprintf(error, sizeof error, "archive is missing diamond.cut");
        ok = false;
    }
    if (ok) {
        const char *allowed[] = {"name", "version", "summary", "license",
                                 "dependencies", "homepage", "source",
                                 "documentation", "issues"};
        for (const DiamondManifestValue *item = manifest->children;
             item != NULL; item = item->next) {
            bool known = false;
            for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
                if (strcmp(item->key, allowed[i]) == 0) known = true;
            if (!known) {
                (void)snprintf(error, sizeof error,
                               "unknown archive manifest key: %s", item->key);
                ok = false;
                break;
            }
        }
    }
    const DiamondManifestValue *name = diamond_manifest_get(manifest, "name");
    const DiamondManifestValue *version = diamond_manifest_get(manifest, "version");
    Semver parsed_version;
    if (ok && (name == NULL || name->kind != DIAMOND_MANIFEST_STRING ||
               !publishable_name(name->string) || version == NULL ||
               version->kind != DIAMOND_MANIFEST_STRING ||
               version->string[0] == 'v' || version->string[0] == 'V' ||
               !semver_parse(version->string, &parsed_version) ||
               parsed_version.build[0] != '\0')) {
        (void)snprintf(error, sizeof error, "archive manifest has invalid name or version");
        ok = false;
    }
    if (ok) {
        const DiamondManifestValue *summary = diamond_manifest_get(manifest, "summary");
        const DiamondManifestValue *license = diamond_manifest_get(manifest, "license");
        if (summary == NULL || summary->kind != DIAMOND_MANIFEST_STRING ||
            summary->string[0] == '\0' || strlen(summary->string) > 160 ||
            strpbrk(summary->string, "\r\n\t") != NULL ||
            license == NULL || license->kind != DIAMOND_MANIFEST_STRING ||
            license->string[0] == '\0') {
            (void)snprintf(error, sizeof error, "archive manifest is missing valid summary or license");
            ok = false;
        }
    }
    if (ok) {
        const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
        if (dependencies != NULL && dependencies->kind != DIAMOND_MANIFEST_HASH) {
            (void)snprintf(error, sizeof error, "archive dependencies must be a Hash");
            ok = false;
        } else if (dependencies != NULL) {
            for (const DiamondManifestValue *dependency = dependencies->children;
                 dependency != NULL; dependency = dependency->next) {
                SemverConstraint range;
                if (!publishable_name(dependency->key) ||
                    dependency->kind != DIAMOND_MANIFEST_STRING ||
                    !semver_constraint_parse(dependency->string, &range)) {
                    (void)snprintf(error, sizeof error, "invalid archive dependency: %s",
                                   dependency->key);
                    ok = false;
                    break;
                }
            }
        }
    }
    if (ok) {
        const char *display_keys[] = {"homepage", "source", "documentation", "issues"};
        for (size_t i = 0; i < sizeof display_keys / sizeof display_keys[0]; i++) {
            const DiamondManifestValue *display = diamond_manifest_get(manifest, display_keys[i]);
            if (display != NULL && display->kind != DIAMOND_MANIFEST_STRING) {
                (void)snprintf(error, sizeof error,
                               "archive manifest '%s' must be a String", display_keys[i]);
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
        for (size_t i = 0; i < files.count; i++) {
            if (runtime_sources[i] != NULL &&
                !audit_source_imports(runtime_sources[i], files.paths[i], dependencies,
                                      error, sizeof error)) {
                ok = false;
                break;
            }
        }
    }
    bool has_readme = false, has_license = false, has_entry = false;
    if (ok) {
        char entry[128];
        (void)snprintf(entry, sizeof entry, "lib/%s.di", name->string);
        for (size_t i = 0; i < files.count; i++) {
            if (strcmp(files.paths[i], "README.md") == 0) has_readme = true;
            if (strcmp(files.paths[i], "LICENSE") == 0) has_license = true;
            if (strcmp(files.paths[i], entry) == 0) has_entry = true;
        }
        if (!has_readme || !has_license || !has_entry) {
            (void)snprintf(error, sizeof error, "archive is missing README.md, LICENSE, or public entry point");
            ok = false;
        }
    }
    if (ok) {
        printf("facet: verified %s %s (%zu files)\nsha256: ",
               name->string, version->string, files.count);
        for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
        putchar('\n');
    } else {
        fprintf(stderr, "facet: %s\n", error);
    }
    diamond_manifest_free(manifest);
    free(manifest_source);
    for (size_t i = 0; i < files.count; i++) free(runtime_sources[i]);
    free(runtime_sources);
    free_file_list(&files);
    return ok ? 0 : 65;
}

static int cmd_check(int argc, char **argv) {
    bool packing = strcmp(argv[1], "pack") == 0;
    bool show_files = !packing && argc == 4 && strcmp(argv[3], "--files") == 0;
    if ((packing && argc != 4) || (!packing && argc != 3 && !show_files)) {
        fputs(packing ? "usage: facet pack <cut-directory> <output.tar>\n" :
                        "usage: facet check <cut-directory> [--files]\n", stderr);
        return 64;
    }
    char *canonical = realpath(argv[2], NULL);
    if (canonical == NULL) {
        fprintf(stderr, "facet: cannot open cut directory '%s': %s\n",
                argv[2], strerror(errno));
        return 66;
    }
    if (strlen(canonical) >= FACET_MAX_PATH) {
        fprintf(stderr, "facet: cut directory path is too long\n");
        free(canonical);
        return 65;
    }
    char root[FACET_MAX_PATH];
    strcpy(root, canonical);
    free(canonical);
    struct stat root_info;
    if (stat(root, &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        fprintf(stderr, "facet: '%s' is not a directory\n", argv[2]);
        return 65;
    }
    const char *directory_name = strrchr(root, '/');
    directory_name = directory_name == NULL ? root : directory_name + 1;
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/diamond.cut", root) >= (int)sizeof path ||
        !regular_file(path)) {
        fprintf(stderr, "facet: '%s' needs a regular diamond.cut\n", root);
        return 65;
    }
    char error[512];
    DiamondManifestValue *manifest = facet_read_hash(path, error, sizeof error);
    if (manifest == NULL) {
        fprintf(stderr, "facet: %s\n", error);
        return 65;
    }
    int status = 65;
    FacetFileList files = {0};
    const char *allowed[] = {"name", "version", "summary", "license",
                             "dependencies", "homepage", "source",
                             "documentation", "issues"};
    for (const DiamondManifestValue *item = manifest->children;
         item != NULL; item = item->next) {
        bool known = false;
        for (size_t i = 0; i < sizeof allowed / sizeof allowed[0]; i++)
            if (strcmp(item->key, allowed[i]) == 0) known = true;
        if (!known) {
            fprintf(stderr, "facet: unknown manifest key '%s'\n", item->key);
            goto done;
        }
    }
    const DiamondManifestValue *name = diamond_manifest_get(manifest, "name");
    if (name == NULL || name->kind != DIAMOND_MANIFEST_STRING ||
        !publishable_name(name->string) || strcmp(name->string, directory_name) != 0) {
        fprintf(stderr, "facet: cut name must match its directory and use lowercase ASCII letters, digits, or underscores (starting with a letter)\n");
        goto done;
    }
    const DiamondManifestValue *version = diamond_manifest_get(manifest, "version");
    Semver parsed_version;
    if (version == NULL || version->kind != DIAMOND_MANIFEST_STRING ||
        version->string[0] == 'v' || version->string[0] == 'V' ||
        !semver_parse(version->string, &parsed_version) || parsed_version.build[0] != '\0') {
        fputs("facet: 'version' must be canonical SemVer without a leading v or build metadata\n", stderr);
        goto done;
    }
    const DiamondManifestValue *summary = diamond_manifest_get(manifest, "summary");
    if (summary == NULL || summary->kind != DIAMOND_MANIFEST_STRING ||
        summary->string[0] == '\0' || strlen(summary->string) > 160 ||
        strpbrk(summary->string, "\r\n\t") != NULL) {
        fputs("facet: 'summary' must be a single-line String of 1-160 bytes\n", stderr);
        goto done;
    }
    const DiamondManifestValue *license = diamond_manifest_get(manifest, "license");
    if (license == NULL || license->kind != DIAMOND_MANIFEST_STRING ||
        license->string[0] == '\0') {
        fputs("facet: 'license' must be a nonempty String\n", stderr);
        goto done;
    }
    const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
    if (dependencies != NULL) {
        if (dependencies->kind != DIAMOND_MANIFEST_HASH) {
            fputs("facet: 'dependencies' must be a Hash\n", stderr);
            goto done;
        }
        for (const DiamondManifestValue *dependency = dependencies->children;
             dependency != NULL; dependency = dependency->next) {
            SemverConstraint range;
            if (!publishable_name(dependency->key) ||
                dependency->kind != DIAMOND_MANIFEST_STRING ||
                !semver_constraint_parse(dependency->string, &range)) {
                fprintf(stderr, "facet: dependency '%s' needs a valid SemVer range String\n",
                        dependency->key);
                goto done;
            }
        }
    }
    const char *display_keys[] = {"homepage", "source", "documentation", "issues"};
    for (size_t i = 0; i < sizeof display_keys / sizeof display_keys[0]; i++) {
        const DiamondManifestValue *display = diamond_manifest_get(manifest, display_keys[i]);
        if (display != NULL && display->kind != DIAMOND_MANIFEST_STRING) {
            fprintf(stderr, "facet: '%s' must be a String\n", display_keys[i]);
            goto done;
        }
    }
    const char *required_files[] = {"README.md", "LICENSE"};
    for (size_t i = 0; i < sizeof required_files / sizeof required_files[0]; i++) {
        if (snprintf(path, sizeof path, "%s/%s", root, required_files[i]) >=
            (int)sizeof path || !regular_file(path)) {
            fprintf(stderr, "facet: '%s' needs a regular %s\n", root,
                    required_files[i]);
            goto done;
        }
    }
    if (snprintf(path, sizeof path, "%s/lib/%s.di", root, name->string) >=
        (int)sizeof path || !regular_file(path)) {
        fprintf(stderr, "facet: '%s' needs a regular lib/%s.di\n", root,
                name->string);
        goto done;
    }
    const char *metadata_files[] = {"diamond.cut", "README.md", "LICENSE"};
    for (size_t i = 0; i < sizeof metadata_files / sizeof metadata_files[0]; i++) {
        if (!add_file(&files, root, metadata_files[i], error, sizeof error)) {
            fprintf(stderr, "facet: %s\n", error);
            goto done;
        }
    }
    const char *runtime_roots[] = {"lib", "bin", "assets"};
    for (size_t i = 0; i < sizeof runtime_roots / sizeof runtime_roots[0]; i++) {
        if (snprintf(path, sizeof path, "%s/%s", root, runtime_roots[i]) >=
            (int)sizeof path) {
            fputs("facet: runtime path is too long\n", stderr);
            goto done;
        }
        struct stat info;
        if (lstat(path, &info) != 0 && errno == ENOENT && i != 0) continue;
        if (!scan_runtime_tree(&files, root, runtime_roots[i], 0,
                               error, sizeof error)) {
            fprintf(stderr, "facet: %s\n", error);
            goto done;
        }
    }
    qsort(files.paths, files.count, sizeof *files.paths, compare_file_paths);
    if (!audit_cut_imports(root, &files, dependencies, error, sizeof error)) {
        fprintf(stderr, "facet: %s\n", error);
        goto done;
    }
    if (show_files || packing)
        for (size_t i = 0; i < files.count; i++) puts(files.paths[i]);
    if (packing) {
        if (!pack_tar(root, &files, argv[3], error, sizeof error)) {
            fprintf(stderr, "facet: %s\n", error);
            goto done;
        }
    } else {
        printf("facet: %s %s is ready for packaging checks\n", name->string,
               version->string);
    }
    status = 0;
done:
    free_file_list(&files);
    diamond_manifest_free(manifest);
    return status;
}

static void print_usage(void) {
    fputs("usage: facet install\n"
          "       facet update\n"
          "       facet init [name]\n"
          "       facet check <cut-directory> [--files]\n"
          "       facet pack <cut-directory> <output.tar>\n"
          "       facet verify <archive.tar> [--sha256 <digest>]\n"
          "       facet add <name> (--git <url> "
          "(--tag <ref> | --branch <ref> | --commit <ref> | --version <constraint>) "
          "| --registry <url> --version <constraint>)\n",
          stderr);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "install") == 0) {
        return run_install_or_update(false);
    }
    if (argc == 2 && strcmp(argv[1], "update") == 0) {
        return run_install_or_update(true);
    }
    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        return cmd_init(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "add") == 0) {
        return cmd_add(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "check") == 0) {
        return cmd_check(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "pack") == 0) {
        return cmd_check(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "verify") == 0) {
        return cmd_verify(argc, argv);
    }
    print_usage();
    return 64;
}
