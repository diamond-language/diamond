#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "facet_internal.h"

/* During `facet update`, a still-compatible registry lock is preferred over
 * an otherwise newer candidate. This keeps updates reproducible while still
 * allowing an explicit manifest constraint change to move the version. */
static const FacetResolution *registry_update_preference = NULL;

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

/* Real backtracking: the AND of every version constraint ever seen for this name,
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

static bool run_curl_download(const char *url, const char *output, uint64_t max_size,
                              char *error, size_t error_size);
static bool resolve_manifest_dependencies(const FacetManifest *manifest,
    const char *required_by, FacetResolution *resolution, FacetPendingTable *pending,
    FacetWorkQueue *queue, const char *scratch_root, FacetConstraintHistory *history,
    bool *needs_restart, char *error, size_t error_size);

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
 * arbitrary commit against a range's intent" --
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
 * (see resolve_full_graph's own comment for the retry loop this feeds): every
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
    char preferred[FACET_MAX_REF] = {0};
    if (registry_update_preference != NULL) {
        for (size_t index = 0; index < registry_update_preference->count; index++) {
            const FacetResolved *locked = &registry_update_preference->packages[index];
            if (locked->source == FACET_SOURCE_REGISTRY &&
                strcmp(locked->name, entry->name) == 0 &&
                strcmp(locked->registry, entry->registry) == 0) {
                (void)snprintf(preferred, sizeof preferred, "%s", locked->version);
                break;
            }
        }
    }
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
                (!found || semver_compare(&candidate, &best) > 0 ||
                 strcmp(version, preferred) == 0)) {
                found = true;
                best = candidate;
                (void)snprintf(out, out_size, "%s", version);
                if (strcmp(version, preferred) == 0) break;
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

/* Real backtracking: attempt_resolve_graph's only way to signal "an already-resolved
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
bool resolve_full_graph(const FacetManifest *manifest, FacetResolution *resolution,
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
        !same_file_timestamps(&before, &after)) ok = false;
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
        !parse_cut_identity(manifest_path, &manifest, error, error_size) ||
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

int run_install_or_update(bool force_resolve) {
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
    FacetResolution previous = {0};
    bool ok;
    if (!force_resolve && file_exists("facet.lock")) {
        ok = parse_lockfile("facet.lock", &resolution, error, sizeof error);
    } else {
        FacetManifest manifest;
        if (force_resolve && file_exists("facet.lock"))
            ok = parse_lockfile("facet.lock", &previous, error, sizeof error);
        else
            ok = true;
        registry_update_preference = ok && force_resolve ? &previous : NULL;
        ok = ok && parse_manifest("diamond.cut", &manifest, error, sizeof error) &&
             resolve_full_graph(&manifest, &resolution, scratch_root, error, sizeof error) &&
             write_lockfile("facet.lock", &resolution, error, sizeof error);
        registry_update_preference = NULL;
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
