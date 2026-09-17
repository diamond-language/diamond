/* See lsp/completion.c's own identical comment. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "references.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "lexer.h"
#include "loader.h"
#include "receiver.h"
#include "workspace_symbol.h"

#include <stdlib.h>
#include <string.h>

/* Same identifier-under-the-cursor lookup hover.c/definition.c use --
 * see definition.c's own comment on why this is duplicated rather than
 * shared. */
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

static JsonValue *references_location(const char *uri,size_t line,size_t column,
        size_t name_length) {
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

/* True iff `name` names a top-level function, class, interface, or
 * module anywhere in `chunk` -- deliberately not filtered by
 * declaration_start against a user_offset (unlike definition.c's own
 * declaration lookup): a workspace reference scan should still find
 * genuine call/access sites of a prelude-declared name, even though
 * definition.c itself won't jump to the prelude's own unfollowable
 * location for it. */
static bool name_is_global_symbol(const DiamondChunk *chunk,const char *name,
        size_t name_length) {
    for(size_t index=0;index<chunk->function_count;index++) {
        const DiamondFunction *function=chunk->functions[index];
        if(function->owner_class==UINT8_MAX&&!function->nested&&
           strlen(function->name)==name_length&&
           memcmp(function->name,name,name_length)==0)
            return true;
    }
    for(size_t index=0;index<chunk->class_count;index++)
        if(strlen(chunk->classes[index].name)==name_length&&
           memcmp(chunk->classes[index].name,name,name_length)==0)
            return true;
    for(size_t index=0;index<chunk->interface_count;index++)
        if(strlen(chunk->interfaces[index].name)==name_length&&
           memcmp(chunk->interfaces[index].name,name,name_length)==0)
            return true;
    for(size_t index=0;index<chunk->module_count;index++)
        if(strlen(chunk->modules[index].name)==name_length&&
           memcmp(chunk->modules[index].name,name,name_length)==0)
            return true;
    return false;
}

typedef struct ReferenceEntry {
    char *uri;
    size_t line;
    size_t column;
} ReferenceEntry;

typedef struct ReferenceList {
    ReferenceEntry *entries;
    size_t count;
    size_t capacity;
} ReferenceList;

static void reference_list_free(ReferenceList *list) {
    for(size_t index=0;index<list->count;index++)free(list->entries[index].uri);
    free(list->entries);
}

/* Same uri/line/column triple can legitimately turn up more than once
 * across the workspace scan: a shared `require`d file's own content is
 * copied verbatim into every requiring document's own combined buffer
 * (lsp/compile_buffer.c), so a reference physically living in it is
 * rediscovered once per requiring file scanned, always resolving back
 * (diamond_resolve_diagnostic_location) to the same physical location. */
static bool reference_list_push(ReferenceList *list,const char *uri,size_t line,
        size_t column) {
    for(size_t index=0;index<list->count;index++)
        if(list->entries[index].line==line&&list->entries[index].column==column&&
           strcmp(list->entries[index].uri,uri)==0)
            return true;
    if(list->count==list->capacity) {
        const size_t grown_capacity=list->capacity==0?16:list->capacity*2;
        ReferenceEntry *grown=realloc(list->entries,grown_capacity*sizeof *grown);
        if(grown==nullptr)return false;
        list->entries=grown;list->capacity=grown_capacity;
    }
    char *uri_copy=malloc(strlen(uri)+1);
    if(uri_copy==nullptr)return false;
    strcpy(uri_copy,uri);
    list->entries[list->count++]=(ReferenceEntry){.uri=uri_copy,.line=line,.column=column};
    return true;
}

/* Compiles `path` and appends every genuine reference-shaped occurrence
 * of `name` it contains to `list` -- see references.h for exactly what
 * "reference-shaped" means and why. Not fatal for the whole scan if this
 * one file can't be read or doesn't compile (matches workspace_symbol.c's
 * own scan_file); only a real allocation failure returns false. */
static bool scan_file_for_references(const DocumentTable *documents,DiamondProgram *scratch,
        const char *path,const char *name,size_t name_length,ReferenceList *list) {
    char *text=read_file_preferring_open(documents,path);
    if(text==nullptr)return true;
    const size_t length=strlen(text);
    DiamondSourceBundle bundle;
    size_t user_offset=0;
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        document_resolve_source,(void *)documents,&bundle,&user_offset);
    free(text);
    if(combined==nullptr)return true;
    DiamondDiagnostic diagnostic;
    diamond_program_free(scratch);
    if(!diamond_compile(combined,scratch,&diagnostic)) {
        free(combined);diamond_source_bundle_free(&bundle);
        return true;
    }
    const DiamondChunk chunk=diamond_program_chunk(scratch);

    /* Tokenize the whole combined buffer once, newline-filtered (same as
     * receiver_resolve_classes) so a next/previous-token adjacency check
     * isn't thrown off by line breaks between a call and its `(`. */
    DiamondLexer lexer;diamond_lexer_init(&lexer,combined);
    size_t token_count=0,token_capacity=64;
    DiamondToken *tokens=malloc(token_capacity*sizeof *tokens);
    bool ok=tokens!=nullptr;
    while(ok) {
        const DiamondToken token=diamond_lexer_next(&lexer);
        if(token.kind==DIAMOND_TOKEN_NEWLINE)continue;
        if(token_count==token_capacity) {
            token_capacity*=2;
            DiamondToken *grown=realloc(tokens,token_capacity*sizeof *tokens);
            if(grown==nullptr) {ok=false;break;}
            tokens=grown;
        }
        tokens[token_count++]=token;
        if(token.kind==DIAMOND_TOKEN_EOF)break;
    }
    if(!ok) {
        free(tokens);free(combined);diamond_source_bundle_free(&bundle);
        return false;
    }

    for(size_t index=0;index<token_count&&ok;index++) {
        const DiamondToken token=tokens[index];
        if(token.kind!=DIAMOND_TOKEN_IDENTIFIER)continue;
        if(token.span.start<user_offset)continue;
        if(token.span.length!=name_length||
           memcmp(combined+token.span.start,name,name_length)!=0)
            continue;
        const bool call_or_access=index+1<token_count&&
            (tokens[index+1].kind==DIAMOND_TOKEN_LEFT_PAREN||
             tokens[index+1].kind==DIAMOND_TOKEN_DOT);
        const bool type_position=index>0&&
            (tokens[index-1].kind==DIAMOND_TOKEN_COLON||
             tokens[index-1].kind==DIAMOND_TOKEN_PIPE||
             tokens[index-1].kind==DIAMOND_TOKEN_LESS);
        /* A class/module/interface declaration header (`class Widget`,
         * with no following `(`/`.` and no preceding `:`/`|`/`<`)
         * matches neither rule above -- without this, only a function's
         * own declaration (always followed by its own `(`) would ever
         * self-match, and a type with no other in-workspace usage would
         * report zero references for its own declaration. */
        const bool declaration_header=index>0&&
            (tokens[index-1].kind==DIAMOND_TOKEN_CLASS||
             tokens[index-1].kind==DIAMOND_TOKEN_MODULE||
             tokens[index-1].kind==DIAMOND_TOKEN_INTERFACE);
        if(!call_or_access&&!type_position&&!declaration_header)continue;
        if(receiver_name_is_local(scratch,&chunk,name,name_length,token.span.start))continue;

        const DiamondDiagnostic synthetic={
            .span={.start=token.span.start,.length=token.span.length,
                   .line=token.span.line,.column=token.span.column},
            .message="",
        };
        const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
            path,combined,synthetic,&bundle,user_offset);
        char *reference_uri=diagnostics_path_to_uri(resolved.path);
        if(reference_uri==nullptr) {ok=false;break;}
        ok=reference_list_push(list,reference_uri,resolved.line,resolved.column);
        free(reference_uri);
    }
    free(tokens);
    free(combined);
    diamond_source_bundle_free(&bundle);
    return ok;
}

/* Shared by references_compute/rename_compute: everything through
 * finding every workspace occurrence of the top-level symbol under the
 * cursor -- see references_compute's own doc comment for exactly what
 * counts as an occurrence. `*out_name_length` gets the resolved
 * symbol's own name length (references_location/a TextEdit's range
 * both need it, to know how far the match extends past its start
 * column).
 *
 * Returns true with `*out_list` populated (caller frees via
 * reference_list_free, even on a technically-empty result) when the
 * position names a workspace-visible global symbol. Returns false
 * otherwise (`*out_list` left zeroed, nothing to free) with
 * `*out_allocation_failed` distinguishing "not applicable, caller
 * should return json_null()" (left false) from "a real allocation
 * failure partway through, caller should return nullptr" (set true) --
 * the same two-case failure contract references_compute's own doc
 * comment already promises its own callers. */
static bool find_workspace_occurrences(const DocumentTable *documents,const char *workspace_root,
        const char *uri,const char *text,size_t length,size_t line,size_t character,
        ReferenceList *out_list,size_t *out_name_length,bool *out_allocation_failed) {
    *out_list=(ReferenceList){0};
    *out_name_length=0;
    *out_allocation_failed=false;
    if(workspace_root==nullptr||workspace_root[0]=='\0')return false;

    char *source_copy=malloc(length+1);
    if(source_copy==nullptr) {*out_allocation_failed=true;return false;}
    memcpy(source_copy,text,length);
    source_copy[length]='\0';
    const DiamondToken identifier=identifier_token_at(source_copy,line+1,character+1);
    if(identifier.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        free(source_copy);
        return false;
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
    if(combined==nullptr) {free(path);return false;}

    /* Independent from scan_scratch below (a workspace scan can revisit
     * `path` itself while this one's still logically "in use") -- same
     * one-scratch-per-independent-compile-lifetime rule every other
     * lsp/ handler already follows (see definition.c's own comment). */
    static DiamondProgram *origin_scratch=nullptr;
    if(origin_scratch==nullptr) {
        origin_scratch=calloc(1,sizeof *origin_scratch);
        if(origin_scratch==nullptr) {
            free(combined);free(path);diamond_source_bundle_free(&bundle);
            *out_allocation_failed=true;
            return false;
        }
    }
    DiamondDiagnostic diagnostic;
    diamond_program_free(origin_scratch);
    const bool origin_ok=diamond_compile(combined,origin_scratch,&diagnostic);
    bool is_global=false;
    if(origin_ok) {
        const DiamondChunk origin_chunk=diamond_program_chunk(origin_scratch);
        is_global=name_is_global_symbol(&origin_chunk,name,name_length);
    }
    free(combined);free(path);diamond_source_bundle_free(&bundle);
    if(!origin_ok||!is_global)return false;

    char **paths=nullptr;
    size_t path_count=0,path_capacity=0;
    if(!collect_di_files(workspace_root,&paths,&path_count,&path_capacity)) {
        for(size_t index=0;index<path_count;index++)free(paths[index]);
        free(paths);
        *out_allocation_failed=true;
        return false;
    }

    static DiamondProgram *scan_scratch=nullptr;
    if(scan_scratch==nullptr) {
        scan_scratch=calloc(1,sizeof *scan_scratch);
        if(scan_scratch==nullptr) {
            for(size_t index=0;index<path_count;index++)free(paths[index]);
            free(paths);
            *out_allocation_failed=true;
            return false;
        }
    }

    ReferenceList list={0};
    bool scan_ok=true;
    for(size_t index=0;index<path_count;index++) {
        if(scan_ok)
            scan_ok=scan_file_for_references(documents,scan_scratch,paths[index],
                name,name_length,&list);
        free(paths[index]);
    }
    free(paths);
    if(!scan_ok) {
        reference_list_free(&list);
        *out_allocation_failed=true;
        return false;
    }
    *out_list=list;
    *out_name_length=name_length;
    return true;
}

JsonValue *references_compute(const DocumentTable *documents,const char *workspace_root,
        const char *uri,const char *text,size_t length,size_t line,size_t character) {
    ReferenceList list;size_t name_length=0;bool allocation_failed=false;
    if(!find_workspace_occurrences(documents,workspace_root,uri,text,length,line,character,
            &list,&name_length,&allocation_failed))
        return allocation_failed?nullptr:json_null();

    JsonValue *result=json_array();
    if(result==nullptr) {reference_list_free(&list);return nullptr;}
    bool build_ok=true;
    for(size_t index=0;index<list.count&&build_ok;index++) {
        JsonValue *entry=references_location(list.entries[index].uri,
            list.entries[index].line,list.entries[index].column,name_length);
        if(entry==nullptr||!json_array_push(result,entry)) {
            json_free(entry);
            build_ok=false;
        }
    }
    reference_list_free(&list);
    if(!build_ok) {json_free(result);return nullptr;}
    return result;
}

/* True iff `name` lexes as exactly one DIAMOND_TOKEN_IDENTIFIER
 * consuming the whole string -- rejects empty, a keyword (the lexer
 * hands those back as their own dedicated token kind, never
 * DIAMOND_TOKEN_IDENTIFIER), a qualified `A::B` name, embedded
 * whitespace, and anything past DIAMOND_MAX_FUNCTION_NAME - 1 bytes
 * (every declared name in this language already shares that same
 * storage limit, so nothing renamed past it could ever be declared
 * anyway). */
static bool is_valid_new_name(const char *name,size_t length) {
    if(length==0||length>=DIAMOND_MAX_FUNCTION_NAME)return false;
    char buffer[DIAMOND_MAX_FUNCTION_NAME];
    memcpy(buffer,name,length);
    buffer[length]='\0';
    DiamondLexer lexer;
    diamond_lexer_init(&lexer,buffer);
    const DiamondToken token=diamond_lexer_next(&lexer);
    if(token.kind!=DIAMOND_TOKEN_IDENTIFIER||token.span.length!=length)return false;
    return diamond_lexer_next(&lexer).kind==DIAMOND_TOKEN_EOF;
}

/* One TextEdit -- see rename_compute's own doc comment for the
 * WorkspaceEdit shape this nests into. Same 0-based line/column
 * conversion as references_location, deliberately not shared with it:
 * a Location has its own "uri" field a TextEdit doesn't (the URI is
 * this edit's own key in the enclosing "changes" map instead), so
 * factoring out just the "range" object alone would save less
 * duplication than it looks like without also complicating both
 * callers' own object-assembly order. */
static JsonValue *rename_text_edit(size_t line,size_t column,size_t old_name_length,
        const char *new_name,size_t new_name_length) {
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
    json_object_set(end,"character",json_number(lsp_character+(double)old_name_length));
    json_object_set(range,"start",start);
    json_object_set(range,"end",end);
    json_object_set(result,"range",range);
    json_object_set(result,"newText",json_string(new_name,new_name_length));
    return result;
}

JsonValue *rename_compute(const DocumentTable *documents,const char *workspace_root,
        const char *uri,const char *text,size_t length,size_t line,size_t character,
        const char *new_name,size_t new_name_length) {
    if(!is_valid_new_name(new_name,new_name_length))return json_null();

    ReferenceList list;size_t name_length=0;bool allocation_failed=false;
    if(!find_workspace_occurrences(documents,workspace_root,uri,text,length,line,character,
            &list,&name_length,&allocation_failed))
        return allocation_failed?nullptr:json_null();

    JsonValue *changes=json_object();
    if(changes==nullptr) {reference_list_free(&list);return nullptr;}

    /* Groups edits by URI in first-seen order: a small, growable
     * "find or create" side-table -- json_object_set has no
     * update-in-place semantics of its own (a second call with the
     * same key would append a duplicate member instead of merging into
     * the array already stored under it, see json.h), and a rename's
     * own occurrence count is small enough that a linear scan per
     * entry (not a hash map) is plenty. */
    typedef struct {const char *uri;JsonValue *edits;} UriGroup;
    UriGroup *groups=nullptr;size_t group_count=0,group_capacity=0;
    bool build_ok=true;
    for(size_t index=0;index<list.count&&build_ok;index++) {
        const ReferenceEntry *entry=&list.entries[index];
        JsonValue *edits=nullptr;
        for(size_t group=0;group<group_count;group++)
            if(strcmp(groups[group].uri,entry->uri)==0) {edits=groups[group].edits;break;}
        if(edits==nullptr) {
            edits=json_array();
            if(edits==nullptr||!json_object_set(changes,entry->uri,edits)) {
                json_free(edits);build_ok=false;break;
            }
            if(group_count==group_capacity) {
                const size_t grown_capacity=group_capacity==0?8:group_capacity*2;
                UriGroup *grown=realloc(groups,grown_capacity*sizeof *grown);
                if(grown==nullptr) {build_ok=false;break;}
                groups=grown;group_capacity=grown_capacity;
            }
            groups[group_count++]=(UriGroup){.uri=entry->uri,.edits=edits};
        }
        JsonValue *edit=rename_text_edit(entry->line,entry->column,name_length,
            new_name,new_name_length);
        if(edit==nullptr||!json_array_push(edits,edit)) {json_free(edit);build_ok=false;}
    }
    free(groups);
    reference_list_free(&list);
    if(!build_ok) {json_free(changes);return nullptr;}

    JsonValue *result=json_object();
    if(result==nullptr||!json_object_set(result,"changes",changes)) {
        json_free(result);json_free(changes);return nullptr;
    }
    return result;
}
