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

/* Class, module, struct and interface compilation, plus visibility, aliasing and delegation. */

/* Decodes a type_set index (as stored in compiler->function->type_sets,
 * the same table parameter_type_sets/return_type_set/field declarations all
 * index into) to a single concrete class id, or -1 if it isn't exactly one
 * concrete class (a union, an interface/generic-variable member, or
 * DIAMOND_NO_TYPE_SET/out-of-range, spelled here as type_set<0 -- every
 * caller already uses that same sentinel for "no annotation at all"). Mirrors
 * src/jit.c's own parameter_is_single_class decode exactly (same encoding,
 * same "reject anything but a lone concrete class" rule), just reading from
 * compiler->function's still-being-compiled table instead of an already-
 * finished DiamondFunction's. */
static int type_set_single_class(const Compiler *compiler, int type_set) {
    if (type_set < 0 || (size_t)type_set >= compiler->function->type_set_count) return -1;
    const DiamondTypeSet *set = &compiler->function->type_sets[(size_t)type_set];
    if (set->count != 1) return -1;
    const uint16_t id = set->members[0].id;
    if (id < DIAMOND_TYPE_CLASS_BASE || id >= DIAMOND_TYPE_VARIABLE_BASE) return -1;
    return (int)(id - DIAMOND_TYPE_CLASS_BASE);
}

/* Feeds one SET_IVAR site's own known-value-class (or -1, "not a single
 * concrete class") into DiamondClass.field_type_status/field_known_class --
 * the exhaustiveness this LSP-only fact needs to also be safely reusable by
 * the JIT (see DiamondFunction.ivar_known_class's own comment, src/vm.h) is
 * that EVERY place SET_IVAR is ever emitted for a real (non-module) field
 * calls this, not just the ordinary `self.field = value` path. Confirmed by
 * grepping every DIAMOND_OP_SET_IVAR emission site in this file: this
 * function's own two extra call sites below (compile_attribute_named's own
 * writer generation and compile_struct's generated `initialize`) used to
 * bypass field_type_status/field_known_class entirely -- a real,
 * pre-existing soundness gap for any class using attr_accessor or struct to
 * declare a field (i.e. almost every real class), silently leaving lsp/
 * receiver.c's own hover/definition support looking only at whatever a
 * *different* field happened to also be assigned via a bare `self.field =`
 * somewhere, or at nothing at all. Same merge logic in all three cases: the
 * first concrete class seen for a field is trusted; a second, different
 * concrete class, or any non-concrete write at all, permanently marks the
 * field "unknown" (status 2) -- once poisoned, never re-trusted, matching
 * this field's own pre-existing status==0/1/2 semantics exactly. */
static void record_field_known_type(DiamondClass *class, size_t field, int known_class) {
    const bool concrete = known_class >= 0;
    if (class->field_type_status[field] == 0 && concrete) {
        class->field_type_status[field] = 1;
        class->field_known_class[field] = (uint16_t)known_class;
    } else if (!concrete || class->field_known_class[field] != (uint16_t)known_class) {
        class->field_type_status[field] = 2;
    }
}

static void compile_attribute_named(Compiler *compiler,bool writer,bool predicate,
                                    DiamondSpan name,int type_set) {
    if(name.length+(writer||predicate?1u:0u)>=DIAMOND_MAX_FUNCTION_NAME||
       compiler->program->function_count==DIAMOND_MAX_FUNCTIONS) {
        fail(compiler,name,"attribute name is too long or function limit reached");
        return;
    }
    char field_name[DIAMOND_MAX_FUNCTION_NAME];
    for(size_t index=0;index<name.length;index++)
        field_name[index]=compiler->source[name.start+index];
    field_name[name.length]='\0';
    uint8_t field=UINT8_MAX;
    DiamondMethod *method=nullptr;
    char method_name[DIAMOND_MAX_FUNCTION_NAME];
    memcpy(method_name,field_name,name.length);
    size_t method_name_length=name.length;
    if(writer)method_name[method_name_length++]='=';
    else if(predicate)method_name[method_name_length++]='?';
    method_name[method_name_length]='\0';
    if(compiler->current_class>=0) {
        DiamondClass *class=
            compiler->program->classes[(size_t)compiler->current_class];
        for(size_t index=0;index<class->method_count;index++)
            if(!class->methods[index].included&&
               strcmp(class->methods[index].name,method_name)==0) {
                fail(compiler,name,"attribute method is already defined");return;
            }
        for(size_t index=0;index<class->field_count;index++)
            if(strcmp(class->fields[index],field_name)==0)field=(uint8_t)index;
        if(field==UINT8_MAX) {
            if(!diamond_class_reserve_fields(class,class->field_count+1)) {
                fail(compiler,name,"too many instance variables");return;
            }
            field=(uint8_t)class->field_count;
            (void)snprintf(class->fields[class->field_count++],
                DIAMOND_MAX_FUNCTION_NAME,"%s",field_name);
        }
        if(!diamond_reserve_methods(&class->methods,&class->method_capacity,class->method_count+1)) {
            fail(compiler,name,"too many methods");return;
        }
        method=&class->methods[class->method_count++];
    } else {
        DiamondModule *module=
            compiler->program->modules[(size_t)compiler->current_module];
        if(compiler->module_function_mode) {
            fail(compiler,name,
                 "stateful attribute cannot use module_function mode");return;
        }
        for(size_t index=0;index<module->method_count;index++)
            if(!module->methods[index].included&&
               strcmp(module->methods[index].name,method_name)==0) {
                fail(compiler,name,"attribute method is already defined");return;
            }
        bool present=false;
        for(size_t index=0;index<module->field_count;index++)
            if(strcmp(module->fields[index],field_name)==0)present=true;
        if(!present) {
            if(!diamond_module_reserve_fields(module,module->field_count+1)) {
                fail(compiler,name,"too many module instance variables");return;
            }
            (void)snprintf(module->fields[module->field_count++],
                DIAMOND_MAX_FUNCTION_NAME,"%s",field_name);
        }
        if(!diamond_reserve_methods(&module->methods,&module->method_capacity,module->method_count+1)) {
            fail(compiler,name,"too many module methods");return;
        }
        method=&module->methods[module->method_count++];method->included=false;
    }
    size_t allocated_function_index=0;
    DiamondFunction *function=
        compiler_add_function(compiler,&allocated_function_index);
    if(function==nullptr) {
        fail(compiler,name,"out of memory");
        return;
    }
    const uint16_t function_index=(uint16_t)allocated_function_index;
    (void)snprintf(function->name,sizeof function->name,"%s",method_name);
    function->owner_class=compiler->current_class>=0?
        (uint16_t)compiler->current_class:DIAMOND_OWNER_MODULE;
    function->arity=writer?2:1;function->required_arity=function->arity;
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    function->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
    if(type_set>=0) {
        function->type_set_count=compiler->function->type_set_count;
        if(!diamond_function_reserve_type_sets(function,function->type_set_count)) {
            fail(compiler,name,"out of memory");return;
        }
        memcpy(function->type_sets,compiler->function->type_sets,
               function->type_set_count*sizeof(DiamondTypeSet));
        if(writer)function->parameter_type_sets[0]=(uint16_t)type_set;
        else function->return_type_set=(uint16_t)type_set;
    }
    if(!writer&&type_set<0&&compiler->current_class>=0)
        function->tooling_reader_field=(uint16_t)(field+1u);
    /* Match a hand-written reader's tooling inference without adding a
     * checked return annotation. Discovery has seen writes in every reopen,
     * including generated writers, so source order cannot hide a conflict.
     * Keep a function-wide fallback only for owners without subclasses;
     * instance calls resolve tooling_reader_field against their receiver. */
    if(!writer&&type_set<0&&!compiler->discovery_pass&&
       compiler->current_class>=0) {
        const DiamondClass *class=
            compiler->program->classes[(size_t)compiler->current_class];
        bool has_subclasses=false;
        for(size_t index=0;index<compiler->program->class_count;index++)
            if(compiler->program->classes[index]->superclass==
               (uint16_t)compiler->current_class)has_subclasses=true;
        if(!has_subclasses&&class->discovered_field_type_status[field]==1) {
            DiamondFunction *outer_function=compiler->function;
            compiler->function=function;
            function->inferred_return_type_set=concrete_type_set(compiler,
                (uint16_t)(DIAMOND_TYPE_CLASS_BASE+
                    class->discovered_field_known_class[field]));
            compiler->function=outer_function;
            if(compiler->failed)return;
        }
    }
    /* GET_IVAR/SET_IVAR/GET_IVAR_NAME/SET_IVAR_NAME/CHECK_TYPE/RETURN are
     * all emitted elsewhere via emit_instruction, which widens *every*
     * operand slot to 2 bytes uniformly (see its own comment) -- this
     * hand-rolled synthetic method body bypasses emit_instruction
     * entirely (writing straight into function->code[]), so it has to
     * match that same 2-bytes-per-operand layout by hand. Every literal
     * operand here (a register number or the attribute's own field/type-
     * set index) is always well under 256, so each pair is just an
     * explicit 0 high byte followed by the real value. */
    if(!diamond_function_reserve_code(function,32)) {
        fail(compiler,name,"out of memory compiling attribute");return;
    }
    size_t code=0;
    if(writer&&type_set>=0) {
        function->code[code++]=DIAMOND_OP_CHECK_TYPE;
        function->code[code++]=0;function->code[code++]=1;
        function->code[code++]=(uint8_t)((uint16_t)type_set>>8);
        function->code[code++]=(uint8_t)type_set;
    }
    if(compiler->current_module>=0&&compiler->current_class<0) {
        function->uses_instance_state=true;
        if(!diamond_function_reserve_strings(function,1)) {
            fail(compiler,name,"out of memory compiling module attribute");return;
        }
        DiamondStringConstant *string=&function->strings[0];
        (void)snprintf(string->chars,sizeof string->chars,"%s",field_name);
        string->length=strlen(field_name);function->string_count=1;
        function->code[code++]=(uint8_t)(writer?DIAMOND_OP_SET_IVAR_NAME:
                                          DIAMOND_OP_GET_IVAR_NAME);
        function->code[code++]=0;function->code[code++]=writer?0:1;
        function->code[code++]=0;function->code[code++]=0;
        function->code[code++]=0;function->code[code++]=writer?1:0;
    } else {
        function->code[code++]=(uint8_t)(writer?DIAMOND_OP_SET_IVAR:
                                          DIAMOND_OP_GET_IVAR);
        function->code[code++]=0;function->code[code++]=writer?0:1;
        function->code[code++]=0;function->code[code++]=writer?field:0;
        function->code[code++]=0;function->code[code++]=writer?1:field;
        /* See record_field_known_type's own comment: this SET_IVAR (an
         * attr_accessor/attr_writer-generated writer, storing its own
         * caller-supplied argument straight into the field, untouched by
         * the value-producing expression compile_assignment_store's own
         * call site inspects) needs to feed the same fact, or a class whose
         * only assignment to a field is through a generated writer would
         * wrongly look "never assigned" (status 0) instead of either a real
         * concrete class or "unknown" (status 2). */
        if(writer)record_field_known_type(
            compiler->program->classes[(size_t)compiler->current_class],
            (size_t)field,type_set_single_class(compiler,type_set));
    }
    if(!writer&&type_set>=0) {
        function->code[code++]=DIAMOND_OP_CHECK_TYPE;
        function->code[code++]=0;function->code[code++]=1;
        function->code[code++]=(uint8_t)((uint16_t)type_set>>8);
        function->code[code++]=(uint8_t)type_set;
    }
    function->code[code++]=DIAMOND_OP_RETURN;
    function->code[code++]=0;function->code[code++]=1;
    function->code_count=code;
    /* The body above only ever touches register 0 (self) and register 1
     * (the argument, or the value read). Left at 0 -- "unset" -- run_chunk
     * zeroes a full DIAMOND_REGISTER_COUNT-wide (4096 x 16 = 64 KB) register
     * file for every call, which made each attr_reader/attr_writer/struct
     * field access about four times slower than a hand-written reader. */
    function->register_count=2;
    (void)snprintf(method->name,sizeof method->name,"%s",method_name);
    method->function_index=function_index;method->arity=writer?1:0;
    method->required_arity=method->arity;method->is_private=compiler->methods_private;
    method->is_protected=compiler->methods_protected;
}

static void compile_attribute(Compiler *compiler,bool reader,bool writer,
                              bool predicate) {
    advance_token(compiler);
    const bool parenthesized=compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN;
    if(parenthesized) {advance_token(compiler);skip_newlines(compiler);}
    while(!compiler->failed) {
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,
                compiler->current.kind==DIAMOND_TOKEN_SYMBOL?
                "expected attribute name (write `attr_reader name: Type`, not `attr_reader :name`)":
                "expected attribute name");return;
        }
        const DiamondSpan name=compiler->current.span;advance_token(compiler);
        int type_set=-1;
        if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
            advance_token(compiler);type_set=parse_type_annotation(compiler);
        }
        if(reader)compile_attribute_named(compiler,false,predicate,name,type_set);
        if(writer&&!compiler->failed)
            compile_attribute_named(compiler,true,false,name,type_set);
        if(compiler->failed)return;
        /* Only skip newlines in the parenthesized form -- a bare,
         * unparenthesized list (`attr_accessor a, b`) relies on a bare
         * trailing newline to end the statement; skipping it here would
         * silently absorb the next line's tokens as more attributes. */
        if(parenthesized)skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        if(parenthesized)skip_newlines(compiler);
    }
    if(parenthesized) {
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,
                 "expected ')' after attribute names");return;
        }
        advance_token(compiler);
    }
}

static void compile_visibility(Compiler *compiler,bool is_private,bool is_protected) {
    advance_token(compiler);
    const bool parenthesized=compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN;
    if(parenthesized) {advance_token(compiler);skip_newlines(compiler);}
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        if(parenthesized) {
            fail(compiler,compiler->current.span,
                 "expected method name in visibility list");return;
        }
        compiler->methods_private=is_private;
        compiler->methods_protected=is_protected;return;
    }
    while(!compiler->failed) {
        const DiamondSpan name=compiler->current.span;
        DiamondLexer name_lookahead=compiler->lexer;
        const bool writer_name=
            diamond_lexer_next(&name_lookahead).kind==DIAMOND_TOKEN_EQUAL;
        DiamondMethod *found=nullptr;
        if(compiler->current_class>=0) {
            DiamondClass *class=
                compiler->program->classes[(size_t)compiler->current_class];
            for(size_t index=class->method_count;index>0;index--)
                if((!writer_name&&name_equals(compiler,
                       class->methods[index-1].name,name,false))||
                   (writer_name&&strlen(class->methods[index-1].name)==name.length+1&&
                    class->methods[index-1].name[name.length]=='='&&
                    memcmp(class->methods[index-1].name,
                           compiler->source+name.start,name.length)==0)) {
                    found=&class->methods[index-1];break;
                }
        } else {
            DiamondModule *module=
                compiler->program->modules[(size_t)compiler->current_module];
            for(size_t index=module->method_count;index>0;index--)
                if((!writer_name&&name_equals(compiler,
                       module->methods[index-1].name,name,false))||
                   (writer_name&&strlen(module->methods[index-1].name)==name.length+1&&
                    module->methods[index-1].name[name.length]=='='&&
                    memcmp(module->methods[index-1].name,
                           compiler->source+name.start,name.length)==0)) {
                    found=&module->methods[index-1];break;
                }
        }
        if(found==nullptr) {
            fail(compiler,name,"visibility target is not defined here");return;
        }
        found->is_private=is_private;found->is_protected=is_protected;
        advance_token(compiler);
        if(writer_name&&compiler->current.kind==DIAMOND_TOKEN_EQUAL)
            advance_token(compiler);
        /* Only skip newlines in the parenthesized form -- see the same
         * note in compile_attribute. */
        if(parenthesized)skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        if(parenthesized)skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected method after ','");return;
        }
    }
    if(parenthesized) {
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,
                 "expected ')' after visibility targets");return;
        }
        advance_token(compiler);
    }
}

static void compile_module_function(Compiler *compiler) {
    DiamondModule *module=
        compiler->program->modules[(size_t)compiler->current_module];
    advance_token(compiler);
    const bool parenthesized=compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN;
    if(parenthesized) {advance_token(compiler);skip_newlines(compiler);}
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        if(parenthesized) {
            fail(compiler,compiler->current.span,
                 "expected method in module_function list");return;
        }
        compiler->module_function_mode=true;return;
    }
    while(!compiler->failed) {
        const DiamondSpan name=compiler->current.span;
        DiamondLexer name_lookahead=compiler->lexer;
        const bool writer_name=
            diamond_lexer_next(&name_lookahead).kind==DIAMOND_TOKEN_EQUAL;
        DiamondMethod *source=nullptr;
        for(size_t index=module->method_count;index>0;index--)
            if((!writer_name&&name_equals(compiler,
                   module->methods[index-1].name,name,false))||
               (writer_name&&strlen(module->methods[index-1].name)==name.length+1&&
                module->methods[index-1].name[name.length]=='='&&
                memcmp(module->methods[index-1].name,
                       compiler->source+name.start,name.length)==0)) {
                source=&module->methods[index-1];break;
            }
        if(source==nullptr) {
            fail(compiler,name,"module_function target is not defined here");return;
        }
        const DiamondFunction *function=
            compiler->program->functions[source->function_index];
        if(function->uses_instance_state) {
            fail(compiler,name,
                 "stateful module method cannot become a module_function");
            return;
        }
        /* Same positional claiming as compile_definition's own
         * module_function early-registration and compile_module's own
         * `def self.x` handling (see DiamondModule's next_singleton_
         * claim field for the full rationale): a discovery pass that
         * reached this `module_function name` line at all already ran
         * the duplicate check below and would have failed compilation
         * on a genuine repeat, so the real pass re-visiting an as-yet-
         * unclaimed carried-over entry can safely skip straight to
         * refreshing it in place instead of reporting it as a spurious
         * duplicate of itself. */
        const bool claiming=!compiler->discovery_pass&&
            module->next_singleton_claim<module->singleton_method_count;
        if(!claiming) {
            for(size_t index=0;index<module->singleton_method_count;index++)
                if(strcmp(module->singleton_methods[index].name,source->name)==0) {
                    fail(compiler,name,"module singleton function is already defined");
                    return;
                }
            if(!diamond_reserve_methods(&module->singleton_methods,&module->singleton_method_capacity,module->singleton_method_count+1)) {
                fail(compiler,name,"too many module singleton functions");return;
            }
        }
        source->is_private=true;source->is_protected=false;
        DiamondMethod exported=*source;exported.needs_receiver=true;
        exported.is_private=false;exported.is_protected=false;
        if(claiming)
            module->singleton_methods[module->next_singleton_claim++]=exported;
        else
            module->singleton_methods[module->singleton_method_count++]=exported;
        advance_token(compiler);
        if(writer_name&&compiler->current.kind==DIAMOND_TOKEN_EQUAL)
            advance_token(compiler);
        /* Only skip newlines in the parenthesized form -- see the same
         * note in compile_attribute. */
        if(parenthesized)skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        if(parenthesized)skip_newlines(compiler);
    }
    if(parenthesized) {
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,
                 "expected ')' after module_function targets");return;
        }
        advance_token(compiler);
    }
}

static void compile_alias_method(Compiler *compiler) {
    advance_token(compiler);
    const bool parenthesized=compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN;
    if(parenthesized) {advance_token(compiler);skip_newlines(compiler);}
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected new alias name");return;
    }
    const DiamondSpan alias=compiler->current.span;advance_token(compiler);
    const bool alias_writer=compiler->current.kind==DIAMOND_TOKEN_EQUAL;
    if(alias_writer)advance_token(compiler);
    if(alias.length+(alias_writer?1u:0u)>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,alias,"alias name is too long");return;
    }
    /* Only skip newlines in the parenthesized form -- see the same note
     * in compile_attribute. */
    if(parenthesized)skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' in alias_method");return;
    }
    advance_token(compiler);
    if(parenthesized)skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected existing method name");return;
    }
    const DiamondSpan original=compiler->current.span;advance_token(compiler);
    const bool original_writer=compiler->current.kind==DIAMOND_TOKEN_EQUAL;
    if(original_writer)advance_token(compiler);
    DiamondMethod **table=nullptr;size_t *count=nullptr,*capacity=nullptr;
    if(compiler->current_class>=0) {
        DiamondClass *class=
            compiler->program->classes[(size_t)compiler->current_class];
        table=&class->methods;count=&class->method_count;
        capacity=&class->method_capacity;
    } else {
        DiamondModule *module=
            compiler->program->modules[(size_t)compiler->current_module];
        table=&module->methods;count=&module->method_count;
        capacity=&module->method_capacity;
    }
    DiamondMethod *methods=*table;
    DiamondMethod *source=nullptr;
    for(size_t index=*count;index>0;index--)
        if((!original_writer&&name_equals(compiler,methods[index-1].name,
                                         original,false))||
           (original_writer&&strlen(methods[index-1].name)==original.length+1&&
            methods[index-1].name[original.length]=='='&&
            memcmp(methods[index-1].name,compiler->source+original.start,
                   original.length)==0)) {
            source=&methods[index-1];break;
        }
    if(source==nullptr) {
        fail(compiler,original,"alias source is not defined here");return;
    }
    for(size_t index=0;index<*count;index++)
        if((!alias_writer&&name_equals(compiler,methods[index].name,alias,false))||
           (alias_writer&&strlen(methods[index].name)==alias.length+1&&
            methods[index].name[alias.length]=='='&&
            memcmp(methods[index].name,compiler->source+alias.start,
                   alias.length)==0)) {
            fail(compiler,alias,"alias name is already defined");return;
        }
    DiamondMethod copied=*source;
    if(!diamond_reserve_methods(table,capacity,*count+1)) {
        fail(compiler,alias,"too many methods");return;
    }
    methods=*table;
    for(size_t index=0;index<alias.length;index++)
        copied.name[index]=compiler->source[alias.start+index];
    if(alias_writer)copied.name[alias.length]='=';
    copied.name[alias.length+(alias_writer?1u:0u)]='\0';copied.included=false;
    methods[(*count)++]=copied;
    if(parenthesized) {
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,
                 "expected ')' after alias_method names");return;
        }
        advance_token(compiler);
    }
}

/* Parses `name(params...), to: @ivar` after the `delegate` keyword. */
static bool parse_delegate_signature(Compiler *compiler, DiamondSpan *method_name_out,
        DiamondSpan *parameter_names, size_t *parameter_count_out, bool *variadic_out,
        bool *forwards_block_out, DiamondSpan *target_out) {
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected method name after 'delegate'");
        return false;
    }
    const DiamondSpan method_name=compiler->current.span;
    if(method_name.length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,method_name,"delegated method name is too long");return false;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             "expected '(' after delegated method name");return false;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    size_t parameter_count=0;
    bool variadic=false;
    bool forwards_block=false;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        bool block_parameter=false;
        if(compiler->current.kind==DIAMOND_TOKEN_AMPERSAND) {
            block_parameter=true;forwards_block=true;advance_token(compiler);
        }
        if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
            if(forwards_block||variadic) {
                fail(compiler,compiler->current.span,
                    "a delegate can only declare one variadic parameter");return false;
            }
            variadic=true;advance_token(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected parameter name");return false;
        }
        if(parameter_count==DIAMOND_MAX_DECLARED_PARAMETERS) {
            fail(compiler,compiler->current.span,"too many parameters");return false;
        }
        parameter_names[parameter_count++]=compiler->current.span;
        advance_token(compiler);
        skip_newlines(compiler);
        if(block_parameter&&compiler->current.kind==DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,
                "a block delegate parameter must be last");return false;
        }
        if(variadic&&!block_parameter&&
           compiler->current.kind==DIAMOND_TOKEN_COMMA) {
            advance_token(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_AMPERSAND) {
                fail(compiler,compiler->current.span,
                    "only a block parameter may follow a variadic delegate parameter");
                return false;
            }
            continue;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
             "expected ')' after delegate parameters");return false;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,
             "expected ', to: @ivar' after delegate parameters");return false;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"to",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'to:' in delegate");return false;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
        fail(compiler,compiler->current.span,"expected ':' after 'to'");return false;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        fail(compiler,compiler->current.span,
             "delegate target must be an instance variable");return false;
    }
    const DiamondSpan target=compiler->current.span;
    advance_token(compiler);
    *method_name_out=method_name;
    *parameter_count_out=parameter_count;
    *variadic_out=variadic;
    *forwards_block_out=forwards_block;
    *target_out=target;
    return true;
}

/* Emits the delegate's body: load the target ivar, forward every parameter
 * (spreading a variadic one, passing the block only when one was given), and
 * return the result. */
static void emit_delegate_forward(Compiler *compiler, bool in_class, DiamondSpan target,
        DiamondSpan method_name, const uint16_t *parameter_registers,
        size_t parameter_count, bool variadic, bool forwards_block) {
    uint16_t ivar_register;
    if(in_class) {
        const int field=field_index(compiler,target,true);
        ivar_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_IVAR,ivar_register,0,
                          (uint8_t)field,3);
    } else {
        const uint16_t field=module_field_name(compiler,target);
        ivar_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,ivar_register,0,
                          field,3);
    }
    uint16_t result;
    if(variadic) {
        const size_t variadic_index=parameter_count-(forwards_block?2u:1u);
        if(forwards_block) {
            const uint16_t absent=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NIL,absent,0,0,1);
            const uint16_t missing=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_EQUAL,missing,
                parameter_registers[variadic_index+1],absent,3);
            const size_t with_block=
                emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,missing);
            const uint16_t without_args=emit_build_spread_arguments(compiler,
                parameter_registers,variadic_index,
                parameter_registers[variadic_index],nullptr,0,false);
            const uint16_t without=emit_invoke_spread(compiler,ivar_register,
                method_name,without_args);
            result=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,result,without,0,2);
            const size_t finished=emit_jump(compiler,DIAMOND_OP_JUMP,0);
            patch_jump(compiler,with_block,compiler->function->code_count);
            const uint16_t with_args=emit_build_spread_arguments(compiler,
                parameter_registers,variadic_index,
                parameter_registers[variadic_index],
                &parameter_registers[variadic_index+1],1,false);
            const uint16_t with=emit_invoke_spread(compiler,ivar_register,
                method_name,with_args);
            emit_instruction(compiler,DIAMOND_OP_MOVE,result,with,0,2);
            patch_jump(compiler,finished,compiler->function->code_count);
        } else {
            const uint16_t forwarded=emit_build_spread_arguments(compiler,
                parameter_registers,variadic_index,
                parameter_registers[variadic_index],nullptr,0,false);
            result=emit_invoke_spread(compiler,ivar_register,method_name,forwarded);
        }
    } else if(forwards_block) {
        const uint16_t absent=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,absent,0,0,1);
        const uint16_t missing=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_EQUAL,missing,
            parameter_registers[parameter_count-1],absent,3);
        const size_t with_block=emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,missing);
        const uint16_t without=emit_invoke_call(compiler,ivar_register,method_name,
            false,nullptr,0,parameter_registers,parameter_count-1);
        result=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,result,without,0,2);
        const size_t finished=emit_jump(compiler,DIAMOND_OP_JUMP,0);
        patch_jump(compiler,with_block,compiler->function->code_count);
        const uint16_t with=emit_invoke_call(compiler,ivar_register,method_name,
            false,nullptr,0,parameter_registers,parameter_count);
        emit_instruction(compiler,DIAMOND_OP_MOVE,result,with,0,2);
        patch_jump(compiler,finished,compiler->function->code_count);
    } else result=emit_invoke_call(compiler,ivar_register,method_name,
        false,nullptr,0,parameter_registers,parameter_count);
    emit_instruction(compiler,DIAMOND_OP_RETURN,result,0,0,1);
}

/* `delegate name(params...), to: @ivar` -- compiles into an ordinary
 * forwarding method, as if the source had literally been
 * `def name(params...) @ivar.name(params...) end`: same method metadata,
 * visibility, inheritance, interface checks, dispatch caches, arity
 * validation, and respond_to? behavior as any other method, not a
 * parallel runtime dispatch mechanism. Deliberately scoped: the target must be a bare
 * instance variable (no arbitrary expression, no `to: some_method()`);
 * parameters are bare names only (no type annotations or defaults). A final
 * `&block` parameter forwards the trailing block closure through the ordinary
 * last-Callable-argument convention, and composes with a splat parameter
 * (`delegate emit(prefix, *values, &block), to: @target`); the forwarded
 * call always uses the same name declared here (no renaming). Valid in
 * both class and module bodies,
 * mirroring compile_attribute_named's own class/module split just above
 * (field-index GET_IVAR for a class, name-keyed GET_IVAR_NAME for a
 * module, module_field_name already handling that field's registration
 * and marking uses_instance_state) -- but unlike that function's fixed
 * one-opcode body, this one's body is a full dynamic-dispatch call, so
 * it's compiled through the ordinary allocate_register/emit_instruction/
 * emit_opcode machinery instead of hand-written bytes, temporarily
 * switching compiler->function to a fresh DiamondFunction the same way
 * compile_definition does for every ordinary `def` -- mirroring the
 * reduced subset of its own outer-state save/restore that actually
 * applies here (this is always a direct class/module member, never
 * nested inside another function's live compile, so the closure-capture
 * bookkeeping compile_definition also carries is simply never needed). */
void compile_delegate(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    DiamondSpan method_name={};
    DiamondSpan parameter_names[DIAMOND_MAX_DECLARED_PARAMETERS];size_t parameter_count=0;
    bool variadic=false;
    bool forwards_block=false;
    DiamondSpan target={};
    if(!parse_delegate_signature(compiler,&method_name,parameter_names,&parameter_count,
           &variadic,&forwards_block,&target))
        return;

    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    for(size_t index=0;index<method_name.length;index++)
        stored_name[index]=compiler->source[method_name.start+index];
    stored_name[method_name.length]='\0';

    const bool in_class=compiler->current_class>=0;
    if(!in_class&&compiler->current_module<0) {
        fail(compiler,keyword,"delegate is only valid inside a class or module");
        return;
    }
    if(!in_class&&compiler->module_function_mode) {
        fail(compiler,keyword,
             "stateful delegate cannot use module_function mode");return;
    }
    DiamondMethod **methods=nullptr;size_t *method_count=nullptr,*method_capacity=nullptr;
    if(in_class) {
        DiamondClass *class=
            compiler->program->classes[(size_t)compiler->current_class];
        methods=&class->methods;method_count=&class->method_count;
        method_capacity=&class->method_capacity;
    } else {
        DiamondModule *module=
            compiler->program->modules[(size_t)compiler->current_module];
        methods=&module->methods;method_count=&module->method_count;
        method_capacity=&module->method_capacity;
    }
    for(size_t index=0;index<*method_count;index++)
        if(!(*methods)[index].included&&strcmp((*methods)[index].name,stored_name)==0) {
            fail(compiler,method_name,"delegated method name is already defined");
            return;
        }
    if(!diamond_reserve_methods(methods,method_capacity,*method_count+1)) {
        fail(compiler,method_name,"too many methods");return;
    }

    /* Same outer-state save shape as compile_definition's own (see its
     * own comments for why each field matters and the inline-then-heap
     * known_types/known_type_sets fallback), minus enclosing_locals/
     * capture_registers -- always empty here, see this function's own
     * top comment. */
    DiamondFunction *outer_function=compiler->function;
    Local outer_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_local_count=compiler->local_count;
    for(size_t index=0;index<outer_local_count;index++)
        outer_locals[index]=compiler->locals[index];
    const uint16_t outer_next_register=compiler->next_register;
    const DiamondSpan outer_method=compiler->current_method;
    const bool outer_in_method=compiler->in_method;
    const bool outer_in_singleton_method=compiler->in_singleton_method;
    const bool outer_in_function=compiler->in_function;
    const bool outer_in_block=compiler->in_block;
    const size_t outer_begin_depth=compiler->begin_depth;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
    LoopContext *outer_loop=compiler->current_loop;
    uint16_t inline_outer_known_types[256];int32_t inline_outer_known_type_sets[256];
    uint16_t *outer_known_types=inline_outer_known_types;
    int32_t *outer_known_type_sets=inline_outer_known_type_sets;
    uint16_t *heap_outer_known_types=nullptr;int32_t *heap_outer_known_type_sets=nullptr;
    if(outer_next_register>256) {
        heap_outer_known_types=malloc((size_t)outer_next_register*sizeof(uint16_t));
        heap_outer_known_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        if(heap_outer_known_types==nullptr||heap_outer_known_type_sets==nullptr) {
            fail(compiler,keyword,"out of memory compiling delegate");
            free(heap_outer_known_types);free(heap_outer_known_type_sets);
            return;
        }
        outer_known_types=heap_outer_known_types;
        outer_known_type_sets=heap_outer_known_type_sets;
    }
    for(size_t index=0;index<outer_next_register;index++)
        {outer_known_types[index]=compiler->known_types[index];
         outer_known_type_sets[index]=compiler->known_type_sets[index];}

    size_t allocated_function_index=0;
    DiamondFunction *function=
        compiler_add_function(compiler,&allocated_function_index);
    if(function==nullptr) {
        fail(compiler,keyword,"out of memory");
        free(heap_outer_known_types);free(heap_outer_known_type_sets);
        return;
    }
    const uint16_t function_index=(uint16_t)allocated_function_index;
    (void)snprintf(function->name,sizeof function->name,"%s",stored_name);
    function->owner_class=in_class?(uint16_t)compiler->current_class:DIAMOND_OWNER_MODULE;
    function->declaration_line=(uint32_t)keyword.line;
    function->declaration_column=(uint32_t)keyword.column;
    function->declaration_start=keyword.start;
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    function->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;

    compiler->function=function;
    compiler->begin_depth=0;
    compiler->local_count=0;
    compiler->next_register=0;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->current_method=method_name;
    compiler->in_method=true;
    compiler->in_singleton_method=false;
    compiler->in_function=false;
    compiler->in_block=false;
    compiler->current_return_type=-1;
    compiler->current_return_type_span=(DiamondSpan){};
    compiler->current_exception=-1;
    compiler->current_retry_target=SIZE_MAX;
    compiler->current_loop=nullptr;

    (void)allocate_register(compiler); /* self */
    function->arity=1;function->required_arity=1;
    uint16_t parameter_registers[DIAMOND_MAX_DECLARED_PARAMETERS];
    for(size_t index=0;index<parameter_count;index++) {
        parameter_registers[index]=allocate_register(compiler);
        compiler->locals[compiler->local_count++]=(Local){
            .name=parameter_names[index],.reg=parameter_registers[index]};
        record_scope_type_fact(compiler,parameter_registers[index],
                               parameter_names[index].start);
        size_t name_length=parameter_names[index].length;
        if(name_length>=DIAMOND_MAX_FUNCTION_NAME)
            name_length=DIAMOND_MAX_FUNCTION_NAME-1;
        for(size_t char_index=0;char_index<name_length;char_index++)
            function->parameter_names[index][char_index]=
                compiler->source[parameter_names[index].start+char_index];
        function->parameter_names[index][name_length]='\0';
    }
    function->arity=(uint8_t)(function->arity+parameter_count);
    function->required_arity=variadic?
        (uint8_t)(parameter_count-(forwards_block?1u:0u)):
        (uint8_t)(function->arity-(forwards_block?1u:0u));
    function->has_variadic=variadic;
    function->has_block_parameter=forwards_block;
    if(variadic) {
        const size_t variadic_index=parameter_count-(forwards_block?2u:1u);
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            parameter_registers[variadic_index],(uint16_t)(variadic_index+1),
            forwards_block?1:0,3);
    }

    emit_delegate_forward(compiler,in_class,target,method_name,parameter_registers,
        parameter_count,variadic,forwards_block);

    function->capture_count=0;
    function->register_count=compiler->next_register;
    if(!compiler->failed) {
        const size_t body_end=target.start+target.length;
        record_scope_locals(compiler,0,compiler->local_count,body_end);
        function->body_end=body_end;
    }

    compiler->function=outer_function;
    compiler->begin_depth=outer_begin_depth;
    compiler->local_count=outer_local_count;
    for(size_t index=0;index<outer_local_count;index++)
        compiler->locals[index]=outer_locals[index];
    compiler->next_register=outer_next_register;
    compiler->last_instruction_offset=SIZE_MAX;
    compiler->current_method=outer_method;
    compiler->in_method=outer_in_method;
    compiler->in_singleton_method=outer_in_singleton_method;
    compiler->in_function=outer_in_function;
    compiler->in_block=outer_in_block;
    compiler->current_return_type=outer_return_type;
    compiler->current_return_type_span=outer_return_type_span;
    compiler->current_exception=outer_exception;
    compiler->current_retry_target=outer_retry_target;
    compiler->current_loop=outer_loop;
    for(size_t index=0;index<outer_next_register;index++)
        {compiler->known_types[index]=outer_known_types[index];
         compiler->known_type_sets[index]=outer_known_type_sets[index];}
    free(heap_outer_known_types);free(heap_outer_known_type_sets);

    if(compiler->failed)return;
    if(!diamond_reserve_methods(methods,method_capacity,*method_count+1)) {
        fail(compiler,method_name,"too many methods");return;
    }
    DiamondMethod *method=&(*methods)[(*method_count)++];
    (void)snprintf(method->name,sizeof method->name,"%s",stored_name);
    method->function_index=function_index;
    method->arity=(uint8_t)parameter_count;
    method->required_arity=variadic?
        (uint8_t)(parameter_count-(forwards_block?2u:1u)):
        (uint8_t)(parameter_count-(forwards_block?1u:0u));
    method->has_variadic=variadic;
    method->included=false;
    method->is_private=compiler->methods_private;
    method->is_protected=compiler->methods_protected;
}

/* The real pass's field facts become the discovered ones (see
 * DiamondClass.discovered_field_type_status). All five field tables share
 * field_capacity, so the whole allocated length copies across. */
static void copy_field_facts_to_discovered(DiamondClass *class) {
    if(class->field_capacity==0)return;
    memcpy(class->discovered_field_type_status,class->field_type_status,
        class->field_capacity*sizeof class->field_type_status[0]);
    memcpy(class->discovered_field_known_class,class->field_known_class,
        class->field_capacity*sizeof class->field_known_class[0]);
}

/* The body of a `class`/`struct` block, from right after `compiler->
 * current_class`/`methods_private`/`methods_protected` are set up
 * through (not including) the closing `end` -- shared by compile_class
 * and compile_struct (the latter calls this after registering its own
 * generated readers/initialize/==/to_s, so a hand-written method here
 * colliding with one of those already fails via compile_definition's/
 * compile_attribute_named's own existing duplicate-name checks, with no
 * struct-specific collision handling needed). Relies entirely on the
 * caller having already set compiler->current_class/methods_private/
 * methods_protected to the right values -- never reads `index` itself,
 * only `class` (for `include`'s own field/method copy). */
/* Copies class->field_type_status/field_known_class's own final answer (see
 * record_field_known_type's own comment for why it's now exhaustive) onto
 * DiamondFunction.ivar_known_class for every one of this class's own
 * (non-`included`) methods -- see that field's own comment in src/vm.h for
 * why the JIT needs its own snapshot rather than reading the class table
 * directly. Called once at the end of compile_class_body, so it runs after
 * every SET_IVAR-emitting construct for this class body has already updated
 * field_type_status (attr_accessor/struct field declarations happen before
 * compile_class_body's own loop for a struct, and inside it for an ordinary
 * class's attr_accessor -- either way, strictly before this point). A
 * reopened class (`class Foo ... end` appearing again later in the same
 * compile pass) calls compile_class_body, and therefore this, again --
 * naturally re-snapshotting *every* one of the class's methods (from every
 * reopening, not just the newest one) with the latest, most complete
 * field_type_status/field_known_class each time, so the state after the
 * whole program finishes compiling is always the final, fully-merged
 * answer regardless of how many times the class was reopened along the way
 * -- an intermediate reopening's own snapshot being less complete is
 * harmless, since a later reopening's own call here overwrites it. Skips
 * an `included` method deliberately: its function belongs to (and may be
 * shared by) the *module* that defined it, not this class -- see
 * compile_attribute_named's own module branch, which never emits index-
 * based SET_IVAR/GET_IVAR at all (always the name-based _NAME opcodes), so
 * a shared module method could never use this fact anyway. */
static void snapshot_ivar_known_classes(Compiler *compiler, DiamondClass *class) {
    const uint16_t class_index = (uint16_t)compiler->current_class;
    for (size_t m = 0; m < class->method_count; m++) {
        if (class->methods[m].included) continue;
        DiamondFunction *function =
            compiler->program->functions[class->methods[m].function_index];
        if (function->owner_class != class_index) continue;
        for (size_t field = 0; field < DIAMOND_MAX_JIT_KNOWN_FIELDS; field++)
            function->ivar_known_class[field] =
                (field < class->field_count && class->field_type_status[field] == 1)
                    ? class->field_known_class[field] : DIAMOND_NO_CLASS;
    }
}

/* Whether `methods` already holds a method the class/module itself defines
 * (or aliases) under `name`, as opposed to one copied in by `include`.
 * Method lookup takes the LAST matching entry, so an include that appended a
 * same-named module method after an own method would silently shadow it;
 * `include` skips those instead, so a class's own methods always beat its
 * included modules' regardless of source order -- as in Ruby. A struct's
 * still-replaceable generated == / to_s do not count as "own": they are
 * defaults, and an included Comparable's derived == has always overridden
 * the generated one (as it does for a Ruby Struct). */
static bool has_own_method_named(const Compiler *compiler,int owner,
        const DiamondMethod *methods,size_t count,const char *name) {
    if(owner==compiler->struct_replaceable_class&&
       ((compiler->struct_replaceable_eq&&strcmp(name,"==")==0)||
        (compiler->struct_replaceable_to_s&&strcmp(name,"to_s")==0)))
        return false;
    for(size_t index=0;index<count;index++)
        if(!methods[index].included&&strcmp(methods[index].name,name)==0)
            return true;
    return false;
}
/* `private`/`protected`/`public` in a class or module body. */
static bool is_visibility_token(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_PRIVATE||kind==DIAMOND_TOKEN_PROTECTED||
        kind==DIAMOND_TOKEN_PUBLIC;
}

static void compile_visibility_token(Compiler *compiler) {
    const bool private_visibility=
        compiler->current.kind==DIAMOND_TOKEN_PRIVATE;
    const bool protected_visibility=
        compiler->current.kind==DIAMOND_TOKEN_PROTECTED;
    compile_visibility(compiler,private_visibility,protected_visibility);
}

/* attr / attr_reader / attr_writer / attr_accessor / attr_predicate. */
static bool is_attribute_token(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_ATTR||kind==DIAMOND_TOKEN_ATTR_READER||
        kind==DIAMOND_TOKEN_ATTR_WRITER||kind==DIAMOND_TOKEN_ATTR_ACCESSOR||
        kind==DIAMOND_TOKEN_ATTR_PREDICATE;
}

static void compile_attribute_declaration(Compiler *compiler) {
    const bool reader=compiler->current.kind!=DIAMOND_TOKEN_ATTR_WRITER;
    const bool writer=compiler->current.kind!=DIAMOND_TOKEN_ATTR_READER;
    const bool shorthand=compiler->current.kind==DIAMOND_TOKEN_ATTR;
    const bool predicate=
        compiler->current.kind==DIAMOND_TOKEN_ATTR_PREDICATE;
    compile_attribute(compiler,reader,
                      shorthand||predicate?false:writer,predicate);
}


static void compile_class_body(Compiler *compiler, DiamondClass *class) {
    prescan_singleton_signatures(compiler,&class->singleton_methods,
        &class->singleton_method_count,&class->singleton_method_capacity,true);
    while(!compiler->failed && compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(is_visibility_token(compiler->current.kind)) {
            compile_visibility_token(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            fail(compiler,compiler->current.span,
                 "module_function is only valid in modules");break;
        } else if(compiler->current.kind==DIAMOND_TOKEN_ALIAS_METHOD) {
            compile_alias_method(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_DELEGATE) {
            compile_delegate(compiler);
        } else if(is_attribute_token(compiler->current.kind)) {
            compile_attribute_declaration(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
                  assignment_ahead(compiler)) {
            const char first=compiler->source[compiler->current.span.start];
            if(first<'A'||first>'Z') {
                fail(compiler,compiler->current.span,
                     "class constants must begin with an uppercase letter");break;
            }
            (void)compile_assignment(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_INCLUDE) {
            advance_token(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,
                     "expected module name after 'include'");break;
            }
            const DiamondSpan include_span=compiler->current.span;
            const int lexical_module=find_module(compiler,include_span);
            char include_name[DIAMOND_MAX_FUNCTION_NAME];
            if(!consume_qualified_name(compiler,include_name,
                                       sizeof include_name)) {
                fail(compiler,include_span,"invalid qualified module name");break;
            }
            const int module_index=strstr(include_name,"::")==nullptr?
                lexical_module:find_module_name(compiler,include_name);
            if(module_index<0) {
                fail(compiler,include_span,"undefined module");break;
            }
            const DiamondModule *module=
                compiler->program->modules[(size_t)module_index];
            if(!record_included_module(class->included_modules,
                    &class->included_module_count,(uint16_t)module_index)) {
                fail(compiler,include_span,"too many included modules");break;
            }
            for(size_t source=0;source<module->field_count;source++) {
                bool present=false;
                for(size_t field=0;field<class->field_count;field++)
                    if(strcmp(class->fields[field],module->fields[source])==0)
                        present=true;
                if(present)continue;
                if(!diamond_class_reserve_fields(class,class->field_count+1)) {
                    fail(compiler,include_span,
                         "included module adds too many fields");break;
                }
                (void)snprintf(class->fields[class->field_count++],
                    DIAMOND_MAX_FUNCTION_NAME,"%s",module->fields[source]);
            }
            if(compiler->failed)break;
            if(!diamond_reserve_methods(&class->methods,&class->method_capacity,
                    class->method_count+module->method_count)) {
                fail(compiler,compiler->current.span,
                     "included module adds too many methods");break;
            }
            for(size_t method=0;method<module->method_count;method++) {
                if(has_own_method_named(compiler,compiler->current_class,
                        class->methods,class->method_count,
                        module->methods[method].name))continue;
                class->methods[class->method_count]=module->methods[method];
                class->methods[class->method_count++].included=true;
            }
        } else if(compiler->current.kind==DIAMOND_TOKEN_DEF) {
            (void)compile_definition(compiler,false);
        } else {
            fail(compiler,compiler->current.span,
                 "expected method definition or include in class");break;
        }
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE) skip_newlines(compiler);
    }
    if(!compiler->failed)snapshot_ivar_known_classes(compiler,class);
}

uint16_t compile_class(Compiler *compiler) {
    const bool this_declaration_sealed=compiler->current.kind==DIAMOND_TOKEN_SEALED;
    advance_token(compiler);
    if(this_declaration_sealed&&compiler->current.kind!=DIAMOND_TOKEN_CLASS) {
        fail(compiler,compiler->current.span,"expected 'class' after 'sealed'");
        return 0;
    }
    if(this_declaration_sealed)advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler, compiler->current.span, "expected valid class name"); return 0;
    }
    DiamondSpan name=compiler->current.span;
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"class name is too long"); return 0;
    }
    const int existing_class=find_class_name(compiler,stored_name);
    /* A class can always be reopened -- see compile_module's identical
     * comment. Only a cross-kind collision stays a hard error. */
    if(find_interface_name(compiler,stored_name)>=0||
       find_module_name(compiler,stored_name)>=0) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    int index;
    DiamondClass *class;
    /* True once this class's superclass (real or "none") has already
     * been decided by an earlier declaration *within this same compile
     * pass* -- a later reopen's own `< Super` clause (if any) gets
     * validated against that decision instead of overwriting it, so a
     * reopen can't silently change what a class inherits from or stomp
     * fields already copied from its superclass. False for a class's
     * first declaration this pass (whether or not it states `< Super`)
     * -- there's nothing yet to conflict with. */
    bool superclass_decided;
    if(existing_class>=0) {
        index=existing_class;
        class=compiler->program->classes[(size_t)index];
        if(class->declared_by_discovery&&class->from_template) {
            /* A reopened template (prelude) class: back to the template's
             * own methods, fields, and superclass, dropping only what the
             * discovery pass added (see diamond_compile_impl). */
            class->discovered_method_count=0;
            if(!diamond_reserve_discovered_methods(class,class->method_count)) {
                fail(compiler,name,"out of memory declaring a class");return 0;
            }
            for(size_t method=class->template_method_count;method<class->method_count;method++)
                if(!class->methods[method].included)
                    class->discovered_methods[class->discovered_method_count++]=
                        class->methods[method].function_index;
            if(class->method_count>class->template_method_count)
                memset(&class->methods[class->template_method_count],0,
                    (class->method_count-class->template_method_count)*sizeof class->methods[0]);
            if(class->singleton_method_count>class->template_singleton_method_count)
                memset(&class->singleton_methods[class->template_singleton_method_count],0,
                    (class->singleton_method_count-class->template_singleton_method_count)*
                        sizeof class->singleton_methods[0]);
            class->method_count=class->template_method_count;
            class->singleton_method_count=class->template_singleton_method_count;
            class->field_count=class->template_field_count;
            class->class_variable_count=class->template_class_variable_count;
            class->declared_by_discovery=false;
            class->from_template=false;
            superclass_decided=true;
        } else if(class->declared_by_discovery) {
            /* First time *this* compile pass touches a slot the *other*
             * (already-finished) pass populated -- see diamond_compile's
             * own comment on declared_by_discovery. Full zero, not just
             * the counts: several registration sites (interface method
             * arity in compile_interface, confirmed directly as the
             * cause of a real bug here) accumulate straight into a
             * slot's own fields trusting they start at zero, rather than
             * assigning an absolute value -- resetting only the counts
             * left the other pass's stale contents sitting in these
             * arrays for a claimed slot to silently accumulate on top
             * of. */
            copy_field_facts_to_discovered(class);
            class->discovered_method_count=0;
            if(!diamond_reserve_discovered_methods(class,class->method_count)) {
                fail(compiler,name,"out of memory declaring a class");return 0;
            }
            for(size_t method=0;method<class->method_count;method++)
                if(!class->methods[method].included)
                    class->discovered_methods[class->discovered_method_count++]=
                        class->methods[method].function_index;
            zero_table(class->methods,class->method_capacity*sizeof class->methods[0]);
            zero_table(class->singleton_methods,class->singleton_method_capacity*sizeof class->singleton_methods[0]);
            zero_table(class->fields,class->field_capacity*sizeof class->fields[0]);
            zero_table(class->field_type_status,class->field_capacity*sizeof class->field_type_status[0]);
            zero_table(class->field_known_class,class->field_capacity*sizeof class->field_known_class[0]);
            zero_table(class->class_variables,class->class_variable_capacity*sizeof class->class_variables[0]);
            class->method_count=0;
            class->singleton_method_count=0;
            class->field_count=0;
            class->class_variable_count=0;
            class->superclass=DIAMOND_NO_CLASS;
            class->sealed=false;
            class->declared_by_discovery=false;
            superclass_decided=false;
        } else {
            /* Already owned by this pass (freshly created earlier in
             * this same pass, or already reset just above) -- a genuine
             * reopen within the current pass. Merge new content on top
             * without resetting anything; the superclass this class
             * already has (if any) was decided by its first declaration
             * this pass. */
            superclass_decided=true;
        }
    } else {
        if(compiler->program->class_count==DIAMOND_MAX_CLASSES) {
            fail(compiler, name, "too many classes: a program holds at most 1024 "
                 "classes and structs, cuts included"); return 0;
        }
        class=diamond_program_class_slot(compiler->program,compiler->program->class_count);
        if(class==nullptr) {fail(compiler,name,"out of memory");return 0;}
        index=(int)compiler->program->class_count++;
        /* Explicit reset, not reliance on this slot already being zero:
         * see the declared_by_discovery branch's own identical reset
         * just above, and compile_module's own copy of this comment.
         * `program` isn't guaranteed freshly calloc'd/memset -- this
         * exact slot can hold a *different*, unrelated class's own
         * leftover methods/fields/class-variables from an earlier
         * compile of a reused DiamondProgram (tests/run_cases.c's own
         * batch loop). This is what makes diamond_program_init's own
         * memset(program,0,...) safe to skip for `classes[]` -- see
         * that function's own comment (src/compiler.c). */
        zero_table(class->methods,class->method_capacity*sizeof class->methods[0]);
        zero_table(class->singleton_methods,class->singleton_method_capacity*sizeof class->singleton_methods[0]);
        zero_table(class->fields,class->field_capacity*sizeof class->fields[0]);
        zero_table(class->field_type_status,class->field_capacity*sizeof class->field_type_status[0]);
        zero_table(class->field_known_class,class->field_capacity*sizeof class->field_known_class[0]);
        zero_table(class->discovered_field_type_status,class->field_capacity*sizeof class->discovered_field_type_status[0]);
        zero_table(class->discovered_field_known_class,class->field_capacity*sizeof class->discovered_field_known_class[0]);
        class->discovered_method_count=0;
        zero_table(class->class_variables,class->class_variable_capacity*sizeof class->class_variables[0]);
        class->method_count=0;
        class->singleton_method_count=0;
        class->field_count=0;
        class->class_variable_count=0;
        class->included_module_count=0;
        class->declared_by_discovery=false;
        class->superclass=DIAMOND_NO_CLASS;
        class->sealed=false;
        superclass_decided=false;
    }
    /* A one-way ratchet, not a plain assignment: any declaration or
     * reopening of this class that says `sealed` makes it sealed for
     * good, within this compile pass -- a later reopen that omits the
     * keyword doesn't silently unseal it. */
    class->sealed=class->sealed||this_declaration_sealed;
    class->declaration_line=(uint32_t)name.line;
    class->declaration_column=(uint32_t)name.column;
    class->declaration_start=name.start;
    (void)snprintf(class->name,sizeof class->name,"%s",stored_name);
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_LESS) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected superclass name after '<'"); return 0;
        }
        const DiamondSpan superclass_span=compiler->current.span;
        char superclass_name[DIAMOND_MAX_FUNCTION_NAME];
        if(!consume_qualified_name(compiler,superclass_name,sizeof superclass_name)) {
            fail(compiler,superclass_span,"superclass name is too long"); return 0;
        }
        const int parent=find_class_qualified_or_scoped(compiler,superclass_name);
        if(parent<0) { fail(compiler,superclass_span,"undefined superclass"); return 0; }
        if(!superclass_decided) {
            class->superclass=(uint16_t)parent;
            const DiamondClass *parent_class=compiler->program->classes[(size_t)parent];
            if(!diamond_class_reserve_fields(class,parent_class->field_count)) {
                fail(compiler,superclass_span,"out of memory declaring a class");return 0;
            }
            class->field_count=parent_class->field_count;
            for(size_t field=0;field<parent_class->field_count;field++) {
                for(size_t ch=0;ch<DIAMOND_MAX_FUNCTION_NAME;ch++)
                    class->fields[field][ch]=parent_class->fields[field][ch];
                class->field_type_status[field]=parent_class->field_type_status[field];
                class->field_known_class[field]=parent_class->field_known_class[field];
            }
        } else if(class->superclass!=(uint16_t)parent) {
            fail(compiler,superclass_span,
                 "superclass mismatch for reopened class");return 0;
        }
        /* else: reopen restates the same superclass already on record --
         * validated no-op; fields were already copied once, don't stomp
         * whatever this class has accumulated on its own since then. */
    }
    if(!consume_block_start(compiler)) return 0;
    const int outer=compiler->current_class; compiler->current_class=index;
    const bool outer_private=compiler->methods_private;
    const bool outer_protected=compiler->methods_protected;
    compiler->methods_private=false;
    compiler->methods_protected=false;
    compile_class_body(compiler,class);
    compiler->current_class=outer;
    compiler->methods_private=outer_private;
    compiler->methods_protected=outer_protected;
    if(compiler->current.kind==DIAMOND_TOKEN_END) advance_token(compiler);
    const uint16_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

/* Same shape as add_string_range/add_name_string just above, but for a
 * literal C string this compile-time synthesis code built itself (e.g.
 * "Point(x: ") rather than a span into the user's own source -- no
 * escape processing needed, the caller already controls every byte. */
static uint16_t add_literal_string(Compiler *compiler,const char *text,
                                   DiamondSpan span) {
    const size_t length=strlen(text);
    if(compiler->function->string_count==DIAMOND_MAX_STRING_CONSTANTS||
       length>DIAMOND_MAX_STRING_LENGTH) {
        fail(compiler,span,"too many or oversized generated string literals");
        return 0;
    }
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,span,"out of memory growing function strings");return 0;
    }
    DiamondStringConstant *string=
        &compiler->function->strings[compiler->function->string_count];
    memcpy(string->chars,text,length);
    string->length=length;string->chars[length]='\0';
    return (uint16_t)compiler->function->string_count++;
}

static bool begin_struct_method(Compiler *compiler,DiamondFunction *function,
                                StructMethodState *state) {
    state->outer_function=compiler->function;
    state->outer_next_register=compiler->next_register;
    state->heap_known_types=nullptr;state->heap_known_type_sets=nullptr;
    uint16_t *known_types=state->inline_known_types;
    int32_t *known_type_sets=state->inline_known_type_sets;
    if(compiler->next_register>256) {
        state->heap_known_types=
            malloc((size_t)compiler->next_register*sizeof(uint16_t));
        state->heap_known_type_sets=
            malloc((size_t)compiler->next_register*sizeof(int32_t));
        if(state->heap_known_types==nullptr||state->heap_known_type_sets==nullptr) {
            free(state->heap_known_types);free(state->heap_known_type_sets);
            return false;
        }
        known_types=state->heap_known_types;
        known_type_sets=state->heap_known_type_sets;
    }
    for(size_t index=0;index<compiler->next_register;index++) {
        known_types[index]=compiler->known_types[index];
        known_type_sets[index]=compiler->known_type_sets[index];
    }
    compiler->function=function;
    compiler->next_register=0;
    compiler->last_instruction_offset=SIZE_MAX;
    return true;
}

static void end_struct_method(Compiler *compiler,StructMethodState *state) {
    compiler->function->register_count=compiler->next_register;
    compiler->function=state->outer_function;
    compiler->next_register=state->outer_next_register;
    const uint16_t *known_types=state->heap_known_types?
        state->heap_known_types:state->inline_known_types;
    const int32_t *known_type_sets=state->heap_known_type_sets?
        state->heap_known_type_sets:state->inline_known_type_sets;
    for(size_t index=0;index<state->outer_next_register;index++) {
        compiler->known_types[index]=known_types[index];
        compiler->known_type_sets[index]=known_type_sets[index];
    }
    free(state->heap_known_types);free(state->heap_known_type_sets);
}

static void register_struct_method(Compiler *compiler,DiamondClass *class,
        DiamondSpan name,const char *method_name,uint16_t function_index,
        uint8_t caller_arity) {
    if(!diamond_reserve_methods(&class->methods,&class->method_capacity,class->method_count+1)) {
        fail(compiler,name,"too many methods");return;
    }
    DiamondMethod *method=&class->methods[class->method_count++];
    (void)snprintf(method->name,sizeof method->name,"%s",method_name);
    method->function_index=function_index;
    method->arity=caller_arity;method->required_arity=caller_arity;
}

/* Registers the struct's class (or claims the slot discovery reserved for
 * it) and resets it to an empty generated struct. Returns nullptr on
 * failure. */
static DiamondClass *declare_struct_class(Compiler *compiler, DiamondSpan name,
        const char *stored_name, int *index_out) {
    const int existing_class=find_class_name(compiler,stored_name);
    int index;DiamondClass *class;
    if(existing_class>=0) {
        class=compiler->program->classes[(size_t)existing_class];
        if(!class->declared_by_discovery) {
            fail(compiler,name,"struct is already defined");return nullptr;
        }
        index=existing_class;
    } else {
        if(compiler->program->class_count==DIAMOND_MAX_CLASSES) {
            fail(compiler,name,"too many classes: a program holds at most 1024 "
                 "classes and structs, cuts included");return nullptr;
        }
        class=diamond_program_class_slot(compiler->program,compiler->program->class_count);
        if(class==nullptr) {fail(compiler,name,"out of memory");return nullptr;}
        index=(int)compiler->program->class_count++;
    }
    if(class->declared_by_discovery) {
        copy_field_facts_to_discovered(class);
    } else {
        zero_table(class->discovered_field_type_status,class->field_capacity*sizeof class->discovered_field_type_status[0]);
        zero_table(class->discovered_field_known_class,class->field_capacity*sizeof class->discovered_field_known_class[0]);
    }
    zero_table(class->methods,class->method_capacity*sizeof class->methods[0]);
    zero_table(class->singleton_methods,class->singleton_method_capacity*sizeof class->singleton_methods[0]);
    zero_table(class->fields,class->field_capacity*sizeof class->fields[0]);
    zero_table(class->field_type_status,class->field_capacity*sizeof class->field_type_status[0]);
    zero_table(class->field_known_class,class->field_capacity*sizeof class->field_known_class[0]);
    zero_table(class->class_variables,class->class_variable_capacity*sizeof class->class_variables[0]);
    class->method_count=0;class->singleton_method_count=0;
    class->field_count=0;class->class_variable_count=0;
    class->declared_by_discovery=false;
    class->superclass=DIAMOND_NO_CLASS;class->sealed=false;
    class->generated_struct=true;
    class->declaration_line=(uint32_t)name.line;
    class->declaration_column=(uint32_t)name.column;
    class->declaration_start=name.start;
    (void)snprintf(class->name,sizeof class->name,"%s",stored_name);
    *index_out=index;
    return class;
}

/* Parses `(field: Type, ...)` after the struct name, recording each field
 * on class. */
static bool parse_struct_fields(Compiler *compiler, DiamondClass *class,
        DiamondSpan *field_name_spans, int *field_type_sets, size_t *field_count_out) {
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after struct name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    size_t field_count=0;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        while(!compiler->failed) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,"expected field name");break;
            }
            if(field_count==DIAMOND_MAX_DECLARED_PARAMETERS) {
                fail(compiler,compiler->current.span,"too many struct fields");break;
            }
            const DiamondSpan field_span=compiler->current.span;
            char field_name[DIAMOND_MAX_FUNCTION_NAME];
            if(field_span.length>=DIAMOND_MAX_FUNCTION_NAME) {
                fail(compiler,field_span,"field name is too long");break;
            }
            for(size_t ch=0;ch<field_span.length;ch++)
                field_name[ch]=compiler->source[field_span.start+ch];
            field_name[field_span.length]='\0';
            if(strcmp(field_name,"initialize")==0||strcmp(field_name,"==")==0||
               strcmp(field_name,"to_s")==0) {
                fail(compiler,field_span,
                     "field name collides with a generated struct method");
                break;
            }
            for(size_t existing=0;existing<class->field_count;existing++)
                if(strcmp(class->fields[existing],field_name)==0) {
                    fail(compiler,field_span,"duplicate struct field name");break;
                }
            if(compiler->failed)break;
            advance_token(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                fail(compiler,compiler->current.span,
                     "expected ':' and a type after field name");break;
            }
            advance_token(compiler);
            const int type_set=parse_type_annotation(compiler);
            if(!diamond_class_reserve_fields(class,class->field_count+1)) {
                fail(compiler,field_span,"too many fields");break;
            }
            (void)snprintf(class->fields[class->field_count],
                DIAMOND_MAX_FUNCTION_NAME,"%s",field_name);
            field_name_spans[field_count]=field_span;
            field_type_sets[field_count]=type_set;
            field_count++;class->field_count++;
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN)break;
        }
    }
    if(compiler->failed)return false;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after struct fields");
        return false;
    }
    advance_token(compiler);
    *field_count_out=field_count;
    return true;
}

/* Parses `< Parent` after the field list: the parent must be an earlier
 * struct, and its fields take the first slots. */
static bool parse_struct_superclass(Compiler *compiler, DiamondClass *class,
        int index, const DiamondSpan *field_name_spans, size_t own_field_count,
        size_t *inherited_count_out, const DiamondFunction **inherited_init_out) {
    size_t inherited_count=0;
    const DiamondFunction *inherited_init=nullptr;
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected superclass name after '<'");return false;
    }
    const DiamondSpan superclass_span=compiler->current.span;
    char superclass_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!consume_qualified_name(compiler,superclass_name,sizeof superclass_name)) {
        fail(compiler,superclass_span,"superclass name is too long");return false;
    }
    const int parent=find_class_qualified_or_scoped(compiler,superclass_name);
    if(parent<0) {
        fail(compiler,superclass_span,"undefined superclass");return false;
    }
    const DiamondClass *parent_class=compiler->program->classes[(size_t)parent];
    if(parent==index||!parent_class->generated_struct) {
        fail(compiler,superclass_span,
             "a struct's superclass must be a struct declared earlier");
        return false;
    }
    for(size_t method=0;method<parent_class->method_count;method++)
        if(strcmp(parent_class->methods[method].name,"initialize")==0&&
           (size_t)parent_class->methods[method].function_index<
               compiler->program->function_count)
            inherited_init=compiler->program->functions[
                parent_class->methods[method].function_index];
    if(inherited_init==nullptr) {
        fail(compiler,superclass_span,
             "a struct's superclass must be a struct declared earlier");
        return false;
    }
    inherited_count=parent_class->field_count;
    if(inherited_count+own_field_count>DIAMOND_MAX_DECLARED_PARAMETERS) {
        fail(compiler,superclass_span,"too many struct fields");return false;
    }
    for(size_t own=0;own<own_field_count;own++)
        for(size_t inherited=0;inherited<inherited_count;inherited++)
            if(strcmp(class->fields[own],parent_class->fields[inherited])==0) {
                fail(compiler,field_name_spans[own],
                     "struct field is already defined by its superclass");
                return false;
            }
    /* Inherited fields take the first slots, as in an ordinary
     * subclass, so the parent's methods and readers keep working. */
    for(size_t own=own_field_count;own-->0;) {
        memcpy(class->fields[own+inherited_count],class->fields[own],
            DIAMOND_MAX_FUNCTION_NAME);
        class->field_type_status[own+inherited_count]=class->field_type_status[own];
        class->field_known_class[own+inherited_count]=class->field_known_class[own];
    }
    for(size_t inherited=0;inherited<inherited_count;inherited++) {
        memcpy(class->fields[inherited],parent_class->fields[inherited],
            DIAMOND_MAX_FUNCTION_NAME);
        class->field_type_status[inherited]=parent_class->field_type_status[inherited];
        class->field_known_class[inherited]=parent_class->field_known_class[inherited];
    }
    class->field_count=inherited_count+own_field_count;
    class->superclass=(uint16_t)parent;
    *inherited_count_out=inherited_count;
    *inherited_init_out=inherited_init;
    return true;
}

    /* initialize(field0, field1, ...): one SET_IVAR per field, register
     * 1+index holding that parameter by the ordinary calling convention
     * (see compile_attribute_named's own identical comment on this). */
static void emit_struct_initialize(Compiler *compiler, DiamondClass *class,
        int index, DiamondSpan name, const int *field_type_sets,
        size_t inherited_count, size_t own_field_count,
        const DiamondFunction *inherited_init) {
    const size_t total_count=inherited_count+own_field_count;
    size_t init_function_index=0;
    DiamondFunction *init=
        compiler_add_function(compiler,&init_function_index);
    if(init==nullptr) {
        fail(compiler,name,"out of memory");
    } else {
        (void)snprintf(init->name,sizeof init->name,"initialize");
        init->owner_class=(uint16_t)index;
        init->arity=(uint8_t)(total_count+1);
        init->required_arity=init->arity;
        init->return_type_set=DIAMOND_NO_TYPE_SET;
        init->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
        for(size_t p=0;p<DIAMOND_MAX_DECLARED_PARAMETERS;p++)
            init->parameter_type_sets[p]=DIAMOND_NO_TYPE_SET;
        for(size_t field=0;field<total_count;field++)
            (void)snprintf(init->parameter_names[field],
                DIAMOND_MAX_FUNCTION_NAME,"%s",class->fields[field]);
        /* Decoded *before* begin_struct_method below retargets
         * compiler->function to `init` (a fresh function with its own,
         * still-empty type_sets table) -- field_type_sets[] indexes into
         * the *enclosing* function's type_sets (valid when parse_type_
         * annotation captured it, back when the field list was parsed),
         * not init's own. Decoding after the retarget silently read the
         * wrong table (index out of init's own much-smaller type_set_
         * count, so type_set_single_class always returned -1) -- caught
         * by this phase's own verification methodology (compared JIT
         * eligibility with/without this fix on a real struct case, not
         * assumed), not proven correct by inspection alone. */
        int field_known_classes[DIAMOND_MAX_DECLARED_PARAMETERS];
        for(size_t field=0;field<own_field_count;field++)
            field_known_classes[inherited_count+field]=
                type_set_single_class(compiler,field_type_sets[field]);
        const DiamondFunction *struct_outer=compiler->function;
        StructMethodState state;
        if(!begin_struct_method(compiler,init,&state)) {
            fail(compiler,name,"out of memory compiling initialize");
        } else {
            (void)allocate_register(compiler);
            for(size_t field=0;field<total_count;field++) {
                const bool inherited=field<inherited_count;
                const uint16_t value_register=allocate_register(compiler);
                /* Check each argument against its field's declared type,
                 * as a hand-written initialize(x: Int, ...) would -- at
                 * construction, not first when the reader is called.
                 * Recording it as the parameter's type also gives calls
                 * the compile-time argument check. */
                const uint16_t parameter_set=inherited
                    ? (inherited_init->parameter_type_sets[field]==
                           DIAMOND_NO_TYPE_SET
                        ? DIAMOND_NO_TYPE_SET
                        : clone_type_set_into_current(compiler,
                            inherited_init->type_sets,
                            inherited_init->type_set_count,
                            inherited_init->parameter_type_sets[field]))
                    : clone_type_set_into_current(compiler,
                        struct_outer->type_sets,struct_outer->type_set_count,
                        (uint16_t)field_type_sets[field-inherited_count]);
                if(parameter_set!=DIAMOND_NO_TYPE_SET) {
                    init->parameter_type_sets[field]=parameter_set;
                    emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,value_register,
                        parameter_set,0,2);
                }
                emit_instruction(compiler,DIAMOND_OP_SET_IVAR,0,
                    (uint16_t)field,value_register,3);
                /* See record_field_known_type's own comment: a struct
                 * field's declared type (required syntax, never
                 * DIAMOND_NO_TYPE_SET) feeds the same class-level fact a
                 * bare `self.field =` assignment would, so this field
                 * doesn't wrongly look "never assigned" (status 0). */
                if(!inherited)
                    record_field_known_type(class,field,field_known_classes[field]);
            }
            emit_instruction(compiler,DIAMOND_OP_RETURN,0,0,0,1);
            end_struct_method(compiler,&state);
        }
        register_struct_method(compiler,class,name,"initialize",
            (uint16_t)init_function_index,(uint8_t)total_count);
    }
}

    /* ==(other): IS_TYPE, then an exact-class check (SAME_CLASS), then a short-circuit AND
     * of one GET_IVAR+GET_IVAR+EQUAL+JUMP_IF_FALSE per field -- the same
     * shape compile_begin's own rescue-type matching already uses. */
static void emit_struct_equals(Compiler *compiler, DiamondClass *class,
        int index, DiamondSpan name, size_t total_count) {
    size_t eq_function_index=0;
    DiamondFunction *eq=compiler_add_function(compiler,&eq_function_index);
    if(eq==nullptr) {
        fail(compiler,name,"out of memory");
    } else {
        (void)snprintf(eq->name,sizeof eq->name,"==");
        eq->owner_class=(uint16_t)index;
        eq->arity=2;eq->required_arity=2;
        eq->return_type_set=DIAMOND_NO_TYPE_SET;
        eq->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
        for(size_t p=0;p<DIAMOND_MAX_DECLARED_PARAMETERS;p++)
            eq->parameter_type_sets[p]=DIAMOND_NO_TYPE_SET;
        (void)snprintf(eq->parameter_names[0],DIAMOND_MAX_FUNCTION_NAME,"other");
        StructMethodState state;
        if(!begin_struct_method(compiler,eq,&state)) {
            fail(compiler,name,"out of memory compiling ==");
        } else {
            (void)allocate_register(compiler);
            const uint16_t other=allocate_register(compiler);
            const uint16_t type_ok=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,type_ok,other,
                (uint16_t)(DIAMOND_TYPE_CLASS_BASE+index),3);
            size_t mismatch_jumps[DIAMOND_MAX_DECLARED_PARAMETERS+1];
            size_t mismatch_count=0;
            mismatch_jumps[mismatch_count++]=
                emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,type_ok);
            /* A subclass instance has the struct's fields too, but is
             * not equal to an instance of another class. */
            const uint16_t same_class=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_SAME_CLASS,same_class,0,other,3);
            mismatch_jumps[mismatch_count++]=
                emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,same_class);
            for(size_t field=0;field<total_count;field++) {
                const uint16_t left=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,left,0,
                    (uint16_t)field,3);
                const uint16_t right=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,right,other,
                    (uint16_t)field,3);
                const uint16_t equal=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_EQUAL,equal,left,right,3);
                mismatch_jumps[mismatch_count++]=
                    emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,equal);
            }
            const uint16_t true_result=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_BOOL,true_result,true,0,2);
            emit_instruction(compiler,DIAMOND_OP_RETURN,true_result,0,0,1);
            const size_t mismatch_target=compiler->function->code_count;
            for(size_t jump=0;jump<mismatch_count;jump++)
                patch_jump(compiler,mismatch_jumps[jump],mismatch_target);
            const uint16_t false_result=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_BOOL,false_result,false,0,2);
            emit_instruction(compiler,DIAMOND_OP_RETURN,false_result,0,0,1);
            end_struct_method(compiler,&state);
        }
        register_struct_method(compiler,class,name,"==",
            (uint16_t)eq_function_index,1);
    }
}

    /* to_s(): "ClassName(field0: v0, field1: v1)" -- a CONSTANT-free
     * chain of STRING/GET_IVAR/TO_STRING/ADD, exactly the shape
     * parse_string's own interpolation-joining loop already uses. */
static void emit_struct_to_s(Compiler *compiler, DiamondClass *class,
        int index, DiamondSpan name, const char *stored_name, size_t total_count) {
    size_t to_s_function_index=0;
    DiamondFunction *to_s=compiler_add_function(compiler,&to_s_function_index);
    if(to_s==nullptr) {
        fail(compiler,name,"out of memory");
    } else {
        (void)snprintf(to_s->name,sizeof to_s->name,"to_s");
        to_s->owner_class=(uint16_t)index;
        to_s->arity=1;to_s->required_arity=1;
        to_s->return_type_set=DIAMOND_NO_TYPE_SET;
        to_s->inferred_return_type_set=DIAMOND_NO_TYPE_SET;
        for(size_t p=0;p<DIAMOND_MAX_DECLARED_PARAMETERS;p++)
            to_s->parameter_type_sets[p]=DIAMOND_NO_TYPE_SET;
        StructMethodState state;
        if(!begin_struct_method(compiler,to_s,&state)) {
            fail(compiler,name,"out of memory compiling to_s");
        } else {
            (void)allocate_register(compiler);
            char piece[DIAMOND_MAX_FUNCTION_NAME+4];
            (void)snprintf(piece,sizeof piece,"%s(",stored_name);
            uint16_t result=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_STRING,result,
                add_literal_string(compiler,piece,name),0,2);
            for(size_t field=0;field<total_count;field++) {
                (void)snprintf(piece,sizeof piece,field==0?"%s: ":", %s: ",
                    class->fields[field]);
                const uint16_t prefix=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_STRING,prefix,
                    add_literal_string(compiler,piece,name),0,2);
                const uint16_t joined=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ADD,joined,result,
                    prefix,3);
                const uint16_t field_value=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,field_value,0,
                    (uint16_t)field,3);
                const uint16_t stringified=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_TO_STRING,stringified,
                    field_value,0,2);
                result=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ADD,result,joined,
                    stringified,3);
            }
            const uint16_t suffix=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_STRING,suffix,
                add_literal_string(compiler,")",name),0,2);
            const uint16_t final_result=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ADD,final_result,result,
                suffix,3);
            emit_instruction(compiler,DIAMOND_OP_RETURN,final_result,0,0,1);
            end_struct_method(compiler,&state);
        }
        register_struct_method(compiler,class,name,"to_s",
            (uint16_t)to_s_function_index,0);
    }
}

/* `struct Name(field: Type, ...) ... end` -- a compile-time-only data
 * class: registers an ordinary DiamondClass (no runtime synthesis, see
 * docs/roadmap.md's "Explicitly deferred" entry on that) with one field
 * per declared member, then generates `initialize`/one reader per
 * field/`==`/`to_s` exactly as if they'd been hand-written. Readers
 * reuse compile_attribute_named verbatim (same as attr_reader); the
 * other three are hand-emitted via begin_struct_method's swap since
 * their bodies (a per-field GET_IVAR/SET_IVAR/EQUAL/IS_TYPE/TO_STRING
 * chain) don't correspond to any parseable source text. Deliberately
 * narrow for this first pass -- no superclass, no reopening, no
 * additional body -- see docs/classes-and-modules.md's own "struct
 * declarations" section for the reasoning. */
uint16_t compile_struct(Compiler *compiler) {
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected valid struct name");
        return 0;
    }
    const DiamondSpan name=compiler->current.span;
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"struct name is too long");return 0;
    }
    if(find_interface_name(compiler,stored_name)>=0||
       find_module_name(compiler,stored_name)>=0) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    /* Registered *before* the field list parses (unlike compile_class's
     * own superclass clause, which is likewise parsed only after this
     * same registration) so a self-referential field type -- `struct
     * Node(value: Int, next: Node)` -- can already resolve this class's
     * own name via parse_type_annotation/resolve_type_name below. */
    int index=0;
    DiamondClass *class=declare_struct_class(compiler,name,stored_name,&index);
    if(class==nullptr)return 0;
    DiamondSpan field_name_spans[DIAMOND_MAX_DECLARED_PARAMETERS];
    int field_type_sets[DIAMOND_MAX_DECLARED_PARAMETERS];
    size_t field_count=0;
    if(!parse_struct_fields(compiler,class,field_name_spans,field_type_sets,&field_count))
        return 0;
    /* The fields declared above, and (below) the whole inherited-then-own
     * list the generated methods cover. */
    const size_t own_field_count=field_count;
    size_t inherited_count=0;
    const DiamondFunction *inherited_init=nullptr;
    if(compiler->current.kind==DIAMOND_TOKEN_LESS&&
       !parse_struct_superclass(compiler,class,index,field_name_spans,
           own_field_count,&inherited_count,&inherited_init))
        return 0;
    const size_t total_count=inherited_count+own_field_count;
    if(!consume_block_start(compiler))return 0;
    const int outer_class=compiler->current_class;
    compiler->current_class=index;
    const bool outer_private=compiler->methods_private;
    const bool outer_protected=compiler->methods_protected;
    compiler->methods_private=false;compiler->methods_protected=false;
    for(size_t field=0;field<field_count&&!compiler->failed;field++)
        compile_attribute_named(compiler,false,false,
            field_name_spans[field],field_type_sets[field]);
    if(!compiler->failed)
        emit_struct_initialize(compiler,class,index,name,field_type_sets,
            inherited_count,own_field_count,inherited_init);
    if(!compiler->failed)emit_struct_equals(compiler,class,index,name,total_count);
    if(!compiler->failed)
        emit_struct_to_s(compiler,class,index,name,stored_name,total_count);
    /* A hand-written def/attr/include supplementing the generated
     * members above -- see compile_class_body's own comment. A def named
     * == or to_s replaces the generated one (struct_replaceable_*; see
     * compile_definition's duplicate check). Any other colliding name (a
     * def or attr matching a generated reader or initialize) still fails
     * via compile_definition's/compile_attribute_named's own existing
     * duplicate-method checks. */
    const int outer_replaceable_class=compiler->struct_replaceable_class;
    const bool outer_replaceable_eq=compiler->struct_replaceable_eq;
    const bool outer_replaceable_to_s=compiler->struct_replaceable_to_s;
    compiler->struct_replaceable_class=(int)index;
    compiler->struct_replaceable_eq=true;
    compiler->struct_replaceable_to_s=true;
    if(!compiler->failed) compile_class_body(compiler,class);
    compiler->struct_replaceable_class=outer_replaceable_class;
    compiler->struct_replaceable_eq=outer_replaceable_eq;
    compiler->struct_replaceable_to_s=outer_replaceable_to_s;
    if(compiler->current.kind==DIAMOND_TOKEN_END) advance_token(compiler);
    compiler->current_class=outer_class;
    compiler->methods_private=outer_private;
    compiler->methods_protected=outer_protected;
    const uint16_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

/* Scans one `(...)` parameter list via a throwaway lookahead lexer copy
 * (never touches compiler->lexer/current/previous), computing just
 * enough -- arity, required_arity, has_variadic -- to register an
 * accurate forward-reference placeholder before any body compiles.
 * Assumes `lookahead` is already positioned to read the token right
 * after `(` next. Mirrors compile_definition's own parameter loop's
 * *counting* rules exactly (see that loop's own comments): every
 * declared parameter (block, variadic, or ordinary) increments arity;
 * only a non-block, non-variadic parameter with no `=` default
 * increments required_arity, and only while no earlier parameter had
 * one (a required parameter after a defaulted one is already a real
 * compile error compile_definition's own loop reports for real moments
 * later -- this scanner doesn't need to detect that itself, just avoid
 * overcounting required_arity for a program that's about to fail
 * compilation anyway). Returns the token right after the closing `)`
 * (DIAMOND_TOKEN_EOF on malformed/unterminated input). */
static DiamondToken prescan_parameter_arity(DiamondLexer *lookahead,
        uint8_t *arity_out, uint8_t *required_arity_out, bool *has_variadic_out) {
    uint8_t arity=0,required_arity=0;bool has_variadic=false,saw_default=false;
    DiamondToken token=diamond_lexer_next(lookahead);
    size_t nesting=0;
    while(token.kind!=DIAMOND_TOKEN_EOF&&
          !(token.kind==DIAMOND_TOKEN_RIGHT_PAREN&&nesting==0)) {
        bool is_block=false,is_variadic=false;
        if(token.kind==DIAMOND_TOKEN_AMPERSAND) {
            is_block=true;token=diamond_lexer_next(lookahead);
        }
        if(token.kind==DIAMOND_TOKEN_STAR) {
            is_variadic=true;token=diamond_lexer_next(lookahead);
        }
        if(arity<UINT8_MAX)arity++;
        bool saw_equal=false;
        while(token.kind!=DIAMOND_TOKEN_EOF) {
            if(token.kind==DIAMOND_TOKEN_LEFT_PAREN||
               token.kind==DIAMOND_TOKEN_LEFT_BRACKET||
               token.kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
            else if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
                if(nesting==0)break;
                nesting--;
            } else if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                      token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                if(nesting>0)nesting--;
            } else if(token.kind==DIAMOND_TOKEN_COMMA&&nesting==0) {
                token=diamond_lexer_next(lookahead);break;
            } else if(token.kind==DIAMOND_TOKEN_EQUAL&&nesting==0) {
                saw_equal=true;
            }
            token=diamond_lexer_next(lookahead);
        }
        if(is_variadic)has_variadic=true;
        else if(saw_equal)saw_default=true;
        else if(!is_block&&!saw_default&&required_arity<UINT8_MAX)required_arity++;
    }
    *arity_out=arity;*required_arity_out=required_arity;*has_variadic_out=has_variadic;
    return token;
}

/* Scan a class/module body without allocating function slots: reserving
 * slots here would change the source-order numbering of generated methods,
 * nested declarations, and closures. Discovery records signatures and patches
 * calls once their definitions compile. Modules retain those entries for the
 * real pass; classes rebuild their tables, so their real-pass scan recovers
 * the reserved functions by source location to preserve type information.
 * Only direct singleton definitions (and module_function-mode definitions in
 * modules) are collected; nested bodies and trailing modifiers are balanced
 * by the token walk below. The normal parser remains responsible for syntax
 * validation and duplicate-definition diagnostics. */
/* Does the `def` just read (the lookahead is positioned after it) use the
 * endless `def name(params) = expr` form, which has no `end`? True when an
 * `=` follows the parameter list on the same line. */
static bool prescan_def_is_endless(DiamondLexer lookahead) {
    size_t paren_depth=0;
    bool seen_parameters=false;
    for(DiamondToken token=diamond_lexer_next(&lookahead);
        token.kind!=DIAMOND_TOKEN_NEWLINE&&token.kind!=DIAMOND_TOKEN_EOF;
        token=diamond_lexer_next(&lookahead)) {
        if(token.kind==DIAMOND_TOKEN_LEFT_PAREN) paren_depth++;
        else if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN&&paren_depth>0) {
            if(--paren_depth==0)seen_parameters=true;
        } else if(token.kind==DIAMOND_TOKEN_EQUAL&&paren_depth==0&&seen_parameters)
            return true;
    }
    return false;
}

/* Can a token of this kind end an expression? An if/unless/while/until
 * right after one is a trailing modifier (`x = 1 if y`, `return if done`),
 * which has no `end` of its own. */
static bool prescan_ends_value(DiamondTokenKind kind) {
    switch(kind) {
        case DIAMOND_TOKEN_IDENTIFIER: case DIAMOND_TOKEN_INTEGER:
        case DIAMOND_TOKEN_FLOAT: case DIAMOND_TOKEN_STRING:
        case DIAMOND_TOKEN_SYMBOL: case DIAMOND_TOKEN_RIGHT_PAREN:
        case DIAMOND_TOKEN_RIGHT_BRACKET: case DIAMOND_TOKEN_RIGHT_BRACE:
        case DIAMOND_TOKEN_TRUE: case DIAMOND_TOKEN_FALSE: case DIAMOND_TOKEN_NIL:
        case DIAMOND_TOKEN_SELF: case DIAMOND_TOKEN_END:
        case DIAMOND_TOKEN_INSTANCE_VARIABLE: case DIAMOND_TOKEN_CLASS_VARIABLE:
        case DIAMOND_TOKEN_RETURN: case DIAMOND_TOKEN_BREAK: case DIAMOND_TOKEN_NEXT:
        case DIAMOND_TOKEN_REGEX:
        case DIAMOND_TOKEN_REDO: case DIAMOND_TOKEN_RETRY:
            return true;
        default:
            return false;
    }
}

void prescan_singleton_signatures(Compiler *compiler,DiamondMethod **methods,
        size_t *method_count,size_t *method_capacity,bool class_scope) {
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=compiler->current;
    size_t depth=0;
    bool module_function_mode=false;
    bool pending_loop_do=false;
    DiamondTokenKind previous=DIAMOND_TOKEN_NEWLINE;
    while(token.kind!=DIAMOND_TOKEN_EOF) {
        const bool modifier_position=prescan_ends_value(previous);
        previous=token.kind;
        if(token.kind==DIAMOND_TOKEN_NEWLINE) {
            pending_loop_do=false;
        } else if((token.kind==DIAMOND_TOKEN_WHILE||token.kind==DIAMOND_TOKEN_UNTIL||
                   token.kind==DIAMOND_TOKEN_IF||token.kind==DIAMOND_TOKEN_UNLESS)&&
                  modifier_position) {
            /* A trailing modifier: no body, no `end`. */
        } else if(token.kind==DIAMOND_TOKEN_WHILE||
                   token.kind==DIAMOND_TOKEN_UNTIL||
                   token.kind==DIAMOND_TOKEN_LOOP) {
            depth++;pending_loop_do=true;
        } else if(token.kind==DIAMOND_TOKEN_DO) {
            if(pending_loop_do)pending_loop_do=false;
            else depth++;
        } else if(token.kind==DIAMOND_TOKEN_IF||token.kind==DIAMOND_TOKEN_UNLESS||
                   token.kind==DIAMOND_TOKEN_CASE||token.kind==DIAMOND_TOKEN_BEGIN||
                   token.kind==DIAMOND_TOKEN_CLASS||token.kind==DIAMOND_TOKEN_INTERFACE||
                   token.kind==DIAMOND_TOKEN_CLOSURE||token.kind==DIAMOND_TOKEN_MODULE) {
            depth++;
        } else if(token.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            if(depth==0&&!class_scope) {
                /* Only the bare `module_function` directive (no args)
                 * toggles mode -- `module_function(:a, :b)`/`module_function
                 * a, b` marks already-declared methods explicitly and
                 * can't itself introduce a forward reference (its own
                 * targets must already exist as real module->methods[]
                 * entries), so this scanner only needs to notice whether
                 * an identifier or '(' follows before treating it as a
                 * mode toggle rather than the explicit-list form. */
                DiamondToken next=diamond_lexer_next(&lookahead);
                if(next.kind!=DIAMOND_TOKEN_LEFT_PAREN&&
                   next.kind!=DIAMOND_TOKEN_IDENTIFIER)
                    module_function_mode=true;
                token=next;continue;
            }
        } else if(token.kind==DIAMOND_TOKEN_DEF) {
            /* An endless def has no `end` to balance: register it the same
             * way at depth 0, but don't count it as opening a body. */
            const bool endless=prescan_def_is_endless(lookahead);
            const bool top_level_def=depth==0;
            if(!endless)depth++;
            if(top_level_def) {
                DiamondToken next=diamond_lexer_next(&lookahead);
                bool is_module_singleton=false;
                if(next.kind==DIAMOND_TOKEN_SELF) {
                    DiamondToken dot=diamond_lexer_next(&lookahead);
                    if(dot.kind==DIAMOND_TOKEN_DOT) {
                        is_module_singleton=true;
                        next=diamond_lexer_next(&lookahead);
                    } else {
                        /* Not actually `self.name` (malformed -- the real
                         * compile will report it for real); `dot` is
                         * already consumed from the lookahead, so resume
                         * the outer walk from there instead of losing it. */
                        token=dot;continue;
                    }
                }
                if((is_module_singleton||module_function_mode)&&
                   next.kind==DIAMOND_TOKEN_IDENTIFIER) {
                    const DiamondSpan name_span=next.span;
                    char method_name[DIAMOND_MAX_FUNCTION_NAME];
                    (void)snprintf(method_name,sizeof method_name,"%.*s",
                        (int)name_span.length,compiler->source+name_span.start);
                    DiamondToken after_name=diamond_lexer_next(&lookahead);
                    if(after_name.kind==DIAMOND_TOKEN_EQUAL) {
                        DiamondLexer writer_probe=lookahead;
                        const DiamondToken parameters=diamond_lexer_next(&writer_probe);
                        if(parameters.kind==DIAMOND_TOKEN_LEFT_PAREN&&
                           name_span.length+1<sizeof method_name) {
                            method_name[name_span.length]='=';
                            method_name[name_span.length+1]='\0';
                            lookahead=writer_probe;after_name=parameters;
                        }
                    }
                    /* `def name[T](...)` -- skip an optional generic
                     * type-parameter list (nesting-aware, though a
                     * simple depth counter suffices here: this scanner
                     * only needs to find the '(' that follows, not
                     * interpret the type parameters themselves). */
                    if(after_name.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
                        size_t bracket_depth=1;
                        after_name=diamond_lexer_next(&lookahead);
                        while(after_name.kind!=DIAMOND_TOKEN_EOF&&bracket_depth>0) {
                            if(after_name.kind==DIAMOND_TOKEN_LEFT_BRACKET)bracket_depth++;
                            else if(after_name.kind==DIAMOND_TOKEN_RIGHT_BRACKET)bracket_depth--;
                            if(bracket_depth>0)after_name=diamond_lexer_next(&lookahead);
                        }
                        if(after_name.kind!=DIAMOND_TOKEN_EOF)
                            after_name=diamond_lexer_next(&lookahead);
                    }
                    uint8_t arity=0,required_arity=0;bool has_variadic=false;
                    DiamondToken past_parameters=after_name;
                    if(after_name.kind==DIAMOND_TOKEN_LEFT_PAREN)
                        past_parameters=prescan_parameter_arity(&lookahead,
                            &arity,&required_arity,&has_variadic);
                    /* A second def of the same name is a duplicate the real
                     * definition pass reports; don't pre-register it too. */
                    bool already_registered=false;
                    for(size_t existing=0;existing<(*method_count);existing++) {
                        const char *known=(*methods)[existing].name;
                        if(strcmp(known,method_name)==0)
                            already_registered=true;
                    }
                    if(!already_registered&&diamond_reserve_methods(methods,
                            method_capacity,*method_count+1)) {
                        DiamondMethod *entry=
                            &(*methods)[(*method_count)++];
                        *entry=(DiamondMethod){0};
                        (void)snprintf(entry->name,sizeof entry->name,"%s",method_name);
                        entry->arity=arity;entry->required_arity=required_arity;
                        entry->has_variadic=has_variadic;
                        /* A module_function-mode method's owner_class
                         * reserves register 0 as an implicit instance-
                         * mixing receiver (compile_definition's own
                         * direct_module_member) even for the qualified
                         * call path, so needs_receiver=true there
                         * (matching compile_definition's own early-
                         * registration -- see that site's own
                         * exported.needs_receiver=true). `def self.x`
                         * reserves no such slot at all (owner_class
                         * stays UINT8_MAX, module_singleton excludes
                         * direct_module_member) -- needs_receiver=true
                         * there would silently reserve and count an
                         * extra call-argument slot nothing ever fills,
                         * inflating call_count by one and failing the
                         * real def's own (correctly slot-less) arity
                         * check at runtime. Confirmed the hard way:
                         * `def self.table(name)` called as
                         * `Widgets.table("authors")` failed "wrong
                         * number of arguments" until this matched
                         * compile_module's own `def self.x` site, which
                         * never sets this field at all (defaults to
                         * false). */
                        entry->needs_receiver=class_scope||!is_module_singleton;
                        entry->function_index=DIAMOND_UNRESOLVED_SINGLETON_FUNCTION;
                        /* The real pass already has complete discovered
                         * functions. Match the source declaration so typed
                         * calls and method references keep their contracts. */
                        if(class_scope&&!compiler->discovery_pass)
                            for(size_t function=0;function<compiler->program->function_count;function++) {
                                const DiamondFunction *candidate=compiler->program->functions[function];
                                if(candidate->declared_by_discovery&&
                                   candidate->declaration_start==name_span.start) {
                                    entry->function_index=(uint16_t)function;break;
                                }
                            }
                    }
                    token=past_parameters;continue;
                }
                token=next;continue;
            }
        } else if(token.kind==DIAMOND_TOKEN_END) {
            if(depth==0)return; /* the module's own terminating end */
            depth--;
        }
        token=diamond_lexer_next(&lookahead);
    }
}

/* Finds or creates the module named by `name` and readies its slot for this
 * pass (see the reopen and reset comments below). Returns nullptr on error. */
static DiamondModule *declare_module(Compiler *compiler,DiamondSpan name,
        const char *stored_name,int *index_out) {
    const int existing_module=find_module_name(compiler,stored_name);
    /* A module can always be reopened -- a second `module Foo ... end`
     * adds to the same module rather than erroring (see docs/syntax.md's
     * "Classes" section and this session's own module/class-reopening
     * design). Only a cross-*kind* collision (the same name already a
     * class or interface) stays a hard error. */
    if(find_class_name(compiler,stored_name)>=0||
       find_interface_name(compiler,stored_name)>=0) {
        fail(compiler,name,"module name is already defined");return nullptr;
    }
    int index;
    DiamondModule *module;
    if(existing_module>=0) {
        index=existing_module;
        module=compiler->program->modules[(size_t)index];
        if(module->declared_by_discovery&&module->from_template) {
            /* A reopened template (prelude) module: back to the template's
             * own methods and fields; singleton functions stay (discovery
             * appended the program's after the template's) and are claimed
             * from just past the template's own. */
            if(module->method_count>module->template_method_count)
                memset(&module->methods[module->template_method_count],0,
                    (module->method_count-module->template_method_count)*sizeof module->methods[0]);
            module->method_count=module->template_method_count;
            module->field_count=module->template_field_count;
            module->next_singleton_claim=module->template_singleton_method_count;
            module->declared_by_discovery=false;
            module->from_template=false;
        } else if(module->declared_by_discovery) {
            /* First time *this* compile pass touches a slot the *other*
             * (already-finished) pass populated -- see diamond_compile's
             * own comment on declared_by_discovery. Full zero, not just
             * the counts: compile_interface's own method->arity++ (and
             * anything else that accumulates into a slot's fields
             * trusting they start at zero) needs truly-empty arrays to
             * rebuild from, not whatever the other pass already left
             * there.
             *
             * singleton_methods[]/singleton_method_count are deliberately
             * NOT included in this reset (unlike methods[]/fields[]):
             * those descriptors (module_function's qualified/exported
             * form, see compile_definition's own early-registration
             * comment) are exactly what let a module_function method
             * compiled *earlier* in the real pass call a sibling defined
             * *later* -- discovery already found and registered every
             * sibling in source order, and each entry's function_index
             * already names the correct real-pass slot (discovery and
             * the real pass reserve functions in identical order), so
             * wiping this table here would only throw away already-
             * correct forward-reference data and reopen exactly the
             * "undefined module singleton function" gap that used to
             * make mutual module_function recursion impossible. Both
             * registration sites (compile_definition's module_function
             * early-registration, and this function's own `def self.x`
             * handling) claim these entries positionally as the real
             * pass reaches them -- next_singleton_claim, reset here,
             * mirrors Compiler's own next_function_claim exactly (see
             * that field's own comment). */
            zero_table(module->methods,module->method_capacity*sizeof module->methods[0]);
            zero_table(module->fields,module->field_capacity*sizeof module->fields[0]);
            module->method_count=0;
            module->field_count=0;
            module->next_singleton_claim=0;
            module->declared_by_discovery=false;
        }
        /* else: already owned by this pass (freshly created earlier in
         * this same pass, or already reset just above) -- this is a
         * genuine reopen within the current pass. Merge new content on
         * top without resetting anything. */
    } else {
        if(compiler->program->module_count==DIAMOND_MAX_MODULES) {
            char message[128];
            snprintf(message,sizeof message,
                "too many modules: a program holds at most %u modules, prelude and cuts included",
                (unsigned)DIAMOND_MAX_MODULES);
            fail(compiler,name,message);return nullptr;
        }
        module=diamond_program_module_slot(compiler->program,
            compiler->program->module_count);
        if(module==nullptr) {
            fail(compiler,name,"out of memory declaring a module");return nullptr;
        }
        index=(int)compiler->program->module_count++;
        /* Explicit reset, not reliance on this slot already being zero:
         * `program` isn't guaranteed freshly calloc'd/memset -- a caller
         * reusing one DiamondProgram across many compiles (tests/
         * run_cases.c's own batch loop) can hand this exact slot back
         * still holding a *different*, unrelated module's own leftover
         * methods/fields/singleton-claim state from an earlier compile.
         * Unlike the reopen branch above (which deliberately keeps
         * singleton_methods/singleton_method_count -- see its own
         * comment on why), a genuinely new slot has no same-pass history
         * worth preserving, so everything resets here. This is what
         * makes diamond_program_init's own memset(program,0,...) safe to
         * skip for `modules[]` -- see that function's own comment
         * (src/compiler.c) and CHANGELOG.md's "Performance". */
        zero_table(module->methods,module->method_capacity*sizeof module->methods[0]);
        zero_table(module->singleton_methods,module->singleton_method_capacity*sizeof module->singleton_methods[0]);
        zero_table(module->fields,module->field_capacity*sizeof module->fields[0]);
        module->method_count=0;
        module->singleton_method_count=0;
        module->field_count=0;
        module->included_module_count=0;
        module->next_singleton_claim=0;
        module->declared_by_discovery=false;
    }
    (void)snprintf(module->name,sizeof module->name,"%s",stored_name);
    module->declaration_line=(uint32_t)name.line;
    module->declaration_column=(uint32_t)name.column;
    module->declaration_start=name.start;
    *index_out=index;
    return module;
}

/* `include Other` inside a module: copies Other's fields and methods in,
 * keeping any of this module's own. The current token is `include`. */
static bool compile_module_include(Compiler *compiler,DiamondModule *module,int index) {
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected module name after 'include'");return false;
    }
    const DiamondSpan include_span=compiler->current.span;
    const int lexical_module=find_module(compiler,include_span);
    char include_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!consume_qualified_name(compiler,include_name,
                               sizeof include_name)) {
        fail(compiler,include_span,"invalid qualified module name");return false;
    }
    const int included=strstr(include_name,"::")==nullptr?
        lexical_module:find_module_name(compiler,include_name);
    if(included==index) {
        fail(compiler,include_span,
             "module cannot include itself");return false;
    }
    if(included<0) {
        fail(compiler,include_span,"undefined module");return false;
    }
    const DiamondModule *source=
        compiler->program->modules[(size_t)included];
    if(!record_included_module(module->included_modules,
            &module->included_module_count,(uint16_t)included)) {
        fail(compiler,include_span,"too many included modules");return false;
    }
    for(size_t imported=0;imported<source->field_count;imported++) {
        bool present=false;
        for(size_t field=0;field<module->field_count;field++)
            if(strcmp(module->fields[field],source->fields[imported])==0)
                present=true;
        if(present)continue;
        if(!diamond_module_reserve_fields(module,module->field_count+1)) {
            fail(compiler,include_span,
                 "included module adds too many fields");break;
        }
        (void)snprintf(module->fields[module->field_count++],
            DIAMOND_MAX_FUNCTION_NAME,"%s",source->fields[imported]);
    }
    if(compiler->failed)return false;
    if(!diamond_reserve_methods(&module->methods,&module->method_capacity,
            module->method_count+source->method_count)) {
        fail(compiler,compiler->current.span,
             "included module adds too many methods");return false;
    }
    for(size_t method=0;method<source->method_count;method++) {
        if(has_own_method_named(compiler,-2,module->methods,
                module->method_count,source->methods[method].name))continue;
        module->methods[module->method_count]=source->methods[method];
        module->methods[module->method_count++].included=true;
    }
    return true;
}

uint16_t compile_module(Compiler *compiler) {
    if(compiler->function!=&compiler->program->entry) {
        fail(compiler,compiler->current.span,
             "modules must be declared at top level");return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected valid module name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"module name is too long");return 0;
    }
    int index=0;
    DiamondModule *module=declare_module(compiler,name,stored_name,&index);
    if(module==nullptr)return 0;
    advance_token(compiler);
    if(!consume_block_start(compiler))return 0;
    /* Only during discovery -- the real pass relies on cross-pass
     * carryover instead (module->next_singleton_claim, see
     * declare_module's reopen comment); see prescan_module_
     * function_signatures's own comment for the full design and why
     * that carryover alone can't close this gap on its own. */
    if(compiler->discovery_pass)
        prescan_singleton_signatures(compiler,&module->singleton_methods,
            &module->singleton_method_count,&module->singleton_method_capacity,false);
    const int outer=compiler->current_module;compiler->current_module=index;
    const bool outer_private=compiler->methods_private;
    const bool outer_protected=compiler->methods_protected;
    const bool outer_module_function=compiler->module_function_mode;
    compiler->methods_private=false;
    compiler->methods_protected=false;
    compiler->module_function_mode=false;
    while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(is_visibility_token(compiler->current.kind)) {
            compile_visibility_token(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            compile_module_function(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_ALIAS_METHOD) {
            compile_alias_method(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_DELEGATE) {
            compile_delegate(compiler);
        } else if(is_attribute_token(compiler->current.kind)) {
            compile_attribute_declaration(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
           assignment_ahead(compiler)) {
            const DiamondSpan constant_name=compiler->current.span;
            const char first=compiler->source[constant_name.start];
            if(first<'A'||first>'Z') {
                fail(compiler,constant_name,
                     "module constants must begin with an uppercase letter");break;
            }
            (void)compile_assignment(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_INCLUDE) {
            if(!compile_module_include(compiler,module,index))break;
        } else if(compiler->current.kind==DIAMOND_TOKEN_DEF) {
            (void)compile_definition(compiler,false);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE) {
            (void)compile_module(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_CLASS||
                  compiler->current.kind==DIAMOND_TOKEN_SEALED) {
            (void)compile_class(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_STRUCT) {
            (void)compile_struct(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_INTERFACE) {
            (void)compile_interface(compiler);
        } else {
            fail(compiler,compiler->current.span,
                 "expected definition or include in module");break;
        }
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE)skip_newlines(compiler);
    }
    compiler->current_module=outer;
    compiler->methods_private=outer_private;
    compiler->methods_protected=outer_protected;
    compiler->module_function_mode=outer_module_function;
    if(compiler->current.kind==DIAMOND_TOKEN_END)advance_token(compiler);
    const uint16_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

uint16_t compile_interface(Compiler *compiler) {
    if(compiler->function!=&compiler->program->entry) {
        fail(compiler,compiler->current.span,
             "interfaces must be declared at top level");return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected valid interface name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"interface name is too long");return 0;
    }
    const int existing_interface=find_interface_name(compiler,stored_name);
    /* See compile_class's own identical comment on declared_by_discovery
     * and the !compiler->discovery_pass conjunct. */
    const bool claiming=existing_interface>=0&&!compiler->discovery_pass&&
        compiler->program->interfaces[(size_t)existing_interface]->declared_by_discovery;
    if(!claiming&&(existing_interface>=0||
       find_class_name(compiler,stored_name)>=0||
       find_module_name(compiler,stored_name)>=0)) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    if(!claiming&&compiler->program->interface_count==DIAMOND_MAX_INTERFACES) {
        char message[128];
        snprintf(message,sizeof message,
            "too many interfaces: a program holds at most %u interfaces, cuts included",
            (unsigned)DIAMOND_MAX_INTERFACES);
        fail(compiler,name,message);return 0;
    }
    DiamondInterface *interface=claiming
        ? compiler->program->interfaces[(size_t)existing_interface]
        : diamond_program_interface_slot(compiler->program,
            compiler->program->interface_count);
    if(interface==nullptr) {
        fail(compiler,name,"out of memory declaring an interface");return 0;
    }
    if(!claiming)compiler->program->interface_count++;
    if(claiming) {
        /* See compile_class's own identical comment on why a full zero
         * of this array, not just the count, is needed here -- this is
         * in fact where the bug was actually found: compile_interface's
         * own method->arity++ below (parsing each `(param, ...)` list)
         * accumulates directly onto whatever was already sitting in
         * interface->methods[n], rather than assigning an absolute
         * value, so a claimed slot's stale discovery-pass arity was
         * silently being added to instead of replaced. */
        zero_table(interface->methods,interface->method_capacity*sizeof interface->methods[0]);
        interface->method_count=0;
        interface->declared_by_discovery=false;
    } else {
        /* Explicit reset, not reliance on this slot already being zero:
         * see compile_class's own identical comment. `program` isn't
         * guaranteed freshly calloc'd/memset -- this exact slot can hold
         * a *different*, unrelated interface's own leftover methods from
         * an earlier compile of a reused DiamondProgram (tests/
         * run_cases.c's own batch loop). This is what makes
         * diamond_program_init's own memset(program,0,...) safe to skip
         * for `interfaces[]` -- see that function's own comment. */
        zero_table(interface->methods,interface->method_capacity*sizeof interface->methods[0]);
        interface->method_count=0;
        interface->declared_by_discovery=compiler->discovery_pass;
    }
    interface->type_sets=compiler->program->entry.type_sets;
    (void)snprintf(interface->name,sizeof interface->name,"%s",stored_name);
    interface->declaration_line=(uint32_t)name.line;
    interface->declaration_column=(uint32_t)name.column;
    interface->declaration_start=name.start;
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_LESS) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(true) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,
                     "expected base interface name after '<'");return 0;
            }
            const int base_index=find_interface(compiler,compiler->current.span);
            if(base_index<0) {
                fail(compiler,compiler->current.span,"undefined base interface");
                return 0;
            }
            /* interfaces[] is a fixed array (DIAMOND_MAX_INTERFACES),
             * never reallocated, so this pointer stays valid even
             * though `interface` itself points at a later slot in the
             * same array. */
            const DiamondInterface *base=
                compiler->program->interfaces[(size_t)base_index];
            for(size_t index=0;index<base->method_count;index++) {
                bool duplicate=false;
                for(size_t existing=0;existing<interface->method_count;existing++)
                    if(strcmp(interface->methods[existing].name,
                              base->methods[index].name)==0) {
                        duplicate=true;break;
                    }
                if(duplicate) {
                    fail(compiler,compiler->current.span,
                         "duplicate interface method");return 0;
                }
                if(!diamond_reserve_interface_methods(&interface->methods,&interface->method_capacity,interface->method_count+1)) {
                    fail(compiler,compiler->current.span,
                         "interface has too many methods");return 0;
                }
                interface->methods[interface->method_count++]=base->methods[index];
            }
            advance_token(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
    }
    if(!consume_block_start(compiler))return 0;
    while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind!=DIAMOND_TOKEN_DEF||
           !diamond_reserve_interface_methods(&interface->methods,&interface->method_capacity,interface->method_count+1)) {
            fail(compiler,compiler->current.span,
                 "expected method signature in interface");break;
        }
        advance_token(compiler);
        /* An interface may require an operator method (see
         * compile_definition's identical broadening for class method
         * definitions) -- a class satisfies it the same way it satisfies
         * any other required method name, no interface-side dispatch
         * changes needed beyond accepting the name here. */
        const bool operator_name=
            compiler->current.kind==DIAMOND_TOKEN_PLUS||
            compiler->current.kind==DIAMOND_TOKEN_MINUS||
            compiler->current.kind==DIAMOND_TOKEN_STAR||
            compiler->current.kind==DIAMOND_TOKEN_SLASH||
            compiler->current.kind==DIAMOND_TOKEN_PERCENT||
            compiler->current.kind==DIAMOND_TOKEN_EQUAL_EQUAL||
            compiler->current.kind==DIAMOND_TOKEN_LESS||
            compiler->current.kind==DIAMOND_TOKEN_LESS_EQUAL||
            compiler->current.kind==DIAMOND_TOKEN_GREATER||
            compiler->current.kind==DIAMOND_TOKEN_GREATER_EQUAL;
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER&&!operator_name) {
            fail(compiler,compiler->current.span,"expected interface method name");break;
        }
        const DiamondSpan method_name=compiler->current.span;
        for(size_t index=0;index<interface->method_count;index++)
            if(name_equals(compiler,interface->methods[index].name,method_name,false)) {
                fail(compiler,method_name,"duplicate interface method");break;
            }
        if(compiler->failed)break;
        DiamondInterfaceMethod *method=&interface->methods[interface->method_count++];
        method->return_type_set=DIAMOND_NO_TYPE_SET;
        for(size_t index=0;index<DIAMOND_MAX_DECLARED_PARAMETERS;index++)
            method->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
        if(compiler->current.span.length>=DIAMOND_MAX_FUNCTION_NAME) {
            fail(compiler,compiler->current.span,"interface method name is too long");break;
        }
        for(size_t index=0;index<compiler->current.span.length;index++)
            method->name[index]=compiler->source[compiler->current.span.start+index];
        method->name[compiler->current.span.length]='\0';
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler,compiler->current.span,"expected '(' after interface method");break;
        }
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||method->arity==DIAMOND_MAX_DECLARED_PARAMETERS) {
                fail(compiler,compiler->current.span,"expected interface parameter");break;
            }
            method->arity++;advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                method->parameter_type_sets[method->arity-1]=
                    (uint16_t)parse_type_annotation(compiler);
            }
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,"expected ')' after interface parameters");break;
        }
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_ARROW) {
            advance_token(compiler);
            method->return_type_set=(uint16_t)parse_type_annotation(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_NEWLINE&&
           compiler->current.kind!=DIAMOND_TOKEN_END) {
            fail(compiler,compiler->current.span,"expected newline after method signature");break;
        }
        skip_newlines(compiler);
    }
    if(compiler->current.kind==DIAMOND_TOKEN_END)advance_token(compiler);
    const uint16_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

/* The "store into a target" half of an assignment, shared between the
 * ordinary single-target path (compile_assignment) and multi-value
 * destructuring (compile_multi_assignment) -- everything here is
 * unchanged behavior lifted verbatim out of what used to be
 * compile_assignment's own body. */
uint16_t compile_assignment_store(Compiler *compiler, DiamondSpan name,
        bool instance_variable, bool class_variable, uint16_t value,
        uint16_t rhs_start_register) {
    if (instance_variable) {
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_SET_IVAR_NAME,0,field,value,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_SET_IVAR,0,(uint8_t)field,
                             value,3);
            if(field>=0) {
                DiamondClass *class=
                    compiler->program->classes[(size_t)compiler->current_class];
                const uint16_t known=compiler->known_types[value];
                const bool concrete=known>=DIAMOND_TYPE_CLASS_BASE&&
                    known<DIAMOND_TYPE_VARIABLE_BASE&&
                    (size_t)(known-DIAMOND_TYPE_CLASS_BASE)<
                        compiler->program->class_count;
                record_field_known_type(class,(size_t)field,
                    concrete?(int)(known-DIAMOND_TYPE_CLASS_BASE):-1);
            }
        }
        return value;
    }
    if (class_variable) {
        const int slot = class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_SET_CVAR,
                         (uint16_t)class_variable_owner(compiler,name),(uint8_t)slot,value,3);
        return value;
    }
    if(compiler->source[name.start]>='A'&&compiler->source[name.start]<='Z')
        return store_namespace_constant(compiler,name,value);
    int local = find_local(compiler, name);
    const uint32_t value_alias_identity=
        alias_identity_for_value(compiler,value);
    if(local>=0 && compiler->locals[(size_t)local].captured) {
        /* See parse_identifier's own BOX_LOCAL re-emission for why this
         * defensive re-box is needed: `captured` doesn't imply this
         * control-flow path actually ran the boxing site. */
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,
                         compiler->locals[(size_t)local].reg,0,0,1);
        emit_instruction(compiler,DIAMOND_OP_SET_CELL,
                         compiler->locals[(size_t)local].reg,value,0,2);
        const uint16_t local_register=compiler->locals[(size_t)local].reg;
        compiler->known_types[local_register]=compiler->known_types[value];
        compiler->known_type_sets[local_register]=compiler->known_type_sets[value];
        compiler->tooling_type_sets[local_register]=compiler->tooling_type_sets[value];
        compiler->locals[(size_t)local].alias_identity=value_alias_identity;
        record_scope_type_fact(compiler,local_register,compiler->current.span.start);
        return value;
    }
    if(local<0) {
        for(size_t i=compiler->enclosing_local_count;i>0;i--) {
            if(!spans_equal(compiler,compiler->enclosing_locals[i-1].name,name))continue;
            size_t capture=0;
            while(capture<compiler->capture_count &&
                  compiler->capture_registers[capture]!=compiler->enclosing_locals[i-1].reg)capture++;
            if(capture==compiler->capture_count) {
                if(capture==DIAMOND_MAX_CAPTURES){fail(compiler,name,"too many captured variables");return 0;}
                compiler->capture_registers[compiler->capture_count++]=compiler->enclosing_locals[i-1].reg;
            }
            emit_instruction(compiler,DIAMOND_OP_SET_CAPTURE,(uint8_t)capture,value,0,2);
            return value;
        }
    }
    /* A brand-new local's first assignment (local < 0): if `value` is a
     * genuine, freshly computed temporary rather than a bare reference to
     * an existing register, it can simply BECOME this local's home
     * instead of allocating a second register and MOVE-ing the value
     * into it -- whatever produced it a moment ago already finished, and
     * nothing else needs that specific register for its own sake. Skips
     * both the extra register and the MOVE for every `x = <expression>`
     * that introduces `x`, likely the single most common shape of
     * register waste in ordinary code (see docs/internal/register-
     * recycling-design.md's own motivating numbers). Deliberately scoped
     * to *first* assignment only -- see that design doc for why reusing
     * an *existing* local's register this way is a materially bigger
     * change, not attempted here.
     *
     * Two independent guards, both required:
     * - register_is_named rules out an ordinary named local/parameter
     *   (`x = y`, `x = some_param`) -- reusing one of *those* registers
     *   as `x`'s own home would alias two different variables onto one.
     * - `value >= rhs_start_register` (the register high-water mark from
     *   immediately before this statement's own RHS started evaluating,
     *   threaded in by every caller) additionally catches registers
     *   register_is_named alone can miss: `self` inside an ordinary
     *   method reads as a bare, hardcoded register 0 (parse_prefix's own
     *   DIAMOND_TOKEN_SELF case) with no compiler->locals[] entry at all
     *   for that method, so `x = self` slipped past register_is_named
     *   alone and later crashed once `x` was captured into a block and
     *   boxed -- boxing register 0 in place corrupted `self` for the
     *   rest of the method, surfacing as "undefined method ... for Cell"
     *   at a completely unrelated later `self.foo(...)` call (found via
     *   tests/cases/active_record_batches.d6, an existing, not new,
     *   test). This second guard doesn't need to know about `self`
     *   specifically, or about any other such case -- anything that
     *   existed before this statement's RHS began is, by this compiler's
     *   own strictly-monotonic register allocation, necessarily below
     *   the high-water mark captured at that point, named local or not. */
    /* - A nil literal is excluded: parse_literal emits no instruction for
     *   it (the register is assumed to stay at its zero-initialised nil),
     *   so nothing ever writes that register on a later pass. Adopted as a
     *   local's home, `seen = nil` at the top of a loop body would then
     *   never reset `seen` once the loop assigned it later in the body --
     *   the next iteration would still see the previous one's value. */
    const bool reuse_as_new_local =
        local < 0 && value >= rhs_start_register &&
        compiler->known_types[value] != DIAMOND_TYPE_NIL &&
        !register_is_named(compiler, value);
    const uint16_t destination = reuse_as_new_local
        ? define_local_with_register(compiler, name, value)
        : local < 0 ? define_local(compiler, name)
        : compiler->locals[(size_t)local].reg;
    /* Register recycling Stage 2 (try_rewrite_producer_destination's own
     * comment): reassigning an *existing* local (local >= 0, so `destination`
     * is that local's own already-fixed register, never `value` itself --
     * Stage 1's reuse_as_new_local already covers local < 0 and the two
     * never overlap). `value != destination` is implied by the rewrite
     * helper's own register_is_named check (a bare `x = x` would have
     * `value` equal to `destination`, which is always a named register),
     * kept explicit here anyway as a second, independent reason to refuse
     * rather than relying on that implication alone. */
    const bool rewrote_producer = !reuse_as_new_local && local >= 0 &&
        value != destination &&
        try_rewrite_producer_destination(compiler, value, destination);
    if (!reuse_as_new_local && !rewrote_producer)
        emit_instruction(compiler, DIAMOND_OP_MOVE, destination, value, 0, 2);
    compiler->known_types[destination]=compiler->known_types[value];
    compiler->known_type_sets[destination]=compiler->known_type_sets[value];
    compiler->tooling_type_sets[destination]=compiler->tooling_type_sets[value];
    for(size_t index=compiler->local_count;index>0;index--)
        if(compiler->locals[index-1].reg==destination) {
            compiler->locals[index-1].alias_identity=value_alias_identity;break;
        }
    record_scope_type_fact(compiler,destination,compiler->current.span.start);
    return destination;
}
