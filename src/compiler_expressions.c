#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiler_internal.h"
#include "jit.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Expression parsing: grouping, array and hash literals, indexing, type-set merging, the prefix and
 * precedence parsers, operators and the ternary. */

static uint16_t parse_grouping(Compiler *compiler) {
    const uint16_t result = parse_expression(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after expression");
        return result;
    }
    advance_token(compiler);
    return result;
}

uint16_t parse_with_expected_set(Compiler *compiler,
        uint16_t expected_set) {
    const int32_t outer=compiler->expected_expression_type_set;
    compiler->expected_expression_type_set=expected_set==DIAMOND_NO_TYPE_SET?
        -1:(int32_t)expected_set;
    const uint16_t result=parse_expression(compiler);
    compiler->expected_expression_type_set=outer;
    return result;
}

uint16_t parse_array_literal(Compiler *compiler,
        const DiamondFunction *expected_function,size_t first_parameter,
        size_t parameter_count) {
    uint16_t elements[DIAMOND_MAX_ARGUMENTS];
    size_t count=0;
    uint16_t expected_element=DIAMOND_NO_TYPE_SET;
    if(compiler->expected_expression_type_set>=0&&
       (size_t)compiler->expected_expression_type_set<
           compiler->function->type_set_count) {
        const DiamondTypeSet *expected=&compiler->function->type_sets[
            (size_t)compiler->expected_expression_type_set];
        if(expected->count==1&&expected->members[0].id==DIAMOND_TYPE_ARRAY)
            expected_element=expected->members[0].argument_set;
    }
    skip_newlines(compiler);
    /* `[head, *rest, tail]`: at most one splat, whose Array's elements are
     * spliced in place. spread_at is its position among `elements`. */
    size_t spread_at=SIZE_MAX;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        do {
            if(count==DIAMOND_MAX_ARGUMENTS) {
                fail(compiler,compiler->current.span,"array literal has too many elements");
                return 0;
            }
            if(compiler->current.kind==DIAMOND_TOKEN_STAR&&expected_function==nullptr) {
                if(spread_at!=SIZE_MAX) {
                    fail(compiler,compiler->current.span,
                        "an array literal can splat only one array");
                    return 0;
                }
                advance_token(compiler);
                spread_at=count;
                elements[count++]=parse_expression(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
                advance_token(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET) break;
                continue;
            }
            if(expected_function!=nullptr&&
               first_parameter+count<parameter_count)
                elements[count]=parse_expected_argument(compiler,
                    expected_function,first_parameter+count);
            else elements[count]=parse_with_expected_set(compiler,
                expected_element);
            count++;
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET) break;
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after array literal");
        return 0;
    }
    advance_token(compiler);
    if(spread_at!=SIZE_MAX) {
        /* The same builder a call's `f(a, *rest, z)` uses: fixed prefix,
         * one spread Array (TypeError if it isn't one), fixed suffix. */
        const uint16_t destination=emit_build_spread_arguments(compiler,elements,
            spread_at,elements[spread_at],elements+spread_at+1,
            count-spread_at-1,false);
        publish_array_spread_tooling_type(compiler,destination,elements,spread_at,
            elements[spread_at],elements+spread_at+1,count-spread_at-1);
        return destination;
    }
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) emit_instruction(compiler,DIAMOND_OP_MOVE,
        (uint16_t)(base+i),elements[i],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,destination,base,(uint8_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_ARRAY;
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_ARRAY,
        joined_value_type_set(compiler,elements,count),-1);
    publish_array_literal_tooling_type(compiler,destination,elements,count);
    if(expected_function!=nullptr) {
        compiler->positional_spread_literal=true;
        compiler->positional_spread_first=first_parameter;
        compiler->positional_spread_count=count;
        for(size_t index=0;index<count;index++)
            compiler->positional_spread_elements[index]=elements[index];
    }
    return destination;
}

static uint16_t parse_array(Compiler *compiler) {
    return parse_array_literal(compiler,nullptr,0,0);
}

static uint16_t parse_hash(Compiler *compiler) {
    uint16_t keys[DIAMOND_MAX_ARGUMENTS],values[DIAMOND_MAX_ARGUMENTS];
    size_t count=0;
    uint16_t expected_key=DIAMOND_NO_TYPE_SET;
    uint16_t expected_value=DIAMOND_NO_TYPE_SET;
    if(compiler->expected_expression_type_set>=0&&
       (size_t)compiler->expected_expression_type_set<
           compiler->function->type_set_count) {
        const DiamondTypeSet *expected=&compiler->function->type_sets[
            (size_t)compiler->expected_expression_type_set];
        if(expected->count==1&&expected->members[0].id==DIAMOND_TYPE_HASH) {
            expected_key=expected->members[0].argument_set;
            expected_value=expected->members[0].second_argument_set;
        }
    }
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        do {
            if(count==DIAMOND_MAX_ARGUMENTS) {
                fail(compiler,compiler->current.span,"hash literal has too many entries");
                return 0;
            }
            keys[count]=parse_with_expected_set(compiler,expected_key);
            /* `{"a": 1}` and Ruby's `{"a" => 1}` mean the same thing. */
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON&&
               compiler->current.kind!=DIAMOND_TOKEN_FAT_ARROW) {
                fail(compiler,compiler->current.span,"expected ':' or '=>' after hash key");
                return 0;
            }
            advance_token(compiler);
            values[count]=parse_with_expected_set(compiler,expected_value);
            count++;
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE) break;
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        fail(compiler,compiler->current.span,"expected '}' after hash literal");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count*2;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) {
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2),keys[i],0,2);
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2+1),values[i],0,2);
    }
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_HASH,destination,base,(uint16_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_HASH;
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_HASH,
        joined_value_type_set(compiler,keys,count),
        joined_value_type_set(compiler,values,count));
    publish_hash_literal_tooling_type(compiler,destination,keys,values,count);
    return destination;
}

int32_t type_set_with_nil(Compiler *compiler,uint16_t source_index) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    for(size_t index=0;index<source.count;index++)
        if(source.members[index].id==DIAMOND_TYPE_NIL)return (int32_t)source_index;
    if(source.count==DIAMOND_MAX_UNION_TYPES||
       !grow_type_sets(compiler->function,1))return -1;
    const size_t result=compiler->function->type_set_count++;
    compiler->function->type_sets[result]=source;
    DiamondTypeSet *set=&compiler->function->type_sets[result];
    set->members[set->count++]=(DiamondTypeMember){
        .id=DIAMOND_TYPE_NIL,.argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,.callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET};
    return (int32_t)result;
}

static bool split_nil_type_set(Compiler *compiler,uint16_t source_index,
                               int32_t *without_nil,int32_t *only_nil) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet narrowed={};bool found_nil=false;
    for(size_t index=0;index<source.count;index++) {
        if(source.members[index].id==DIAMOND_TYPE_NIL)found_nil=true;
        else narrowed.members[narrowed.count++]=source.members[index];
    }
    if(!found_nil||narrowed.count==0||
       !grow_type_sets(compiler->function,2))return false;
    *without_nil=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=narrowed;
    *only_nil=(int32_t)compiler->function->type_set_count;
    DiamondTypeSet *nil_set=&compiler->function->type_sets[
        compiler->function->type_set_count++];
    *nil_set=(DiamondTypeSet){.members={{.id=DIAMOND_TYPE_NIL,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
    return true;
}

static bool split_type_set(Compiler *compiler,uint16_t source_index,
                           uint16_t tested_type,int32_t *matching,
                           int32_t *remaining) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet yes={},no={};
    for(size_t index=0;index<source.count;index++) {
        const DiamondTypeMember member=source.members[index];
        if(known_type_satisfies_one(compiler,member.id,tested_type))
            yes.members[yes.count++]=member;
        else no.members[no.count++]=member;
    }
    if(yes.count==0||no.count==0||
       !grow_type_sets(compiler->function,2))return false;
    *matching=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=yes;
    *remaining=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=no;
    return true;
}

void apply_type_set_fact(Compiler *compiler,uint16_t reg,int32_t set_index) {
    compiler->known_type_sets[reg]=set_index;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)set_index];
    compiler->known_types[reg]=set->count==1?set->members[0].id:TYPE_UNKNOWN;
}

void apply_narrowing_facts(Compiler *compiler,const NarrowingFact *facts,
                                  size_t count) {
    for(size_t index=0;index<count;index++)
        if(!facts[index].tooling_only)
            apply_type_set_fact(compiler,facts[index].reg,facts[index].type_set);
}

/* Export guards only at control-flow boundaries whose restoration is also
 * recorded. Other users (notably ternaries) keep their existing checked facts. */
void apply_scope_narrowing_facts(Compiler *compiler,const NarrowingFact *facts,
        size_t count) {
    apply_narrowing_facts(compiler,facts,count);
    for(size_t index=0;index<count;index++) {
        if(facts[index].tooling_only)
            compiler->tooling_type_sets[facts[index].reg]=facts[index].type_set;
        else if(facts[index].has_tooling)
            compiler->tooling_type_sets[facts[index].reg]=facts[index].tooling_type_set;
        if(register_is_local(compiler,facts[index].reg))
            record_scope_type_fact(compiler,facts[index].reg,compiler->current.span.start);
    }
}

static void append_narrowing_facts(NarrowingFact *destination,size_t *destination_count,
                                   const NarrowingFact *source,size_t source_count) {
    for(size_t index=0;index<source_count;index++) {
        if(*destination_count==DIAMOND_MAX_NARROWING_FACTS) {
            if(source[index].tooling_only)continue;
            size_t discard=0;
            while(discard<*destination_count&&!destination[discard].tooling_only)
                discard++;
            if(discard==*destination_count)continue;
            /* Editor proofs may not displace checked proofs or change their order. */
            memmove(destination+discard,destination+discard+1,
                (*destination_count-discard-1)*sizeof *destination);
            (*destination_count)--;
        }
        destination[(*destination_count)++]=source[index];
    }
}

bool type_members_equal(DiamondTypeMember left,DiamondTypeMember right) {
    if(left.id!=right.id||left.argument_set!=right.argument_set||
       left.second_argument_set!=right.second_argument_set||
       left.callable_arity!=right.callable_arity||
       left.callable_return_set!=right.callable_return_set||
       left.callable_parameters_typed!=right.callable_parameters_typed)return false;
    if(!left.callable_parameters_typed)return true;
    for(size_t index=0;index<left.callable_arity;index++)
        if(left.callable_parameter_sets[index]!=right.callable_parameter_sets[index])return false;
    return true;
}

bool type_sets_equal_unordered(DiamondTypeSet left,DiamondTypeSet right) {
    if(left.count!=right.count)return false;
    for(size_t left_index=0;left_index<left.count;left_index++) {
        bool found=false;
        for(size_t right_index=0;right_index<right.count;right_index++)
            if(type_members_equal(left.members[left_index],right.members[right_index])) {
                found=true;break;
            }
        if(!found)return false;
    }
    return true;
}

static DiamondTypeMember plain_type_member(uint16_t type) {
    return (DiamondTypeMember){.id=type,.argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,.callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET};
}

static bool append_merged_member(DiamondTypeSet *merged,DiamondTypeMember member) {
    for(size_t index=0;index<merged->count;index++) {
        if(type_members_equal(merged->members[index],member))return true;
    }
    if(merged->count==DIAMOND_MAX_UNION_TYPES)return false;
    merged->members[merged->count++]=member;return true;
}

static bool append_flow_type(const Compiler *compiler,DiamondTypeSet *merged,
        uint16_t known_type,int32_t known_set) {
    if(known_set>=0) {
        if((size_t)known_set>=compiler->function->type_set_count)return false;
        const DiamondTypeSet set=compiler->function->type_sets[(size_t)known_set];
        for(size_t index=0;index<set.count;index++)
            if(!append_merged_member(merged,set.members[index]))return false;
        return set.count>0;
    }
    return known_type!=TYPE_UNKNOWN&&
        append_merged_member(merged,plain_type_member(known_type));
}

/* Conservatively joins two control-flow type states. Unlike runtime type
 * annotations this metadata is advisory: an unrepresentable or exhausted
 * union simply becomes unknown and must never make compilation fail. */
void merge_flow_types(Compiler *compiler,uint16_t left_type,int32_t left_set,
        uint16_t right_type,int32_t right_set,uint16_t *result_type,int32_t *result_set) {
    *result_type=TYPE_UNKNOWN;*result_set=-1;
    if(left_type==right_type&&left_set==right_set) {
        *result_type=left_type;*result_set=left_set;return;
    }
    DiamondTypeSet merged={};
    if(!append_flow_type(compiler,&merged,left_type,left_set)||
       !append_flow_type(compiler,&merged,right_type,right_set))return;
    merged.inferred=true;
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        if(!compiler->function->type_sets[index].inferred)continue;
        if(!type_sets_equal_unordered(merged,compiler->function->type_sets[index]))continue;
        *result_set=(int32_t)index;
        *result_type=merged.count==1?merged.members[0].id:TYPE_UNKNOWN;
        return;
    }
    if(!grow_type_sets(compiler->function,1))return;
    const size_t index=compiler->function->type_set_count++;
    compiler->function->type_sets[index]=merged;
    *result_set=(int32_t)index;
    *result_type=merged.count==1?merged.members[0].id:TYPE_UNKNOWN;
}

/* Tooling-only receiver facts are sometimes cloned independently while
 * compiling separate control-flow arms. Their table indices then differ even
 * though they describe the same receiver graph, so compare the graphs before
 * conservatively discarding the fact at the join. */
int32_t merge_tooling_type_sets(const Compiler *compiler,int32_t left,
        int32_t right) {
    if(left==right)return left;
    if(left<0||right<0||(size_t)left>=compiler->function->type_set_count||
       (size_t)right>=compiler->function->type_set_count)return -1;
    return type_sets_structurally_equal(compiler->function->type_sets,
        compiler->function->type_set_count,(uint16_t)left,
        compiler->function->type_sets,compiler->function->type_set_count,
        (uint16_t)right,0)?left:-1;
}

/* Conservatively joins two control-flow alias-identity states for the same
 * local. Agreeing branches keep the shared identity; disagreeing branches
 * (including either being unaliased) detach to 0 rather than guessing which
 * object is actually live -- alias_identity_for_value/
 * propagate_collection_alias_fact already treat 0 as "assign a fresh identity
 * lazily on next use", so a detached local still tracks its own future
 * mutations/aliases correctly, just independently of either branch's object. */
uint32_t merge_alias_identity(uint32_t left,uint32_t right) {
    return left==right?left:0;
}

bool register_is_local(const Compiler *compiler,uint16_t reg) {
    for(size_t index=0;index<compiler->local_count;index++)
        if(compiler->locals[index].reg==reg)return true;
    return false;
}

void set_index_provenance(Compiler *compiler,uint16_t destination,
        uint16_t source_receiver) {
    if(!register_is_local(compiler,source_receiver))return;
    compiler->has_index_provenance[destination]=true;
    compiler->index_provenance_root[destination]=source_receiver;
}

/* Joins a local first bound inside *some* branch of a multi-exit-point
 * construct (a `case`'s `when`/`else` clauses, or a loop's exit points --
 * the zero-iteration path, every `break`, and the natural loop-back/
 * condition-false path) -- shared because both need the exact same
 * seed-vs-merge shape, keyed by `new_local_count`/`new_types`/`new_sets`/
 * `new_alias` fields living on the caller's own join/loop struct (passed by
 * pointer/array here since CaseFlowJoin and LoopContext aren't the same
 * type). Slots [0, *new_local_count) have already been reached by an
 * earlier-processed branch/exit-point and merge normally; a slot reached
 * for the first time either seeds directly with this branch's value (this
 * is the very first branch/exit-point overall, `earlier_branch_exists`
 * false, so there's no earlier one to have implicitly skipped it) or seeds
 * as `merge(Nil, this branch's value)` (every other case: some earlier
 * branch/exit-point already ran without this local existing at all, so it
 * implicitly contributed Nil down that path). */
void merge_new_local_facts(Compiler *compiler,bool earlier_branch_exists,
        size_t flow_local_count,size_t *new_local_count,uint16_t *new_types,
        int32_t *new_sets,uint32_t *new_alias) {
    const size_t current_local_count=compiler->local_count;
    for(size_t index=flow_local_count;
        index<current_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const size_t slot=index-flow_local_count;
        const uint16_t reg=compiler->locals[index].reg;
        if(slot>=*new_local_count) {
            if(earlier_branch_exists)
                merge_flow_types(compiler,DIAMOND_TYPE_NIL,-1,
                    compiler->known_types[reg],compiler->known_type_sets[reg],
                    &new_types[slot],&new_sets[slot]);
            else {
                new_types[slot]=compiler->known_types[reg];
                new_sets[slot]=compiler->known_type_sets[reg];
            }
            new_alias[slot]=compiler->locals[index].alias_identity;
        } else {
            merge_flow_types(compiler,new_types[slot],new_sets[slot],
                compiler->known_types[reg],compiler->known_type_sets[reg],
                &new_types[slot],&new_sets[slot]);
            new_alias[slot]=merge_alias_identity(new_alias[slot],
                compiler->locals[index].alias_identity);
        }
    }
    if(current_local_count>flow_local_count) {
        const size_t reached=current_local_count-flow_local_count;
        if(reached>*new_local_count)
            *new_local_count=reached<DIAMOND_MAX_LOCALS?reached:DIAMOND_MAX_LOCALS;
    }
}

void merge_loop_exit(Compiler *compiler,LoopContext *loop,
        uint16_t result_type,int32_t result_set,int32_t result_tooling_set) {
    if(!loop->exit_initialized) {
        for(size_t index=0;index<loop->flow_reg_count;index++) {
            loop->exit_types[index]=compiler->known_types[index];
            loop->exit_sets[index]=compiler->known_type_sets[index];
            loop->exit_tooling_sets[index]=compiler->tooling_type_sets[index];
        }
        for(size_t index=0;index<loop->flow_local_count;index++)
            loop->exit_alias[index]=compiler->locals[index].alias_identity;
        merge_new_local_facts(compiler,false,loop->flow_local_count,
            &loop->new_local_count,loop->new_types,loop->new_sets,loop->new_alias);
        loop->result_type=result_type;loop->result_set=result_set;
        loop->result_tooling_set=result_tooling_set;
        loop->exit_initialized=true;return;
    }
    for(size_t index=0;index<loop->flow_reg_count;index++) {
        merge_flow_types(compiler,loop->exit_types[index],loop->exit_sets[index],
            compiler->known_types[index],compiler->known_type_sets[index],
            &loop->exit_types[index],&loop->exit_sets[index]);
        loop->exit_tooling_sets[index]=merge_tooling_type_sets(compiler,
            loop->exit_tooling_sets[index],compiler->tooling_type_sets[index]);
    }
    for(size_t index=0;index<loop->flow_local_count;index++)
        loop->exit_alias[index]=merge_alias_identity(loop->exit_alias[index],
            compiler->locals[index].alias_identity);
    merge_new_local_facts(compiler,true,loop->flow_local_count,
        &loop->new_local_count,loop->new_types,loop->new_sets,loop->new_alias);
    merge_flow_types(compiler,loop->result_type,loop->result_set,
        result_type,result_set,&loop->result_type,&loop->result_set);
    loop->result_tooling_set=merge_tooling_type_sets(compiler,
        loop->result_tooling_set,result_tooling_set);
}

void finish_loop_flow(Compiler *compiler,LoopContext *loop,
        size_t effective_start) {
    if(!loop->exit_initialized)return;
    for(size_t index=0;index<loop->flow_local_count;index++)
        compiler->locals[index].alias_identity=loop->exit_alias[index];
    for(size_t index=0;index<loop->flow_reg_count;index++) {
        compiler->known_types[index]=loop->exit_types[index];
        compiler->known_type_sets[index]=loop->exit_sets[index];
        compiler->tooling_type_sets[index]=loop->exit_tooling_sets[index];
        if(register_is_local(compiler,(uint16_t)index))
            record_scope_type_fact(compiler,(uint16_t)index,effective_start);
    }
    const size_t final_local_count=compiler->local_count;
    for(size_t index=loop->flow_local_count;
        index<final_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const size_t slot=index-loop->flow_local_count;
        const uint16_t reg=compiler->locals[index].reg;
        compiler->known_types[reg]=loop->new_types[slot];
        compiler->known_type_sets[reg]=loop->new_sets[slot];
        compiler->tooling_type_sets[reg]=-1;
        compiler->locals[index].alias_identity=loop->new_alias[slot];
        record_scope_type_fact(compiler,reg,effective_start);
    }
    compiler->known_types[loop->result_register]=loop->result_type;
    compiler->known_type_sets[loop->result_register]=loop->result_set;
    compiler->tooling_type_sets[loop->result_register]=loop->result_tooling_set;
}

/* Shared by parse_index (`x[i]` as an ordinary expression) and
 * compile_index_assignment's own chain loop (`x[a][b]... = value`'s
 * every-group-but-the-last read) -- both emit an INDEX_GET and need the
 * same "what type is the indexed result" computation on `destination`, or a
 * later mutation through `destination` (nested writeback, or simply reading
 * its own hover) starts from an untyped register instead of the element/
 * value graph it actually just read. */
void publish_indexed_result_type(Compiler *compiler,uint16_t destination,
        uint16_t receiver) {
    const int32_t tooling_element=joined_collection_argument(compiler,
        compiler->tooling_type_sets[receiver],DIAMOND_TYPE_ARRAY,false);
    if(tooling_element>=0)compiler->tooling_type_sets[destination]=tooling_element;
    const int32_t tooling_value=joined_collection_argument(compiler,
        compiler->tooling_type_sets[receiver],DIAMOND_TYPE_HASH,true);
    if(tooling_value>=0)compiler->tooling_type_sets[destination]=
        type_set_with_nil(compiler,(uint16_t)tooling_value);
    int32_t receiver_set=compiler->known_type_sets[receiver];
    const bool tooling_only=receiver_set<0;
    if(tooling_only)receiver_set=compiler->tooling_type_sets[receiver];
    if(receiver_set<0)return;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)receiver_set];
    uint16_t indexed_sets[DIAMOND_MAX_UNION_TYPES];
    uint16_t collection_type=TYPE_UNKNOWN;
    const size_t member_count=set->count;
    bool compatible=member_count>0;
    for(size_t member_index=0;member_index<member_count;member_index++) {
        const DiamondTypeMember member=set->members[member_index];
        if(member.id!=DIAMOND_TYPE_ARRAY&&member.id!=DIAMOND_TYPE_HASH) {
            compatible=false;break;
        }
        if(collection_type==TYPE_UNKNOWN)collection_type=member.id;
        else if(collection_type!=member.id) {compatible=false;break;}
        indexed_sets[member_index]=member.id==DIAMOND_TYPE_ARRAY?
            member.argument_set:member.second_argument_set;
        if(indexed_sets[member_index]==DIAMOND_NO_TYPE_SET) {
            compatible=false;break;
        }
    }
    int32_t indexed=-1;
    for(size_t member_index=0;compatible&&member_index<member_count;
        member_index++) {
        indexed=indexed<0?(int32_t)indexed_sets[member_index]:
            join_type_set_indices(compiler,indexed,
                (int32_t)indexed_sets[member_index]);
        if(indexed<0)compatible=false;
    }
    if(compatible&&collection_type==DIAMOND_TYPE_HASH)
        indexed=type_set_with_nil(compiler,(uint16_t)indexed);
    if(compatible&&indexed>=0) {
        /* An inferred call's collection shape can identify an assigned
         * element for the editor, but cannot change checks or dispatch. */
        if(tooling_only)compiler->tooling_type_sets[destination]=indexed;
        else publish_known_type_set(compiler,destination,(uint16_t)indexed);
    }
}

/* Is this string token a plain literal -- no unescaped `#{` interpolation -- the same scan
 * parse_string uses to decide whether it builds a single STRING or a concatenation? */
static bool string_token_is_plain(const Compiler *compiler,DiamondSpan span) {
    const size_t end=span.start+span.length-1;
    for(size_t index=span.start+1;index+1<end;) {
        if(compiler->source[index]=='\\') {index+=2;continue;}
        if(compiler->source[index]=='#'&&compiler->source[index+1]=='{')return false;
        index++;
    }
    return true;
}

static bool token_is_index_assignment(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_EQUAL||kind==DIAMOND_TOKEN_OR_OR_EQUAL||
        kind==DIAMOND_TOKEN_AND_AND_EQUAL||kind==DIAMOND_TOKEN_PLUS_EQUAL||
        kind==DIAMOND_TOKEN_MINUS_EQUAL||kind==DIAMOND_TOKEN_STAR_EQUAL||
        kind==DIAMOND_TOKEN_SLASH_EQUAL||kind==DIAMOND_TOKEN_PERCENT_EQUAL;
}

uint16_t parse_index(Compiler *compiler,uint16_t receiver) {
    advance_token(compiler);
    /* `receiver["literal"]` read: skip the STRING instruction that would allocate the key on
     * every execution and emit INDEX_GET_STRING, which looks a Hash up by the constant's
     * bytes. Only for a read (an assignment still needs the key in a register, for
     * INDEX_SET) and only for a plain literal followed directly by `]` (not the
     * `value["a", n]` slice form). */
    if(compiler->current.kind==DIAMOND_TOKEN_STRING&&
       string_token_is_plain(compiler,compiler->current.span)) {
        DiamondLexer ahead=compiler->lexer;
        if(diamond_lexer_next(&ahead).kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
           !token_is_index_assignment(diamond_lexer_next(&ahead).kind)) {
            advance_token(compiler);
            const uint16_t key=add_string(compiler,compiler->previous.span);
            advance_token(compiler);
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_INDEX_GET_STRING,destination,receiver,key,3);
            set_index_provenance(compiler,destination,receiver);
            publish_indexed_result_type(compiler,destination,receiver);
            return destination;
        }
    }
    const uint16_t index=parse_expression(compiler);
    /* `value[start, length]` reads a run, as value.slice(start, length)
     * does (Array and String both have slice). */
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        const uint16_t arguments[2]={index,parse_expression(compiler)};
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,"expected ']' after index"); return 0;
        }
        advance_token(compiler);
        return emit_invoke_named(compiler,receiver,"slice",arguments,2);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after index"); return 0;
    }
    advance_token(compiler);
    /* `expr[index] = value` (and `+=` and friends) where expr isn't a
     * plain local/@ivar/@@cvar -- `obj.table()[key] = value`,
     * `lookup()[key] += 1`. Those three simple receivers are handled
     * before expression parsing starts (compile_index_assignment and
     * compile_index_compound_assignment); any other receiver reaches here
     * as an ordinary value, and storing into it is the same INDEX_SET.
     * Only the receiver object is mutated, so there's no local whose
     * collection type facts need widening. */
    const DiamondTokenKind assign=compiler->current.kind;
    if(assign==DIAMOND_TOKEN_EQUAL) {
        advance_token(compiler);
        const uint16_t value=parse_expression(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,value,3);
        return value;
    }
    if(assign==DIAMOND_TOKEN_OR_OR_EQUAL||assign==DIAMOND_TOKEN_AND_AND_EQUAL||
       assign==DIAMOND_TOKEN_PLUS_EQUAL||assign==DIAMOND_TOKEN_MINUS_EQUAL||
       assign==DIAMOND_TOKEN_STAR_EQUAL||assign==DIAMOND_TOKEN_SLASH_EQUAL||
       assign==DIAMOND_TOKEN_PERCENT_EQUAL) {
        advance_token(compiler);
        const uint16_t left=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,left,receiver,index,3);
        const uint16_t destination=allocate_register(compiler);
        if(assign==DIAMOND_TOKEN_OR_OR_EQUAL||assign==DIAMOND_TOKEN_AND_AND_EQUAL) {
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                assign==DIAMOND_TOKEN_AND_AND_EQUAL?DIAMOND_OP_JUMP_IF_FALSE:
                    DIAMOND_OP_JUMP_IF_TRUE,left);
            const uint16_t right=parse_expression(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
            emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,destination,3);
            patch_jump(compiler,end_jump,compiler->function->code_count);
            return destination;
        }
        const DiamondTokenKind plain_op=
            assign==DIAMOND_TOKEN_PLUS_EQUAL?DIAMOND_TOKEN_PLUS:
            assign==DIAMOND_TOKEN_MINUS_EQUAL?DIAMOND_TOKEN_MINUS:
            assign==DIAMOND_TOKEN_STAR_EQUAL?DIAMOND_TOKEN_STAR:
            assign==DIAMOND_TOKEN_SLASH_EQUAL?DIAMOND_TOKEN_SLASH:
            DIAMOND_TOKEN_PERCENT;
        const uint16_t right=parse_expression(compiler);
        const uint16_t result=compile_binary_op(compiler,plain_op,left,right);
        emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,result,3);
        return result;
    }
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_GET,destination,receiver,index,3);
    set_index_provenance(compiler,destination,receiver);
    publish_indexed_result_type(compiler,destination,receiver);
    return destination;
}

/* /pattern/imx -- the same as Regexp.new("pattern", options), compiled each time the literal is
 * evaluated. The pattern is kept as written, so \d reaches the regex engine intact. */
static uint16_t parse_regex_literal(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    size_t close=span.start+span.length;
    int64_t options=0;
    while(compiler->source[close-1]!='/') {
        const char flag=compiler->source[--close];
        options|=flag=='i'?1:flag=='m'?2:4;
    }
    const size_t pattern_start=span.start+1,pattern_length=close-1-pattern_start;
    for(size_t index=0;index+1<pattern_length;index++) {
        if(compiler->source[pattern_start+index]=='\\') {index++;continue;}
        if(compiler->source[pattern_start+index]=='#'&&
           compiler->source[pattern_start+index+1]=='{') {
            fail(compiler,span,"regex literals do not interpolate; build the pattern with Regexp.new");
            return 0;
        }
    }
    const uint16_t pattern_register=allocate_register(compiler);
    const uint16_t pattern=add_string_range_mode(compiler,pattern_start,pattern_length,span,true);
    emit_instruction(compiler,DIAMOND_OP_STRING,pattern_register,pattern,0,2);
    const uint16_t options_register=allocate_register(compiler);
    const uint16_t options_constant=add_constant(compiler,DIAMOND_INT(options));
    emit_instruction(compiler,DIAMOND_OP_CONSTANT,options_register,options_constant,0,2);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_REGEXP_NEW);
    emit_register(compiler,dest);
    emit_register(compiler,pattern_register);
    emit_register(compiler,options_register);
    return dest;
}

uint16_t parse_prefix(Compiler *compiler) {
    advance_token(compiler);
    switch (compiler->previous.kind) {
        case DIAMOND_TOKEN_INTEGER:
            return parse_integer(compiler);
        case DIAMOND_TOKEN_FLOAT:
            return parse_float(compiler);
        case DIAMOND_TOKEN_STRING:
            return parse_string(compiler);
        case DIAMOND_TOKEN_REGEX:
            return parse_regex_literal(compiler);
        case DIAMOND_TOKEN_SYMBOL:
            return parse_symbol(compiler);
        case DIAMOND_TOKEN_TRUE:
        case DIAMOND_TOKEN_FALSE:
        case DIAMOND_TOKEN_NIL:
            return parse_literal(compiler);
        case DIAMOND_TOKEN_IDENTIFIER:
            return parse_name(compiler);
        case DIAMOND_TOKEN_INSTANCE_VARIABLE: {
            const uint16_t destination = allocate_register(compiler);
            if(compiler->current_module>=0&&compiler->current_class<0) {
                const uint16_t field=module_field_name(
                    compiler,compiler->previous.span);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,destination,0,
                                 field,3);
            } else {
                const int field=field_index(compiler,compiler->previous.span,true);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,destination,0,
                                 (uint8_t)field,3);
                /* The discovery pass has already seen every write in every
                 * reopening of this class. Its final field fact is therefore
                 * safe to use here regardless of where this method appears
                 * in source. This feeds the ordinary declared-return path in
                 * publish_instance_return_type, allowing @field.make().use()
                 * to reach Phase 12's existing JIT proof. */
                if(!compiler->discovery_pass&&field>=0&&
                   compiler->current_class>=0) {
                    const DiamondClass *class=compiler->program->classes[
                        (size_t)compiler->current_class];
                    if(class->discovered_field_type_status[(size_t)field]==1)
                        compiler->known_types[destination]=(uint8_t)(
                            DIAMOND_TYPE_CLASS_BASE+
                            class->discovered_field_known_class[(size_t)field]);
                }
            }
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
                return parse_closure_call_arguments(compiler,destination);
            return destination;
        }
        case DIAMOND_TOKEN_CLASS_VARIABLE: {
            const uint16_t destination = allocate_register(compiler);
            const int slot = class_variable_index(compiler,compiler->previous.span,true);
            emit_instruction(compiler,DIAMOND_OP_GET_CVAR,destination,
                             (uint16_t)class_variable_owner(compiler,compiler->previous.span),(uint8_t)slot,3);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
                return parse_closure_call_arguments(compiler,destination);
            return destination;
        }
        case DIAMOND_TOKEN_SELF:
            if (!compiler->in_method) {
                fail(compiler, compiler->previous.span, "'self' used outside a method");
                return 0;
            }
            /* self.foo(...) inside a class-owned singleton method's own
             * body needs real dynamic dispatch (see
             * parse_self_class_method_call's own comment) -- everywhere
             * else (an ordinary instance method, or bare `self` with no
             * following call inside a singleton method), self stays an
             * ordinary register-0 value, unchanged. A `closure name()
             * ... end` body reuses this exact same code unmodified: its
             * own register 0 already holds a materialized copy of the
             * captured self (see compile_definition's captures_self
             * handling), and in_singleton_method is set true there too
             * when appropriate, so this check and the plain `return 0`
             * both already do the right thing with no capture-specific
             * branch needed here at all. */
            if(compiler->in_singleton_method&&compiler->current_class>=0&&
               compiler->current.kind==DIAMOND_TOKEN_DOT) {
                return parse_self_class_method_call(compiler);
            }
            return 0;
        case DIAMOND_TOKEN_SUPER:
            return parse_super(compiler);
        case DIAMOND_TOKEN_LEFT_PAREN:
            return parse_grouping(compiler);
        case DIAMOND_TOKEN_LEFT_BRACKET:
            return parse_array(compiler);
        case DIAMOND_TOKEN_LEFT_BRACE:
            return parse_hash(compiler);
        case DIAMOND_TOKEN_MINUS: {
            const uint16_t operand = parse_precedence(compiler, PREC_PREFIX);
            const uint16_t destination = allocate_register(compiler);
            emit_instruction(compiler, DIAMOND_OP_NEGATE, destination,
                             operand, 0, 2);
            if(compiler->known_types[operand]==DIAMOND_TYPE_INT)
                compiler->known_types[destination]=DIAMOND_TYPE_INT;
            else if(compiler->known_types[operand]==DIAMOND_TYPE_FLOAT)
                compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
            return destination;
        }
        case DIAMOND_TOKEN_BANG:
        case DIAMOND_TOKEN_NOT: {
            const uint16_t operand=parse_precedence(compiler,PREC_PREFIX);
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,operand,0,2);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            return destination;
        }
        case DIAMOND_TOKEN_AMPERSAND:
            /* A block is already represented as an ordinary Callable value.
             * `&block` therefore marks forwarding intent syntactically while
             * preserving the closure register unchanged. */
            return parse_precedence(compiler,PREC_PREFIX);
        case DIAMOND_TOKEN_CASE:
            return parse_case(compiler);
        case DIAMOND_TOKEN_IF:
            return parse_if(compiler,false);
        case DIAMOND_TOKEN_UNLESS:
            return parse_if(compiler,true);
        case DIAMOND_TOKEN_WHILE:
            return parse_while(compiler,false);
        case DIAMOND_TOKEN_UNTIL:
            return parse_while(compiler,true);
        case DIAMOND_TOKEN_LOOP:
            return parse_loop(compiler);
        case DIAMOND_TOKEN_BEGIN:
            return compile_begin(compiler);
        case DIAMOND_TOKEN_YIELD:
            return compile_yield(compiler);
        default:
            fail(compiler, compiler->previous.span, "expected expression");
            return 0;
    }
}

static DiamondOpCode binary_opcode(DiamondTokenKind operator) {
    switch (operator) {
        case DIAMOND_TOKEN_PLUS: return DIAMOND_OP_ADD;
        case DIAMOND_TOKEN_MINUS: return DIAMOND_OP_SUBTRACT;
        case DIAMOND_TOKEN_STAR: return DIAMOND_OP_MULTIPLY;
        case DIAMOND_TOKEN_SLASH: return DIAMOND_OP_DIVIDE;
        case DIAMOND_TOKEN_EQUAL_EQUAL: return DIAMOND_OP_EQUAL;
        case DIAMOND_TOKEN_BANG_EQUAL: return DIAMOND_OP_NOT_EQUAL;
        case DIAMOND_TOKEN_LESS: return DIAMOND_OP_LESS;
        case DIAMOND_TOKEN_LESS_EQUAL: return DIAMOND_OP_LESS_EQUAL;
        case DIAMOND_TOKEN_GREATER: return DIAMOND_OP_GREATER;
        case DIAMOND_TOKEN_GREATER_EQUAL: return DIAMOND_OP_GREATER_EQUAL;
        case DIAMOND_TOKEN_LESS_LESS: return DIAMOND_OP_SHIFT_LEFT;
        case DIAMOND_TOKEN_GREATER_GREATER: return DIAMOND_OP_SHIFT_RIGHT;
        case DIAMOND_TOKEN_AMPERSAND: return DIAMOND_OP_BITWISE_AND;
        case DIAMOND_TOKEN_PIPE: return DIAMOND_OP_BITWISE_OR;
        case DIAMOND_TOKEN_CARET: return DIAMOND_OP_BITWISE_XOR;
        case DIAMOND_TOKEN_PERCENT: return DIAMOND_OP_MODULO;
        case DIAMOND_TOKEN_SPACESHIP: return DIAMOND_OP_COMPARE;
        default: return DIAMOND_OP_ADD;
    }
}

/* The generic (non-short-circuit, non-`is`) binary-operator codegen --
 * factored out of parse_precedence's own infix loop so
 * compile_compound_assignment (below) can reuse it verbatim for `+=`/
 * `-=`/`*=`/`/=`/`%=` instead of re-parsing the desugared `x = x + y`
 * form or duplicating the Int-fast-path/type-narrowing logic. Takes
 * `left`/`right` as already-evaluated registers -- this is deliberately
 * NOT `right = parse_precedence(...)` itself, since a compound
 * assignment's RHS is parsed as a full expression (`parse_expression`),
 * not at `operator_precedence + 1` the way a chained infix operand is. */
uint16_t compile_binary_op(Compiler *compiler, DiamondTokenKind operator,
                                   uint16_t left, uint16_t right) {
    const uint16_t destination = allocate_register(compiler);
    DiamondOpCode opcode=binary_opcode(operator);
    if(operator==DIAMOND_TOKEN_PLUS&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_ADD_INT;
    if(operator==DIAMOND_TOKEN_MINUS&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_SUBTRACT_INT;
    if(operator==DIAMOND_TOKEN_STAR&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_MULTIPLY_INT;
    if(operator==DIAMOND_TOKEN_SLASH&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_DIVIDE_INT;
    if(operator==DIAMOND_TOKEN_EQUAL_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_EQUAL_INT;
    if(operator==DIAMOND_TOKEN_BANG_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_NOT_EQUAL_INT;
    if(operator==DIAMOND_TOKEN_LESS&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_LESS_INT;
    if(operator==DIAMOND_TOKEN_LESS_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_LESS_EQUAL_INT;
    if(operator==DIAMOND_TOKEN_GREATER&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_GREATER_INT;
    if(operator==DIAMOND_TOKEN_GREATER_EQUAL&&
       compiler->known_types[left]==DIAMOND_TYPE_INT&&
       compiler->known_types[right]==DIAMOND_TYPE_INT)
        opcode=DIAMOND_OP_GREATER_EQUAL_INT;
    emit_instruction(compiler, opcode, destination,
                     left, right, 3);
    if(operator==DIAMOND_TOKEN_EQUAL_EQUAL || operator==DIAMOND_TOKEN_BANG_EQUAL ||
       operator==DIAMOND_TOKEN_LESS || operator==DIAMOND_TOKEN_LESS_EQUAL ||
       operator==DIAMOND_TOKEN_GREATER || operator==DIAMOND_TOKEN_GREATER_EQUAL) {
        compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
        if(operator==DIAMOND_TOKEN_EQUAL_EQUAL||operator==DIAMOND_TOKEN_BANG_EQUAL) {
            uint16_t narrowed=left,nil_value=right;
            if(compiler->known_types[left]==DIAMOND_TYPE_NIL) {
                narrowed=right;nil_value=left;
            }
            if(compiler->known_types[nil_value]==DIAMOND_TYPE_NIL) {
                Narrowing narrowing={.condition=destination};
                NarrowingFact yes={.reg=narrowed},no={.reg=narrowed};
                bool have_fact=false;
                for(size_t source=0;source<2;source++) {
                    const int32_t set=source==0?compiler->known_type_sets[narrowed]:
                        compiler->tooling_type_sets[narrowed];
                    if(set<0)continue;
                    int32_t non_nil=-1,nil_only=-1;
                    if(!split_nil_type_set(compiler,(uint16_t)set,
                            &non_nil,&nil_only))continue;
                    const bool unequal=operator==DIAMOND_TOKEN_BANG_EQUAL;
                    if(source==0||!have_fact) {
                        yes.type_set=unequal?non_nil:nil_only;
                        no.type_set=unequal?nil_only:non_nil;
                        yes.tooling_only=no.tooling_only=source!=0;
                    } else {
                        yes.has_tooling=no.has_tooling=true;
                        yes.tooling_type_set=unequal?non_nil:nil_only;
                        no.tooling_type_set=unequal?nil_only:non_nil;
                    }
                    have_fact=true;
                }
                if(have_fact) {
                    narrowing.when_true[0]=yes;narrowing.when_true_count=1;
                    narrowing.when_false[0]=no;narrowing.when_false_count=1;
                }
                narrowing.valid=have_fact;
                if(narrowing.valid)compiler->narrowing=narrowing;
            }
        }
    } else if(compiler->known_types[left]==DIAMOND_TYPE_INT &&
              compiler->known_types[right]==DIAMOND_TYPE_INT) {
        compiler->known_types[destination]=DIAMOND_TYPE_INT;
    } else if(operator!=DIAMOND_TOKEN_SPACESHIP &&
              (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[left]==DIAMOND_TYPE_INT) &&
              (compiler->known_types[right]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[right]==DIAMOND_TYPE_INT) &&
              (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[right]==DIAMOND_TYPE_FLOAT)) {
        /* Mixed Int/Float statically known to auto-promote to Float --
         * excludes spaceship explicitly: unlike the arithmetic operators,
         * its own result is always Int-or-Nil, never Float, regardless of
         * operand types (the DIAMOND_TYPE_INT branch just above stays
         * correct for spaceship unmodified, since two known Ints really
         * do always produce an Int result under this design). Found and
         * fixed during this feature's own design, before any code was
         * written -- see the Comparable design doc. */
        compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
    } else if(operator==DIAMOND_TOKEN_PLUS &&
              compiler->known_types[left]==DIAMOND_TYPE_STRING &&
              compiler->known_types[right]==DIAMOND_TYPE_STRING) {
        compiler->known_types[destination]=DIAMOND_TYPE_STRING;
    }
    return destination;
}

/* A call whose do-block can `break` returns the break value instead of its
 * usual result, so none of the call's type facts hold. */
static void drop_facts_after_block_break(Compiler *compiler,uint16_t reg) {
    if(!compiler->block_break_pending)return;
    compiler->block_break_pending=false;
    compiler->known_types[reg]=TYPE_UNKNOWN;
    compiler->known_type_sets[reg]=-1;
}

/* After `..` or `...`: is this an endless range (`a[1..]`, `(1..)`, `when 5..`) with no end
 * expression? That is so when the next token closes or separates rather than starts one. */
static bool range_end_absent(const Compiler *compiler) {
    switch(compiler->current.kind) {
        case DIAMOND_TOKEN_RIGHT_BRACKET:
        case DIAMOND_TOKEN_RIGHT_PAREN:
        case DIAMOND_TOKEN_RIGHT_BRACE:
        case DIAMOND_TOKEN_COMMA:
        case DIAMOND_TOKEN_NEWLINE:
        case DIAMOND_TOKEN_EOF:
        case DIAMOND_TOKEN_THEN:
        case DIAMOND_TOKEN_END:
        case DIAMOND_TOKEN_DO:
            return true;
        default:
            return false;
    }
}

uint16_t parse_precedence(Compiler *compiler, Precedence precedence) {
    uint16_t left = parse_prefix(compiler);
    drop_facts_after_block_break(compiler,left);
    while (!compiler->failed &&
           (compiler->current.kind == DIAMOND_TOKEN_DOT ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACKET ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN)) {
        /* `expr(...)` directly after an already-fully-parsed expression
         * (no `.method`/`[index]` in between) means "call the Callable
         * value `left` itself" -- `type.coerce_input()(value)`,
         * `(a)(b)`, `arr[0]()`, and so on. A previously-undiscovered
         * gap, not a deliberate cut: nothing in this grammar has any
         * other meaning for two adjacent expressions with no operator
         * between them, so this can only ever turn a program that used
         * to be a hard parse error ("expected newline after
         * expression", since the leftover `(...)` had nowhere to go)
         * into a working one -- it can't reinterpret anything that
         * used to compile. parse_closure_call_arguments already exists
         * for exactly this "call whatever's in this register" shape
         * (shared with a local variable or `@ivar`/`@@cvar` holding a
         * Callable, both followed by `(...)`); reusing it here just
         * extends where its receiver register is allowed to come from. */
        if(compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN) {
            left = parse_closure_call_arguments(compiler,left);
        } else {
            left = compiler->current.kind == DIAMOND_TOKEN_DOT
                ? parse_invoke(compiler,left) : parse_index(compiler,left);
        }
        drop_facts_after_block_break(compiler,left);
    }
    while (!compiler->failed && infix_precedence(compiler) >= precedence) {
        /* `base ** exponent`: a call of the `**` method, so Int/Float get
         * it from the prelude (numeric_pow) and a class can define its
         * own. The right side parses at the same precedence, making it
         * right-associative. */
        if(power_operator_ahead(compiler)) {
            const DiamondSpan operator_span={.start=compiler->current.span.start,
                .length=2,.line=compiler->current.span.line,
                .column=compiler->current.span.column};
            advance_token(compiler);advance_token(compiler);
            skip_newlines(compiler);
            const uint16_t exponent=parse_precedence(compiler,PREC_POWER);
            left=emit_invoke_call(compiler,left,operator_span,false,nullptr,0,
                                  &exponent,1);
            continue;
        }
        const DiamondTokenKind operator = compiler->current.kind;
        const Precedence operator_precedence = token_precedence(operator);
        advance_token(compiler);
        /* A newline right after a binary operator can only mean "the
         * right operand continues on the next line" -- the while
         * condition above already confirmed `operator` is a genuine
         * infix operator at or above the caller's minimum precedence,
         * and no such operator can legally end a statement, so there's
         * no ambiguity to preserve by leaving this newline for the
         * statement-separator logic elsewhere to see. Fixes `x = 1 +\n
         * 2` and `if a &&\n b` (previously "expected expression"):
         * newline-skipping had only ever existed at bracket-delimited
         * list boundaries, never inside a general binary expression. */
        skip_newlines(compiler);
        if(operator==DIAMOND_TOKEN_IS) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,"expected type after 'is'");
                return left;
            }
            const DiamondSpan tested_type_span=compiler->current.span;
            char tested_type_name[DIAMOND_MAX_FUNCTION_NAME];
            if(!consume_qualified_name(compiler,tested_type_name,
                                       sizeof tested_type_name)) {
                fail(compiler,tested_type_span,"type name is too long");
                return left;
            }
            const int tested=resolve_type_name(
                compiler,tested_type_name,tested_type_span);
            const bool tested_native=diamond_type_operand_is_native(tested);
            const uint16_t tested_type=tested_native?DIAMOND_TYPE_NATIVE:(uint16_t)tested;
            if(tested_type>=DIAMOND_TYPE_VARIABLE_BASE&&
               tested_type<DIAMOND_TYPE_INTERFACE_BASE)
                fail(compiler,tested_type_span,
                     "generic type variables cannot be used with 'is' before binding");
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,destination,left,
                             (uint16_t)tested,3);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            /* Narrowing splits by type id alone, which can't tell native
             * kinds apart; a native test doesn't narrow. */
            if(!tested_native&&compiler->known_type_sets[left]>=0) {
                int32_t matching=-1,remaining=-1;
                if(split_type_set(compiler,
                   (uint16_t)compiler->known_type_sets[left],tested_type,
                   &matching,&remaining))
                    compiler->narrowing=(Narrowing){.valid=true,
                        .condition=destination,
                        .when_true={{.reg=left,.type_set=matching}},
                        .when_true_count=1,
                        .when_false={{.reg=left,.type_set=remaining}},
                        .when_false_count=1};
            }
            left=destination;continue;
        }
        if(operator==DIAMOND_TOKEN_AND_AND||operator==DIAMOND_TOKEN_AND||
           operator==DIAMOND_TOKEN_OR_OR||operator==DIAMOND_TOKEN_OR) {
            const bool is_and=operator==DIAMOND_TOKEN_AND_AND||
                operator==DIAMOND_TOKEN_AND;
            const Narrowing left_narrowing=compiler->narrowing.condition==left
                ?compiler->narrowing:(Narrowing){};
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
            const uint16_t right=parse_precedence(
                compiler,(Precedence)(operator_precedence+1));
            const Narrowing right_narrowing=compiler->narrowing.condition==right
                ?compiler->narrowing:(Narrowing){};
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
            patch_jump(compiler,end_jump,compiler->function->code_count);
            if(compiler->known_types[left]==compiler->known_types[right])
                compiler->known_types[destination]=compiler->known_types[left];
            /* AND: both sides must hold for the whole expression to be
             * true, so the when-true facts compose (conjunction is
             * sound). The false case of AND is a disjunction of
             * failures, not soundly reducible to independent
             * per-variable facts, so when_false stays empty rather
             * than guessed. OR is the mirror image: when_false
             * composes (both sides must have failed), when_true stays
             * empty. An empty side is always safe to concatenate --
             * it contributes nothing -- so this composes correctly
             * through chains and mixed &&/|| automatically, with no
             * special-casing for either. */
            Narrowing merged={.condition=destination};
            if(is_and) {
                append_narrowing_facts(merged.when_true,&merged.when_true_count,
                    left_narrowing.when_true,left_narrowing.when_true_count);
                append_narrowing_facts(merged.when_true,&merged.when_true_count,
                    right_narrowing.when_true,right_narrowing.when_true_count);
            } else {
                append_narrowing_facts(merged.when_false,&merged.when_false_count,
                    left_narrowing.when_false,left_narrowing.when_false_count);
                append_narrowing_facts(merged.when_false,&merged.when_false_count,
                    right_narrowing.when_false,right_narrowing.when_false_count);
            }
            merged.valid=merged.when_true_count>0||merged.when_false_count>0;
            compiler->narrowing=merged;
            left=destination;
            continue;
        }
        if(operator==DIAMOND_TOKEN_DOT_DOT||operator==DIAMOND_TOKEN_DOT_DOT_DOT) {
            const bool exclusive=operator==DIAMOND_TOKEN_DOT_DOT_DOT;
            /* `1..` with nothing after it is an endless range: the end is nil. */
            uint16_t right=0;
            if(range_end_absent(compiler)) {
                right=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_NIL,right,0,0,1);
            } else {
                right=parse_precedence(compiler,(Precedence)(operator_precedence+1));
            }
            const int class_index=find_class_name(compiler,"Range");
            if(class_index<0) {
                fail(compiler,compiler->previous.span,
                     "'Range' is not defined -- is the prelude loaded?");
                return left;
            }
            const uint16_t exclusive_reg=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_BOOL,exclusive_reg,exclusive,0,2);
            const uint16_t base=allocate_register(compiler);
            (void)allocate_register(compiler);
            (void)allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,base,left,0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+1),right,0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+2),
                             exclusive_reg,0,2);
            const uint16_t dest=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_NEW); emit_register(compiler,dest);
            emit_register(compiler,(uint16_t)class_index); emit_register(compiler,base);
            emit_byte(compiler,3);
            compiler->known_types[dest]=(uint16_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
            left=dest;continue;
        }
        const uint16_t right = parse_precedence(
            compiler, (Precedence)(operator_precedence + 1));
        left = compile_binary_op(compiler, operator, left, right);
    }
    return left;
}

/* `cond ? a : b` -- binds looser than every binary operator (including
 * `..`/`&&`/`||`), tighter than assignment, matching Ruby's own
 * precedence table. Implemented as the wrapper around parse_expression's
 * own top-level entry point (parse_precedence(PREC_RANGE) used to be
 * that entry point directly; it's now just how a ternary's condition
 * gets parsed) rather than folded into parse_precedence's infix loop --
 * unlike every operator that loop handles, the "operator" here isn't a
 * single token, and the right-hand side needs its own nested-ternary-
 * aware entry point (a true/false branch can itself contain a `?:`,
 * right-associating the same way `a ? b : c ? d : e` does in Ruby),
 * which is exactly what recursing into parse_expression itself gives
 * for free, the same way parse_if's own elsif chain recurses into
 * itself. No new DiamondOpCode: same JUMP_IF_FALSE/MOVE/JUMP shape
 * parse_if's then/else already uses, just without a full compile_
 * sequence body on either side (a ternary's branches are expressions,
 * not statement lists, so -- unlike parse_if/parse_case_branches --
 * there's no local-variable-reassignment-inside-a-branch hazard here
 * that would need a register-snapshot save/restore: an expression alone
 * can't reassign a local, only compile_assignment/compile_compound_
 * assignment can, and neither is reachable from here). */
static uint16_t parse_ternary(Compiler *compiler) {
    const uint16_t condition = parse_precedence(compiler, PREC_RANGE);
    /* Only touch compiler->narrowing once this has actually turned out to
     * be a ternary -- parse_expression (this function) is also how every
     * non-ternary condition gets parsed (parse_if's own condition, for
     * one), and those callers read compiler->narrowing themselves right
     * after calling parse_expression to pick up whatever the condition's
     * own comparison set. Resetting it here unconditionally, even on the
     * plain "no '?' follows, just return condition" path, would silently
     * erase that for every single caller -- confirmed as a real
     * regression (parse_if's own then/else type-narrowing broke,
     * de-optimizing an elidable CHECK_TYPE back on) before catching it
     * in tests/run.sh's existing diagnostic-only-on-failure assertion. */
    if(compiler->current.kind!=DIAMOND_TOKEN_QUESTION) return condition;
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    advance_token(compiler);
    skip_newlines(compiler);
    const size_t false_jump=
        emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,condition);
    const uint16_t destination=allocate_register(compiler);
    if(narrowing.valid)
        apply_narrowing_facts(compiler,narrowing.when_true,narrowing.when_true_count);
    const uint16_t true_result=parse_expression(compiler);
    const uint16_t true_type=compiler->known_types[true_result];
    const int32_t true_set=compiler->known_type_sets[true_result];
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,true_result,0,2);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,false_jump,compiler->function->code_count);
    if(narrowing.valid)
        apply_narrowing_facts(compiler,narrowing.when_false,narrowing.when_false_count);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
        fail(compiler,compiler->current.span,"expected ':' in ternary expression");
        return destination;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t false_result=parse_expression(compiler);
    const uint16_t false_type=compiler->known_types[false_result];
    const int32_t false_set=compiler->known_type_sets[false_result];
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,false_result,0,2);
    patch_jump(compiler,end_jump,compiler->function->code_count);
    merge_flow_types(compiler,true_type,true_set,false_type,false_set,
        &compiler->known_types[destination],
        &compiler->known_type_sets[destination]);
    return destination;
}

uint16_t parse_expression(Compiler *compiler) {
    return parse_ternary(compiler);
}
