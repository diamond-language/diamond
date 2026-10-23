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

/* `case`/`when`/`in` compilation: pattern parsing, destructuring, and exhaustiveness checking. */

static void fail_non_exhaustive_case(Compiler *compiler,
        const CaseExhaustiveness *exhaustiveness) {
    char message[sizeof compiler->diagnostic->message];
    size_t used=(size_t)snprintf(message,sizeof message,
        "case is not exhaustive over its subject's known closed type; missing: ");
    bool first=true;
    for(uint8_t member=0;member<exhaustiveness->required_count;member++) {
        if(exhaustiveness->covered[member])continue;
        const char *name=closed_type_name(compiler,
            exhaustiveness->required_ids[member]);
        if(used<sizeof message)
            used+=(size_t)snprintf(message+used,sizeof message-used,
                "%s%s",first?"":", ",name);
        first=false;
    }
    if(used<sizeof message)
        snprintf(message+used,sizeof message-used,
            "; add the missing branch or an 'else'");
    fail(compiler,exhaustiveness->case_span,message);
}

static bool span_is_underscore(const Compiler *compiler,DiamondSpan span) {
    return span.length==1&&compiler->source[span.start]=='_';
}

static const DiamondMethod *case_pattern_reader(const Compiler *compiler,
        uint16_t class_index,DiamondSpan name) {
    const DiamondClass *class=compiler->program->classes[class_index];
    while(class!=nullptr) {
        for(size_t method=class->method_count;method>0;method--) {
            const DiamondMethod *candidate=&class->methods[method-1];
            if(name_equals(compiler,candidate->name,name,false))return candidate;
        }
        class=class->superclass==DIAMOND_NO_CLASS?nullptr:
            compiler->program->classes[class->superclass];
    }
    return nullptr;
}

static int probe_case_pattern_class(Compiler *compiler,
        DiamondTokenKind *after_kind) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER)return -1;
    char name[DIAMOND_MAX_FUNCTION_NAME]={};
    Compiler probe=*compiler;
    if(!consume_qualified_name(&probe,name,sizeof name))return -1;
    if(after_kind!=nullptr)*after_kind=probe.current.kind;
    if(strstr(name,"::")==nullptr&&
       (find_local(compiler,compiler->current.span)>=0||
        find_function(compiler,compiler->current.span)>=0))return -1;
    return find_class_qualified_or_scoped(compiler,name);
}

static uint16_t compile_case_pattern_class(Compiler *compiler,int class_index) {
    char name[DIAMOND_MAX_FUNCTION_NAME]={};
    const uint16_t result=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
    emit_register(compiler,result);emit_register(compiler,(uint16_t)class_index);
    (void)consume_qualified_name(compiler,name,sizeof name);
    return result;
}

static uint8_t parse_case_array_node(Compiler *compiler,CaseArrayNode *nodes,
        size_t *node_count,size_t depth) {
    if(*node_count==64||depth>8) {
        fail(compiler,compiler->current.span,"case Array pattern is too complex");
        return 0;
    }
    const uint8_t index=(uint8_t)(*node_count);
    CaseArrayNode *node=&nodes[(*node_count)++];*node=(CaseArrayNode){};
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        node->kind=CASE_ARRAY_GROUP;advance_token(compiler);
        bool rest_seen=false;
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&!compiler->failed) {
            if(node->child_count==16) {
                fail(compiler,compiler->current.span,"too many case Array elements");return index;
            }
            node->children[node->child_count++]=
                parse_case_array_node(compiler,nodes,node_count,depth+1);
            const CaseArrayNodeKind child_kind=
                nodes[node->children[node->child_count-1]].kind;
            if(child_kind==CASE_ARRAY_REST_BIND||
               child_kind==CASE_ARRAY_REST_WILDCARD) {
                if(rest_seen) {
                    fail(compiler,nodes[node->children[node->child_count-1]].name,
                        "case Array pattern may contain only one rest binding");
                    return index;
                }
                rest_seen=true;
            }
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET)break;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                fail(compiler,compiler->current.span,"expected ',' in case Array pattern");
                return index;
            }
            advance_token(compiler);
        }
        if(node->child_count==0) {
            fail(compiler,compiler->current.span,"case Array pattern cannot be empty");
            return index;
        }
        advance_token(compiler);return index;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        node->kind=CASE_HASH_GROUP;advance_token(compiler);skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE&&!compiler->failed) {
            if(node->child_count==16) {
                fail(compiler,compiler->current.span,"too many case Hash entries");
                return index;
            }
            DiamondLexer rest_lookahead=compiler->lexer;
            const DiamondToken second_star=diamond_lexer_next(&rest_lookahead);
            if(compiler->current.kind==DIAMOND_TOKEN_STAR&&
               second_star.kind==DIAMOND_TOKEN_STAR) {
                advance_token(compiler);advance_token(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                    fail(compiler,compiler->current.span,
                        "expected lowercase binding or '_' after '**' in case Hash pattern");
                    return index;
                }
                const uint8_t child=(uint8_t)(*node_count);
                if(*node_count==64) {
                    fail(compiler,compiler->current.span,
                        "case Hash pattern is too complex");return index;
                }
                CaseArrayNode *rest=&nodes[(*node_count)++];
                *rest=(CaseArrayNode){.name=compiler->current.span};
                if(span_is_underscore(compiler,rest->name))
                    rest->kind=CASE_HASH_REST_WILDCARD;
                else {
                    const char first=compiler->source[rest->name.start];
                    if(first<'a'||first>'z') {
                        fail(compiler,rest->name,
                            "rest binding in case Hash pattern must be lowercase");
                        return index;
                    }
                    rest->kind=CASE_HASH_REST_BIND;
                }
                node->children[node->child_count++]=child;
                advance_token(compiler);skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
                    fail(compiler,compiler->current.span,
                        "rest binding must be last in case Hash pattern");
                    return index;
                }
                break;
            }
            const uint16_t key=parse_expression(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                fail(compiler,compiler->current.span,
                    "expected ':' after case Hash pattern key");return index;
            }
            advance_token(compiler);
            const uint8_t child=parse_case_array_node(
                compiler,nodes,node_count,depth+1);
            if(nodes[child].kind==CASE_ARRAY_REST_BIND||
               nodes[child].kind==CASE_ARRAY_REST_WILDCARD||
               nodes[child].kind==CASE_HASH_REST_BIND||
               nodes[child].kind==CASE_HASH_REST_WILDCARD) {
                fail(compiler,nodes[child].name,
                    "rest bindings are not supported in case Hash patterns");
                return index;
            }
            nodes[child].key_register=key;
            node->children[node->child_count++]=child;
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE)break;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                fail(compiler,compiler->current.span,
                    "expected ',' in case Hash pattern");return index;
            }
            advance_token(compiler);skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
            fail(compiler,compiler->current.span,"expected '}' after case Hash pattern");
            return index;
        }
        advance_token(compiler);return index;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER) {
        char object_name[DIAMOND_MAX_FUNCTION_NAME]={};
        Compiler object_probe=*compiler;
        (void)consume_qualified_name(&object_probe,object_name,
                                     sizeof object_name);
        const int object_class=find_class_qualified_or_scoped(
            compiler,object_name);
        if(object_class>=0&&
           object_probe.current.kind==DIAMOND_TOKEN_LEFT_BRACE) {
            node->kind=CASE_OBJECT_GROUP;
            node->class_index=(uint16_t)object_class;
            node->value_register=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
            emit_register(compiler,node->value_register);
            emit_register(compiler,(uint16_t)object_class);
            (void)consume_qualified_name(compiler,object_name,
                                         sizeof object_name);
            advance_token(compiler);skip_newlines(compiler);
            while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE&&
                  !compiler->failed) {
                if(node->child_count==16) {
                    fail(compiler,compiler->current.span,
                        "too many case object fields");return index;
                }
                if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
                   compiler->source[compiler->current.span.start]<'a'||
                   compiler->source[compiler->current.span.start]>'z') {
                    fail(compiler,compiler->current.span,
                        "expected lowercase reader in case object pattern");
                    return index;
                }
                const DiamondSpan reader_name=compiler->current.span;
                const DiamondMethod *reader=case_pattern_reader(
                    compiler,(uint16_t)object_class,reader_name);
                if(reader==nullptr||reader->is_private||reader->required_arity>0) {
                    fail(compiler,reader_name,
                        "case object pattern requires a public zero-argument reader");
                    return index;
                }
                advance_token(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                    fail(compiler,compiler->current.span,
                        "expected ':' after case object reader");return index;
                }
                advance_token(compiler);
                const uint8_t child=parse_case_array_node(
                    compiler,nodes,node_count,depth+1);
                if(nodes[child].kind==CASE_ARRAY_REST_BIND||
                   nodes[child].kind==CASE_ARRAY_REST_WILDCARD) {
                    fail(compiler,nodes[child].name,
                        "rest bindings are not supported in case object patterns");
                    return index;
                }
                nodes[child].member_name=reader_name;
                node->children[node->child_count++]=child;
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE)break;
                if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                    fail(compiler,compiler->current.span,
                        "expected ',' in case object pattern");return index;
                }
                advance_token(compiler);skip_newlines(compiler);
            }
            if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
                fail(compiler,compiler->current.span,
                    "expected '}' after case object pattern");return index;
            }
            advance_token(compiler);return index;
        }
    }
    if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,
                "expected lowercase binding or '_' after '*' in case Array pattern");
            return index;
        }
        node->name=compiler->current.span;
        if(span_is_underscore(compiler,node->name))
            node->kind=CASE_ARRAY_REST_WILDCARD;
        else {
            const char first=compiler->source[node->name.start];
            if(first<'a'||first>'z') {
                fail(compiler,node->name,
                    "rest binding in case Array pattern must be lowercase");
                return index;
            }
            node->kind=CASE_ARRAY_REST_BIND;
        }
        advance_token(compiler);return index;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_CARET) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,
                "expected lowercase local after '^' in case Array pattern");
            return index;
        }
        node->name=compiler->current.span;
        const char first=compiler->source[node->name.start];
        if(first<'a'||first>'z'||span_is_underscore(compiler,node->name)) {
            fail(compiler,node->name,
                "pin in case Array pattern must name a lowercase local");
            return index;
        }
        bool found=find_local(compiler,node->name)>=0;
        for(size_t local=compiler->enclosing_local_count;local>0&&!found;local--)
            found=spans_equal(compiler,
                compiler->enclosing_locals[local-1].name,node->name);
        if(!found) {
            fail(compiler,node->name,
                "pin references undefined local variable");return index;
        }
        node->kind=CASE_ARRAY_PIN;
        advance_token(compiler);
        node->value_register=parse_identifier(compiler);
        return index;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER) {
        const char first=compiler->source[compiler->current.span.start];
        if(span_is_underscore(compiler,compiler->current.span)) {
            node->kind=CASE_ARRAY_WILDCARD;advance_token(compiler);return index;
        }
        if(first>='a'&&first<='z') {
            node->kind=CASE_ARRAY_BIND;node->name=compiler->current.span;
            advance_token(compiler);return index;
        }
    }
    node->kind=CASE_ARRAY_VALUE;
    DiamondTokenKind after=DIAMOND_TOKEN_ERROR;
    const int pattern_class=probe_case_pattern_class(compiler,&after);
    if(pattern_class>=0&&after!=DIAMOND_TOKEN_DOT&&
       after!=DIAMOND_TOKEN_LEFT_BRACE)
        node->value_register=compile_case_pattern_class(compiler,pattern_class);
    else node->value_register=parse_expression(compiler);
    return index;
}

static void emit_case_array_match(Compiler *compiler,CaseArrayNode *nodes,
        uint8_t node_index,uint16_t subject,uint16_t match_reg,
        size_t *failure_jumps,size_t *failure_count) {
    CaseArrayNode *node=&nodes[node_index];node->subject_register=subject;
    if(node->kind==CASE_ARRAY_BIND||node->kind==CASE_ARRAY_WILDCARD||
       node->kind==CASE_ARRAY_REST_BIND||
       node->kind==CASE_ARRAY_REST_WILDCARD||
       node->kind==CASE_HASH_REST_BIND||
       node->kind==CASE_HASH_REST_WILDCARD)return;
    const uint16_t test=allocate_register(compiler);
    if(node->kind==CASE_ARRAY_VALUE||node->kind==CASE_ARRAY_PIN||
       node->kind==CASE_OBJECT_GROUP) {
        emit_instruction(compiler,DIAMOND_OP_CASE_MATCH,test,node->value_register,subject,3);
    } else if(node->kind==CASE_ARRAY_GROUP) {
        bool has_rest=false;
        for(size_t child=0;child<node->child_count;child++)
            if(nodes[node->children[child]].kind==CASE_ARRAY_REST_BIND||
               nodes[node->children[child]].kind==CASE_ARRAY_REST_WILDCARD)
                has_rest=true;
        const uint16_t fixed_count=(uint16_t)(node->child_count-(has_rest?1u:0u));
        emit_instruction(compiler,DIAMOND_OP_CASE_ARRAY_SHAPE,test,subject,
            fixed_count|(has_rest?0x8000u:0u),3);
    } else {
        emit_instruction(compiler,DIAMOND_OP_CASE_HASH_SHAPE,test,subject,0,2);
    }
    compiler->known_types[test]=DIAMOND_TYPE_BOOL;
    emit_instruction(compiler,DIAMOND_OP_MOVE,match_reg,test,0,2);
    failure_jumps[(*failure_count)++]=
        emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,match_reg);
    if(node->kind!=CASE_ARRAY_GROUP&&node->kind!=CASE_HASH_GROUP&&
       node->kind!=CASE_OBJECT_GROUP)return;
    size_t array_rest_index=SIZE_MAX;
    if(node->kind==CASE_ARRAY_GROUP)
        for(size_t child=0;child<node->child_count;child++)
            if(nodes[node->children[child]].kind==CASE_ARRAY_REST_BIND||
               nodes[node->children[child]].kind==CASE_ARRAY_REST_WILDCARD)
                array_rest_index=child;
    for(size_t child=0;child<node->child_count;child++) {
        CaseArrayNode *child_node=&nodes[node->children[child]];
        if(child_node->kind==CASE_HASH_REST_BIND||
           child_node->kind==CASE_HASH_REST_WILDCARD) {
            if(child_node->kind==CASE_HASH_REST_BIND) {
                const uint16_t key_base=allocate_register(compiler);
                for(size_t key=1;key<child;key++)(void)allocate_register(compiler);
                for(size_t key=0;key<child;key++)
                    emit_instruction(compiler,DIAMOND_OP_MOVE,
                        (uint16_t)(key_base+key),
                        nodes[node->children[key]].key_register,0,2);
                const uint16_t excluded=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARRAY,excluded,key_base,
                    (uint16_t)child,3);
                compiler->known_types[excluded]=DIAMOND_TYPE_ARRAY;
                const uint16_t rest=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_HASH_REST,rest,subject,
                    excluded,3);
                compiler->known_types[rest]=DIAMOND_TYPE_HASH;
                child_node->subject_register=rest;
            }
            continue;
        }
        if(child_node->kind==CASE_ARRAY_REST_BIND||
           child_node->kind==CASE_ARRAY_REST_WILDCARD) {
            if(child_node->kind==CASE_ARRAY_REST_BIND) {
                const uint16_t rest=allocate_register(compiler);
                const uint16_t suffix=(uint16_t)(node->child_count-child-1);
                emit_instruction(compiler,DIAMOND_OP_ARRAY_MIDDLE,rest,subject,
                    (uint16_t)((child<<8)|suffix),3);
                compiler->known_types[rest]=DIAMOND_TYPE_ARRAY;
                child_node->subject_register=rest;
            }
            continue;
        }
        if(node->kind==CASE_OBJECT_GROUP) {
            const uint16_t element=emit_invoke_call(compiler,subject,
                child_node->member_name,false,nullptr,0,nullptr,0);
            emit_case_array_match(compiler,nodes,node->children[child],element,
                match_reg,failure_jumps,failure_count);continue;
        }
        if(node->kind==CASE_ARRAY_GROUP&&array_rest_index!=SIZE_MAX&&
           child>array_rest_index) {
            const uint16_t element=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ARRAY_SUFFIX,element,subject,
                (uint16_t)(node->child_count-child),3);
            emit_case_array_match(compiler,nodes,node->children[child],element,
                match_reg,failure_jumps,failure_count);continue;
        }
        uint16_t index_reg=child_node->key_register;
        if(node->kind==CASE_ARRAY_GROUP) {
            const uint16_t constant=add_constant(compiler,DIAMOND_INT((int64_t)child));
            index_reg=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_CONSTANT,index_reg,constant,0,2);
        } else {
            const uint16_t present=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_CASE_HASH_HAS,present,subject,
                index_reg,3);
            compiler->known_types[present]=DIAMOND_TYPE_BOOL;
            emit_instruction(compiler,DIAMOND_OP_MOVE,match_reg,present,0,2);
            failure_jumps[(*failure_count)++]=
                emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,match_reg);
        }
        const uint16_t element=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,element,subject,index_reg,3);
        emit_case_array_match(compiler,nodes,node->children[child],element,
            match_reg,failure_jumps,failure_count);
    }
}

static bool validate_case_array_bindings(Compiler *compiler,
        const CaseArrayNode *nodes,size_t node_count) {
    for(size_t left=0;left<node_count;left++) {
        if(nodes[left].kind!=CASE_ARRAY_BIND&&
           nodes[left].kind!=CASE_ARRAY_REST_BIND&&
           nodes[left].kind!=CASE_HASH_REST_BIND)continue;
        for(size_t right=left+1;right<node_count;right++) {
            if((nodes[right].kind==CASE_ARRAY_BIND||
                nodes[right].kind==CASE_ARRAY_REST_BIND||
                nodes[right].kind==CASE_HASH_REST_BIND)&&
               spans_equal(compiler,nodes[left].name,nodes[right].name)) {
                fail(compiler,nodes[right].name,"duplicate binding in case collection pattern");
                return false;
            }
        }
    }
    return true;
}

static size_t collect_case_bindings(const CaseArrayNode *nodes,size_t node_count,
        size_t *indices) {
    size_t count=0;
    for(size_t node=0;node<node_count;node++)
        if(nodes[node].kind==CASE_ARRAY_BIND||
           nodes[node].kind==CASE_ARRAY_REST_BIND||
           nodes[node].kind==CASE_HASH_REST_BIND)indices[count++]=node;
    return count;
}

static bool align_case_alternative_bindings(Compiler *compiler,
        const CaseArrayNode *nodes,const size_t *indices,size_t count,
        CaseBinding *bindings,size_t binding_count,size_t *aligned) {
    if(count!=binding_count) {
        fail(compiler,compiler->previous.span,
            "case pattern alternatives must bind the same names");return false;
    }
    for(size_t binding=0;binding<binding_count;binding++) {
        bool found=false;
        for(size_t candidate=0;candidate<count;candidate++)
            if(spans_equal(compiler,bindings[binding].name,
                           nodes[indices[candidate]].name)) {
                aligned[binding]=indices[candidate];found=true;break;
            }
        if(!found) {
            fail(compiler,compiler->previous.span,
                "case pattern alternatives must bind the same names");
            return false;
        }
    }
    return true;
}

/* Makes provisional pattern bindings visible while compiling a guard without
 * emitting any stores. Their registers already hold extracted values, so a
 * guard reads them directly; restoring local_count removes the temporary name
 * overlay before the successful branch performs the real atomic commit. */
static size_t push_case_guard_bindings(Compiler *compiler,
        const CaseBinding *bindings,size_t binding_count) {
    const size_t saved=compiler->local_count;
    for(size_t binding=0;binding<binding_count;binding++) {
        if(compiler->local_count==DIAMOND_MAX_LOCALS) {
            fail(compiler,bindings[binding].name,
                "too many local variables in pattern guard");
            break;
        }
        compiler->locals[compiler->local_count++]=(Local){
            .name=bindings[binding].name,.reg=bindings[binding].reg};
    }
    return saved;
}

static void commit_case_bindings(Compiler *compiler,
        const CaseBinding *bindings,size_t binding_count) {
    for(size_t binding=0;binding<binding_count;binding++)
        /* rhs_start_register=compiler->next_register (the current, not an
         * earlier, high-water mark): bindings[binding].reg was resolved
         * well before this point (pattern matching, not a fresh RHS
         * expression), so this unconditionally disables compile_
         * assignment_store's own register-reuse optimization here,
         * exactly as intended -- see that function's own comment. */
        (void)compile_assignment_store(compiler,bindings[binding].name,
            false,false,bindings[binding].reg,compiler->next_register);
}

static void merge_case_new_locals(Compiler *compiler,CaseFlowJoin *join,
        size_t flow_local_count) {
    /* join->initialized read before this call's own merge_case_branch
     * caller flips it (the !initialized branch there calls this first,
     * then sets it true) -- true here means "an earlier when/else clause
     * already ran in this case statement", which is exactly when a slot
     * reached for the first time needs an implicit Nil folded in for that
     * earlier clause. See merge_new_local_facts for the shared logic. */
    merge_new_local_facts(compiler,join->initialized,flow_local_count,
        &join->new_local_count,join->new_types,join->new_sets,join->new_alias);
}

static void merge_case_branch(Compiler *compiler,CaseFlowJoin *join,
        size_t flow_reg_count,size_t flow_local_count,uint16_t branch_result) {
    const uint16_t branch_result_type=compiler->known_types[branch_result];
    const int32_t branch_result_set=compiler->known_type_sets[branch_result];
    const int32_t branch_result_tooling=compiler->tooling_type_sets[branch_result];
    if(!join->initialized) {
        for(size_t index=0;index<flow_reg_count;index++) {
            join->types[index]=compiler->known_types[index];
            join->sets[index]=compiler->known_type_sets[index];
            join->tooling_sets[index]=compiler->tooling_type_sets[index];
        }
        for(size_t index=0;index<flow_local_count;index++)
            join->alias[index]=compiler->locals[index].alias_identity;
        merge_case_new_locals(compiler,join,flow_local_count);
        join->result_type=branch_result_type;join->result_set=branch_result_set;
        join->result_tooling_set=branch_result_tooling;
        join->initialized=true;return;
    }
    for(size_t index=0;index<flow_reg_count;index++) {
        if(join->types[index]!=compiler->known_types[index]||
           join->sets[index]!=compiler->known_type_sets[index])join->varied[index]=true;
        merge_flow_types(compiler,join->types[index],join->sets[index],
            compiler->known_types[index],compiler->known_type_sets[index],
            &join->types[index],&join->sets[index]);
        join->tooling_sets[index]=merge_tooling_type_sets(compiler,
            join->tooling_sets[index],compiler->tooling_type_sets[index]);
    }
    for(size_t index=0;index<flow_local_count;index++)
        join->alias[index]=merge_alias_identity(join->alias[index],
            compiler->locals[index].alias_identity);
    merge_case_new_locals(compiler,join,flow_local_count);
    merge_flow_types(compiler,join->result_type,join->result_set,
        branch_result_type,branch_result_set,&join->result_type,&join->result_set);
    join->result_tooling_set=merge_tooling_type_sets(compiler,
        join->result_tooling_set,branch_result_tooling);
}

static void finish_case_flow(Compiler *compiler,CaseFlowJoin *join,
        size_t flow_reg_count,size_t flow_local_count,uint16_t destination,
        size_t effective_start) {
    for(size_t index=0;index<flow_reg_count;index++) {
        compiler->known_types[index]=join->types[index];
        compiler->known_type_sets[index]=join->sets[index];
        compiler->tooling_type_sets[index]=join->tooling_sets[index];
        if(register_is_local(compiler,(uint16_t)index))
            record_scope_type_fact(compiler,(uint16_t)index,effective_start);
    }
    for(size_t index=0;index<flow_local_count;index++)
        compiler->locals[index].alias_identity=join->alias[index];
    const size_t final_local_count=compiler->local_count;
    for(size_t index=flow_local_count;
        index<final_local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const size_t slot=index-flow_local_count;
        const uint16_t reg=compiler->locals[index].reg;
        compiler->known_types[reg]=join->new_types[slot];
        compiler->known_type_sets[reg]=join->new_sets[slot];
        compiler->tooling_type_sets[reg]=-1;
        compiler->locals[index].alias_identity=join->new_alias[slot];
        record_scope_type_fact(compiler,reg,effective_start);
    }
    compiler->known_types[destination]=join->result_type;
    compiler->known_type_sets[destination]=join->result_set;
    compiler->tooling_type_sets[destination]=join->result_tooling_set;
}

/* Compiles one `when`/`else`/`end` branch of a case expression and
 * everything after it, recursively -- same shape as parse_if's own
 * elsif recursion, and for the same reason: each level's "jump past the
 * rest of the chain" target only becomes known once the recursive call
 * has parsed all the way through to the final `end`, at which point
 * compiler->function->code_count is already sitting at the true end of
 * the whole chain, so every level's jump converges on that same address
 * without this function needing to collect and patch a jump list itself.
 *
 * `entry_types`/`entry_sets`/`entry_tooling` (snapshots of the three
 * corresponding compiler fact arrays taken once, in parse_case, before the first branch)
 * gets restored at the top of every call -- a when-clause's values and
 * body are compiled as though no earlier when-clause's (also-compiled,
 * possibly-speculative) body actually ran, mirroring why parse_if resets
 * to its own before_types before compiling the else branch. `join` then
 * accumulates every branch's result and local state into the same advisory
 * unions parse_if uses; a missing else contributes the entry state and Nil. */
uint16_t parse_case_branches(Compiler *compiler, uint16_t subject,
        size_t flow_reg_count, const uint16_t *entry_types,
        const int32_t *entry_sets,const int32_t *entry_tooling,
        size_t flow_local_count,
        const uint32_t *entry_alias, uint16_t destination,CaseFlowJoin *join,
        bool subjectless,CaseExhaustiveness *exhaustiveness) {
    for(size_t index=0;index<flow_reg_count;index++) {
        compiler->known_types[index]=entry_types[index];
        compiler->known_type_sets[index]=entry_sets[index];
        compiler->tooling_type_sets[index]=entry_tooling[index];
    }
    for(size_t index=0;index<flow_local_count;index++)
        compiler->locals[index].alias_identity=entry_alias[index];
    /* A local bound by an earlier sibling `when`/`else` clause in this same
     * chain (no binding before the `case` at all) is reset to Nil before
     * this clause compiles -- it's a different, mutually exclusive branch,
     * so that local was never actually reached down this path, matching
     * the VM's own nil-reads-back-on-the-untaken-path behavior. Bounded by
     * the *current* compiler->local_count, which only grows as earlier
     * clauses introduce more such locals -- see merge_case_new_locals for
     * the matching accumulation on the way out. */
    for(size_t index=flow_local_count;
        index<compiler->local_count&&index<DIAMOND_MAX_LOCALS;index++) {
        const uint16_t reg=compiler->locals[index].reg;
        compiler->known_types[reg]=DIAMOND_TYPE_NIL;
        compiler->known_type_sets[reg]=-1;
        compiler->tooling_type_sets[reg]=-1;
        compiler->locals[index].alias_identity=0;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE)skip_newlines(compiler);
        const uint16_t body_result=compile_sequence(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,body_result,0,2);
        merge_case_branch(compiler,join,flow_reg_count,flow_local_count,body_result);
        if(compiler->current.kind!=DIAMOND_TOKEN_END) {
            fail(compiler,compiler->current.span,
                 "expected 'end' after case expression");
            return destination;
        }
        const size_t join_offset=compiler->current.span.start;
        advance_token(compiler);
        finish_case_flow(compiler,join,flow_reg_count,flow_local_count,
            destination,join_offset);
        return destination;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_END) {
        if(exhaustiveness->active) {
            bool all_covered=true;
            for(uint8_t member=0;member<exhaustiveness->required_count;member++)
                if(!exhaustiveness->covered[member]){all_covered=false;break;}
            if(!all_covered) {
                fail_non_exhaustive_case(compiler,exhaustiveness);
                return destination;
            }
        }
        emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
        compiler->known_types[destination]=DIAMOND_TYPE_NIL;
        compiler->known_type_sets[destination]=-1;
        merge_case_branch(compiler,join,flow_reg_count,flow_local_count,destination);
        const size_t join_offset=compiler->current.span.start;
        advance_token(compiler);
        finish_case_flow(compiler,join,flow_reg_count,flow_local_count,
            destination,join_offset);
        return destination;
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_WHEN) {
        fail(compiler,compiler->current.span,
             "expected 'when', 'else', or 'end' in case expression");
        return destination;
    }
    advance_token(compiler);
    /* One `when` clause can list several comma-separated values (`when
     * 1, 2`) -- match_reg accumulates whether *any* of them matches the
     * subject, short-circuiting like `||` does (skip evaluating/
     * comparing a later value once an earlier one already matched)
     * rather than always evaluating every value in the list. CASE_MATCH
     * implements Range inclusion, Regexp search, class/subclass matching,
     * and ordinary/custom equality fallback in one runtime operation. */
    const uint16_t match_reg=allocate_register(compiler);
    /* Type ids this `when` clause covers unconditionally, for
     * exhaustiveness -- only ever populated by the scalar (non-array-
     * pattern) branch below, and only kept if this clause turns out to
     * have no `if` guard (see the guard-handling block further down). */
    uint16_t covered_ids[DIAMOND_MAX_UNION_TYPES]={0};size_t covered_id_count=0;
    CaseArrayNode array_nodes[64]={};uint8_t array_root=0;size_t node_count=0;
    CaseBinding bindings[64]={};size_t binding_count=0;
    DiamondTokenKind after_pattern_head=DIAMOND_TOKEN_ERROR;
    const int pattern_head_class=
        probe_case_pattern_class(compiler,&after_pattern_head);
    const bool object_pattern=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        pattern_head_class>=0&&after_pattern_head==DIAMOND_TOKEN_LEFT_BRACE;
    const bool array_pattern=!subjectless&&(
        compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
        compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE||object_pattern);
    if(array_pattern) {
        size_t success_jumps[16];size_t success_count=0;bool first_pattern=true;
        for(;;) {
            if(success_count==16) {
                fail(compiler,compiler->current.span,
                    "too many case pattern alternatives");return destination;
            }
            memset(array_nodes,0,sizeof array_nodes);node_count=0;
            array_root=parse_case_array_node(compiler,array_nodes,&node_count,0);
            if(compiler->failed||!validate_case_array_bindings(
                    compiler,array_nodes,node_count))return destination;
            /* Exhaustiveness, per alternative: an object pattern whose
             * readers are all bare bindings or `_` (including an empty
             * `Class{}`) matches every instance of that class exactly as
             * a bare scalar `when Class` does, so it soundly covers that
             * union/sealed-hierarchy member -- and when several such
             * alternatives are comma-joined, each one covers its own
             * class, since any of them matching selects this same
             * branch. A literal, pin, or nested pattern among the
             * readers only matches a subset of the class and never
             * counts; neither does an Array/Hash pattern (it names no
             * class). A trailing `if` guard zeroes covered_id_count
             * further down regardless, so a guarded clause never counts. */
            {
                bool unconstrained_object=
                    array_nodes[array_root].kind==CASE_OBJECT_GROUP;
                for(uint8_t child=0;unconstrained_object&&
                    child<array_nodes[array_root].child_count;child++) {
                    const CaseArrayNodeKind kind=
                        array_nodes[array_nodes[array_root].children[child]].kind;
                    if(kind!=CASE_ARRAY_BIND&&kind!=CASE_ARRAY_WILDCARD)
                        unconstrained_object=false;
                }
                if(unconstrained_object&&covered_id_count<DIAMOND_MAX_UNION_TYPES)
                    covered_ids[covered_id_count++]=(uint16_t)
                        (DIAMOND_TYPE_CLASS_BASE+array_nodes[array_root].class_index);
            }
            size_t indices[64],aligned[64];
            const size_t alternative_binding_count=
                collect_case_bindings(array_nodes,node_count,indices);
            if(first_pattern) {
                binding_count=alternative_binding_count;
                for(size_t binding=0;binding<binding_count;binding++) {
                    bindings[binding].name=array_nodes[indices[binding]].name;
                    bindings[binding].reg=allocate_register(compiler);
                    aligned[binding]=indices[binding];
                }
            } else if(!align_case_alternative_bindings(compiler,array_nodes,
                    indices,alternative_binding_count,bindings,binding_count,
                    aligned))return destination;
            emit_instruction(compiler,DIAMOND_OP_BOOL,match_reg,true,0,2);
            compiler->known_types[match_reg]=DIAMOND_TYPE_BOOL;
            size_t failure_jumps[128];size_t failure_count=0;
            emit_case_array_match(compiler,array_nodes,array_root,subject,match_reg,
                failure_jumps,&failure_count);
            /* Each binding's type is whatever the matcher proved about the
             * element it binds. This must come after emit_case_array_match:
             * that's what assigns each node's subject_register (before it,
             * every one is still 0, which would hand bindings the facts of
             * whatever lives in register 0). A later alternative that binds
             * a differently typed element makes the binding untyped. */
            for(size_t binding=0;binding<binding_count;binding++) {
                const uint16_t source=array_nodes[aligned[binding]].subject_register;
                if(first_pattern) {
                    compiler->known_types[bindings[binding].reg]=compiler->known_types[source];
                    compiler->known_type_sets[bindings[binding].reg]=
                        compiler->known_type_sets[source];
                } else if(compiler->known_types[bindings[binding].reg]!=
                              compiler->known_types[source]||
                          compiler->known_type_sets[bindings[binding].reg]!=
                              compiler->known_type_sets[source]) {
                    compiler->known_types[bindings[binding].reg]=TYPE_UNKNOWN;
                    compiler->known_type_sets[bindings[binding].reg]=-1;
                }
            }
            for(size_t binding=0;binding<binding_count;binding++)
                emit_instruction(compiler,DIAMOND_OP_MOVE,bindings[binding].reg,
                    array_nodes[aligned[binding]].subject_register,0,2);
            success_jumps[success_count++]=emit_jump(compiler,DIAMOND_OP_JUMP,0);
            for(size_t failure=0;failure<failure_count;failure++)
                patch_jump(compiler,failure_jumps[failure],
                    compiler->function->code_count);
            first_pattern=false;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        }
        for(size_t success=0;success<success_count;success++)
            patch_jump(compiler,success_jumps[success],compiler->function->code_count);
    } else {
        bool first_value=true;size_t skip_jump=0;
        for(;;) {
            if(!first_value)
                skip_jump=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,match_reg);
            uint16_t value_reg;
            DiamondTokenKind after_pattern=DIAMOND_TOKEN_ERROR;
            const int pattern_class=
                probe_case_pattern_class(compiler,&after_pattern);
            if(pattern_class>=0&&after_pattern!=DIAMOND_TOKEN_DOT&&
               after_pattern!=DIAMOND_TOKEN_LEFT_BRACE) {
                value_reg=compile_case_pattern_class(compiler,pattern_class);
                if(covered_id_count<DIAMOND_MAX_UNION_TYPES)
                    covered_ids[covered_id_count++]=
                        (uint16_t)(DIAMOND_TYPE_CLASS_BASE+pattern_class);
            } else {
                value_reg=parse_expression(compiler);
                if(compiler->known_types[value_reg]==DIAMOND_TYPE_NIL&&
                   compiler->known_type_sets[value_reg]<0&&
                   covered_id_count<DIAMOND_MAX_UNION_TYPES)
                    covered_ids[covered_id_count++]=DIAMOND_TYPE_NIL;
            }
            const uint16_t eq_reg=allocate_register(compiler);
            if(subjectless) {
                emit_instruction(compiler,DIAMOND_OP_NOT,eq_reg,value_reg,0,2);
                emit_instruction(compiler,DIAMOND_OP_NOT,eq_reg,eq_reg,0,2);
            } else
                emit_instruction(compiler,DIAMOND_OP_CASE_MATCH,eq_reg,
                    value_reg,subject,3);
            compiler->known_types[eq_reg]=DIAMOND_TYPE_BOOL;
            emit_instruction(compiler,DIAMOND_OP_MOVE,match_reg,eq_reg,0,2);
            if(!first_value)patch_jump(compiler,skip_jump,compiler->function->code_count);
            first_value=false;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        }
    }
    if(compiler->current.kind==DIAMOND_TOKEN_IF) {
        const size_t skip_guard=
            emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,match_reg);
        advance_token(compiler);
        const size_t saved_local_count=array_pattern?
            push_case_guard_bindings(compiler,bindings,binding_count):
            compiler->local_count;
        const uint16_t guard=parse_expression(compiler);
        compiler->local_count=saved_local_count;
        compiler->narrowing=(Narrowing){};
        emit_instruction(compiler,DIAMOND_OP_NOT,match_reg,guard,0,2);
        emit_instruction(compiler,DIAMOND_OP_NOT,match_reg,match_reg,0,2);
        patch_jump(compiler,skip_guard,compiler->function->code_count);
        /* A guard can reject an otherwise-matching pattern at runtime, so
         * a guarded `when` never proves a type is unconditionally
         * covered -- see CaseExhaustiveness's own comment. */
        covered_id_count=0;
    }
    /* A `when` naming a class covers a required member that is that class
     * or any descendant of it: the pattern matches subclass instances at
     * runtime, so `when Shape` leaves nothing of `Circle < Shape` unmatched. */
    if(exhaustiveness->active)
        for(size_t covered_index=0;covered_index<covered_id_count;covered_index++)
            for(uint8_t member=0;member<exhaustiveness->required_count;member++) {
                size_t walk=exhaustiveness->required_ids[member];
                if(walk==covered_ids[covered_index])
                    exhaustiveness->covered[member]=true;
                for(size_t hops=0;hops<=DIAMOND_MAX_CLASSES&&
                        !exhaustiveness->covered[member]&&
                        walk>=DIAMOND_TYPE_CLASS_BASE&&
                        walk<DIAMOND_TYPE_CLASS_BASE+DIAMOND_MAX_CLASSES;hops++) {
                    if(walk==covered_ids[covered_index]) {
                        exhaustiveness->covered[member]=true;
                        break;
                    }
                    const uint16_t parent=
                        compiler->program->classes[walk-DIAMOND_TYPE_CLASS_BASE]->superclass;
                    if(parent==DIAMOND_NO_CLASS)break;
                    walk=(size_t)DIAMOND_TYPE_CLASS_BASE+parent;
                }
            }
    if(!consume_conditional_start(compiler))return destination;
    const size_t false_jump=
        emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,match_reg);
    if(array_pattern)commit_case_bindings(compiler,bindings,binding_count);
    const uint16_t body_result=compile_sequence(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,body_result,0,2);
    merge_case_branch(compiler,join,flow_reg_count,flow_local_count,body_result);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,false_jump,compiler->function->code_count);
    const uint16_t result=parse_case_branches(
        compiler,subject,flow_reg_count,entry_types,entry_sets,entry_tooling,
        flow_local_count,
        entry_alias,destination,join,subjectless,exhaustiveness);
    patch_jump(compiler,end_jump,compiler->function->code_count);
    return result;
}

/* Subject-bearing and subjectless `case` are both expressions. Subject-bearing
 * scalar/value patterns use CASE_MATCH; subjectless when values are tested for
 * truthiness. Both forms share the same branch/result flow-join machinery.
 * Like `if`, each desugars to the same MOVE-into-destination-then-jump-to-end
 * shape parse_if already uses. Nested
 * Array/Hash binding patterns combine non-raising shape/key checks, INDEX_GET,
 * and CASE_MATCH, then commit their bindings only on the successful path. */
uint16_t parse_case(Compiler *compiler) {
    const DiamondSpan case_span=compiler->previous.span;
    const bool subjectless=compiler->current.kind==DIAMOND_TOKEN_NEWLINE;
    uint16_t subject=0;
    if(subjectless) {
        subject=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_BOOL,subject,true,0,2);
        compiler->known_types[subject]=DIAMOND_TYPE_BOOL;
        skip_newlines(compiler);
    } else {
        subject=parse_expression(compiler);
        skip_newlines(compiler);
    }
    /* Only ever active outside the discovery pass -- a forward-referenced
     * class in the subject's own union type annotation may not be known
     * yet during discovery (see resolve_type_name's own comment for the
     * same, pre-existing concern), so checking here could fail() based on
     * incomplete information even though the real, second pass -- which
     * always runs with every declaration already known -- would not.
     * fail() only remembers the first failure, so a false positive here
     * would incorrectly abort compilation before the real pass ever gets
     * a chance to see the complete picture. */
    CaseExhaustiveness exhaustiveness={0};
    /* A *genuine* multi-member union (`set->count>1`) is the only signal
     * that activates the explicit-union path below -- a plain, single-
     * type annotation (`shape: Shape`, no `|` at all) still gets
     * represented as a `known_type_sets[subject]>=0` trivial one-member
     * DiamondTypeSet internally (confirmed empirically: known_types[reg]
     * held the real concrete class id, not TYPE_UNKNOWN, even while
     * known_type_sets[reg] was also >=0), so `known_type_sets[subject]
     * >=0` alone is NOT a reliable "this is a real union" test on its
     * own -- checking `set->count>1` is what actually distinguishes the
     * two, and is why this and the sealed-single-type branch below are
     * both plain `if`s (never else-if): whichever one's own precise
     * condition matches is the only one that can ever activate. */
    if(!subjectless&&!compiler->discovery_pass&&compiler->known_type_sets[subject]>=0) {
        const DiamondTypeSet *set=
            &compiler->function->type_sets[(uint16_t)compiler->known_type_sets[subject]];
        bool exhaustible=set->count>1;
        for(uint8_t member=0;member<set->count&&exhaustible;member++) {
            const uint16_t id=set->members[member].id;
            if(id!=DIAMOND_TYPE_NIL&&
               !(id>=DIAMOND_TYPE_CLASS_BASE&&id<DIAMOND_TYPE_CLASS_BASE+DIAMOND_MAX_CLASSES))
                exhaustible=false;
        }
        if(exhaustible) {
            exhaustiveness.active=true;
            exhaustiveness.required_count=set->count;
            for(uint8_t member=0;member<set->count;member++)
                exhaustiveness.required_ids[member]=set->members[member].id;
            exhaustiveness.case_span=case_span;
        }
    }
    if(!exhaustiveness.active&&!subjectless&&!compiler->discovery_pass&&
       compiler->known_types[subject]>=DIAMOND_TYPE_CLASS_BASE&&
       compiler->known_types[subject]<DIAMOND_TYPE_CLASS_BASE+DIAMOND_MAX_CLASSES&&
       compiler->program->classes[
           compiler->known_types[subject]-DIAMOND_TYPE_CLASS_BASE]->sealed) {
        /* A single (non-union) subject type naming a sealed class --
         * every direct subclass is required, found by a plain linear
         * scan since discovery has already registered every class in
         * the whole (flat-compiled) program by the time the real pass
         * reaches here. Sound specifically because a sealed class can't
         * be instantiated directly (see the `.new`-dispatch site) -- no
         * stray direct-Shape instance could ever fall through uncovered. */
        uint16_t required_ids[DIAMOND_MAX_UNION_TYPES];
        size_t direct_subclass_count=0;
        for(size_t candidate=0;candidate<compiler->program->class_count;candidate++) {
            if(compiler->program->classes[candidate]->superclass!=
                    compiler->known_types[subject]-DIAMOND_TYPE_CLASS_BASE)
                continue;
            /* Counted unconditionally, past DIAMOND_MAX_UNION_TYPES too --
             * a >8-direct-subclass hierarchy must be detected as such and
             * left unchecked, not silently truncated to its first 8
             * (which would be unsound: the check would then believe
             * exactly those 8 were the whole set). */
            if(direct_subclass_count<DIAMOND_MAX_UNION_TYPES)
                required_ids[direct_subclass_count]=(uint16_t)(DIAMOND_TYPE_CLASS_BASE+candidate);
            direct_subclass_count++;
        }
        if(direct_subclass_count>0&&direct_subclass_count<=DIAMOND_MAX_UNION_TYPES) {
            exhaustiveness.active=true;
            exhaustiveness.required_count=(uint8_t)direct_subclass_count;
            for(size_t member=0;member<direct_subclass_count;member++)
                exhaustiveness.required_ids[member]=required_ids[member];
            exhaustiveness.case_span=case_span;
        }
    }
    const uint16_t destination=allocate_register(compiler);
    const size_t flow_reg_count=compiler->next_register;
    const size_t flow_local_count=compiler->local_count;
    uint32_t entry_alias[DIAMOND_MAX_LOCALS];
    for(size_t index=0;index<flow_local_count;index++)
        entry_alias[index]=compiler->locals[index].alias_identity;
    /* Same inline-then-heap fallback as parse_if's before_types/
     * then_types -- see that function's own comment for why, and for the
     * ASan-confirmed bug a fixed [256] array with no bounds check caused
     * here otherwise. */
    uint16_t inline_entry_types[256],inline_join_types[256];
    int32_t inline_entry_sets[256],inline_join_sets[256];
    int32_t inline_entry_tooling[256],inline_join_tooling[256];
    bool inline_varied[256]={};
    uint16_t *entry_types=inline_entry_types;int32_t *entry_sets=inline_entry_sets;
    uint16_t *join_types=inline_join_types;int32_t *join_sets=inline_join_sets;
    int32_t *entry_tooling=inline_entry_tooling,*join_tooling=inline_join_tooling;
    bool *varied=inline_varied;
    uint16_t *heap_types=nullptr;int32_t *heap_sets=nullptr,*heap_tooling=nullptr;
    bool *heap_varied=nullptr;
    if(flow_reg_count>256) {
        heap_types=malloc(flow_reg_count*2*sizeof(uint16_t));
        heap_sets=malloc(flow_reg_count*2*sizeof(int32_t));
        heap_tooling=malloc(flow_reg_count*2*sizeof(int32_t));
        heap_varied=calloc(flow_reg_count,sizeof(bool));
        if(heap_types==nullptr||heap_sets==nullptr||heap_tooling==nullptr||
           heap_varied==nullptr) {
            fail(compiler,compiler->previous.span,
                 "out of memory compiling case expression");
            free(heap_types);free(heap_sets);free(heap_tooling);free(heap_varied);
            return destination;
        }
        entry_types=heap_types;join_types=heap_types+flow_reg_count;
        entry_sets=heap_sets;join_sets=heap_sets+flow_reg_count;varied=heap_varied;
        entry_tooling=heap_tooling;join_tooling=heap_tooling+flow_reg_count;
    }
    for(size_t index=0;index<flow_reg_count;index++) {
        entry_types[index]=compiler->known_types[index];
        entry_sets[index]=compiler->known_type_sets[index];
        entry_tooling[index]=compiler->tooling_type_sets[index];
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_WHEN) {
        fail(compiler,compiler->current.span,
             "expected 'when' after case expression");
        free(heap_types);free(heap_sets);free(heap_tooling);free(heap_varied);
        return destination;
    }
    CaseFlowJoin join={.types=join_types,.sets=join_sets,
        .tooling_sets=join_tooling,.varied=varied,
        .result_type=TYPE_UNKNOWN,.result_set=-1,.result_tooling_set=-1};
    const uint16_t result=parse_case_branches(
        compiler,subject,flow_reg_count,entry_types,entry_sets,entry_tooling,
        flow_local_count,entry_alias,destination,&join,subjectless,&exhaustiveness);
    free(heap_types);free(heap_sets);free(heap_tooling);free(heap_varied);
    return result;
}
