#ifndef DIAMOND_LSP_REFERENCES_H
#define DIAMOND_LSP_REFERENCES_H

#include "document.h"
#include "json.h"

#include <stddef.h>

/* Computes a textDocument/references result for a 0-based LSP `line`/
 * `character` position in `text` (the open document's current
 * contents): every workspace occurrence of the top-level function,
 * class, module, or interface name under the cursor -- the same "no
 * overloading, single inheritance, globally unambiguous by name"
 * declaration kinds hover/definition/documentSymbol already single out
 * (see lsp/definition.h), scanned across every *.di file under
 * `workspace_root` the same way workspace/symbol does (lsp/
 * workspace_symbol.h's collect_di_files/read_file_preferring_open).
 *
 * Deliberately name-based, not a full alias/shadow-aware reference
 * resolver: a candidate occurrence only counts when it sits in a
 * call/access position (immediately followed by `(` or `.`), a type
 * position (immediately preceded by `:`, `|`, or `<`), or a class/
 * module/interface declaration header (immediately preceded by `class`/
 * `module`/`interface`, the one declaration shape with no following
 * `(`/`.` of its own), and when no lexical local of the same name is in
 * scope there (receiver_name_is_local, lsp/receiver.h) -- ruling out the
 * common false-positive shapes (a keyword-argument label, a hash key, a
 * shadowing local or parameter). It does not rule out the rarer
 * inverse: if the cursor itself sits on a local/parameter that merely
 * happens to share a name with an unrelated global declared elsewhere,
 * this still searches for that global (there is no requirement that the
 * *origin* occurrence itself resolve to the global -- only that a
 * same-named global declaration exists somewhere reachable). A
 * bare-name reference with none of those three adjacent shapes (e.g. a
 * class passed as a first-class value, `puts(ClassName)`) is not found
 * -- a known, deliberate under-approximation in the "conservative"
 * direction: every location this does report is a real occurrence of
 * the name in a resolvable position, but not every real reference is
 * guaranteed to be found. `context.includeDeclaration` is not read; a
 * function's declaration is always included when found (it's always
 * followed by its own `(`), and a class/module/interface's own
 * declaration is now covered by the same header rule above -- the
 * common client default in both cases.
 *
 * Returns a (possibly empty) `Location[]` JsonValue when the identifier
 * under the cursor names a workspace-visible top-level declaration,
 * `json_null()` when the position isn't on such an identifier, the
 * origin document doesn't currently compile cleanly, or `workspace_root`
 * is empty/unset, or nullptr only on allocation failure. Like workspace/
 * symbol, one unparseable file among many is silently skipped rather
 * than failing the whole request. */
JsonValue *references_compute(const DocumentTable *documents,const char *workspace_root,
    const char *uri,const char *text,size_t length,size_t line,size_t character);

#endif
