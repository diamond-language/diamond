#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "facet_internal.h"


/* Darwin names the nanosecond fields differently; keep archive race checks
 * equally precise on every supported platform. */
bool same_file_timestamps(const struct stat *left, const struct stat *right) {
#ifdef __APPLE__
    return left->st_mtimespec.tv_sec == right->st_mtimespec.tv_sec &&
           left->st_mtimespec.tv_nsec == right->st_mtimespec.tv_nsec &&
           left->st_ctimespec.tv_sec == right->st_ctimespec.tv_sec &&
           left->st_ctimespec.tv_nsec == right->st_ctimespec.tv_nsec;
#else
    return left->st_mtim.tv_sec == right->st_mtim.tv_sec &&
           left->st_mtim.tv_nsec == right->st_mtim.tv_nsec &&
           left->st_ctim.tv_sec == right->st_ctim.tv_sec &&
           left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
#endif
}


bool file_exists(const char *path) {
    struct stat info;
    return stat(path, &info) == 0;
}

bool valid_sha256(const char *text) {
    if (strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((text[i] >= '0' && text[i] <= '9') ||
              (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

bool valid_registry_url(const char *url) {
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

bool canonical_registry_version(const char *text) {
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

char *read_whole_file(const char *path, char *error, size_t error_size) {
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

bool ensure_directory(const char *path) {
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

bool remove_directory_recursive(const char *path) {
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
bool git_clone(const char *url, const char *ref, const char *destination,
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

bool git_rev_parse_head(const char *repository, char *out, size_t out_size,
                               char *error, size_t error_size) {
    char *const argv[] = {(char *)"git", (char *)"-C", (char *)repository,
        (char *)"rev-parse", (char *)"HEAD", nullptr};
    return run_git_capture(argv, "resolving the checked-out commit", out,
                           out_size, error, error_size);
}

/* Lists every tag on `url`'s remote whose name parses as a semver
 * version (with or without a leading v/V -- see tools/semver.h),
 * writing each matching tag's own original name (not normalized) into
 * tags[0..*out_count). This is the entire "version database" a
 * version-constrained git dependency consults: a repository's own tags
 * are the only source of "what versions exist." (A registry dependency
 * asks the registry for its versions instead.) `--refs` excludes the `^{}` peeled-commit
 * duplicate entry an annotated tag would otherwise also produce. */
bool git_list_semver_tags(const char *url, char tags[][FACET_MAX_REF],
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
 * `constraint` -- deterministic, with no
 * backtracking of its own (that lives a level up, in resolve_full_graph's
 * retry loop). Returns false if none satisfy it. */
bool pick_best_matching_tag(char tags[][FACET_MAX_REF], size_t tag_count,
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
DiamondManifestValue *facet_read_hash(const char *path, char *error,
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

bool parse_dependencies(const DiamondManifestValue *dependencies,
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

bool parse_manifest(const char *path, FacetManifest *manifest,
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

/* Published cuts use dependency ranges, unlike application source specs.
 * Full artifact validation happens in check/verify; this reads identity only. */
bool parse_cut_identity(const char *path, FacetManifest *manifest,
                               char *error, size_t error_size) {
    memset(manifest, 0, sizeof *manifest);
    DiamondManifestValue *hash = facet_read_hash(path, error, error_size);
    if (hash == NULL) return false;
    bool ok = diamond_manifest_get_string(hash, "name", manifest->name, sizeof manifest->name) &&
              is_safe_field(manifest->name) &&
              diamond_manifest_get_string(hash, "version", manifest->version, sizeof manifest->version);
    manifest->has_version = ok;
    diamond_manifest_free(hash);
    if (!ok) snprintf(error, error_size, "cut requires String name and version");
    return ok;
}

bool parse_lockfile(const char *path, FacetResolution *resolution,
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

bool write_lockfile(const char *path, const FacetResolution *resolution,
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


static void print_usage(void) {
    fputs("usage: facet install\n"
          "       facet update\n"
          "       facet init [name]\n"
          "       facet check <cut-directory> [--files]\n"
          "       facet pack <cut-directory> <output.tar>\n"
          "       facet verify <archive.tar> [--sha256 <digest>] [--json | --readme]\n"
          "       facet publish <cut-directory> --registry <https-url> --token <token>\n"
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
    if (argc >= 2 && strcmp(argv[1], "publish") == 0) {
        return cmd_publish(argc, argv);
    }
    print_usage();
    return 64;
}
