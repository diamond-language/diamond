#ifndef DIAMOND_LSP_DIAGNOSTICS_H
#define DIAMOND_LSP_DIAGNOSTICS_H

#include "json.h"

#include <stddef.h>

/* Compiles `text` (a document's current contents, identified by its LSP
 * `uri` so `require` resolves relative to the right on-disk directory)
 * the same way src/main.c's own run_source does -- lib/core.di prepended
 * (so a document's own top-level functions can call anything the
 * prelude defines the way they always could natively) and `require`d
 * files resolved and bundled in via diamond_load_program (read from
 * disk -- only the document identified by `uri` itself reflects
 * possibly-unsaved editor content). Returns an LSP `Diagnostic[]` JSON
 * array: empty on success or when the one error diamond_compile/
 * diamond_load_program can report doesn't land in this document itself
 * (a broken `require`d file reports nothing here yet -- see
 * docs/lsp.md), one entry otherwise. Returns nullptr only on allocation
 * failure. */
JsonValue *diagnostics_compute(const char *uri,const char *text,size_t length);

#endif
