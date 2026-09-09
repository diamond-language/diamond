/* See lsp/completion.c's own identical comment. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "diagnostics.h"

#include "compiler.h"
#include "div.h"
#include "loader.h"
#include "prelude.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

char *diagnostics_path_to_uri(const char *path) {
    static constexpr char prefix[]="file://";
    static constexpr size_t prefix_length=sizeof(prefix)-1;
    static constexpr char hex_digits[]="0123456789ABCDEF";
    const size_t path_length=strlen(path);
    char *uri=malloc(prefix_length+path_length*3+1);
    if(uri==nullptr)return nullptr;
    memcpy(uri,prefix,prefix_length);
    size_t out=prefix_length;
    for(size_t index=0;index<path_length;index++) {
        const unsigned char byte=(unsigned char)path[index];
        const bool unreserved=(byte>='A'&&byte<='Z')||(byte>='a'&&byte<='z')||
            (byte>='0'&&byte<='9')||byte=='-'||byte=='_'||byte=='.'||byte=='~'||
            byte=='/';
        if(unreserved) {
            uri[out++]=(char)byte;
        } else {
            uri[out++]='%';
            uri[out++]=hex_digits[byte>>4];
            uri[out++]=hex_digits[byte&0xF];
        }
    }
    uri[out]='\0';
    return uri;
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

/* Builds the `{"uri","diagnostics":[one entry]}` params object
 * out_dependency_publish documents -- naming `resolved`'s own file,
 * not the requesting document. Returns nullptr on allocation failure
 * (the caller already has a real diagnostics array to return either
 * way, so this failing just means no dependency publish this round,
 * not a hard error). */
static JsonValue *build_dependency_publish(const DiamondResolvedLocation *resolved,
        size_t highlight_length,const char *message) {
    char *dependency_uri=diagnostics_path_to_uri(resolved->path);
    if(dependency_uri==nullptr)return nullptr;
    JsonValue *entry=build_diagnostic(resolved->line,resolved->column,
        highlight_length,message);
    JsonValue *dependency_diagnostics=json_array();
    JsonValue *params=json_object();
    if(entry==nullptr||dependency_diagnostics==nullptr||params==nullptr||
       !json_array_push(dependency_diagnostics,entry)) {
        free(dependency_uri);
        json_free(entry);json_free(dependency_diagnostics);json_free(params);
        return nullptr;
    }
    json_object_set(params,"uri",json_string_z(dependency_uri));
    json_object_set(params,"diagnostics",dependency_diagnostics);
    free(dependency_uri);
    return params;
}

/* Builds *out_dependency_paths per diagnostics.h's own comment: every
 * unique path in `bundle`'s segments other than `own_path`. `bundle`
 * always has at least one segment (the document's own, even with zero
 * requires -- expand() in src/loader.c always records one for whatever
 * it copies in), so this only ever adds *other* files' paths. Leaves
 * *out_dependency_paths untouched on allocation failure -- the
 * caller's own diagnostics array is still real and worth returning
 * either way, same tradeoff build_dependency_publish's own caller
 * already makes. */
static void build_dependency_paths(const DiamondSourceBundle *bundle,
        const char *own_path,JsonValue **out_dependency_paths) {
    JsonValue *paths=json_array();
    if(paths==nullptr)return;
    for(size_t index=0;index<bundle->segment_count;index++) {
        const char *segment_path=bundle->segments[index].path;
        if(strcmp(segment_path,own_path)==0)continue;
        bool seen=false;
        for(size_t existing=0;existing<paths->as.array.count&&!seen;existing++)
            seen=strcmp(paths->as.array.items[existing]->as.string.chars,
                segment_path)==0;
        if(seen)continue;
        JsonValue *entry=json_string_z(segment_path);
        if(entry==nullptr||!json_array_push(paths,entry)) {
            json_free(entry);
            json_free(paths);
            return;
        }
    }
    *out_dependency_paths=paths;
}

JsonValue *diagnostics_compute(const DocumentTable *documents,const char *uri,
        const char *text,size_t length,
        JsonValue **out_dependency_publish,JsonValue **out_dependency_paths) {
    /* DiamondProgram is tens of MB (fixed-size arrays sized for
     * self-hosting-scale programs, per src/compiler.h's own comment on
     * the struct) -- heap-allocated here for the same reason main.c
     * heap-allocates it rather than putting it on the stack, and kept
     * across calls (lazily allocated once, reused for every subsequent
     * didOpen/didChange) since a fresh malloc+free of that size on every
     * keystroke would be wasteful for no benefit. Each compile below
     * still needs an explicit diamond_program_free right before it: the
     * function table is independently heap-allocated, and
     * diamond_program_init's memset alone would leak the previous
     * keystroke's functions instead of freeing them. */
    static DiamondProgram *scratch=nullptr;
    if(scratch==nullptr) {
        scratch=calloc(1,sizeof *scratch);
        if(scratch==nullptr)return nullptr;
    }

    JsonValue *diagnostics=json_array();
    if(diagnostics==nullptr)return nullptr;

    /* A div template (packages/div, any ".div" path) never `require`s
     * anything of its own -- see lsp/div.h's own comment -- so it skips
     * the require-bundling path entirely: translate its tag syntax into
     * ordinary Diamond source with div_translate, compile *that* exactly
     * the way the no-on-disk-location branch below compiles raw text
     * (prelude + "#line 1" reset, diagnostic.span.line indexing directly
     * into the user content with no segment-table indirection needed),
     * then map any resulting diagnostic's line back through the position
     * array div_translate returned instead of through diamond_resolve_
     * diagnostic_location's bundle segments (there is no bundle here).
     * No dependency tracking either (out_dependency_publish/
     * out_dependency_paths stay untouched, same as every early-return
     * path below already leaves them) -- a template has nothing to
     * `require`, so nothing to publish or track. */
    if(div_is_template_path(uri)) {
        DivPosition *positions=nullptr;size_t position_count=0;
        size_t error_line=1,error_column=1;const char *error_message=nullptr;
        char *generated=div_translate(text,length,&positions,&position_count,
            &error_line,&error_column,&error_message);
        if(generated==nullptr) {
            JsonValue *entry=build_diagnostic(error_line,error_column,1,error_message);
            if(entry==nullptr||!json_array_push(diagnostics,entry)) {
                json_free(entry);
                json_free(diagnostics);
                return nullptr;
            }
            return diagnostics;
        }
        const bool include_json=diamond_prelude_needs_json(generated);
        const size_t prelude_length=diamond_prelude_length(include_json);
        const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
        const size_t generated_length=strlen(generated);
        char *combined=malloc(prelude_length+reset_length+generated_length+1);
        if(combined==nullptr) {
            free(generated);free(positions);
            json_free(diagnostics);
            return nullptr;
        }
        size_t combined_offset=diamond_prelude_write(combined,include_json);
        memcpy(combined+combined_offset,DIAMOND_USER_LINE_RESET,reset_length);
        combined_offset+=reset_length;
        memcpy(combined+combined_offset,generated,generated_length+1);
        free(generated);

        DiamondDiagnostic diagnostic;
        diamond_program_free(scratch);
        const bool ok=diamond_compile(combined,scratch,&diagnostic);
        free(combined);
        if(!ok) {
            /* positions[line-1].line==0 means this generated line is
             * fixed boilerplate with no single corresponding template
             * position (see DivPosition's own comment) -- anchor at the
             * template's own start rather than reporting nothing, same
             * fallback the require-resolution-failure path below uses. */
            size_t template_line=1,template_column=1;
            if(diagnostic.span.line>=1&&diagnostic.span.line<=position_count) {
                const DivPosition mapped=positions[diagnostic.span.line-1];
                if(mapped.line>0) {
                    template_line=mapped.line;
                    template_column=mapped.column+
                        (diagnostic.span.column>0?diagnostic.span.column-1:0);
                }
            }
            JsonValue *entry=build_diagnostic(template_line,template_column,
                diagnostic.span.length,diagnostic.message);
            if(entry==nullptr||!json_array_push(diagnostics,entry)) {
                json_free(entry);
                free(positions);
                json_free(diagnostics);
                return nullptr;
            }
        }
        free(positions);
        return diagnostics;
    }

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
        const bool include_json=diamond_prelude_needs_json(text);
        const size_t prelude_length=diamond_prelude_length(include_json);
        const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
        char *combined=malloc(prelude_length+reset_length+length+1);
        if(combined==nullptr) {
            json_free(diagnostics);
            return nullptr;
        }
        size_t offset=diamond_prelude_write(combined,include_json);
        memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
        memcpy(combined+offset,text,length);
        combined[offset+length]='\0';
        DiamondDiagnostic diagnostic;
        diamond_program_free(scratch);
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
    const bool loaded=diamond_load_program_with_override(path,text_copy,
        document_resolve_source,(void *)documents,&bundle,load_error,sizeof load_error);
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

    const bool include_json=diamond_prelude_needs_json(bundle.source);
    const size_t prelude_length=diamond_prelude_length(include_json);
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    const size_t bundle_length=strlen(bundle.source);
    char *combined=malloc(prelude_length+reset_length+bundle_length+1);
    if(combined==nullptr) {
        free(path);
        diamond_source_bundle_free(&bundle);
        json_free(diagnostics);
        return nullptr;
    }
    size_t offset=diamond_prelude_write(combined,include_json);
    memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
    memcpy(combined+offset,bundle.source,bundle_length+1);

    DiamondDiagnostic diagnostic;
    diamond_program_free(scratch);
    const bool ok=diamond_compile(combined,scratch,&diagnostic);
    if(!ok) {
        const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
            path,combined,diagnostic,&bundle,prelude_length+reset_length);
        /* A diagnostic resolved to a different (required) file's own
         * source is a real error, but publishing it against *this*
         * document's uri would point an editor at the wrong file --
         * out_dependency_publish, if the caller wants it, gets a
         * second, independently-addressed publish for that file
         * instead (see diagnostics.h). Failing to build that second
         * publish (OOM) isn't treated as this call's own failure: this
         * document's own diagnostics (correctly empty, since its own
         * error is elsewhere) are still real and worth returning. */
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
        } else if(out_dependency_publish!=nullptr) {
            *out_dependency_publish=build_dependency_publish(&resolved,
                diagnostic.span.length,diagnostic.message);
        }
    }
    if(out_dependency_paths!=nullptr)
        build_dependency_paths(&bundle,path,out_dependency_paths);
    free(combined);
    free(path);
    diamond_source_bundle_free(&bundle);
    return diagnostics;
}
