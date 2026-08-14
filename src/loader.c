#define _XOPEN_SOURCE 700
#include "loader.h"
#include "compiler.h"
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

typedef struct Loader {
    DiamondSourceBundle *bundle;
    char *buffer;
    size_t length;
    size_t capacity;
    char loaded[DIAMOND_MAX_LOADED_FILES][DIAMOND_MAX_SOURCE_PATH];
    size_t loaded_count;
    char active[DIAMOND_MAX_REQUIRE_DEPTH][DIAMOND_MAX_SOURCE_PATH];
    size_t active_count;
    char *error;
    size_t error_capacity;
    DiamondSourceOverride override;
    void *override_data;
} Loader;

static bool append(Loader *loader,const char *text,size_t length) {
    if(length>SIZE_MAX-loader->length-1)return false;
    if(loader->length+length+1>loader->capacity) {
        size_t capacity=loader->capacity==0?4096:loader->capacity;
        while(capacity<loader->length+length+1) {
            if(capacity>SIZE_MAX/2)return false;
            capacity*=2;
        }
        char *grown=realloc(loader->buffer,capacity);
        if(grown==nullptr)return false;
        loader->buffer=grown;loader->capacity=capacity;
    }
    memcpy(loader->buffer+loader->length,text,length);
    loader->length+=length;loader->buffer[loader->length]='\0';return true;
}

static char *read_source(const char *path) {
    FILE *file=fopen(path,"rb");if(file==nullptr)return nullptr;
    /* fopen(3) succeeds on a directory on Linux; failing cleanly with
     * EISDIR is then left to read(2), which is a filesystem-driver
     * behavior, not a POSIX guarantee (see main.c's read_file, same
     * fix, found via a directory-shaped manifest test failing silently
     * in CI on whatever backs GitLab's runner's build directory --
     * evidently not the same driver as this development machine's).
     * Checking the file type explicitly makes this deterministic. */
    struct stat file_status;
    if(fstat(fileno(file),&file_status)==0&&S_ISDIR(file_status.st_mode)) {
        errno=EISDIR;fclose(file);return nullptr;
    }
    if(fseek(file,0,SEEK_END)!=0){fclose(file);return nullptr;}
    const long size=ftell(file);
    if(size<0||fseek(file,0,SEEK_SET)!=0){fclose(file);return nullptr;}
    char *source=malloc((size_t)size+1);
    if(source==nullptr){fclose(file);return nullptr;}
    if(fread(source,1,(size_t)size,file)!=(size_t)size) {
        free(source);fclose(file);return nullptr;
    }
    source[size]='\0';fclose(file);return source;
}

static bool manifest_hash_get_string(const DiamondHash *hash,const char *key,
                                     DiamondValue *out) {
    const size_t key_length=strlen(key);
    for(size_t index=0;index<hash->count;index++) {
        const DiamondValue candidate=hash->entries[index].key;
        if(candidate.kind!=DIAMOND_VALUE_OBJECT||
           candidate.as.object->kind!=DIAMOND_OBJECT_STRING)continue;
        const DiamondString *string=(const DiamondString *)candidate.as.object;
        if(string->length==key_length&&memcmp(string->chars,key,key_length)==0) {
            *out=hash->entries[index].value;return true;
        }
    }
    return false;
}

/* A package manifest is compiled and run standalone (not through expand()'s
 * own require pipeline - manifests are metadata, not programs, so require
 * inside one is deliberately unsupported) to get back a Hash whose "name"
 * must match the package's own directory. DiamondProgram/DiamondVm are
 * heap-allocated rather than stack-declared: expand() recurses once per
 * require depth, and sizeof(DiamondProgram) is over 3MB - a stack-declared
 * instance in every frame is exactly the mistake that once overflowed the
 * stack at DIAMOND_MAX_REQUIRE_DEPTH nesting. */
static bool validate_package_manifest(Loader *loader,const char *name,
        size_t name_length,const char *including_path,size_t including_line) {
    char *manifest_path=malloc(DIAMOND_MAX_SOURCE_PATH);
    if(manifest_path==nullptr)return true;
    const int written=snprintf(manifest_path,DIAMOND_MAX_SOURCE_PATH,
        "diamond_packages/%.*s/package.di",(int)name_length,name);
    char canonical_manifest[DIAMOND_MAX_SOURCE_PATH];
    const bool has_manifest=written>0&&(size_t)written<DIAMOND_MAX_SOURCE_PATH&&
        realpath(manifest_path,canonical_manifest)!=nullptr;
    free(manifest_path);
    if(!has_manifest)return true;

    char *manifest_source=read_source(canonical_manifest);
    if(manifest_source==nullptr) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: cannot read package manifest '%s': %s",
            including_path,including_line,canonical_manifest,strerror(errno));
        return false;
    }

    DiamondProgram *program=malloc(sizeof *program);
    DiamondVm *vm=malloc(sizeof *vm);
    if(program==nullptr||vm==nullptr) {
        free(manifest_source);free(program);free(vm);
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: out of memory validating package manifest '%s'",
            including_path,including_line,canonical_manifest);
        return false;
    }

    bool ok=true;
    DiamondDiagnostic diagnostic;
    bool vm_initialized=false;
    DiamondValue result=DIAMOND_NIL;
    if(!diamond_compile(manifest_source,program,&diagnostic)) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: package manifest '%s' failed to compile at line %zu: %s",
            including_path,including_line,canonical_manifest,
            diagnostic.span.line,diagnostic.message);
        ok=false;
    }
    if(ok) {
        DiamondChunk chunk=diamond_program_chunk(program);
        chunk.name=canonical_manifest;
        diamond_vm_init(vm);
        vm_initialized=true;
        const DiamondVmStatus status=diamond_vm_run(vm,&chunk,&result);
        if(status!=DIAMOND_VM_OK) {
            const char *detail=diamond_vm_error(vm);
            (void)snprintf(loader->error,loader->error_capacity,
                "%s:%zu: package manifest '%s' failed: %s",
                including_path,including_line,canonical_manifest,
                detail!=nullptr?detail:diamond_vm_status_name(status));
            ok=false;
        }
    }
    if(ok&&(result.kind!=DIAMOND_VALUE_OBJECT||
            result.as.object->kind!=DIAMOND_OBJECT_HASH)) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: package manifest '%s' must evaluate to a Hash",
            including_path,including_line,canonical_manifest);
        ok=false;
    }
    const DiamondHash *hash=ok?(const DiamondHash *)result.as.object:nullptr;
    DiamondValue declared_name=DIAMOND_NIL;
    if(ok&&(!manifest_hash_get_string(hash,"name",&declared_name)||
            declared_name.kind!=DIAMOND_VALUE_OBJECT||
            declared_name.as.object->kind!=DIAMOND_OBJECT_STRING)) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: package manifest '%s' must have a String 'name' key",
            including_path,including_line,canonical_manifest);
        ok=false;
    }
    if(ok) {
        const DiamondString *declared=(const DiamondString *)declared_name.as.object;
        if(declared->length!=name_length||memcmp(declared->chars,name,name_length)!=0) {
            (void)snprintf(loader->error,loader->error_capacity,
                "%s:%zu: package manifest '%s' declares name '%.*s', expected '%.*s'",
                including_path,including_line,canonical_manifest,
                (int)declared->length,declared->chars,(int)name_length,name);
            ok=false;
        }
    }
    DiamondValue declared_version=DIAMOND_NIL;
    if(ok&&manifest_hash_get_string(hash,"version",&declared_version)&&
       (declared_version.kind!=DIAMOND_VALUE_OBJECT||
        declared_version.as.object->kind!=DIAMOND_OBJECT_STRING)) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: package manifest '%s' key 'version' must be a String",
            including_path,including_line,canonical_manifest);
        ok=false;
    }

    if(vm_initialized)diamond_vm_free(vm);
    free(manifest_source);free(program);free(vm);
    return ok;
}

static bool same_path(const char paths[][DIAMOND_MAX_SOURCE_PATH],size_t count,
                      const char *path) {
    for(size_t index=0;index<count;index++)if(strcmp(paths[index],path)==0)return true;
    return false;
}

static bool record_segment(Loader *loader,const char *path,size_t line,
                           size_t start,size_t end) {
    if(start==end)return true;
    if(end<start) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "%s:%zu: invalid source segment range",path,line);
        return false;
    }
    if(loader->bundle->segment_count==DIAMOND_MAX_SOURCE_SEGMENTS)return false;
    DiamondSourceSegment *segment=
        &loader->bundle->segments[loader->bundle->segment_count++];
    *segment=(DiamondSourceSegment){.start=start,.end=end,.original_line=line};
    const int written=snprintf(segment->path,sizeof segment->path,"%s",path);
    if(written<0||(size_t)written>=sizeof segment->path) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "%s:%zu: source path is too long for mapping",path,line);
        return false;
    }
    return true;
}

static bool expand(Loader *loader,const char *path,const char *source,
                   const char *including_path,size_t including_line) {
    if(same_path(loader->active,loader->active_count,path)) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "circular require involving '%s' (required from %s:%zu)",
                       path,including_path!=nullptr?including_path:"<root>",
                       including_line);return false;
    }
    if(same_path(loader->loaded,loader->loaded_count,path))return true;
    if(loader->active_count==DIAMOND_MAX_REQUIRE_DEPTH) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "%s: require nesting limit reached",path);
        return false;
    }
    if(loader->loaded_count==DIAMOND_MAX_LOADED_FILES) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "%s: loaded-file limit reached",path);
        return false;
    }
    (void)snprintf(loader->active[loader->active_count++],DIAMOND_MAX_SOURCE_PATH,
                   "%s",path);
    (void)snprintf(loader->loaded[loader->loaded_count++],DIAMOND_MAX_SOURCE_PATH,
                   "%s",path);
    size_t offset=0,line=1,chunk=0,chunk_line=1;
    while(source[offset]!='\0') {
        const size_t line_start=offset;
        while(source[offset]!='\0'&&source[offset]!='\n')offset++;
        size_t line_end=offset;
        if(line_end>line_start&&source[line_end-1]=='\r')line_end--;
        if(source[offset]=='\n')offset++;
        size_t cursor=line_start;
        while(cursor<line_end&&(source[cursor]==' '||source[cursor]=='\t'))cursor++;
        const char keyword[]="require";
        bool required=line_end-cursor>=sizeof keyword-1&&
            memcmp(source+cursor,keyword,sizeof keyword-1)==0;
        const size_t keyword_end=cursor+sizeof keyword-1;
        const bool require_candidate=required &&
            (keyword_end==line_end||source[keyword_end]==' '||
             source[keyword_end]=='\t'||source[keyword_end]=='"');
        required=require_candidate;
        size_t quote=cursor+sizeof keyword-1;
        while(required&&quote<line_end&&(source[quote]==' '||source[quote]=='\t'))quote++;
        required=required&&quote<line_end&&source[quote]=='"';
        size_t close=required?quote+1:quote;
        while(required&&close<line_end&&source[close]!='"')close++;
        required=required&&close<line_end;
        size_t tail=close+1;
        while(required&&tail<line_end&&(source[tail]==' '||source[tail]=='\t'))tail++;
        if(required && tail<line_end && source[tail]!='#') {
            (void)snprintf(loader->error,loader->error_capacity,
                "%s:%zu: require directive cannot have trailing syntax",
                path,line);
            return false;
        }
        required=required&&(tail==line_end||source[tail]=='#');
        if(required) {
            const size_t start=loader->length;
            if(!append(loader,"\n#line 1\n",9)||
               !append(loader,source+chunk,line_start-chunk)) {
                (void)snprintf(loader->error,loader->error_capacity,
                               "%s:%zu: expanded source is too large",
                               path,line);
                return false;
            }
            if(!record_segment(loader,path,chunk_line,start+9,loader->length)) {
                if(loader->error[0]=='\0')
                    (void)snprintf(loader->error,loader->error_capacity,
                                   "%s:%zu: source-file segment limit reached",
                                   path,line);
                return false;
            }
            char requested[DIAMOND_MAX_SOURCE_PATH];
            const size_t request_length=close-(quote+1);
            if(request_length+4>=sizeof requested) {
                (void)snprintf(loader->error,loader->error_capacity,
                    "%s:%zu: required path is too long",path,line);
                return false;
            }
            memcpy(requested,source+quote+1,request_length);requested[request_length]='\0';
            const bool bare_name=memchr(requested,'/',request_length)==nullptr;
            if(request_length<3||strcmp(requested+request_length-3,".di")!=0)
                memcpy(requested+request_length,".di",4);
            char joined[DIAMOND_MAX_SOURCE_PATH];
            const char *slash=strrchr(path,'/');
            const size_t directory=slash==nullptr?0:(size_t)(slash-path)+1;
            if(requested[0]=='/') {
                if(strlen(requested)+1>sizeof joined) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: resolved required path is too long",path,line);
                    return false;
                }
                strcpy(joined,requested);
            } else {
                if(directory+strlen(requested)+1>sizeof joined) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: resolved required path is too long",path,line);
                    return false;
                }
                memcpy(joined,path,directory);strcpy(joined+directory,requested);
            }
            char canonical[DIAMOND_MAX_SOURCE_PATH];
            if(realpath(joined,canonical)==nullptr) {
                const int relative_errno=errno;
                bool resolved_as_package=false;
                if(bare_name) {
                    /* Heap-allocated rather than a stack buffer: expand() recurses
                     * once per require depth (up to DIAMOND_MAX_REQUIRE_DEPTH), and
                     * a fixed-size path buffer in every frame is enough to overflow
                     * the stack at that depth. */
                    char *package_path=malloc(DIAMOND_MAX_SOURCE_PATH);
                    if(package_path!=nullptr) {
                        const int written=snprintf(package_path,DIAMOND_MAX_SOURCE_PATH,
                            "diamond_packages/%.*s/%.*s.di",
                            (int)request_length,requested,(int)request_length,requested);
                        resolved_as_package=written>0&&(size_t)written<DIAMOND_MAX_SOURCE_PATH&&
                            realpath(package_path,canonical)!=nullptr;
                        free(package_path);
                    }
                }
                if(!resolved_as_package) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: cannot require '%s': %s",path,line,joined,
                        strerror(relative_errno));return false;
                }
                if(!validate_package_manifest(loader,requested,request_length,
                                              path,line))
                    return false;
            }
            char *dependency=loader->override!=nullptr?
                loader->override(canonical,loader->override_data):nullptr;
            if(dependency==nullptr)dependency=read_source(canonical);
            if(dependency==nullptr) {
                (void)snprintf(loader->error,loader->error_capacity,
                    "%s:%zu: cannot read required file '%s': %s",
                    path,line,canonical,strerror(errno));
                return false;
            }
            const bool ok=expand(loader,canonical,dependency,path,line);
            free(dependency);
            if(!ok)return false;
            chunk=offset;chunk_line=line+1;
        }
        line++;
    }
    const size_t start=loader->length;
    if(!append(loader,"\n#line 1\n",9)||
       !append(loader,source+chunk,offset-chunk)) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "%s: expanded source is too large",path);
        return false;
    }
    if(!record_segment(loader,path,chunk_line,start+9,loader->length)) {
        if(loader->error[0]=='\0')
            (void)snprintf(loader->error,loader->error_capacity,
                           "%s: source-file segment limit reached",path);
        return false;
    }
    loader->active_count--;return true;
}

bool diamond_load_program_with_override(const char *name,const char *source,
                          DiamondSourceOverride override,void *user_data,
                          DiamondSourceBundle *bundle,char *error,
                          size_t error_capacity) {
    *bundle=(DiamondSourceBundle){};Loader loader={.bundle=bundle,.error=error,
        .error_capacity=error_capacity,.override=override,.override_data=user_data};
    error[0]='\0';
    char path[DIAMOND_MAX_SOURCE_PATH];
    if(realpath(name,path)==nullptr) {
        const int written=snprintf(path,sizeof path,"%s",name);
        if(written<0||(size_t)written>=sizeof path) {
            (void)snprintf(error,error_capacity,
                           "source path is too long: '%s'",name);
            return false;
        }
    }
    if(!expand(&loader,path,source,nullptr,0)) {
        if(error[0]=='\0')snprintf(error,error_capacity,"unable to expand program sources");
        free(loader.buffer);return false;
    }
    bundle->source=loader.buffer;return true;
}

bool diamond_load_program(const char *name,const char *source,
                          DiamondSourceBundle *bundle,char *error,
                          size_t error_capacity) {
    return diamond_load_program_with_override(name,source,nullptr,nullptr,
        bundle,error,error_capacity);
}

void diamond_source_bundle_free(DiamondSourceBundle *bundle) {
    free(bundle->source);
    *bundle=(DiamondSourceBundle){};
}
