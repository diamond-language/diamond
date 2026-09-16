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

static size_t function_return_classes_bound(const DiamondChunk *chunk,
        const DiamondFunction *function,const size_t *bindings,
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
            if(variable>=binding_count||bindings==nullptr)return 0;
            class_index=bindings[variable];
        } else if(!decode_class_type(chunk,type,&class_index))return 0;
        count=append_class(classes,count,capacity,class_index);
    }
    return count;
}

/* Parse the deliberately narrow, provable generic-call form `[Class, ...]`.
 * Parameterized/union arguments remain unresolved until the receiver resolver
 * has a full source-level type-expression parser of its own. */
static size_t explicit_class_bindings(const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        size_t *bindings,size_t capacity) {
    size_t count=0;
    for(size_t index=start;index<=end;) {
        if(tokens[index].kind!=DIAMOND_TOKEN_IDENTIFIER||count==capacity)return 0;
        const char *name=source+tokens[index].span.start;
        const size_t length=tokens[index].span.length;
        bool found=false;
        for(size_t candidate=0;candidate<chunk->class_count;candidate++)
            if(strlen(chunk->classes[candidate].name)==length&&
               memcmp(chunk->classes[candidate].name,name,length)==0) {
                bindings[count++]=candidate;found=true;break;
            }
        if(!found)return 0;
        index++;
        if(index>end)break;
        if(tokens[index].kind!=DIAMOND_TOKEN_COMMA)return 0;
        index++;
    }
    return count;
}

static size_t resolve_expression(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        size_t *classes,size_t capacity,bool *is_singleton,unsigned depth);

static size_t resolve_indexed_local(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t start,size_t end,size_t *classes,size_t capacity,
        bool *is_singleton) {
    if(tokens[start].kind!=DIAMOND_TOKEN_IDENTIFIER)return 0;
    const DiamondToken name_token=tokens[start];
    const DiamondFunction *owner=nullptr;
    const DiamondScopeLocal *local=find_scope_local(program,chunk,
        source+name_token.span.start,name_token.span.length,
        name_token.span.start,&owner);
    if(local==nullptr||owner==nullptr)return 0;
    uint8_t known_type;int32_t known_set,tooling_set;
    local_type_at_offset(owner,local,name_token.span.start,&known_type,&known_set,
        &tooling_set);
    (void)known_type;
    if(known_set<0)known_set=tooling_set;
    if(known_set<0||(size_t)known_set>=owner->type_set_count)return 0;
    size_t token=start+1;
    while(token<=end) {
        if(tokens[token].kind!=DIAMOND_TOKEN_LEFT_BRACKET)return 0;
        size_t nesting=0,close=token;
        for(size_t index=token;index<=end;index++) {
            if(tokens[index].kind==DIAMOND_TOKEN_LEFT_BRACKET)nesting++;
            else if(tokens[index].kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
                    --nesting==0) {close=index;break;}
        }
        if(close==token)return 0;
        const DiamondTypeSet *outer=&owner->type_sets[(size_t)known_set];
        if(outer->count!=1||outer->members[0].id!=DIAMOND_TYPE_ARRAY||
           outer->members[0].argument_set==DIAMOND_NO_TYPE_SET||
           outer->members[0].argument_set>=owner->type_set_count)return 0;
        known_set=(int32_t)outer->members[0].argument_set;
        token=close+1;
    }
    const DiamondTypeSet *elements=&owner->type_sets[(size_t)known_set];
    size_t count=0;
    for(size_t index=0;index<elements->count;index++) {
        size_t class_index;
        if(!decode_class_type(chunk,elements->members[index].id,&class_index))
            return 0;
        count=append_class(classes,count,capacity,class_index);
    }
    *is_singleton=false;
    return count;
}

static size_t infer_class_bindings(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,const DiamondToken *tokens,
        size_t start,size_t end,const DiamondFunction *function,
        size_t *bindings,unsigned depth) {
    if(start>end||function->type_variable_count==0)return 0;
    for(size_t index=0;index<function->type_variable_count;index++)
        bindings[index]=SIZE_MAX;
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
                if(expected->count==1&&
                   expected->members[0].id>=DIAMOND_TYPE_VARIABLE_BASE&&
                   expected->members[0].id<DIAMOND_TYPE_INTERFACE_BASE) {
                    size_t classes[DIAMOND_MAX_UNION_TYPES];bool singleton=false;
                    const size_t count=resolve_expression(program,chunk,source,
                        tokens,argument_start,index-1,classes,
                        DIAMOND_MAX_UNION_TYPES,&singleton,depth+1);
                    const size_t variable=(size_t)(expected->members[0].id-
                        DIAMOND_TYPE_VARIABLE_BASE);
                    if(count!=1||singleton||variable>=function->type_variable_count)
                        return 0;
                    if(bindings[variable]==SIZE_MAX)bindings[variable]=classes[0];
                    else if(bindings[variable]!=classes[0])return 0;
                }
            }
        }
        parameter++;argument_start=index+1;
    }
    for(size_t index=0;index<function->type_variable_count;index++)
        if(bindings[index]==SIZE_MAX)return 0;
    return function->type_variable_count;
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
    size_t nesting=0,left=end;
    for(size_t index=end+1;index>start;index--) {
        const DiamondTokenKind kind=tokens[index-1].kind;
        if(kind==DIAMOND_TOKEN_RIGHT_PAREN)nesting++;
        else if(kind==DIAMOND_TOKEN_LEFT_PAREN&&--nesting==0) {left=index-1;break;}
    }
    if(left==start)return 0;
    size_t callee_index=left-1;
    size_t bindings[8]={};size_t binding_count=0;bool has_explicit_bindings=false;
    if(tokens[callee_index].kind==DIAMOND_TOKEN_RIGHT_BRACKET) {
        size_t bracket_nesting=0,open=callee_index;
        for(size_t index=callee_index+1;index>start;index--) {
            const DiamondTokenKind kind=tokens[index-1].kind;
            if(kind==DIAMOND_TOKEN_RIGHT_BRACKET)bracket_nesting++;
            else if(kind==DIAMOND_TOKEN_LEFT_BRACKET&&--bracket_nesting==0) {
                open=index-1;break;
            }
        }
        if(open==start||tokens[open-1].kind!=DIAMOND_TOKEN_IDENTIFIER)return 0;
        binding_count=explicit_class_bindings(chunk,source,tokens,open+1,
            callee_index-1,bindings,8);
        if(binding_count==0)return 0;
        has_explicit_bindings=true;
        callee_index=open-1;
    }
    if(tokens[callee_index].kind!=DIAMOND_TOKEN_IDENTIFIER)return 0;
    const DiamondToken callee=tokens[callee_index];
    const char *name=source+callee.span.start;const size_t name_length=callee.span.length;
    if(callee_index==start) {
        for(size_t index=chunk->function_count;index>0;index--) {
            const DiamondFunction *function=chunk->functions[index-1];
            if(function->owner_class==UINT8_MAX&&!function->nested&&
               strlen(function->name)==name_length&&memcmp(function->name,name,name_length)==0) {
                if(!has_explicit_bindings&&function->type_variable_count>0)
                    binding_count=infer_class_bindings(program,chunk,source,tokens,
                        left+1,end-1,function,bindings,depth);
                if(binding_count!=function->type_variable_count)return 0;
                *is_singleton=false;
                return function_return_classes_bound(chunk,function,bindings,
                    binding_count,classes,capacity);
            }
        }
        return 0;
    }
    if(callee_index<2||tokens[callee_index-1].kind!=DIAMOND_TOKEN_DOT)return 0;
    size_t receiver_classes[DIAMOND_MAX_UNION_TYPES];bool receiver_singleton=false;
    const size_t receiver_count=resolve_expression(program,chunk,source,tokens,start,
        callee_index-2,
        receiver_classes,DIAMOND_MAX_UNION_TYPES,&receiver_singleton,depth+1);
    if(receiver_count==0)return 0;
    if(name_length==3&&memcmp(name,"new",3)==0&&receiver_singleton) {
        size_t count=0;
        for(size_t index=0;index<receiver_count;index++)
            count=append_class(classes,count,capacity,receiver_classes[index]);
        *is_singleton=false;return count;
    }
    size_t count=0;
    for(size_t index=0;index<receiver_count;index++) {
        const DiamondMethod *method=receiver_lookup_method(chunk,receiver_classes[index],
            receiver_singleton,name,name_length);
        if(method==nullptr||method->function_index>=chunk->function_count)return 0;
        const DiamondFunction *function=chunk->functions[method->function_index];
        size_t candidate_binding_count=binding_count;
        size_t candidate_bindings[8];
        memcpy(candidate_bindings,bindings,sizeof candidate_bindings);
        if(!has_explicit_bindings&&function->type_variable_count>0)
            candidate_binding_count=infer_class_bindings(program,chunk,source,
                tokens,left+1,end-1,function,candidate_bindings,depth);
        if(candidate_binding_count!=function->type_variable_count)return 0;
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
        return resolve_indexed_local(program,chunk,source,tokens,start,end,
            classes,capacity,is_singleton);
    if(tokens[end].kind==DIAMOND_TOKEN_RIGHT_PAREN)
        return resolve_call(program,chunk,source,tokens,start,end,classes,capacity,is_singleton,depth);
    return 0;
}

size_t receiver_resolve_classes(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,size_t stop_offset,
        size_t *class_indices,size_t max_candidates,bool *is_singleton) {
    if(max_candidates==0)return 0;
    DiamondLexer lexer;diamond_lexer_init(&lexer,source);
    size_t count=0,capacity=32;
    DiamondToken *tokens=malloc(capacity*sizeof *tokens);
    if(tokens==nullptr)return 0;
    while(true) {
        const DiamondToken token=diamond_lexer_next(&lexer);
        if(token.kind==DIAMOND_TOKEN_EOF||token.span.start>=stop_offset)break;
        if(token.kind==DIAMOND_TOKEN_NEWLINE)continue;
        if(count==capacity) {
            capacity*=2;
            DiamondToken *grown=realloc(tokens,capacity*sizeof *tokens);
            if(grown==nullptr) {free(tokens);return 0;}
            tokens=grown;
        }
        tokens[count++]=token;
    }
    size_t dot=count;
    if(count>0&&tokens[count-1].kind==DIAMOND_TOKEN_DOT)dot=count-1;
    else if(count>1&&tokens[count-1].kind==DIAMOND_TOKEN_IDENTIFIER&&
            tokens[count-2].kind==DIAMOND_TOKEN_DOT)dot=count-2;
    if(dot==0||dot==count) {free(tokens);return 0;}
    /* Walk backward to the start of the receiver's current expression. A
     * newline was discarded above, so the last statement boundary is the
     * nearest token that cannot participate in a postfix call chain. */
    size_t start=dot-1,nesting=0,bracket_nesting=0;
    for(size_t index=dot;index>0;index--) {
        const DiamondTokenKind kind=tokens[index-1].kind;
        if(kind==DIAMOND_TOKEN_RIGHT_PAREN)nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_BRACKET)bracket_nesting++;
        else if(kind==DIAMOND_TOKEN_LEFT_BRACKET&&bracket_nesting>0)
            bracket_nesting--;
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
        if(nesting==0&&bracket_nesting==0&&index>1&&
           tokens[index-2].kind!=DIAMOND_TOKEN_DOT&&
           kind!=DIAMOND_TOKEN_RIGHT_PAREN&&kind!=DIAMOND_TOKEN_LEFT_PAREN&&
           kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&kind!=DIAMOND_TOKEN_LEFT_BRACKET&&
           kind!=DIAMOND_TOKEN_DOT)break;
    }
    const size_t result=resolve_expression(program,chunk,source,tokens,start,dot-1,
        class_indices,max_candidates,is_singleton,0);
    free(tokens);return result;
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
