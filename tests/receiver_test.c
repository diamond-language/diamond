#include "compile_buffer.h"
#include "receiver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check_receiver(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *combined,const char *needle,const char *expected,bool singleton) {
    const char *position=strstr(combined,needle);
    if(position==nullptr) {
        fprintf(stderr,"receiver test expression not found: %s\n",needle);
        return 1;
    }
    size_t classes[DIAMOND_MAX_UNION_TYPES];
    bool actual_singleton=false;
    const size_t count=receiver_resolve_classes(program,chunk,combined,
        (size_t)(position-combined)+strlen(needle),classes,
        DIAMOND_MAX_UNION_TYPES,&actual_singleton);
    if(expected==nullptr) {
        if(count==0)return 0;
        fprintf(stderr,"expected no receiver for %s, got %zu\n",needle,count);
        return 1;
    }
    if(count!=1||classes[0]>=chunk->class_count||
       strcmp(chunk->classes[classes[0]].name,expected)!=0||
       actual_singleton!=singleton) {
        fprintf(stderr,"unexpected receiver for %s: count=%zu class=%s singleton=%d\n",
            needle,count,count==1&&classes[0]<chunk->class_count
                ?chunk->classes[classes[0]].name:"<none>",actual_singleton);
        return 1;
    }
    return 0;
}

static int check_receiver_pair(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *combined,const char *needle,const char *first,const char *second) {
    const char *position=strstr(combined,needle);
    if(position==nullptr)return 1;
    size_t classes[DIAMOND_MAX_UNION_TYPES];bool singleton=false;
    const size_t count=receiver_resolve_classes(program,chunk,combined,
        (size_t)(position-combined)+strlen(needle),classes,
        DIAMOND_MAX_UNION_TYPES,&singleton);
    bool found_first=false,found_second=false;
    for(size_t index=0;index<count;index++) {
        if(strcmp(chunk->classes[classes[index]].name,first)==0)found_first=true;
        if(strcmp(chunk->classes[classes[index]].name,second)==0)found_second=true;
    }
    if(count==2&&!singleton&&found_first&&found_second)return 0;
    fprintf(stderr,"unexpected union receiver for %s: count=%zu\n",needle,count);
    return 1;
}

int main(void) {
    static const char source[] =
        "class Pet\n"
        "  def bark()\n"
        "    1\n"
        "  end\n"
        "end\n"
        "class Leaf\n"
        "  def ping()\n"
        "    1\n"
        "  end\n"
        "end\n"
        "def identity[T](value: T)\n"
        "  value\n"
        "end\n"
        "class PetFactoryLeft\n"
        "  def pets()\n"
        "    [Pet.new()]\n"
        "  end\n"
        "end\n"
        "class PetFactoryRight\n"
        "  def pets()\n"
        "    [Pet.new()]\n"
        "  end\n"
        "end\n"
        "class LeafFactory\n"
        "  def pets()\n"
        "    [Leaf.new()]\n"
        "  end\n"
        "end\n"
        "def matching_factory(flag)\n"
        "  flag ? PetFactoryLeft.new() : PetFactoryRight.new()\n"
        "end\n"
        "def conflicting_factory(flag)\n"
        "  flag ? PetFactoryLeft.new() : LeafFactory.new()\n"
        "end\n"
        "def inspect_receivers()\n"
        "  Pet.new().bark()\n"
        "  identity(Pet.new()).bark()\n"
        "  identity[Array[Array[Pet]]]([[Pet.new()]])[0][0].bark()\n"
        "  identity[Pet | Leaf](Pet.new()).bark()\n"
        "  identity[Array[Pet | Leaf]]([Pet.new()])[0].bark()\n"
        "  matching_factory(true).pets()[0].bark()\n"
        "  conflicting_factory(true).pets()[0].bark()\n"
        "end\n";

    char *combined=diamond_lsp_build_compile_buffer(nullptr,source,sizeof source-1,
        nullptr,nullptr,nullptr,nullptr);
    if(combined==nullptr)return 1;
    DiamondProgram *program=calloc(1,sizeof *program);
    if(program==nullptr) {free(combined);return 1;}
    DiamondDiagnostic diagnostic;
    if(!diamond_compile(combined,program,&diagnostic)) {
        fprintf(stderr,"receiver test compile failed: %s\n",diagnostic.message);
        free(combined);free(program);return 1;
    }
    const DiamondChunk chunk=diamond_program_chunk(program);
    int failed=0;
    failed|=check_receiver(program,&chunk,combined,"Pet.new().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"identity(Pet.new()).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,
        "identity[Array[Array[Pet]]]([[Pet.new()]])[0][0].","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,
        "identity[Pet | Leaf](Pet.new()).","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "identity[Array[Pet | Leaf]]([Pet.new()])[0].","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,
        "matching_factory(true).pets()[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).pets()[0].",nullptr,false);
    diamond_program_free(program);
    free(program);free(combined);
    return failed?1:0;
}
