#ifndef DIAMOND_INTERFACE_INFERENCE_H
#define DIAMOND_INTERFACE_INFERENCE_H

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
    DiamondTypeSet set;
    bool inferred,invalid;
} InterfaceWitness;

static bool interface_sets_equal(const DiamondTypeSet *sets,uint16_t a,uint16_t b,size_t depth);

static bool interface_members_equal(const DiamondTypeSet *sets,DiamondTypeMember a,
        DiamondTypeMember b,size_t depth) {
    if(a.id!=b.id||a.callable_arity!=b.callable_arity||
       a.callable_parameters_typed!=b.callable_parameters_typed||depth>32)return false;
    if(!interface_sets_equal(sets,a.argument_set,b.argument_set,depth+1)||
       !interface_sets_equal(sets,a.second_argument_set,b.second_argument_set,depth+1)||
       !interface_sets_equal(sets,a.callable_return_set,b.callable_return_set,depth+1))return false;
    if(a.callable_parameters_typed)
        for(size_t p=0;p<a.callable_arity;p++)
            if(!interface_sets_equal(sets,a.callable_parameter_sets[p],b.callable_parameter_sets[p],depth+1))return false;
    return true;
}

static bool interface_sets_equal(const DiamondTypeSet *sets,uint16_t a,uint16_t b,size_t depth) {
    if(a==DIAMOND_NO_TYPE_SET||b==DIAMOND_NO_TYPE_SET)return a==b;
    if(depth>32||sets[a].count!=sets[b].count)return false;
    for(size_t i=0;i<sets[a].count;i++) {
        bool found=false;
        for(size_t j=0;j<sets[b].count&&!found;j++)
            found=interface_members_equal(sets,sets[a].members[i],sets[b].members[j],depth);
        if(!found)return false;
    }
    return true;
}

static void interface_join_witness(InterfaceWitness *witness,const DiamondTypeSet *sets,uint16_t input) {
    witness->inferred=true;
    for(size_t i=0;i<sets[input].count&&!witness->invalid;i++) {
        const DiamondTypeMember member=sets[input].members[i];
        bool found=false;
        for(size_t j=0;j<witness->set.count&&!found;j++)
            found=interface_members_equal(sets,witness->set.members[j],member,0);
        if(found)continue;
        if(witness->set.count==DIAMOND_BOUND_TYPE_MEMBERS) {witness->invalid=true;break;}
        witness->set.members[witness->set.count++]=member;
    }
    if(sets[input].count==0)witness->invalid=true;
}

/* Infer method-owned variables only from inputs the interface supplies. The
 * occurrences join into a union witness; the complete specialized signature
 * must subsequently pass variance checks. Return-only variables have no witness.
 * Receiver-owned variables are fixed and must never be rebound here. */
static void interface_infer_variables(const DiamondFunction *function,
        uint16_t pattern,const DiamondTypeSet *inputs,uint16_t input,
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
            const DiamondTypeMember source=actual->members[j];
            if(source.id!=target.id)continue;
            interface_infer_variables(function,target.argument_set,inputs,source.argument_set,bindings,depth+1);
            interface_infer_variables(function,target.second_argument_set,inputs,source.second_argument_set,bindings,depth+1);
            if(target.id!=DIAMOND_TYPE_CALLABLE)continue;
            interface_infer_variables(function,target.callable_return_set,inputs,source.callable_return_set,bindings,depth+1);
            if(target.callable_parameters_typed&&source.callable_parameters_typed&&
               target.callable_arity==source.callable_arity)
                for(size_t p=0;p<target.callable_arity;p++)
                    interface_infer_variables(function,target.callable_parameter_sets[p],inputs,
                        source.callable_parameter_sets[p],bindings,depth+1);
        }
    }
}

#endif
