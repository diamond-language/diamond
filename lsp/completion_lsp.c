#include "completion.h"

#include "diagnostics.h"
#include "document.h"

#include <stdlib.h>

/* Split out of completion.c so that file (and the REPL, src/repl.c,
 * which links it directly for Tab-completion) never needs
 * diagnostics.c/document.c -- neither has any meaning for a REPL
 * session (no URI, no open-document table). This is the only piece
 * that does, so it's the only piece in its own translation unit. */
JsonValue *completion_compute(const DocumentTable *documents,const char *uri,
        const char *text,size_t length,size_t line,size_t character) {
    char *path=diagnostics_uri_to_path(uri);
    JsonValue *result=completion_compute_with_resolver(document_resolve_source,
        (void *)documents,path,text,length,line,character);
    free(path);
    return result;
}
