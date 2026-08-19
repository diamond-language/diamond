/* strncasecmp is POSIX, not ISO C -- needs requesting explicitly under
 * -std=c23, same reason hover.c requests _POSIX_C_SOURCE for
 * open_memstream (see its own comment). */
#define _DEFAULT_SOURCE

#include "workspace_symbol.h"

#include "compile_buffer.h"
#include "compiler.h"
#include "diagnostics.h"
#include "loader.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* Recursively appends every *.di file's own absolute path under
 * `directory` to `*paths` (grown as needed, `*count`/`*capacity`
 * tracking it the same way document.c's own DocumentTable does),
 * skipping dotfiles/dotdirs entirely (never meaningful Diamond source,
 * and some -- .git -- are expensive to walk for no benefit). Not
 * fatal if `directory` can't be opened (permission, a broken symlink,
 * a race with something deleting it mid-walk) -- just nothing found
 * there; only an allocation failure returns false. */
static bool collect_di_files(const char *directory,char ***paths,
        size_t *count,size_t *capacity) {
    DIR *dir=opendir(directory);
    if(dir==nullptr)return true;
    struct dirent *entry;
    bool ok=true;
    while(ok&&(entry=readdir(dir))!=nullptr) {
        if(entry->d_name[0]=='.')continue;
        char child[4096];
        const int written=snprintf(child,sizeof child,"%s/%s",directory,entry->d_name);
        if(written<0||(size_t)written>=sizeof child)continue;
        struct stat info;
        if(stat(child,&info)!=0)continue;
        if(S_ISDIR(info.st_mode)) {
            ok=collect_di_files(child,paths,count,capacity);
            continue;
        }
        if(!S_ISREG(info.st_mode))continue;
        const size_t name_length=strlen(entry->d_name);
        if(name_length<3||strcmp(entry->d_name+name_length-3,".di")!=0)continue;
        if(*count==*capacity) {
            const size_t grown_capacity=*capacity==0?32:*capacity*2;
            char **grown=realloc(*paths,grown_capacity*sizeof(char *));
            if(grown==nullptr) {ok=false;break;}
            *paths=grown;*capacity=grown_capacity;
        }
        char *copy=malloc(strlen(child)+1);
        if(copy==nullptr) {ok=false;break;}
        strcpy(copy,child);
        (*paths)[(*count)++]=copy;
    }
    closedir(dir);
    return ok;
}

/* `path`'s content, preferring an open document's live buffer
 * (document_resolve_source, lsp/document.h) over disk -- same "live
 * over stale" rule every other lsp/ feature that resolves source text
 * already follows. Returns a malloc'd, null-terminated buffer, or
 * nullptr if `path` can't be read at all (deleted mid-walk, a
 * permission error, ...). */
static char *read_file_preferring_open(const DocumentTable *documents,const char *path) {
    char *live=document_resolve_source(path,(void *)documents);
    if(live!=nullptr)return live;
    FILE *file=fopen(path,"rb");
    if(file==nullptr)return nullptr;
    if(fseek(file,0,SEEK_END)!=0) {fclose(file);return nullptr;}
    const long size=ftell(file);
    if(size<0||fseek(file,0,SEEK_SET)!=0) {fclose(file);return nullptr;}
    char *source=malloc((size_t)size+1);
    if(source==nullptr) {fclose(file);return nullptr;}
    if(fread(source,1,(size_t)size,file)!=(size_t)size) {
        free(source);fclose(file);return nullptr;
    }
    source[size]='\0';fclose(file);
    return source;
}

static bool contains_ignore_case(const char *haystack,const char *needle) {
    if(*needle=='\0')return true;
    const size_t needle_length=strlen(needle);
    for(const char *cursor=haystack;*cursor!='\0';cursor++) {
        if(strncasecmp(cursor,needle,needle_length)==0)return true;
    }
    return false;
}

static JsonValue *build_location(const char *uri,size_t line,size_t column,
        size_t name_length) {
    JsonValue *range=json_object();
    JsonValue *start=json_object();
    JsonValue *end=json_object();
    JsonValue *location=json_object();
    if(range==nullptr||start==nullptr||end==nullptr||location==nullptr) {
        json_free(range);json_free(start);json_free(end);json_free(location);
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
    json_object_set(location,"uri",json_string_z(uri));
    json_object_set(location,"range",range);
    return location;
}

static bool push_symbol(JsonValue *symbols,const char *name,int kind,const char *uri,
        size_t line,size_t column,size_t name_length) {
    JsonValue *location=build_location(uri,line,column,name_length);
    JsonValue *entry=json_object();
    if(location==nullptr||entry==nullptr) {
        json_free(location);json_free(entry);
        return false;
    }
    json_object_set(entry,"name",json_string_z(name));
    json_object_set(entry,"kind",json_number(kind));
    json_object_set(entry,"location",location);
    return json_array_push(symbols,entry);
}

/* Same "does this declaration actually live in the file being scanned,
 * as opposed to something it merely `require`s" resolution lsp/
 * document_symbol.c's own resolve_own_declaration does -- duplicated
 * rather than shared (a few lines, workspace-scanning's own uri/query
 * plumbing around it doesn't fit document_symbol.c's own shape any
 * better than the reverse). */
static bool resolve_own_declaration(const char *combined,const DiamondSourceBundle *bundle,
        size_t user_offset,const char *path,size_t declaration_start,
        size_t declaration_line,size_t declaration_column,size_t name_length,
        size_t *out_line,size_t *out_column) {
    if(declaration_start<user_offset)return false;
    const DiamondDiagnostic synthetic={
        .span={.start=declaration_start,.length=name_length,
               .line=declaration_line,.column=declaration_column},
        .message="",
    };
    const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
        path,combined,synthetic,bundle,user_offset);
    if(strcmp(resolved.path,path)!=0)return false;
    *out_line=resolved.line;
    *out_column=resolved.column;
    return true;
}

/* Compiles `path` and pushes every matching symbol actually declared
 * in it. Not fatal for the whole request if this one file doesn't
 * compile, can't be read, or can't be turned into a uri -- see
 * workspace_symbol.h's own comment on why. Only an allocation failure
 * (OOM building the JSON result itself) propagates out as false. */
static bool scan_file(const DocumentTable *documents,DiamondProgram *scratch,
        const char *path,const char *query,JsonValue *symbols) {
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
    if(!diamond_compile(combined,scratch,&diagnostic)) {
        free(combined);diamond_source_bundle_free(&bundle);
        return true;
    }
    char *uri=diagnostics_path_to_uri(path);
    if(uri==nullptr) {
        free(combined);diamond_source_bundle_free(&bundle);
        return true;
    }
    bool ok=true;
    const DiamondChunk chunk=diamond_program_chunk(scratch);
    for(size_t index=0;index<chunk.function_count&&ok;index++) {
        const DiamondFunction *function=&chunk.functions[index];
        if(function->owner_class!=UINT8_MAX||function->nested)continue;
        if(!contains_ignore_case(function->name,query))continue;
        size_t line=0,column=0;
        const size_t name_length=strlen(function->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,path,
                function->declaration_start,function->declaration_line,
                function->declaration_column,name_length,&line,&column))
            ok=push_symbol(symbols,function->name,12,uri,line,column,name_length);
    }
    for(size_t index=0;index<chunk.class_count&&ok;index++) {
        const DiamondClass *class=&chunk.classes[index];
        if(!contains_ignore_case(class->name,query))continue;
        size_t line=0,column=0;
        const size_t name_length=strlen(class->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,path,
                class->declaration_start,class->declaration_line,
                class->declaration_column,name_length,&line,&column))
            ok=push_symbol(symbols,class->name,5,uri,line,column,name_length);
    }
    for(size_t index=0;index<chunk.interface_count&&ok;index++) {
        const DiamondInterface *interface=&chunk.interfaces[index];
        if(!contains_ignore_case(interface->name,query))continue;
        size_t line=0,column=0;
        const size_t name_length=strlen(interface->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,path,
                interface->declaration_start,interface->declaration_line,
                interface->declaration_column,name_length,&line,&column))
            ok=push_symbol(symbols,interface->name,11,uri,line,column,name_length);
    }
    for(size_t index=0;index<chunk.module_count&&ok;index++) {
        const DiamondModule *module=&chunk.modules[index];
        if(!contains_ignore_case(module->name,query))continue;
        size_t line=0,column=0;
        const size_t name_length=strlen(module->name);
        if(resolve_own_declaration(combined,&bundle,user_offset,path,
                module->declaration_start,module->declaration_line,
                module->declaration_column,name_length,&line,&column))
            ok=push_symbol(symbols,module->name,2,uri,line,column,name_length);
    }
    free(uri);
    free(combined);
    diamond_source_bundle_free(&bundle);
    return ok;
}

JsonValue *workspace_symbol_compute(const DocumentTable *documents,
        const char *workspace_root,const char *query) {
    JsonValue *symbols=json_array();
    if(symbols==nullptr)return nullptr;
    if(workspace_root==nullptr||workspace_root[0]=='\0')return symbols;

    char **paths=nullptr;
    size_t count=0,capacity=0;
    if(!collect_di_files(workspace_root,&paths,&count,&capacity)) {
        for(size_t index=0;index<count;index++)free(paths[index]);
        free(paths);
        json_free(symbols);
        return nullptr;
    }

    /* One DiamondProgram, reused across every file this request scans
     * -- see tests/run_cases.c's own comment on why (docs/roadmap.md):
     * malloc/free-ing an ~83MB DiamondProgram per file is real mmap/
     * munmap kernel work, and a workspace can easily have hundreds of
     * *.di files. Lazily allocated on first use, kept for the life of
     * the process (like every other lsp/ handler's own scratch
     * buffer) rather than per-request, since diamond_compile always
     * re-initializes it from scratch before compiling. */
    static DiamondProgram *scratch=nullptr;
    if(scratch==nullptr) {
        scratch=malloc(sizeof *scratch);
        if(scratch==nullptr) {
            for(size_t index=0;index<count;index++)free(paths[index]);
            free(paths);
            json_free(symbols);
            return nullptr;
        }
    }

    bool ok=true;
    for(size_t index=0;index<count;index++) {
        if(ok)ok=scan_file(documents,scratch,paths[index],query,symbols);
        free(paths[index]);
    }
    free(paths);
    if(!ok) {json_free(symbols);return nullptr;}
    return symbols;
}
