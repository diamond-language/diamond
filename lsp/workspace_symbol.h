#ifndef DIAMOND_LSP_WORKSPACE_SYMBOL_H
#define DIAMOND_LSP_WORKSPACE_SYMBOL_H

#include "document.h"
#include "json.h"

/* Computes a workspace/symbol result: every top-level function/class,
 * across every *.di file found by recursively walking `workspace_root`
 * (skipping dotfiles/dotdirs -- .git and friends), whose name contains
 * `query` as a case-insensitive substring (an empty query matches
 * everything, same as leaving the picker unfiled in most editors).
 * Each file is compiled independently the same way any open document
 * is (lib/core.di prepended, its own `require`s bundled in via
 * diamond_load_program_with_override) -- only symbols actually
 * declared *in that file itself* are reported, the exact "own
 * declarations only" rule lsp/document_symbol.c already applies to a
 * single open document, applied here across every file in the
 * workspace instead of just one. A file that's also open (and
 * possibly unsaved) is read from its live buffer via document_
 * resolve_source (lsp/document.h) rather than disk, same as every
 * other lsp/ feature that resolves source text.
 *
 * Deliberately no caching across separate workspace/symbol requests --
 * every call re-walks and re-compiles the whole workspace from
 * scratch. Real editors only send this on an explicit "go to symbol in
 * workspace" action (not on every keystroke the way completion is),
 * so this is a real, documented v1 scope cut, not an oversight; see
 * docs/lsp.md.
 *
 * Returns a flat `SymbolInformation[]` JsonValue (`{"name","kind",
 * "location":{"uri","range"}}` each), empty (not `json_null()`) when
 * `workspace_root` is empty/unset (`initialize` never got a
 * `rootUri`/`workspaceFolders`) or nothing matched, or nullptr only on
 * allocation failure. Unlike a textDocument/ feature, a single
 * unparseable file among many doesn't fail the whole request -- it's
 * silently skipped (its own diagnostics, if the client has it open,
 * already say why), the same way one broken file in a big project
 * shouldn't hide every other file's symbols from a workspace-wide
 * search. */
JsonValue *workspace_symbol_compute(const DocumentTable *documents,
    const char *workspace_root,const char *query);

#endif
