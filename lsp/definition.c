/* See lsp/completion.c's own identical comment. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include "definition.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "lexer.h"
#include "loader.h"
#include "receiver.h"

#include <stdint.h>
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

/* A raw byte offset into `text` for 1-based `line`/`column` -- only
 * needed for an untitled/non-file:// document, same as completion.c's
 * own copy (see its comment); duplicated rather than shared for the
 * same reason identifier_token_at above already is. */
static size_t raw_offset_for(const char *text,size_t length,size_t line,size_t column) {
    size_t offset=0,current_line=1;
    while(offset<length&&current_line<line) {
        if(text[offset]=='\n')current_line++;
        offset++;
    }
    size_t result=offset;
    for(size_t moved=1;moved<column&&result<length&&text[result]!='\n';moved++)
        result++;
    return result;
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
    const size_t identifier_line=identifier.span.line;
    const size_t identifier_column=identifier.span.column;
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
    const DiamondFunction *matched_functions[DIAMOND_MAX_UNION_TYPES];
    size_t match_count=0;
    if(found) {
        /* A top-level name is unambiguous by construction (single
         * inheritance, no overloading) -- always exactly one match. */
        matched_functions[match_count++]=nullptr;
    } else {
        /* Not a top-level function/class/interface/module name -- see
         * whether `identifier` is instead a method name reached through
         * `receiver.method(...)` (lsp/receiver.h), possibly against
         * several candidate classes for a union receiver. */
        const size_t identifier_offset=path!=nullptr
            ? diamond_resolve_source_position(path,combined,&bundle,user_offset,
                  identifier_line,identifier_column)
            : user_offset+raw_offset_for(text,length,identifier_line,identifier_column);
        size_t class_indices[DIAMOND_MAX_UNION_TYPES];bool is_singleton;
        const size_t candidate_count=identifier_offset!=SIZE_MAX
            ? receiver_resolve_classes(scratch,&chunk,combined,identifier_offset,
                  class_indices,DIAMOND_MAX_UNION_TYPES,&is_singleton)
            : 0;
        for(size_t index=0;index<candidate_count;index++) {
            const DiamondMethod *method=
                receiver_lookup_method(&chunk,class_indices[index],is_singleton,name,name_length);
            if(method==nullptr)continue;
            const DiamondFunction *function=chunk.functions[method->function_index];
            if(function->declaration_start<user_offset)continue;
            matched_functions[match_count++]=function;
        }
        found=match_count>0;
    }
    if(!found) {
        free(combined);free(path);diamond_source_bundle_free(&bundle);
        return json_null();
    }

    const char *display_name=path!=nullptr?path:uri;
    JsonValue *locations[DIAMOND_MAX_UNION_TYPES];
    for(size_t index=0;index<match_count;index++) {
        /* matched_functions[index]==nullptr means the top-level-name
         * path already populated declaration_line/column/start/
         * declaration_name_length directly -- otherwise pull them from
         * the resolved receiver method's own DiamondFunction. */
        uint32_t match_line=declaration_line,match_column=declaration_column;
        size_t match_start=declaration_start,match_name_length=declaration_name_length;
        if(matched_functions[index]!=nullptr) {
            match_line=matched_functions[index]->declaration_line;
            match_column=matched_functions[index]->declaration_column;
            match_start=matched_functions[index]->declaration_start;
            match_name_length=strlen(matched_functions[index]->name);
        }
        const DiamondDiagnostic synthetic={
            .span={.start=match_start,.length=match_name_length,
                   .line=match_line,.column=match_column},
            .message="",
        };
        const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
            display_name,combined,synthetic,&bundle,user_offset);
        char *resolved_uri_owned=nullptr;
        const char *result_uri=uri;
        if(strcmp(resolved.path,display_name)!=0) {
            resolved_uri_owned=diagnostics_path_to_uri(resolved.path);
            if(resolved_uri_owned==nullptr) {
                for(size_t cleanup=0;cleanup<index;cleanup++)json_free(locations[cleanup]);
                free(combined);free(path);diamond_source_bundle_free(&bundle);
                return nullptr;
            }
            result_uri=resolved_uri_owned;
        }
        locations[index]=location_result(result_uri,resolved.line,resolved.column,match_name_length);
        free(resolved_uri_owned);
    }
    free(combined);
    free(path);
    diamond_source_bundle_free(&bundle);
    if(match_count==1)return locations[0];
    JsonValue *result=json_array();
    if(result==nullptr) {
        for(size_t index=0;index<match_count;index++)json_free(locations[index]);
        return nullptr;
    }
    for(size_t index=0;index<match_count;index++)
        if(!json_array_push(result,locations[index])) {json_free(result);return nullptr;}
    return result;
}
