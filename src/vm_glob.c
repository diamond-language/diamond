/* Dir.glob(pattern, base: nil): the paths matching a shell-style pattern.
 *
 * `*` and `?` match within one path segment, `[abc]`/`[a-z]`/`[!a]` match one
 * character, `{a,b}` is alternation (nested too), a segment that is exactly `**`
 * matches any number of directories, and `\` escapes the next character.
 * Names starting with "." match only a segment that starts with a literal ".",
 * and `**` never descends into hidden directories or through a symlink to a
 * directory (so a link cycle cannot loop). The result is sorted bytewise
 * without duplicates. Paths are written as the pattern wrote them; with
 * `base`, the pattern is relative to that directory and so are the results.
 * A directory that cannot be read is skipped, as Ruby's Dir.glob does. */
#include "vm_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Brace expansion is exponential in the number of groups; past this many
 * patterns the call fails instead of exhausting memory. */
enum { GLOB_MAX_PATTERNS = 4096, GLOB_MAX_DEPTH = 256 };

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} GlobList;

typedef struct {
    DiamondVm *vm;
    GlobList *matches;
    const char *base;      /* nullptr, or the directory paths are relative to */
    bool out_of_memory;
} GlobWalk;

static void glob_list_free(GlobList *list) {
    for(size_t index=0;index<list->count;index++)free(list->items[index]);
    free(list->items);
    *list=(GlobList){};
}

static bool glob_list_push(GlobList *list,char *item) {
    if(item==nullptr)return false;
    if(list->count==list->capacity) {
        const size_t capacity=list->capacity==0?16:list->capacity*2;
        char **items=realloc(list->items,capacity*sizeof *items);
        if(items==nullptr){free(item);return false;}
        list->items=items;list->capacity=capacity;
    }
    list->items[list->count++]=item;
    return true;
}

static char *glob_concat(const char *a,size_t a_length,const char *b,size_t b_length,
        const char *c,size_t c_length) {
    char *joined=malloc(a_length+b_length+c_length+1);
    if(joined==nullptr)return nullptr;
    memcpy(joined,a,a_length);
    memcpy(joined+a_length,b,b_length);
    memcpy(joined+a_length+b_length,c,c_length);
    joined[a_length+b_length+c_length]='\0';
    return joined;
}

/* Expands the first top-level {..} group of `pattern` and recurses on each
 * result. Returns false when out of memory; *too_many is set past the cap. */
static bool glob_expand_braces(const char *pattern,GlobList *out,bool *too_many) {
    const size_t length=strlen(pattern);
    size_t open=SIZE_MAX,close=SIZE_MAX,depth=0;
    for(size_t index=0;index<length;index++) {
        const char c=pattern[index];
        if(c=='\\'&&index+1<length){index++;continue;}
        if(c=='{'){if(depth++==0)open=index;}
        else if(c=='}'&&depth>0&&--depth==0){close=index;break;}
    }
    if(open==SIZE_MAX||close==SIZE_MAX) {
        if(out->count>=GLOB_MAX_PATTERNS){*too_many=true;return true;}
        return glob_list_push(out,strdup(pattern));
    }
    size_t start=open+1;
    depth=0;
    for(size_t index=open+1;index<=close;index++) {
        const char c=pattern[index];
        if(c=='\\'&&index+1<close){index++;continue;}
        if(c=='{')depth++;
        else if(c=='}'&&depth>0)depth--;
        else if((c==','&&depth==0)||index==close) {
            char *candidate=glob_concat(pattern,open,pattern+start,index-start,
                pattern+close+1,length-close-1);
            if(candidate==nullptr)return false;
            const bool ok=glob_expand_braces(candidate,out,too_many);
            free(candidate);
            if(!ok)return false;
            if(*too_many)return true;
            start=index+1;
        }
    }
    return true;
}

static bool glob_has_magic(const char *segment,size_t length) {
    for(size_t index=0;index<length;index++) {
        const char c=segment[index];
        if(c=='\\'){index++;continue;}
        if(c=='*'||c=='?'||c=='[')return true;
    }
    return false;
}

/* A segment without magic, with its escapes removed. */
static char *glob_unescape(const char *segment,size_t length) {
    char *plain=malloc(length+1);
    if(plain==nullptr)return nullptr;
    size_t used=0;
    for(size_t index=0;index<length;index++) {
        if(segment[index]=='\\'&&index+1<length)index++;
        plain[used++]=segment[index];
    }
    plain[used]='\0';
    return plain;
}

/* The filesystem path for a result path: under base when there is one. */
static char *glob_fs_path(const GlobWalk *walk,const char *shown) {
    if(walk->base==nullptr)return strdup(shown[0]=='\0'?".":shown);
    if(shown[0]=='\0')return strdup(walk->base);
    return glob_concat(walk->base,strlen(walk->base),"/",1,shown,strlen(shown));
}

static bool glob_is_directory(const GlobWalk *walk,const char *shown,bool follow) {
    char *fs=glob_fs_path(walk,shown);
    if(fs==nullptr)return false;
    struct stat status;
    const bool directory=(follow?stat(fs,&status):lstat(fs,&status))==0&&S_ISDIR(status.st_mode);
    free(fs);
    return directory;
}

static bool glob_exists(const GlobWalk *walk,const char *shown) {
    char *fs=glob_fs_path(walk,shown);
    if(fs==nullptr)return false;
    struct stat status;
    const bool exists=lstat(fs,&status)==0;
    free(fs);
    return exists;
}

/* `shown` + "/" + name, without a separator after an empty or "/" prefix. */
static char *glob_child(const char *shown,const char *name) {
    const size_t length=strlen(shown);
    const bool separate=length>0&&shown[length-1]!='/';
    return glob_concat(shown,length,separate?"/":"",separate?1:0,name,strlen(name));
}



/* Matches the segments in `rest` (no leading '/') against the directory `shown`. */
static void glob_match(GlobWalk *walk,const char *shown,const char *rest,size_t depth) {
    if(walk->out_of_memory||depth>GLOB_MAX_DEPTH)return;
    while(rest[0]=='/')rest++;
    if(rest[0]=='\0')return;
    const char *slash=strchr(rest,'/');
    const size_t length=slash==nullptr?strlen(rest):(size_t)(slash-rest);
    const char *after=slash==nullptr?nullptr:slash+1;
    const bool last=after==nullptr;
    /* A pattern ending in "/" matches directories only, shown with the slash. */
    const bool trailing=after!=nullptr&&after[strspn(after,"/")]=='\0';
    if(length==2&&rest[0]=='*'&&rest[1]=='*'&&!last&&!trailing) {
        /* Zero directories here, then each visible real subdirectory, recursively. */
        glob_match(walk,shown,after,depth+1);
        char *fs=glob_fs_path(walk,shown);
        if(fs==nullptr){walk->out_of_memory=true;return;}
        DIR *directory=opendir(fs);
        free(fs);
        if(directory==nullptr)return;
        for(struct dirent *entry=readdir(directory);entry!=nullptr;entry=readdir(directory)) {
            if(entry->d_name[0]=='.')continue;
            char *child=glob_child(shown,entry->d_name);
            if(child==nullptr){walk->out_of_memory=true;break;}
            if(glob_is_directory(walk,child,false))glob_match(walk,child,rest,depth+1);
            free(child);
            if(walk->out_of_memory)break;
        }
        closedir(directory);
        return;
    }
    if(!glob_has_magic(rest,length)) {
        char *plain=glob_unescape(rest,length);
        char *child=plain==nullptr?nullptr:glob_child(shown,plain);
        free(plain);
        if(child==nullptr){walk->out_of_memory=true;return;}
        if(last) {
            if(!glob_exists(walk,child))free(child);
            else if(!glob_list_push(walk->matches,child))walk->out_of_memory=true;
            return;
        }
        if(trailing) {
            if(glob_is_directory(walk,child,true)) {
                char *with_slash=glob_concat(child,strlen(child),"/",1,"",0);
                if(!glob_list_push(walk->matches,with_slash))walk->out_of_memory=true;
            }
        } else if(glob_is_directory(walk,child,true)) {
            glob_match(walk,child,after,depth+1);
        }
        free(child);
        return;
    }
    char *segment=malloc(length+1);
    if(segment==nullptr){walk->out_of_memory=true;return;}
    memcpy(segment,rest,length);segment[length]='\0';
    /* A lone "**" at the end matches like "*". */
    const bool dot_allowed=segment[0]=='.';
    char *fs=glob_fs_path(walk,shown);
    DIR *directory=fs==nullptr?nullptr:opendir(fs);
    if(fs==nullptr)walk->out_of_memory=true;
    free(fs);
    if(directory!=nullptr) {
        for(struct dirent *entry=readdir(directory);entry!=nullptr;entry=readdir(directory)) {
            const char *name=entry->d_name;
            if(strcmp(name,".")==0||strcmp(name,"..")==0)continue;
            if(name[0]=='.'&&!dot_allowed)continue;
            const bool matched=fnmatch(segment,name,0)==0||
                (length==2&&segment[0]=='*'&&segment[1]=='*');
            if(!matched)continue;
            char *child=glob_child(shown,name);
            if(child==nullptr){walk->out_of_memory=true;break;}
            if(last) {
                if(!glob_list_push(walk->matches,child)){walk->out_of_memory=true;break;}
                continue;
            }
            if(trailing) {
                if(glob_is_directory(walk,child,true)) {
                    char *with_slash=glob_concat(child,strlen(child),"/",1,"",0);
                    if(!glob_list_push(walk->matches,with_slash))walk->out_of_memory=true;
                }
            } else if(glob_is_directory(walk,child,true)) {
                glob_match(walk,child,after,depth+1);
            }
            free(child);
            if(walk->out_of_memory)break;
        }
        closedir(directory);
    }
    free(segment);
}

static int glob_compare(const void *a,const void *b) {
    return strcmp(*(char *const *)a,*(char *const *)b);
}

DiamondVmStatus dir_glob_helper(DiamondVm *vm,const DiamondString *pattern,
        const DiamondString *base,DiamondValue *result) {
    if(memchr(pattern->chars,'\0',pattern->length)!=nullptr||
       (base!=nullptr&&memchr(base->chars,'\0',base->length)!=nullptr)) {
        snprintf(vm->error,sizeof vm->error,"Dir.glob pattern and base must not contain a NUL byte");
        return DIAMOND_VM_TYPE_ERROR;
    }
    GlobList patterns={};
    bool too_many=false;
    if(!glob_expand_braces(pattern->chars,&patterns,&too_many)) {
        glob_list_free(&patterns);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    if(too_many) {
        glob_list_free(&patterns);
        snprintf(vm->error,sizeof vm->error,"Dir.glob pattern expands to more than %d patterns",
            GLOB_MAX_PATTERNS);
        return DIAMOND_VM_TYPE_ERROR;
    }
    GlobList matches={};
    GlobWalk walk={.vm=vm,.matches=&matches,
        .base=base==nullptr||base->length==0?nullptr:base->chars};
    for(size_t index=0;index<patterns.count&&!walk.out_of_memory;index++) {
        const char *text=patterns.items[index];
        if(text[0]=='/')glob_match(&walk,"/",text+1,0);
        else glob_match(&walk,"",text,0);
    }
    glob_list_free(&patterns);
    if(walk.out_of_memory){glob_list_free(&matches);return DIAMOND_VM_OUT_OF_MEMORY;}
    if(matches.count>1)qsort(matches.items,matches.count,sizeof *matches.items,glob_compare);
    /* Rooted in *result (a register) before any String is allocated. */
    DiamondArray *array=allocate_array(vm,nullptr,0);
    if(array==nullptr){glob_list_free(&matches);return DIAMOND_VM_OUT_OF_MEMORY;}
    *result=DIAMOND_OBJECT(array);
    for(size_t index=0;index<matches.count;index++) {
        if(index>0&&strcmp(matches.items[index],matches.items[index-1])==0)continue;
        DiamondString *text=allocate_string(vm,matches.items[index],strlen(matches.items[index]));
        if(text==nullptr||!array_push(vm,array,DIAMOND_OBJECT(text))) {
            glob_list_free(&matches);
            return DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    glob_list_free(&matches);
    return DIAMOND_VM_OK;
}
