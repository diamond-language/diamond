/* See lsp/completion.c's own identical comment. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "receiver.h"

#include "lexer.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* True iff `function` is one of `class`'s own singleton methods (as
 * opposed to one of its ordinary instance methods, or a function
 * belonging to some other class entirely). Matched by pointer identity
 * against chunk->functions[...] rather than by name, since a class can
 * legally have an instance method and a singleton method sharing a
 * name -- pointer identity is exact regardless. */
static bool function_is_singleton_of(const DiamondChunk *chunk,
        const DiamondClass *class,const DiamondFunction *function) {
    for(size_t index=0;index<class->singleton_method_count;index++)
        if(chunk->functions[class->singleton_methods[index].function_index]==function)
            return true;
    return false;
}

/* Which function's body lexically contains byte offset `offset`, using
 * each candidate's own [declaration_start, body_end) extent (src/vm.h) --
 * the candidate with the *smallest* body_end that still contains
 * `offset` is the innermost enclosing one, since a nested function's own
 * closing offset always falls before its enclosing function's. Unlike a
 * scope_locals-range heuristic, this works even for a method with zero
 * parameters and zero locals (a very common shape for exactly the
 * `self.foo()` one-liners this exists to resolve), since body_end is
 * populated unconditionally at every function-closing site
 * (src/compiler.c), not derived from whether any locals happened to be
 * declared. */
static void consider_enclosing_candidate(const DiamondFunction *candidate,size_t offset,
        const DiamondFunction **best,size_t *best_end) {
    if(offset<candidate->declaration_start||offset>=candidate->body_end)return;
    if(candidate->body_end<*best_end) {*best_end=candidate->body_end;*best=candidate;}
}

static const DiamondFunction *find_enclosing_function(
        const DiamondProgram *program,const DiamondChunk *chunk,size_t offset) {
    const DiamondFunction *best=nullptr;
    size_t best_end=SIZE_MAX;
    consider_enclosing_candidate(&program->entry,offset,&best,&best_end);
    for(size_t index=0;index<chunk->function_count;index++)
        consider_enclosing_candidate(chunk->functions[index],offset,&best,&best_end);
    return best;
}

/* Same heuristic as find_enclosing_function, but keyed by a local
 * variable's own name plus containment of `offset` in its valid range,
 * rather than by which function's body contains it -- used to resolve a
 * local-variable receiver (`a.save()`) to its own DiamondScopeLocal so
 * its known_type field can be decoded. Prefers the innermost (smallest
 * valid_end) match for the same reason: correctly picks an inner
 * shadowing declaration over an outer one of the same name. */
static void consider_local_candidate(const DiamondFunction *candidate,
        const char *name,size_t name_length,size_t offset,
        const DiamondScopeLocal **best,size_t *best_end,const DiamondFunction **owner) {
    for(size_t index=0;index<candidate->scope_local_count;index++) {
        const DiamondScopeLocal *local=&candidate->scope_locals[index];
        if(offset<local->valid_start||offset>=local->valid_end)continue;
        if(strlen(local->name)!=name_length||memcmp(local->name,name,name_length)!=0)continue;
        if(local->valid_end<*best_end) {*best_end=local->valid_end;*best=local;*owner=candidate;}
    }
}

/* `*owner` receives the DiamondFunction the match's scope_locals entry
 * belongs to -- a local's own known_type_set (if it has one) only means
 * anything against *that* function's own type_sets[] table, never
 * chunk-wide, so a union-receiver caller needs it to decode the set. */
static const DiamondScopeLocal *find_scope_local(
        const DiamondProgram *program,const DiamondChunk *chunk,
        const char *name,size_t name_length,size_t offset,
        const DiamondFunction **owner) {
    const DiamondScopeLocal *best=nullptr;
    size_t best_end=SIZE_MAX;
    *owner=nullptr;
    consider_local_candidate(&program->entry,name,name_length,offset,&best,&best_end,owner);
    for(size_t index=0;index<chunk->function_count;index++)
        consider_local_candidate(chunk->functions[index],name,name_length,offset,&best,&best_end,owner);
    return best;
}

/* True iff `known_type` (a compiler known_types[reg]-shaped byte) names
 * a specific, in-range class -- shared by the single-class known_type
 * path and by each member of a known_type_set's own class-kind check. */
static bool decode_class_type(const DiamondChunk *chunk,uint8_t known_type,size_t *class_index) {
    if(known_type<DIAMOND_TYPE_CLASS_BASE||known_type>=DIAMOND_TYPE_VARIABLE_BASE)return false;
    const size_t index=(size_t)(known_type-DIAMOND_TYPE_CLASS_BASE);
    if(index>=chunk->class_count)return false;
    *class_index=index;
    return true;
}

static void local_type_at_offset(const DiamondFunction *owner,
        const DiamondScopeLocal *local,size_t offset,uint8_t *known_type,
        int32_t *known_type_set,int32_t *tooling_type_set) {
    *known_type=local->known_type;
    *known_type_set=local->known_type_set;
    *tooling_type_set=local->tooling_type_set;
    size_t latest=0;bool found=false;
    for(size_t index=0;index<owner->scope_type_fact_count;index++) {
        const DiamondScopeTypeFact *fact=&owner->scope_type_facts[index];
        if(fact->reg!=local->reg||fact->effective_start>offset)continue;
        if(!found||fact->effective_start>=latest) {
            found=true;latest=fact->effective_start;
            *known_type=fact->known_type;
            *known_type_set=fact->known_type_set;
            *tooling_type_set=fact->tooling_type_set;
        }
    }
}

bool receiver_name_is_local(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *name,size_t name_length,size_t offset) {
    const DiamondFunction *owner=nullptr;
    return find_scope_local(program,chunk,name,name_length,offset,&owner)!=nullptr;
}

bool receiver_resolve_local_type_set(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *name,size_t name_length,
        size_t offset,const DiamondFunction **owner,uint16_t *set_index) {
    const DiamondScopeLocal *local=find_scope_local(program,chunk,name,
        name_length,offset,owner);
    if(local==nullptr||*owner==nullptr)return false;
    uint8_t known_type;int32_t known_type_set,tooling_type_set;
    local_type_at_offset(*owner,local,offset,&known_type,&known_type_set,
        &tooling_type_set);
    (void)known_type;
    if(known_type_set<0)known_type_set=tooling_type_set;
    if(known_type_set<0||(size_t)known_type_set>=(*owner)->type_set_count)
        return false;
    const DiamondTypeSet *set=&(*owner)->type_sets[(size_t)known_type_set];
    if(set->count==0)return false;
    *set_index=(uint16_t)known_type_set;
    return true;
}

const DiamondMethod *receiver_lookup_method(const DiamondChunk *chunk,
        size_t class_index,bool is_singleton,const char *name,size_t name_length);

static size_t append_class(size_t *classes,size_t count,size_t capacity,size_t value) {
    for(size_t index=0;index<count;index++)if(classes[index]==value)return count;
    if(count<capacity)classes[count++]=value;
    return count;
}

typedef struct ReceiverTypeBinding {
    size_t classes[DIAMOND_MAX_UNION_TYPES];
    size_t class_count;
    uint8_t array_depth;
} ReceiverTypeBinding;

static bool binding_matches_classes(const ReceiverTypeBinding *binding,
        const size_t *classes,size_t count) {
    if(binding->class_count!=count||binding->array_depth!=0)return false;
    for(size_t index=0;index<count;index++) {
        bool found=false;
        for(size_t known=0;known<binding->class_count;known++)
            if(binding->classes[known]==classes[index]) {found=true;break;}
        if(!found)return false;
    }
    return true;
}

static bool bind_classes(ReceiverTypeBinding *binding,const size_t *classes,
        size_t count) {
    if(count==0||count>DIAMOND_MAX_UNION_TYPES)return false;
    if(binding->class_count!=0)return binding_matches_classes(binding,classes,count);
    for(size_t index=0;index<count;index++)
        binding->class_count=append_class(binding->classes,binding->class_count,
            DIAMOND_MAX_UNION_TYPES,classes[index]);
    binding->array_depth=0;
    return binding->class_count==count;
}

static size_t function_return_classes_bound(const DiamondChunk *chunk,
        const DiamondFunction *function,const ReceiverTypeBinding *bindings,
        size_t binding_count,size_t *classes,size_t capacity) {
    /* return_type_set (an explicit `-> Type` annotation) wins when
     * present; inferred_return_type_set (compile_definition's own
     * best-effort inference from an unannotated body's last expression)
     * is only ever consulted as a fallback, so an explicit annotation
     * always takes precedence over what the compiler guessed. */
    uint16_t return_set=function->return_type_set;
    if(return_set==DIAMOND_NO_TYPE_SET)return_set=function->inferred_return_type_set;
    if(return_set==DIAMOND_NO_TYPE_SET||return_set>=function->type_set_count)return 0;
    const DiamondTypeSet *set=&function->type_sets[return_set];
    size_t count=0;
    for(size_t index=0;index<set->count;index++) {
        size_t class_index;
        const uint8_t type=set->members[index].id;
        if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable>=binding_count||bindings==nullptr||
               bindings[variable].array_depth!=0)return 0;
            if(bindings[variable].class_count==0)return 0;
            for(size_t member=0;member<bindings[variable].class_count;member++)
                count=append_class(classes,count,capacity,
                    bindings[variable].classes[member]);
            continue;
        } else if(!decode_class_type(chunk,type,&class_index))return 0;
        count=append_class(classes,count,capacity,class_index);
    }
    return count;
}

/* Parse the deliberately narrow, provable generic-call forms `Class`,
 * `Class | Other`, and nested class-element `Array[...]` shapes. */
static bool parse_explicit_type_binding_union(const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t *index,size_t end,
        ReceiverTypeBinding *binding);

static bool parse_explicit_type_binding(const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t *index,size_t end,
        ReceiverTypeBinding *binding) {
    if(*index>end||tokens[*index].kind!=DIAMOND_TOKEN_IDENTIFIER)return false;
    const char *name=source+tokens[*index].span.start;
    const size_t length=tokens[*index].span.length;
    if(length==5&&memcmp(name,"Array",5)==0) {
        (*index)++;
        if(*index>end||tokens[*index].kind!=DIAMOND_TOKEN_LEFT_BRACKET)return false;
        (*index)++;
        if(!parse_explicit_type_binding_union(chunk,source,tokens,index,end,binding)||
           *index>end||tokens[*index].kind!=DIAMOND_TOKEN_RIGHT_BRACKET||
           binding->array_depth==UINT8_MAX)return false;
        binding->array_depth++;(*index)++;return true;
    }
    for(size_t candidate=0;candidate<chunk->class_count;candidate++)
        if(strlen(chunk->classes[candidate].name)==length&&
           memcmp(chunk->classes[candidate].name,name,length)==0) {
            binding->classes[0]=candidate;binding->class_count=1;
            binding->array_depth=0;
            (*index)++;return true;
        }
    return false;
}

static bool parse_explicit_type_binding_union(const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t *index,size_t end,
        ReceiverTypeBinding *binding) {
    if(!parse_explicit_type_binding(chunk,source,tokens,index,end,binding))
        return false;
    while(*index<=end&&tokens[*index].kind==DIAMOND_TOKEN_PIPE) {
        ReceiverTypeBinding member={};(*index)++;
        if(!parse_explicit_type_binding(chunk,source,tokens,index,end,&member)||
           member.array_depth!=binding->array_depth)return false;
        for(size_t candidate=0;candidate<member.class_count;candidate++)
            binding->class_count=append_class(binding->classes,
                binding->class_count,DIAMOND_MAX_UNION_TYPES,
                member.classes[candidate]);
    }
    return binding->class_count>0;
}

static size_t explicit_type_bindings(const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        ReceiverTypeBinding *bindings,size_t capacity) {
    size_t count=0,index=start;
    while(index<=end) {
        if(count==capacity||!parse_explicit_type_binding_union(chunk,source,tokens,
                &index,end,&bindings[count]))return 0;
        count++;
        if(index>end)break;
        if(tokens[index].kind!=DIAMOND_TOKEN_COMMA)return 0;
        index++;
    }
    return count;
}

typedef struct ReceiverCallSyntax {
    size_t left;
    size_t callee_index;
    ReceiverTypeBinding bindings[8];
    size_t binding_count;
    bool has_explicit_bindings;
} ReceiverCallSyntax;

/* Shared source-shape parser for both an ordinary receiver call and a call
 * used as the base of indexing. `close` is the call's final `)` token. */
static bool parse_receiver_call_syntax(const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t close,
        ReceiverCallSyntax *call) {
    *call=(ReceiverCallSyntax){};
    size_t nesting=0;call->left=close;
    for(size_t index=close+1;index>start;index--) {
        const DiamondTokenKind kind=tokens[index-1].kind;
        if(kind==DIAMOND_TOKEN_RIGHT_PAREN)nesting++;
        else if(kind==DIAMOND_TOKEN_LEFT_PAREN&&--nesting==0) {
            call->left=index-1;break;
        }
    }
    if(call->left==start)return false;
    call->callee_index=call->left-1;
    if(tokens[call->callee_index].kind==DIAMOND_TOKEN_RIGHT_BRACKET) {
        size_t bracket_nesting=0,open=call->callee_index;
        for(size_t index=call->callee_index+1;index>start;index--) {
            const DiamondTokenKind kind=tokens[index-1].kind;
            if(kind==DIAMOND_TOKEN_RIGHT_BRACKET)bracket_nesting++;
            else if(kind==DIAMOND_TOKEN_LEFT_BRACKET&&--bracket_nesting==0) {
                open=index-1;break;
            }
        }
        if(open==start||tokens[open-1].kind!=DIAMOND_TOKEN_IDENTIFIER)
            return false;
        call->binding_count=explicit_type_bindings(chunk,source,tokens,open+1,
            call->callee_index-1,call->bindings,8);
        if(call->binding_count==0)return false;
        call->has_explicit_bindings=true;call->callee_index=open-1;
    }
    return tokens[call->callee_index].kind==DIAMOND_TOKEN_IDENTIFIER;
}

static size_t resolve_expression(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        size_t *classes,size_t capacity,bool *is_singleton,unsigned depth);

typedef struct ReceiverCallTargets {
    const DiamondFunction *functions[DIAMOND_MAX_UNION_TYPES];
    size_t function_count;
    size_t receiver_classes[DIAMOND_MAX_UNION_TYPES];
    size_t receiver_count;
    bool receiver_singleton;
    bool constructor;
} ReceiverCallTargets;

static bool resolve_call_targets(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t start,const ReceiverCallSyntax *call,unsigned depth,
        ReceiverCallTargets *targets) {
    *targets=(ReceiverCallTargets){};
    const DiamondToken callee=tokens[call->callee_index];
    const char *name=source+callee.span.start;
    const size_t name_length=callee.span.length;
    if(call->callee_index==start) {
        for(size_t index=chunk->function_count;index>0;index--) {
            const DiamondFunction *function=chunk->functions[index-1];
            if(function->owner_class==UINT8_MAX&&!function->nested&&
               strlen(function->name)==name_length&&
               memcmp(function->name,name,name_length)==0) {
                targets->functions[0]=function;targets->function_count=1;
                return true;
            }
        }
        return false;
    }
    if(call->callee_index<2||
       tokens[call->callee_index-1].kind!=DIAMOND_TOKEN_DOT)return false;
    targets->receiver_count=resolve_expression(program,chunk,source,tokens,
        start,call->callee_index-2,targets->receiver_classes,
        DIAMOND_MAX_UNION_TYPES,&targets->receiver_singleton,depth+1);
    if(targets->receiver_count==0)return false;
    if(name_length==3&&memcmp(name,"new",3)==0&&
       targets->receiver_singleton) {
        targets->constructor=true;return true;
    }
    for(size_t index=0;index<targets->receiver_count;index++) {
        const DiamondMethod *method=receiver_lookup_method(chunk,
            targets->receiver_classes[index],targets->receiver_singleton,name,
            name_length);
        if(method==nullptr||method->function_index>=chunk->function_count)
            return false;
        targets->functions[targets->function_count++]=
            chunk->functions[method->function_index];
    }
    return true;
}

static size_t infer_class_bindings(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t start,size_t end,const DiamondFunction *function,
        ReceiverTypeBinding *bindings,unsigned depth);

static bool resolve_target_bindings(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t argument_start,size_t argument_end,
        const DiamondFunction *function,const ReceiverCallSyntax *call,
        ReceiverTypeBinding *bindings,size_t *binding_count,unsigned depth);

static bool infer_structural_class_bindings(const DiamondChunk *chunk,
        const DiamondTypeSet *expected_sets,size_t expected_count,
        uint16_t expected_index,const DiamondTypeSet *actual_sets,
        size_t actual_count,uint16_t actual_index,ReceiverTypeBinding *bindings,
        size_t binding_count,unsigned depth) {
    if(depth>32||expected_index>=expected_count||actual_index>=actual_count)
        return false;
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    const DiamondTypeSet *actual=&actual_sets[actual_index];
    if(expected->count==1&&
       expected->members[0].id>=DIAMOND_TYPE_VARIABLE_BASE&&
       expected->members[0].id<DIAMOND_TYPE_INTERFACE_BASE) {
        size_t classes[DIAMOND_MAX_UNION_TYPES];size_t class_count=0;
        for(size_t index=0;index<actual->count;index++) {
            size_t class_index;
            if(!decode_class_type(chunk,actual->members[index].id,&class_index))
                return false;
            class_count=append_class(classes,class_count,
                DIAMOND_MAX_UNION_TYPES,class_index);
        }
        const size_t variable=(size_t)(expected->members[0].id-
            DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=binding_count)return false;
        return bind_classes(&bindings[variable],classes,class_count);
    }
    if(expected->count!=1||actual->count!=1||
       expected->members[0].id!=actual->members[0].id)return false;
    const DiamondTypeMember *wanted=&expected->members[0];
    const DiamondTypeMember *known=&actual->members[0];
    if(wanted->argument_set!=DIAMOND_NO_TYPE_SET) {
        if(known->argument_set==DIAMOND_NO_TYPE_SET||
           !infer_structural_class_bindings(chunk,expected_sets,expected_count,
               wanted->argument_set,actual_sets,actual_count,
               known->argument_set,bindings,binding_count,depth+1))return false;
    }
    if(wanted->second_argument_set!=DIAMOND_NO_TYPE_SET) {
        if(known->second_argument_set==DIAMOND_NO_TYPE_SET||
           !infer_structural_class_bindings(chunk,expected_sets,expected_count,
               wanted->second_argument_set,actual_sets,actual_count,
               known->second_argument_set,bindings,binding_count,depth+1))
            return false;
    }
    return true;
}

static bool receiver_type_sets_equal(const DiamondTypeSet *left_sets,
        size_t left_count,uint16_t left_index,const DiamondTypeSet *right_sets,
        size_t right_count,uint16_t right_index,unsigned depth) {
    if(depth>32||left_index>=left_count||right_index>=right_count)return false;
    const DiamondTypeSet *left=&left_sets[left_index];
    const DiamondTypeSet *right=&right_sets[right_index];
    if(left->count!=right->count)return false;
    for(size_t index=0;index<left->count;index++) {
        const DiamondTypeMember *a=&left->members[index];
        const DiamondTypeMember *b=&right->members[index];
        if(a->id!=b->id||
           (a->argument_set==DIAMOND_NO_TYPE_SET)!=
               (b->argument_set==DIAMOND_NO_TYPE_SET)||
           (a->second_argument_set==DIAMOND_NO_TYPE_SET)!=
               (b->second_argument_set==DIAMOND_NO_TYPE_SET))return false;
        if(a->argument_set!=DIAMOND_NO_TYPE_SET&&
           !receiver_type_sets_equal(left_sets,left_count,a->argument_set,
               right_sets,right_count,b->argument_set,depth+1))return false;
        if(a->second_argument_set!=DIAMOND_NO_TYPE_SET&&
           !receiver_type_sets_equal(left_sets,left_count,a->second_argument_set,
               right_sets,right_count,b->second_argument_set,depth+1))
            return false;
    }
    return true;
}

static uint16_t receiver_return_set(const DiamondFunction *function) {
    return function->return_type_set!=DIAMOND_NO_TYPE_SET?
        function->return_type_set:function->inferred_return_type_set;
}

static size_t resolve_indexed_expression(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t start,size_t end,size_t *classes,size_t capacity,
        bool *is_singleton,unsigned depth) {
    if(tokens[start].kind!=DIAMOND_TOKEN_IDENTIFIER)return 0;
    size_t first_index=end,paren_depth=0;
    for(size_t index=start+1;index<=end;index++) {
        const DiamondTokenKind kind=tokens[index].kind;
        if(kind==DIAMOND_TOKEN_LEFT_PAREN)paren_depth++;
        else if(kind==DIAMOND_TOKEN_RIGHT_PAREN&&paren_depth>0)paren_depth--;
        else if(kind==DIAMOND_TOKEN_LEFT_BRACKET&&paren_depth==0) {
            size_t bracket_depth=0,close=index;
            for(size_t candidate=index;candidate<=end;candidate++) {
                if(tokens[candidate].kind==DIAMOND_TOKEN_LEFT_BRACKET)
                    bracket_depth++;
                else if(tokens[candidate].kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
                        --bracket_depth==0) {close=candidate;break;}
            }
            if(close>index&&close<end&&
               tokens[close+1].kind==DIAMOND_TOKEN_LEFT_PAREN) {
                index=close;continue;
            }
            first_index=index;break;
        }
    }
    if(tokens[first_index].kind!=DIAMOND_TOKEN_LEFT_BRACKET)return 0;

    const DiamondTypeSet *type_sets=nullptr;size_t type_set_count=0;
    int32_t known_set=-1;
    ReceiverTypeBinding bindings[8]={};size_t binding_count=0;
    const DiamondToken name_token=tokens[start];
    if(first_index==start+1) {
        const DiamondFunction *owner=nullptr;
        const DiamondScopeLocal *local=find_scope_local(program,chunk,
            source+name_token.span.start,name_token.span.length,
            name_token.span.start,&owner);
        if(local==nullptr||owner==nullptr)return 0;
        uint8_t known_type;int32_t tooling_set;
        local_type_at_offset(owner,local,name_token.span.start,&known_type,
            &known_set,&tooling_set);
        (void)known_type;
        if(known_set<0)known_set=tooling_set;
        type_sets=owner->type_sets;type_set_count=owner->type_set_count;
    } else {
        if(first_index<start+3||
           tokens[first_index-1].kind!=DIAMOND_TOKEN_RIGHT_PAREN)return 0;
        ReceiverCallSyntax call;
        if(!parse_receiver_call_syntax(chunk,source,tokens,start,first_index-1,
                &call))return 0;
        ReceiverCallTargets targets;
        if(!resolve_call_targets(program,chunk,source,tokens,start,&call,depth,
                &targets)||targets.constructor||targets.function_count==0)
            return 0;
        const DiamondFunction *target=targets.functions[0];
        for(size_t index=1;index<targets.function_count;index++) {
            const DiamondFunction *candidate=targets.functions[index];
            if(candidate->type_variable_count>0||target->type_variable_count>0)
                return 0;
            const uint16_t target_return=receiver_return_set(target);
            const uint16_t candidate_return=receiver_return_set(candidate);
            if(target_return==DIAMOND_NO_TYPE_SET||
               candidate_return==DIAMOND_NO_TYPE_SET||
               !receiver_type_sets_equal(target->type_sets,
                   target->type_set_count,target_return,candidate->type_sets,
                   candidate->type_set_count,candidate_return,0))return 0;
        }
        if(first_index<2||!resolve_target_bindings(program,chunk,source,tokens,
                call.left+1,first_index-2,target,&call,bindings,&binding_count,
                depth))return 0;
        const uint16_t return_set=receiver_return_set(target);
        if(return_set==DIAMOND_NO_TYPE_SET||return_set>=target->type_set_count)
            return 0;
        type_sets=target->type_sets;type_set_count=target->type_set_count;
        known_set=(int32_t)return_set;
    }
    if(known_set<0||(size_t)known_set>=type_set_count)return 0;
    size_t token=first_index;
    int active_binding=-1;
    while(token<=end) {
        if(tokens[token].kind!=DIAMOND_TOKEN_LEFT_BRACKET)return 0;
        size_t nesting=0,close=token;
        for(size_t index=token;index<=end;index++) {
            if(tokens[index].kind==DIAMOND_TOKEN_LEFT_BRACKET)nesting++;
            else if(tokens[index].kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
                    --nesting==0) {close=index;break;}
        }
        if(close==token)return 0;
        if(active_binding>=0) {
            if(bindings[(size_t)active_binding].array_depth==0)return 0;
            bindings[(size_t)active_binding].array_depth--;
        } else {
            const DiamondTypeSet *outer=&type_sets[(size_t)known_set];
            if(outer->count==1&&
               outer->members[0].id>=DIAMOND_TYPE_VARIABLE_BASE&&
               outer->members[0].id<DIAMOND_TYPE_INTERFACE_BASE) {
                const size_t variable=(size_t)(outer->members[0].id-
                    DIAMOND_TYPE_VARIABLE_BASE);
                if(variable>=binding_count||bindings[variable].array_depth==0)
                    return 0;
                bindings[variable].array_depth--;active_binding=(int)variable;
            } else {
                if(outer->count!=1||outer->members[0].id!=DIAMOND_TYPE_ARRAY||
                   outer->members[0].argument_set==DIAMOND_NO_TYPE_SET||
                   outer->members[0].argument_set>=type_set_count)return 0;
                known_set=(int32_t)outer->members[0].argument_set;
            }
        }
        token=close+1;
    }
    if(active_binding>=0) {
        const ReceiverTypeBinding binding=bindings[(size_t)active_binding];
        if(binding.array_depth!=0)return 0;
        size_t count=0;
        for(size_t index=0;index<binding.class_count;index++)
            count=append_class(classes,count,capacity,binding.classes[index]);
        *is_singleton=false;return count;
    }
    const DiamondTypeSet *elements=&type_sets[(size_t)known_set];
    size_t count=0;
    for(size_t index=0;index<elements->count;index++) {
        size_t class_index;
        const uint8_t type=elements->members[index].id;
        if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable>=binding_count)return 0;
            if(bindings[variable].array_depth!=0)return 0;
            if(bindings[variable].class_count==0)return 0;
            for(size_t member=0;member<bindings[variable].class_count;member++)
                count=append_class(classes,count,capacity,
                    bindings[variable].classes[member]);
            continue;
        } else if(!decode_class_type(chunk,type,&class_index))return 0;
        count=append_class(classes,count,capacity,class_index);
    }
    *is_singleton=false;
    return count;
}

static size_t infer_class_bindings(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t start,size_t end,const DiamondFunction *function,
        ReceiverTypeBinding *bindings,unsigned depth) {
    if(start>end||function->type_variable_count==0)return 0;
    for(size_t index=0;index<function->type_variable_count;index++)
        bindings[index]=(ReceiverTypeBinding){};
    size_t argument_start=start,parameter=0,paren_depth=0,bracket_depth=0;
    for(size_t index=start;index<=end+1;index++) {
        const bool at_end=index==end+1;
        const DiamondTokenKind kind=at_end?DIAMOND_TOKEN_COMMA:tokens[index].kind;
        if(!at_end&&kind==DIAMOND_TOKEN_LEFT_PAREN)paren_depth++;
        else if(!at_end&&kind==DIAMOND_TOKEN_RIGHT_PAREN&&paren_depth>0)paren_depth--;
        else if(!at_end&&kind==DIAMOND_TOKEN_LEFT_BRACKET)bracket_depth++;
        else if(!at_end&&kind==DIAMOND_TOKEN_RIGHT_BRACKET&&bracket_depth>0)
            bracket_depth--;
        if(kind!=DIAMOND_TOKEN_COMMA||paren_depth!=0||bracket_depth!=0)continue;
        if(parameter<DIAMOND_MAX_DECLARED_PARAMETERS&&argument_start<index) {
            const uint16_t expected_index=function->parameter_type_sets[parameter];
            if(expected_index!=DIAMOND_NO_TYPE_SET&&
               expected_index<function->type_set_count) {
                const DiamondTypeSet *expected=&function->type_sets[expected_index];
                bool inferred_structurally=false;
                if(argument_start+1==index&&
                   tokens[argument_start].kind==DIAMOND_TOKEN_IDENTIFIER) {
                    const DiamondToken argument=tokens[argument_start];
                    const DiamondFunction *owner=nullptr;
                    const DiamondScopeLocal *local=find_scope_local(program,chunk,
                        source+argument.span.start,argument.span.length,
                        argument.span.start,&owner);
                    if(local!=nullptr&&owner!=nullptr) {
                        uint8_t known_type;int32_t actual,tooling;
                        local_type_at_offset(owner,local,argument.span.start,
                            &known_type,&actual,&tooling);
                        (void)known_type;
                        if(actual<0)actual=tooling;
                        if(actual>=0&&(size_t)actual<owner->type_set_count)
                            inferred_structurally=infer_structural_class_bindings(
                                chunk,function->type_sets,function->type_set_count,
                                expected_index,owner->type_sets,
                                owner->type_set_count,(uint16_t)actual,bindings,
                                function->type_variable_count,0);
                    }
                }
                if(!inferred_structurally&&expected->count==1&&
                   expected->members[0].id>=DIAMOND_TYPE_VARIABLE_BASE&&
                   expected->members[0].id<DIAMOND_TYPE_INTERFACE_BASE) {
                    size_t classes[DIAMOND_MAX_UNION_TYPES];bool singleton=false;
                    const size_t count=resolve_expression(program,chunk,source,
                        tokens,argument_start,index-1,classes,
                        DIAMOND_MAX_UNION_TYPES,&singleton,depth+1);
                    const size_t variable=(size_t)(expected->members[0].id-
                        DIAMOND_TYPE_VARIABLE_BASE);
                    if(count==0||singleton||variable>=function->type_variable_count)
                        return 0;
                    if(!bind_classes(&bindings[variable],classes,count))return 0;
                }
            }
        }
        parameter++;argument_start=index+1;
    }
    for(size_t index=0;index<function->type_variable_count;index++)
        if(bindings[index].class_count==0)return 0;
    return function->type_variable_count;
}

static bool resolve_target_bindings(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t argument_start,size_t argument_end,
        const DiamondFunction *function,const ReceiverCallSyntax *call,
        ReceiverTypeBinding *bindings,size_t *binding_count,unsigned depth) {
    memcpy(bindings,call->bindings,8*sizeof bindings[0]);
    *binding_count=call->binding_count;
    if(function->type_variable_count==0)
        return !call->has_explicit_bindings;
    if(!call->has_explicit_bindings)
        *binding_count=infer_class_bindings(program,chunk,source,tokens,
            argument_start,argument_end,function,bindings,depth);
    return *binding_count==function->type_variable_count;
}

static size_t resolve_name(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,DiamondToken token,size_t *classes,size_t capacity,
        bool *is_singleton) {
    char name[DIAMOND_MAX_FUNCTION_NAME];
    size_t length=token.span.length;
    if(length>=sizeof name)length=sizeof name-1;
    memcpy(name,source+token.span.start,length);name[length]='\0';
    for(size_t index=0;index<chunk->class_count;index++) {
        if(strcmp(chunk->classes[index].name,name)==0) {
            classes[0]=index;*is_singleton=true;return 1;
        }
    }
    const DiamondFunction *owner=nullptr;
    const DiamondScopeLocal *local=find_scope_local(program,chunk,name,length,
        token.span.start,&owner);
    if(local==nullptr)return 0;
    *is_singleton=false;
    uint8_t known_type=local->known_type;int32_t known_type_set=local->known_type_set;
    int32_t tooling_type_set=local->tooling_type_set;
    local_type_at_offset(owner,local,token.span.start,&known_type,&known_type_set,
        &tooling_type_set);
    size_t class_index;
    if(decode_class_type(chunk,known_type,&class_index)) {
        classes[0]=class_index;return 1;
    }
    if(known_type_set<0)known_type_set=tooling_type_set;
    if(known_type_set<0||(size_t)known_type_set>=owner->type_set_count)return 0;
    const DiamondTypeSet *set=&owner->type_sets[(size_t)known_type_set];
    size_t count=0;
    for(size_t index=0;index<set->count;index++)
        if(decode_class_type(chunk,set->members[index].id,&class_index))
            count=append_class(classes,count,capacity,class_index);
    return count;
}

/* Resolves a call expression whose final token is `)`. Argument contents do
 * not affect its result type, so only the matching `(` and callee are needed.
 * A method must resolve for every possible receiver class: silently keeping
 * just one arm of a union would make a later completion look safer than the
 * source annotation says it is. */
static size_t resolve_call(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        size_t *classes,size_t capacity,bool *is_singleton,unsigned depth) {
    ReceiverCallSyntax call;
    if(!parse_receiver_call_syntax(chunk,source,tokens,start,end,&call))return 0;
    ReceiverCallTargets targets;
    if(!resolve_call_targets(program,chunk,source,tokens,start,&call,depth,
            &targets))return 0;
    if(targets.constructor) {
        size_t count=0;
        for(size_t index=0;index<targets.receiver_count;index++)
            count=append_class(classes,count,capacity,
                targets.receiver_classes[index]);
        *is_singleton=false;return count;
    }
    size_t count=0;
    for(size_t index=0;index<targets.function_count;index++) {
        const DiamondFunction *function=targets.functions[index];
        size_t candidate_binding_count=0;
        ReceiverTypeBinding candidate_bindings[8];
        if(!resolve_target_bindings(program,chunk,source,tokens,call.left+1,
                end-1,function,&call,candidate_bindings,
                &candidate_binding_count,depth))return 0;
        size_t returned[DIAMOND_MAX_UNION_TYPES];
        const size_t returned_count=function_return_classes_bound(chunk,function,
            candidate_bindings,candidate_binding_count,returned,
            DIAMOND_MAX_UNION_TYPES);
        if(returned_count==0)return 0;
        for(size_t member=0;member<returned_count;member++)
            count=append_class(classes,count,capacity,returned[member]);
    }
    *is_singleton=false;return count;
}

static size_t resolve_expression(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        size_t *classes,size_t capacity,bool *is_singleton,unsigned depth) {
    if(start>end||capacity==0||depth>32)return 0;
    /* Grouping parentheses do not change the receiver. Only unwrap when the
     * opening token's matching close is this range's final token; otherwise
     * the trailing `)` belongs to a call and resolve_call must inspect it. */
    if(tokens[start].kind==DIAMOND_TOKEN_LEFT_PAREN&&
       tokens[end].kind==DIAMOND_TOKEN_RIGHT_PAREN) {
        size_t nesting=0;
        for(size_t index=start;index<=end;index++) {
            if(tokens[index].kind==DIAMOND_TOKEN_LEFT_PAREN)nesting++;
            else if(tokens[index].kind==DIAMOND_TOKEN_RIGHT_PAREN&&
                    --nesting==0) {
                if(index==end)
                    return resolve_expression(program,chunk,source,tokens,
                        start+1,end-1,classes,capacity,is_singleton,depth+1);
                break;
            }
        }
    }
    if(start==end) {
        const DiamondToken token=tokens[start];
        if(token.kind==DIAMOND_TOKEN_IDENTIFIER)
            return resolve_name(program,chunk,source,token,classes,capacity,is_singleton);
        if(token.kind==DIAMOND_TOKEN_SELF) {
            const DiamondFunction *function=find_enclosing_function(program,chunk,token.span.start);
            if(function==nullptr||function->owner_class>=chunk->class_count)return 0;
            classes[0]=function->owner_class;
            *is_singleton=function_is_singleton_of(chunk,&chunk->classes[function->owner_class],function);
            return 1;
        }
        if(token.kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
            const DiamondFunction *function=find_enclosing_function(program,chunk,token.span.start);
            if(function==nullptr||function->owner_class>=chunk->class_count)return 0;
            const DiamondClass *class=&chunk->classes[function->owner_class];
            const char *name=source+token.span.start+1;const size_t length=token.span.length-1;
            for(size_t field=0;field<class->field_count;field++) {
                if(strlen(class->fields[field])==length&&memcmp(class->fields[field],name,length)==0&&
                   class->field_type_status[field]==1&&class->field_known_class[field]<chunk->class_count) {
                    classes[0]=class->field_known_class[field];*is_singleton=false;return 1;
                }
            }
        }
        return 0;
    }
    if(tokens[end].kind==DIAMOND_TOKEN_RIGHT_BRACKET)
        return resolve_indexed_expression(program,chunk,source,tokens,start,end,
            classes,capacity,is_singleton,depth);
    if(tokens[end].kind==DIAMOND_TOKEN_RIGHT_PAREN)
        return resolve_call(program,chunk,source,tokens,start,end,classes,capacity,is_singleton,depth);
    return 0;
}

/* Tokenizes `source` up to `stop_offset` and finds the receiver expression
 * that ends just before a trailing `.` or `.partial` identifier: on success
 * `*tokens` (caller frees) holds every token, and [*start, *end] is the
 * receiver's inclusive token range. */
static bool locate_receiver(const char *source,size_t stop_offset,
        DiamondToken **tokens_out,size_t *start_out,size_t *end_out) {
    DiamondLexer lexer;diamond_lexer_init(&lexer,source);
    size_t count=0,capacity=32;
    DiamondToken *tokens=malloc(capacity*sizeof *tokens);
    if(tokens==nullptr)return false;
    while(true) {
        const DiamondToken token=diamond_lexer_next(&lexer);
        if(token.kind==DIAMOND_TOKEN_EOF||token.span.start>=stop_offset)break;
        if(token.kind==DIAMOND_TOKEN_NEWLINE)continue;
        if(count==capacity) {
            capacity*=2;
            DiamondToken *grown=realloc(tokens,capacity*sizeof *tokens);
            if(grown==nullptr) {free(tokens);return false;}
            tokens=grown;
        }
        tokens[count++]=token;
    }
    size_t dot=count;
    if(count>0&&tokens[count-1].kind==DIAMOND_TOKEN_DOT)dot=count-1;
    else if(count>1&&tokens[count-1].kind==DIAMOND_TOKEN_IDENTIFIER&&
            tokens[count-2].kind==DIAMOND_TOKEN_DOT)dot=count-2;
    if(dot==0||dot==count) {free(tokens);return false;}
    /* Walk backward to the start of the receiver's current expression. A
     * newline was discarded above, so the last statement boundary is the
     * nearest token that cannot participate in a postfix call chain. */
    size_t start=dot-1,nesting=0,bracket_nesting=0,brace_nesting=0;
    for(size_t index=dot;index>0;index--) {
        const DiamondTokenKind kind=tokens[index-1].kind;
        if(kind==DIAMOND_TOKEN_RIGHT_PAREN)nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_BRACKET)bracket_nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_BRACE)brace_nesting++;
        else if(kind==DIAMOND_TOKEN_LEFT_BRACE&&brace_nesting>0&&--brace_nesting==0&&
                nesting==0&&bracket_nesting==0&&
                (index==1||tokens[index-2].span.line!=tokens[index-1].span.line||
                 (tokens[index-2].kind!=DIAMOND_TOKEN_IDENTIFIER&&
                  tokens[index-2].kind!=DIAMOND_TOKEN_RIGHT_PAREN&&
                  tokens[index-2].kind!=DIAMOND_TOKEN_RIGHT_BRACKET))) {
            /* A brace group not following an expression is a Hash literal and
             * the receiver's first token. */
            start=index-1;break;
        }
        else if(kind==DIAMOND_TOKEN_LEFT_BRACKET&&bracket_nesting>0) {
            bracket_nesting--;
            /* A bracket group not following an expression is an Array literal
             * (after one on the same line it is an index and the receiver continues left). */
            if(bracket_nesting==0&&nesting==0&&brace_nesting==0&&
               (index==1||tokens[index-2].span.line!=tokens[index-1].span.line||
                (tokens[index-2].kind!=DIAMOND_TOKEN_IDENTIFIER&&
                 tokens[index-2].kind!=DIAMOND_TOKEN_RIGHT_PAREN&&
                 tokens[index-2].kind!=DIAMOND_TOKEN_RIGHT_BRACKET))) {
                start=index-1;break;
            }
        }
        else if(kind==DIAMOND_TOKEN_LEFT_PAREN&&nesting>0) {
            nesting--;
            /* A matched `(` preceded by an identifier belongs to a call and
             * its callee is still part of the receiver chain. Otherwise it is
             * a grouping boundary: do not absorb the preceding statement. */
            if(nesting==0&&
               (index==1||(tokens[index-2].kind!=DIAMOND_TOKEN_IDENTIFIER&&
                            tokens[index-2].kind!=DIAMOND_TOKEN_RIGHT_BRACKET))) {
                start=index-1;break;
            }
        }
        start=index-1;
        if(nesting==0&&bracket_nesting==0&&brace_nesting==0&&index>1&&
           tokens[index-2].kind!=DIAMOND_TOKEN_DOT&&
           kind!=DIAMOND_TOKEN_RIGHT_PAREN&&kind!=DIAMOND_TOKEN_LEFT_PAREN&&
           kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&kind!=DIAMOND_TOKEN_LEFT_BRACKET&&
           kind!=DIAMOND_TOKEN_DOT)break;
    }
    *tokens_out=tokens;*start_out=start;*end_out=dot-1;
    return true;
}

size_t receiver_resolve_classes(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,size_t stop_offset,
        size_t *class_indices,size_t max_candidates,bool *is_singleton) {
    if(max_candidates==0)return 0;
    DiamondToken *tokens;size_t start,end;
    if(!locate_receiver(source,stop_offset,&tokens,&start,&end))return 0;
    const size_t result=resolve_expression(program,chunk,source,tokens,start,end,
        class_indices,max_candidates,is_singleton,0);
    free(tokens);return result;
}

static bool is_builtin_value_type(uint8_t type) {
    return type==DIAMOND_TYPE_STRING||type==DIAMOND_TYPE_ARRAY||
           type==DIAMOND_TYPE_HASH||type==DIAMOND_TYPE_INT||type==DIAMOND_TYPE_FLOAT;
}

const char *const *receiver_builtin_prefixes(uint8_t builtin_type,size_t *count) {
    static const char *const array_prefixes[]={"array_","enumerable_"};
    static const char *const hash_prefixes[]={"hash_","enumerable_"};
    static const char *const string_prefixes[]={"string_"};
    static const char *const int_prefixes[]={"integer_","numeric_"};
    static const char *const float_prefixes[]={"float_","numeric_"};
    switch(builtin_type) {
        case DIAMOND_TYPE_ARRAY:*count=2;return array_prefixes;
        case DIAMOND_TYPE_HASH:*count=2;return hash_prefixes;
        case DIAMOND_TYPE_STRING:*count=1;return string_prefixes;
        case DIAMOND_TYPE_INT:*count=2;return int_prefixes;
        case DIAMOND_TYPE_FLOAT:*count=2;return float_prefixes;
        default:*count=0;return nullptr;
    }
}

bool receiver_hash_uses_enumerable(const char *name) {
    static const char *const shared[]={"lazy","select","count","any","all","reduce","map"};
    for(size_t index=0;index<sizeof shared/sizeof shared[0];index++)
        if(strcmp(name,shared[index])==0)return true;
    return false;
}

/* The one built-in type a function's declared (or, failing that, inferred)
 * return type names, if it names exactly one. */
static bool function_builtin_return(const DiamondFunction *function,uint8_t *type) {
    const uint16_t sets[2]={function->return_type_set,function->inferred_return_type_set};
    for(size_t index=0;index<2;index++) {
        if(sets[index]==DIAMOND_NO_TYPE_SET||sets[index]>=function->type_set_count)continue;
        const DiamondTypeSet *set=&function->type_sets[sets[index]];
        if(set->count==1&&is_builtin_value_type(set->members[0].id)) {
            *type=set->members[0].id;return true;
        }
    }
    return false;
}

static const DiamondFunction *find_plain_function(const DiamondChunk *chunk,
        const char *name,size_t length) {
    for(size_t index=chunk->function_count;index>0;index--) {
        const DiamondFunction *function=chunk->functions[index-1];
        if(function->owner_class==UINT8_MAX&&!function->nested&&
           strlen(function->name)==length&&memcmp(function->name,name,length)==0)
            return function;
    }
    return nullptr;
}

/* What calling `method` on a built-in value of `receiver` returns, when known:
 * a native with one fixed result type first (natives win at runtime), then the
 * prelude extension function the VM would dispatch to. */
static bool builtin_method_result(const DiamondChunk *chunk,uint8_t receiver,
        const char *method,size_t length,uint8_t *result) {
    if(diamond_native_method_return_type(receiver,method,length,result)&&
       is_builtin_value_type(*result))return true;
    if(length>0&&method[length-1]=='?')length--;
    size_t prefix_count;
    const char *const *prefixes=receiver_builtin_prefixes(receiver,&prefix_count);
    char bridge[DIAMOND_MAX_FUNCTION_NAME];
    for(size_t pass=0;pass<2;pass++)
        for(size_t prefix=0;prefix<prefix_count;prefix++) {
            if(receiver==DIAMOND_TYPE_HASH&&strcmp(prefixes[prefix],"enumerable_")==0) {
                char plain[DIAMOND_MAX_FUNCTION_NAME];
                if(length>=sizeof plain)continue;
                memcpy(plain,method,length);plain[length]='\0';
                if(!receiver_hash_uses_enumerable(plain))continue;
            }
            const int written=snprintf(bridge,sizeof bridge,"%s%s%.*s",
                pass==0?"":"diamond_",prefixes[prefix],(int)length,method);
            if(written<=0||(size_t)written>=sizeof bridge)continue;
            const DiamondFunction *function=find_plain_function(chunk,bridge,(size_t)written);
            if(function!=nullptr)
                return function_builtin_return(function,result);
        }
    return false;
}

/* Index of the token that opens the bracket closed by tokens[close], or
 * `start` when none is found in range. */
static size_t matching_open(const DiamondToken *tokens,size_t start,size_t close,
        DiamondTokenKind open_kind,DiamondTokenKind close_kind) {
    size_t nesting=0;
    for(size_t index=close+1;index>start;index--) {
        const DiamondTokenKind kind=tokens[index-1].kind;
        if(kind==close_kind)nesting++;
        else if(kind==open_kind&&--nesting==0)return index-1;
    }
    return start;
}

static bool builtin_type_of(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        unsigned depth,uint8_t *type) {
    if(start>end||depth>32)return false;
    if(tokens[start].kind==DIAMOND_TOKEN_LEFT_PAREN&&
       tokens[end].kind==DIAMOND_TOKEN_RIGHT_PAREN&&
       matching_open(tokens,start,end,DIAMOND_TOKEN_LEFT_PAREN,
           DIAMOND_TOKEN_RIGHT_PAREN)==start&&end>start)
        return builtin_type_of(program,chunk,source,tokens,start+1,end-1,depth+1,type);
    if(start==end) {
        const DiamondToken token=tokens[start];
        uint8_t found=DIAMOND_TYPE_NIL;
        if(token.kind==DIAMOND_TOKEN_STRING)found=DIAMOND_TYPE_STRING;
        else if(token.kind==DIAMOND_TOKEN_INTEGER)found=DIAMOND_TYPE_INT;
        else if(token.kind==DIAMOND_TOKEN_FLOAT)found=DIAMOND_TYPE_FLOAT;
        else if(token.kind==DIAMOND_TOKEN_IDENTIFIER) {
            char name[DIAMOND_MAX_FUNCTION_NAME];
            size_t length=token.span.length;
            if(length>=sizeof name)length=sizeof name-1;
            memcpy(name,source+token.span.start,length);name[length]='\0';
            const DiamondFunction *owner=nullptr;
            const DiamondScopeLocal *local=find_scope_local(program,chunk,name,
                length,token.span.start,&owner);
            if(local!=nullptr) {
                int32_t known_type_set,tooling_type_set;
                local_type_at_offset(owner,local,token.span.start,&found,
                    &known_type_set,&tooling_type_set);
            }
        }
        if(!is_builtin_value_type(found))return false;
        *type=found;return true;
    }
    /* [..] / {..} spanning the whole range is an Array / Hash literal; after
     * something else it is an index, whose element type is not known. */
    if(tokens[end].kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
       matching_open(tokens,start,end,DIAMOND_TOKEN_LEFT_BRACKET,
           DIAMOND_TOKEN_RIGHT_BRACKET)==start) {*type=DIAMOND_TYPE_ARRAY;return true;}
    if(tokens[end].kind==DIAMOND_TOKEN_RIGHT_BRACE&&
       matching_open(tokens,start,end,DIAMOND_TOKEN_LEFT_BRACE,
           DIAMOND_TOKEN_RIGHT_BRACE)==start) {*type=DIAMOND_TYPE_HASH;return true;}
    if(tokens[end].kind!=DIAMOND_TOKEN_RIGHT_PAREN)return false;
    const size_t open=matching_open(tokens,start,end,DIAMOND_TOKEN_LEFT_PAREN,
        DIAMOND_TOKEN_RIGHT_PAREN);
    if(open==start||tokens[open-1].kind!=DIAMOND_TOKEN_IDENTIFIER)return false;
    const size_t callee=open-1;
    const char *name=source+tokens[callee].span.start;
    const size_t length=tokens[callee].span.length;
    if(callee==start) {
        const DiamondFunction *function=find_plain_function(chunk,name,length);
        return function!=nullptr&&function_builtin_return(function,type);
    }
    if(callee<2||tokens[callee-1].kind!=DIAMOND_TOKEN_DOT)return false;
    uint8_t receiver;
    if(!builtin_type_of(program,chunk,source,tokens,start,callee-2,depth+1,&receiver))
        return false;
    return builtin_method_result(chunk,receiver,name,length,type);
}

bool receiver_resolve_builtin_type(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,size_t stop_offset,
        uint8_t *builtin_type) {
    DiamondToken *tokens;size_t start,end;
    if(!locate_receiver(source,stop_offset,&tokens,&start,&end))return false;
    const bool found=builtin_type_of(program,chunk,source,tokens,start,end,0,builtin_type);
    free(tokens);return found;
}

const DiamondMethod *receiver_lookup_method(const DiamondChunk *chunk,
        size_t class_index,bool is_singleton,const char *name,size_t name_length) {
    const DiamondClass *current=&chunk->classes[class_index];
    while(current!=nullptr) {
        const DiamondMethod *methods=is_singleton?current->singleton_methods:current->methods;
        const size_t count=is_singleton?current->singleton_method_count:current->method_count;
        for(size_t index=count;index>0;index--) {
            const DiamondMethod *method=&methods[index-1];
            if(strlen(method->name)==name_length&&memcmp(method->name,name,name_length)==0)
                return method;
        }
        current=current->superclass==UINT8_MAX?nullptr:&chunk->classes[current->superclass];
    }
    return nullptr;
}
