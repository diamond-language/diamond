#ifndef DIAMOND_LSP_DIAGNOSTICS_H
#define DIAMOND_LSP_DIAGNOSTICS_H

#include "document.h"
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
 * files resolved and bundled in via diamond_load_program_with_override,
 * preferring `documents`' live buffer over disk for any required file
 * that's also open (document_resolve_source, lsp/document.h) -- so an
 * unsaved edit to an open dependency is seen immediately, not just
 * after a save. Returns an LSP `Diagnostic[]` JSON array for `uri`
 * itself: empty on success, or when the one error diamond_compile/
 * diamond_load_program_with_override can report doesn't land in this
 * document (see `out_dependency_publish` below for that case), one
 * entry otherwise. Returns nullptr only on allocation failure.
 *
 * `out_dependency_publish` (optional -- pass nullptr if unneeded, as
 * hover.c/definition.c/document_symbol.c do, none of which call this at
 * all): when the document doesn't currently compile and the error's
 * real location is inside a `require`d file rather than `uri` itself,
 * `*out_dependency_publish` receives a second, fully-formed
 * `{"uri","diagnostics"}` params object naming that file, resolved via
 * diamond_resolve_diagnostic_location's segment table the same way
 * lsp/definition.c resolves a cross-file Location -- publish it as a
 * second `textDocument/publishDiagnostics` notification. Left
 * unchanged (so a caller should initialize it to nullptr first) when
 * there's nothing extra to publish.
 *
 * `out_dependency_paths` (optional -- pass nullptr if unneeded): when
 * the document loads successfully (a bundle was built, whether or not
 * it went on to compile cleanly), `*out_dependency_paths` receives a
 * freshly built `JSON_ARRAY` of `JSON_STRING` entries -- every unique
 * on-disk file path (other than `uri`'s own) that ended up in this
 * document's compiled bundle, direct or transitive `require`s alike.
 * Left unchanged (nullptr) when there's no bundle to report from (an
 * untitled/non-file:// document, or a require-resolution failure) or on
 * allocation failure. lsp/main.c feeds this into a DependencyTable
 * (lsp/dependencies.h) to know which other open documents to re-publish
 * diagnostics for when *this* document itself changes and something
 * else requires it. */
JsonValue *diagnostics_compute(const DocumentTable *documents,const char *uri,
    const char *text,size_t length,
    JsonValue **out_dependency_publish,JsonValue **out_dependency_paths);

#endif
