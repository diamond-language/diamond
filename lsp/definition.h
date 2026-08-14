#ifndef DIAMOND_LSP_DEFINITION_H
#define DIAMOND_LSP_DEFINITION_H

#include "document.h"
#include "json.h"

#include <stddef.h>

/* Computes a textDocument/definition result for a 0-based LSP `line`/
 * `character` position in `text` (the open document's current
 * contents). Resolves the exact same two identifier kinds hover.c
 * does, for the exact same reason (see hover.h) -- a top-level function
 * name or a class name, each globally unambiguous by construction.
 *
 * Unlike hover, a match can legitimately live in a *different* file: if
 * the identifier under the cursor names something declared in a
 * `require`d file rather than the open document itself, the returned
 * Location's own uri names that file, resolved via
 * diamond_resolve_diagnostic_location's segment table (src/compiler.h)
 * -- the same machinery a compile error's own location already goes
 * through. A match declared in lib/core.di's own prelude is
 * deliberately excluded (it has no meaningful open-document-relative
 * position and no editor-visible uri to jump to) -- practically, hover
 * still shows a prelude function's real signature (useful, no location
 * needed), but go-to-definition on the same name returns null rather
 * than trying to point at a file outside the user's own project.
 *
 * Returns a single LSP `Location` (`{"uri", "range"}`) JsonValue on a
 * hit, `json_null()` when the position doesn't land on a hoverable
 * identifier, the identifier doesn't resolve to a function/class
 * declared in the user's own source (as opposed to the prelude), or the
 * document doesn't currently compile cleanly, or nullptr only on
 * allocation failure.
 *
 * `documents` (the server's open-document table) lets a `require`
 * resolving to another open document see its live buffer instead of
 * stale on-disk content, via document_resolve_source (lsp/document.h)
 * -- see docs/lsp.md. */
JsonValue *definition_compute(const DocumentTable *documents,const char *uri,
    const char *text,size_t length,size_t line,size_t character);

#endif
