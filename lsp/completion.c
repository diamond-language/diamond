/* See src/bignum.c's own identical comment: needed transitively for
 * vm.h's <ucontext.h> use (via compiler.h), only under musl
 * (docs/portability.md). Must precede this file's own
 * first #include -- document.h (pulled in by completion.h) may reach a
 * libc header before compiler.h otherwise. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "completion.h"

#include "compile_buffer.h"
#include "analysis_cache.h"
#include "compiler.h"
#include "loader.h"
#include "receiver.h"
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

/* Pushes every method name in `class_index`'s own singleton_methods[]
 * (is_singleton true) or methods[] (false), then its superclass's, and
 * so on -- matching a receiver.<partial>| completion's own name
 * candidates (lsp/receiver.h), not deduplicated against an override
 * further down the chain, same as this file's other completion lists
 * don't dedupe against each other either. */
static bool push_class_methods(JsonValue *items,const DiamondChunk *chunk,
        size_t class_index,bool is_singleton) {
    size_t current=class_index;
    while(current!=UINT8_MAX) {
        const DiamondClass *class=&chunk->classes[current];
        const DiamondMethod *methods=is_singleton?class->singleton_methods:class->methods;
        const size_t count=is_singleton?class->singleton_method_count:class->method_count;
        for(size_t index=0;index<count;index++)
            if(!push_item(items,methods[index].name,COMPLETION_KIND_FUNCTION))return false;
        current=class->superclass;
    }
    return true;
}

/* The members a built-in value answers to: the VM's native method names
 * (diamond_native_member_names) plus the prelude's extension functions, which
 * the VM dispatches by name -- `items.foo` calls array_foo, then
 * enumerable_foo; strings use string_, Int integer_/numeric_, Float
 * float_/numeric_ (find_collection_extension/find_value_extension in
 * src/vm.c), each also tried under a `diamond_` prefix. A trailing `?` is
 * dropped from the bridge name, so a Bool-returning function is offered with
 * the `?` it is written with. */
typedef struct MemberNames {
    char **names;
    size_t count;
} MemberNames;

static bool member_seen(const MemberNames *seen,const char *name) {
    const size_t length=strlen(name)-(name[strlen(name)-1]=='?');
    for(size_t index=0;index<seen->count;index++) {
        const char *other=seen->names[index];
        const size_t other_length=strlen(other)-(other[strlen(other)-1]=='?');
        if(length==other_length&&memcmp(name,other,length)==0)return true;
    }
    return false;
}

static bool push_member(JsonValue *items,MemberNames *seen,const char *name) {
    if(member_seen(seen,name))return true;
    char **grown=realloc(seen->names,(seen->count+1)*sizeof *grown);
    if(grown==nullptr)return false;
    seen->names=grown;
    seen->names[seen->count]=strdup(name);
    if(seen->names[seen->count]==nullptr)return false;
    seen->count++;
    return push_item(items,name,COMPLETION_KIND_FUNCTION);
}

static bool function_returns_bool(const DiamondFunction *function) {
    if(function->return_type_set==DIAMOND_NO_TYPE_SET||
       function->return_type_set>=function->type_set_count)return false;
    const DiamondTypeSet *set=&function->type_sets[function->return_type_set];
    return set->count==1&&set->members[0].id==DIAMOND_TYPE_BOOL;
}

static bool push_builtin_members(JsonValue *items,const DiamondChunk *chunk,
        uint8_t type) {
    size_t prefix_count;
    const char *const *prefixes=receiver_builtin_prefixes(type,&prefix_count);
    if(prefixes==nullptr)return true;
    MemberNames seen={};
    bool okay=true;
    const char *const *natives;
    const size_t native_count=diamond_native_member_names(type,&natives);
    for(size_t index=0;okay&&index<native_count;index++)
        okay=push_member(items,&seen,natives[index]);
    for(size_t index=0;okay&&index<chunk->function_count;index++) {
        const DiamondFunction *function=chunk->functions[index];
        if(function->owner_class!=UINT8_MAX||function->nested)continue;
        const char *name=function->name;
        if(strncmp(name,"diamond_",8)==0)name+=8;
        for(size_t prefix=0;okay&&prefix<prefix_count;prefix++) {
            const size_t prefix_length=strlen(prefixes[prefix]);
            if(strncmp(name,prefixes[prefix],prefix_length)!=0)continue;
            const char *member=name+prefix_length;
            /* Operator bridges (array_op_minus) and prelude internals. */
            if(*member=='\0'||strncmp(member,"op_",3)==0||strcmp(member,"call_pair")==0)continue;
            if(type==DIAMOND_TYPE_HASH&&strcmp(prefixes[prefix],"enumerable_")==0&&
               !receiver_hash_uses_enumerable(member))continue;
            char label[DIAMOND_MAX_FUNCTION_NAME+2];
            snprintf(label,sizeof label,"%s%s",member,function_returns_bool(function)?"?":"");
            okay=push_member(items,&seen,label);
        }
    }
    for(size_t index=0;index<seen.count;index++)free(seen.names[index]);
    free(seen.names);
    return okay;
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

JsonValue *completion_compute_with_resolver(DiamondSourceOverride resolver,
        void *resolver_data,const char *path,const char *text,size_t length,
        size_t line,size_t character) {
    DiamondSourceBundle bundle;
    size_t user_offset=0;
    char *combined=diamond_lsp_build_compile_buffer(path,text,length,
        resolver,resolver_data,&bundle,&user_offset);
    if(combined==nullptr) {return json_null();}

    const DiamondProgram *scratch=diamond_lsp_analyze(combined);
    if(scratch==nullptr) {
        free(combined);diamond_source_bundle_free(&bundle);
        return json_null();
    }

    const size_t cursor_offset=path!=nullptr
        ? diamond_resolve_source_position(path,combined,&bundle,user_offset,
              line+1,character+1)
        : user_offset+raw_offset_for(text,length,line+1,character+1);

    JsonValue *items=json_array();
    if(items==nullptr) {
        free(combined);diamond_source_bundle_free(&bundle);
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
        size_t class_indices[DIAMOND_MAX_UNION_TYPES];bool is_singleton;
        if(okay) {
            const size_t candidate_count=receiver_resolve_classes(scratch,&chunk,combined,
                cursor_offset,class_indices,DIAMOND_MAX_UNION_TYPES,&is_singleton);
            for(size_t index=0;okay&&index<candidate_count;index++)
                okay=push_class_methods(items,&chunk,class_indices[index],is_singleton);
            uint8_t builtin_type;
            if(okay&&candidate_count==0&&
               receiver_resolve_builtin_type(scratch,&chunk,combined,cursor_offset,&builtin_type))
                okay=push_builtin_members(items,&chunk,builtin_type);
        }
    }
    free(combined);diamond_source_bundle_free(&bundle);
    if(!okay) {json_free(items);return nullptr;}
    return items;
}
