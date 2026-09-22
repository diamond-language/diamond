#define _XOPEN_SOURCE 700
#include "loader.h"
#include "manifest_literal.h"

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
    if(memchr(source,'\0',(size_t)size)!=nullptr) {
        free(source);fclose(file);errno=EINVAL;return nullptr;
    }
    source[size]='\0';fclose(file);return source;
}

/* Manifest metadata is parsed without compiling or running Diamond code. */
static bool validate_cut_manifest(Loader *loader,const char *name,
        size_t name_length,const char *including_path,size_t including_line) {
    char manifest_path[DIAMOND_MAX_SOURCE_PATH];
    const int written=snprintf(manifest_path,sizeof manifest_path,
        "cuts/%.*s/diamond.cut",(int)name_length,name);
    char canonical_manifest[DIAMOND_MAX_SOURCE_PATH];
    const bool has_manifest=written>0&&(size_t)written<sizeof manifest_path&&
        realpath(manifest_path,canonical_manifest)!=nullptr;
    if(!has_manifest)return true;

    char *source=read_source(canonical_manifest);
    if(source==nullptr) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: cannot read cut manifest '%s': %s",
            including_path,including_line,canonical_manifest,strerror(errno));
        return false;
    }
    char detail[160];
    DiamondManifestValue *manifest=diamond_manifest_parse(source,detail,sizeof detail);
    free(source);
    if(manifest==nullptr) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: cut manifest '%s' must be data only: %s",
            including_path,including_line,canonical_manifest,detail);
        return false;
    }
    const DiamondManifestValue *declared=diamond_manifest_get(manifest,"name");
    bool ok=true;
    if(declared==nullptr||declared->kind!=DIAMOND_MANIFEST_STRING) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: cut manifest '%s' must have a String 'name' key",
            including_path,including_line,canonical_manifest);
        ok=false;
    } else if(strlen(declared->string)!=name_length||
              memcmp(declared->string,name,name_length)!=0) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: cut manifest '%s' declares name '%s', expected '%.*s'",
            including_path,including_line,canonical_manifest,
            declared->string,(int)name_length,name);
        ok=false;
    }
    const DiamondManifestValue *version=diamond_manifest_get(manifest,"version");
    if(ok&&version!=nullptr&&version->kind!=DIAMOND_MANIFEST_STRING) {
        (void)snprintf(loader->error,loader->error_capacity,
            "%s:%zu: cut manifest '%s' key 'version' must be a String",
            including_path,including_line,canonical_manifest);
        ok=false;
    }
    diamond_manifest_free(manifest);
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
        const char keyword_cut[]="require_cut";
        const char keyword_plain[]="require";
        const bool is_cut=line_end-cursor>=sizeof keyword_cut-1&&
            memcmp(source+cursor,keyword_cut,sizeof keyword_cut-1)==0;
        const size_t keyword_length=is_cut?sizeof keyword_cut-1:sizeof keyword_plain-1;
        bool required=is_cut||
            (line_end-cursor>=sizeof keyword_plain-1&&
             memcmp(source+cursor,keyword_plain,sizeof keyword_plain-1)==0);
        const size_t keyword_end=cursor+keyword_length;
        const bool require_candidate=required &&
            (keyword_end==line_end||source[keyword_end]==' '||
             source[keyword_end]=='\t'||source[keyword_end]=='"');
        required=require_candidate;
        size_t quote=cursor+keyword_length;
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
            char canonical[DIAMOND_MAX_SOURCE_PATH];
            if(is_cut) {
                if(memchr(requested,'/',request_length)!=nullptr) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: require_cut path must be a bare cut name, not '%s'",
                        path,line,requested);
                    return false;
                }
                /* Heap-allocated rather than a stack buffer: expand() recurses
                 * once per require depth (up to DIAMOND_MAX_REQUIRE_DEPTH), and
                 * a fixed-size path buffer in every frame is enough to overflow
                 * the stack at that depth. */
                char *cut_path=malloc(DIAMOND_MAX_SOURCE_PATH);
                if(cut_path==nullptr) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: out of memory resolving require_cut '%s'",
                        path,line,requested);
                    return false;
                }
                int written=snprintf(cut_path,DIAMOND_MAX_SOURCE_PATH,
                    "cuts/%.*s/lib/%.*s.di",
                    (int)request_length,requested,(int)request_length,requested);
                bool resolved=written>0&&(size_t)written<DIAMOND_MAX_SOURCE_PATH&&
                    realpath(cut_path,canonical)!=nullptr;
                if(!resolved) {
                    written=snprintf(cut_path,DIAMOND_MAX_SOURCE_PATH,
                        "cuts/%.*s/%.*s.di",
                        (int)request_length,requested,(int)request_length,requested);
                    resolved=written>0&&(size_t)written<DIAMOND_MAX_SOURCE_PATH&&
                        realpath(cut_path,canonical)!=nullptr;
                }
                free(cut_path);
                if(!resolved) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: cannot require_cut '%s': no cuts/%.*s/lib/%.*s.di",
                        path,line,requested,
                        (int)request_length,requested,(int)request_length,requested);
                    return false;
                }
                if(!validate_cut_manifest(loader,requested,request_length,path,line))
                    return false;
            } else {
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
                if(realpath(joined,canonical)==nullptr) {
                    (void)snprintf(loader->error,loader->error_capacity,
                        "%s:%zu: cannot require '%s': %s",path,line,joined,
                        strerror(errno));
                    return false;
                }
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
    /* Heap-allocated, not a local -- Loader's own `loaded`/`active`
     * arrays (DIAMOND_MAX_LOADED_FILES/DIAMOND_MAX_REQUIRE_DEPTH, each
     * DIAMOND_MAX_SOURCE_PATH bytes wide) are several MiB combined, and
     * this function's own frame sits underneath every recursive
     * `expand()` call a deeply-nested require chain makes (nesting
     * itself bounded by DIAMOND_MAX_REQUIRE_DEPTH, but each level still
     * adds its own frame on top of this one) -- confirmed directly as
     * a real stack overflow (SIGSEGV) once DIAMOND_MAX_LOADED_FILES
     * grew past its original, much smaller value: a local `Loader`
     * here plus a near-max-depth require chain together exceeded a
     * normal 8MiB thread stack. `expand` already takes `Loader *`, so
     * nothing about the recursive calls themselves needed to change --
     * only where the struct itself lives. */
    Loader *loader=malloc(sizeof *loader);
    if(loader==nullptr) {
        (void)snprintf(error,error_capacity,"out of memory loading program sources");
        return false;
    }
    /* Heap-allocated for the same reason Loader itself just above is --
     * see DiamondSourceBundle's own `segments` field (src/loader.h) for
     * the exact stack-overflow history this replaced. */
    DiamondSourceSegment *segments=malloc(DIAMOND_MAX_SOURCE_SEGMENTS*sizeof *segments);
    if(segments==nullptr) {
        (void)snprintf(error,error_capacity,"out of memory loading program sources");
        free(loader);
        return false;
    }
    *bundle=(DiamondSourceBundle){.segments=segments};
    *loader=(Loader){.bundle=bundle,.error=error,
        .error_capacity=error_capacity,.override=override,.override_data=user_data};
    error[0]='\0';
    char path[DIAMOND_MAX_SOURCE_PATH];
    if(realpath(name,path)==nullptr) {
        const int written=snprintf(path,sizeof path,"%s",name);
        if(written<0||(size_t)written>=sizeof path) {
            (void)snprintf(error,error_capacity,
                           "source path is too long: '%s'",name);
            free(loader);free(segments);*bundle=(DiamondSourceBundle){};return false;
        }
    }
    if(!expand(loader,path,source,nullptr,0)) {
        if(error[0]=='\0')snprintf(error,error_capacity,"unable to expand program sources");
        free(loader->buffer);free(loader);free(segments);*bundle=(DiamondSourceBundle){};
        return false;
    }
    bundle->source=loader->buffer;free(loader);return true;
}

bool diamond_load_program(const char *name,const char *source,
                          DiamondSourceBundle *bundle,char *error,
                          size_t error_capacity) {
    return diamond_load_program_with_override(name,source,nullptr,nullptr,
        bundle,error,error_capacity);
}

void diamond_source_bundle_free(DiamondSourceBundle *bundle) {
    free(bundle->source);
    free(bundle->segments);
    *bundle=(DiamondSourceBundle){};
}
