#ifndef DIAMOND_LSP_DEPENDENCIES_H
#define DIAMOND_LSP_DEPENDENCIES_H

#include "json.h"

#include <stdbool.h>
#include <stddef.h>

/* The reverse of "what does this document require": for every open
 * document, which on-disk file paths its last-computed compiled bundle
 * actually included (direct or transitive `require`s alike -- see
 * diagnostics_compute's own out_dependency_paths, lsp/diagnostics.h).
 * Lets lsp/main.c answer "which open documents need their diagnostics
 * refreshed" when a *particular* file changes, without re-deriving
 * every open document's dependency set from scratch on every edit. A
 * flat array of (uri, path) edges with linear scan/update -- same
 * "simplest structure that fits the actual scale" reasoning as
 * lsp/document.h's own DocumentTable; nowhere near enough open
 * documents or requires-per-document in a real editor session to need
 * anything smarter. */
typedef struct DependencyTable DependencyTable;

DependencyTable *dependency_table_create(void);
void dependency_table_free(DependencyTable *table);

/* Replaces every recorded dependency edge for `uri` with the unique
 * paths in `paths` (a JSON_ARRAY of JSON_STRING entries, exactly the
 * shape diagnostics_compute's out_dependency_paths produces) --
 * removes `uri`'s old edges first, so a document that no longer
 * requires something it used to correctly stops being tracked as
 * depending on it. `paths` is borrowed (copies whatever it needs, does
 * not free or retain it); passing nullptr or an empty array just
 * clears `uri`'s edges. Returns false only on allocation failure,
 * having still cleared `uri`'s old edges either way. */
bool dependency_table_update(DependencyTable *table,const char *uri,
    const JsonValue *paths);

/* Removes every edge for `uri` -- call on didClose, so a closed
 * document is never returned as a dependent needing a republish it can
 * no longer receive (didClose already clears its diagnostics
 * separately, lsp/main.c). */
void dependency_table_remove_document(DependencyTable *table,const char *uri);

/* Returns a freshly malloc'd array of `*out_count` freshly malloc'd,
 * null-terminated uri strings: every open document whose last-recorded
 * dependency set (dependency_table_update) included `path`. Chains of
 * requires need no special handling here -- if X requires A requires
 * B, X's own last-recorded dependency set already includes B directly
 * (diamond_load_program's bundle segments are fully transitive), so a
 * single lookup on B already returns both A and X. Independently owned
 * and decoupled from the table's own internal storage (not borrowed
 * pointers into it), so it's safe to keep using after further
 * dependency_table_update/remove_document calls -- lsp/main.c relies
 * on this, since republishing a dependent's diagnostics updates its
 * own dependency edges mid-iteration. Caller frees each string, then
 * the array itself (nullptr array is possible on allocation failure;
 * *out_count is 0 in that case too, so a caller can loop unconditionally
 * either way). */
char **dependency_table_dependents(const DependencyTable *table,const char *path,
    size_t *out_count);

#endif
