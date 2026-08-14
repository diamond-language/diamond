#ifndef DIAMOND_LSP_COMPLETION_H
#define DIAMOND_LSP_COMPLETION_H

#include "document.h"
#include "json.h"

#include <stddef.h>

/* Computes a textDocument/completion result for a 0-based LSP `line`/
 * `character` position in `text` (the open document's current
 * contents). Two kinds of suggestion, unioned into one flat list:
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
 *
 * Deliberately does not: filter by whatever prefix is already typed
 * (every mainstream LSP client already narrows a full candidate list
 * live as the user keeps typing -- narrowing server-side would just
 * duplicate that for no benefit), suggest language keywords (`def`,
 * `if`, `end`, ...), or resolve a method name reached through
 * `receiver.method(...)` (same reason hover/definition/documentSymbol
 * don't -- needs type inference on `receiver` this project doesn't
 * have; see hover.h).
 *
 * Returns a flat `CompletionItem[]` JsonValue (`{"label","kind"}`
 * each -- `kind` is `CompletionItemKind`: 3 Function, 6 Variable, 7
 * Class), or `json_null()` when the document doesn't currently compile
 * cleanly (matching every other lsp/ feature's "no stale result"
 * rule), or nullptr only on allocation failure.
 *
 * `documents` (the server's open-document table) lets a `require`
 * resolving to another open document see its live buffer instead of
 * stale on-disk content, via document_resolve_source (lsp/document.h)
 * -- see docs/lsp.md. */
JsonValue *completion_compute(const DocumentTable *documents,const char *uri,
    const char *text,size_t length,size_t line,size_t character);

#endif
