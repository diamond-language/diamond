#include "compile_buffer.h"

#include "loader.h"

#include <stdlib.h>
#include <string.h>

/* Same five embedded prelude files, in the same numeric -> core ->
 * string_builder -> json_codec -> json concatenation order, as
 * src/run_source.c and src/repl.c -- kept in sync manually (no shared
 * helper exists between any of the three), since lib/core.di itself no
 * longer defines StringBuilder/JSONCodec/JSONError/JSON at all (they
 * were extracted into these four files). This previously only embedded
 * lib/core.di alone, silently leaving the LSP unable to compile any
 * document using JSON/StringBuilder/numeric.di's own top-level
 * functions (abs/min/max/mod/array_sort/...) -- confirmed directly: a
 * file with `JSON.stringify(...)` compiled and ran fine via
 * build/diamond but produced a spurious "undefined local variable"
 * diagnostic on `JSON` here, which also meant hover/definition/
 * completion silently returned nothing for that document (all three
 * require a clean compile). */
static constexpr unsigned char DIAMOND_CORE_SOURCE[] = {
#embed "../lib/core.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_STRING_BUILDER_SOURCE[] = {
#embed "../lib/core/string_builder.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_NUMERIC_SOURCE[] = {
#embed "../lib/core/numeric.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_JSON_CODEC_SOURCE[] = {
#embed "../lib/core/json_codec.di" suffix(,)
    0
};
static constexpr unsigned char DIAMOND_CORE_JSON_SOURCE[] = {
#embed "../lib/core/json.di" suffix(,)
    0
};
static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

char *diamond_lsp_build_compile_buffer(const char *path,const char *text,size_t length,
        DiamondSourceOverride override,void *override_data,
        DiamondSourceBundle *out_bundle,size_t *out_user_offset) {
    const size_t numeric_length=sizeof(DIAMOND_CORE_NUMERIC_SOURCE)-1;
    const size_t core_length=sizeof(DIAMOND_CORE_SOURCE)-1;
    const size_t string_builder_length=sizeof(DIAMOND_CORE_STRING_BUILDER_SOURCE)-1;
    const size_t json_codec_length=sizeof(DIAMOND_CORE_JSON_CODEC_SOURCE)-1;
    const size_t json_length=sizeof(DIAMOND_CORE_JSON_SOURCE)-1;
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    const size_t prelude_length=numeric_length+core_length+string_builder_length+
        json_codec_length+json_length;
    if(out_user_offset!=nullptr)*out_user_offset=prelude_length+reset_length;
    if(path==nullptr) {
        if(out_bundle!=nullptr)*out_bundle=(DiamondSourceBundle){};
        char *combined=malloc(prelude_length+reset_length+length+1);
        if(combined==nullptr)return nullptr;
        size_t offset=0;
        memcpy(combined+offset,DIAMOND_CORE_NUMERIC_SOURCE,numeric_length);offset+=numeric_length;
        memcpy(combined+offset,DIAMOND_CORE_SOURCE,core_length);offset+=core_length;
        memcpy(combined+offset,DIAMOND_CORE_STRING_BUILDER_SOURCE,string_builder_length);
        offset+=string_builder_length;
        memcpy(combined+offset,DIAMOND_CORE_JSON_CODEC_SOURCE,json_codec_length);
        offset+=json_codec_length;
        memcpy(combined+offset,DIAMOND_CORE_JSON_SOURCE,json_length);offset+=json_length;
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
    const size_t bundle_length=strlen(bundle.source);
    char *combined=malloc(prelude_length+reset_length+bundle_length+1);
    if(combined!=nullptr) {
        size_t offset=0;
        memcpy(combined+offset,DIAMOND_CORE_NUMERIC_SOURCE,numeric_length);offset+=numeric_length;
        memcpy(combined+offset,DIAMOND_CORE_SOURCE,core_length);offset+=core_length;
        memcpy(combined+offset,DIAMOND_CORE_STRING_BUILDER_SOURCE,string_builder_length);
        offset+=string_builder_length;
        memcpy(combined+offset,DIAMOND_CORE_JSON_CODEC_SOURCE,json_codec_length);
        offset+=json_codec_length;
        memcpy(combined+offset,DIAMOND_CORE_JSON_SOURCE,json_length);offset+=json_length;
        memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
        memcpy(combined+offset,bundle.source,bundle_length+1);
    }
    if(out_bundle!=nullptr)*out_bundle=bundle;
    else diamond_source_bundle_free(&bundle);
    return combined;
}
