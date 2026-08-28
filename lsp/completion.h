#ifndef DIAMOND_LSP_COMPLETION_H
#define DIAMOND_LSP_COMPLETION_H

#include "document.h"
#include "json.h"
#include "loader.h"

#include <stddef.h>

/* The real implementation behind completion_compute below, minus the
 * two LSP-protocol-specific pieces (URI parsing, the DocumentTable-
 * shaped require-resolution override) -- reusable by any caller that
 * already has a plain source `path`/`text` and its own resolver, such
 * as the REPL (src/repl.c), which has no URIs or open-document table
 * at all and passes resolver=nullptr,resolver_data=nullptr (already a
 * fully supported "fall back to disk" mode -- see
 * DiamondSourceOverride, src/loader.h) and path=nullptr (a REPL
 * session is never file-backed, the same "untitled document" mode
 * completion_compute itself already has). Computes a
 * textDocument/completion-shaped result for a 0-based `line`/
 * `character` position in `text`. Two kinds of suggestion, unioned
 * into one flat list:
 *
 *   - Every top-level function and class in the whole compiled
 *     program (not just this document's own -- lib/core.di's prelude
 *     and anything pulled in via `require` are all valid to type here,
 *     unlike lsp/document_symbol.c's deliberately narrower "this file's
 *     own outline").
 *   - Every local variable/parameter actually in scope at the cursor
 *     position, via DiamondFunction.scope_locals (src/vm.h) -- real
 *     lexical scoping, not name-matching: a name's own valid byte
 *     range is checked against the cursor's, so a local declared later
 *     in the same function, or one scoped narrowly to a `rescue`
 *     clause the cursor isn't inside, correctly doesn't show up.
 *     Nested-`def`-into-outer-scope capture visibility works too, for
 *     free: an outer function's own recorded range spans everything
 *     nested inside it, including any closures it declares, the same
 *     way the compiler's own capture mechanism (compiler.c's
 *     enclosing_locals) actually resolves them.
 *   - Every method (including inherited ones) on the class(es) a
 *     `receiver.<partial>` expression resolves to, via
 *     receiver_resolve_classes (lsp/receiver.h) -- the same machinery
 *     hover/definition use, for the same statically-known-without-
 *     real-type-inference receiver forms docs/lsp.md describes. A
 *     receiver expression with nothing (or a partial name) typed after
 *     the `.` doesn't compile on its own; callers that want this case
 *     to work (any REPL/editor completion trigger, since that's the
 *     common shape) need to substitute a syntactically-valid
 *     placeholder identifier for the partial text before calling this,
 *     confirmed directly against completion_compute itself: a document
 *     ending in a bare `x.` returns json_null() unless it's first
 *     rewritten to something like `x.__c` (querying at the original,
 *     pre-substitution position still resolves `x`'s real methods
 *     correctly, since receiver_resolve_classes only looks at tokens
 *     strictly before the query offset).
 *
 * Deliberately does not filter by whatever prefix is already typed --
 * every caller (an LSP client, or the REPL) is expected to narrow the
 * returned list itself, the same way completion_compute's own callers
 * already do; narrowing server-side would just duplicate that for no
 * benefit -- nor does it suggest language keywords (`def`, `if`,
 * `end`, ...).
 *
 * Returns a flat `CompletionItem`-shaped JsonValue array
 * (`{"label","kind"}` each -- `kind` is `CompletionItemKind`: 3
 * Function, 6 Variable, 7 Class, 8 Interface, 9 Module), or
 * `json_null()` when the buffer doesn't currently compile cleanly
 * (matching every other lsp/ feature's "no stale result" rule), or
 * nullptr only on allocation failure. */
JsonValue *completion_compute_with_resolver(DiamondSourceOverride resolver,
    void *resolver_data,const char *path,const char *text,size_t length,
    size_t line,size_t character);

/* The LSP-facing entry point: resolves `uri` to a plain path
 * (diagnostics_uri_to_path) and supplies document_resolve_source
 * (lsp/document.h) bound to `documents` (the server's open-document
 * table, so a `require` resolving to another open document sees its
 * live buffer instead of stale on-disk content -- see docs/lsp.md) as
 * the resolver, then delegates to completion_compute_with_resolver
 * above for everything else. */
JsonValue *completion_compute(const DocumentTable *documents,const char *uri,
    const char *text,size_t length,size_t line,size_t character);

#endif
