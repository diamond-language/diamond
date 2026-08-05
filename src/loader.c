#define _XOPEN_SOURCE 700
#include "loader.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Loader {
    DiamondSourceBundle *bundle;
    char *buffer;
    size_t length;
    size_t capacity;
    char loaded[128][DIAMOND_MAX_SOURCE_PATH];
    size_t loaded_count;
    char active[128][DIAMOND_MAX_SOURCE_PATH];
    size_t active_count;
    char *error;
    size_t error_capacity;
} Loader;

static bool append(Loader *loader,const char *text,size_t length) {
    if(loader->length+length+1>loader->capacity) {
        size_t capacity=loader->capacity==0?4096:loader->capacity;
        while(capacity<loader->length+length+1)capacity*=2;
        char *grown=realloc(loader->buffer,capacity);
        if(grown==nullptr)return false;
        loader->buffer=grown;loader->capacity=capacity;
    }
    memcpy(loader->buffer+loader->length,text,length);
    loader->length+=length;loader->buffer[loader->length]='\0';return true;
}

static char *read_source(const char *path) {
    FILE *file=fopen(path,"rb");if(file==nullptr)return nullptr;
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

static bool same_path(const char paths[][DIAMOND_MAX_SOURCE_PATH],size_t count,
                      const char *path) {
    for(size_t index=0;index<count;index++)if(strcmp(paths[index],path)==0)return true;
    return false;
}

static bool record_segment(Loader *loader,const char *path,size_t line,
                           size_t start,size_t end) {
    if(start==end)return true;
    if(loader->bundle->segment_count==DIAMOND_MAX_SOURCE_SEGMENTS)return false;
    DiamondSourceSegment *segment=
        &loader->bundle->segments[loader->bundle->segment_count++];
    *segment=(DiamondSourceSegment){.start=start,.end=end,.original_line=line};
    (void)snprintf(segment->path,sizeof segment->path,"%s",path);return true;
}

static bool expand(Loader *loader,const char *path,const char *source) {
    if(same_path(loader->active,loader->active_count,path)) {
        (void)snprintf(loader->error,loader->error_capacity,
                       "circular require involving '%s'",path);return false;
    }
    if(same_path(loader->loaded,loader->loaded_count,path))return true;
    if(loader->active_count==128||loader->loaded_count==128)return false;
    (void)snprintf(loader->active[loader->active_count++],DIAMOND_MAX_SOURCE_PATH,
                   "%s",path);
    (void)snprintf(loader->loaded[loader->loaded_count++],DIAMOND_MAX_SOURCE_PATH,
                   "%s",path);
    size_t offset=0,line=1,chunk=0,chunk_line=1;
    while(source[offset]!='\0') {
        const size_t line_start=offset;
        while(source[offset]!='\0'&&source[offset]!='\n')offset++;
        const size_t line_end=offset;
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
               !append(loader,source+chunk,line_start-chunk)||
               !record_segment(loader,path,chunk_line,start+9,loader->length))return false;
            char requested[DIAMOND_MAX_SOURCE_PATH];
            const size_t request_length=close-(quote+1);
            if(request_length+5>=sizeof requested)return false;
            memcpy(requested,source+quote+1,request_length);requested[request_length]='\0';
            if(request_length<4||strcmp(requested+request_length-4,".dia")!=0)
                memcpy(requested+request_length,".dia",5);
            char joined[DIAMOND_MAX_SOURCE_PATH];
            const char *slash=strrchr(path,'/');
            const size_t directory=slash==nullptr?0:(size_t)(slash-path)+1;
            if(requested[0]=='/') {
                if(strlen(requested)+1>sizeof joined)return false;
                strcpy(joined,requested);
            } else {
                if(directory+strlen(requested)+1>sizeof joined)return false;
                memcpy(joined,path,directory);strcpy(joined+directory,requested);
            }
            char canonical[DIAMOND_MAX_SOURCE_PATH];
            if(realpath(joined,canonical)==nullptr) {
                (void)snprintf(loader->error,loader->error_capacity,
                    "%s:%zu: cannot require '%s': %s",path,line,joined,
                    strerror(errno));return false;
            }
            char *dependency=read_source(canonical);
            if(dependency==nullptr)return false;
            const bool ok=expand(loader,canonical,dependency);free(dependency);
            if(!ok)return false;
            chunk=offset;chunk_line=line+1;
        }
        line++;
    }
    const size_t start=loader->length;
    if(!append(loader,"\n#line 1\n",9)||!append(loader,source+chunk,offset-chunk)||
       !record_segment(loader,path,chunk_line,start+9,loader->length))return false;
    loader->active_count--;return true;
}

bool diamond_load_program(const char *name,const char *source,
                          DiamondSourceBundle *bundle,char *error,
                          size_t error_capacity) {
    *bundle=(DiamondSourceBundle){};Loader loader={.bundle=bundle,.error=error,
        .error_capacity=error_capacity};error[0]='\0';
    char path[DIAMOND_MAX_SOURCE_PATH];
    if(realpath(name,path)==nullptr)(void)snprintf(path,sizeof path,"%s",name);
    if(!expand(&loader,path,source)) {
        if(error[0]=='\0')snprintf(error,error_capacity,"unable to expand program sources");
        free(loader.buffer);return false;
    }
    bundle->source=loader.buffer;return true;
}

void diamond_source_bundle_free(DiamondSourceBundle *bundle) {
    free(bundle->source);bundle->source=nullptr;bundle->segment_count=0;
}
