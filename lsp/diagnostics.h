#ifndef DIAMOND_LSP_DIAGNOSTICS_H
#define DIAMOND_LSP_DIAGNOSTICS_H

#include "json.h"

#include <stddef.h>

/* LSP URIs are `file:///absolute/path`, percent-encoded. Only the
 * `file` scheme is understood -- anything else (an in-memory/untitled
 * buffer with a different scheme) has no on-disk location to resolve
 * against anyway. Returns a freshly malloc'd, null-terminated path, or
 * nullptr if `uri` isn't a `file://` URI or on allocation failure.
 * Shared with hover.c, which needs the same file/no-file distinction to
 * decide whether `require` bundling applies before compiling. */
char *diagnostics_uri_to_path(const char *uri);

/* The inverse of diagnostics_uri_to_path: percent-encodes `path` (every
 * byte outside the unreserved set plus `/`, which stays literal as the
 * path separator) and prepends `file://`. Used by lsp/definition.c to
 * turn a resolved on-disk path (from a `require`d file a symbol turned
 * out to live in, via diamond_resolve_diagnostic_location) into a
 * Location's own uri -- the *requesting* document's already-valid,
 * already-encoded uri is reused directly instead of round-tripping it
 * through here when the resolved path is that same document's own, so
 * this only ever needs to handle a genuinely different file. Returns
 * nullptr only on allocation failure. */
char *diagnostics_path_to_uri(const char *path);

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
