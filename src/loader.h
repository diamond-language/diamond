#ifndef DIAMOND_LOADER_H
#define DIAMOND_LOADER_H

#include <stddef.h>
#include <stdbool.h>

enum {
    /* Kept equal to DIAMOND_MAX_LOADED_FILES below -- a segment is
     * recorded at least once per loaded file (record_segment,
     * src/loader.c), so a segment cap lower than the file cap becomes
     * the real, more confusing bottleneck the moment a program's
     * files individually contain enough of their own nested requires
     * to split into more than one segment each (confirmed directly:
     * raising DIAMOND_MAX_LOADED_FILES alone, without also raising
     * this, left a large-distinct-file test fixture failing with
     * "source-file segment limit reached" instead of the file-count
     * error it was actually testing for).
     *
     * 400, not something rounder like 512: kept equal to
     * DIAMOND_MAX_LOADED_FILES per this comment's own first paragraph,
     * not chosen for any remaining stack-budget reason -- see
     * DiamondSourceBundle's own `segments` field below for why a
     * segment count this large no longer costs any stack at all. */
    DIAMOND_MAX_SOURCE_SEGMENTS=400,
    DIAMOND_MAX_SOURCE_PATH=4096,
    /* A real, growing app (applications/skindicate.dia -- ~50 files of
     * its own plus everything require'd transitively across every
     * package it pulls in, active_record/arel/rack/... included) hit
     * the old value of 128 outright once packages/websocket joined
     * that list, with no cycle or accidental double-require involved
     * -- just ordinary growth. Loader (this file's own Loader struct,
     * src/loader.c) holds `loaded`/`active` as
     * [DIAMOND_MAX_LOADED_FILES/DIAMOND_MAX_REQUIRE_DEPTH][DIAMOND_MAX_SOURCE_PATH]
     * arrays, now heap-allocated (not embedded in the Loader struct
     * itself) specifically so raising this has no stack cost at all --
     * see diamond_load_program_with_override's own comment. The actual
     * ceiling on how far this constant (and DIAMOND_MAX_SOURCE_SEGMENTS
     * above, kept equal to it) can safely go is instead set by
     * DiamondSourceBundle's own still-stack-resident segments array --
     * see that constant's own comment for the empirical bisection this
     * value came from.
     */
    DIAMOND_MAX_LOADED_FILES=400,
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
    /* Heap-allocated (DIAMOND_MAX_SOURCE_SEGMENTS entries, by
     * diamond_load_program_with_override) rather than embedded, the same
     * fix already applied to Loader's own loaded/active arrays
     * (src/loader.c) and for the identical reason: several callers
     * (run_source.c, repl.c, several lsp/ files) declare a plain
     * `DiamondSourceBundle bundle;` local, and this field alone used to
     * be ~1.6MB (400 * sizeof(DiamondSourceSegment), dominated by each
     * entry's own DIAMOND_MAX_SOURCE_PATH-sized path buffer) --
     * confirmed as a real stack overflow (SIGSEGV) on an -O0 debug
     * build even at zero `require` depth, well before
     * DIAMOND_MAX_REQUIRE_DEPTH's own recursive-expand()-frame concern
     * ever enters into it (two independent `DiamondSourceBundle bundle`
     * locals alone -- one in an lsp/ handler, another inside
     * lsp/compile_buffer.c's own call into it -- were already enough).
     * nullptr/0 (a zero-initialized DiamondSourceBundle, or one a
     * caller populated without ever calling
     * diamond_load_program_with_override, such as
     * lsp/compile_buffer.c's own path==nullptr branch) is a valid empty
     * bundle; every reader of this field loops `for(i=0;i<segment_count;
     * i++)`, so segments is never dereferenced when segment_count is 0.
     * Freed by diamond_source_bundle_free alongside `source`. */
    DiamondSourceSegment *segments;
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
