#include "definition.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "lexer.h"
#include "loader.h"

#include <stdlib.h>
#include <string.h>

/* Same identifier-under-the-cursor lookup hover.c uses -- see its own
 * comment. Duplicated rather than shared: it's five lines, and neither
 * file is a natural home for the other to depend on. */
static DiamondToken identifier_token_at(const char *source,size_t target_line,
        size_t target_column) {
    DiamondLexer lexer;
    diamond_lexer_init(&lexer,source);
    while(true) {
        const DiamondToken current=diamond_lexer_next(&lexer);
        if(current.kind==DIAMOND_TOKEN_EOF)return current;
        if(current.kind==DIAMOND_TOKEN_IDENTIFIER&&current.span.line==target_line&&
           target_column>=current.span.column&&
           target_column<current.span.column+current.span.length)
            return current;
    }
}

static JsonValue *location_result(const char *uri,size_t line,size_t column,size_t name_length) {
    JsonValue *range=json_object();
    JsonValue *start=json_object();
    JsonValue *end=json_object();
    JsonValue *result=json_object();
    if(range==nullptr||start==nullptr||end==nullptr||result==nullptr) {
        json_free(range);json_free(start);json_free(end);json_free(result);
        return nullptr;
    }
    const double lsp_line=line>0?(double)(line-1):0;
    const double lsp_character=column>0?(double)(column-1):0;
    json_object_set(start,"line",json_number(lsp_line));
    json_object_set(start,"character",json_number(lsp_character));
    json_object_set(end,"line",json_number(lsp_line));
    json_object_set(end,"character",json_number(lsp_character+(double)name_length));
    json_object_set(range,"start",start);
    json_object_set(range,"end",end);
    json_object_set(result,"uri",json_string_z(uri));
    json_object_set(result,"range",range);
    return result;
}

JsonValue *definition_compute(const DocumentTable *documents,const char *uri,
        const char *text,size_t length,size_t line,size_t character) {
    char *source_copy=malloc(length+1);
    if(source_copy==nullptr)return nullptr;
    memcpy(source_copy,text,length);
    source_copy[length]='\0';
    const DiamondToken identifier=identifier_token_at(source_copy,line+1,character+1);
    if(identifier.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        free(source_copy);
        return json_null();
    }
    char name[64];
    size_t name_length=identifier.span.length;
    if(name_length>=sizeof name)name_length=sizeof name-1;
    memcpy(name,source_copy+identifier.span.start,name_length);
    name[name_length]='\0';
    free(source_copy);

    char *path=diagnostics_uri_to_path(uri);
    DiamondSourceBundle bundle;
    size_t user_offset=0;
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        document_resolve_source,(void *)documents,&bundle,&user_offset);
    if(combined==nullptr) {free(path);return json_null();}

    /* A third independent lazily-allocated scratch DiamondProgram --
     * see hover.c's own comment on why diagnostics.c/hover.c each keep
     * a separate one rather than sharing: a didChange, a hover request,
     * and a definition request can all be genuinely in flight at once. */
    static DiamondProgram *scratch=nullptr;
    if(scratch==nullptr) {
        scratch=calloc(1,sizeof *scratch);
        if(scratch==nullptr) {
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            return nullptr;
        }
    }
    DiamondDiagnostic diagnostic;
    diamond_program_free(scratch);
    const bool ok=diamond_compile(combined,scratch,&diagnostic);
    if(!ok) {
        free(combined);free(path);diamond_source_bundle_free(&bundle);
        return json_null();
    }

    const DiamondChunk chunk=diamond_program_chunk(scratch);
    uint32_t declaration_line=0,declaration_column=0;
    size_t declaration_start=0,declaration_name_length=0;
    bool found=false;
    for(size_t index=0;index<chunk.function_count&&!found;index++) {
        const DiamondFunction *function=chunk.functions[index];
        /* declaration_start>=user_offset excludes lib/core.di's own
         * prelude -- see definition.h for why a prelude match returns
         * null instead of a location nobody can jump to. */
        if(function->owner_class==UINT8_MAX&&!function->nested&&
           function->declaration_start>=user_offset&&
           strcmp(function->name,name)==0) {
            declaration_line=function->declaration_line;
            declaration_column=function->declaration_column;
            declaration_start=function->declaration_start;
            declaration_name_length=strlen(function->name);
            found=true;
        }
    }
    for(size_t index=0;index<chunk.class_count&&!found;index++) {
        const DiamondClass *class=&chunk.classes[index];
        if(class->declaration_start>=user_offset&&strcmp(class->name,name)==0) {
            declaration_line=class->declaration_line;
            declaration_column=class->declaration_column;
            declaration_start=class->declaration_start;
            declaration_name_length=strlen(class->name);
            found=true;
        }
    }
    for(size_t index=0;index<chunk.interface_count&&!found;index++) {
        const DiamondInterface *interface=&chunk.interfaces[index];
        if(interface->declaration_start>=user_offset&&strcmp(interface->name,name)==0) {
            declaration_line=interface->declaration_line;
            declaration_column=interface->declaration_column;
            declaration_start=interface->declaration_start;
            declaration_name_length=strlen(interface->name);
            found=true;
        }
    }
    for(size_t index=0;index<chunk.module_count&&!found;index++) {
        const DiamondModule *module=&chunk.modules[index];
        if(module->declaration_start>=user_offset&&strcmp(module->name,name)==0) {
            declaration_line=module->declaration_line;
            declaration_column=module->declaration_column;
            declaration_start=module->declaration_start;
            declaration_name_length=strlen(module->name);
            found=true;
        }
    }
    if(!found) {
        free(combined);free(path);diamond_source_bundle_free(&bundle);
        return json_null();
    }

    const char *display_name=path!=nullptr?path:uri;
    const DiamondDiagnostic synthetic={
        .span={.start=declaration_start,.length=declaration_name_length,
               .line=declaration_line,.column=declaration_column},
        .message="",
    };
    const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
        display_name,combined,synthetic,&bundle,user_offset);

    char *resolved_uri_owned=nullptr;
    const char *result_uri=uri;
    if(strcmp(resolved.path,display_name)!=0) {
        resolved_uri_owned=diagnostics_path_to_uri(resolved.path);
        if(resolved_uri_owned==nullptr) {
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            return nullptr;
        }
        result_uri=resolved_uri_owned;
    }
    JsonValue *result=location_result(result_uri,resolved.line,resolved.column,
        declaration_name_length);
    free(resolved_uri_owned);
    free(combined);
    free(path);
    diamond_source_bundle_free(&bundle);
    return result;
}
