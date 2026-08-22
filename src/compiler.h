#ifndef DIAMOND_COMPILER_H
#define DIAMOND_COMPILER_H

#include "lexer.h"
#include "loader.h"
#include "vm.h"

typedef struct DiamondProgram {
    /* REPL-only compatibility; ordinary source still rejects duplicates. */
    bool allow_top_level_redefinition;
    DiamondFunction entry;
    /* Root chunk display name, kept separate from entry.name: that field
     * shares DiamondFunction's DIAMOND_MAX_FUNCTION_NAME (64-byte) buffer
     * with every ordinary function/class/field name, but a real source
     * path (as set by the ProgramBuilder#expand_source native bridge, see
     * src/vm.c) routinely exceeds that. diamond_program_chunk() prefers
     * this field over entry.name when it's set. Native's own run_source
     * (src/main.c) sidesteps the whole problem by overwriting chunk.name
     * with its own unbounded `name` pointer after compiling; the bridge
     * has no equivalent post-compile hook, so it needs a field to persist
     * the untruncated path across expand_source and the later run. */
    char entry_path[DIAMOND_MAX_SOURCE_PATH];
    DiamondFunction **functions;
    size_t function_count;
    size_t function_capacity;
    DiamondClass classes[DIAMOND_MAX_CLASSES];
    size_t class_count;
    DiamondInterface interfaces[DIAMOND_MAX_INTERFACES];
    size_t interface_count;
    DiamondModule modules[DIAMOND_MAX_MODULES];
    size_t module_count;
    char namespace_constants[DIAMOND_MAX_NAMESPACE_CONSTANTS]
                            [DIAMOND_MAX_FUNCTION_NAME];
    size_t namespace_constant_count;
    /* See DiamondChunk's own copy of this field (src/vm.h) for what it's
     * for -- resolved once at the end of diamond_compile, UINT8_MAX
     * (set in diamond_program_init, alongside every other zero-init
     * default) until then/if never found. */
    uint8_t range_class_index;
} DiamondProgram;

typedef struct DiamondDiagnostic {
    DiamondSpan span;
    const char *message;
} DiamondDiagnostic;

/* Zeroes *program and populates the fixed built-in exception-class table.
 * Call diamond_program_free before reinitializing an already compiled program
 * and once more when its final compiled contents are no longer needed.
 * (Exception/StandardError/TypeError/.../RegexpError) with valid shapes,
 * so the result is immediately safe to run even before any user code is
 * compiled or emitted into it -- shared by diamond_compile and the
 * ProgramBuilder native bridge (src/vm.c), which needs the same baseline
 * without going through the parser at all. See docs/roadmap.md. */
void diamond_program_init(DiamondProgram *program);
void diamond_program_free(DiamondProgram *program);
/* Appends one zeroed, independently allocated function record, growing the
 * stable pointer table geometrically without moving existing records. */
DiamondFunction *diamond_program_add_function(DiamondProgram *program);
bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic);
DiamondChunk diamond_program_chunk(const DiamondProgram *program);

typedef struct DiamondResolvedLocation {
    /* Either `name` unchanged (the diagnostic landed in the entry file
     * itself) or one of `bundle`'s own segment paths (it landed inside a
     * `require`d file that got inlined into the bundled source) --
     * never separately allocated, always borrowed from one of those two
     * existing sources. */
    const char *path;
    size_t line;
    size_t column;
    /* Byte offsets into `source` bracketing the offending line, for
     * printing it back out (e.g. with a caret under the column) --
     * excludes the trailing newline/EOF. */
    size_t line_start;
    size_t line_end;
} DiamondResolvedLocation;

/* Maps a diagnostic's span (an offset into `source`, the fully expanded
 * buffer diamond_compile actually saw -- core prelude + reset + bundled
 * user source) back to whichever original file and line it actually
 * came from, using `bundle`'s segment table the same way
 * diamond_load_program built it. `user_offset` is the byte offset where
 * the bundled user source begins within `source` (i.e. where the
 * prelude+reset prefix ends) -- segment offsets are relative to the
 * bundle's own source, not `source` as a whole. `name` is the entry
 * file's own display name/path, used verbatim when the diagnostic never
 * left it. Extracted from src/main.c's own print_diagnostic so a second
 * caller (the LSP) doesn't have to reimplement this segment-mapping
 * arithmetic -- src/main.c's own diagnostic printing is now a thin
 * formatting layer over this. */
DiamondResolvedLocation diamond_resolve_diagnostic_location(
    const char *name, const char *source, DiamondDiagnostic diagnostic,
    const DiamondSourceBundle *bundle, size_t user_offset);

/* The inverse of diamond_resolve_diagnostic_location: given a 1-based
 * `line`/`column` in `path`'s own original source text, finds the
 * matching byte offset within `combined` (the fully expanded buffer
 * diamond_compile actually saw) -- i.e. where that exact position
 * ended up after prelude-prepending and require-bundling. Only ever
 * considers segments whose own path equals `path` (a document can
 * appear as more than one segment, interleaved with whatever it
 * `require`s between them, if it has more than one `require` line);
 * `user_offset` means the same thing it does above. Returns SIZE_MAX
 * if `line` doesn't fall inside any segment belonging to `path` --
 * shouldn't happen for a real position within a document that's part
 * of a bundle that compiled successfully, but a defensive result
 * either way, not a crash. lsp/completion.c's only reason for
 * existing: mapping an LSP cursor position (always expressed in the
 * open document's own, unbundled text) into the compiled-buffer byte
 * offsets DiamondFunction.scope_locals (src/vm.h) is expressed in. */
size_t diamond_resolve_source_position(const char *path, const char *combined,
    const DiamondSourceBundle *bundle, size_t user_offset,
    size_t line, size_t column);

#endif
