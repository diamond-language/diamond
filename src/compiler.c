#include "compiler.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
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
    size_t redo_target;
    uint8_t result_register;
    size_t breaks[64];
    size_t break_count;
} LoopContext;

enum { DIAMOND_MAX_NARROWING_FACTS = 8 };

typedef struct NarrowingFact {
    uint8_t reg;
    int16_t type_set;
} NarrowingFact;

/* A pending narrowing, keyed by the register holding the boolean
 * condition it came from (`condition`). when_true/when_false hold the
 * per-register facts that are known if the condition turns out
 * true/false, respectively. A plain `is`/nil-check produces exactly
 * one fact in each array; `&&`/`||` compose facts from both operands
 * into these same arrays (see the AND/OR handling in
 * parse_precedence) rather than needing a separate mechanism. */
typedef struct Narrowing {
    bool valid;
    uint8_t condition;
    NarrowingFact when_true[DIAMOND_MAX_NARROWING_FACTS];
    size_t when_true_count;
    NarrowingFact when_false[DIAMOND_MAX_NARROWING_FACTS];
    size_t when_false_count;
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
    int current_module;
    bool methods_private;
    bool module_function_mode;
    DiamondSpan current_method;
    bool in_method;
    /* True while compiler->function is itself a class/module singleton
     * method (`def self.name`). Tracked separately from in_method (which
     * covers ordinary instance methods too) because it drives whether a
     * closure nested *directly* inside this function should get the same
     * self-register-reservation/owner_class treatment an instance method
     * gets, even though the closure itself isn't a class member -- see
     * compile_definition's own use, and docs/roadmap.md for why. */
    bool in_singleton_method;
    uint8_t known_types[256];
    int16_t known_type_sets[256];
    bool in_function;
    int current_return_type;
    DiamondSpan current_return_type_span;
    LoopContext *current_loop;
    int current_exception;
    size_t current_retry_target;
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
static uint8_t compile_yield(Compiler *compiler);
static uint8_t compile_interface(Compiler *compiler);

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

/* Big-endian, matching patch_jump/emit_absolute_jump's existing 16-bit
 * operand convention -- function indices are a CALL/CALL_TYPED/CLOSURE
 * operand wide enough to exceed one byte now that DIAMOND_MAX_FUNCTIONS
 * is 512 (see its own comment in src/vm.h). */
static bool emit_function_index(Compiler *compiler, size_t function_index) {
    return emit_byte(compiler, (uint8_t)(function_index >> 8)) &&
           emit_byte(compiler, (uint8_t)(function_index & UINT8_MAX));
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
    if(expected>=DIAMOND_TYPE_VARIABLE_BASE&&
       expected<DIAMOND_TYPE_INTERFACE_BASE)return true;
    if(known>=DIAMOND_TYPE_VARIABLE_BASE&&known<DIAMOND_TYPE_INTERFACE_BASE)
        return false;
    if(expected>=DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t interface_index=(size_t)(expected-DIAMOND_TYPE_INTERFACE_BASE);
        if(interface_index>=compiler->program->interface_count)return false;
        const DiamondInterface *interface=&compiler->program->interfaces[interface_index];
        if(known==DIAMOND_TYPE_STRING||known==DIAMOND_TYPE_ARRAY||
           known==DIAMOND_TYPE_HASH) {
            for(size_t required=0;required<interface->method_count;required++) {
                const DiamondInterfaceMethod *method=&interface->methods[required];
                uint8_t native_return=UINT8_MAX;
                if(!diamond_native_method_satisfies(known,method->name,
                    method->arity,&native_return))return false;
                if(method->return_type_set!=UINT8_MAX) {
                    if(native_return==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
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
                       interface->methods[required].arity>=
                           class->methods[method].required_arity&&
                       interface->methods[required].arity<=class->methods[method].arity) {
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
        if(expected.callable_parameters_typed)
            for(size_t parameter=0;parameter<expected.callable_arity;parameter++) {
                const uint8_t wanted=expected.callable_parameter_sets[parameter];
                if(wanted==UINT8_MAX)continue;
                const uint8_t actual=known.callable_parameter_sets[parameter];
                if(actual!=UINT8_MAX&&
                   !type_sets_satisfy_across(compiler,expected_sets,wanted,
                                              known_sets,actual))return false;
            }
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
        if(expected.callable_parameters_typed)
            for(size_t parameter=0;parameter<expected.callable_arity;parameter++) {
                const uint8_t wanted=expected.callable_parameter_sets[parameter];
                if(wanted==UINT8_MAX)continue;
                const uint8_t actual=known.callable_parameter_sets[parameter];
                if(actual!=UINT8_MAX&&
                   !type_set_satisfies(compiler,wanted,actual))return false;
            }
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

static bool type_set_contains_variable(const Compiler *compiler,uint8_t set_index) {
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember member=set->members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           member.id<DIAMOND_TYPE_INTERFACE_BASE)return true;
        if(member.argument_set!=UINT8_MAX&&
           type_set_contains_variable(compiler,member.argument_set))return true;
        if(member.second_argument_set!=UINT8_MAX&&
           type_set_contains_variable(compiler,member.second_argument_set))return true;
        if(member.callable_return_set!=UINT8_MAX&&
           type_set_contains_variable(compiler,member.callable_return_set))return true;
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++)
                if(type_set_contains_variable(compiler,
                       member.callable_parameter_sets[parameter]))return true;
    }
    return false;
}

static void emit_type_check(Compiler *compiler, uint8_t reg, uint8_t set_index,
                            DiamondSpan span) {
    if(type_set_contains_variable(compiler,set_index)) {
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
        return;
    }
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

static uint8_t add_string_range(Compiler *compiler,size_t start,size_t length,
                                DiamondSpan span) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS) {
        fail(compiler, span, "function has too many string literals");
        return 0;
    }
    DiamondStringConstant *string =
        &compiler->function->strings[compiler->function->string_count];
    for(size_t index=0;index<length;index++) {
        char character=compiler->source[start+index];
        if (character == '\\') {
            index++;
            if(index==length) {fail(compiler,span,"incomplete string escape");return 0;}
            character=compiler->source[start+index];
            switch (character) {
                case 'n': character = '\n'; break;
                case 'r': character = '\r'; break;
                case 't': character = '\t'; break;
                case '"': character = '"'; break;
                case '\\': character = '\\'; break;
                case '#': character = '#'; break;
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

static uint8_t add_string(Compiler *compiler,DiamondSpan span) {
    return add_string_range(compiler,span.start+1,span.length-2,span);
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
        case DIAMOND_TOKEN_OR:
            return PREC_OR;
        case DIAMOND_TOKEN_AND_AND:
        case DIAMOND_TOKEN_AND:
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

static uint8_t parse_float(Compiler *compiler) {
    const DiamondSpan span = compiler->previous.span;
    char buffer[80];
    size_t length = 0;
    for (size_t index = 0; index < span.length; index++) {
        const char ch = compiler->source[span.start + index];
        if (ch == '_') continue;
        if (length >= sizeof(buffer) - 1) {
            fail(compiler, span, "float literal is too long");
            return 0;
        }
        buffer[length++] = ch;
    }
    buffer[length] = '\0';
    errno = 0;
    char *end = nullptr;
    const double value = strtod(buffer, &end);
    if (end != buffer + length ||
        (errno == ERANGE && (value == HUGE_VAL || value == -HUGE_VAL))) {
        fail(compiler, span, "float literal is too large");
        return 0;
    }
    const uint8_t destination = allocate_register(compiler);
    const uint8_t constant = add_constant(compiler, DIAMOND_FLOAT(value));
    emit_instruction(compiler, DIAMOND_OP_CONSTANT, destination, constant, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
    return destination;
}

static uint8_t parse_string(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    const size_t end=span.start+span.length-1;
    size_t piece=span.start+1;uint8_t result=UINT8_MAX;
    while(piece<end&&!compiler->failed) {
        size_t index=piece;
        while(index+1<end) {
            if(compiler->source[index]=='\\') {index+=2;continue;}
            if(compiler->source[index]=='#'&&compiler->source[index+1]=='{')break;
            index++;
        }
        if(index+1>=end)index=end;
        const uint8_t literal_register=allocate_register(compiler);
        const uint8_t literal=add_string_range(compiler,piece,index-piece,span);
        emit_instruction(compiler,DIAMOND_OP_STRING,literal_register,literal,0,2);
        if(result==UINT8_MAX)result=literal_register;
        else {
            const uint8_t joined=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ADD,joined,result,literal_register,3);
            result=joined;
        }
        if(index>=end)break;
        DiamondLexer outer_lexer=compiler->lexer;
        const DiamondToken outer_current=compiler->current;
        const DiamondToken outer_previous=compiler->previous;
        DiamondLexer embedded={.source=compiler->source,.current=index+2,
            .start=index+2,.line=span.line,.column=span.column+(index-span.start)+2,
            .token_line=span.line,.token_column=span.column+(index-span.start)+2};
        compiler->lexer=embedded;
        compiler->current=diamond_lexer_next(&compiler->lexer);
        const uint8_t value=parse_expression(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE)
            fail(compiler,compiler->current.span,"expected '}' after interpolation");
        const size_t close=compiler->current.span.start;
        compiler->lexer=outer_lexer;compiler->current=outer_current;
        compiler->previous=outer_previous;
        const uint8_t converted=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_TO_STRING,converted,value,0,2);
        const uint8_t joined=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_ADD,joined,result,converted,3);
        result=joined;piece=close+1;
    }
    if(result==UINT8_MAX) {
        result=allocate_register(compiler);
        const uint8_t string=add_string(compiler,span);
        emit_instruction(compiler,DIAMOND_OP_STRING,result,string,0,2);
    }
    compiler->known_types[result]=DIAMOND_TYPE_STRING;return result;
}

static uint8_t parse_symbol(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    const DiamondSpan name_span={.start=span.start+1,.length=span.length-1,
        .line=span.line,.column=span.column+1};
    const uint8_t destination=allocate_register(compiler);
    const uint8_t name=add_name_string(compiler,name_span);
    emit_instruction(compiler,DIAMOND_OP_SYMBOL,destination,name,0,2);
    compiler->known_types[destination]=DIAMOND_TYPE_SYMBOL;
    return destination;
}

static uint8_t parse_literal(Compiler *compiler) {
    const uint8_t destination = allocate_register(compiler);
    if (compiler->previous.kind == DIAMOND_TOKEN_NIL) {
        /* No NIL opcode needed: run_chunk already zero-inits every
         * register in [0, register_count), and this destination is a
         * sole writer (freshly allocated for this literal). */
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
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module].name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",scope,
                (int)name.length,compiler->source+name.start);
            if(written>0&&(size_t)written<sizeof qualified)
                for(size_t index=0;index<compiler->program->class_count;index++)
                    if(strcmp(compiler->program->classes[index].name,qualified)==0)
                        return (int)index;
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    for (size_t index = 0; index < compiler->program->class_count; index++) {
        if (name_equals(compiler, compiler->program->classes[index].name, name, false))
            return (int)index;
    }
    return -1;
}

static int find_interface(const Compiler *compiler,DiamondSpan name) {
    if(compiler->current_module>=0) {
        char qualified[DIAMOND_MAX_FUNCTION_NAME];
        const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",
            compiler->program->modules[(size_t)compiler->current_module].name,
            (int)name.length,compiler->source+name.start);
        if(written>0&&(size_t)written<sizeof qualified)
            for(size_t index=0;index<compiler->program->interface_count;index++)
                if(strcmp(compiler->program->interfaces[index].name,qualified)==0)
                    return (int)index;
    }
    for(size_t index=0;index<compiler->program->interface_count;index++)
        if(name_equals(compiler,compiler->program->interfaces[index].name,name,false))
            return (int)index;
    return -1;
}

static int find_module(const Compiler *compiler,DiamondSpan name) {
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module].name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",scope,
                (int)name.length,compiler->source+name.start);
            if(written>0&&(size_t)written<sizeof qualified)
                for(size_t index=0;index<compiler->program->module_count;index++)
                    if(strcmp(compiler->program->modules[index].name,qualified)==0)
                        return (int)index;
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    for(size_t index=0;index<compiler->program->module_count;index++)
        if(name_equals(compiler,compiler->program->modules[index].name,name,false))
            return (int)index;
    return -1;
}

static bool stored_name_equals(const char *stored,const char *name) {
    return strcmp(stored,name)==0;
}

static int find_class_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->class_count;index++)
        if(stored_name_equals(compiler->program->classes[index].name,name))
            return (int)index;
    return -1;
}

static int find_interface_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->interface_count;index++)
        if(stored_name_equals(compiler->program->interfaces[index].name,name))
            return (int)index;
    return -1;
}

static int find_module_name(const Compiler *compiler,const char *name) {
    for(size_t index=0;index<compiler->program->module_count;index++)
        if(stored_name_equals(compiler->program->modules[index].name,name))
            return (int)index;
    return -1;
}

static int find_namespace_constant_name(const Compiler *compiler,
                                        const char *name) {
    for(size_t index=0;index<compiler->program->namespace_constant_count;index++)
        if(strcmp(compiler->program->namespace_constants[index],name)==0)
            return (int)index;
    return -1;
}

static int find_namespace_constant(const Compiler *compiler,DiamondSpan name) {
    if(compiler->current_module<0)return -1;
    char scope[DIAMOND_MAX_FUNCTION_NAME];
    (void)snprintf(scope,sizeof scope,"%s",
        compiler->program->modules[(size_t)compiler->current_module].name);
    while(true) {
        char qualified[DIAMOND_MAX_FUNCTION_NAME];
        const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",scope,
            (int)name.length,compiler->source+name.start);
        if(written>0&&(size_t)written<sizeof qualified) {
            const int found=find_namespace_constant_name(compiler,qualified);
            if(found>=0)return found;
        }
        char *separator=strrchr(scope,':');
        if(separator==nullptr)break;
        separator[-1]='\0';
    }
    return -1;
}

static bool append_span_name(Compiler *compiler,char *buffer,size_t capacity,
                             DiamondSpan span) {
    const size_t used=strlen(buffer);
    if(used+span.length>=capacity)return false;
    for(size_t index=0;index<span.length;index++)
        buffer[used+index]=compiler->source[span.start+index];
    buffer[used+span.length]='\0';return true;
}

static bool declaration_name(Compiler *compiler,char *buffer,size_t capacity,
                             DiamondSpan local) {
    buffer[0]='\0';
    if(compiler->current_module>=0) {
        const char *parent=
            compiler->program->modules[(size_t)compiler->current_module].name;
        if(strlen(parent)+2>=capacity)return false;
        (void)snprintf(buffer,capacity,"%s::",parent);
    }
    return append_span_name(compiler,buffer,capacity,local);
}

static bool consume_qualified_name(Compiler *compiler,char *buffer,
                                   size_t capacity) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER)return false;
    buffer[0]='\0';
    if(!append_span_name(compiler,buffer,capacity,compiler->current.span))return false;
    advance_token(compiler);
    while(compiler->current.kind==DIAMOND_TOKEN_DOUBLE_COLON) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
           strlen(buffer)+2>=capacity)return false;
        (void)strncat(buffer,"::",capacity-strlen(buffer)-1);
        if(!append_span_name(compiler,buffer,capacity,compiler->current.span))return false;
        advance_token(compiler);
    }
    return true;
}

static int resolve_type(Compiler *compiler, DiamondSpan name) {
    if (name_equals(compiler, "Int", name, false)) return DIAMOND_TYPE_INT;
    if (name_equals(compiler, "Float", name, false)) return DIAMOND_TYPE_FLOAT;
    if (name_equals(compiler, "String", name, false)) return DIAMOND_TYPE_STRING;
    if (name_equals(compiler, "Bool", name, false)) return DIAMOND_TYPE_BOOL;
    if (name_equals(compiler, "Nil", name, false)) return DIAMOND_TYPE_NIL;
    if (name_equals(compiler, "Array", name, false)) return DIAMOND_TYPE_ARRAY;
    if (name_equals(compiler, "Hash", name, false)) return DIAMOND_TYPE_HASH;
    if (name_equals(compiler, "Callable", name, false)) return DIAMOND_TYPE_CALLABLE;
    if (name_equals(compiler, "Sized", name, false)) return DIAMOND_TYPE_SIZED;
    if (name_equals(compiler, "Symbol", name, false)) return DIAMOND_TYPE_SYMBOL;
    for(size_t index=0;index<compiler->function->type_variable_count;index++)
        if(name_equals(compiler,compiler->function->type_variables[index],name,false))
            return DIAMOND_TYPE_VARIABLE_BASE+(int)index;
    const int interface_index=find_interface(compiler,name);
    if(interface_index>=0)return DIAMOND_TYPE_INTERFACE_BASE+interface_index;
    const int class_index = find_class(compiler, name);
    if (class_index >= 0) return DIAMOND_TYPE_CLASS_BASE + class_index;
    fail(compiler, name, "unknown type annotation");
    return DIAMOND_TYPE_NIL;
}

static int resolve_type_name(Compiler *compiler,const char *name,
                             DiamondSpan diagnostic) {
    static const struct {const char *name;uint8_t type;} builtins[]={
        {"Int",DIAMOND_TYPE_INT},{"Float",DIAMOND_TYPE_FLOAT},
        {"String",DIAMOND_TYPE_STRING},
        {"Bool",DIAMOND_TYPE_BOOL},{"Nil",DIAMOND_TYPE_NIL},
        {"Array",DIAMOND_TYPE_ARRAY},{"Hash",DIAMOND_TYPE_HASH},
        {"Callable",DIAMOND_TYPE_CALLABLE},{"Sized",DIAMOND_TYPE_SIZED},
        {"Symbol",DIAMOND_TYPE_SYMBOL}};
    for(size_t index=0;index<sizeof builtins/sizeof builtins[0];index++)
        if(strcmp(name,builtins[index].name)==0)return builtins[index].type;
    for(size_t index=0;index<compiler->function->type_variable_count;index++)
        if(strcmp(name,compiler->function->type_variables[index])==0)
            return DIAMOND_TYPE_VARIABLE_BASE+(int)index;
    int found=find_interface_name(compiler,name);
    if(found>=0)return DIAMOND_TYPE_INTERFACE_BASE+found;
    found=find_class_name(compiler,name);
    if(found>=0)return DIAMOND_TYPE_CLASS_BASE+found;
    fail(compiler,diagnostic,"unknown type annotation");return DIAMOND_TYPE_NIL;
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
        char type_name_buffer[DIAMOND_MAX_FUNCTION_NAME];
        if(!consume_qualified_name(compiler,type_name_buffer,
                                   sizeof type_name_buffer)) {
            fail(compiler,member_span,"type name is too long");break;
        }
        const uint8_t type=(uint8_t)resolve_type_name(
            compiler,type_name_buffer,member_span);
        for(size_t index=0;index<set->count;index++) {
            if(set->members[index].id==type) {
                fail(compiler,compiler->current.span,"duplicate type in union");break;
            }
        }
        if(set->count==DIAMOND_MAX_UNION_TYPES) {
            fail(compiler,compiler->current.span,"too many types in union");break;
        }
        uint8_t argument_set=UINT8_MAX,second_argument_set=UINT8_MAX;
        uint8_t callable_arity=UINT8_MAX;
        uint8_t callable_return_set=UINT8_MAX;
        bool callable_parameters_typed=false;
        uint8_t callable_parameter_sets[16];
        for(size_t index=0;index<16;index++)callable_parameter_sets[index]=UINT8_MAX;
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            if(type==DIAMOND_TYPE_CALLABLE) {
                advance_token(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_INTEGER) {
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
                } else if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
                    advance_token(compiler);callable_arity=0;
                    callable_parameters_typed=true;
                    skip_newlines(compiler);
                    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
                          !compiler->failed) {
                        if(callable_arity==16) {
                            fail(compiler,compiler->current.span,
                                 "Callable cannot exceed 16 parameters");break;
                        }
                        callable_parameter_sets[callable_arity++]=
                            (uint8_t)parse_type_annotation(compiler);
                        skip_newlines(compiler);
                        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
                        advance_token(compiler);
                        skip_newlines(compiler);
                    }
                    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                        fail(compiler,compiler->current.span,
                             "expected ']' after Callable parameters");break;
                    }
                    advance_token(compiler);
                } else {
                    fail(compiler,compiler->current.span,
                         "expected Callable arity or parameter list");break;
                }
                if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                    advance_token(compiler);
                    callable_return_set=(uint8_t)parse_type_annotation(compiler);
                }
            } else if(type==DIAMOND_TYPE_ARRAY||type==DIAMOND_TYPE_HASH) {
                advance_token(compiler);
                skip_newlines(compiler);
                argument_set=(uint8_t)parse_type_annotation(compiler);
                if(type==DIAMOND_TYPE_HASH) {
                    skip_newlines(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                        fail(compiler,compiler->current.span,
                             "expected ',' between Hash key and value types");break;
                    }
                    advance_token(compiler);
                    skip_newlines(compiler);
                    second_argument_set=(uint8_t)parse_type_annotation(compiler);
                }
            } else {
                fail(compiler,member_span,
                     "this type does not accept arguments");break;
            }
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                fail(compiler,compiler->current.span,
                     "expected ']' after collection type arguments");break;
            }
            advance_token(compiler);
        }
        DiamondTypeMember parsed=(DiamondTypeMember){
            .id=type,.argument_set=argument_set,
            .second_argument_set=second_argument_set,
            .callable_arity=callable_arity,
            .callable_return_set=callable_return_set,
            .callable_parameters_typed=callable_parameters_typed};
        for(size_t index=0;index<16;index++)
            parsed.callable_parameter_sets[index]=callable_parameter_sets[index];
        set->members[set->count++]=parsed;
        if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
        advance_token(compiler);
    }
    return (int)set_index;
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

static uint8_t module_field_name(Compiler *compiler,DiamondSpan name) {
    compiler->function->uses_instance_state=true;
    DiamondModule *module=
        &compiler->program->modules[(size_t)compiler->current_module];
    const size_t length=name.length-1;
    for(size_t field=0;field<module->field_count;field++)
        if(strlen(module->fields[field])==length&&
           memcmp(module->fields[field],compiler->source+name.start+1,length)==0)
            return add_string_range(compiler,name.start+1,length,name);
    if(module->field_count==DIAMOND_MAX_FIELDS) {
        fail(compiler,name,"too many module instance variables");return 0;
    }
    if(length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,name,"instance variable name is too long");return 0;
    }
    memcpy(module->fields[module->field_count],compiler->source+name.start+1,length);
    module->fields[module->field_count][length]='\0';module->field_count++;
    return add_string_range(compiler,name.start+1,length,name);
}

static uint8_t parse_call(Compiler *compiler, DiamondSpan name) {
    const int callable_local=find_local(compiler,name);
    if(callable_local>=0&&compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN) {
        uint8_t callable=compiler->locals[(size_t)callable_local].reg;
        if(compiler->locals[(size_t)callable_local].captured) {
            const uint8_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,callable,0,2);
            callable=loaded;
        }
        advance_token(compiler);
        skip_newlines(compiler);
        uint8_t arguments[16]; size_t argument_count=0;
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
            if(argument_count==16){fail(compiler,compiler->current.span,"too many call arguments");return 0;}
            arguments[argument_count++]=parse_expression(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
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
    const DiamondFunction *function =
        &compiler->program->functions[(size_t)function_index];
    uint8_t type_arguments[8];
    size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            if(type_argument_count==8) {
                fail(compiler,compiler->current.span,"too many generic arguments");
                return 0;
            }
            type_arguments[type_argument_count++]=
                (uint8_t)parse_type_annotation(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
        if(type_argument_count!=function->type_variable_count) {
            fail(compiler,name,"wrong number of generic arguments");return 0;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler,compiler->current.span,
                 "expected '(' after generic arguments");return 0;
        }
    }
    advance_token(compiler);
    skip_newlines(compiler);
    /* Keyword arguments (direct top-level calls only -- see docs/roadmap.md):
     * each argument is placed into its declared positional slot rather than
     * appended, so a keyword can fill any parameter regardless of the order
     * it's written at the call site. Positional arguments still fill slots
     * left-to-right in declaration order. */
    uint8_t slot_registers[16];
    bool slot_filled[16]={};
    size_t next_positional_slot=0;
    bool seen_keyword=false;
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            DiamondLexer keyword_lookahead=compiler->lexer;
            const bool is_keyword=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
                diamond_lexer_next(&keyword_lookahead).kind==DIAMOND_TOKEN_COLON;
            size_t slot;
            if(is_keyword) {
                const DiamondSpan keyword_name=compiler->current.span;
                advance_token(compiler); /* consume the name */
                advance_token(compiler); /* consume ':' */
                slot=SIZE_MAX;
                for(size_t index=0;index<function->arity&&index<16;index++)
                    if(name_equals(compiler,function->parameter_names[index],
                                  keyword_name,false)) {slot=index;break;}
                if(slot==SIZE_MAX) {
                    fail(compiler,keyword_name,"no parameter with this name");
                    return 0;
                }
                seen_keyword=true;
            } else {
                if(seen_keyword) {
                    fail(compiler,compiler->current.span,
                         "positional argument cannot follow a keyword argument");
                    return 0;
                }
                if(next_positional_slot==16) {
                    fail(compiler,compiler->current.span,"too many call arguments");
                    return 0;
                }
                slot=next_positional_slot++;
            }
            if(slot_filled[slot]) {
                fail(compiler,name,"multiple values for the same argument");
                return 0;
            }
            slot_registers[slot]=parse_expression(compiler);
            slot_filled[slot]=true;
            skip_newlines(compiler);
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        } while (!compiler->failed);
    }
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    size_t argument_count=0;
    for(size_t index=0;index<16;index++)
        if(slot_filled[index])argument_count=index+1;
    /* Every slot below the highest filled one must be filled too -- a
     * keyword argument can fill any slot, but a gap below the highest one
     * (an earlier default relied on while a later slot is explicitly
     * supplied) isn't supported: Diamond's default values are compiled
     * inline into the callee's own bytecode, conditioned on a contiguous
     * argument_count, not stored as independently re-evaluable
     * expressions a call site could reach around a gap. */
    for(size_t index=0;index<argument_count;index++)
        if(!slot_filled[index]) {
            fail(compiler,name,"missing argument");
            return 0;
        }
    if (argument_count < function->required_arity||argument_count > function->arity) {
        fail(compiler, name, "wrong number of arguments");
        return 0;
    }

    const uint8_t argument_base = allocate_register(compiler);
    for (size_t index = 1; index < argument_count; index++) {
        (void)allocate_register(compiler);
    }
    for (size_t index = 0; index < argument_count; index++) {
        emit_instruction(compiler, DIAMOND_OP_MOVE,
                         (uint8_t)(argument_base + index), slot_registers[index], 0, 2);
    }
    const uint8_t destination = allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?
        DIAMOND_OP_CALL:DIAMOND_OP_CALL_TYPED);
    emit_byte(compiler, destination);
    emit_function_index(compiler, (size_t)function_index);
    emit_byte(compiler, argument_base);
    emit_byte(compiler, (uint8_t)argument_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_byte(compiler,type_arguments[index]);
    }
    return destination;
}

static uint8_t parse_singleton_call(Compiler *compiler,
                                    const DiamondMethod *method,
                                    DiamondSpan namespace_name) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected singleton function after module name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    const DiamondFunction *function=
        &compiler->program->functions[method->function_index];
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL)advance_token(compiler);
    uint8_t type_arguments[8];size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            if(type_argument_count==8) {
                fail(compiler,compiler->current.span,"too many generic arguments");
                return 0;
            }
            type_arguments[type_argument_count++]=
                (uint8_t)parse_type_annotation(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
        if(type_argument_count!=function->type_variable_count) {
            fail(compiler,name,"wrong number of generic arguments");return 0;
        }
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,namespace_name,"expected '(' after singleton function");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint8_t arguments[16];size_t argument_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        if(argument_count==16) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        arguments[argument_count++]=parse_expression(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;
    }
    advance_token(compiler);
    if(argument_count<method->required_arity||argument_count>method->arity) {
        fail(compiler,name,"wrong number of arguments");return 0;
    }
    const size_t call_count=argument_count+(method->needs_receiver?1:0);
    const uint8_t base=allocate_register(compiler);
    for(size_t index=1;index<call_count;index++)(void)allocate_register(compiler);
    /* No NIL for `base` when needs_receiver: sole writer, already
     * zero-inited by run_chunk's [0, register_count) init. */
    for(size_t index=0;index<argument_count;index++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,
                         (uint8_t)(base+index+(method->needs_receiver?1:0)),
                         arguments[index],0,2);
    const uint8_t destination=allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_CALL:
                DIAMOND_OP_CALL_TYPED);
    emit_byte(compiler,destination);
    emit_function_index(compiler,method->function_index);
    emit_byte(compiler,base);emit_byte(compiler,(uint8_t)call_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_byte(compiler,type_arguments[index]);
    }
    return destination;
}

static bool singleton_call_name_equals(const Compiler *compiler,
                                       const char *candidate,DiamondSpan name) {
    const size_t length=strlen(candidate);
    if(length==name.length)return name_equals(compiler,candidate,name,false);
    if(length!=name.length+1||candidate[length-1]!='=')return false;
    DiamondLexer lookahead=compiler->lexer;
    if(diamond_lexer_next(&lookahead).kind!=DIAMOND_TOKEN_EQUAL)return false;
    return memcmp(candidate,compiler->source+name.start,name.length)==0;
}

static uint8_t parse_redefine_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'redefine_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'redefine_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after redefine_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t callable_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after redefine_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_REDEFINE_METHOD);
    emit_byte(compiler, dest);
    emit_byte(compiler, (uint8_t)class_index);
    emit_byte(compiler, name_register);
    emit_byte(compiler, callable_register);
    compiler->known_types[dest] = DIAMOND_TYPE_NIL;
    return dest;
}

static uint8_t parse_fiber_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Fiber'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Fiber.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t callable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Fiber.new argument");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FIBER_NEW);
    emit_byte(compiler,dest);
    emit_byte(compiler,callable_register);
    return dest;
}

static uint8_t parse_file_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'File'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'File.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after File.open path");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t mode_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_OPEN);
    emit_byte(compiler,dest);
    emit_byte(compiler,path_register);
    emit_byte(compiler,mode_register);
    return dest;
}

static uint8_t parse_regexp_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Regexp'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Regexp.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t pattern_register=parse_expression(compiler);
    skip_newlines(compiler);
    /* options is optional -- Regexp.new(pattern) is the common case,
     * defaulting to a compile-time 0 constant (REGINOLD_OPTION_NONE)
     * rather than requiring every call site to spell it out, unlike
     * File.open's two always-required arguments. */
    uint8_t options_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        options_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        options_register=allocate_register(compiler);
        const uint8_t zero=add_constant(compiler,DIAMOND_INT(0));
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,options_register,zero,0,2);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Regexp.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_REGEXP_NEW);
    emit_byte(compiler,dest);
    emit_byte(compiler,pattern_register);
    emit_byte(compiler,options_register);
    return dest;
}

/* ProgramBuilder.new() -- the one ProgramBuilder call needing dedicated
 * compiler recognition (constructing the object). Every instance method
 * (.declare_function/.emit_byte/.add_constant/.add_string/
 * .set_register_count/.run) dispatches through the ordinary INVOKE opcode
 * like any other native-kind receiver (Fiber/File/Regexp), needing no
 * compiler changes at all. See docs/roadmap.md's self-hosting Phase 1
 * entry. */
static uint8_t parse_program_builder_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'ProgramBuilder'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'ProgramBuilder.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after ProgramBuilder.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PROGRAM_BUILDER_NEW);
    emit_byte(compiler,dest);
    return dest;
}

static uint8_t parse_tcp_connect_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"connect",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'connect' after 'TCPSocket'");
        return 0;
    }
    advance_token(compiler); /* consume 'connect' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TCPSocket.connect'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TCPSocket.connect host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TCPSocket.connect arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TCP_CONNECT);
    emit_byte(compiler,dest);
    emit_byte(compiler,host_register);
    emit_byte(compiler,port_register);
    return dest;
}

static uint8_t parse_tcp_listen_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool nonblocking=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"listen_nonblocking",compiler->current.span,false);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !(nonblocking||name_equals(compiler,"listen",compiler->current.span,false))) {
        fail(compiler,compiler->current.span,
             "expected 'listen' or 'listen_nonblocking' after 'TCPServer'");
        return 0;
    }
    advance_token(compiler); /* consume 'listen'/'listen_nonblocking' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TCPServer.listen'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TCPServer.listen argument");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,nonblocking?DIAMOND_OP_TCP_LISTEN_NONBLOCK:DIAMOND_OP_TCP_LISTEN);
    emit_byte(compiler,dest);
    emit_byte(compiler,port_register);
    return dest;
}

static uint8_t parse_io_poll_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"poll",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'poll' after 'IO'");
        return 0;
    }
    advance_token(compiler); /* consume 'poll' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'IO.poll'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t readable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after IO.poll readables");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t writable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after IO.poll writables");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t timeout_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after IO.poll arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_IO_POLL);
    emit_byte(compiler,dest);
    emit_byte(compiler,readable_register);
    emit_byte(compiler,writable_register);
    emit_byte(compiler,timeout_register);
    return dest;
}

static uint8_t parse_udp_socket_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool is_bind=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"bind",compiler->current.span,false);
    const bool is_open=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"open",compiler->current.span,false);
    if(!is_bind&&!is_open) {
        fail(compiler,compiler->current.span,"expected 'bind' or 'open' after 'UDPSocket'");
        return 0;
    }
    advance_token(compiler); /* consume 'bind'/'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             is_bind?"expected '(' after 'UDPSocket.bind'":"expected '(' after 'UDPSocket.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint8_t port_register=0;
    if(is_bind) {
        port_register=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
             is_bind?"expected ')' after UDPSocket.bind argument":
                     "expected ')' after UDPSocket.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,is_bind?DIAMOND_OP_UDP_BIND:DIAMOND_OP_UDP_OPEN);
    emit_byte(compiler,dest);
    if(is_bind)emit_byte(compiler,port_register);
    return dest;
}

static uint8_t parse_signal_trap_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"trap",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'trap' after 'Signal'");
        return 0;
    }
    advance_token(compiler); /* consume 'trap' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Signal.trap'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t name_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Signal.trap name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t handler_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Signal.trap arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SIGNAL_TRAP);
    emit_byte(compiler,dest);
    emit_byte(compiler,name_register);
    emit_byte(compiler,handler_register);
    return dest;
}

static uint8_t parse_chr_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint8_t source=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CHR);
    emit_byte(compiler,dest);
    emit_byte(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

static uint8_t parse_to_float_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint8_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_FLOAT);
    emit_byte(compiler,dest);
    emit_byte(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

static uint8_t parse_to_int_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint8_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_INT);
    emit_byte(compiler,dest);
    emit_byte(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_INT;
    return dest;
}

static uint8_t parse_to_sym_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint8_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_SYMBOL);
    emit_byte(compiler,dest);
    emit_byte(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_SYMBOL;
    return dest;
}

static uint8_t parse_math_unary_call(Compiler *compiler, DiamondMathFunction id) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint8_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MATH_UNARY);
    emit_byte(compiler,dest);
    emit_byte(compiler,source);
    emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

static uint8_t parse_math_binary_call(Compiler *compiler, DiamondMathFunction id) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint8_t left = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' between arguments");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint8_t right = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MATH_BINARY);
    emit_byte(compiler,dest);
    emit_byte(compiler,left);
    emit_byte(compiler,right);
    emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

static uint8_t parse_print_call(Compiler *compiler, bool newline) {
    advance_token(compiler); /* consume '(' */
    const uint8_t source=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PRINT);
    emit_byte(compiler,dest);
    emit_byte(compiler,source);
    emit_byte(compiler,newline?1:0);
    compiler->known_types[dest]=DIAMOND_TYPE_NIL;
    return dest;
}

static uint8_t parse_gets_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint8_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_GETS);
    emit_byte(compiler,dest);
    return dest;
}

static uint8_t parse_name(Compiler *compiler) {
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
        const int constant=find_namespace_constant_name(compiler,qualified);
        if(constant>=0) {
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_NAMESPACE_CONSTANT,
                destination,(uint8_t)constant,0,2);
            return destination;
        }
        if(class_index<0&&module_index<0) {
            fail(compiler,name,"undefined namespaced class");return 0;
        }
    }
    if(module_index>=0&&compiler->current.kind==DIAMOND_TOKEN_DOT) {
        advance_token(compiler);
        const DiamondModule *module=
            &compiler->program->modules[(size_t)module_index];
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
        return parse_singleton_call(compiler,method,name);
    }
    const int constant=find_namespace_constant(compiler,name);
    if(constant>=0&&find_local(compiler,name)<0) {
        const uint8_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_NAMESPACE_CONSTANT,destination,
                         (uint8_t)constant,0,2);
        return destination;
    }
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Fiber",name,false))
        return parse_fiber_new_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"File",name,false))
        return parse_file_open_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Regexp",name,false))
        return parse_regexp_new_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"ProgramBuilder",name,false))
        return parse_program_builder_new_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"TCPSocket",name,false))
        return parse_tcp_connect_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"TCPServer",name,false))
        return parse_tcp_listen_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"IO",name,false))
        return parse_io_poll_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"UDPSocket",name,false))
        return parse_udp_socket_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Signal",name,false))
        return parse_signal_trap_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       (name_equals(compiler,"print",name,false)||
        name_equals(compiler,"puts",name,false)))
        return parse_print_call(compiler,name_equals(compiler,"puts",name,false));
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"gets",name,false))
        return parse_gets_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"chr",name,false))
        return parse_chr_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"to_f",name,false))
        return parse_to_float_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"to_i",name,false))
        return parse_to_int_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"to_sym",name,false))
        return parse_to_sym_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"sqrt",name,false))
        return parse_math_unary_call(compiler,DIAMOND_MATH_SQRT);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"sin",name,false))
        return parse_math_unary_call(compiler,DIAMOND_MATH_SIN);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"cos",name,false))
        return parse_math_unary_call(compiler,DIAMOND_MATH_COS);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"tan",name,false))
        return parse_math_unary_call(compiler,DIAMOND_MATH_TAN);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       name_equals(compiler,"pow",name,false))
        return parse_math_binary_call(compiler,DIAMOND_MATH_POW);
    if (class_index >= 0 && compiler->current.kind == DIAMOND_TOKEN_DOT) {
        advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,
                 "expected constructor or singleton method");return 0;
        }
        if(name_equals(compiler,"redefine_method",compiler->current.span,false))
            return parse_redefine_method_call(compiler,class_index);
        if(!name_equals(compiler,"new",compiler->current.span,false)) {
            const DiamondMethod *method=nullptr;
            const DiamondClass *owner=
                &compiler->program->classes[(size_t)class_index];
            while(owner!=nullptr&&method==nullptr) {
                for(size_t index=0;index<owner->singleton_method_count;index++)
                    if(singleton_call_name_equals(compiler,
                           owner->singleton_methods[index].name,
                           compiler->current.span))
                        method=&owner->singleton_methods[index];
                owner=owner->superclass==UINT8_MAX?nullptr:
                    &compiler->program->classes[owner->superclass];
            }
            if(method==nullptr) {
                fail(compiler,compiler->current.span,
                     "undefined class singleton method");return 0;
            }
            return parse_singleton_call(compiler,method,name);
        }
        advance_token(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler, compiler->current.span, "expected '(' after 'new'");
            return 0;
        }
        advance_token(compiler);
        skip_newlines(compiler);
        uint8_t args[16]; size_t count = 0;
        while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
            if (count == 16) { fail(compiler, compiler->current.span, "too many arguments"); break; }
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
    if (compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN||
        (compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
         find_local(compiler,name)<0&&find_function(compiler,name)>=0)) {
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
    bool writer_name=false;
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        writer_name=true;advance_token(compiler);
    }
    uint8_t type_arguments[8];size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            if(type_argument_count==8) {
                fail(compiler,compiler->current.span,"too many generic arguments");
                return 0;
            }
            type_arguments[type_argument_count++]=
                (uint8_t)parse_type_annotation(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
    }
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after method name"); return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint8_t args[16]; size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == 16) { fail(compiler, compiler->current.span, "too many arguments"); break; }
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
    const uint8_t base = allocate_register(compiler);
    for (size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for (size_t i=0;i<count;i++) emit_instruction(compiler, DIAMOND_OP_MOVE,
        (uint8_t)(base+i), args[i], 0, 2);
    const uint8_t dest=allocate_register(compiler);
    const uint8_t method=add_name_string(compiler,name);
    if(writer_name&&!compiler->failed) {
        DiamondStringConstant *string=&compiler->function->strings[method];
        if(string->length==DIAMOND_MAX_STRING_LENGTH)
            fail(compiler,name,"method name is too long");
        else {
            string->chars[string->length++]='=';
            string->chars[string->length]='\0';
        }
    }
    emit_opcode(compiler,type_argument_count==0?
        DIAMOND_OP_INVOKE:DIAMOND_OP_INVOKE_TYPED);emit_byte(compiler,dest);
    emit_byte(compiler,receiver); emit_byte(compiler,method); emit_byte(compiler,base);
    emit_byte(compiler,(uint8_t)count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_byte(compiler,type_arguments[index]);
    }
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
    skip_newlines(compiler);
    uint8_t arguments[16];
    size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == 16) {
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
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        do {
            if(count==32) {
                fail(compiler,compiler->current.span,"array literal has too many elements");
                return 0;
            }
            elements[count++]=parse_expression(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
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
    skip_newlines(compiler);
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
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
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

static void apply_narrowing_facts(Compiler *compiler,const NarrowingFact *facts,
                                  size_t count) {
    for(size_t index=0;index<count;index++)
        apply_type_set_fact(compiler,facts[index].reg,facts[index].type_set);
}

static void append_narrowing_facts(NarrowingFact *destination,size_t *destination_count,
                                   const NarrowingFact *source,size_t source_count) {
    for(size_t index=0;index<source_count&&
        *destination_count<DIAMOND_MAX_NARROWING_FACTS;index++)
        destination[(*destination_count)++]=source[index];
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

static bool consume_conditional_start(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_THEN)
        return consume_block_start(compiler);
    advance_token(compiler);
    skip_newlines(compiler);
    return true;
}

static bool consume_loop_start(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_DO)
        return consume_block_start(compiler);
    advance_token(compiler);
    skip_newlines(compiler);
    return true;
}

static uint8_t parse_if(Compiler *compiler,bool inverted) {
    compiler->narrowing=(Narrowing){};
    const uint8_t condition = parse_expression(compiler);
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    if (!consume_conditional_start(compiler)) return 0;

    uint8_t branch_condition=condition;
    if(inverted) {
        branch_condition=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NOT,branch_condition,condition,0,2);
    }
    const size_t false_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, branch_condition);
    const uint8_t destination = allocate_register(compiler);
    const size_t flow_reg_count=compiler->next_register;
    uint8_t before_types[256];int16_t before_sets[256];
    for(size_t index=0;index<flow_reg_count;index++) {
        before_types[index]=compiler->known_types[index];
        before_sets[index]=compiler->known_type_sets[index];
    }
    if(narrowing.valid)
        apply_narrowing_facts(compiler,
            inverted?narrowing.when_false:narrowing.when_true,
            inverted?narrowing.when_false_count:narrowing.when_true_count);
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
        apply_narrowing_facts(compiler,
            inverted?narrowing.when_true:narrowing.when_false,
            inverted?narrowing.when_true_count:narrowing.when_false_count);

    uint8_t result_type=TYPE_UNKNOWN;int16_t result_set=-1;
    bool end_consumed=false;
    if (compiler->current.kind == DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE)skip_newlines(compiler);
        const uint8_t else_result = compile_sequence(compiler);
        const uint8_t else_type=compiler->known_types[else_result];
        const int16_t else_set=compiler->known_type_sets[else_result];
        emit_instruction(compiler, DIAMOND_OP_MOVE, destination, else_result, 0, 2);
        if(then_type==else_type)result_type=then_type;
        if(then_set==else_set)result_set=then_set;
    } else if(compiler->current.kind==DIAMOND_TOKEN_ELSIF) {
        advance_token(compiler);
        const uint8_t else_result=parse_if(compiler,false);
        const uint8_t else_type=compiler->known_types[else_result];
        const int16_t else_set=compiler->known_type_sets[else_result];
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,else_result,0,2);
        if(then_type==else_type)result_type=then_type;
        if(then_set==else_set)result_set=then_set;
        end_consumed=true;
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

    if (!end_consumed&&compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after if expression");
        return destination;
    }
    if(!end_consumed)advance_token(compiler);
    patch_jump(compiler, end_jump, compiler->function->code_count);
    return destination;
}

static uint8_t parse_while(Compiler *compiler,bool inverted) {
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    const size_t loop_start = compiler->function->code_count;
    const uint8_t condition = parse_expression(compiler);
    if (!consume_loop_start(compiler)) return 0;
    uint8_t branch_condition=condition;
    if(inverted) {
        branch_condition=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NOT,branch_condition,condition,0,2);
    }
    const size_t exit_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, branch_condition);
    LoopContext loop={
        .previous=compiler->current_loop,
        .continue_target=loop_start,
        .redo_target=compiler->function->code_count,
        .result_register=destination,
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
    return destination;
}

static uint8_t parse_loop(Compiler *compiler) {
    const uint8_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    if(!consume_loop_start(compiler))return destination;
    const size_t body_start=compiler->function->code_count;
    LoopContext loop={.previous=compiler->current_loop,
        .continue_target=body_start,.redo_target=body_start,
        .result_register=destination};
    compiler->current_loop=&loop;
    (void)compile_sequence(compiler);
    compiler->current_loop=loop.previous;
    emit_absolute_jump(compiler,body_start);
    for(size_t index=0;index<loop.break_count;index++)
        patch_jump(compiler,loop.breaks[index],compiler->function->code_count);
    if(compiler->current.kind!=DIAMOND_TOKEN_END) {
        fail(compiler,compiler->current.span,"expected 'end' after loop");
        return destination;
    }
    advance_token(compiler);return destination;
}

static uint8_t parse_prefix(Compiler *compiler) {
    advance_token(compiler);
    switch (compiler->previous.kind) {
        case DIAMOND_TOKEN_INTEGER:
            return parse_integer(compiler);
        case DIAMOND_TOKEN_FLOAT:
            return parse_float(compiler);
        case DIAMOND_TOKEN_STRING:
            return parse_string(compiler);
        case DIAMOND_TOKEN_SYMBOL:
            return parse_symbol(compiler);
        case DIAMOND_TOKEN_TRUE:
        case DIAMOND_TOKEN_FALSE:
        case DIAMOND_TOKEN_NIL:
            return parse_literal(compiler);
        case DIAMOND_TOKEN_IDENTIFIER:
            return parse_name(compiler);
        case DIAMOND_TOKEN_INSTANCE_VARIABLE: {
            const uint8_t destination = allocate_register(compiler);
            if(compiler->current_module>=0&&compiler->current_class<0) {
                const uint8_t field=module_field_name(
                    compiler,compiler->previous.span);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,destination,0,
                                 field,3);
            } else {
                const int field=field_index(compiler,compiler->previous.span,true);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,destination,0,
                                 (uint8_t)field,3);
            }
            return destination;
        }
        case DIAMOND_TOKEN_SELF:
            if (!compiler->in_method) {
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
            emit_instruction(compiler, DIAMOND_OP_NEGATE, destination,
                             operand, 0, 2);
            if(compiler->known_types[operand]==DIAMOND_TYPE_INT)
                compiler->known_types[destination]=DIAMOND_TYPE_INT;
            else if(compiler->known_types[operand]==DIAMOND_TYPE_FLOAT)
                compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
            return destination;
        }
        case DIAMOND_TOKEN_BANG:
        case DIAMOND_TOKEN_NOT: {
            const uint8_t operand=parse_precedence(compiler,PREC_PREFIX);
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,operand,0,2);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            return destination;
        }
        case DIAMOND_TOKEN_IF:
            return parse_if(compiler,false);
        case DIAMOND_TOKEN_UNLESS:
            return parse_if(compiler,true);
        case DIAMOND_TOKEN_WHILE:
            return parse_while(compiler,false);
        case DIAMOND_TOKEN_UNTIL:
            return parse_while(compiler,true);
        case DIAMOND_TOKEN_LOOP:
            return parse_loop(compiler);
        case DIAMOND_TOKEN_BEGIN:
            return compile_begin(compiler);
        case DIAMOND_TOKEN_YIELD:
            return compile_yield(compiler);
        default:
            fail(compiler, compiler->previous.span, "expected expression");
            return 0;
    }
}

static DiamondOpCode binary_opcode(DiamondTokenKind operator) {
    switch (operator) {
        case DIAMOND_TOKEN_PLUS: return DIAMOND_OP_ADD;
        case DIAMOND_TOKEN_MINUS: return DIAMOND_OP_SUBTRACT;
        case DIAMOND_TOKEN_STAR: return DIAMOND_OP_MULTIPLY;
        case DIAMOND_TOKEN_SLASH: return DIAMOND_OP_DIVIDE;
        case DIAMOND_TOKEN_EQUAL_EQUAL: return DIAMOND_OP_EQUAL;
        case DIAMOND_TOKEN_BANG_EQUAL: return DIAMOND_OP_NOT_EQUAL;
        case DIAMOND_TOKEN_LESS: return DIAMOND_OP_LESS;
        case DIAMOND_TOKEN_LESS_EQUAL: return DIAMOND_OP_LESS_EQUAL;
        case DIAMOND_TOKEN_GREATER: return DIAMOND_OP_GREATER;
        case DIAMOND_TOKEN_GREATER_EQUAL: return DIAMOND_OP_GREATER_EQUAL;
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
            if(tested_type>=DIAMOND_TYPE_VARIABLE_BASE&&
               tested_type<DIAMOND_TYPE_INTERFACE_BASE)
                fail(compiler,compiler->current.span,
                     "generic type variables cannot be used with 'is' before binding");
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
                        .condition=destination,
                        .when_true={{.reg=left,.type_set=matching}},
                        .when_true_count=1,
                        .when_false={{.reg=left,.type_set=remaining}},
                        .when_false_count=1};
            }
            left=destination;continue;
        }
        if(operator==DIAMOND_TOKEN_AND_AND||operator==DIAMOND_TOKEN_AND||
           operator==DIAMOND_TOKEN_OR_OR||operator==DIAMOND_TOKEN_OR) {
            const bool is_and=operator==DIAMOND_TOKEN_AND_AND||
                operator==DIAMOND_TOKEN_AND;
            const Narrowing left_narrowing=compiler->narrowing.condition==left
                ?compiler->narrowing:(Narrowing){};
            const uint8_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
            const uint8_t right=parse_precedence(
                compiler,(Precedence)(operator_precedence+1));
            const Narrowing right_narrowing=compiler->narrowing.condition==right
                ?compiler->narrowing:(Narrowing){};
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
            patch_jump(compiler,end_jump,compiler->function->code_count);
            if(compiler->known_types[left]==compiler->known_types[right])
                compiler->known_types[destination]=compiler->known_types[left];
            /* AND: both sides must hold for the whole expression to be
             * true, so the when-true facts compose (conjunction is
             * sound). The false case of AND is a disjunction of
             * failures, not soundly reducible to independent
             * per-variable facts, so when_false stays empty rather
             * than guessed. OR is the mirror image: when_false
             * composes (both sides must have failed), when_true stays
             * empty. An empty side is always safe to concatenate --
             * it contributes nothing -- so this composes correctly
             * through chains and mixed &&/|| automatically, with no
             * special-casing for either. */
            Narrowing merged={.condition=destination};
            if(is_and) {
                append_narrowing_facts(merged.when_true,&merged.when_true_count,
                    left_narrowing.when_true,left_narrowing.when_true_count);
                append_narrowing_facts(merged.when_true,&merged.when_true_count,
                    right_narrowing.when_true,right_narrowing.when_true_count);
            } else {
                append_narrowing_facts(merged.when_false,&merged.when_false_count,
                    left_narrowing.when_false,left_narrowing.when_false_count);
                append_narrowing_facts(merged.when_false,&merged.when_false_count,
                    right_narrowing.when_false,right_narrowing.when_false_count);
            }
            merged.valid=merged.when_true_count>0||merged.when_false_count>0;
            compiler->narrowing=merged;
            left=destination;
            continue;
        }
        const uint8_t right = parse_precedence(
            compiler, (Precedence)(operator_precedence + 1));
        const uint8_t destination = allocate_register(compiler);
        DiamondOpCode opcode=binary_opcode(operator);
        if(operator==DIAMOND_TOKEN_PLUS&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_ADD_INT;
        if(operator==DIAMOND_TOKEN_MINUS&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_SUBTRACT_INT;
        if(operator==DIAMOND_TOKEN_STAR&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_MULTIPLY_INT;
        if(operator==DIAMOND_TOKEN_SLASH&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_DIVIDE_INT;
        if(operator==DIAMOND_TOKEN_EQUAL_EQUAL&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_EQUAL_INT;
        if(operator==DIAMOND_TOKEN_BANG_EQUAL&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_NOT_EQUAL_INT;
        if(operator==DIAMOND_TOKEN_LESS&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_LESS_INT;
        if(operator==DIAMOND_TOKEN_LESS_EQUAL&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_LESS_EQUAL_INT;
        if(operator==DIAMOND_TOKEN_GREATER&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_GREATER_INT;
        if(operator==DIAMOND_TOKEN_GREATER_EQUAL&&
           compiler->known_types[left]==DIAMOND_TYPE_INT&&
           compiler->known_types[right]==DIAMOND_TYPE_INT)
            opcode=DIAMOND_OP_GREATER_EQUAL_INT;
        emit_instruction(compiler, opcode, destination,
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
                        const int16_t when_true=operator==DIAMOND_TOKEN_BANG_EQUAL
                            ?non_nil:nil_only;
                        const int16_t when_false=operator==DIAMOND_TOKEN_BANG_EQUAL
                            ?nil_only:non_nil;
                        compiler->narrowing=(Narrowing){.valid=true,
                            .condition=destination,
                            .when_true={{.reg=narrowed,.type_set=when_true}},
                            .when_true_count=1,
                            .when_false={{.reg=narrowed,.type_set=when_false}},
                            .when_false_count=1};
                    }
                }
            }
        } else if(compiler->known_types[left]==DIAMOND_TYPE_INT &&
                  compiler->known_types[right]==DIAMOND_TYPE_INT) {
            compiler->known_types[destination]=DIAMOND_TYPE_INT;
        } else if((compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
                   compiler->known_types[left]==DIAMOND_TYPE_INT) &&
                  (compiler->known_types[right]==DIAMOND_TYPE_FLOAT||
                   compiler->known_types[right]==DIAMOND_TYPE_INT) &&
                  (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
                   compiler->known_types[right]==DIAMOND_TYPE_FLOAT)) {
            /* Mixed Int/Float statically known to auto-promote to Float. */
            compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
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

static DiamondTokenKind postfix_modifier_ahead(const Compiler *compiler) {
    if (compiler->current.kind == DIAMOND_TOKEN_DEF ||
        compiler->current.kind == DIAMOND_TOKEN_CLASS ||
        compiler->current.kind == DIAMOND_TOKEN_INTERFACE ||
        compiler->current.kind == DIAMOND_TOKEN_MODULE) {
        return DIAMOND_TOKEN_EOF;
    }
    DiamondLexer lookahead = compiler->lexer;
    /* compiler->current may itself already be an opening bracket (this
     * whole statement is a bracketed literal, e.g. `[if x ... end]`) --
     * the scan below only sees tokens *after* current, so depth has to
     * start accounting for that already-consumed bracket, or a nested
     * if/unless at the literal's own top level would be miscounted as
     * depth 0 and misread as a trailing modifier on the statement. */
    size_t depth = compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN ||
                   compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACKET ||
                   compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACE ? 1 : 0;
    bool seen = true;
    /* An if/unless immediately after '=' is the start of the
     * assignment's own RHS expression (`x = if ... end`), not a
     * trailing postfix modifier on a value that hasn't been parsed yet.
     * Deliberately NOT extended to 'return'/'raise': `return if cond`/
     * `raise if cond` already have an established, different meaning
     * (a bare return/raise, postfix-conditioned on cond -- see
     * compile_return/compile_raise's own current.kind==IF/UNLESS
     * branches) that this function's callers already rely on; treating
     * a leading if/unless there as the start of a value instead would
     * silently change what `return if flag` means. Narrower than fully
     * structure-aware (an if/unless immediately after some other
     * operator, e.g. `x = y && if ... end`, is still misread the old
     * way), but covers the position this most commonly comes up in. */
    bool expression_expected = false;
    for (;;) {
        DiamondToken token = diamond_lexer_next(&lookahead);
        if (token.kind == DIAMOND_TOKEN_EOF ||
            token.kind == DIAMOND_TOKEN_ERROR ||
            token.kind == DIAMOND_TOKEN_NEWLINE) {
            return DIAMOND_TOKEN_EOF;
        }
        if (depth == 0 && seen &&
            (token.kind == DIAMOND_TOKEN_IF ||
             token.kind == DIAMOND_TOKEN_UNLESS)) {
            if (expression_expected) return DIAMOND_TOKEN_EOF;
            return token.kind;
        }
        switch (token.kind) {
            case DIAMOND_TOKEN_LEFT_PAREN:
            case DIAMOND_TOKEN_LEFT_BRACKET:
            case DIAMOND_TOKEN_LEFT_BRACE:
                depth++;
                break;
            case DIAMOND_TOKEN_RIGHT_PAREN:
            case DIAMOND_TOKEN_RIGHT_BRACKET:
            case DIAMOND_TOKEN_RIGHT_BRACE:
                if (depth > 0) depth--;
                break;
            default:
                break;
        }
        expression_expected = depth == 0 && token.kind == DIAMOND_TOKEN_EQUAL;
        seen = true;
    }
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
       compiler->current.kind==DIAMOND_TOKEN_IF ||
       compiler->current.kind==DIAMOND_TOKEN_UNLESS ||
       compiler->current.kind==DIAMOND_TOKEN_EOF) {
        value=allocate_register(compiler);
        /* Sole writer; run_chunk's zero-init already covers this. */
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

static uint8_t compile_yield(Compiler *compiler) {
    uint8_t source;
    if (compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN) {
        advance_token(compiler);
        source = parse_expression(compiler);
        if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler, compiler->current.span, "expected ')' after yield value");
            return 0;
        }
        advance_token(compiler);
    } else {
        source = allocate_register(compiler);
        /* Sole writer; run_chunk's zero-init already covers this. */
    }
    const uint8_t dest = allocate_register(compiler);
    emit_instruction(compiler, DIAMOND_OP_YIELD, dest, source, 0, 2);
    return dest;
}

static uint8_t compile_raise(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE ||
       compiler->current.kind==DIAMOND_TOKEN_END ||
       compiler->current.kind==DIAMOND_TOKEN_ELSE ||
       compiler->current.kind==DIAMOND_TOKEN_ENSURE ||
       compiler->current.kind==DIAMOND_TOKEN_IF ||
       compiler->current.kind==DIAMOND_TOKEN_UNLESS ||
       compiler->current.kind==DIAMOND_TOKEN_EOF) {
        if(compiler->current_exception<0) {
            fail(compiler,keyword,"bare 'raise' used outside rescue");return 0;
        }
        emit_instruction(compiler,DIAMOND_OP_RAISE,
                         (uint8_t)compiler->current_exception,0,0,1);
        return (uint8_t)compiler->current_exception;
    }
    const uint8_t value=parse_expression(compiler);
    emit_instruction(compiler,DIAMOND_OP_RAISE,value,0,0,1);
    return value;
}

/* Snapshots every Local in compiler->locals[start_index, end_index) --
 * a scope that's about to close, whether a whole function body
 * (compile_definition) or just one `rescue` clause (compile_begin) --
 * into compiler->function->scope_locals (DiamondScopeLocal, src/vm.h).
 * A local's own valid_start is just its name token's own byte offset
 * (already exactly right: the moment its declaring `Local` was pushed
 * is the moment it became valid), so the only new information this
 * needs from the caller is `valid_end`, the closing scope's own byte
 * offset. Silently stops recording past DIAMOND_MAX_SCOPE_LOCALS
 * (see that constant's own comment) rather than failing compilation --
 * this is an LSP-only convenience feature (docs/lsp.md), losing some
 * completions for a pathological function isn't worth a hard error
 * over. */
static void record_scope_locals(Compiler *compiler,size_t start_index,
        size_t end_index,size_t valid_end) {
    DiamondFunction *function=compiler->function;
    for(size_t index=start_index;index<end_index;index++) {
        if(function->scope_local_count==DIAMOND_MAX_SCOPE_LOCALS)return;
        const Local *local=&compiler->locals[index];
        DiamondScopeLocal *recorded=
            &function->scope_locals[function->scope_local_count++];
        size_t length=local->name.length;
        if(length>=DIAMOND_MAX_FUNCTION_NAME)length=DIAMOND_MAX_FUNCTION_NAME-1;
        for(size_t char_index=0;char_index<length;char_index++)
            recorded->name[char_index]=compiler->source[local->name.start+char_index];
        recorded->name[length]='\0';
        recorded->valid_start=local->name.start;
        recorded->valid_end=valid_end;
    }
}

static uint8_t compile_begin(Compiler *compiler) {
    if(!consume_block_start(compiler))return 0;
    const size_t ensure_operand=compiler->function->code_count+1;
    emit_opcode(compiler,DIAMOND_OP_PUSH_ENSURE);
    emit_byte(compiler,0);emit_byte(compiler,0);
    const uint8_t exception=allocate_register(compiler);
    const size_t retry_target=compiler->function->code_count;
    const size_t handler_type_operand=compiler->function->code_count+2;
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
    size_t rescue_end_jumps[16];size_t rescue_count=0;
    uint8_t seen_rescue_types[128];size_t seen_rescue_type_count=0;
    bool saw_rescue=false,catch_all=false;
    while(compiler->current.kind==DIAMOND_TOKEN_RESCUE&&!compiler->failed) {
        if(catch_all) {
            fail(compiler,compiler->current.span,
                 "rescue clause after catch-all is unreachable");break;
        }
        if(rescue_count==16) {
            fail(compiler,compiler->current.span,"too many rescue clauses");break;
        }
        saw_rescue=true;
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
        }
        uint8_t rescue_types[8];size_t type_count=0;
        if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
            advance_token(compiler);
            while(!compiler->failed) {
                if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                    fail(compiler,compiler->current.span,"expected rescue type");break;
                }
                if(type_count==8) {
                    fail(compiler,compiler->current.span,"too many rescue types");break;
                }
                const uint8_t rescue_type=(uint8_t)resolve_type(
                    compiler,compiler->current.span);
                for(size_t existing=0;existing<type_count;existing++)
                    if(rescue_types[existing]==rescue_type)
                        fail(compiler,compiler->current.span,
                             "duplicate rescue type");
                for(size_t existing=0;existing<seen_rescue_type_count;existing++)
                    if(seen_rescue_types[existing]==rescue_type)
                        fail(compiler,compiler->current.span,
                             "rescue type was already handled");
                    else if(seen_rescue_types[existing]>=DIAMOND_TYPE_CLASS_BASE&&
                            rescue_type>=DIAMOND_TYPE_CLASS_BASE) {
                        size_t child=(size_t)(rescue_type-DIAMOND_TYPE_CLASS_BASE);
                        const size_t ancestor=(size_t)(
                            seen_rescue_types[existing]-DIAMOND_TYPE_CLASS_BASE);
                        while(child<compiler->program->class_count&&child!=ancestor&&
                              compiler->program->classes[child].superclass!=UINT8_MAX)
                            child=compiler->program->classes[child].superclass;
                        if(child==ancestor)
                            fail(compiler,compiler->current.span,
                                 "rescue type is covered by an earlier clause");
                    }
                rescue_types[type_count++]=rescue_type;
                seen_rescue_types[seen_rescue_type_count++]=rescue_type;
                advance_token(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
                advance_token(compiler);
            }
        }
        size_t match_jumps[8];size_t mismatch_jump=SIZE_MAX;
        for(size_t index=0;index<type_count;index++) {
            const uint8_t matched=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,matched,exception,
                             rescue_types[index],3);
            match_jumps[index]=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,matched);
        }
        if(type_count>0)mismatch_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
        if(!consume_block_start(compiler))return destination;
        for(size_t index=0;index<type_count;index++)
            patch_jump(compiler,match_jumps[index],compiler->function->code_count);
        const int outer_exception=compiler->current_exception;
        const size_t outer_retry_target=compiler->current_retry_target;
        compiler->current_exception=exception;
        compiler->current_retry_target=retry_target;
        const uint8_t rescued=compile_sequence(compiler);
        compiler->current_exception=outer_exception;
        compiler->current_retry_target=outer_retry_target;
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,rescued,0,2);
        if(!compiler->failed)
            record_scope_locals(compiler,rescue_local_count,compiler->local_count,
                compiler->previous.span.start+compiler->previous.span.length);
        compiler->local_count=rescue_local_count;
        rescue_end_jumps[rescue_count++]=emit_jump(compiler,DIAMOND_OP_JUMP,0);
        if(mismatch_jump!=SIZE_MAX)
            patch_jump(compiler,mismatch_jump,compiler->function->code_count);
        else catch_all=true;
    }
    if(saw_rescue&&!catch_all)
        emit_instruction(compiler,DIAMOND_OP_RAISE,exception,0,0,1);
    if(compiler->current.kind!=DIAMOND_TOKEN_ENSURE&&!saw_rescue) {
        fail(compiler,compiler->current.span,"expected 'rescue' or 'ensure' after begin body");
        return destination;
    }
    patch_jump(compiler,end_jump,compiler->function->code_count);
    if(compiler->current.kind==DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(!consume_block_start(compiler))return destination;
        const uint8_t normal=compile_sequence(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,normal,0,2);
    }
    for(size_t index=0;index<rescue_count;index++)
        patch_jump(compiler,rescue_end_jumps[index],compiler->function->code_count);
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

static uint8_t compile_retry(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current_retry_target==SIZE_MAX) {
        fail(compiler,keyword,"'retry' used outside rescue");return 0;
    }
    emit_absolute_jump(compiler,compiler->current_retry_target);
    const uint8_t result=allocate_register(compiler);
    /* Dead code: the unconditional jump above means this NIL would
     * never execute at runtime, on top of being a sole-writer register
     * run_chunk's zero-init already covers. */
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}

static uint8_t compile_loop_control(Compiler *compiler) {
    const DiamondTokenKind kind=compiler->current.kind;
    const DiamondSpan keyword=compiler->current.span;
    if(compiler->current_loop==nullptr) {
        fail(compiler,keyword,kind==DIAMOND_TOKEN_BREAK
            ? "'break' used outside a loop":kind==DIAMOND_TOKEN_NEXT
            ? "'next' used outside a loop":"'redo' used outside a loop");
        return 0;
    }
    advance_token(compiler);
    const bool has_value=compiler->current.kind!=DIAMOND_TOKEN_NEWLINE&&
       compiler->current.kind!=DIAMOND_TOKEN_END&&
       compiler->current.kind!=DIAMOND_TOKEN_ELSE&&
       compiler->current.kind!=DIAMOND_TOKEN_EOF;
    const bool has_modifier = compiler->current.kind == DIAMOND_TOKEN_IF ||
                              compiler->current.kind == DIAMOND_TOKEN_UNLESS;
    const bool actual_value = has_value && !has_modifier;
    if(kind!=DIAMOND_TOKEN_BREAK&&actual_value) {
        fail(compiler,compiler->current.span,
             "next and redo do not accept values");
        return 0;
    }
    if(kind==DIAMOND_TOKEN_BREAK) {
        if(compiler->current_loop->break_count==64) {
            fail(compiler,keyword,"too many break statements in loop");
            return 0;
        }
        if(actual_value) {
            const uint8_t value=parse_expression(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,
                             compiler->current_loop->result_register,value,0,2);
        }
        compiler->current_loop->breaks[compiler->current_loop->break_count++]=
            emit_jump(compiler,DIAMOND_OP_JUMP,0);
    } else if(kind==DIAMOND_TOKEN_NEXT) {
        emit_absolute_jump(compiler,compiler->current_loop->continue_target);
    } else {
        emit_absolute_jump(compiler,compiler->current_loop->redo_target);
    }
    const uint8_t result=allocate_register(compiler);
    /* Dead code: break/next/redo all jump unconditionally above, so
     * this NIL never executes -- also a sole-writer register run_chunk's
     * zero-init already covers even if it somehow did. */
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}

static uint8_t compile_definition(Compiler *compiler) {
    const bool at_top_level = compiler->function == &compiler->program->entry;
    advance_token(compiler);
    bool module_singleton=false;
    if(compiler->current.kind==DIAMOND_TOKEN_SELF&&
       (compiler->current_module>=0||compiler->current_class>=0)) {
        module_singleton=true;advance_token(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_DOT) {
            fail(compiler,compiler->current.span,"expected '.' after 'self'");
            return 0;
        }
        advance_token(compiler);
    }
    /* Operator overloading: a method literally named "+"/"=="/etc. is
     * already legal at the VM level (lookup_method dispatches purely by
     * name-string + arity, no charset restriction) -- the only barrier is
     * this check. Unary minus is deliberately NOT included here: it's
     * named "negate", an ordinary identifier, recognized by name at
     * NEGATE's dispatch point the same way "to_s" is recognized for
     * stringification, needing no parser accommodation at all. */
    const bool operator_name =
        compiler->current.kind==DIAMOND_TOKEN_PLUS||
        compiler->current.kind==DIAMOND_TOKEN_MINUS||
        compiler->current.kind==DIAMOND_TOKEN_STAR||
        compiler->current.kind==DIAMOND_TOKEN_SLASH||
        compiler->current.kind==DIAMOND_TOKEN_EQUAL_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_LESS||
        compiler->current.kind==DIAMOND_TOKEN_LESS_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_GREATER||
        compiler->current.kind==DIAMOND_TOKEN_GREATER_EQUAL;
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER && !operator_name) {
        fail(compiler, compiler->current.span, "expected function name after 'def'");
        return 0;
    }
    if (operator_name && !(compiler->current_class>=0 && !module_singleton)) {
        fail(compiler, compiler->current.span,
             "operator methods can only be defined inside a class");
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
    if(compiler->current_class<0&&compiler->current_module<0&&
        find_function(compiler, name) >= 0) {
        fail(compiler, name, "function is already defined");
        return 0;
    }
    DiamondFunction *function =
        &compiler->program->functions[compiler->program->function_count++];
    function->return_type_set=UINT8_MAX;
    for(size_t index=0;index<16;index++)
        function->parameter_type_sets[index]=UINT8_MAX;
    const size_t function_index = compiler->program->function_count - 1;
    /* current_class/current_module are compiler-wide "lexically inside a
     * class/module body" flags, true for a nested closure at any depth,
     * not just a direct member -- direct_class_member/direct_module_member
     * narrow that to "is *this* def itself the direct member" (at_top_level
     * true means compiler->function, before this def switches it below,
     * was the program entry or another already-direct member's own body,
     * never a nested closure's). A closure nested *immediately* inside a
     * singleton method (`def self.make_patch(); def replacement(...); ...;
     * end; replacement; end`) is a deliberate exception: redefine_method's
     * patch-factory idiom (vm.c) relies on such a closure carrying the
     * same owner_class/self-register treatment a genuine instance method
     * gets, even though it's just a local closure value -- confirmed by
     * legacy_0093.di/legacy_0094.di/legacy_0095.di, which construct
     * exactly this shape and require it to work. A closure nested any
     * deeper, or inside an ordinary instance method, or inside a plain
     * top-level function, gets neither: it's an ordinary closure, no
     * implicit self of any kind. See docs/roadmap.md for the bug this
     * replaced (the blanket at_top_level-only gate that broke redefine_
     * method) and how it was found. */
    const bool direct_class_member=
        at_top_level&&compiler->current_class>=0&&!module_singleton;
    const bool direct_module_member=
        at_top_level&&compiler->current_module>=0&&!module_singleton;
    const bool nested_in_singleton_method=
        !at_top_level&&compiler->in_singleton_method;
    function->owner_class=
        (direct_class_member||(nested_in_singleton_method&&compiler->current_class>=0))?
            (uint8_t)compiler->current_class:
        (direct_module_member||(nested_in_singleton_method&&compiler->current_module>=0))?
            UINT8_MAX-1:UINT8_MAX;
    function->nested=!at_top_level;
    size_t copy_length=name.length;
    for (size_t index = 0; index < copy_length; index++) {
        function->name[index] = compiler->source[name.start + index];
    }
    function->name[copy_length] = '\0';
    function->declaration_line=(uint32_t)name.line;
    function->declaration_column=(uint32_t)name.column;
    function->declaration_start=name.start;
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        DiamondLexer lookahead=compiler->lexer;
        if(diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_LEFT_PAREN) {
            if(copy_length+1>=DIAMOND_MAX_FUNCTION_NAME) {
                fail(compiler,name,"writer method name is too long");return 0;
            }
            function->name[copy_length++]='=';
            function->name[copy_length]='\0';advance_token(compiler);
        }
    }
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
               function->type_variable_count==8) {
                fail(compiler,compiler->current.span,"expected generic type parameter");break;
            }
            for(size_t existing=0;existing<function->type_variable_count;existing++)
                if(name_equals(compiler,function->type_variables[existing],
                               compiler->current.span,false)) {
                    fail(compiler,compiler->current.span,
                         "duplicate generic type parameter");break;
                }
            if(compiler->failed)break;
            char *type_variable=
                function->type_variables[function->type_variable_count++];
            if(compiler->current.span.length>=DIAMOND_MAX_FUNCTION_NAME) {
                fail(compiler,compiler->current.span,"generic type name is too long");break;
            }
            for(size_t index=0;index<compiler->current.span.length;index++)
                type_variable[index]=compiler->source[compiler->current.span.start+index];
            type_variable[compiler->current.span.length]='\0';advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
            skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET)
            fail(compiler,compiler->current.span,
                 "expected ']' after generic type parameters");
        else advance_token(compiler);
    }
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
    const bool outer_in_singleton_method = compiler->in_singleton_method;
    const bool outer_in_function=compiler->in_function;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
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
    compiler->in_singleton_method = at_top_level && module_singleton;
    compiler->current_loop=nullptr;
    compiler->current_exception=-1;
    compiler->current_retry_target=SIZE_MAX;
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
    if(direct_class_member||direct_module_member||nested_in_singleton_method) {
        (void)allocate_register(compiler);
        function->arity = 1;
        function->required_arity=1;
        compiler->current_method = name;
        compiler->in_method = true;
    }

    size_t parameter_count=0;
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        parameter_count=1;DiamondLexer lookahead=compiler->lexer;
        DiamondToken token=compiler->current;size_t nesting=0;
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
            } else if(token.kind==DIAMOND_TOKEN_COMMA&&nesting==0)parameter_count++;
            token=diamond_lexer_next(&lookahead);
        }
    }
    if(parameter_count>16) {
        fail(compiler,name,"functions cannot declare more than 16 parameters");
        parameter_count=16;
    }
    const uint8_t parameter_base=(uint8_t)compiler->next_register;
    for(size_t index=0;index<parameter_count;index++)(void)allocate_register(compiler);
    size_t declared_parameter_count=0;bool saw_default=false;
    skip_newlines(compiler);
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
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->current.span,"too many local variables");break;
            }
            const uint8_t parameter=(uint8_t)(parameter_base+declared_parameter_count);
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->current.span,.reg=parameter};
            if(declared_parameter_count<16) {
                const DiamondSpan parameter_name_span=compiler->current.span;
                size_t parameter_name_length=parameter_name_span.length;
                if(parameter_name_length>=DIAMOND_MAX_FUNCTION_NAME)
                    parameter_name_length=DIAMOND_MAX_FUNCTION_NAME-1;
                for(size_t index=0;index<parameter_name_length;index++)
                    function->parameter_names[declared_parameter_count][index]=
                        compiler->source[parameter_name_span.start+index];
                function->parameter_names[declared_parameter_count][parameter_name_length]='\0';
            }
            function->arity++;
            advance_token(compiler);
            int parameter_type=-1;DiamondSpan parameter_type_span={};
            if (compiler->current.kind == DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                parameter_type_span=compiler->current.span;
                const int type = parse_type_annotation(compiler);
                parameter_type=type;
                if(declared_parameter_count<16)
                    function->parameter_type_sets[declared_parameter_count]=
                        (uint8_t)type;
            }
            if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                saw_default=true;advance_token(compiler);
                const uint8_t provided=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,provided,
                                 (uint8_t)(function->arity-1),0,2);
                const size_t skip=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,provided);
                const uint8_t fallback=parse_expression(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,parameter,fallback,0,2);
                patch_jump(compiler,skip,compiler->function->code_count);
            } else if(saw_default) {
                fail(compiler,compiler->previous.span,
                     "required parameter cannot follow a default parameter");
            } else function->required_arity++;
            if(parameter_type>=0) {
                emit_type_check(compiler,parameter,(uint8_t)parameter_type,
                                parameter_type_span);
                compiler->known_type_sets[parameter]=(int16_t)parameter_type;
                const DiamondTypeSet *parameter_set=
                    &function->type_sets[(size_t)parameter_type];
                if(parameter_set->count==1)
                    compiler->known_types[parameter]=parameter_set->members[0].id;
            }
            declared_parameter_count++;
            skip_newlines(compiler);
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
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
        const DiamondTokenKind postfix=postfix_modifier_ahead(compiler);
        const bool has_postfix=postfix==DIAMOND_TOKEN_IF||
                               postfix==DIAMOND_TOKEN_UNLESS;
        const uint8_t postfix_result=has_postfix?allocate_register(compiler):0;
        const size_t condition_jump=has_postfix
            ? emit_jump(compiler,DIAMOND_OP_JUMP,0):SIZE_MAX;
        const size_t body_start=compiler->function->code_count;
        body_result=parse_expression(compiler);
        if(has_postfix) {
            if(compiler->current.kind!=postfix) {
                fail(compiler,compiler->current.span,"expected postfix condition");
            } else {
                advance_token(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,postfix_result,
                                 body_result,0,2);
                const size_t body_exit=emit_jump(compiler,DIAMOND_OP_JUMP,0);
                const size_t condition_start=compiler->function->code_count;
                const uint8_t condition=parse_expression(compiler);
                const size_t body_jump=emit_jump(
                    compiler,postfix==DIAMOND_TOKEN_IF
                        ? DIAMOND_OP_JUMP_IF_TRUE:DIAMOND_OP_JUMP_IF_FALSE,
                    condition);
                emit_instruction(compiler,DIAMOND_OP_NIL,postfix_result,0,0,1);
                patch_jump(compiler,condition_jump,condition_start);
                patch_jump(compiler,body_exit,compiler->function->code_count);
                patch_jump(compiler,body_jump,body_start);
                body_result=postfix_result;
            }
        }
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
    function->register_count=compiler->next_register;
    uint8_t captures[16];
    for(size_t i=0;i<compiler->capture_count;i++)captures[i]=compiler->capture_registers[i];
    const size_t capture_count=compiler->capture_count;
    if(!compiler->failed)
        record_scope_locals(compiler,0,compiler->local_count,
            compiler->previous.span.start+compiler->previous.span.length);
    compiler->function = outer_function;
    compiler->local_count = outer_local_count;
    for (size_t index = 0; index < outer_local_count; index++) {
        compiler->locals[index] = outer_locals[index];
    }
    compiler->next_register = outer_next_register;
    compiler->current_method = outer_method;
    compiler->in_method = outer_in_method;
    compiler->in_singleton_method = outer_in_singleton_method;
    compiler->in_function=outer_in_function;
    compiler->current_return_type=outer_return_type;
    compiler->current_return_type_span=outer_return_type_span;
    compiler->current_exception=outer_exception;
    compiler->current_retry_target=outer_retry_target;
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
    if(compiler->current_class>=0&&!module_singleton&&
       !compiler->failed&&at_top_level) {
        DiamondClass *class = &compiler->program->classes[(size_t)compiler->current_class];
        bool duplicate=false;
        for(size_t existing=0;existing<class->method_count;existing++)
            if(!class->methods[existing].included&&
               strcmp(class->methods[existing].name,function->name)==0)
                duplicate=true;
        if(class->method_count==DIAMOND_MAX_METHODS||duplicate) {
            fail(compiler, name, "duplicate or excessive method definition");
        } else {
            DiamondMethod *method = &class->methods[class->method_count++];
            for (size_t i=0;i<copy_length;i++) method->name[i]=function->name[i];
            method->name[copy_length]='\0';
            method->function_index=(uint16_t)function_index;
            method->arity=(uint8_t)(function->arity-1);
            method->required_arity=(uint8_t)(function->required_arity-1);
            method->included=false;
            method->is_private=compiler->methods_private;
        }
    } else if(compiler->current_class>=0&&module_singleton&&
              !compiler->failed&&at_top_level) {
        DiamondClass *class=
            &compiler->program->classes[(size_t)compiler->current_class];
        bool duplicate=false;
        for(size_t existing=0;existing<class->singleton_method_count;existing++)
            if(strcmp(class->singleton_methods[existing].name,
                      function->name)==0)duplicate=true;
        if(duplicate||class->singleton_method_count==DIAMOND_MAX_METHODS)
            fail(compiler,name,"duplicate or excessive class singleton method");
        else {
            DiamondMethod *method=
                &class->singleton_methods[class->singleton_method_count++];
            for(size_t i=0;i<copy_length;i++)method->name[i]=function->name[i];
            method->name[copy_length]='\0';method->function_index=(uint16_t)function_index;
            method->arity=function->arity;method->required_arity=function->required_arity;
        }
    } else if(compiler->current_module>=0&&!module_singleton&&
              !compiler->failed&&at_top_level) {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
        for(size_t existing=0;existing<module->method_count;existing++)
            if(!module->methods[existing].included&&
               strcmp(module->methods[existing].name,function->name)==0) {
                fail(compiler,name,"duplicate module method");break;
            }
        if(!compiler->failed) {
            if(module->method_count==DIAMOND_MAX_METHODS)
                fail(compiler,name,"too many module methods");
            else {
                DiamondMethod *method=&module->methods[module->method_count++];
                for(size_t i=0;i<copy_length;i++)method->name[i]=function->name[i];
                method->name[copy_length]='\0';
                method->function_index=(uint16_t)function_index;
                method->arity=(uint8_t)(function->arity-1);
                method->required_arity=(uint8_t)(function->required_arity-1);
                method->included=false;
                method->is_private=compiler->methods_private;
                if(compiler->module_function_mode) {
                    if(function->uses_instance_state)
                        fail(compiler,name,
                            "stateful method cannot use module_function mode");
                    else if(module->singleton_method_count==DIAMOND_MAX_METHODS)
                        fail(compiler,name,"too many module singleton functions");
                    else {
                        method->is_private=true;
                        DiamondMethod exported=*method;
                        exported.is_private=false;exported.needs_receiver=true;
                        module->singleton_methods[
                            module->singleton_method_count++]=exported;
                    }
                }
            }
        }
    } else if(compiler->current_module>=0&&module_singleton&&
              !compiler->failed&&at_top_level) {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
        bool duplicate=false;
        for(size_t existing=0;existing<module->singleton_method_count;existing++)
            if(strcmp(module->singleton_methods[existing].name,
                      function->name)==0)duplicate=true;
        if(duplicate||module->singleton_method_count==DIAMOND_MAX_METHODS)
            fail(compiler,name,"duplicate or excessive module singleton function");
        else {
            DiamondMethod *method=
                &module->singleton_methods[module->singleton_method_count++];
            for(size_t i=0;i<copy_length;i++)method->name[i]=function->name[i];
            method->name[copy_length]='\0';
            method->function_index=(uint16_t)function_index;
            method->arity=function->arity;
            method->required_arity=function->required_arity;
        }
    }
    /* A class/module member def's "value" is never read: both call sites
     * that reach this point with at_top_level true and current_class/
     * current_module set (compile_class's and compile_module's own
     * DIAMOND_TOKEN_DEF branches) discard compile_definition's return with
     * an explicit (void). Only a genuine top-level def -- one that could be
     * the final statement compile_sequence threads through as the whole
     * sequence's value -- needs a real, permanently-reserved register here.
     * Register allocation is monotonic and never recycled within a
     * function body (see docs/roadmap.md's self-hosting register-budget
     * notes), so for a large class this reservation is pure, cumulative
     * waste against the entry function's own 256-register ceiling --
     * confirmed the hard way while porting the self-hosted parser
     * (docs/roadmap.md's Phase 3 follow-up File.open entry). */
    const bool member_result_discarded =
        at_top_level && (compiler->current_class>=0||compiler->current_module>=0);
    const uint8_t result =
        member_result_discarded ? 0 : allocate_register(compiler);
    if(!at_top_level) {
        /* Always emit BOX_LOCAL here, even if this local was already boxed
         * at an earlier capture site elsewhere in the function: that earlier
         * site might be in a sibling if/else branch that doesn't dominate
         * this one, so it may not actually have executed on this runtime
         * path. DIAMOND_OP_BOX_LOCAL is idempotent (a no-op if the register
         * already holds a Cell), so emitting it redundantly on paths where
         * the earlier boxing did run is always safe. */
        for(size_t i=0;i<capture_count;i++) {
            for(size_t local=0;local<compiler->local_count;local++) {
                if(compiler->locals[local].reg!=captures[i])continue;
                emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,captures[i],0,0,1);
                compiler->locals[local].captured=true;
                break;
            }
        }
        emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_byte(compiler,result);
        emit_function_index(compiler,function_index);emit_byte(compiler,(uint8_t)capture_count);
        for(size_t i=0;i<capture_count;i++)emit_byte(compiler,captures[i]);
        compiler->locals[compiler->local_count++]=(Local){.name=name,.reg=result};
    }
    /* else: top-level def -- no NIL needed. A genuine top-level def's
     * `result` is the sole writer of its (allocated) register, already
     * zero-inited; a class/module member's `result` is the unallocated
     * placeholder 0 from above, safe only because both callers that reach
     * this path discard it. */
    return result;
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
    (void)snprintf(method_name,sizeof method_name,"%s%s",field_name,
                   writer?"=":predicate?"?":"");
    if(compiler->current_class>=0) {
        DiamondClass *class=
            &compiler->program->classes[(size_t)compiler->current_class];
        for(size_t index=0;index<class->method_count;index++)
            if(!class->methods[index].included&&
               strcmp(class->methods[index].name,method_name)==0) {
                fail(compiler,name,"attribute method is already defined");return;
            }
        for(size_t index=0;index<class->field_count;index++)
            if(strcmp(class->fields[index],field_name)==0)field=(uint8_t)index;
        if(field==UINT8_MAX) {
            if(class->field_count==DIAMOND_MAX_FIELDS) {
                fail(compiler,name,"too many instance variables");return;
            }
            field=(uint8_t)class->field_count;
            (void)snprintf(class->fields[class->field_count++],
                DIAMOND_MAX_FUNCTION_NAME,"%s",field_name);
        }
        if(class->method_count==DIAMOND_MAX_METHODS) {
            fail(compiler,name,"too many methods");return;
        }
        method=&class->methods[class->method_count++];
    } else {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
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
            if(module->field_count==DIAMOND_MAX_FIELDS) {
                fail(compiler,name,"too many module instance variables");return;
            }
            (void)snprintf(module->fields[module->field_count++],
                DIAMOND_MAX_FUNCTION_NAME,"%s",field_name);
        }
        if(module->method_count==DIAMOND_MAX_METHODS) {
            fail(compiler,name,"too many module methods");return;
        }
        method=&module->methods[module->method_count++];method->included=false;
    }
    DiamondFunction *function=
        &compiler->program->functions[compiler->program->function_count];
    const uint16_t function_index=(uint16_t)compiler->program->function_count++;
    (void)snprintf(function->name,sizeof function->name,"%s",method_name);
    function->owner_class=compiler->current_class>=0?
        (uint8_t)compiler->current_class:UINT8_MAX-1;
    function->arity=writer?2:1;function->required_arity=function->arity;
    function->return_type_set=UINT8_MAX;
    for(size_t index=0;index<16;index++)function->parameter_type_sets[index]=UINT8_MAX;
    if(type_set>=0) {
        function->type_set_count=compiler->function->type_set_count;
        memcpy(function->type_sets,compiler->function->type_sets,
               function->type_set_count*sizeof(DiamondTypeSet));
        if(writer)function->parameter_type_sets[0]=(uint8_t)type_set;
        else function->return_type_set=(uint8_t)type_set;
    }
    size_t code=0;
    if(writer&&type_set>=0) {
        function->code[code++]=DIAMOND_OP_CHECK_TYPE;
        function->code[code++]=1;
        function->code[code++]=(uint8_t)type_set;
    }
    if(compiler->current_module>=0&&compiler->current_class<0) {
        function->uses_instance_state=true;
        DiamondStringConstant *string=&function->strings[0];
        (void)snprintf(string->chars,sizeof string->chars,"%s",field_name);
        string->length=strlen(field_name);function->string_count=1;
        function->code[code++]=(uint8_t)(writer?DIAMOND_OP_SET_IVAR_NAME:
                                          DIAMOND_OP_GET_IVAR_NAME);
        function->code[code++]=writer?0:1;function->code[code++]=0;
        function->code[code++]=writer?1:0;
    } else {
        function->code[code++]=(uint8_t)(writer?DIAMOND_OP_SET_IVAR:
                                          DIAMOND_OP_GET_IVAR);
        function->code[code++]=writer?0:1;
        function->code[code++]=writer?field:0;
        function->code[code++]=writer?1:field;
    }
    if(!writer&&type_set>=0) {
        function->code[code++]=DIAMOND_OP_CHECK_TYPE;
        function->code[code++]=1;
        function->code[code++]=(uint8_t)type_set;
    }
    function->code[code++]=DIAMOND_OP_RETURN;function->code[code++]=1;
    function->code_count=code;
    (void)snprintf(method->name,sizeof method->name,"%s",method_name);
    method->function_index=function_index;method->arity=writer?1:0;
    method->required_arity=method->arity;method->is_private=compiler->methods_private;
}

static void compile_attribute(Compiler *compiler,bool reader,bool writer,
                              bool predicate) {
    advance_token(compiler);
    const bool parenthesized=compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN;
    if(parenthesized) {advance_token(compiler);skip_newlines(compiler);}
    while(!compiler->failed) {
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected attribute name");return;
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

static void compile_visibility(Compiler *compiler,bool is_private) {
    advance_token(compiler);
    const bool parenthesized=compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN;
    if(parenthesized) {advance_token(compiler);skip_newlines(compiler);}
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        if(parenthesized) {
            fail(compiler,compiler->current.span,
                 "expected method name in visibility list");return;
        }
        compiler->methods_private=is_private;return;
    }
    while(!compiler->failed) {
        const DiamondSpan name=compiler->current.span;
        DiamondLexer name_lookahead=compiler->lexer;
        const bool writer_name=
            diamond_lexer_next(&name_lookahead).kind==DIAMOND_TOKEN_EQUAL;
        DiamondMethod *found=nullptr;
        if(compiler->current_class>=0) {
            DiamondClass *class=
                &compiler->program->classes[(size_t)compiler->current_class];
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
                &compiler->program->modules[(size_t)compiler->current_module];
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
        found->is_private=is_private;advance_token(compiler);
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
        &compiler->program->modules[(size_t)compiler->current_module];
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
            &compiler->program->functions[source->function_index];
        if(function->uses_instance_state) {
            fail(compiler,name,
                 "stateful module method cannot become a module_function");
            return;
        }
        for(size_t index=0;index<module->singleton_method_count;index++)
            if(strcmp(module->singleton_methods[index].name,source->name)==0) {
                fail(compiler,name,"module singleton function is already defined");
                return;
            }
        if(module->singleton_method_count==DIAMOND_MAX_METHODS) {
            fail(compiler,name,"too many module singleton functions");return;
        }
        source->is_private=true;
        DiamondMethod exported=*source;exported.needs_receiver=true;
        exported.is_private=false;
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
    DiamondMethod *methods=nullptr;size_t *count=nullptr;
    if(compiler->current_class>=0) {
        DiamondClass *class=
            &compiler->program->classes[(size_t)compiler->current_class];
        methods=class->methods;count=&class->method_count;
    } else {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
        methods=module->methods;count=&module->method_count;
    }
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
    if(*count==DIAMOND_MAX_METHODS) {
        fail(compiler,alias,"too many methods");return;
    }
    DiamondMethod copied=*source;
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

static uint8_t compile_class(Compiler *compiler) {
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER ||
        compiler->program->class_count == DIAMOND_MAX_CLASSES) {
        fail(compiler, compiler->current.span, "expected valid class name"); return 0;
    }
    DiamondSpan name=compiler->current.span;
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"class name is too long"); return 0;
    }
    if(find_class_name(compiler,stored_name)>=0||
       find_interface_name(compiler,stored_name)>=0||
       find_module_name(compiler,stored_name)>=0) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    const int index=(int)compiler->program->class_count++;
    DiamondClass *class=&compiler->program->classes[(size_t)index];
    class->superclass=UINT8_MAX;
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
    const bool outer_private=compiler->methods_private;
    compiler->methods_private=false;
    while(!compiler->failed && compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind==DIAMOND_TOKEN_PRIVATE||
           compiler->current.kind==DIAMOND_TOKEN_PUBLIC) {
            const bool private_visibility=
                compiler->current.kind==DIAMOND_TOKEN_PRIVATE;
            compile_visibility(compiler,private_visibility);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            fail(compiler,compiler->current.span,
                 "module_function is only valid in modules");break;
        } else if(compiler->current.kind==DIAMOND_TOKEN_ALIAS_METHOD) {
            compile_alias_method(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_ATTR||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_READER||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_WRITER||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_ACCESSOR||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_PREDICATE) {
            const bool reader=compiler->current.kind!=DIAMOND_TOKEN_ATTR_WRITER;
            const bool writer=compiler->current.kind!=DIAMOND_TOKEN_ATTR_READER;
            const bool shorthand=compiler->current.kind==DIAMOND_TOKEN_ATTR;
            const bool predicate=
                compiler->current.kind==DIAMOND_TOKEN_ATTR_PREDICATE;
            compile_attribute(compiler,reader,
                              shorthand||predicate?false:writer,predicate);
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
                &compiler->program->modules[(size_t)module_index];
            for(size_t source=0;source<module->field_count;source++) {
                bool present=false;
                for(size_t field=0;field<class->field_count;field++)
                    if(strcmp(class->fields[field],module->fields[source])==0)
                        present=true;
                if(present)continue;
                if(class->field_count==DIAMOND_MAX_FIELDS) {
                    fail(compiler,include_span,
                         "included module adds too many fields");break;
                }
                (void)snprintf(class->fields[class->field_count++],
                    DIAMOND_MAX_FUNCTION_NAME,"%s",module->fields[source]);
            }
            if(compiler->failed)break;
            if(class->method_count+module->method_count>DIAMOND_MAX_METHODS) {
                fail(compiler,compiler->current.span,
                     "included module adds too many methods");break;
            }
            for(size_t method=0;method<module->method_count;method++) {
                class->methods[class->method_count]=module->methods[method];
                class->methods[class->method_count++].included=true;
            }
        } else if(compiler->current.kind==DIAMOND_TOKEN_DEF) {
            (void)compile_definition(compiler);
        } else {
            fail(compiler,compiler->current.span,
                 "expected method definition or include in class");break;
        }
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE) skip_newlines(compiler);
    }
    compiler->current_class=outer;
    compiler->methods_private=outer_private;
    if(compiler->current.kind==DIAMOND_TOKEN_END) advance_token(compiler);
    const uint8_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

static uint8_t compile_module(Compiler *compiler) {
    if(compiler->function!=&compiler->program->entry) {
        fail(compiler,compiler->current.span,
             "modules must be declared at top level");return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       compiler->program->module_count==DIAMOND_MAX_MODULES) {
        fail(compiler,compiler->current.span,"expected valid module name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"module name is too long");return 0;
    }
    if(find_module_name(compiler,stored_name)>=0||
       find_class_name(compiler,stored_name)>=0||
       find_interface_name(compiler,stored_name)>=0) {
        fail(compiler,name,"module name is already defined");return 0;
    }
    const int index=(int)compiler->program->module_count++;
    DiamondModule *module=&compiler->program->modules[(size_t)index];
    (void)snprintf(module->name,sizeof module->name,"%s",stored_name);
    advance_token(compiler);
    if(!consume_block_start(compiler))return 0;
    const int outer=compiler->current_module;compiler->current_module=index;
    const bool outer_private=compiler->methods_private;
    const bool outer_module_function=compiler->module_function_mode;
    compiler->methods_private=false;
    compiler->module_function_mode=false;
    while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind==DIAMOND_TOKEN_PRIVATE||
           compiler->current.kind==DIAMOND_TOKEN_PUBLIC) {
            const bool private_visibility=
                compiler->current.kind==DIAMOND_TOKEN_PRIVATE;
            compile_visibility(compiler,private_visibility);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            compile_module_function(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_ALIAS_METHOD) {
            compile_alias_method(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_ATTR||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_READER||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_WRITER||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_ACCESSOR||
                  compiler->current.kind==DIAMOND_TOKEN_ATTR_PREDICATE) {
            const bool reader=compiler->current.kind!=DIAMOND_TOKEN_ATTR_WRITER;
            const bool writer=compiler->current.kind!=DIAMOND_TOKEN_ATTR_READER;
            const bool shorthand=compiler->current.kind==DIAMOND_TOKEN_ATTR;
            const bool predicate=
                compiler->current.kind==DIAMOND_TOKEN_ATTR_PREDICATE;
            compile_attribute(compiler,reader,
                              shorthand||predicate?false:writer,predicate);
        } else if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
           assignment_ahead(compiler)) {
            const DiamondSpan constant_name=compiler->current.span;
            const char first=compiler->source[constant_name.start];
            if(first<'A'||first>'Z') {
                fail(compiler,constant_name,
                     "module constants must begin with an uppercase letter");break;
            }
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%.*s",
                module->name,(int)constant_name.length,
                compiler->source+constant_name.start);
            if(written<0||(size_t)written>=sizeof qualified) {
                fail(compiler,constant_name,"constant name is too long");break;
            }
            if(find_namespace_constant_name(compiler,qualified)>=0) {
                fail(compiler,constant_name,"constant is already defined");break;
            }
            if(compiler->program->namespace_constant_count==
               DIAMOND_MAX_NAMESPACE_CONSTANTS) {
                fail(compiler,constant_name,"too many namespace constants");break;
            }
            const uint8_t constant=(uint8_t)
                compiler->program->namespace_constant_count++;
            (void)snprintf(compiler->program->namespace_constants[constant],
                DIAMOND_MAX_FUNCTION_NAME,"%s",qualified);
            advance_token(compiler);advance_token(compiler);
            const uint8_t value=parse_expression(compiler);
            emit_instruction(compiler,DIAMOND_OP_SET_NAMESPACE_CONSTANT,
                             constant,value,0,2);
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
            const int included=strstr(include_name,"::")==nullptr?
                lexical_module:find_module_name(compiler,include_name);
            if(included==index) {
                fail(compiler,include_span,
                     "module cannot include itself");break;
            }
            if(included<0) {
                fail(compiler,include_span,"undefined module");break;
            }
            const DiamondModule *source=
                &compiler->program->modules[(size_t)included];
            for(size_t imported=0;imported<source->field_count;imported++) {
                bool present=false;
                for(size_t field=0;field<module->field_count;field++)
                    if(strcmp(module->fields[field],source->fields[imported])==0)
                        present=true;
                if(present)continue;
                if(module->field_count==DIAMOND_MAX_FIELDS) {
                    fail(compiler,include_span,
                         "included module adds too many fields");break;
                }
                (void)snprintf(module->fields[module->field_count++],
                    DIAMOND_MAX_FUNCTION_NAME,"%s",source->fields[imported]);
            }
            if(compiler->failed)break;
            if(module->method_count+source->method_count>DIAMOND_MAX_METHODS) {
                fail(compiler,compiler->current.span,
                     "included module adds too many methods");break;
            }
            for(size_t method=0;method<source->method_count;method++) {
                module->methods[module->method_count]=source->methods[method];
                module->methods[module->method_count++].included=true;
            }
        } else if(compiler->current.kind==DIAMOND_TOKEN_DEF) {
            (void)compile_definition(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE) {
            (void)compile_module(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_CLASS) {
            (void)compile_class(compiler);
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
    compiler->module_function_mode=outer_module_function;
    if(compiler->current.kind==DIAMOND_TOKEN_END)advance_token(compiler);
    const uint8_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
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
    char stored_name[DIAMOND_MAX_FUNCTION_NAME];
    if(!declaration_name(compiler,stored_name,sizeof stored_name,name)) {
        fail(compiler,name,"interface name is too long");return 0;
    }
    if(find_interface_name(compiler,stored_name)>=0||
       find_class_name(compiler,stored_name)>=0||
       find_module_name(compiler,stored_name)>=0) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    DiamondInterface *interface=
        &compiler->program->interfaces[compiler->program->interface_count++];
    interface->type_sets=compiler->program->entry.type_sets;
    (void)snprintf(interface->name,sizeof interface->name,"%s",stored_name);
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
                &compiler->program->interfaces[(size_t)base_index];
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
                if(interface->method_count==DIAMOND_MAX_METHODS) {
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
           interface->method_count==DIAMOND_MAX_METHODS) {
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
        skip_newlines(compiler);
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
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

static uint8_t compile_assignment(Compiler *compiler) {
    const DiamondSpan name = compiler->current.span;
    const bool instance_variable =
        compiler->current.kind == DIAMOND_TOKEN_INSTANCE_VARIABLE;
    advance_token(compiler);
    advance_token(compiler);
    const uint8_t value = parse_expression(compiler);
    if (instance_variable) {
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint8_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_SET_IVAR_NAME,0,field,value,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_SET_IVAR,0,(uint8_t)field,
                             value,3);
        }
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
           compiler->current.kind == DIAMOND_TOKEN_ELSIF ||
           compiler->current.kind == DIAMOND_TOKEN_RESCUE ||
           compiler->current.kind == DIAMOND_TOKEN_ENSURE ||
           compiler->current.kind == DIAMOND_TOKEN_END;
}

static uint8_t compile_sequence(Compiler *compiler) {
    skip_newlines(compiler);
    uint8_t result = allocate_register(compiler);
    /* No NIL here: `result` is a sole writer for an empty block (still
     * correctly nil via run_chunk's zero-init), and is superseded by
     * the first statement's own result register whenever the block is
     * non-empty -- the highest-frequency NIL-elision site, since this
     * fires once per compiled block. */

    while (!compiler->failed && !at_block_end(compiler)) {
        const DiamondTokenKind postfix = postfix_modifier_ahead(compiler);
        const bool has_postfix = postfix == DIAMOND_TOKEN_IF ||
                                 postfix == DIAMOND_TOKEN_UNLESS;
        const uint8_t body_result_slot = result;
        const uint8_t postfix_result = has_postfix
            ? allocate_register(compiler)
            : body_result_slot;
        const size_t condition_jump = has_postfix
            ? emit_jump(compiler, DIAMOND_OP_JUMP, 0)
            : SIZE_MAX;
        const size_t body_start = compiler->function->code_count;
        const bool declaration_statement =
            compiler->current.kind == DIAMOND_TOKEN_DEF ||
            compiler->current.kind == DIAMOND_TOKEN_CLASS ||
            compiler->current.kind == DIAMOND_TOKEN_INTERFACE ||
            compiler->current.kind == DIAMOND_TOKEN_MODULE;
        if (compiler->current.kind == DIAMOND_TOKEN_DEF) {
            result = compile_definition(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_CLASS) {
            result = compile_class(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_INTERFACE) {
            result = compile_interface(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE) {
            result=compile_module(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RETURN) {
            result=compile_return(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RAISE) {
            result=compile_raise(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_RETRY) {
            result=compile_retry(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_BEGIN) {
            advance_token(compiler);
            result=compile_begin(compiler);
        } else if (compiler->current.kind == DIAMOND_TOKEN_BREAK ||
                   compiler->current.kind == DIAMOND_TOKEN_NEXT ||
                   compiler->current.kind == DIAMOND_TOKEN_REDO) {
            result=compile_loop_control(compiler);
        } else {
            if(index_assignment_ahead(compiler))
                result=compile_index_assignment(compiler);
            else
                result = assignment_ahead(compiler)
                    ? compile_assignment(compiler)
                    : parse_expression(compiler);
        }
        if (has_postfix) {
            if (compiler->current.kind != postfix) {
                fail(compiler, compiler->current.span,
                     "expected postfix condition");
                break;
            }
            advance_token(compiler);
            emit_instruction(compiler, DIAMOND_OP_MOVE, postfix_result,
                             result, 0, 2);
            const size_t body_exit = emit_jump(compiler, DIAMOND_OP_JUMP, 0);
            const size_t condition_start = compiler->function->code_count;
            const uint8_t condition = parse_expression(compiler);
            const size_t body_jump = emit_jump(
                compiler,
                postfix == DIAMOND_TOKEN_IF
                    ? DIAMOND_OP_JUMP_IF_TRUE
                    : DIAMOND_OP_JUMP_IF_FALSE,
                condition);
            emit_instruction(compiler, DIAMOND_OP_NIL, postfix_result,
                             0, 0, 1);
            patch_jump(compiler, condition_jump, condition_start);
            patch_jump(compiler, body_exit, compiler->function->code_count);
            patch_jump(compiler, body_jump, body_start);
            result = postfix_result;
        }
        if (!has_postfix && declaration_statement &&
            (compiler->current.kind == DIAMOND_TOKEN_IF ||
             compiler->current.kind == DIAMOND_TOKEN_UNLESS)) {
            fail(compiler, compiler->current.span,
                 "postfix modifiers cannot follow declarations");
        } else if (compiler->current.kind == DIAMOND_TOKEN_NEWLINE) {
            skip_newlines(compiler);
        } else if (!at_block_end(compiler)) {
            fail(compiler, compiler->current.span, "expected newline after expression");
        }
    }
    return result;
}

void diamond_program_init(DiamondProgram *program) {
    /* memset rather than `*program = (DiamondProgram){};`: a compound-literal
     * assignment materializes a full temporary DiamondProgram (3MB+) on this
     * function's own stack frame regardless of where `program` itself points,
     * which is unsafe for any caller running at nontrivial stack depth (e.g.
     * a required package's manifest, compiled from inside expand()'s own
     * recursive call chain, while the top-level program's own DiamondProgram
     * is still live further up the stack in main.c's run_source). */
    memset(program, 0, sizeof *program);
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
        [DIAMOND_CLASS_FIBER_ERROR]={"FiberError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_IO_ERROR]={"IOError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_REGEXP_ERROR]={"RegexpError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_WOULD_BLOCK_ERROR]={"WouldBlockError",DIAMOND_CLASS_STANDARD_ERROR},
    };
    program->class_count=DIAMOND_BUILTIN_CLASS_COUNT;
    for(size_t index=0;index<DIAMOND_BUILTIN_CLASS_COUNT;index++) {
        DiamondClass *class=&program->classes[index];
        (void)snprintf(class->name,sizeof class->name,"%s",builtins[index].name);
        class->superclass=builtins[index].superclass;
        class->field_count=2;
        (void)snprintf(class->fields[0],DIAMOND_MAX_FUNCTION_NAME,"message");
        (void)snprintf(class->fields[1],DIAMOND_MAX_FUNCTION_NAME,"cause");
        /* diamond_compile only computes shapes for every class (built-in
         * and user-declared) once compilation finishes -- done here too,
         * scoped to just these built-ins, so a program that never gets
         * that far (e.g. a ProgramBuilder that only ever calls this
         * function, never diamond_compile) still has instantiable
         * built-in exception classes from construction on, the same
         * guarantee diamond_compile itself provides. */
        for(size_t field_count=0;field_count<=class->field_count;field_count++) {
            class->shapes[field_count]=(DiamondShape){
                .class=class,.field_count=(uint8_t)field_count};
        }
    }
    snprintf(program->entry.name, sizeof(program->entry.name), "<main>");
}

DiamondResolvedLocation diamond_resolve_diagnostic_location(
        const char *name,const char *source,DiamondDiagnostic diagnostic,
        const DiamondSourceBundle *bundle,size_t user_offset) {
    size_t line=diagnostic.span.line;
    bool mapped_segment=false;
    size_t mapped_segment_start=0;
    size_t mapped_segment_end=0;
    size_t mapped_original_line=1;
    if(diagnostic.span.start>=user_offset) {
        const size_t offset=diagnostic.span.start-user_offset;
        for(size_t index=0;index<bundle->segment_count;index++) {
            const DiamondSourceSegment *segment=&bundle->segments[index];
            if(offset<segment->start||
               (offset>segment->end&&offset-segment->end>9))continue;
            name=segment->path;
            mapped_segment=true;
            mapped_segment_start=user_offset+segment->start;
            mapped_segment_end=user_offset+segment->end;
            mapped_original_line=segment->original_line;
            break;
        }
    }
    size_t line_start=diagnostic.span.start;
    if(mapped_segment&&line_start>=mapped_segment_end)
        line_start=mapped_segment_end;
    if(mapped_segment&&line_start>0&&source[line_start]=='\n')line_start--;
    if(mapped_segment&&source[line_start]=='#'&&line_start>0) {
        line_start--;
        while(line_start>0&&source[line_start-1]!='\n')line_start--;
    }
    while(line_start>0&&source[line_start-1]!='\n')line_start--;
    if(mapped_segment) {
        line=mapped_original_line;
        for(size_t index=mapped_segment_start;index<line_start;index++)
            if(source[index]=='\n')line++;
    }
    size_t line_end=line_start;
    while(source[line_end]!='\0'&&source[line_end]!='\n')line_end++;
    return (DiamondResolvedLocation){.path=name,.line=line,
        .column=diagnostic.span.column,.line_start=line_start,.line_end=line_end};
}

size_t diamond_resolve_source_position(const char *path,const char *combined,
        const DiamondSourceBundle *bundle,size_t user_offset,
        size_t line,size_t column) {
    for(size_t index=0;index<bundle->segment_count;index++) {
        const DiamondSourceSegment *segment=&bundle->segments[index];
        if(strcmp(segment->path,path)!=0)continue;
        const size_t segment_start=user_offset+segment->start;
        const size_t segment_end=user_offset+segment->end;
        size_t segment_line=segment->original_line;
        size_t offset=segment_start;
        while(offset<segment_end&&segment_line<line) {
            if(combined[offset]=='\n')segment_line++;
            offset++;
        }
        if(segment_line!=line)continue;
        size_t result=offset;
        for(size_t moved=1;moved<column&&result<segment_end&&
                combined[result]!='\n';moved++)
            result++;
        return result;
    }
    return SIZE_MAX;
}

bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic) {
    diamond_program_init(program);
    *diagnostic = (DiamondDiagnostic){};
    Compiler compiler = {
        .source = source,
        .program = program,
        .function = &program->entry,
        .current_class = -1,
        .current_module = -1,
        .current_return_type = -1,
        .current_exception = -1,
        .current_retry_target = SIZE_MAX,
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
    program->entry.register_count = compiler.next_register;
    if (!compiler.failed) {
        record_scope_locals(&compiler,0,compiler.local_count,strlen(source));
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
        .name = program->entry_path[0] != '\0'
            ? program->entry_path : program->entry.name,
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
        .register_count=program->entry.register_count,
    };
}
