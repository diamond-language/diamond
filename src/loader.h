#ifndef DIAMOND_LOADER_H
#define DIAMOND_LOADER_H

#include <stddef.h>
#include <stdbool.h>

enum {
    DIAMOND_MAX_SOURCE_SEGMENTS=256,
    DIAMOND_MAX_SOURCE_PATH=4096,
    DIAMOND_MAX_LOADED_FILES=128,
    DIAMOND_MAX_REQUIRE_DEPTH=128
};

typedef struct DiamondSourceSegment {
    size_t start;
    size_t end;
    size_t original_line;
    char path[DIAMOND_MAX_SOURCE_PATH];
} DiamondSourceSegment;

typedef struct DiamondSourceBundle {
    char *source;
    DiamondSourceSegment segments[DIAMOND_MAX_SOURCE_SEGMENTS];
    size_t segment_count;
} DiamondSourceBundle;

bool diamond_load_program(const char *name,const char *source,
                          DiamondSourceBundle *bundle,char *error,
                          size_t error_capacity);
void diamond_source_bundle_free(DiamondSourceBundle *bundle);

#endif
