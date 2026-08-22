#include "prelude.h"

#include <string.h>

/* The prelude's five source files, embedded once here and shared by
 * every call site that used to keep its own independent copy
 * (src/run_source.c, src/repl.c, lsp/compile_buffer.c,
 * lsp/diagnostics.c) -- that duplication had already drifted once
 * (lsp/compile_buffer.c and lsp/diagnostics.c each fell behind to
 * embedding lib/core.di alone, silently breaking LSP support for any
 * document using JSON/StringBuilder/numeric.di's own functions, fixed
 * separately before this file existed), so consolidating here makes
 * that parity structural instead of hand-maintained. */
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

bool diamond_prelude_needs_json(const char *text) {
    return strstr(text,"JSON.")!=nullptr ||
           strstr(text,"JSONCodec")!=nullptr ||
           strstr(text,"JSONError")!=nullptr;
}

size_t diamond_prelude_length(bool include_json) {
    size_t length=sizeof(DIAMOND_CORE_NUMERIC_SOURCE)-1+
        sizeof(DIAMOND_CORE_SOURCE)-1+
        sizeof(DIAMOND_CORE_STRING_BUILDER_SOURCE)-1;
    if(include_json)
        length+=sizeof(DIAMOND_CORE_JSON_CODEC_SOURCE)-1+
            sizeof(DIAMOND_CORE_JSON_SOURCE)-1;
    return length;
}

size_t diamond_prelude_write(char *destination,bool include_json) {
    size_t offset=0;
    const size_t numeric_length=sizeof(DIAMOND_CORE_NUMERIC_SOURCE)-1;
    const size_t core_length=sizeof(DIAMOND_CORE_SOURCE)-1;
    const size_t string_builder_length=sizeof(DIAMOND_CORE_STRING_BUILDER_SOURCE)-1;
    memcpy(destination+offset,DIAMOND_CORE_NUMERIC_SOURCE,numeric_length);offset+=numeric_length;
    memcpy(destination+offset,DIAMOND_CORE_SOURCE,core_length);offset+=core_length;
    memcpy(destination+offset,DIAMOND_CORE_STRING_BUILDER_SOURCE,string_builder_length);
    offset+=string_builder_length;
    if(include_json) {
        const size_t json_codec_length=sizeof(DIAMOND_CORE_JSON_CODEC_SOURCE)-1;
        const size_t json_length=sizeof(DIAMOND_CORE_JSON_SOURCE)-1;
        memcpy(destination+offset,DIAMOND_CORE_JSON_CODEC_SOURCE,json_codec_length);
        offset+=json_codec_length;
        memcpy(destination+offset,DIAMOND_CORE_JSON_SOURCE,json_length);offset+=json_length;
    }
    return offset;
}
