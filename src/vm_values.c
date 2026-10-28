#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"

#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Array/Hash: the old implementation staged copied elements in a
 * bare malloc'd buffer (Array) or plain C locals (Hash) before
 * they had any GC root, so a collection triggered by copying one
 * element could free a sibling already copied moments earlier --
 * the same bug this session already found and fixed in
 * regexp_scan_helper/regexp_match_helper, just unreachable there
 * (both had a real destination register to root through
 * immediately). Fixed the same way: allocate the empty container
 * first, protect *it* via gc_protect (there's no destination
 * register here, unlike the regexp helpers), then populate it
 * incrementally via array_push/hash_set so each already-copied
 * element becomes reachable through the now-rooted container
 * before the next one is computed. array_push/hash_set only ever
 * grow their own backing storage via plain realloc, never trigger
 * a GC pass themselves, so there's no unrooted window between a
 * recursive copy_value_into_vm call returning and its result
 * being pushed. */
static bool copy_array_step(DiamondVm *dest_vm, DiamondValue value,
        DiamondProgram *source_program,
        DiamondClass *const *rebase_source_classes,
        DiamondClass *const *rebase_dest_classes,
        const DiamondChunk **adopted_owner,
        DiamondValue *out) {
    const DiamondArray *source=(const DiamondArray *)value.as.object;
    DiamondArray *copy=allocate_array(dest_vm,nullptr,0);
    if(copy==nullptr)return false;
    const size_t mark=dest_vm->gc_protected_count;
    if(!gc_protect(dest_vm,DIAMOND_OBJECT(copy)))return false;
    for(size_t index=0;index<source->count;index++) {
        DiamondValue element=DIAMOND_NIL;
        if(!copy_value_into_vm(dest_vm,source->values[index],
                               source_program,rebase_source_classes,rebase_dest_classes,
                               adopted_owner,&element)||
           !array_push(dest_vm,copy,element)) {
            gc_unprotect(dest_vm,mark);return false;
        }
    }
    gc_unprotect(dest_vm,mark);
    *out=DIAMOND_OBJECT(copy);return true;
}

/* Hash: see copy_array_step for the rooting order. */
static bool copy_hash_step(DiamondVm *dest_vm, DiamondValue value,
        DiamondProgram *source_program,
        DiamondClass *const *rebase_source_classes,
        DiamondClass *const *rebase_dest_classes,
        const DiamondChunk **adopted_owner,
        DiamondValue *out) {
    const DiamondHash *source=(const DiamondHash *)value.as.object;
    DiamondHash *copy=allocate_hash(dest_vm);
    if(copy==nullptr)return false;
    const size_t mark=dest_vm->gc_protected_count;
    if(!gc_protect(dest_vm,DIAMOND_OBJECT(copy)))return false;
    for(size_t index=0;index<source->count;index++) {
        DiamondValue key=DIAMOND_NIL,copied_value=DIAMOND_NIL;
        if(!copy_value_into_vm(dest_vm,source->entries[index].key,
                               source_program,rebase_source_classes,rebase_dest_classes,
                               adopted_owner,&key)) {
            gc_unprotect(dest_vm,mark);return false;
        }
        /* `key` needs its own protection window: it's not
         * reachable through `copy` yet (hash_set hasn't run), and
         * copying the value below can itself trigger a GC. */
        const size_t key_mark=dest_vm->gc_protected_count;
        if(!gc_protect(dest_vm,key)) {
            gc_unprotect(dest_vm,mark);return false;
        }
        if(!copy_value_into_vm(dest_vm,source->entries[index].value,
                               source_program,rebase_source_classes,rebase_dest_classes,
                               adopted_owner,&copied_value)) {
            gc_unprotect(dest_vm,mark);return false;
        }
        gc_unprotect(dest_vm,key_mark);
        if(!hash_set(dest_vm,copy,key,copied_value)) {
            gc_unprotect(dest_vm,mark);return false;
        }
    }
    gc_unprotect(dest_vm,mark);
    *out=DIAMOND_OBJECT(copy);return true;
}

/* Instance: adopted or rebased onto dest_vm's classes, see copy_value_step. */
static bool copy_instance_step(DiamondVm *dest_vm, DiamondValue value,
        DiamondProgram *source_program,
        DiamondClass *const *rebase_source_classes,
        DiamondClass *const *rebase_dest_classes,
        const DiamondChunk **adopted_owner,
        DiamondValue *out) {
    const DiamondInstance *source=(const DiamondInstance *)value.as.object;
    DiamondInstance *copy;
    if(rebase_dest_classes!=nullptr) {
        (void)rebase_source_classes;
        copy=allocate_instance(dest_vm,
            rebase_dest_classes[source->class->index],nullptr);
    } else {
        if(*adopted_owner==nullptr) {
            const DiamondAdoptedProgram *adopted=
                adopt_program(dest_vm,source_program,nullptr,0);
            if(adopted==nullptr)return false;
            *adopted_owner=&adopted->chunk;
        }
        copy=allocate_instance(dest_vm,source->class,*adopted_owner);
    }
    if(copy==nullptr)return false;
    if(source->type_binding!=nullptr)
        *copy->type_binding=*source->type_binding;
    /* Same unrooted-window bug as Array/Hash above, fixed the
     * same way -- `copy` itself is the container to protect;
     * writing straight into copy->fields[index] already puts each
     * field where mark_object's DIAMOND_OBJECT_INSTANCE case will
     * find it, once `copy` is a root. */
    const size_t mark=dest_vm->gc_protected_count;
    if(!gc_protect(dest_vm,DIAMOND_OBJECT(copy)))return false;
    for(size_t index=0;index<source->field_count;index++) {
        if(!copy_value_into_vm(dest_vm,source->fields[index],
                               source_program,rebase_source_classes,rebase_dest_classes,
                               adopted_owner,&copy->fields[index])) {
            gc_unprotect(dest_vm,mark);return false;
        }
        /* `copy` is only reachable through gc_protect (not a real
         * VM register), but that's still enough for a minor
         * collection triggered by a later iteration's own
         * recursive copy to see it as a root and promote it --
         * this raw field write bypasses DIAMOND_OP_SET_IVAR
         * entirely, so it needs its own barrier call each time,
         * same as the other raw-field-write sites. */
        if(!gc_write_barrier(dest_vm,(DiamondObject *)copy)) {
            gc_unprotect(dest_vm,mark);return false;
        }
    }
    /* allocate_instance always starts a fresh instance at
     * class->shapes[0] (nothing "materialized" yet, in the
     * gradual-field-initialization sense GET_IVAR's own inline
     * cache relies on -- see lookup_field_cached/DIAMOND_OP_
     * GET_IVAR) since it has no way to know how many fields this
     * particular caller is about to fill in. Writing `fields[]`
     * directly above (never going through the real SET_IVAR
     * opcode, which is what normally advances an instance's shape
     * one field at a time) leaves that shape stuck at 0 -- every
     * field would read back as a cache-materialized nil despite
     * genuinely holding a copied value, exactly the same class of
     * bug this session's DiamondInstance.owner fix targeted, just
     * one field over. Fixed by rebasing `source`'s own shape the
     * same way its class was rebased/adopted just above: shapes[]
     * is a fixed-size array *inside* DiamondClass (not separately
     * allocated), so the shape index within it is portable the
     * same way a class index is. */
    copy->shape=&copy->class->shapes[
        (size_t)(source->shape-source->class->shapes)];
    gc_unprotect(dest_vm,mark);
    *out=DIAMOND_OBJECT(copy);return true;
}

/* Bignum: a plain limb copy, allocated directly on dest_vm's young list. */
static bool copy_bignum_step(DiamondVm *dest_vm,DiamondValue value,DiamondValue *out) {
    const DiamondBignum *source=(const DiamondBignum *)value.as.object;
    if (!maybe_collect(dest_vm)) return false;
    const size_t size=
        sizeof(DiamondBignum)+source->limb_count*sizeof(uint32_t);
    DiamondBignum *copy=malloc(size);
    if(copy==nullptr)return false;
    copy->object=(DiamondObject){.next=dest_vm->young_objects,
        .kind=DIAMOND_OBJECT_BIGNUM};
    copy->negative=source->negative;copy->limb_count=source->limb_count;
    memcpy(copy->limbs,source->limbs,source->limb_count*sizeof(uint32_t));
    dest_vm->young_objects=&copy->object;dest_vm->bytes_allocated+=size;
    *out=DIAMOND_OBJECT(copy);return true;
}

/* Closure: zero-capture closures in Thread's rebase mode only. */
static bool copy_closure_step(DiamondVm *dest_vm,DiamondValue value,
        DiamondClass *const *rebase_dest_classes,DiamondValue *out) {
    /* Only a *zero-capture* closure, and only in Thread's rebase
     * mode (rebase_dest_classes!=nullptr) -- a capturing closure
     * still holds live Cell/GC state tied to one specific heap,
     * categorically forbidden here exactly as Thread.new's own
     * primary-callable check already requires. In adopt mode
     * (ProgramBuilder#run) the source and destination programs are
     * genuinely different, so a bare function_index wouldn't mean
     * the same function in both -- unlike Thread, whose
     * child_program is a byte-for-byte clone of the whole
     * functions[] table (see clone_program_from_chunk), so a valid
     * index in one is the same function in the other with no
     * offset arithmetic needed at all (unlike the Instance case
     * just above, which rebases a pointer). See docs/threads.md. */
    const DiamondClosure *source=(const DiamondClosure *)value.as.object;
    if(source->capture_count!=0||rebase_dest_classes==nullptr)return false;
    DiamondClosure *copy=
        allocate_closure(dest_vm,source->function_index,nullptr,0);
    if(copy==nullptr)return false;
    copy->is_block=source->is_block;
    *out=DIAMOND_OBJECT(copy);return true;
}

/* A Channel needs neither rebasing nor adoption -- unlike Instance,
 * nothing about it points into source_program's own tables, so
 * this is the one kind both of copy_value_into_vm's modes accept
 * identically (rebase mode: Thread.new/#join and Channel's own
 * send/receive; adopt mode: ProgramBuilder#run). The channel
 * itself is a genuinely shared, refcounted resource (see
 * DiamondChannel's own comment) -- this bumps that refcount and
 * hands dest_vm a fresh handle pointing at the *same* underlying
 * channel, never a copy of its contents. */
static bool copy_channel_step(DiamondVm *dest_vm,DiamondValue value,DiamondValue *out) {
    DiamondChannel *channel=((DiamondChannelHandle *)value.as.object)->channel;
    atomic_fetch_add(&channel->refcount,1);
    DiamondChannelHandle *copy=allocate_channel_handle(dest_vm,channel);
    if(copy==nullptr) {
        atomic_fetch_sub(&channel->refcount,1);
        return false;
    }
    *out=DIAMOND_OBJECT(copy);return true;
}

/* A Regexp owns no shared state, only a compiled pattern: copy it by
 * compiling the same source and options again in dest_vm. */
static bool copy_regexp_step(DiamondVm *dest_vm,DiamondValue value,DiamondValue *out) {
    const DiamondRegexp *source=(const DiamondRegexp *)value.as.object;
    reginold_regex *compiled=nullptr;
    reginold_error compile_error={0};
    if(reginold_compile(source->source,source->source_length,source->options,
            &compiled,&compile_error)!=REGINOLD_OK)return false;
    DiamondRegexp *copy=allocate_regexp_handle(dest_vm,compiled,source->source,
        source->source_length,source->options);
    if(copy==nullptr) {reginold_regex_free(compiled);return false;}
    *out=DIAMOND_OBJECT(copy);return true;
}

/* Deep-copies a DiamondValue rooted in some other VM's heap (typically
 * program_builder_run_helper's temporary run_vm, about to be freed) into
 * dest_vm's own heap, so the result stays valid once the source VM is
 * gone. Object kinds that wrap live VM/OS state rather than plain data
 * (Closure, Fiber, File, Listener, Regexp, ProgramBuilder) aren't safe to
 * hand across this boundary at all -- those return false and the caller
 * reports a TypeError. Composite kinds (Array, Hash, ...) recurse into
 * this same function per element, and deliberately drop the source's own
 * generic constraint metadata -- `constraints[]` holds pointers into the
 * *source* type-set/class tables, whose lifetime isn't tracked across
 * this boundary, so a copy comes back a plain, unconstrained collection
 * rather than trying to carry that metadata across intact.
 *
 * Instance is the one kind that isn't a plain value copy: its ->class
 * points into `source_program`'s own classes[] (superclass chain and
 * method function_index are indices into that *same* program, not
 * portable numbers), so the class itself can't simply be copied without
 * also copying every function/class it can reach -- effectively
 * relinking a whole program. Instead, on the first Instance actually
 * encountered during this copy, `source_program` is adopted into dest_vm
 * (see DiamondVm.adopted_programs, src/vm.h) so it survives for the rest
 * of dest_vm's lifetime, and every copied instance's `owner` field
 * (DiamondInstance, src/object.h) points at the adopted program's own
 * chunk -- dispatch sites read a receiver's own `owner` instead of
 * trusting the ambient chunk specifically so this works (see
 * invoke_operator_method/stringify_value/DIAMOND_OP_INVOKE above).
 * `*adopted_owner` caches that adoption across the whole recursive copy
 * (and across sibling instances in the same Array/Hash) so a single
 * result containing several instances only adopts `source_program` once,
 * not once per instance. Left nullptr (no adoption) for a result that
 * turns out not to contain any Instance at all -- adopting unconditionally
 * would leak the adopted program and all dynamically owned functions on
 * every ProgramBuilder#run call regardless of what it actually returned.
 *
 * `rebase_source_classes`/`rebase_dest_classes` are the alternative to
 * adoption, used by Thread (see docs/threads.md) instead of ProgramBuilder:
 * Thread.new clones the classes/functions/interfaces tables of whatever
 * program is ambient at spawn time (safe -- those tables are themselves
 * pointer-free) for the new thread to run against, rather than adopting a
 * genuinely foreign program. Because the clone's classes[] table has
 * byte-identical layout to the source table, an Instance's `->class`
 * pointer can simply be *rebased* by array-offset arithmetic --
 * `&rebase_dest_classes[source->class-rebase_source_classes]` -- with no
 * adoption and no DiamondVm.adopted_programs involvement; `copy->owner`
 * stays nullptr (the destination's own ambient chunk is already built from
 * the same cloned tables, so the ordinary owner-is-nullptr dispatch
 * fallback already resolves correctly). Both sides are plain `const
 * DiamondClass *` base pointers -- deliberately not `DiamondProgram *`,
 * since at both Thread.new (copying args into a freshly cloned program)
 * and .join() (copying the result back out) exactly one side of the copy
 * is a real, owned DiamondProgram and the other is only ever reachable as
 * a DiamondChunk view's own `.classes` pointer, never as a whole owned
 * program. Rebase mode is active whenever `rebase_dest_classes!=nullptr`;
 * passing it alongside a meaningfully-used `*adopted_owner` is not a
 * supported combination -- exactly one caller mode applies per call. */
static bool copy_value_step(DiamondVm *dest_vm, DiamondValue value,
                               DiamondProgram *source_program,
                               DiamondClass *const *rebase_source_classes,
                               DiamondClass *const *rebase_dest_classes,
                               const DiamondChunk **adopted_owner,
                               DiamondValue *out) {
    if(value.kind!=DIAMOND_VALUE_OBJECT) {*out=value;return true;}
    switch(value.as.object->kind) {
        case DIAMOND_OBJECT_STRING: {
            const DiamondString *source=(const DiamondString *)value.as.object;
            DiamondString *copy=allocate_string(dest_vm,source->chars,source->length);
            if(copy==nullptr)return false;
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_SYMBOL: {
            const DiamondSymbol *source=(const DiamondSymbol *)value.as.object;
            DiamondSymbol *copy=allocate_symbol(dest_vm,source->chars,source->length);
            if(copy==nullptr)return false;
            *out=DIAMOND_OBJECT(copy);return true;
        }
        case DIAMOND_OBJECT_ARRAY:
            return copy_array_step(dest_vm,value,source_program,
                rebase_source_classes,rebase_dest_classes,adopted_owner,out);
        case DIAMOND_OBJECT_HASH:
            return copy_hash_step(dest_vm,value,source_program,
                rebase_source_classes,rebase_dest_classes,adopted_owner,out);
        case DIAMOND_OBJECT_INSTANCE:
            return copy_instance_step(dest_vm,value,source_program,
                rebase_source_classes,rebase_dest_classes,adopted_owner,out);
        case DIAMOND_OBJECT_BIGNUM:
            return copy_bignum_step(dest_vm,value,out);
        case DIAMOND_OBJECT_CLOSURE:
            return copy_closure_step(dest_vm,value,rebase_dest_classes,out);
        case DIAMOND_OBJECT_CHANNEL:
            return copy_channel_step(dest_vm,value,out);
        case DIAMOND_OBJECT_REGEXP:
            return copy_regexp_step(dest_vm,value,out);
        default: return false;
    }
}

/* copy_value_step recurses once per nesting level on the C stack, so a deep
 * enough structure (or a self-containing one, which never ends) used to
 * overflow it and crash the process -- the same class of bug the GC's mark
 * phase had. Copying is bounded rather than made iterative: the Instance
 * and rebasing cases need each parent live (and GC-protected) while its
 * children are copied, which an explicit work stack would have to
 * reproduce. This counts the levels actually in flight on this OS thread
 * and fails the copy past DIAMOND_COPY_MAX_DEPTH, comfortably inside a
 * Thread's 8MB stack even on a sanitizer build's fatter frames. Callers
 * report the failure with copy_failure_reason() so a too-deep value is not
 * blamed on its type. */
enum { DIAMOND_COPY_MAX_DEPTH = 4096 };
static thread_local int copy_depth;
static thread_local bool copy_too_deep;

const char *copy_failure_reason(void) {
    return copy_too_deep?"is nested too deeply to copy (or contains itself)":
                         "does not support this type";
}

bool copy_value_into_vm(DiamondVm *dest_vm, DiamondValue value,
                               DiamondProgram *source_program,
                               DiamondClass *const *rebase_source_classes,
                               DiamondClass *const *rebase_dest_classes,
                               const DiamondChunk **adopted_owner,
                               DiamondValue *out) {
    if(value.kind!=DIAMOND_VALUE_OBJECT) {*out=value;return true;}
    if(copy_depth==0)copy_too_deep=false;
    if(copy_depth>=DIAMOND_COPY_MAX_DEPTH) {copy_too_deep=true;return false;}
    copy_depth++;
    const bool copied=copy_value_step(dest_vm,value,source_program,
        rebase_source_classes,rebase_dest_classes,adopted_owner,out);
    copy_depth--;
    return copied;
}

/* Gives a spawned Thread or Supervisor child the constants defined when
 * Thread.new or add_child ran. The thread never executes the program's top-level
 * statements (that is what sets constants), so without this every
 * module-level constant, even `PAGE_SIZE = 30`, would be unset in it. Each
 * value is deep-copied like a Thread.new argument, so the thread owns its
 * own copy and nothing is shared. A constant whose value cannot cross a
 * thread boundary (a File, a capturing closure, ...) is left unset
 * in the thread, and reading it there raises the usual "uninitialized
 * constant" error. Each copy is stored immediately, where the collector
 * reaches it, so no allocation happens between producing it and rooting it. */
void copy_namespace_constants_into_vm(const DiamondVm *parent, DiamondVm *child,
                                      DiamondClass *const *source_classes,
                                      DiamondClass *const *dest_classes) {
    /* A child that can't get the room just sees these as unset. */
    if(!vm_reserve_namespace_constants(child,parent->namespace_constant_capacity))return;
    for(size_t index=0;index<parent->namespace_constant_capacity;index++) {
        if(!parent->namespace_constant_initialized[index])continue;
        DiamondValue copy=DIAMOND_NIL;
        if(!copy_value_into_vm(child,parent->namespace_constants[index],nullptr,
                source_classes,dest_classes,nullptr,&copy))continue;
        child->namespace_constants[index]=copy;
        child->namespace_constant_initialized[index]=true;
    }
}

bool vm_reserve_namespace_constants(DiamondVm *vm, size_t count) {
    if(count<=vm->namespace_constant_capacity)return true;
    if(count>DIAMOND_MAX_NAMESPACE_CONSTANTS)return false;
    size_t capacity=vm->namespace_constant_capacity<16?16:vm->namespace_constant_capacity*2;
    if(capacity<count)capacity=count;
    if(capacity>DIAMOND_MAX_NAMESPACE_CONSTANTS)capacity=DIAMOND_MAX_NAMESPACE_CONSTANTS;
    DiamondValue *values=realloc(vm->namespace_constants,capacity*sizeof *values);
    if(values==nullptr)return false;
    vm->namespace_constants=values;
    bool *initialized=realloc(vm->namespace_constant_initialized,capacity*sizeof *initialized);
    if(initialized==nullptr)return false;
    vm->namespace_constant_initialized=initialized;
    for(size_t index=vm->namespace_constant_capacity;index<capacity;index++) {
        values[index]=DIAMOND_NIL;
        initialized[index]=false;
    }
    vm->namespace_constant_capacity=capacity;
    return true;
}

bool numeric_as_double(DiamondValue value, double *out) {
    if(value.kind==DIAMOND_VALUE_INT) {*out=(double)value.as.integer;return true;}
    if(value.kind==DIAMOND_VALUE_FLOAT) {*out=value.as.real;return true;}
    if(value_is_bignum(value)) {
        *out=diamond_bignum_to_double((const DiamondBignum *)value.as.object);
        return true;
    }
    return false;
}

/* hash_value_at_depth's recursion is bounded so a self-referential
 * Array/Hash (`a = []; a.push(a)`) can never overflow the C stack: past this
 * depth it stops mixing in deeper structure, which is always valid for a
 * hash (equal values still hash equally, since both truncate identically).
 * Equality may not truncate -- that would call equal deep values unequal --
 * so it has its own scheme: values_equal_at_depth recurses only to
 * DIAMOND_EQUAL_FAST_DEPTH, and anything deeper is redone by
 * values_equal_iterative on an explicit heap stack. */
enum { DIAMOND_STRUCTURAL_MAX_DEPTH = 256, DIAMOND_EQUAL_FAST_DEPTH = 32 };

static bool values_equal_at_depth(DiamondValue left,DiamondValue right,int depth);
static bool values_equal_iterative(DiamondValue left,DiamondValue right);

/* Set by values_equal_at_depth when it gave up at DIAMOND_EQUAL_FAST_DEPTH,
 * meaning its `false` is "too deep to tell", not "unequal". */
static thread_local bool equal_too_deep;
/* hash_find (reached for Hash comparison) calls back into values_equal for
 * key comparison, so Hashes used as keys of Hashes nest values_equal calls on
 * the C stack; bound that the one way it can still grow. */
static thread_local int equal_reentry;

bool values_equal(DiamondValue left, DiamondValue right) {
    const bool saved_too_deep=equal_too_deep;
    equal_too_deep=false;
    bool result=values_equal_at_depth(left,right,0);
    if(!result&&equal_too_deep)result=values_equal_iterative(left,right);
    equal_too_deep=saved_too_deep;
    return result;
}

static bool values_equal_at_depth(DiamondValue left, DiamondValue right, int depth) {
    /* Bignum-aware equality ahead of everything else: a Float, however
     * large, is deliberately never treated as equal to a bignum (the
     * existing Int/Float cross-equality special-case below only
     * handles floats within int64 range, and that's left as-is rather
     * than extended -- see the bignum design notes). Two bignums, or a
     * bignum and a small Int, compare by value either way. */
    if(value_is_bignum(left)||value_is_bignum(right)) {
        if(left.kind==DIAMOND_VALUE_FLOAT||right.kind==DIAMOND_VALUE_FLOAT)return false;
        if(!is_int_value(left)||!is_int_value(right))return false;
        DiamondIntView left_view, right_view;
        diamond_int_view(left,&left_view);
        diamond_int_view(right,&right_view);
        return diamond_bignum_compare(left_view,right_view)==0;
    }
    /* Cross-type numeric equality (3 == 3.0) ahead of the kind guard,
     * per the auto-promotion design: Int widens to double for the
     * comparison. */
    if(left.kind==DIAMOND_VALUE_INT&&right.kind==DIAMOND_VALUE_FLOAT)
        return (double)left.as.integer==right.as.real;
    if(left.kind==DIAMOND_VALUE_FLOAT&&right.kind==DIAMOND_VALUE_INT)
        return left.as.real==(double)right.as.integer;
    if (left.kind != right.kind) {
        return false;
    }
    switch (left.kind) {
        case DIAMOND_VALUE_NIL:
            return true;
        case DIAMOND_VALUE_UNDEFINED:
            return true;
        case DIAMOND_VALUE_BOOL:
            return left.as.boolean == right.as.boolean;
        case DIAMOND_VALUE_INT:
            return left.as.integer == right.as.integer;
        case DIAMOND_VALUE_FLOAT:
            return left.as.real == right.as.real;
        case DIAMOND_VALUE_CLASS:
            return left.as.class_index == right.as.class_index;
        case DIAMOND_VALUE_OBJECT: {
            if (left.as.object->kind != right.as.object->kind) return false;
            /* Instance keeps identity-only equality here (unchanged) --
             * a `==` method override is checked by the DIAMOND_OP_EQUAL
             * opcode handler *before* it ever calls this function at
             * all (see that case's own comment); an Instance reaching
             * this point genuinely has no override and falls back to
             * identity, exactly as before Array/Hash below got their
             * own real structural equality. */
            if(left.as.object->kind==DIAMOND_OBJECT_INSTANCE)
                return left.as.object==right.as.object;
            /* Same object trivially satisfies equality (and, for a
             * self-referential Array/Hash, is the only way this could
             * ever legitimately recurse into itself -- checked first so
             * that case resolves in O(1) instead of by the depth-limit
             * fallback below). */
            if(left.as.object==right.as.object)return true;
            if(left.as.object->kind==DIAMOND_OBJECT_ARRAY) {
                /* Deep or cyclic structures leave the recursion here:
                 * the guard flags "too deep to tell" and values_equal
                 * redoes the comparison iteratively. */
                if(depth>=DIAMOND_EQUAL_FAST_DEPTH){equal_too_deep=true;return false;}
                const DiamondArray *a=(const DiamondArray *)left.as.object;
                const DiamondArray *b=(const DiamondArray *)right.as.object;
                if(a->count!=b->count)return false;
                for(size_t index=0;index<a->count;index++)
                    if(!values_equal_at_depth(a->values[index],b->values[index],depth+1))
                        return false;
                return true;
            }
            if(left.as.object->kind==DIAMOND_OBJECT_HASH) {
                if(depth>=DIAMOND_EQUAL_FAST_DEPTH){equal_too_deep=true;return false;}
                const DiamondHash *a=(const DiamondHash *)left.as.object;
                const DiamondHash *b=(const DiamondHash *)right.as.object;
                if(a->count!=b->count)return false;
                /* Order-independent (a Hash's own equality never cares
                 * about insertion order): every key in `a` must exist
                 * in `b` (hash_find does its own key lookup, keyed by
                 * hash_value + values_equal, exactly the same way any
                 * ordinary Hash#[] lookup already works) with an equal
                 * value; matching counts above plus that containment
                 * check both ways is enough to prove the reverse
                 * direction too (no key in `b` could be left over). */
                for(size_t index=0;index<a->count;index++) {
                    const ptrdiff_t found=hash_find(b,a->entries[index].key);
                    if(found<0)return false;
                    if(!values_equal_at_depth(a->entries[index].value,
                            b->entries[(size_t)found].value,depth+1))
                        return false;
                }
                return true;
            }
            if(left.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *a=(const DiamondSymbol *)left.as.object;
                const DiamondSymbol *b=(const DiamondSymbol *)right.as.object;
                return a->length==b->length&&
                    memcmp(a->chars,b->chars,a->length)==0;
            }
            if(left.as.object->kind==DIAMOND_OBJECT_TIME) {
                /* By value (epoch), not identity -- two separately
                 * constructed Times at the same instant are `==`, same as
                 * Ruby, regardless of each one's own utc/local flag. */
                const DiamondTime *a=(const DiamondTime *)left.as.object;
                const DiamondTime *b=(const DiamondTime *)right.as.object;
                return a->epoch==b->epoch;
            }
            if(left.as.object->kind==DIAMOND_OBJECT_STRING) {
                const DiamondString *a = (const DiamondString *)left.as.object;
                const DiamondString *b = (const DiamondString *)right.as.object;
                return a->length == b->length &&
                       memcmp(a->chars, b->chars, a->length) == 0;
            }
            /* Every other native object kind (Closure, Fiber, File,
             * Listener, Socket, UdpSocket, TlsSocket, Regexp,
             * ProgramBuilder, Thread, SQLite3, SQLite3 Statement,
             * Postgres, MySQL, ProcessResult, and any future addition)
             * has no meaningful value-equality of its own -- compare by
             * identity, the same rule Instance/Array/Hash above already
             * use, instead of falling through to reading String fields
             * (chars/length) off an unrelated struct layout, which was
             * reading garbage and could crash (confirmed: hashing one of
             * these as a Hash key via the identical fallback in
             * hash_value below segfaulted). */
            return left.as.object == right.as.object;
        }
    }
    return false;
}

/* MurmurHash3's fmix64 finalizer (public domain) -- gives scalar/pointer
 * keys good bit distribution across all bits, not just the low bits a
 * power-of-two bucket mask would otherwise expose (a real risk for the
 * common case of small sequential Int keys). */
static uint64_t hash_mix64(uint64_t value) {
    value^=value>>33;value*=0xff51afd7ed558ccdULL;
    value^=value>>33;value*=0xc4ceb9fe1a85ec53ULL;
    value^=value>>33;
    return value;
}

/* Equality on an explicit heap stack, so nesting depth is bounded by memory,
 * not the C stack. Containers push their element pairs; everything else is
 * compared by values_equal_at_depth (no recursion for a non-container).
 * Cycles: past DIAMOND_STRUCTURAL_MAX_DEPTH levels a container pair is
 * remembered, and meeting the same pair again counts as equal -- the
 * standard coinductive reading, so two structures with identical shape that
 * both contain themselves compare equal, as in Ruby. Out of memory reports
 * unequal rather than guessing. */
typedef struct EqualPair {
    DiamondValue left;
    DiamondValue right;
    size_t depth;
} EqualPair;

typedef struct EqualSeen {
    const void **slots; /* left,right object pairs, flattened; nullptr = empty */
    size_t capacity;    /* number of pairs, a power of two (or 0) */
    size_t count;
} EqualSeen;

static size_t equal_seen_slot(const void *left,const void *right,size_t capacity) {
    uintptr_t mixed=(uintptr_t)left*0x9E3779B97F4A7C15u^(uintptr_t)right*0xC2B2AE3D27D4EB4Fu;
    mixed^=mixed>>29;
    return (size_t)mixed&(capacity-1);
}

/* 1 = newly added, 0 = already present, -1 = out of memory */
static int equal_seen_add(EqualSeen *seen,const void *left,const void *right) {
    if((seen->count+1)*2>seen->capacity) {
        const size_t capacity=seen->capacity==0?64:seen->capacity*2;
        const void **slots=calloc(capacity*2,sizeof *slots);
        if(slots==nullptr)return -1;
        for(size_t old=0;old<seen->capacity;old++) {
            if(seen->slots[old*2]==nullptr)continue;
            size_t slot=equal_seen_slot(seen->slots[old*2],seen->slots[old*2+1],capacity);
            while(slots[slot*2]!=nullptr)slot=(slot+1)&(capacity-1);
            slots[slot*2]=seen->slots[old*2];slots[slot*2+1]=seen->slots[old*2+1];
        }
        free(seen->slots);
        seen->slots=slots;seen->capacity=capacity;
    }
    size_t slot=equal_seen_slot(left,right,seen->capacity);
    while(seen->slots[slot*2]!=nullptr) {
        if(seen->slots[slot*2]==left&&seen->slots[slot*2+1]==right)return 0;
        slot=(slot+1)&(seen->capacity-1);
    }
    seen->slots[slot*2]=left;seen->slots[slot*2+1]=right;seen->count++;
    return 1;
}

static bool equal_stack_push(EqualPair **items,size_t *count,size_t *capacity,
                             DiamondValue left,DiamondValue right,size_t depth) {
    if(*count==*capacity) {
        const size_t grown_capacity=*capacity==0?64:*capacity*2;
        EqualPair *grown=realloc(*items,grown_capacity*sizeof **items);
        if(grown==nullptr)return false;
        *items=grown;*capacity=grown_capacity;
    }
    (*items)[(*count)++]=(EqualPair){left,right,depth};
    return true;
}

static bool values_equal_iterative(DiamondValue left,DiamondValue right) {
    if(equal_reentry>=DIAMOND_STRUCTURAL_MAX_DEPTH)return false;
    equal_reentry++;
    EqualPair *stack=nullptr;
    size_t count=0,capacity=0;
    EqualSeen seen={};
    bool equal=equal_stack_push(&stack,&count,&capacity,left,right,0);
    while(equal&&count>0) {
        const EqualPair pair=stack[--count];
        const DiamondValue l=pair.left,r=pair.right;
        const bool containers=l.kind==DIAMOND_VALUE_OBJECT&&r.kind==DIAMOND_VALUE_OBJECT&&
            l.as.object->kind==r.as.object->kind&&l.as.object!=r.as.object&&
            (l.as.object->kind==DIAMOND_OBJECT_ARRAY||l.as.object->kind==DIAMOND_OBJECT_HASH);
        if(!containers) {
            /* Leaves, mismatched kinds, and the same object: no recursion. */
            if(!values_equal_at_depth(l,r,0))equal=false;
            continue;
        }
        if(pair.depth>=DIAMOND_STRUCTURAL_MAX_DEPTH) {
            const int added=equal_seen_add(&seen,l.as.object,r.as.object);
            if(added<0){equal=false;break;}
            if(added==0)continue;
        }
        if(l.as.object->kind==DIAMOND_OBJECT_ARRAY) {
            const DiamondArray *a=(const DiamondArray *)l.as.object;
            const DiamondArray *b=(const DiamondArray *)r.as.object;
            if(a->count!=b->count){equal=false;break;}
            for(size_t index=0;equal&&index<a->count;index++)
                equal=equal_stack_push(&stack,&count,&capacity,
                    a->values[index],b->values[index],pair.depth+1);
        } else {
            const DiamondHash *a=(const DiamondHash *)l.as.object;
            const DiamondHash *b=(const DiamondHash *)r.as.object;
            if(a->count!=b->count){equal=false;break;}
            for(size_t index=0;equal&&index<a->count;index++) {
                const ptrdiff_t found=hash_find(b,a->entries[index].key);
                if(found<0){equal=false;break;}
                equal=equal_stack_push(&stack,&count,&capacity,
                    a->entries[index].value,b->entries[(size_t)found].value,pair.depth+1);
            }
        }
    }
    free(stack);free(seen.slots);
    equal_reentry--;
    return equal;
}

/* FNV-1a over String content -- keys with equal bytes (values_equal's
 * String case, memcmp) must hash equal regardless of which String
 * object holds them. */
static uint64_t hash_bytes(const char *data,size_t length) {
    uint64_t hash=0xcbf29ce484222325ULL;
    for(size_t index=0;index<length;index++) {
        hash^=(unsigned char)data[index];
        hash*=0x100000001b3ULL;
    }
    return hash;
}

/* Must stay consistent with values_equal's exact equality semantics:
 * Int/Bool/Nil by value, String/Symbol by content, Array/Hash by
 * structure (recursively, same as values_equal_at_depth), Instance and
 * everything else by pointer identity. */
static uint64_t hash_value_at_depth(DiamondValue value,int depth);

static uint64_t hash_value(DiamondValue value) {
    return hash_value_at_depth(value,0);
}

static uint64_t hash_value_at_depth(DiamondValue value, int depth) {
    switch(value.kind) {
        case DIAMOND_VALUE_NIL:return hash_mix64(0);
        case DIAMOND_VALUE_UNDEFINED:return hash_mix64(UINT64_MAX);
        case DIAMOND_VALUE_BOOL:return hash_mix64(value.as.boolean?1:2);
        case DIAMOND_VALUE_INT:return hash_mix64((uint64_t)value.as.integer);
        case DIAMOND_VALUE_CLASS:return hash_mix64((uint64_t)value.as.class_index);
        case DIAMOND_VALUE_FLOAT: {
            const double real=value.as.real;
            /* A Float that's exactly equal to some Int64 (e.g. 3.0)
             * must hash the same way that Int64 does, since
             * values_equal treats 3 == 3.0 as true. Bounds-checked
             * before the int64 cast to avoid UB on an out-of-range or
             * non-finite double. */
            if(!isnan(real)&&!isinf(real)&&
               real>=-9223372036854775808.0&&real<9223372036854775808.0&&
               real==(double)(int64_t)real)
                return hash_mix64((uint64_t)(int64_t)real);
            uint64_t bits;
            memcpy(&bits,&real,sizeof bits);
            return hash_mix64(bits);
        }
        case DIAMOND_VALUE_OBJECT: {
            const DiamondObject *object=value.as.object;
            if(object->kind==DIAMOND_OBJECT_INSTANCE)
                return hash_mix64((uint64_t)(uintptr_t)object);
            if(object->kind==DIAMOND_OBJECT_ARRAY) {
                /* Order-sensitive (Array's own equality is): sequential
                 * mixing, each element's hash folded into the running
                 * one via hash_mix64, not combined commutatively --
                 * [1,2] and [2,1] must hash differently, matching that
                 * they're != each other. Depth-limited past
                 * DIAMOND_STRUCTURAL_MAX_DEPTH the same way values_equal_
                 * at_depth's own Array case is (see that one's comment)
                 * -- stops mixing in anything deeper rather than
                 * recursing forever; still a valid (if coarser) hash,
                 * never a correctness problem on its own, since a hash
                 * collision is always allowed and just falls through to
                 * hash_find's own values_equal call to confirm a real
                 * match. */
                const DiamondArray *array=(const DiamondArray *)object;
                uint64_t running=hash_mix64((uint64_t)array->count);
                if(depth<DIAMOND_STRUCTURAL_MAX_DEPTH)
                    for(size_t index=0;index<array->count;index++)
                        running=hash_mix64(running^hash_value_at_depth(array->values[index],depth+1));
                return running;
            }
            if(object->kind==DIAMOND_OBJECT_HASH) {
                /* Order-independent (Hash's own equality is): every
                 * entry's key^value hash is XORed together (commutative
                 * -- {"a":1,"b":2} and {"b":2,"a":1} must hash the same
                 * way, matching that they're == each other), not
                 * sequentially mixed. Depth-limited the same way the
                 * Array case above is. */
                const DiamondHash *hash=(const DiamondHash *)object;
                uint64_t combined=0;
                if(depth<DIAMOND_STRUCTURAL_MAX_DEPTH)
                    for(size_t index=0;index<hash->count;index++) {
                        const uint64_t key_hash=
                            hash_value_at_depth(hash->entries[index].key,depth+1);
                        const uint64_t value_hash=
                            hash_value_at_depth(hash->entries[index].value,depth+1);
                        combined^=hash_mix64(key_hash^hash_mix64(value_hash));
                    }
                return hash_mix64(combined^(uint64_t)hash->count);
            }
            /* No consistency requirement with the small-int hash path
             * above: the canonicalization invariant guarantees a
             * bignum and a small Int can never represent the same
             * value, so there's nothing for their hashes to need to
             * agree with. */
            if(object->kind==DIAMOND_OBJECT_BIGNUM)
                return diamond_bignum_hash((const DiamondBignum *)object);
            if(object->kind==DIAMOND_OBJECT_TIME) {
                /* Must agree with values_equal's by-epoch comparison --
                 * hashed the same bit-pattern way DIAMOND_VALUE_FLOAT is
                 * above, since epoch is itself just a double. */
                const double epoch=((const DiamondTime *)object)->epoch;
                uint64_t bits;
                memcpy(&bits,&epoch,sizeof bits);
                return hash_mix64(bits);
            }
            if(object->kind==DIAMOND_OBJECT_SYMBOL) {
                const DiamondSymbol *symbol=(const DiamondSymbol *)object;
                return hash_bytes(symbol->chars,symbol->length);
            }
            if(object->kind==DIAMOND_OBJECT_STRING) {
                const DiamondString *string=(const DiamondString *)object;
                return hash_bytes(string->chars,string->length);
            }
            /* Every other native object kind (Closure, Fiber, File,
             * Listener, Socket, UdpSocket, TlsSocket, Regexp,
             * ProgramBuilder, Thread, SQLite3, SQLite3 Statement,
             * Postgres, MySQL, ProcessResult, and any future addition)
             * has no meaningful value-equality of its own -- hash by
             * identity, the same rule Instance/Array/Hash above already
             * use, instead of falling through to reading String fields
             * (chars/length) off an unrelated struct layout. This was a
             * real, reproducible segfault: `cache[SQLite3.open(...)] =
             * x` read garbage chars/length off the connection struct
             * and dereferenced them. Must stay consistent with
             * values_equal's own identical fallback above. */
            return hash_mix64((uint64_t)(uintptr_t)object);
        }
    }
    return 0;
}

/* Rebuilds only the bucket index table from entries[]'s already-cached
 * per-entry hash -- entries[] itself is never reordered, which is what
 * keeps insertion order (and "update doesn't move position") intact
 * across any number of rehashes. No tombstones: hash_delete closes the gap
 * in entries[] and rebuilds the buckets, so an empty slot (SIZE_MAX)
 * always safely ends a probe. */
static bool hash_rehash(DiamondVm *vm,DiamondHash *hash,size_t new_capacity) {
    size_t *buckets=malloc(new_capacity*sizeof(size_t));
    if(buckets==nullptr)return false;
    for(size_t index=0;index<new_capacity;index++)buckets[index]=SIZE_MAX;
    for(size_t index=0;index<hash->count;index++) {
        size_t slot=(size_t)(hash->entries[index].hash&(new_capacity-1));
        while(buckets[slot]!=SIZE_MAX)slot=(slot+1)&(new_capacity-1);
        buckets[slot]=index;
    }
    free(hash->buckets);
    if(hash->bucket_capacity>0)
        vm->bytes_allocated-=hash->bucket_capacity*sizeof(size_t);
    hash->buckets=buckets;hash->bucket_capacity=new_capacity;
    vm->bytes_allocated+=new_capacity*sizeof(size_t);
    note_storage_growth(vm);
    return true;
}

ptrdiff_t hash_find(const DiamondHash *hash,DiamondValue key) {
    if(hash->bucket_capacity==0)return -1;
    const uint64_t key_hash=hash_value(key);
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    for(size_t probe=0;probe<hash->bucket_capacity;probe++) {
        const size_t index=hash->buckets[slot];
        if(index==SIZE_MAX)return -1;
        if(hash->entries[index].hash==key_hash&&
           values_equal(hash->entries[index].key,key))
            return (ptrdiff_t)index;
        slot=(slot+1)&(hash->bucket_capacity-1);
    }
    return -1;
}

/* hash_find for a String key given only as bytes, so DIAMOND_OP_INDEX_GET_STRING can look up a
 * literal without allocating it. Must agree with hash_find(hash,<String with these bytes>):
 * the same hash_bytes hash, and values_equal's rule that objects of different kinds are never
 * equal (a Symbol entry with the same bytes does not match a String key). */
ptrdiff_t hash_find_string_bytes(const DiamondHash *hash,const char *chars,size_t length) {
    if(hash->bucket_capacity==0)return -1;
    const uint64_t key_hash=hash_bytes(chars,length);
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    for(size_t probe=0;probe<hash->bucket_capacity;probe++) {
        const size_t index=hash->buckets[slot];
        if(index==SIZE_MAX)return -1;
        const DiamondHashEntry *entry=&hash->entries[index];
        if(entry->hash==key_hash&&entry->key.kind==DIAMOND_VALUE_OBJECT&&
           entry->key.as.object->kind==DIAMOND_OBJECT_STRING) {
            const DiamondString *key=(const DiamondString *)entry->key.as.object;
            if(key->length==length&&memcmp(key->chars,chars,length)==0)
                return (ptrdiff_t)index;
        }
        slot=(slot+1)&(hash->bucket_capacity-1);
    }
    return -1;
}

bool hash_set(DiamondVm *vm,DiamondHash *hash,DiamondValue key,
                     DiamondValue value) {
    const ptrdiff_t existing=hash_find(hash,key);
    if(existing>=0) {
        hash->entries[(size_t)existing].value=value;
        return gc_write_barrier_index(vm,(DiamondObject *)hash,(size_t)existing);
    }
    if(hash->count==hash->capacity) {
        const size_t old_capacity=hash->capacity;
        const size_t capacity=old_capacity<8?8:old_capacity*2;
        DiamondHashEntry *entries=realloc(hash->entries,
            capacity*sizeof(DiamondHashEntry));
        if(entries==nullptr)return false;
        hash->entries=entries;hash->capacity=capacity;
        vm->bytes_allocated+=(capacity-old_capacity)*sizeof(DiamondHashEntry);
        note_storage_growth(vm);
    }
    /* Load factor >= 0.75, checked with integer arithmetic. */
    if(hash->bucket_capacity==0||
       (hash->count+1)*4>=hash->bucket_capacity*3) {
        const size_t new_capacity=
            hash->bucket_capacity==0?8:hash->bucket_capacity*2;
        if(!hash_rehash(vm,hash,new_capacity))return false;
    }
    const uint64_t key_hash=hash_value(key);
    const size_t new_index=hash->count;
    hash->entries[hash->count++]=
        (DiamondHashEntry){.key=key,.value=value,.hash=key_hash};
    size_t slot=(size_t)(key_hash&(hash->bucket_capacity-1));
    while(hash->buckets[slot]!=SIZE_MAX)slot=(slot+1)&(hash->bucket_capacity-1);
    hash->buckets[slot]=new_index;
    return gc_write_barrier_index(vm,(DiamondObject *)hash,new_index);
}

/* Hash#delete: removes `key`'s entry, storing its value in *removed and
 * returning true, or returns false when the key is absent. Later entries
 * shift down one place, so insertion order is kept, and the bucket table is
 * rebuilt at its current capacity since every shifted entry's index
 * changed. O(count), not O(1): a deliberate trade for keeping entries[]
 * dense rather than adding tombstones to every probe and iteration. The
 * shifted entries' GC cards are re-dirtied, since dirty_cards is keyed by
 * entry index and a young value may have moved into a clean card. */
bool hash_delete(DiamondVm *vm,DiamondHash *hash,DiamondValue key,
                        DiamondValue *removed,bool *found) {
    *found=false;
    const ptrdiff_t existing=hash_find(hash,key);
    if(existing<0)return true;
    const size_t index=(size_t)existing;
    *removed=hash->entries[index].value;
    *found=true;
    memmove(&hash->entries[index],&hash->entries[index+1],
            (hash->count-index-1)*sizeof(DiamondHashEntry));
    hash->count--;
    if(!hash_rehash(vm,hash,hash->bucket_capacity))return false;
    return gc_write_barrier_range(vm,(DiamondObject *)hash,index,
                                  hash->count-index);
}

/* Phase 4 (docs/internal/jit-design.md). Shared implementation for
 * DIAMOND_OP_INVOKE's `dup`/`freeze`/`frozen?` pseudo-methods --
 * receiver-kind-agnostic native operations checked before any per-type
 * or Instance method dispatch (see that opcode's own comment on
 * interception order), extracted so the interpreter and the JIT
 * (src/jit.c's compile_body, own DIAMOND_OP_INVOKE case) share one
 * implementation rather than two independently maintained copies. None
 * of the three can ever invoke arbitrary user code (no operator-override
 * equivalent for any of them, unlike EQUAL/INDEX_GET), so compiling one
 * of these never needs jc->has_called = true. `dup_defined` (Array/Hash/
 * String/Symbol, or any non-Object primitive) is folded directly into
 * each function rather than left to a caller to check first: a receiver
 * that doesn't qualify (an Instance, or any other Object kind --
 * Time/Regexp/File/...) returns a plain nonzero DiamondVmStatus, safe to
 * treat as "not handled here, fall back" by any caller that only ever
 * reaches these before jc->has_called could be true (see compile_body's
 * own comment for why that's guaranteed). */
/* Copies `source`'s entries and bucket table into the freshly allocated, still-empty
 * `copy` without hashing or probing anything: each entry already carries its hash, and a
 * copy with identical entry indices and bucket_capacity can reuse the bucket table
 * byte for byte. `copy` is young (allocate_hash just made it and nothing has collected
 * since), so no write barrier or dirty card is needed. entries is sized to exactly
 * `count`; hash_set grows it from there on the first insert. The typed-container
 * constraints are deliberately not copied, as before (docs/internal/jit-design.md's
 * Phase 4). */
static bool hash_clone_contents(DiamondVm *vm,DiamondHash *copy,const DiamondHash *source) {
    if(source->count==0)return true;
    DiamondHashEntry *entries=malloc(source->count*sizeof(DiamondHashEntry));
    if(entries==nullptr)return false;
    size_t *buckets=malloc(source->bucket_capacity*sizeof(size_t));
    if(buckets==nullptr){free(entries);return false;}
    memcpy(entries,source->entries,source->count*sizeof(DiamondHashEntry));
    memcpy(buckets,source->buckets,source->bucket_capacity*sizeof(size_t));
    copy->entries=entries;copy->capacity=source->count;copy->count=source->count;
    copy->buckets=buckets;copy->bucket_capacity=source->bucket_capacity;
    vm->bytes_allocated+=source->count*sizeof(DiamondHashEntry)+
        source->bucket_capacity*sizeof(size_t);
    note_storage_growth(vm);
    return true;
}

DiamondVmStatus diamond_jit_dup(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    if(receiver->kind!=DIAMOND_VALUE_OBJECT) {*out=*receiver;return DIAMOND_VM_OK;}
    const DiamondObjectKind dup_kind=receiver->as.object->kind;
    if(dup_kind==DIAMOND_OBJECT_STRING||dup_kind==DIAMOND_OBJECT_SYMBOL) {
        /* Both are immutable in this VM (every String/Symbol-producing
         * operation returns a new object rather than mutating in place),
         * so a distinct copy would be observably identical -- returning
         * the same object is correct, not just an optimization. */
        *out=*receiver;return DIAMOND_VM_OK;
    }
    if(dup_kind==DIAMOND_OBJECT_ARRAY) {
        const DiamondArray *source=(const DiamondArray *)receiver->as.object;
        DiamondArray *copy=allocate_array(vm,source->values,source->count);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        /* constraints[]/constraint_count deliberately NOT copied -- see
         * this same rationale preserved from the interpreter's own former
         * inline copy of this logic (docs/internal/jit-design.md's
         * Phase 4). */
        *out=DIAMOND_OBJECT(copy);return DIAMOND_VM_OK;
    }
    if(dup_kind==DIAMOND_OBJECT_HASH) {
        const DiamondHash *source=(const DiamondHash *)receiver->as.object;
        DiamondHash *copy=allocate_hash(vm);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(!hash_clone_contents(vm,copy,source))return DIAMOND_VM_OUT_OF_MEMORY;
        *out=DIAMOND_OBJECT(copy);return DIAMOND_VM_OK;
    }
    if(dup_kind==DIAMOND_OBJECT_INSTANCE) {
        /* Phase 6: an Instance's own `dup`/`freeze`/`frozen?` (src/vm.c's
         * real INVOKE case) is only ever intercepted here when its class
         * does NOT define its own same-named method -- lookup_method is a
         * pure, deterministic table lookup, never arbitrary code, so
         * folding that same check into this trampoline doesn't need
         * jc->has_called (already true unconditionally for other reasons)
         * to mean anything new. A class that *does* override dup/freeze/
         * frozen? still correctly falls back to full interpretation (the
         * "not dup_defined" return below), which then reaches real
         * Instance method dispatch -- this trampoline never attempts
         * that itself. */
        const DiamondInstance *instance=(const DiamondInstance *)receiver->as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(lookup_method(owner,instance->class,"dup",3)!=nullptr)
            return DIAMOND_VM_TYPE_ERROR; /* real override -- caller must fall back */
        DiamondInstance *copy=allocate_instance(vm,instance->class,instance->owner);
        if(copy==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        if(instance->type_binding!=nullptr)
            *copy->type_binding=*instance->type_binding;
        for(size_t index=0;index<instance->field_count;index++)
            copy->fields[index]=instance->fields[index];
        copy->shape=instance->shape;
        *out=DIAMOND_OBJECT(copy);return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR; /* not dup_defined -- caller must fall back */
}

DiamondVmStatus diamond_jit_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    (void)vm; /* unused -- kept for a calling convention uniform with diamond_jit_dup */
    if(receiver->kind!=DIAMOND_VALUE_OBJECT) {*out=*receiver;return DIAMOND_VM_OK;}
    const DiamondObjectKind freeze_kind=receiver->as.object->kind;
    if(freeze_kind==DIAMOND_OBJECT_STRING||freeze_kind==DIAMOND_OBJECT_SYMBOL) {
        *out=*receiver;return DIAMOND_VM_OK;
    }
    if(freeze_kind==DIAMOND_OBJECT_ARRAY||freeze_kind==DIAMOND_OBJECT_HASH) {
        receiver->as.object->frozen=true;
        *out=*receiver;return DIAMOND_VM_OK;
    }
    if(freeze_kind==DIAMOND_OBJECT_INSTANCE) {
        /* Phase 6: see diamond_jit_dup's own comment on why folding the
         * lookup_method check in here is safe. */
        const DiamondInstance *instance=(const DiamondInstance *)receiver->as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(lookup_method(owner,instance->class,"freeze",6)!=nullptr)
            return DIAMOND_VM_TYPE_ERROR; /* real override -- caller must fall back */
        receiver->as.object->frozen=true;
        *out=*receiver;return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR; /* not dup_defined -- caller must fall back */
}

/* `deep_freeze`: freezes the receiver and, transitively, every Array
 * element, Hash key/value, and Instance field reachable from it. Native
 * and shallow-by-design siblings (`freeze`) stay as they are; this is a
 * separate universal method so a class's own `freeze` is never bypassed
 * for the receiver itself, but nested instances are frozen directly,
 * without calling any user-defined `freeze` on them -- a deep freeze is a
 * guarantee about the graph, not a request to run arbitrary user code
 * mid-walk. Cycles are handled with an explicit visited set (pointer-keyed,
 * open addressing) and an explicit work stack, so neither a cyclic graph
 * nor a very deep one can recurse the C stack. Only Array/Hash/Instance
 * carry a frozen flag; every other object kind is skipped (immutable
 * String/Symbol, or native resources with no guarded mutation). Never
 * allocates on the VM heap, so no GC can run mid-walk. */
typedef struct DeepFreezeWalk {
    const DiamondObject **visited;
    size_t visited_capacity;
    size_t visited_count;
    DiamondObject **stack;
    size_t stack_count;
    size_t stack_capacity;
} DeepFreezeWalk;

static size_t deep_freeze_slot(const DiamondObject *object,size_t capacity) {
    return (size_t)(((uintptr_t)object>>4)*(uintptr_t)11400714819323198485ull)&(capacity-1);
}

/* Returns 1 if newly queued, 0 if already seen or not freezable, -1 on OOM. */
static int deep_freeze_visit(DeepFreezeWalk *walk,DiamondValue value) {
    if(value.kind!=DIAMOND_VALUE_OBJECT||value.as.object==nullptr)return 0;
    DiamondObject *object=value.as.object;
    if(object->kind!=DIAMOND_OBJECT_ARRAY&&object->kind!=DIAMOND_OBJECT_HASH&&
       object->kind!=DIAMOND_OBJECT_INSTANCE)return 0;
    if(walk->visited_count*2>=walk->visited_capacity) {
        const size_t new_capacity=walk->visited_capacity==0?64:walk->visited_capacity*2;
        const DiamondObject **grown=calloc(new_capacity,sizeof *grown);
        if(grown==nullptr)return -1;
        for(size_t index=0;index<walk->visited_capacity;index++) {
            const DiamondObject *entry=walk->visited[index];
            if(entry==nullptr)continue;
            size_t slot=deep_freeze_slot(entry,new_capacity);
            while(grown[slot]!=nullptr)slot=(slot+1)&(new_capacity-1);
            grown[slot]=entry;
        }
        free(walk->visited);
        walk->visited=grown;walk->visited_capacity=new_capacity;
    }
    size_t slot=deep_freeze_slot(object,walk->visited_capacity);
    while(walk->visited[slot]!=nullptr) {
        if(walk->visited[slot]==object)return 0;
        slot=(slot+1)&(walk->visited_capacity-1);
    }
    walk->visited[slot]=object;walk->visited_count++;
    if(walk->stack_count==walk->stack_capacity) {
        const size_t new_capacity=walk->stack_capacity==0?64:walk->stack_capacity*2;
        DiamondObject **grown=realloc(walk->stack,new_capacity*sizeof *grown);
        if(grown==nullptr)return -1;
        walk->stack=grown;walk->stack_capacity=new_capacity;
    }
    walk->stack[walk->stack_count++]=object;
    return 1;
}

DiamondVmStatus diamond_deep_freeze(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    (void)vm;
    DeepFreezeWalk walk={};
    DiamondVmStatus status=DIAMOND_VM_OK;
    if(deep_freeze_visit(&walk,*receiver)<0)status=DIAMOND_VM_OUT_OF_MEMORY;
    while(status==DIAMOND_VM_OK&&walk.stack_count>0) {
        DiamondObject *object=walk.stack[--walk.stack_count];
        object->frozen=true;
        if(object->kind==DIAMOND_OBJECT_ARRAY) {
            const DiamondArray *array=(const DiamondArray *)object;
            for(size_t index=0;index<array->count&&status==DIAMOND_VM_OK;index++)
                if(deep_freeze_visit(&walk,array->values[index])<0)
                    status=DIAMOND_VM_OUT_OF_MEMORY;
        } else if(object->kind==DIAMOND_OBJECT_HASH) {
            const DiamondHash *hash=(const DiamondHash *)object;
            for(size_t index=0;index<hash->count&&status==DIAMOND_VM_OK;index++)
                if(deep_freeze_visit(&walk,hash->entries[index].key)<0||
                   deep_freeze_visit(&walk,hash->entries[index].value)<0)
                    status=DIAMOND_VM_OUT_OF_MEMORY;
        } else {
            const DiamondInstance *instance=(const DiamondInstance *)object;
            for(size_t index=0;index<instance->field_count&&status==DIAMOND_VM_OK;index++)
                if(deep_freeze_visit(&walk,instance->fields[index])<0)
                    status=DIAMOND_VM_OUT_OF_MEMORY;
        }
    }
    free(walk.visited);free(walk.stack);
    if(status==DIAMOND_VM_OK)*out=*receiver;
    return status;
}

DiamondVmStatus diamond_jit_frozen(DiamondVm *vm, const DiamondValue *receiver,
        DiamondValue *out) {
    if(receiver->kind!=DIAMOND_VALUE_OBJECT) {*out=DIAMOND_BOOL(true);return DIAMOND_VM_OK;}
    const DiamondObjectKind kind=receiver->as.object->kind;
    if(kind==DIAMOND_OBJECT_STRING||kind==DIAMOND_OBJECT_SYMBOL) {
        *out=DIAMOND_BOOL(true);return DIAMOND_VM_OK;
    }
    if(kind==DIAMOND_OBJECT_ARRAY||kind==DIAMOND_OBJECT_HASH) {
        *out=DIAMOND_BOOL(receiver->as.object->frozen);return DIAMOND_VM_OK;
    }
    if(kind==DIAMOND_OBJECT_INSTANCE) {
        /* Phase 6: see diamond_jit_dup's own comment on why folding the
         * lookup_method check in here is safe. */
        const DiamondInstance *instance=(const DiamondInstance *)receiver->as.object;
        const DiamondChunk *owner=instance->owner!=nullptr?instance->owner:vm->root_chunk;
        if(lookup_method(owner,instance->class,"frozen?",7)!=nullptr)
            return DIAMOND_VM_TYPE_ERROR; /* real override -- caller must fall back */
        *out=DIAMOND_BOOL(receiver->as.object->frozen);return DIAMOND_VM_OK;
    }
    return DIAMOND_VM_TYPE_ERROR; /* not dup_defined -- caller must fall back */
}
