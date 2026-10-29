#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "compiler_internal.h"
#include "jit.h"

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* Name resolution and method invocation: `parse_name`, `parse_invoke`, `super`, and the receiver
 * and signature inference they rely on. */

/* Shared tail of a dynamic-dispatch method call: copies `receiver` and
 * every one of `args[0..count)` into one contiguous register run (INVOKE's
 * own calling convention needs its receiver/args argument block
 * contiguous, unlike arbitrary scattered registers a caller might already
 * hold), resolves `method_name` into the current function's string-
 * constant table, and emits DIAMOND_OP_INVOKE (or DIAMOND_OP_INVOKE_TYPED
 * when `type_argument_count>0`). Originally inline at the end of
 * parse_invoke (this is that same code, unchanged); pulled out so
 * compile_delegate (src/compiler.c, `delegate name(params), to: @ivar`)
 * can reuse it directly with already-known registers instead of
 * re-parsing `@ivar.name(args)` from source tokens it doesn't have --
 * `delegate` always calls this with writer_name=false, type_arguments=
 * nullptr, type_argument_count=0, since neither writer-call syntax nor
 * generics apply to its scope. */
static uint16_t emit_invoke_with_name(Compiler *compiler, uint16_t receiver,
        uint16_t method, const uint16_t *type_arguments, size_t type_argument_count,
        const uint16_t *args, size_t count);

typedef uint16_t (*BuiltinCallParser)(Compiler *compiler);

static uint16_t parse_digest_call(Compiler *compiler) {
    return parse_crypto_call(compiler,false);
}

static uint16_t parse_hmac_call(Compiler *compiler) {
    return parse_crypto_call(compiler,true);
}

/* Built-in namespaces reached as `Name.call(...)`. Each is shadowed by a
 * class, local or function of the same name; parse_name checks that before
 * consulting this table. */
static const struct { const char *name; BuiltinCallParser parse; }
builtin_namespace_calls[]={
    {"Fiber",parse_fiber_call},{"File",parse_file_call},
    {"Dir",parse_dir_entries_call},{"Regexp",parse_regexp_new_call},
    {"SQLite3",parse_sqlite3_open_call},{"Tensor",parse_tensor_call},
    {"PostgreSQL",parse_postgres_open_call},{"MySQL",parse_mysql_open_call},
    {"Time",parse_time_call},{"Process",parse_process_call},
    {"BCrypt",parse_bcrypt_call},{"Cipher",parse_cipher_call},
    {"Gzip",parse_gzip_call},{"Base64",parse_base64_call},
    {"SecureRandom",parse_secure_random_call},{"Digest",parse_digest_call},
    {"HMAC",parse_hmac_call},{"ProgramBuilder",parse_program_builder_new_call},
    {"Thread",parse_thread_new_call},{"Channel",parse_channel_new_call},
    {"Supervisor",parse_supervisor_new_call},{"Array",parse_array_new_call},
    {"TCPSocket",parse_tcp_connect_call},{"TCPServer",parse_tcp_listen_call},
    {"DNS",parse_dns_resolve_call},{"IO",parse_io_poll_call},
    {"UDPSocket",parse_udp_socket_call},{"Signal",parse_signal_trap_call},
    {"TLSSocket",parse_tls_connect_call},{"TLSServer",parse_tls_listen_call},
};

/* Built-in functions reached as `name(...)`, shadowed by a local or function
 * of the same name. print/puts/warn are handled separately because they
 * pass flags. */
static const struct { const char *name; BuiltinCallParser parse; }
builtin_function_calls[]={
    {"gets",parse_gets_call},{"exit",parse_exit_call},
    {"debugger",parse_debugger_call},{"breakpoint",parse_debugger_call},
    {"chr",parse_chr_call},{"to_f",parse_to_float_call},
    {"to_i",parse_to_int_call},{"to_sym",parse_to_sym_call},
    {"diamond_json_stringify",parse_json_stringify_call},
};

static const struct { const char *name; DiamondMathFunction id; bool binary; }
builtin_math_calls[]={
    {"sqrt",DIAMOND_MATH_SQRT,false},{"sin",DIAMOND_MATH_SIN,false},
    {"cos",DIAMOND_MATH_COS,false},{"tan",DIAMOND_MATH_TAN,false},
    {"pow",DIAMOND_MATH_POW,true},{"exp",DIAMOND_MATH_EXP,false},
    {"log",DIAMOND_MATH_LOG,false},{"tanh",DIAMOND_MATH_TANH,false},
};

static BuiltinCallParser builtin_namespace_parser(const Compiler *compiler,
        DiamondSpan name) {
    for(size_t index=0;index<sizeof builtin_namespace_calls/
            sizeof builtin_namespace_calls[0];index++)
        if(name_equals(compiler,builtin_namespace_calls[index].name,name,false))
            return builtin_namespace_calls[index].parse;
    return nullptr;
}

/* Parses `name(...)` when name is a built-in function; returns false, having
 * consumed nothing, when it is not. */
static bool parse_builtin_function_call(Compiler *compiler, DiamondSpan name,
        uint16_t *result) {
    if(name_equals(compiler,"print",name,false)||
       name_equals(compiler,"puts",name,false)) {
        *result=parse_print_call(compiler,name_equals(compiler,"puts",name,false)?
            DIAMOND_PRINT_NEWLINE:0);
        return true;
    }
    if(name_equals(compiler,"warn",name,false)) {
        *result=parse_print_call(compiler,DIAMOND_PRINT_NEWLINE|DIAMOND_PRINT_STDERR);
        return true;
    }
    for(size_t index=0;index<sizeof builtin_function_calls/
            sizeof builtin_function_calls[0];index++)
        if(name_equals(compiler,builtin_function_calls[index].name,name,false)) {
            *result=builtin_function_calls[index].parse(compiler);
            return true;
        }
    for(size_t index=0;index<sizeof builtin_math_calls/
            sizeof builtin_math_calls[0];index++)
        if(name_equals(compiler,builtin_math_calls[index].name,name,false)) {
            *result=builtin_math_calls[index].binary?
                parse_math_binary_call(compiler,builtin_math_calls[index].id):
                parse_math_unary_call(compiler,builtin_math_calls[index].id);
            return true;
        }
    return false;
}

/* `Module.function(...)`: the current token is the '.'. */
static uint16_t parse_module_singleton_call(Compiler *compiler, DiamondSpan name,
        int module_index) {
    advance_token(compiler);
    const DiamondModule *module=
        compiler->program->modules[(size_t)module_index];
    const DiamondMethod *method=nullptr;
    if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER)
        for(size_t index=0;index<module->singleton_method_count;index++)
            if(singleton_call_name_equals(compiler,
                   module->singleton_methods[index].name,
                   compiler->current.span))
                method=&module->singleton_methods[index];
    if(method==nullptr) {
        fail(compiler,compiler->current.span,
             "undefined module singleton function");return 0;
    }
    return parse_singleton_call(compiler,method,name,-1);
}

typedef struct ConstructorInference {
    uint16_t bindings[8];
    bool complete;
    bool exact;
} ConstructorInference;

static ConstructorInference constructor_inference(void) {
    ConstructorInference inference={.complete=true,.exact=true};
    for(size_t index=0;index<8;index++)inference.bindings[index]=DIAMOND_NO_TYPE_SET;
    return inference;
}

static void infer_constructor_input(Compiler *compiler,const DiamondFunction *initializer,
        ConstructorInference *inference,size_t parameter,uint16_t argument) {
    if(!infer_constructor_argument(compiler,initializer,parameter,argument,
            inference->bindings,&inference->exact))inference->complete=false;
}

/* Consume the parser's positional layout before compiling a block can replace
 * that scratch metadata. A dynamic spread has no provable slot layout. */
static void infer_constructor_positionals(Compiler *compiler,
        const DiamondFunction *initializer,ConstructorInference *inference) {
    const size_t prefix=compiler->positional_spread_index;
    const size_t fixed=compiler->positional_spread_fixed_count;
    if(!compiler->positional_spread_literal&&prefix!=fixed) {
        inference->complete=false;return;
    }
    /* A spread with no fixed suffix can also be dynamic. Its index is zero
     * for `*[...]` and for `*values`; the literal flag distinguishes them.
     * Callers with a spread mark incomplete separately below. */
    for(size_t index=0;index<prefix;index++)
        infer_constructor_input(compiler,initializer,inference,index,
            compiler->positional_spread_fixed[index]);
    if(compiler->positional_spread_literal) {
        for(size_t index=0;index<compiler->positional_spread_count;index++)
            infer_constructor_input(compiler,initializer,inference,prefix+index,
                compiler->positional_spread_elements[index]);
        for(size_t index=prefix;index<fixed;index++)
            infer_constructor_input(compiler,initializer,inference,
                compiler->positional_spread_count+index,
                compiler->positional_spread_fixed[index]);
    }
}

static uint16_t compile_constructor_block(Compiler *compiler,
        const DiamondFunction *initializer,const ConstructorInference *inference,
        uint16_t explicit_type) {
    uint16_t context[8];memcpy(context,inference->bindings,sizeof context);
    if(explicit_type!=DIAMOND_NO_TYPE_SET) {
        if(initializer!=nullptr&&initializer->class_type_variable==2) {
            const DiamondTypeMember descriptor=compiler->function->type_sets[explicit_type].members[0];
            context[0]=descriptor.argument_set;context[1]=descriptor.second_argument_set;
        } else context[0]=explicit_type;
    }
    else if(initializer!=nullptr&&initializer->class_type_variable&&
            initializer->type_variable_count==initializer->class_type_variable&&
            (!inference->complete||!inference->exact))
        for(size_t variable=0;variable<initializer->class_type_variable;variable++)context[variable]=DIAMOND_NO_TYPE_SET;
    /* A proved scalar binding can type an unannotated block. Broader
     * constructor contributions can still join at runtime. */
    return compile_contextual_typed_block(compiler,initializer,
        initializer==nullptr||initializer->arity<=1?0:initializer->arity-2,
        context,initializer==nullptr?0:initializer->type_variable_count);
}

/* Instance arguments survive runtime value inference. Callable signatures
 * still have no payload in the runtime graph, so erase only that shape. */
static uint16_t constructor_value_hint(Compiler *compiler,uint16_t source,size_t depth) {
    if(depth>32||source>=compiler->function->type_set_count)return DIAMOND_NO_TYPE_SET;
    DiamondTypeSet set=compiler->function->type_sets[source];
    for(size_t index=0;index<set.count;index++) {
        DiamondTypeMember *member=&set.members[index];
        if(member->id==DIAMOND_TYPE_CALLABLE) {
            member->argument_set=DIAMOND_NO_TYPE_SET;
            member->second_argument_set=DIAMOND_NO_TYPE_SET;
            member->callable_arity=UINT8_MAX;member->callable_parameters_typed=false;
            member->callable_return_set=DIAMOND_NO_TYPE_SET;
        } else {
            if(member->argument_set!=DIAMOND_NO_TYPE_SET) {
                member->argument_set=constructor_value_hint(compiler,member->argument_set,depth+1);
                if(member->argument_set==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
            }
            if(member->second_argument_set!=DIAMOND_NO_TYPE_SET) {
                member->second_argument_set=constructor_value_hint(compiler,member->second_argument_set,depth+1);
                if(member->second_argument_set==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
            }
        }
    }
    if(!reserve_type_sets(compiler,1))return DIAMOND_NO_TYPE_SET;
    const uint16_t result=(uint16_t)compiler->function->type_set_count++;
    compiler->function->type_sets[result]=set;
    return result;
}

/* Fixed superclass arguments own graphs, independent of the declaration's
 * function table. Import them into the caller's table for receiver facts. */
static uint16_t import_fixed_binding(Compiler *compiler,const DiamondTypeBinding *binding) {
    if(binding->invalid||binding->node_count==0||
       !reserve_type_sets(compiler,binding->node_count))return DIAMOND_NO_TYPE_SET;
    const uint16_t base=(uint16_t)compiler->function->type_set_count;
    compiler->function->type_set_count+=binding->node_count;
    for(size_t node=0;node<binding->node_count;node++) {
        DiamondTypeSet set={.count=binding->nodes[node].count};
        for(size_t i=0;i<set.count;i++) {
            const DiamondBoundTypeMember from=binding->nodes[node].members[i];
            DiamondTypeMember member={.id=from.id,
                .argument_set=from.argument_node==UINT8_MAX?DIAMOND_NO_TYPE_SET:(uint16_t)(base+from.argument_node),
                .second_argument_set=from.second_argument_node==UINT8_MAX?DIAMOND_NO_TYPE_SET:(uint16_t)(base+from.second_argument_node),
                .callable_arity=from.id==DIAMOND_TYPE_NATIVE?from.native_kind:UINT8_MAX,
                .callable_return_set=DIAMOND_NO_TYPE_SET};
            if(from.id==DIAMOND_TYPE_CALLABLE&&from.signature<binding->signature_count) {
                const DiamondBoundSignature signature=binding->signatures[from.signature];
                member.callable_arity=signature.arity;
                member.callable_parameters_typed=signature.parameters_typed;
                if(signature.return_node!=UINT8_MAX)member.callable_return_set=(uint16_t)(base+signature.return_node);
                if(signature.parameters_typed)
                    for(size_t p=0;p<signature.arity;p++)
                        member.callable_parameter_sets[p]=signature.parameter_nodes[p]==UINT8_MAX?
                            DIAMOND_NO_TYPE_SET:(uint16_t)(base+signature.parameter_nodes[p]);
            }
            set.members[i]=member;
        }
        compiler->function->type_sets[base+node]=set;
    }
    return base;
}

static void publish_constructor_type(Compiler *compiler,uint16_t reg,int class_index,
        const ConstructorInference *inference,uint16_t explicit_type) {
    compiler->known_types[reg]=(uint16_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
    if(compiler->program->classes[class_index]->type_parameter[0]=='\0')return;
    const size_t count=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(compiler->program->classes[class_index]);
    uint16_t arguments[2]={DIAMOND_NO_TYPE_SET,DIAMOND_NO_TYPE_SET};
    const DiamondClass *class=compiler->program->classes[class_index];
    if(class->fixed_type_arguments) {
        for(size_t variable=0;variable<count;variable++) {
            arguments[variable]=import_fixed_binding(compiler,&class->fixed_type_bindings[variable]);
            if(arguments[variable]==DIAMOND_NO_TYPE_SET)return;
        }
    } else if(explicit_type!=DIAMOND_NO_TYPE_SET) {
        if(count==1)arguments[0]=explicit_type;
        else {
            const DiamondTypeMember descriptor=compiler->function->type_sets[explicit_type].members[0];
            arguments[0]=descriptor.argument_set;arguments[1]=descriptor.second_argument_set;
        }
    } else {
        if(!inference->complete)return;
        for(size_t variable=0;variable<count;variable++) {
            uint16_t argument=inference->bindings[variable];
            if(argument>=compiler->function->type_set_count||type_set_contains_variable(compiler,argument))return;
            if(!inference->exact)argument=constructor_value_hint(compiler,argument,0);
            else {
                DiamondTypeSet exact=compiler->function->type_sets[argument];
                exact.inferred=false;
                if(!reserve_type_sets(compiler,1))return;
                argument=(uint16_t)compiler->function->type_set_count++;
                compiler->function->type_sets[argument]=exact;
            }
            if(argument==DIAMOND_NO_TYPE_SET)return;
            arguments[variable]=argument;
        }
    }
    if(!reserve_type_sets(compiler,1))return;
    const uint16_t set=(uint16_t)compiler->function->type_set_count++;
    compiler->function->type_sets[set]=(DiamondTypeSet){.count=1,.members={{
        .id=(uint16_t)(DIAMOND_TYPE_CLASS_BASE+class_index),
        .argument_set=arguments[0],.second_argument_set=arguments[1],
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}}};
    if(class->fixed_type_arguments||explicit_type!=DIAMOND_NO_TYPE_SET||inference->exact)
        publish_known_type_set(compiler,reg,set);
    else compiler->tooling_type_sets[reg]=(int32_t)set;
}

/* `Class.new(...)` whose arguments include a keyword argument: the current
 * token is the first argument. */
static uint16_t parse_class_new_keywords(Compiler *compiler, int class_index,
        const DiamondFunction *initializer,uint16_t explicit_type) {
    const bool has_spread=call_arguments_have_spread(compiler);
    DiamondSpan keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];uint16_t keyword_values[DIAMOND_MAX_DECLARED_PARAMETERS];
    size_t keyword_count=0;
    const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
        keyword_names,keyword_values,&keyword_count,initializer);
    ConstructorInference inference=constructor_inference();
    infer_constructor_positionals(compiler,initializer,&inference);
    if(has_spread&&!compiler->positional_spread_literal)inference.complete=false;
    if(initializer!=nullptr)
        for(size_t keyword=0;keyword<keyword_count;keyword++)
            for(size_t parameter=0;parameter<initializer->arity&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++)
                if(name_equals(compiler,initializer->parameter_names[parameter],keyword_names[keyword],false))
                    infer_constructor_input(compiler,initializer,&inference,parameter,keyword_values[keyword]);
    bool has_block=false;uint16_t block=0;
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(explicit_type!=DIAMOND_NO_TYPE_SET)inference.bindings[0]=explicit_type;
        for(size_t index=0;index<keyword_count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                keyword_values[index],0,2);
            keyword_values[index]=snapshot;
        }
        block=compile_constructor_block(compiler,initializer,&inference,explicit_type);
        if(initializer!=nullptr&&initializer->arity>1)
            infer_constructor_input(compiler,initializer,&inference,initializer->arity-2,block);
        has_block=true;
    }
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,explicit_type==DIAMOND_NO_TYPE_SET?
        DIAMOND_OP_NEW_KEYWORDS:DIAMOND_OP_NEW_TYPED);
    emit_register(compiler,destination);
    emit_register(compiler,(uint16_t)class_index);
    if(explicit_type!=DIAMOND_NO_TYPE_SET)emit_register(compiler,explicit_type);
    emit_register(compiler,positional);
    emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
    for(size_t index=0;index<keyword_count;index++) {
        emit_register(compiler,add_name_string(compiler,keyword_names[index]));
        emit_register(compiler,keyword_values[index]);
    }
    if(has_block)emit_register(compiler,block);
    publish_constructor_type(compiler,destination,class_index,&inference,explicit_type);
    return destination;
}

/* `Class.new(...)` whose arguments include a spread: the current token is
 * the first argument. */
static uint16_t parse_class_new_spread(Compiler *compiler, int class_index,
        const DiamondFunction *initializer) {
    uint16_t spread=parse_spread_argument_array(compiler,nullptr,
        nullptr,nullptr,nullptr,initializer,
        initializer==nullptr||initializer->arity==0?0:
            initializer->arity-1);
    ConstructorInference inference=constructor_inference();
    infer_constructor_positionals(compiler,initializer,&inference);
    if(!compiler->positional_spread_literal)inference.complete=false;
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        const uint16_t spread_snapshot=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
            spread,0,2);
        spread=spread_snapshot;
        const uint16_t block=compile_constructor_block(compiler,initializer,&inference,DIAMOND_NO_TYPE_SET);
        if(initializer!=nullptr&&initializer->arity>1)
            infer_constructor_input(compiler,initializer,&inference,initializer->arity-2,block);
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_NEW_KEYWORDS);
        emit_register(compiler,destination);
        emit_register(compiler,(uint16_t)class_index);
        emit_register(compiler,spread);emit_byte(compiler,0x80u);
        emit_register(compiler,block);
        publish_constructor_type(compiler,destination,class_index,&inference,DIAMOND_NO_TYPE_SET);
        return destination;
    }
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_NEW_SPREAD);
    emit_register(compiler,destination);
    emit_register(compiler,(uint16_t)class_index);
    emit_register(compiler,spread);
    publish_constructor_type(compiler,destination,class_index,&inference,DIAMOND_NO_TYPE_SET);
    return destination;
}

/* `Class.new(...)`: the current token is `new`. */
static uint16_t parse_class_new_call(Compiler *compiler, int class_index) {
    advance_token(compiler);
    const DiamondFunction *initializer=
        constructor_signature(compiler,class_index);
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(call_arguments_have_keyword(compiler))
        return parse_class_new_keywords(compiler,class_index,initializer,DIAMOND_NO_TYPE_SET);
    if(call_arguments_have_spread(compiler))
        return parse_class_new_spread(compiler,class_index,initializer);
    uint16_t args[DIAMOND_MAX_ARGUMENTS]; size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == DIAMOND_MAX_ARGUMENTS) { fail(compiler, compiler->current.span, "too many arguments"); break; }
        args[count++] = parse_expression(compiler);
        skip_newlines(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments"); return 0;
    }
    advance_token(compiler);
    ConstructorInference inference=constructor_inference();
    for(size_t index=0;index<count;index++)
        infer_constructor_input(compiler,initializer,&inference,index,args[index]);
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many arguments");return 0;
        }
        for(size_t index=0;index<count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,args[index],0,2);
            args[index]=snapshot;
        }
        const uint16_t block=compile_constructor_block(compiler,initializer,&inference,DIAMOND_NO_TYPE_SET);
        const size_t block_slot=initializer==nullptr||initializer->arity<=1?
            0:(size_t)initializer->arity-2;
        infer_constructor_input(compiler,initializer,&inference,block_slot,block);
        if(count<block_slot) {
            const uint16_t positional=emit_argument_array(compiler,args,count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_NEW_KEYWORDS);
            emit_register(compiler,destination);
            emit_register(compiler,(uint16_t)class_index);
            emit_register(compiler,positional);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_constructor_type(compiler,destination,class_index,&inference,DIAMOND_NO_TYPE_SET);
            return destination;
        }
        args[count++]=block;
    }
    const uint16_t base = allocate_register(compiler);
    for (size_t i = 1; i < count; i++) (void)allocate_register(compiler);
    for (size_t i = 0; i < count; i++)
        emit_instruction(compiler, DIAMOND_OP_MOVE, (uint16_t)(base+i), args[i], 0, 2);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_NEW); emit_register(compiler,dest);
    emit_register(compiler,(uint16_t)class_index); emit_register(compiler,base);
    emit_byte(compiler, (uint8_t)count);
    publish_constructor_type(compiler,dest,class_index,&inference,DIAMOND_NO_TYPE_SET);
    return dest;
}

/* `Class.method(...)`, including `Class.new(...)`: the current token is the
 * '.'. */
static uint16_t parse_class_member_call(Compiler *compiler, DiamondSpan name,
        int class_index) {
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected constructor or singleton method");return 0;
    }
    if(name_equals(compiler,"redefine_method",compiler->current.span,false))
        return parse_redefine_method_call(compiler,class_index);
    if(name_equals(compiler,"define_method",compiler->current.span,false))
        return parse_define_method_call(compiler,class_index);
    if(name_equals(compiler,"compile_method",compiler->current.span,false))
        return parse_compile_method_call(compiler,class_index);
    if(!name_equals(compiler,"new",compiler->current.span,false)) {
        const DiamondMethod *method=nullptr;
        const DiamondClass *owner=
            compiler->program->classes[(size_t)class_index];
        while(owner!=nullptr&&method==nullptr) {
            for(size_t index=0;index<owner->singleton_method_count;index++)
                if(singleton_call_name_equals(compiler,
                       owner->singleton_methods[index].name,
                       compiler->current.span))
                    method=&owner->singleton_methods[index];
            owner=owner->superclass==DIAMOND_NO_CLASS?nullptr:
                compiler->program->classes[owner->superclass];
        }
        if(method==nullptr) {
            fail(compiler,compiler->current.span,
                 "undefined class singleton method");return 0;
        }
        return parse_singleton_call(compiler,method,name,class_index);
    }
    /* Gated to the real pass only, same reasoning as case/when
     * exhaustiveness's own discovery-pass guard (resolve_type_name
     * has the original precedent): a `sealed class Shape` declared
     * later in the same source might not have set its own `sealed`
     * flag yet if discovery reaches this call site first, and fail()
     * only remembers the first failure. */
    if(!compiler->discovery_pass&&
       compiler->program->classes[(size_t)class_index]->sealed) {
        fail(compiler,compiler->current.span,
            "cannot instantiate a sealed class directly -- "
            "use one of its subclasses");
        return 0;
    }
    return parse_class_new_call(compiler,class_index);
}

/* Preserve generic function calls/references when a function and a class
 * share a name. Only the explicit `.new` suffix selects the class. */
static bool specialized_constructor_ahead(Compiler *compiler) {
    DiamondLexer lexer=compiler->lexer;
    size_t brackets=1;
    while(brackets>0) {
        const DiamondToken token=diamond_lexer_next(&lexer);
        if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR)return false;
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET)brackets++;
        else if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET)brackets--;
    }
    if(diamond_lexer_next(&lexer).kind!=DIAMOND_TOKEN_DOT)return false;
    const DiamondToken member=diamond_lexer_next(&lexer);
    return member.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"new",member.span,false);
}

static bool generic_self_call_ahead(Compiler *compiler) {
    DiamondLexer lexer=compiler->lexer;
    size_t brackets=1;
    while(brackets>0) {
        const DiamondToken token=diamond_lexer_next(&lexer);
        if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR)return false;
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET)brackets++;
        else if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET)brackets--;
    }
    return diamond_lexer_next(&lexer).kind==DIAMOND_TOKEN_LEFT_PAREN;
}

uint16_t parse_name(Compiler *compiler) {
    const DiamondSpan name = compiler->previous.span;
    int class_index=find_class(compiler,name);
    int module_index=find_module(compiler,name);
    if(compiler->current.kind==DIAMOND_TOKEN_DOUBLE_COLON) {
        char qualified[DIAMOND_MAX_FUNCTION_NAME]={};
        if(!append_span_name(compiler,qualified,sizeof qualified,name)) {
            fail(compiler,name,"qualified name is too long");return 0;
        }
        while(compiler->current.kind==DIAMOND_TOKEN_DOUBLE_COLON) {
            advance_token(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
               strlen(qualified)+2>=sizeof qualified) {
                fail(compiler,compiler->current.span,
                     "expected name after '::'");return 0;
            }
            (void)strncat(qualified,"::",sizeof qualified-strlen(qualified)-1);
            if(!append_span_name(compiler,qualified,sizeof qualified,
                                 compiler->current.span)) {
                fail(compiler,compiler->current.span,
                     "qualified name is too long");return 0;
            }
            advance_token(compiler);
        }
        class_index=find_class_name(compiler,qualified);
        module_index=find_module_name(compiler,qualified);
        int constant=find_lexical_constant_name(compiler,qualified);
        if(constant<0)constant=find_qualified_inherited_constant(compiler,qualified);
        if(constant>=0) {
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_NAMESPACE_CONSTANT,
                destination,(uint16_t)constant,0,2);
            return destination;
        }
        if(class_index<0&&module_index<0) {
            if(compiler->discovery_pass)return allocate_register(compiler);
            fail(compiler,name,"undefined namespaced class");return 0;
        }
    }
    if(module_index>=0&&compiler->current.kind==DIAMOND_TOKEN_DOT)
        return parse_module_singleton_call(compiler,name,module_index);
    const int constant=find_namespace_constant(compiler,name);
    if(constant>=0&&find_local(compiler,name)<0) {
        const uint16_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_NAMESPACE_CONSTANT,destination,
                         (uint16_t)constant,0,2);
        return destination;
    }
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT) {
        const BuiltinCallParser parse=builtin_namespace_parser(compiler,name);
        if(parse!=nullptr)return parse(compiler);
    }
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN) {
        uint16_t result=0;
        if(parse_builtin_function_call(compiler,name,&result))return result;
    }
    /* ARGV/ENV -- plain values, not calls, so unlike puts/gets/Time/etc.
     * above there's no `current.kind==LEFT_PAREN` gate: `ARGV` alone is
     * already a complete expression. Still shadowable by a constant or
     * user-defined function of the same name, same convention as every
     * other built-in name. See docs/syntax.md. */
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       name_equals(compiler,"ARGV",name,false)) {
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_ARGV);
        emit_register(compiler,destination);
        compiler->known_types[destination]=DIAMOND_TYPE_ARRAY;
        return destination;
    }
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       name_equals(compiler,"ENV",name,false)) {
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_ENV);
        emit_register(compiler,destination);
        compiler->known_types[destination]=DIAMOND_TYPE_HASH;
        return destination;
    }
    if(class_index>=0&&compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
       find_local(compiler,name)<0&&
       (find_function(compiler,name)<0||specialized_constructor_ahead(compiler))) {
        uint16_t arguments[8];size_t count=0;
        if(!parse_explicit_type_arguments(compiler,arguments,&count))return 0;
        const DiamondClass *class=compiler->program->classes[class_index];
        if(class->fixed_type_arguments) {
            fail(compiler,name,"inherited type arguments are fixed by the superclass");return 0;
        }
        if(class->type_parameter[0]=='\0'||count!=DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)) {
            fail(compiler,name,DIAMOND_CLASS_TYPE_PARAMETER_COUNT(class)==2?
                "class specialization requires exactly two declared type arguments":
                "class specialization requires exactly one declared type argument");
            return 0;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_DOT) {
            fail(compiler,name,"expected '.new(...)' after class type argument");return 0;
        }
        advance_token(compiler);
        if(!name_equals(compiler,"new",compiler->current.span,false)||class->sealed) {
            fail(compiler,name,"expected constructor of an instantiable generic class");return 0;
        }
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler,name,"expected '(' after 'new'");return 0;
        }
        advance_token(compiler);skip_newlines(compiler);
        return parse_class_new_keywords(compiler,class_index,
            constructor_signature(compiler,class_index),count==1?arguments[0]:(uint16_t)collection_type_set(compiler,DIAMOND_TYPE_HASH,arguments[0],arguments[1]));
    }
    if (class_index >= 0 && compiler->current.kind == DIAMOND_TOKEN_DOT)
        return parse_class_member_call(compiler,name,class_index);
    if (compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN||
        (compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
         find_local(compiler,name)<0&&(find_function(compiler,name)>=0||
          (compiler->in_method&&compiler->current_class>=0&&generic_self_call_ahead(compiler))))) {
        return parse_call(compiler, name);
    }
    /* Discovery may encounter a constant or class before its declaration.
     * Its bytecode is discarded; the real pass resolves the discovered name
     * or reports an undefined identifier. */
    if(compiler->discovery_pass&&class_index<0&&module_index<0&&
       compiler->source[name.start]>='A'&&compiler->source[name.start]<='Z') {
        const uint16_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
        return destination;
    }
    return parse_identifier(compiler);
}

uint16_t emit_invoke_call(Compiler *compiler, uint16_t receiver,
        DiamondSpan method_name, bool writer_name,
        const uint16_t *type_arguments, size_t type_argument_count,
        const uint16_t *args, size_t count) {
    const uint16_t method=add_name_string(compiler,method_name);
    if(writer_name&&!compiler->failed) {
        DiamondStringConstant *string=&compiler->function->strings[method];
        if(string->length==DIAMOND_MAX_STRING_LENGTH)
            fail(compiler,method_name,"method name is too long");
        else {
            string->chars[string->length++]='=';
            string->chars[string->length]='\0';
        }
    }
    return emit_invoke_with_name(compiler,receiver,method,type_arguments,
                                 type_argument_count,args,count);
}

/* A method call whose name the compiler supplies (desugaring, such as
 * `value[start, length]` into value.slice(start, length)), not source. */
uint16_t emit_invoke_named(Compiler *compiler, uint16_t receiver,
        const char *name, const uint16_t *args, size_t count) {
    const size_t length=strlen(name);
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,compiler->previous.span,"out of memory growing function strings");
        return 0;
    }
    if(compiler->function->string_count==DIAMOND_MAX_STRING_CONSTANTS) {
        fail(compiler,compiler->previous.span,"too many or oversized names in function");
        return 0;
    }
    DiamondStringConstant *string=
        &compiler->function->strings[compiler->function->string_count];
    memcpy(string->chars,name,length);
    string->length=length;
    string->chars[length]='\0';
    const uint16_t method=(uint16_t)compiler->function->string_count++;
    return emit_invoke_with_name(compiler,receiver,method,nullptr,0,args,count);
}

static uint16_t emit_invoke_with_name(Compiler *compiler, uint16_t receiver,
        uint16_t method, const uint16_t *type_arguments, size_t type_argument_count,
        const uint16_t *args, size_t count) {
    const uint16_t base = allocate_register(compiler);
    for (size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for (size_t i=0;i<count;i++) emit_instruction(compiler, DIAMOND_OP_MOVE,
        (uint16_t)(base+i), args[i], 0, 2);
    const uint16_t dest=allocate_register(compiler);
    /* Phases 15 and 17 (docs/internal/jit-design.md): record this specific
     * call site's own receiver type, if the compiler's real known_types
     * tracking already proves one right here -- narrowing included, via
     * whatever apply_narrowing_facts/apply_type_set_fact already applied
     * for this exact lexical point. Concrete classes feed instance dispatch;
     * String, Array, and Hash feed selected allocation-free native reads. offset
     * must be captured before
     * emit_opcode below (which itself reads code_count for the same
     * "this instruction's own start" purpose) -- src/jit.c's compile_body
     * reads instruction_start the same way, before decoding the opcode
     * byte, so the two must agree exactly. Plain DIAMOND_OP_INVOKE only:
     * src/jit.c has no case for DIAMOND_OP_INVOKE_TYPED. */
    uint16_t receiver_type=compiler->known_types[receiver];
    const int32_t receiver_set=compiler->known_type_sets[receiver];
    if(receiver_type==TYPE_UNKNOWN&&receiver_set>=0&&
       (size_t)receiver_set<compiler->function->type_set_count) {
        const DiamondTypeSet *set=
            &compiler->function->type_sets[(size_t)receiver_set];
        if(set->count==1)receiver_type=set->members[0].id;
    }
    if(type_argument_count==0&&
       ((receiver_type>=DIAMOND_TYPE_CLASS_BASE&&
         receiver_type<DIAMOND_TYPE_INTERFACE_BASE)||
        receiver_type==DIAMOND_TYPE_STRING||receiver_type==DIAMOND_TYPE_ARRAY||
        receiver_type==DIAMOND_TYPE_HASH)&&
       compiler->function->invoke_site_known_class_count<DIAMOND_MAX_INVOKE_SITES) {
        DiamondFunction *fn=compiler->function;
        fn->invoke_site_known_class[fn->invoke_site_known_class_count++]=
            (DiamondInvokeSiteFact){.offset=(uint32_t)fn->code_count,
                .known_type=receiver_type};
    }
    emit_opcode(compiler,type_argument_count==0?
        DIAMOND_OP_INVOKE:DIAMOND_OP_INVOKE_TYPED);emit_register(compiler,dest);
    emit_register(compiler,receiver);emit_register(compiler,method);emit_register(compiler,base);
    emit_byte(compiler,(uint8_t)count);
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    return dest;
}

static uint16_t emit_invoke_typed_spread(Compiler *compiler,uint16_t receiver,
        DiamondSpan method_name,uint16_t spread,const uint16_t *type_arguments,
        size_t type_argument_count) {
    const uint16_t destination=allocate_register(compiler);
    const uint16_t method=add_name_string(compiler,method_name);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_INVOKE_SPREAD:
        DIAMOND_OP_INVOKE_TYPED_SPREAD);
    emit_register(compiler,destination);emit_register(compiler,receiver);
    emit_register(compiler,method);emit_register(compiler,spread);
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    return destination;
}

uint16_t emit_invoke_spread(Compiler *compiler,uint16_t receiver,
        DiamondSpan method_name,uint16_t spread) {
    return emit_invoke_typed_spread(compiler,receiver,method_name,spread,
        nullptr,0);
}

static uint16_t emit_invoke_keywords(Compiler *compiler,uint16_t receiver,
        DiamondSpan method_name,uint16_t positional,
        const DiamondSpan *keyword_names,const uint16_t *keyword_values,
        size_t keyword_count,const uint16_t *type_arguments,
        size_t type_argument_count,bool has_block,uint16_t block) {
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_INVOKE_KEYWORDS:
        DIAMOND_OP_INVOKE_TYPED_KEYWORDS);
    emit_register(compiler,destination);emit_register(compiler,receiver);
    emit_register(compiler,add_name_string(compiler,method_name));
    emit_register(compiler,positional);
    emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
    for(size_t index=0;index<keyword_count;index++) {
        emit_register(compiler,add_name_string(compiler,keyword_names[index]));
        emit_register(compiler,keyword_values[index]);
    }
    if(has_block)emit_register(compiler,block);
    emit_type_argument_trailer(compiler,type_arguments,type_argument_count);
    return destination;
}

uint16_t emit_build_spread_arguments(Compiler *compiler,
        const uint16_t *prefix,size_t prefix_count,uint16_t spread,
        const uint16_t *suffix,size_t suffix_count,bool optional_block) {
    uint16_t prefix_base=spread,suffix_base=spread;
    if(prefix_count>0) {
        prefix_base=allocate_register(compiler);
        for(size_t index=1;index<prefix_count;index++)
            (void)allocate_register(compiler);
        for(size_t index=0;index<prefix_count;index++)
            emit_instruction(compiler,DIAMOND_OP_MOVE,
                (uint16_t)(prefix_base+index),prefix[index],0,2);
    }
    if(suffix_count>0) {
        suffix_base=allocate_register(compiler);
        for(size_t index=1;index<suffix_count;index++)
            (void)allocate_register(compiler);
        for(size_t index=0;index<suffix_count;index++)
            emit_instruction(compiler,DIAMOND_OP_MOVE,
                (uint16_t)(suffix_base+index),suffix[index],0,2);
    }
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BUILD_SPREAD_ARGS);
    emit_register(compiler,destination);
    emit_register(compiler,prefix_base);emit_byte(compiler,(uint8_t)prefix_count);
    emit_register(compiler,spread);
    emit_register(compiler,suffix_base);
    emit_byte(compiler,(uint8_t)(suffix_count|(optional_block?0x80u:0u)));
    compiler->known_types[destination]=DIAMOND_TYPE_ARRAY;
    int32_t element_set=array_element_type_set(compiler,spread);
    if(prefix_count>0) {
        const int32_t prefix_set=joined_value_type_set(compiler,prefix,
            prefix_count);
        element_set=join_type_set_indices(compiler,element_set,prefix_set);
    }
    if(suffix_count>0) {
        const int32_t suffix_set=joined_value_type_set(compiler,suffix,
            suffix_count);
        element_set=join_type_set_indices(compiler,element_set,suffix_set);
    }
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_ARRAY,
        element_set,-1);
    return destination;
}

/* Only consulted when a writer-call's '=' is immediately followed by
 * '[' -- disambiguates the existing (real, if never yet exercised)
 * explicit generic writer call recv.attr=[T](value) from the new bare
 * assignment sugar's array-literal right-hand side recv.attr = [1, 2].
 * Same balanced-bracket lookahead index_assignment_ahead already uses;
 * only reads as generics when the matching ']' is directly followed by
 * '(', exactly the shape every ordinary generic call already requires. */
static bool writer_generic_arguments_ahead(const Compiler *compiler) {
    DiamondLexer lookahead = compiler->lexer;
    size_t depth = 1;
    for (;;) {
        DiamondToken token = diamond_lexer_next(&lookahead);
        if (token.kind == DIAMOND_TOKEN_EOF || token.kind == DIAMOND_TOKEN_ERROR)
            return false;
        if (token.kind == DIAMOND_TOKEN_LEFT_BRACKET) depth++;
        if (token.kind == DIAMOND_TOKEN_RIGHT_BRACKET && --depth == 0) break;
    }
    return diamond_lexer_next(&lookahead).kind == DIAMOND_TOKEN_LEFT_PAREN;
}

/* Resolve only the compile-time signature behind an ordinary virtual call.
 * Runtime dispatch remains fully dynamic: this is a conservative type hint
 * used solely while compiling a trailing block.  A receiver without one
 * concrete known class simply receives the existing untyped block path. */
const DiamondFunction *class_instance_signature(
        const Compiler *compiler,uint16_t type,DiamondSpan name) {
    size_t class_index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
    while(class_index<compiler->program->class_count) {
        const DiamondClass *class=compiler->program->classes[class_index];
        for(size_t index=0;index<class->method_count;index++) {
            const DiamondMethod *method=&class->methods[index];
            if(!singleton_call_name_equals(compiler,method->name,name))continue;
            if(method->function_index>=compiler->program->function_count)
                return nullptr;
            return compiler->program->functions[method->function_index];
        }
        if(class->superclass==DIAMOND_NO_CLASS)break;
        class_index=class->superclass;
    }
    return nullptr;
}

bool type_members_structurally_equal(const DiamondTypeMember *left,
        const DiamondTypeSet *left_sets,size_t left_count,
        const DiamondTypeMember *right,const DiamondTypeSet *right_sets,
        size_t right_count,size_t depth) {
    if(left->id!=right->id||left->callable_arity!=right->callable_arity||
       left->callable_parameters_typed!=right->callable_parameters_typed)
        return false;
    if((left->argument_set==DIAMOND_NO_TYPE_SET)!=
       (right->argument_set==DIAMOND_NO_TYPE_SET)||
       (left->second_argument_set==DIAMOND_NO_TYPE_SET)!=
       (right->second_argument_set==DIAMOND_NO_TYPE_SET)||
       (left->callable_return_set==DIAMOND_NO_TYPE_SET)!=
       (right->callable_return_set==DIAMOND_NO_TYPE_SET))return false;
    if(left->argument_set!=DIAMOND_NO_TYPE_SET&&
       !type_sets_structurally_equal(left_sets,left_count,left->argument_set,
           right_sets,right_count,right->argument_set,depth+1))return false;
    if(left->second_argument_set!=DIAMOND_NO_TYPE_SET&&
       !type_sets_structurally_equal(left_sets,left_count,
           left->second_argument_set,right_sets,right_count,
           right->second_argument_set,depth+1))return false;
    if(left->callable_return_set!=DIAMOND_NO_TYPE_SET&&
       !type_sets_structurally_equal(left_sets,left_count,
           left->callable_return_set,right_sets,right_count,
           right->callable_return_set,depth+1))return false;
    if(left->callable_parameters_typed)
        for(size_t parameter=0;parameter<left->callable_arity;parameter++)
            if(!type_sets_structurally_equal(left_sets,left_count,
                    left->callable_parameter_sets[parameter],right_sets,
                    right_count,right->callable_parameter_sets[parameter],
                    depth+1))return false;
    return true;
}

bool type_sets_structurally_equal(const DiamondTypeSet *left_sets,
        size_t left_count,uint16_t left_index,
        const DiamondTypeSet *right_sets,size_t right_count,
        uint16_t right_index,size_t depth) {
    if(depth>32||left_index>=left_count||right_index>=right_count)return false;
    const DiamondTypeSet *left=&left_sets[left_index];
    const DiamondTypeSet *right=&right_sets[right_index];
    if(left->count!=right->count)return false;
    for(size_t left_member=0;left_member<left->count;left_member++) {
        bool found=false;
        for(size_t right_member=0;right_member<right->count;right_member++)
            if(type_members_structurally_equal(&left->members[left_member],
                    left_sets,left_count,&right->members[right_member],
                    right_sets,right_count,depth)) {found=true;break;}
        if(!found)return false;
    }
    return true;
}

static bool contextual_signatures_equal(const DiamondFunction *left,
        const DiamondFunction *right) {
    if(left->arity!=right->arity||left->required_arity!=right->required_arity||
       left->has_variadic!=right->has_variadic||
       left->class_type_variable!=right->class_type_variable||
       left->type_variable_count!=right->type_variable_count)return false;
    for(size_t parameter=0;parameter<left->arity&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;parameter++) {
        if(strcmp(left->parameter_names[parameter],
                  right->parameter_names[parameter])!=0)return false;
        const uint16_t left_set=left->parameter_type_sets[parameter];
        const uint16_t right_set=right->parameter_type_sets[parameter];
        if((left_set==DIAMOND_NO_TYPE_SET)!=(right_set==DIAMOND_NO_TYPE_SET))
            return false;
        if(left_set!=DIAMOND_NO_TYPE_SET&&
           !type_sets_structurally_equal(left->type_sets,left->type_set_count,
               left_set,right->type_sets,right->type_set_count,right_set,0))
            return false;
    }
    return true;
}

static bool return_contracts_equal(const DiamondFunction *left,
        const DiamondFunction *right) {
    if((left->return_type_set==DIAMOND_NO_TYPE_SET)!=
       (right->return_type_set==DIAMOND_NO_TYPE_SET))return false;
    return left->return_type_set==DIAMOND_NO_TYPE_SET||
        type_sets_structurally_equal(left->type_sets,left->type_set_count,
            left->return_type_set,right->type_sets,right->type_set_count,
            right->return_type_set,0);
}

const DiamondFunction *instance_call_signature(
        const Compiler *compiler,uint16_t receiver,DiamondSpan name,
        bool require_matching_return) {
    const uint16_t type=compiler->known_types[receiver];
    if(type>=DIAMOND_TYPE_CLASS_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE)
        return class_instance_signature(compiler,type,name);
    int32_t set_index=compiler->known_type_sets[receiver];
    bool tooling_only=false;
    if(set_index<0&&require_matching_return) {
        set_index=compiler->tooling_type_sets[receiver];
        tooling_only=set_index>=0;
    }
    if(set_index<0||(size_t)set_index>=compiler->function->type_set_count)
        return nullptr;
    const DiamondTypeSet *set=
        &compiler->function->type_sets[(size_t)set_index];
    const DiamondFunction *shared=nullptr;
    /* A tooling-only union would need to join each candidate method's own
     * inferred return. Keep this first slice exact and conservative. */
    if(set->count==0||(tooling_only&&set->count!=1))return nullptr;
    for(size_t index=0;index<set->count;index++) {
        const uint16_t member=set->members[index].id;
        if(member<DIAMOND_TYPE_CLASS_BASE||
           member>=DIAMOND_TYPE_INTERFACE_BASE)return nullptr;
        const DiamondFunction *candidate=
            class_instance_signature(compiler,member,name);
        if(candidate==nullptr)return nullptr;
        if(shared==nullptr)shared=candidate;
        else if(shared!=candidate&&
                (!contextual_signatures_equal(shared,candidate)||
                 (require_matching_return&&
                  !return_contracts_equal(shared,candidate))))return nullptr;
    }
    return shared;
}

static void publish_union_instance_return_type(Compiler *compiler,uint16_t reg,
        int32_t receiver_set_index,DiamondSpan name,const uint16_t *bindings,
        size_t binding_count,bool tooling_only) {
    if(receiver_set_index<0||
       (size_t)receiver_set_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet *receiver_set=
        &compiler->function->type_sets[(size_t)receiver_set_index];
    if(receiver_set->count<2)return;
    uint16_t members[DIAMOND_MAX_UNION_TYPES];
    const size_t member_count=receiver_set->count;
    for(size_t index=0;index<member_count;index++) {
        members[index]=receiver_set->members[index].id;
        if(members[index]<DIAMOND_TYPE_CLASS_BASE||
           members[index]>=DIAMOND_TYPE_INTERFACE_BASE)return;
    }
    const size_t original_count=compiler->function->type_set_count;
    int32_t joined=-1;
    for(size_t index=0;index<member_count;index++) {
        const DiamondFunction *target=class_instance_signature(compiler,
            members[index],name);
        if(target==nullptr||target->class_type_variable) {
            compiler->function->type_set_count=original_count;return;
        }
        uint16_t source_set=target->return_type_set;
        if(source_set==DIAMOND_NO_TYPE_SET) {
            if(!tooling_only) {
                compiler->function->type_set_count=original_count;return;
            }
            source_set=target->inferred_return_type_set;
            if(source_set==DIAMOND_NO_TYPE_SET) {
                compiler->function->type_set_count=original_count;return;
            }
        }
        uint16_t current;
        if(target->type_variable_count>0) {
            bool resolved=true;
            current=clone_substituted_type_set(compiler,target,
                source_set,bindings,binding_count,&resolved);
            if(!resolved) {
                compiler->function->type_set_count=original_count;return;
            }
        } else current=clone_type_set_into_current(compiler,target->type_sets,
            target->type_set_count,source_set);
        if(current==DIAMOND_NO_TYPE_SET) {
            compiler->function->type_set_count=original_count;return;
        }
        joined=joined<0?(int32_t)current:
            join_type_set_indices(compiler,joined,(int32_t)current);
        if(joined<0) {
            compiler->function->type_set_count=original_count;return;
        }
    }
    if(tooling_only)compiler->tooling_type_sets[reg]=joined;
    else publish_known_type_set(compiler,reg,(uint16_t)joined);
}

static CollectionRelay collection_relay(const Compiler *compiler,
        DiamondSpan name) {
    static const CollectionRelayContract contracts[]={
        {"first",COLLECTION_RELAY_ELEMENT},{"last",COLLECTION_RELAY_ELEMENT},
        {"min",COLLECTION_RELAY_ELEMENT},{"max",COLLECTION_RELAY_ELEMENT},
        {"min_by",COLLECTION_RELAY_ELEMENT},{"max_by",COLLECTION_RELAY_ELEMENT},
        {"find",COLLECTION_RELAY_NULLABLE_ELEMENT},
        {"delete_at",COLLECTION_RELAY_NULLABLE_ELEMENT},
        {"pop",COLLECTION_RELAY_NULLABLE_ELEMENT},
        {"reverse",COLLECTION_RELAY_ARRAY_SAME},
        {"uniq",COLLECTION_RELAY_ARRAY_SAME},
        {"compact",COLLECTION_RELAY_ARRAY_SAME},
        {"sort",COLLECTION_RELAY_ARRAY_SAME},
        {"sort_by",COLLECTION_RELAY_ARRAY_SAME},
        {"select",COLLECTION_RELAY_ARRAY_SAME},
        {"reject",COLLECTION_RELAY_ARRAY_SAME},
        {"each_with_index",COLLECTION_RELAY_ARRAY_SAME},
        {"take",COLLECTION_RELAY_ARRAY_SAME},{"drop",COLLECTION_RELAY_ARRAY_SAME},
        {"keys",COLLECTION_RELAY_KEYS},{"values",COLLECTION_RELAY_VALUES},
        {"delete",COLLECTION_RELAY_NULLABLE_VALUE},
        {"first_or",COLLECTION_RELAY_FALLBACK},
        {"last_or",COLLECTION_RELAY_FALLBACK},
        {"concat",COLLECTION_RELAY_CONCAT},{"map",COLLECTION_RELAY_MAP},
        {"flat_map",COLLECTION_RELAY_FLAT_MAP},
        {"partition",COLLECTION_RELAY_NESTED_ARRAY},
        {"each_slice",COLLECTION_RELAY_NESTED_ARRAY},
        {"each_cons",COLLECTION_RELAY_NESTED_ARRAY},
        {"group_by",COLLECTION_RELAY_GROUP_BY},{"zip",COLLECTION_RELAY_ZIP},
        {"tally",COLLECTION_RELAY_TALLY},{"fetch",COLLECTION_RELAY_FETCH},
        {"merge",COLLECTION_RELAY_MERGE},
        {"map_values",COLLECTION_RELAY_MAP_VALUES},{"push",COLLECTION_RELAY_PUSH},
        {"sum",COLLECTION_RELAY_SUM}
    };
    for(size_t index=0;index<sizeof contracts/sizeof contracts[0];index++)
        if(name_equals(compiler,contracts[index].name,name,false))
            return contracts[index].relay;
    return COLLECTION_RELAY_NONE;
}

/* Native Array/Hash extension dispatch is resolved by the VM rather than an
 * instance signature, so its result cannot use the declared-return path above.
 * Preserve the receiver's nested collection graphs for the small family whose
 * result is structurally determined without inspecting arguments or blocks. */
/* Was the call just emitted into `reg` a plain INVOKE with no arguments?
 * first/last/sum mean something else with one (first(n) is an Array,
 * sum with a block sums the block's results). */
static bool invoke_had_no_arguments(const Compiler *compiler,uint16_t reg) {
    const size_t count=compiler->function->code_count;
    if(count<10)return false;
    const uint8_t *code=compiler->function->code+count-10;
    return code[0]==DIAMOND_OP_INVOKE&&
        (uint16_t)((code[1]<<8)|code[2])==reg&&code[9]==0;
}

static void publish_collection_method_return_type(Compiler *compiler,
        uint16_t reg,int32_t receiver_set_index,DiamondSpan name) {
    CollectionRelay relay=collection_relay(compiler,name);
    if((relay==COLLECTION_RELAY_SUM||name_equals(compiler,"first",name,false)||
        name_equals(compiler,"last",name,false))&&
       !invoke_had_no_arguments(compiler,reg))
        relay=COLLECTION_RELAY_NONE;
    if(receiver_set_index<0||
       (size_t)receiver_set_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet receiver=
        compiler->function->type_sets[(size_t)receiver_set_index];
    if(receiver.count==0)return;
    uint16_t collection_type=TYPE_UNKNOWN;
    uint16_t first_arguments[DIAMOND_MAX_UNION_TYPES];
    uint16_t second_arguments[DIAMOND_MAX_UNION_TYPES];
    for(size_t index=0;index<receiver.count;index++) {
        const DiamondTypeMember member=receiver.members[index];
        if(member.id!=DIAMOND_TYPE_ARRAY&&member.id!=DIAMOND_TYPE_HASH)return;
        if(collection_type==TYPE_UNKNOWN)collection_type=member.id;
        else if(collection_type!=member.id)return;
        if(member.argument_set==DIAMOND_NO_TYPE_SET)return;
        first_arguments[index]=member.argument_set;
        second_arguments[index]=member.second_argument_set;
        if(member.id==DIAMOND_TYPE_HASH&&
           member.second_argument_set==DIAMOND_NO_TYPE_SET)return;
    }
    int32_t first=-1,second=-1;
    for(size_t index=0;index<receiver.count;index++) {
        first=first<0?(int32_t)first_arguments[index]:
            join_type_set_indices(compiler,first,(int32_t)first_arguments[index]);
        if(first<0)return;
        if(collection_type==DIAMOND_TYPE_HASH) {
            second=second<0?(int32_t)second_arguments[index]:
                join_type_set_indices(compiler,second,
                    (int32_t)second_arguments[index]);
            if(second<0)return;
        }
    }
    if(collection_type==DIAMOND_TYPE_ARRAY) {
        if(relay==COLLECTION_RELAY_SUM) {
            const DiamondTypeSet *elements=
                &compiler->function->type_sets[(size_t)first];
            bool has_float=false;
            for(size_t index=0;index<elements->count;index++) {
                const uint16_t type=elements->members[index].id;
                if(type==DIAMOND_TYPE_FLOAT)has_float=true;
                else if(type!=DIAMOND_TYPE_INT)return;
            }
            const uint16_t integer=concrete_type_set(compiler,DIAMOND_TYPE_INT);
            if(integer==DIAMOND_NO_TYPE_SET)return;
            int32_t result=(int32_t)integer;
            if(has_float) {
                const uint16_t floating=
                    concrete_type_set(compiler,DIAMOND_TYPE_FLOAT);
                if(floating==DIAMOND_NO_TYPE_SET)return;
                result=join_type_set_indices(compiler,result,(int32_t)floating);
            }
            if(result>=0)publish_known_type_set(compiler,reg,(uint16_t)result);
            return;
        }
        if(relay==COLLECTION_RELAY_ELEMENT) {
            publish_known_type_set(compiler,reg,(uint16_t)first);return;
        }
        if(relay==COLLECTION_RELAY_NULLABLE_ELEMENT) {
            const int32_t nullable=type_set_with_nil(compiler,(uint16_t)first);
            if(nullable>=0)publish_known_type_set(compiler,reg,
                (uint16_t)nullable);
            return;
        }
        if(relay==COLLECTION_RELAY_ARRAY_SAME)
            record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,
                first,-1);
        return;
    }
    if(relay==COLLECTION_RELAY_KEYS)
        record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,first,-1);
    else if(relay==COLLECTION_RELAY_VALUES)
        record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,second,-1);
    else if(relay==COLLECTION_RELAY_NULLABLE_VALUE) {
        /* Hash#delete: the removed value, or nil for a missing key. */
        const int32_t nullable=type_set_with_nil(compiler,(uint16_t)second);
        if(nullable>=0)publish_known_type_set(compiler,reg,(uint16_t)nullable);
    }
}

/* A small number of native String/Array/Hash methods (.length, .to_i,
 * .ord, .strip, ...) always return one fixed scalar type regardless of
 * the receiver's own element type -- unlike publish_collection_method_
 * return_type just above, which exists specifically to preserve what a
 * transform's own *nested* element/value type sets are. Reuses
 * DIAMOND_NATIVE_METHODS (src/vm.c), the exact same table already
 * exposed to this file for structural interface conformance checking
 * (see known_type_satisfies_one's own call to diamond_native_method_
 * satisfies above) -- one source of truth for "what does this native
 * method return," not a second, parallel list of the same facts.
 * Deliberately narrow: only when the receiver's own type set has
 * exactly one member and it's a plain String/Array/Hash, never a
 * union or a user class. Sound because none of these three are ever
 * reachable through the user-facing method-redefinition machinery
 * (that operates on a real DiamondClass's own mutable method table;
 * these are native value kinds dispatched through a fixed C-level
 * switch instead) -- "a plain Array's own .length() always returns
 * Int" can never be invalidated by user code at runtime. See
 * docs/internal/jit-design.md's own note on why this specific gap
 * mattered: `index < keys.length()` couldn't compile-time-select
 * DIAMOND_OP_LESS_INT without it. */
static void publish_native_scalar_method_return_type(Compiler *compiler,
        uint16_t reg,uint16_t receiver,int32_t receiver_set_index,
        DiamondSpan name,uint8_t arity) {
    uint16_t receiver_type;
    if(receiver_set_index>=0) {
        if((size_t)receiver_set_index>=compiler->function->type_set_count)return;
        const DiamondTypeSet *receiver_set=
            &compiler->function->type_sets[(size_t)receiver_set_index];
        if(receiver_set->count!=1)return;
        receiver_type=receiver_set->members[0].id;
    } else {
        /* No registered type set at all -- the common case for a plain
         * scalar String local (parse_string never registers one; there's
         * no element type to track), which Array/Hash literals always
         * get via record_collection_type_set regardless of this
         * function's own needs. Fall back to the receiver's own plain
         * known_types[] scalar tag directly. */
        receiver_type=compiler->known_types[receiver];
    }
    if(receiver_type!=DIAMOND_TYPE_STRING&&receiver_type!=DIAMOND_TYPE_ARRAY&&
       receiver_type!=DIAMOND_TYPE_HASH&&receiver_type!=DIAMOND_TYPE_INT&&
       receiver_type!=DIAMOND_TYPE_FLOAT)return;
    char method_name[DIAMOND_MAX_FUNCTION_NAME];
    if(name.length>=sizeof method_name)return;
    for(size_t index=0;index<name.length;index++)
        method_name[index]=compiler->source[name.start+index];
    method_name[name.length]='\0';
    uint16_t return_type=DIAMOND_TYPE_NONE;
    if(diamond_native_method_satisfies(receiver_type,method_name,arity,&return_type)&&
       return_type!=DIAMOND_TYPE_NONE)
        compiler->known_types[reg]=return_type;
}

int32_t joined_collection_argument(Compiler *compiler,
        int32_t collection_set_index,uint16_t collection_type,bool second) {
    if(collection_set_index<0||
       (size_t)collection_set_index>=compiler->function->type_set_count)return -1;
    const DiamondTypeSet collection=
        compiler->function->type_sets[(size_t)collection_set_index];
    uint16_t arguments[DIAMOND_MAX_UNION_TYPES];
    if(collection.count==0)return -1;
    for(size_t index=0;index<collection.count;index++) {
        const DiamondTypeMember member=collection.members[index];
        if(member.id!=collection_type)return -1;
        arguments[index]=second?member.second_argument_set:member.argument_set;
        if(arguments[index]==DIAMOND_NO_TYPE_SET)return -1;
    }
    int32_t joined=-1;
    for(size_t index=0;index<collection.count;index++) {
        joined=joined<0?(int32_t)arguments[index]:
            join_type_set_indices(compiler,joined,(int32_t)arguments[index]);
        if(joined<0)return -1;
    }
    return joined;
}

/* These Array transforms copy/subset/reorder the source elements, so a callback
 * (`select`, `reject`, `sort_by`) cannot change the element graph. Carry an
 * inferred factory graph to the editor without narrowing checked contracts.
 * Callbacks that produce new elements need their own result inference; methods
 * returning the receiver itself also need alias tracking before joining this
 * path. */
static void publish_array_transform_tooling_type(Compiler *compiler,
        uint16_t result,uint16_t receiver,DiamondSpan name) {
    if(!name_equals(compiler,"reverse",name,false)&&
       !name_equals(compiler,"uniq",name,false)&&
       !name_equals(compiler,"compact",name,false)&&
       !name_equals(compiler,"take",name,false)&&
       !name_equals(compiler,"drop",name,false)&&
       !name_equals(compiler,"sort",name,false)&&
       !name_equals(compiler,"sort_by",name,false)&&
       !name_equals(compiler,"select",name,false)&&
       !name_equals(compiler,"reject",name,false))return;
    const int32_t elements=joined_collection_argument(compiler,
        compiler->tooling_type_sets[receiver],DIAMOND_TYPE_ARRAY,false);
    if(elements>=0)compiler->tooling_type_sets[result]=collection_type_set(
        compiler,DIAMOND_TYPE_ARRAY,elements,-1);
}

static int32_t joined_callable_return(Compiler *compiler,uint16_t reg) {
    const int32_t set_index=compiler->known_type_sets[reg];
    if(set_index<0||(size_t)set_index>=compiler->function->type_set_count)
        return -1;
    const DiamondTypeSet callable=
        compiler->function->type_sets[(size_t)set_index];
    uint16_t returns[DIAMOND_MAX_UNION_TYPES];
    if(callable.count==0)return -1;
    for(size_t index=0;index<callable.count;index++) {
        const DiamondTypeMember member=callable.members[index];
        if(member.id!=DIAMOND_TYPE_CALLABLE||
           member.callable_return_set==DIAMOND_NO_TYPE_SET)return -1;
        returns[index]=member.callable_return_set;
    }
    int32_t joined=-1;
    for(size_t index=0;index<callable.count;index++) {
        joined=joined<0?(int32_t)returns[index]:
            join_type_set_indices(compiler,joined,(int32_t)returns[index]);
        if(joined<0)return -1;
    }
    return joined;
}

static void publish_nested_array_result(Compiler *compiler,uint16_t reg,
        int32_t element_set) {
    record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,element_set,-1);
    const int32_t inner=compiler->known_type_sets[reg];
    if(inner>=0)record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,
        inner,-1);
}

/* Describes a block's result graph on its closure register for the editor
 * only, so `map`/`flat_map` can relay it. Checked facts are untouched. */
void publish_block_tooling_callable(Compiler *compiler,uint16_t reg,
        const DiamondFunction *block,uint16_t return_set) {
    const uint16_t cloned=clone_type_set_into_current(compiler,block->type_sets,
        block->type_set_count,return_set);
    if(cloned==DIAMOND_NO_TYPE_SET||!reserve_type_sets(compiler,1))return;
    DiamondTypeMember callable={.id=DIAMOND_TYPE_CALLABLE,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=cloned,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        callable.callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;set->inferred=true;set->members[0]=callable;
    compiler->tooling_type_sets[reg]=(int32_t)set_index;
}

/* `map` and `flat_map` over an Array with a known element graph: the result
 * elements are whatever the callback returns. Editor-only, like the copying
 * transforms above; an unknown callback result leaves the result unresolved. */
static void publish_array_map_tooling_type(Compiler *compiler,uint16_t result,
        uint16_t receiver,DiamondSpan name,const uint16_t *arguments,
        size_t argument_count) {
    const bool flat=name_equals(compiler,"flat_map",name,false);
    if(argument_count!=1||(!flat&&!name_equals(compiler,"map",name,false)))return;
    const int32_t source=compiler->tooling_type_sets[receiver]>=0?
        compiler->tooling_type_sets[receiver]:compiler->known_type_sets[receiver];
    if(joined_collection_argument(compiler,source,DIAMOND_TYPE_ARRAY,false)<0)
        return;
    const int32_t callable=compiler->tooling_type_sets[arguments[0]];
    if(callable<0)return;
    int32_t mapped=-1;
    {
        const DiamondTypeSet set=compiler->function->type_sets[(size_t)callable];
        if(set.count!=1||set.members[0].id!=DIAMOND_TYPE_CALLABLE||
           set.members[0].callable_return_set==DIAMOND_NO_TYPE_SET)return;
        mapped=(int32_t)set.members[0].callable_return_set;
    }
    const int32_t elements=flat?joined_collection_argument(compiler,mapped,
        DIAMOND_TYPE_ARRAY,false):mapped;
    if(elements>=0)compiler->tooling_type_sets[result]=collection_type_set(
        compiler,DIAMOND_TYPE_ARRAY,elements,-1);
}

static void publish_collection_argument_return_type(Compiler *compiler,
        uint16_t reg,int32_t receiver_set_index,DiamondSpan name,
        const uint16_t *arguments,size_t argument_count) {
    const CollectionRelay relay=collection_relay(compiler,name);
    const int32_t array_elements=joined_collection_argument(compiler,
        receiver_set_index,DIAMOND_TYPE_ARRAY,false);
    if(array_elements>=0) {
        if(argument_count==1&&relay==COLLECTION_RELAY_FALLBACK) {
            const int32_t fallback=joined_value_type_set(compiler,arguments,1);
            const int32_t joined=join_type_set_indices(compiler,array_elements,
                fallback);
            if(joined>=0)publish_known_type_set(compiler,reg,(uint16_t)joined);
        } else if(argument_count==1&&relay==COLLECTION_RELAY_CONCAT) {
            const int32_t other=joined_collection_argument(compiler,
                compiler->known_type_sets[arguments[0]],DIAMOND_TYPE_ARRAY,
                false);
            const int32_t joined=join_type_set_indices(compiler,array_elements,
                other);
            if(joined>=0)record_collection_type_set(compiler,reg,
                DIAMOND_TYPE_ARRAY,joined,-1);
        } else if(argument_count==1&&relay==COLLECTION_RELAY_MAP) {
            const int32_t mapped=joined_callable_return(compiler,arguments[0]);
            if(mapped>=0)record_collection_type_set(compiler,reg,
                DIAMOND_TYPE_ARRAY,mapped,-1);
        } else if(argument_count==1&&relay==COLLECTION_RELAY_FLAT_MAP) {
            const int32_t mapped=joined_callable_return(compiler,arguments[0]);
            const int32_t element=joined_collection_argument(compiler,mapped,
                DIAMOND_TYPE_ARRAY,false);
            if(element>=0)record_collection_type_set(compiler,reg,
                DIAMOND_TYPE_ARRAY,element,-1);
        } else if(argument_count==1&&relay==COLLECTION_RELAY_NESTED_ARRAY) {
            publish_nested_array_result(compiler,reg,array_elements);
        } else if(argument_count==1&&relay==COLLECTION_RELAY_GROUP_BY) {
            const int32_t key=joined_callable_return(compiler,arguments[0]);
            if(key>=0) {
                record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,
                    array_elements,-1);
                const int32_t grouped=compiler->known_type_sets[reg];
                if(grouped>=0)record_collection_type_set(compiler,reg,
                    DIAMOND_TYPE_HASH,key,grouped);
            }
        } else if(argument_count==1&&relay==COLLECTION_RELAY_ZIP) {
            const int32_t other=joined_collection_argument(compiler,
                compiler->known_type_sets[arguments[0]],DIAMOND_TYPE_ARRAY,
                false);
            const int32_t nullable_other=other<0?-1:
                type_set_with_nil(compiler,(uint16_t)other);
            const int32_t pair=join_type_set_indices(compiler,array_elements,
                nullable_other);
            if(pair>=0)publish_nested_array_result(compiler,reg,pair);
        } else if(argument_count==0&&relay==COLLECTION_RELAY_TALLY) {
            const uint16_t count=concrete_type_set(compiler,DIAMOND_TYPE_INT);
            if(count!=DIAMOND_NO_TYPE_SET)
                record_collection_type_set(compiler,reg,DIAMOND_TYPE_HASH,
                    array_elements,(int32_t)count);
        }
        return;
    }
    const int32_t hash_keys=joined_collection_argument(compiler,
        receiver_set_index,DIAMOND_TYPE_HASH,false);
    const int32_t hash_values=joined_collection_argument(compiler,
        receiver_set_index,DIAMOND_TYPE_HASH,true);
    if(hash_keys<0||hash_values<0)return;
    if(argument_count==2&&relay==COLLECTION_RELAY_FETCH) {
        const int32_t fallback=joined_value_type_set(compiler,&arguments[1],1);
        const int32_t joined=join_type_set_indices(compiler,hash_values,fallback);
        if(joined>=0)publish_known_type_set(compiler,reg,(uint16_t)joined);
    } else if(argument_count==1&&relay==COLLECTION_RELAY_MERGE) {
        const int32_t other_set=compiler->known_type_sets[arguments[0]];
        const int32_t other_keys=joined_collection_argument(compiler,other_set,
            DIAMOND_TYPE_HASH,false);
        const int32_t other_values=joined_collection_argument(compiler,other_set,
            DIAMOND_TYPE_HASH,true);
        const int32_t keys=join_type_set_indices(compiler,hash_keys,other_keys);
        const int32_t values=join_type_set_indices(compiler,hash_values,
            other_values);
        if(keys>=0&&values>=0)record_collection_type_set(compiler,reg,
            DIAMOND_TYPE_HASH,keys,values);
    } else if(argument_count==1&&relay==COLLECTION_RELAY_MAP_VALUES) {
        const int32_t mapped=joined_callable_return(compiler,arguments[0]);
        if(mapped>=0)record_collection_type_set(compiler,reg,
            DIAMOND_TYPE_HASH,hash_keys,mapped);
    }
}

static int find_keyword_argument(const Compiler *compiler,const char *wanted,
        const DiamondSpan *names,size_t count) {
    for(size_t index=0;index<count;index++)
        if(name_equals(compiler,wanted,names[index],false))return (int)index;
    return -1;
}

static void publish_collection_keyword_return_type(Compiler *compiler,
        uint16_t reg,int32_t receiver_set_index,DiamondSpan method_name,
        const DiamondSpan *names,const uint16_t *values,size_t count) {
    const CollectionRelay relay=collection_relay(compiler,method_name);
    const char *wanted[2]={nullptr,nullptr};size_t wanted_count=0;
    if(relay==COLLECTION_RELAY_FALLBACK) {
        wanted[0]="fallback";wanted_count=1;
    } else if(relay==COLLECTION_RELAY_CONCAT) {
        wanted[0]="other";wanted_count=1;
    } else if(relay==COLLECTION_RELAY_MERGE) {
        wanted[0]="other";wanted_count=1;
    } else if(relay==COLLECTION_RELAY_FETCH) {
        wanted[0]="key";wanted[1]="fallback";wanted_count=2;
    } else return;
    uint16_t ordered[2];
    for(size_t index=0;index<wanted_count;index++) {
        const int found=find_keyword_argument(compiler,wanted[index],names,count);
        if(found<0)return;
        ordered[index]=values[(size_t)found];
    }
    publish_collection_argument_return_type(compiler,reg,receiver_set_index,
        method_name,ordered,wanted_count);
}

static bool register_holds_collection(const Compiler *compiler,uint16_t reg) {
    if(compiler->known_types[reg]==DIAMOND_TYPE_ARRAY||
       compiler->known_types[reg]==DIAMOND_TYPE_HASH)return true;
    const int32_t tooling=compiler->tooling_type_sets[reg];
    if(tooling<0||(size_t)tooling>=compiler->function->type_set_count)return false;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)tooling];
    return set->count==1&&(set->members[0].id==DIAMOND_TYPE_ARRAY||
        set->members[0].id==DIAMOND_TYPE_HASH);
}

static uint32_t fresh_alias_identity(Compiler *compiler) {
    compiler->next_alias_identity++;
    if(compiler->next_alias_identity==0)compiler->next_alias_identity++;
    return compiler->next_alias_identity;
}

uint32_t alias_identity_for_value(Compiler *compiler,uint16_t reg) {
    if(!register_holds_collection(compiler,reg))return 0;
    for(size_t index=compiler->local_count;index>0;index--)
        if(compiler->locals[index-1].reg==reg) {
            if(compiler->locals[index-1].alias_identity==0)
                compiler->locals[index-1].alias_identity=
                    fresh_alias_identity(compiler);
            return compiler->locals[index-1].alias_identity;
        }
    return fresh_alias_identity(compiler);
}

void propagate_collection_alias_fact(Compiler *compiler,uint16_t reg) {
    uint32_t identity=0;
    for(size_t index=compiler->local_count;index>0;index--)
        if(compiler->locals[index-1].reg==reg) {
            if(compiler->locals[index-1].alias_identity==0)
                compiler->locals[index-1].alias_identity=
                    fresh_alias_identity(compiler);
            identity=compiler->locals[index-1].alias_identity;break;
        }
    if(identity==0)return;
    for(size_t index=0;index<compiler->local_count;index++) {
        Local *local=&compiler->locals[index];
        if(local->alias_identity!=identity||local->reg==reg)continue;
        compiler->known_types[local->reg]=compiler->known_types[reg];
        compiler->known_type_sets[local->reg]=compiler->known_type_sets[reg];
        compiler->tooling_type_sets[local->reg]=compiler->tooling_type_sets[reg];
        record_scope_type_fact(compiler,local->reg,compiler->current.span.start);
    }
}

static void record_mutated_collection_fact(Compiler *compiler,uint16_t reg,
        uint16_t collection_type,int32_t first,int32_t second) {
    record_collection_type_set(compiler,reg,collection_type,first,second);
    compiler->tooling_type_sets[reg]=-1;
    for(size_t index=0;index<compiler->local_count;index++)
        if(compiler->locals[index].reg==reg) {
            record_scope_type_fact(compiler,reg,compiler->current.span.start);
            break;
        }
    propagate_collection_alias_fact(compiler,reg);
}

static void clear_mutated_collection_fact(Compiler *compiler,uint16_t reg,
        uint16_t collection_type) {
    compiler->known_types[reg]=collection_type;
    compiler->known_type_sets[reg]=-1;
    compiler->tooling_type_sets[reg]=-1;
    for(size_t index=0;index<compiler->local_count;index++)
        if(compiler->locals[index].reg==reg) {
            record_scope_type_fact(compiler,reg,compiler->current.span.start);
            break;
        }
    propagate_collection_alias_fact(compiler,reg);
}

/* Mirrors record_mutated_collection_fact/clear_mutated_collection_fact, one
 * level removed: `mutation_receiver` just finished widening (or losing) its
 * own element/key/value fact from an ordinary flat mutation. If it was
 * itself read via one INDEX_GET off a tracked root local (set_index_
 * provenance, defined further down alongside register_is_local -- this
 * function only reads the plain arrays it fills in, no ordering issue),
 * fold that same post-mutation fact back into the root's own element
 * contract -- root's element type describes *every* element uniformly (an
 * Array[T] contract, not a per-index one), so this widens unconditionally
 * rather than trying to track which index changed. Only Array roots are
 * handled: a Hash receiver's own indexed read already carries a `| Nil` arm
 * (parse_index's own type_set_with_nil), which fails joined_collection_
 * argument's single-kind check before this would even be reached, so
 * hash-rooted nesting stays exactly as conservative as it was before this
 * existed. */
void propagate_nested_mutation_writeback(Compiler *compiler,
        uint16_t mutation_receiver) {
    if(!compiler->has_index_provenance[mutation_receiver])return;
    const uint16_t root=compiler->index_provenance_root[mutation_receiver];
    if(compiler->known_types[root]!=DIAMOND_TYPE_ARRAY)return;
    const int32_t new_element_set=compiler->known_type_sets[mutation_receiver];
    if(new_element_set>=0)
        record_mutated_collection_fact(compiler,root,DIAMOND_TYPE_ARRAY,
            new_element_set,-1);
    else clear_mutated_collection_fact(compiler,root,DIAMOND_TYPE_ARRAY);
}

void update_collection_mutation_type(Compiler *compiler,uint16_t receiver,
        const uint16_t *keys,size_t key_count,const uint16_t *values,
        size_t value_count) {
    if(value_count==0)return;
    /* An inferred collection still needs alias invalidation, but its editor
     * graph must not become a checked collection contract through mutation. */
    if(compiler->known_types[receiver]!=DIAMOND_TYPE_ARRAY&&
       compiler->known_types[receiver]!=DIAMOND_TYPE_HASH&&
       register_holds_collection(compiler,receiver)) {
        compiler->tooling_type_sets[receiver]=-1;
        record_scope_type_fact(compiler,receiver,compiler->current.span.start);
        propagate_collection_alias_fact(compiler,receiver);
        return;
    }
    const int32_t receiver_set=compiler->known_type_sets[receiver];
    const int32_t array_elements=joined_collection_argument(compiler,
        receiver_set,DIAMOND_TYPE_ARRAY,false);
    const bool array_receiver=array_elements>=0||
        compiler->known_types[receiver]==DIAMOND_TYPE_ARRAY;
    const int32_t hash_keys=joined_collection_argument(compiler,receiver_set,
        DIAMOND_TYPE_HASH,false);
    const int32_t hash_values=joined_collection_argument(compiler,receiver_set,
        DIAMOND_TYPE_HASH,true);
    const bool hash_receiver=(hash_keys>=0&&hash_values>=0)||
        compiler->known_types[receiver]==DIAMOND_TYPE_HASH;
    const int32_t added_values=joined_value_type_set(compiler,values,value_count);
    if(added_values<0) {
        if(array_receiver)clear_mutated_collection_fact(compiler,receiver,
            DIAMOND_TYPE_ARRAY);
        else if(hash_receiver)clear_mutated_collection_fact(compiler,receiver,
            DIAMOND_TYPE_HASH);
        return;
    }
    if(array_receiver) {
        const int32_t joined=array_elements<0?added_values:
            join_type_set_indices(compiler,array_elements,added_values);
        if(joined>=0)record_mutated_collection_fact(compiler,receiver,
            DIAMOND_TYPE_ARRAY,joined,-1);
        else clear_mutated_collection_fact(compiler,receiver,
            DIAMOND_TYPE_ARRAY);
        return;
    }
    if(!hash_receiver||key_count==0)return;
    const int32_t added_keys=joined_value_type_set(compiler,keys,key_count);
    if(added_keys<0) {
        clear_mutated_collection_fact(compiler,receiver,DIAMOND_TYPE_HASH);
        return;
    }
    const int32_t joined_keys=hash_keys<0?added_keys:
        join_type_set_indices(compiler,hash_keys,added_keys);
    const int32_t joined_values=hash_values<0?added_values:
        join_type_set_indices(compiler,hash_values,added_values);
    if(joined_keys>=0&&joined_values>=0)
        record_mutated_collection_fact(compiler,receiver,DIAMOND_TYPE_HASH,
            joined_keys,joined_values);
    else clear_mutated_collection_fact(compiler,receiver,DIAMOND_TYPE_HASH);
}

static void publish_array_push_return_type(Compiler *compiler,uint16_t result,
        uint16_t receiver,DiamondSpan name,const uint16_t *arguments,
        size_t argument_count) {
    if(argument_count!=1||
       collection_relay(compiler,name)!=COLLECTION_RELAY_PUSH)return;
    update_collection_mutation_type(compiler,receiver,nullptr,0,arguments,1);
    propagate_nested_mutation_writeback(compiler,receiver);
    const int32_t set=compiler->known_type_sets[receiver];
    if(set>=0)publish_known_type_set(compiler,result,(uint16_t)set);
}

/* Built-in collection methods have deliberately broad Callable signatures in
 * core.d6, but their runtime callback inputs are still determined by the
 * receiver's nested graph. Publish that graph while compiling a trailing
 * anonymous block so its parameters participate in ordinary local inference.
 * This complements (rather than replaces) declared Callable context: user
 * methods and class-owned collection wrappers continue through the declared
 * signature path below. */
static bool prepare_native_collection_block(Compiler *compiler,
        int32_t receiver_set,DiamondSpan name) {
    int32_t parameters[2]={-1,-1};size_t arity=0;
    const int32_t array_elements=joined_collection_argument(compiler,
        receiver_set,DIAMOND_TYPE_ARRAY,false);
    const int32_t hash_keys=joined_collection_argument(compiler,
        receiver_set,DIAMOND_TYPE_HASH,false);
    const int32_t hash_values=joined_collection_argument(compiler,
        receiver_set,DIAMOND_TYPE_HASH,true);
    const bool each=name_equals(compiler,"each",name,false);
    const bool one_element=each||name_equals(compiler,"each_until",name,false)||
        name_equals(compiler,"map",name,false)||
        name_equals(compiler,"flat_map",name,false)||
        name_equals(compiler,"select",name,false)||
        name_equals(compiler,"reject",name,false)||
        name_equals(compiler,"sort_by",name,false)||
        name_equals(compiler,"min_by",name,false)||
        name_equals(compiler,"max_by",name,false)||
        name_equals(compiler,"find",name,false)||
        name_equals(compiler,"group_by",name,false)||
        name_equals(compiler,"partition",name,false);
    if(array_elements>=0&&one_element) {
        parameters[0]=array_elements;arity=1;
    } else if(array_elements>=0&&
              name_equals(compiler,"each_with_index",name,false)) {
        parameters[0]=array_elements;
        const uint16_t integer=concrete_type_set(compiler,DIAMOND_TYPE_INT);
        if(integer==DIAMOND_NO_TYPE_SET)return false;
        parameters[1]=(int32_t)integer;arity=2;
    } else if(hash_keys>=0&&hash_values>=0&&
              (each||name_equals(compiler,"map",name,false)||
               name_equals(compiler,"select",name,false)||
               name_equals(compiler,"reject",name,false))) {
        parameters[0]=hash_keys;parameters[1]=hash_values;arity=2;
    } else if(hash_keys>=0&&hash_values>=0&&
              name_equals(compiler,"map_values",name,false)) {
        parameters[0]=hash_values;arity=1;
    } else if(hash_keys>=0&&hash_values>=0&&
              name_equals(compiler,"each_until",name,false)) {
        parameters[0]=hash_values;arity=1;
    } else return false;
    compiler->has_contextual_block_types=true;
    compiler->contextual_block_arity=(uint8_t)arity;
    compiler->contextual_block_return_set=-1;
    for(size_t index=0;index<arity;index++) {
        compiler->contextual_block_type_sets[index]=parameters[index];
        compiler->contextual_block_types[index]=TYPE_UNKNOWN;
        const DiamondTypeSet *set=&compiler->function->type_sets[
            (size_t)parameters[index]];
        if(set->count==1&&set->members[0].id<DIAMOND_TYPE_VARIABLE_BASE)
            compiler->contextual_block_types[index]=set->members[0].id;
    }
    return true;
}

static uint16_t compile_instance_contextual_block(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_index,
        const uint16_t *type_arguments,size_t type_argument_count,
        int32_t receiver_set,int32_t tooling_receiver_set,DiamondSpan name) {
    if(target!=nullptr) {
        if(target->class_type_variable) {
            if(target->type_variable_count==0||target->type_variable_count>8) {
                fail(compiler,name,"invalid generic method metadata");return 0;
            }
            uint16_t bindings[8];
            for(size_t i=0;i<8;i++)bindings[i]=DIAMOND_NO_TYPE_SET;
            const size_t offset=type_argument_count==target->type_variable_count?0u:target->class_type_variable;
            for(size_t i=0;i<type_argument_count&&i+offset<8;i++)
                bindings[i+offset]=type_arguments[i];
            for(size_t variable=0;variable<target->class_type_variable;variable++)bindings[variable]=DIAMOND_NO_TYPE_SET;
            if(receiver_set>=0&&(size_t)receiver_set<compiler->function->type_set_count) {
                const DiamondTypeSet *known=&compiler->function->type_sets[receiver_set];
                if(known->count==1&&known->members[0].id==
                   DIAMOND_TYPE_CLASS_BASE+target->owner_class)
                    {bindings[0]=known->members[0].argument_set;
                    if(target->class_type_variable==2)bindings[1]=known->members[0].second_argument_set;}
            }
            return compile_contextual_typed_block(compiler,target,parameter_index,
                bindings,target->type_variable_count);
        }
        return compile_contextual_typed_block(compiler,target,
            parameter_index,type_arguments,type_argument_count);
    }
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    compiler->contextual_block_tooling_only=false;
    if(!prepare_native_collection_block(compiler,receiver_set,name)&&
       receiver_set<0&&tooling_receiver_set>=0&&
       prepare_native_collection_block(compiler,tooling_receiver_set,name))
        compiler->contextual_block_tooling_only=true;
    const uint16_t block=compile_block(compiler);
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    compiler->contextual_block_tooling_only=false;
    return block;
}

int diamond_reader_return_class(DiamondClass *const *classes,size_t class_count,
        const DiamondFunction *function,size_t receiver_class) {
    if(function->return_type_set!=DIAMOND_NO_TYPE_SET||
       function->tooling_reader_field==0||function->owner_class>=class_count||
       receiver_class>=class_count)return -1;
    const DiamondClass *owner=classes[function->owner_class];
    const size_t owner_field=(size_t)function->tooling_reader_field-1;
    if(owner_field>=owner->field_count)return -1;
    const char *name=owner->fields[owner_field];
    int result=-1;bool found_owner=false;
    /* Revisit every ancestor's final facts instead of trusting the field
     * snapshot copied when a subclass was declared: a later reopening can
     * invalidate that snapshot. Keep all writes, even overridden initializer
     * writes, since an explicit super call can still execute them. */
    for(size_t depth=0;receiver_class<class_count&&depth<class_count;depth++) {
        const DiamondClass *class=classes[receiver_class];
        if(receiver_class==function->owner_class)found_owner=true;
        for(size_t field=0;field<class->field_count;field++) {
            if(strcmp(class->fields[field],name)!=0)continue;
            const uint8_t status=class->declared_by_discovery?
                class->field_type_status[field]:
                class->discovered_field_type_status[field];
            const uint16_t known=class->declared_by_discovery?
                class->field_known_class[field]:
                class->discovered_field_known_class[field];
            if(status==2)return -1;
            if(status==1) {
                if(known>=class_count||(result>=0&&result!=known))return -1;
                result=known;
            }
            break;
        }
        receiver_class=class->superclass;
    }
    return found_owner?result:-1;
}

/* Return true when reader metadata owns this call, including an unresolved
 * reader. Never fall back to the shared function's owner-class guess. */
static bool publish_reader_return_type(Compiler *compiler,uint16_t reg,
        uint16_t receiver,int32_t receiver_set,DiamondSpan name) {
    uint16_t types[DIAMOND_MAX_UNION_TYPES];size_t count=0;
    const uint16_t known=compiler->known_types[receiver];
    if(known>=DIAMOND_TYPE_CLASS_BASE&&known<DIAMOND_TYPE_VARIABLE_BASE)
        types[count++]=known;
    else {
        if(receiver_set<0)receiver_set=compiler->tooling_type_sets[receiver];
        if(receiver_set<0||
           (size_t)receiver_set>=compiler->function->type_set_count)return false;
        const DiamondTypeSet *set=&compiler->function->type_sets[receiver_set];
        count=set->count;
        for(size_t index=0;index<count;index++)types[index]=set->members[index].id;
    }
    bool reader=false,complete=count>0;
    int returns[DIAMOND_MAX_UNION_TYPES];
    for(size_t index=0;index<count;index++) {
        returns[index]=-1;
        const DiamondFunction *target=class_instance_signature(compiler,
            types[index],name);
        if(target!=nullptr&&target->tooling_reader_field!=0) {
            reader=true;
            returns[index]=diamond_reader_return_class(compiler->program->classes,
                compiler->program->class_count,target,
                (size_t)(types[index]-DIAMOND_TYPE_CLASS_BASE));
        }
        if(returns[index]<0)complete=false;
    }
    if(!reader)return false;
    if(!complete)return true;
    int32_t joined=-1;
    for(size_t index=0;index<count;index++) {
        const uint16_t current=concrete_type_set(compiler,
            (uint16_t)(DIAMOND_TYPE_CLASS_BASE+returns[index]));
        if(current==DIAMOND_NO_TYPE_SET)return true;
        joined=joined<0?(int32_t)current:
            join_type_set_indices(compiler,joined,(int32_t)current);
        if(joined<0)return true;
    }
    compiler->tooling_type_sets[reg]=joined;
    return true;
}

static const DiamondFunction *tooling_forward_instance_target(
        Compiler *compiler,uint16_t receiver,DiamondSpan name) {
    if(compiler->tooling_forward_calls==nullptr)return nullptr;
    const uint16_t type=compiler->known_types[receiver];
    if(type<DIAMOND_TYPE_CLASS_BASE||type>=DIAMOND_TYPE_VARIABLE_BASE)return nullptr;
    size_t class_index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
    for(size_t depth=0;class_index<compiler->program->class_count&&
            depth<compiler->program->class_count;depth++) {
        const DiamondClass *class=compiler->program->classes[class_index];
        for(size_t index=0;index<class->method_count;index++)
            if(singleton_call_name_equals(compiler,class->methods[index].name,name))
                return nullptr;
        for(size_t index=0;index<class->discovered_method_count;index++) {
            const uint16_t id=class->discovered_methods[index];
            if(id>=compiler->program->function_count)continue;
            const DiamondFunction *target=compiler->program->functions[id];
            if(target->declared_by_discovery&&
               singleton_call_name_equals(compiler,target->name,name))return target;
        }
        class_index=class->superclass;
    }
    return nullptr;
}

void publish_instance_return_type(Compiler *compiler,uint16_t reg,
        uint16_t receiver,
        int32_t receiver_set_index,DiamondSpan name,
        const DiamondFunction *matching_target,const uint16_t *bindings,
        size_t binding_count) {
    const DiamondFunction *forward_target=
        tooling_forward_instance_target(compiler,receiver,name);
    if(forward_target!=nullptr)matching_target=forward_target;
    if(publish_reader_return_type(compiler,reg,receiver,receiver_set_index,name)) {
        /* Only advisory scope metadata was published. */
    } else if(matching_target!=nullptr&&matching_target->class_type_variable) {
        if(matching_target->type_variable_count==0||
           matching_target->type_variable_count>8)return;
        uint16_t resolved[8];
        for(size_t index=0;index<8;index++)resolved[index]=DIAMOND_NO_TYPE_SET;
        /* Explicit arguments omit the receiver prefix; inferred argument arrays
         * already reserve it. Only the receiver supplies those bindings. */
        const size_t offset=binding_count==matching_target->type_variable_count?0u:matching_target->class_type_variable;
        for(size_t index=0;index<binding_count&&index+offset<8;index++)
            resolved[index+offset]=bindings[index];
        for(size_t variable=0;variable<matching_target->class_type_variable;variable++)resolved[variable]=DIAMOND_NO_TYPE_SET;
        int32_t receiver_set=receiver_set_index;
        const bool tooling_only=receiver_set<0;
        if(tooling_only)receiver_set=compiler->tooling_type_sets[receiver];
        if(receiver_set>=0&&(size_t)receiver_set<compiler->function->type_set_count) {
            const DiamondTypeSet *set=&compiler->function->type_sets[receiver_set];
            if(set->count==1&&set->members[0].id==
               DIAMOND_TYPE_CLASS_BASE+matching_target->owner_class)
                {resolved[0]=set->members[0].argument_set;
                    if(matching_target->class_type_variable==2)resolved[1]=set->members[0].second_argument_set;}
        }
        publish_bound_instance_return_type(compiler,reg,matching_target,resolved,
            matching_target->type_variable_count,tooling_only);
    } else if(matching_target!=nullptr)
        publish_call_return_type(compiler,reg,matching_target,bindings,
            binding_count);
    else {
        int32_t effective_set=receiver_set_index;bool tooling_only=false;
        if(effective_set<0) {
            effective_set=compiler->tooling_type_sets[receiver];
            tooling_only=effective_set>=0;
        }
        publish_union_instance_return_type(compiler,reg,effective_set,
            name,bindings,binding_count,tooling_only);
    }
    /* Phase 12 (docs/internal/jit-design.md): snapshot whatever known_
     * types[reg] the real (non-tooling) branches just above set -- either
     * one, matching_target's own declared return type or a union
     * receiver's joined one -- onto DiamondFunction.register_known_class
     * for src/jit.c's later use, bounds-checked since a function with
     * more registers than DIAMOND_JIT_MAX_REGISTERS is already
     * unconditionally JIT-ineligible (see that field's own comment,
     * src/vm.h, for the full reasoning). Reading known_types[reg] here,
     * after the fact, rather than threading a "was this concrete" flag
     * out of publish_call_return_type/publish_union_instance_return_type
     * individually keeps this a single check regardless of which path
     * (or neither -- an inferred-only or unresolved return leaves
     * known_types[reg] exactly as it was before this call, decoding to
     * "not concrete" the same way an untouched register already does)
     * actually fired. */
    if(reg<DIAMOND_JIT_MAX_REGISTERS) {
        const uint16_t known=compiler->known_types[reg];
        /* known_types[reg] holds the full type id (DIAMOND_TYPE_CLASS_BASE
         * + class index, compile_assignment_store's own identical decode)
         * -- subtract the base to get the plain class index Phase 10/11's
         * own ivar_known_class/NEW-class-index terminals already use. */
        compiler->function->register_known_class[reg]=
            known>=DIAMOND_TYPE_CLASS_BASE&&known<DIAMOND_TYPE_VARIABLE_BASE?
                (uint16_t)(known-DIAMOND_TYPE_CLASS_BASE):DIAMOND_NO_CLASS;
    }
    publish_collection_method_return_type(compiler,reg,receiver_set_index,name);
    publish_array_transform_tooling_type(compiler,reg,receiver,name);
}

/* `receiver.method` -- a capturing, variadic Callable whose single captured
 * value is the receiver and whose collected positional arguments are forwarded
 * through the ordinary dynamic INVOKE_SPREAD matrix. */
static uint16_t parse_bound_method_reference(Compiler *compiler,uint16_t receiver,
        DiamondSpan method_name,const uint16_t *type_arguments,
        size_t type_argument_count) {
    const DiamondFunction *target=
        instance_call_signature(compiler,receiver,method_name,true);
    for(size_t i=0;i<type_argument_count;i++)
        if(type_set_contains_variable(compiler,type_arguments[i])) {
            fail(compiler,method_name,"bound method references require concrete type arguments");
            return 0;
        }
    uint16_t inferred_arguments[8];
    if(target!=nullptr&&target->type_variable_count>0&&
       type_argument_count==0&&infer_reference_type_arguments(compiler,target,
           0,target->arity>0?target->arity-1:0,inferred_arguments)) {
        type_arguments=inferred_arguments;
        type_argument_count=target->type_variable_count;
    }
    uint16_t class_bindings[8];
    const uint16_t *contract_arguments=type_arguments;
    size_t contract_count=type_argument_count;
    if(target!=nullptr&&target->class_type_variable) {
        for(size_t i=0;i<8;i++)class_bindings[i]=DIAMOND_NO_TYPE_SET;
        const int32_t receiver_set=compiler->known_type_sets[receiver];
        if(receiver_set>=0) {
            const DiamondTypeSet *known=&compiler->function->type_sets[receiver_set];
            if(known->count==1&&known->members[0].id==DIAMOND_TYPE_CLASS_BASE+target->owner_class)
                {class_bindings[0]=known->members[0].argument_set;
                    if(target->class_type_variable==2)class_bindings[1]=known->members[0].second_argument_set;}
        }
        for(size_t i=0;i<type_argument_count&&i+target->class_type_variable<8;i++)class_bindings[i+target->class_type_variable]=type_arguments[i];
        contract_arguments=class_bindings;contract_count=target->type_variable_count;
    }
    const bool typed_wrapper=target!=nullptr&&target->arity>0&&
        target->type_variable_count<=8&&
        (!target->class_type_variable||(!target->has_variadic&&target->type_variable_count>0))&&
        (target->type_variable_count==0||
         type_argument_count==target->type_variable_count-(target->class_type_variable));
    const bool checked_class_reference=typed_wrapper&&target->class_type_variable;
    uint16_t parameter_sets[DIAMOND_MAX_DECLARED_PARAMETERS];
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
        parameter_sets[index]=DIAMOND_NO_TYPE_SET;
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    if(typed_wrapper) {
        bool resolved=true;
        const size_t original_count=compiler->function->type_set_count;
        for(size_t parameter=0;parameter+1<target->arity&&parameter<DIAMOND_MAX_DECLARED_PARAMETERS;
            parameter++) {
            const uint16_t source=target->parameter_type_sets[parameter];
            if(source==DIAMOND_NO_TYPE_SET)continue;
            parameter_sets[parameter]=target->type_variable_count>0?
                clone_substituted_type_set(compiler,target,source,
                    contract_arguments,contract_count,&resolved):
                clone_type_set_into_current(compiler,target->type_sets,
                    target->type_set_count,source);
        }
        if(target->return_type_set!=DIAMOND_NO_TYPE_SET)
            return_set=target->type_variable_count>0?
                clone_substituted_type_set(compiler,target,
                    target->return_type_set,contract_arguments,contract_count,
                    &resolved):clone_type_set_into_current(compiler,
                    target->type_sets,target->type_set_count,
                    target->return_type_set);
        if(target->class_type_variable) {
            for(size_t i=0;i<DIAMOND_MAX_DECLARED_PARAMETERS;i++)
                if(parameter_sets[i]!=DIAMOND_NO_TYPE_SET&&
                   type_set_contains_variable(compiler,parameter_sets[i]))resolved=false;
            if(return_set!=DIAMOND_NO_TYPE_SET&&type_set_contains_variable(compiler,return_set))resolved=false;
        }
        if(!resolved) {
            compiler->function->type_set_count=original_count;
            return_set=DIAMOND_NO_TYPE_SET;
            for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
                parameter_sets[index]=DIAMOND_NO_TYPE_SET;
        }
    }
    size_t function_index=0;
    DiamondFunction *wrapper=compiler_add_function(compiler,&function_index);
    if(wrapper==nullptr) {fail(compiler,method_name,"out of memory");return 0;}
    (void)snprintf(wrapper->name,sizeof wrapper->name,"<bound method>");
    wrapper->owner_class=DIAMOND_OWNER_NONE;wrapper->nested=true;
    wrapper->arity=typed_wrapper?(uint8_t)(target->arity-1):1;
    wrapper->required_arity=typed_wrapper?
        (uint8_t)(target->required_arity>0?target->required_arity-1:0):0;
    wrapper->has_variadic=typed_wrapper?target->has_variadic:true;
    wrapper->return_type_set=return_set;
    wrapper->type_set_count=compiler->function->type_set_count;
    if(!diamond_function_reserve_type_sets(wrapper,wrapper->type_set_count)) {
        fail(compiler,method_name,"out of memory");return 0;
    }
    if(wrapper->type_set_count>0)
        memcpy(wrapper->type_sets,compiler->function->type_sets,
            wrapper->type_set_count*sizeof wrapper->type_sets[0]);
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++) {
        wrapper->parameter_type_sets[index]=parameter_sets[index];
        if(typed_wrapper&&index<wrapper->arity)
            (void)snprintf(wrapper->parameter_names[index],
                DIAMOND_MAX_FUNCTION_NAME,"%s",
                target->parameter_names[index]);
    }
    if(!typed_wrapper)
        (void)snprintf(wrapper->parameter_names[0],DIAMOND_MAX_FUNCTION_NAME,
            "arguments");

    const uint16_t captured=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,captured,receiver,0,2);
    emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,captured,0,0,1);

    DiamondFunction *outer_function=compiler->function;
    const uint16_t outer_next_register=compiler->next_register;
    const size_t outer_local_count=compiler->local_count;
    const bool outer_in_function=compiler->in_function;
    const bool outer_in_block=compiler->in_block;
    uint16_t *outer_types=malloc(outer_next_register*sizeof *outer_types);
    int32_t *outer_type_sets=malloc(outer_next_register*sizeof *outer_type_sets);
    if((outer_types==nullptr||outer_type_sets==nullptr)&&outer_next_register>0) {
        free(outer_types);free(outer_type_sets);
        fail(compiler,method_name,"out of memory");return 0;
    }
    memcpy(outer_types,compiler->known_types,outer_next_register*sizeof *outer_types);
    memcpy(outer_type_sets,compiler->known_type_sets,
        outer_next_register*sizeof *outer_type_sets);
    const size_t outer_begin_depth=compiler->begin_depth;
    compiler->function=wrapper;compiler->next_register=0;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->local_count=0;compiler->in_function=true;
    compiler->in_block=false;
    compiler->begin_depth=0;
    uint16_t arguments[DIAMOND_MAX_DECLARED_PARAMETERS];
    const size_t argument_count=typed_wrapper?wrapper->arity:1;
    for(size_t index=0;index<argument_count;index++)
        arguments[index]=allocate_register(compiler);
    if(wrapper->has_variadic) {
        const size_t fixed_count=wrapper->arity-1;
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            arguments[fixed_count],(uint16_t)fixed_count,0,3);
    }
    if(checked_class_reference) {
        /* The wrapper advertises these concrete contracts even if the
         * forwarded method is subsequently replaced. Check them here. */
        for(size_t index=0;index<argument_count;index++) {
            if(parameter_sets[index]==DIAMOND_NO_TYPE_SET)continue;
            size_t skip=SIZE_MAX;
            if(index>=wrapper->required_arity) {
                const uint16_t provided=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,provided,(uint16_t)index,0,2);
                skip=emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,provided);
            }
            emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,arguments[index],parameter_sets[index],0,2);
            if(skip!=SIZE_MAX)patch_jump(compiler,skip,compiler->function->code_count);
        }
    }
    const uint16_t bound_receiver=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,bound_receiver,0,0,2);
    uint16_t body;
    if(typed_wrapper&&!wrapper->has_variadic&&
       wrapper->required_arity<wrapper->arity) {
        for(size_t count=wrapper->required_arity;count<wrapper->arity;count++) {
            const uint16_t provided=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,provided,
                (uint16_t)count,0,2);
            const size_t next=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,
                provided);
            const uint16_t partial=emit_invoke_call(compiler,bound_receiver,
                method_name,false,type_arguments,type_argument_count,
                arguments,count);
            if(checked_class_reference&&return_set!=DIAMOND_NO_TYPE_SET)
                emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,partial,return_set,0,2);
            emit_instruction(compiler,DIAMOND_OP_RETURN,partial,0,0,1);
            patch_jump(compiler,next,compiler->function->code_count);
        }
        body=emit_invoke_call(compiler,bound_receiver,method_name,false,
            type_arguments,type_argument_count,arguments,argument_count);
    } else if(wrapper->has_variadic) {
        const size_t fixed_count=wrapper->arity-1;
        if(typed_wrapper&&wrapper->required_arity<fixed_count)
            for(size_t count=wrapper->required_arity;count<fixed_count;
                count++) {
                const uint16_t provided=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,
                    provided,(uint16_t)count,0,2);
                const size_t next=emit_jump(compiler,
                    DIAMOND_OP_JUMP_IF_TRUE,provided);
                uint16_t partial_spread=arguments[fixed_count];
                if(count>0)partial_spread=emit_build_spread_arguments(
                    compiler,arguments,count,partial_spread,nullptr,0,false);
                const uint16_t partial=emit_invoke_typed_spread(compiler,
                    bound_receiver,method_name,partial_spread,type_arguments,
                    type_argument_count);
                emit_instruction(compiler,DIAMOND_OP_RETURN,partial,0,0,1);
                patch_jump(compiler,next,compiler->function->code_count);
            }
        uint16_t spread=arguments[fixed_count];
        if(fixed_count>0)spread=emit_build_spread_arguments(compiler,arguments,
            fixed_count,spread,nullptr,0,false);
        body=emit_invoke_typed_spread(compiler,bound_receiver,method_name,
            spread,type_arguments,type_argument_count);
    } else body=emit_invoke_call(compiler,bound_receiver,method_name,false,
        type_arguments,type_argument_count,arguments,argument_count);
    if(checked_class_reference&&return_set!=DIAMOND_NO_TYPE_SET)
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,body,return_set,0,2);
    emit_instruction(compiler,DIAMOND_OP_RETURN,body,0,0,1);
    wrapper->register_count=compiler->next_register;

    compiler->function=outer_function;compiler->next_register=outer_next_register;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->local_count=outer_local_count;compiler->in_function=outer_in_function;
    compiler->in_block=outer_in_block;
    compiler->begin_depth=outer_begin_depth;
    memcpy(compiler->known_types,outer_types,outer_next_register*sizeof *outer_types);
    memcpy(compiler->known_type_sets,outer_type_sets,
        outer_next_register*sizeof *outer_type_sets);
    free(outer_types);free(outer_type_sets);
    const uint16_t result=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
    emit_function_index(compiler,function_index);emit_byte(compiler,1);
    emit_register(compiler,captured);
    if(typed_wrapper)publish_function_callable_type(compiler,result,wrapper);
    else compiler->known_types[result]=TYPE_UNKNOWN;
    return result;
}

/* A trailing `do` block may box the receiver's local in place (see the
 * comment at parse_invoke's plain-argument DIAMOND_TOKEN_DO branch), so the
 * receiver is copied into a fresh temp first. `self` inside a method is the
 * exception: it lives in register 0, which a block never boxes in place (it
 * boxes a copy -- see compile_block's self_copy_register), and the VM only
 * lets a private method be called on register 0. Copying it would turn
 * `self.helper() do ... end` into an "explicit receiver" call and reject it. */
static uint16_t snapshot_block_call_receiver(Compiler *compiler,
        uint16_t receiver) {
    if(receiver==0&&compiler->in_method)return receiver;
    const uint16_t snapshot=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,receiver,0,2);
    /* The copy still holds the receiver's editor-only element graph. */
    compiler->tooling_type_sets[snapshot]=compiler->tooling_type_sets[receiver];
    return snapshot;
}

uint16_t parse_invoke(Compiler *compiler, uint16_t receiver) {
    uint16_t mutation_receiver=receiver;
    if(compiler->previous.kind==DIAMOND_TOKEN_IDENTIFIER) {
        const int source_local=find_local(compiler,compiler->previous.span);
        if(source_local>=0)
            mutation_receiver=compiler->locals[(size_t)source_local].reg;
    }
    const int32_t receiver_set_index=compiler->known_type_sets[receiver];
    advance_token(compiler);
    /* DIAMOND_TOKEN_CLASS is otherwise the `class Foo ... end` keyword,
     * but right after '.' it can only ever be a member name (`.class`)
     * -- a class *declaration* never starts mid-expression after a
     * dot, so accepting it here is unambiguous. Needed for the
     * compiler-recognized `value.class()` special form below; nothing
     * else about method-name parsing changes for it (a `.class` used
     * any other way, e.g. `obj.class = 5` or a bare reference, falls
     * through to this function's own ordinary handling exactly like
     * any other name would, and fails at runtime the normal
     * "undefined method" way if nothing real answers to it). */
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER &&
        compiler->current.kind != DIAMOND_TOKEN_CLASS) {
        fail(compiler, compiler->current.span, "expected method name after '.'"); return 0;
    }
    const DiamondSpan name = compiler->current.span;
    advance_token(compiler);
    return parse_invoke_named(compiler,receiver,mutation_receiver,
                              receiver_set_index,name);
}

/* The receiver side of an instance call, as parse_invoke_named resolved it:
 * the receiver register, the register a mutating call (push) updates, the
 * receiver's type set, the method name, and the signatures used for argument
 * and return-type inference (either may be nullptr). */
typedef struct {
    uint16_t receiver;
    uint16_t mutation_receiver;
    int32_t receiver_set_index;
    DiamondSpan name;
    const DiamondFunction *contextual_target;
    const DiamondFunction *return_target;
} InvokeSite;

/* `value.class()`: the current token is the '('. */
static uint16_t parse_class_name_call(Compiler *compiler,uint16_t receiver) {
    advance_token(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"class() takes no arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_CLASS_NAME,destination,receiver,0,2);
    compiler->known_types[destination]=DIAMOND_TYPE_STRING;
    return destination;
}

/* `value.is_a?(Type)`: the type name resolves at compile time; the current
 * token is the '('. */
static uint16_t parse_is_a_call(Compiler *compiler,uint16_t receiver) {
    advance_token(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected a type name");
        return 0;
    }
    const DiamondSpan type_span=compiler->current.span;
    char type_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!consume_qualified_name(compiler,type_name,sizeof type_name)) {
        fail(compiler,type_span,"type name is too long");
        return 0;
    }
    const uint16_t resolved_type=(uint16_t)resolve_type_name(
        compiler,type_name,type_span);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
             "expected ')' after is_a? type");
        return 0;
    }
    advance_token(compiler);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_IS_TYPE,destination,receiver,
                     resolved_type,3);
    compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
    return destination;
}

/* An instance call whose arguments include a keyword argument: the current
 * token is the first argument. */
static uint16_t parse_invoke_keyword_call(Compiler *compiler,const InvokeSite *site,bool writer_name,
        const uint16_t *type_arguments,size_t type_argument_count) {
    uint16_t receiver=site->receiver;
    const uint16_t mutation_receiver=site->mutation_receiver;
    const int32_t receiver_set_index=site->receiver_set_index;
    const DiamondSpan name=site->name;
    const DiamondFunction *contextual_target=site->contextual_target;
    const DiamondFunction *return_target=site->return_target;
    if(writer_name) {fail(compiler,compiler->current.span,
        "keyword method calls do not support writers");return 0;}
    DiamondSpan keyword_names[DIAMOND_MAX_DECLARED_PARAMETERS];uint16_t keyword_values[DIAMOND_MAX_DECLARED_PARAMETERS];
    size_t keyword_count=0;
    const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
        keyword_names,keyword_values,&keyword_count,contextual_target);
    uint16_t inferred_arguments[8];
    if(type_argument_count==0) {
        const size_t block_count=
            compiler->current.kind==DIAMOND_TOKEN_DO?1u:0u;
        const size_t parameter_count=contextual_target==nullptr?0:
            contextual_target->arity>block_count?
                contextual_target->arity-block_count:0;
        infer_contextual_spread_arguments(compiler,contextual_target,
            positional,parameter_count,inferred_arguments);
        infer_contextual_keyword_arguments(compiler,contextual_target,
            keyword_names,keyword_values,keyword_count,inferred_arguments);
    }
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:contextual_target==nullptr?0:
            contextual_target->type_variable_count;
    bool has_block=false;uint16_t block=0;
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        receiver=snapshot_block_call_receiver(compiler,receiver);
        for(size_t index=0;index<keyword_count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                keyword_values[index],0,2);
            keyword_values[index]=snapshot;
        }
        block=compile_instance_contextual_block(compiler,contextual_target,
            contextual_target==nullptr||contextual_target->arity<=1?0:
                contextual_target->arity-2,resolved_arguments,
            resolved_count,receiver_set_index,
            compiler->tooling_type_sets[receiver],name);
        has_block=true;
    }
    const uint16_t result=emit_invoke_keywords(compiler,receiver,name,positional,
        keyword_names,keyword_values,keyword_count,type_arguments,
        type_argument_count,has_block,block);
    publish_instance_return_type(compiler,result,receiver,receiver_set_index,name,
        return_target,resolved_arguments,resolved_count);
    publish_collection_keyword_return_type(compiler,result,
        receiver_set_index,name,keyword_names,keyword_values,keyword_count);
    const int push=find_keyword_argument(compiler,"value",keyword_names,
        keyword_count);
    if(push>=0)publish_array_push_return_type(compiler,result,
        mutation_receiver,name,&keyword_values[(size_t)push],1);
    return result;
}

/* An instance call whose arguments include a spread: the current token is
 * the first argument. */
static uint16_t parse_invoke_spread_call(Compiler *compiler,const InvokeSite *site,bool writer_name,
        const uint16_t *type_arguments,size_t type_argument_count) {
    uint16_t receiver=site->receiver;
    const int32_t receiver_set_index=site->receiver_set_index;
    const DiamondSpan name=site->name;
    const DiamondFunction *contextual_target=site->contextual_target;
    const DiamondFunction *return_target=site->return_target;
    if(writer_name) {
        fail(compiler,compiler->current.span,
            "spread method calls do not support writers");
        return 0;
    }
    uint16_t spread=parse_spread_argument_array(compiler,nullptr,
        nullptr,nullptr,nullptr,contextual_target,
        contextual_target==nullptr||contextual_target->arity==0?0:
            contextual_target->arity-1);
    uint16_t inferred_arguments[8];
    const size_t spread_parameter_count=contextual_target==nullptr?0:
        contextual_target->arity>(compiler->current.kind==DIAMOND_TOKEN_DO?
            2u:1u)?contextual_target->arity-
            (compiler->current.kind==DIAMOND_TOKEN_DO?2u:1u):0;
    const size_t inferred_count=type_argument_count==0?
        infer_contextual_spread_arguments(compiler,contextual_target,
            spread,spread_parameter_count,inferred_arguments):0;
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:inferred_count;
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        receiver=snapshot_block_call_receiver(compiler,receiver);
        const uint16_t spread_snapshot=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
            spread,0,2);
        spread=spread_snapshot;
        const uint16_t block=compile_instance_contextual_block(compiler,
            contextual_target,
                contextual_target==nullptr||contextual_target->arity<=1?0:
                contextual_target->arity-2,resolved_arguments,
            resolved_count,receiver_set_index,
            compiler->tooling_type_sets[receiver],name);
        if(contextual_target!=nullptr) {
            const uint16_t result=emit_invoke_keywords(compiler,receiver,name,spread,
                nullptr,nullptr,0,type_arguments,type_argument_count,true,
                block);
            publish_instance_return_type(compiler,result,receiver,
                receiver_set_index,name,return_target,resolved_arguments,
                resolved_count);
            return result;
        }
        spread=emit_build_spread_arguments(compiler,nullptr,0,spread,
            &block,1,false);
    }
    const uint16_t result=emit_invoke_typed_spread(compiler,receiver,name,
        spread,type_arguments,type_argument_count);
    publish_instance_return_type(compiler,result,receiver,receiver_set_index,name,
        return_target,resolved_arguments,resolved_count);
    return result;
}

/* parse_invoke after `.name` has been consumed -- also reached from
 * parse_call for a bare `name(...)` inside a method, with self as the
 * receiver. */
uint16_t parse_invoke_named(Compiler *compiler, uint16_t receiver,
        uint16_t mutation_receiver, int32_t receiver_set_index, DiamondSpan name) {
    const DiamondFunction *contextual_target=
        instance_call_signature(compiler,receiver,name,false);
    const DiamondFunction *return_target=
        instance_call_signature(compiler,receiver,name,true);
    /* `value.class()`/`value.is_a?(Type)` -- compiler-recognized special
     * forms, not real methods on any class (no DiamondFunction/method-
     * table entry exists for either name anywhere), so they're
     * intercepted here rather than falling into the ordinary dynamic-
     * dispatch INVOKE path below, which would just fail to find a
     * method by that name on most receivers. Checked *after* consuming
     * the name but *before* everything else in this function (writer
     * syntax, generic arguments, keyword arguments) since neither
     * special form supports any of that -- and only when immediately
     * followed by '(' (a real call), so a bare `.class`/`.is_a?` (no
     * parens) still falls through unchanged to this function's own
     * existing bare-reference handling below, matching every other
     * `obj.method` (no parens) in this language already being a method
     * *reference*, not a call.
     *
     * `is_a?`'s own single argument is a type name, not a general
     * expression -- resolved at compile time via resolve_type_name,
     * the exact same helper `rescue error: SomeClass` already uses, so
     * `is_a?` inherits that function's own class/interface/generic/
     * Sized handling for free (this is not merely a narrow "same
     * class" check -- see value_matches_type, src/vm.c). DIAMOND_VALUE_
     * CLASS (value.h) does exist, but stays deliberately narrow (self
     * inside a class-owned singleton method, a bare class name as a
     * `case`/`when` pattern -- see docs/design.md's own section on it)
     * and is never constructed here: there is no general first-class
     * Class value a program could pass as an ordinary runtime argument
     * instead, so this compile-time resolution is the only way `is_a?`
     * could take a type name as its argument at all. */
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"class",name,false)) {
        return parse_class_name_call(compiler,receiver);
    }
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"is_a?",name,false)) {
        return parse_is_a_call(compiler,receiver);
    }
    bool writer_name=false;
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        writer_name=true;advance_token(compiler);
    }
    uint16_t type_arguments[8];size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET &&
       (!writer_name||writer_generic_arguments_ahead(compiler))) {
        if(!parse_explicit_type_arguments(compiler,type_arguments,&type_argument_count))
            return 0;
    }
    /* `recv.attr = value` -- sugar for `recv.attr=(value)`. Only a plain
     * assignment is supported (mirrors compile_assignment's own RHS): one
     * expression, no trailing '(', no do-block (a setter takes exactly one
     * argument, never a block). */
    if (writer_name && compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        const uint16_t value = parse_expression(compiler);
        return emit_invoke_call(compiler, receiver, name, true,
            type_arguments, type_argument_count, &value, 1);
    }
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        if(!writer_name)return parse_bound_method_reference(compiler,receiver,name,
            type_arguments,type_argument_count);
        fail(compiler,compiler->current.span,
            "member access requires a method call with '()'");return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const InvokeSite site={.receiver=receiver,.mutation_receiver=mutation_receiver,
        .receiver_set_index=receiver_set_index,.name=name,
        .contextual_target=contextual_target,.return_target=return_target};
    if(call_arguments_have_keyword(compiler))
        return parse_invoke_keyword_call(compiler,&site,writer_name,type_arguments,
            type_argument_count);
    if(call_arguments_have_spread(compiler))
        return parse_invoke_spread_call(compiler,&site,writer_name,type_arguments,
            type_argument_count);
    uint16_t args[DIAMOND_MAX_ARGUMENTS]; size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == DIAMOND_MAX_ARGUMENTS) { fail(compiler, compiler->current.span, "too many arguments"); break; }
        args[count]=parse_expected_argument(compiler,contextual_target,count);
        count++;
        skip_newlines(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments"); return 0;
    }
    advance_token(compiler);
    uint16_t inferred_arguments[8];
    const size_t inferred_count=type_argument_count==0?
        infer_contextual_type_arguments(compiler,contextual_target,args,count,
            inferred_arguments):0;
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:inferred_count;
    /* `recv.method(args) do |x| ... end` -- the block becomes one more
     * (always last) positional argument, exactly as if it had been
     * written as a named closure and passed explicitly. See compile_
     * block's own comment for the full design. */
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(count==DIAMOND_MAX_ARGUMENTS) {
            fail(compiler,compiler->current.span,"too many arguments");return 0;
        }
        /* The receiver and every argument above were read before any of
         * this call's locals were necessarily marked `captured` yet, so
         * parse_identifier handed back their bare register directly
         * (compiler.c:807-808), aliasing whatever local they came from.
         * If the block below happens to eagerly capture that same local
         * (unconditional -- every enclosing local, whether the block's
         * body actually references it or not), compile_block's own
         * BOX_LOCAL turns that register into a Cell in place *after*
         * this point, and this call would silently read the Cell instead
         * of the value it already resolved. Snapshot the receiver and
         * every argument into fresh temps first: a temp is never a
         * registered local, so BOX_LOCAL's locals-table lookup can never
         * retarget it. */
        receiver=snapshot_block_call_receiver(compiler,receiver);
        for(size_t i=0;i<count;i++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,args[i],0,2);
            args[i]=snapshot;
        }
        const uint16_t block=compile_instance_contextual_block(compiler,
            contextual_target,
            contextual_target==nullptr||contextual_target->arity<=1?0:
                contextual_target->arity-2,resolved_arguments,
            resolved_count,receiver_set_index,
                compiler->tooling_type_sets[receiver],name);
        const size_t block_slot=
            contextual_target==nullptr||contextual_target->arity<=1?0:
                (size_t)contextual_target->arity-2;
        if(contextual_target!=nullptr&&count<block_slot) {
            const uint16_t positional=emit_argument_array(compiler,args,count);
            const uint16_t result=emit_invoke_keywords(compiler,receiver,name,positional,
                nullptr,nullptr,0,type_arguments,type_argument_count,true,block);
            publish_instance_return_type(compiler,result,receiver,receiver_set_index,
                name,return_target,resolved_arguments,resolved_count);
            return result;
        }
        args[count++]=block;
    }
    const uint16_t result=emit_invoke_call(compiler,receiver,name,writer_name,
        type_arguments,type_argument_count,args,count);
    publish_instance_return_type(compiler,result,receiver,receiver_set_index,name,
        return_target,resolved_arguments,resolved_count);
    publish_native_scalar_method_return_type(compiler,result,receiver,
        receiver_set_index,name,(uint8_t)count);
    publish_collection_argument_return_type(compiler,result,receiver_set_index,
        name,args,count);
    publish_array_map_tooling_type(compiler,result,receiver,name,args,count);
    publish_array_push_return_type(compiler,result,mutation_receiver,name,args,
        count);
    return result;
}

uint16_t parse_self_class_method_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected method name after 'self.'");
        return 0;
    }
    /* self.define_method / compile_method / redefine_method: the class is
     * self's, resolved at run time (DIAMOND_CLASS_FROM_SELF), so a base
     * class's singleton method can add methods to the subclass it's called
     * on. The parse helpers take the class index as an int and emit it as a
     * byte, so passing the sentinel is all it takes. */
    if(name_equals(compiler,"define_method",compiler->current.span,false))
        return parse_define_method_call(compiler,DIAMOND_CLASS_FROM_SELF);
    if(name_equals(compiler,"compile_method",compiler->current.span,false))
        return parse_compile_method_call(compiler,DIAMOND_CLASS_FROM_SELF);
    if(name_equals(compiler,"redefine_method",compiler->current.span,false)) {
        compiler->program->uses_redefine_method=true;
        return parse_redefine_method_call(compiler,DIAMOND_CLASS_FROM_SELF);
    }
    const DiamondSpan name=compiler->current.span;
    advance_token(compiler);
    bool writer_name=false;
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        writer_name=true;advance_token(compiler);
    }
    return parse_self_class_method_arguments(compiler,name,writer_name);
}

/* The argument list and optional block of `self.name(...)`, or of a bare
 * `name(...)` that resolved to one of the class's own singleton methods;
 * the name is already consumed and the current token should be '('. */
uint16_t parse_self_class_method_arguments(Compiler *compiler,
        DiamondSpan name,bool writer_name) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             "member access requires a method call with '()'");return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t args[DIAMOND_MAX_ARGUMENTS];size_t count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        if(count==DIAMOND_MAX_ARGUMENTS) {fail(compiler,compiler->current.span,"too many arguments");break;}
        args[count++]=parse_expression(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN)break;
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(count==DIAMOND_MAX_ARGUMENTS) {fail(compiler,compiler->current.span,"too many arguments");return 0;}
        for(size_t i=0;i<count;i++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,args[i],0,2);
            args[i]=snapshot;
        }
        args[count++]=compile_block(compiler);
    }
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count;i++)(void)allocate_register(compiler);
    for(size_t i=0;i<count;i++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i),args[i],0,2);
    const uint16_t dest=allocate_register(compiler);
    const uint16_t method=add_name_string(compiler,name);
    if(writer_name&&!compiler->failed) {
        DiamondStringConstant *string=&compiler->function->strings[method];
        if(string->length==DIAMOND_MAX_STRING_LENGTH)
            fail(compiler,name,"method name is too long");
        else {
            string->chars[string->length++]='=';
            string->chars[string->length]='\0';
        }
    }
    emit_opcode(compiler,DIAMOND_OP_INVOKE_SELF_METHOD);
    emit_register(compiler,dest);
    emit_register(compiler,method);
    emit_register(compiler,base);
    emit_byte(compiler,(uint8_t)count);
    return dest;
}

uint16_t parse_super(Compiler *compiler) {
    const DiamondSpan keyword = compiler->previous.span;
    if (!compiler->in_method || compiler->current_class < 0) {
        fail(compiler, keyword, "'super' used outside a method");
        return 0;
    }
    const DiamondClass *owner =
        compiler->program->classes[(size_t)compiler->current_class];
    if (owner->superclass == DIAMOND_NO_CLASS) {
        fail(compiler, keyword, "'super' used in a class without a superclass");
        return 0;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'super'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t arguments[DIAMOND_MAX_ARGUMENTS];
    size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == DIAMOND_MAX_ARGUMENTS) {
            fail(compiler, compiler->current.span, "too many arguments");
            return 0;
        }
        arguments[count++] = parse_expression(compiler);
        skip_newlines(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base = allocate_register(compiler);
    for (size_t index = 1; index < count; index++) (void)allocate_register(compiler);
    for (size_t index = 0; index < count; index++) {
        emit_instruction(compiler, DIAMOND_OP_MOVE, (uint16_t)(base + index),
                         arguments[index], 0, 2);
    }
    const uint16_t destination = allocate_register(compiler);
    const uint16_t method = add_name_string(compiler, compiler->current_method);
    emit_opcode(compiler, DIAMOND_OP_SUPER);
    emit_register(compiler,destination);
    emit_register(compiler,(uint16_t)compiler->current_class);
    emit_register(compiler,method);
    emit_register(compiler,base);
    emit_byte(compiler, (uint8_t)count);
    return destination;
}
