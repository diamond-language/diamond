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
 * position (immediately preceded by `:`, `|`, `<`, or `->`), or a class/
 * module/interface declaration header (immediately preceded by `class`/
 * `module`/`interface`, the one declaration shape with no following
 * `(`/`.` of its own), and when no lexical local of the same name is in
 * scope there (receiver_name_is_local, lsp/receiver.h) -- ruling out the
 * common false-positive shapes (a keyword-argument label, a hash key, a
 * shadowing local or parameter). Explicit receiver calls and non-global
 * function/method declarations are excluded from both origin selection and
 * the workspace scan. A local/parameter origin cannot select a same-named
 * global elsewhere. A
 * bare-name reference with none of those three adjacent shapes (e.g. a
 * class passed as a first-class value, `puts(ClassName)`) is not found
 * -- a known, deliberate under-approximation in the "conservative"
 * direction: every location this does report is a real occurrence of
 * the name in a resolvable position, but not every real reference is
 * guaranteed to be found. `include_declaration` controls whether function
 * declarations and class/module/interface headers (including reopenings)
 * are included. The protocol handler defaults to including declarations
 * when context.includeDeclaration is absent.
 *
 * Returns a (possibly empty) `Location[]` JsonValue when the identifier
 * under the cursor names a workspace-visible top-level declaration,
 * `json_null()` when the position isn't on such an identifier, the
 * origin document doesn't currently compile cleanly, or `workspace_root`
 * is empty/unset, or nullptr only on allocation failure. Like workspace/
 * symbol, one unparseable file among many is silently skipped rather
 * than failing the whole request. */
JsonValue *references_compute(const DocumentTable *documents,const char *workspace_root,
    const char *uri,const char *text,size_t length,size_t line,size_t character,
    bool include_declaration);

/* Computes a textDocument/rename result: a WorkspaceEdit renaming every
 * workspace occurrence of the top-level symbol under the cursor to
 * `new_name` -- the exact same scan, and the exact same "reference-
 * shaped occurrence" false-positive/under-approximation tradeoffs,
 * references_compute's own doc comment above describes (this shares
 * that scan directly, differing only in how the results get built into
 * JSON). Rejects a different target name already used by a top-level
 * function, class, module, or interface in the origin compilation or any
 * successfully compiled workspace file, including imported and prelude
 * symbols. Open documents take precedence over disk contents. Renaming to
 * the current name is allowed. This is a conservative workspace-wide global
 * check; it does not detect capture by same-named locals or methods. Files
 * that cannot be read or compiled are skipped as in the references scan.
 *
 * `new_name` additionally has to lex as exactly one DIAMOND_TOKEN_
 * IDENTIFIER consuming the whole string (rejects empty, a keyword, a
 * qualified `A::B` name, embedded whitespace, ...) -- accepting
 * anything else would hand back an edit guaranteed to leave the
 * workspace not recompiling. Diamond enforces no class-vs-function
 * naming-case convention at the language level (confirmed directly:
 * `class lowercase ... end` compiles), so neither does this -- renaming
 * a class to a lowercase name is accepted the same way declaring one
 * with a lowercase name already is.
 *
 * Returns a WorkspaceEdit (`{"changes": {uri: TextEdit[], ...}}`)
 * JsonValue on success, `json_null()` under every condition
 * references_compute itself returns it for, plus an invalid or colliding
 * `new_name`, and nullptr only on allocation failure. */
JsonValue *rename_compute(const DocumentTable *documents,const char *workspace_root,
    const char *uri,const char *text,size_t length,size_t line,size_t character,
    const char *new_name,size_t new_name_length);

#endif
