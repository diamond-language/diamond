#define _POSIX_C_SOURCE 200809L
#include "analysis_cache.h"
#include "compile_buffer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *override_source(const char *path,void *data) {
    (void)path;
    return strdup((const char *)data);
}

static bool write_source(const char *path,const char *source) {
    FILE *file=fopen(path,"w");
    if(file==nullptr)return false;
    const bool written=fputs(source,file)>=0;
    return fclose(file)==0&&written;
}

static bool check(const char *path,const char *source,const char *override,
        const char *expected,const char *absent) {
    DiamondSourceBundle bundle;
    char *combined=diamond_lsp_build_compile_buffer(path,source,strlen(source),
        override!=nullptr?override_source:nullptr,(void *)override,&bundle,nullptr);
    if(combined==nullptr)return false;
    const DiamondProgram *program=diamond_lsp_analyze(combined);
    bool found=false,unexpected=false;
    if(program!=nullptr) {
        for(size_t index=0;index<program->function_count;index++) {
            const char *name=program->functions[index]->name;
            if(expected!=nullptr&&strcmp(name,expected)==0)found=true;
            if(absent!=nullptr&&strcmp(name,absent)==0)unexpected=true;
        }
    }
    const bool ok=expected==nullptr?program==nullptr:found&&!unexpected;
    if(!ok)fprintf(stderr,"cache test: expected %s, absent %s\n",
        expected!=nullptr?expected:"compile failure",absent!=nullptr?absent:"<none>");
    free(combined);diamond_source_bundle_free(&bundle);
    return ok;
}

int main(void) {
    char directory[]="/tmp/diamond-analysis-cache-XXXXXX";
    if(mkdtemp(directory)==nullptr)return 1;
    char root[256],helper[256];
    snprintf(root,sizeof root,"%s/main.di",directory);
    snprintf(helper,sizeof helper,"%s/helper.di",directory);
    const char *source="require \"./helper\"\n";
    bool ok=write_source(helper,"def cached_disk() = 1\n");
    /* Repeated unchanged buffers must compile only once. The shell harness
     * counts actual compiler traces, not allocation addresses or cache flags. */
    for(size_t index=0;ok&&index<3;index++)
        ok=check(root,source,nullptr,"cached_disk",nullptr);
    /* Unsaved imports override disk; closing the import restores disk facts. */
    if(ok)ok=check(root,source,"def cached_open() = 2\n","cached_open","cached_disk");
    if(ok)ok=check(root,source,nullptr,"cached_disk","cached_open");
    /* Disk edits must invalidate even when the root buffer stays unchanged. */
    if(ok)ok=write_source(helper,"def cached_changed() = 3\n");
    if(ok)ok=check(root,source,nullptr,"cached_changed","cached_disk");
    const char *edited="require \"./helper\"\ndef cached_root() = 4\n";
    if(ok)ok=check(root,edited,nullptr,"cached_root",nullptr);
    if(ok)ok=check(root,edited,nullptr,"cached_changed","cached_disk");
    /* A failed compile must never return the preceding successful program. */
    if(ok)ok=check(root,"def broken(\n",nullptr,nullptr,nullptr);
    if(ok)ok=check(root,edited,nullptr,"cached_root",nullptr);
    diamond_lsp_analysis_clear();
    /* Clearing must release both the key and program, and permit reuse. */
    if(ok)ok=check(root,edited,nullptr,"cached_root",nullptr);
    diamond_lsp_analysis_clear();
    unlink(helper);rmdir(directory);
    return ok?0:1;
}
