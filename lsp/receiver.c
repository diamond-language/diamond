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
        int32_t *known_type_set) {
    *known_type=local->known_type;
    *known_type_set=local->known_type_set;
    size_t latest=0;bool found=false;
    for(size_t index=0;index<owner->scope_type_fact_count;index++) {
        const DiamondScopeTypeFact *fact=&owner->scope_type_facts[index];
        if(fact->reg!=local->reg||fact->effective_start>offset)continue;
        if(!found||fact->effective_start>=latest) {
            found=true;latest=fact->effective_start;
            *known_type=fact->known_type;
            *known_type_set=fact->known_type_set;
        }
    }
}

const DiamondMethod *receiver_lookup_method(const DiamondChunk *chunk,
        size_t class_index,bool is_singleton,const char *name,size_t name_length);

static size_t append_class(size_t *classes,size_t count,size_t capacity,size_t value) {
    for(size_t index=0;index<count;index++)if(classes[index]==value)return count;
    if(count<capacity)classes[count++]=value;
    return count;
}

static size_t function_return_classes(const DiamondChunk *chunk,
        const DiamondFunction *function,size_t *classes,size_t capacity) {
    if(function->return_type_set==UINT8_MAX||
       function->return_type_set>=function->type_set_count)return 0;
    const DiamondTypeSet *set=&function->type_sets[function->return_type_set];
    size_t count=0;
    for(size_t index=0;index<set->count;index++) {
        size_t class_index;
        if(!decode_class_type(chunk,set->members[index].id,&class_index))return 0;
        count=append_class(classes,count,capacity,class_index);
    }
    return count;
}

static size_t resolve_expression(const DiamondProgram *program,const DiamondChunk *chunk,
        const char *source,const DiamondToken *tokens,size_t start,size_t end,
        size_t *classes,size_t capacity,bool *is_singleton,unsigned depth);

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
    local_type_at_offset(owner,local,token.span.start,&known_type,&known_type_set);
    size_t class_index;
    if(decode_class_type(chunk,known_type,&class_index)) {
        classes[0]=class_index;return 1;
    }
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
    if(left==start||tokens[left-1].kind!=DIAMOND_TOKEN_IDENTIFIER)return 0;
    const DiamondToken callee=tokens[left-1];
    const char *name=source+callee.span.start;const size_t name_length=callee.span.length;
    if(left-1==start) {
        for(size_t index=chunk->function_count;index>0;index--) {
            const DiamondFunction *function=chunk->functions[index-1];
            if(function->owner_class==UINT8_MAX&&!function->nested&&
               strlen(function->name)==name_length&&memcmp(function->name,name,name_length)==0) {
                *is_singleton=false;
                return function_return_classes(chunk,function,classes,capacity);
            }
        }
        return 0;
    }
    if(tokens[left-2].kind!=DIAMOND_TOKEN_DOT||left<3)return 0;
    size_t receiver_classes[DIAMOND_MAX_UNION_TYPES];bool receiver_singleton=false;
    const size_t receiver_count=resolve_expression(program,chunk,source,tokens,start,left-3,
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
        size_t returned[DIAMOND_MAX_UNION_TYPES];
        const size_t returned_count=function_return_classes(chunk,
            chunk->functions[method->function_index],returned,DIAMOND_MAX_UNION_TYPES);
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
    size_t start=dot-1,nesting=0;
    for(size_t index=dot;index>0;index--) {
        const DiamondTokenKind kind=tokens[index-1].kind;
        if(kind==DIAMOND_TOKEN_RIGHT_PAREN)nesting++;
        else if(kind==DIAMOND_TOKEN_LEFT_PAREN&&nesting>0)nesting--;
        start=index-1;
        if(nesting==0&&index>1&&tokens[index-2].kind!=DIAMOND_TOKEN_DOT&&
           kind!=DIAMOND_TOKEN_RIGHT_PAREN&&kind!=DIAMOND_TOKEN_LEFT_PAREN&&
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
