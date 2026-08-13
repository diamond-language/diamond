#ifndef DIAMOND_LSP_DOCUMENT_H
#define DIAMOND_LSP_DOCUMENT_H

#include <stdbool.h>
#include <stddef.h>

/* The set of currently-open documents, keyed by their LSP URI. A small
 * dynamic array with linear lookup -- an editor session realistically
 * has a handful to a few dozen files open at once, nowhere near enough
 * to need a hash table, and this stays consistent with the rest of this
 * codebase's preference for the simplest structure that fits the actual
 * scale. */
typedef struct DocumentTable DocumentTable;

DocumentTable *document_table_create(void);
void document_table_free(DocumentTable *table);

/* Inserts a newly opened document, or replaces one already open at the
 * same uri (a client re-sending didOpen for an already-tracked uri is
 * treated as a fresh snapshot, not an error). `text` is copied. */
bool document_open(DocumentTable *table,const char *uri,const char *text,size_t length);

/* Replaces the text of an already-open document (full-sync didChange).
 * Returns false without modifying anything if `uri` isn't open. */
bool document_update(DocumentTable *table,const char *uri,const char *text,size_t length);

void document_close(DocumentTable *table,const char *uri);

/* Returns the document's current text (not null-terminated guarantees
 * beyond what the caller copied in -- always treat it as `length` bytes,
 * matching how rpc.c hands bodies around), or nullptr if not open. */
const char *document_get_text(const DocumentTable *table,const char *uri,size_t *length);

#endif
