#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"

#include <inttypes.h>
#include <math.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* value.respond_to?(name) on a String, Array, Hash, Int or Float: true for a native method
 * (the same tables the editor completes from), for a prelude or program extension function
 * (string_x, array_x, integer_x, ...) and for the universal respond_to? itself. The argument
 * is a Symbol or a String, as for the other universal methods. */
static DiamondVmStatus builtin_respond_to(const DiamondChunk *chunk,DiamondValue receiver,
        DiamondValue name_value,uint8_t argc,DiamondValue *out) {
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    const char *name=nullptr;size_t length=0;
    if(name_value.kind==DIAMOND_VALUE_OBJECT&&
       name_value.as.object->kind==DIAMOND_OBJECT_SYMBOL) {
        name=((const DiamondSymbol *)name_value.as.object)->chars;
        length=((const DiamondSymbol *)name_value.as.object)->length;
    } else if(name_value.kind==DIAMOND_VALUE_OBJECT&&
              name_value.as.object->kind==DIAMOND_OBJECT_STRING) {
        name=((const DiamondString *)name_value.as.object)->chars;
        length=((const DiamondString *)name_value.as.object)->length;
    } else return DIAMOND_VM_TYPE_ERROR;
    bool found=length==11&&memcmp(name,"respond_to?",11)==0;
    uint16_t type=0;
    const DiamondFunction *extension=nullptr;
    if(receiver.kind==DIAMOND_VALUE_INT)type=DIAMOND_TYPE_INT;
    else if(receiver.kind==DIAMOND_VALUE_FLOAT)type=DIAMOND_TYPE_FLOAT;
    else if(receiver.kind==DIAMOND_VALUE_OBJECT) {
        switch(receiver.as.object->kind) {
            case DIAMOND_OBJECT_STRING: type=DIAMOND_TYPE_STRING;break;
            case DIAMOND_OBJECT_ARRAY: type=DIAMOND_TYPE_ARRAY;break;
            case DIAMOND_OBJECT_HASH: type=DIAMOND_TYPE_HASH;break;
            default: break;
        }
    }
    const char *const *natives=nullptr;
    const size_t native_count=diamond_native_member_names(type,&natives);
    for(size_t index=0;!found&&index<native_count;index++)
        found=strlen(natives[index])==length&&memcmp(natives[index],name,length)==0;
    if(!found&&type==DIAMOND_TYPE_ARRAY)
        extension=find_collection_extension(chunk,DIAMOND_OBJECT_ARRAY,name,length);
    else if(!found&&type==DIAMOND_TYPE_HASH)
        extension=find_collection_extension(chunk,DIAMOND_OBJECT_HASH,name,length);
    else if(!found&&type==DIAMOND_TYPE_STRING) {
        static const char *const prefixes[]={"string_"};
        extension=find_value_extension(chunk,prefixes,1,name,length);
    } else if(!found&&type==DIAMOND_TYPE_INT) {
        static const char *const prefixes[]={"integer_","numeric_"};
        extension=find_value_extension(chunk,prefixes,2,name,length);
    } else if(!found&&type==DIAMOND_TYPE_FLOAT) {
        static const char *const prefixes[]={"float_","numeric_"};
        extension=find_value_extension(chunk,prefixes,2,name,length);
    }
    *out=DIAMOND_BOOL(found||extension!=nullptr);
    return DIAMOND_VM_OK;
}

/* Array/Hash methods implemented by a prelude function: the method name,
 * then the function for an Array receiver and for a Hash receiver (nullptr
 * when the method does not exist on that kind). A Hash's select/count/any?/
 * all?/map pass a two-parameter block (key, value); the hash_ versions
 * check the block's arity. count() on either takes no block at all. */
#define COLLECTION_BRIDGE(name,array_target,hash_target) \
    {name,sizeof name-1,array_target,hash_target}
static const struct {
    const char *name; size_t length;
    const char *array_target; const char *hash_target;
} collection_bridges[]={
    COLLECTION_BRIDGE("each","array_each","hash_each"),
    COLLECTION_BRIDGE("lazy","enumerable_lazy",nullptr),
    COLLECTION_BRIDGE("select","enumerable_select","diamond_hash_select"),
    COLLECTION_BRIDGE("count","diamond_array_count","diamond_hash_count"),
    COLLECTION_BRIDGE("any?","enumerable_any","diamond_hash_any"),
    COLLECTION_BRIDGE("all?","enumerable_all","diamond_hash_all"),
    COLLECTION_BRIDGE("reduce","enumerable_reduce","enumerable_reduce"),
    COLLECTION_BRIDGE("map","enumerable_map","diamond_hash_map"),
    COLLECTION_BRIDGE("sum","array_sum",nullptr),
    COLLECTION_BRIDGE("reject","array_reject",nullptr),
    COLLECTION_BRIDGE("find","array_find",nullptr),
    COLLECTION_BRIDGE("each_with_index","array_each_with_index",nullptr),
    COLLECTION_BRIDGE("sort","enumerable_sort",nullptr),
    COLLECTION_BRIDGE("sort_by","enumerable_sort_by",nullptr),
    COLLECTION_BRIDGE("min","enumerable_min",nullptr),
    COLLECTION_BRIDGE("max","enumerable_max",nullptr),
    COLLECTION_BRIDGE("min_by","array_min_by",nullptr),
    COLLECTION_BRIDGE("max_by","array_max_by",nullptr),
    COLLECTION_BRIDGE("take","array_take",nullptr),
    COLLECTION_BRIDGE("drop","array_drop",nullptr),
    COLLECTION_BRIDGE("flat_map","array_flat_map",nullptr),
    COLLECTION_BRIDGE("partition","array_partition",nullptr),
    COLLECTION_BRIDGE("group_by","array_group_by",nullptr),
    COLLECTION_BRIDGE("zip","array_zip",nullptr),
    COLLECTION_BRIDGE("each_slice","array_each_slice",nullptr),
    COLLECTION_BRIDGE("each_cons","array_each_cons",nullptr),
    COLLECTION_BRIDGE("tally","array_tally",nullptr),
    COLLECTION_BRIDGE("first","array_first",nullptr),
    COLLECTION_BRIDGE("first_or","array_first_or",nullptr),
    COLLECTION_BRIDGE("last","array_last",nullptr),
    COLLECTION_BRIDGE("last_or","array_last_or",nullptr),
    COLLECTION_BRIDGE("include?","array_include",nullptr),
    COLLECTION_BRIDGE("reverse","array_reverse",nullptr),
    COLLECTION_BRIDGE("concat","array_concat",nullptr),
    COLLECTION_BRIDGE("compact","array_compact",nullptr),
    COLLECTION_BRIDGE("uniq","array_uniq",nullptr),
    COLLECTION_BRIDGE("flatten","array_flatten",nullptr),
    COLLECTION_BRIDGE("delete_at","array_delete_at",nullptr),
    COLLECTION_BRIDGE("empty?","array_empty","hash_empty"),
    COLLECTION_BRIDGE("fetch",nullptr,"hash_fetch"),
    COLLECTION_BRIDGE("keys",nullptr,"hash_keys"),
    COLLECTION_BRIDGE("values",nullptr,"hash_values"),
    COLLECTION_BRIDGE("map_values",nullptr,"hash_map_values"),
    COLLECTION_BRIDGE("merge",nullptr,"hash_merge"),
};
#undef COLLECTION_BRIDGE

/* The prelude function behind an Array/Hash method, or nullptr when the
 * method has none for this receiver kind. */
static const char *collection_bridge_target(DiamondObjectKind receiver_kind,
        const DiamondStringConstant *method_name) {
    for(size_t index=0;index<sizeof collection_bridges/sizeof collection_bridges[0];index++)
        /* The first-character test skips most memcmp calls: eleven names
         * share length 4, and this runs on every bridged call. */
        if(method_name->length==collection_bridges[index].length&&
           method_name->chars[0]==collection_bridges[index].name[0]&&
           memcmp(method_name->chars,collection_bridges[index].name,
               collection_bridges[index].length)==0)
            return receiver_kind==DIAMOND_OBJECT_ARRAY?
                collection_bridges[index].array_target:
                collection_bridges[index].hash_target;
    return nullptr;
}

/* Hash#key_at / Hash#value_at, and the undefined-method error for any other
 * Hash method that reached the end of collection_invoke_helper. */
__attribute__((noinline))
static DiamondVmStatus hash_entry_at_method(DiamondVm *vm,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name) {
    const bool key_method=method_name->length==6&&
        memcmp(method_name->chars,"key_at",6)==0;
    const bool value_method=method_name->length==8&&
        memcmp(method_name->chars,"value_at",8)==0;
    if(!key_method&&!value_method) {
        snprintf(vm->error,sizeof vm->error,
            "undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Hash");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondHash *hash=(DiamondHash *)registers[recv].as.object;
    const int64_t index=registers[base].as.integer;
    if(index<0||(uint64_t)index>=hash->count) {
        snprintf(vm->error,sizeof vm->error,
            "index %" PRId64 " out of bounds for Hash of length %zu",
            index,hash->count);
        return DIAMOND_VM_INDEX_ERROR;
    }
    const DiamondHashEntry entry=hash->entries[(size_t)index];
    registers[dest]=key_method?entry.key:entry.value;return DIAMOND_VM_OK;
}

/* (start, length) -- same bounds/clamping contract as
 * String#slice (see that one's own comment): start
 * must be in [0, count] (start==count is a valid,
 * always-empty slice), length must be >=0, and the
 * actual element count taken clamps to whatever's
 * actually available rather than erroring on a
 * length that runs past the end. Array had no #slice
 * at all before this -- String's own existed, Array's
 * didn't, an inconsistency with no principled reason
 * behind it. */
__attribute__((noinline))
static DiamondVmStatus array_slice_method(DiamondVm *vm,DiamondArray *array,DiamondValue *registers,uint16_t base,uint8_t argc,uint16_t dest) {
    if(argc!=2)return DIAMOND_VM_ARITY_ERROR;
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[(size_t)base+1].kind!=DIAMOND_VALUE_INT) {
        snprintf(vm->error,sizeof vm->error,"Array#slice arguments must be Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t start=registers[base].as.integer;
    const int64_t requested_length=registers[(size_t)base+1].as.integer;
    if(start<0||(uint64_t)start>array->count||requested_length<0) {
        snprintf(vm->error,sizeof vm->error,
            "index %" PRId64 " out of bounds for Array of length %zu",
            start,array->count);
        return DIAMOND_VM_INDEX_ERROR;
    }
    const size_t available=array->count-(size_t)start;
    const size_t take=(size_t)requested_length<available?
        (size_t)requested_length:available;
    DiamondArray *sliced=allocate_array(vm,array->values+(size_t)start,take);
    if(sliced==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(sliced);return DIAMOND_VM_OK;
}

DiamondVmStatus collection_invoke_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name) {
    if(method_name->length==11&&memcmp(method_name->chars,"respond_to?",11)==0)
        return builtin_respond_to(chunk,registers[recv],registers[base],argc,&registers[dest]);
    const DiamondObjectKind receiver_kind=registers[recv].as.object->kind;
    const bool length_method=method_name->length==6&&
        memcmp(method_name->chars,"length",6)==0;
    if(length_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        size_t length=0;
        if(receiver_kind==DIAMOND_OBJECT_ARRAY)
            length=((DiamondArray *)registers[recv].as.object)->count;
        else if(receiver_kind==DIAMOND_OBJECT_HASH)
            length=((DiamondHash *)registers[recv].as.object)->count;
        else length=((DiamondString *)registers[recv].as.object)->length;
        registers[dest]=DIAMOND_INT((int64_t)length);return DIAMOND_VM_OK;
    }
    /* array_join_helper below does the real work -- kept out
     * of this switch (like program_builder_run_helper is
     * kept out of run_chunk's own INVOKE case) because its
     * local StringBuilder alone (a 32-entry pointer array
     * inside the struct) is real stack weight that would
     * otherwise be baked into every run_chunk call's own
     * frame, not just calls that actually reach #join --
     * confirmed by an ASan stack-overflow regression this
     * exact addition caused in legacy_0092.d6's deep-
     * recursion SystemStackError test before this was
     * factored out. */
    if(receiver_kind==DIAMOND_OBJECT_ARRAY) {
        const bool join_method=method_name->length==4&&
            memcmp(method_name->chars,"join",4)==0;
        if(join_method) {
            if(argc>1)return DIAMOND_VM_ARITY_ERROR;
            const char *separator_chars="";size_t separator_length=0;
            if(argc==1) {
                if(registers[base].kind!=DIAMOND_VALUE_OBJECT||
                   registers[base].as.object->kind!=DIAMOND_OBJECT_STRING) {
                    char actual[80];
                    diamond_format_value_type(actual,sizeof actual,registers[base]);
                    snprintf(vm->error,sizeof vm->error,
                        "Array#join separator must be a String, got %s",
                        actual);
                    return DIAMOND_VM_TYPE_ERROR;
                }
                const DiamondString *separator=
                    (const DiamondString *)registers[base].as.object;
                separator_chars=separator->chars;
                separator_length=separator->length;
            }
            DiamondValue joined=DIAMOND_NIL;
            const DiamondVmStatus join_status=array_join_helper(vm,chunk,depth,
                (const DiamondArray *)registers[recv].as.object,
                separator_chars,separator_length,&joined);
            if(join_status!=DIAMOND_VM_OK)return join_status;
            registers[dest]=joined;return DIAMOND_VM_OK;
        }
    }
    /* A hash lookup, O(1). This was a prelude loop over
     * every key, which made include_key? -- and fetch, and
     * anything checking membership in a loop -- linear.
     * Checked before the prelude bridges below, since the
     * free-function spelling hash_include_key still exists
     * and calls this method. */
    if(receiver_kind==DIAMOND_OBJECT_HASH&&
       method_name->length==12&&
       memcmp(method_name->chars,"include_key?",12)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        const DiamondHash *hash=(const DiamondHash *)registers[recv].as.object;
        registers[dest]=DIAMOND_BOOL(hash_find(hash,registers[base])>=0);
        return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_ARRAY||receiver_kind==DIAMOND_OBJECT_HASH) {
        /* Not chunk->code+instruction_offset (an ordinary call site's
         * own would be stable, but the native-spread synthetic re-entry
         * just above builds its own tiny bytecode buffer fresh on the C
         * stack every time -- a different logical call site, same reused
         * stack address, which collided two unrelated method names onto
         * one cache entry and returned the wrong function). method_name
         * itself is a pointer into the calling function's own permanent,
         * never-reallocated string-constant table (fn->strings, set at
         * compile time), stable and correctly distinct per method name
         * regardless of which of the several dispatch paths reaches it. */
        const uint8_t *extension_site=(const uint8_t *)(const void *)method_name;
        const DiamondFunction *target=nullptr;
        /* The bridge table is scanned only on a cache miss, so cached calls
         * cost no more than the inline name tests it replaced. A bridged
         * name whose prelude function is missing is never cached: that call
         * reports the error below, and so does every later one, so a cached
         * nullptr always means "no bridge" and needs no table scan. */
        const char *target_name=nullptr;
        if(!cached_extension_lookup(vm,extension_site,
                (uint8_t)receiver_kind,&target)) {
            target_name=collection_bridge_target(receiver_kind,method_name);
            target=target_name!=nullptr?
                find_top_level_function(chunk,target_name,strlen(target_name)):
                find_collection_extension(chunk,receiver_kind,
                    method_name->chars,method_name->length);
            if(target!=nullptr||target_name==nullptr)
                store_extension_lookup(vm,extension_site,
                    (uint8_t)receiver_kind,target);
        }
        if(target==nullptr&&target_name!=nullptr) {
                snprintf(vm->error,sizeof vm->error,
                    "internal error: missing standard library function '%s'",
                    target_name);
                return DIAMOND_VM_TYPE_ERROR;
        }
        if(target!=nullptr) {
            DiamondValue call_result=DIAMOND_NIL;
            const DiamondVmStatus status=forward_to_top_level_helper(
                vm,chunk,target,registers,recv,base,argc,depth,
                &call_result);
            if(status!=DIAMOND_VM_OK)return status;
            registers[dest]=call_result;return DIAMOND_VM_OK;
        }
    }
    if(receiver_kind==DIAMOND_OBJECT_HASH&&
       method_name->length==5&&
       memcmp(method_name->chars,"clear",5)==0) {
        /* Removes every pair in one O(buckets) pass -- not a
         * loop of delete, which rehashes the whole table on
         * each call. Nothing is allocated, so there is no
         * out-of-memory path, and dropping references needs no
         * write barrier. Returns the (now empty) Hash itself,
         * as Ruby's does. */
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        DiamondHash *hash=(DiamondHash *)registers[recv].as.object;
        if(hash->object.frozen)return DIAMOND_VM_FROZEN_ERROR;
        hash->count=0;
        for(size_t slot=0;slot<hash->bucket_capacity;slot++)
            hash->buckets[slot]=SIZE_MAX;
        registers[dest]=registers[recv];return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_HASH&&
       method_name->length==6&&
       memcmp(method_name->chars,"delete",6)==0) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        DiamondHash *hash=(DiamondHash *)registers[recv].as.object;
        if(hash->object.frozen)return DIAMOND_VM_FROZEN_ERROR;
        DiamondValue removed=DIAMOND_NIL;bool found=false;
        if(!hash_delete(vm,hash,registers[base],&removed,&found))
            return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=found?removed:DIAMOND_NIL;return DIAMOND_VM_OK;
    }
    if(receiver_kind==DIAMOND_OBJECT_HASH)
        return hash_entry_at_method(vm,registers,recv,base,argc,dest,method_name);
    if(receiver_kind==DIAMOND_OBJECT_STRING)
        return string_invoke_helper(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
    if(receiver_kind!=DIAMOND_OBJECT_ARRAY)
        return DIAMOND_VM_TYPE_ERROR;
    DiamondArray *array=(DiamondArray *)registers[recv].as.object;
    const bool push_method=method_name->length==4&&
        memcmp(method_name->chars,"push",4)==0;
    const bool pop_method=method_name->length==3&&
        memcmp(method_name->chars,"pop",3)==0;
    if(push_method) {
        if(argc!=1)return DIAMOND_VM_ARITY_ERROR;
        if(array->object.frozen)return DIAMOND_VM_FROZEN_ERROR;
        if(!array_value_satisfies_constraints(array,registers[base])) {
            snprintf(vm->error,sizeof vm->error,
                     "array element violates its type annotation");
            return DIAMOND_VM_TYPE_ERROR;
        }
        if(!array_push(vm,array,registers[base]))
            return DIAMOND_VM_OUT_OF_MEMORY;
        registers[dest]=registers[recv];return DIAMOND_VM_OK;
    }
    if(pop_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        if(array->object.frozen)return DIAMOND_VM_FROZEN_ERROR;
        registers[dest]=array->count==0?DIAMOND_NIL:
            array->values[--array->count];return DIAMOND_VM_OK;
    }
    if(method_name->length==5&&memcmp(method_name->chars,"slice",5)==0)
        return array_slice_method(vm,array,registers,base,argc,dest);
    snprintf(vm->error,sizeof vm->error,"undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"Array");
    return DIAMOND_VM_TYPE_ERROR;
}

/* The prelude function behind a numeric duration method (`5.minutes()`),
 * or nullptr. */
static const char *numeric_duration_target(const DiamondStringConstant *method_name) {
    const char *duration_target=nullptr;
    if((method_name->length==6&&
        memcmp(method_name->chars,"second",6)==0)||
       (method_name->length==7&&
        memcmp(method_name->chars,"seconds",7)==0))
        duration_target="numeric_seconds";
    else if((method_name->length==6&&
             memcmp(method_name->chars,"minute",6)==0)||
            (method_name->length==7&&
             memcmp(method_name->chars,"minutes",7)==0))
        duration_target="numeric_minutes";
    else if((method_name->length==4&&
             memcmp(method_name->chars,"hour",4)==0)||
            (method_name->length==5&&
             memcmp(method_name->chars,"hours",5)==0))
        duration_target="numeric_hours";
    else if((method_name->length==3&&
             memcmp(method_name->chars,"day",3)==0)||
            (method_name->length==4&&
             memcmp(method_name->chars,"days",4)==0))
        duration_target="numeric_days";
    else if((method_name->length==4&&
             memcmp(method_name->chars,"week",4)==0)||
            (method_name->length==5&&
             memcmp(method_name->chars,"weeks",5)==0))
        duration_target="numeric_weeks";
    return duration_target;
}

    /* Int#to_s(base): digits in base 2..36, lowercase,
     * with a leading '-' when negative -- as in Ruby. */
__attribute__((noinline))
static DiamondVmStatus int_to_s_radix_method(DiamondVm *vm,DiamondValue *registers,uint16_t recv,uint16_t base,uint16_t dest) {
    if(registers[base].kind!=DIAMOND_VALUE_INT||
       registers[base].as.integer<2||registers[base].as.integer>36) {
        snprintf(vm->error,sizeof vm->error,
            "Int#to_s base must be an Int from 2 to 36");
        return DIAMOND_VM_ARITY_ERROR;
    }
    const uint64_t radix=(uint64_t)registers[base].as.integer;
    const int64_t integer=registers[recv].as.integer;
    uint64_t magnitude=integer<0?(uint64_t)0-(uint64_t)integer:
        (uint64_t)integer;
    char digits[72];size_t length=0;
    do {
        digits[length++]="0123456789abcdefghijklmnopqrstuvwxyz"[magnitude%radix];
        magnitude/=radix;
    } while(magnitude>0);
    if(integer<0)digits[length++]='-';
    for(size_t low=0,high=length-1;low<high;low++,high--) {
        const char swap=digits[low];digits[low]=digits[high];digits[high]=swap;
    }
    DiamondString *formatted=allocate_string(vm,digits,length);
    if(formatted==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(formatted);return DIAMOND_VM_OK;
}

/* A method on a Float that the native cases did not handle: a float_ or
 * numeric_ prelude extension, or an undefined-method error. */
__attribute__((noinline))
static DiamondVmStatus float_extension_method(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name) {
    /* Not chunk->code+instruction_offset (an ordinary call site's
     * own would be stable, but the native-spread synthetic re-entry
     * just above builds its own tiny bytecode buffer fresh on the C
     * stack every time -- a different logical call site, same reused
     * stack address, which collided two unrelated method names onto
     * one cache entry and returned the wrong function). method_name
     * itself is a pointer into the calling function's own permanent,
     * never-reallocated string-constant table (fn->strings, set at
     * compile time), stable and correctly distinct per method name
     * regardless of which of the several dispatch paths reaches it. */
    const uint8_t *extension_site=(const uint8_t *)(const void *)method_name;
    const DiamondFunction *extension=nullptr;
    if(!cached_extension_lookup(vm,extension_site,
            DIAMOND_EXTENSION_KIND_FLOAT,&extension)) {
        static const char *const float_prefixes[]={"float_","numeric_"};
        extension=find_value_extension(chunk,
            float_prefixes,2,method_name->chars,method_name->length);
        store_extension_lookup(vm,extension_site,
            DIAMOND_EXTENSION_KIND_FLOAT,extension);
    }
    if(extension!=nullptr) {
        DiamondValue call_result=DIAMOND_NIL;
        const DiamondVmStatus status=forward_to_top_level_helper(vm,
            chunk,extension,registers,recv,base,argc,depth,&call_result);
        if(status!=DIAMOND_VM_OK)return status;
        registers[dest]=call_result;return DIAMOND_VM_OK;
    }
    snprintf(vm->error,sizeof vm->error,
        "undefined method '%.*s' for %s",
        (int)method_name->length,method_name->chars,"Float");
    return DIAMOND_VM_TYPE_ERROR;
}

/* chr, the inverse of String#ord -- a single byte (0-255),
 * matching every other String primitive in this VM
 * staying byte- rather than codepoint-oriented. */
__attribute__((noinline))
static DiamondVmStatus int_chr_method(DiamondVm *vm,DiamondValue *registers,uint16_t recv,uint8_t argc,uint16_t dest) {
    if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
    const int64_t code=registers[recv].as.integer;
    if(code<0||code>255) {
        snprintf(vm->error,sizeof vm->error,
            "Int#chr argument must be between 0 and 255");
        return DIAMOND_VM_INTEGER_OVERFLOW;
    }
    const char byte=(char)(unsigned char)code;
    DiamondString *chr_string=allocate_string(vm,&byte,1);
    if(chr_string==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    registers[dest]=DIAMOND_OBJECT(chr_string);return DIAMOND_VM_OK;
}

/* A method on an Int that the native cases did not handle: times/upto/downto,
 * an integer_ or numeric_ prelude extension, or an undefined-method error. */
__attribute__((noinline))
static DiamondVmStatus int_extension_method(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name) {
    /* times/upto/downto -- trivial Callable[1] consumers of
     * block syntax, forwarded to ordinary prelude Diamond
     * functions (lib/core.d6) exactly like Array/Hash's own
     * Enumerable methods below, rather than hand-rolled
     * here. */
    const char *target_name=nullptr;
    if(method_name->length==5&&
       memcmp(method_name->chars,"times",5)==0)
        target_name="integer_times";
    else if(method_name->length==4&&
            memcmp(method_name->chars,"upto",4)==0)
        target_name="integer_upto";
    else if(method_name->length==6&&
            memcmp(method_name->chars,"downto",6)==0)
        target_name="integer_downto";
    /* Not chunk->code+instruction_offset (an ordinary call site's
     * own would be stable, but the native-spread synthetic re-entry
     * just above builds its own tiny bytecode buffer fresh on the C
     * stack every time -- a different logical call site, same reused
     * stack address, which collided two unrelated method names onto
     * one cache entry and returned the wrong function). method_name
     * itself is a pointer into the calling function's own permanent,
     * never-reallocated string-constant table (fn->strings, set at
     * compile time), stable and correctly distinct per method name
     * regardless of which of the several dispatch paths reaches it. */
    const uint8_t *extension_site=(const uint8_t *)(const void *)method_name;
    const DiamondFunction *target=nullptr;
    if(!cached_extension_lookup(vm,extension_site,
            DIAMOND_EXTENSION_KIND_INT,&target)) {
        static const char *const integer_prefixes[]={"integer_","numeric_"};
        target=target_name!=nullptr?
            find_top_level_function(chunk,target_name,strlen(target_name)):
            find_value_extension(chunk,integer_prefixes,2,
                method_name->chars,method_name->length);
        store_extension_lookup(vm,extension_site,
            DIAMOND_EXTENSION_KIND_INT,target);
    }
    if(target==nullptr&&target_name==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "undefined method '%.*s' for %s",
            (int)method_name->length,method_name->chars,"Int");
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(target==nullptr) {
        snprintf(vm->error,sizeof vm->error,
            "internal error: missing standard library function '%s'",
            target_name);
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondValue call_result=DIAMOND_NIL;
    const DiamondVmStatus status=forward_to_top_level_helper(vm,
        chunk,target,registers,recv,base,argc,depth,&call_result);
    if(status!=DIAMOND_VM_OK)return status;
    registers[dest]=call_result;return DIAMOND_VM_OK;
}

DiamondVmStatus numeric_invoke_helper(DiamondVm *vm,const DiamondChunk *chunk,size_t depth,DiamondValue *registers,uint16_t recv,uint16_t base,uint8_t argc,uint16_t dest,const DiamondStringConstant *method_name) {
    if(method_name->length==11&&memcmp(method_name->chars,"respond_to?",11)==0)
        return builtin_respond_to(chunk,registers[recv],registers[base],argc,&registers[dest]);
    const bool ago_method=method_name->length==3&&
        memcmp(method_name->chars,"ago",3)==0;
    const bool from_now_method=method_name->length==8&&
        memcmp(method_name->chars,"from_now",8)==0;
    if(ago_method||from_now_method) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const DiamondVmStatus time_status=time_relative_now_helper(
            vm,registers[recv],from_now_method,&registers[dest]);
        return time_status;
    }
    const char *duration_target=numeric_duration_target(method_name);
    if(duration_target!=nullptr) {
        if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
        const DiamondFunction *duration_function=find_top_level_function(
            chunk,duration_target,strlen(duration_target));
        if(duration_function==nullptr) {
            snprintf(vm->error,sizeof vm->error,
                "internal error: missing standard library function '%s'",
                duration_target);
            return DIAMOND_VM_TYPE_ERROR;
        }
        DiamondValue duration_result=DIAMOND_NIL;
        const DiamondVmStatus duration_status=forward_to_top_level_helper(
            vm,chunk,duration_function,registers,recv,base,argc,depth,
            &duration_result);
        if(duration_status!=DIAMOND_VM_OK)return duration_status;
        registers[dest]=duration_result;return DIAMOND_VM_OK;
    }
    /* Conversions and rounding shared by Int and Float.
     * to_s matches string interpolation exactly; floor/ceil/
     * round() return Int (promoting past 64 bits like to_i,
     * RangeError for NaN/Infinity) and round(digits) returns
     * Float. */
    {
        const bool is_float=registers[recv].kind==DIAMOND_VALUE_FLOAT;
        const double real=is_float?registers[recv].as.real:
            (double)registers[recv].as.integer;
        #define NUMERIC_METHOD(text) (method_name->length==sizeof(text)-1&& \
            memcmp(method_name->chars,text,sizeof(text)-1)==0)
        if(NUMERIC_METHOD("to_s")&&argc==1&&!is_float)
            return int_to_s_radix_method(vm,registers,recv,base,dest);
        if(NUMERIC_METHOD("to_s")) {
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            StringBuilder text={};
            if(!builder_format_value(&text,registers[recv])) {
                free(text.chars);return DIAMOND_VM_OUT_OF_MEMORY;
            }
            DiamondString *formatted=allocate_string(vm,text.chars,text.length);
            free(text.chars);
            if(formatted==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
            registers[dest]=DIAMOND_OBJECT(formatted);return DIAMOND_VM_OK;
        }
        if(NUMERIC_METHOD("to_f")) {
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            registers[dest]=DIAMOND_FLOAT(real);return DIAMOND_VM_OK;
        }
        if(NUMERIC_METHOD("abs")) {
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            if(is_float) {registers[dest]=DIAMOND_FLOAT(fabs(real));return DIAMOND_VM_OK;}
            const int64_t integer=registers[recv].as.integer;
            if(integer==INT64_MIN) {
                /* Its magnitude needs a bignum, as with negation. */
                DiamondIntView view;
                diamond_int_view_int64(integer,&view);
                const DiamondValue promoted=diamond_bignum_negate(vm,view);
                if(promoted.kind==DIAMOND_VALUE_NIL)return DIAMOND_VM_OUT_OF_MEMORY;
                registers[dest]=promoted;return DIAMOND_VM_OK;
            }
            registers[dest]=DIAMOND_INT(integer<0?-integer:integer);return DIAMOND_VM_OK;
        }
        if(NUMERIC_METHOD("round")&&is_float&&argc==1) {
            const DiamondValue digits=registers[base];
            if(digits.kind!=DIAMOND_VALUE_INT||digits.as.integer<-15||
               digits.as.integer>15) {
                snprintf(vm->error,sizeof vm->error,
                    "Float#round digits must be an Int from -15 to 15");
                return DIAMOND_VM_TYPE_ERROR;
            }
            const double scale=pow(10.0,(double)digits.as.integer);
            registers[dest]=DIAMOND_FLOAT(isfinite(real)?round(real*scale)/scale:real);
            return DIAMOND_VM_OK;
        }
        const bool to_int=NUMERIC_METHOD("to_i");
        const bool floor_method=NUMERIC_METHOD("floor");
        const bool ceil_method=NUMERIC_METHOD("ceil");
        const bool round_method=NUMERIC_METHOD("round");
        #undef NUMERIC_METHOD
        if(to_int||(is_float&&(floor_method||ceil_method||round_method))) {
            if(argc!=0)return DIAMOND_VM_ARITY_ERROR;
            if(!is_float) {registers[dest]=registers[recv];return DIAMOND_VM_OK;}
            const double whole=floor_method?floor(real):ceil_method?ceil(real):
                round_method?round(real):trunc(real);
            DiamondValue converted=DIAMOND_NIL;
            const DiamondVmStatus convert_status=float_to_int(vm,whole,floor_method?"floor":
                ceil_method?"ceil":round_method?"round":"to_i",&converted);
            if(convert_status!=DIAMOND_VM_OK)return convert_status;
            registers[dest]=converted;return DIAMOND_VM_OK;
        }
    }
    if(registers[recv].kind==DIAMOND_VALUE_FLOAT)
        return float_extension_method(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
    const bool chr_method=method_name->length==3&&
        memcmp(method_name->chars,"chr",3)==0;
    if(chr_method)
        return int_chr_method(vm,registers,recv,argc,dest);
    return int_extension_method(vm,chunk,depth,registers,recv,base,argc,dest,method_name);
}
