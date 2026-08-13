#include "diagnostics.h"

#include "compiler.h"

#include <stdlib.h>
#include <string.h>

/* Mirrors src/main.c's own run_source recipe exactly: lib/core.di,
 * embedded at compile time the same way main.c embeds it (`lsp/` sits at
 * the same directory depth as `src/`, so the relative path is
 * unchanged), then a "#line 1" reset so line numbers in a compiled
 * diagnostic are 1-based from the document's own first line -- confirmed
 * against src/lexer.c's handling of that exact comment, not assumed. */
static constexpr unsigned char DIAMOND_CORE_SOURCE[] = {
#embed "../lib/core.di" suffix(,)
    0
};
static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

static JsonValue *build_diagnostic(DiamondDiagnostic diagnostic) {
    JsonValue *entry=json_object();
    if(entry==nullptr)return nullptr;
    JsonValue *range=json_object();
    JsonValue *start=json_object();
    JsonValue *end=json_object();
    if(range==nullptr||start==nullptr||end==nullptr) {
        json_free(range);
        json_free(start);
        json_free(end);
        json_free(entry);
        return nullptr;
    }
    /* Diamond's own line/column are 1-based; LSP positions are 0-based.
     * There's no end position in a DiamondDiagnostic (compiler.c reports
     * a span's start, not a resolved end line/column), so the end
     * position highlights the span's own byte length on the same line --
     * exactly right for the common case (a single token), an
     * underestimate for a span that itself contains a newline, which no
     * diagnostic message in this compiler currently produces. */
    const double line=diagnostic.span.line>0?(double)(diagnostic.span.line-1):0;
    const double character=diagnostic.span.column>0?(double)(diagnostic.span.column-1):0;
    const double highlight_width=diagnostic.span.length>0?(double)diagnostic.span.length:1;
    json_object_set(start,"line",json_number(line));
    json_object_set(start,"character",json_number(character));
    json_object_set(end,"line",json_number(line));
    json_object_set(end,"character",json_number(character+highlight_width));
    json_object_set(range,"start",start);
    json_object_set(range,"end",end);
    json_object_set(entry,"range",range);
    json_object_set(entry,"severity",json_number(1));
    json_object_set(entry,"source",json_string_z("diamond"));
    json_object_set(entry,"message",json_string_z(diagnostic.message));
    return entry;
}

JsonValue *diagnostics_compute(const char *text,size_t length) {
    /* DiamondProgram is tens of MB (fixed-size arrays sized for
     * self-hosting-scale programs, per src/compiler.h's own comment on
     * the struct) -- heap-allocated here for the same reason main.c
     * heap-allocates it rather than putting it on the stack, and kept
     * across calls (lazily allocated once, reused for every subsequent
     * didOpen/didChange) since a fresh malloc+free of that size on every
     * keystroke would be wasteful for no benefit: diamond_compile always
     * re-initializes it from scratch via diamond_program_init before
     * compiling. */
    static DiamondProgram *scratch=nullptr;
    if(scratch==nullptr) {
        scratch=malloc(sizeof *scratch);
        if(scratch==nullptr)return nullptr;
    }
    const size_t core_length=sizeof(DIAMOND_CORE_SOURCE)-1;
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    char *combined=malloc(core_length+reset_length+length+1);
    if(combined==nullptr)return nullptr;
    memcpy(combined,DIAMOND_CORE_SOURCE,core_length);
    memcpy(combined+core_length,DIAMOND_USER_LINE_RESET,reset_length);
    memcpy(combined+core_length+reset_length,text,length);
    combined[core_length+reset_length+length]='\0';

    DiamondDiagnostic diagnostic;
    const bool ok=diamond_compile(combined,scratch,&diagnostic);
    free(combined);

    JsonValue *diagnostics=json_array();
    if(diagnostics==nullptr)return nullptr;
    if(!ok) {
        JsonValue *entry=build_diagnostic(diagnostic);
        if(entry==nullptr) {
            json_free(diagnostics);
            return nullptr;
        }
        if(!json_array_push(diagnostics,entry)) {
            json_free(entry);
            json_free(diagnostics);
            return nullptr;
        }
    }
    return diagnostics;
}
