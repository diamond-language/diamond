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
#include "loader.h"
#include "receiver.h"

#include <stdint.h>
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
 * parameter (`parameter_type_sets[i]==DIAMOND_NO_TYPE_SET`, gradual typing's own
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
        /* A trailing `*name` (splat/variadic) parameter is the last
         * slot whenever function->has_variadic is set (src/vm.h's own
         * comment on that field) -- it carries neither a type
         * annotation nor a default (the parser rejects both, see
         * compile_definition), and required_arity never counts it as
         * required either, so without this check the generic "index >=
         * required_arity" rule below would misprint it as an ordinary
         * optional parameter (`rest = ...`) instead of `*rest`. */
        const bool is_variadic_slot=function->has_variadic&&
            index+1==function->arity;
        if(is_variadic_slot)fputc('*',stream);
        fputs(function->parameter_names[index],stream);
        if(!is_variadic_slot&&
           function->parameter_type_sets[index]!=DIAMOND_NO_TYPE_SET) {
            fputs(": ",stream);
            diamond_print_type_set(stream,&function_chunk,function->parameter_type_sets[index]);
        }
        if(!is_variadic_slot&&index>=function->required_arity)fputs(" = ...",stream);
    }
    fputc(')',stream);
    if(function->return_type_set!=DIAMOND_NO_TYPE_SET) {
        fputs(" -> ",stream);
        diamond_print_type_set(stream,&function_chunk,function->return_type_set);
    }
    fclose(stream);
    return buffer;
}

/* Formats every match in `functions[0..count)` (each already known to
 * define the looked-up method) as a hover string -- just that one
 * signature when every match agrees on it textually (the common case:
 * a shared interface-like method, or the ordinary single-candidate
 * path), otherwise one `ClassName#signature` line per match so a real
 * union receiver's ambiguity is visible instead of silently picking
 * one. `class_names[i]` is `chunk->classes[class_indices[i]].name`,
 * passed separately since the caller already has both handy. */
static char *format_receiver_signatures(const DiamondChunk *chunk,
        const DiamondFunction *const *functions,const char *const *class_names,size_t count) {
    if(count==0)return nullptr;
    char *first_signature=format_function_signature(chunk,functions[0]);
    if(first_signature==nullptr)return nullptr;
    bool all_same=true;
    for(size_t index=1;index<count&&all_same;index++) {
        char *other=format_function_signature(chunk,functions[index]);
        if(other==nullptr) {free(first_signature);return nullptr;}
        if(strcmp(first_signature,other)!=0)all_same=false;
        free(other);
    }
    if(all_same||count==1)return first_signature;
    char *buffer=nullptr;size_t buffer_length=0;
    FILE *stream=open_memstream(&buffer,&buffer_length);
    if(stream==nullptr) {free(first_signature);return nullptr;}
    fprintf(stream,"%s#%s",class_names[0],first_signature);
    free(first_signature);
    for(size_t index=1;index<count;index++) {
        char *signature=format_function_signature(chunk,functions[index]);
        if(signature==nullptr) {fclose(stream);free(buffer);return nullptr;}
        fprintf(stream,"\n%s#%s",class_names[index],signature);
        free(signature);
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
    const size_t identifier_line=identifier.span.line;
    const size_t identifier_column=identifier.span.column;
    free(source_copy);

    char *path=diagnostics_uri_to_path(uri);
    /* Kept alive (not freed right after diamond_compile like before
     * receiver-method support existed) since the receiver-resolution
     * fallback below needs to re-lex `combined` itself -- see
     * lsp/receiver.h. bundle/user_offset let it translate the
     * identifier's own raw-document line/column into that same
     * compiled-buffer coordinate space, the same translation
     * definition.c/completion.c already needed for their own reasons. */
    DiamondSourceBundle bundle;
    size_t user_offset=0;
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        document_resolve_source,(void *)documents,&bundle,&user_offset);
    if(combined==nullptr) {free(path);return json_null();}

    /* Same lazily-allocated, reused-across-calls scratch buffer
     * diagnostics_compute keeps (see its own comment) -- a fresh
     * multi-ten-MB DiamondProgram malloc per hover request would be
     * wasteful for no benefit. The explicit diamond_program_free below,
     * right before compiling, is still required on every reuse: the
     * function table is independently heap-allocated, and
     * diamond_program_init's memset alone would leak the previous
     * request's functions instead of freeing them. A separate instance from
     * diagnostics_compute's own static, not the same one: hover and
     * diagnostics can each be mid-request independently (didChange
     * publishing diagnostics while a hover request from before the
     * edit is still being answered), and sharing one buffer between
     * them would let one clobber the other's in-flight compile. */
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
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(function->owner_class==UINT8_MAX&&!function->nested&&
           strcmp(function->name,name)==0) {
            char *signature=format_function_signature(&chunk,function);
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            if(signature==nullptr)return nullptr;
            JsonValue *result=hover_result(signature);
            free(signature);
            return result;
        }
    }
    for(size_t index=0;index<chunk.class_count;index++) {
        if(strcmp(chunk.classes[index].name,name)==0) {
            char *signature=format_class_signature(&chunk,&chunk.classes[index]);
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            if(signature==nullptr)return nullptr;
            JsonValue *result=hover_result(signature);
            free(signature);
            return result;
        }
    }
    for(size_t index=0;index<chunk.interface_count;index++) {
        if(strcmp(chunk.interfaces[index].name,name)==0) {
            char signature[96];
            (void)snprintf(signature,sizeof signature,"interface %s",name);
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            return hover_result(signature);
        }
    }
    for(size_t index=0;index<chunk.module_count;index++) {
        if(strcmp(chunk.modules[index].name,name)==0) {
            char signature[96];
            (void)snprintf(signature,sizeof signature,"module %s",name);
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            return hover_result(signature);
        }
    }

    /* Fallback: not a top-level function/class/interface/module name --
     * see whether `identifier` is instead a method name reached through
     * `receiver.method(...)` (lsp/receiver.h). */
    const size_t identifier_offset=path!=nullptr
        ? diamond_resolve_source_position(path,combined,&bundle,user_offset,
              identifier_line,identifier_column)
        : user_offset+raw_offset_for(text,length,identifier_line,identifier_column);
    size_t class_indices[DIAMOND_MAX_UNION_TYPES];bool is_singleton;
    if(identifier_offset!=SIZE_MAX) {
        const size_t candidate_count=receiver_resolve_classes(scratch,&chunk,combined,
            identifier_offset,class_indices,DIAMOND_MAX_UNION_TYPES,&is_singleton);
        const DiamondFunction *matched_functions[DIAMOND_MAX_UNION_TYPES];
        const char *matched_class_names[DIAMOND_MAX_UNION_TYPES];
        size_t matched_count=0;
        for(size_t index=0;index<candidate_count;index++) {
            const DiamondMethod *method=receiver_lookup_method(
                &chunk,class_indices[index],is_singleton,name,name_length);
            if(method==nullptr)continue;
            matched_functions[matched_count]=chunk.functions[method->function_index];
            matched_class_names[matched_count]=chunk.classes[class_indices[index]].name;
            matched_count++;
        }
        if(matched_count>0) {
            char *signature=format_receiver_signatures(
                &chunk,matched_functions,matched_class_names,matched_count);
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            if(signature==nullptr)return nullptr;
            JsonValue *result=hover_result(signature);
            free(signature);
            return result;
        }
    }
    free(combined);free(path);diamond_source_bundle_free(&bundle);
    return json_null();
}
