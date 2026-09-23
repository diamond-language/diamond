#ifndef DIAMOND_MANIFEST_LITERAL_H
#define DIAMOND_MANIFEST_LITERAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum DiamondManifestKind {
    DIAMOND_MANIFEST_OTHER,
    DIAMOND_MANIFEST_INTEGER,
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

#endif
