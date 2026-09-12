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
/* Same as diamond_program_init, but skips its memset -- ONLY safe when
 * `program` is already all-zero (a freshly calloc'd DiamondProgram
 * calloc's own zero-fill already guarantees this; anything else, e.g.
 * a struct diamond_program_free was just called on for reuse, does
 * NOT and must go through diamond_program_init instead). See
 * diamond_program_init_builtins's own comment (src/compiler.c) for
 * measurements: that memset alone costs several milliseconds per call
 * purely from first-touch page faults across DiamondProgram's ~14MB,
 * paid for nothing when the memory was already zero. */
void diamond_program_init_fresh(DiamondProgram *program);
void diamond_program_free(DiamondProgram *program);
/* Recomputes every class's own shapes[] (self-referential `shape->class`
 * back-pointers) in `program`. A fresh compile always gets this for free
 * from run_compile_pass's own tail (src/compiler.c); anything that
 * populates program->classes[] without running a real compile pass
 * afterward -- diamond_program_init_fresh's own built-in exception
 * classes, or diamond_program_read_compiled (src/compiled_prelude.c)
 * deserializing an already-fully-compiled program into a fresh
 * DiamondProgram at a different memory address -- must call this
 * explicitly or every shape lookup reads stale/dangling `class`
 * pointers instead. */
void diamond_program_recompute_shapes(DiamondProgram *program);
/* Appends one zeroed, independently allocated function record, growing the
 * stable pointer table geometrically without moving existing records. */
DiamondFunction *diamond_program_add_function(DiamondProgram *program);
bool diamond_function_reserve_code(DiamondFunction *function,size_t capacity);
bool diamond_function_reserve_constants(DiamondFunction *function,size_t capacity);
bool diamond_function_reserve_strings(DiamondFunction *function,size_t capacity);
bool diamond_function_reserve_type_sets(DiamondFunction *function,size_t capacity);
bool diamond_function_copy(DiamondFunction *destination,
                           const DiamondFunction *source);
bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic);
/* See its own doc comment in compiler.c: compiles `source` against an
 * already-compiled `template` program (e.g. the prelude alone) without
 * re-parsing whatever produced `template`. */
bool diamond_compile_incremental(const char *source, DiamondProgram *program,
                                 const DiamondProgram *template,
                                 DiamondDiagnostic *diagnostic);
/* Same as diamond_compile, but additionally emits a DIAMOND_OP_DEBUGGER
 * pause (see emit_debugger_pause in compiler.c) at the start of every
 * statement whose first token lands on one of `breakpoint_lines`
 * (`breakpoint_line_count` entries, combined-buffer line numbers -- see
 * diamond_combined_buffer_line below for how a caller gets one of those
 * from a resolved source position) -- the compile-time-breakpoint
 * mechanism the DAP server (dap/main.c) drives. A line with no statement
 * start on it (blank line, comment, mid-expression continuation) has no
 * effect, matching how an editor already snaps a gutter breakpoint to
 * the nearest valid line for most languages. `breakpoint_lines` may be
 * nullptr when `breakpoint_line_count` is 0 (an ordinary compile with no
 * breakpoints requested, distinct from diamond_compile only in that it
 * doesn't need its own separate code path). */
bool diamond_compile_with_breakpoints(const char *source, DiamondProgram *program,
                                      const size_t *breakpoint_lines,
                                      size_t breakpoint_line_count,
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

/* The inverse direction diamond_resolve_source_position doesn't provide:
 * given `offset`, a byte offset into `combined` (the same fully expanded
 * buffer, in the same terms diamond_resolve_source_position's own return
 * value and DiamondSpan.line/chunk->lines[] are already expressed in),
 * returns the 1-based line diamond_compile itself would assign a token
 * at that exact position. *Not* a flat newline count from the start of
 * `combined`: the lexer resets its own line counter at every "#line 1"
 * marker the loader writes ahead of each segment (the top-level user
 * source right after the prelude, and every `require`d file's own
 * inlined text) -- see the implementation's own comment for why this
 * has to recognize that same marker to agree with what the compiler
 * will actually see. Exists for a DAP server (dap/main.c) translating a
 * resolved breakpoint position into the line-number space
 * diamond_compile_with_breakpoints' own breakpoint_lines expects. */
size_t diamond_combined_buffer_line(const char *combined, size_t offset);

#endif
