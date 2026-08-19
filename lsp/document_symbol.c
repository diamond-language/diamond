#include "document_symbol.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "loader.h"

#include <stdlib.h>
#include <string.h>

/* One `{"start":{"line","character"},"end":{"line","character"}}`
 * range covering just the name token at 1-based `line`/`column`,
 * `name_length` bytes wide -- built fresh on each call since
 * `push_symbol` below needs two independently-owned copies (`range`
 * and `selectionRange`; json_free walks and frees each field once, so
 * aliasing the same JsonValue into both would double-free). Returns
 * nullptr only on allocation failure. */
static JsonValue *build_range(size_t line,size_t column,size_t name_length) {
    JsonValue *range=json_object();
    JsonValue *start=json_object();
    JsonValue *end=json_object();
    if(range==nullptr||start==nullptr||end==nullptr) {
        json_free(range);json_free(start);json_free(end);
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
    return range;
}

static bool push_symbol(JsonValue *symbols,const char *name,int kind,
        size_t line,size_t column,size_t name_length) {
    JsonValue *range=build_range(line,column,name_length);
    JsonValue *selection_range=build_range(line,column,name_length);
    JsonValue *entry=json_object();
    if(range==nullptr||selection_range==nullptr||entry==nullptr) {
        json_free(range);json_free(selection_range);json_free(entry);
        return false;
    }
    json_object_set(entry,"name",json_string_z(name));
    json_object_set(entry,"kind",json_number(kind));
    json_object_set(entry,"range",range);
    json_object_set(entry,"selectionRange",selection_range);
    return json_array_push(symbols,entry);
}

/* Resolves `declaration_start`/`declaration_line`/`declaration_column`
 * (raw positions in the *compiled* buffer -- see src/vm.h's own comment
 * on why these need resolving at all: every contiguous chunk
 * diamond_load_program copies into the bundle, including the
 * requesting document's own content and not just a `require`d file's,
 * sits behind its own `#line 1` reset, so the raw lexer-tracked line is
 * only ever correct for the first thing after the most recent reset --
 * anything else needs the same segment-relative remap
 * diamond_resolve_diagnostic_location already does for a compile
 * error's own position) and reports whether the result lands in this
 * document itself (as opposed to a `require`d file it pulled in, which
 * has its own outline instead -- see document_symbol.h) via *out_line/
 * *out_column on success. */
static bool resolve_own_declaration(const char *combined,const DiamondSourceBundle *bundle,
        size_t user_offset,const char *display_name,size_t declaration_start,
        size_t declaration_line,size_t declaration_column,size_t name_length,
        size_t *out_line,size_t *out_column) {
    /* Below user_offset: lib/core.di's own prelude. diamond_resolve_
     * diagnostic_location only ever remaps `.path` away from
     * `display_name` when it finds a *segment* match, and never even
     * looks for one below user_offset (the prelude was never part of
     * any require-bundle segment to begin with) -- so without this
     * explicit check, a prelude declaration would pass the strcmp
     * below trivially (resolved.path left untouched, i.e. still equal
     * to display_name) and get misreported as this document's own. */
    if(declaration_start<user_offset)return false;
    const DiamondDiagnostic synthetic={
        .span={.start=declaration_start,.length=name_length,
               .line=declaration_line,.column=declaration_column},
        .message="",
    };
    const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
        display_name,combined,synthetic,bundle,user_offset);
    if(strcmp(resolved.path,display_name)!=0)return false;
    *out_line=resolved.line;
    *out_column=resolved.column;
    return true;
}

JsonValue *document_symbol_compute(const DocumentTable *documents,const char *uri,
        const char *text,size_t length) {
    char *path=diagnostics_uri_to_path(uri);
    DiamondSourceBundle bundle;
    size_t user_offset=0;
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        document_resolve_source,(void *)documents,&bundle,&user_offset);
    if(combined==nullptr) {free(path);return json_null();}

    /* A fourth independent lazily-allocated scratch DiamondProgram --
     * see hover.c's own comment on why each of these handlers keeps a
     * separate one rather than sharing. */
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

    JsonValue *symbols=json_array();
    if(symbols==nullptr) {
        free(combined);free(path);diamond_source_bundle_free(&bundle);
        return nullptr;
    }

    const char *display_name=path!=nullptr?path:uri;
    const DiamondChunk chunk=diamond_program_chunk(scratch);
    bool ok_so_far=true;
    for(size_t index=0;index<chunk.function_count&&ok_so_far;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(function->owner_class!=UINT8_MAX||function->nested)continue;
        size_t line=0,column=0;
        const size_t name_length=strlen(function->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,display_name,
                function->declaration_start,function->declaration_line,
                function->declaration_column,name_length,&line,&column))
            ok_so_far=push_symbol(symbols,function->name,12,line,column,name_length);
    }
    for(size_t index=0;index<chunk.class_count&&ok_so_far;index++) {
        const DiamondClass *class=&chunk.classes[index];
        size_t line=0,column=0;
        const size_t name_length=strlen(class->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,display_name,
                class->declaration_start,class->declaration_line,
                class->declaration_column,name_length,&line,&column))
            ok_so_far=push_symbol(symbols,class->name,5,line,column,name_length);
    }
    for(size_t index=0;index<chunk.interface_count&&ok_so_far;index++) {
        const DiamondInterface *interface=&chunk.interfaces[index];
        size_t line=0,column=0;
        const size_t name_length=strlen(interface->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,display_name,
                interface->declaration_start,interface->declaration_line,
                interface->declaration_column,name_length,&line,&column))
            ok_so_far=push_symbol(symbols,interface->name,11,line,column,name_length);
    }
    for(size_t index=0;index<chunk.module_count&&ok_so_far;index++) {
        const DiamondModule *module=&chunk.modules[index];
        size_t line=0,column=0;
        const size_t name_length=strlen(module->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,display_name,
                module->declaration_start,module->declaration_line,
                module->declaration_column,name_length,&line,&column))
            ok_so_far=push_symbol(symbols,module->name,2,line,column,name_length);
    }
    free(combined);
    free(path);
    diamond_source_bundle_free(&bundle);
    if(!ok_so_far) {json_free(symbols);return nullptr;}
    return symbols;
}
