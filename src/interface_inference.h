#ifndef DIAMOND_INTERFACE_INFERENCE_H
#define DIAMOND_INTERFACE_INFERENCE_H

#include <stdlib.h>

/* Method witnesses use the same bounded, owned graph as ordinary calls. */
static bool interface_binding_supported(const DiamondTypeSet *sets,uint16_t index,size_t *nodes);

static bool interface_members_supported(const DiamondTypeSet *sets,const DiamondTypeSet *set,
        size_t *nodes) {
    if(++*nodes>DIAMOND_BOUND_TYPE_NODES)return false;
    if(set->count==0||set->count>DIAMOND_BOUND_TYPE_MEMBERS)return false;
    for(size_t i=0;i<set->count;i++) {
        const DiamondTypeMember member=set->members[i];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&member.id<DIAMOND_TYPE_INTERFACE_BASE)return false;
        for(size_t j=0;j<i;j++)if(set->members[j].id==member.id&&
            (member.id!=DIAMOND_TYPE_NATIVE||set->members[j].callable_arity==member.callable_arity))return false;
        if(member.callable_return_set!=DIAMOND_NO_TYPE_SET&&
           !interface_binding_supported(sets,member.callable_return_set,nodes))return false;
        if(member.callable_parameters_typed)
            for(size_t p=0;p<member.callable_arity;p++)
                if(member.callable_parameter_sets[p]!=DIAMOND_NO_TYPE_SET&&
                   !interface_binding_supported(sets,member.callable_parameter_sets[p],nodes))return false;
        if(member.argument_set!=DIAMOND_NO_TYPE_SET&&
           !interface_binding_supported(sets,member.argument_set,nodes))return false;
        if(member.second_argument_set!=DIAMOND_NO_TYPE_SET&&
           !interface_binding_supported(sets,member.second_argument_set,nodes))return false;
    }
    return true;
}

static bool interface_binding_supported(const DiamondTypeSet *sets,uint16_t index,size_t *nodes) {
    return index!=DIAMOND_NO_TYPE_SET&&interface_members_supported(sets,&sets[index],nodes);
}

typedef struct InterfaceWitness {
    DiamondTypeBinding graph;
    bool inferred,invalid;
} InterfaceWitness;

static void interface_join_witness(InterfaceWitness *witness,const DiamondTypeSet *sets,uint16_t input) {
    witness->inferred=true;
    size_t nodes=0;DiamondTypeBinding incoming;
    if(!interface_binding_supported(sets,input,&nodes)||
       !diamond_bind_type_template(sets,DIAMOND_NO_TYPE_SET,input,&incoming)) {
        witness->invalid=true;return;
    }
    diamond_join_type_binding(&witness->graph,&incoming);
    if(witness->graph.invalid)witness->invalid=true;
}

/* Infer method-owned variables only from inputs the interface supplies. The
 * occurrences join into a union witness; the complete specialized signature
 * must subsequently pass variance checks. Return-only variables have no witness.
 * Receiver-owned variables are fixed and must never be rebound here. */
static void interface_infer_variables(const DiamondFunction *function,
        DiamondClass *const *classes,size_t class_count,uint16_t pattern,const DiamondTypeSet *inputs,uint16_t input,
        InterfaceWitness bindings[8],size_t depth) {
    if(pattern==DIAMOND_NO_TYPE_SET||input==DIAMOND_NO_TYPE_SET||
       pattern>=function->type_set_count||depth>32)return;
    const DiamondTypeSet *wanted=&function->type_sets[pattern];
    const DiamondTypeSet *actual=&inputs[input];
    for(size_t i=0;i<wanted->count;i++) {
        const DiamondTypeMember target=wanted->members[i];
        if(target.id>=DIAMOND_TYPE_VARIABLE_BASE&&target.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=target.id-DIAMOND_TYPE_VARIABLE_BASE;
            if(variable>=function->class_type_variable&&variable<function->type_variable_count&&
               variable<8)interface_join_witness(&bindings[variable],inputs,input);
            continue;
        }
        for(size_t j=0;j<actual->count;j++) {
            DiamondTypeMember source=actual->members[j];
            DiamondTypeSet *owned=nullptr;
            if(!diamond_project_type_member(classes,class_count,target.id,&source,inputs,&owned))continue;
            const DiamondTypeSet *projected=owned==nullptr?inputs:owned;
            interface_infer_variables(function,classes,class_count,target.argument_set,projected,source.argument_set,bindings,depth+1);
            interface_infer_variables(function,classes,class_count,target.second_argument_set,projected,source.second_argument_set,bindings,depth+1);
            free(owned);
            if(target.id!=DIAMOND_TYPE_CALLABLE)continue;
            interface_infer_variables(function,classes,class_count,target.callable_return_set,inputs,source.callable_return_set,bindings,depth+1);
            if(target.callable_parameters_typed&&source.callable_parameters_typed&&
               target.callable_arity==source.callable_arity)
                for(size_t p=0;p<target.callable_arity;p++)
                    interface_infer_variables(function,classes,class_count,target.callable_parameter_sets[p],inputs,
                        source.callable_parameter_sets[p],bindings,depth+1);
        }
    }
}

#endif
