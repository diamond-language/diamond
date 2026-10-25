/* Declarations shared by facet's source files: facet.c (manifests, the lockfile, git, `init`,
 * `add` and main), facet_resolve.c (dependency resolution and install), facet_package.c
 * (`check`, `pack`, `verify` and `publish`) and facet_new.c (`new`). */
#ifndef DIAMOND_FACET_INTERNAL_H
#define DIAMOND_FACET_INTERNAL_H

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
     * see docs/packages.md for the version-constraint syntax. */
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

/* facet.c */
bool same_file_timestamps(const struct stat *left, const struct stat *right);
bool file_exists(const char *path);
bool valid_sha256(const char *text);
bool valid_registry_url(const char *url);
bool canonical_registry_version(const char *text);
char *read_whole_file(const char *path, char *error, size_t error_size);
bool ensure_directory(const char *path);
bool remove_directory_recursive(const char *path);
bool git_clone(const char *url, const char *ref, const char *destination,
                      char *error, size_t error_size);
bool git_rev_parse_head(const char *repository, char *out, size_t out_size,
                               char *error, size_t error_size);
bool git_list_semver_tags(const char *url, char tags[][FACET_MAX_REF],
        size_t capacity, size_t *out_count, char *error, size_t error_size);
bool pick_best_matching_tag(char tags[][FACET_MAX_REF], size_t tag_count,
        const SemverConstraint *constraint, char *out_tag, size_t out_tag_size);
DiamondManifestValue *facet_read_hash(const char *path, char *error,
                                             size_t error_size);
bool parse_dependencies(const DiamondManifestValue *dependencies,
                               FacetManifest *manifest, char *error,
                               size_t error_size);
bool parse_manifest(const char *path, FacetManifest *manifest,
                           char *error, size_t error_size);
bool parse_cut_identity(const char *path, FacetManifest *manifest,
                               char *error, size_t error_size);
bool parse_lockfile(const char *path, FacetResolution *resolution,
                           char *error, size_t error_size);
bool write_lockfile(const char *path, const FacetResolution *resolution,
                           char *error, size_t error_size);

bool run_git_capture(char *const argv[], const char *context, char *out,
                     size_t out_size, char *error, size_t error_size);

/* facet_new.c */
int cmd_new(int argc, char **argv);

/* facet_resolve.c */
bool resolve_full_graph(const FacetManifest *manifest, FacetResolution *resolution,
        const char *scratch_root, char *error, size_t error_size);
int run_install_or_update(bool force_resolve);

/* facet_package.c */
bool publishable_name(const char *name);
void tar_header(unsigned char header[512], const char *relative,
                       unsigned mode, unsigned long long size);
bool digest_file(const char *path, unsigned char digest[32],
                        char *error, size_t error_size);
bool tar_number(const unsigned char *field, size_t width,
                       unsigned long long *out);
bool all_zero(const unsigned char *bytes, size_t length);
bool valid_archive_path(const char *path);
bool expected_digest(const char *text, const unsigned char actual[32]);
int cmd_verify(int argc, char **argv);
int cmd_check(int argc, char **argv);
int cmd_publish(int argc, char **argv);

#endif
