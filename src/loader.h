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

/* Called (if non-null) with a required file's own already-canonicalized
 * (realpath'd) path before diamond_load_program_with_override falls
 * back to reading it from disk. Returns a malloc'd, null-terminated
 * buffer the loader takes ownership of (freed exactly like a disk
 * read) if the caller has a more current version of that file's
 * content to use instead (lsp/'s open-document table, letting a
 * require resolve against a live, possibly-unsaved editor buffer
 * instead of stale on-disk content -- see docs/lsp.md), or nullptr to
 * fall back to reading `path` from disk as normal. `user_data` is
 * passed through unchanged from the diamond_load_program_with_override
 * call that started this expansion. */
typedef char *(*DiamondSourceOverride)(const char *path,void *user_data);

/* Same as diamond_load_program, except a required file's content is
 * resolved through `override` first (see DiamondSourceOverride above)
 * before falling back to disk -- `source` (the root document's own
 * text) is unaffected, since callers already supply that directly.
 * diamond_load_program itself is a thin wrapper passing override=
 * nullptr, unchanged disk-only behavior for every existing caller
 * (main.c, run_source.c, tests, ProgramBuilder's native bridge). */
bool diamond_load_program_with_override(const char *name,const char *source,
                          DiamondSourceOverride override,void *user_data,
                          DiamondSourceBundle *bundle,char *error,
                          size_t error_capacity);

void diamond_source_bundle_free(DiamondSourceBundle *bundle);

#endif
