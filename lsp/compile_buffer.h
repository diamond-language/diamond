#ifndef DIAMOND_LSP_COMPILE_BUFFER_H
#define DIAMOND_LSP_COMPILE_BUFFER_H

#include "loader.h"

#include <stddef.h>

/* Builds the exact buffer diamond_compile expects for an open document
 * -- lib/core.di prepended, a `#line 1` reset, then either `text`
 * as-is (`path==nullptr`: an untitled/non-file:// document, compiled in
 * isolation) or `text` resolved and require-bundled via
 * diamond_load_program (`path`: a real on-disk location). Mirrors
 * diagnostics_compute's own recipe (lsp/diagnostics.c) -- that one
 * stays independent since it also has its own, differently-shaped
 * position-translation-on-failure logic already tangled up with its
 * diagnostic-building, not worth disentangling just for this. Returns
 * nullptr on allocation failure or (silently -- callers that need a
 * reason should get it from diagnostics instead, not duplicate it
 * here) an unresolved require. Caller frees the result.
 *
 * `out_bundle` and `out_user_offset` are both optional (pass nullptr
 * for either/both if unneeded, as hover.c does): if provided,
 * `*out_bundle` receives the require-bundle's own segment table (a
 * zeroed, safe-to-free bundle when `path` was nullptr and no bundling
 * happened at all) and `*out_user_offset` receives the byte offset in
 * the *returned* buffer where the bundled user source begins -- segment
 * offsets in `*out_bundle` are relative to that point, not to the start
 * of the returned buffer. A caller that asks for these takes ownership
 * of `*out_bundle` (diamond_source_bundle_free it) and must keep it
 * (and the returned buffer, since segment offsets are meaningless
 * without it) alive exactly as long as it still needs to resolve
 * positions through it -- this is what lsp/definition.c needs to turn a
 * symbol pulled in from a required file into a Location naming *that*
 * file, via diamond_resolve_diagnostic_location's own segment-mapping
 * (src/compiler.h), the same machinery a compile error's own location
 * already goes through.
 *
 * `override`/`override_data` (both optional, pass nullptr for both if
 * unneeded) are forwarded to diamond_load_program_with_override
 * (loader.h) unchanged -- callers pass document_resolve_source
 * (lsp/document.h) and their DocumentTable* so a require resolving to
 * another open document sees its live buffer instead of stale on-disk
 * content. */
char *diamond_lsp_build_compile_buffer(const char *path,const char *text,size_t length,
    DiamondSourceOverride override,void *override_data,
    DiamondSourceBundle *out_bundle,size_t *out_user_offset);

#endif
