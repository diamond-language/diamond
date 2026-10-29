#ifndef DIAMOND_INTERFACE_INFERENCE_H
#define DIAMOND_INTERFACE_INFERENCE_H

/* Method witnesses use the same bounded graph as ordinary call inference.
 * Do not prove contracts whose native/Callable payload that graph would lose. */
static bool interface_binding_supported(const DiamondTypeSet *sets,uint16_t index,
        size_t *nodes) {
    if(index==DIAMOND_NO_TYPE_SET||++*nodes>DIAMOND_BOUND_TYPE_NODES)return false;
    const DiamondTypeSet *set=&sets[index];
    if(set->count==0||set->count>DIAMOND_BOUND_TYPE_MEMBERS)return false;
    for(size_t i=0;i<set->count;i++) {
        const DiamondTypeMember member=set->members[i];
        if(member.id==DIAMOND_TYPE_NATIVE||
           (member.id>=DIAMOND_TYPE_VARIABLE_BASE&&member.id<DIAMOND_TYPE_INTERFACE_BASE)||
           (member.id==DIAMOND_TYPE_CALLABLE&&(member.callable_arity!=UINT8_MAX||
            member.callable_parameters_typed||member.callable_return_set!=DIAMOND_NO_TYPE_SET)))return false;
        for(size_t j=0;j<i;j++)if(set->members[j].id==member.id)return false;
        if(member.argument_set!=DIAMOND_NO_TYPE_SET&&
           !interface_binding_supported(sets,member.argument_set,nodes))return false;
        if(member.second_argument_set!=DIAMOND_NO_TYPE_SET&&
           !interface_binding_supported(sets,member.second_argument_set,nodes))return false;
    }
    return true;
}

/* Infer method-owned variables only from inputs the interface supplies. The
 * first occurrence chooses a witness; the complete specialized signature must
 * subsequently pass variance checks. Return-only variables have no witness.
 * Receiver-owned variables are fixed and must never be rebound here. */
static void interface_infer_variables(const DiamondFunction *function,
        uint16_t pattern,const DiamondTypeSet *inputs,uint16_t input,
        uint16_t bindings[8],size_t depth) {
    if(pattern==DIAMOND_NO_TYPE_SET||input==DIAMOND_NO_TYPE_SET||
       pattern>=function->type_set_count||depth>32)return;
    const DiamondTypeSet *wanted=&function->type_sets[pattern];
    const DiamondTypeSet *actual=&inputs[input];
    for(size_t i=0;i<wanted->count;i++) {
        const DiamondTypeMember target=wanted->members[i];
        if(target.id>=DIAMOND_TYPE_VARIABLE_BASE&&target.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=target.id-DIAMOND_TYPE_VARIABLE_BASE;
            if(variable>=function->class_type_variable&&variable<function->type_variable_count&&
               variable<8&&bindings[variable]==DIAMOND_NO_TYPE_SET)bindings[variable]=input;
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
