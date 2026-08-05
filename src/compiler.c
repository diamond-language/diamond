#include "compiler.h"

#include <limits.h>
#include <string.h>

enum { DIAMOND_MAX_LOCALS = 64 };
enum { TYPE_UNKNOWN = UINT8_MAX };

typedef enum Precedence {
    PREC_NONE,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_TERM,
    PREC_FACTOR,
    PREC_PREFIX,
} Precedence;

typedef struct Local {
    DiamondSpan name;
    uint8_t reg;
    bool captured;
} Local;

typedef struct LoopContext {
    struct LoopContext *previous;
    size_t continue_target;
    size_t breaks[64];
    size_t break_count;
} LoopContext;

typedef struct Narrowing {
    bool valid;
    uint8_t condition;
    uint8_t reg;
    int16_t when_true;
    int16_t when_false;
} Narrowing;

typedef struct Compiler {
    const char *source;
    DiamondLexer lexer;
    DiamondToken current;
    DiamondToken previous;
    DiamondProgram *program;
    DiamondFunction *function;
    DiamondDiagnostic *diagnostic;
    Local locals[DIAMOND_MAX_LOCALS];
    size_t local_count;
    uint16_t next_register;
    int current_class;
    DiamondSpan current_method;
    bool in_method;
    uint8_t known_types[256];
    int16_t known_type_sets[256];
    bool in_function;
    int current_return_type;
    DiamondSpan current_return_type_span;
    LoopContext *current_loop;
    bool failed;
    Local enclosing_locals[DIAMOND_MAX_LOCALS];
    size_t enclosing_local_count;
    uint8_t capture_registers[16];
    size_t capture_count;
    Narrowing narrowing;
} Compiler;

static uint8_t parse_expression(Compiler *compiler);
static uint8_t compile_sequence(Compiler *compiler);
static uint8_t compile_begin(Compiler *compiler);

static void fail(Compiler *compiler, DiamondSpan span, const char *message) {
    if (!compiler->failed) {
        compiler->diagnostic->span = span;
        compiler->diagnostic->message = message;
        compiler->failed = true;
    }
}

static void advance_token(Compiler *compiler) {
    compiler->previous = compiler->current;
    compiler->current = diamond_lexer_next(&compiler->lexer);
    if (compiler->current.kind == DIAMOND_TOKEN_ERROR) {
        fail(compiler, compiler->current.span, "unexpected character");
    }
}

static void skip_newlines(Compiler *compiler) {
    while (compiler->current.kind == DIAMOND_TOKEN_NEWLINE) {
        advance_token(compiler);
    }
}

static bool emit_byte(Compiler *compiler, uint8_t byte) {
    if (compiler->function->code_count == DIAMOND_MAX_CODE) {
        fail(compiler, compiler->previous.span, "program produces too much bytecode");
        return false;
    }
    compiler->function->code[compiler->function->code_count++] = byte;
    return true;
}

static bool emit_opcode(Compiler *compiler, DiamondOpCode opcode) {
    const size_t offset = compiler->function->code_count;
    if (!emit_byte(compiler, (uint8_t)opcode)) return false;
    compiler->function->lines[offset] = (uint32_t)compiler->previous.span.line;
    compiler->function->columns[offset] = (uint32_t)compiler->previous.span.column;
    return true;
}

static bool emit_instruction(Compiler *compiler, DiamondOpCode opcode,
                             uint8_t a, uint8_t b, uint8_t c, size_t operands) {
    if (!emit_opcode(compiler, opcode)) {
        return false;
    }
    const uint8_t values[] = {a, b, c};
    for (size_t index = 0; index < operands; index++) {
        if (!emit_byte(compiler, values[index])) {
            return false;
        }
    }
    return true;
}

static uint8_t allocate_register(Compiler *compiler) {
    if (compiler->next_register > UINT8_MAX) {
        fail(compiler, compiler->previous.span, "program needs too many registers");
        return 0;
    }
    const uint8_t reg=(uint8_t)compiler->next_register++;
    compiler->known_types[reg]=TYPE_UNKNOWN;
    compiler->known_type_sets[reg]=-1;
    return reg;
}

static bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint8_t known_index,
    const DiamondTypeSet *expected_sets,uint8_t expected_index);

static bool known_type_satisfies_one(const Compiler *compiler, uint8_t known,
                                     uint8_t expected) {
    if(known==expected) return true;
    if(expected>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(expected-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=compiler->program->interface_count)return false;
        const DiamondInterface *interface=&compiler->program->interfaces[interface_index];
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                const bool length=strcmp(method->name,"length")==0&&method->arity==0;
                const bool array_method=known==DIAMOND_TYPE_ARRAY&&
                    ((strcmp(method->name,"push")==0&&method->arity==1)||
                     (strcmp(method->name,"pop")==0&&method->arity==0));
                const bool hash_method=known==DIAMOND_TYPE_HASH&&method->arity==1&&
                    (strcmp(method->name,"key_at")==0||
                     strcmp(method->name,"value_at")==0);
                if(!length&&!array_method&&!hash_method)return false;
                if(method->return_type_set!=UINT8_MAX) {
                    uint8_t result=UINT8_MAX;
                    if(length)result=DIAMOND_TYPE_INT;
                    else if(known==DIAMOND_TYPE_ARRAY&&
                            strcmp(method->name,"push")==0)result=DIAMOND_TYPE_ARRAY;
                    if(result==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=result,
                        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
                        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
                    if(!type_sets_satisfy_across(compiler,&native,0,
                        compiler->program->entry.type_sets,
                        method->return_type_set))return false;
                }
            }
            return true;
        }
        if(known<DIAMOND_TYPE_CLASS_BASE||known>=DIAMOND_TYPE_INTERFACE_BASE)
            return false;
        for(size_t required=0;required<interface->method_count;required++) {
            bool found=false;
            size_t class_index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
            while(class_index<compiler->program->class_count&&!found) {
                const DiamondClass *class=&compiler->program->classes[class_index];
                for(size_t method=0;method<class->method_count;method++)
                    if(strcmp(class->methods[method].name,
                              interface->methods[required].name)==0&&
                       class->methods[method].arity==interface->methods[required].arity) {
                        const DiamondInterfaceMethod *wanted=
                            &interface->methods[required];
                        const DiamondFunction *implementation=
                            &compiler->program->functions[class->methods[method].function_index];
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint8_t required_set=wanted->parameter_type_sets[parameter];
                            const uint8_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==UINT8_MAX) {
                                if(actual_set!=UINT8_MAX)found=false;
                            } else if(actual_set!=UINT8_MAX&&
                                !type_sets_satisfy_across(compiler,
                                    compiler->program->entry.type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=UINT8_MAX&&
                           (implementation->return_type_set==UINT8_MAX||
                            !type_sets_satisfy_across(compiler,
                                implementation->type_sets,
                                implementation->return_type_set,
                                compiler->program->entry.type_sets,
                                wanted->return_type_set)))found=false;
                        if(found)break;
                    }
                if(found||class->superclass==UINT8_MAX)break;
                class_index=class->superclass;
            }
            if(!found)return false;
        }
        return true;
    }
    if(expected==DIAMOND_TYPE_SIZED) {
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH)return true;
        if(known<DIAMOND_TYPE_CLASS_BASE)return false;
        size_t class_index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
        while(class_index<compiler->program->class_count) {
            const DiamondClass *class=&compiler->program->classes[class_index];
            for(size_t index=0;index<class->method_count;index++)
                if(strcmp(class->methods[index].name,"length")==0&&
                   class->methods[index].arity==0)return true;
            if(class->superclass==UINT8_MAX)break;
            class_index=class->superclass;
        }
        return false;
    }
    if(known<DIAMOND_TYPE_CLASS_BASE || expected<DIAMOND_TYPE_CLASS_BASE)
        return false;
    size_t class_index=(size_t)(known-DIAMOND_TYPE_CLASS_BASE);
    const size_t wanted=(size_t)(expected-DIAMOND_TYPE_CLASS_BASE);
    while(class_index<compiler->program->class_count) {
        if(class_index==wanted) return true;
        const uint8_t parent=compiler->program->classes[class_index].superclass;
        if(parent==UINT8_MAX) break;
        class_index=parent;
    }
    return false;
}

static bool type_member_satisfies(const Compiler *compiler,
                                  DiamondTypeMember known,
                                  DiamondTypeMember expected);

static bool type_set_satisfies(const Compiler *compiler,uint8_t known_index,
                               uint8_t expected_index) {
    const DiamondTypeSet *known=&compiler->function->type_sets[known_index];
    const DiamondTypeSet *expected=&compiler->function->type_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=type_member_satisfies(compiler,known->members[source],
                                           expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

static bool type_members_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,DiamondTypeMember known,
    const DiamondTypeSet *expected_sets,DiamondTypeMember expected) {
    if(!known_type_satisfies_one(compiler,known.id,expected.id))return false;
    if(expected.id==DIAMOND_TYPE_CALLABLE) {
        if(expected.callable_arity!=UINT8_MAX&&
           known.callable_arity!=expected.callable_arity)return false;
        return expected.callable_return_set==UINT8_MAX||
            (known.callable_return_set!=UINT8_MAX&&
             type_sets_satisfy_across(compiler,known_sets,
                 known.callable_return_set,expected_sets,
                 expected.callable_return_set));
    }
    if(expected.argument_set==UINT8_MAX)return true;
    if(known.argument_set==UINT8_MAX||
       !type_sets_satisfy_across(compiler,known_sets,known.argument_set,
                                 expected_sets,expected.argument_set))return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=UINT8_MAX&&
        type_sets_satisfy_across(compiler,known_sets,known.second_argument_set,
                                 expected_sets,expected.second_argument_set);
}

static bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint8_t known_index,
    const DiamondTypeSet *expected_sets,uint8_t expected_index) {
    const DiamondTypeSet *known=&known_sets[known_index];
    const DiamondTypeSet *expected=&expected_sets[expected_index];
    for(size_t source=0;source<known->count;source++) {
        bool accepted=false;
        for(size_t target=0;target<expected->count&&!accepted;target++)
            accepted=type_members_satisfy_across(compiler,known_sets,
                known->members[source],expected_sets,expected->members[target]);
        if(!accepted)return false;
    }
    return true;
}

static bool type_member_satisfies(const Compiler *compiler,
                                  DiamondTypeMember known,
                                  DiamondTypeMember expected) {
    if(!known_type_satisfies_one(compiler,known.id,expected.id))return false;
    if(expected.id==DIAMOND_TYPE_CALLABLE) {
        if(expected.callable_arity!=UINT8_MAX&&
           known.callable_arity!=expected.callable_arity)return false;
        return expected.callable_return_set==UINT8_MAX||
            (known.callable_return_set!=UINT8_MAX&&
             type_set_satisfies(compiler,known.callable_return_set,
                                expected.callable_return_set));
    }
    if(expected.argument_set==UINT8_MAX)return true;
    if(known.argument_set==UINT8_MAX||
       !type_set_satisfies(compiler,known.argument_set,expected.argument_set))
        return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=UINT8_MAX&&
        expected.second_argument_set!=UINT8_MAX&&
        type_set_satisfies(compiler,known.second_argument_set,
                           expected.second_argument_set);
}

static void emit_type_check(Compiler *compiler, uint8_t reg, uint8_t set_index,
                            DiamondSpan span) {
    if(compiler->known_type_sets[reg]>=0) {
        if(type_set_satisfies(compiler,
           (uint8_t)compiler->known_type_sets[reg],set_index))return;
        fail(compiler,span,"expression cannot satisfy type annotation");return;
    }
    const uint8_t known=compiler->known_types[reg];
    if(known==TYPE_UNKNOWN) {
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
        return;
    }
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        if(!known_type_satisfies_one(compiler,known,set->members[index].id))continue;
        if(set->members[index].argument_set!=UINT8_MAX)
            emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
        return;
    }
    fail(compiler,span,"expression cannot satisfy type annotation");
}

static uint8_t add_constant(Compiler *compiler, DiamondValue value) {
    if (compiler->function->constant_count == DIAMOND_MAX_CONSTANTS) {
        fail(compiler, compiler->previous.span, "program has too many constants");
        return 0;
    }
    const size_t index = compiler->function->constant_count++;
    compiler->function->constants[index] = value;
    return (uint8_t)index;
}

static uint8_t add_string(Compiler *compiler, DiamondSpan span) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS) {
        fail(compiler, span, "function has too many string literals");
        return 0;
    }
    DiamondStringConstant *string =
        &compiler->function->strings[compiler->function->string_count];
    for (size_t index = 1; index + 1 < span.length; index++) {
        char character = compiler->source[span.start + index];
        if (character == '\\') {
            index++;
            character = compiler->source[span.start + index];
            switch (character) {
                case 'n': character = '\n'; break;
                case 'r': character = '\r'; break;
                case 't': character = '\t'; break;
                case '"': character = '"'; break;
                case '\\': character = '\\'; break;
                default:
                    fail(compiler, span, "unsupported string escape");
                    return 0;
            }
        }
        if (string->length == DIAMOND_MAX_STRING_LENGTH) {
            fail(compiler, span, "string literal is too long");
            return 0;
        }
        string->chars[string->length++] = character;
    }
    string->chars[string->length] = '\0';
    return (uint8_t)compiler->function->string_count++;
}

static uint8_t add_name_string(Compiler *compiler, DiamondSpan span) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS ||
        span.length > DIAMOND_MAX_STRING_LENGTH) {
        fail(compiler, span, "too many or oversized names in function");
        return 0;
    }
    DiamondStringConstant *string =
        &compiler->function->strings[compiler->function->string_count];
    for (size_t i = 0; i < span.length; i++)
        string->chars[i] = compiler->source[span.start + i];
    string->length = span.length;
    string->chars[span.length] = '\0';
    return (uint8_t)compiler->function->string_count++;
}

static size_t emit_jump(Compiler *compiler, DiamondOpCode opcode,
                        uint8_t condition) {
    const bool conditional=opcode==DIAMOND_OP_JUMP_IF_FALSE ||
                           opcode==DIAMOND_OP_JUMP_IF_TRUE;
    const size_t operand = compiler->function->code_count +
        (conditional ? 2 : 1);
    emit_instruction(compiler, opcode, condition, 0, 0,
                     conditional ? 3 : 2);
    return operand;
}

static void patch_jump(Compiler *compiler, size_t operand, size_t target) {
    if (target > UINT16_MAX) {
        fail(compiler, compiler->previous.span, "jump target is too distant");
        return;
    }
    compiler->function->code[operand] = (uint8_t)(target >> 8);
    compiler->function->code[operand + 1] = (uint8_t)(target & UINT8_MAX);
}

static void emit_absolute_jump(Compiler *compiler, size_t target) {
    if (target > UINT16_MAX) {
        fail(compiler, compiler->previous.span, "jump target is too distant");
        return;
    }
    emit_instruction(compiler, DIAMOND_OP_JUMP, (uint8_t)(target >> 8),
                     (uint8_t)(target & UINT8_MAX), 0, 2);
}

static bool spans_equal(const Compiler *compiler, DiamondSpan left,
                        DiamondSpan right) {
    if (left.length != right.length) {
        return false;
    }
    for (size_t index = 0; index < left.length; index++) {
        if (compiler->source[left.start + index] !=
            compiler->source[right.start + index]) {
            return false;
        }
    }
    return true;
}

static int find_local(const Compiler *compiler, DiamondSpan name) {
    for (size_t index = compiler->local_count; index > 0; index--) {
        if (spans_equal(compiler, compiler->locals[index - 1].name, name)) {
            return (int)(index - 1);
        }
    }
    return -1;
}

static uint8_t define_local(Compiler *compiler, DiamondSpan name) {
    if (compiler->local_count == DIAMOND_MAX_LOCALS) {
        fail(compiler, name, "too many local variables");
        return 0;
    }
    const uint8_t reg = allocate_register(compiler);
    compiler->locals[compiler->local_count++] = (Local){.name = name, .reg = reg};
    return reg;
}

static Precedence token_precedence(DiamondTokenKind kind) {
    switch (kind) {
        case DIAMOND_TOKEN_OR_OR:
            return PREC_OR;
        case DIAMOND_TOKEN_AND_AND:
            return PREC_AND;
        case DIAMOND_TOKEN_EQUAL_EQUAL:
        case DIAMOND_TOKEN_BANG_EQUAL:
            return PREC_EQUALITY;
        case DIAMOND_TOKEN_LESS:
        case DIAMOND_TOKEN_LESS_EQUAL:
        case DIAMOND_TOKEN_GREATER:
        case DIAMOND_TOKEN_GREATER_EQUAL:
        case DIAMOND_TOKEN_IS:
            return PREC_COMPARISON;
        case DIAMOND_TOKEN_PLUS:
        case DIAMOND_TOKEN_MINUS:
            return PREC_TERM;
        case DIAMOND_TOKEN_STAR:
        case DIAMOND_TOKEN_SLASH:
            return PREC_FACTOR;
        default:
            return PREC_NONE;
    }
}

static uint8_t parse_precedence(Compiler *compiler, Precedence precedence);

static uint8_t parse_integer(Compiler *compiler) {
    const DiamondSpan span = compiler->previous.span;
    int64_t value = 0;
    for (size_t index = 0; index < span.length; index++) {
        if(compiler->source[span.start+index]=='_') continue;
        const int digit = compiler->source[span.start + index] - '0';
        if (value > (INT64_MAX - digit) / 10) {
            fail(compiler, span, "integer literal is too large");
            return 0;
        }
        value = value * 10 + digit;
    }
    const uint8_t destination = allocate_register(compiler);
    const uint8_t constant = add_constant(compiler, DIAMOND_INT(value));
    emit_instruction(compiler, DIAMOND_OP_CONSTANT, destination, constant, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_INT;
    return destination;
}

static uint8_t parse_string(Compiler *compiler) {
    const uint8_t destination = allocate_register(compiler);
    const uint8_t string = add_string(compiler, compiler->previous.span);
    emit_instruction(compiler, DIAMOND_OP_STRING, destination, string, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_STRING;
    return destination;
}

static uint8_t parse_literal(Compiler *compiler) {
    const uint8_t destination = allocate_register(compiler);
    if (compiler->previous.kind == DIAMOND_TOKEN_NIL) {
        emit_instruction(compiler, DIAMOND_OP_NIL, destination, 0, 0, 1);
        compiler->known_types[destination]=DIAMOND_TYPE_NIL;
    } else {
        emit_instruction(compiler, DIAMOND_OP_BOOL, destination,
                         compiler->previous.kind == DIAMOND_TOKEN_TRUE, 0, 2);
        compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
    }
    return destination;
}

static uint8_t parse_identifier(Compiler *compiler) {
    const int local = find_local(compiler, compiler->previous.span);
    if (local < 0) {
        for(size_t i=compiler->enclosing_local_count;i>0;i--) {
            if(!spans_equal(compiler,compiler->enclosing_locals[i-1].name,
                            compiler->previous.span)) continue;
            size_t capture=0;
            while(capture<compiler->capture_count &&
                  compiler->capture_registers[capture]!=compiler->enclosing_locals[i-1].reg)
                capture++;
            if(capture==compiler->capture_count) {
                if(capture==16){fail(compiler,compiler->previous.span,"too many captured variables");return 0;}
                compiler->capture_registers[compiler->capture_count++]=compiler->enclosing_locals[i-1].reg;
            }
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,destination,(uint8_t)capture,0,2);
            return destination;
        }
        fail(compiler, compiler->previous.span, "undefined local variable"); return 0;
    }
    if(!compiler->locals[(size_t)local].captured)
        return compiler->locals[(size_t)local].reg;
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_GET_CELL,destination,
                     compiler->locals[(size_t)local].reg,0,2);
    return destination;
}

static int find_function(const Compiler *compiler, DiamondSpan name) {
    for (size_t index = 0; index < compiler->program->function_count; index++) {
        const char *candidate = compiler->program->functions[index].name;
        if (compiler->program->functions[index].owner_class != UINT8_MAX ||
            compiler->program->functions[index].nested) continue;
        size_t length = 0;
        while (candidate[length] != '\0') length++;
        if (length != name.length) continue;
        bool equal = true;
        for (size_t character = 0; character < length; character++) {
            if (candidate[character] != compiler->source[name.start + character]) {
                equal = false;
                break;
            }
        }
        if (equal) return (int)index;
    }
    return -1;
}

static bool name_equals(const Compiler *compiler, const char *candidate,
                        DiamondSpan name, bool skip_at) {
    const size_t start = skip_at ? 1 : 0;
    size_t length = 0;
    while (candidate[length] != '\0') length++;
    if (name.length - start != length) return false;
    for (size_t index = 0; index < length; index++) {
        if (candidate[index] != compiler->source[name.start + start + index]) return false;
    }
    return true;
}

static int find_class(const Compiler *compiler, DiamondSpan name) {
    for (size_t index = 0; index < compiler->program->class_count; index++) {
        if (name_equals(compiler, compiler->program->classes[index].name, name, false))
            return (int)index;
    }
    return -1;
}

static int find_interface(const Compiler *compiler,DiamondSpan name) {
    for(size_t index=0;index<compiler->program->interface_count;index++)
        if(name_equals(compiler,compiler->program->interfaces[index].name,name,false))
            return (int)index;
    return -1;
}

static int resolve_type(Compiler *compiler, DiamondSpan name) {
    if (name_equals(compiler, "Int", name, false)) return DIAMOND_TYPE_INT;
    if (name_equals(compiler, "String", name, false)) return DIAMOND_TYPE_STRING;
    if (name_equals(compiler, "Bool", name, false)) return DIAMOND_TYPE_BOOL;
    if (name_equals(compiler, "Nil", name, false)) return DIAMOND_TYPE_NIL;
    if (name_equals(compiler, "Array", name, false)) return DIAMOND_TYPE_ARRAY;
    if (name_equals(compiler, "Hash", name, false)) return DIAMOND_TYPE_HASH;
    if (name_equals(compiler, "Callable", name, false)) return DIAMOND_TYPE_CALLABLE;
    if (name_equals(compiler, "Sized", name, false)) return DIAMOND_TYPE_SIZED;
    const int interface_index=find_interface(compiler,name);
    if(interface_index>=0)return DIAMOND_TYPE_INTERFACE_BASE+interface_index;
    const int class_index = find_class(compiler, name);
    if (class_index >= 0) return DIAMOND_TYPE_CLASS_BASE + class_index;
    fail(compiler, name, "unknown type annotation");
    return DIAMOND_TYPE_NIL;
}

static int parse_type_annotation(Compiler *compiler) {
    if(compiler->function->type_set_count==DIAMOND_MAX_TYPE_SETS) {
        fail(compiler,compiler->current.span,"function has too many type annotations");
        return 0;
    }
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    while(!compiler->failed) {
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected type annotation");break;
        }
        const DiamondSpan member_span=compiler->current.span;
        const uint8_t type=(uint8_t)resolve_type(compiler,member_span);
        for(size_t index=0;index<set->count;index++) {
            if(set->members[index].id==type) {
                fail(compiler,compiler->current.span,"duplicate type in union");break;
            }
        }
        if(set->count==DIAMOND_MAX_UNION_TYPES) {
            fail(compiler,compiler->current.span,"too many types in union");break;
        }
        advance_token(compiler);
        uint8_t argument_set=UINT8_MAX,second_argument_set=UINT8_MAX;
        uint8_t callable_arity=UINT8_MAX;
        uint8_t callable_return_set=UINT8_MAX;
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            if(type==DIAMOND_TYPE_CALLABLE) {
                advance_token(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_INTEGER) {
                    fail(compiler,compiler->current.span,
                         "expected Callable arity");break;
                }
                size_t arity=0;
                for(size_t index=0;index<compiler->current.span.length;index++) {
                    const char ch=compiler->source[compiler->current.span.start+index];
                    if(ch=='_')continue;
                    arity=arity*10+(size_t)(ch-'0');
                }
                if(arity>16) {
                    fail(compiler,compiler->current.span,
                         "Callable arity cannot exceed 16");break;
                }
                callable_arity=(uint8_t)arity;advance_token(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                    advance_token(compiler);
                    callable_return_set=(uint8_t)parse_type_annotation(compiler);
                }
            } else if(type==DIAMOND_TYPE_ARRAY||type==DIAMOND_TYPE_HASH) {
                advance_token(compiler);
                argument_set=(uint8_t)parse_type_annotation(compiler);
                if(type==DIAMOND_TYPE_HASH) {
                    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                        fail(compiler,compiler->current.span,
                             "expected ',' between Hash key and value types");break;
                    }
                    advance_token(compiler);
                    second_argument_set=(uint8_t)parse_type_annotation(compiler);
                }
            } else {
                fail(compiler,member_span,
                     "this type does not accept arguments");break;
            }
            if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                fail(compiler,compiler->current.span,
                     "expected ']' after collection type arguments");break;
            }
            advance_token(compiler);
        }
        set->members[set->count++]=(DiamondTypeMember){
            .id=type,.argument_set=argument_set,
            .second_argument_set=second_argument_set,
            .callable_arity=callable_arity,
            .callable_return_set=callable_return_set};
        if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
        advance_token(compiler);
    }
    return (int)set_index;
}

static int find_method(const Compiler *compiler, int class_index, DiamondSpan name) {
    const DiamondClass *class = &compiler->program->classes[(size_t)class_index];
    for (size_t index = 0; index < class->method_count; index++) {
        if (name_equals(compiler, class->methods[index].name, name, false))
            return (int)index;
    }
    return -1;
}

static int field_index(Compiler *compiler, DiamondSpan name, bool create) {
    if (compiler->current_class < 0) {
        fail(compiler, name, "instance variable used outside a method");
        return -1;
    }
    DiamondClass *class = &compiler->program->classes[(size_t)compiler->current_class];
    for (size_t index = 0; index < class->field_count; index++) {
        if (name_equals(compiler, class->fields[index], name, true)) return (int)index;
    }
    if (!create) return -1;
    if (class->field_count == DIAMOND_MAX_FIELDS) {
        fail(compiler, name, "too many instance variables");
        return -1;
    }
    char *field = class->fields[class->field_count];
    const size_t length = name.length - 1;
    if (length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "instance variable name is too long");
        return -1;
    }
    for (size_t i = 0; i < length; i++) field[i] = compiler->source[name.start + 1 + i];
    field[length] = '\0';
    return (int)class->field_count++;
}

static uint8_t parse_call(Compiler *compiler, DiamondSpan name) {
    const int callable_local=find_local(compiler,name);
    if(callable_local>=0) {
        uint8_t callable=compiler->locals[(size_t)callable_local].reg;
        if(compiler->locals[(size_t)callable_local].captured) {
            const uint8_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,callable,0,2);
            callable=loaded;
        }
        advance_token(compiler);
        uint8_t arguments[16]; size_t argument_count=0;
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
            if(argument_count==16){fail(compiler,compiler->current.span,"too many call arguments");return 0;}
            arguments[argument_count++]=parse_expression(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN){fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;}
        advance_token(compiler);
        const uint8_t base=allocate_register(compiler);
        for(size_t i=1;i<argument_count;i++)(void)allocate_register(compiler);
        for(size_t i=0;i<argument_count;i++)emit_instruction(compiler,DIAMOND_OP_MOVE,(uint8_t)(base+i),arguments[i],0,2);
        const uint8_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE);emit_byte(compiler,destination);
        emit_byte(compiler,callable);emit_byte(compiler,base);emit_byte(compiler,(uint8_t)argument_count);
        return destination;
    }
    const int function_index = find_function(compiler, name);
    if (function_index < 0) {
        fail(compiler, name, "undefined function");
        return 0;
    }
    advance_token(compiler);
    uint8_t arguments[16];
    size_t argument_count = 0;
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            if (argument_count == sizeof arguments / sizeof arguments[0]) {
                fail(compiler, compiler->current.span, "too many call arguments");
                return 0;
            }
            arguments[argument_count++] = parse_expression(compiler);
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        } while (!compiler->failed);
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const DiamondFunction *function =
        &compiler->program->functions[(size_t)function_index];
    if (argument_count != function->arity) {
        fail(compiler, name, "wrong number of arguments");
        return 0;
    }

    const uint8_t argument_base = allocate_register(compiler);
    for (size_t index = 1; index < argument_count; index++) {
        (void)allocate_register(compiler);
    }
    for (size_t index = 0; index < argument_count; index++) {
        emit_instruction(compiler, DIAMOND_OP_MOVE,
                         (uint8_t)(argument_base + index), arguments[index], 0, 2);
    }
    const uint8_t destination = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_CALL);
    emit_byte(compiler, destination);
    emit_byte(compiler, (uint8_t)function_index);
    emit_byte(compiler, argument_base);
    emit_byte(compiler, (uint8_t)argument_count);
    return destination;
}

static uint8_t parse_name(Compiler *compiler) {
    const DiamondSpan name = compiler->previous.span;
    const int class_index = find_class(compiler, name);
    if (class_index >= 0 && compiler->current.kind == DIAMOND_TOKEN_DOT) {
        advance_token(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER ||
            !name_equals(compiler, "new", compiler->current.span, false)) {
            fail(compiler, compiler->current.span, "expected 'new' after class name");
            return 0;
        }
        advance_token(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler, compiler->current.span, "expected '(' after 'new'");
            return 0;
        }
        advance_token(compiler);
        uint8_t args[16]; size_t count = 0;
        while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
            if (count == 16) { fail(compiler, compiler->current.span, "too many arguments"); break; }
            args[count++] = parse_expression(compiler);
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        }
        if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler, compiler->current.span, "expected ')' after arguments"); return 0;
        }
        advance_token(compiler);
        const uint8_t base = allocate_register(compiler);
        for (size_t i = 1; i < count; i++) (void)allocate_register(compiler);
        for (size_t i = 0; i < count; i++)
            emit_instruction(compiler, DIAMOND_OP_MOVE, (uint8_t)(base+i), args[i], 0, 2);
        const uint8_t dest = allocate_register(compiler);
        emit_opcode(compiler, DIAMOND_OP_NEW); emit_byte(compiler, dest);
        emit_byte(compiler, (uint8_t)class_index); emit_byte(compiler, base);
        emit_byte(compiler, (uint8_t)count);
        compiler->known_types[dest]=(uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
        return dest;
    }
    if (compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN) {
        return parse_call(compiler, name);
    }
    return parse_identifier(compiler);
}

static uint8_t parse_invoke(Compiler *compiler, uint8_t receiver) {
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler, compiler->current.span, "expected method name after '.'"); return 0;
    }
    const DiamondSpan name = compiler->current.span;
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after method name"); return 0;
    }
    advance_token(compiler);
    uint8_t args[16]; size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == 16) { fail(compiler, compiler->current.span, "too many arguments"); break; }
        args[count++] = parse_expression(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments"); return 0;
    }
    advance_token(compiler);
    const uint8_t base = allocate_register(compiler);
    for (size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for (size_t i=0;i<count;i++) emit_instruction(compiler, DIAMOND_OP_MOVE,
        (uint8_t)(base+i), args[i], 0, 2);
    const uint8_t dest=allocate_register(compiler);
    const uint8_t method=add_name_string(compiler,name);
    emit_opcode(compiler,DIAMOND_OP_INVOKE); emit_byte(compiler,dest);
    emit_byte(compiler,receiver); emit_byte(compiler,method); emit_byte(compiler,base);
    emit_byte(compiler,(uint8_t)count);
    return dest;
}

static uint8_t parse_super(Compiler *compiler) {
    const DiamondSpan keyword = compiler->previous.span;
    if (!compiler->in_method || compiler->current_class < 0) {
        fail(compiler, keyword, "'super' used outside a method");
        return 0;
    }
    const DiamondClass *owner =
        &compiler->program->classes[(size_t)compiler->current_class];
    if (owner->superclass == UINT8_MAX) {
        fail(compiler, keyword, "'super' used in a class without a superclass");
        return 0;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'super'");
        return 0;
    }
    advance_token(compiler);
    uint8_t arguments[16];
    size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == 16) {
            fail(compiler, compiler->current.span, "too many arguments");
            return 0;
        }
        arguments[count++] = parse_expression(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t base = allocate_register(compiler);
    for (size_t index = 1; index < count; index++) (void)allocate_register(compiler);
    for (size_t index = 0; index < count; index++) {
        emit_instruction(compiler, DIAMOND_OP_MOVE, (uint8_t)(base + index),
                         arguments[index], 0, 2);
    }
    const uint8_t destination = allocate_register(compiler);
    const uint8_t method = add_name_string(compiler, compiler->current_method);
    emit_opcode(compiler, DIAMOND_OP_SUPER);
    emit_byte(compiler, destination);
    emit_byte(compiler, (uint8_t)compiler->current_class);
    emit_byte(compiler, method);
    emit_byte(compiler, base);
    emit_byte(compiler, (uint8_t)count);
    return destination;
}

static uint8_t parse_grouping(Compiler *compiler) {
    const uint8_t result = parse_expression(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after expression");
        return result;
    }
    advance_token(compiler);
    return result;
}

static uint8_t parse_array(Compiler *compiler) {
    uint8_t elements[32];
    size_t count=0;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        do {
            if(count==32) {
                fail(compiler,compiler->current.span,"array literal has too many elements");
                return 0;
            }
            elements[count++]=parse_expression(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET) break;
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after array literal");
        return 0;
    }
    advance_token(compiler);
    const uint8_t base=allocate_register(compiler);
    for(size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) emit_instruction(compiler,DIAMOND_OP_MOVE,
        (uint8_t)(base+i),elements[i],0,2);
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,destination,base,(uint8_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_ARRAY;
    return destination;
}

static uint8_t parse_hash(Compiler *compiler) {
    uint8_t keys[16],values[16];
    size_t count=0;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        do {
            if(count==16) {
                fail(compiler,compiler->current.span,"hash literal has too many entries");
                return 0;
            }
            keys[count]=parse_expression(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                fail(compiler,compiler->current.span,"expected ':' after hash key");
                return 0;
            }
            advance_token(compiler);
            values[count]=parse_expression(compiler);
            count++;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE) break;
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        fail(compiler,compiler->current.span,"expected '}' after hash literal");
        return 0;
    }
    advance_token(compiler);
    const uint8_t base=allocate_register(compiler);
    for(size_t i=1;i<count*2;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) {
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint8_t)(base+i*2),keys[i],0,2);
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint8_t)(base+i*2+1),values[i],0,2);
    }
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_HASH,destination,base,(uint8_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_HASH;
    return destination;
}

static int16_t type_set_with_nil(Compiler *compiler,uint8_t source_index) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    for(size_t index=0;index<source.count;index++)
        if(source.members[index].id==DIAMOND_TYPE_NIL)return (int16_t)source_index;
    if(source.count==DIAMOND_MAX_UNION_TYPES||
       compiler->function->type_set_count==DIAMOND_MAX_TYPE_SETS)return -1;
    const size_t result=compiler->function->type_set_count++;
    compiler->function->type_sets[result]=source;
    DiamondTypeSet *set=&compiler->function->type_sets[result];
    set->members[set->count++]=(DiamondTypeMember){
        .id=DIAMOND_TYPE_NIL,.argument_set=UINT8_MAX,
        .second_argument_set=UINT8_MAX,.callable_arity=UINT8_MAX,
        .callable_return_set=UINT8_MAX};
    return (int16_t)result;
}

static bool split_nil_type_set(Compiler *compiler,uint8_t source_index,
                               int16_t *without_nil,int16_t *only_nil) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet narrowed={};bool found_nil=false;
    for(size_t index=0;index<source.count;index++) {
        if(source.members[index].id==DIAMOND_TYPE_NIL)found_nil=true;
        else narrowed.members[narrowed.count++]=source.members[index];
    }
    if(!found_nil||narrowed.count==0||
       compiler->function->type_set_count+2>DIAMOND_MAX_TYPE_SETS)return false;
    *without_nil=(int16_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=narrowed;
    *only_nil=(int16_t)compiler->function->type_set_count;
    DiamondTypeSet *nil_set=&compiler->function->type_sets[
        compiler->function->type_set_count++];
    *nil_set=(DiamondTypeSet){.members={{.id=DIAMOND_TYPE_NIL,
        .argument_set=UINT8_MAX,.second_argument_set=UINT8_MAX,
        .callable_arity=UINT8_MAX,.callable_return_set=UINT8_MAX}},.count=1};
    return true;
}

static bool split_type_set(Compiler *compiler,uint8_t source_index,
                           uint8_t tested_type,int16_t *matching,
                           int16_t *remaining) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet yes={},no={};
    for(size_t index=0;index<source.count;index++) {
        const DiamondTypeMember member=source.members[index];
        if(known_type_satisfies_one(compiler,member.id,tested_type))
            yes.members[yes.count++]=member;
        else no.members[no.count++]=member;
    }
    if(yes.count==0||no.count==0||
       compiler->function->type_set_count+2>DIAMOND_MAX_TYPE_SETS)return false;
    *matching=(int16_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=yes;
    *remaining=(int16_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=no;
    return true;
}

static void apply_type_set_fact(Compiler *compiler,uint8_t reg,int16_t set_index) {
    compiler->known_type_sets[reg]=set_index;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)set_index];
    compiler->known_types[reg]=set->count==1?set->members[0].id:TYPE_UNKNOWN;
}

static uint8_t parse_index(Compiler *compiler,uint8_t receiver) {
    advance_token(compiler);
    const uint8_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after index"); return 0;
    }
    advance_token(compiler);
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_GET,destination,receiver,index,3);
    const int16_t receiver_set=compiler->known_type_sets[receiver];
    if(receiver_set>=0) {
        const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)receiver_set];
        if(set->count==1) {
            const DiamondTypeMember member=set->members[0];
            if(member.id==DIAMOND_TYPE_ARRAY&&member.argument_set!=UINT8_MAX)
                compiler->known_type_sets[destination]=(int16_t)member.argument_set;
            else if(member.id==DIAMOND_TYPE_HASH&&
                    member.second_argument_set!=UINT8_MAX)
                compiler->known_type_sets[destination]=type_set_with_nil(
                    compiler,member.second_argument_set);
        }
    }
    return destination;
}

static bool consume_block_start(Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_NEWLINE) {
        fail(compiler, compiler->current.span, "expected newline before block body");
        return false;
    }
    skip_newlines(compiler);
    return true;
}

static uint8_t parse_if(Compiler *compiler) {
    compiler->narrowing=(Narrowing){};
    const uint8_t condition = parse_expression(compiler);
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    if (!consume_block_start(compiler)) return 0;

    const size_t false_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, condition);
    const uint8_t destination = allocate_register(compiler);
    const size_t flow_reg_count=compiler->next_register;
    uint8_t before_types[256];int16_t before_sets[256];
    for(size_t index=0;index<flow_reg_count;index++) {
        before_types[index]=compiler->known_types[index];
        before_sets[index]=compiler->known_type_sets[index];
    }
    if(narrowing.valid)
        apply_type_set_fact(compiler,narrowing.reg,narrowing.when_true);
    const uint8_t then_result = compile_sequence(compiler);
    const uint8_t then_type=compiler->known_types[then_result];
    const int16_t then_set=compiler->known_type_sets[then_result];
    uint8_t then_types[256];int16_t then_sets[256];
    for(size_t index=0;index<flow_reg_count;index++) {
        then_types[index]=compiler->known_types[index];
        then_sets[index]=compiler->known_type_sets[index];
    }
    emit_instruction(compiler, DIAMOND_OP_MOVE, destination, then_result, 0, 2);
    const size_t end_jump = emit_jump(compiler, DIAMOND_OP_JUMP, 0);
    patch_jump(compiler, false_jump, compiler->function->code_count);

    for(size_t index=0;index<flow_reg_count;index++) {
        compiler->known_types[index]=before_types[index];
        compiler->known_type_sets[index]=before_sets[index];
    }
    if(narrowing.valid)
        apply_type_set_fact(compiler,narrowing.reg,narrowing.when_false);

    uint8_t result_type=TYPE_UNKNOWN;int16_t result_set=-1;
    if (compiler->current.kind == DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if (!consume_block_start(compiler)) return destination;
        const uint8_t else_result = compile_sequence(compiler);
        const uint8_t else_type=compiler->known_types[else_result];
        const int16_t else_set=compiler->known_type_sets[else_result];
        emit_instruction(compiler, DIAMOND_OP_MOVE, destination, else_result, 0, 2);
        if(then_type==else_type)result_type=then_type;
        if(then_set==else_set)result_set=then_set;
    } else {
        emit_instruction(compiler, DIAMOND_OP_NIL, destination, 0, 0, 1);
        if(then_type==DIAMOND_TYPE_NIL)result_type=DIAMOND_TYPE_NIL;
    }

    for(size_t index=0;index<flow_reg_count;index++) {
        const uint8_t false_type=compiler->known_types[index];
        const int16_t false_set=compiler->known_type_sets[index];
        compiler->known_types[index]=then_types[index]==false_type
            ?then_types[index]:TYPE_UNKNOWN;
        compiler->known_type_sets[index]=then_sets[index]==false_set
            ?then_sets[index]:-1;
    }
    compiler->known_types[destination]=result_type;
    compiler->known_type_sets[destination]=result_set;

    if (compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after if expression");
        return destination;
    }
    advance_token(compiler);
    patch_jump(compiler, end_jump, compiler->function->code_count);
    return destination;
}

static uint8_t parse_while(Compiler *compiler) {
    const size_t loop_start = compiler->function->code_count;
    const uint8_t condition = parse_expression(compiler);
    if (!consume_block_start(compiler)) return 0;
    const size_t exit_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, condition);
    LoopContext loop={
        .previous=compiler->current_loop,
        .continue_target=loop_start,
    };
    compiler->current_loop=&loop;
    (void)compile_sequence(compiler);
    compiler->current_loop=loop.previous;
    emit_absolute_jump(compiler, loop_start);
    patch_jump(compiler, exit_jump, compiler->function->code_count);
    for(size_t index=0;index<loop.break_count;index++)
        patch_jump(compiler,loop.breaks[index],compiler->function->code_count);

    if (compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after while expression");
        return 0;
    }
    advance_token(compiler);
    const uint8_t destination = allocate_register(compiler);
    emit_instruction(compiler, DIAMOND_OP_NIL, destination, 0, 0, 1);
    compiler->known_types[destination]=DIAMOND_TYPE_NIL;
    return destination;
}

static uint8_t parse_prefix(Compiler *compiler) {
    advance_token(compiler);
    switch (compiler->previous.kind) {
        case DIAMOND_TOKEN_INTEGER:
            return parse_integer(compiler);
        case DIAMOND_TOKEN_STRING:
            return parse_string(compiler);
        case DIAMOND_TOKEN_TRUE:
        case DIAMOND_TOKEN_FALSE:
        case DIAMOND_TOKEN_NIL:
            return parse_literal(compiler);
        case DIAMOND_TOKEN_IDENTIFIER:
            return parse_name(compiler);
        case DIAMOND_TOKEN_INSTANCE_VARIABLE: {
            const int field = field_index(compiler, compiler->previous.span, true);
            const uint8_t destination = allocate_register(compiler);
            emit_instruction(compiler, DIAMOND_OP_GET_IVAR, destination, 0,
                             (uint8_t)field, 3);
            return destination;
        }
        case DIAMOND_TOKEN_SELF:
            if (compiler->current_class < 0) {
                fail(compiler, compiler->previous.span, "'self' used outside a method");
                return 0;
            }
            return 0;
        case DIAMOND_TOKEN_SUPER:
            return parse_super(compiler);
        case DIAMOND_TOKEN_LEFT_PAREN:
            return parse_grouping(compiler);
        case DIAMOND_TOKEN_LEFT_BRACKET:
            return parse_array(compiler);
        case DIAMOND_TOKEN_LEFT_BRACE:
            return parse_hash(compiler);
        case DIAMOND_TOKEN_MINUS: {
            const uint8_t operand = parse_precedence(compiler, PREC_PREFIX);
            const uint8_t destination = allocate_register(compiler);
            emit_instruction(compiler, DIAMOND_OP_NEGATE_INT, destination,
                             operand, 0, 2);
            if(compiler->known_types[operand]==DIAMOND_TYPE_INT)
                compiler->known_types[destination]=DIAMOND_TYPE_INT;
            return destination;
        }
        case DIAMOND_TOKEN_BANG: {
            const uint8_t operand=parse_precedence(compiler,PREC_PREFIX);
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,operand,0,2);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            return destination;
        }
        case DIAMOND_TOKEN_IF:
            return parse_if(compiler);
        case DIAMOND_TOKEN_WHILE:
            return parse_while(compiler);
        case DIAMOND_TOKEN_BEGIN:
            return compile_begin(compiler);
        default:
            fail(compiler, compiler->previous.span, "expected expression");
            return 0;
    }
}

static DiamondOpCode binary_opcode(DiamondTokenKind operator) {
    switch (operator) {
        case DIAMOND_TOKEN_PLUS: return DIAMOND_OP_ADD;
        case DIAMOND_TOKEN_MINUS: return DIAMOND_OP_SUBTRACT_INT;
        case DIAMOND_TOKEN_STAR: return DIAMOND_OP_MULTIPLY_INT;
        case DIAMOND_TOKEN_SLASH: return DIAMOND_OP_DIVIDE_INT;
        case DIAMOND_TOKEN_EQUAL_EQUAL: return DIAMOND_OP_EQUAL;
        case DIAMOND_TOKEN_BANG_EQUAL: return DIAMOND_OP_NOT_EQUAL;
        case DIAMOND_TOKEN_LESS: return DIAMOND_OP_LESS_INT;
        case DIAMOND_TOKEN_LESS_EQUAL: return DIAMOND_OP_LESS_EQUAL_INT;
        case DIAMOND_TOKEN_GREATER: return DIAMOND_OP_GREATER_INT;
        case DIAMOND_TOKEN_GREATER_EQUAL: return DIAMOND_OP_GREATER_EQUAL_INT;
        default: return DIAMOND_OP_ADD;
    }
}

static uint8_t parse_precedence(Compiler *compiler, Precedence precedence) {
    uint8_t left = parse_prefix(compiler);
    while (!compiler->failed &&
           (compiler->current.kind == DIAMOND_TOKEN_DOT ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACKET)) {
        left = compiler->current.kind == DIAMOND_TOKEN_DOT
            ? parse_invoke(compiler,left) : parse_index(compiler,left);
    }
    while (!compiler->failed &&
           token_precedence(compiler->current.kind) >= precedence) {
        const DiamondTokenKind operator = compiler->current.kind;
        const Precedence operator_precedence = token_precedence(operator);
        advance_token(compiler);
        if(operator==DIAMOND_TOKEN_IS) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,"expected type after 'is'");
                return left;
            }
            const uint8_t tested_type=(uint8_t)resolve_type(
                compiler,compiler->current.span);
            advance_token(compiler);
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,destination,left,
                             tested_type,3);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            if(compiler->known_type_sets[left]>=0) {
                int16_t matching=-1,remaining=-1;
                if(split_type_set(compiler,
                   (uint8_t)compiler->known_type_sets[left],tested_type,
                   &matching,&remaining))
                    compiler->narrowing=(Narrowing){.valid=true,
                        .condition=destination,.reg=left,
                        .when_true=matching,.when_false=remaining};
            }
            left=destination;continue;
        }
        if(operator==DIAMOND_TOKEN_AND_AND || operator==DIAMOND_TOKEN_OR_OR) {
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                operator==DIAMOND_TOKEN_AND_AND ? DIAMOND_OP_JUMP_IF_FALSE
                                                : DIAMOND_OP_JUMP_IF_TRUE,
                left);
            const uint8_t right=parse_precedence(
                compiler,(Precedence)(operator_precedence+1));
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
            patch_jump(compiler,end_jump,compiler->function->code_count);
            if(compiler->known_types[left]==compiler->known_types[right])
                compiler->known_types[destination]=compiler->known_types[left];
            left=destination;
            continue;
        }
        const uint8_t right = parse_precedence(
            compiler, (Precedence)(operator_precedence + 1));
        const uint8_t destination = allocate_register(compiler);
        emit_instruction(compiler, binary_opcode(operator), destination,
                         left, right, 3);
        if(operator==DIAMOND_TOKEN_EQUAL_EQUAL || operator==DIAMOND_TOKEN_BANG_EQUAL ||
           operator==DIAMOND_TOKEN_LESS || operator==DIAMOND_TOKEN_LESS_EQUAL ||
           operator==DIAMOND_TOKEN_GREATER || operator==DIAMOND_TOKEN_GREATER_EQUAL) {
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            if(operator==DIAMOND_TOKEN_EQUAL_EQUAL||operator==DIAMOND_TOKEN_BANG_EQUAL) {
                uint8_t narrowed=left,nil_value=right;
                if(compiler->known_types[left]==DIAMOND_TYPE_NIL) {
                    narrowed=right;nil_value=left;
                }
                if(compiler->known_types[nil_value]==DIAMOND_TYPE_NIL&&
                   compiler->known_type_sets[narrowed]>=0) {
                    int16_t non_nil=-1,nil_only=-1;
                    if(split_nil_type_set(compiler,
                       (uint8_t)compiler->known_type_sets[narrowed],
                       &non_nil,&nil_only)) {
                        compiler->narrowing=(Narrowing){.valid=true,
                            .condition=destination,.reg=narrowed,
                            .when_true=operator==DIAMOND_TOKEN_BANG_EQUAL
                                ?non_nil:nil_only,
                            .when_false=operator==DIAMOND_TOKEN_BANG_EQUAL
                                ?nil_only:non_nil};
                    }
                }
            }
        } else if(compiler->known_types[left]==DIAMOND_TYPE_INT &&
                  compiler->known_types[right]==DIAMOND_TYPE_INT) {
            compiler->known_types[destination]=DIAMOND_TYPE_INT;
        } else if(operator==DIAMOND_TOKEN_PLUS &&
                  compiler->known_types[left]==DIAMOND_TYPE_STRING &&
                  compiler->known_types[right]==DIAMOND_TYPE_STRING) {
            compiler->known_types[destination]=DIAMOND_TYPE_STRING;
        }
        left = destination;
    }
    return left;
}

static uint8_t parse_expression(Compiler *compiler) {
    return parse_precedence(compiler, PREC_OR);
}

static bool assignment_ahead(const Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER &&
        compiler->current.kind != DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        return false;
    }
    DiamondLexer lookahead = compiler->lexer;
    return diamond_lexer_next(&lookahead).kind == DIAMOND_TOKEN_EQUAL;
}

static bool index_assignment_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) return false;
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=diamond_lexer_next(&lookahead);
    if(token.kind!=DIAMOND_TOKEN_LEFT_BRACKET) return false;
    size_t depth=1;
    while(depth>0) {
        token=diamond_lexer_next(&lookahead);
        if(token.kind==DIAMOND_TOKEN_EOF || token.kind==DIAMOND_TOKEN_ERROR)
            return false;
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) depth++;
        if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET) depth--;
    }
    return diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_EQUAL;
}

static uint8_t compile_index_assignment(Compiler *compiler) {
    const DiamondSpan name=compiler->current.span;
    const int local=find_local(compiler,name);
    if(local<0) { fail(compiler,name,"undefined local variable"); return 0; }
    uint8_t receiver=compiler->locals[(size_t)local].reg;
    if(compiler->locals[(size_t)local].captured) {
        const uint8_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
        receiver=loaded;
    }
    advance_token(compiler);
    advance_token(compiler);
    const uint8_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after assignment index");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_EQUAL) {
        fail(compiler,compiler->current.span,"expected '=' after indexed target");
        return 0;
    }
    advance_token(compiler);
    const uint8_t value=parse_expression(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,value,3);
    return value;
}

static uint8_t compile_return(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    if(!compiler->in_function) {
        fail(compiler,keyword,"'return' used outside a function");
        return 0;
    }
    advance_token(compiler);
    uint8_t value=0;
    if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE ||
       compiler->current.kind==DIAMOND_TOKEN_END ||
       compiler->current.kind==DIAMOND_TOKEN_ELSE ||
       compiler->current.kind==DIAMOND_TOKEN_EOF) {
        value=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,value,0,0,1);
        compiler->known_types[value]=DIAMOND_TYPE_NIL;
    } else {
        value=parse_expression(compiler);
    }
    if(compiler->current_return_type>=0)
        emit_type_check(compiler,value,(uint8_t)compiler->current_return_type,
                        compiler->current_return_type_span);
    emit_instruction(compiler,DIAMOND_OP_RETURN,value,0,0,1);
    return value;
}

static uint8_t compile_raise(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE ||
       compiler->current.kind==DIAMOND_TOKEN_END ||
       compiler->current.kind==DIAMOND_TOKEN_EOF) {
        fail(compiler,keyword,"'raise' requires a value");return 0;
    }
    const uint8_t value=parse_expression(compiler);
    emit_instruction(compiler,DIAMOND_OP_RAISE,value,0,0,1);
    return value;
}

static uint8_t compile_begin(Compiler *compiler) {
    if(!consume_block_start(compiler))return 0;
    const size_t ensure_operand=compiler->function->code_count+1;
    emit_opcode(compiler,DIAMOND_OP_PUSH_ENSURE);
    emit_byte(compiler,0);emit_byte(compiler,0);
    const uint8_t exception=allocate_register(compiler);
    const size_t handler_type_operand=compiler->function->code_count+2;
    const size_t handler_types_operand=compiler->function->code_count+3;
    const size_t handler_operand=compiler->function->code_count+11;
    emit_opcode(compiler,DIAMOND_OP_PUSH_RESCUE);
    emit_byte(compiler,exception);emit_byte(compiler,0x80);
    for(size_t i=0;i<8;i++)emit_byte(compiler,0);
    emit_byte(compiler,0);emit_byte(compiler,0);
    const uint8_t body=compile_sequence(compiler);
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,body,0,2);
    emit_instruction(compiler,DIAMOND_OP_POP_RESCUE,0,0,0,0);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,handler_operand,compiler->function->code_count);
    if(compiler->current.kind==DIAMOND_TOKEN_RESCUE) {
        advance_token(compiler);
        compiler->function->code[handler_type_operand]=0;
        const size_t rescue_local_count=compiler->local_count;
        if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER) {
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->current.span,"too many local variables");
                return destination;
            }
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->current.span,.reg=exception};
            advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                size_t type_count=0;
                while(!compiler->failed) {
                    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                        fail(compiler,compiler->current.span,"expected rescue type");break;
                    }
                    if(type_count==8) {
                        fail(compiler,compiler->current.span,"too many rescue types");break;
                    }
                    compiler->function->code[handler_types_operand+type_count++]=
                        (uint8_t)resolve_type(compiler,compiler->current.span);
                    advance_token(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
                    advance_token(compiler);
                }
                compiler->function->code[handler_type_operand]=(uint8_t)type_count;
            }
        }
        if(!consume_block_start(compiler))return destination;
        const uint8_t rescued=compile_sequence(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,rescued,0,2);
        compiler->local_count=rescue_local_count;
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_ENSURE &&
       compiler->function->code[handler_type_operand]==0x80) {
        fail(compiler,compiler->current.span,"expected 'rescue' or 'ensure' after begin body");
        return destination;
    }
    patch_jump(compiler,end_jump,compiler->function->code_count);
    emit_opcode(compiler,DIAMOND_OP_RUN_ENSURE);
    const size_t continuation_operand=compiler->function->code_count;
    emit_byte(compiler,0);emit_byte(compiler,0);
    patch_jump(compiler,ensure_operand,compiler->function->code_count);
    if(compiler->current.kind==DIAMOND_TOKEN_ENSURE) {
        advance_token(compiler);
        if(!consume_block_start(compiler))return destination;
        (void)compile_sequence(compiler);
    }
    emit_instruction(compiler,DIAMOND_OP_END_ENSURE,0,0,0,0);
    if(compiler->current.kind!=DIAMOND_TOKEN_END) {
        fail(compiler,compiler->current.span,"expected 'end' after begin body");
        return destination;
    }
    advance_token(compiler);
    patch_jump(compiler,continuation_operand,compiler->function->code_count);
    return destination;
}

static uint8_t compile_loop_control(Compiler *compiler) {
    const DiamondTokenKind kind=compiler->current.kind;
    const DiamondSpan keyword=compiler->current.span;
    if(compiler->current_loop==nullptr) {
        fail(compiler,keyword,kind==DIAMOND_TOKEN_BREAK
            ? "'break' used outside a loop" : "'next' used outside a loop");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_NEWLINE &&
       compiler->current.kind!=DIAMOND_TOKEN_END &&
       compiler->current.kind!=DIAMOND_TOKEN_ELSE &&
       compiler->current.kind!=DIAMOND_TOKEN_EOF) {
        fail(compiler,compiler->current.span,
             "break and next do not accept values yet");
        return 0;
    }
    if(kind==DIAMOND_TOKEN_BREAK) {
        if(compiler->current_loop->break_count==64) {
            fail(compiler,keyword,"too many break statements in loop");
            return 0;
        }
        compiler->current_loop->breaks[compiler->current_loop->break_count++]=
            emit_jump(compiler,DIAMOND_OP_JUMP,0);
    } else {
        emit_absolute_jump(compiler,compiler->current_loop->continue_target);
    }
    const uint8_t result=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,result,0,0,1);
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}

static uint8_t compile_definition(Compiler *compiler) {
    const bool at_top_level = compiler->function == &compiler->program->entry;
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler, compiler->current.span, "expected function name after 'def'");
        return 0;
    }
    if (compiler->program->function_count == DIAMOND_MAX_FUNCTIONS) {
        fail(compiler, compiler->current.span, "too many functions");
        return 0;
    }
    const DiamondSpan name = compiler->current.span;
    if (name.length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "function name is too long");
        return 0;
    }
    if (compiler->current_class < 0 && find_function(compiler, name) >= 0) {
        fail(compiler, name, "function is already defined");
        return 0;
    }
    DiamondFunction *function =
        &compiler->program->functions[compiler->program->function_count++];
    function->return_type_set=UINT8_MAX;
    for(size_t index=0;index<16;index++)
        function->parameter_type_sets[index]=UINT8_MAX;
    const size_t function_index = compiler->program->function_count - 1;
    function->owner_class = compiler->current_class < 0
        ? UINT8_MAX : (uint8_t)compiler->current_class;
    function->nested=!at_top_level;
    const size_t copy_length = name.length;
    for (size_t index = 0; index < copy_length; index++) {
        function->name[index] = compiler->source[name.start + index];
    }
    function->name[copy_length] = '\0';
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after function name");
        return 0;
    }
    advance_token(compiler);

    DiamondFunction *outer_function = compiler->function;
    Local outer_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_local_count = compiler->local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        outer_locals[index] = compiler->locals[index];
    }
    const uint16_t outer_next_register = compiler->next_register;
    const DiamondSpan outer_method = compiler->current_method;
    const bool outer_in_method = compiler->in_method;
    const bool outer_in_function=compiler->in_function;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    LoopContext *outer_loop=compiler->current_loop;
    Local outer_enclosing_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_enclosing_local_count=compiler->enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        outer_enclosing_locals[i]=compiler->enclosing_locals[i];
    uint8_t outer_capture_registers[16];
    const size_t outer_capture_count=compiler->capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        outer_capture_registers[i]=compiler->capture_registers[i];
    uint8_t outer_known_types[256];
    int16_t outer_known_type_sets[256];
    for(size_t index=0;index<256;index++)
        {outer_known_types[index]=compiler->known_types[index];
         outer_known_type_sets[index]=compiler->known_type_sets[index];}
    compiler->function = function;
    compiler->current_loop=nullptr;
    compiler->local_count = 0;
    compiler->next_register = 0;
    compiler->enclosing_local_count=at_top_level ? 0 : outer_local_count;
    for(size_t i=0;i<compiler->enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_locals[i];
    compiler->capture_count=0;
    if(!at_top_level) {
        if(compiler->enclosing_local_count>16) {
            fail(compiler,name,"nested function sees too many lexical bindings");
        } else {
            compiler->capture_count=compiler->enclosing_local_count;
            for(size_t i=0;i<compiler->capture_count;i++)
                compiler->capture_registers[i]=compiler->enclosing_locals[i].reg;
        }
    }
    if (compiler->current_class >= 0) {
        (void)allocate_register(compiler);
        function->arity = 1;
        compiler->current_method = name;
        compiler->in_method = true;
    }

    size_t declared_parameter_count=0;
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler, compiler->current.span, "expected parameter name");
                break;
            }
            if (function->arity == UINT8_MAX) {
                fail(compiler, compiler->current.span, "too many parameters");
                break;
            }
            const uint8_t parameter = define_local(compiler, compiler->current.span);
            function->arity++;
            advance_token(compiler);
            if (compiler->current.kind == DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                const int type = parse_type_annotation(compiler);
                if(declared_parameter_count<16)
                    function->parameter_type_sets[declared_parameter_count]=
                        (uint8_t)type;
                emit_type_check(compiler,parameter,(uint8_t)type,
                                compiler->previous.span);
                compiler->known_type_sets[parameter]=(int16_t)type;
                const DiamondTypeSet *parameter_set=&function->type_sets[(size_t)type];
                if(parameter_set->count==1)
                    compiler->known_types[parameter]=parameter_set->members[0].id;
            }
            declared_parameter_count++;
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        } while (!compiler->failed);
    }
    if (!compiler->failed && compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after parameters");
    }
    if(!compiler->failed && !at_top_level) {
        for(size_t i=0;i<compiler->enclosing_local_count;i++) {
            if(find_local(compiler,compiler->enclosing_locals[i].name)>=0)continue;
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->enclosing_locals[i].name,"too many lexical bindings");break;
            }
            const uint8_t cell=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE_CELL,cell,(uint8_t)i,0,2);
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->enclosing_locals[i].name,.reg=cell,.captured=true};
        }
    }
    int return_type = -1;
    DiamondSpan return_type_span = {};
    if (!compiler->failed) advance_token(compiler);
    if (!compiler->failed && compiler->current.kind == DIAMOND_TOKEN_ARROW) {
        advance_token(compiler);
        return_type_span=compiler->current.span;
        return_type = parse_type_annotation(compiler);
        function->return_type_set=(uint8_t)return_type;
    }
    compiler->in_function=true;
    compiler->current_return_type=return_type;
    compiler->current_return_type_span=return_type_span;
    const bool endless=!compiler->failed&&
        compiler->current.kind==DIAMOND_TOKEN_EQUAL;
    uint8_t body_result=0;
    if(endless) {
        advance_token(compiler);
        body_result=parse_expression(compiler);
    } else {
        if(!compiler->failed)(void)consume_block_start(compiler);
        body_result=compiler->failed?0:compile_sequence(compiler);
    }
    if (!compiler->failed) {
        if (return_type >= 0) {
            emit_type_check(compiler,body_result,(uint8_t)return_type,
                            return_type_span);
        }
        emit_instruction(compiler, DIAMOND_OP_RETURN, body_result, 0, 0, 1);
        if (!endless&&compiler->current.kind != DIAMOND_TOKEN_END) {
            fail(compiler, compiler->current.span, "expected 'end' after function body");
        } else if(!endless) {
            advance_token(compiler);
        }
    }

    function->capture_count=(uint8_t)compiler->capture_count;
    uint8_t captures[16];
    for(size_t i=0;i<compiler->capture_count;i++)captures[i]=compiler->capture_registers[i];
    const size_t capture_count=compiler->capture_count;
    compiler->function = outer_function;
    compiler->local_count = outer_local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        compiler->locals[index] = outer_locals[index];
    }
    compiler->next_register = outer_next_register;
    compiler->current_method = outer_method;
    compiler->in_method = outer_in_method;
    compiler->in_function=outer_in_function;
    compiler->current_return_type=outer_return_type;
    compiler->current_return_type_span=outer_return_type_span;
    compiler->current_loop=outer_loop;
    compiler->enclosing_local_count=outer_enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_enclosing_locals[i];
    compiler->capture_count=outer_capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        compiler->capture_registers[i]=outer_capture_registers[i];
    for(size_t index=0;index<256;index++)
        {compiler->known_types[index]=outer_known_types[index];
         compiler->known_type_sets[index]=outer_known_type_sets[index];}
    if (compiler->current_class >= 0 && !compiler->failed && at_top_level) {
        DiamondClass *class = &compiler->program->classes[(size_t)compiler->current_class];
        if (class->method_count == DIAMOND_MAX_METHODS ||
            find_method(compiler, compiler->current_class, name) >= 0) {
            fail(compiler, name, "duplicate or excessive method definition");
        } else {
            DiamondMethod *method = &class->methods[class->method_count++];
            for (size_t i=0;i<copy_length;i++) method->name[i]=function->name[i];
            method->name[copy_length]='\0';
            method->function_index=(uint8_t)function_index;
            method->arity=(uint8_t)(function->arity-1);
        }
    }
    const uint8_t result = allocate_register(compiler);
    if(!at_top_level) {
        for(size_t i=0;i<capture_count;i++) {
            for(size_t local=0;local<compiler->local_count;local++) {
                if(compiler->locals[local].reg!=captures[i])continue;
                if(!compiler->locals[local].captured) {
                    emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,captures[i],0,0,1);
                    compiler->locals[local].captured=true;
                }
                break;
            }
        }
        emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_byte(compiler,result);
        emit_byte(compiler,(uint8_t)function_index);emit_byte(compiler,(uint8_t)capture_count);
        for(size_t i=0;i<capture_count;i++)emit_byte(compiler,captures[i]);
        compiler->locals[compiler->local_count++]=(Local){.name=name,.reg=result};
    } else emit_instruction(compiler, DIAMOND_OP_NIL, result, 0, 0, 1);
    return result;
}

static uint8_t compile_class(Compiler *compiler) {
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER ||
        compiler->program->class_count == DIAMOND_MAX_CLASSES) {
        fail(compiler, compiler->current.span, "expected valid class name"); return 0;
    }
    DiamondSpan name=compiler->current.span;
    if(name.length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,name,"class name is too long"); return 0;
    }
    if (find_class(compiler,name)>=0||find_interface(compiler,name)>=0) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    const int index=(int)compiler->program->class_count++;
    DiamondClass *class=&compiler->program->classes[(size_t)index];
    class->superclass=UINT8_MAX;
    for(size_t i=0;i<name.length;i++) class->name[i]=compiler->source[name.start+i];
    class->name[name.length]='\0';
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_LESS) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected superclass name after '<'"); return 0;
        }
        const int parent=find_class(compiler,compiler->current.span);
        if(parent<0) { fail(compiler,compiler->current.span,"undefined superclass"); return 0; }
        class->superclass=(uint8_t)parent;
        const DiamondClass *parent_class=&compiler->program->classes[(size_t)parent];
        class->field_count=parent_class->field_count;
        for(size_t field=0;field<parent_class->field_count;field++)
            for(size_t ch=0;ch<DIAMOND_MAX_FUNCTION_NAME;ch++)
                class->fields[field][ch]=parent_class->fields[field][ch];
        advance_token(compiler);
    }
    if(!consume_block_start(compiler)) return 0;
    const int outer=compiler->current_class; compiler->current_class=index;
    while(!compiler->failed && compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind!=DIAMOND_TOKEN_DEF) {
            fail(compiler,compiler->current.span,"expected method definition in class"); break;
        }
        (void)compile_definition(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE) skip_newlines(compiler);
    }
    compiler->current_class=outer;
    if(compiler->current.kind==DIAMOND_TOKEN_END) advance_token(compiler);
    const uint8_t result=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,result,0,0,1); return result;
}

static uint8_t compile_interface(Compiler *compiler) {
    if(compiler->function!=&compiler->program->entry) {
        fail(compiler,compiler->current.span,
             "interfaces must be declared at top level");return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       compiler->program->interface_count==DIAMOND_MAX_INTERFACES) {
        fail(compiler,compiler->current.span,"expected valid interface name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    if(name.length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,name,"interface name is too long");return 0;
    }
    if(find_interface(compiler,name)>=0||find_class(compiler,name)>=0) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    DiamondInterface *interface=
        &compiler->program->interfaces[compiler->program->interface_count++];
    for(size_t index=0;index<name.length;index++)
        interface->name[index]=compiler->source[name.start+index];
    interface->name[name.length]='\0';
    advance_token(compiler);
    if(!consume_block_start(compiler))return 0;
    while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind!=DIAMOND_TOKEN_DEF||
           interface->method_count==DIAMOND_MAX_METHODS) {
            fail(compiler,compiler->current.span,
                 "expected method signature in interface");break;
        }
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected interface method name");break;
        }
        const DiamondSpan method_name=compiler->current.span;
        for(size_t index=0;index<interface->method_count;index++)
            if(name_equals(compiler,interface->methods[index].name,method_name,false)) {
                fail(compiler,method_name,"duplicate interface method");break;
            }
        if(compiler->failed)break;
        DiamondInterfaceMethod *method=&interface->methods[interface->method_count++];
        method->return_type_set=UINT8_MAX;
        for(size_t index=0;index<16;index++)
            method->parameter_type_sets[index]=UINT8_MAX;
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
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||method->arity==16) {
                fail(compiler,compiler->current.span,"expected interface parameter");break;
            }
            method->arity++;advance_token(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                method->parameter_type_sets[method->arity-1]=
                    (uint8_t)parse_type_annotation(compiler);
            }
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,"expected ')' after interface parameters");break;
        }
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_ARROW) {
            advance_token(compiler);
            method->return_type_set=(uint8_t)parse_type_annotation(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_NEWLINE&&
           compiler->current.kind!=DIAMOND_TOKEN_END) {
            fail(compiler,compiler->current.span,"expected newline after method signature");break;
        }
        skip_newlines(compiler);
    }
    if(compiler->current.kind==DIAMOND_TOKEN_END)advance_token(compiler);
    const uint8_t result=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,result,0,0,1);return result;
}

static uint8_t compile_assignment(Compiler *compiler) {
    const DiamondSpan name = compiler->current.span;
    const bool instance_variable =
        compiler->current.kind == DIAMOND_TOKEN_INSTANCE_VARIABLE;
    advance_token(compiler);
    advance_token(compiler);
    const uint8_t value = parse_expression(compiler);
    if (instance_variable) {
        const int field = field_index(compiler, name, true);
        emit_instruction(compiler, DIAMOND_OP_SET_IVAR, 0, (uint8_t)field,
                         value, 3);
        return value;
    }
    int local = find_local(compiler, name);
    if(local>=0 && compiler->locals[(size_t)local].captured) {
        emit_instruction(compiler,DIAMOND_OP_SET_CELL,
                         compiler->locals[(size_t)local].reg,value,0,2);
        return value;
    }
    if(local<0) {
        for(size_t i=compiler->enclosing_local_count;i>0;i--) {
            if(!spans_equal(compiler,compiler->enclosing_locals[i-1].name,name))continue;
            size_t capture=0;
            while(capture<compiler->capture_count &&
                  compiler->capture_registers[capture]!=compiler->enclosing_locals[i-1].reg)capture++;
            if(capture==compiler->capture_count) {
                if(capture==16){fail(compiler,name,"too many captured variables");return 0;}
                compiler->capture_registers[compiler->capture_count++]=compiler->enclosing_locals[i-1].reg;
            }
            emit_instruction(compiler,DIAMOND_OP_SET_CAPTURE,(uint8_t)capture,value,0,2);
            return value;
        }
    }
    const uint8_t destination = local < 0
        ? define_local(compiler, name)
        : compiler->locals[(size_t)local].reg;
    emit_instruction(compiler, DIAMOND_OP_MOVE, destination, value, 0, 2);
    compiler->known_types[destination]=compiler->known_types[value];
    compiler->known_type_sets[destination]=compiler->known_type_sets[value];
    return destination;
}

static bool at_block_end(const Compiler *compiler) {
    return compiler->current.kind == DIAMOND_TOKEN_EOF ||
           compiler->current.kind == DIAMOND_TOKEN_ELSE ||
           compiler->current.kind == DIAMOND_TOKEN_RESCUE ||
           compiler->current.kind == DIAMOND_TOKEN_ENSURE ||
           compiler->current.kind == DIAMOND_TOKEN_END;
}

static uint8_t compile_sequence(Compiler *compiler) {
    skip_newlines(compiler);
    uint8_t result = allocate_register(compiler);
    emit_instruction(compiler, DIAMOND_OP_NIL, result, 0, 0, 1);

    while (!compiler->failed && !at_block_end(compiler)) {
        if (compiler->current.kind == DIAMOND_TOKEN_DEF) {
            result = compile_definition(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_CLASS) {
            result = compile_class(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_INTERFACE) {
            result = compile_interface(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RETURN) {
            result=compile_return(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RAISE) {
            result=compile_raise(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_BEGIN) {
            advance_token(compiler);
            result=compile_begin(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_BREAK ||
                   compiler->current.kind == DIAMOND_TOKEN_NEXT) {
            result=compile_loop_control(compiler);
        } else {
            if(index_assignment_ahead(compiler))
                result=compile_index_assignment(compiler);
            else
                result = assignment_ahead(compiler)
                    ? compile_assignment(compiler)
                    : parse_expression(compiler);
        }
        if (compiler->current.kind == DIAMOND_TOKEN_NEWLINE) {
            skip_newlines(compiler);
        } else if (!at_block_end(compiler)) {
            fail(compiler, compiler->current.span, "expected newline after expression");
        }
    }
    return result;
}

bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic) {
    *program = (DiamondProgram){};
    static const struct {
        const char *name;
        uint8_t superclass;
    } builtins[DIAMOND_BUILTIN_CLASS_COUNT] = {
        [DIAMOND_CLASS_EXCEPTION]={"Exception",UINT8_MAX},
        [DIAMOND_CLASS_STANDARD_ERROR]={"StandardError",DIAMOND_CLASS_EXCEPTION},
        [DIAMOND_CLASS_RUNTIME_ERROR]={"RuntimeError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_TYPE_ERROR]={"TypeError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_ARGUMENT_ERROR]={"ArgumentError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_INDEX_ERROR]={"IndexError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_ZERO_DIVISION_ERROR]={"ZeroDivisionError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_RANGE_ERROR]={"RangeError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_SYSTEM_STACK_ERROR]={"SystemStackError",DIAMOND_CLASS_EXCEPTION},
    };
    program->class_count=DIAMOND_BUILTIN_CLASS_COUNT;
    for(size_t index=0;index<DIAMOND_BUILTIN_CLASS_COUNT;index++) {
        DiamondClass *class=&program->classes[index];
        (void)snprintf(class->name,sizeof class->name,"%s",builtins[index].name);
        class->superclass=builtins[index].superclass;
    }
    snprintf(program->entry.name, sizeof(program->entry.name), "<main>");
    *diagnostic = (DiamondDiagnostic){};
    Compiler compiler = {
        .source = source,
        .program = program,
        .function = &program->entry,
        .current_class = -1,
        .current_return_type = -1,
        .diagnostic = diagnostic,
    };
    diamond_lexer_init(&compiler.lexer, source);
    compiler.current = diamond_lexer_next(&compiler.lexer);
    if(compiler.current.kind==DIAMOND_TOKEN_ERROR)
        fail(&compiler,compiler.current.span,"unexpected character");

    const uint8_t result = compile_sequence(&compiler);
    if (!compiler.failed && compiler.current.kind != DIAMOND_TOKEN_EOF) {
        fail(&compiler, compiler.current.span, "unexpected block terminator");
    }
    if (!compiler.failed) {
        emit_instruction(&compiler, DIAMOND_OP_RETURN, result, 0, 0, 1);
        for(size_t class_index=0;class_index<program->class_count;class_index++) {
            DiamondClass *class=&program->classes[class_index];
            for(size_t field_count=0;field_count<=class->field_count;field_count++) {
                class->shapes[field_count]=(DiamondShape){
                    .class=class,.field_count=(uint8_t)field_count};
            }
        }
    }
    return !compiler.failed;
}

DiamondChunk diamond_program_chunk(const DiamondProgram *program) {
    return (DiamondChunk){
        .name = program->entry.name,
        .code = program->entry.code,
        .lines = program->entry.lines,
        .columns = program->entry.columns,
        .code_count = program->entry.code_count,
        .constants = program->entry.constants,
        .constant_count = program->entry.constant_count,
        .strings = program->entry.strings,
        .string_count = program->entry.string_count,
        .type_sets = program->entry.type_sets,
        .type_set_count = program->entry.type_set_count,
        .functions = program->functions,
        .function_count = program->function_count,
        .classes = program->classes,
        .class_count = program->class_count,
        .interfaces=program->interfaces,
        .interface_count=program->interface_count,
    };
}
