#ifndef DIAMOND_LSP_DIAGNOSTICS_H
#define DIAMOND_LSP_DIAGNOSTICS_H

#include "json.h"

#include <stddef.h>

/* Compiles `text` (a document's current contents) the same way src/main.c's
 * own run_source does -- lib/core.di prepended, so a document's own
 * top-level functions can call anything the prelude defines the way they
 * always could natively -- and returns an LSP `Diagnostic[]` JSON array:
 * empty on success, one entry on failure (diamond_compile stops at the
 * first error, so there's never more than one to report). Returns
 * nullptr only on allocation failure. */
JsonValue *diagnostics_compute(const char *text,size_t length);

#endif
