#ifndef DIAMOND_MANIFEST_LITERAL_H
#define DIAMOND_MANIFEST_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum DiamondManifestKind {
    DIAMOND_MANIFEST_OTHER,
    DIAMOND_MANIFEST_INTEGER,
    DIAMOND_MANIFEST_BOOLEAN,
    DIAMOND_MANIFEST_STRING,
    DIAMOND_MANIFEST_HASH,
    DIAMOND_MANIFEST_ARRAY,
} DiamondManifestKind;

typedef struct DiamondManifestValue {
    DiamondManifestKind kind;
    char *string;
    char *key;
    struct DiamondManifestValue *children;
    struct DiamondManifestValue *next;
} DiamondManifestValue;

/* A cut's manifest is `Cutfile`. Published archives and older projects still
 * use the former name, and those stay readable. */
#define DIAMOND_CUT_MANIFEST "Cutfile"
#define DIAMOND_LEGACY_CUT_MANIFEST "diamond.cut"

typedef enum DiamondCutManifestLookup {
    DIAMOND_CUT_MANIFEST_MISSING,
    DIAMOND_CUT_MANIFEST_FOUND,
    /* Both names exist. Picking one would silently ignore the other. */
    DIAMOND_CUT_MANIFEST_AMBIGUOUS,
    DIAMOND_CUT_MANIFEST_PATH_TOO_LONG,
} DiamondCutManifestLookup;

/* Finds the manifest in `directory`. On FOUND, `path` holds its path. */
DiamondCutManifestLookup diamond_cut_manifest_find(const char *directory, char *path,
                                                   size_t path_size);

/* Parses one data-only Hash. The returned tree owns all decoded strings. */
DiamondManifestValue *diamond_manifest_parse(const char *source, char *error,
                                              size_t error_size);
void diamond_manifest_free(DiamondManifestValue *value);
const DiamondManifestValue *diamond_manifest_get(const DiamondManifestValue *hash,
                                                 const char *key);
bool diamond_manifest_get_string(const DiamondManifestValue *hash, const char *key,
                                 char *out, size_t out_size);
bool diamond_manifest_get_u64(const DiamondManifestValue *hash, const char *key,
                              uint64_t *out);
bool diamond_manifest_get_bool(const DiamondManifestValue *hash, const char *key,
                               bool *out);

#endif
