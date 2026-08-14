#ifndef DIAMOND_LSP_HOVER_H
#define DIAMOND_LSP_HOVER_H

#include "json.h"

#include <stddef.h>

/* Computes a textDocument/hover result for a 0-based LSP `line`/
 * `character` position in `text` (the open document's current
 * contents, `length` bytes, not necessarily null-terminated -- see
 * lsp/document.h). Resolves only two kinds of identifier, deliberately
 * -- a top-level function name or a class name -- since both are
 * globally unambiguous by construction in this language (a bare call
 * always resolves to exactly one top-level function by that name, `is
 * Foo`/`Foo.new`/`< Foo` always name exactly one class), unlike a
 * method name reached through `receiver.method(...)`, which would need
 * real type inference on `receiver` to know *which* class's method is
 * meant. See hover.c and docs/lsp.md for the reasoning in full.
 *
 * Returns a `{"contents": {"kind": "plaintext", "value": "..."}}`
 * JsonValue on a hit, `json_null()` when the position doesn't land on
 * a hoverable identifier or the document doesn't currently compile
 * cleanly (diagnostics already cover that case; hover just declines),
 * or nullptr only on allocation failure. */
JsonValue *hover_compute(const char *uri,const char *text,size_t length,
    size_t line,size_t character);

#endif
