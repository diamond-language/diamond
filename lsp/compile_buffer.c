#include "compile_buffer.h"

#include "loader.h"
#include "prelude.h"

#include <stdlib.h>
#include <string.h>

static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

char *diamond_lsp_build_compile_buffer(const char *path,const char *text,size_t length,
        DiamondSourceOverride override,void *override_data,
        DiamondSourceBundle *out_bundle,size_t *out_user_offset) {
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    if(path==nullptr) {
        if(out_bundle!=nullptr)*out_bundle=(DiamondSourceBundle){};
        const bool include_json=diamond_prelude_needs_json(text);
        const size_t prelude_length=diamond_prelude_length(include_json);
        if(out_user_offset!=nullptr)*out_user_offset=prelude_length+reset_length;
        char *combined=malloc(prelude_length+reset_length+length+1);
        if(combined==nullptr)return nullptr;
        size_t offset=diamond_prelude_write(combined,include_json);
        memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
        memcpy(combined+offset,text,length);
        combined[offset+length]='\0';
        return combined;
    }
    char *text_copy=malloc(length+1);
    if(text_copy==nullptr)return nullptr;
    memcpy(text_copy,text,length);
    text_copy[length]='\0';
    DiamondSourceBundle bundle;
    char load_error[768];
    const bool loaded=diamond_load_program_with_override(path,text_copy,
        override,override_data,&bundle,load_error,sizeof load_error);
    free(text_copy);
    if(!loaded)return nullptr;
    const bool include_json=diamond_prelude_needs_json(bundle.source);
    const size_t prelude_length=diamond_prelude_length(include_json);
    if(out_user_offset!=nullptr)*out_user_offset=prelude_length+reset_length;
    const size_t bundle_length=strlen(bundle.source);
    char *combined=malloc(prelude_length+reset_length+bundle_length+1);
    if(combined!=nullptr) {
        size_t offset=diamond_prelude_write(combined,include_json);
        memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
        memcpy(combined+offset,bundle.source,bundle_length+1);
    }
    if(out_bundle!=nullptr)*out_bundle=bundle;
    else diamond_source_bundle_free(&bundle);
    return combined;
}
