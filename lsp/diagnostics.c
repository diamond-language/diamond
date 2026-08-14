#include "diagnostics.h"

#include "compiler.h"
#include "loader.h"

#include <stdio.h>
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

char *diagnostics_uri_to_path(const char *uri) {
    static constexpr char prefix[]="file://";
    static constexpr size_t prefix_length=sizeof(prefix)-1;
    if(strncmp(uri,prefix,prefix_length)!=0)return nullptr;
    const char *encoded=uri+prefix_length;
    const size_t encoded_length=strlen(encoded);
    char *path=malloc(encoded_length+1);
    if(path==nullptr)return nullptr;
    size_t out=0;
    for(size_t index=0;index<encoded_length;index++) {
        if(encoded[index]=='%'&&index+2<encoded_length) {
            const char high=encoded[index+1],low=encoded[index+2];
            const bool high_hex=(high>='0'&&high<='9')||(high>='a'&&high<='f')||
                (high>='A'&&high<='F');
            const bool low_hex=(low>='0'&&low<='9')||(low>='a'&&low<='f')||
                (low>='A'&&low<='F');
            if(high_hex&&low_hex) {
                unsigned value=0;
                sscanf((char[]){high,low,'\0'},"%x",&value);
                path[out++]=(char)value;
                index+=2;
                continue;
            }
        }
        path[out++]=encoded[index];
    }
    path[out]='\0';
    return path;
}

static JsonValue *build_diagnostic(size_t line,size_t column,size_t highlight_length,
                                   const char *message) {
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
    const double lsp_line=line>0?(double)(line-1):0;
    const double lsp_character=column>0?(double)(column-1):0;
    const double highlight_width=highlight_length>0?(double)highlight_length:1;
    json_object_set(start,"line",json_number(lsp_line));
    json_object_set(start,"character",json_number(lsp_character));
    json_object_set(end,"line",json_number(lsp_line));
    json_object_set(end,"character",json_number(lsp_character+highlight_width));
    json_object_set(range,"start",start);
    json_object_set(range,"end",end);
    json_object_set(entry,"range",range);
    json_object_set(entry,"severity",json_number(1));
    json_object_set(entry,"source",json_string_z("diamond"));
    json_object_set(entry,"message",json_string_z(message));
    return entry;
}

JsonValue *diagnostics_compute(const char *uri,const char *text,size_t length) {
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

    JsonValue *diagnostics=json_array();
    if(diagnostics==nullptr)return nullptr;

    /* No on-disk location (an untitled/unsaved buffer, or a non-file://
     * scheme): compile the document in isolation, same as before this
     * required-file support existed. A bare `require` line in such a
     * document still can't resolve (there's no directory to resolve it
     * against), but that's the same "no false diagnostic for valid code,
     * no cross-file resolution either" tradeoff every path through this
     * function makes for a *different* file's own errors -- see
     * docs/lsp.md. */
    char *path=diagnostics_uri_to_path(uri);
    if(path==nullptr) {
        char *combined=malloc(sizeof(DIAMOND_CORE_SOURCE)-1+sizeof(DIAMOND_USER_LINE_RESET)-1+length+1);
        if(combined==nullptr) {
            json_free(diagnostics);
            return nullptr;
        }
        const size_t core_length=sizeof(DIAMOND_CORE_SOURCE)-1;
        const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
        memcpy(combined,DIAMOND_CORE_SOURCE,core_length);
        memcpy(combined+core_length,DIAMOND_USER_LINE_RESET,reset_length);
        memcpy(combined+core_length+reset_length,text,length);
        combined[core_length+reset_length+length]='\0';
        DiamondDiagnostic diagnostic;
        const bool ok=diamond_compile(combined,scratch,&diagnostic);
        free(combined);
        if(!ok) {
            JsonValue *entry=build_diagnostic(diagnostic.span.line,diagnostic.span.column,
                diagnostic.span.length,diagnostic.message);
            if(entry==nullptr||!json_array_push(diagnostics,entry)) {
                json_free(entry);
                json_free(diagnostics);
                return nullptr;
            }
        }
        return diagnostics;
    }

    /* A null-terminated copy: document_get_text's own buffer is already
     * null-terminated, but diamond_load_program takes a plain `const
     * char *`, and length-bounding it against `length` here (rather than
     * trusting the caller's null terminator) keeps this function correct
     * even if that guarantee ever changes upstream. */
    char *text_copy=malloc(length+1);
    if(text_copy==nullptr) {
        free(path);
        json_free(diagnostics);
        return nullptr;
    }
    memcpy(text_copy,text,length);
    text_copy[length]='\0';

    DiamondSourceBundle bundle;
    char load_error[768];
    const bool loaded=diamond_load_program(path,text_copy,&bundle,load_error,sizeof load_error);
    free(text_copy);
    if(!loaded) {
        free(path);
        /* No resolvable range for a require-resolution failure (the
         * message is prose, not a machine-parseable location) -- anchor
         * it at the document's own start rather than reporting nothing,
         * since this is a real problem with *this* document (an
         * unresolvable require), not a downstream file's own error. */
        JsonValue *entry=build_diagnostic(1,1,1,load_error);
        if(entry==nullptr||!json_array_push(diagnostics,entry)) {
            json_free(entry);
            json_free(diagnostics);
            return nullptr;
        }
        return diagnostics;
    }

    const size_t core_length=sizeof(DIAMOND_CORE_SOURCE)-1;
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    const size_t bundle_length=strlen(bundle.source);
    char *combined=malloc(core_length+reset_length+bundle_length+1);
    if(combined==nullptr) {
        free(path);
        diamond_source_bundle_free(&bundle);
        json_free(diagnostics);
        return nullptr;
    }
    memcpy(combined,DIAMOND_CORE_SOURCE,core_length);
    memcpy(combined+core_length,DIAMOND_USER_LINE_RESET,reset_length);
    memcpy(combined+core_length+reset_length,bundle.source,bundle_length+1);

    DiamondDiagnostic diagnostic;
    const bool ok=diamond_compile(combined,scratch,&diagnostic);
    if(!ok) {
        const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
            path,combined,diagnostic,&bundle,core_length+reset_length);
        /* Only report a diagnostic that actually lands in *this*
         * document -- one resolved to a different (required) file's own
         * source is a real error, but publishing it against this
         * document's uri would point an editor at the wrong file. See
         * docs/lsp.md for why that's left for a later slice rather than
         * also publishing against the dependency's own uri here. */
        if(strcmp(resolved.path,path)==0) {
            JsonValue *entry=build_diagnostic(resolved.line,resolved.column,
                diagnostic.span.length,diagnostic.message);
            if(entry==nullptr||!json_array_push(diagnostics,entry)) {
                json_free(entry);
                free(combined);
                free(path);
                diamond_source_bundle_free(&bundle);
                json_free(diagnostics);
                return nullptr;
            }
        }
    }
    free(combined);
    free(path);
    diamond_source_bundle_free(&bundle);
    return diagnostics;
}
