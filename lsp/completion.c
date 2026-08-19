#include "completion.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "loader.h"
#include "vm.h"

#include <stdlib.h>
#include <string.h>

enum {
    COMPLETION_KIND_FUNCTION = 3,
    COMPLETION_KIND_VARIABLE = 6,
    COMPLETION_KIND_CLASS = 7,
    COMPLETION_KIND_INTERFACE = 8,
    COMPLETION_KIND_MODULE = 9,
};

static bool push_item(JsonValue *items,const char *name,int kind) {
    JsonValue *item=json_object();
    if(item==nullptr)return false;
    json_object_set(item,"label",json_string_z(name));
    json_object_set(item,"kind",json_number(kind));
    return json_array_push(items,item);
}

/* A raw byte offset into `text` for 1-based `line`/`column` -- only
 * needed for an untitled/non-file:// document (no on-disk path, so no
 * DiamondSourceBundle segments diamond_resolve_source_position could
 * walk); combined = core prelude + reset + `text` verbatim in that
 * case (diamond_lsp_build_compile_buffer's own path==nullptr branch),
 * so this plus `user_offset` gives the matching compiled-buffer
 * offset directly. */
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

/* Pushes every DiamondScopeLocal in `function` whose valid range
 * contains `cursor_offset` -- see completion.h's own comment on why
 * this correctly covers both "not yet declared"/"already out of a
 * narrower rescue scope" exclusion and nested-closure capture
 * visibility, with no special-casing needed for either. */
static bool push_scope_locals(JsonValue *items,const DiamondFunction *function,
        size_t cursor_offset) {
    for(size_t index=0;index<function->scope_local_count;index++) {
        const DiamondScopeLocal *local=&function->scope_locals[index];
        if(cursor_offset<local->valid_start||cursor_offset>=local->valid_end)continue;
        if(!push_item(items,local->name,COMPLETION_KIND_VARIABLE))return false;
    }
    return true;
}

JsonValue *completion_compute(const DocumentTable *documents,const char *uri,
        const char *text,size_t length,size_t line,size_t character) {
    char *path=diagnostics_uri_to_path(uri);
    DiamondSourceBundle bundle;
    size_t user_offset=0;
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        document_resolve_source,(void *)documents,&bundle,&user_offset);
    if(combined==nullptr) {free(path);return json_null();}

    /* A fifth independent lazily-allocated scratch DiamondProgram --
     * see hover.c's own comment on why each lsp/ handler keeps a
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

    const size_t cursor_offset=path!=nullptr
        ? diamond_resolve_source_position(path,combined,&bundle,user_offset,
              line+1,character+1)
        : user_offset+raw_offset_for(text,length,line+1,character+1);

    JsonValue *items=json_array();
    if(items==nullptr) {
        free(combined);free(path);diamond_source_bundle_free(&bundle);
        return nullptr;
    }
    bool okay=true;
    const DiamondChunk chunk=diamond_program_chunk(scratch);
    for(size_t index=0;okay&&index<chunk.function_count;index++)
        if(chunk.functions[index]->owner_class==UINT8_MAX&&!chunk.functions[index]->nested)
            okay=push_item(items,chunk.functions[index]->name,COMPLETION_KIND_FUNCTION);
    for(size_t index=0;okay&&index<chunk.class_count;index++)
        okay=push_item(items,chunk.classes[index].name,COMPLETION_KIND_CLASS);
    for(size_t index=0;okay&&index<chunk.interface_count;index++)
        okay=push_item(items,chunk.interfaces[index].name,COMPLETION_KIND_INTERFACE);
    for(size_t index=0;okay&&index<chunk.module_count;index++)
        okay=push_item(items,chunk.modules[index].name,COMPLETION_KIND_MODULE);
    if(okay&&cursor_offset!=SIZE_MAX) {
        okay=push_scope_locals(items,&scratch->entry,cursor_offset);
        for(size_t index=0;okay&&index<scratch->function_count;index++)
            okay=push_scope_locals(items,scratch->functions[index],cursor_offset);
    }
    free(combined);free(path);diamond_source_bundle_free(&bundle);
    if(!okay) {json_free(items);return nullptr;}
    return items;
}
