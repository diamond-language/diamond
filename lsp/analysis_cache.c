#include "analysis_cache.h"

#include <stdlib.h>
#include <string.h>

static DiamondProgram *cached_program;
static char *cached_source;

const DiamondProgram *diamond_lsp_analyze(const char *combined) {
    if(cached_source!=nullptr&&strcmp(cached_source,combined)==0)
        return cached_program;
    free(cached_source);cached_source=nullptr;
    if(cached_program==nullptr) {
        cached_program=calloc(1,sizeof *cached_program);
        if(cached_program==nullptr)return nullptr;
    }
    DiamondDiagnostic diagnostic;
    if(!diamond_compile_for_tooling(combined,cached_program,&diagnostic))
        return nullptr;
    const size_t length=strlen(combined);
    cached_source=malloc(length+1);
    if(cached_source!=nullptr)memcpy(cached_source,combined,length+1);
    /* A failed key allocation only disables reuse, not the current result. */
    return cached_program;
}

void diamond_lsp_analysis_clear(void) {
    free(cached_source);cached_source=nullptr;
    if(cached_program!=nullptr)diamond_program_free(cached_program);
    free(cached_program);cached_program=nullptr;
}
