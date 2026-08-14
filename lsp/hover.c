/* open_memstream is POSIX.1-2008, not ISO C -- -std=c23 alone hides it
 * behind glibc's feature-test macros, so it needs requesting explicitly
 * (before any header pulls in <stdio.h> transitively) rather than
 * hand-rolling a growable-buffer FILE* substitute. */
#define _POSIX_C_SOURCE 200809L

#include "hover.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "disassemble.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The identifier token covering 1-based `target_line`/`target_column`
 * (matching DiamondSpan's own convention -- the caller converts from
 * LSP's 0-based position), or an EOF-kind token if none does. Always
 * tokenizes the raw open-document text directly, never the prelude-
 * bundled compile buffer -- there's no position to translate through
 * that bundle at all, since the only thing this needs from the
 * tokenizer is the identifier's own text, which a name-based lookup in
 * the *compiled* program's function/class tables (built separately,
 * from the bundled buffer, by hover_compute below) resolves against
 * afterward. */
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

static JsonValue *hover_result(const char *text) {
    JsonValue *contents=json_object();
    JsonValue *result=json_object();
    if(contents==nullptr||result==nullptr) {
        json_free(contents);json_free(result);return nullptr;
    }
    json_object_set(contents,"kind",json_string_z("plaintext"));
    json_object_set(contents,"value",json_string_z(text));
    json_object_set(result,"contents",contents);
    return result;
}

/* `def name(p0: T0, p1: T1 = ..., ...) -> Return`, matching what a
 * reader would type to declare `function` themselves. An untyped
 * parameter (`parameter_type_sets[i]==UINT8_MAX`, gradual typing's own
 * "no annotation" sentinel) or return simply omits its `: T`/`-> T`,
 * the same way the real declaration would have. Default-value
 * expressions aren't reconstructed -- the compiler doesn't keep source
 * text or an AST around once a function is compiled, only its emitted
 * bytecode -- but `required_arity` at least marks which parameters are
 * optional, as a trailing `= ...`. */
static char *format_function_signature(const DiamondChunk *chunk,
        const DiamondFunction *function) {
    /* parameter_type_sets/return_type_set index into *this function's
     * own* type_sets[] array, not `chunk`'s -- `chunk.type_sets` is
     * just the entry/top-level function's own table (diamond_program_
     * chunk, src/compiler.c). diamond_disassemble hits the exact same
     * distinction and fixes it the same way: a per-function DiamondChunk
     * with only `type_sets`/`type_set_count` swapped out, everything
     * else (functions/classes/interfaces, needed for a member type that
     * names another class) borrowed from the real chunk. */
    const DiamondChunk function_chunk={
        .type_sets=function->type_sets,
        .type_set_count=function->type_set_count,
        .functions=chunk->functions,.function_count=chunk->function_count,
        .classes=chunk->classes,.class_count=chunk->class_count,
        .interfaces=chunk->interfaces,.interface_count=chunk->interface_count,
    };
    char *buffer=nullptr;size_t buffer_length=0;
    FILE *stream=open_memstream(&buffer,&buffer_length);
    if(stream==nullptr)return nullptr;
    fprintf(stream,"def %s(",function->name);
    for(size_t index=0;index<function->arity;index++) {
        if(index>0)fputs(", ",stream);
        fputs(function->parameter_names[index],stream);
        if(function->parameter_type_sets[index]!=UINT8_MAX) {
            fputs(": ",stream);
            diamond_print_type_set(stream,&function_chunk,function->parameter_type_sets[index]);
        }
        if(index>=function->required_arity)fputs(" = ...",stream);
    }
    fputc(')',stream);
    if(function->return_type_set!=UINT8_MAX) {
        fputs(" -> ",stream);
        diamond_print_type_set(stream,&function_chunk,function->return_type_set);
    }
    fclose(stream);
    return buffer;
}

static char *format_class_signature(const DiamondChunk *chunk,const DiamondClass *class) {
    char *buffer=nullptr;size_t buffer_length=0;
    FILE *stream=open_memstream(&buffer,&buffer_length);
    if(stream==nullptr)return nullptr;
    fprintf(stream,"class %s",class->name);
    if(class->superclass!=UINT8_MAX&&(size_t)class->superclass<chunk->class_count)
        fprintf(stream," < %s",chunk->classes[class->superclass].name);
    fclose(stream);
    return buffer;
}

JsonValue *hover_compute(const DocumentTable *documents,const char *uri,
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
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        document_resolve_source,(void *)documents,nullptr,nullptr);
    free(path);
    if(combined==nullptr)return json_null();

    /* Same lazily-allocated, reused-across-calls scratch buffer
     * diagnostics_compute keeps (see its own comment) -- a fresh
     * multi-ten-MB DiamondProgram malloc per hover request would be
     * wasteful for no benefit, since diamond_compile always
     * re-initializes it from scratch anyway. A separate instance from
     * diagnostics_compute's own static, not the same one: hover and
     * diagnostics can each be mid-request independently (didChange
     * publishing diagnostics while a hover request from before the
     * edit is still being answered), and sharing one buffer between
     * them would let one clobber the other's in-flight compile. */
    static DiamondProgram *scratch=nullptr;
    if(scratch==nullptr) {
        scratch=malloc(sizeof *scratch);
        if(scratch==nullptr) {free(combined);return nullptr;}
    }
    DiamondDiagnostic diagnostic;
    const bool ok=diamond_compile(combined,scratch,&diagnostic);
    free(combined);
    if(!ok)return json_null();

    const DiamondChunk chunk=diamond_program_chunk(scratch);
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=&chunk.functions[index];
        if(function->owner_class==UINT8_MAX&&!function->nested&&
           strcmp(function->name,name)==0) {
            char *signature=format_function_signature(&chunk,function);
            if(signature==nullptr)return nullptr;
            JsonValue *result=hover_result(signature);
            free(signature);
            return result;
        }
    }
    for(size_t index=0;index<chunk.class_count;index++) {
        if(strcmp(chunk.classes[index].name,name)==0) {
            char *signature=format_class_signature(&chunk,&chunk.classes[index]);
            if(signature==nullptr)return nullptr;
            JsonValue *result=hover_result(signature);
            free(signature);
            return result;
        }
    }
    return json_null();
}
