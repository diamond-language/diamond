#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"

#include <inttypes.h>
#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Operator overloading (see docs/syntax.md): dispatches to a user-class
 * method named "+"/"=="/"negate"/etc. when a builtin arithmetic/comparison
 * opcode's operand doesn't otherwise know how to combine with the other
 * one. `argument` is the other operand for a binary operator; nullptr for
 * a unary one (negate), meaning the receiver is the method's only
 * argument. `*found` tells the caller whether a method was located at
 * all -- when false, the caller falls through to its own existing
 * TypeError path unchanged, so this never changes behavior for a class
 * that doesn't define the operator. Uses lookup_method_cached (the same
 * cache INVOKE/INVOKE_MONO use, keyed off `site`) rather than a plain
 * lookup_method, so a hot operator-overload call site gets the same
 * monomorphic-class fast path any other polymorphic call site does, with
 * no new caching mechanism needed. Deliberately does not check
 * method->is_private: `a + b` is operator syntax, not an explicit-receiver
 * method call the way `a.plus(b)` would be -- matches how the to_s
 * dispatch in stringify_value below also ignores privacy. */
DiamondVmStatus invoke_operator_method(DiamondVm *vm,
        const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondInstance *receiver,
        const char *name, size_t name_length, const DiamondValue *arguments,
        size_t explicit_argument_count, DiamondValue *result, bool *found) {
    /* No longer needed for the owner fallback below (vm->root_chunk
     * replaced it, see that field's own comment) -- kept as a parameter
     * so every call site doesn't need updating for what's otherwise an
     * internal implementation detail. */
    (void)chunk;
    const DiamondChunk *owner=receiver->owner!=nullptr?receiver->owner:vm->root_chunk;
    const DiamondMethod *method=lookup_method_cached(vm,owner,site,
        receiver->class,name,name_length,nullptr);
    if(method==nullptr) {*found=false;return DIAMOND_VM_OK;}
    *found=true;
    /* method->arity/required_arity are stored receiver-exclusive (see
     * compile_definition's `function->arity-1` when registering a class
     * method), matching how the real INVOKE site checks its own `argc`
     * (also receiver-exclusive) against them -- only the explicit
     * operands count here, not the receiver. */
    if(explicit_argument_count<method->required_arity||
       (explicit_argument_count>method->arity && !method->has_variadic))
        return DIAMOND_VM_ARITY_ERROR;
    /* Sized for the receiver (1) + every operator this dispatches for
     * today (at most 2 explicit arguments, "[]="'s index+value) + a
     * ClassName.compile_method-installed method's own bound values. */
    DiamondValue args[1+2+DIAMOND_MAX_BOUND_VALUES]={DIAMOND_OBJECT((DiamondObject *)receiver)};
    for(size_t i=0;i<explicit_argument_count;i++)args[1+i]=arguments[i];
    size_t argument_count=1+explicit_argument_count;
    for(size_t i=0;i<method->bound_value_count;i++)
        args[argument_count+i]=method->bound_values[i];
    argument_count+=method->bound_value_count;
    /* See DIAMOND_OP_INVOKE_TYPED's own comment on function_chunk -- a
     * ClassName.compile_method-installed operator method's bytecode
     * lives in its own chunk, not `owner`. */
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    const DiamondChunk child={.name=fn->name,.code=fn->code,
      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
      .constants=fn->constants,.constant_count=fn->constant_count,
      .strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=function_chunk->functions,.function_count=function_chunk->function_count,
      .classes=function_chunk->classes,.class_count=function_chunk->class_count,
      .interfaces=function_chunk->interfaces,.interface_count=function_chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .class_type_variable=fn->class_type_variable,
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    return run_chunk(&child,vm,args,argument_count,depth+1,nullptr,result);
}

/* JIT trampoline for DIAMOND_OP_EQUAL/NOT_EQUAL's general case -- Phase 2e
 * fix, extracted verbatim from that case's own real tail (src/vm.c's
 * EQUAL/NOT_EQUAL case, right after this file's own INT fast path, which
 * the JIT's own compile_equal_op already replicates directly in generated
 * code and never routes through here). The Phase 2d version of this
 * trampoline (diamond_jit_values_equal) called values_equal() directly for
 * every non-fast-path case, silently skipping the "==" override check
 * below for a DIAMOND_OBJECT_INSTANCE operand -- a real, shipped
 * correctness bug (a class defining `def ==` would get identity
 * comparison instead of its own override when compared via JIT'd EQUAL).
 * This version checks the override first, exactly like the interpreter's
 * own case does, and only falls back to values_equal when no override is
 * found -- matching that case's own comment ("two instances of a class
 * with no '==' compare by identity exactly as before this feature
 * existed"). Unlike the old version, this can genuinely invoke arbitrary
 * interpreted code (the override method), so it's status-bearing, not
 * infallible -- compile_equal_op sets jc->has_called = true accordingly. */
DiamondVmStatus diamond_jit_equal_general(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, bool negate, DiamondValue *out) {
    if (left->kind == DIAMOND_VALUE_OBJECT &&
        left->as.object->kind == DIAMOND_OBJECT_INSTANCE) {
        bool found = false;
        DiamondValue op_result = DIAMOND_NIL;
        const DiamondVmStatus status = invoke_operator_method(vm, chunk, depth, site,
            (const DiamondInstance *)left->as.object, "==", 2, right, 1, &op_result, &found);
        if (found) {
            if (status != DIAMOND_VM_OK) return status;
            const bool overloaded_equal = is_truthy(op_result);
            *out = DIAMOND_BOOL(negate ? !overloaded_equal : overloaded_equal);
            return DIAMOND_VM_OK;
        }
    }
    const bool equal = values_equal(*left, *right);
    *out = DIAMOND_BOOL(negate ? !equal : equal);
    return DIAMOND_VM_OK;
}

static bool case_numeric_compare(DiamondValue left,DiamondValue right,int *comparison) {
    if(is_int_value(left)&&is_int_value(right)) {
        DiamondIntView left_view,right_view;
        diamond_int_view(left,&left_view);diamond_int_view(right,&right_view);
        *comparison=diamond_bignum_compare(left_view,right_view);return true;
    }
    return false;
}

DiamondVmStatus case_match_value(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,const uint8_t *site,DiamondValue pattern,DiamondValue subject,
        bool *matched) {
    *matched=false;
    if(pattern.kind==DIAMOND_VALUE_CLASS) {
        if(pattern.as.class_index>=chunk->class_count||
           subject.kind!=DIAMOND_VALUE_OBJECT||
           subject.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return DIAMOND_VM_OK;
        const DiamondInstance *instance=(const DiamondInstance *)subject.as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(owner==nullptr||owner->classes!=chunk->classes)return DIAMOND_VM_OK;
        const DiamondClass *target=chunk->classes[pattern.as.class_index];
        const DiamondClass *current=instance->class;
        while(current!=nullptr) {
            if(current==target) {*matched=true;break;}
            current=current->superclass==DIAMOND_NO_CLASS?nullptr:
                owner->classes[current->superclass];
        }
        return DIAMOND_VM_OK;
    }
    if(pattern.kind==DIAMOND_VALUE_OBJECT&&
       pattern.as.object->kind==DIAMOND_OBJECT_REGEXP) {
        if(subject.kind!=DIAMOND_VALUE_OBJECT||
           subject.as.object->kind!=DIAMOND_OBJECT_STRING)return DIAMOND_VM_OK;
        const reginold_status status=reginold_search(
            ((const DiamondRegexp *)pattern.as.object)->handle,
            ((const DiamondString *)subject.as.object)->chars,
            ((const DiamondString *)subject.as.object)->length,0,nullptr);
        if(status==REGINOLD_ERROR) {
            snprintf(vm->error,sizeof vm->error,"regexp match failed");
            return DIAMOND_VM_REGEXP_ERROR;
        }
        *matched=status==REGINOLD_OK;return DIAMOND_VM_OK;
    }
    if(vm->range_class_index!=DIAMOND_NO_CLASS&&pattern.kind==DIAMOND_VALUE_OBJECT&&
       pattern.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const DiamondInstance *range=(const DiamondInstance *)pattern.as.object;
        if(range->class==chunk->classes[vm->range_class_index]&&range->field_count>=3) {
            int from_start=0,from_end=0;
            if(range->fields[1].kind==DIAMOND_VALUE_NIL) {
                /* Endless: `when 5..` matches anything from 5 up. */
                if(!case_numeric_compare(subject,range->fields[0],&from_start))return DIAMOND_VM_OK;
                *matched=from_start>=0;return DIAMOND_VM_OK;
            }
            if(!case_numeric_compare(subject,range->fields[0],&from_start)||
               !case_numeric_compare(subject,range->fields[1],&from_end))return DIAMOND_VM_OK;
            const bool exclusive=range->fields[2].kind==DIAMOND_VALUE_BOOL&&
                range->fields[2].as.boolean;
            *matched=from_start>=0&&(exclusive?from_end<0:from_end<=0);
            return DIAMOND_VM_OK;
        }
    }
    if(pattern.kind==DIAMOND_VALUE_OBJECT&&
       pattern.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        bool found=false;DiamondValue result=DIAMOND_NIL;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)pattern.as.object,"==",2,&subject,1,
            &result,&found);
        if(found) {if(status!=DIAMOND_VM_OK)return status;*matched=is_truthy(result);return status;}
    }
    *matched=values_equal(pattern,subject);return DIAMOND_VM_OK;
}

/* ClassName#method_missing(name, args) -- called when ordinary instance
 * method dispatch (the main DIAMOND_OP_INVOKE-family site) fails to find
 * `attempted_name` on the receiver's own class. Mirrors
 * invoke_operator_method's own found-or-not shape directly (*found stays
 * false, *result untouched, status DIAMOND_VM_OK, when the class has no
 * method_missing of its own either) so the caller raises the default
 * NoMethodError instead -- there is no risk of this recursing into
 * itself: reaching this function at all already means lookup_method_cached
 * failed to find `attempted_name`, and the lookup_method call here is for
 * the fixed literal name "method_missing", a different lookup entirely.
 * `call_args` points into the caller's own live register file (the
 * original call's explicit arguments, receiver excluded) -- already part
 * of the current frame's GC roots via mark_frame_chain, same as every
 * other dispatch site's own args[] construction. name_symbol is not: it's
 * a brand-new allocation with no register or container to live in until
 * args[] is built below, so it's gc_protect'd across the allocate_array
 * call that follows it (confirmed as a real bug under
 * DIAMOND_STRESS_GC=1, not just theoretical -- same "root it before the
 * next allocation" lesson as populate_default_argv_env's own key_mark).
 *
 * Deliberately scoped to this one dispatch site (ordinary instance method
 * calls) for a first version -- not operator overloading
 * (invoke_operator_method), not #to_s (stringify_value), not super, not
 * self.-singleton dispatch. Each of those already has its own sensible
 * fallback (native operator semantics, a default object representation,
 * a real "no such superclass method" error, a separate method table) that
 * silently redirecting through method_missing would be more likely to
 * surprise than help; a class wanting custom behavior there defines the
 * specific method directly. See docs/design.md. */
DiamondVmStatus method_missing_helper(DiamondVm *vm,const DiamondChunk *owner,
        const DiamondInstance *receiver,const char *attempted_name,size_t attempted_length,
        const DiamondValue *call_args,size_t call_arg_count,size_t depth,
        DiamondValue *result,bool *found) {
    static const char missing_name[]="method_missing";
    const DiamondMethod *method=lookup_method(owner,receiver->class,
        missing_name,sizeof(missing_name)-1);
    if(method==nullptr) {*found=false;return DIAMOND_VM_OK;}
    *found=true;
    if(2<method->required_arity||2>method->arity)
        return DIAMOND_VM_ARITY_ERROR;
    DiamondSymbol *name_symbol=allocate_symbol(vm,attempted_name,attempted_length);
    if(name_symbol==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const size_t name_mark=vm->gc_protected_count;
    if(!gc_protect(vm,DIAMOND_OBJECT(name_symbol)))return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondArray *args_array=allocate_array(vm,call_args,call_arg_count);
    gc_unprotect(vm,name_mark);
    if(args_array==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    DiamondValue args[3]={DIAMOND_OBJECT((DiamondObject *)receiver),
        DIAMOND_OBJECT(name_symbol),DIAMOND_OBJECT(args_array)};
    /* See DIAMOND_OP_INVOKE_TYPED's own comment on function_chunk. */
    const DiamondChunk *function_chunk=
        method->source_chunk!=nullptr?method->source_chunk:owner;
    const DiamondFunction *fn=function_chunk->functions[method->function_index];
    const DiamondChunk child={.name=fn->name,.code=fn->code,
      .lines=fn->lines,.columns=fn->columns,.code_count=fn->code_count,
      .constants=fn->constants,.constant_count=fn->constant_count,
      .strings=fn->strings,.string_count=fn->string_count,
      .type_sets=fn->type_sets,.type_set_count=fn->type_set_count,
      .functions=function_chunk->functions,.function_count=function_chunk->function_count,
      .classes=function_chunk->classes,.class_count=function_chunk->class_count,
      .interfaces=function_chunk->interfaces,.interface_count=function_chunk->interface_count,
      .parameter_type_sets=fn->parameter_type_sets,
      .type_variable_count=fn->type_variable_count,
      .class_type_variable=fn->class_type_variable,
      .parameter_offset=fn->owner_class==DIAMOND_OWNER_NONE?0:1,
      .register_count=fn->register_count,.has_variadic=fn->has_variadic};
    return run_chunk(&child,vm,args,3,depth+1,nullptr,result);
}

/* Shared fallback for `+` once the fast Int/Int path doesn't apply:
 * bignum promotion, mixed Int/Float promotion, String concatenation, and
 * an Instance's own `+` operator-overload method, in that exact order --
 * used by DIAMOND_OP_ADD's own case block (after its Int/Int fast path
 * finds the operands aren't both plain Int) and by ADD_INT's deopt
 * branch (which retargets the opcode back to ADD but, unlike a real
 * fresh dispatch of the now-generic instruction, has to run this same
 * fallback logic on the very instruction that just deopted, rather than
 * relying on ADD's own case block to run next). Previously hand-
 * duplicated at the second call site, and that copy was missing the
 * mixed-Int/Float branch entirely -- a real, previously-undiscovered
 * bug: once a `+` call site had been quickened to ADD_INT from earlier
 * Int+Int calls, a later Int+Float call at the same site raised a
 * spurious TypeError instead of promoting to Float, because only ADD's
 * own block had ever had the Float check. Confirmed with
 * DIAMOND_QUICKEN=1 DIAMOND_QUICKEN_THRESHOLD=1 before this fix. */
/* Array - & | * and String * are written in the prelude: `left OP right`
 * with an Array or String on the left calls diamond_array_op_WORD(left,
 * right) or diamond_string_op_WORD(left, right) when that function exists. Returns false,
 * with nothing called, when it doesn't. */
bool operator_prelude_fallback(DiamondVm *vm,const DiamondChunk *chunk,
        size_t depth,DiamondValue left,DiamondValue right,const char *word,
        DiamondValue *out,DiamondVmStatus *status) {
    if(left.kind!=DIAMOND_VALUE_OBJECT)return false;
    const char *prefix=left.as.object->kind==DIAMOND_OBJECT_ARRAY?"diamond_array_op_":
        left.as.object->kind==DIAMOND_OBJECT_STRING?"diamond_string_op_":nullptr;
    if(prefix==nullptr)return false;
    char bridge[DIAMOND_MAX_FUNCTION_NAME];
    const int written=snprintf(bridge,sizeof bridge,"%s%s",prefix,word);
    if(written<=0||(size_t)written>=sizeof bridge)return false;
    const DiamondFunction *target=find_top_level_function(chunk,bridge,(size_t)written);
    if(target==nullptr)return false;
    const DiamondValue arguments[2]={left,right};
    *status=forward_to_top_level_helper(vm,chunk,target,arguments,0,1,1,depth,out);
    return true;
}

DiamondVmStatus add_fallback(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,
        size_t instruction_offset,DiamondValue left_value,DiamondValue right_value,
        DiamondValue *out_result) {
    if (is_int_value(left_value) && is_int_value(right_value) &&
        (value_is_bignum(left_value) || value_is_bignum(right_value))) {
        DiamondIntView left_view, right_view;
        diamond_int_view(left_value,&left_view);
        diamond_int_view(right_value,&right_view);
        const DiamondValue bignum_result=diamond_bignum_add(vm,left_view,right_view);
        if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=bignum_result;
        return DIAMOND_VM_OK;
    }
    if ((left_value.kind==DIAMOND_VALUE_FLOAT||left_value.kind==DIAMOND_VALUE_INT) &&
        (right_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_INT) &&
        (left_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        /* Mixed Int/Float auto-promotes: the Int side widens to double
         * before the operation (user-confirmed design). */
        const double left_double=left_value.kind==DIAMOND_VALUE_FLOAT?
            left_value.as.real:(double)left_value.as.integer;
        const double right_double=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        *out_result=DIAMOND_FLOAT(left_double+right_double);
        return DIAMOND_VM_OK;
    }
    if (left_value.kind==DIAMOND_VALUE_OBJECT && right_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_STRING &&
        right_value.as.object->kind==DIAMOND_OBJECT_STRING) {
        const DiamondString *left_string=(const DiamondString *)left_value.as.object;
        const DiamondString *right_string=(const DiamondString *)right_value.as.object;
        const size_t length=left_string->length+right_string->length;
        char *chars=malloc(length+1);
        if(chars==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        memcpy(chars,left_string->chars,left_string->length);
        memcpy(chars+left_string->length,right_string->chars,right_string->length);
        DiamondString *string=allocate_string(vm,chars,length);
        free(chars);
        if(string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(string);
        return DIAMOND_VM_OK;
    }
    /* Array + Array: a new Array with left's elements, then right's.
     * Writes both halves straight into the new array's own storage with
     * allocate_array_uninitialized (see its own comment) instead of
     * building a throwaway buffer and letting allocate_array copy that a
     * second time -- this used to copy every element of both arrays
     * twice. */
    if (left_value.kind==DIAMOND_VALUE_OBJECT && right_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_ARRAY &&
        right_value.as.object->kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *left_array=(const DiamondArray *)left_value.as.object;
        const DiamondArray *right_array=(const DiamondArray *)right_value.as.object;
        const size_t count=left_array->count+right_array->count;
        DiamondArray *joined=allocate_array_uninitialized(vm,count);
        if(joined==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(left_array->count>0)
            memcpy(joined->values,left_array->values,
                   left_array->count*sizeof *joined->values);
        if(right_array->count>0)
            memcpy(joined->values+left_array->count,right_array->values,
                   right_array->count*sizeof *joined->values);
        *out_result=DIAMOND_OBJECT(joined);
        return DIAMOND_VM_OK;
    }
    if (left_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        bool found=false;DiamondValue op_result=DIAMOND_NIL;
        const uint8_t *site=chunk->code+instruction_offset;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)left_value.as.object,"+",1,&right_value,1,
            &op_result,&found);
        if(found) {
            if(status!=DIAMOND_VM_OK)return status;
            *out_result=op_result;
            return DIAMOND_VM_OK;
        }
    }
    /* Time + Int|Float -> Time (offset forward, same timezone mode as the
     * receiver); Time + Time is a TypeError, matching Ruby -- there's no
     * branch here for a Time right operand, so it simply falls through
     * to the type-error return below. */
    if (left_value.kind==DIAMOND_VALUE_OBJECT &&
        left_value.as.object->kind==DIAMOND_OBJECT_TIME &&
        (right_value.kind==DIAMOND_VALUE_INT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        const DiamondTime *left_time=(const DiamondTime *)left_value.as.object;
        const double offset=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        DiamondTime *result=allocate_time(vm,left_time->epoch+offset,
            left_time->zone_mode,left_time->utc_offset);
        if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(result);
        return DIAMOND_VM_OK;
    }
    format_operator_type_error(vm,left_value,right_value,"+");
    return DIAMOND_VM_TYPE_ERROR;
}

/* Time vs Time only, matching Ruby -- no Time-vs-numeric ordering.
 * Compares epoch, ignoring each display timezone. Same stack-frame-budget reasoning
 * as time_subtract_fallback above for why this is its own function
 * rather than inline in run_chunk's LESS/LESS_EQUAL/GREATER/GREATER_
 * EQUAL case block, and same reason it writes a ready-to-store
 * DiamondValue into *out_result rather than a bare bool -- the call site
 * needs zero locals of its own this way, just an `if` around the call. */
/* Plain byte-lexicographic ordering (memcmp on the shared prefix, then
 * shorter-is-less on a tie) -- the same ordering C's own strcmp/memcmp
 * and Ruby's String#<=> use. Diamond's String is a raw byte buffer,
 * not UTF-8-validated (see e.g. examples/transformer/lib/tokenizer.d6's
 * own comment on why that property gets relied on elsewhere), so this
 * is a byte comparison, not a locale-aware or codepoint-aware one --
 * matches every other place in this codebase that already treats
 * String as bytes rather than text. */
int diamond_string_compare(const DiamondString *a,const DiamondString *b) {
    const size_t min_length=a->length<b->length?a->length:b->length;
    const int result=min_length==0?0:memcmp(a->chars,b->chars,min_length);
    if(result!=0)return result;
    if(a->length<b->length)return -1;
    if(a->length>b->length)return 1;
    return 0;
}

/* Phase 3 (docs/internal/jit-design.md). Shared slow path for ADD_INT/
 * SUBTRACT_INT/MULTIPLY_INT/DIVIDE_INT and their generic (non-_INT)
 * counterparts -- covers everything the fast, purely-native path in both
 * run_chunk's own case and the JIT (src/jit.c's compile_binary_int_op)
 * can't handle inline: a non-plain-int operand (deopts an _INT opcode
 * back to its generic form first, exactly like this always did, then
 * dispatches through the same fallback logic every generic ADD/SUBTRACT/
 * MULTIPLY/DIVIDE already used) and genuine Int-Int overflow or DIVIDE's
 * own zero/INT64_MIN edge cases, which promote to Diamond's real bignum
 * representation rather than erroring. `opcode` may be any of the seven
 * opcodes this covers (generic or _INT); only ever called once the
 * caller has already ruled out the fast, non-overflowing, both-plain-Int
 * path. Reuses add_fallback for ADD's own generic dispatch (String
 * concat, Instance `+`, Time offset) rather than duplicating it -- the
 * same anti-duplication discipline established in Phase 2e after the
 * EQUAL/INDEX_GET trampolines shipped with their own, subtly incomplete
 * copies of logic that already lived elsewhere. */
DiamondVmStatus int_arith_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, size_t instruction_offset, DiamondOpCode opcode,
        DiamondValue left_value, DiamondValue right_value, DiamondValue *out_result) {
    const bool kind_mismatch=left_value.kind!=DIAMOND_VALUE_INT||
        right_value.kind!=DIAMOND_VALUE_INT;
    DiamondOpCode generic=opcode;
    if(opcode==DIAMOND_OP_ADD_INT)generic=DIAMOND_OP_ADD;
    else if(opcode==DIAMOND_OP_SUBTRACT_INT)generic=DIAMOND_OP_SUBTRACT;
    else if(opcode==DIAMOND_OP_MULTIPLY_INT)generic=DIAMOND_OP_MULTIPLY;
    else if(opcode==DIAMOND_OP_DIVIDE_INT)generic=DIAMOND_OP_DIVIDE;
    if(kind_mismatch&&generic!=opcode) {
        uint8_t *code=(uint8_t *)(void *)chunk->code;
        code[instruction_offset]=(uint8_t)generic;
        vm->deoptimized_sites++;
    }
    if(kind_mismatch) {
        if(generic==DIAMOND_OP_ADD)
            return add_fallback(vm,chunk,depth,instruction_offset,left_value,right_value,out_result);
        if(is_int_value(left_value)&&is_int_value(right_value)&&
           (value_is_bignum(left_value)||value_is_bignum(right_value))) {
            DiamondIntView left_view, right_view;
            diamond_int_view(left_value,&left_view);
            diamond_int_view(right_value,&right_view);
            DiamondValue bignum_result;
            if(generic==DIAMOND_OP_SUBTRACT)
                bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
            else if(generic==DIAMOND_OP_MULTIPLY)
                bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
            else {
                DiamondIntView zero_view;
                diamond_int_view_int64(0,&zero_view);
                if(diamond_bignum_compare(right_view,zero_view)==0)
                    return DIAMOND_VM_DIVISION_BY_ZERO;
                bignum_result=diamond_bignum_divide_truncated(vm,left_view,right_view);
            }
            if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
            *out_result=bignum_result;
            return DIAMOND_VM_OK;
        }
        if((left_value.kind==DIAMOND_VALUE_FLOAT||left_value.kind==DIAMOND_VALUE_INT)&&
           (right_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_INT)&&
           (left_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
            const double left_real=left_value.kind==DIAMOND_VALUE_FLOAT?
                left_value.as.real:(double)left_value.as.integer;
            const double right_real=right_value.kind==DIAMOND_VALUE_FLOAT?
                right_value.as.real:(double)right_value.as.integer;
            double float_result=0;
            if(generic==DIAMOND_OP_SUBTRACT)float_result=left_real-right_real;
            else if(generic==DIAMOND_OP_MULTIPLY)float_result=left_real*right_real;
            else float_result=left_real/right_real;
            *out_result=DIAMOND_FLOAT(float_result);
            return DIAMOND_VM_OK;
        }
        const char *name=generic==DIAMOND_OP_SUBTRACT?"-":
            generic==DIAMOND_OP_MULTIPLY?"*":"/";
        if(left_value.kind==DIAMOND_VALUE_OBJECT&&
           left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
            bool found=false;DiamondValue op_result=DIAMOND_NIL;
            const uint8_t *site=chunk->code+instruction_offset;
            const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
                (const DiamondInstance *)left_value.as.object,name,strlen(name),
                &right_value,1,&op_result,&found);
            if(found) {
                if(status!=DIAMOND_VM_OK)return status;
                *out_result=op_result;
                return DIAMOND_VM_OK;
            }
        }
        if(generic==DIAMOND_OP_SUBTRACT) {
            const DiamondVmStatus time_status=
                time_subtract_fallback(vm,left_value,right_value,out_result);
            if(time_status!=DIAMOND_VM_TYPE_ERROR)return time_status;
        }
        if(generic==DIAMOND_OP_SUBTRACT||generic==DIAMOND_OP_MULTIPLY) {
            DiamondVmStatus prelude_status=DIAMOND_VM_OK;
            if(operator_prelude_fallback(vm,chunk,depth,left_value,right_value,
                   generic==DIAMOND_OP_SUBTRACT?"minus":"times",out_result,&prelude_status))
                return prelude_status;
        }
        format_operator_type_error(vm,left_value,right_value,name);
        return DIAMOND_VM_TYPE_ERROR;
    }
    /* Both operands confirmed plain (non-bignum) Int -- reached only for
     * a genuine overflow (ADD/SUBTRACT/MULTIPLY) or DIVIDE's own zero/
     * INT64_MIN edge cases; the caller already ruled out the fast,
     * non-overflowing native path before calling here. */
    const int64_t left_int=left_value.as.integer;
    const int64_t right_int=right_value.as.integer;
    if(generic==DIAMOND_OP_DIVIDE) {
        if(right_int==0)return DIAMOND_VM_DIVISION_BY_ZERO;
        if(left_int==INT64_MIN&&right_int==-1) {
            /* -INT64_MIN doesn't fit int64_t; promote instead of
             * erroring, matching every other overflow site here. */
            DiamondIntView left_view;
            diamond_int_view(left_value,&left_view);
            const DiamondValue bignum_result=diamond_bignum_negate(vm,left_view);
            if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
            *out_result=bignum_result;
            return DIAMOND_VM_OK;
        }
        *out_result=DIAMOND_INT(left_int/right_int);
        return DIAMOND_VM_OK;
    }
    DiamondIntView left_view, right_view;
    diamond_int_view(left_value,&left_view);
    diamond_int_view(right_value,&right_view);
    DiamondValue bignum_result;
    if(generic==DIAMOND_OP_ADD)bignum_result=diamond_bignum_add(vm,left_view,right_view);
    else if(generic==DIAMOND_OP_SUBTRACT)bignum_result=diamond_bignum_subtract(vm,left_view,right_view);
    else bignum_result=diamond_bignum_multiply(vm,left_view,right_view);
    if(bignum_result.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=bignum_result;
    return DIAMOND_VM_OK;
}

/* Phase 3. Shared slow path for LESS/LESS_EQUAL/GREATER/GREATER_EQUAL and
 * their _INT forms -- mirrors int_arith_slow's own shape exactly, except
 * comparisons never overflow, so the only reason this is ever called at
 * all is a non-plain-int operand (which, for an _INT form, always deopts
 * the opcode back to its generic counterpart first, matching this
 * always did before this extraction). */
DiamondVmStatus compare_int_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, size_t instruction_offset, DiamondOpCode opcode,
        DiamondValue left_value, DiamondValue right_value, DiamondValue *out_result) {
    DiamondOpCode generic=opcode;
    if(opcode==DIAMOND_OP_LESS_INT)generic=DIAMOND_OP_LESS;
    else if(opcode==DIAMOND_OP_LESS_EQUAL_INT)generic=DIAMOND_OP_LESS_EQUAL;
    else if(opcode==DIAMOND_OP_GREATER_INT)generic=DIAMOND_OP_GREATER;
    else if(opcode==DIAMOND_OP_GREATER_EQUAL_INT)generic=DIAMOND_OP_GREATER_EQUAL;
    if(generic!=opcode) {
        uint8_t *code=(uint8_t *)(void *)chunk->code;
        code[instruction_offset]=(uint8_t)generic;
        vm->deoptimized_sites++;
    }
    if(is_int_value(left_value)&&is_int_value(right_value)&&
       (value_is_bignum(left_value)||value_is_bignum(right_value))) {
        DiamondIntView left_view, right_view;
        diamond_int_view(left_value,&left_view);
        diamond_int_view(right_value,&right_view);
        const int comparison=diamond_bignum_compare(left_view,right_view);
        bool bignum_comparison=false;
        if(generic==DIAMOND_OP_LESS)bignum_comparison=comparison<0;
        else if(generic==DIAMOND_OP_LESS_EQUAL)bignum_comparison=comparison<=0;
        else if(generic==DIAMOND_OP_GREATER)bignum_comparison=comparison>0;
        else bignum_comparison=comparison>=0;
        *out_result=DIAMOND_BOOL(bignum_comparison);
        return DIAMOND_VM_OK;
    }
    if((left_value.kind==DIAMOND_VALUE_FLOAT||left_value.kind==DIAMOND_VALUE_INT)&&
       (right_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_INT)&&
       (left_value.kind==DIAMOND_VALUE_FLOAT||right_value.kind==DIAMOND_VALUE_FLOAT)) {
        const double left_real=left_value.kind==DIAMOND_VALUE_FLOAT?
            left_value.as.real:(double)left_value.as.integer;
        const double right_real=right_value.kind==DIAMOND_VALUE_FLOAT?
            right_value.as.real:(double)right_value.as.integer;
        bool float_comparison=false;
        if(generic==DIAMOND_OP_LESS)float_comparison=left_real<right_real;
        else if(generic==DIAMOND_OP_LESS_EQUAL)float_comparison=left_real<=right_real;
        else if(generic==DIAMOND_OP_GREATER)float_comparison=left_real>right_real;
        else float_comparison=left_real>=right_real;
        *out_result=DIAMOND_BOOL(float_comparison);
        return DIAMOND_VM_OK;
    }
    if(left_value.kind==DIAMOND_VALUE_OBJECT&&left_value.as.object->kind==DIAMOND_OBJECT_STRING&&
       right_value.kind==DIAMOND_VALUE_OBJECT&&right_value.as.object->kind==DIAMOND_OBJECT_STRING) {
        const int comparison=diamond_string_compare(
            (const DiamondString *)left_value.as.object,(const DiamondString *)right_value.as.object);
        bool string_comparison=false;
        if(generic==DIAMOND_OP_LESS)string_comparison=comparison<0;
        else if(generic==DIAMOND_OP_LESS_EQUAL)string_comparison=comparison<=0;
        else if(generic==DIAMOND_OP_GREATER)string_comparison=comparison>0;
        else string_comparison=comparison>=0;
        *out_result=DIAMOND_BOOL(string_comparison);
        return DIAMOND_VM_OK;
    }
    if(left_value.kind==DIAMOND_VALUE_OBJECT&&left_value.as.object->kind==DIAMOND_OBJECT_INSTANCE) {
        const char *name=generic==DIAMOND_OP_LESS?"<":
            generic==DIAMOND_OP_LESS_EQUAL?"<=":
            generic==DIAMOND_OP_GREATER?">":">=";
        bool found=false;DiamondValue op_result=DIAMOND_NIL;
        const uint8_t *site=chunk->code+instruction_offset;
        const DiamondVmStatus status=invoke_operator_method(vm,chunk,depth,site,
            (const DiamondInstance *)left_value.as.object,name,strlen(name),
            &right_value,1,&op_result,&found);
        if(found) {
            if(status!=DIAMOND_VM_OK)return status;
            *out_result=DIAMOND_BOOL(is_truthy(op_result));
            return DIAMOND_VM_OK;
        }
    }
    if(time_comparison_fallback(left_value,right_value,generic,out_result))
        return DIAMOND_VM_OK;
    {
        const char *name=generic==DIAMOND_OP_LESS?"<":
            generic==DIAMOND_OP_LESS_EQUAL?"<=":
            generic==DIAMOND_OP_GREATER?">":">=";
        format_operator_type_error(vm,left_value,right_value,name);
    }
    return DIAMOND_VM_TYPE_ERROR;
}

/* Phase 3. JIT trampolines wrapping int_arith_slow/compare_int_slow above
 * for compile_binary_int_op (src/jit.c) -- see that function's own
 * comment for why compiling ADD_INT/SUBTRACT_INT/MULTIPLY_INT/
 * DIVIDE_INT/LESS_INT no longer needs jc->has_called to already be false.
 * `site` is this occurrence's own bytecode address (jc->function->code +
 * instruction_start), the same per-occurrence convention every other
 * overload-checking trampoline here already uses; converted back to an
 * offset via pointer subtraction since the shared helpers key off
 * chunk->code + instruction_offset the same way run_chunk's own case
 * does. */
DiamondVmStatus diamond_jit_arith_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, DiamondOpCode opcode, DiamondValue *out) {
    return int_arith_slow(vm,chunk,depth,(size_t)(site-chunk->code),opcode,*left,*right,out);
}
DiamondVmStatus diamond_jit_compare_slow(DiamondVm *vm, const DiamondChunk *chunk,
        size_t depth, const uint8_t *site, const DiamondValue *left,
        const DiamondValue *right, DiamondOpCode opcode, DiamondValue *out) {
    return compare_int_slow(vm,chunk,depth,(size_t)(site-chunk->code),opcode,*left,*right,out);
}
