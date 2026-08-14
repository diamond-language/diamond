#ifndef DIAMOND_LSP_DOCUMENT_SYMBOL_H
#define DIAMOND_LSP_DOCUMENT_SYMBOL_H

#include "document.h"
#include "json.h"

#include <stddef.h>

/* Computes a textDocument/documentSymbol result: every top-level
 * function and class actually declared *in this document* -- not
 * lib/core.di's prelude, and not anything pulled in through `require`
 * (an outline view showing symbols that aren't actually in the file
 * would be more confusing than useful; require'd symbols are each
 * their own document's own outline instead, one didOpen/didChange
 * away). Same two identifier kinds hover.c and definition.c resolve,
 * for the same reason (see hover.h) -- this just lists all of them
 * instead of resolving one at a cursor position.
 *
 * Returns a flat `DocumentSymbol[]` JsonValue (`{"name","kind","range",
 * "selectionRange"}` each; `range`/`selectionRange` are identical --
 * both just the name token, since nothing here tracks a declaration's
 * full extent, only where its name starts) -- empty, not `json_null()`,
 * when the document has no top-level functions/classes of its own, so
 * a client can always tell "empty outline" apart from "document
 * doesn't compile" (`json_null()`, the same as hover/definition use for
 * that). Returns nullptr only on allocation failure.
 *
 * `documents` (the server's open-document table) lets a `require`
 * resolving to another open document see its live buffer instead of
 * stale on-disk content, via document_resolve_source (lsp/document.h)
 * -- see docs/lsp.md. */
JsonValue *document_symbol_compute(const DocumentTable *documents,const char *uri,
    const char *text,size_t length);

#endif
