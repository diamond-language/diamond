#include "receiver.h"

#include "lexer.h"

#include <stdint.h>
#include <string.h>

typedef enum ReceiverKind {
    RECEIVER_NONE,
    RECEIVER_SELF,
    /* Either a literal class name or a local variable -- classify_receiver
     * can't tell which without consulting chunk->classes, so resolution
     * disambiguates once it has the receiver's own text. */
    RECEIVER_NAME,
} ReceiverKind;

typedef struct ReceiverContext {
    ReceiverKind kind;
    /* Valid only for RECEIVER_SELF: the `self` token's own byte offset,
     * used to find which function's body it's lexically inside. */
    size_t self_offset;
    /* Valid only for RECEIVER_NAME: the receiver identifier's own span. */
    DiamondSpan name_span;
} ReceiverContext;

/* Re-lexes `source` from its start up to (not including) `stop_offset`,
 * keeping the last three tokens seen, to classify what immediately
 * precedes that offset as a receiver expression. Re-lexing rather than
 * scanning backward directly: DiamondLexer (src/lexer.h) only exposes
 * diamond_lexer_init/diamond_lexer_next, no reverse-direction API.
 *
 * Two shapes are recognized, covering both "cursor sits on the method
 * name itself" (hover/definition, stop_offset == that identifier's own
 * span.start) and "cursor sits right after `receiver.`, method name
 * partially typed or not yet typed at all" (completion, stop_offset ==
 * the raw cursor offset):
 *   ... DOT <stop>                  -- `receiver.|` or `receiver.<name>|`
 *       (only reachable when stop_offset lands exactly on the dot's own
 *       end, i.e. hover/definition's identifier-span case)
 *   ... DOT IDENTIFIER <stop>       -- `receiver.partial|` (completion,
 *       stop_offset lands after the partial method name)
 * Anything else (no DOT at all, a chained call's `)` before the DOT, an
 * instance-variable receiver -- lexed as its own distinct token kind,
 * never IDENTIFIER) yields RECEIVER_NONE. */
static ReceiverContext classify_receiver(const char *source,size_t stop_offset) {
    DiamondLexer lexer;
    diamond_lexer_init(&lexer,source);
    DiamondToken history[3]={
        {.kind=DIAMOND_TOKEN_EOF},{.kind=DIAMOND_TOKEN_EOF},{.kind=DIAMOND_TOKEN_EOF}};
    while(true) {
        const DiamondToken current=diamond_lexer_next(&lexer);
        if(current.kind==DIAMOND_TOKEN_EOF||current.span.start>=stop_offset)break;
        history[0]=history[1];history[1]=history[2];history[2]=current;
    }
    const DiamondToken last=history[2];
    const DiamondToken second_last=history[1];
    const DiamondToken third_last=history[0];
    DiamondToken receiver_token={.kind=DIAMOND_TOKEN_EOF};
    if(last.kind==DIAMOND_TOKEN_DOT) {
        receiver_token=second_last;
    } else if(last.kind==DIAMOND_TOKEN_IDENTIFIER&&second_last.kind==DIAMOND_TOKEN_DOT) {
        receiver_token=third_last;
    } else {
        return (ReceiverContext){.kind=RECEIVER_NONE};
    }
    if(receiver_token.kind==DIAMOND_TOKEN_SELF)
        return (ReceiverContext){.kind=RECEIVER_SELF,.self_offset=receiver_token.span.start};
    if(receiver_token.kind==DIAMOND_TOKEN_IDENTIFIER)
        return (ReceiverContext){.kind=RECEIVER_NAME,.name_span=receiver_token.span};
    return (ReceiverContext){.kind=RECEIVER_NONE};
}

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
        const DiamondScopeLocal **best,size_t *best_end) {
    for(size_t index=0;index<candidate->scope_local_count;index++) {
        const DiamondScopeLocal *local=&candidate->scope_locals[index];
        if(offset<local->valid_start||offset>=local->valid_end)continue;
        if(strlen(local->name)!=name_length||memcmp(local->name,name,name_length)!=0)continue;
        if(local->valid_end<*best_end) {*best_end=local->valid_end;*best=local;}
    }
}

static const DiamondScopeLocal *find_scope_local(
        const DiamondProgram *program,const DiamondChunk *chunk,
        const char *name,size_t name_length,size_t offset) {
    const DiamondScopeLocal *best=nullptr;
    size_t best_end=SIZE_MAX;
    consider_local_candidate(&program->entry,name,name_length,offset,&best,&best_end);
    for(size_t index=0;index<chunk->function_count;index++)
        consider_local_candidate(chunk->functions[index],name,name_length,offset,&best,&best_end);
    return best;
}

bool receiver_resolve_class(const DiamondProgram *program,
        const DiamondChunk *chunk,const char *source,size_t stop_offset,
        size_t *class_index,bool *is_singleton) {
    const ReceiverContext context=classify_receiver(source,stop_offset);
    if(context.kind==RECEIVER_SELF) {
        const DiamondFunction *enclosing=find_enclosing_function(program,chunk,context.self_offset);
        if(enclosing==nullptr||enclosing->owner_class==UINT8_MAX)return false;
        *class_index=enclosing->owner_class;
        *is_singleton=function_is_singleton_of(chunk,&chunk->classes[enclosing->owner_class],enclosing);
        return true;
    }
    if(context.kind==RECEIVER_NAME) {
        char name[DIAMOND_MAX_FUNCTION_NAME];
        size_t length=context.name_span.length;
        if(length>=sizeof name)length=sizeof name-1;
        memcpy(name,source+context.name_span.start,length);
        name[length]='\0';
        for(size_t index=0;index<chunk->class_count;index++) {
            if(strcmp(chunk->classes[index].name,name)==0) {
                *class_index=index;*is_singleton=true;
                return true;
            }
        }
        const DiamondScopeLocal *local=
            find_scope_local(program,chunk,name,length,context.name_span.start);
        if(local==nullptr)return false;
        if(local->known_type<DIAMOND_TYPE_CLASS_BASE||
           local->known_type>=DIAMOND_TYPE_VARIABLE_BASE)
            return false;
        const size_t index=(size_t)(local->known_type-DIAMOND_TYPE_CLASS_BASE);
        if(index>=chunk->class_count)return false;
        *class_index=index;*is_singleton=false;
        return true;
    }
    return false;
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
