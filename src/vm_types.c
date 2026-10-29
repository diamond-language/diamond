#define _GNU_SOURCE

#include "vm_internal.h"
#include "bignum.h"
#include "interface_inference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint16_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint16_t expected_index);

static bool value_is_native_kind(DiamondValue value,uint8_t kind);

/* Every native method String/Array/Hash actually implement, for
 * structural-interface matching (see diamond_native_method_satisfies
 * below). Kept next to the real DIAMOND_OP_INVOKE dispatch further
 * down in this file so a future native-method addition is naturally
 * visible nearby. return_type is UINT8_MAX when a method's result
 * isn't one fixed scalar type (pop/key_at/value_at return the
 * container's stored element type; index_of returns Int | Nil) --
 * such a method can still satisfy an interface method with no return
 * annotation, just never one that requires a specific return type,
 * matching the same conservative convention already used for
 * user-class methods with an unannotated return. Enumerable-style
 * receiver methods (.each/.select/.count/.any?/.all?/.reduce/.map)
 * are deliberately not listed here -- they forward to ordinary prelude
 * Diamond functions rather than being native primitives, so an
 * interface already matches them the normal way, through a class's
 * own method table. */
typedef struct DiamondNativeMethod {
    uint16_t receiver_type;
    const char *name;
    uint8_t arity;
    uint16_t return_type;
} DiamondNativeMethod;

static const DiamondNativeMethod DIAMOND_NATIVE_METHODS[]={
    {DIAMOND_TYPE_STRING,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_ARRAY,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_HASH,"length",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"repeat",1,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"ord",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"split",1,DIAMOND_TYPE_ARRAY},
    {DIAMOND_TYPE_STRING,"strip",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"empty?",0,DIAMOND_TYPE_BOOL},
    {DIAMOND_TYPE_STRING,"lstrip",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"rstrip",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"reverse",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"downcase",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"upcase",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_STRING,"to_i",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_STRING,"to_f",0,DIAMOND_TYPE_FLOAT},
    {DIAMOND_TYPE_STRING,"index_of",1,DIAMOND_TYPE_NONE},
    {DIAMOND_TYPE_STRING,"slice",2,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_ARRAY,"push",1,DIAMOND_TYPE_ARRAY},
    {DIAMOND_TYPE_ARRAY,"pop",0,DIAMOND_TYPE_NONE},
    {DIAMOND_TYPE_HASH,"key_at",1,DIAMOND_TYPE_NONE},
    {DIAMOND_TYPE_HASH,"value_at",1,DIAMOND_TYPE_NONE},
    {DIAMOND_TYPE_HASH,"delete",1,DIAMOND_TYPE_NONE},
    {DIAMOND_TYPE_HASH,"clear",0,DIAMOND_TYPE_HASH},
    {DIAMOND_TYPE_HASH,"include_key?",1,DIAMOND_TYPE_BOOL},
    {DIAMOND_TYPE_INT,"to_s",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_INT,"to_i",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_INT,"to_f",0,DIAMOND_TYPE_FLOAT},
    {DIAMOND_TYPE_INT,"abs",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_FLOAT,"to_s",0,DIAMOND_TYPE_STRING},
    {DIAMOND_TYPE_FLOAT,"to_i",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_FLOAT,"to_f",0,DIAMOND_TYPE_FLOAT},
    {DIAMOND_TYPE_FLOAT,"abs",0,DIAMOND_TYPE_FLOAT},
    {DIAMOND_TYPE_FLOAT,"floor",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_FLOAT,"ceil",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_FLOAT,"round",0,DIAMOND_TYPE_INT},
    {DIAMOND_TYPE_FLOAT,"round",1,DIAMOND_TYPE_FLOAT},
};

/* Every method name the VM implements natively (or as a universal
 * pseudo-method) per built-in receiver type, for editor completion
 * (lsp/completion.c). Unlike DIAMOND_NATIVE_METHODS above this is names only
 * and covers the whole surface, including natives dispatched inline in
 * run_chunk; the prelude's extension functions (array_x, string_x, ...) are
 * discovered from the compiled program instead and merged by the caller, so a
 * name may appear in both. tests/lsp_test.sh calls every completed name on a
 * real value, so a name listed here that the VM does not implement fails CI. */
static const char *const NATIVE_MEMBERS_STRING[]={
    "bytes","capitalize","chars","chomp","count","deep_freeze","delete","downcase",
    "dup","empty?","end_with?","format","freeze","frozen?","getbyte","gsub",
    "include?","index_of","inspect","length","ljust","lstrip","match","match?","nil?","ord",
    "partition","repeat","reverse","rjust","rstrip","scan","size","slice","split",
    "start_with?","strip","sub","tap","to_f","to_i","to_s","tr","upcase"};
static const char *const NATIVE_MEMBERS_ARRAY[]={
    "all?","any?","clear","compact","concat","count","deep_freeze","delete",
    "delete_at","drop","dup","each","each_cons","each_slice","each_with_index",
    "empty?","find","first","first_or","flat_map","flatten","freeze","frozen?",
    "group_by","include?","index_of","inspect","join","last","last_or","lazy","length","map",
    "max","max_by","min","min_by","nil?","partition","pop","push","reduce","reject",
    "reverse","sample","select","shuffle","size","slice","sort","sort_by","sum",
    "take","tally","tap","to_a","to_s","uniq","zip"};
static const char *const NATIVE_MEMBERS_HASH[]={
    "all?","any?","clear","compact","count","deep_freeze","delete","dup","each",
    "each_with_index","empty?","fetch","find","freeze","frozen?","group_by",
    "include?","include_key?","inspect","key_at","keys","lazy","length","map","map_values",
    "max_by","merge","min_by","nil?","partition","reduce","reject","select","size",
    "slice","sort","sort_by","sum","tap","to_a","to_s","value_at","values"};
static const char *const NATIVE_MEMBERS_INT[]={
    "abs","ago","chr","day","days","deep_freeze","downto","dup","freeze","from_now",
    "frozen?","hour","hours","inspect","minute","minutes","nil?","second","seconds","tap",
    "times","to_f","to_i","to_s","upto","week","weeks"};
static const char *const NATIVE_MEMBERS_FLOAT[]={
    "abs","ago","ceil","day","days","deep_freeze","dup","floor","freeze","from_now",
    "frozen?","hour","hours","inspect","minute","minutes","nil?","round","second","seconds",
    "tap","to_f","to_i","to_s","week","weeks"};

bool diamond_native_method_return_type(uint16_t receiver_type,const char *name,
                                       size_t length,uint16_t *return_type) {
    bool found=false;
    for(size_t index=0;index<sizeof DIAMOND_NATIVE_METHODS/
        sizeof DIAMOND_NATIVE_METHODS[0];index++) {
        const DiamondNativeMethod *method=&DIAMOND_NATIVE_METHODS[index];
        if(method->receiver_type!=receiver_type||strlen(method->name)!=length||
           memcmp(method->name,name,length)!=0)continue;
        /* Overloads (Float#round with and without digits) that disagree, or a
         * result that is not one fixed type, leave the answer unknown. */
        if(method->return_type==DIAMOND_TYPE_NONE||(found&&*return_type!=method->return_type))
            return false;
        *return_type=method->return_type;found=true;
    }
    return found;
}

size_t diamond_native_member_names(uint16_t receiver_type,const char *const **names) {
#define NATIVE_MEMBERS_CASE(type,list) \
    case type: *names=list;return sizeof list/sizeof list[0];
    switch(receiver_type) {
        NATIVE_MEMBERS_CASE(DIAMOND_TYPE_STRING,NATIVE_MEMBERS_STRING)
        NATIVE_MEMBERS_CASE(DIAMOND_TYPE_ARRAY,NATIVE_MEMBERS_ARRAY)
        NATIVE_MEMBERS_CASE(DIAMOND_TYPE_HASH,NATIVE_MEMBERS_HASH)
        NATIVE_MEMBERS_CASE(DIAMOND_TYPE_INT,NATIVE_MEMBERS_INT)
        NATIVE_MEMBERS_CASE(DIAMOND_TYPE_FLOAT,NATIVE_MEMBERS_FLOAT)
        default: *names=nullptr;return 0;
    }
#undef NATIVE_MEMBERS_CASE
}

bool diamond_native_method_satisfies(uint16_t receiver_type,const char *name,
                                     uint8_t arity,uint16_t *return_type) {
    for(size_t index=0;index<sizeof DIAMOND_NATIVE_METHODS/
        sizeof DIAMOND_NATIVE_METHODS[0];index++) {
        const DiamondNativeMethod *method=&DIAMOND_NATIVE_METHODS[index];
        if(method->receiver_type==receiver_type&&
           strcmp(method->name,name)==0&&method->arity==arity) {
            *return_type=method->return_type;
            return true;
        }
    }
    return false;
}

static bool bound_nodes_equal(const DiamondTypeBinding *left,uint8_t a,
        const DiamondTypeBinding *right,uint8_t b,size_t depth);

static bool bound_payload_equal(const DiamondTypeBinding *left,DiamondBoundTypeMember m,
        const DiamondTypeBinding *right,DiamondBoundTypeMember n,size_t depth) {
    if(m.id==DIAMOND_TYPE_NATIVE&&m.native_kind!=n.native_kind)return false;
    if(m.id!=DIAMOND_TYPE_CALLABLE)return true;
    if(m.signature==UINT8_MAX||n.signature==UINT8_MAX)return m.signature==n.signature;
    if(m.signature>=left->signature_count||n.signature>=right->signature_count)return false;
    const DiamondBoundSignature *x=&left->signatures[m.signature],*y=&right->signatures[n.signature];
    if(x->arity!=y->arity||x->required_arity!=y->required_arity||x->variadic!=y->variadic||
       x->parameters_typed!=y->parameters_typed||
       !bound_nodes_equal(left,x->return_node,right,y->return_node,depth+1))return false;
    if(x->parameters_typed)
        for(size_t p=0;p<x->arity;p++)
            if(!bound_nodes_equal(left,x->parameter_nodes[p],right,y->parameter_nodes[p],depth+1))return false;
    return true;
}

static bool bound_nodes_equal(const DiamondTypeBinding *left,uint8_t a,
        const DiamondTypeBinding *right,uint8_t b,size_t depth) {
    if(left->invalid||right->invalid)return false;
    if(a==UINT8_MAX||b==UINT8_MAX)return a==b;
    if(depth>=DIAMOND_BOUND_TYPE_NODES||a>=left->node_count||b>=right->node_count)
        return false;
    const DiamondBoundTypeNode *x=&left->nodes[a],*y=&right->nodes[b];
    if(x->count!=y->count)return false;
    for(size_t i=0;i<x->count;i++) {
        bool found=false;
        for(size_t j=0;j<y->count&&!found;j++) {
            const DiamondBoundTypeMember m=x->members[i],n=y->members[j];
            found=m.id==n.id&&bound_payload_equal(left,m,right,n,depth)&&
                bound_nodes_equal(left,m.argument_node,right,n.argument_node,depth+1)&&
                bound_nodes_equal(left,m.second_argument_node,right,n.second_argument_node,depth+1);
        }
        if(!found)return false;
    }
    return true;
}

/* Bindings own all signature metadata, including native kinds and Callable
 * child nodes. Unresolved variables cannot establish an explicit contract. */
static bool supported_binding_graph(const DiamondTypeBinding *binding) {
    if(binding->invalid||binding->node_count==0||binding->node_count>DIAMOND_BOUND_TYPE_NODES)return false;
    for(size_t n=0;n<binding->node_count;n++)
        for(size_t m=0;m<binding->nodes[n].count;m++) {
            const uint16_t id=binding->nodes[n].members[m].id;
            if(id>=DIAMOND_TYPE_VARIABLE_BASE&&id<DIAMOND_TYPE_INTERFACE_BASE)return false;
        }
    return true;
}

static bool class_binding_fits(const DiamondChunk *chunk,const DiamondTypeSet *sets,
        uint16_t index,size_t *nodes,size_t depth) {
    if(index>=chunk->type_set_count||depth>=DIAMOND_BOUND_TYPE_NODES||
       ++*nodes>DIAMOND_BOUND_TYPE_NODES)return false;
    const DiamondTypeSet *set=&sets[index];
    if(set->count==0||set->count>DIAMOND_BOUND_TYPE_MEMBERS)return false;
    for(size_t i=0;i<set->count;i++) {
        const DiamondTypeMember m=set->members[i];
        for(size_t j=0;j<i;j++)if(set->members[j].id==m.id&&
            (m.id!=DIAMOND_TYPE_NATIVE||set->members[j].callable_arity==m.callable_arity))return false;
        if(m.id>=DIAMOND_TYPE_VARIABLE_BASE&&m.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=m.id-DIAMOND_TYPE_VARIABLE_BASE;
            if(set->count!=1||variable>=chunk->type_variable_count||
               chunk->type_variable_bindings==nullptr)return false;
            const DiamondTypeBinding *binding=&chunk->type_variable_bindings[variable];
            if(!supported_binding_graph(binding)||binding->nodes[0].count==0)return false;
            *nodes+=binding->node_count-1;
            if(*nodes>DIAMOND_BOUND_TYPE_NODES)return false;
        }
        if(m.argument_set!=DIAMOND_NO_TYPE_SET&&
           !class_binding_fits(chunk,sets,m.argument_set,nodes,depth+1))return false;
        if(m.second_argument_set!=DIAMOND_NO_TYPE_SET&&
           !class_binding_fits(chunk,sets,m.second_argument_set,nodes,depth+1))return false;
        if(m.callable_return_set!=DIAMOND_NO_TYPE_SET&&
           !class_binding_fits(chunk,sets,m.callable_return_set,nodes,depth+1))return false;
        if(m.callable_parameters_typed)
            for(size_t parameter=0;parameter<m.callable_arity;parameter++)
                if(m.callable_parameter_sets[parameter]!=DIAMOND_NO_TYPE_SET&&
                   !class_binding_fits(chunk,sets,m.callable_parameter_sets[parameter],nodes,depth+1))return false;
    }
    return true;
}

bool bind_explicit_class_type(const DiamondChunk *chunk,uint16_t index,
        DiamondTypeBinding *binding) {
    if(index>=chunk->type_set_count)return false;
    size_t nodes=0;
    if(!class_binding_fits(chunk,chunk->type_sets,index,&nodes,0))return false;
    *binding=(DiamondTypeBinding){};
    (void)binding_node(binding);
    bind_context_set(binding,0,chunk,chunk->type_sets,index);
    return !binding->invalid;
}

bool bind_explicit_instance_types(const DiamondChunk *chunk,uint16_t index,DiamondInstance *instance) {
    if(index>=chunk->type_set_count||instance->type_binding==nullptr)return false;
    if(DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class)==1)
        return bind_explicit_class_type(chunk,index,instance->type_binding);
    const DiamondTypeSet *set=&chunk->type_sets[index];
    if(set->count!=1||set->members[0].id!=DIAMOND_TYPE_HASH)return false;
    return bind_explicit_class_type(chunk,set->members[0].argument_set,&instance->type_binding[0])&&
        bind_explicit_class_type(chunk,set->members[0].second_argument_set,&instance->type_binding[1]);
}

static void bind_value_graph(const DiamondChunk *chunk,DiamondTypeBinding *binding,
                             uint8_t node,DiamondValue value);
static bool runtime_graph_satisfies(const DiamondChunk *chunk,
        const DiamondTypeBinding *known,uint8_t a,
        const DiamondTypeBinding *expected,uint8_t b,size_t depth);

static bool value_matches_bound_node(const DiamondChunk *chunk,DiamondValue value,
                                     const DiamondTypeBinding *binding,
                                     uint8_t node_index) {
    if(binding->invalid)return false;
    if(node_index>=binding->node_count)return true;
    const DiamondBoundTypeNode *node=&binding->nodes[node_index];
    if(node->count==0)return true;
    for(size_t index=0;index<node->count;index++) {
        const DiamondBoundTypeMember member=node->members[index];
        if(!value_matches_type(chunk,value,member.id))continue;
        if(member.id==DIAMOND_TYPE_NATIVE) {
            if(member.native_kind==UINT8_MAX||value_is_native_kind(value,member.native_kind))return true;
        } else if(member.id==DIAMOND_TYPE_CALLABLE&&member.signature!=UINT8_MAX) {
            DiamondTypeBinding actual={.node_count=1};
            bind_value_graph(chunk,&actual,0,value);
            if(runtime_graph_satisfies(chunk,&actual,0,binding,node_index,0))return true;
        } else if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_node!=UINT8_MAX) {
            const DiamondArray *array=(const DiamondArray *)value.as.object;
            bool matches=true;
            for(size_t item=0;item<array->count&&matches;item++)
                matches=value_matches_bound_node(chunk,array->values[item],binding,
                                                 member.argument_node);
            if(matches)return true;
        } else if(member.id==DIAMOND_TYPE_HASH&&
                  member.argument_node!=UINT8_MAX&&
                  member.second_argument_node!=UINT8_MAX) {
            const DiamondHash *hash=(const DiamondHash *)value.as.object;
            bool matches=true;
            for(size_t item=0;item<hash->count&&matches;item++)
                matches=value_matches_bound_node(chunk,hash->entries[item].key,binding,
                                                 member.argument_node)&&
                    value_matches_bound_node(chunk,hash->entries[item].value,binding,
                                             member.second_argument_node);
            if(matches)return true;
        } else if(member.id>=DIAMOND_TYPE_CLASS_BASE&&member.id<DIAMOND_TYPE_VARIABLE_BASE&&
                  member.argument_node!=UINT8_MAX) {
            const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
            if(instance->type_binding!=nullptr&&
               bound_nodes_equal(binding,member.argument_node,instance->type_binding,0,0)&&
               (member.second_argument_node==UINT8_MAX||
                (DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class)==2&&
                 bound_nodes_equal(binding,member.second_argument_node,&instance->type_binding[1],0,0))))return true;
        } else return true;
    }
    return false;
}

static bool runtime_set_graph_supported(const DiamondTypeSet *sets,uint16_t index,size_t depth);
static void bind_bound_node(DiamondTypeBinding *target,uint8_t target_node,
                            const DiamondTypeBinding *source,uint8_t source_node);

static bool interface_signature_resolved(const DiamondFunction *function,uint16_t set,
        const DiamondTypeBinding *bindings,size_t count,size_t depth) {
    if(set==DIAMOND_NO_TYPE_SET)return true;
    if(depth>32||set>=function->type_set_count)return false;
    for(size_t i=0;i<function->type_sets[set].count;i++) {
        const DiamondTypeMember member=function->type_sets[set].members[i];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=member.id-DIAMOND_TYPE_VARIABLE_BASE;
            if(variable>=count||bindings==nullptr||bindings[variable].invalid||
               bindings[variable].node_count==0||bindings[variable].nodes[0].count==0)return false;

        }
        if(!interface_signature_resolved(function,member.argument_set,bindings,count,depth+1)||
           !interface_signature_resolved(function,member.second_argument_set,bindings,count,depth+1)||
           !interface_signature_resolved(function,member.callable_return_set,bindings,count,depth+1))return false;
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++)
                if(!interface_signature_resolved(function,member.callable_parameter_sets[parameter],bindings,count,depth+1))return false;
    }
    return true;
}

/* Interface matching is synchronous and cannot yield into another VM. Keep
 * recursive structural checks bounded independently on each OS thread. */
static thread_local unsigned runtime_generic_interface_depth;

static void bind_context_members(DiamondTypeBinding *binding,uint8_t node,
        const DiamondChunk *context,const DiamondTypeSet *sets,const DiamondTypeSet *set);

static bool generic_class_implements(const DiamondChunk *chunk,const DiamondClass *class,
        const DiamondTypeBinding *bindings,size_t count,const DiamondInterface *interface) {
    if(runtime_generic_interface_depth>=32)return false;
    runtime_generic_interface_depth++;
    bool matches=true;
    for(size_t required=0;required<interface->method_count&&matches;required++) {
        const DiamondInterfaceMethod *wanted=&interface->methods[required];
        bool found=false;
        for(size_t method=0;method<class->method_count&&!found;method++) {
            const DiamondMethod *candidate=&class->methods[method];
            if(strcmp(candidate->name,wanted->name)!=0||wanted->arity<candidate->required_arity||
               wanted->arity>candidate->arity||candidate->function_index>=chunk->function_count||
               candidate->source_chunk!=nullptr)continue;
            const DiamondFunction *implementation=chunk->functions[candidate->function_index];
            if(implementation->type_variable_count>8||
               (implementation->class_type_variable!=0&&
                implementation->class_type_variable!=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)))continue;
            DiamondTypeBinding specialized[8]={};
            const size_t receiver_count=implementation->class_type_variable==0?0:count;
            if(receiver_count>implementation->class_type_variable)continue;
            if(receiver_count>0)memcpy(specialized,bindings,receiver_count*sizeof *bindings);
            InterfaceWitness inferred[8]={};
            for(size_t p=0;p<wanted->arity;p++)
                interface_infer_variables(implementation,implementation->parameter_type_sets[p],
                    interface->type_sets,wanted->parameter_type_sets[p],inferred,0);
            DiamondChunk context=*chunk;
            /* Receiver slots are fixed; method slots come solely from this
             * interface's inputs, never an unrelated caller's variables. */
            context.type_variable_count=0;
            context.type_variable_bindings=nullptr;
            bool supported=true;
            for(size_t variable=implementation->class_type_variable;
                variable<implementation->type_variable_count&&supported;variable++) {
                if(!inferred[variable].inferred)continue;
                size_t nodes=0;
                supported=!inferred[variable].invalid&&
                    interface_members_supported(interface->type_sets,&inferred[variable].set,&nodes);
                if(!supported)break;
                (void)binding_node(&specialized[variable]);
                bind_context_members(&specialized[variable],0,&context,interface->type_sets,&inferred[variable].set);
                supported=!specialized[variable].invalid;
            }
            if(!supported)continue;
            context.type_variable_count=implementation->type_variable_count;
            context.type_variable_bindings=specialized;
            found=true;
            for(size_t parameter=0;parameter<wanted->arity&&found;parameter++) {
                const uint16_t needed=wanted->parameter_type_sets[parameter];
                const uint16_t actual=implementation->parameter_type_sets[parameter];
                if(needed==DIAMOND_NO_TYPE_SET) {found=actual==DIAMOND_NO_TYPE_SET;continue;}
                if(actual==DIAMOND_NO_TYPE_SET)continue;
                found=interface_signature_resolved(implementation,actual,context.type_variable_bindings,
                    context.type_variable_count,0)&&runtime_set_satisfies(&context,
                    interface->type_sets,needed,implementation->type_sets,actual);
            }
            if(found&&wanted->return_type_set!=DIAMOND_NO_TYPE_SET)
                found=implementation->return_type_set!=DIAMOND_NO_TYPE_SET&&
                    interface_signature_resolved(implementation,implementation->return_type_set,
                        context.type_variable_bindings,context.type_variable_count,0)&&
                    runtime_set_satisfies(&context,implementation->type_sets,
                        implementation->return_type_set,interface->type_sets,wanted->return_type_set);
        }
        matches=found;
    }
    runtime_generic_interface_depth--;return matches;
}

static bool generic_member_implements(const DiamondChunk *chunk,const DiamondTypeSet *sets,
        DiamondTypeMember member,uint16_t expected) {
    const size_t index=member.id-DIAMOND_TYPE_CLASS_BASE;
    const size_t interface_index=expected-DIAMOND_TYPE_INTERFACE_BASE;
    if(index>=chunk->class_count||interface_index>=chunk->interface_count)return false;
    const DiamondClass *class=chunk->classes[index];
    DiamondTypeBinding bindings[2]={};
    const uint16_t arguments[2]={member.argument_set,member.second_argument_set};
    const size_t count=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class);
    size_t bound=0;
    for(size_t variable=0;variable<count;variable++) {
        if(arguments[variable]==DIAMOND_NO_TYPE_SET)break;
        if(!runtime_set_graph_supported(sets,arguments[variable],0))return false;
        (void)binding_node(&bindings[variable]);
        bind_context_set(&bindings[variable],0,chunk,sets,arguments[variable]);
        if(bindings[variable].invalid)return false;
        bound++;
    }
    return generic_class_implements(chunk,class,bindings,bound,chunk->interfaces[interface_index]);
}

bool value_matches_type(const DiamondChunk *chunk, DiamondValue value,
                               uint16_t type) {
    /* A bare id can't say which native kind; any native object will do.
     * (Members check the kind -- see value_matches_member.) */
    if(type==DIAMOND_TYPE_NATIVE) {
        if(value.kind!=DIAMOND_VALUE_OBJECT)return false;
        const char *name=diamond_native_type_name((uint8_t)value.as.object->kind);
        return strcmp(name,"<native>")!=0;
    }
    if(type>=DIAMOND_TYPE_VARIABLE_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(type-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr)return true;
        return value_matches_bound_node(chunk,value,
            &chunk->type_variable_bindings[variable],0);
    }
    const int plain=diamond_plain_type_match(value,type);
    if(plain>=0)return plain!=0;
    if(type==DIAMOND_TYPE_CALLABLE) return value.kind==DIAMOND_VALUE_OBJECT &&
        value.as.object->kind==DIAMOND_OBJECT_CLOSURE;
    if(type==DIAMOND_TYPE_SIZED) {
        if(value.kind!=DIAMOND_VALUE_OBJECT)return false;
        if(value.as.object->kind==DIAMOND_OBJECT_STRING||
           value.as.object->kind==DIAMOND_OBJECT_ARRAY||
           value.as.object->kind==DIAMOND_OBJECT_HASH)return true;
        if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
        const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
        while(class!=nullptr) {
            for(size_t index=0;index<class->method_count;index++)
                if(strcmp(class->methods[index].name,"length")==0&&
                   class->methods[index].arity==0)return true;
            class=class->superclass==DIAMOND_NO_CLASS?nullptr:
                chunk->classes[class->superclass];
        }
        return false;
    }
    if(type>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(type-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=chunk->interface_count||value.kind!=DIAMOND_VALUE_OBJECT)
            return false;
        const DiamondInterface *interface=chunk->interfaces[interface_index];
        uint16_t builtin=DIAMOND_TYPE_NONE;
        if(value.as.object->kind==DIAMOND_OBJECT_STRING)builtin=DIAMOND_TYPE_STRING;
        else if(value.as.object->kind==DIAMOND_OBJECT_ARRAY)builtin=DIAMOND_TYPE_ARRAY;
        else if(value.as.object->kind==DIAMOND_OBJECT_HASH)builtin=DIAMOND_TYPE_HASH;
        if(builtin!=DIAMOND_TYPE_NONE) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint16_t native_return=DIAMOND_TYPE_NONE;
                if(!diamond_native_method_satisfies(builtin,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=DIAMOND_NO_TYPE_SET) {
                    if(native_return==DIAMOND_TYPE_NONE)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
                        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,interface->type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;
            const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
            while(class!=nullptr&&!found) {
                if(class->type_parameter[0]!='\0') {
                    const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
                    return generic_class_implements(chunk,class,instance->type_binding,
                        instance->type_binding==nullptr?0:DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class),interface);
                }
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=
                            &interface->methods[required];
                        const DiamondFunction *implementation=
                            chunk->functions[class->methods[method].function_index];
                        if(implementation->type_variable_count>0) {
                            const DiamondInterface single={.methods=interface->methods+required,.method_count=1,
                                .type_sets=interface->type_sets};
                            found=generic_class_implements(chunk,class,nullptr,0,&single);
                            if(found)break;
                            continue;
                        }
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint16_t required_set=wanted->parameter_type_sets[parameter];
                            const uint16_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==DIAMOND_NO_TYPE_SET) {
                                if(actual_set!=DIAMOND_NO_TYPE_SET)found=false;
                            } else if(actual_set!=DIAMOND_NO_TYPE_SET&&
                                !runtime_set_satisfies(chunk,interface->type_sets,
                                    required_set,implementation->type_sets,
                                    actual_set))found=false;
                        }
                        if(wanted->return_type_set!=DIAMOND_NO_TYPE_SET&&
                           (implementation->return_type_set==DIAMOND_NO_TYPE_SET||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,interface->type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                class=class->superclass==DIAMOND_NO_CLASS?nullptr:
                    chunk->classes[class->superclass];
            }
            if(!found)return false;
        }
        return true;
    }
    const size_t class_index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
    if(class_index>=chunk->class_count || value.kind!=DIAMOND_VALUE_OBJECT ||
       value.as.object->kind!=DIAMOND_OBJECT_INSTANCE) return false;
    const DiamondClass *wanted=chunk->classes[class_index];
    const DiamondClass *actual=((DiamondInstance *)value.as.object)->class;
    while(actual!=nullptr) {
        if(actual==wanted) return true;
        actual=actual->superclass==DIAMOND_NO_CLASS?nullptr:chunk->classes[actual->superclass];
    }
    return false;
}

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint16_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint16_t expected_index);

static bool runtime_type_id_satisfies(const DiamondChunk *chunk,uint16_t known,
                                      uint16_t expected) {
    if(known==expected)return true;
    if(expected>=DIAMOND_TYPE_VARIABLE_BASE&&
       expected<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(expected-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr||
           chunk->type_variable_bindings[variable].node_count==0)return true;
        const DiamondBoundTypeNode *root=
            &chunk->type_variable_bindings[variable].nodes[0];
        for(size_t index=0;index<root->count;index++)
            if(runtime_type_id_satisfies(chunk,known,
               root->members[index].id))return true;
        return false;
    }
    if(known>=DIAMOND_TYPE_VARIABLE_BASE&&known<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(known-DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=chunk->type_variable_count||
           chunk->type_variable_bindings==nullptr||
           chunk->type_variable_bindings[variable].node_count==0)return true;
        const DiamondBoundTypeNode *root=
            &chunk->type_variable_bindings[variable].nodes[0];
        for(size_t index=0;index<root->count;index++)
            if(!runtime_type_id_satisfies(chunk,root->members[index].id,expected))
                return false;
        return true;
    }
    if(expected==DIAMOND_TYPE_SIZED) {
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH)return true;
        if(known<DIAMOND_TYPE_CLASS_BASE)return false;
        size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
        while(index<chunk->class_count) {
            const DiamondClass *class=chunk->classes[index];
            for(size_t method=0;method<class->method_count;method++)
                if(strcmp(class->methods[method].name,"length")==0&&
                   class->methods[method].arity==0)return true;
            if(class->superclass==DIAMOND_NO_CLASS)break;
            index=class->superclass;
        }
        return false;
    }
    if(expected>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(expected-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=chunk->interface_count)return false;
        const DiamondInterface *interface=chunk->interfaces[interface_index];
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint16_t native_return=DIAMOND_TYPE_NONE;
                if(!diamond_native_method_satisfies(known,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=DIAMOND_NO_TYPE_SET) {
                    if(native_return==DIAMOND_TYPE_NONE)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
                        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
                    if(!runtime_set_satisfies(chunk,&native,0,interface->type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(known<DIAMOND_TYPE_CLASS_BASE||known>=DIAMOND_TYPE_INTERFACE_BASE)
            return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
            while(index<chunk->class_count&&!found) {
                const DiamondClass *class=chunk->classes[index];
                if(class->type_parameter[0]!='\0')
                    return generic_class_implements(chunk,class,nullptr,0,interface);
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
                        const DiamondInterfaceMethod *wanted=&interface->methods[required];
                        const DiamondFunction *implementation=
                            chunk->functions[class->methods[method].function_index];
                        if(implementation->type_variable_count>0) {
                            const DiamondInterface single={.methods=interface->methods+required,.method_count=1,
                                .type_sets=interface->type_sets};
                            found=generic_class_implements(chunk,class,nullptr,0,&single);
                            if(found)break;
                            continue;
                        }
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint16_t required_set=wanted->parameter_type_sets[parameter];
                            const uint16_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==DIAMOND_NO_TYPE_SET) {
                                if(actual_set!=DIAMOND_NO_TYPE_SET)found=false;
                            } else if(actual_set!=DIAMOND_NO_TYPE_SET&&
                                !runtime_set_satisfies(chunk,interface->type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=DIAMOND_NO_TYPE_SET&&
                           (implementation->return_type_set==DIAMOND_NO_TYPE_SET||
                            !runtime_set_satisfies(chunk,implementation->type_sets,
                                implementation->return_type_set,interface->type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                if(found||class->superclass==DIAMOND_NO_CLASS)break;
                index=class->superclass;
            }
            if(!found)return false;
        }
        return true;
    }
    if(known<DIAMOND_TYPE_CLASS_BASE||expected<DIAMOND_TYPE_CLASS_BASE)return false;
    size_t index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
    const size_t wanted=(size_t)(expected-DIAMOND_TYPE_CLASS_BASE);
    while(index<chunk->class_count) {
        if(index==wanted)return true;
        const uint16_t parent=chunk->classes[index]->superclass;
        if(parent==DIAMOND_NO_CLASS)break;
        index=parent;
    }
    return false;
}

/* A bound variable may stand for a complete class/container graph, not just
 * its root id. Callback variance checks must substitute that graph as well. */
static bool runtime_bound_has_arguments(const DiamondChunk *chunk,uint16_t id) {
    if(id<DIAMOND_TYPE_VARIABLE_BASE||id>=DIAMOND_TYPE_INTERFACE_BASE||
       chunk->type_variable_bindings==nullptr)return false;
    const size_t variable=id-DIAMOND_TYPE_VARIABLE_BASE;
    if(variable>=chunk->type_variable_count)return false;
    const DiamondTypeBinding *graph=&chunk->type_variable_bindings[variable];
    if(graph->invalid||graph->node_count==0)return false;
    if(graph->nodes[0].count>1)return true;
    for(size_t i=0;i<graph->nodes[0].count;i++)
        if(graph->nodes[0].members[i].argument_node!=UINT8_MAX||
           graph->nodes[0].members[i].id==DIAMOND_TYPE_NATIVE||
           graph->nodes[0].members[i].signature!=UINT8_MAX)return true;
    return false;
}

static bool runtime_graph_satisfies(const DiamondChunk *chunk,
        const DiamondTypeBinding *known,uint8_t a,
        const DiamondTypeBinding *expected,uint8_t b,size_t depth) {
    if(known->invalid||expected->invalid||depth>=DIAMOND_BOUND_TYPE_NODES||
       a>=known->node_count||b>=expected->node_count)return false;
    if(expected->nodes[b].count==0)return true;
    if(known->nodes[a].count==0)return false;
    for(size_t i=0;i<known->nodes[a].count;i++) {
        const DiamondBoundTypeMember actual=known->nodes[a].members[i];
        bool accepted=false;
        for(size_t j=0;j<expected->nodes[b].count&&!accepted;j++) {
            const DiamondBoundTypeMember wanted=expected->nodes[b].members[j];
            if(wanted.id>=DIAMOND_TYPE_INTERFACE_BASE&&actual.id>=DIAMOND_TYPE_CLASS_BASE&&
               actual.id<DIAMOND_TYPE_CLASS_LIMIT&&(size_t)(actual.id-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count&&
               DIAMOND_CLASS_TYPE_PARAMETER_COUNT(chunk->classes[actual.id-DIAMOND_TYPE_CLASS_BASE])>0) {
                const size_t interface_index=wanted.id-DIAMOND_TYPE_INTERFACE_BASE;
                if(interface_index>=chunk->interface_count)continue;
                const DiamondClass *class=chunk->classes[actual.id-DIAMOND_TYPE_CLASS_BASE];
                DiamondTypeBinding arguments[2]={};size_t count=0;
                const uint8_t children[2]={actual.argument_node,actual.second_argument_node};
                for(size_t variable=0;variable<DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class);variable++) {
                    if(children[variable]==UINT8_MAX)break;
                    (void)binding_node(&arguments[variable]);
                    bind_bound_node(&arguments[variable],0,known,children[variable]);count++;
                }
                accepted=generic_class_implements(chunk,class,arguments,count,chunk->interfaces[interface_index]);
                continue;
            }
            if(!runtime_type_id_satisfies(chunk,actual.id,wanted.id))continue;
            if(wanted.id==DIAMOND_TYPE_NATIVE) {
                accepted=wanted.native_kind==UINT8_MAX||actual.native_kind==wanted.native_kind;
                continue;
            }
            if(wanted.id==DIAMOND_TYPE_CALLABLE) {
                if(wanted.signature==UINT8_MAX) {accepted=true;continue;}
                if(actual.signature==UINT8_MAX||actual.signature>=known->signature_count||
                   wanted.signature>=expected->signature_count)continue;
                const DiamondBoundSignature *x=&known->signatures[actual.signature],
                    *y=&expected->signatures[wanted.signature];
                if(y->arity!=UINT8_MAX&&(x->required_arity>y->required_arity||
                   (!x->variadic&&(y->variadic||x->arity<y->arity))))continue;
                bool matches=true;
                if(y->parameters_typed)
                    for(size_t p=0;p<y->arity&&matches;p++) {
                        if(y->parameter_nodes[p]==UINT8_MAX)continue;
                        if(x->parameters_typed&&p>=x->arity) {matches=false;continue;}
                        if(x->parameters_typed&&x->parameter_nodes[p]!=UINT8_MAX)
                            matches=runtime_graph_satisfies(chunk,expected,y->parameter_nodes[p],
                                known,x->parameter_nodes[p],depth+1);
                    }
                if(matches&&y->return_node!=UINT8_MAX)
                    matches=x->return_node!=UINT8_MAX&&runtime_graph_satisfies(chunk,known,
                        x->return_node,expected,y->return_node,depth+1);
                if(matches)accepted=true;
                continue;
            }
            if(wanted.argument_node==UINT8_MAX) {accepted=true;continue;}
            if(actual.argument_node==UINT8_MAX||
               !runtime_graph_satisfies(chunk,known,actual.argument_node,expected,wanted.argument_node,depth+1))continue;
            const bool invariant=wanted.id>=DIAMOND_TYPE_CLASS_BASE&&wanted.id<DIAMOND_TYPE_CLASS_LIMIT;
            if(invariant&&!runtime_graph_satisfies(chunk,expected,wanted.argument_node,known,actual.argument_node,depth+1))continue;
            if(wanted.second_argument_node!=UINT8_MAX&&
               (actual.second_argument_node==UINT8_MAX||
                !runtime_graph_satisfies(chunk,known,actual.second_argument_node,expected,wanted.second_argument_node,depth+1)||
                (invariant&&!runtime_graph_satisfies(chunk,expected,wanted.second_argument_node,known,actual.second_argument_node,depth+1))))continue;
            accepted=true;
        }
        if(!accepted)return false;
    }
    return true;
}

static bool runtime_set_graph_supported(const DiamondTypeSet *sets,uint16_t index,size_t depth) {
    if(depth>=DIAMOND_BOUND_TYPE_NODES)return false;
    const DiamondTypeSet *set=&sets[index];
    for(size_t i=0;i<set->count;i++) {
        const DiamondTypeMember member=set->members[i];
        /* Container alternatives still share a root; native kinds are
         * separate members, and Callable payloads have owned child nodes. */
        for(size_t prior=0;prior<i;prior++)if(set->members[prior].id==member.id&&
            (member.id!=DIAMOND_TYPE_NATIVE||set->members[prior].callable_arity==member.callable_arity))return false;
        if(member.callable_return_set!=DIAMOND_NO_TYPE_SET&&
           !runtime_set_graph_supported(sets,member.callable_return_set,depth+1))return false;
        if(member.callable_parameters_typed)
            for(size_t p=0;p<member.callable_arity;p++)
                if(member.callable_parameter_sets[p]!=DIAMOND_NO_TYPE_SET&&
                   !runtime_set_graph_supported(sets,member.callable_parameter_sets[p],depth+1))return false;
        if(member.argument_set!=DIAMOND_NO_TYPE_SET&&
           !runtime_set_graph_supported(sets,member.argument_set,depth+1))return false;
        if(member.second_argument_set!=DIAMOND_NO_TYPE_SET&&
           !runtime_set_graph_supported(sets,member.second_argument_set,depth+1))return false;
    }
    return true;
}

static bool runtime_member_satisfies(const DiamondChunk *chunk,
                                     const DiamondTypeSet *known_sets,
                                     DiamondTypeMember known,
                                     const DiamondTypeSet *expected_sets,
                                     DiamondTypeMember expected) {
    if(known.id==DIAMOND_TYPE_NATIVE&&expected.id==DIAMOND_TYPE_NATIVE)
        return known.callable_arity==expected.callable_arity;
    if(expected.id>=DIAMOND_TYPE_INTERFACE_BASE&&known.id>=DIAMOND_TYPE_CLASS_BASE&&
       known.id<DIAMOND_TYPE_CLASS_LIMIT&&(size_t)(known.id-DIAMOND_TYPE_CLASS_BASE)<chunk->class_count&&
       DIAMOND_CLASS_TYPE_PARAMETER_COUNT(chunk->classes[known.id-DIAMOND_TYPE_CLASS_BASE])>0)
        return generic_member_implements(chunk,known_sets,known,expected.id);
    if(!runtime_type_id_satisfies(chunk,known.id,expected.id))return false;
    if(expected.id==DIAMOND_TYPE_CALLABLE) {
        if(expected.callable_arity!=UINT8_MAX&&
           known.callable_arity!=expected.callable_arity)return false;
        if(expected.callable_parameters_typed)
            for(size_t parameter=0;parameter<expected.callable_arity;parameter++) {
                const uint16_t wanted=expected.callable_parameter_sets[parameter];
                const uint16_t actual=known.callable_parameter_sets[parameter];
                if(actual!=DIAMOND_NO_TYPE_SET&&
                   !runtime_set_satisfies(chunk,expected_sets,wanted,
                                           known_sets,actual))return false;
            }
        return expected.callable_return_set==DIAMOND_NO_TYPE_SET||
            (known.callable_return_set!=DIAMOND_NO_TYPE_SET&&
             runtime_set_satisfies(chunk,known_sets,known.callable_return_set,
                                   expected_sets,expected.callable_return_set));
    }
    if(expected.argument_set==DIAMOND_NO_TYPE_SET)return true;
    if(known.argument_set==DIAMOND_NO_TYPE_SET||
       !runtime_set_satisfies(chunk,known_sets,known.argument_set,
                              expected_sets,expected.argument_set))return false;
    if(expected.id>=DIAMOND_TYPE_CLASS_BASE&&expected.id<DIAMOND_TYPE_CLASS_LIMIT)
        return runtime_set_satisfies(chunk,expected_sets,expected.argument_set,
            known_sets,known.argument_set)&&
            (expected.second_argument_set==DIAMOND_NO_TYPE_SET||
             (known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
              runtime_set_satisfies(chunk,known_sets,known.second_argument_set,expected_sets,expected.second_argument_set)&&
              runtime_set_satisfies(chunk,expected_sets,expected.second_argument_set,known_sets,known.second_argument_set)));
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        expected.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        runtime_set_satisfies(chunk,known_sets,known.second_argument_set,
                              expected_sets,expected.second_argument_set);
}

static bool runtime_set_satisfies(const DiamondChunk *chunk,
                                  const DiamondTypeSet *known_sets,
                                  uint16_t known_index,
                                  const DiamondTypeSet *expected_sets,
                                  uint16_t expected_index) {
    bool expand=false;
    for(size_t i=0;i<known_sets[known_index].count;i++)
        expand|=runtime_bound_has_arguments(chunk,known_sets[known_index].members[i].id);
    for(size_t i=0;i<expected_sets[expected_index].count;i++)
        expand|=runtime_bound_has_arguments(chunk,expected_sets[expected_index].members[i].id);
    if(expand) {
        if(!runtime_set_graph_supported(known_sets,known_index,0)||
           !runtime_set_graph_supported(expected_sets,expected_index,0))return false;
        DiamondTypeBinding known_graph={.node_count=1},expected_graph={.node_count=1};
        bind_context_set(&known_graph,0,chunk,known_sets,known_index);
        bind_context_set(&expected_graph,0,chunk,expected_sets,expected_index);
        return !known_graph.invalid&&!expected_graph.invalid&&
            runtime_graph_satisfies(chunk,&known_graph,0,&expected_graph,0,0);
    }
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=runtime_member_satisfies(chunk,known_sets,
                known->members[source],expected_sets,expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

/* Whether a container that already carries a constraint (its element/key/
 * value type sets, resolved against `held_sets`) needs no further one for
 * `wanted_set` in `chunk`: everything the held constraint admits, the
 * wanted one admits too, so recording it would add nothing. Without this,
 * every distinct function that declares the same `Array[String]` (each has
 * its own type_sets table, so the exact-match test in the attach code
 * never sees them as the same) took one of the container's four slots,
 * and passing one array down a chain of five such functions failed with
 * "expected Array[String], got Array[String]". Only compared when neither
 * side has type variables (their meaning depends on per-call bindings)
 * and both resolve class/interface indexes against the same tables. */
static bool constraint_already_implies(const DiamondChunk *chunk,
        const DiamondTypeSet *held_sets,size_t held_set_count,uint16_t held_set,
        DiamondClass *const *held_classes,DiamondInterface *const *held_interfaces,
        size_t held_type_variable_count,uint16_t wanted_set) {
    if(held_type_variable_count!=0||chunk->type_variable_count!=0)return false;
    if(held_classes!=chunk->classes||held_interfaces!=chunk->interfaces)return false;
    if((size_t)held_set>=held_set_count||(size_t)wanted_set>=chunk->type_set_count)
        return false;
    return runtime_set_satisfies(chunk,held_sets,held_set,chunk->type_sets,wanted_set);
}

static bool value_matches_member(const DiamondChunk *chunk,DiamondValue value,
                                 const DiamondTypeMember *member,bool attach) {
    if(member->id==DIAMOND_TYPE_NATIVE)
        return value_is_native_kind(value,member->callable_arity);
    if(!value_matches_type(chunk,value,member->id))return false;
    if(member->id>=DIAMOND_TYPE_CLASS_BASE&&member->id<DIAMOND_TYPE_VARIABLE_BASE&&
       member->argument_set!=DIAMOND_NO_TYPE_SET) {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        DiamondTypeBinding wanted;
        return instance->type_binding!=nullptr&&
            bind_explicit_class_type(chunk,member->argument_set,&wanted)&&
            bound_nodes_equal(instance->type_binding,0,&wanted,0,0)&&
            (member->second_argument_set==DIAMOND_NO_TYPE_SET||
             (DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class)==2&&
              bind_explicit_class_type(chunk,member->second_argument_set,&wanted)&&
              bound_nodes_equal(&instance->type_binding[1],0,&wanted,0,0)));
    }
    if(member->id==DIAMOND_TYPE_CALLABLE) {
        const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
        /* A compile_method result is scoped to define_method, not a
         * general typed Callable -- see DIAMOND_OP_CALL_CLOSURE's own
         * comment; its function_index means nothing against `chunk`. */
        if(closure->foreign_chunk!=nullptr)return false;
        if(closure->function_index>=chunk->function_count)return false;
        const DiamondFunction *function=chunk->functions[closure->function_index];
        /* function->arity/required_arity count an implicit self slot for
         * some owner_class shapes (see diamond_function_self_offset's own
         * comment, src/vm.h) -- a *value* satisfying Callable[N] only ever
         * supplies N real, self-less arguments, so that offset must come
         * out here before comparing against member->callable_arity. */
        /* A zero-parameter block has no parameters to check: it ignores
         * whatever it is called with (diamond_block_ignores_arguments). */
        const bool ignores_arguments=diamond_block_ignores_arguments(closure,function);
        const uint8_t self_offset=diamond_function_self_offset(function);
        const uint8_t declared_arity=(uint8_t)(function->arity-self_offset);
        const uint8_t declared_required_arity=
            (uint8_t)(function->required_arity-self_offset);
        if(!ignores_arguments&&member->callable_arity!=UINT8_MAX&&
           (member->callable_arity<declared_required_arity||
            (member->callable_arity>declared_arity&&!function->has_variadic)))
            return false;
        if(!ignores_arguments&&member->callable_parameters_typed)
            for(size_t parameter=0;parameter<member->callable_arity;parameter++) {
                const uint16_t actual=function->parameter_type_sets[parameter];
                if(actual!=DIAMOND_NO_TYPE_SET&&
                   !runtime_set_satisfies(chunk,chunk->type_sets,
                       member->callable_parameter_sets[parameter],
                       function->type_sets,actual))return false;
            }
        return member->callable_return_set==DIAMOND_NO_TYPE_SET||
            ((size_t)member->callable_return_set<chunk->type_set_count&&
             function->return_type_set!=DIAMOND_NO_TYPE_SET&&
             (size_t)function->return_type_set<function->type_set_count&&
             runtime_set_satisfies(chunk,function->type_sets,
                function->return_type_set,chunk->type_sets,
                member->callable_return_set));
    }
    if(member->argument_set==DIAMOND_NO_TYPE_SET)return true;
    if((size_t)member->argument_set>=chunk->type_set_count)return false;
    if(member->id==DIAMOND_TYPE_ARRAY) {
        DiamondArray *array=(DiamondArray *)value.as.object;
        for(size_t index=0;index<array->count;index++)
            if(!value_matches_set(chunk,array->values[index],member->argument_set,false))
                return false;
        if(!attach)return true;
        for(size_t index=0;index<array->count;index++)
            if(!value_matches_set(chunk,array->values[index],member->argument_set,true))
                return false;
        for(size_t index=0;index<array->constraint_count;index++)
            if(array->constraints[index].type_sets==chunk->type_sets&&
               array->constraints[index].set_index==member->argument_set)return true;
        for(size_t index=0;index<array->constraint_count;index++)
            if(constraint_already_implies(chunk,array->constraints[index].type_sets,
                   array->constraints[index].type_set_count,
                   array->constraints[index].set_index,
                   array->constraints[index].classes,
                   array->constraints[index].interfaces,
                   array->constraints[index].type_variable_count,
                   member->argument_set))return true;
        if(array->constraint_count==4)return false;
        array->constraints[array->constraint_count++]=(typeof(array->constraints[0])){
            .type_sets=chunk->type_sets,.type_set_count=chunk->type_set_count,
            .set_index=member->argument_set,.classes=chunk->classes,
            .class_count=chunk->class_count,.interfaces=chunk->interfaces,
            .interface_count=chunk->interface_count,
            .type_variable_count=chunk->type_variable_count};
        typeof(array->constraints[0]) *constraint=
            &array->constraints[array->constraint_count-1];
        if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr) {
            constraint->type_variable_bindings=malloc(
                chunk->type_variable_count*sizeof(DiamondTypeBinding));
            if(constraint->type_variable_bindings==nullptr) {
                array->constraint_count--;return false;
            }
            memcpy(constraint->type_variable_bindings,chunk->type_variable_bindings,
                   chunk->type_variable_count*sizeof(DiamondTypeBinding));
        }
        return true;
    }
    if(member->id!=DIAMOND_TYPE_HASH||member->second_argument_set==DIAMOND_NO_TYPE_SET||
       (size_t)member->second_argument_set>=chunk->type_set_count)return false;
    DiamondHash *hash=(DiamondHash *)value.as.object;
    for(size_t index=0;index<hash->count;index++)
        if(!value_matches_set(chunk,hash->entries[index].key,member->argument_set,false)||
           !value_matches_set(chunk,hash->entries[index].value,
                              member->second_argument_set,false))return false;
    if(!attach)return true;
    for(size_t index=0;index<hash->count;index++)
        if(!value_matches_set(chunk,hash->entries[index].key,member->argument_set,true)||
           !value_matches_set(chunk,hash->entries[index].value,
                              member->second_argument_set,true))return false;
    for(size_t index=0;index<hash->constraint_count;index++)
        if(hash->constraints[index].type_sets==chunk->type_sets&&
           hash->constraints[index].key_set==member->argument_set&&
           hash->constraints[index].value_set==member->second_argument_set)return true;
    for(size_t index=0;index<hash->constraint_count;index++)
        if(constraint_already_implies(chunk,hash->constraints[index].type_sets,
               hash->constraints[index].type_set_count,
               hash->constraints[index].key_set,
               hash->constraints[index].classes,
               hash->constraints[index].interfaces,
               hash->constraints[index].type_variable_count,
               member->argument_set)&&
           constraint_already_implies(chunk,hash->constraints[index].type_sets,
               hash->constraints[index].type_set_count,
               hash->constraints[index].value_set,
               hash->constraints[index].classes,
               hash->constraints[index].interfaces,
               hash->constraints[index].type_variable_count,
               member->second_argument_set))return true;
    if(hash->constraint_count==4)return false;
    hash->constraints[hash->constraint_count++]=(typeof(hash->constraints[0])){
        .type_sets=chunk->type_sets,.type_set_count=chunk->type_set_count,
        .key_set=member->argument_set,.value_set=member->second_argument_set,
        .classes=chunk->classes,.class_count=chunk->class_count};
    hash->constraints[hash->constraint_count-1].interfaces=chunk->interfaces;
    hash->constraints[hash->constraint_count-1].interface_count=chunk->interface_count;
    hash->constraints[hash->constraint_count-1].type_variable_count=
        chunk->type_variable_count;
    if(chunk->type_variable_count>0&&chunk->type_variable_bindings!=nullptr) {
        hash->constraints[hash->constraint_count-1].type_variable_bindings=malloc(
            chunk->type_variable_count*sizeof(DiamondTypeBinding));
        if(hash->constraints[hash->constraint_count-1].
           type_variable_bindings==nullptr) {
            hash->constraint_count--;return false;
        }
        memcpy(hash->constraints[hash->constraint_count-1].type_variable_bindings,
               chunk->type_variable_bindings,
               chunk->type_variable_count*sizeof(DiamondTypeBinding));
    }
    return true;
}

/* -1 requires the full matcher. Handle each member in declaration order:
 * a preceding generic member may attach constraints before a plain fallback. */
static inline int plain_member_match(DiamondValue value,
                                    const DiamondTypeMember *member) {
    if (member->argument_set != DIAMOND_NO_TYPE_SET ||
        member->second_argument_set != DIAMOND_NO_TYPE_SET) return -1;
    return diamond_plain_type_match(value, member->id);
}

/* Keep structural calls out of the plain matcher's register lifetimes.
 * Start at the first nonplain member, preserving generic attachment before
 * any later plain fallback. Runtime type sets remain read-only. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static bool value_matches_set_structural(const DiamondChunk *chunk,DiamondValue value,
        const DiamondTypeSet *set,size_t start,bool attach) {
    for(size_t index=start;index<set->count;index++) {
        const DiamondTypeMember *member=&set->members[index];
        const int plain=plain_member_match(value,member);
        if(plain>=0) {
            if(plain)return true;
        } else if(value_matches_member(chunk,value,member,attach))return true;
    }
    return false;
}

bool value_matches_set(const DiamondChunk *chunk,DiamondValue value,
                              uint16_t set_index,bool attach) {
    if((size_t)set_index>=chunk->type_set_count)return false;
    const DiamondTypeSet *set=&chunk->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const int plain=plain_member_match(value,&set->members[index]);
        if(plain<0)return value_matches_set_structural(chunk,value,set,index,attach);
        if(plain)return true;
    }
    return false;
}

/* JIT trampoline for DIAMOND_OP_CHECK_TYPE -- see jit.h's own comment.
 * value_matches_set's own structural/generic matching logic (interfaces,
 * type variables, unions) is too deep to safely hand-roll in machine
 * code; this is a thin wrapper translating its bool result into the
 * DiamondVmStatus every other JIT trampoline already returns. */
DiamondVmStatus diamond_jit_check_type(const DiamondChunk *chunk,
        const DiamondValue *value, uint16_t set_index) {
    if (set_index >= chunk->type_set_count) return DIAMOND_VM_INVALID_BYTECODE;
    return value_matches_set(chunk, *value, set_index, true) ? DIAMOND_VM_OK : DIAMOND_VM_TYPE_ERROR;
}

/* JIT trampoline for DIAMOND_OP_IS_TYPE -- see jit.h's own comment. Same
 * shape as the interpreter's own DIAMOND_OP_IS_TYPE case (registers[dest]
 * = value_matches_type(...)), just through the trampoline's out-pointer
 * convention instead of a direct register write. */
DiamondVmStatus diamond_jit_is_type(const DiamondChunk *chunk,
        const DiamondValue *value, uint16_t type, DiamondValue *out) {
    *out = DIAMOND_BOOL(value_matches_type(chunk, *value, type));
    return DIAMOND_VM_OK;
}

/* True when both values are instances of exactly the same class. */
bool diamond_values_same_class(DiamondValue left, DiamondValue right) {
    if (left.kind != DIAMOND_VALUE_OBJECT || right.kind != DIAMOND_VALUE_OBJECT ||
        left.as.object->kind != DIAMOND_OBJECT_INSTANCE ||
        right.as.object->kind != DIAMOND_OBJECT_INSTANCE)
        return false;
    return ((const DiamondInstance *)left.as.object)->class ==
        ((const DiamondInstance *)right.as.object)->class;
}

/* JIT trampoline for DIAMOND_OP_SAME_CLASS. */
DiamondVmStatus diamond_jit_same_class(const DiamondValue *left,
        const DiamondValue *right, DiamondValue *out) {
    *out = DIAMOND_BOOL(diamond_values_same_class(*left, *right));
    return DIAMOND_VM_OK;
}

static uint16_t runtime_value_type(const DiamondChunk *chunk,DiamondValue value) {
    if(value.kind==DIAMOND_VALUE_NIL)return DIAMOND_TYPE_NIL;
    if(value.kind==DIAMOND_VALUE_BOOL)return DIAMOND_TYPE_BOOL;
    if(value.kind==DIAMOND_VALUE_INT)return DIAMOND_TYPE_INT;
    if(value.kind==DIAMOND_VALUE_FLOAT)return DIAMOND_TYPE_FLOAT;
    /* A class value (`self` in a class singleton) carries no object. */
    if(value.kind!=DIAMOND_VALUE_OBJECT)return DIAMOND_TYPE_NONE;
    if(value.as.object->kind==DIAMOND_OBJECT_STRING)return DIAMOND_TYPE_STRING;
    if(value.as.object->kind==DIAMOND_OBJECT_ARRAY)return DIAMOND_TYPE_ARRAY;
    if(value.as.object->kind==DIAMOND_OBJECT_HASH)return DIAMOND_TYPE_HASH;
    if(value.as.object->kind==DIAMOND_OBJECT_CLOSURE)return DIAMOND_TYPE_CALLABLE;
    if(strcmp(diamond_native_type_name((uint8_t)value.as.object->kind),"<native>")!=0)return DIAMOND_TYPE_NATIVE;
    if(value.as.object->kind!=DIAMOND_OBJECT_INSTANCE)return DIAMOND_TYPE_NONE;
    const DiamondClass *class=((DiamondInstance *)value.as.object)->class;
    for(size_t index=0;index<chunk->class_count;index++)
        if(chunk->classes[index]==class)return (uint16_t)(DIAMOND_TYPE_CLASS_BASE+index);
    return DIAMOND_TYPE_NONE;
}

static DiamondBoundTypeMember *binding_member(DiamondTypeBinding *binding,
                                               uint8_t node,uint16_t id,uint8_t native_kind) {
    if(binding->invalid||node>=binding->node_count)return nullptr;
    DiamondBoundTypeNode *target=&binding->nodes[node];
    for(size_t index=0;index<target->count;index++)
        if(target->members[index].id==id&&
           (id!=DIAMOND_TYPE_NATIVE||target->members[index].native_kind==native_kind))return &target->members[index];
    if(target->count==DIAMOND_BOUND_TYPE_MEMBERS) {binding->invalid=true;return nullptr;}
    target->members[target->count]=(DiamondBoundTypeMember){.id=id,
        .argument_node=UINT8_MAX,.second_argument_node=UINT8_MAX,
        .native_kind=native_kind,.signature=UINT8_MAX};
    return &target->members[target->count++];
}

static void bind_bound_node(DiamondTypeBinding *target,uint8_t target_node,
                            const DiamondTypeBinding *source,uint8_t source_node);

/* All signature edges are node indexes in the binding itself. Repeated
 * incompatible Callable contracts fail closed instead of merging parameters
 * and returns independently and inventing a signature no value supplied. */
static void bind_callable_metadata(DiamondTypeBinding *binding,DiamondBoundTypeMember *member,
        const DiamondChunk *context,const DiamondTypeSet *sets,DiamondTypeMember known,
        uint8_t required_arity,bool variadic) {
    if(binding->invalid)return;
    if(member->signature!=UINT8_MAX) {
        DiamondTypeBinding incoming={.node_count=1};
        DiamondBoundTypeMember *root=binding_member(&incoming,0,DIAMOND_TYPE_CALLABLE,UINT8_MAX);
        bind_callable_metadata(&incoming,root,context,sets,known,required_arity,variadic);
        if(incoming.invalid||!bound_payload_equal(binding,*member,&incoming,*root,0))binding->invalid=true;
        return;
    }
    if(binding->signature_count==DIAMOND_BOUND_SIGNATURES||
       (known.callable_parameters_typed&&known.callable_arity>16)) {binding->invalid=true;return;}
    member->signature=binding->signature_count++;
    DiamondBoundSignature *signature=&binding->signatures[member->signature];
    *signature=(DiamondBoundSignature){.arity=known.callable_arity,
        .required_arity=required_arity,.variadic=variadic,
        .parameters_typed=known.callable_parameters_typed,.return_node=UINT8_MAX};
    for(size_t p=0;p<16;p++)signature->parameter_nodes[p]=UINT8_MAX;
    if(known.callable_parameters_typed)
        for(size_t p=0;p<known.callable_arity;p++) {
            if(known.callable_parameter_sets[p]==DIAMOND_NO_TYPE_SET)continue;
            signature->parameter_nodes[p]=binding_node(binding);
            if(signature->parameter_nodes[p]!=UINT8_MAX)
                bind_context_set(binding,signature->parameter_nodes[p],context,sets,known.callable_parameter_sets[p]);
        }
    if(known.callable_return_set!=DIAMOND_NO_TYPE_SET) {
        signature->return_node=binding_node(binding);
        if(signature->return_node!=UINT8_MAX)
            bind_context_set(binding,signature->return_node,context,sets,known.callable_return_set);
    }
}

static void bind_value_graph(const DiamondChunk *chunk,DiamondTypeBinding *binding,
                             uint8_t node,DiamondValue value) {
    const uint16_t type=runtime_value_type(chunk,value);
    /* A value with no type id (for example a class value) leaves the
     * variable unconstrained rather than binding it to nothing at all. */
    if(type==DIAMOND_TYPE_NONE)return;
    DiamondBoundTypeMember *member=binding_member(binding,node,type,
        type==DIAMOND_TYPE_NATIVE?(uint8_t)value.as.object->kind:UINT8_MAX);
    if(member==nullptr)return;
    if(type==DIAMOND_TYPE_CALLABLE) {
        const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
        if(closure->foreign_chunk!=nullptr||closure->function_index>=chunk->function_count) {
            binding->invalid=true;return;
        }
        const DiamondFunction *function=chunk->functions[closure->function_index];
        const uint8_t arity=(uint8_t)(function->arity-diamond_function_self_offset(function));
        if(arity>16) {binding->invalid=true;return;}
        const bool ignores=diamond_block_ignores_arguments(closure,function);
        DiamondTypeMember signature={.id=DIAMOND_TYPE_CALLABLE,
            .callable_arity=arity,.callable_parameters_typed=!ignores,
            .callable_return_set=function->return_type_set};
        for(size_t p=0;p<arity;p++)signature.callable_parameter_sets[p]=function->parameter_type_sets[p];
        /* Function-owned variables have no relation to the caller's slots. */
        const DiamondChunk context={};
        bind_callable_metadata(binding,member,&context,function->type_sets,signature,
            ignores?0:(uint8_t)(function->required_arity-diamond_function_self_offset(function)),
            ignores||function->has_variadic);
    } else if(type>=DIAMOND_TYPE_CLASS_BASE&&type<DIAMOND_TYPE_CLASS_LIMIT) {
        const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
        const size_t count=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class);
        if(count==0)return;
        if(instance->type_binding==nullptr) {binding->invalid=true;return;}
        for(size_t variable=0;variable<count;variable++) {
            uint8_t *child=variable==0?&member->argument_node:&member->second_argument_node;
            const DiamondTypeBinding *source=&instance->type_binding[variable];
            if(source->invalid||source->node_count==0||source->nodes[0].count==0) {
                binding->invalid=true;return;
            }
            if(*child!=UINT8_MAX) {
                if(!bound_nodes_equal(binding,*child,source,0,0))binding->invalid=true;
            } else {
                *child=binding_node(binding);
                if(*child!=UINT8_MAX)bind_bound_node(binding,*child,source,0);
            }
            if(binding->invalid)return;
        }
    } else if(type==DIAMOND_TYPE_ARRAY) {
        if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
        if(member->argument_node==UINT8_MAX)return;
        const DiamondArray *array=(const DiamondArray *)value.as.object;
        for(size_t index=0;index<array->count;index++)
            bind_value_graph(chunk,binding,member->argument_node,array->values[index]);
    } else if(type==DIAMOND_TYPE_HASH) {
        if(member->argument_node==UINT8_MAX)member->argument_node=binding_node(binding);
        if(member->second_argument_node==UINT8_MAX)
            member->second_argument_node=binding_node(binding);
        if(member->argument_node==UINT8_MAX||member->second_argument_node==UINT8_MAX)return;
        const DiamondHash *hash=(const DiamondHash *)value.as.object;
        for(size_t index=0;index<hash->count;index++) {
            bind_value_graph(chunk,binding,member->argument_node,hash->entries[index].key);
            bind_value_graph(chunk,binding,member->second_argument_node,
                             hash->entries[index].value);
        }
    }
}

static void bind_known_set(DiamondTypeBinding *binding,uint8_t node,
                           const DiamondTypeSet *sets,uint16_t set_index) {
    const DiamondChunk context={};
    bind_context_set(binding,node,&context,sets,set_index);
}

static void bind_bound_node(DiamondTypeBinding *target,uint8_t target_node,
                            const DiamondTypeBinding *source,uint8_t source_node) {
    if(source->invalid) {target->invalid=true;return;}
    if(target->invalid||source_node>=source->node_count)return;
    const DiamondBoundTypeNode *node=&source->nodes[source_node];
    for(size_t index=0;index<node->count;index++) {
        const DiamondBoundTypeMember known=node->members[index];
        if(known.id>=DIAMOND_TYPE_CLASS_BASE&&known.id<DIAMOND_TYPE_CLASS_LIMIT&&
           target_node<target->node_count) {
            const DiamondBoundTypeNode *held=&target->nodes[target_node];
            for(size_t existing=0;existing<held->count;existing++) {
                const DiamondBoundTypeMember prior=held->members[existing];
                if(prior.id!=known.id)continue;
                if(!bound_nodes_equal(target,prior.argument_node,source,known.argument_node,0)||
                   !bound_nodes_equal(target,prior.second_argument_node,source,known.second_argument_node,0)) {
                    target->invalid=true;return;
                }
            }
        }
        DiamondBoundTypeMember *member=
            binding_member(target,target_node,known.id,known.native_kind);
        if(member==nullptr)continue;
        if(known.id==DIAMOND_TYPE_CALLABLE) {
            if(member->signature!=UINT8_MAX) {
                if(!bound_payload_equal(target,*member,source,known,0))target->invalid=true;
                continue;
            }
            if(known.signature==UINT8_MAX)continue;
            if(known.signature>=source->signature_count||target->signature_count==DIAMOND_BOUND_SIGNATURES) {
                target->invalid=true;return;
            }
            const DiamondBoundSignature original=source->signatures[known.signature];
            member->signature=target->signature_count++;
            DiamondBoundSignature *signature=&target->signatures[member->signature];
            *signature=original;
            for(size_t p=0;p<16;p++)signature->parameter_nodes[p]=UINT8_MAX;
            signature->return_node=UINT8_MAX;
            if(original.parameters_typed)
                for(size_t p=0;p<original.arity;p++) {
                    if(original.parameter_nodes[p]==UINT8_MAX)continue;
                    signature->parameter_nodes[p]=binding_node(target);
                    if(signature->parameter_nodes[p]!=UINT8_MAX)
                        bind_bound_node(target,signature->parameter_nodes[p],source,original.parameter_nodes[p]);
                }
            if(original.return_node!=UINT8_MAX) {
                signature->return_node=binding_node(target);
                if(signature->return_node!=UINT8_MAX)
                    bind_bound_node(target,signature->return_node,source,original.return_node);
            }
        }
        if(known.argument_node!=UINT8_MAX) {
            if(member->argument_node==UINT8_MAX)
                member->argument_node=binding_node(target);
            if(member->argument_node!=UINT8_MAX)
                bind_bound_node(target,member->argument_node,source,
                                known.argument_node);
        }
        if(known.second_argument_node!=UINT8_MAX) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(target);
            if(member->second_argument_node!=UINT8_MAX)
                bind_bound_node(target,member->second_argument_node,source,
                                known.second_argument_node);
        }
    }
}

static void bind_context_members(DiamondTypeBinding *binding,uint8_t node,
    const DiamondChunk *context,const DiamondTypeSet *sets,const DiamondTypeSet *set) {
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember known=set->members[index];
        if(known.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           known.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(known.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable<context->type_variable_count&&
               context->type_variable_bindings!=nullptr)
                bind_bound_node(binding,node,
                    &context->type_variable_bindings[variable],0);
            continue;
        }
        if(known.id>=DIAMOND_TYPE_CLASS_BASE&&known.id<DIAMOND_TYPE_CLASS_LIMIT&&
           !binding->invalid&&node<binding->node_count) {
            for(size_t prior=0;prior<binding->nodes[node].count;prior++) {
                const DiamondBoundTypeMember held=binding->nodes[node].members[prior];
                if(held.id!=known.id)continue;
                DiamondTypeBinding incoming={.node_count=1};
                const uint16_t arguments[2]={known.argument_set,known.second_argument_set};
                uint8_t children[2]={UINT8_MAX,UINT8_MAX};
                for(size_t argument=0;argument<2;argument++)
                    if(arguments[argument]!=DIAMOND_NO_TYPE_SET) {
                        children[argument]=binding_node(&incoming);
                        if(children[argument]!=UINT8_MAX)
                            bind_context_set(&incoming,children[argument],context,sets,arguments[argument]);
                    }
                if(incoming.invalid||
                   !bound_nodes_equal(binding,held.argument_node,&incoming,children[0],0)||
                   !bound_nodes_equal(binding,held.second_argument_node,&incoming,children[1],0)) {
                    binding->invalid=true;return;
                }
            }
        }
        DiamondBoundTypeMember *member=binding_member(binding,node,known.id,known.id==DIAMOND_TYPE_NATIVE?known.callable_arity:UINT8_MAX);
        if(member==nullptr)continue;
        if(known.id==DIAMOND_TYPE_CALLABLE)
            bind_callable_metadata(binding,member,context,sets,known,known.callable_arity,false);
        if(known.argument_set!=DIAMOND_NO_TYPE_SET) {
            if(member->argument_node==UINT8_MAX)
                member->argument_node=binding_node(binding);
            if(member->argument_node!=UINT8_MAX)
                bind_context_set(binding,member->argument_node,context,sets,
                                 known.argument_set);
        }
        if(known.second_argument_set!=DIAMOND_NO_TYPE_SET) {
            if(member->second_argument_node==UINT8_MAX)
                member->second_argument_node=binding_node(binding);
            if(member->second_argument_node!=UINT8_MAX)
                bind_context_set(binding,member->second_argument_node,context,sets,
                                 known.second_argument_set);
        }
    }
}

void bind_context_set(DiamondTypeBinding *binding,uint8_t node,
    const DiamondChunk *context,const DiamondTypeSet *sets,uint16_t set_index) {
    bind_context_members(binding,node,context,sets,&sets[set_index]);
}

static void infer_from_context_set(const DiamondChunk *known_context,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t target=0;target<expected->count;target++) {
        const DiamondTypeMember wanted=expected->members[target];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=
                (uint8_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)
                (void)binding_node(&bindings[variable]);
            bind_context_set(&bindings[variable],0,known_context,known_sets,
                             known_index);
            continue;
        }
        for(size_t source=0;source<known->count;source++) {
            const DiamondTypeMember actual=known->members[source];
            if(actual.id!=wanted.id)continue;
            if(wanted.argument_set!=DIAMOND_NO_TYPE_SET&&actual.argument_set!=DIAMOND_NO_TYPE_SET)
                infer_from_context_set(known_context,known_sets,
                    actual.argument_set,expected_sets,wanted.argument_set,bindings);
            if(wanted.second_argument_set!=DIAMOND_NO_TYPE_SET&&
               actual.second_argument_set!=DIAMOND_NO_TYPE_SET)
                infer_from_context_set(known_context,known_sets,
                    actual.second_argument_set,expected_sets,
                    wanted.second_argument_set,bindings);
        }
    }
}

static void infer_from_known_set(const DiamondChunk *chunk,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t target=0;target<expected->count;target++) {
        const DiamondTypeMember wanted=expected->members[target];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=
                (uint8_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)
                (void)binding_node(&bindings[variable]);
            bind_known_set(&bindings[variable],0,known_sets,known_index);
        } else {
            for(size_t source=0;source<known->count;source++) {
                const DiamondTypeMember actual=known->members[source];
                if(actual.id!=wanted.id)continue;
                if(wanted.argument_set!=DIAMOND_NO_TYPE_SET&&actual.argument_set!=DIAMOND_NO_TYPE_SET)
                    infer_from_known_set(chunk,known_sets,actual.argument_set,
                        expected_sets,wanted.argument_set,bindings);
                if(wanted.second_argument_set!=DIAMOND_NO_TYPE_SET&&
                   actual.second_argument_set!=DIAMOND_NO_TYPE_SET)
                    infer_from_known_set(chunk,known_sets,actual.second_argument_set,
                        expected_sets,wanted.second_argument_set,bindings);
            }
        }
    }
    (void)chunk;
}

static void infer_from_bound_node(const DiamondTypeBinding *binding,uint8_t node,
        const DiamondTypeSet *sets,uint16_t set,DiamondTypeBinding bindings[8],size_t depth) {
    if(node>=binding->node_count||depth>=DIAMOND_BOUND_TYPE_NODES)return;
    for(size_t i=0;i<sets[set].count;i++) {
        const DiamondTypeMember expected=sets[set].members[i];
        if(expected.id>=DIAMOND_TYPE_VARIABLE_BASE&&expected.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=expected.id-DIAMOND_TYPE_VARIABLE_BASE;
            if(variable>=8)continue;
            if(bindings[variable].node_count==0)(void)binding_node(&bindings[variable]);
            bind_bound_node(&bindings[variable],0,binding,node);
        } else for(size_t j=0;j<binding->nodes[node].count;j++) {
            const DiamondBoundTypeMember actual=binding->nodes[node].members[j];
            if(actual.id!=expected.id)continue;
            if(expected.argument_set!=DIAMOND_NO_TYPE_SET)
                infer_from_bound_node(binding,actual.argument_node,sets,expected.argument_set,bindings,depth+1);
            if(expected.second_argument_set!=DIAMOND_NO_TYPE_SET)
                infer_from_bound_node(binding,actual.second_argument_node,sets,expected.second_argument_set,bindings,depth+1);
        }
    }
}

void infer_from_value(const DiamondChunk *chunk,DiamondValue value,
    const DiamondTypeSet *sets,uint16_t set_index,
    DiamondTypeBinding bindings[8]) {
    const DiamondTypeSet *set=&sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember member=set->members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const uint8_t variable=(uint8_t)(member.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(bindings[variable].node_count==0)(void)binding_node(&bindings[variable]);
            bind_value_graph(chunk,&bindings[variable],0,value);continue;
        }
        if(!value_matches_type(chunk,value,member.id))continue;
        if(member.id>=DIAMOND_TYPE_CLASS_BASE&&member.id<DIAMOND_TYPE_CLASS_LIMIT&&
           member.argument_set!=DIAMOND_NO_TYPE_SET) {
            const DiamondInstance *instance=(const DiamondInstance *)value.as.object;
            if(instance->type_binding!=nullptr) {
                infer_from_bound_node(instance->type_binding,0,sets,member.argument_set,bindings,0);
                if(member.second_argument_set!=DIAMOND_NO_TYPE_SET&&DIAMOND_CLASS_TYPE_PARAMETER_COUNT(instance->class)==2)
                    infer_from_bound_node(&instance->type_binding[1],0,sets,member.second_argument_set,bindings,0);
            }
            continue;
        }
        if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_set!=DIAMOND_NO_TYPE_SET) {
            const DiamondArray *array=(const DiamondArray *)value.as.object;
            for(size_t constraint=0;constraint<array->constraint_count;constraint++) {
                const typeof(array->constraints[0]) *known=
                    &array->constraints[constraint];
                const DiamondChunk context={.type_sets=known->type_sets,
                    .type_set_count=known->type_set_count,.classes=known->classes,
                    .class_count=known->class_count,.interfaces=known->interfaces,
                    .interface_count=known->interface_count,
                    .type_variable_bindings=known->type_variable_bindings,
                    .type_variable_count=known->type_variable_count};
                infer_from_context_set(&context,known->type_sets,known->set_index,
                    sets,member.argument_set,bindings);
            }
            for(size_t item=0;item<array->count;item++)
                infer_from_value(chunk,array->values[item],sets,
                                 member.argument_set,bindings);
        } else if(member.id==DIAMOND_TYPE_HASH&&member.argument_set!=DIAMOND_NO_TYPE_SET&&
                  member.second_argument_set!=DIAMOND_NO_TYPE_SET) {
            const DiamondHash *hash=(const DiamondHash *)value.as.object;
            for(size_t constraint=0;constraint<hash->constraint_count;constraint++) {
                const typeof(hash->constraints[0]) *known=
                    &hash->constraints[constraint];
                const DiamondChunk context={.type_sets=known->type_sets,
                    .type_set_count=known->type_set_count,.classes=known->classes,
                    .class_count=known->class_count,.interfaces=known->interfaces,
                    .interface_count=known->interface_count,
                    .type_variable_bindings=known->type_variable_bindings,
                    .type_variable_count=known->type_variable_count};
                infer_from_context_set(&context,known->type_sets,known->key_set,
                    sets,member.argument_set,bindings);
                infer_from_context_set(&context,known->type_sets,known->value_set,
                    sets,member.second_argument_set,bindings);
            }
            for(size_t item=0;item<hash->count;item++) {
                infer_from_value(chunk,hash->entries[item].key,sets,
                                 member.argument_set,bindings);
                infer_from_value(chunk,hash->entries[item].value,sets,
                                 member.second_argument_set,bindings);
            }
        } else if(member.id==DIAMOND_TYPE_CALLABLE) {
            const DiamondClosure *closure=(const DiamondClosure *)value.as.object;
            /* See value_matches_member's own comment on foreign_chunk. */
            if(closure->foreign_chunk==nullptr&&closure->function_index<chunk->function_count) {
                const DiamondFunction *function=chunk->functions[closure->function_index];
                if(member.callable_parameters_typed)
                    for(size_t parameter=0;parameter<member.callable_arity;parameter++) {
                        const uint16_t actual=function->parameter_type_sets[parameter];
                        if(actual!=DIAMOND_NO_TYPE_SET)
                            infer_from_known_set(chunk,function->type_sets,actual,
                                sets,member.callable_parameter_sets[parameter],bindings);
                    }
                if(function->return_type_set!=DIAMOND_NO_TYPE_SET)
                    if(member.callable_return_set!=DIAMOND_NO_TYPE_SET)
                    infer_from_known_set(chunk,function->type_sets,
                        function->return_type_set,sets,member.callable_return_set,
                        bindings);
            }
        }
    }
}
