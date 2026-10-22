#include "compile_buffer.h"
#include "compiled_prelude.h"
#include "disassemble.h"
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

/* An editor-only guard must not elide the checked return contract. */
static int check_return_contract(const DiamondChunk *chunk,const char *name) {
    FILE *file=tmpfile();
    if(file==nullptr)return 1;
    if(!diamond_disassemble(file,"receiver-test",chunk)) {fclose(file);return 1;}
    const long length=ftell(file);
    if(length<0) {fclose(file);return 1;}
    char *text=malloc((size_t)length+1);
    if(text==nullptr) {fclose(file);return 1;}
    rewind(file);
    const size_t read=fread(text,1,(size_t)length,file);
    fclose(file);text[read]='\0';
    char header[128];snprintf(header,sizeof header,"== %s ==\n",name);
    const char *body=strstr(text,header);
    size_t checks=0;
    if(body!=nullptr) {
        body+=strlen(header);
        const char *end=strstr(body,"\n== ");
        for(const char *cursor=body;(cursor=strstr(cursor,"CHECK_TYPE"))!=nullptr;cursor++) {
            if(end!=nullptr&&cursor>=end)break;
            checks++;
        }
    }
    const bool found=body!=nullptr;
    free(text);
    if(read==(size_t)length&&found&&checks==1)return 0;
    fprintf(stderr,"%s lost its checked return contract: %zu checks\n",name,checks);
    return 1;
}

static int check_receiver_after(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *combined,const char *after,const char *needle,const char *expected,bool singleton) {
    const char *anchor=after==nullptr?combined:strstr(combined,after);
    const char *position=anchor==nullptr?nullptr:strstr(anchor,needle);
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
       strcmp(chunk->classes[classes[0]]->name,expected)!=0||
       actual_singleton!=singleton) {
        fprintf(stderr,"unexpected receiver for %s: count=%zu class=%s singleton=%d\n",
            needle,count,count==1&classes[0]<chunk->class_count
                ?chunk->classes[classes[0]]->name:"<none>",actual_singleton);
        return 1;
    }
    return 0;
}

static int check_receiver(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *combined,const char *needle,const char *expected,bool singleton) {
    return check_receiver_after(program,chunk,combined,nullptr,needle,expected,singleton);
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
        if(strcmp(chunk->classes[classes[index]]->name,first)==0)found_first=true;
        if(strcmp(chunk->classes[classes[index]]->name,second)==0)found_second=true;
    }
    if(count==2&&!singleton&&found_first&&found_second)return 0;
    fprintf(stderr,"unexpected union receiver for %s: count=%zu\n",needle,count);
    return 1;
}

/* A built-in receiver's method result. A predicate call tries the extension function written
 * with the `?` first, then the one without it; a name without a `?` never reaches the other. */
static int check_builtin(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *combined,const char *needle,bool expect_found,uint16_t expected) {
    const char *position=strstr(combined,needle);
    if(position==nullptr) {
        fprintf(stderr,"receiver test expression not found: %s\n",needle);
        return 1;
    }
    uint16_t type=0;
    const bool found=receiver_resolve_builtin_type(program,chunk,combined,
        (size_t)(position-combined)+strlen(needle),&type);
    if(found==expect_found&&(!found||type==expected))return 0;
    fprintf(stderr,"unexpected built-in type for %s: found=%d type=%u\n",needle,found,type);
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
        "  def nested() = [[Pet.new()]]\n"
        "  def pick[T, U](first: T, second: U) -> Array[T] = [first]\n"
        "  def unsafe(value) = [Pet.new()]\n"
        "  def mixed() = [Pet.new()]\n"
        "  def nullable() = [Pet.new()]\n"
        "  def different_depth() = [[Pet.new()]]\n"
        "  def only() = [Pet.new()]\n"
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
        "  def nested() = [[Leaf.new()]]\n"
        "  def pick[T, U](first: T, second: U) -> Array[U] = [second]\n"
        "  def unsafe(value) = value\n"
        "  def mixed() = 42\n"
        "  def nullable() = nil\n"
        "  def different_depth() = [Leaf.new()]\n"
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
        "  conflicting_factory(true).pets().reverse().take(1)[0].bark()\n"
        "  conflicting_factory(true).nested()[0][0].bark()\n"
        "  conflicting_factory(true).pick(Pet.new(), Leaf.new())[0].bark()\n"
        "  conflicting_factory(true).pick[Pet, Leaf](Pet.new(), Leaf.new())[0].bark()\n"
        "  conflicting_factory(true).unsafe(nil)[0].bark()\n"
        "  conflicting_factory(true).mixed()[0].bark()\n"
        "  conflicting_factory(true).nullable()[0].bark()\n"
        "  conflicting_factory(true).only()[0].bark()\n"
        "  conflicting_factory(true).different_depth()[0][0].bark()\n"
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
        "def spread_factory() = [literal_pet_factory()]\n"
        "def spread_wrapper() = [literal_pet_factory(), *spread_factory(), Pet.new()]\n"
        "def inspect_spread_results(value, checked: Array[Pet])\n"
        "  spread_pets = [*spread_factory()]\n"
        "  spread_pets[0].bark()\n"
        "  spread_pet = spread_wrapper()[0]\n"
        "  spread_pet.bark()\n"
        "  spread_mixed = [Leaf.new(), *spread_factory()]\n"
        "  spread_mixed[0].bark()\n"
        "  spread_nested = [*literal_nested_factory()]\n"
        "  spread_nested[0][0].bark()\n"
        "  spread_checked = [literal_pet_factory(), *checked]\n"
        "  spread_checked[0].bark()\n"
        "  spread_unknown = [value, *spread_factory()]\n"
        "  spread_unknown[0].bark()\n"
        "  spread_dynamic = [literal_pet_factory(), *value]\n"
        "  spread_dynamic[0].bark()\n"
        "end\n"
        "class TransformOwner\n"
        "  def reverse() = [Leaf.new()]\n"
        "end\n"
        "def transform_wrapper() = spread_factory().reverse().take(1)\n"
        "def inspect_transform_results(value)\n"
        "  spread_factory().reverse().take(1)[0].bark()\n"
        "  TransformOwner.new().reverse()[0].bark()\n"
        "  reversed_pet = spread_factory().reverse()[0]\n"
        "  reversed_pet.bark()\n"
        "  unique_pets = spread_factory().uniq()\n"
        "  unique_pets[0].bark()\n"
        "  compact_pets = spread_factory().compact()\n"
        "  compact_pets[0].bark()\n"
        "  taken_pets = spread_factory().take(1)\n"
        "  taken_pets[0].bark()\n"
        "  dropped_pets = spread_factory().drop(n: 0)\n"
        "  dropped_pets[0].bark()\n"
        "  transformed_nested = literal_nested_factory().reverse().take(1)\n"
        "  transformed_nested[0][0].bark()\n"
        "  transformed_union = [literal_pet_factory(), Leaf.new()].reverse()\n"
        "  transformed_union[0].bark()\n"
        "  wrapped_pet = transform_wrapper()[0]\n"
        "  wrapped_pet.bark()\n"
        "  unknown_transform = [literal_pet_factory(), value].reverse()\n"
        "  unknown_transform[0].bark()\n"
        "  mutable_transform = spread_factory().reverse()\n"
        "  transform_alias = mutable_transform\n"
        "  mutable_transform.push(value)\n"
        "  transform_alias[0].bark()\n"
        "  changed_transform = spread_factory().reverse()\n"
        "  changed_alias = changed_transform\n"
        "  changed_transform[0] = value\n"
        "  changed_alias[0].bark()\n"
        "end\n"
        "def inspect_inferred_unions(value: Pet | Leaf, values: Array[Pet | Leaf], map: Hash[String, Pet | Leaf])\n"
        "  identity(value).bark()\n"
        "  array_identity(values)[0].bark()\n"
        "  hash_value(map).bark()\n"
        "end\n"
        "module Qualified\n"
        "  class Widget\n"
        "    def spin()\n"
        "      1\n"
        "    end\n"
        "    def part()\n"
        "      Gear.new()\n"
        "    end\n"
        "    def self.build()\n"
        "      Widget.new()\n"
        "    end\n"
        "  end\n"
        "  class Gear\n"
        "    def turn()\n"
        "      1\n"
        "    end\n"
        "  end\n"
        "  module Inner\n"
        "    class Deep\n"
        "      def dive()\n"
        "        1\n"
        "      end\n"
        "    end\n"
        "  end\n"
        "  def self.make()\n"
        "    Widget.new()\n"
        "  end\n"
        "  def self.make_inner()\n"
        "    Qualified::Inner::Deep.new()\n"
        "  end\n"
        "  def self.unknown(value)\n"
        "    value\n"
        "  end\n"
        "end\n"
        "def qualified_probes()\n"
        "  Qualified::Widget.new().spin()\n"
        "  Qualified::Widget.new().part().turn()\n"
        "  Qualified::Widget.build().spin()\n"
        "  Qualified.make().spin()\n"
        "  Qualified.make().part().turn()\n"
        "  Qualified::Inner::Deep.new().dive()\n"
        "  Qualified.make_inner().dive()\n"
        "  Qualified.unknown(1).spin()\n"
        "end\n"
        "def guarded_hash(pets: Hash[String, Pet], flag)\n"
        "  pet = pets[\"one\"]\n"
        "  # before_guard\n"
        "  pet.bark()\n"
        "  if pet != nil\n"
        "    # non_nil_branch\n"
        "    pet.bark()\n"
        "  else\n"
        "    # nil_branch\n"
        "    pet.bark()\n"
        "  end\n"
        "  # after_guard\n"
        "  pet.bark()\n"
        "  if pet != nil && flag\n"
        "    # conjunctive_branch\n"
        "    pet.bark()\n"
        "  else\n"
        "    # uncertain_else\n"
        "    pet.bark()\n"
        "  end\n"
        "  unless pet == nil\n"
        "    # unless_branch\n"
        "    pet.bark()\n"
        "  end\n"
        "  if pet == nil\n"
        "    0\n"
        "  else\n"
        "    # inverted_else\n"
        "    pet.bark()\n"
        "  end\n"
        "  if pet != nil || flag\n"
        "    # uncertain_or\n"
        "    pet.bark()\n"
        "  end\n"
        "  if pet != nil\n"
        "    pet = nil\n"
        "    # reassigned_branch\n"
        "    pet.bark()\n"
        "  end\n"
        "end\n"
        "def early_hash(pets: Hash[String, Pet])\n"
        "  early_pet = pets[\"one\"]\n"
        "  return if early_pet == nil\n"
        "  # after_early_return\n"
        "  early_pet.bark()\n"
        "end\n"
        "def guarded_union_hash(pets: Hash[String, Pet | Leaf])\n"
        "  union_pet = pets[\"one\"]\n"
        "  if union_pet != nil\n"
        "    union_pet.bark()\n"
        "  end\n"
        "end\n"
        "def guarded_mixed_hash(pets: Hash[String, Pet | Int])\n"
        "  mixed_pet = pets[\"one\"]\n"
        "  if mixed_pet != nil\n"
        "    mixed_pet.bark()\n"
        "  end\n"
        "end\n"
        "def nested_hash_guard(pets: Hash[String, Pet], flag)\n"
        "  nested_pet = pets[\"one\"]\n"
        "  if nested_pet != nil\n"
        "    if flag\n"
        "      nested_pet = nil\n"
        "    else\n"
        "      # restored_nested_else\n"
        "      nested_pet.bark()\n"
        "    end\n"
        "    # uncertain_nested_join\n"
        "    nested_pet.bark()\n"
        "  end\n"
        "end\n"
        "def inferred_pet_hash() = {\"one\": Pet.new()}\n"
        "def indirect_pet_hash() = {\"one\": literal_pet_factory()}\n"
        "def inferred_hash_wrapper() = inferred_pet_hash()\n"
        "def inferred_unknown_hash(value) = {\"one\": value}\n"
        "def inferred_union_hash() = {\"one\": Pet.new(), \"two\": Leaf.new()}\n"
        "def inferred_hash_guards(flag, value)\n"
        "  inferred_pet = inferred_hash_wrapper()[\"one\"]\n"
        "  # inferred_before_guard\n"
        "  inferred_pet.bark()\n"
        "  if inferred_pet != nil\n"
        "    # inferred_non_nil\n"
        "    inferred_pet.bark()\n"
        "  else\n"
        "    # inferred_nil\n"
        "    inferred_pet.bark()\n"
        "  end\n"
        "  # inferred_after_guard\n"
        "  inferred_pet.bark()\n"
        "  if inferred_pet != nil && flag\n"
        "    # inferred_repeat_guard\n"
        "    inferred_pet.bark()\n"
        "  else\n"
        "    # inferred_uncertain_else\n"
        "    inferred_pet.bark()\n"
        "  end\n"
        "  unless inferred_pet == nil\n"
        "    # inferred_repeat_unless\n"
        "    inferred_pet.bark()\n"
        "  end\n"
        "  indirect_pet = indirect_pet_hash()[\"one\"]\n"
        "  unless indirect_pet == nil\n"
        "    indirect_pet.bark()\n"
        "  end\n"
        "  inferred_pair = inferred_union_hash()[\"one\"]\n"
        "  if inferred_pair != nil\n"
        "    inferred_pair.bark()\n"
        "  end\n"
        "  unknown_hash_pet = inferred_unknown_hash(value)[\"one\"]\n"
        "  if unknown_hash_pet != nil\n"
        "    unknown_hash_pet.bark()\n"
        "  end\n"
        "  changing_hash = inferred_pet_hash()\n"
        "  alias_hash = changing_hash\n"
        "  changing_hash[\"one\"] = value\n"
        "  changed_hash_pet = alias_hash[\"one\"]\n"
        "  if changed_hash_pet != nil\n"
        "    changed_hash_pet.bark()\n"
        "  end\n"
        "end\n"
        "def inferred_hash_early()\n"
        "  early_inferred = inferred_pet_hash()[\"one\"]\n"
        "  return if early_inferred == nil\n"
        "  early_inferred.bark()\n"
        "end\n"
        "def inferred_hash_return_contract() -> Pet\n"
        "  result = inferred_pet_hash()[\"one\"]\n"
        "  return Pet.new() if result == nil\n"
        "  result\n"
        "end\n"
        "def inferred_hash_branch_contract() -> Pet\n"
        "  result = inferred_pet_hash()[\"one\"]\n"
        "  if result != nil\n"
        "    result\n"
        "  else\n"
        "    Pet.new()\n"
        "  end\n"
        "end\n"
        "def inferred_nested_hash() = {\"list\": [Pet.new()]}\n"
        "def inferred_nested_hash_guard()\n"
        "  nested_hash_items = inferred_nested_hash()[\"list\"]\n"
        "  if nested_hash_items != nil\n"
        "    nested_hash_items[0].bark()\n"
        "  end\n"
        "  literal_hash = {\"list\": [literal_pet_factory()]}\n"
        "  literal_hash_items = literal_hash[\"list\"]\n"
        "  if literal_hash_items != nil\n"
        "    literal_hash_items[0].bark()\n"
        "  end\n"
        "end\n"
        "def inferred_hash_ternary()\n"
        "  ternary_hash_pet = inferred_pet_hash()[\"one\"]\n"
        "  choice = ternary_hash_pet != nil ? ternary_hash_pet : Pet.new()\n"
        "  ternary_hash_pet.bark()\n"
        "end\n"
        "def inferred_hash_checked_guard_budget(value: String | Nil) -> String\n"
        "  pet0 = inferred_pet_hash()[\"one\"]\n"
        "  pet1 = inferred_pet_hash()[\"one\"]\n"
        "  pet2 = inferred_pet_hash()[\"one\"]\n"
        "  pet3 = inferred_pet_hash()[\"one\"]\n"
        "  pet4 = inferred_pet_hash()[\"one\"]\n"
        "  pet5 = inferred_pet_hash()[\"one\"]\n"
        "  pet6 = inferred_pet_hash()[\"one\"]\n"
        "  pet7 = inferred_pet_hash()[\"one\"]\n"
        "  if pet0 != nil && pet1 != nil && pet2 != nil && pet3 != nil && pet4 != nil && pet5 != nil && pet6 != nil && pet7 != nil && value != nil\n"
        "    value\n"
        "  else\n"
        "    \"fallback\"\n"
        "  end\n"
        "end\n"
        "def greeting() -> String = \"abc\"\n"
        "def string_twin?(text) -> Int = 1\n"
        "def string_twin(text) -> String = \"twin\"\n"
        "def string_lonely?(text) -> Int = 1\n"
        "def predicate_twins()\n"
        "  greeting().twin?().to_s()\n"
        "  greeting().twin().to_s()\n"
        "  greeting().lonely?().to_s()\n"
        "  greeting().lonely().to_s()\n"
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
    failed|=check_receiver_pair(program,&chunk,combined,
        "conflicting_factory(true).pets()[0].","Pet","Leaf");
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
    failed|=check_receiver(program,&chunk,combined,"spread_pets[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"spread_pet.","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,"spread_mixed[0].","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"spread_nested[0][0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"spread_checked[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"spread_unknown[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"spread_dynamic[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,
        "spread_factory().reverse().take(1)[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,
        "TransformOwner.new().reverse()[0].","Leaf",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified::Widget.","Qualified::Widget",true);
    failed|=check_receiver(program,&chunk,combined,"Qualified::Widget.new().","Qualified::Widget",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified::Widget.new().part().","Qualified::Gear",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified::Widget.build().","Qualified::Widget",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified.make().","Qualified::Widget",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified.make().part().","Qualified::Gear",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified::Inner::Deep.new().","Qualified::Inner::Deep",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified.make_inner().","Qualified::Inner::Deep",false);
    failed|=check_receiver(program,&chunk,combined,"Qualified.unknown(1).",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"reversed_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"unique_pets[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"compact_pets[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"taken_pets[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"dropped_pets[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"transformed_nested[0][0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"wrapped_pet.","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,"transformed_union[0].","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"unknown_transform[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"transform_alias[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"changed_alias[0].",nullptr,false);
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
    bool found_spread_local=false;
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(strcmp(function->name,"inspect_spread_results")!=0)continue;
        for(size_t local=0;local<function->scope_local_count;local++) {
            const DiamondScopeLocal *fact=&function->scope_locals[local];
            if(strcmp(fact->name,"spread_pets")!=0)continue;
            found_spread_local=true;
            if(fact->known_type!=DIAMOND_TYPE_ARRAY||
               fact->known_type_set>=0||fact->tooling_type_set<0) {
                fprintf(stderr,"spread element graph entered checked facts\n");
                failed=1;
            }
        }
    }
    if(!found_spread_local)failed=1;
    bool found_transform_local=false;
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(strcmp(function->name,"inspect_transform_results")!=0)continue;
        for(size_t local=0;local<function->scope_local_count;local++) {
            const DiamondScopeLocal *fact=&function->scope_locals[local];
            if(strcmp(fact->name,"taken_pets")!=0)continue;
            found_transform_local=true;
            if(fact->known_type_set>=0||fact->tooling_type_set<0) {
                fprintf(stderr,"transform element graph entered checked facts\n");
                failed=1;
            }
        }
    }
    if(!found_transform_local)failed=1;
    failed|=check_receiver_pair(program,&chunk,combined,
        "conflicting_factory(true).pets().reverse().take(1)[0].","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "conflicting_factory(true).nested()[0][0].","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "conflicting_factory(true).pick(Pet.new(), Leaf.new())[0].","Pet","Leaf");
    failed|=check_receiver_pair(program,&chunk,combined,
        "conflicting_factory(true).pick[Pet, Leaf](Pet.new(), Leaf.new())[0].","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).unsafe(nil)[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).mixed()[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).nullable()[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).only()[0].",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,
        "conflicting_factory(true).different_depth()[0][0].",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# before_guard\n",
        "pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# non_nil_branch\n",
        "pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# nil_branch\n",
        "pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# after_guard\n",
        "pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# conjunctive_branch\n",
        "pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# uncertain_else\n",
        "pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# unless_branch\n",
        "pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# inverted_else\n",
        "pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# uncertain_or\n",
        "pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# reassigned_branch\n",
        "pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# after_early_return\n",
        "early_pet.","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,"union_pet.","Pet","Leaf");
    failed|=check_receiver(program,&chunk,combined,"mixed_pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# restored_nested_else\n",
        "nested_pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# uncertain_nested_join\n",
        "nested_pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_before_guard\n",
        "inferred_pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_non_nil\n",
        "inferred_pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_nil\n",
        "inferred_pet.",nullptr,false);
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_after_guard\n",
        "inferred_pet.",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"indirect_pet.","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"unknown_hash_pet.",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"changed_hash_pet.",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"early_inferred.","Pet",false);
    failed|=check_receiver_pair(program,&chunk,combined,"inferred_pair.","Pet","Leaf");
    failed|=check_return_contract(&chunk,"inferred_hash_checked_guard_budget");
    failed|=check_return_contract(&chunk,"inferred_hash_return_contract");
    failed|=check_return_contract(&chunk,"inferred_hash_branch_contract");
    bool found_inferred_hash_fact=false;
    for(size_t index=0;index<chunk.function_count;index++) {
        const DiamondFunction *function=chunk.functions[index];
        if(strcmp(function->name,"inferred_hash_guards")!=0)continue;
        for(size_t local=0;local<function->scope_local_count;local++) {
            const DiamondScopeLocal *entry=&function->scope_locals[local];
            if(strcmp(entry->name,"inferred_pet")!=0)continue;
            for(size_t fact_index=0;fact_index<function->scope_type_fact_count;fact_index++) {
                const DiamondScopeTypeFact *fact=&function->scope_type_facts[fact_index];
                if(fact->reg!=entry->reg)continue;
                if(fact->tooling_type_set>=0)found_inferred_hash_fact=true;
                if(fact->known_type!=DIAMOND_TYPE_NONE||fact->known_type_set>=0) {
                    fprintf(stderr,"inferred Hash guard entered checked facts\n");
                    failed=1;
                }
            }
        }
    }
    if(!found_inferred_hash_fact)failed=1;
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_repeat_guard\n",
        "inferred_pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_repeat_unless\n",
        "inferred_pet.","Pet",false);
    failed|=check_receiver_after(program,&chunk,combined,"# inferred_uncertain_else\n",
        "inferred_pet.",nullptr,false);
    failed|=check_receiver(program,&chunk,combined,"nested_hash_items[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"literal_hash_items[0].","Pet",false);
    failed|=check_receiver(program,&chunk,combined,"ternary_hash_pet.",nullptr,false);
    DiamondProgram *restored=round_trip(program);
    if(restored==nullptr) {
        fprintf(stderr,"receiver program serialization failed\n");failed=1;
    } else {
        const DiamondChunk restored_chunk=diamond_program_chunk(restored);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "taken_pets[0].","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "transformed_nested[0][0].","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "spread_pets[0].","Pet",false);
        failed|=check_receiver(restored,&restored_chunk,combined,
            "spread_nested[0][0].","Pet",false);
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
        failed|=check_receiver_pair(restored,&restored_chunk,combined,
            "conflicting_factory(true).nested()[0][0].","Pet","Leaf");
        failed|=check_receiver_after(restored,&restored_chunk,combined,"# non_nil_branch\n",
            "pet.","Pet",false);
        failed|=check_receiver_after(restored,&restored_chunk,combined,"# inferred_non_nil\n",
            "inferred_pet.","Pet",false);
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
    failed|=check_builtin(program,&chunk,combined,"greeting().twin?().",true,DIAMOND_TYPE_INT);
    failed|=check_builtin(program,&chunk,combined,"greeting().twin().",true,DIAMOND_TYPE_STRING);
    failed|=check_builtin(program,&chunk,combined,"greeting().lonely?().",true,DIAMOND_TYPE_INT);
    failed|=check_builtin(program,&chunk,combined,"greeting().lonely().",false,0);
    diamond_program_free(program);
    free(program);free(combined);
    return failed?1:0;
}
