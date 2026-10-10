#include "compile_buffer.h"
#include "compiled_prelude.h"
#include "receiver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DiamondProgram *round_trip(const DiamondProgram *program) {
    FILE *file=tmpfile();
    if(file==nullptr)return nullptr;
    if(!diamond_program_write_compiled(program,file)) {fclose(file);return nullptr;}
    const long length=ftell(file);
    if(length<=0) {fclose(file);return nullptr;}
    rewind(file);
    uint8_t *bytes=malloc((size_t)length);
    DiamondProgram *restored=calloc(1,sizeof *restored);
    const bool ok=bytes!=nullptr&&restored!=nullptr&&
        fread(bytes,1,(size_t)length,file)==(size_t)length&&
        diamond_program_read_compiled(bytes,(size_t)length,restored);
    fclose(file);free(bytes);
    if(!ok) {
        if(restored!=nullptr)diamond_program_free(restored);
        free(restored);return nullptr;
    }
    return restored;
}

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
        "class SchemaHolder\n"
        "  attr_reader pet\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class LateSchemaHolder\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "  attr_reader pet\n"
        "end\n"
        "class MutableHolder\n"
        "  attr_accessor pet\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class ConflictingHolder\n"
        "  attr_reader pet\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "  def replace()\n"
        "    @pet = Leaf.new()\n"
        "  end\n"
        "end\n"
        "class ReopenedHolder\n"
        "  attr_reader pet\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class ReopenedHolder\n"
        "  def replace(value)\n"
        "    @pet = value\n"
        "  end\n"
        "end\n"
        "class AnnotatedHolder\n"
        "  attr_reader pet: Leaf\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class UnknownHolder\n"
        "  attr_reader pet\n"
        "end\n"
        "class ParentHolder\n"
        "  attr_reader pet\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class ChildHolder < ParentHolder\n"
        "  def initialize()\n"
        "    @pet = Leaf.new()\n"
        "  end\n"
        "end\n"
        "class InheritedHolder < ParentHolder\n"
        "end\n"
        "class GrandchildHolder < InheritedHolder\n"
        "end\n"
        "class SameHolder < ParentHolder\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class EmptyParent\n"
        "  attr_reader pet\n"
        "end\n"
        "class LeafHolder < EmptyParent\n"
        "  def initialize()\n"
        "    @pet = Leaf.new()\n"
        "  end\n"
        "end\n"
        "class PetHolder < EmptyParent\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class MutableChild < ParentHolder\n"
        "  attr_writer pet\n"
        "end\n"
        "class ReopenedParent\n"
        "  attr_reader pet\n"
        "  def initialize()\n"
        "    @pet = Pet.new()\n"
        "  end\n"
        "end\n"
        "class EarlyChild < ReopenedParent\n"
        "end\n"
        "class ReopenedParent\n"
        "  def replace(value)\n"
        "    @pet = value\n"
        "  end\n"
        "end\n"
        "class ExplicitChild < AnnotatedHolder\n"
        "end\n"
        "class OverrideReader < ParentHolder\n"
        "  def pet() = Leaf.new()\n"
        "end\n"
        "def identity[T](value: T)\n"
        "  value\n"
        "end\n"
        "def array_identity[T](values: Array[T]) -> Array[T]\n"
        "  values\n"
        "end\n"
        "def hash_value[T](values: Hash[String, T]) -> T\n"
        "  values[\"value\"]\n"
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
        "def make_pet() = Pet.new()\n"
        "def wrap_pet() = make_pet()\n"
        "def wrap_again() = wrap_pet()\n"
        "def wrap_reader(holder: SchemaHolder) = holder.pet()\n"
        "def early_wrapper(flag)\n"
        "  if flag then return make_pet() end\n"
        "  wrap_again()\n"
        "end\n"
        "def unknown_wrapper(flag, unknown)\n"
        "  if flag then return make_pet() end\n"
        "  unknown\n"
        "end\n"
        "def unknown_early_wrapper(flag, unknown)\n"
        "  if flag then return unknown end\n"
        "  make_pet()\n"
        "end\n"
        "def union_wrapper(flag)\n"
        "  if flag then return make_pet() end\n"
        "  Leaf.new()\n"
        "end\n"
        "def nested_wrapper(flag)\n"
        "  if flag then return make_pet() end\n"
        "  def unrelated()\n"
        "    return Leaf.new()\n"
        "  end\n"
        "  make_pet()\n"
        "end\n"
        "class PetWrappers\n"
        "  def self.make() = wrap_again()\n"
        "  def make() = wrap_again()\n"
        "end\n"
        "def known_then_unknown(flag, unknown)\n"
        "  if flag then return Pet.new() end\n"
        "  unknown\n"
        "end\n"
        "def all_early_wrapper(flag)\n"
        "  if flag then return make_pet() else return wrap_again() end\n"
        "end\n"
        "def annotated_wrapper() -> Leaf = make_pet()\n"
        "def block_wrapper(flag)\n"
        "  if flag then return make_pet() end\n"
        "  [1].each() do |item|\n"
        "    next Leaf.new()\n"
        "  end\n"
        "  wrap_again()\n"
        "end\n"
        "def nonlocal_wrapper(flag)\n"
        "  [1].each() do |item|\n"
        "    if flag then return Leaf.new() end\n"
        "  end\n"
        "  make_pet()\n"
        "end\n"
        "def forward_use()\n"
        "  early_pet = forward_outer()\n"
        "  early_pet.bark()\n"
        "end\n"
        "def forward_return()\n"
        "  return forward_outer()\n"
        "end\n"
        "def forward_unknown_tail(flag, value)\n"
        "  if flag then return forward_inner() end\n"
        "  value\n"
        "end\n"
        "def forward_unknown_early(flag, value)\n"
        "  if flag then return value end\n"
        "  forward_inner()\n"
        "end\n"
        "def forward_generic_wrapper() = forward_generic[Pet](Pet.new())\n"
        "def forward_generic[T](value: T) = value\n"
        "def forward_outer() = forward_middle()\n"
        "def backward_consumer() = forward_outer()\n"
        "def forward_middle() = forward_inner()\n"
        "def forward_inner() = Pet.new()\n"
        "def recursive_left() = recursive_right()\n"
        "def recursive_right() = recursive_left()\n"
        "class ForwardFactory\n"
        "  def self.outer() = ForwardFactory.middle()\n"
        "  def self.middle() = ForwardFactory.inner()\n"
        "  def self.inner() = Leaf.new()\n"
        "  def outer() = self.middle()\n"
        "  def middle() = self.inner()\n"
        "  def inner() = Pet.new()\n"
        "end\n"
        "class ForwardBase\n"
        "  def item() = Leaf.new()\n"
        "end\n"
        "class ForwardOverride < ForwardBase\n"
        "  def wrapped() = self.item()\n"
        "  def item() = Pet.new()\n"
        "end\n"
        "class ForwardAnnotated < ForwardBase\n"
        "  def wrapped() = self.item()\n"
        "  def item() -> Pet = Pet.new()\n"
        "end\n"
        "def inspect_receivers()\n"
        "  LateSchemaHolder.new().pet().bark()\n"
        "  MutableHolder.new().pet().bark()\n"
        "  ConflictingHolder.new().pet().bark()\n"
        "  ReopenedHolder.new().pet().bark()\n"
        "  AnnotatedHolder.new().pet().bark()\n"
        "  UnknownHolder.new().pet().bark()\n"
        "  ParentHolder.new().pet().bark()\n"
        "  ChildHolder.new().pet().bark()\n"
        "  InheritedHolder.new().pet().bark()\n"
        "  GrandchildHolder.new().pet().bark()\n"
        "  SameHolder.new().pet().bark()\n"
        "  LeafHolder.new().pet().bark()\n"
        "  PetHolder.new().pet().bark()\n"
        "  MutableChild.new().pet().bark()\n"
        "  EarlyChild.new().pet().bark()\n"
        "  inherited_pet = InheritedHolder.new().pet()\n"
        "  inherited_pet.bark()\n"
        "  leaf_pet = LeafHolder.new().pet()\n"
        "  leaf_pet.bark()\n"
        "  ExplicitChild.new().pet().bark()\n"
        "  OverrideReader.new().pet().bark()\n"
        "  overridden_pet = OverrideReader.new().pet()\n"
        "  overridden_pet.bark()\n"
        "  wrap_pet().bark()\n"
        "  wrap_again().bark()\n"
        "  wrap_reader(SchemaHolder.new()).bark()\n"
        "  early_wrapper(true).bark()\n"
        "  unknown_wrapper(true, nil).bark()\n"
        "  unknown_early_wrapper(true, nil).bark()\n"
        "  union_wrapper(true).bark()\n"
        "  nested_wrapper(true).bark()\n"
        "  PetWrappers.make().bark()\n"
        "  PetWrappers.new().make().bark()\n"
        "  wrapped_pet = wrap_again()\n"
        "  wrapped_pet.bark()\n"
        "  known_then_unknown(true, nil).bark()\n"
        "  all_early_wrapper(true).bark()\n"
        "  annotated_wrapper().bark()\n"
        "  block_wrapper(true).bark()\n"
        "  nonlocal_wrapper(true).bark()\n"
        "  ForwardOverride.new().wrapped().bark()\n"
        "  forward_unknown_tail(true, nil).bark()\n"
        "  forward_unknown_early(true, nil).bark()\n"
        "  forward_generic_wrapper().bark()\n"
        "  ForwardAnnotated.new().wrapped().bark()\n"
        "  forward_outer().bark()\n"
        "  forward_return().bark()\n"
        "  backward_pet = backward_consumer()\n"
        "  backward_pet.bark()\n"
        "  ForwardFactory.new().outer().bark()\n"
        "  ForwardFactory.outer().bark()\n"
        "  forward_pet = forward_outer()\n"
        "  forward_pet.bark()\n"
        "  recursive_left().bark()\n"
        "  holder = SchemaHolder.new()\n"
        "  holder.pet().bark()\n"
        "  held_pet = holder.pet()\n"
        "  held_pet.bark()\n"
        "  Pet.new().bark()\n"
        "  identity(Pet.new()).bark()\n"
        "  identity[Array[Array[Pet]]]([[Pet.new()]])[0][0].bark()\n"
        "  identity[Pet | Leaf](Pet.new()).bark()\n"
        "  identity[Array[Pet | Leaf]]([Pet.new()])[0].bark()\n"
        "  matching_factory(true).pets()[0].bark()\n"
        "  conflicting_factory(true).pets()[0].bark()\n"
        "end\n"
        "def inspect_reader_unions(holders: PetHolder | LeafHolder, uncertain: PetHolder | MutableChild)\n"
        "  holders.pet().bark()\n"
        "  union_pet = holders.pet()\n"
        "  union_pet.bark()\n"
        "  uncertain.pet().bark()\n"
        "  uncertain_pet = uncertain.pet()\n"
        "  uncertain_pet.bark()\n"
        "end\n"
        "def indexed_factory() = [[Pet.new()]]\n"
        "def indexed_union_factory() = [Pet.new(), Leaf.new()]\n"
        "def inspect_indexed_assignments()\n"
        "  indexed_pet = indexed_factory()[0][0]\n"
        "  indexed_pet.bark()\n"
        "  indexed_union = indexed_union_factory()[0]\n"
        "  indexed_union.bark()\n"
        "end\n"
        "def literal_pet_factory() = Pet.new()\n"
        "def literal_nested_factory() = [[literal_pet_factory()]]\n"
        "def inspect_literal_results(value)\n"
        "  literal_pets = [literal_pet_factory()]\n"
        "  literal_pets[0].bark()\n"
        "  literal_pet = literal_pets[0]\n"
        "  literal_pet.bark()\n"
        "  nested_literal_pet = literal_nested_factory()[0][0]\n"
        "  nested_literal_pet.bark()\n"
        "  literal_union = [literal_pet_factory(), Leaf.new()]\n"
        "  literal_union[0].bark()\n"
        "  unknown_literal = [literal_pet_factory(), value]\n"
        "  unknown_literal[0].bark()\n"
        "end\n"
        "def inspect_inferred_unions(value: Pet | Leaf, values: Array[Pet | Leaf], map: Hash[String, Pet | Leaf])\n"
        "  identity(value).bark()\n"
        "  array_identity(values)[0].bark()\n"
        "  hash_value(map).bark()\n"
        "end\n";

    char *combined=diamond_lsp_build_compile_buffer(nullptr,source,sizeof source-1,
        nullptr,nullptr,nullptr,nullptr);
    if(combined==nullptr)return 1;
    DiamondProgram *program=calloc(1,sizeof *program);
    if(program==nullptr) {free(combined);return 1;}
    DiamondDiagnostic diagnostic;
    if(!diamond_compile_for_tooling(combined,program,&diagnostic)) {
        fprintf(stderr,"receiver test compile failed: %s\n",diagnostic.message);
        free(combined);free(program);return 1;
    }
    const DiamondChunk chunk=diamond_program_chunk(program);
    int failed=0;
    failed|=check_receiver(program,&chunk,combined,"LateSchemaHolder.new().pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"MutableHolder.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"ConflictingHolder.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"ReopenedHolder.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"AnnotatedHolder.new().pet().","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"UnknownHolder.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"ParentHolder.new().pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"ChildHolder.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"InheritedHolder.new().pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"GrandchildHolder.new().pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"SameHolder.new().pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"LeafHolder.new().pet().","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"PetHolder.new().pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"MutableChild.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"EarlyChild.new().pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"inherited_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"leaf_pet.","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"ExplicitChild.new().pet().","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"OverrideReader.new().pet().","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"overridden_pet.","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"holder.pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"held_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"Pet.new().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"identity(Pet.new()).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,
        "identity[Array[Array[Pet]]]([[Pet.new()]])[0][0].","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,
        "identity[Pet | Leaf](Pet.new()).","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "identity[Array[Pet | Leaf]]([Pet.new()])[0].","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "identity(value).","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "array_identity(values)[0].","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "hash_value(map).","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,
        "matching_factory(true).pets()[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).pets()[0].",nullptr,false);
    failed|=check_receiver_pair(program,&chunk,combined,"holders.pet().","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,"union_pet.","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"uncertain.pet().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"uncertain_pet.",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"indexed_pet.","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,"indexed_union.","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"literal_pets[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"literal_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"nested_literal_pet.","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,"literal_union[0].","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"unknown_literal[0].",nullptr,false);
    /* Inferred element facts must stay outside checked compiler contracts. */
    bool found_indexed_local=false;
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(strcmp(function->name,"inspect_indexed_assignments")!=0)continue;
        for(size_t local=0;local<function->scope_local_count;local++) {
            const DiamondScopeLocal *fact=&function->scope_locals[local];
            if(strcmp(fact->name,"indexed_pet")!=0)continue;
            found_indexed_local=true;
            if(fact->known_type_set>=0||fact->tooling_type_set<0) {
                fprintf(stderr,"indexed receiver fact is not tooling-only\n");
                failed=1;
            }
        }
    }
    if(!found_indexed_local)failed=1;
    bool found_literal_local=false;
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(strcmp(function->name,"inspect_literal_results")!=0)continue;
        for(size_t local=0;local<function->scope_local_count;local++) {
            const DiamondScopeLocal *fact=&function->scope_locals[local];
            if(strcmp(fact->name,"literal_pets")!=0)continue;
            found_literal_local=true;
            if(fact->known_type!=DIAMOND_TYPE_ARRAY||
               fact->known_type_set>=0||fact->tooling_type_set<0) {
                fprintf(stderr,"literal element graph entered checked facts\n");
                failed=1;
            }
        }
    }
    if(!found_literal_local)failed=1;
    DiamondProgram *restored=round_trip(program);
    if(restored==nullptr) {
        fprintf(stderr,"receiver program serialization failed\n");failed=1;
    } else {
        const DiamondChunk restored_chunk=diamond_program_chunk(restored);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "literal_pets[0].","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "nested_literal_pet.","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "indexed_pet.","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "GrandchildHolder.new().pet().","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "LeafHolder.new().pet().","Leaf",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "EarlyChild.new().pet().",nullptr,false);
        failed|=check_receiver_pair(restored,&restored_chunk,combined,
            "holders.pet().","Pet","Leaf");
        diamond_program_free(restored);free(restored);
    }
    failed|=check_receiver(program,&chunk,combined,"wrap_pet().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"wrap_again().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"wrap_reader(SchemaHolder.new()).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"early_wrapper(true).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"nested_wrapper(true).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"PetWrappers.make().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"PetWrappers.new().make().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"wrapped_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"unknown_wrapper(true, nil).",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"unknown_early_wrapper(true, nil).",nullptr,false);
    failed|=check_receiver_pair(program,&chunk,combined,"union_wrapper(true).","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"known_then_unknown(true, nil).",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"all_early_wrapper(true).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"annotated_wrapper().","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"block_wrapper(true).","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"nonlocal_wrapper(true).",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"forward_outer().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"ForwardFactory.outer().","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"forward_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"backward_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"recursive_left().",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"early_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"forward_return().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"ForwardFactory.new().outer().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"ForwardOverride.new().wrapped().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"forward_unknown_tail(true, nil).",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"forward_unknown_early(true, nil).",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"forward_generic_wrapper().","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"ForwardAnnotated.new().wrapped().","Pet",false);
    diamond_program_free(program);
    free(program);free(combined);
    return failed?1:0;
}
