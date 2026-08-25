#ifndef DIAMOND_LSP_DIV_H
#define DIAMOND_LSP_DIV_H

#include <stddef.h>
#include <stdbool.h>

/* True iff `path_or_uri` names a div template (packages/div) -- anything
 * ending in ".div", matching packages/div/lib/div/compiler.di's own
 * function_name_for (which strips exactly that suffix and nothing more:
 * a plain ".div" file, not necessarily ".html.div", is a template). Safe
 * to call with either a real filesystem path or a still-percent-encoded
 * uri: '.' is never percent-encoded (see diagnostics_path_to_uri's own
 * "unreserved" set), so the suffix survives either form unchanged. */
bool div_is_template_path(const char *path_or_uri);

/* One generated line's origin in the original .div source (1-based, same
 * convention as DiamondSpan) -- {0,0} means "no single .div source
 * position corresponds to this line" (the fixed escape-helper boilerplate,
 * or the def/sb=.../sb.to_s()/end scaffolding lines div_translate itself
 * adds), which a caller mapping a diagnostic through this should treat as
 * "anchor at the template's own start" instead of trusting the zero. */
typedef struct DivPosition {
    size_t line;
    size_t column;
} DivPosition;

/* Translates `source` (raw .div template text) into ordinary Diamond
 * source implementing the same tag semantics as packages/div/lib/div/
 * compiler.di's own Div.compile_source (see that file's own header
 * comment for the tag grammar) -- a single top-level function taking
 * every name from a `<%# locals: ... %>` directive as its parameters,
 * with a fixed escape helper inlined ahead of it. The generated function
 * and helper are both given fixed internal names (unlike divc.di's own
 * basename-derived ones): nothing outside this one compile ever
 * references them, since this exists purely to type-check a template's
 * own embedded Diamond code for diagnostics, never to actually produce
 * runnable output or interoperate with a real divc-compiled file.
 *
 * On success, returns a malloc'd, null-terminated buffer (caller frees)
 * and fills *out_positions with a malloc'd array (caller frees) of
 * *out_position_count entries, one per '\n'-separated line of the
 * returned buffer (entry i, 0-based, describes generated line i+1,
 * 1-based) -- exactly the shape a compiled buffer's own DiamondSpan.line
 * can index into directly once this translated source is itself compiled
 * with the same prelude+"#line 1" convention diagnostics_compute already
 * uses for a path-less document (see diagnostics.c), since that
 * convention already makes a resulting diagnostic's line/column count
 * within the user text directly, with no segment-table indirection
 * needed the way a require-bundled bundle.source's line/column does.
 *
 * On failure (currently only an unterminated `<%` tag, matching Div.scan's
 * own `raise` in compiler.di), returns nullptr, leaves *out_positions/
 * *out_position_count untouched, and instead fills *out_error_line/
 * *out_error_column (1-based, in `source`) and *out_error_message (a
 * static string, never freed) with a diagnostic the caller can report
 * directly against the original document -- there is no generated buffer
 * to compile at all in this case. */
char *div_translate(const char *source,size_t length,
        DivPosition **out_positions,size_t *out_position_count,
        size_t *out_error_line,size_t *out_error_column,
        const char **out_error_message);

#endif
