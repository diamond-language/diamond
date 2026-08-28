#include "compiler.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { TYPE_UNKNOWN = UINT8_MAX };

typedef enum Precedence {
    PREC_NONE,
    /* Looser than every other binary operator (including ||/&&), matching
     * Ruby's own precedence table -- `a > 0 .. b < 10` reads as
     * `(a>0)..(b<10)`, and `1..n+1` reads as `1..(n+1)`. */
    PREC_RANGE,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_SHIFT,
    PREC_TERM,
    PREC_FACTOR,
    PREC_PREFIX,
} Precedence;

typedef struct Local {
    DiamondSpan name;
    uint16_t reg;
    bool captured;
} Local;

typedef struct LoopContext {
    struct LoopContext *previous;
    size_t continue_target;
    size_t redo_target;
    uint16_t result_register;
    size_t breaks[64];
    size_t break_count;
    size_t flow_reg_count;
    uint8_t *exit_types;
    int32_t *exit_sets;
    bool exit_initialized;
    uint8_t result_type;
    int32_t result_set;
} LoopContext;

enum { DIAMOND_MAX_NARROWING_FACTS = 8 };

typedef struct NarrowingFact {
    uint16_t reg;
    int32_t type_set;
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
    uint16_t condition;
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
    bool methods_protected;
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
    /* True while compiling a while/until/loop's own condition+body once
     * a def/closure has been found anywhere within it (loop_body_may_
     * capture) -- makes every local reference/declaration from that point
     * go through the existing .captured-flag-driven BOX_LOCAL/GET_CELL/
     * SET_CELL codegen (parse_identifier, compile_assignment_store,
     * define_local) from its very first compilation, rather than only
     * from wherever the actual capturing def/closure happens to sit in
     * source order. Fixes a real bug: a loop body is compiled once and
     * reached again via a jump back, so any reference compiled *before*
     * a mid-body capture was discovered stays a stale raw-register read
     * that only a lucky first iteration (box hasn't happened yet)
     * survives -- confirmed directly, not assumed, see docs/roadmap.md.
     * Sticky across nested loops (an inner loop reached while this is
     * already true skips its own scan/premark -- see parse_while/
     * parse_loop's own comments). */
    bool loop_captures_pending;
    uint8_t known_types[DIAMOND_REGISTER_COUNT];
    int32_t known_type_sets[DIAMOND_REGISTER_COUNT];
    bool in_function;
    /* The explicit `&name` parameter for the function currently being
     * compiled. In that lexical context `yield(...)` invokes this Callable;
     * without one, yield retains its fiber-suspension meaning. */
    bool has_current_block;
    uint16_t current_block_register;
    uint16_t current_block_type_set;
    bool has_contextual_block_types;
    uint8_t contextual_block_arity;
    uint8_t contextual_block_types[16];
    int32_t contextual_block_type_sets[16];
    int32_t contextual_block_return_set;
    int32_t expected_expression_type_set;
    bool positional_spread_literal;
    size_t positional_spread_first;
    size_t positional_spread_count;
    uint16_t positional_spread_elements[32];
    size_t positional_spread_fixed_count;
    size_t positional_spread_index;
    uint16_t positional_spread_fixed[16];
    int current_return_type;
    DiamondSpan current_return_type_span;
    LoopContext *current_loop;
    int current_exception;
    size_t current_retry_target;
    bool failed;
    Local enclosing_locals[DIAMOND_MAX_LOCALS];
    size_t enclosing_local_count;
    uint16_t capture_registers[16];
    size_t capture_count;
    Narrowing narrowing;
    /* Set by compile_sequence right before it returns, true only when the
     * sequence's own last top-level statement was a bare `raise` -- that
     * statement's "result" register (compile_raise returns the raised
     * value itself, e.g. an exception instance) never actually reaches a
     * caller normally, since raise always unwinds instead of completing.
     * Read immediately after a compile_sequence call by whichever of
     * compile_function_body/compile_method_body's endless-form branch
     * needs it, to skip a doomed-to-fail return-type check against a
     * value that was never meant to satisfy the return type in the first
     * place -- there's no "never returns" type this could otherwise be
     * checked against. Not itself return-type-check logic, just a signal
     * of whether that check applies at all this time. */
    bool sequence_diverges;
    /* True only during diamond_compile's first, throwaway pass over the
     * source (see diamond_compile's own comment) -- lets a
     * forward reference to a not-yet-declared class/module/interface
     * name (used as a value: construction, a singleton call, a type
     * annotation -- never a superclass/base-interface, which still needs
     * the referenced declaration fully compiled already, not just known
     * by name) survive instead of aborting compilation, so this pass can
     * walk the *entire* source and fully register every declaration's
     * name/fields/methods regardless of textual order. This pass's own
     * bytecode is discarded; declaration tables and function slots survive
     * into the real second pass. Unknown bare calls/function values are also
     * tolerated here so their later top-level declarations can be recorded.
     * The real second pass runs with discovery_pass false and behaves as
     * a normal compile always has, except every declaration is already
     * known up front. */
    bool discovery_pass;
    /* Real-pass cursor through discovery's pre-reserved function slots. */
    size_t next_function_claim;
} Compiler;

static uint16_t parse_expression(Compiler *compiler);
static uint16_t compile_sequence(Compiler *compiler);
static uint16_t compile_begin(Compiler *compiler);
static uint16_t compile_yield(Compiler *compiler);
static uint16_t compile_interface(Compiler *compiler);
static int find_function(const Compiler *compiler, DiamondSpan name);
static void record_scope_type_fact(Compiler *compiler,uint16_t reg,
        size_t effective_start);
static uint16_t compile_assignment_store(Compiler *compiler,DiamondSpan name,
        bool instance_variable,bool class_variable,uint16_t value);

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
    if(compiler->function->code_count==compiler->function->code_capacity) {
        size_t capacity=compiler->function->code_capacity==0?256:
            compiler->function->code_capacity*2;
        if(capacity>DIAMOND_MAX_CODE)capacity=DIAMOND_MAX_CODE;
        if(!diamond_function_reserve_code(compiler->function,capacity)) {
            fail(compiler,compiler->previous.span,
                "out of memory growing function bytecode");
            return false;
        }
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

/* Big-endian, matching patch_jump/emit_absolute_jump's existing 16-bit
 * operand convention -- function indices are a CALL/CALL_TYPED/CLOSURE
 * operand wide enough to exceed one byte now that DIAMOND_MAX_FUNCTIONS
 * is 512 (see its own comment in src/vm.h). */
static bool emit_function_index(Compiler *compiler, size_t function_index) {
    return emit_byte(compiler, (uint8_t)(function_index >> 8)) &&
           emit_byte(compiler, (uint8_t)(function_index & UINT8_MAX));
}

/* Same big-endian 16-bit convention as emit_function_index, for a register
 * operand -- registers are wide enough to exceed one byte now that
 * DIAMOND_REGISTER_COUNT is 4096 (see its own comment in src/vm.h). */
static bool emit_register(Compiler *compiler, uint16_t reg) {
    return emit_byte(compiler, (uint8_t)(reg >> 8)) &&
           emit_byte(compiler, (uint8_t)(reg & UINT8_MAX));
}

/* Every emit_instruction operand is emitted 2 bytes wide via
 * emit_register, even the (more common) ones that are logically a
 * register -- and even the few that are actually a narrower index
 * (a type-set/constant/field/capture index, each with its own separate,
 * much smaller cap; see e.g. DIAMOND_MAX_TYPE_SETS/DIAMOND_MAX_FIELDS in
 * src/vm.h). This opcode operand slot mix varies per opcode, not
 * uniformly by position, so emit_instruction deliberately doesn't try to
 * track which -- uniformly widening every slot keeps this one shared
 * helper (and run_chunk's matching per-opcode reads, see READ_SHORT)
 * simple and consistent, at the cost of a couple of harmless extra bytes
 * per instruction for the operands that didn't strictly need them. */
static bool emit_instruction(Compiler *compiler, DiamondOpCode opcode,
                             uint16_t a, uint16_t b, uint16_t c, size_t operands) {
    if (!emit_opcode(compiler, opcode)) {
        return false;
    }
    const uint16_t values[] = {a, b, c};
    for (size_t index = 0; index < operands; index++) {
        if (!emit_register(compiler, values[index])) {
            return false;
        }
    }
    return true;
}

static uint16_t allocate_register(Compiler *compiler) {
    if (compiler->next_register >= DIAMOND_REGISTER_COUNT) {
        fail(compiler, compiler->previous.span, "program needs too many registers");
        return 0;
    }
    const uint16_t reg=(uint16_t)compiler->next_register++;
    compiler->known_types[reg]=TYPE_UNKNOWN;
    compiler->known_type_sets[reg]=-1;
    return reg;
}

static bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index);

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
                if(method->return_type_set!=DIAMOND_NO_TYPE_SET) {
                    if(native_return==UINT8_MAX)return false;
                    const DiamondTypeSet native={.members={{.id=native_return,
                        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
                        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
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
                            compiler->program->functions[class->methods[method].function_index];
                        found=true;
                        for(size_t parameter=0;parameter<wanted->arity;parameter++) {
                            const uint16_t required_set=wanted->parameter_type_sets[parameter];
                            const uint16_t actual_set=implementation->parameter_type_sets[parameter];
                            if(required_set==DIAMOND_NO_TYPE_SET) {
                                if(actual_set!=DIAMOND_NO_TYPE_SET)found=false;
                            } else if(actual_set!=DIAMOND_NO_TYPE_SET&&
                                !type_sets_satisfy_across(compiler,
                                    compiler->program->entry.type_sets,required_set,
                                    implementation->type_sets,actual_set))found=false;
                        }
                        if(wanted->return_type_set!=DIAMOND_NO_TYPE_SET&&
                           (implementation->return_type_set==DIAMOND_NO_TYPE_SET||
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

static bool type_set_satisfies(const Compiler *compiler,uint16_t known_index,
                               uint16_t expected_index) {
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
                const uint16_t wanted=expected.callable_parameter_sets[parameter];
                if(wanted==DIAMOND_NO_TYPE_SET)continue;
                const uint16_t actual=known.callable_parameter_sets[parameter];
                if(actual!=DIAMOND_NO_TYPE_SET&&
                   !type_sets_satisfy_across(compiler,expected_sets,wanted,
                                              known_sets,actual))return false;
            }
        return expected.callable_return_set==DIAMOND_NO_TYPE_SET||
            (known.callable_return_set!=DIAMOND_NO_TYPE_SET&&
             type_sets_satisfy_across(compiler,known_sets,
                 known.callable_return_set,expected_sets,
                 expected.callable_return_set));
    }
    if(expected.argument_set==DIAMOND_NO_TYPE_SET)return true;
    if(known.argument_set==DIAMOND_NO_TYPE_SET||
       !type_sets_satisfy_across(compiler,known_sets,known.argument_set,
                                 expected_sets,expected.argument_set))return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        type_sets_satisfy_across(compiler,known_sets,known.second_argument_set,
                                 expected_sets,expected.second_argument_set);
}

static bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index) {
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
                const uint16_t wanted=expected.callable_parameter_sets[parameter];
                if(wanted==DIAMOND_NO_TYPE_SET)continue;
                const uint16_t actual=known.callable_parameter_sets[parameter];
                if(actual!=DIAMOND_NO_TYPE_SET&&
                   !type_set_satisfies(compiler,wanted,actual))return false;
            }
        return expected.callable_return_set==DIAMOND_NO_TYPE_SET||
            (known.callable_return_set!=DIAMOND_NO_TYPE_SET&&
             type_set_satisfies(compiler,known.callable_return_set,
                                expected.callable_return_set));
    }
    if(expected.argument_set==DIAMOND_NO_TYPE_SET)return true;
    if(known.argument_set==DIAMOND_NO_TYPE_SET||
       !type_set_satisfies(compiler,known.argument_set,expected.argument_set))
        return false;
    if(expected.id!=DIAMOND_TYPE_HASH)return true;
    return known.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        expected.second_argument_set!=DIAMOND_NO_TYPE_SET&&
        type_set_satisfies(compiler,known.second_argument_set,
                           expected.second_argument_set);
}

static bool type_set_contains_variable(const Compiler *compiler,uint16_t set_index) {
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember member=set->members[index];
        if(member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           member.id<DIAMOND_TYPE_INTERFACE_BASE)return true;
        if(member.argument_set!=DIAMOND_NO_TYPE_SET&&
           type_set_contains_variable(compiler,member.argument_set))return true;
        if(member.second_argument_set!=DIAMOND_NO_TYPE_SET&&
           type_set_contains_variable(compiler,member.second_argument_set))return true;
        if(member.callable_return_set!=DIAMOND_NO_TYPE_SET&&
           type_set_contains_variable(compiler,member.callable_return_set))return true;
        if(member.callable_parameters_typed)
            for(size_t parameter=0;parameter<member.callable_arity;parameter++)
                if(type_set_contains_variable(compiler,
                       member.callable_parameter_sets[parameter]))return true;
    }
    return false;
}

static void emit_type_check(Compiler *compiler, uint16_t reg, uint16_t set_index,
                            DiamondSpan span) {
    if(type_set_contains_variable(compiler,set_index)) {
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
        return;
    }
    if(compiler->known_type_sets[reg]>=0) {
        const uint16_t known_set=(uint16_t)compiler->known_type_sets[reg];
        if(type_set_satisfies(compiler,known_set,set_index))return;
        if(compiler->function->type_sets[known_set].inferred) {
            emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
            return;
        }
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
        if(set->members[index].argument_set!=DIAMOND_NO_TYPE_SET)
            emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,reg,set_index,0,2);
        return;
    }
    fail(compiler,span,"expression cannot satisfy type annotation");
}

static uint16_t add_constant(Compiler *compiler, DiamondValue value) {
    if (compiler->function->constant_count == DIAMOND_MAX_CONSTANTS) {
        fail(compiler, compiler->previous.span, "program has too many constants");
        return 0;
    }
    if(compiler->function->constant_count==compiler->function->constant_capacity) {
        size_t capacity=compiler->function->constant_capacity==0?32:
            compiler->function->constant_capacity*2;
        if(capacity>DIAMOND_MAX_CONSTANTS)capacity=DIAMOND_MAX_CONSTANTS;
        if(!diamond_function_reserve_constants(compiler->function,capacity)) {
            fail(compiler,compiler->previous.span,
                "out of memory growing function constants");return 0;
        }
    }
    const size_t index = compiler->function->constant_count++;
    compiler->function->constants[index] = value;
    return (uint16_t)index;
}

static bool grow_type_sets(DiamondFunction *function,size_t additional) {
    if(additional>DIAMOND_MAX_TYPE_SETS-function->type_set_count)
        return false;
    const size_t needed=function->type_set_count+additional;
    if(needed>function->type_set_capacity) {
        size_t capacity=function->type_set_capacity==0?8:
            function->type_set_capacity*2;
        if(capacity<needed)capacity=needed;
        if(capacity>DIAMOND_MAX_TYPE_SETS)capacity=DIAMOND_MAX_TYPE_SETS;
        if(!diamond_function_reserve_type_sets(function,capacity))return false;
    }
    return true;
}

static bool reserve_type_sets(Compiler *compiler,size_t additional) {
    if(additional>DIAMOND_MAX_TYPE_SETS-compiler->function->type_set_count) {
        fail(compiler,compiler->current.span,"function has too many type annotations");
        return false;
    }
    if(grow_type_sets(compiler->function,additional))return true;
    fail(compiler,compiler->current.span,"out of memory");
    return false;
}

static uint16_t concrete_type_set(Compiler *compiler,uint8_t type) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==type&&
           set->members[0].argument_set==DIAMOND_NO_TYPE_SET&&
           set->members[0].second_argument_set==DIAMOND_NO_TYPE_SET)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return DIAMOND_NO_TYPE_SET;
    const size_t index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[index];
    set->count=1;set->inferred=true;
    set->members[0]=(DiamondTypeMember){.id=type,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        set->members[0].callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)index;
}

static bool type_members_equal(DiamondTypeMember left,DiamondTypeMember right);
static bool type_sets_equal_unordered(DiamondTypeSet left,DiamondTypeSet right);
static bool type_sets_structurally_equal(const DiamondTypeSet *left_sets,
        size_t left_count,uint16_t left_index,
        const DiamondTypeSet *right_sets,size_t right_count,
        uint16_t right_index,size_t depth);

static int32_t join_type_set_indices(Compiler *compiler,int32_t left,
        int32_t right) {
    if(left<0||right<0||(size_t)left>=compiler->function->type_set_count||
       (size_t)right>=compiler->function->type_set_count)return -1;
    if(left==right)return left;
    DiamondTypeSet joined=compiler->function->type_sets[(size_t)left];
    const DiamondTypeSet addition=compiler->function->type_sets[(size_t)right];
    for(size_t source=0;source<addition.count;source++) {
        const DiamondTypeMember member=addition.members[source];
        bool duplicate=false;
        for(size_t target=0;target<joined.count;target++) {
            if(type_members_equal(joined.members[target],member)) {
                duplicate=true;break;
            }
        }
        if(duplicate)continue;
        if(joined.count==DIAMOND_MAX_UNION_TYPES)return -1;
        joined.members[joined.count++]=member;
    }
    joined.inferred=true;
    for(size_t index=0;index<compiler->function->type_set_count;index++)
        if(type_sets_equal_unordered(joined,
                compiler->function->type_sets[index]))return (int32_t)index;
    if(!reserve_type_sets(compiler,1))return -1;
    const size_t index=compiler->function->type_set_count++;
    compiler->function->type_sets[index]=joined;
    return (int32_t)index;
}

static int32_t joined_value_type_set(Compiler *compiler,
        const uint16_t *values,size_t count) {
    if(count==0)return -1;
    int32_t joined=-1;
    for(size_t index=0;index<count;index++) {
        int32_t current=compiler->known_type_sets[values[index]];
        if(current<0) {
            const uint8_t type=compiler->known_types[values[index]];
            if(type==TYPE_UNKNOWN||type>=DIAMOND_TYPE_VARIABLE_BASE)return -1;
            current=(int32_t)concrete_type_set(compiler,type);
        }
        if(current<0||(size_t)current>=compiler->function->type_set_count)
            return -1;
        joined=joined<0?current:join_type_set_indices(compiler,joined,current);
        if(joined<0)return -1;
    }
    return joined;
}

static void record_collection_type_set(Compiler *compiler,uint16_t reg,
        uint8_t type,int32_t argument_set,int32_t second_argument_set) {
    if(argument_set<0||
       (type==DIAMOND_TYPE_HASH&&second_argument_set<0))return;
    const uint16_t second=second_argument_set<0?DIAMOND_NO_TYPE_SET:
        (uint16_t)second_argument_set;
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *known=&compiler->function->type_sets[index];
        if(known->count==1&&known->members[0].id==type&&
           known->members[0].argument_set==(uint16_t)argument_set&&
           known->members[0].second_argument_set==second) {
            compiler->known_type_sets[reg]=(int32_t)index;return;
        }
    }
    if(!reserve_type_sets(compiler,1))return;
    const size_t index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[index];
    set->count=1;set->inferred=true;
    set->members[0]=(DiamondTypeMember){.id=type,
        .argument_set=(uint16_t)argument_set,
        .second_argument_set=second,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        set->members[0].callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    compiler->known_type_sets[reg]=(int32_t)index;
}

static int32_t array_element_type_set(const Compiler *compiler,uint16_t reg) {
    const int32_t outer=compiler->known_type_sets[reg];
    if(outer<0||(size_t)outer>=compiler->function->type_set_count)return -1;
    const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)outer];
    if(set->count!=1||set->members[0].id!=DIAMOND_TYPE_ARRAY||
       set->members[0].argument_set==DIAMOND_NO_TYPE_SET)return -1;
    return (int32_t)set->members[0].argument_set;
}

static uint16_t clone_type_set_into_current_impl(Compiler *compiler,
        const DiamondTypeSet *source_sets,size_t source_count,
        uint16_t source_index,bool source_is_current) {
    if(source_index==DIAMOND_NO_TYPE_SET||source_index>=source_count)
        return DIAMOND_NO_TYPE_SET;
    DiamondTypeSet cloned=source_is_current?
        compiler->function->type_sets[source_index]:source_sets[source_index];
    if(!reserve_type_sets(compiler,1))return DIAMOND_NO_TYPE_SET;
    const size_t destination_index=compiler->function->type_set_count++;
    for(size_t member_index=0;member_index<cloned.count;member_index++) {
        DiamondTypeMember *member=&cloned.members[member_index];
        member->argument_set=clone_type_set_into_current_impl(compiler,
            source_sets,source_count,member->argument_set,source_is_current);
        member->second_argument_set=clone_type_set_into_current_impl(compiler,
            source_sets,source_count,member->second_argument_set,
            source_is_current);
        member->callable_return_set=clone_type_set_into_current_impl(compiler,
            source_sets,source_count,member->callable_return_set,
            source_is_current);
        if(member->callable_parameters_typed)
            for(size_t parameter=0;parameter<member->callable_arity;parameter++)
                member->callable_parameter_sets[parameter]=
                    clone_type_set_into_current_impl(compiler,source_sets,
                        source_count,member->callable_parameter_sets[parameter],
                        source_is_current);
    }
    compiler->function->type_sets[destination_index]=cloned;
    return (uint16_t)destination_index;
}

static uint16_t clone_type_set_into_current(Compiler *compiler,
        const DiamondTypeSet *source_sets,size_t source_count,
        uint16_t source_index) {
    return clone_type_set_into_current_impl(compiler,source_sets,source_count,
        source_index,source_sets==compiler->function->type_sets);
}

static uint16_t clone_substituted_type_set(Compiler *compiler,
        const DiamondFunction *target,uint16_t source_index,
        const uint16_t *bindings,size_t binding_count,bool *resolved) {
    if(source_index==DIAMOND_NO_TYPE_SET)return DIAMOND_NO_TYPE_SET;
    if(source_index>=target->type_set_count) {
        *resolved=false;return DIAMOND_NO_TYPE_SET;
    }
    const DiamondTypeSet source=target->type_sets[source_index];
    if(source.count==1&&
       source.members[0].id>=DIAMOND_TYPE_VARIABLE_BASE&&
       source.members[0].id<DIAMOND_TYPE_INTERFACE_BASE) {
        const size_t variable=(size_t)(source.members[0].id-
            DIAMOND_TYPE_VARIABLE_BASE);
        if(variable>=binding_count||bindings==nullptr||
           bindings[variable]>=compiler->function->type_set_count) {
            *resolved=false;return DIAMOND_NO_TYPE_SET;
        }
        return bindings[variable];
    }
    if(!reserve_type_sets(compiler,1)) {
        *resolved=false;return DIAMOND_NO_TYPE_SET;
    }
    const size_t destination_index=compiler->function->type_set_count++;
    DiamondTypeSet substituted={.inferred=source.inferred};
    for(size_t member_index=0;member_index<source.count;member_index++) {
        DiamondTypeMember source_member=source.members[member_index];
        if(source_member.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           source_member.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=(size_t)(source_member.id-
                DIAMOND_TYPE_VARIABLE_BASE);
            if(variable>=binding_count||bindings==nullptr||
               bindings[variable]>=compiler->function->type_set_count) {
                *resolved=false;return DIAMOND_NO_TYPE_SET;
            }
            const DiamondTypeSet bound=
                compiler->function->type_sets[bindings[variable]];
            for(size_t bound_index=0;bound_index<bound.count;bound_index++) {
                bool duplicate=false;
                for(size_t known=0;known<substituted.count;known++)
                    if(type_members_equal(substituted.members[known],
                            bound.members[bound_index]))duplicate=true;
                if(duplicate)continue;
                if(substituted.count==DIAMOND_MAX_UNION_TYPES) {
                    *resolved=false;return DIAMOND_NO_TYPE_SET;
                }
                substituted.members[substituted.count++]=
                    bound.members[bound_index];
            }
            continue;
        }
        DiamondTypeMember member_value=source_member;
        DiamondTypeMember *member=&member_value;
        if(member->argument_set!=DIAMOND_NO_TYPE_SET)
            member->argument_set=clone_substituted_type_set(compiler,target,
                member->argument_set,bindings,binding_count,resolved);
        if(member->second_argument_set!=DIAMOND_NO_TYPE_SET)
            member->second_argument_set=clone_substituted_type_set(compiler,
                target,member->second_argument_set,bindings,binding_count,
                resolved);
        if(member->callable_return_set!=DIAMOND_NO_TYPE_SET)
            member->callable_return_set=clone_substituted_type_set(compiler,
                target,member->callable_return_set,bindings,binding_count,
                resolved);
        if(member->callable_parameters_typed)
            for(size_t parameter=0;parameter<member->callable_arity;parameter++)
                if(member->callable_parameter_sets[parameter]!=
                        DIAMOND_NO_TYPE_SET)
                    member->callable_parameter_sets[parameter]=
                        clone_substituted_type_set(compiler,target,
                            member->callable_parameter_sets[parameter],bindings,
                            binding_count,resolved);
        if(!*resolved)return DIAMOND_NO_TYPE_SET;
        if(substituted.count==DIAMOND_MAX_UNION_TYPES) {
            *resolved=false;return DIAMOND_NO_TYPE_SET;
        }
        substituted.members[substituted.count++]=member_value;
    }
    compiler->function->type_sets[destination_index]=substituted;
    return (uint16_t)destination_index;
}

static void publish_known_type_set(Compiler *compiler,uint16_t reg,
        uint16_t set_index) {
    if(set_index==DIAMOND_NO_TYPE_SET||
       set_index>=compiler->function->type_set_count)return;
    compiler->known_type_sets[reg]=(int32_t)set_index;
    const DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    if(set->count==0)return;
    const uint8_t outer=set->members[0].id;
    for(size_t index=1;index<set->count;index++)
        if(set->members[index].id!=outer)return;
    compiler->known_types[reg]=outer;
}

static void publish_callable_return_type(Compiler *compiler,uint16_t reg,
        int32_t callable_set_index) {
    if(callable_set_index<0||
       (size_t)callable_set_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet *set=
        &compiler->function->type_sets[(size_t)callable_set_index];
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<set->count;index++) {
        const DiamondTypeMember *member=&set->members[index];
        if(member->id!=DIAMOND_TYPE_CALLABLE||
           member->callable_return_set==DIAMOND_NO_TYPE_SET)return;
        if(return_set==DIAMOND_NO_TYPE_SET)
            return_set=member->callable_return_set;
        else if(return_set!=member->callable_return_set&&
                !type_sets_structurally_equal(
                    compiler->function->type_sets,
                    compiler->function->type_set_count,return_set,
                    compiler->function->type_sets,
                    compiler->function->type_set_count,
                    member->callable_return_set,0))return;
    }
    publish_known_type_set(compiler,reg,return_set);
}

static void publish_declared_return_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target,const uint16_t *bindings,
        size_t binding_count) {
    if(target==nullptr||target->return_type_set==DIAMOND_NO_TYPE_SET)return;
    uint16_t return_set;
    if(target->type_variable_count>0) {
        bool resolved=true;
        const size_t original_count=compiler->function->type_set_count;
        return_set=clone_substituted_type_set(compiler,target,
            target->return_type_set,bindings,binding_count,&resolved);
        if(!resolved) {
            compiler->function->type_set_count=original_count;return;
        }
    } else return_set=target==compiler->function?
        target->return_type_set:clone_type_set_into_current(compiler,
            target->type_sets,target->type_set_count,target->return_type_set);
    publish_known_type_set(compiler,reg,return_set);
}

static void publish_function_callable_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target) {
    compiler->known_types[reg]=DIAMOND_TYPE_CALLABLE;
    /* Cloning from the function currently being compiled could reallocate
     * its type-set table while that same table is the clone source. A
     * recursive self-reference remains a known Callable, but conservatively
     * omits structural signature facts until its declaration is complete. */
    if(target==compiler->function)return;
    DiamondTypeMember callable={.id=DIAMOND_TYPE_CALLABLE,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=target->arity,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=true};
    for(size_t parameter=0;parameter<16;parameter++)
        callable.callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    for(size_t parameter=0;parameter<target->arity;parameter++) {
        if(target->parameter_type_sets[parameter]==DIAMOND_NO_TYPE_SET) {
            callable.callable_parameters_typed=false;break;
        }
        callable.callable_parameter_sets[parameter]=
            clone_type_set_into_current(compiler,target->type_sets,
                target->type_set_count,target->parameter_type_sets[parameter]);
    }
    callable.callable_return_set=clone_type_set_into_current(compiler,
        target->type_sets,target->type_set_count,target->return_type_set);
    if(!reserve_type_sets(compiler,1))return;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;set->inferred=true;set->members[0]=callable;
    compiler->known_type_sets[reg]=(int32_t)set_index;
}

static uint16_t add_string_range(Compiler *compiler,size_t start,size_t length,
                                DiamondSpan span) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS) {
        fail(compiler, span, "function has too many string literals");
        return 0;
    }
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,span,"out of memory growing function strings");return 0;
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
    return (uint16_t)compiler->function->string_count++;
}

static uint16_t add_string(Compiler *compiler,DiamondSpan span) {
    return add_string_range(compiler,span.start+1,span.length-2,span);
}

static uint16_t add_name_string(Compiler *compiler, DiamondSpan span) {
    if (compiler->function->string_count == DIAMOND_MAX_STRING_CONSTANTS ||
        span.length > DIAMOND_MAX_STRING_LENGTH) {
        fail(compiler, span, "too many or oversized names in function");
        return 0;
    }
    if(compiler->function->string_count==compiler->function->string_capacity&&
       !diamond_function_reserve_strings(compiler->function,
          compiler->function->string_capacity==0?16:
          compiler->function->string_capacity*2)) {
        fail(compiler,span,"out of memory growing function strings");return 0;
    }
    DiamondStringConstant *string =
        &compiler->function->strings[compiler->function->string_count];
    for (size_t i = 0; i < span.length; i++)
        string->chars[i] = compiler->source[span.start + i];
    string->length = span.length;
    string->chars[span.length] = '\0';
    return (uint16_t)compiler->function->string_count++;
}

/* Hand-rolled rather than routed through emit_instruction: the jump-target
 * placeholder deliberately stays exactly 2 raw bytes (matching
 * patch_jump's own direct code[]-array writes below), independent of
 * emit_instruction's own operand width -- these two placeholder bytes are
 * never "an operand" from emit_instruction's point of view, just reserved
 * space patched in later once the jump's real destination is known. */
static size_t emit_jump(Compiler *compiler, DiamondOpCode opcode,
                        uint16_t condition) {
    const bool conditional=opcode==DIAMOND_OP_JUMP_IF_FALSE ||
                           opcode==DIAMOND_OP_JUMP_IF_TRUE;
    emit_opcode(compiler, opcode);
    if (conditional) emit_register(compiler, condition);
    const size_t operand = compiler->function->code_count;
    emit_byte(compiler, 0);
    emit_byte(compiler, 0);
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
    emit_opcode(compiler, DIAMOND_OP_JUMP);
    emit_byte(compiler, (uint8_t)(target >> 8));
    emit_byte(compiler, (uint8_t)(target & UINT8_MAX));
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

static uint16_t define_local(Compiler *compiler, DiamondSpan name) {
    if (compiler->local_count == DIAMOND_MAX_LOCALS) {
        fail(compiler, name, "too many local variables");
        return 0;
    }
    const uint16_t reg = allocate_register(compiler);
    /* .captured starts true, rather than the usual false, whenever this
     * declaration happens inside a loop body a def/closure was found
     * somewhere in (loop_captures_pending) -- a local declared fresh
     * inside such a loop, then captured later in that same body, needs
     * every one of its own subsequent reads/writes (including ones
     * compiled before that later def/closure is even reached) to already
     * be box-aware, for the same reason a pre-existing outer local does
     * (see loop_captures_pending's own comment). */
    compiler->locals[compiler->local_count++] =
        (Local){.name = name, .reg = reg, .captured = compiler->loop_captures_pending};
    return reg;
}

static Precedence token_precedence(DiamondTokenKind kind) {
    switch (kind) {
        case DIAMOND_TOKEN_DOT_DOT:
        case DIAMOND_TOKEN_DOT_DOT_DOT:
            return PREC_RANGE;
        case DIAMOND_TOKEN_OR_OR:
        case DIAMOND_TOKEN_OR:
            return PREC_OR;
        case DIAMOND_TOKEN_AND_AND:
        case DIAMOND_TOKEN_AND:
            return PREC_AND;
        case DIAMOND_TOKEN_EQUAL_EQUAL:
        case DIAMOND_TOKEN_BANG_EQUAL:
        case DIAMOND_TOKEN_SPACESHIP:
            return PREC_EQUALITY;
        case DIAMOND_TOKEN_LESS:
        case DIAMOND_TOKEN_LESS_EQUAL:
        case DIAMOND_TOKEN_GREATER:
        case DIAMOND_TOKEN_GREATER_EQUAL:
        case DIAMOND_TOKEN_IS:
            return PREC_COMPARISON;
        case DIAMOND_TOKEN_LESS_LESS:
            return PREC_SHIFT;
        case DIAMOND_TOKEN_PLUS:
        case DIAMOND_TOKEN_MINUS:
            return PREC_TERM;
        case DIAMOND_TOKEN_STAR:
        case DIAMOND_TOKEN_SLASH:
        case DIAMOND_TOKEN_PERCENT:
            return PREC_FACTOR;
        default:
            return PREC_NONE;
    }
}

static uint16_t parse_precedence(Compiler *compiler, Precedence precedence);
static uint16_t parse_case(Compiler *compiler);
static uint16_t compile_block(Compiler *compiler);

static uint16_t parse_integer(Compiler *compiler) {
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
    const uint16_t destination = allocate_register(compiler);
    const uint16_t constant = add_constant(compiler, DIAMOND_INT(value));
    emit_instruction(compiler, DIAMOND_OP_CONSTANT, destination, constant, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_INT;
    return destination;
}

static uint16_t parse_float(Compiler *compiler) {
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
    const uint16_t destination = allocate_register(compiler);
    const uint16_t constant = add_constant(compiler, DIAMOND_FLOAT(value));
    emit_instruction(compiler, DIAMOND_OP_CONSTANT, destination, constant, 0, 2);
    compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
    return destination;
}

static uint16_t parse_string(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    const size_t end=span.start+span.length-1;
    size_t piece=span.start+1;uint16_t result=UINT16_MAX;
    while(piece<end&&!compiler->failed) {
        size_t index=piece;
        while(index+1<end) {
            if(compiler->source[index]=='\\') {index+=2;continue;}
            if(compiler->source[index]=='#'&&compiler->source[index+1]=='{')break;
            index++;
        }
        if(index+1>=end)index=end;
        const uint16_t literal_register=allocate_register(compiler);
        const uint16_t literal=add_string_range(compiler,piece,index-piece,span);
        emit_instruction(compiler,DIAMOND_OP_STRING,literal_register,literal,0,2);
        if(result==UINT16_MAX)result=literal_register;
        else {
            const uint16_t joined=allocate_register(compiler);
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
        const uint16_t value=parse_expression(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE)
            fail(compiler,compiler->current.span,"expected '}' after interpolation");
        const size_t close=compiler->current.span.start;
        compiler->lexer=outer_lexer;compiler->current=outer_current;
        compiler->previous=outer_previous;
        const uint16_t converted=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_TO_STRING,converted,value,0,2);
        const uint16_t joined=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_ADD,joined,result,converted,3);
        result=joined;piece=close+1;
    }
    if(result==UINT16_MAX) {
        result=allocate_register(compiler);
        const uint16_t string=add_string(compiler,span);
        emit_instruction(compiler,DIAMOND_OP_STRING,result,string,0,2);
    }
    compiler->known_types[result]=DIAMOND_TYPE_STRING;return result;
}

static uint16_t parse_symbol(Compiler *compiler) {
    const DiamondSpan span=compiler->previous.span;
    const DiamondSpan name_span={.start=span.start+1,.length=span.length-1,
        .line=span.line,.column=span.column+1};
    const uint16_t destination=allocate_register(compiler);
    const uint16_t name=add_name_string(compiler,name_span);
    emit_instruction(compiler,DIAMOND_OP_SYMBOL,destination,name,0,2);
    compiler->known_types[destination]=DIAMOND_TYPE_SYMBOL;
    return destination;
}

static uint16_t parse_literal(Compiler *compiler) {
    const uint16_t destination = allocate_register(compiler);
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

static uint16_t parse_identifier(Compiler *compiler) {
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
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,destination,(uint8_t)capture,0,2);
            return destination;
        }
        /* A top-level `def`'s bare name, used as a value rather than
         * called outright (parse_name already routed the call-with-
         * parens case to parse_call, never reaching here) -- previously
         * always "undefined local variable", since a top-level function
         * is otherwise never registered as anything parse_identifier's
         * local/capture lookups above can find. A nested `def` already
         * works this way (compile_definition registers it as an
         * ordinary local holding a zero-or-more-capture DIAMOND_OP_
         * CLOSURE value); this is the same closure value with zero
         * captures, for a function that needs none since it isn't
         * nested inside anything with locals to close over. find_function
         * already applies the same owner_class==UINT8_MAX && !nested
         * filter find_top_level_function (src/vm.c) uses at runtime, so
         * this can never resolve a class/module method or another
         * function's own nested def by bare name. */
        const int function_index = find_function(compiler, compiler->previous.span);
        if (function_index >= 0) {
            const uint16_t destination = allocate_register(compiler);
            emit_opcode(compiler, DIAMOND_OP_CLOSURE);
            emit_register(compiler,destination);
            emit_function_index(compiler, (size_t)function_index);
            emit_byte(compiler, 0);
            publish_function_callable_type(compiler,destination,
                compiler->program->functions[(size_t)function_index]);
            return destination;
        }
        if(compiler->discovery_pass) {
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
            return destination;
        }
        fail(compiler, compiler->previous.span, "undefined local variable"); return 0;
    }
    if(!compiler->locals[(size_t)local].captured)
        return compiler->locals[(size_t)local].reg;
    /* `captured` may have been set by a nested def in a sibling if/else
     * branch that doesn't dominate this read, so the register may not
     * actually be boxed yet on this runtime path. BOX_LOCAL is idempotent
     * (a no-op once the register already holds a Cell), so re-emitting it
     * here guarantees GET_CELL is always safe, regardless of which branch
     * ran. */
    emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,
                     compiler->locals[(size_t)local].reg,0,0,1);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_GET_CELL,destination,
                     compiler->locals[(size_t)local].reg,0,2);
    compiler->known_types[destination]=
        compiler->known_types[compiler->locals[(size_t)local].reg];
    compiler->known_type_sets[destination]=
        compiler->known_type_sets[compiler->locals[(size_t)local].reg];
    return destination;
}

static int find_function(const Compiler *compiler, DiamondSpan name) {
    for (size_t index = compiler->program->function_count; index > 0; index--) {
        const size_t function_index = index - 1;
        const char *candidate = compiler->program->functions[function_index]->name;
        if (compiler->program->functions[function_index]->owner_class != UINT8_MAX ||
            compiler->program->functions[function_index]->nested) continue;
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
        if (equal) return (int)function_index;
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

static DiamondFunction *compiler_add_function(Compiler *compiler,
                                               size_t *function_index) {
    if(!compiler->discovery_pass&&
       compiler->next_function_claim<compiler->program->function_count&&
       compiler->program->functions[compiler->next_function_claim]
           ->declared_by_discovery) {
        *function_index=compiler->next_function_claim++;
        DiamondFunction *function=compiler->program->functions[*function_index];
        free(function->code);free(function->lines);free(function->columns);
        free(function->constants);
        free(function->strings);
        free(function->type_sets);
        memset(function,0,sizeof *function);
        return function;
    }
    DiamondFunction *function=diamond_program_add_function(compiler->program);
    if(function!=nullptr)*function_index=compiler->program->function_count-1;
    return function;
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

/* Resolves a class name consume_qualified_name already collected into a
 * plain C string -- used by resolve_type_name (parameter/return type
 * annotations and, via resolve_type_name, `is` checks) and by superclass
 * resolution, neither of which could previously represent an explicit
 * `A::B` path at all (find_class_name alone is exact-string-only; find_class
 * takes a single DiamondSpan token, one identifier, never a qualified
 * path). Walks the current-module scope outward first, trying `name`
 * qualified against each enclosing module in turn -- the same order
 * find_class already uses for bare `.new()`/superclass references -- and
 * only once none of those match falls back to `name` taken bare/as-given
 * (handles a fully-qualified path like `Shapes::Base`, or an ordinary
 * un-nested class name). Trying the bare name first (the previous order)
 * meant a bare sibling reference (`Table` from code lexically inside
 * `module Arel`) would resolve to an unrelated top-level class of the same
 * name whenever the enclosing program happened to declare one, instead of
 * the module's own `Arel::Table` -- silently wrong instead of merely
 * unresolved, since both names exist and only one is intended. Fixed so a
 * bare sibling name resolves for type annotations and `is` checks the same
 * way it already does for construction and inheritance. */
static int find_class_qualified_or_scoped(const Compiler *compiler,const char *name) {
    if(compiler->current_module>=0) {
        char scope[DIAMOND_MAX_FUNCTION_NAME];
        (void)snprintf(scope,sizeof scope,"%s",
            compiler->program->modules[(size_t)compiler->current_module].name);
        while(true) {
            char qualified[DIAMOND_MAX_FUNCTION_NAME];
            const int written=snprintf(qualified,sizeof qualified,"%s::%s",scope,name);
            if(written>0&&(size_t)written<sizeof qualified) {
                const int found=find_class_name(compiler,qualified);
                if(found>=0)return found;
            }
            char *separator=strrchr(scope,':');
            if(separator==nullptr)break;
            separator[-1]='\0';
        }
    }
    return find_class_name(compiler,name);
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
    found=find_class_qualified_or_scoped(compiler,name);
    if(found>=0)return DIAMOND_TYPE_CLASS_BASE+found;
    /* During diamond_compile's own discovery pass only: a type annotation
     * naming a class/interface declared *later* in the source hasn't
     * been discovered yet by this point in discovery's own walk (that's
     * the whole reason discovery exists -- see diamond_compile's own
     * comment). Tolerate it here rather than aborting discovery, so it
     * can keep walking to the real, later declaration; the real second
     * pass runs with every declaration already known up front, so it
     * either resolves this correctly there or, if the name genuinely
     * never exists, hits this exact fail() again for real. */
    if(!compiler->discovery_pass)
        fail(compiler,diagnostic,"unknown type annotation");
    return DIAMOND_TYPE_NIL;
}

static bool type_members_structurally_equal(const DiamondTypeMember *left,
        const DiamondTypeSet *left_sets,size_t left_count,
        const DiamondTypeMember *right,const DiamondTypeSet *right_sets,
        size_t right_count,size_t depth);

static int parse_type_annotation(Compiler *compiler) {
    if(!reserve_type_sets(compiler,1))return 0;
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
        if(set->count==DIAMOND_MAX_UNION_TYPES) {
            fail(compiler,compiler->current.span,"too many types in union");break;
        }
        uint16_t argument_set=DIAMOND_NO_TYPE_SET,second_argument_set=DIAMOND_NO_TYPE_SET;
        uint8_t callable_arity=UINT8_MAX;
        uint16_t callable_return_set=DIAMOND_NO_TYPE_SET;
        bool callable_parameters_typed=false;
        uint16_t callable_parameter_sets[16];
        for(size_t index=0;index<16;index++)callable_parameter_sets[index]=DIAMOND_NO_TYPE_SET;
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
                            (uint16_t)parse_type_annotation(compiler);
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
                    callable_return_set=(uint16_t)parse_type_annotation(compiler);
                }
            } else if(type==DIAMOND_TYPE_ARRAY||type==DIAMOND_TYPE_HASH) {
                advance_token(compiler);
                skip_newlines(compiler);
                argument_set=(uint16_t)parse_type_annotation(compiler);
                if(type==DIAMOND_TYPE_HASH) {
                    skip_newlines(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                        fail(compiler,compiler->current.span,
                             "expected ',' between Hash key and value types");break;
                    }
                    advance_token(compiler);
                    skip_newlines(compiler);
                    second_argument_set=(uint16_t)parse_type_annotation(compiler);
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
        set=&compiler->function->type_sets[set_index];
        for(size_t index=0;index<set->count;index++)
            if(type_members_structurally_equal(&set->members[index],
                    compiler->function->type_sets,
                    compiler->function->type_set_count,&parsed,
                    compiler->function->type_sets,
                    compiler->function->type_set_count,0)) {
                fail(compiler,member_span,"duplicate type in union");break;
            }
        if(compiler->failed)break;
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

/* Same shape as field_index just above (compile-time name -> slot
 * resolution, scoped to the current class), but for `@@name` class
 * variables rather than `@name` instance fields -- kept as its own
 * function rather than generalizing field_index/name_equals to a
 * variable skip count, since `@@` is the only two-char sigil in the
 * language and threading a skip-count parameter through name_equals'
 * 70-odd other call sites (all of which pass a plain bool today) isn't
 * worth it for one caller. */
static bool class_variable_name_equals(const Compiler *compiler,
        const char *candidate, DiamondSpan name) {
    size_t length = 0;
    while (candidate[length] != '\0') length++;
    if (name.length - 2 != length) return false;
    for (size_t index = 0; index < length; index++) {
        if (candidate[index] != compiler->source[name.start + 2 + index]) return false;
    }
    return true;
}

static int class_variable_index(Compiler *compiler, DiamondSpan name, bool create) {
    if (compiler->current_class < 0) {
        fail(compiler, name, "class variable used outside a class");
        return -1;
    }
    DiamondClass *class = &compiler->program->classes[(size_t)compiler->current_class];
    for (size_t index = 0; index < class->class_variable_count; index++) {
        if (class_variable_name_equals(compiler, class->class_variables[index], name))
            return (int)index;
    }
    if (!create) return -1;
    if (class->class_variable_count == DIAMOND_MAX_FIELDS) {
        fail(compiler, name, "too many class variables");
        return -1;
    }
    char *variable = class->class_variables[class->class_variable_count];
    const size_t length = name.length - 2;
    if (length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "class variable name is too long");
        return -1;
    }
    for (size_t i = 0; i < length; i++) variable[i] = compiler->source[name.start + 2 + i];
    variable[length] = '\0';
    return (int)class->class_variable_count++;
}

static uint16_t module_field_name(Compiler *compiler,DiamondSpan name) {
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

static uint16_t emit_build_spread_arguments(Compiler *compiler,
        const uint16_t *prefix,size_t prefix_count,uint16_t spread,
        const uint16_t *suffix,size_t suffix_count,bool optional_block);

static bool call_arguments_have_keyword(Compiler *compiler) {
    Compiler probe=*compiler;size_t nesting=0;bool argument_start=true;
    while(!probe.failed) {
        const DiamondTokenKind kind=probe.current.kind;
        if(nesting==0&&kind==DIAMOND_TOKEN_RIGHT_PAREN)return false;
        if(nesting==0&&argument_start&&kind==DIAMOND_TOKEN_IDENTIFIER) {
            DiamondLexer lookahead=probe.lexer;
            if(diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON)
                return true;
        }
        if(kind==DIAMOND_TOKEN_LEFT_PAREN||kind==DIAMOND_TOKEN_LEFT_BRACKET||
           kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_PAREN||
                kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                kind==DIAMOND_TOKEN_RIGHT_BRACE) {if(nesting>0)nesting--;}
        argument_start=nesting==0&&kind==DIAMOND_TOKEN_COMMA;
        if(kind!=DIAMOND_TOKEN_NEWLINE&&kind!=DIAMOND_TOKEN_COMMA)
            argument_start=false;
        advance_token(&probe);
    }
    return false;
}

static uint16_t emit_argument_array(Compiler *compiler,const uint16_t *values,
        size_t count) {
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<count;index++)(void)allocate_register(compiler);
    for(size_t index=0;index<count;index++)emit_instruction(compiler,
        DIAMOND_OP_MOVE,(uint16_t)(base+index),values[index],0,2);
    const uint16_t result=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,result,base,(uint16_t)count,3);
    compiler->known_types[result]=DIAMOND_TYPE_ARRAY;return result;
}

static uint16_t parse_expected_argument(Compiler *compiler,
        const DiamondFunction *target,size_t parameter);
static uint16_t parse_array_literal(Compiler *compiler,
        const DiamondFunction *expected_function,size_t first_parameter,
        size_t parameter_count);
static int32_t homogeneous_spread_expectation(Compiler *compiler,
        const DiamondFunction *target,size_t first,size_t parameter_count);

/* Dynamic keyword targets retain names until runtime lookup identifies the
 * concrete DiamondFunction. Positional values are normalized to one Array,
 * whether or not the source used `*`. */
static uint16_t parse_dynamic_keyword_arguments(Compiler *compiler,
        DiamondSpan *keyword_names,uint16_t *keyword_values,
        size_t *keyword_count,const DiamondFunction *target) {
    uint16_t fixed[16];size_t fixed_count=0,spread_index=0;
    uint16_t spread=0;bool saw_spread=false,seen_keyword=false;
    bool literal_positions=false;size_t literal_first=0,literal_count=0;
    uint16_t literal_elements[32];
    compiler->positional_spread_literal=false;
    compiler->positional_spread_first=0;
    compiler->positional_spread_count=0;
    compiler->positional_spread_fixed_count=0;
    compiler->positional_spread_index=0;
    *keyword_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        DiamondLexer lookahead=compiler->lexer;
        const bool keyword=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
            diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON;
        if(keyword) {
            seen_keyword=true;
            if(*keyword_count==16) {
                fail(compiler,compiler->current.span,"too many keyword arguments");return 0;
            }
            const DiamondSpan name=compiler->current.span;
            for(size_t index=0;index<*keyword_count;index++)
                if(keyword_names[index].length==name.length&&
                   memcmp(compiler->source+keyword_names[index].start,
                          compiler->source+name.start,name.length)==0) {
                    fail(compiler,name,"multiple values for the same argument");return 0;
                }
            keyword_names[*keyword_count]=name;
            advance_token(compiler);advance_token(compiler);
            size_t parameter=SIZE_MAX;
            if(target!=nullptr)
                for(size_t index=0;index<target->arity&&index<16;index++)
                    if(name_equals(compiler,target->parameter_names[index],
                            name,false)) {parameter=index;break;}
            keyword_values[*keyword_count]=parameter==SIZE_MAX?
                parse_expression(compiler):
                parse_expected_argument(compiler,target,parameter);
            (*keyword_count)++;
        } else if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
            if(seen_keyword) {fail(compiler,compiler->current.span,
                "positional argument cannot follow a keyword argument");return 0;}
            if(saw_spread) {fail(compiler,compiler->current.span,
                "a call can contain only one spread argument");return 0;}
            saw_spread=true;spread_index=fixed_count;
            advance_token(compiler);skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
               target!=nullptr) {
                advance_token(compiler);
                spread=parse_array_literal(compiler,target,fixed_count,
                    target->arity);
                literal_positions=compiler->positional_spread_literal;
                literal_first=compiler->positional_spread_first;
                literal_count=compiler->positional_spread_count;
                for(size_t index=0;index<literal_count;index++)
                    literal_elements[index]=
                        compiler->positional_spread_elements[index];
            } else {
                const int32_t outer_expected=
                    compiler->expected_expression_type_set;
                compiler->expected_expression_type_set=
                    homogeneous_spread_expectation(compiler,target,
                        fixed_count,target==nullptr?0:target->arity);
                spread=parse_expression(compiler);
                compiler->expected_expression_type_set=outer_expected;
            }
        } else {
            if(seen_keyword) {fail(compiler,compiler->current.span,
                "positional argument cannot follow a keyword argument");return 0;}
            if(fixed_count==16) {fail(compiler,compiler->current.span,
                "too many fixed call arguments");return 0;}
            fixed[fixed_count]=parse_expected_argument(compiler,target,
                fixed_count);
            fixed_count++;
        }
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;
    }
    advance_token(compiler);
    if(!saw_spread) {
        compiler->positional_spread_fixed_count=fixed_count;
        compiler->positional_spread_index=fixed_count;
        for(size_t index=0;index<fixed_count;index++)
            compiler->positional_spread_fixed[index]=fixed[index];
        return emit_argument_array(compiler,fixed,fixed_count);
    }
    compiler->positional_spread_literal=literal_positions;
    compiler->positional_spread_first=literal_first;
    compiler->positional_spread_count=literal_count;
    for(size_t index=0;index<literal_count;index++)
        compiler->positional_spread_elements[index]=literal_elements[index];
    compiler->positional_spread_fixed_count=fixed_count;
    compiler->positional_spread_index=spread_index;
    for(size_t index=0;index<fixed_count;index++)
        compiler->positional_spread_fixed[index]=fixed[index];
    return emit_build_spread_arguments(compiler,fixed,spread_index,spread,
        fixed+spread_index,fixed_count-spread_index,false);
}

static bool call_arguments_have_spread(Compiler *compiler) {
    Compiler probe=*compiler;
    size_t nesting=0;bool argument_start=true;
    while(!probe.failed) {
        const DiamondTokenKind kind=probe.current.kind;
        if(nesting==0&&kind==DIAMOND_TOKEN_RIGHT_PAREN)return false;
        if(nesting==0&&argument_start&&kind==DIAMOND_TOKEN_STAR)return true;
        if(kind==DIAMOND_TOKEN_LEFT_PAREN||kind==DIAMOND_TOKEN_LEFT_BRACKET||
           kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
        else if(kind==DIAMOND_TOKEN_RIGHT_PAREN||
                kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                kind==DIAMOND_TOKEN_RIGHT_BRACE) {
            if(nesting>0)nesting--;
        }
        argument_start=nesting==0&&kind==DIAMOND_TOKEN_COMMA;
        if(kind!=DIAMOND_TOKEN_NEWLINE&&kind!=DIAMOND_TOKEN_COMMA)
            argument_start=false;
        advance_token(&probe);
    }
    return false;
}

static int32_t homogeneous_spread_expectation(Compiler *compiler,
        const DiamondFunction *target,size_t first,size_t parameter_count) {
    if(target==nullptr||first>=parameter_count||parameter_count>target->arity)
        return -1;
    const uint16_t source=target->parameter_type_sets[first];
    if(source==DIAMOND_NO_TYPE_SET||source>=target->type_set_count)return -1;
    for(size_t parameter=first+1;parameter<parameter_count;parameter++) {
        const uint16_t other=target->parameter_type_sets[parameter];
        if(other==DIAMOND_NO_TYPE_SET||other>=target->type_set_count||
           !type_sets_structurally_equal(target->type_sets,
               target->type_set_count,source,target->type_sets,
               target->type_set_count,other,0))return -1;
    }
    const uint16_t element=target==compiler->function?source:
        clone_type_set_into_current(compiler,target->type_sets,
            target->type_set_count,source);
    if(element==DIAMOND_NO_TYPE_SET||!reserve_type_sets(compiler,1))return -1;
    const size_t array_index=compiler->function->type_set_count++;
    DiamondTypeSet *array=&compiler->function->type_sets[array_index];
    array->count=1;array->inferred=true;
    array->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_ARRAY,
        .argument_set=element,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        array->members[0].callable_parameter_sets[parameter]=
            DIAMOND_NO_TYPE_SET;
    return (int32_t)array_index;
}

/* Parses the argument-list interior with exactly one `*expression`; current
 * is the first argument and the closing ')' is consumed. */
static uint16_t parse_spread_argument_array(Compiler *compiler,
        const DiamondFunction *keyword_function,uint8_t *keyword_slots,
        uint16_t *keyword_registers,size_t *keyword_count,
        const DiamondFunction *expected_function,size_t expected_count) {
    uint16_t fixed[16];size_t fixed_count=0,spread_index=0;
    uint16_t spread=0;bool saw_spread=false,seen_keyword=false;
    bool optional_block=false;
    bool literal_positions=false;size_t literal_first=0,literal_count=0;
    uint16_t literal_elements[32];
    compiler->positional_spread_literal=false;
    compiler->positional_spread_first=0;
    compiler->positional_spread_count=0;
    compiler->positional_spread_fixed_count=0;
    compiler->positional_spread_index=0;
    if(keyword_count!=nullptr)*keyword_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        DiamondLexer keyword_lookahead=compiler->lexer;
        const bool is_keyword=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
            diamond_lexer_next(&keyword_lookahead).kind==DIAMOND_TOKEN_COLON;
        if(is_keyword) {
            const DiamondSpan keyword_name=compiler->current.span;
            if(keyword_function==nullptr) {
                fail(compiler,keyword_name,
                    "keyword arguments are only supported for direct function calls");
                return 0;
            }
            seen_keyword=true;advance_token(compiler);advance_token(compiler);
            size_t slot=SIZE_MAX;
            for(size_t index=0;index<keyword_function->arity&&index<16;index++)
                if(name_equals(compiler,keyword_function->parameter_names[index],
                               keyword_name,false)) {slot=index;break;}
            if(slot==SIZE_MAX) {
                fail(compiler,keyword_name,"no parameter with this name");return 0;
            }
            for(size_t index=0;index<*keyword_count;index++)
                if(keyword_slots[index]==slot) {
                    fail(compiler,keyword_name,
                        "multiple values for the same argument");return 0;
                }
            keyword_slots[*keyword_count]=(uint8_t)slot;
            keyword_registers[*keyword_count]=parse_expression(compiler);
            (*keyword_count)++;
        } else if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
            if(seen_keyword) {
                fail(compiler,compiler->current.span,
                    "positional argument cannot follow a keyword argument");return 0;
            }
            if(saw_spread) {
                fail(compiler,compiler->current.span,
                    "a call can contain only one spread argument");return 0;
            }
            saw_spread=true;spread_index=fixed_count;
            advance_token(compiler);skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
               expected_function!=nullptr) {
                advance_token(compiler);
                spread=parse_array_literal(compiler,expected_function,
                    fixed_count,expected_count);
                literal_positions=compiler->positional_spread_literal;
                literal_first=compiler->positional_spread_first;
                literal_count=compiler->positional_spread_count;
                for(size_t index=0;index<literal_count;index++)
                    literal_elements[index]=
                        compiler->positional_spread_elements[index];
            } else {
                const int32_t outer_expected=
                    compiler->expected_expression_type_set;
                compiler->expected_expression_type_set=
                    homogeneous_spread_expectation(compiler,
                        expected_function,fixed_count,expected_count);
                spread=parse_expression(compiler);
                compiler->expected_expression_type_set=outer_expected;
            }
        } else {
            if(seen_keyword) {
                fail(compiler,compiler->current.span,
                    "positional argument cannot follow a keyword argument");return 0;
            }
            if(fixed_count==16) {
                fail(compiler,compiler->current.span,"too many fixed call arguments");
                return 0;
            }
            const bool forwarded_block=
                compiler->current.kind==DIAMOND_TOKEN_AMPERSAND;
            fixed[fixed_count++]=parse_expression(compiler);
            if(forwarded_block) {
                optional_block=true;
                if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
                    fail(compiler,compiler->current.span,
                        "a forwarded block argument must be last");return 0;
                }
            }
        }
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    if(!saw_spread) {
        fail(compiler,compiler->previous.span,"expected spread argument");return 0;
    }
    compiler->positional_spread_literal=literal_positions;
    compiler->positional_spread_first=literal_first;
    compiler->positional_spread_count=literal_count;
    for(size_t index=0;index<literal_count;index++)
        compiler->positional_spread_elements[index]=literal_elements[index];
    compiler->positional_spread_fixed_count=fixed_count;
    compiler->positional_spread_index=spread_index;
    for(size_t index=0;index<fixed_count;index++)
        compiler->positional_spread_fixed[index]=fixed[index];
    if(fixed_count==0)return spread;
    return emit_build_spread_arguments(compiler,fixed,spread_index,spread,
        fixed+spread_index,fixed_count-spread_index,optional_block);
}

static uint16_t compile_callable_value_block(Compiler *compiler,
        int32_t callable_set_index);

/* Parses `(arg, arg, ...)` (the `(` itself still current) and emits a
 * DIAMOND_OP_CALL_CLOSURE against `callable` -- shared between a local
 * variable holding a Callable followed by `(...)` (parse_call's own
 * shape, below) and an instance variable holding one, followed by
 * `(...)` (`@field(...)`, parse_prefix's DIAMOND_TOKEN_INSTANCE_VARIABLE
 * case) -- previously only the local-variable shape was supported at
 * all; `@cb()` failed to parse outright ("expected newline after
 * expression"), forcing an extra `cb = @cb; cb()` local-binding step for
 * a stored-callback-field pattern that's otherwise completely ordinary. */
static uint16_t parse_closure_call_arguments(Compiler *compiler, uint16_t callable) {
    const int32_t callable_type_set=compiler->known_type_sets[callable];
    size_t contextual_block_slot=SIZE_MAX;
    if(callable_type_set>=0&&
       (size_t)callable_type_set<compiler->function->type_set_count) {
        const DiamondTypeSet *set=
            &compiler->function->type_sets[(size_t)callable_type_set];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_CALLABLE&&
           set->members[0].callable_parameters_typed&&
           set->members[0].callable_arity>0)
            contextual_block_slot=set->members[0].callable_arity-1;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(call_arguments_have_keyword(compiler)) {
        DiamondSpan keyword_names[16];uint16_t keyword_values[16];
        size_t keyword_count=0;
        const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
            keyword_names,keyword_values,&keyword_count,nullptr);
        bool has_block=false;uint16_t block=0;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t callable_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,callable_snapshot,
                callable,0,2);callable=callable_snapshot;
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_values[index],0,2);
                keyword_values[index]=snapshot;
            }
            block=compile_callable_value_block(compiler,callable_type_set);
            has_block=true;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
        emit_register(compiler,destination);emit_register(compiler,callable);
        emit_register(compiler,positional);
        emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
        for(size_t index=0;index<keyword_count;index++) {
            emit_register(compiler,add_name_string(compiler,keyword_names[index]));
            emit_register(compiler,keyword_values[index]);
        }
        if(has_block)emit_register(compiler,block);
        publish_callable_return_type(compiler,destination,callable_type_set);
        return destination;
    }
    if(call_arguments_have_spread(compiler)) {
        uint16_t spread=parse_spread_argument_array(compiler,nullptr,
            nullptr,nullptr,nullptr,nullptr,0);
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t callable_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,callable_snapshot,
                callable,0,2);
            callable=callable_snapshot;
            const uint16_t block=
                compile_callable_value_block(compiler,callable_type_set);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
            emit_register(compiler,destination);emit_register(compiler,callable);
            emit_register(compiler,spread);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_callable_return_type(compiler,destination,callable_type_set);
            return destination;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_SPREAD);
        emit_register(compiler,destination);emit_register(compiler,callable);
        emit_register(compiler,spread);
        publish_callable_return_type(compiler,destination,callable_type_set);
        return destination;
    }
    uint16_t arguments[16]; size_t argument_count=0;
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
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(argument_count==16) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        const uint16_t callable_snapshot=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,callable_snapshot,callable,0,2);
        callable=callable_snapshot;
        for(size_t index=0;index<argument_count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,arguments[index],0,2);
            arguments[index]=snapshot;
        }
        const uint16_t block=
            compile_callable_value_block(compiler,callable_type_set);
        if(argument_count<contextual_block_slot) {
            const uint16_t positional=
                emit_argument_array(compiler,arguments,argument_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
            emit_register(compiler,destination);emit_register(compiler,callable);
            emit_register(compiler,positional);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_callable_return_type(compiler,destination,callable_type_set);
            return destination;
        }
        arguments[argument_count++]=block;
    }
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<argument_count;i++)(void)allocate_register(compiler);
    for(size_t i=0;i<argument_count;i++)emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i),arguments[i],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE);emit_register(compiler,destination);
    emit_register(compiler,callable);emit_register(compiler,base);emit_byte(compiler,(uint8_t)argument_count);
    publish_callable_return_type(compiler,destination,callable_type_set);
    return destination;
}

/* The discovery pass can encounter a call before its top-level function's
 * signature has been seen. It only needs to keep walking far enough to record
 * that later declaration; the real pass will validate the call against the
 * copied signature. Parse the complete call shape here without emitting a
 * callable function index that does not exist in this throwaway program yet. */
static uint16_t parse_discovery_unknown_call(Compiler *compiler) {
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        advance_token(compiler);skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            (void)parse_type_annotation(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,
                 "expected ']' after generic arguments");return 0;
        }
        advance_token(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after function name");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    if(call_arguments_have_spread(compiler)) {
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&
              !compiler->failed) {
            DiamondLexer lookahead=compiler->lexer;
            if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
               diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);advance_token(compiler);
                skip_newlines(compiler);
            } else if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
                advance_token(compiler);skip_newlines(compiler);
            }
            (void)parse_expression(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        }
    } else if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            DiamondLexer lookahead=compiler->lexer;
            if(compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
               diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_COLON) {
                advance_token(compiler);advance_token(compiler);
                skip_newlines(compiler);
            }
            (void)parse_expression(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);skip_newlines(compiler);
        } while(!compiler->failed);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    return destination;
}

static uint16_t load_current_block(Compiler *compiler) {
    uint16_t block=compiler->current_block_register;
    for(size_t index=0;index<compiler->local_count;index++) {
        if(compiler->locals[index].reg!=block||
           !compiler->locals[index].captured)continue;
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,block,0,0,1);
        const uint16_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,block,0,2);
        return loaded;
    }
    return block;
}

static uint16_t callable_type_set_index(Compiler *compiler) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_CALLABLE&&
           set->members[0].callable_arity==UINT8_MAX)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[index];
    set->count=1;
    set->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_CALLABLE,
        .argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t parameter=0;parameter<16;parameter++)
        set->members[0].callable_parameter_sets[parameter]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)index;
}

static void prepare_contextual_block(Compiler *compiler,
        const DiamondTypeSet *sets,size_t set_count,
        const DiamondTypeMember *callable) {
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    uint16_t parameter_sets[16];
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<16;index++) {
        compiler->contextual_block_type_sets[index]=-1;
        parameter_sets[index]=DIAMOND_NO_TYPE_SET;
    }
    if(callable!=nullptr&&callable->id==DIAMOND_TYPE_CALLABLE&&
       callable->callable_parameters_typed&&callable->callable_arity<=16) {
        compiler->has_contextual_block_types=true;
        compiler->contextual_block_arity=callable->callable_arity;
        return_set=callable->callable_return_set;
        for(size_t index=0;index<callable->callable_arity;index++) {
            compiler->contextual_block_types[index]=TYPE_UNKNOWN;
            const uint16_t parameter_set=callable->callable_parameter_sets[index];
            parameter_sets[index]=parameter_set;
            if(parameter_set>=set_count)continue;
            const DiamondTypeSet *parameter=&sets[parameter_set];
            if(parameter->count==1&&
               parameter->members[0].id<DIAMOND_TYPE_VARIABLE_BASE)
                compiler->contextual_block_types[index]=parameter->members[0].id;
        }
    }
    /* Clone last: `sets` and `callable` can point into the current function's
     * table, whose reserve may move while cloning. Capture every source index
     * before the first clone, then let the index-aware cloner reacquire it. */
    for(size_t index=0;index<compiler->contextual_block_arity;index++)
        if(parameter_sets[index]!=DIAMOND_NO_TYPE_SET) {
            const uint16_t cloned=clone_type_set_into_current(compiler,sets,
                set_count,parameter_sets[index]);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->contextual_block_type_sets[index]=(int32_t)cloned;
        }
    if(return_set!=DIAMOND_NO_TYPE_SET) {
        const uint16_t cloned=clone_type_set_into_current(compiler,sets,
            set_count,return_set);
        if(cloned!=DIAMOND_NO_TYPE_SET)
            compiler->contextual_block_return_set=(int32_t)cloned;
    }
}

static uint16_t compile_contextual_typed_block(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_index,
        const uint16_t *type_arguments,size_t type_argument_count) {
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    if(target!=nullptr&&parameter_index<16) {
        const uint16_t set_index=target->parameter_type_sets[parameter_index];
        if(set_index!=DIAMOND_NO_TYPE_SET&&set_index<target->type_set_count) {
            const DiamondTypeSet *set=&target->type_sets[set_index];
            if(set->count==1) {
                const DiamondTypeMember *callable=&set->members[0];
                const bool callable_typed=
                    callable->id==DIAMOND_TYPE_CALLABLE&&
                    callable->callable_parameters_typed;
                const uint16_t callable_return_set=
                    callable->callable_return_set;
                prepare_contextual_block(compiler,target->type_sets,
                    target->type_set_count,callable);
                if(callable_typed&&target->type_variable_count>0)
                    for(size_t index=0;index<callable->callable_arity;index++) {
                        const uint16_t parameter_set=
                            callable->callable_parameter_sets[index];
                        if(parameter_set>=target->type_set_count)continue;
                        bool resolved=true;
                        const uint16_t substituted=clone_substituted_type_set(
                            compiler,target,parameter_set,type_arguments,
                            type_argument_count,&resolved);
                        if(!resolved||substituted==DIAMOND_NO_TYPE_SET)continue;
                        compiler->contextual_block_type_sets[index]=
                            (int32_t)substituted;
                        const DiamondTypeSet *bound=&compiler->function->
                            type_sets[substituted];
                        if(bound->count==1&&bound->members[0].id<
                               DIAMOND_TYPE_VARIABLE_BASE)
                            compiler->contextual_block_types[index]=
                                bound->members[0].id;
                    }
                if(callable_typed&&callable_return_set!=DIAMOND_NO_TYPE_SET) {
                    /* The first clone preserves non-generic contracts. A
                     * generic return must instead be rebuilt against this
                     * call site's concrete bindings; never publish an
                     * unbound type variable on the anonymous closure. */
                    compiler->contextual_block_return_set=-1;
                    bool resolved=true;
                    const uint16_t substituted=clone_substituted_type_set(
                        compiler,target,callable_return_set,
                        type_arguments,type_argument_count,&resolved);
                    if(resolved&&substituted!=DIAMOND_NO_TYPE_SET)
                        compiler->contextual_block_return_set=
                            (int32_t)substituted;
                }
            }
        }
    }
    const uint16_t block=compile_block(compiler);
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    return block;
}

static void infer_contextual_type_set(Compiler *compiler,
        const DiamondFunction *target,uint16_t expected_index,
        uint16_t actual_index,uint16_t *bindings) {
    if(expected_index>=target->type_set_count||
       actual_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet *expected=&target->type_sets[expected_index];
    const DiamondTypeSet *actual=&compiler->function->type_sets[actual_index];
    for(size_t wanted_index=0;wanted_index<expected->count;wanted_index++) {
        const DiamondTypeMember wanted=expected->members[wanted_index];
        if(wanted.id>=DIAMOND_TYPE_VARIABLE_BASE&&
           wanted.id<DIAMOND_TYPE_INTERFACE_BASE) {
            const size_t variable=
                (size_t)(wanted.id-DIAMOND_TYPE_VARIABLE_BASE);
            if(variable<8) {
                if(bindings[variable]==DIAMOND_NO_TYPE_SET)
                    bindings[variable]=actual_index;
                else if(bindings[variable]!=actual_index&&
                        !type_sets_structurally_equal(
                            compiler->function->type_sets,
                            compiler->function->type_set_count,
                            bindings[variable],compiler->function->type_sets,
                            compiler->function->type_set_count,actual_index,0))
                    bindings[variable]=(uint16_t)(DIAMOND_NO_TYPE_SET-1u);
            }
            continue;
        }
        for(size_t known_index=0;known_index<actual->count;known_index++) {
            const DiamondTypeMember known=actual->members[known_index];
            if(known.id!=wanted.id)continue;
            if(wanted.argument_set!=DIAMOND_NO_TYPE_SET&&
               known.argument_set!=DIAMOND_NO_TYPE_SET)
                infer_contextual_type_set(compiler,target,wanted.argument_set,
                    known.argument_set,bindings);
            if(wanted.second_argument_set!=DIAMOND_NO_TYPE_SET&&
               known.second_argument_set!=DIAMOND_NO_TYPE_SET)
                infer_contextual_type_set(compiler,target,
                    wanted.second_argument_set,known.second_argument_set,
                    bindings);
            if(wanted.id==DIAMOND_TYPE_CALLABLE&&
               wanted.callable_parameters_typed&&
               known.callable_parameters_typed&&
               wanted.callable_arity==known.callable_arity) {
                for(size_t parameter=0;parameter<wanted.callable_arity;
                    parameter++) {
                    const uint16_t wanted_parameter=
                        wanted.callable_parameter_sets[parameter];
                    const uint16_t known_parameter=
                        known.callable_parameter_sets[parameter];
                    if(wanted_parameter!=DIAMOND_NO_TYPE_SET&&
                       known_parameter!=DIAMOND_NO_TYPE_SET)
                        infer_contextual_type_set(compiler,target,
                            wanted_parameter,known_parameter,bindings);
                }
                if(wanted.callable_return_set!=DIAMOND_NO_TYPE_SET&&
                   known.callable_return_set!=DIAMOND_NO_TYPE_SET)
                    infer_contextual_type_set(compiler,target,
                        wanted.callable_return_set,
                        known.callable_return_set,bindings);
            }
        }
    }
}

static void infer_contextual_argument(Compiler *compiler,
        const DiamondFunction *target,size_t parameter,uint16_t argument,
        uint16_t *bindings) {
    if(parameter>=16)return;
    const uint16_t expected=target->parameter_type_sets[parameter];
    if(expected==DIAMOND_NO_TYPE_SET||expected>=target->type_set_count)return;
    int32_t actual=compiler->known_type_sets[argument];
    if(actual<0) {
        const uint8_t known=compiler->known_types[argument];
        if(known==TYPE_UNKNOWN||known>=DIAMOND_TYPE_VARIABLE_BASE)return;
        actual=(int32_t)concrete_type_set(compiler,known);
    }
    if(actual>=0&&(size_t)actual<compiler->function->type_set_count)
        infer_contextual_type_set(compiler,target,expected,(uint16_t)actual,
            bindings);
}

/* Parses one argument while exposing its declared parameter contract to the
 * expression itself. This is deliberately a one-expression conduit: generic
 * method references can consume it bidirectionally, while every nested or
 * subsequent expression sees the prior context restored. */
static uint16_t parse_expected_argument(Compiler *compiler,
        const DiamondFunction *target,size_t parameter) {
    const int32_t outer=compiler->expected_expression_type_set;
    compiler->expected_expression_type_set=-1;
    if(target!=nullptr&&parameter<target->arity) {
        const uint16_t source=target->parameter_type_sets[parameter];
        if(source!=DIAMOND_NO_TYPE_SET&&source<target->type_set_count) {
            const uint16_t cloned=target==compiler->function?source:
                clone_type_set_into_current(compiler,target->type_sets,
                    target->type_set_count,source);
            if(cloned!=DIAMOND_NO_TYPE_SET)
                compiler->expected_expression_type_set=(int32_t)cloned;
        }
    }
    const uint16_t result=parse_expression(compiler);
    compiler->expected_expression_type_set=outer;
    return result;
}

static size_t infer_contextual_type_arguments(Compiler *compiler,
        const DiamondFunction *target,const uint16_t *arguments,
        size_t argument_count,uint16_t *bindings) {
    for(size_t index=0;index<8;index++)bindings[index]=DIAMOND_NO_TYPE_SET;
    if(target==nullptr)return 0;
    for(size_t parameter=0;parameter<argument_count&&parameter<16;parameter++)
        infer_contextual_argument(compiler,target,parameter,
            arguments[parameter],bindings);
    return target->type_variable_count;
}

static size_t infer_contextual_spread_arguments(Compiler *compiler,
        const DiamondFunction *target,uint16_t spread,size_t parameter_count,
        uint16_t *bindings) {
    for(size_t index=0;index<8;index++)bindings[index]=DIAMOND_NO_TYPE_SET;
    if(target==nullptr)return 0;
    const size_t prefix_count=compiler->positional_spread_index;
    for(size_t index=0;index<prefix_count&&index<parameter_count;index++)
        infer_contextual_argument(compiler,target,index,
            compiler->positional_spread_fixed[index],bindings);
    const size_t fixed_count=compiler->positional_spread_fixed_count;
    const size_t suffix_count=fixed_count>prefix_count?
        fixed_count-prefix_count:0;
    const size_t suffix_start=compiler->positional_spread_literal?
        prefix_count+compiler->positional_spread_count:
        parameter_count>=suffix_count?parameter_count-suffix_count:
            parameter_count;
    if(suffix_start+suffix_count<=parameter_count)
        for(size_t index=0;index<suffix_count;index++)
            infer_contextual_argument(compiler,target,
                suffix_start+index,
                compiler->positional_spread_fixed[prefix_count+index],
                bindings);
    if(compiler->positional_spread_literal) {
        for(size_t index=0;index<compiler->positional_spread_count;index++) {
            const size_t parameter=compiler->positional_spread_first+index;
            if(parameter>=parameter_count||parameter>=16)break;
            infer_contextual_argument(compiler,target,parameter,
                compiler->positional_spread_elements[index],bindings);
        }
        return target->type_variable_count;
    }
    const int32_t element_set=array_element_type_set(compiler,spread);
    if(element_set<0)return target->type_variable_count;
    const size_t spread_end=parameter_count>=suffix_count?
        parameter_count-suffix_count:prefix_count;
    for(size_t parameter=prefix_count;
        parameter<spread_end&&parameter<16;parameter++) {
        const uint16_t expected=target->parameter_type_sets[parameter];
        if(expected!=DIAMOND_NO_TYPE_SET&&expected<target->type_set_count)
            infer_contextual_type_set(compiler,target,expected,
                (uint16_t)element_set,bindings);
    }
    return target->type_variable_count;
}

static void infer_contextual_keyword_arguments(Compiler *compiler,
        const DiamondFunction *target,const DiamondSpan *names,
        const uint16_t *values,size_t count,uint16_t *bindings) {
    if(target==nullptr)return;
    for(size_t keyword=0;keyword<count;keyword++)
        for(size_t parameter=0;parameter<target->arity&&parameter<16;
            parameter++)
            if(name_equals(compiler,target->parameter_names[parameter],
                    names[keyword],false)) {
                infer_contextual_argument(compiler,target,parameter,
                    values[keyword],bindings);
                break;
            }
}

static uint16_t compile_callable_value_block(Compiler *compiler,
        int32_t callable_set_index) {
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    if(callable_set_index>=0&&
       (size_t)callable_set_index<compiler->function->type_set_count) {
        const DiamondTypeSet *outer=
            &compiler->function->type_sets[(size_t)callable_set_index];
        uint16_t block_sets[DIAMOND_MAX_UNION_TYPES];
        size_t block_set_count=0;
        bool compatible=outer->count>0;
        for(size_t index=0;index<outer->count&&compatible;index++) {
            const DiamondTypeMember *callable=&outer->members[index];
            if(callable->id!=DIAMOND_TYPE_CALLABLE||
               !callable->callable_parameters_typed||
               callable->callable_arity==0) {compatible=false;break;}
            const uint16_t block_set=callable->callable_parameter_sets[
                callable->callable_arity-1];
            if(block_set>=compiler->function->type_set_count) {
                compatible=false;break;
            }
            const DiamondTypeSet *set=&compiler->function->type_sets[block_set];
            if(set->count!=1||set->members[0].id!=DIAMOND_TYPE_CALLABLE) {
                compatible=false;break;
            }
            block_sets[block_set_count++]=block_set;
        }
        uint16_t shared_block_set=DIAMOND_NO_TYPE_SET;
        for(size_t candidate=0;candidate<block_set_count&&
            shared_block_set==DIAMOND_NO_TYPE_SET;candidate++) {
            const DiamondTypeMember actual=compiler->function->type_sets[
                block_sets[candidate]].members[0];
            bool satisfies_all=true;
            for(size_t expected=0;expected<block_set_count;expected++)
                if(!type_members_satisfy_across(compiler,
                        compiler->function->type_sets,actual,
                        compiler->function->type_sets,
                        compiler->function->type_sets[
                            block_sets[expected]].members[0])) {
                    satisfies_all=false;break;
                }
            if(satisfies_all)shared_block_set=block_sets[candidate];
        }
        DiamondTypeMember synthesized={.id=DIAMOND_TYPE_CALLABLE,
            .argument_set=DIAMOND_NO_TYPE_SET,
            .second_argument_set=DIAMOND_NO_TYPE_SET,
            .callable_arity=UINT8_MAX,
            .callable_return_set=DIAMOND_NO_TYPE_SET,
            .callable_parameters_typed=false};
        bool synthesized_context=false;
        if(compatible&&shared_block_set==DIAMOND_NO_TYPE_SET&&
           block_set_count>0) {
            const DiamondTypeMember first=compiler->function->type_sets[
                block_sets[0]].members[0];
            synthesized.callable_arity=first.callable_arity;
            synthesized.callable_parameters_typed=
                first.callable_parameters_typed;
            synthesized_context=first.callable_parameters_typed;
            for(size_t arm=1;arm<block_set_count&&synthesized_context;arm++) {
                const DiamondTypeMember member=compiler->function->type_sets[
                    block_sets[arm]].members[0];
                if(!member.callable_parameters_typed||
                   member.callable_arity!=synthesized.callable_arity)
                    synthesized_context=false;
            }
            for(size_t parameter=0;parameter<synthesized.callable_arity&&
                synthesized_context;parameter++) {
                uint16_t selected=DIAMOND_NO_TYPE_SET;
                for(size_t candidate=0;candidate<block_set_count&&
                    selected==DIAMOND_NO_TYPE_SET;candidate++) {
                    const uint16_t candidate_set=
                        compiler->function->type_sets[block_sets[candidate]].
                            members[0].callable_parameter_sets[parameter];
                    if(candidate_set==DIAMOND_NO_TYPE_SET)continue;
                    bool accepts_all=true;
                    for(size_t arm=0;arm<block_set_count;arm++) {
                        const uint16_t arm_set=compiler->function->type_sets[
                            block_sets[arm]].members[0].
                                callable_parameter_sets[parameter];
                        if(arm_set==DIAMOND_NO_TYPE_SET||
                           !type_sets_satisfy_across(compiler,
                               compiler->function->type_sets,arm_set,
                               compiler->function->type_sets,candidate_set)) {
                            accepts_all=false;break;
                        }
                    }
                    if(accepts_all)selected=candidate_set;
                }
                if(selected==DIAMOND_NO_TYPE_SET) {
                    int32_t joined=-1;
                    for(size_t arm=0;arm<block_set_count;arm++) {
                        const uint16_t arm_set=compiler->function->type_sets[
                            block_sets[arm]].members[0].
                                callable_parameter_sets[parameter];
                        if(arm_set==DIAMOND_NO_TYPE_SET) {joined=-1;break;}
                        joined=joined<0?(int32_t)arm_set:
                            join_type_set_indices(compiler,joined,arm_set);
                        if(joined<0)break;
                    }
                    if(joined>=0)selected=(uint16_t)joined;
                }
                if(selected==DIAMOND_NO_TYPE_SET)synthesized_context=false;
                else synthesized.callable_parameter_sets[parameter]=selected;
            }
            /* Callable returns are covariant. Select an existing declared
             * return graph only when it can satisfy every arm; unlike input
             * synthesis above, this therefore chooses a shared subtype. */
            for(size_t candidate=0;candidate<block_set_count&&
                synthesized.callable_return_set==DIAMOND_NO_TYPE_SET;
                candidate++) {
                const uint16_t candidate_set=compiler->function->type_sets[
                    block_sets[candidate]].members[0].callable_return_set;
                if(candidate_set==DIAMOND_NO_TYPE_SET)continue;
                bool satisfies_all=true;
                for(size_t arm=0;arm<block_set_count;arm++) {
                    const uint16_t arm_set=compiler->function->type_sets[
                        block_sets[arm]].members[0].callable_return_set;
                    if(arm_set==DIAMOND_NO_TYPE_SET||
                       !type_sets_satisfy_across(compiler,
                           compiler->function->type_sets,candidate_set,
                           compiler->function->type_sets,arm_set)) {
                        satisfies_all=false;break;
                    }
                }
                if(satisfies_all)
                    synthesized.callable_return_set=candidate_set;
            }
        }
        if(compatible&&shared_block_set!=DIAMOND_NO_TYPE_SET) {
            const DiamondTypeSet *set=
                &compiler->function->type_sets[shared_block_set];
            prepare_contextual_block(compiler,compiler->function->type_sets,
                compiler->function->type_set_count,&set->members[0]);
        } else if(synthesized_context)
            prepare_contextual_block(compiler,compiler->function->type_sets,
                compiler->function->type_set_count,&synthesized);
    }
    const uint16_t block=compile_block(compiler);
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    return block;
}

static uint16_t parse_call(Compiler *compiler, DiamondSpan name) {
    const int callable_local=find_local(compiler,name);
    if(callable_local>=0&&compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN) {
        uint16_t callable=compiler->locals[(size_t)callable_local].reg;
        if(compiler->locals[(size_t)callable_local].captured) {
            /* See parse_identifier's own BOX_LOCAL re-emission for why this
             * defensive re-box is needed: `captured` doesn't imply this
             * control-flow path actually ran the boxing site. */
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,callable,0,0,1);
            const uint16_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,callable,0,2);
            callable=loaded;
        }
        return parse_closure_call_arguments(compiler, callable);
    }
    if(name_equals(compiler,"block_given?",name,false)) {
        advance_token(compiler);skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
            fail(compiler,compiler->current.span,
                "block_given? does not accept arguments");return 0;
        }
        advance_token(compiler);
        const uint16_t destination=allocate_register(compiler);
        if(!compiler->has_current_block) {
            emit_instruction(compiler,DIAMOND_OP_BOOL,destination,false,0,2);
        } else {
            const uint16_t block=load_current_block(compiler);
            const uint16_t absent=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NIL,absent,0,0,1);
            const uint16_t missing=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_EQUAL,missing,
                block,absent,3);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,missing,0,2);
        }
        compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
        return destination;
    }
    const int function_index = find_function(compiler, name);
    if (function_index < 0) {
        if(compiler->discovery_pass)
            return parse_discovery_unknown_call(compiler);
        fail(compiler, name, "undefined function");
        return 0;
    }
    const DiamondFunction *function =
        compiler->program->functions[(size_t)function_index];
    uint16_t type_arguments[8];
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
                (uint16_t)parse_type_annotation(compiler);
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
    if(call_arguments_have_spread(compiler)) {
        uint8_t keyword_slots[16];uint16_t keyword_registers[16];
        size_t keyword_count=0;
        uint16_t array_register=parse_spread_argument_array(compiler,
            function,keyword_slots,keyword_registers,&keyword_count,
            function,function->arity);
        uint16_t inferred_arguments[8];
        const size_t spread_parameter_count=
            compiler->current.kind==DIAMOND_TOKEN_DO?
                (function->arity==0?0:function->arity-1):function->arity;
        const size_t inferred_count=type_argument_count==0?
            infer_contextual_spread_arguments(compiler,function,
                array_register,spread_parameter_count,
                inferred_arguments):0;
        if(type_argument_count==0)
            for(size_t index=0;index<keyword_count;index++)
                infer_contextual_argument(compiler,function,
                    keyword_slots[index],keyword_registers[index],
                    inferred_arguments);
        const uint16_t *resolved_arguments=type_argument_count>0?
            type_arguments:inferred_arguments;
        const size_t resolved_count=type_argument_count>0?
            type_argument_count:inferred_count;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t spread_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
                array_register,0,2);
            array_register=spread_snapshot;
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_registers[index],0,2);
                keyword_registers[index]=snapshot;
            }
            const uint16_t block=compile_contextual_typed_block(compiler,
                function,function->arity==0?0:function->arity-1,
                resolved_arguments,resolved_count);
            if(keyword_count>0) {
                if(keyword_count==16) {
                    fail(compiler,compiler->previous.span,
                        "too many keyword arguments");return 0;
                }
                keyword_slots[keyword_count]=(uint8_t)(function->arity-1);
                keyword_registers[keyword_count]=block;
                keyword_count++;
            } else array_register=emit_build_spread_arguments(compiler,
                nullptr,0,array_register,&block,1,false);
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,keyword_count==0?
            (type_argument_count==0?DIAMOND_OP_CALL_SPREAD:
             DIAMOND_OP_CALL_TYPED_SPREAD):
            (type_argument_count==0?DIAMOND_OP_CALL_KEYWORD_SPREAD:
             DIAMOND_OP_CALL_TYPED_KEYWORD_SPREAD));
        emit_register(compiler,destination);
        emit_function_index(compiler,(size_t)function_index);
        emit_register(compiler,array_register);
        if(keyword_count>0) {
            emit_byte(compiler,(uint8_t)keyword_count);
            for(size_t index=0;index<keyword_count;index++) {
                emit_byte(compiler,keyword_slots[index]);
                emit_register(compiler,keyword_registers[index]);
            }
        }
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
        publish_declared_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    /* Keyword arguments (direct top-level calls only -- see docs/roadmap.md):
     * each argument is placed into its declared positional slot rather than
     * appended, so a keyword can fill any parameter regardless of the order
     * it's written at the call site. Positional arguments still fill slots
     * left-to-right in declaration order. */
    uint16_t slot_registers[16];
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
            slot_registers[slot]=parse_expected_argument(compiler,function,slot);
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
    uint16_t inferred_arguments[8];
    for(size_t index=0;index<8;index++)
        inferred_arguments[index]=DIAMOND_NO_TYPE_SET;
    if(type_argument_count==0&&function->type_variable_count>0)
        for(size_t parameter=0;parameter<argument_count&&
            parameter<function->arity;parameter++) {
            if(!slot_filled[parameter])continue;
            infer_contextual_argument(compiler,function,parameter,
                slot_registers[parameter],inferred_arguments);
        }
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:function->type_variable_count;
    /* `function_name(args) do |x| ... end` -- same "block is the last
     * positional argument" desugaring parse_invoke's own DIAMOND_TOKEN_DO
     * check does; if a keyword argument already filled this slot too
     * (nonsensical: a callback passed both by name and by trailing
     * block), the ordinary arity check a few lines down catches it, no
     * special-casing needed here. */
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(argument_count==16) {
            fail(compiler,compiler->current.span,"too many arguments");return 0;
        }
        /* Same register-aliasing hazard parse_invoke's own DIAMOND_TOKEN_DO
         * branch guards against (see its comment): every slot filled above
         * may still be a bare alias of a local the block below is about to
         * eagerly capture and BOX_LOCAL. Snapshot each filled slot into a
         * fresh (never-a-local) temp first. */
        for(size_t index=0;index<argument_count;index++) {
            if(!slot_filled[index])continue;
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,slot_registers[index],0,2);
            slot_registers[index]=snapshot;
        }
        const uint16_t block=compile_contextual_typed_block(compiler,
            function,function->arity==0?0:function->arity-1,
            resolved_arguments,resolved_count);
        const size_t block_slot=function->arity==0?0:function->arity-1;
        bool contiguous=true;
        for(size_t index=0;index<argument_count;index++)
            if(!slot_filled[index])contiguous=false;
        if(type_argument_count==0&&contiguous&&argument_count<block_slot) {
            const uint16_t callable=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,callable);
            emit_function_index(compiler,(size_t)function_index);emit_byte(compiler,0);
            const uint16_t positional=emit_argument_array(compiler,slot_registers,
                argument_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE_KEYWORDS);
            emit_register(compiler,destination);emit_register(compiler,callable);
            emit_register(compiler,positional);emit_byte(compiler,0x80u);
            emit_register(compiler,block);
            publish_declared_return_type(compiler,destination,function,
                resolved_arguments,resolved_count);
            return destination;
        }
        slot_registers[argument_count]=block;
        slot_filled[argument_count]=true;
        argument_count++;
    }
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
    if (argument_count < function->required_arity||
        (argument_count > function->arity && !function->has_variadic)) {
        fail(compiler, name, "wrong number of arguments");
        return 0;
    }

    const uint16_t argument_base = allocate_register(compiler);
    for (size_t index = 1; index < argument_count; index++) {
        (void)allocate_register(compiler);
    }
    for (size_t index = 0; index < argument_count; index++) {
        emit_instruction(compiler, DIAMOND_OP_MOVE,
                         (uint16_t)(argument_base + index), slot_registers[index], 0, 2);
    }
    const uint16_t destination = allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?
        DIAMOND_OP_CALL:DIAMOND_OP_CALL_TYPED);
    emit_register(compiler,destination);
    emit_function_index(compiler, (size_t)function_index);
    emit_register(compiler,argument_base);
    emit_byte(compiler, (uint8_t)argument_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    publish_declared_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
    return destination;
}

/* receiver_class_index: the literal class named at this call site
 * (`Author.find(...)` -> Author's own class index), or -1 for a module
 * singleton call (modules have no receiver value at all). Only used when
 * method->needs_receiver -- populates the callee's implicit self slot
 * (register 0) with a real DIAMOND_VALUE_CLASS literal for a class-owned
 * singleton method, so `self` inside an inherited method's body reflects
 * the actual receiver rather than owner_class. A module call's slot stays
 * zero-inited (DIAMOND_VALUE_NIL) exactly as before -- modules have no
 * `self` story and nothing reads that slot as one. */
/* Shared tail of an ordinary `Namespace.method(args)` call and a bare
 * `Namespace.method` *reference* (parse_singleton_reference, below) --
 * arity-checks `argument_count` against `method`'s own (with the same
 * has_variadic upper-bound relaxation every other call site has),
 * allocates the contiguous call-argument register range, loads the
 * literal receiver class for a class singleton call, MOVEs each
 * already-resolved argument register into place, and emits the CALL/
 * CALL_TYPED instruction against method->function_index -- the one
 * place `emit_function_index(method->function_index)` happens, so a
 * synthesized reference wrapper's body is byte-for-byte the same shape
 * an ordinary hand-written call site would compile to. */
static uint16_t emit_singleton_call(Compiler *compiler,const DiamondMethod *method,
        int receiver_class_index,const uint16_t *arguments,size_t argument_count,
        DiamondSpan name,uint8_t type_argument_count,const uint16_t *type_arguments) {
    if(argument_count<method->required_arity||
       (argument_count>method->arity && !method->has_variadic)) {
        fail(compiler,name,"wrong number of arguments");return 0;
    }
    const size_t call_count=argument_count+(method->needs_receiver?1:0);
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<call_count;index++)(void)allocate_register(compiler);
    /* No NIL for `base` when needs_receiver and there's no literal class
     * (a module call): sole writer, already zero-inited by run_chunk's
     * [0, register_count) init. A class singleton call instead loads the
     * literal receiver class right here, the one place this slot gets a
     * real value instead of relying on zero-init. */
    if(method->needs_receiver&&receiver_class_index>=0) {
        emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
        emit_register(compiler,base);
        emit_byte(compiler,(uint8_t)receiver_class_index);
    }
    for(size_t index=0;index<argument_count;index++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,
                         (uint16_t)(base+index+(method->needs_receiver?1:0)),
                         arguments[index],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_CALL:
                DIAMOND_OP_CALL_TYPED);
    emit_register(compiler,destination);
    emit_function_index(compiler,method->function_index);
    emit_register(compiler,base);emit_byte(compiler,(uint8_t)call_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    const DiamondFunction *target=
        compiler->program->functions[method->function_index];
    publish_declared_return_type(compiler,destination,target,
        type_arguments,type_argument_count);
    return destination;
}

static bool infer_reference_type_arguments(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_offset,size_t arity,
        uint16_t *bindings) {
    if(target==nullptr||target->type_variable_count==0||
       compiler->expected_expression_type_set<0||
       (size_t)compiler->expected_expression_type_set>=
           compiler->function->type_set_count)return false;
    const DiamondTypeSet *expected=&compiler->function->type_sets[
        (size_t)compiler->expected_expression_type_set];
    if(expected->count!=1||expected->members[0].id!=DIAMOND_TYPE_CALLABLE||
       !expected->members[0].callable_parameters_typed||
       expected->members[0].callable_arity!=arity)return false;
    const DiamondTypeMember callable=expected->members[0];
    for(size_t index=0;index<8;index++)bindings[index]=DIAMOND_NO_TYPE_SET;
    for(size_t parameter=0;parameter<arity;parameter++) {
        const size_t target_parameter=parameter+parameter_offset;
        if(target_parameter>=target->arity) return false;
        const uint16_t wanted=target->parameter_type_sets[target_parameter];
        const uint16_t actual=callable.callable_parameter_sets[parameter];
        if(wanted!=DIAMOND_NO_TYPE_SET&&actual!=DIAMOND_NO_TYPE_SET)
            infer_contextual_type_set(compiler,target,wanted,actual,bindings);
    }
    if(target->return_type_set!=DIAMOND_NO_TYPE_SET&&
       callable.callable_return_set!=DIAMOND_NO_TYPE_SET)
        infer_contextual_type_set(compiler,target,target->return_type_set,
            callable.callable_return_set,bindings);
    for(size_t index=0;index<target->type_variable_count;index++)
        if(bindings[index]==DIAMOND_NO_TYPE_SET)return false;
    return true;
}

/* `Namespace.method`, no call following -- a bare reference to a class/
 * module singleton method as a value, not a call. Synthesizes a small
 * hidden top-level function (owner_class=UINT8_MAX, nested=true -- not
 * findable by bare name, only reachable via the DIAMOND_OP_CLOSURE this
 * emits at the reference site) whose entire body forwards its own
 * parameters into method via emit_singleton_call, the exact same call
 * shape parse_singleton_call itself would compile -- so the resulting
 * value is exactly as correct as any hand-written `Namespace.method
 * (args)` call site already is (see docs/design.md's "Bare singleton
 * method references" section for why: this call is already resolved
 * fully at compile time, never re-dispatched at runtime, so a wrapper
 * reusing this same compiled shape has nothing dynamic left to get
 * wrong). Mirrors compile_block's own save/restore-outer-compiler-state
 * shape (immediately above `compile_definition` further down), but
 * skips everything block-specific: no real parameter list to parse (the
 * wrapper's signature is copied from `method`'s own), no eager capture
 * materialization (this closure captures nothing at all -- the target
 * class/module is a compile-time constant baked directly into the
 * synthesized body via emit_function_index, same as any ordinary call),
 * no user-written body to compile_sequence.
 *
 * Variadic wrappers collect their trailing arguments and forward through
 * CALL_SINGLETON_SPREAD; explicit generic bindings are baked into the wrapper's
 * CALL_TYPED/CALL_TYPED_SINGLETON_SPREAD instruction. */
static uint16_t parse_singleton_reference(Compiler *compiler,
                                         const DiamondMethod *method,
                                         DiamondSpan namespace_name,
                                         int receiver_class_index,
                                         const uint16_t *type_arguments,
                                         size_t type_argument_count) {
    const DiamondFunction *target=
        compiler->program->functions[method->function_index];
    uint16_t inferred_arguments[8];
    if(target->type_variable_count>0&&type_argument_count==0&&
       infer_reference_type_arguments(compiler,target,0,method->arity,
           inferred_arguments)) {
        type_arguments=inferred_arguments;
        type_argument_count=target->type_variable_count;
    }
    if(target->type_variable_count>0&&type_argument_count==0) {
        fail(compiler,namespace_name,
             "generic singleton method reference requires explicit bindings");
        return 0;
    }
    uint16_t parameter_sets[16];
    for(size_t index=0;index<16;index++)
        parameter_sets[index]=DIAMOND_NO_TYPE_SET;
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    bool resolved=true;
    const size_t original_type_set_count=compiler->function->type_set_count;
    for(size_t parameter=0;parameter<method->arity&&parameter<16;parameter++) {
        const uint16_t source=target->parameter_type_sets[parameter];
        if(source==DIAMOND_NO_TYPE_SET)continue;
        parameter_sets[parameter]=target->type_variable_count>0?
            clone_substituted_type_set(compiler,target,source,type_arguments,
                type_argument_count,&resolved):clone_type_set_into_current(
                compiler,target->type_sets,target->type_set_count,source);
    }
    if(target->return_type_set!=DIAMOND_NO_TYPE_SET)
        return_set=target->type_variable_count>0?
            clone_substituted_type_set(compiler,target,target->return_type_set,
                type_arguments,type_argument_count,&resolved):
            clone_type_set_into_current(compiler,target->type_sets,
                target->type_set_count,target->return_type_set);
    if(!resolved) {
        compiler->function->type_set_count=original_type_set_count;
        fail(compiler,namespace_name,
            "could not resolve generic method reference contract");
        return 0;
    }
    if(compiler->program->function_count==DIAMOND_MAX_FUNCTIONS) {
        fail(compiler,namespace_name,"too many functions");
        return 0;
    }
    size_t function_index=0;
    DiamondFunction *function=compiler_add_function(compiler,&function_index);
    if(function==nullptr) {
        fail(compiler,namespace_name,"out of memory");
        return 0;
    }
    function->owner_class=UINT8_MAX;
    function->nested=true;
    function->return_type_set=return_set;
    function->arity=method->arity;
    function->required_arity=method->required_arity;
    function->has_variadic=method->has_variadic;
    function->type_set_count=compiler->function->type_set_count;
    if(!diamond_function_reserve_type_sets(function,function->type_set_count)) {
        fail(compiler,namespace_name,"out of memory");return 0;
    }
    memcpy(function->type_sets,compiler->function->type_sets,
        function->type_set_count*sizeof function->type_sets[0]);
    static const char reference_name[]="<method reference>";
    for(size_t index=0;index<sizeof(reference_name);index++)
        function->name[index]=reference_name[index];
    function->declaration_line=(uint32_t)compiler->previous.span.line;
    function->declaration_column=(uint32_t)compiler->previous.span.column;
    function->declaration_start=compiler->previous.span.start;
    for(size_t index=0;index<16;index++) {
        function->parameter_type_sets[index]=parameter_sets[index];
        (void)snprintf(function->parameter_names[index],
            DIAMOND_MAX_FUNCTION_NAME,"%s",target->parameter_names[index]);
    }

    DiamondFunction *outer_function=compiler->function;
    const uint16_t outer_next_register=compiler->next_register;
    const size_t outer_local_count=compiler->local_count;
    const bool outer_in_function=compiler->in_function;

    compiler->function=function;
    compiler->next_register=0;
    compiler->local_count=0;
    compiler->in_function=true;

    uint16_t arguments[16];
    for(size_t index=0;index<method->arity;index++) {
        arguments[index]=allocate_register(compiler);
    }
    uint16_t body_result=0;
    if(method->has_variadic) {
        const size_t fixed_count=method->arity-1;
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            arguments[fixed_count],(uint16_t)fixed_count,0,3);
        uint16_t spread=arguments[fixed_count];
        if(fixed_count>0)spread=emit_build_spread_arguments(compiler,arguments,
            fixed_count,spread,nullptr,0,false);
        body_result=allocate_register(compiler);
        emit_opcode(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_SPREAD:
            DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD);
        emit_register(compiler,body_result);
        emit_function_index(compiler,method->function_index);
        emit_register(compiler,spread);
        emit_byte(compiler,receiver_class_index<0?UINT8_MAX:
            (uint8_t)receiver_class_index);
        emit_byte(compiler,method->needs_receiver?1:0);
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
    } else body_result=emit_singleton_call(compiler,method,
        receiver_class_index,arguments,method->arity,namespace_name,
        (uint8_t)type_argument_count,type_arguments);
    emit_instruction(compiler,DIAMOND_OP_RETURN,body_result,0,0,1);

    function->register_count=compiler->next_register;
    function->body_end=compiler->previous.span.start+compiler->previous.span.length;

    compiler->function=outer_function;
    compiler->next_register=outer_next_register;
    compiler->local_count=outer_local_count;
    compiler->in_function=outer_in_function;

    const uint16_t result=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
    emit_function_index(compiler,function_index);emit_byte(compiler,0);
    publish_function_callable_type(compiler,result,function);
    return result;
}

static uint16_t parse_singleton_call(Compiler *compiler,
                                    const DiamondMethod *method,
                                    DiamondSpan namespace_name,
                                    int receiver_class_index) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected singleton function after module name");return 0;
    }
    const DiamondSpan name=compiler->current.span;
    const DiamondFunction *function=
        compiler->program->functions[method->function_index];
    advance_token(compiler);
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL)advance_token(compiler);
    uint16_t type_arguments[8];size_t type_argument_count=0;
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
                (uint16_t)parse_type_annotation(compiler);
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
        /* `Namespace.method`, no call following -- a bare reference to
         * the method as a value (parse_singleton_reference, above)
         * rather than a call, unless it was given generic type
         * arguments (`Namespace.method[T]` with no call after): a
         * reference wrapper can't itself be generic, so that combination
         * stays a real error, not silently falling through. */
        return parse_singleton_reference(compiler,method,namespace_name,
            receiver_class_index,type_arguments,type_argument_count);
    }
    advance_token(compiler);
    skip_newlines(compiler);
    if(call_arguments_have_keyword(compiler)) {
        DiamondSpan keyword_names[16];uint16_t keyword_values[16];
        size_t keyword_count=0;
        const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
            keyword_names,keyword_values,&keyword_count,function);
        uint16_t inferred_arguments[8];
        if(type_argument_count==0) {
            const size_t block_count=
                compiler->current.kind==DIAMOND_TOKEN_DO?1u:0u;
            infer_contextual_spread_arguments(compiler,function,positional,
                method->arity>block_count?method->arity-block_count:0,
                inferred_arguments);
            infer_contextual_keyword_arguments(compiler,function,
                keyword_names,keyword_values,keyword_count,inferred_arguments);
        }
        const uint16_t *resolved_arguments=type_argument_count>0?
            type_arguments:inferred_arguments;
        const size_t resolved_count=type_argument_count>0?
            type_argument_count:function->type_variable_count;
        bool has_block=false;uint16_t block=0;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_values[index],0,2);
                keyword_values[index]=snapshot;
            }
            block=compile_contextual_typed_block(compiler,function,
                method->arity==0?0:method->arity-1,
                resolved_arguments,resolved_count);
            has_block=true;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
            DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS);
        emit_register(compiler,destination);
        emit_function_index(compiler,method->function_index);
        emit_register(compiler,positional);
        emit_byte(compiler,receiver_class_index<0?UINT8_MAX:
            (uint8_t)receiver_class_index);
        emit_byte(compiler,method->needs_receiver?1:0);
        emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
        for(size_t index=0;index<keyword_count;index++) {
            emit_register(compiler,add_name_string(compiler,keyword_names[index]));
            emit_register(compiler,keyword_values[index]);
        }
        if(has_block)emit_register(compiler,block);
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
        publish_declared_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    if(call_arguments_have_spread(compiler)) {
        uint16_t spread=parse_spread_argument_array(compiler,nullptr,
            nullptr,nullptr,nullptr,function,method->arity);
        uint16_t inferred_arguments[8];
        const size_t block_count=
            compiler->current.kind==DIAMOND_TOKEN_DO?1u:0u;
        const size_t spread_parameter_count=method->arity>block_count?
            method->arity-block_count:0;
        const size_t inferred_count=type_argument_count==0?
            infer_contextual_spread_arguments(compiler,function,spread,
                spread_parameter_count,inferred_arguments):0;
        const uint16_t *resolved_arguments=type_argument_count>0?
            type_arguments:inferred_arguments;
        const size_t resolved_count=type_argument_count>0?
            type_argument_count:inferred_count;
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            const uint16_t spread_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
                spread,0,2);
            spread=spread_snapshot;
            const uint16_t block=compile_contextual_typed_block(compiler,
                function,method->arity==0?0:method->arity-1,
                resolved_arguments,resolved_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,type_argument_count==0?
                DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
                DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS);
            emit_register(compiler,destination);
            emit_function_index(compiler,method->function_index);
            emit_register(compiler,spread);
            emit_byte(compiler,receiver_class_index<0?UINT8_MAX:
                (uint8_t)receiver_class_index);
            emit_byte(compiler,method->needs_receiver?1:0);
            emit_byte(compiler,0x80u);emit_register(compiler,block);
            if(type_argument_count>0) {
                emit_byte(compiler,(uint8_t)type_argument_count);
                for(size_t index=0;index<type_argument_count;index++)
                    emit_register(compiler,type_arguments[index]);
            }
            publish_declared_return_type(compiler,destination,function,
                resolved_arguments,resolved_count);
            return destination;
        }
        const uint16_t destination=allocate_register(compiler);
        emit_opcode(compiler,type_argument_count==0?
            DIAMOND_OP_CALL_SINGLETON_SPREAD:
            DIAMOND_OP_CALL_TYPED_SINGLETON_SPREAD);
        emit_register(compiler,destination);
        emit_function_index(compiler,method->function_index);
        emit_register(compiler,spread);
        emit_byte(compiler,receiver_class_index<0?UINT8_MAX:
            (uint8_t)receiver_class_index);
        emit_byte(compiler,method->needs_receiver?1:0);
        if(type_argument_count>0) {
            emit_byte(compiler,(uint8_t)type_argument_count);
            for(size_t index=0;index<type_argument_count;index++)
                emit_register(compiler,type_arguments[index]);
        }
        publish_declared_return_type(compiler,destination,function,
            resolved_arguments,resolved_count);
        return destination;
    }
    uint16_t arguments[16];size_t argument_count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        if(argument_count==16) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        arguments[argument_count]=parse_expected_argument(compiler,function,
            argument_count);
        argument_count++;
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");return 0;
    }
    advance_token(compiler);
    uint16_t inferred_arguments[8];
    const size_t inferred_count=type_argument_count==0?
        infer_contextual_type_arguments(compiler,function,arguments,
            argument_count,inferred_arguments):0;
    const uint16_t *resolved_arguments=type_argument_count>0?
        type_arguments:inferred_arguments;
    const size_t resolved_count=type_argument_count>0?
        type_argument_count:inferred_count;
    /* `Namespace.method(args) do |x| ... end` -- same trailing-block-as-
     * last-positional-argument desugaring parse_invoke's own DIAMOND_TOKEN_DO
     * handling does for instance-method calls (see its comment there for
     * the full design, including why every argument is snapshotted into a
     * fresh temp first: compile_block's eager, unconditional BOX_LOCAL
     * over every enclosing local could otherwise retarget a bare local
     * register one of these arguments already resolved to). Singleton
     * calls have no separate caller-side receiver register to snapshot
     * (unlike parse_invoke's `receiver`) -- `self`/the owning module is
     * filled in by the call machinery itself when method->needs_receiver,
     * not read from a register here. */
    if(compiler->current.kind==DIAMOND_TOKEN_DO) {
        if(argument_count==16) {
            fail(compiler,compiler->current.span,"too many call arguments");return 0;
        }
        for(size_t index=0;index<argument_count;index++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,arguments[index],0,2);
            arguments[index]=snapshot;
        }
        const uint16_t block=compile_contextual_typed_block(compiler,
            function,method->arity==0?0:method->arity-1,
            resolved_arguments,resolved_count);
        const size_t block_slot=method->arity==0?0:method->arity-1;
        if(argument_count<block_slot) {
            const uint16_t positional=
                emit_argument_array(compiler,arguments,argument_count);
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,type_argument_count==0?
                DIAMOND_OP_CALL_SINGLETON_KEYWORDS:
                DIAMOND_OP_CALL_TYPED_SINGLETON_KEYWORDS);
            emit_register(compiler,destination);
            emit_function_index(compiler,method->function_index);
            emit_register(compiler,positional);
            emit_byte(compiler,receiver_class_index<0?UINT8_MAX:
                (uint8_t)receiver_class_index);
            emit_byte(compiler,method->needs_receiver?1:0);
            emit_byte(compiler,0x80u);emit_register(compiler,block);
            if(type_argument_count>0) {
                emit_byte(compiler,(uint8_t)type_argument_count);
                for(size_t index=0;index<type_argument_count;index++)
                    emit_register(compiler,type_arguments[index]);
            }
            publish_declared_return_type(compiler,destination,function,
                resolved_arguments,resolved_count);
            return destination;
        }
        arguments[argument_count++]=block;
    }
    if(argument_count<method->required_arity||
       (argument_count>method->arity && !method->has_variadic)) {
        fail(compiler,name,"wrong number of arguments");return 0;
    }
    const size_t call_count=argument_count+(method->needs_receiver?1:0);
    const uint16_t base=allocate_register(compiler);
    for(size_t index=1;index<call_count;index++)(void)allocate_register(compiler);
    /* No NIL for `base` when needs_receiver and there's no literal class
     * (a module call): sole writer, already zero-inited by run_chunk's
     * [0, register_count) init. A class singleton call instead loads the
     * literal receiver class right here, the one place this slot gets a
     * real value instead of relying on zero-init. */
    if(method->needs_receiver&&receiver_class_index>=0) {
        emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
        emit_register(compiler,base);
        emit_byte(compiler,(uint8_t)receiver_class_index);
    }
    for(size_t index=0;index<argument_count;index++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,
                         (uint16_t)(base+index+(method->needs_receiver?1:0)),
                         arguments[index],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_opcode(compiler,type_argument_count==0?DIAMOND_OP_CALL:
                DIAMOND_OP_CALL_TYPED);
    emit_register(compiler,destination);
    emit_function_index(compiler,method->function_index);
    emit_register(compiler,base);emit_byte(compiler,(uint8_t)call_count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    publish_declared_return_type(compiler,destination,function,
        resolved_arguments,resolved_count);
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

static uint16_t parse_redefine_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'redefine_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'redefine_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after redefine_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t callable_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after redefine_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_REDEFINE_METHOD);
    emit_register(compiler,dest);
    emit_byte(compiler, (uint8_t)class_index);
    emit_register(compiler,name_register);
    emit_register(compiler,callable_register);
    compiler->known_types[dest] = DIAMOND_TYPE_NIL;
    return dest;
}

/* `ClassName.define_method(name, callable)` -- same shape as
 * redefine_method above (mirrors it line for line at the parser level;
 * the two opcodes differ only in what the VM does with an existing vs.
 * absent method slot, see vm.c's own handler comments for both). */
static uint16_t parse_define_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'define_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'define_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after define_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t callable_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after define_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_DEFINE_METHOD);
    emit_register(compiler,dest);
    emit_byte(compiler, (uint8_t)class_index);
    emit_register(compiler,name_register);
    emit_register(compiler,callable_register);
    compiler->known_types[dest] = DIAMOND_TYPE_NIL;
    return dest;
}

/* `ClassName.compile_method(name, params, body_source, bound_values)` --
 * compiles a new method body from a source string at runtime and returns
 * a Callable meant to be passed to define_method above (not
 * redefine_method, and not general Callable use -- see vm.c's own
 * DIAMOND_OP_COMPILE_METHOD/DIAMOND_OP_DEFINE_METHOD handler comments
 * and docs/design.md's "Runtime method synthesis" section for the full
 * scope). `bound_values` is a Hash of already-evaluated values (e.g. the
 * result of calling a *different* class's own method, which body_source
 * itself has no way to name directly -- see that same doc section)
 * spliced in as extra trailing parameters; pass `{}` when there are
 * none. Four arguments instead of redefine_method/define_method's two,
 * otherwise the same shape. */
static uint16_t parse_compile_method_call(Compiler *compiler, int class_index) {
    advance_token(compiler); /* consume 'compile_method' */
    if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler, compiler->current.span, "expected '(' after 'compile_method'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t name_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after compile_method name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t params_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after compile_method params");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t body_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_COMMA) {
        fail(compiler, compiler->current.span, "expected ',' after compile_method body_source");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t bound_values_register = parse_expression(compiler);
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after compile_method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest = allocate_register(compiler);
    emit_opcode(compiler, DIAMOND_OP_COMPILE_METHOD);
    emit_register(compiler,dest);
    emit_byte(compiler, (uint8_t)class_index);
    emit_register(compiler,name_register);
    emit_register(compiler,params_register);
    emit_register(compiler,body_register);
    emit_register(compiler,bound_values_register);
    compiler->known_types[dest] = DIAMOND_TYPE_CALLABLE;
    return dest;
}

static uint16_t parse_fiber_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    const bool creates=compiler->current.kind==DIAMOND_TOKEN_IDENTIFIER&&
        name_equals(compiler,"new",compiler->current.span,false);
    const bool yields=compiler->current.kind==DIAMOND_TOKEN_YIELD;
    if(!creates&&!yields) {
        fail(compiler,compiler->current.span,
            "expected 'new' or 'yield' after 'Fiber'");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
            creates?"expected '(' after 'Fiber.new'":
                    "expected '(' after 'Fiber.yield'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t value=0;
    if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
        if(creates) {
            fail(compiler,compiler->current.span,
                "Fiber.new requires a Callable argument");return 0;
        }
        value=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,value,0,0,1);
    } else value=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
            creates?"expected ')' after Fiber.new argument":
                    "Fiber.yield accepts at most one value");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,creates?DIAMOND_OP_FIBER_NEW:DIAMOND_OP_YIELD);
    emit_register(compiler,dest);
    emit_register(compiler,value);
    return dest;
}

/* `Thread.new(callable, *args)` -- unlike Fiber.new (exactly one zero-arg
 * closure, no other arguments: a suspended fiber gets later inputs via
 * .resume(value) instead), a spawned OS thread has no interactive resume
 * dialogue, so every input must be handed over here at construction. The
 * argument-list parsing/register-packing below mirrors
 * parse_closure_call_arguments's own shape (base register + consecutive
 * MOVEs) since the runtime call convention is the same; only the trailing
 * opcode differs (THREAD_NEW, not CALL_CLOSURE). See docs/threads.md. */
static uint16_t parse_thread_new_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"new",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'new' after 'Thread'");
        return 0;
    }
    advance_token(compiler); /* consume 'new' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Thread.new'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t callable_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t arguments[16]; size_t argument_count=0;
    while(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        if(argument_count==16) {
            fail(compiler,compiler->current.span,"too many Thread.new arguments");
            return 0;
        }
        arguments[argument_count++]=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Thread.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<argument_count;i++)(void)allocate_register(compiler);
    for(size_t i=0;i<argument_count;i++)
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i),arguments[i],0,2);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_THREAD_NEW);
    emit_register(compiler,dest);emit_register(compiler,callable_register);
    emit_register(compiler,base);emit_byte(compiler,(uint8_t)argument_count);
    return dest;
}

static uint16_t parse_file_open_call(Compiler *compiler) {
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
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after File.open path");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t mode_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after File.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_FILE_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,path_register);
    emit_register(compiler,mode_register);
    return dest;
}

static uint16_t parse_sqlite3_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'SQLite3'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'SQLite3.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t path_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after SQLite3.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SQLITE3_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,path_register);
    return dest;
}

static uint16_t parse_postgres_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'PostgreSQL'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'PostgreSQL.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t conninfo_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after PostgreSQL.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_POSTGRES_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,conninfo_register);
    return dest;
}

/* `MySQL.open(host, user, password, database, port)` -- unlike
 * PostgreSQL.open's single conninfo String (libpq parses that key=value
 * format itself), MariaDB Connector/C's mysql_real_connect wants these as
 * discrete arguments with no such string to parse, so this takes them the
 * same explicit way rather than inventing a DSN mini-language this project
 * would then own the parsing/escaping/documentation of. Five required
 * positional arguments, comma-separated like TCPSocket.connect's host/port
 * pair, just longer -- no optional/defaulted trailing argument the way
 * TCPServer.listen's reuse_port is, since a bind port default would hide a
 * real, easy-to-get-wrong choice (3306 vs. a nonstandard port) rather than
 * a rarely-needed knob. */
static uint16_t parse_mysql_open_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"open",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'open' after 'MySQL'");
        return 0;
    }
    advance_token(compiler); /* consume 'open' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'MySQL.open'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t user_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open user");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t password_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open password");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t database_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after MySQL.open database");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after MySQL.open arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MYSQL_OPEN);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,user_register);
    emit_register(compiler,password_register);
    emit_register(compiler,database_register);
    emit_register(compiler,port_register);
    return dest;
}

static uint16_t parse_regexp_new_call(Compiler *compiler) {
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
    const uint16_t pattern_register=parse_expression(compiler);
    skip_newlines(compiler);
    /* options is optional -- Regexp.new(pattern) is the common case,
     * defaulting to a compile-time 0 constant (REGINOLD_OPTION_NONE)
     * rather than requiring every call site to spell it out, unlike
     * File.open's two always-required arguments. */
    uint16_t options_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        options_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        options_register=allocate_register(compiler);
        const uint16_t zero=add_constant(compiler,DIAMOND_INT(0));
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,options_register,zero,0,2);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Regexp.new arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_REGEXP_NEW);
    emit_register(compiler,dest);
    emit_register(compiler,pattern_register);
    emit_register(compiler,options_register);
    return dest;
}

/* ProgramBuilder.new() -- the one ProgramBuilder call needing dedicated
 * compiler recognition (constructing the object). Every instance method
 * (.declare_function/.emit_byte/.add_constant/.add_string/
 * .set_register_count/.run) dispatches through the ordinary INVOKE opcode
 * like any other native-kind receiver (Fiber/File/Regexp), needing no
 * compiler changes at all. See docs/roadmap.md's self-hosting Phase 1
 * entry. */
static uint16_t parse_program_builder_new_call(Compiler *compiler) {
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
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PROGRAM_BUILDER_NEW);
    emit_register(compiler,dest);
    return dest;
}

static uint16_t parse_tcp_connect_call(Compiler *compiler) {
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
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TCPSocket.connect host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TCPSocket.connect arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TCP_CONNECT);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,port_register);
    return dest;
}

/* `TCPServer.listen(port)`/`listen_nonblocking(port)`, optionally followed
 * by `, reuse_port: <expr>` -- a compiler special form like Thread.new's
 * own trailing-argument parsing, so this hand-rolls the keyword rather than
 * reusing the generic call-argument path (which only exists for ordinary
 * Diamond-defined functions). Omitting the keyword compiles to a literal
 * `false` (see parse_literal's own DIAMOND_OP_BOOL emission), so the
 * opcode always receives exactly 3 operands and reuse_port stays off by
 * default -- every existing single-listener caller (http_serve, arbitrary
 * user code) keeps today's exclusive-port-ownership behavior unless it
 * explicitly opts in. See docs/io.md and packages/gremlin's own
 * gremlin_worker for why a caller would want this. */
static uint16_t parse_tcp_listen_call(Compiler *compiler) {
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
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    uint16_t reuse_port_register;
    if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
           !name_equals(compiler,"reuse_port",compiler->current.span,false)) {
            fail(compiler,compiler->current.span,
                 "expected 'reuse_port' after ',' in TCPServer.listen arguments");
            return 0;
        }
        advance_token(compiler); /* consume 'reuse_port' */
        if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
            fail(compiler,compiler->current.span,"expected ':' after 'reuse_port'");
            return 0;
        }
        advance_token(compiler); /* consume ':' */
        skip_newlines(compiler);
        reuse_port_register=parse_expression(compiler);
        skip_newlines(compiler);
    } else {
        reuse_port_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_BOOL,reuse_port_register,false,0,2);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TCPServer.listen arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,nonblocking?DIAMOND_OP_TCP_LISTEN_NONBLOCK:DIAMOND_OP_TCP_LISTEN);
    emit_register(compiler,dest);
    emit_register(compiler,port_register);
    emit_register(compiler,reuse_port_register);
    return dest;
}

static uint16_t parse_tls_connect_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"connect",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'connect' after 'TLSSocket'");
        return 0;
    }
    advance_token(compiler); /* consume 'connect' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TLSSocket.connect'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t host_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TLSSocket.connect host");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TLSSocket.connect arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TLS_CONNECT);
    emit_register(compiler,dest);
    emit_register(compiler,host_register);
    emit_register(compiler,port_register);
    return dest;
}

static uint16_t parse_tls_listen_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"listen",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'listen' after 'TLSServer'");
        return 0;
    }
    advance_token(compiler); /* consume 'listen' */
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'TLSServer.listen'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t port_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after TLSServer.listen port");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t cert_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,
             "expected ',' after TLSServer.listen certificate path");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t key_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after TLSServer.listen arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TLS_LISTEN);
    emit_register(compiler,dest);
    emit_register(compiler,port_register);
    emit_register(compiler,cert_register);
    emit_register(compiler,key_register);
    return dest;
}

static uint16_t parse_io_poll_call(Compiler *compiler) {
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
    const uint16_t readable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after IO.poll readables");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t writable_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after IO.poll writables");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t timeout_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after IO.poll arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_IO_POLL);
    emit_register(compiler,dest);
    emit_register(compiler,readable_register);
    emit_register(compiler,writable_register);
    emit_register(compiler,timeout_register);
    return dest;
}

static uint16_t parse_udp_socket_call(Compiler *compiler) {
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
    uint16_t port_register=0;
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
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,is_bind?DIAMOND_OP_UDP_BIND:DIAMOND_OP_UDP_OPEN);
    emit_register(compiler,dest);
    if(is_bind)emit_register(compiler,port_register);
    return dest;
}

static uint16_t parse_signal_trap_call(Compiler *compiler) {
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
    const uint16_t name_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after Signal.trap name");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t handler_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Signal.trap arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SIGNAL_TRAP);
    emit_register(compiler,dest);
    emit_register(compiler,name_register);
    emit_register(compiler,handler_register);
    return dest;
}

static uint16_t parse_chr_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CHR);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

static uint16_t parse_to_float_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_FLOAT);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

static uint16_t parse_to_int_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_INT);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_INT;
    return dest;
}

static uint16_t parse_to_sym_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TO_SYMBOL);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    compiler->known_types[dest]=DIAMOND_TYPE_SYMBOL;
    return dest;
}

static uint16_t parse_math_unary_call(Compiler *compiler, DiamondMathFunction id) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t source = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MATH_UNARY);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

static uint16_t parse_math_binary_call(Compiler *compiler, DiamondMathFunction id) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    const uint16_t left = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' between arguments");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t right = parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_MATH_BINARY);
    emit_register(compiler,dest);
    emit_register(compiler,left);
    emit_register(compiler,right);
    emit_byte(compiler,(uint8_t)id);
    compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

static uint16_t parse_print_call(Compiler *compiler, bool newline) {
    advance_token(compiler); /* consume '(' */
    const uint16_t source=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PRINT);
    emit_register(compiler,dest);
    emit_register(compiler,source);
    emit_byte(compiler,newline?1:0);
    compiler->known_types[dest]=DIAMOND_TYPE_NIL;
    return dest;
}

static uint16_t parse_gets_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_GETS);
    emit_register(compiler,dest);
    return dest;
}

/* exit(code = 0) -- code is optional, defaulting to a compile-time 0
 * constant (same shape as parse_regexp_new_call's own optional `options`
 * default). The allocated `dest` register is never actually written by
 * the VM (DIAMOND_OP_EXIT terminates the process outright on success) --
 * it exists only so this remains an ordinary expression-producing call
 * from the compiler's perspective, same as every other parse_*_call. */
static uint16_t parse_exit_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    skip_newlines(compiler);
    uint16_t code_register;
    if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) {
        code_register=allocate_register(compiler);
        const uint16_t zero=add_constant(compiler,DIAMOND_INT(0));
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,code_register,zero,0,2);
    } else {
        code_register=parse_expression(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after exit arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_EXIT);
    emit_register(compiler,code_register);
    compiler->known_types[dest]=DIAMOND_TYPE_NIL;
    return dest;
}

/* debugger()/breakpoint() -- pauses execution, prints the current call
 * site and every currently-live local (name + value, read-only; no
 * expression evaluation against them, see docs/syntax.md for the scope
 * this was deliberately kept to), then blocks on a single line of stdin
 * (EOF -- e.g. stdin redirected from /dev/null, the normal case under a
 * non-interactive test/CI run -- continues immediately rather than
 * hanging) before resuming normally.
 *
 * compiler->locals' (name, register) pairs *at this exact point in
 * compilation* get baked into the opcode's own operand data, the same
 * way a closure's captured-register list is baked in at CLOSURE-emission
 * time -- this is the one piece of information only the compiler has;
 * a register index doesn't carry its source variable's name once
 * compiled, so the VM has no way to reconstruct this after the fact.
 * compiler->local_count is always <= DIAMOND_MAX_LOCALS (enforced by
 * allocate_local), so no separate bounds check is needed before the
 * uint8_t cast below. */
static uint16_t parse_debugger_call(Compiler *compiler) {
    advance_token(compiler); /* consume '(' */
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_DEBUGGER);
    emit_register(compiler,dest);
    emit_byte(compiler,(uint8_t)compiler->local_count);
    for(size_t index=0;index<compiler->local_count;index++) {
        const uint16_t name_index=add_name_string(compiler,compiler->locals[index].name);
        emit_register(compiler,name_index);
        emit_register(compiler,compiler->locals[index].reg);
    }
    return dest;
}

/* Time.monotonic()/Time.now()/Time.utc_now() -- each zero-argument,
 * distinguished only by which opcode (and, for TIME_NOW, which utc
 * flag byte) they emit. */
static uint16_t parse_time_zero_argument_call(Compiler *compiler,
        DiamondOpCode opcode, uint8_t utc_flag,
        bool has_utc_flag, bool result_is_float) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after Time method name");
        return 0;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Time method arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,opcode);
    emit_register(compiler,dest);
    if(has_utc_flag)emit_byte(compiler,utc_flag);
    if(result_is_float)compiler->known_types[dest]=DIAMOND_TYPE_FLOAT;
    return dest;
}

/* Time.at(epoch) -- the one Time constructor taking an argument;
 * mirrors parse_sqlite3_open_call's own one-argument-constructor shape. */
static uint16_t parse_time_at_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Time.at'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t epoch_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Time.at arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_TIME_AT);
    emit_register(compiler,dest);
    emit_register(compiler,epoch_register);
    return dest;
}

static uint16_t parse_time_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,
             "expected 'monotonic', 'now', 'utc_now', or 'at' after 'Time'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"monotonic",method,false)) {
        advance_token(compiler);
        return parse_time_zero_argument_call(compiler,
            DIAMOND_OP_TIME_MONOTONIC,0,false,true);
    }
    if(name_equals(compiler,"now",method,false)) {
        advance_token(compiler);
        return parse_time_zero_argument_call(compiler,
            DIAMOND_OP_TIME_NOW,0,true,false);
    }
    if(name_equals(compiler,"utc_now",method,false)) {
        advance_token(compiler);
        return parse_time_zero_argument_call(compiler,
            DIAMOND_OP_TIME_NOW,1,true,false);
    }
    if(name_equals(compiler,"at",method,false)) {
        advance_token(compiler);
        return parse_time_at_call(compiler);
    }
    fail(compiler,method,"expected 'monotonic', 'now', 'utc_now', or 'at' after 'Time'");
    return 0;
}

/* Process.run(argv) -- the one Process method; mirrors parse_time_at_call's
 * one-argument-constructor shape (dest register, then a single register
 * operand -- here the argv Array rather than an epoch). argv-array-only
 * by design (see docs/syntax.md): there is no shell-string form to parse
 * at all, so no injection surface exists to guard against here. */
static uint16_t parse_process_run_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'Process.run'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t argv_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after Process.run arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_PROCESS_RUN);
    emit_register(compiler,dest);
    emit_register(compiler,argv_register);
    return dest;
}

static uint16_t parse_process_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'run' after 'Process'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"run",method,false)) {
        advance_token(compiler);
        return parse_process_run_call(compiler);
    }
    fail(compiler,method,"expected 'run' after 'Process'");
    return 0;
}

/* BCrypt.hash(password, cost) -- both arguments always required (no
 * optional-argument support at this hand-rolled class-call parse layer);
 * `cost`'s default of 12 lives one layer up, in
 * ActiveRecord::Model#secure_password=, the one real caller that wants a
 * default. Mirrors parse_tls_connect_call's own two-fixed-argument shape. */
static uint16_t parse_bcrypt_hash_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'BCrypt.hash'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t password_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after BCrypt.hash password");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t cost_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after BCrypt.hash arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BCRYPT_HASH);
    emit_register(compiler,dest);
    emit_register(compiler,password_register);
    emit_register(compiler,cost_register);
    return dest;
}

static uint16_t parse_bcrypt_verify_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'BCrypt.verify'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t password_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,"expected ',' after BCrypt.verify password");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t digest_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after BCrypt.verify arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_BCRYPT_VERIFY);
    emit_register(compiler,dest);
    emit_register(compiler,password_register);
    emit_register(compiler,digest_register);
    return dest;
}

static uint16_t parse_bcrypt_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'hash' or 'verify' after 'BCrypt'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"hash",method,false)) {
        advance_token(compiler);
        return parse_bcrypt_hash_call(compiler);
    }
    if(name_equals(compiler,"verify",method,false)) {
        advance_token(compiler);
        return parse_bcrypt_verify_call(compiler);
    }
    fail(compiler,method,"expected 'hash' or 'verify' after 'BCrypt'");
    return 0;
}

/* SecureRandom.bytes(n) / SecureRandom.hex(n) -- single fixed argument,
 * mirrors parse_process_run_call's own one-argument shape. */
static uint16_t parse_secure_random_bytes_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'SecureRandom.bytes'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t count_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after SecureRandom.bytes arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SECURE_RANDOM_BYTES);
    emit_register(compiler,dest);
    emit_register(compiler,count_register);
    return dest;
}

static uint16_t parse_secure_random_hex_call(Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after 'SecureRandom.hex'");
        return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t count_register=parse_expression(compiler);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,"expected ')' after SecureRandom.hex arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_SECURE_RANDOM_HEX);
    emit_register(compiler,dest);
    emit_register(compiler,count_register);
    return dest;
}

static uint16_t parse_secure_random_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected 'bytes' or 'hex' after 'SecureRandom'");
        return 0;
    }
    const DiamondSpan method=compiler->current.span;
    if(name_equals(compiler,"bytes",method,false)) {
        advance_token(compiler);
        return parse_secure_random_bytes_call(compiler);
    }
    if(name_equals(compiler,"hex",method,false)) {
        advance_token(compiler);
        return parse_secure_random_hex_call(compiler);
    }
    fail(compiler,method,"expected 'bytes' or 'hex' after 'SecureRandom'");
    return 0;
}

static uint16_t parse_sha256_call(Compiler *compiler,bool keyed) {
    const char *owner=keyed?"HMAC.sha256":"Digest.sha256";
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,"expected '(' after sha256");
        return 0;
    }
    advance_token(compiler);skip_newlines(compiler);
    const uint16_t first=parse_expression(compiler);
    uint16_t second=0;
    if(keyed) {
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,"expected ',' after HMAC.sha256 key");
            return 0;
        }
        advance_token(compiler);skip_newlines(compiler);
        second=parse_expression(compiler);
    }
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        (void)owner;
        fail(compiler,compiler->current.span,"expected ')' after sha256 arguments");
        return 0;
    }
    advance_token(compiler);
    const uint16_t dest=allocate_register(compiler);
    emit_opcode(compiler,keyed?DIAMOND_OP_HMAC_SHA256:DIAMOND_OP_DIGEST_SHA256);
    emit_register(compiler,dest);emit_register(compiler,first);
    if(keyed)emit_register(compiler,second);
    compiler->known_types[dest]=DIAMOND_TYPE_STRING;
    return dest;
}

static uint16_t parse_crypto_call(Compiler *compiler,bool keyed) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"sha256",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'sha256' after crypto namespace");
        return 0;
    }
    advance_token(compiler);
    return parse_sha256_call(compiler,keyed);
}

static const DiamondFunction *constructor_signature(const Compiler *compiler,
        int class_index) {
    while(class_index>=0&&(size_t)class_index<compiler->program->class_count) {
        const DiamondClass *class=&compiler->program->classes[(size_t)class_index];
        for(size_t index=0;index<class->method_count;index++) {
            const DiamondMethod *method=&class->methods[index];
            if(strcmp(method->name,"initialize")!=0)continue;
            if(method->function_index>=compiler->program->function_count)
                return nullptr;
            return compiler->program->functions[method->function_index];
        }
        class_index=class->superclass==UINT8_MAX?-1:(int)class->superclass;
    }
    return nullptr;
}

static uint16_t parse_name(Compiler *compiler) {
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
            const uint16_t destination=allocate_register(compiler);
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
        return parse_singleton_call(compiler,method,name,-1);
    }
    const int constant=find_namespace_constant(compiler,name);
    if(constant>=0&&find_local(compiler,name)<0) {
        const uint16_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_NAMESPACE_CONSTANT,destination,
                         (uint8_t)constant,0,2);
        return destination;
    }
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Fiber",name,false))
        return parse_fiber_call(compiler);
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
       name_equals(compiler,"SQLite3",name,false))
        return parse_sqlite3_open_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"PostgreSQL",name,false))
        return parse_postgres_open_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"MySQL",name,false))
        return parse_mysql_open_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Time",name,false))
        return parse_time_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Process",name,false))
        return parse_process_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"BCrypt",name,false))
        return parse_bcrypt_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"SecureRandom",name,false))
        return parse_secure_random_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Digest",name,false))
        return parse_crypto_call(compiler,false);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"HMAC",name,false))
        return parse_crypto_call(compiler,true);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"ProgramBuilder",name,false))
        return parse_program_builder_new_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"Thread",name,false))
        return parse_thread_new_call(compiler);
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
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"TLSSocket",name,false))
        return parse_tls_connect_call(compiler);
    if(class_index<0&&find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       name_equals(compiler,"TLSServer",name,false))
        return parse_tls_listen_call(compiler);
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
       name_equals(compiler,"exit",name,false))
        return parse_exit_call(compiler);
    if(find_local(compiler,name)<0&&find_function(compiler,name)<0&&
       compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN&&
       (name_equals(compiler,"debugger",name,false)||
        name_equals(compiler,"breakpoint",name,false)))
        return parse_debugger_call(compiler);
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
    /* ARGV/ENV -- plain values, not calls, so unlike puts/gets/Time/etc.
     * above there's no `current.kind==LEFT_PAREN` gate: `ARGV` alone is
     * already a complete expression. Still shadowable by a local or
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
    if (class_index >= 0 && compiler->current.kind == DIAMOND_TOKEN_DOT) {
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
            return parse_singleton_call(compiler,method,name,class_index);
        }
        advance_token(compiler);
        const DiamondFunction *initializer=
            constructor_signature(compiler,class_index);
        if (compiler->current.kind != DIAMOND_TOKEN_LEFT_PAREN) {
            fail(compiler, compiler->current.span, "expected '(' after 'new'");
            return 0;
        }
        advance_token(compiler);
        skip_newlines(compiler);
        if(call_arguments_have_keyword(compiler)) {
            DiamondSpan keyword_names[16];uint16_t keyword_values[16];
            size_t keyword_count=0;
            const uint16_t positional=parse_dynamic_keyword_arguments(compiler,
                keyword_names,keyword_values,&keyword_count,initializer);
            bool has_block=false;uint16_t block=0;
            if(compiler->current.kind==DIAMOND_TOKEN_DO) {
                uint16_t inferred_arguments[8];
                infer_contextual_spread_arguments(compiler,initializer,
                    positional,initializer==nullptr||initializer->arity<=1?0:
                        initializer->arity-2,inferred_arguments);
                infer_contextual_keyword_arguments(compiler,initializer,
                    keyword_names,keyword_values,keyword_count,
                    inferred_arguments);
                for(size_t index=0;index<keyword_count;index++) {
                    const uint16_t snapshot=allocate_register(compiler);
                    emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                        keyword_values[index],0,2);
                    keyword_values[index]=snapshot;
                }
                block=compile_contextual_typed_block(compiler,initializer,
                    initializer==nullptr||initializer->arity<=1?0:
                        initializer->arity-2,inferred_arguments,
                    initializer==nullptr?0:initializer->type_variable_count);
                has_block=true;
            }
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_NEW_KEYWORDS);
            emit_register(compiler,destination);
            emit_byte(compiler,(uint8_t)class_index);
            emit_register(compiler,positional);
            emit_byte(compiler,(uint8_t)(keyword_count|(has_block?0x80u:0u)));
            for(size_t index=0;index<keyword_count;index++) {
                emit_register(compiler,add_name_string(compiler,keyword_names[index]));
                emit_register(compiler,keyword_values[index]);
            }
            if(has_block)emit_register(compiler,block);
            compiler->known_types[destination]=
                (uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
            return destination;
        }
        if(call_arguments_have_spread(compiler)) {
            uint16_t spread=parse_spread_argument_array(compiler,nullptr,
                nullptr,nullptr,nullptr,initializer,
                initializer==nullptr||initializer->arity==0?0:
                    initializer->arity-1);
            if(compiler->current.kind==DIAMOND_TOKEN_DO) {
                uint16_t inferred_arguments[8];
                const size_t inferred_count=infer_contextual_spread_arguments(
                    compiler,initializer,spread,
                    initializer==nullptr||initializer->arity<=1?0:
                        initializer->arity-2,inferred_arguments);
                const uint16_t spread_snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
                    spread,0,2);
                spread=spread_snapshot;
                const uint16_t block=compile_contextual_typed_block(compiler,
                    initializer,
                    initializer==nullptr||initializer->arity<=1?0:
                        initializer->arity-2,inferred_arguments,
                    inferred_count);
                const uint16_t destination=allocate_register(compiler);
                emit_opcode(compiler,DIAMOND_OP_NEW_KEYWORDS);
                emit_register(compiler,destination);
                emit_byte(compiler,(uint8_t)class_index);
                emit_register(compiler,spread);emit_byte(compiler,0x80u);
                emit_register(compiler,block);
                compiler->known_types[destination]=
                    (uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
                return destination;
            }
            const uint16_t destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_NEW_SPREAD);
            emit_register(compiler,destination);
            emit_byte(compiler,(uint8_t)class_index);
            emit_register(compiler,spread);
            compiler->known_types[destination]=
                (uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
            return destination;
        }
        uint16_t args[16]; size_t count = 0;
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
        if(compiler->current.kind==DIAMOND_TOKEN_DO) {
            if(count==16) {
                fail(compiler,compiler->current.span,"too many arguments");return 0;
            }
            uint16_t inferred_arguments[8];
            const size_t inferred_count=infer_contextual_type_arguments(
                compiler,initializer,args,count,inferred_arguments);
            for(size_t index=0;index<count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,args[index],0,2);
                args[index]=snapshot;
            }
            const uint16_t block=compile_contextual_typed_block(compiler,
                initializer,
                initializer==nullptr||initializer->arity<=1?0:
                    initializer->arity-2,inferred_arguments,inferred_count);
            const size_t block_slot=initializer==nullptr||initializer->arity<=1?
                0:(size_t)initializer->arity-2;
            if(count<block_slot) {
                const uint16_t positional=emit_argument_array(compiler,args,count);
                const uint16_t destination=allocate_register(compiler);
                emit_opcode(compiler,DIAMOND_OP_NEW_KEYWORDS);
                emit_register(compiler,destination);
                emit_byte(compiler,(uint8_t)class_index);
                emit_register(compiler,positional);emit_byte(compiler,0x80u);
                emit_register(compiler,block);
                compiler->known_types[destination]=
                    (uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
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
        emit_byte(compiler, (uint8_t)class_index); emit_register(compiler,base);
        emit_byte(compiler, (uint8_t)count);
        compiler->known_types[dest]=(uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
        return dest;
    }
    if (compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN||
        (compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET&&
         find_local(compiler,name)<0&&find_function(compiler,name)>=0)) {
        return parse_call(compiler, name);
    }
    /* During diamond_compile's own discovery pass only: `class_index<0`
     * here means this identifier isn't a known class (or module, or any
     * of the built-in Fiber/File/SQLite3/etc. names already ruled out
     * above) *yet* -- for a genuine forward reference (the whole reason
     * discovery exists, see that function's own comment), the class
     * declaration just hasn't been reached by this pass's own walk. A
     * capitalized name immediately followed by '.' is exactly the shape
     * `SomeClass.new(...)`/`SomeClass.someMethod(...)` -- rather than
     * re-deriving how to parse either of those forms here, substitute a
     * harmless NIL for the (as far as this pass knows) unresolved name
     * and let this expression's own postfix-chain loop (parse_precedence)
     * consume the following '.method(...)' exactly the way it already
     * does for any other receiver, via parse_invoke -- discovery's own
     * bytecode is discarded regardless of what it computes here. If the
     * name is genuinely undefined (a real typo, not a forward reference),
     * this pass simply won't record that -- the second, real pass has no
     * such tolerance and will correctly fail on it there instead. */
    if(compiler->discovery_pass&&class_index<0&&module_index<0&&
       compiler->current.kind==DIAMOND_TOKEN_DOT&&
       compiler->source[name.start]>='A'&&compiler->source[name.start]<='Z') {
        const uint16_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
        return destination;
    }
    return parse_identifier(compiler);
}

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
static uint16_t emit_invoke_call(Compiler *compiler, uint16_t receiver,
        DiamondSpan method_name, bool writer_name,
        const uint16_t *type_arguments, size_t type_argument_count,
        const uint16_t *args, size_t count) {
    const uint16_t base = allocate_register(compiler);
    for (size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for (size_t i=0;i<count;i++) emit_instruction(compiler, DIAMOND_OP_MOVE,
        (uint16_t)(base+i), args[i], 0, 2);
    const uint16_t dest=allocate_register(compiler);
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
    emit_opcode(compiler,type_argument_count==0?
        DIAMOND_OP_INVOKE:DIAMOND_OP_INVOKE_TYPED);emit_register(compiler,dest);
    emit_register(compiler,receiver);emit_register(compiler,method);emit_register(compiler,base);
    emit_byte(compiler,(uint8_t)count);
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
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
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    return destination;
}

static uint16_t emit_invoke_spread(Compiler *compiler,uint16_t receiver,
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
    if(type_argument_count>0) {
        emit_byte(compiler,(uint8_t)type_argument_count);
        for(size_t index=0;index<type_argument_count;index++)
            emit_register(compiler,type_arguments[index]);
    }
    return destination;
}

static uint16_t emit_build_spread_arguments(Compiler *compiler,
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
static const DiamondFunction *class_instance_signature(
        const Compiler *compiler,uint8_t type,DiamondSpan name) {
    size_t class_index=(size_t)(type-DIAMOND_TYPE_CLASS_BASE);
    while(class_index<compiler->program->class_count) {
        const DiamondClass *class=&compiler->program->classes[class_index];
        for(size_t index=0;index<class->method_count;index++) {
            const DiamondMethod *method=&class->methods[index];
            if(!singleton_call_name_equals(compiler,method->name,name))continue;
            if(method->function_index>=compiler->program->function_count)
                return nullptr;
            return compiler->program->functions[method->function_index];
        }
        if(class->superclass==UINT8_MAX)break;
        class_index=class->superclass;
    }
    return nullptr;
}

static bool type_sets_structurally_equal(const DiamondTypeSet *left_sets,
        size_t left_count,uint16_t left_index,
        const DiamondTypeSet *right_sets,size_t right_count,
        uint16_t right_index,size_t depth);

static bool type_members_structurally_equal(const DiamondTypeMember *left,
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

static bool type_sets_structurally_equal(const DiamondTypeSet *left_sets,
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
       left->type_variable_count!=right->type_variable_count)return false;
    for(size_t parameter=0;parameter<left->arity&&parameter<16;parameter++) {
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

static const DiamondFunction *instance_call_signature(
        const Compiler *compiler,uint16_t receiver,DiamondSpan name,
        bool require_matching_return) {
    const uint8_t type=compiler->known_types[receiver];
    if(type>=DIAMOND_TYPE_CLASS_BASE&&type<DIAMOND_TYPE_INTERFACE_BASE)
        return class_instance_signature(compiler,type,name);
    const int32_t set_index=compiler->known_type_sets[receiver];
    if(set_index<0||(size_t)set_index>=compiler->function->type_set_count)
        return nullptr;
    const DiamondTypeSet *set=
        &compiler->function->type_sets[(size_t)set_index];
    const DiamondFunction *shared=nullptr;
    if(set->count==0)return nullptr;
    for(size_t index=0;index<set->count;index++) {
        const uint8_t member=set->members[index].id;
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
        size_t binding_count) {
    if(receiver_set_index<0||
       (size_t)receiver_set_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet *receiver_set=
        &compiler->function->type_sets[(size_t)receiver_set_index];
    if(receiver_set->count<2)return;
    uint8_t members[DIAMOND_MAX_UNION_TYPES];
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
        if(target==nullptr||target->return_type_set==DIAMOND_NO_TYPE_SET) {
            compiler->function->type_set_count=original_count;return;
        }
        uint16_t current;
        if(target->type_variable_count>0) {
            bool resolved=true;
            current=clone_substituted_type_set(compiler,target,
                target->return_type_set,bindings,binding_count,&resolved);
            if(!resolved) {
                compiler->function->type_set_count=original_count;return;
            }
        } else current=clone_type_set_into_current(compiler,target->type_sets,
            target->type_set_count,target->return_type_set);
        if(current==DIAMOND_NO_TYPE_SET) {
            compiler->function->type_set_count=original_count;return;
        }
        joined=joined<0?(int32_t)current:
            join_type_set_indices(compiler,joined,(int32_t)current);
        if(joined<0) {
            compiler->function->type_set_count=original_count;return;
        }
    }
    publish_known_type_set(compiler,reg,(uint16_t)joined);
}

/* Native Array/Hash extension dispatch is resolved by the VM rather than an
 * instance signature, so its result cannot use the declared-return path above.
 * Preserve the receiver's nested collection graphs for the small family whose
 * result is structurally determined without inspecting arguments or blocks. */
static void publish_collection_method_return_type(Compiler *compiler,
        uint16_t reg,int32_t receiver_set_index,DiamondSpan name) {
    if(receiver_set_index<0||
       (size_t)receiver_set_index>=compiler->function->type_set_count)return;
    const DiamondTypeSet receiver=
        compiler->function->type_sets[(size_t)receiver_set_index];
    if(receiver.count==0)return;
    uint8_t collection_type=TYPE_UNKNOWN;
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
        if(name_equals(compiler,"first",name,false)||
           name_equals(compiler,"last",name,false)) {
            publish_known_type_set(compiler,reg,(uint16_t)first);return;
        }
        if(name_equals(compiler,"reverse",name,false)||
           name_equals(compiler,"uniq",name,false)||
           name_equals(compiler,"compact",name,false)||
           name_equals(compiler,"sort",name,false)||
           name_equals(compiler,"sort_by",name,false)||
           name_equals(compiler,"select",name,false)||
           name_equals(compiler,"reject",name,false)||
           name_equals(compiler,"take",name,false)||
           name_equals(compiler,"drop",name,false))
            record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,
                first,-1);
        return;
    }
    if(name_equals(compiler,"keys",name,false))
        record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,first,-1);
    else if(name_equals(compiler,"values",name,false))
        record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,second,-1);
}

static int32_t joined_collection_argument(Compiler *compiler,
        int32_t collection_set_index,uint8_t collection_type,bool second) {
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

static void publish_collection_argument_return_type(Compiler *compiler,
        uint16_t reg,int32_t receiver_set_index,DiamondSpan name,
        const uint16_t *arguments,size_t argument_count) {
    const int32_t array_elements=joined_collection_argument(compiler,
        receiver_set_index,DIAMOND_TYPE_ARRAY,false);
    if(array_elements>=0) {
        if(argument_count==1&&
           (name_equals(compiler,"first_or",name,false)||
            name_equals(compiler,"last_or",name,false))) {
            const int32_t fallback=joined_value_type_set(compiler,arguments,1);
            const int32_t joined=join_type_set_indices(compiler,array_elements,
                fallback);
            if(joined>=0)publish_known_type_set(compiler,reg,(uint16_t)joined);
        } else if(argument_count==1&&
                  name_equals(compiler,"concat",name,false)) {
            const int32_t other=joined_collection_argument(compiler,
                compiler->known_type_sets[arguments[0]],DIAMOND_TYPE_ARRAY,
                false);
            const int32_t joined=join_type_set_indices(compiler,array_elements,
                other);
            if(joined>=0)record_collection_type_set(compiler,reg,
                DIAMOND_TYPE_ARRAY,joined,-1);
        } else if(argument_count==1&&
                  name_equals(compiler,"map",name,false)) {
            const int32_t mapped=joined_callable_return(compiler,arguments[0]);
            if(mapped>=0)record_collection_type_set(compiler,reg,
                DIAMOND_TYPE_ARRAY,mapped,-1);
        } else if(argument_count==1&&
                  name_equals(compiler,"flat_map",name,false)) {
            const int32_t mapped=joined_callable_return(compiler,arguments[0]);
            const int32_t element=joined_collection_argument(compiler,mapped,
                DIAMOND_TYPE_ARRAY,false);
            if(element>=0)record_collection_type_set(compiler,reg,
                DIAMOND_TYPE_ARRAY,element,-1);
        } else if(argument_count==1&&
                  (name_equals(compiler,"partition",name,false)||
                   name_equals(compiler,"each_slice",name,false)||
                   name_equals(compiler,"each_cons",name,false))) {
            publish_nested_array_result(compiler,reg,array_elements);
        } else if(argument_count==1&&
                  name_equals(compiler,"group_by",name,false)) {
            const int32_t key=joined_callable_return(compiler,arguments[0]);
            if(key>=0) {
                record_collection_type_set(compiler,reg,DIAMOND_TYPE_ARRAY,
                    array_elements,-1);
                const int32_t grouped=compiler->known_type_sets[reg];
                if(grouped>=0)record_collection_type_set(compiler,reg,
                    DIAMOND_TYPE_HASH,key,grouped);
            }
        } else if(argument_count==1&&
                  name_equals(compiler,"zip",name,false)) {
            const int32_t other=joined_collection_argument(compiler,
                compiler->known_type_sets[arguments[0]],DIAMOND_TYPE_ARRAY,
                false);
            const int32_t pair=join_type_set_indices(compiler,array_elements,
                other);
            if(pair>=0)publish_nested_array_result(compiler,reg,pair);
        } else if(argument_count==0&&
                  name_equals(compiler,"tally",name,false)) {
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
    if(argument_count==2&&name_equals(compiler,"fetch",name,false)) {
        const int32_t fallback=joined_value_type_set(compiler,&arguments[1],1);
        const int32_t joined=join_type_set_indices(compiler,hash_values,fallback);
        if(joined>=0)publish_known_type_set(compiler,reg,(uint16_t)joined);
    } else if(argument_count==1&&name_equals(compiler,"merge",name,false)) {
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
    } else if(argument_count==1&&
              name_equals(compiler,"map_values",name,false)) {
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
    const char *wanted[2]={nullptr,nullptr};size_t wanted_count=0;
    if(name_equals(compiler,"first_or",method_name,false)||
       name_equals(compiler,"last_or",method_name,false)) {
        wanted[0]="fallback";wanted_count=1;
    } else if(name_equals(compiler,"concat",method_name,false)) {
        wanted[0]="other";wanted_count=1;
    } else if(name_equals(compiler,"merge",method_name,false)) {
        wanted[0]="other";wanted_count=1;
    } else if(name_equals(compiler,"fetch",method_name,false)) {
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

static void publish_instance_return_type(Compiler *compiler,uint16_t reg,
        int32_t receiver_set_index,DiamondSpan name,
        const DiamondFunction *matching_target,const uint16_t *bindings,
        size_t binding_count) {
    if(matching_target!=nullptr)
        publish_declared_return_type(compiler,reg,matching_target,bindings,
            binding_count);
    else publish_union_instance_return_type(compiler,reg,receiver_set_index,
        name,bindings,binding_count);
    publish_collection_method_return_type(compiler,reg,receiver_set_index,name);
}

/* `receiver.method` -- a capturing, variadic Callable whose single captured
 * value is the receiver and whose collected positional arguments are forwarded
 * through the ordinary dynamic INVOKE_SPREAD matrix. */
static uint16_t parse_bound_method_reference(Compiler *compiler,uint16_t receiver,
        DiamondSpan method_name,const uint16_t *type_arguments,
        size_t type_argument_count) {
    const DiamondFunction *target=
        instance_call_signature(compiler,receiver,method_name,true);
    uint16_t inferred_arguments[8];
    if(target!=nullptr&&target->type_variable_count>0&&
       type_argument_count==0&&infer_reference_type_arguments(compiler,target,
           0,target->arity>0?target->arity-1:0,inferred_arguments)) {
        type_arguments=inferred_arguments;
        type_argument_count=target->type_variable_count;
    }
    const bool typed_wrapper=target!=nullptr&&target->arity>0&&
        (target->type_variable_count==0||
         type_argument_count==target->type_variable_count);
    uint16_t parameter_sets[16];
    for(size_t index=0;index<16;index++)
        parameter_sets[index]=DIAMOND_NO_TYPE_SET;
    uint16_t return_set=DIAMOND_NO_TYPE_SET;
    if(typed_wrapper) {
        bool resolved=true;
        const size_t original_count=compiler->function->type_set_count;
        for(size_t parameter=0;parameter+1<target->arity&&parameter<16;
            parameter++) {
            const uint16_t source=target->parameter_type_sets[parameter];
            if(source==DIAMOND_NO_TYPE_SET)continue;
            parameter_sets[parameter]=target->type_variable_count>0?
                clone_substituted_type_set(compiler,target,source,
                    type_arguments,type_argument_count,&resolved):
                clone_type_set_into_current(compiler,target->type_sets,
                    target->type_set_count,source);
        }
        if(target->return_type_set!=DIAMOND_NO_TYPE_SET)
            return_set=target->type_variable_count>0?
                clone_substituted_type_set(compiler,target,
                    target->return_type_set,type_arguments,type_argument_count,
                    &resolved):clone_type_set_into_current(compiler,
                    target->type_sets,target->type_set_count,
                    target->return_type_set);
        if(!resolved) {
            compiler->function->type_set_count=original_count;
            return_set=DIAMOND_NO_TYPE_SET;
            for(size_t index=0;index<16;index++)
                parameter_sets[index]=DIAMOND_NO_TYPE_SET;
        }
    }
    size_t function_index=0;
    DiamondFunction *wrapper=compiler_add_function(compiler,&function_index);
    if(wrapper==nullptr) {fail(compiler,method_name,"out of memory");return 0;}
    (void)snprintf(wrapper->name,sizeof wrapper->name,"<bound method>");
    wrapper->owner_class=UINT8_MAX;wrapper->nested=true;
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
    for(size_t index=0;index<16;index++) {
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
    uint8_t *outer_types=malloc(outer_next_register*sizeof *outer_types);
    int32_t *outer_type_sets=malloc(outer_next_register*sizeof *outer_type_sets);
    if((outer_types==nullptr||outer_type_sets==nullptr)&&outer_next_register>0) {
        free(outer_types);free(outer_type_sets);
        fail(compiler,method_name,"out of memory");return 0;
    }
    memcpy(outer_types,compiler->known_types,outer_next_register);
    memcpy(outer_type_sets,compiler->known_type_sets,
        outer_next_register*sizeof *outer_type_sets);
    compiler->function=wrapper;compiler->next_register=0;
    compiler->local_count=0;compiler->in_function=true;
    uint16_t arguments[16];
    const size_t argument_count=typed_wrapper?wrapper->arity:1;
    for(size_t index=0;index<argument_count;index++)
        arguments[index]=allocate_register(compiler);
    if(wrapper->has_variadic) {
        const size_t fixed_count=wrapper->arity-1;
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            arguments[fixed_count],(uint16_t)fixed_count,0,3);
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
    emit_instruction(compiler,DIAMOND_OP_RETURN,body,0,0,1);
    wrapper->register_count=compiler->next_register;

    compiler->function=outer_function;compiler->next_register=outer_next_register;
    compiler->local_count=outer_local_count;compiler->in_function=outer_in_function;
    memcpy(compiler->known_types,outer_types,outer_next_register);
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

static uint16_t parse_invoke(Compiler *compiler, uint16_t receiver) {
    const int32_t receiver_set_index=compiler->known_type_sets[receiver];
    advance_token(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler, compiler->current.span, "expected method name after '.'"); return 0;
    }
    const DiamondSpan name = compiler->current.span;
    const DiamondFunction *contextual_target=
        instance_call_signature(compiler,receiver,name,false);
    const DiamondFunction *return_target=
        instance_call_signature(compiler,receiver,name,true);
    advance_token(compiler);
    bool writer_name=false;
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        writer_name=true;advance_token(compiler);
    }
    uint16_t type_arguments[8];size_t type_argument_count=0;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET &&
       (!writer_name||writer_generic_arguments_ahead(compiler))) {
        advance_token(compiler);
        skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&
              !compiler->failed) {
            if(type_argument_count==8) {
                fail(compiler,compiler->current.span,"too many generic arguments");
                return 0;
            }
            type_arguments[type_argument_count++]=
                (uint16_t)parse_type_annotation(compiler);
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
    if(call_arguments_have_keyword(compiler)) {
        if(writer_name) {fail(compiler,compiler->current.span,
            "keyword method calls do not support writers");return 0;}
        DiamondSpan keyword_names[16];uint16_t keyword_values[16];
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
            const uint16_t receiver_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,receiver_snapshot,
                receiver,0,2);receiver=receiver_snapshot;
            for(size_t index=0;index<keyword_count;index++) {
                const uint16_t snapshot=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,
                    keyword_values[index],0,2);
                keyword_values[index]=snapshot;
            }
            block=compile_contextual_typed_block(compiler,contextual_target,
                contextual_target==nullptr||contextual_target->arity<=1?0:
                    contextual_target->arity-2,resolved_arguments,
                resolved_count);
            has_block=true;
        }
        const uint16_t result=emit_invoke_keywords(compiler,receiver,name,positional,
            keyword_names,keyword_values,keyword_count,type_arguments,
            type_argument_count,has_block,block);
        publish_instance_return_type(compiler,result,receiver_set_index,name,
            return_target,resolved_arguments,resolved_count);
        publish_collection_keyword_return_type(compiler,result,
            receiver_set_index,name,keyword_names,keyword_values,keyword_count);
        return result;
    }
    if(call_arguments_have_spread(compiler)) {
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
            const uint16_t receiver_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,receiver_snapshot,
                receiver,0,2);
            receiver=receiver_snapshot;
            const uint16_t spread_snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,spread_snapshot,
                spread,0,2);
            spread=spread_snapshot;
            const uint16_t block=compile_contextual_typed_block(compiler,
                contextual_target,
                    contextual_target==nullptr||contextual_target->arity<=1?0:
                    contextual_target->arity-2,resolved_arguments,
                resolved_count);
            if(contextual_target!=nullptr) {
                const uint16_t result=emit_invoke_keywords(compiler,receiver,name,spread,
                    nullptr,nullptr,0,type_arguments,type_argument_count,true,
                    block);
                publish_instance_return_type(compiler,result,
                    receiver_set_index,name,return_target,resolved_arguments,
                    resolved_count);
                return result;
            }
            spread=emit_build_spread_arguments(compiler,nullptr,0,spread,
                &block,1,false);
        }
        const uint16_t result=emit_invoke_typed_spread(compiler,receiver,name,
            spread,type_arguments,type_argument_count);
        publish_instance_return_type(compiler,result,receiver_set_index,name,
            return_target,resolved_arguments,resolved_count);
        return result;
    }
    uint16_t args[16]; size_t count = 0;
    while (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN && !compiler->failed) {
        if (count == 16) { fail(compiler, compiler->current.span, "too many arguments"); break; }
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
        if(count==16) {
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
        const uint16_t receiver_snapshot=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,receiver_snapshot,receiver,0,2);
        receiver=receiver_snapshot;
        for(size_t i=0;i<count;i++) {
            const uint16_t snapshot=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,snapshot,args[i],0,2);
            args[i]=snapshot;
        }
        const uint16_t block=compile_contextual_typed_block(compiler,
            contextual_target,
            contextual_target==nullptr||contextual_target->arity<=1?0:
                contextual_target->arity-2,resolved_arguments,
            resolved_count);
        const size_t block_slot=
            contextual_target==nullptr||contextual_target->arity<=1?0:
                (size_t)contextual_target->arity-2;
        if(contextual_target!=nullptr&&count<block_slot) {
            const uint16_t positional=emit_argument_array(compiler,args,count);
            const uint16_t result=emit_invoke_keywords(compiler,receiver,name,positional,
                nullptr,nullptr,0,type_arguments,type_argument_count,true,block);
            publish_instance_return_type(compiler,result,receiver_set_index,
                name,return_target,resolved_arguments,resolved_count);
            return result;
        }
        args[count++]=block;
    }
    const uint16_t result=emit_invoke_call(compiler,receiver,name,writer_name,
        type_arguments,type_argument_count,args,count);
    publish_instance_return_type(compiler,result,receiver_set_index,name,
        return_target,resolved_arguments,resolved_count);
    publish_collection_argument_return_type(compiler,result,receiver_set_index,
        name,args,count);
    return result;
}

/* `self.method_name(...)` written inside a class-owned singleton method's
 * own body -- `self` (register 0) already holds a real DIAMOND_VALUE_CLASS
 * value there (see compile_definition's direct_class_singleton_member
 * branch), but unlike an ordinary instance method's self.foo() (which is
 * just ordinary parse_invoke against register 0 as any other receiver),
 * the target here genuinely can't be resolved at compile time: this
 * method body is compiled once, but self may hold a *different* class at
 * each call (an inherited method reached via a subclass). Emits a
 * dedicated opcode that resolves by name against self's actual
 * class_index at runtime instead -- see DIAMOND_OP_INVOKE_SELF_METHOD's
 * own comment in vm.h. Mirrors parse_invoke's argument-marshaling shape
 * exactly, minus generic type arguments (not needed for this first
 * pass -- self.foo[T](...) isn't supported). */
static uint16_t parse_self_class_method_call(Compiler *compiler) {
    advance_token(compiler); /* consume '.' */
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected method name after 'self.'");
        return 0;
    }
    const DiamondSpan name=compiler->current.span;
    advance_token(compiler);
    bool writer_name=false;
    if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
        writer_name=true;advance_token(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             "member access requires a method call with '()'");return 0;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    uint16_t args[16];size_t count=0;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN&&!compiler->failed) {
        if(count==16) {fail(compiler,compiler->current.span,"too many arguments");break;}
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
        if(count==16) {fail(compiler,compiler->current.span,"too many arguments");return 0;}
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

static uint16_t parse_super(Compiler *compiler) {
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
    uint16_t arguments[16];
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
    emit_byte(compiler, (uint8_t)compiler->current_class);
    emit_register(compiler,method);
    emit_register(compiler,base);
    emit_byte(compiler, (uint8_t)count);
    return destination;
}

static uint16_t parse_grouping(Compiler *compiler) {
    const uint16_t result = parse_expression(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after expression");
        return result;
    }
    advance_token(compiler);
    return result;
}

static uint16_t parse_with_expected_set(Compiler *compiler,
        uint16_t expected_set) {
    const int32_t outer=compiler->expected_expression_type_set;
    compiler->expected_expression_type_set=expected_set==DIAMOND_NO_TYPE_SET?
        -1:(int32_t)expected_set;
    const uint16_t result=parse_expression(compiler);
    compiler->expected_expression_type_set=outer;
    return result;
}

static uint16_t parse_array_literal(Compiler *compiler,
        const DiamondFunction *expected_function,size_t first_parameter,
        size_t parameter_count) {
    uint16_t elements[32];
    size_t count=0;
    uint16_t expected_element=DIAMOND_NO_TYPE_SET;
    if(compiler->expected_expression_type_set>=0&&
       (size_t)compiler->expected_expression_type_set<
           compiler->function->type_set_count) {
        const DiamondTypeSet *expected=&compiler->function->type_sets[
            (size_t)compiler->expected_expression_type_set];
        if(expected->count==1&&expected->members[0].id==DIAMOND_TYPE_ARRAY)
            expected_element=expected->members[0].argument_set;
    }
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        do {
            if(count==32) {
                fail(compiler,compiler->current.span,"array literal has too many elements");
                return 0;
            }
            if(expected_function!=nullptr&&
               first_parameter+count<parameter_count)
                elements[count]=parse_expected_argument(compiler,
                    expected_function,first_parameter+count);
            else elements[count]=parse_with_expected_set(compiler,
                expected_element);
            count++;
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
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) emit_instruction(compiler,DIAMOND_OP_MOVE,
        (uint16_t)(base+i),elements[i],0,2);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_ARRAY,destination,base,(uint8_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_ARRAY;
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_ARRAY,
        joined_value_type_set(compiler,elements,count),-1);
    if(expected_function!=nullptr) {
        compiler->positional_spread_literal=true;
        compiler->positional_spread_first=first_parameter;
        compiler->positional_spread_count=count;
        for(size_t index=0;index<count;index++)
            compiler->positional_spread_elements[index]=elements[index];
    }
    return destination;
}

static uint16_t parse_array(Compiler *compiler) {
    return parse_array_literal(compiler,nullptr,0,0);
}

static uint16_t parse_hash(Compiler *compiler) {
    uint16_t keys[16],values[16];
    size_t count=0;
    uint16_t expected_key=DIAMOND_NO_TYPE_SET;
    uint16_t expected_value=DIAMOND_NO_TYPE_SET;
    if(compiler->expected_expression_type_set>=0&&
       (size_t)compiler->expected_expression_type_set<
           compiler->function->type_set_count) {
        const DiamondTypeSet *expected=&compiler->function->type_sets[
            (size_t)compiler->expected_expression_type_set];
        if(expected->count==1&&expected->members[0].id==DIAMOND_TYPE_HASH) {
            expected_key=expected->members[0].argument_set;
            expected_value=expected->members[0].second_argument_set;
        }
    }
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
        do {
            if(count==16) {
                fail(compiler,compiler->current.span,"hash literal has too many entries");
                return 0;
            }
            keys[count]=parse_with_expected_set(compiler,expected_key);
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                fail(compiler,compiler->current.span,"expected ':' after hash key");
                return 0;
            }
            advance_token(compiler);
            values[count]=parse_with_expected_set(compiler,expected_value);
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
    const uint16_t base=allocate_register(compiler);
    for(size_t i=1;i<count*2;i++) (void)allocate_register(compiler);
    for(size_t i=0;i<count;i++) {
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2),keys[i],0,2);
        emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+i*2+1),values[i],0,2);
    }
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_HASH,destination,base,(uint8_t)count,3);
    compiler->known_types[destination]=DIAMOND_TYPE_HASH;
    record_collection_type_set(compiler,destination,DIAMOND_TYPE_HASH,
        joined_value_type_set(compiler,keys,count),
        joined_value_type_set(compiler,values,count));
    return destination;
}

static int32_t type_set_with_nil(Compiler *compiler,uint16_t source_index) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    for(size_t index=0;index<source.count;index++)
        if(source.members[index].id==DIAMOND_TYPE_NIL)return (int32_t)source_index;
    if(source.count==DIAMOND_MAX_UNION_TYPES||
       !grow_type_sets(compiler->function,1))return -1;
    const size_t result=compiler->function->type_set_count++;
    compiler->function->type_sets[result]=source;
    DiamondTypeSet *set=&compiler->function->type_sets[result];
    set->members[set->count++]=(DiamondTypeMember){
        .id=DIAMOND_TYPE_NIL,.argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,.callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET};
    return (int32_t)result;
}

static bool split_nil_type_set(Compiler *compiler,uint16_t source_index,
                               int32_t *without_nil,int32_t *only_nil) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet narrowed={};bool found_nil=false;
    for(size_t index=0;index<source.count;index++) {
        if(source.members[index].id==DIAMOND_TYPE_NIL)found_nil=true;
        else narrowed.members[narrowed.count++]=source.members[index];
    }
    if(!found_nil||narrowed.count==0||
       !grow_type_sets(compiler->function,2))return false;
    *without_nil=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=narrowed;
    *only_nil=(int32_t)compiler->function->type_set_count;
    DiamondTypeSet *nil_set=&compiler->function->type_sets[
        compiler->function->type_set_count++];
    *nil_set=(DiamondTypeSet){.members={{.id=DIAMOND_TYPE_NIL,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET}},.count=1};
    return true;
}

static bool split_type_set(Compiler *compiler,uint16_t source_index,
                           uint8_t tested_type,int32_t *matching,
                           int32_t *remaining) {
    const DiamondTypeSet source=compiler->function->type_sets[source_index];
    DiamondTypeSet yes={},no={};
    for(size_t index=0;index<source.count;index++) {
        const DiamondTypeMember member=source.members[index];
        if(known_type_satisfies_one(compiler,member.id,tested_type))
            yes.members[yes.count++]=member;
        else no.members[no.count++]=member;
    }
    if(yes.count==0||no.count==0||
       !grow_type_sets(compiler->function,2))return false;
    *matching=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=yes;
    *remaining=(int32_t)compiler->function->type_set_count;
    compiler->function->type_sets[compiler->function->type_set_count++]=no;
    return true;
}

static void apply_type_set_fact(Compiler *compiler,uint16_t reg,int32_t set_index) {
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

static bool type_members_equal(DiamondTypeMember left,DiamondTypeMember right) {
    if(left.id!=right.id||left.argument_set!=right.argument_set||
       left.second_argument_set!=right.second_argument_set||
       left.callable_arity!=right.callable_arity||
       left.callable_return_set!=right.callable_return_set||
       left.callable_parameters_typed!=right.callable_parameters_typed)return false;
    if(!left.callable_parameters_typed)return true;
    for(size_t index=0;index<left.callable_arity;index++)
        if(left.callable_parameter_sets[index]!=right.callable_parameter_sets[index])return false;
    return true;
}

static bool type_sets_equal_unordered(DiamondTypeSet left,DiamondTypeSet right) {
    if(left.count!=right.count)return false;
    for(size_t left_index=0;left_index<left.count;left_index++) {
        bool found=false;
        for(size_t right_index=0;right_index<right.count;right_index++)
            if(type_members_equal(left.members[left_index],right.members[right_index])) {
                found=true;break;
            }
        if(!found)return false;
    }
    return true;
}

static DiamondTypeMember plain_type_member(uint8_t type) {
    return (DiamondTypeMember){.id=type,.argument_set=DIAMOND_NO_TYPE_SET,
        .second_argument_set=DIAMOND_NO_TYPE_SET,.callable_arity=UINT8_MAX,
        .callable_return_set=DIAMOND_NO_TYPE_SET};
}

static bool append_merged_member(DiamondTypeSet *merged,DiamondTypeMember member) {
    for(size_t index=0;index<merged->count;index++) {
        if(type_members_equal(merged->members[index],member))return true;
    }
    if(merged->count==DIAMOND_MAX_UNION_TYPES)return false;
    merged->members[merged->count++]=member;return true;
}

static bool append_flow_type(const Compiler *compiler,DiamondTypeSet *merged,
        uint8_t known_type,int32_t known_set) {
    if(known_set>=0) {
        if((size_t)known_set>=compiler->function->type_set_count)return false;
        const DiamondTypeSet set=compiler->function->type_sets[(size_t)known_set];
        for(size_t index=0;index<set.count;index++)
            if(!append_merged_member(merged,set.members[index]))return false;
        return set.count>0;
    }
    return known_type!=TYPE_UNKNOWN&&
        append_merged_member(merged,plain_type_member(known_type));
}

/* Conservatively joins two control-flow type states. Unlike runtime type
 * annotations this metadata is advisory: an unrepresentable or exhausted
 * union simply becomes unknown and must never make compilation fail. */
static void merge_flow_types(Compiler *compiler,uint8_t left_type,int32_t left_set,
        uint8_t right_type,int32_t right_set,uint8_t *result_type,int32_t *result_set) {
    *result_type=TYPE_UNKNOWN;*result_set=-1;
    if(left_type==right_type&&left_set==right_set) {
        *result_type=left_type;*result_set=left_set;return;
    }
    DiamondTypeSet merged={};
    if(!append_flow_type(compiler,&merged,left_type,left_set)||
       !append_flow_type(compiler,&merged,right_type,right_set))return;
    merged.inferred=true;
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        if(!compiler->function->type_sets[index].inferred)continue;
        if(!type_sets_equal_unordered(merged,compiler->function->type_sets[index]))continue;
        *result_set=(int32_t)index;
        *result_type=merged.count==1?merged.members[0].id:TYPE_UNKNOWN;
        return;
    }
    if(!grow_type_sets(compiler->function,1))return;
    const size_t index=compiler->function->type_set_count++;
    compiler->function->type_sets[index]=merged;
    *result_set=(int32_t)index;
    *result_type=merged.count==1?merged.members[0].id:TYPE_UNKNOWN;
}

static bool register_is_local(const Compiler *compiler,uint16_t reg) {
    for(size_t index=0;index<compiler->local_count;index++)
        if(compiler->locals[index].reg==reg)return true;
    return false;
}

static void merge_loop_exit(Compiler *compiler,LoopContext *loop,
        uint8_t result_type,int32_t result_set) {
    if(!loop->exit_initialized) {
        for(size_t index=0;index<loop->flow_reg_count;index++) {
            loop->exit_types[index]=compiler->known_types[index];
            loop->exit_sets[index]=compiler->known_type_sets[index];
        }
        loop->result_type=result_type;loop->result_set=result_set;
        loop->exit_initialized=true;return;
    }
    for(size_t index=0;index<loop->flow_reg_count;index++)
        merge_flow_types(compiler,loop->exit_types[index],loop->exit_sets[index],
            compiler->known_types[index],compiler->known_type_sets[index],
            &loop->exit_types[index],&loop->exit_sets[index]);
    merge_flow_types(compiler,loop->result_type,loop->result_set,
        result_type,result_set,&loop->result_type,&loop->result_set);
}

static void finish_loop_flow(Compiler *compiler,LoopContext *loop,
        size_t effective_start) {
    if(!loop->exit_initialized)return;
    for(size_t index=0;index<loop->flow_reg_count;index++) {
        compiler->known_types[index]=loop->exit_types[index];
        compiler->known_type_sets[index]=loop->exit_sets[index];
        if(register_is_local(compiler,(uint16_t)index))
            record_scope_type_fact(compiler,(uint16_t)index,effective_start);
    }
    compiler->known_types[loop->result_register]=loop->result_type;
    compiler->known_type_sets[loop->result_register]=loop->result_set;
}

static uint16_t parse_index(Compiler *compiler,uint16_t receiver) {
    advance_token(compiler);
    const uint16_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after index"); return 0;
    }
    advance_token(compiler);
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_GET,destination,receiver,index,3);
    const int32_t receiver_set=compiler->known_type_sets[receiver];
    if(receiver_set>=0) {
        const DiamondTypeSet *set=&compiler->function->type_sets[(size_t)receiver_set];
        uint16_t indexed_sets[DIAMOND_MAX_UNION_TYPES];
        uint8_t collection_type=TYPE_UNKNOWN;
        const size_t member_count=set->count;
        bool compatible=member_count>0;
        for(size_t member_index=0;member_index<member_count;member_index++) {
            const DiamondTypeMember member=set->members[member_index];
            if(member.id!=DIAMOND_TYPE_ARRAY&&member.id!=DIAMOND_TYPE_HASH) {
                compatible=false;break;
            }
            if(collection_type==TYPE_UNKNOWN)collection_type=member.id;
            else if(collection_type!=member.id) {compatible=false;break;}
            indexed_sets[member_index]=member.id==DIAMOND_TYPE_ARRAY?
                member.argument_set:member.second_argument_set;
            if(indexed_sets[member_index]==DIAMOND_NO_TYPE_SET) {
                compatible=false;break;
            }
        }
        int32_t indexed=-1;
        for(size_t member_index=0;compatible&&member_index<member_count;
            member_index++) {
            indexed=indexed<0?(int32_t)indexed_sets[member_index]:
                join_type_set_indices(compiler,indexed,
                    (int32_t)indexed_sets[member_index]);
            if(indexed<0)compatible=false;
        }
        if(compatible&&collection_type==DIAMOND_TYPE_HASH)
            indexed=type_set_with_nil(compiler,(uint16_t)indexed);
        if(compatible&&indexed>=0)
            publish_known_type_set(compiler,destination,(uint16_t)indexed);
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

static uint16_t parse_if(Compiler *compiler,bool inverted) {
    compiler->narrowing=(Narrowing){};
    const uint16_t condition = parse_expression(compiler);
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    if (!consume_conditional_start(compiler)) return 0;

    uint16_t branch_condition=condition;
    if(inverted) {
        branch_condition=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NOT,branch_condition,condition,0,2);
    }
    const size_t false_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, branch_condition);
    const uint16_t destination = allocate_register(compiler);
    const size_t flow_reg_count=compiler->next_register;
    /* Inline arrays cover the overwhelming majority of if/elsif sites (a
     * function rarely has more than 256 registers live before one) at no
     * heap cost; only a flow_reg_count beyond that -- reachable in real
     * programs, since allocate_register never recycles a slot within one
     * function body -- falls back to malloc. Mirrors run_chunk's own
     * inline-then-heap register-array fallback (vm.c,
     * DIAMOND_INLINE_REGISTER_COUNT) rather than widening these to a
     * fixed DIAMOND_REGISTER_COUNT, which would cost every ordinary
     * if/elsif four ~12KB stack arrays whether it needs them or not.
     * Found via ASan: the previous fixed-256 arrays, indexed up to
     * flow_reg_count with no bounds check at all, were a confirmed
     * stack-buffer-overflow once a function had allocated more than 256
     * registers before reaching an if/unless -- not just theoretical,
     * reproduced with a 260-line register-churning program. */
    uint8_t inline_before_types[256];int32_t inline_before_sets[256];
    uint8_t inline_then_types[256];int32_t inline_then_sets[256];
    uint8_t *before_types=inline_before_types,*then_types=inline_then_types;
    int32_t *before_sets=inline_before_sets,*then_sets=inline_then_sets;
    uint8_t *heap_types=nullptr;int32_t *heap_sets=nullptr;
    if(flow_reg_count>256) {
        heap_types=malloc(flow_reg_count*2*sizeof(uint8_t));
        heap_sets=malloc(flow_reg_count*2*sizeof(int32_t));
        if(heap_types==nullptr||heap_sets==nullptr) {
            fail(compiler,compiler->previous.span,
                 "out of memory compiling if expression");
            free(heap_types);free(heap_sets);
            return destination;
        }
        before_types=heap_types;then_types=heap_types+flow_reg_count;
        before_sets=heap_sets;then_sets=heap_sets+flow_reg_count;
    }
    for(size_t index=0;index<flow_reg_count;index++) {
        before_types[index]=compiler->known_types[index];
        before_sets[index]=compiler->known_type_sets[index];
    }
    if(narrowing.valid)
        apply_narrowing_facts(compiler,
            inverted?narrowing.when_false:narrowing.when_true,
            inverted?narrowing.when_false_count:narrowing.when_true_count);
    const uint16_t then_result = compile_sequence(compiler);
    const uint8_t then_type=compiler->known_types[then_result];
    const int32_t then_set=compiler->known_type_sets[then_result];
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

    uint8_t false_result_type=DIAMOND_TYPE_NIL;int32_t false_result_set=-1;
    bool end_consumed=false;
    if (compiler->current.kind == DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE)skip_newlines(compiler);
        const uint16_t else_result = compile_sequence(compiler);
        const uint8_t else_type=compiler->known_types[else_result];
        const int32_t else_set=compiler->known_type_sets[else_result];
        emit_instruction(compiler, DIAMOND_OP_MOVE, destination, else_result, 0, 2);
        false_result_type=else_type;false_result_set=else_set;
    } else if(compiler->current.kind==DIAMOND_TOKEN_ELSIF) {
        advance_token(compiler);
        const uint16_t else_result=parse_if(compiler,false);
        const uint8_t else_type=compiler->known_types[else_result];
        const int32_t else_set=compiler->known_type_sets[else_result];
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,else_result,0,2);
        false_result_type=else_type;false_result_set=else_set;
        end_consumed=true;
    } else {
        emit_instruction(compiler, DIAMOND_OP_NIL, destination, 0, 0, 1);
    }

    for(size_t index=0;index<flow_reg_count;index++) {
        const uint8_t false_type=compiler->known_types[index];
        const int32_t false_set=compiler->known_type_sets[index];
        merge_flow_types(compiler,then_types[index],then_sets[index],
            false_type,false_set,&compiler->known_types[index],
            &compiler->known_type_sets[index]);
        if(register_is_local(compiler,(uint16_t)index)&&
           (then_types[index]!=false_type||then_sets[index]!=false_set))
            record_scope_type_fact(compiler,(uint16_t)index,
                compiler->current.span.start);
    }
    free(heap_types);free(heap_sets);
    merge_flow_types(compiler,then_type,then_set,false_result_type,
        false_result_set,&compiler->known_types[destination],
        &compiler->known_type_sets[destination]);

    if (!end_consumed&&compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after if expression");
        return destination;
    }
    if(!end_consumed)advance_token(compiler);
    patch_jump(compiler, end_jump, compiler->function->code_count);
    return destination;
}

/* Cheap lookahead: does a def/closure declaration appear anywhere within
 * the loop body about to be compiled -- from the current parse position
 * through *this* loop's own matching `end`, at any nesting depth inside
 * it (a def/closure nested inside an if/case/etc. within the loop still
 * counts, since enclosing_locals/capture threading already works
 * transitively through those) -- without scanning past it into whatever
 * follows. A throwaway DiamondLexer copy, the same zero-cost snapshot
 * idiom postfix_modifier_ahead/others already use for lookahead; never
 * advances the real parse position. Tracks end-terminated block nesting
 * via the complete set of opener keywords (confirmed against every
 * compiler function that consumes a trailing DIAMOND_TOKEN_END):
 * if/unless/while/until/loop/case/begin/class/module/interface/def/
 * closure. `do`/`then` are decoration, not openers (consume_loop_start/
 * consume_conditional_start); else/elsif/when/rescue/ensure are
 * mid-block separators the *same* opener already accounts for, not new
 * openers of their own. Deliberately doesn't special-case the endless
 * `def foo() = ...`/`closure foo() = ...` form (no matching `end` at
 * all): it returns true the instant it sees the def/closure token,
 * before it would ever need to know whether one exists. */
static bool loop_body_may_capture(const Compiler *compiler) {
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=compiler->current;
    size_t depth=0;
    while(token.kind!=DIAMOND_TOKEN_EOF) {
        if(token.kind==DIAMOND_TOKEN_DEF||token.kind==DIAMOND_TOKEN_CLOSURE)return true;
        if(token.kind==DIAMOND_TOKEN_IF||token.kind==DIAMOND_TOKEN_UNLESS||
           token.kind==DIAMOND_TOKEN_WHILE||token.kind==DIAMOND_TOKEN_UNTIL||
           token.kind==DIAMOND_TOKEN_LOOP||token.kind==DIAMOND_TOKEN_CASE||
           token.kind==DIAMOND_TOKEN_BEGIN||token.kind==DIAMOND_TOKEN_CLASS||
           token.kind==DIAMOND_TOKEN_MODULE||token.kind==DIAMOND_TOKEN_INTERFACE) {
            depth++;
        } else if(token.kind==DIAMOND_TOKEN_END) {
            if(depth==0)return false;
            depth--;
        }
        token=diamond_lexer_next(&lookahead);
    }
    return false;
}

/* Marks every currently-visible local in the enclosing function
 * captured, so its subsequent reads/writes -- including ones textually
 * before wherever the def/closure that triggered this actually sits --
 * go through the existing box-aware codegen from here on. Called once,
 * at a capturing loop's own entry; see loop_captures_pending's own
 * comment for why this needs to happen before the condition/body
 * compiles rather than lazily at the real capture site. */
static void mark_locals_captured(Compiler *compiler) {
    for(size_t index=0;index<compiler->local_count;index++)
        compiler->locals[index].captured=true;
}

static uint16_t parse_while(Compiler *compiler,bool inverted) {
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    const size_t loop_start = compiler->function->code_count;
    const bool outer_loop_captures_pending=compiler->loop_captures_pending;
    if(!compiler->loop_captures_pending&&loop_body_may_capture(compiler)) {
        compiler->loop_captures_pending=true;
        mark_locals_captured(compiler);
    }
    const uint16_t condition = parse_expression(compiler);
    if (!consume_loop_start(compiler)) {
        compiler->loop_captures_pending=outer_loop_captures_pending;
        return 0;
    }
    uint16_t branch_condition=condition;
    if(inverted) {
        branch_condition=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NOT,branch_condition,condition,0,2);
    }
    const size_t exit_jump = emit_jump(
        compiler, DIAMOND_OP_JUMP_IF_FALSE, branch_condition);
    const size_t flow_reg_count=compiler->next_register;
    uint8_t *exit_types=malloc(flow_reg_count*sizeof(uint8_t));
    int32_t *exit_sets=malloc(flow_reg_count*sizeof(int32_t));
    if(exit_types==nullptr||exit_sets==nullptr) {
        free(exit_types);free(exit_sets);
        fail(compiler,compiler->previous.span,"out of memory compiling loop flow");
        compiler->loop_captures_pending=outer_loop_captures_pending;
        return destination;
    }
    LoopContext loop={
        .previous=compiler->current_loop,
        .continue_target=loop_start,
        .redo_target=compiler->function->code_count,
        .result_register=destination,
        .flow_reg_count=flow_reg_count,
        .exit_types=exit_types,
        .exit_sets=exit_sets,
    };
    /* The condition may be false before the first iteration, so the entry
     * state and Nil result are always one real exit path. */
    merge_loop_exit(compiler,&loop,DIAMOND_TYPE_NIL,-1);
    compiler->current_loop=&loop;
    (void)compile_sequence(compiler);
    compiler->current_loop=loop.previous;
    /* A completed body reaches the condition again and may then exit. One
     * conservative source-level join is sufficient for advisory metadata. */
    merge_loop_exit(compiler,&loop,DIAMOND_TYPE_NIL,-1);
    compiler->loop_captures_pending=outer_loop_captures_pending;
    emit_absolute_jump(compiler, loop_start);
    patch_jump(compiler, exit_jump, compiler->function->code_count);
    for(size_t index=0;index<loop.break_count;index++)
        patch_jump(compiler,loop.breaks[index],compiler->function->code_count);

    if (compiler->current.kind != DIAMOND_TOKEN_END) {
        fail(compiler, compiler->current.span, "expected 'end' after while expression");
        free(exit_types);free(exit_sets);
        return 0;
    }
    const size_t join_offset=compiler->current.span.start;
    advance_token(compiler);
    /* A break-free loop may have no reachable expression result at all
     * (`while true` with only return/raise exits). Without constant-condition
     * reachability analysis, keep that result unknown instead of claiming Nil
     * and rejecting an otherwise valid enclosing return annotation. */
    if(loop.break_count==0) {loop.result_type=TYPE_UNKNOWN;loop.result_set=-1;}
    finish_loop_flow(compiler,&loop,join_offset);
    free(exit_types);free(exit_sets);
    return destination;
}

static uint16_t parse_loop(Compiler *compiler) {
    const uint16_t destination=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
    if(!consume_loop_start(compiler))return destination;
    const bool outer_loop_captures_pending=compiler->loop_captures_pending;
    if(!compiler->loop_captures_pending&&loop_body_may_capture(compiler)) {
        compiler->loop_captures_pending=true;
        mark_locals_captured(compiler);
    }
    const size_t body_start=compiler->function->code_count;
    const size_t flow_reg_count=compiler->next_register;
    uint8_t *exit_types=malloc(flow_reg_count*sizeof(uint8_t));
    int32_t *exit_sets=malloc(flow_reg_count*sizeof(int32_t));
    if(exit_types==nullptr||exit_sets==nullptr) {
        free(exit_types);free(exit_sets);
        fail(compiler,compiler->previous.span,"out of memory compiling loop flow");
        compiler->loop_captures_pending=outer_loop_captures_pending;
        return destination;
    }
    LoopContext loop={.previous=compiler->current_loop,
        .continue_target=body_start,.redo_target=body_start,
        .result_register=destination,.flow_reg_count=flow_reg_count,
        .exit_types=exit_types,.exit_sets=exit_sets};
    compiler->current_loop=&loop;
    (void)compile_sequence(compiler);
    compiler->current_loop=loop.previous;
    compiler->loop_captures_pending=outer_loop_captures_pending;
    emit_absolute_jump(compiler,body_start);
    for(size_t index=0;index<loop.break_count;index++)
        patch_jump(compiler,loop.breaks[index],compiler->function->code_count);
    if(compiler->current.kind!=DIAMOND_TOKEN_END) {
        fail(compiler,compiler->current.span,"expected 'end' after loop");
        free(exit_types);free(exit_sets);
        return destination;
    }
    const size_t join_offset=compiler->current.span.start;
    advance_token(compiler);
    finish_loop_flow(compiler,&loop,join_offset);
    free(exit_types);free(exit_sets);return destination;
}

static uint16_t parse_prefix(Compiler *compiler) {
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
            const uint16_t destination = allocate_register(compiler);
            if(compiler->current_module>=0&&compiler->current_class<0) {
                const uint16_t field=module_field_name(
                    compiler,compiler->previous.span);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,destination,0,
                                 field,3);
            } else {
                const int field=field_index(compiler,compiler->previous.span,true);
                emit_instruction(compiler,DIAMOND_OP_GET_IVAR,destination,0,
                                 (uint8_t)field,3);
            }
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
                return parse_closure_call_arguments(compiler,destination);
            return destination;
        }
        case DIAMOND_TOKEN_CLASS_VARIABLE: {
            const uint16_t destination = allocate_register(compiler);
            const int slot = class_variable_index(compiler,compiler->previous.span,true);
            emit_instruction(compiler,DIAMOND_OP_GET_CVAR,destination,
                             (uint8_t)compiler->current_class,(uint8_t)slot,3);
            if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN)
                return parse_closure_call_arguments(compiler,destination);
            return destination;
        }
        case DIAMOND_TOKEN_SELF:
            if (!compiler->in_method) {
                fail(compiler, compiler->previous.span, "'self' used outside a method");
                return 0;
            }
            /* self.foo(...) inside a class-owned singleton method's own
             * body needs real dynamic dispatch (see
             * parse_self_class_method_call's own comment) -- everywhere
             * else (an ordinary instance method, or bare `self` with no
             * following call inside a singleton method), self stays an
             * ordinary register-0 value, unchanged. A `closure name()
             * ... end` body reuses this exact same code unmodified: its
             * own register 0 already holds a materialized copy of the
             * captured self (see compile_definition's captures_self
             * handling), and in_singleton_method is set true there too
             * when appropriate, so this check and the plain `return 0`
             * both already do the right thing with no capture-specific
             * branch needed here at all. */
            if(compiler->in_singleton_method&&compiler->current_class>=0&&
               compiler->current.kind==DIAMOND_TOKEN_DOT) {
                return parse_self_class_method_call(compiler);
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
            const uint16_t operand = parse_precedence(compiler, PREC_PREFIX);
            const uint16_t destination = allocate_register(compiler);
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
            const uint16_t operand=parse_precedence(compiler,PREC_PREFIX);
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_NOT,destination,operand,0,2);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            return destination;
        }
        case DIAMOND_TOKEN_AMPERSAND:
            /* A block is already represented as an ordinary Callable value.
             * `&block` therefore marks forwarding intent syntactically while
             * preserving the closure register unchanged. */
            return parse_precedence(compiler,PREC_PREFIX);
        case DIAMOND_TOKEN_CASE:
            return parse_case(compiler);
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
        case DIAMOND_TOKEN_LESS_LESS: return DIAMOND_OP_SHIFT_LEFT;
        case DIAMOND_TOKEN_PERCENT: return DIAMOND_OP_MODULO;
        case DIAMOND_TOKEN_SPACESHIP: return DIAMOND_OP_COMPARE;
        default: return DIAMOND_OP_ADD;
    }
}

/* The generic (non-short-circuit, non-`is`) binary-operator codegen --
 * factored out of parse_precedence's own infix loop so
 * compile_compound_assignment (below) can reuse it verbatim for `+=`/
 * `-=`/`*=`/`/=`/`%=` instead of re-parsing the desugared `x = x + y`
 * form or duplicating the Int-fast-path/type-narrowing logic. Takes
 * `left`/`right` as already-evaluated registers -- this is deliberately
 * NOT `right = parse_precedence(...)` itself, since a compound
 * assignment's RHS is parsed as a full expression (`parse_expression`),
 * not at `operator_precedence + 1` the way a chained infix operand is. */
static uint16_t compile_binary_op(Compiler *compiler, DiamondTokenKind operator,
                                   uint16_t left, uint16_t right) {
    const uint16_t destination = allocate_register(compiler);
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
            uint16_t narrowed=left,nil_value=right;
            if(compiler->known_types[left]==DIAMOND_TYPE_NIL) {
                narrowed=right;nil_value=left;
            }
            if(compiler->known_types[nil_value]==DIAMOND_TYPE_NIL&&
               compiler->known_type_sets[narrowed]>=0) {
                int32_t non_nil=-1,nil_only=-1;
                if(split_nil_type_set(compiler,
                   (uint16_t)compiler->known_type_sets[narrowed],
                   &non_nil,&nil_only)) {
                    const int32_t when_true=operator==DIAMOND_TOKEN_BANG_EQUAL
                        ?non_nil:nil_only;
                    const int32_t when_false=operator==DIAMOND_TOKEN_BANG_EQUAL
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
    } else if(operator!=DIAMOND_TOKEN_SPACESHIP &&
              (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[left]==DIAMOND_TYPE_INT) &&
              (compiler->known_types[right]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[right]==DIAMOND_TYPE_INT) &&
              (compiler->known_types[left]==DIAMOND_TYPE_FLOAT||
               compiler->known_types[right]==DIAMOND_TYPE_FLOAT)) {
        /* Mixed Int/Float statically known to auto-promote to Float --
         * excludes spaceship explicitly: unlike the arithmetic operators,
         * its own result is always Int-or-Nil, never Float, regardless of
         * operand types (the DIAMOND_TYPE_INT branch just above stays
         * correct for spaceship unmodified, since two known Ints really
         * do always produce an Int result under this design). Found and
         * fixed during this feature's own design, before any code was
         * written -- see the Comparable design doc. */
        compiler->known_types[destination]=DIAMOND_TYPE_FLOAT;
    } else if(operator==DIAMOND_TOKEN_PLUS &&
              compiler->known_types[left]==DIAMOND_TYPE_STRING &&
              compiler->known_types[right]==DIAMOND_TYPE_STRING) {
        compiler->known_types[destination]=DIAMOND_TYPE_STRING;
    }
    return destination;
}

typedef struct CaseFlowJoin {
    uint8_t *types;
    int32_t *sets;
    bool *varied;
    bool initialized;
    uint8_t result_type;
    int32_t result_set;
} CaseFlowJoin;

typedef enum CaseArrayNodeKind {CASE_ARRAY_GROUP,CASE_ARRAY_VALUE,
    CASE_ARRAY_BIND,CASE_ARRAY_WILDCARD,CASE_ARRAY_REST_BIND,
    CASE_ARRAY_REST_WILDCARD,CASE_ARRAY_PIN,CASE_HASH_GROUP,
    CASE_OBJECT_GROUP,CASE_HASH_REST_BIND,CASE_HASH_REST_WILDCARD} CaseArrayNodeKind;

typedef struct CaseArrayNode {
    CaseArrayNodeKind kind;
    DiamondSpan name;
    DiamondSpan member_name;
    uint16_t value_register;
    uint16_t key_register;
    uint16_t subject_register;
    uint8_t children[16];
    uint8_t child_count;
} CaseArrayNode;

static bool span_is_underscore(const Compiler *compiler,DiamondSpan span) {
    return span.length==1&&compiler->source[span.start]=='_';
}

static const DiamondMethod *case_pattern_reader(const Compiler *compiler,
        uint8_t class_index,DiamondSpan name) {
    const DiamondClass *class=&compiler->program->classes[class_index];
    while(class!=nullptr) {
        for(size_t method=class->method_count;method>0;method--) {
            const DiamondMethod *candidate=&class->methods[method-1];
            if(name_equals(compiler,candidate->name,name,false))return candidate;
        }
        class=class->superclass==UINT8_MAX?nullptr:
            &compiler->program->classes[class->superclass];
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
    emit_register(compiler,result);emit_byte(compiler,(uint8_t)class_index);
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
            node->value_register=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_LOAD_CLASS);
            emit_register(compiler,node->value_register);
            emit_byte(compiler,(uint8_t)object_class);
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
                    compiler,(uint8_t)object_class,reader_name);
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

typedef struct CaseBinding {
    DiamondSpan name;
    uint16_t reg;
} CaseBinding;

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
        (void)compile_assignment_store(compiler,bindings[binding].name,
            false,false,bindings[binding].reg);
}

static void merge_case_branch(Compiler *compiler,CaseFlowJoin *join,
        size_t flow_reg_count,uint16_t branch_result) {
    const uint8_t branch_result_type=compiler->known_types[branch_result];
    const int32_t branch_result_set=compiler->known_type_sets[branch_result];
    if(!join->initialized) {
        for(size_t index=0;index<flow_reg_count;index++) {
            join->types[index]=compiler->known_types[index];
            join->sets[index]=compiler->known_type_sets[index];
        }
        join->result_type=branch_result_type;join->result_set=branch_result_set;
        join->initialized=true;return;
    }
    for(size_t index=0;index<flow_reg_count;index++) {
        if(join->types[index]!=compiler->known_types[index]||
           join->sets[index]!=compiler->known_type_sets[index])join->varied[index]=true;
        merge_flow_types(compiler,join->types[index],join->sets[index],
            compiler->known_types[index],compiler->known_type_sets[index],
            &join->types[index],&join->sets[index]);
    }
    merge_flow_types(compiler,join->result_type,join->result_set,
        branch_result_type,branch_result_set,&join->result_type,&join->result_set);
}

static void finish_case_flow(Compiler *compiler,CaseFlowJoin *join,
        size_t flow_reg_count,uint16_t destination,size_t effective_start) {
    for(size_t index=0;index<flow_reg_count;index++) {
        compiler->known_types[index]=join->types[index];
        compiler->known_type_sets[index]=join->sets[index];
        if(join->varied[index]&&register_is_local(compiler,(uint16_t)index))
            record_scope_type_fact(compiler,(uint16_t)index,effective_start);
    }
    compiler->known_types[destination]=join->result_type;
    compiler->known_type_sets[destination]=join->result_set;
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
 * `entry_types`/`entry_sets` (a snapshot of compiler->known_types/
 * known_type_sets taken once, in parse_case, before the first branch)
 * gets restored at the top of every call -- a when-clause's values and
 * body are compiled as though no earlier when-clause's (also-compiled,
 * possibly-speculative) body actually ran, mirroring why parse_if resets
 * to its own before_types before compiling the else branch. `join` then
 * accumulates every branch's result and local state into the same advisory
 * unions parse_if uses; a missing else contributes the entry state and Nil. */
static uint16_t parse_case_branches(Compiler *compiler, uint16_t subject,
        size_t flow_reg_count, const uint8_t *entry_types,
        const int32_t *entry_sets, uint16_t destination,CaseFlowJoin *join,
        bool subjectless) {
    for(size_t index=0;index<flow_reg_count;index++) {
        compiler->known_types[index]=entry_types[index];
        compiler->known_type_sets[index]=entry_sets[index];
    }
    if(compiler->current.kind==DIAMOND_TOKEN_ELSE) {
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE)skip_newlines(compiler);
        const uint16_t body_result=compile_sequence(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,body_result,0,2);
        merge_case_branch(compiler,join,flow_reg_count,body_result);
        if(compiler->current.kind!=DIAMOND_TOKEN_END) {
            fail(compiler,compiler->current.span,
                 "expected 'end' after case expression");
            return destination;
        }
        const size_t join_offset=compiler->current.span.start;
        advance_token(compiler);
        finish_case_flow(compiler,join,flow_reg_count,destination,join_offset);
        return destination;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_END) {
        emit_instruction(compiler,DIAMOND_OP_NIL,destination,0,0,1);
        compiler->known_types[destination]=DIAMOND_TYPE_NIL;
        compiler->known_type_sets[destination]=-1;
        merge_case_branch(compiler,join,flow_reg_count,destination);
        const size_t join_offset=compiler->current.span.start;
        advance_token(compiler);
        finish_case_flow(compiler,join,flow_reg_count,destination,join_offset);
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
            size_t indices[64],aligned[64];
            const size_t alternative_binding_count=
                collect_case_bindings(array_nodes,node_count,indices);
            if(first_pattern) {
                binding_count=alternative_binding_count;
                for(size_t binding=0;binding<binding_count;binding++) {
                    bindings[binding].name=array_nodes[indices[binding]].name;
                    bindings[binding].reg=allocate_register(compiler);
                    aligned[binding]=indices[binding];
                    compiler->known_types[bindings[binding].reg]=
                        compiler->known_types[array_nodes[aligned[binding]].subject_register];
                    compiler->known_type_sets[bindings[binding].reg]=
                        compiler->known_type_sets[array_nodes[aligned[binding]].subject_register];
                }
            } else if(!align_case_alternative_bindings(compiler,array_nodes,
                    indices,alternative_binding_count,bindings,binding_count,
                    aligned))return destination;
            if(!first_pattern)
                for(size_t binding=0;binding<binding_count;binding++) {
                    const uint16_t source=
                        array_nodes[aligned[binding]].subject_register;
                    if(compiler->known_types[bindings[binding].reg]!=
                           compiler->known_types[source]||
                       compiler->known_type_sets[bindings[binding].reg]!=
                           compiler->known_type_sets[source]) {
                        compiler->known_types[bindings[binding].reg]=TYPE_UNKNOWN;
                        compiler->known_type_sets[bindings[binding].reg]=-1;
                    }
                }
            emit_instruction(compiler,DIAMOND_OP_BOOL,match_reg,true,0,2);
            compiler->known_types[match_reg]=DIAMOND_TYPE_BOOL;
            size_t failure_jumps[128];size_t failure_count=0;
            emit_case_array_match(compiler,array_nodes,array_root,subject,match_reg,
                failure_jumps,&failure_count);
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
               after_pattern!=DIAMOND_TOKEN_LEFT_BRACE)
                value_reg=compile_case_pattern_class(compiler,pattern_class);
            else value_reg=parse_expression(compiler);
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
    }
    if(!consume_conditional_start(compiler))return destination;
    const size_t false_jump=
        emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,match_reg);
    if(array_pattern)commit_case_bindings(compiler,bindings,binding_count);
    const uint16_t body_result=compile_sequence(compiler);
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,body_result,0,2);
    merge_case_branch(compiler,join,flow_reg_count,body_result);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,false_jump,compiler->function->code_count);
    const uint16_t result=parse_case_branches(
        compiler,subject,flow_reg_count,entry_types,entry_sets,destination,join,
        subjectless);
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
static uint16_t parse_case(Compiler *compiler) {
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
    const uint16_t destination=allocate_register(compiler);
    const size_t flow_reg_count=compiler->next_register;
    /* Same inline-then-heap fallback as parse_if's before_types/
     * then_types -- see that function's own comment for why, and for the
     * ASan-confirmed bug a fixed [256] array with no bounds check caused
     * here otherwise. */
    uint8_t inline_entry_types[256],inline_join_types[256];
    int32_t inline_entry_sets[256],inline_join_sets[256];
    bool inline_varied[256]={};
    uint8_t *entry_types=inline_entry_types;int32_t *entry_sets=inline_entry_sets;
    uint8_t *join_types=inline_join_types;int32_t *join_sets=inline_join_sets;
    bool *varied=inline_varied;
    uint8_t *heap_types=nullptr;int32_t *heap_sets=nullptr;bool *heap_varied=nullptr;
    if(flow_reg_count>256) {
        heap_types=malloc(flow_reg_count*2*sizeof(uint8_t));
        heap_sets=malloc(flow_reg_count*2*sizeof(int32_t));
        heap_varied=calloc(flow_reg_count,sizeof(bool));
        if(heap_types==nullptr||heap_sets==nullptr||heap_varied==nullptr) {
            fail(compiler,compiler->previous.span,
                 "out of memory compiling case expression");
            free(heap_types);free(heap_sets);free(heap_varied);
            return destination;
        }
        entry_types=heap_types;join_types=heap_types+flow_reg_count;
        entry_sets=heap_sets;join_sets=heap_sets+flow_reg_count;varied=heap_varied;
    }
    for(size_t index=0;index<flow_reg_count;index++) {
        entry_types[index]=compiler->known_types[index];
        entry_sets[index]=compiler->known_type_sets[index];
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_WHEN) {
        fail(compiler,compiler->current.span,
             "expected 'when' after case expression");
        free(heap_types);free(heap_sets);free(heap_varied);
        return destination;
    }
    CaseFlowJoin join={.types=join_types,.sets=join_sets,.varied=varied,
        .result_type=TYPE_UNKNOWN,.result_set=-1};
    const uint16_t result=parse_case_branches(
        compiler,subject,flow_reg_count,entry_types,entry_sets,destination,&join,
        subjectless);
    free(heap_types);free(heap_sets);free(heap_varied);
    return result;
}

static uint16_t parse_precedence(Compiler *compiler, Precedence precedence) {
    uint16_t left = parse_prefix(compiler);
    while (!compiler->failed &&
           (compiler->current.kind == DIAMOND_TOKEN_DOT ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_BRACKET ||
            compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN)) {
        /* `expr(...)` directly after an already-fully-parsed expression
         * (no `.method`/`[index]` in between) means "call the Callable
         * value `left` itself" -- `type.coerce_input()(value)`,
         * `(a)(b)`, `arr[0]()`, and so on. A previously-undiscovered
         * gap, not a deliberate cut: nothing in this grammar has any
         * other meaning for two adjacent expressions with no operator
         * between them, so this can only ever turn a program that used
         * to be a hard parse error ("expected newline after
         * expression", since the leftover `(...)` had nowhere to go)
         * into a working one -- it can't reinterpret anything that
         * used to compile. parse_closure_call_arguments already exists
         * for exactly this "call whatever's in this register" shape
         * (shared with a local variable or `@ivar`/`@@cvar` holding a
         * Callable, both followed by `(...)`); reusing it here just
         * extends where its receiver register is allowed to come from. */
        if(compiler->current.kind == DIAMOND_TOKEN_LEFT_PAREN) {
            left = parse_closure_call_arguments(compiler,left);
        } else {
            left = compiler->current.kind == DIAMOND_TOKEN_DOT
                ? parse_invoke(compiler,left) : parse_index(compiler,left);
        }
    }
    while (!compiler->failed &&
           token_precedence(compiler->current.kind) >= precedence) {
        const DiamondTokenKind operator = compiler->current.kind;
        const Precedence operator_precedence = token_precedence(operator);
        advance_token(compiler);
        /* A newline right after a binary operator can only mean "the
         * right operand continues on the next line" -- the while
         * condition above already confirmed `operator` is a genuine
         * infix operator at or above the caller's minimum precedence,
         * and no such operator can legally end a statement, so there's
         * no ambiguity to preserve by leaving this newline for the
         * statement-separator logic elsewhere to see. Fixes `x = 1 +\n
         * 2` and `if a &&\n b` (previously "expected expression"):
         * newline-skipping had only ever existed at bracket-delimited
         * list boundaries, never inside a general binary expression. */
        skip_newlines(compiler);
        if(operator==DIAMOND_TOKEN_IS) {
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                fail(compiler,compiler->current.span,"expected type after 'is'");
                return left;
            }
            const DiamondSpan tested_type_span=compiler->current.span;
            char tested_type_name[DIAMOND_MAX_FUNCTION_NAME];
            if(!consume_qualified_name(compiler,tested_type_name,
                                       sizeof tested_type_name)) {
                fail(compiler,tested_type_span,"type name is too long");
                return left;
            }
            const uint8_t tested_type=(uint8_t)resolve_type_name(
                compiler,tested_type_name,tested_type_span);
            if(tested_type>=DIAMOND_TYPE_VARIABLE_BASE&&
               tested_type<DIAMOND_TYPE_INTERFACE_BASE)
                fail(compiler,tested_type_span,
                     "generic type variables cannot be used with 'is' before binding");
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_IS_TYPE,destination,left,
                             tested_type,3);
            compiler->known_types[destination]=DIAMOND_TYPE_BOOL;
            if(compiler->known_type_sets[left]>=0) {
                int32_t matching=-1,remaining=-1;
                if(split_type_set(compiler,
                   (uint16_t)compiler->known_type_sets[left],tested_type,
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
            const uint16_t destination=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
            const size_t end_jump=emit_jump(compiler,
                is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
            const uint16_t right=parse_precedence(
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
        if(operator==DIAMOND_TOKEN_DOT_DOT||operator==DIAMOND_TOKEN_DOT_DOT_DOT) {
            const bool exclusive=operator==DIAMOND_TOKEN_DOT_DOT_DOT;
            const uint16_t right=parse_precedence(
                compiler,(Precedence)(operator_precedence+1));
            const int class_index=find_class_name(compiler,"Range");
            if(class_index<0) {
                fail(compiler,compiler->previous.span,
                     "'Range' is not defined -- is the prelude loaded?");
                return left;
            }
            const uint16_t exclusive_reg=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_BOOL,exclusive_reg,exclusive,0,2);
            const uint16_t base=allocate_register(compiler);
            (void)allocate_register(compiler);
            (void)allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,base,left,0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+1),right,0,2);
            emit_instruction(compiler,DIAMOND_OP_MOVE,(uint16_t)(base+2),
                             exclusive_reg,0,2);
            const uint16_t dest=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_NEW); emit_register(compiler,dest);
            emit_byte(compiler,(uint8_t)class_index); emit_register(compiler,base);
            emit_byte(compiler,3);
            compiler->known_types[dest]=(uint8_t)(DIAMOND_TYPE_CLASS_BASE+class_index);
            left=dest;continue;
        }
        const uint16_t right = parse_precedence(
            compiler, (Precedence)(operator_precedence + 1));
        left = compile_binary_op(compiler, operator, left, right);
    }
    return left;
}

/* `cond ? a : b` -- binds looser than every binary operator (including
 * `..`/`&&`/`||`), tighter than assignment, matching Ruby's own
 * precedence table. Implemented as the wrapper around parse_expression's
 * own top-level entry point (parse_precedence(PREC_RANGE) used to be
 * that entry point directly; it's now just how a ternary's condition
 * gets parsed) rather than folded into parse_precedence's infix loop --
 * unlike every operator that loop handles, the "operator" here isn't a
 * single token, and the right-hand side needs its own nested-ternary-
 * aware entry point (a true/false branch can itself contain a `?:`,
 * right-associating the same way `a ? b : c ? d : e` does in Ruby),
 * which is exactly what recursing into parse_expression itself gives
 * for free, the same way parse_if's own elsif chain recurses into
 * itself. No new DiamondOpCode: same JUMP_IF_FALSE/MOVE/JUMP shape
 * parse_if's then/else already uses, just without a full compile_
 * sequence body on either side (a ternary's branches are expressions,
 * not statement lists, so -- unlike parse_if/parse_case_branches --
 * there's no local-variable-reassignment-inside-a-branch hazard here
 * that would need a register-snapshot save/restore: an expression alone
 * can't reassign a local, only compile_assignment/compile_compound_
 * assignment can, and neither is reachable from here). */
static uint16_t parse_ternary(Compiler *compiler) {
    const uint16_t condition = parse_precedence(compiler, PREC_RANGE);
    /* Only touch compiler->narrowing once this has actually turned out to
     * be a ternary -- parse_expression (this function) is also how every
     * non-ternary condition gets parsed (parse_if's own condition, for
     * one), and those callers read compiler->narrowing themselves right
     * after calling parse_expression to pick up whatever the condition's
     * own comparison set. Resetting it here unconditionally, even on the
     * plain "no '?' follows, just return condition" path, would silently
     * erase that for every single caller -- confirmed as a real
     * regression (parse_if's own then/else type-narrowing broke,
     * de-optimizing an elidable CHECK_TYPE back on) before catching it
     * in tests/run.sh's existing diagnostic-only-on-failure assertion. */
    if(compiler->current.kind!=DIAMOND_TOKEN_QUESTION) return condition;
    const Narrowing narrowing=compiler->narrowing.condition==condition
        ? compiler->narrowing:(Narrowing){};
    compiler->narrowing=(Narrowing){};
    advance_token(compiler);
    skip_newlines(compiler);
    const size_t false_jump=
        emit_jump(compiler,DIAMOND_OP_JUMP_IF_FALSE,condition);
    const uint16_t destination=allocate_register(compiler);
    if(narrowing.valid)
        apply_narrowing_facts(compiler,narrowing.when_true,narrowing.when_true_count);
    const uint16_t true_result=parse_expression(compiler);
    const uint8_t true_type=compiler->known_types[true_result];
    const int32_t true_set=compiler->known_type_sets[true_result];
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,true_result,0,2);
    const size_t end_jump=emit_jump(compiler,DIAMOND_OP_JUMP,0);
    patch_jump(compiler,false_jump,compiler->function->code_count);
    if(narrowing.valid)
        apply_narrowing_facts(compiler,narrowing.when_false,narrowing.when_false_count);
    skip_newlines(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
        fail(compiler,compiler->current.span,"expected ':' in ternary expression");
        return destination;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    const uint16_t false_result=parse_expression(compiler);
    const uint8_t false_type=compiler->known_types[false_result];
    const int32_t false_set=compiler->known_type_sets[false_result];
    emit_instruction(compiler,DIAMOND_OP_MOVE,destination,false_result,0,2);
    patch_jump(compiler,end_jump,compiler->function->code_count);
    merge_flow_types(compiler,true_type,true_set,false_type,false_set,
        &compiler->known_types[destination],
        &compiler->known_type_sets[destination]);
    return destination;
}

static uint16_t parse_expression(Compiler *compiler) {
    return parse_ternary(compiler);
}

static bool assignment_ahead(const Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER &&
        compiler->current.kind != DIAMOND_TOKEN_INSTANCE_VARIABLE &&
        compiler->current.kind != DIAMOND_TOKEN_CLASS_VARIABLE) {
        return false;
    }
    DiamondLexer lookahead = compiler->lexer;
    return diamond_lexer_next(&lookahead).kind == DIAMOND_TOKEN_EQUAL;
}

static bool compound_assignment_token(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_PLUS_EQUAL||kind==DIAMOND_TOKEN_MINUS_EQUAL||
           kind==DIAMOND_TOKEN_STAR_EQUAL||kind==DIAMOND_TOKEN_SLASH_EQUAL||
           kind==DIAMOND_TOKEN_PERCENT_EQUAL||kind==DIAMOND_TOKEN_OR_OR_EQUAL||
           kind==DIAMOND_TOKEN_AND_AND_EQUAL;
}

/* Same shape as assignment_ahead above (only a plain local/@ivar/@@cvar
 * target -- indexed (`arr[i] += 1`) compound assignment is a deliberate
 * v1 scope cut, same spirit as `<<`/`%`'s own cuts), but for `+=`/`-=`/
 * `*=`/`/=`/`%=`/`||=`/`&&=` instead of plain `=`. */
static bool compound_assignment_ahead(const Compiler *compiler) {
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER &&
        compiler->current.kind != DIAMOND_TOKEN_INSTANCE_VARIABLE &&
        compiler->current.kind != DIAMOND_TOKEN_CLASS_VARIABLE) {
        return false;
    }
    DiamondLexer lookahead = compiler->lexer;
    return compound_assignment_token(diamond_lexer_next(&lookahead).kind);
}

/* `x[a][b]... = value` -- any number of chained `[...]` groups before
 * the `=`, not just one. Originally scanned only a single balanced
 * `[...]` group before checking for `=`, so `x[a][b] = value` (a
 * genuine, previously-undiscovered gap, not a deliberate cut) fell
 * through to ordinary expression parsing instead -- `x[a][b]` compiled
 * fine as a *read* (parse_precedence's own postfix-chaining loop
 * already handles repeated `[...]`/`.` unconditionally), leaving the
 * trailing `= value` as unconsumed tokens, surfacing as "expected
 * newline after expression" at the statement level. After each closing
 * `]`, peek one more token: another `[` means "keep scanning, this
 * wasn't the last index"; anything else ends the scan the same way it
 * always did. */
static bool index_assignment_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER&&
       compiler->current.kind!=DIAMOND_TOKEN_INSTANCE_VARIABLE&&
       compiler->current.kind!=DIAMOND_TOKEN_CLASS_VARIABLE) return false;
    DiamondLexer lookahead=compiler->lexer;
    DiamondToken token=diamond_lexer_next(&lookahead);
    if(token.kind!=DIAMOND_TOKEN_LEFT_BRACKET) return false;
    bool more_groups=true;
    while(more_groups) {
        size_t depth=1;
        while(depth>0) {
            token=diamond_lexer_next(&lookahead);
            if(token.kind==DIAMOND_TOKEN_EOF || token.kind==DIAMOND_TOKEN_ERROR)
                return false;
            if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) depth++;
            if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET) depth--;
        }
        DiamondLexer probe=lookahead;
        token=diamond_lexer_next(&probe);
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            lookahead=probe;
            more_groups=true;
        } else {
            more_groups=false;
        }
    }
    return diamond_lexer_next(&lookahead).kind==DIAMOND_TOKEN_EQUAL;
}

/* Same balanced-`[...]`-bracket scan as index_assignment_ahead just
 * above, but for `arr[i] += 1`-shaped indexed compound assignment
 * instead of plain `arr[i] = v` -- checks for a compound-assignment
 * operator (compound_assignment_token, shared with compound_assignment_
 * ahead) after the closing `]` instead of a bare `=`. */
static bool index_compound_assignment_ahead(const Compiler *compiler) {
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER&&
       compiler->current.kind!=DIAMOND_TOKEN_INSTANCE_VARIABLE&&
       compiler->current.kind!=DIAMOND_TOKEN_CLASS_VARIABLE) return false;
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
    return compound_assignment_token(diamond_lexer_next(&lookahead).kind);
}

static bool destructuring_target_kind(DiamondTokenKind kind) {
    return kind==DIAMOND_TOKEN_IDENTIFIER||kind==DIAMOND_TOKEN_INSTANCE_VARIABLE||
           kind==DIAMOND_TOKEN_CLASS_VARIABLE;
}

static DiamondToken scan_destructuring_target_suffix(DiamondLexer *lexer,
        DiamondToken token) {
    for(;;) {
        if(token.kind==DIAMOND_TOKEN_DOT) {
            token=diamond_lexer_next(lexer);
            if(token.kind!=DIAMOND_TOKEN_IDENTIFIER)return token;
            token=diamond_lexer_next(lexer);
            continue;
        }
        if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
            size_t depth=1;
            while(depth>0) {
                token=diamond_lexer_next(lexer);
                if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR)
                    return token;
                if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET)depth++;
                if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET)depth--;
            }
            token=diamond_lexer_next(lexer);
            continue;
        }
        return token;
    }
}

static bool scan_destructuring_pattern(DiamondLexer *lexer,DiamondToken token,
        DiamondToken *after,size_t depth) {
    if(depth>8)return false;
    if(token.kind==DIAMOND_TOKEN_STAR) {
        token=diamond_lexer_next(lexer);
        if(!destructuring_target_kind(token.kind))return false;
        *after=scan_destructuring_target_suffix(lexer,diamond_lexer_next(lexer));
        return true;
    }
    if(destructuring_target_kind(token.kind)) {
        *after=scan_destructuring_target_suffix(lexer,diamond_lexer_next(lexer));
        return true;
    }
    if(token.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        token=diamond_lexer_next(lexer);
        if(token.kind==DIAMOND_TOKEN_RIGHT_BRACE)return false;
        for(;;) {
            if(token.kind==DIAMOND_TOKEN_STAR) {
                token=diamond_lexer_next(lexer);
                if(token.kind!=DIAMOND_TOKEN_STAR)return false;
                token=diamond_lexer_next(lexer);
                if(!destructuring_target_kind(token.kind))return false;
                token=scan_destructuring_target_suffix(
                    lexer,diamond_lexer_next(lexer));
                if(token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                    *after=diamond_lexer_next(lexer);return true;
                }
                if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
                token=diamond_lexer_next(lexer);
                continue;
            }
            size_t nesting=0;
            for(;;) {
                if(token.kind==DIAMOND_TOKEN_EOF||token.kind==DIAMOND_TOKEN_ERROR||
                   token.kind==DIAMOND_TOKEN_NEWLINE)return false;
                if(token.kind==DIAMOND_TOKEN_COLON&&nesting==0)break;
                if(token.kind==DIAMOND_TOKEN_LEFT_PAREN||
                   token.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                   token.kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
                if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN||
                   token.kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                   token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                    if(nesting==0)return false;
                    nesting--;
                }
                token=diamond_lexer_next(lexer);
            }
            token=diamond_lexer_next(lexer);
            if(!scan_destructuring_pattern(lexer,token,&token,depth+1))return false;
            if(token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
                *after=diamond_lexer_next(lexer);return true;
            }
            if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
            token=diamond_lexer_next(lexer);
        }
    }
    if(token.kind!=DIAMOND_TOKEN_LEFT_BRACKET)return false;
    token=diamond_lexer_next(lexer);
    if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET)return false;
    for(;;) {
        if(!scan_destructuring_pattern(lexer,token,&token,depth+1))return false;
        if(token.kind==DIAMOND_TOKEN_RIGHT_BRACKET) {
            *after=diamond_lexer_next(lexer);return true;
        }
        if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
        token=diamond_lexer_next(lexer);
    }
}

/* Same clone-the-lexer-and-scan-forward technique as assignment_ahead/
 * index_assignment_ahead above. Recognizes the historical comma-root form
 * (`a, b =`) and bracketed recursive patterns (`[a, [b, c]] =`) without
 * stealing an ordinary Array literal from expression parsing. */
static bool multi_assignment_ahead(const Compiler *compiler) {
    DiamondLexer lookahead = compiler->lexer;
    DiamondToken token=compiler->current;
    if(token.kind==DIAMOND_TOKEN_LEFT_BRACKET||token.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        return scan_destructuring_pattern(&lookahead,token,&token,0)&&
            token.kind==DIAMOND_TOKEN_EQUAL;
    }
    if(!destructuring_target_kind(token.kind))return false;
    token=scan_destructuring_target_suffix(
        &lookahead,diamond_lexer_next(&lookahead));
    if(token.kind!=DIAMOND_TOKEN_COMMA)return false;
    do {
        token=diamond_lexer_next(&lookahead);
        if(!scan_destructuring_pattern(&lookahead,token,&token,0))return false;
    } while(token.kind==DIAMOND_TOKEN_COMMA);
    return token.kind==DIAMOND_TOKEN_EQUAL;
}

static DiamondTokenKind postfix_modifier_ahead(const Compiler *compiler) {
    if (compiler->current.kind == DIAMOND_TOKEN_DEF ||
        compiler->current.kind == DIAMOND_TOKEN_CLOSURE ||
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

static uint16_t compile_index_assignment(Compiler *compiler) {
    const DiamondSpan name=compiler->current.span;
    uint16_t receiver;
    /* @ivar[key] = value / @@cvar[key] = value: unlike the plain local
     * case below, there's no boxed-Cell concern here -- Hash/Array are
     * heap-allocated, mutable-in-place reference objects (confirmed
     * directly: mutating through a freshly loaded register affects the
     * same object @ivar/@@cvar itself still points at), so loading the
     * current value into a fresh register and running DIAMOND_OP_INDEX_SET
     * against that register is enough; there's no separate "write the
     * mutated value back to the ivar/cvar" step. This mirrors exactly how
     * parse_prefix's own DIAMOND_TOKEN_INSTANCE_VARIABLE/CLASS_VARIABLE
     * cases read one as an ordinary expression. */
    if(compiler->current.kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        receiver=allocate_register(compiler);
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,receiver,0,field,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR,receiver,0,(uint8_t)field,3);
        }
    } else if(compiler->current.kind==DIAMOND_TOKEN_CLASS_VARIABLE) {
        receiver=allocate_register(compiler);
        const int slot=class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_GET_CVAR,receiver,
                         (uint8_t)compiler->current_class,(uint8_t)slot,3);
    } else {
        const int local=find_local(compiler,name);
        if(local<0) { fail(compiler,name,"undefined local variable"); return 0; }
        receiver=compiler->locals[(size_t)local].reg;
        if(compiler->locals[(size_t)local].captured) {
            /* See parse_identifier's own BOX_LOCAL re-emission for why this
             * defensive re-box is needed: `captured` doesn't imply this
             * control-flow path actually ran the boxing site. */
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,receiver,0,0,1);
            const uint16_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
            receiver=loaded;
        }
    }
    advance_token(compiler);
    advance_token(compiler);
    uint16_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after assignment index");
        return 0;
    }
    advance_token(compiler);
    /* `x[a][b]... = value`: every `[...]` group before the last one is
     * an ordinary read (INDEX_GET) that produces the *next* receiver --
     * only the final group before `=` becomes the actual assignment
     * target (INDEX_SET). index_assignment_ahead's own lookahead already
     * confirmed a chain shaped exactly like this exists, so this loop
     * always terminates at a real `=`, never falls off the end. */
    while(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        const uint16_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,loaded,receiver,index,3);
        receiver=loaded;
        advance_token(compiler);
        index=parse_expression(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
            fail(compiler,compiler->current.span,"expected ']' after assignment index");
            return 0;
        }
        advance_token(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_EQUAL) {
        fail(compiler,compiler->current.span,"expected '=' after indexed target");
        return 0;
    }
    advance_token(compiler);
    const uint16_t value=parse_expression(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,value,3);
    return value;
}

/* `arr[i] += 1`-shaped indexed compound assignment -- the receiver-
 * loading prologue is a verbatim copy of compile_index_assignment's own
 * just above (identical regardless of what follows the index), and the
 * operator dispatch is the same shape compile_compound_assignment uses
 * below for a plain local/@ivar/@@cvar target (||=/&&= short-circuit via
 * a conditional jump around evaluating the right-hand side at all;
 * +=, -=, *=, /=, and %= all reduce to one compile_binary_op call). The one new
 * concern beyond compile_index_assignment's own is avoiding double-
 * evaluation of the index expression -- solved by keeping the single
 * register parse_expression already returns for it and reusing that
 * same register for both the read (INDEX_GET) and the write back
 * (INDEX_SET), the same way the receiver register is already reused for
 * both today. */
static uint16_t compile_index_compound_assignment(Compiler *compiler) {
    const DiamondSpan name=compiler->current.span;
    uint16_t receiver;
    if(compiler->current.kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        receiver=allocate_register(compiler);
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,receiver,0,field,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR,receiver,0,(uint8_t)field,3);
        }
    } else if(compiler->current.kind==DIAMOND_TOKEN_CLASS_VARIABLE) {
        receiver=allocate_register(compiler);
        const int slot=class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_GET_CVAR,receiver,
                         (uint8_t)compiler->current_class,(uint8_t)slot,3);
    } else {
        const int local=find_local(compiler,name);
        if(local<0) { fail(compiler,name,"undefined local variable"); return 0; }
        receiver=compiler->locals[(size_t)local].reg;
        if(compiler->locals[(size_t)local].captured) {
            /* See compile_index_assignment's own comment on this exact
             * defensive re-box. */
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,receiver,0,0,1);
            const uint16_t loaded=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
            receiver=loaded;
        }
    }
    advance_token(compiler);
    advance_token(compiler);
    const uint16_t index=parse_expression(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
        fail(compiler,compiler->current.span,"expected ']' after assignment index");
        return 0;
    }
    advance_token(compiler);
    const uint16_t left=allocate_register(compiler);
    emit_instruction(compiler,DIAMOND_OP_INDEX_GET,left,receiver,index,3);
    const DiamondTokenKind op_kind=compiler->current.kind;
    advance_token(compiler);
    if(op_kind==DIAMOND_TOKEN_OR_OR_EQUAL||op_kind==DIAMOND_TOKEN_AND_AND_EQUAL) {
        const bool is_and=op_kind==DIAMOND_TOKEN_AND_AND_EQUAL;
        const uint16_t destination=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
        const size_t end_jump=emit_jump(compiler,
            is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
        const uint16_t right=parse_expression(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
        patch_jump(compiler,end_jump,compiler->function->code_count);
        emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,destination,3);
        return destination;
    }
    const DiamondTokenKind plain_op=
        op_kind==DIAMOND_TOKEN_PLUS_EQUAL?DIAMOND_TOKEN_PLUS:
        op_kind==DIAMOND_TOKEN_MINUS_EQUAL?DIAMOND_TOKEN_MINUS:
        op_kind==DIAMOND_TOKEN_STAR_EQUAL?DIAMOND_TOKEN_STAR:
        op_kind==DIAMOND_TOKEN_SLASH_EQUAL?DIAMOND_TOKEN_SLASH:
        DIAMOND_TOKEN_PERCENT;
    const uint16_t right=parse_expression(compiler);
    const uint16_t destination=compile_binary_op(compiler,plain_op,left,right);
    emit_instruction(compiler,DIAMOND_OP_INDEX_SET,receiver,index,destination,3);
    return destination;
}

static uint16_t compile_return(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    if(!compiler->in_function) {
        fail(compiler,keyword,"'return' used outside a function");
        return 0;
    }
    advance_token(compiler);
    uint16_t value=0;
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
        value=compiler->current_return_type>=0?
            parse_with_expected_set(compiler,
                (uint16_t)compiler->current_return_type):
            parse_expression(compiler);
    }
    if(compiler->current_return_type>=0)
        emit_type_check(compiler,value,(uint8_t)compiler->current_return_type,
                        compiler->current_return_type_span);
    emit_instruction(compiler,DIAMOND_OP_RETURN,value,0,0,1);
    return value;
}

static uint16_t compile_yield(Compiler *compiler) {
    if(compiler->has_current_block) {
        const uint16_t block=load_current_block(compiler);
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,block,
            callable_type_set_index(compiler),0,2);
        uint16_t destination=0;
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_PAREN) {
            destination=parse_closure_call_arguments(compiler,block);
        } else {
            const uint16_t base=allocate_register(compiler);
            destination=allocate_register(compiler);
            emit_opcode(compiler,DIAMOND_OP_CALL_CLOSURE);
            emit_register(compiler,destination);
            emit_register(compiler,block);
            emit_register(compiler,base);emit_byte(compiler,0);
        }
        const uint16_t block_set=compiler->current_block_type_set;
        if(block_set!=DIAMOND_NO_TYPE_SET&&
           block_set<compiler->function->type_set_count) {
            const DiamondTypeSet *set=&compiler->function->type_sets[block_set];
            uint16_t return_set=DIAMOND_NO_TYPE_SET;
            bool consistent=set->count>0;
            for(size_t index=0;index<set->count;index++) {
                const DiamondTypeMember *member=&set->members[index];
                if(member->id!=DIAMOND_TYPE_CALLABLE||
                   member->callable_return_set==DIAMOND_NO_TYPE_SET) {
                    consistent=false;break;
                }
                if(return_set==DIAMOND_NO_TYPE_SET)
                    return_set=member->callable_return_set;
                else if(return_set!=member->callable_return_set) {
                    consistent=false;break;
                }
            }
            if(consistent&&return_set<compiler->function->type_set_count) {
                compiler->known_type_sets[destination]=(int32_t)return_set;
                const DiamondTypeSet *returns=
                    &compiler->function->type_sets[return_set];
                if(returns->count==1)
                    compiler->known_types[destination]=returns->members[0].id;
            }
        }
        return destination;
    }
    uint16_t source;
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
    const uint16_t dest = allocate_register(compiler);
    emit_instruction(compiler, DIAMOND_OP_YIELD, dest, source, 0, 2);
    return dest;
}

static uint16_t compile_raise(Compiler *compiler) {
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
    const uint16_t value=parse_expression(compiler);
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
        recorded->reg=local->reg;
        recorded->known_type=compiler->known_types[local->reg];
        recorded->known_type_set=compiler->known_type_sets[local->reg];
    }
}

static void record_scope_type_fact(Compiler *compiler,uint16_t reg,
        size_t effective_start) {
    DiamondFunction *function=compiler->function;
    if(function->scope_type_fact_count==DIAMOND_MAX_SCOPE_TYPE_FACTS)return;
    DiamondScopeTypeFact *fact=
        &function->scope_type_facts[function->scope_type_fact_count++];
    *fact=(DiamondScopeTypeFact){.reg=reg,.effective_start=effective_start,
        .known_type=compiler->known_types[reg],
        .known_type_set=compiler->known_type_sets[reg]};
}

static uint16_t compile_begin(Compiler *compiler) {
    if(!consume_block_start(compiler))return 0;
    const size_t ensure_operand=compiler->function->code_count+1;
    emit_opcode(compiler,DIAMOND_OP_PUSH_ENSURE);
    emit_byte(compiler,0);emit_byte(compiler,0);
    const uint16_t exception=allocate_register(compiler);
    const size_t retry_target=compiler->function->code_count;
    /* Byte layout after PUSH_RESCUE's opcode: exception (2-byte register,
     * emit_register below), then this 1-byte catch-all flag, then 8
     * 1-byte rescue-type-id slots, then a 2-byte jump-target placeholder
     * -- opcode(+0) + exception(+1,+2) puts the flag at +3 and the jump
     * placeholder at +3+1+8=+12. */
    const size_t handler_type_operand=compiler->function->code_count+3;
    const size_t handler_operand=compiler->function->code_count+12;
    emit_opcode(compiler,DIAMOND_OP_PUSH_RESCUE);
    emit_register(compiler,exception);emit_byte(compiler,0x80);
    for(size_t i=0;i<8;i++)emit_byte(compiler,0);
    emit_byte(compiler,0);emit_byte(compiler,0);
    const uint16_t body=compile_sequence(compiler);
    const uint16_t destination=allocate_register(compiler);
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
                const DiamondSpan rescue_type_span=compiler->current.span;
                char rescue_type_name[DIAMOND_MAX_FUNCTION_NAME];
                if(!consume_qualified_name(compiler,rescue_type_name,
                                           sizeof rescue_type_name)) {
                    fail(compiler,rescue_type_span,"rescue type name is too long");break;
                }
                const uint8_t rescue_type=(uint8_t)resolve_type_name(
                    compiler,rescue_type_name,rescue_type_span);
                for(size_t existing=0;existing<type_count;existing++)
                    if(rescue_types[existing]==rescue_type)
                        fail(compiler,rescue_type_span,
                             "duplicate rescue type");
                for(size_t existing=0;existing<seen_rescue_type_count;existing++)
                    if(seen_rescue_types[existing]==rescue_type)
                        fail(compiler,rescue_type_span,
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
                            fail(compiler,rescue_type_span,
                                 "rescue type is covered by an earlier clause");
                    }
                rescue_types[type_count++]=rescue_type;
                seen_rescue_types[seen_rescue_type_count++]=rescue_type;
                if(compiler->current.kind!=DIAMOND_TOKEN_PIPE)break;
                advance_token(compiler);
            }
        }
        size_t match_jumps[8];size_t mismatch_jump=SIZE_MAX;
        for(size_t index=0;index<type_count;index++) {
            const uint16_t matched=allocate_register(compiler);
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
        const uint16_t rescued=compile_sequence(compiler);
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
        const uint16_t normal=compile_sequence(compiler);
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

static uint16_t compile_retry(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current_retry_target==SIZE_MAX) {
        fail(compiler,keyword,"'retry' used outside rescue");return 0;
    }
    emit_absolute_jump(compiler,compiler->current_retry_target);
    const uint16_t result=allocate_register(compiler);
    /* Dead code: the unconditional jump above means this NIL would
     * never execute at runtime, on top of being a sole-writer register
     * run_chunk's zero-init already covers. */
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}

static uint16_t compile_loop_control(Compiler *compiler) {
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
        uint8_t break_type=DIAMOND_TYPE_NIL;int32_t break_set=-1;
        if(actual_value) {
            const uint16_t value=parse_expression(compiler);
            emit_instruction(compiler,DIAMOND_OP_MOVE,
                             compiler->current_loop->result_register,value,0,2);
            break_type=compiler->known_types[value];
            break_set=compiler->known_type_sets[value];
        }
        merge_loop_exit(compiler,compiler->current_loop,break_type,break_set);
        compiler->current_loop->breaks[compiler->current_loop->break_count++]=
            emit_jump(compiler,DIAMOND_OP_JUMP,0);
    } else if(kind==DIAMOND_TOKEN_NEXT) {
        emit_absolute_jump(compiler,compiler->current_loop->continue_target);
    } else {
        emit_absolute_jump(compiler,compiler->current_loop->redo_target);
    }
    const uint16_t result=allocate_register(compiler);
    /* Dead code: break/next/redo all jump unconditionally above, so
     * this NIL never executes -- also a sole-writer register run_chunk's
     * zero-init already covers even if it somehow did. */
    compiler->known_types[result]=DIAMOND_TYPE_NIL;
    return result;
}

/* `do |param, param, ...| BODY end` attached right after a call's closing
 * `)` -- see docs/roadmap.md's design writeup for the full rationale.
 * Deliberately a separate, much smaller function rather than a refactor
 * of compile_definition (below): that function carries a lot of ceremony
 * that doesn't apply to an anonymous block (operator-method names,
 * module-singleton `self.` binding, class/module member registration,
 * return-type annotations, docstrings, default parameter values, the
 * endless `def foo() = expr` form) and has several hard-won, comment-
 * documented correctness fixes (e.g. the redefine_method patch-factory
 * nested_in_singleton_method exception below) not worth risking by
 * routing an unrelated caller through it. What *is* reused is the
 * underlying mechanism: a block is compiled into a real new
 * DiamondFunction slot and produces a real, capturing DIAMOND_OP_CLOSURE
 * value -- the exact same runtime shape a nested `def` already produces
 * -- so this function mirrors only the subset of compile_definition that
 * builds that shape (save/restore outer compiler state, bind parameters
 * as locals, eagerly materialize every enclosing local as a directly
 * accessible local via GET_CAPTURE_CELL, compile the body, then BOX_LOCAL
 * + CLOSURE). No new DiamondOpCode. Caller has already confirmed
 * compiler->current.kind == DIAMOND_TOKEN_DO and not yet consumed it. */
static uint16_t compile_block(Compiler *compiler) {
    const bool has_contextual_types=compiler->has_contextual_block_types;
    const uint8_t contextual_arity=compiler->contextual_block_arity;
    uint8_t contextual_types[16];
    int32_t contextual_type_sets[16];
    for(size_t index=0;index<contextual_arity;index++) {
        contextual_types[index]=compiler->contextual_block_types[index];
        contextual_type_sets[index]=compiler->contextual_block_type_sets[index];
    }
    const int32_t contextual_return_set=compiler->contextual_block_return_set;
    compiler->has_contextual_block_types=false;
    compiler->contextual_block_arity=0;
    compiler->contextual_block_return_set=-1;
    advance_token(compiler);
    if (compiler->program->function_count == DIAMOND_MAX_FUNCTIONS) {
        fail(compiler, compiler->current.span, "too many functions");
        return 0;
    }
    size_t function_index=0;
    DiamondFunction *function=compiler_add_function(compiler,&function_index);
    if(function==nullptr) {
        fail(compiler,compiler->current.span,"out of memory");
        return 0;
    }
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<16;index++)
        function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
    function->owner_class=UINT8_MAX;
    function->nested=true;
    static const char block_name[]="<block>";
    for(size_t index=0;index<sizeof(block_name);index++)
        function->name[index]=block_name[index];
    function->declaration_line=(uint32_t)compiler->previous.span.line;
    function->declaration_column=(uint32_t)compiler->previous.span.column;
    function->declaration_start=compiler->previous.span.start;

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
    const bool outer_has_current_block=compiler->has_current_block;
    const uint16_t outer_current_block_register=
        compiler->current_block_register;
    const uint16_t outer_current_block_type_set=
        compiler->current_block_type_set;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
    LoopContext *outer_loop=compiler->current_loop;
    Local outer_enclosing_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_enclosing_local_count=compiler->enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        outer_enclosing_locals[i]=compiler->enclosing_locals[i];
    uint16_t outer_capture_registers[16];
    const size_t outer_capture_count=compiler->capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        outer_capture_registers[i]=compiler->capture_registers[i];
    /* Same inline-then-heap fallback as parse_if/compile_definition's own
     * (see either's comment) -- necessary here too, for the same reason:
     * a block can appear after arbitrarily many registers have already
     * been allocated in the enclosing function body. */
    uint8_t inline_outer_known_types[256];int32_t inline_outer_known_type_sets[256];
    uint8_t *outer_known_types=inline_outer_known_types;
    int32_t *outer_known_type_sets=inline_outer_known_type_sets;
    uint8_t *heap_outer_known_types=nullptr;int32_t *heap_outer_known_type_sets=nullptr;
    if(outer_next_register>256) {
        heap_outer_known_types=malloc((size_t)outer_next_register*sizeof(uint8_t));
        heap_outer_known_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        if(heap_outer_known_types==nullptr||heap_outer_known_type_sets==nullptr) {
            fail(compiler,compiler->previous.span,"out of memory compiling block");
            free(heap_outer_known_types);free(heap_outer_known_type_sets);
            return 0;
        }
        outer_known_types=heap_outer_known_types;
        outer_known_type_sets=heap_outer_known_type_sets;
    }
    for(size_t index=0;index<outer_next_register;index++)
        {outer_known_types[index]=compiler->known_types[index];
         outer_known_type_sets[index]=compiler->known_type_sets[index];}

    compiler->function = function;
    uint16_t declared_return_set=DIAMOND_NO_TYPE_SET;
    if(contextual_return_set>=0&&
       (size_t)contextual_return_set<outer_function->type_set_count)
        declared_return_set=clone_type_set_into_current(compiler,
            outer_function->type_sets,outer_function->type_set_count,
            (uint16_t)contextual_return_set);
    compiler->has_current_block=false;
    compiler->current_block_register=0;
    compiler->current_block_type_set=DIAMOND_NO_TYPE_SET;
    compiler->current_loop=nullptr;
    compiler->current_exception=-1;
    compiler->current_retry_target=SIZE_MAX;
    compiler->local_count = 0;
    compiler->next_register = 0;
    compiler->enclosing_local_count=outer_local_count;
    for(size_t i=0;i<compiler->enclosing_local_count;i++)
        compiler->enclosing_locals[i]=outer_locals[i];
    compiler->capture_count=0;
    if(compiler->enclosing_local_count>16) {
        fail(compiler,compiler->previous.span,"block sees too many lexical bindings");
    } else {
        compiler->capture_count=compiler->enclosing_local_count;
        for(size_t i=0;i<compiler->capture_count;i++)
            compiler->capture_registers[i]=compiler->enclosing_locals[i].reg;
    }

    /* `|x, y|` -- bare identifiers only, no type annotations, no default
     * values (deliberate v1 scope cut, see this feature's own design
     * doc). Absent entirely (`do ... end`) means a genuine zero-arity
     * block -- correct as-is, not a gap: calling it with an argument
     * raises the same "wrong number of arguments" any arity mismatch
     * already does, no Ruby-style silent leniency to replicate. */
    if(!compiler->failed&&compiler->current.kind==DIAMOND_TOKEN_PIPE) {
        advance_token(compiler);
        skip_newlines(compiler);
        if(compiler->current.kind!=DIAMOND_TOKEN_PIPE) {
            size_t parameter_count=0;
            do {
                if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                    fail(compiler,compiler->current.span,"expected block parameter name");
                    break;
                }
                if(function->arity==UINT8_MAX||parameter_count==16) {
                    fail(compiler,compiler->current.span,"too many block parameters");
                    break;
                }
                if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                    fail(compiler,compiler->current.span,"too many local variables");break;
                }
                const uint16_t parameter=allocate_register(compiler);
                if(has_contextual_types&&parameter_count<contextual_arity&&
                   contextual_type_sets[parameter_count]>=0&&
                   (size_t)contextual_type_sets[parameter_count]<
                       outer_function->type_set_count) {
                    const uint16_t cloned=clone_type_set_into_current(compiler,
                        outer_function->type_sets,outer_function->type_set_count,
                        (uint16_t)contextual_type_sets[parameter_count]);
                    if(cloned!=DIAMOND_NO_TYPE_SET) {
                        compiler->known_type_sets[parameter]=(int32_t)cloned;
                        function->parameter_type_sets[parameter_count]=cloned;
                    }
                }
                if(has_contextual_types&&parameter_count<contextual_arity&&
                   contextual_types[parameter_count]!=TYPE_UNKNOWN) {
                    compiler->known_types[parameter]=
                        contextual_types[parameter_count];
                    /* The contextual type is part of the block's callable
                     * signature as well as a body-local inference fact.
                     * Dynamic invocation validates typed Callable arguments
                     * from this metadata, so omitting it would compile a
                     * well-typed body and then reject its closure at runtime. */
                    if(function->parameter_type_sets[parameter_count]==
                           DIAMOND_NO_TYPE_SET&&reserve_type_sets(compiler,1)) {
                        const size_t set_index=function->type_set_count++;
                        DiamondTypeSet *set=&function->type_sets[set_index];
                        set->count=1;set->inferred=true;
                        set->members[0]=(DiamondTypeMember){
                            .id=contextual_types[parameter_count],
                            .argument_set=DIAMOND_NO_TYPE_SET,
                            .second_argument_set=DIAMOND_NO_TYPE_SET,
                            .callable_arity=UINT8_MAX,
                            .callable_return_set=DIAMOND_NO_TYPE_SET,
                            .callable_parameters_typed=false};
                        for(size_t index=0;index<16;index++)
                            set->members[0].callable_parameter_sets[index]=
                                DIAMOND_NO_TYPE_SET;
                        function->parameter_type_sets[parameter_count]=
                            (uint16_t)set_index;
                    }
                }
                compiler->locals[compiler->local_count++]=(Local){
                    .name=compiler->current.span,.reg=parameter};
                record_scope_type_fact(compiler,parameter,compiler->current.span.start);
                const DiamondSpan parameter_name_span=compiler->current.span;
                size_t parameter_name_length=parameter_name_span.length;
                if(parameter_name_length>=DIAMOND_MAX_FUNCTION_NAME)
                    parameter_name_length=DIAMOND_MAX_FUNCTION_NAME-1;
                for(size_t index=0;index<parameter_name_length;index++)
                    function->parameter_names[parameter_count][index]=
                        compiler->source[parameter_name_span.start+index];
                function->parameter_names[parameter_count][parameter_name_length]='\0';
                function->arity++;
                function->required_arity++;
                parameter_count++;
                advance_token(compiler);
                skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
                advance_token(compiler);
                skip_newlines(compiler);
            } while(!compiler->failed);
        }
        if(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_PIPE) {
            fail(compiler,compiler->current.span,"expected '|' after block parameters");
        } else if(!compiler->failed) {
            advance_token(compiler);
        }
    }

    /* Eager capture materialization: every enclosing local not shadowed
     * by a block parameter of the same name becomes a directly-accessible
     * local right away, same as compile_definition's own nested-def
     * handling below. */
    if(!compiler->failed) {
        for(size_t i=0;i<compiler->enclosing_local_count;i++) {
            if(find_local(compiler,compiler->enclosing_locals[i].name)>=0)continue;
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->enclosing_locals[i].name,
                     "too many lexical bindings");break;
            }
            const uint16_t cell=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE_CELL,cell,(uint8_t)i,0,2);
            compiler->locals[compiler->local_count++]=(Local){
                .name=compiler->enclosing_locals[i].name,.reg=cell,.captured=true};
        }
    }

    compiler->in_function=true;
    const uint16_t body_result=compiler->failed?0:compile_sequence(compiler);
    if(!compiler->failed) {
        if(declared_return_set!=DIAMOND_NO_TYPE_SET) {
            emit_type_check(compiler,body_result,declared_return_set,
                compiler->previous.span);
            function->return_type_set=declared_return_set;
        } else if(compiler->known_type_sets[body_result]>=0) {
            function->return_type_set=
                (uint16_t)compiler->known_type_sets[body_result];
        } else if(compiler->known_types[body_result]!=TYPE_UNKNOWN&&
                  reserve_type_sets(compiler,1)) {
            const size_t return_set=function->type_set_count++;
            DiamondTypeSet *set=&function->type_sets[return_set];
            set->count=1;set->inferred=true;
            set->members[0]=(DiamondTypeMember){
                .id=compiler->known_types[body_result],
                .argument_set=DIAMOND_NO_TYPE_SET,
                .second_argument_set=DIAMOND_NO_TYPE_SET,
                .callable_arity=UINT8_MAX,
                .callable_return_set=DIAMOND_NO_TYPE_SET,
                .callable_parameters_typed=false};
            for(size_t parameter=0;parameter<16;parameter++)
                set->members[0].callable_parameter_sets[parameter]=
                    DIAMOND_NO_TYPE_SET;
            function->return_type_set=(uint16_t)return_set;
        }
        emit_instruction(compiler,DIAMOND_OP_RETURN,body_result,0,0,1);
        if(compiler->current.kind!=DIAMOND_TOKEN_END) {
            fail(compiler,compiler->current.span,"expected 'end' after block body");
        } else {
            advance_token(compiler);
        }
    }

    function->capture_count=(uint8_t)compiler->capture_count;
    function->register_count=compiler->next_register;
    uint16_t captures[16];
    for(size_t i=0;i<compiler->capture_count;i++)captures[i]=compiler->capture_registers[i];
    const size_t capture_count=compiler->capture_count;
    if(!compiler->failed) {
        const size_t body_end=compiler->previous.span.start+compiler->previous.span.length;
        record_scope_locals(compiler,0,compiler->local_count,body_end);
        function->body_end=body_end;
    }

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
    compiler->has_current_block=outer_has_current_block;
    compiler->current_block_register=outer_current_block_register;
    compiler->current_block_type_set=outer_current_block_type_set;
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
    for(size_t index=0;index<outer_next_register;index++)
        {compiler->known_types[index]=outer_known_types[index];
         compiler->known_type_sets[index]=outer_known_type_sets[index];}
    free(heap_outer_known_types);free(heap_outer_known_type_sets);

    /* BOX_LOCAL + CLOSURE, emitted into the *outer* (caller's) bytecode
     * now that compiler->function/locals have been restored -- same
     * shape compile_definition ends with. */
    for(size_t i=0;i<capture_count;i++) {
        for(size_t local=0;local<compiler->local_count;local++) {
            if(compiler->locals[local].reg!=captures[i])continue;
            emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,captures[i],0,0,1);
            compiler->locals[local].captured=true;
            break;
        }
    }
    const uint16_t result=allocate_register(compiler);
    emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
    emit_function_index(compiler,function_index);emit_byte(compiler,(uint8_t)capture_count);
    for(size_t i=0;i<capture_count;i++)emit_register(compiler,captures[i]);
    compiler->known_types[result]=TYPE_UNKNOWN;
    return result;
}

static uint16_t compile_definition(Compiler *compiler, bool captures_self) {
    const bool at_top_level = compiler->function == &compiler->program->entry;
    if(captures_self && !compiler->in_method) {
        fail(compiler,compiler->current.span,
             "closure requires an enclosing method -- no 'self' is available here");
        return 0;
    }
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
    bool operator_name =
        compiler->current.kind==DIAMOND_TOKEN_PLUS||
        compiler->current.kind==DIAMOND_TOKEN_MINUS||
        compiler->current.kind==DIAMOND_TOKEN_STAR||
        compiler->current.kind==DIAMOND_TOKEN_SLASH||
        compiler->current.kind==DIAMOND_TOKEN_PERCENT||
        compiler->current.kind==DIAMOND_TOKEN_EQUAL_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_LESS||
        compiler->current.kind==DIAMOND_TOKEN_LESS_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_GREATER||
        compiler->current.kind==DIAMOND_TOKEN_GREATER_EQUAL||
        compiler->current.kind==DIAMOND_TOKEN_SPACESHIP;
    /* `[]`/`[]=` -- the one operator-overload shape that isn't a single
     * lexer token, so unlike every case above it needs its own
     * multi-token lookahead/consumption here, not just a membership
     * check. `[` has no other legal meaning in "expecting a method name"
     * position (unlike right *after* a name, where DIAMOND_TOKEN_LEFT_
     * BRACKET already means generic type parameters -- see the `def
     * name[T](...)` handling below; that check runs strictly later, on
     * whatever follows the name this block already consumed, so the two
     * never conflict), so this can commit to consuming eagerly on `[`
     * rather than needing to backtrack. Requires `[`/`]` (and `=`, for
     * the setter) strictly adjacent -- no whitespace -- matching every
     * other operator name here being a single, ungappable token; `def [
     * ](i)` or `def [] =(i, v)` fall through to the ordinary "expected
     * function name" error below, same as any other malformed name. */
    bool is_index_operator=false;
    DiamondSpan index_operator_name={};
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
        const DiamondSpan left=compiler->current.span;
        DiamondLexer lookahead=compiler->lexer;
        const DiamondToken right=diamond_lexer_next(&lookahead);
        if(right.kind==DIAMOND_TOKEN_RIGHT_BRACKET&&
           right.span.start==left.start+left.length) {
            is_index_operator=true;
            advance_token(compiler);
            advance_token(compiler);
            size_t total_length=right.span.start+right.span.length-left.start;
            if(compiler->current.kind==DIAMOND_TOKEN_EQUAL&&
               compiler->current.span.start==right.span.start+right.span.length) {
                total_length+=compiler->current.span.length;
                advance_token(compiler);
            }
            index_operator_name=(DiamondSpan){.start=left.start,
                .length=total_length,.line=left.line,.column=left.column};
            operator_name=true;
        }
    }
    if (compiler->current.kind != DIAMOND_TOKEN_IDENTIFIER && !operator_name) {
        fail(compiler, compiler->current.span, "expected function name after 'def'");
        return 0;
    }
    /* A module's own operator method is inert until some class `include`s
     * it (a bare module isn't itself an instantiable receiver), but once
     * included it's copied into the including class's own method table
     * exactly like any other module method (see the `include` handling
     * below) -- dispatch already doesn't care where a method was
     * originally *written*, only which class ends up owning it, so
     * there's nothing else to teach the VM here. This is what lets
     * `module Comparable`'s own `<`/`<=`/`>`/`>=` derive from a `<=>` an
     * including class defines, the same relationship `Enumerable`
     * already has with `each`. */
    if (operator_name &&
        !((compiler->current_class>=0||compiler->current_module>=0) &&
          !module_singleton)) {
        fail(compiler, compiler->current.span,
             "operator methods can only be defined inside a class or module");
        return 0;
    }
    const DiamondSpan name = is_index_operator?index_operator_name:compiler->current.span;
    if (name.length >= DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler, name, "function name is too long");
        return 0;
    }
    const bool genuine_top_level=at_top_level&&compiler->current_class<0&&
        compiler->current_module<0&&!module_singleton;
    const bool claiming_discovered=!compiler->discovery_pass&&
        compiler->next_function_claim<compiler->program->function_count&&
        compiler->program->functions[compiler->next_function_claim]
            ->declared_by_discovery;
    if(!compiler->program->allow_top_level_redefinition&&genuine_top_level&&
       find_function(compiler,name)>=0&&!claiming_discovered) {
        fail(compiler, name, "function is already defined");
        return 0;
    }
    size_t function_index=0;
    DiamondFunction *function=compiler_add_function(compiler,&function_index);
    if(function==nullptr) {
        fail(compiler,name,"out of memory");
        return 0;
    }
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<16;index++)
        function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
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
    /* `def self.x` directly inside a *class* (not a module -- modules have
     * no superclass chain and nothing to virtually dispatch against, so
     * this deliberately excludes compiler->current_module). Gains the same
     * owner_class/implicit-self treatment direct_class_member already
     * gets below, so `self` becomes usable inside the singleton method's
     * own body and external calls can populate it with the literal
     * receiver class (see DIAMOND_OP_LOAD_CLASS's own comment). */
    const bool direct_class_singleton_member=
        at_top_level&&compiler->current_class>=0&&module_singleton;
    /* owner_class serves two genuinely different purposes bundled into
     * one field: (1) "this function is a real class/module method,
     * eligible for redefine_method/define_method" -- checked as an
     * *exact* value match against a real class index
     * (DIAMOND_OP_REDEFINE_METHOD's own `new_function->owner_class!=
     * class_operand`, src/vm.c) -- and (2) "register 0 is already
     * spoken for, every calling convention that places arguments must
     * skip it" (parameter_offset, computed everywhere in src/vm.c as a
     * plain `fn->owner_class==UINT8_MAX?0:1` binary check -- confirmed
     * nowhere in this codebase tests for one *specific* non-UINT8_MAX
     * value except the assignment sites themselves, so any non-UINT8_MAX
     * sentinel gets (2) for free with no other call site changes).
     *
     * A `closure` (captures_self) needs (2) without (1): it reserves
     * register 0 itself, internally, for the materialized captured self
     * (see the captures_self handling below), so calls to it must still
     * skip register 0 the same way a real method's implicit receiver
     * already does -- confirmed the hard way as a real bug: leaving
     * owner_class at UINT8_MAX here computed parameter_offset=0,
     * silently colliding a closure's own first *declared* parameter
     * with its self-capture register. But it must never satisfy (1) --
     * it's never a patch factory, never installable via redefine_method/
     * define_method. A third sentinel, distinct from UINT8_MAX (no
     * owner_class at all) and UINT8_MAX-1 (the existing module-method
     * sentinel, diamond_compile's own convention, comfortably above
     * DIAMOND_MAX_CLASSES=180's real range either way), gives it (2)
     * while permanently failing (1)'s exact-match check.
     *
     * One more site needed a matching fix, not just this one:
     * call_closure_helper's own needs_self_slot padding (src/vm.c) --
     * used by CALL_CLOSURE and tap's block-invocation sites, the only
     * ways a Callable *value* (as opposed to ordinary method dispatch)
     * ever actually runs -- inflates its own arity-bounds check by one
     * for *any* owner_class!=UINT8_MAX function, correct for a genuine
     * method's real implicit-receiver argument but wrong here too: this
     * sentinel's whole point is that nothing external ever supplies
     * register 0's value, so the bounds check must stay un-inflated,
     * matching this closure's own true, undistorted declared arity. */
    function->owner_class=
        (direct_class_member||direct_class_singleton_member||
         (nested_in_singleton_method&&!captures_self&&compiler->current_class>=0))?
            (uint8_t)compiler->current_class:
        (direct_module_member||(nested_in_singleton_method&&!captures_self&&compiler->current_module>=0))?
            UINT8_MAX-1:
        captures_self?UINT8_MAX-2:UINT8_MAX;
    function->nested=!at_top_level;
    size_t copy_length=name.length;
    for (size_t index = 0; index < copy_length; index++) {
        function->name[index] = compiler->source[name.start + index];
    }
    function->name[copy_length] = '\0';
    function->declaration_line=(uint32_t)name.line;
    function->declaration_column=(uint32_t)name.column;
    function->declaration_start=name.start;
    /* is_index_operator already consumed every token of its own name
     * ("[]"/"[]=") above -- nothing left to advance past here, unlike
     * every other name shape (a single token, always consumed by this
     * one advance_token). */
    if(!is_index_operator) advance_token(compiler);
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
    const bool outer_has_current_block=compiler->has_current_block;
    const uint16_t outer_current_block_register=
        compiler->current_block_register;
    const uint16_t outer_current_block_type_set=
        compiler->current_block_type_set;
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
    LoopContext *outer_loop=compiler->current_loop;
    Local outer_enclosing_locals[DIAMOND_MAX_LOCALS];
    const size_t outer_enclosing_local_count=compiler->enclosing_local_count;
    for(size_t i=0;i<outer_enclosing_local_count;i++)
        outer_enclosing_locals[i]=compiler->enclosing_locals[i];
    uint16_t outer_capture_registers[16];
    const size_t outer_capture_count=compiler->capture_count;
    for(size_t i=0;i<outer_capture_count;i++)
        outer_capture_registers[i]=compiler->capture_registers[i];
    /* Same inline-then-heap fallback as parse_if's before_types/then_types
     * (see that function's own comment) -- this one had a different bug
     * shape: a fixed `index<256` bound (not indexed by outer_next_register)
     * doesn't overflow, but silently under-saves/-restores whenever the
     * enclosing scope already has more than 256 live registers, leaving
     * entries [256, outer_next_register) holding whatever the nested def's
     * own (unrelated, register-index-0-based) body happened to write into
     * those same slots after this function returns. */
    uint8_t inline_outer_known_types[256];int32_t inline_outer_known_type_sets[256];
    uint8_t *outer_known_types=inline_outer_known_types;
    int32_t *outer_known_type_sets=inline_outer_known_type_sets;
    uint8_t *heap_outer_known_types=nullptr;int32_t *heap_outer_known_type_sets=nullptr;
    if(outer_next_register>256) {
        heap_outer_known_types=malloc((size_t)outer_next_register*sizeof(uint8_t));
        heap_outer_known_type_sets=
            malloc((size_t)outer_next_register*sizeof(int32_t));
        if(heap_outer_known_types==nullptr||heap_outer_known_type_sets==nullptr) {
            fail(compiler,compiler->previous.span,
                 "out of memory compiling function definition");
            free(heap_outer_known_types);free(heap_outer_known_type_sets);
            return 0;
        }
        outer_known_types=heap_outer_known_types;
        outer_known_type_sets=heap_outer_known_type_sets;
    }
    for(size_t index=0;index<outer_next_register;index++)
        {outer_known_types[index]=compiler->known_types[index];
         outer_known_type_sets[index]=compiler->known_type_sets[index];}
    /* Copy self into a fresh register of the *enclosing* function and box
     * that copy, rather than boxing register 0 (self's own real home)
     * in place the way an ordinary captured Local already does. Every
     * other self/@ivar access in the enclosing method hardcodes literal
     * register 0 unconditionally (see parse_prefix's DIAMOND_TOKEN_SELF/
     * INSTANCE_VARIABLE cases and compile_assignment_store) -- none of
     * them check a `.captured`-style flag the way an ordinary Local read
     * does, so boxing register 0 itself would silently corrupt every
     * other self/@ivar access in this same method that runs after this
     * point, turning a raw Instance/Class value into a Cell those sites
     * never expect. Capturing an isolated copy instead leaves register 0
     * completely untouched. */
    uint16_t self_copy_register=0;
    if(captures_self) {
        self_copy_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,self_copy_register,0,0,2);
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,self_copy_register,0,0,1);
    }
    compiler->function = function;
    compiler->has_current_block=false;
    compiler->current_block_register=0;
    compiler->current_block_type_set=DIAMOND_NO_TYPE_SET;
    /* A captures_self closure nested *directly* inside a `def self.x`
     * method also needs in_singleton_method true, for exactly the same
     * reason nested_in_singleton_method's own existing arm below does:
     * self.foo(...) inside its body must resolve through
     * parse_self_class_method_call/DIAMOND_OP_INVOKE_SELF_METHOD, not an
     * ordinary instance-shaped call -- and that opcode reads its class
     * receiver from register 0 unconditionally (confirmed directly in
     * src/vm.c, not just a compiler convention the way GET_IVAR's own
     * receiver operand is), which is exactly the register this closure's
     * own self-capture materializes into below. Nested any deeper than
     * directly inside the singleton method gets neither, the same
     * pre-existing depth-1-only limit nested_in_singleton_method already
     * has for the patch-factory case (see the class_operand comment
     * above `direct_class_member`). */
    compiler->in_singleton_method =
        (at_top_level && module_singleton) ||
        (captures_self && nested_in_singleton_method);
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
    if((direct_class_member||direct_module_member||direct_class_singleton_member||
        nested_in_singleton_method)&&!captures_self) {
        (void)allocate_register(compiler);
        function->arity = 1;
        function->required_arity=1;
        compiler->current_method = name;
        compiler->in_method = true;
    } else if(captures_self) {
        /* self arrives entirely via capture, not an implicit-receiver
         * call convention -- no extra arity (a closure is called with
         * exactly the arguments its own parameter list declares). But
         * register 0 *is* reserved here, unlike a plain nested def:
         * every ordinary self/@ivar/self.foo() code path (parse_prefix's
         * DIAMOND_TOKEN_SELF/INSTANCE_VARIABLE, compile_assignment_store,
         * DIAMOND_OP_INVOKE_SELF_METHOD's own VM handler) hardcodes
         * literal register 0 unconditionally and unchanged -- reusing
         * all of that as-is (rather than teaching each one about a
         * capture index) means this closure's own register 0 must
         * actually hold self, materialized here via GET_CAPTURE, exactly
         * once, before any of its own body compiles. Guaranteed to land
         * in register 0: next_register was just reset to 0 above and
         * nothing else has allocated from it yet on this path. */
        if(compiler->capture_count==16) {
            fail(compiler,name,"nested function sees too many lexical bindings");
        } else {
            const size_t self_capture_index=compiler->capture_count;
            compiler->capture_registers[compiler->capture_count++]=self_copy_register;
            const uint16_t self_register=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_GET_CAPTURE,self_register,
                             (uint8_t)self_capture_index,0,2);
        }
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
    const uint16_t parameter_base=compiler->next_register;
    for(size_t index=0;index<parameter_count;index++)(void)allocate_register(compiler);
    size_t declared_parameter_count=0;bool saw_default=false;
    bool has_block_parameter=false;
    uint16_t variadic_parameter=0,variadic_fixed_count=0;
    int block_parameter_type=-1;DiamondSpan block_parameter_type_span={};
    skip_newlines(compiler);
    if (compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        do {
            /* `*name` -- a trailing variadic parameter, bare name only
             * (no type annotation, no default -- neither makes sense for
             * a collected Array), and must be the last parameter (no
             * comma may follow it). See docs/design.md's "Splat/variadic
             * parameters" section. */
            bool is_variadic=false;
            bool is_block_parameter=false;
            if(compiler->current.kind==DIAMOND_TOKEN_AMPERSAND) {
                if(has_block_parameter) {
                    fail(compiler,compiler->current.span,
                         "a function can only declare one block parameter");break;
                }
                is_block_parameter=true;has_block_parameter=true;
                advance_token(compiler);
            }
            if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
                if(function->has_variadic) {
                    fail(compiler,compiler->current.span,
                         "a function can only declare one variadic parameter");
                    break;
                }
                is_variadic=true;
                advance_token(compiler);
            }
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
            const uint16_t parameter=(uint16_t)(parameter_base+declared_parameter_count);
            if(is_block_parameter) {
                compiler->has_current_block=true;
                compiler->current_block_register=parameter;
            }
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
            if(is_variadic) {
                if(compiler->current.kind==DIAMOND_TOKEN_COLON) {
                    fail(compiler,compiler->current.span,
                         "a variadic parameter cannot have a type annotation");
                    break;
                }
                if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                    fail(compiler,compiler->current.span,
                         "a variadic parameter cannot have a default value");
                    break;
                }
                function->has_variadic=true;
                /* A captures_self closure (`closure name() ... end`) is
                 * the one shape where this doesn't reduce to plain
                 * `arity-1`: it reserves register 0 for a materialized
                 * self, exactly like a real method, but (unlike a real
                 * method) its own arity/required_arity are never inflated
                 * to account for that slot -- call_closure_helper's own
                 * needs_self_slot padding (src/vm.c) still shifts the
                 * *runtime* argument_count/arguments by +1 for it
                 * regardless, so the fixed_count operand here must add
                 * that slot back to line up with what DIAMOND_OP_
                 * COLLECT_VARIADIC actually sees at runtime -- confirmed
                 * directly (a self-capturing variadic closure's collected
                 * array was off by one, silently including its own first
                 * real parameter, before this fix). Every other owner
                 * shape (an ordinary method, whose arity already started
                 * at 1 for its own real implicit receiver; a plain
                 * top-level function or non-capturing nested def, which
                 * has no implicit slot and no runtime padding either)
                 * already has arity counted consistently with its own
                 * runtime layout, so only this one shape needs the
                 * adjustment. */
                const uint8_t fixed_count=captures_self?
                    function->arity:(uint8_t)(function->arity-1);
                variadic_parameter=parameter;variadic_fixed_count=fixed_count;
                declared_parameter_count++;
                skip_newlines(compiler);
                if(compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                    advance_token(compiler);skip_newlines(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_AMPERSAND)
                        fail(compiler,compiler->current.span,
                            "only a block parameter may follow a variadic parameter");
                    else continue;
                }
                break;
            }
            if(is_block_parameter&&compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                fail(compiler,compiler->current.span,
                     "a block parameter cannot have a default value");break;
            }
            int parameter_type=-1;DiamondSpan parameter_type_span={};
            if (compiler->current.kind == DIAMOND_TOKEN_COLON) {
                advance_token(compiler);
                parameter_type_span=compiler->current.span;
                const int type = parse_type_annotation(compiler);
                parameter_type=type;
                if(declared_parameter_count<16)
                    function->parameter_type_sets[declared_parameter_count]=
                        (uint8_t)type;
                if(is_block_parameter) {
                    block_parameter_type=type;
                    block_parameter_type_span=parameter_type_span;
                    compiler->current_block_type_set=(uint16_t)type;
                }
            }
            if(compiler->current.kind==DIAMOND_TOKEN_EQUAL) {
                saw_default=true;advance_token(compiler);
                const uint16_t provided=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARGUMENT_PROVIDED,provided,
                                 (uint8_t)(function->arity-1),0,2);
                const size_t skip=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,provided);
                const uint16_t fallback=parse_expression(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,parameter,fallback,0,2);
                patch_jump(compiler,skip,compiler->function->code_count);
            } else if(saw_default&&!is_block_parameter) {
                fail(compiler,compiler->previous.span,
                     "required parameter cannot follow a default parameter");
            } else if(!is_block_parameter)
                function->required_arity++;
            if(parameter_type>=0&&!is_block_parameter) {
                emit_type_check(compiler,parameter,(uint16_t)parameter_type,
                                parameter_type_span);
                compiler->known_type_sets[parameter]=(int32_t)parameter_type;
                const DiamondTypeSet *parameter_set=
                    &function->type_sets[(size_t)parameter_type];
                if(parameter_set->count==1)
                    compiler->known_types[parameter]=parameter_set->members[0].id;
            }
            record_scope_type_fact(compiler,parameter,
                                   compiler->locals[compiler->local_count-1].name.start);
            declared_parameter_count++;
            skip_newlines(compiler);
            if(is_block_parameter&&compiler->current.kind==DIAMOND_TOKEN_COMMA) {
                fail(compiler,compiler->current.span,
                     "a block parameter must be the last parameter");break;
            }
            if (compiler->current.kind != DIAMOND_TOKEN_COMMA) break;
            advance_token(compiler);
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_PAREN) break;
        } while (!compiler->failed);
    }
    if(function->has_variadic&&!compiler->failed)
        emit_instruction(compiler,DIAMOND_OP_COLLECT_VARIADIC,
            variadic_parameter,variadic_fixed_count,has_block_parameter?1:0,3);
    function->has_block_parameter=has_block_parameter;
    if(block_parameter_type>=0&&!compiler->failed) {
        const uint16_t absent=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_NIL,absent,0,0,1);
        const uint16_t missing=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_EQUAL,missing,
            compiler->current_block_register,absent,3);
        const size_t skip=emit_jump(compiler,DIAMOND_OP_JUMP_IF_TRUE,missing);
        emit_type_check(compiler,compiler->current_block_register,
            (uint16_t)block_parameter_type,block_parameter_type_span);
        patch_jump(compiler,skip,compiler->function->code_count);
    }
    if (!compiler->failed && compiler->current.kind != DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler, compiler->current.span, "expected ')' after parameters");
    }
    /* A module_function-mode method's exported (qualified-call-reachable)
     * singleton descriptor used to only get registered *after* this
     * whole def's body finished compiling (see the module_function_mode
     * block below, further down) -- so a self-referencing qualified call
     * inside that same body (`ModuleName.method(...)` calling itself,
     * directly or through a sibling also defined via module_function)
     * could never find its own not-yet-registered descriptor: "undefined
     * module singleton function" at compile time, confirmed as a real,
     * previously-undiscovered gap, not a documented cut, building
     * packages/graphql. Fixed by registering it *here* instead --
     * function->name/arity/required_arity/has_variadic are all already
     * final at this exact point (the parameter list, and only the
     * parameter list, is what determines them), well before body
     * compilation starts -- so a recursive qualified call inside the
     * body can resolve normally. The module_function_mode block further
     * down still runs (marking module->methods[]'s own instance-style
     * entry private, same as always), it just skips re-adding to
     * module->singleton_methods[] a second time when this flag is set. */
    bool module_function_singleton_registered_early=false;
    if(!compiler->failed && compiler->current_module>=0 && !module_singleton &&
       at_top_level && compiler->module_function_mode) {
        DiamondModule *early_module=
            &compiler->program->modules[(size_t)compiler->current_module];
        /* Deliberately NOT checking function->uses_instance_state here,
         * unlike the later (already-existing) registration site that
         * also checks it -- this flag only becomes accurate once the
         * body has actually been compiled (it's set while compiling an
         * `@ivar` access, which hasn't happened yet at this point in the
         * def), so checking it now would just always read false
         * regardless of what the body turns out to contain. The later
         * site still runs this check for real, after the body compiles;
         * if it turns out this method IS stateful, `fail` there marks
         * the whole compilation failed the same as always, and it not
         * mattering that a descriptor was already speculatively written
         * into singleton_methods[] below -- a failed compilation never
         * produces a runnable program either way. */
        if(early_module->singleton_method_count==DIAMOND_MAX_METHODS) {
            fail(compiler,name,"too many module singleton functions");
        } else {
            DiamondMethod exported={0};
            for(size_t i=0;i<copy_length;i++)exported.name[i]=function->name[i];
            exported.name[copy_length]='\0';
            exported.function_index=(uint16_t)function_index;
            /* function->arity/required_arity both carry an implicit +1
             * here for the module-mixing receiver slot every direct
             * module instance method reserves in register 0 (see this
             * function's own direct_module_member seeding, well above
             * the parameter loop) -- subtracted back out, exactly like
             * the later (already-existing) registration site does when
             * copying function's arity into module->methods[]'s own
             * entry, so a qualified call's own arity check isn't off by
             * one. */
            exported.arity=(uint8_t)(function->arity-1);
            exported.required_arity=(uint8_t)(function->required_arity-1);
            exported.has_variadic=function->has_variadic;
            exported.included=false;
            exported.is_private=false;exported.is_protected=false;
            exported.needs_receiver=true;
            early_module->singleton_methods[
                early_module->singleton_method_count++]=exported;
            module_function_singleton_registered_early=true;
        }
    }
    if(!compiler->failed && !at_top_level) {
        for(size_t i=0;i<compiler->enclosing_local_count;i++) {
            if(find_local(compiler,compiler->enclosing_locals[i].name)>=0)continue;
            if(compiler->local_count==DIAMOND_MAX_LOCALS) {
                fail(compiler,compiler->enclosing_locals[i].name,"too many lexical bindings");break;
            }
            const uint16_t cell=allocate_register(compiler);
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
    uint16_t body_result=0;
    bool body_diverges=false;
    if(endless) {
        advance_token(compiler);
        const DiamondTokenKind postfix=postfix_modifier_ahead(compiler);
        const bool has_postfix=postfix==DIAMOND_TOKEN_IF||
                               postfix==DIAMOND_TOKEN_UNLESS;
        const uint16_t postfix_result=has_postfix?allocate_register(compiler):0;
        const size_t condition_jump=has_postfix
            ? emit_jump(compiler,DIAMOND_OP_JUMP,0):SIZE_MAX;
        const size_t body_start=compiler->function->code_count;
        body_result=return_type>=0?
            parse_with_expected_set(compiler,(uint16_t)return_type):
            parse_expression(compiler);
        if(has_postfix) {
            if(compiler->current.kind!=postfix) {
                fail(compiler,compiler->current.span,"expected postfix condition");
            } else {
                advance_token(compiler);
                emit_instruction(compiler,DIAMOND_OP_MOVE,postfix_result,
                                 body_result,0,2);
                const size_t body_exit=emit_jump(compiler,DIAMOND_OP_JUMP,0);
                const size_t condition_start=compiler->function->code_count;
                const uint16_t condition=parse_expression(compiler);
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
        body_diverges=!compiler->failed&&compiler->sequence_diverges;
    }
    if (!compiler->failed) {
        if (return_type >= 0 && !body_diverges) {
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
    uint16_t captures[16];
    for(size_t i=0;i<compiler->capture_count;i++)captures[i]=compiler->capture_registers[i];
    const size_t capture_count=compiler->capture_count;
    if(!compiler->failed) {
        const size_t body_end=compiler->previous.span.start+compiler->previous.span.length;
        record_scope_locals(compiler,0,compiler->local_count,body_end);
        function->body_end=body_end;
    }
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
    compiler->has_current_block=outer_has_current_block;
    compiler->current_block_register=outer_current_block_register;
    compiler->current_block_type_set=outer_current_block_type_set;
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
    for(size_t index=0;index<outer_next_register;index++)
        {compiler->known_types[index]=outer_known_types[index];
         compiler->known_type_sets[index]=outer_known_type_sets[index];}
    free(heap_outer_known_types);free(heap_outer_known_type_sets);
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
            method->has_variadic=function->has_variadic;
            method->included=false;
            method->is_private=compiler->methods_private;
            method->is_protected=compiler->methods_protected;
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
            /* Class-owned (unlike module) singleton methods now reserve
             * register 0 for an implicit `self` -- function->arity/
             * required_arity already include that slot (compile_definition's
             * direct_class_singleton_member branch), so the method table's
             * own arity (what external callers are checked against, see
             * parse_singleton_call) must subtract it back out, the same
             * way an ordinary instance method's entry does just above.
             * needs_receiver tells parse_singleton_call's call site to
             * reserve and fill that slot instead of leaving it zero-inited. */
            method->arity=(uint8_t)(function->arity-1);
            method->required_arity=(uint8_t)(function->required_arity-1);
            method->has_variadic=function->has_variadic;
            method->needs_receiver=true;
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
                method->has_variadic=function->has_variadic;
                method->included=false;
                method->is_private=compiler->methods_private;
                method->is_protected=compiler->methods_protected;
                if(compiler->module_function_mode) {
                    if(function->uses_instance_state)
                        fail(compiler,name,
                            "stateful method cannot use module_function mode");
                    else if(module_function_singleton_registered_early) {
                        /* Already added to singleton_methods[] above,
                         * before the body compiled -- just the private-
                         * marking side effect remains to do here. */
                        method->is_private=true;method->is_protected=false;
                    }
                    else if(module->singleton_method_count==DIAMOND_MAX_METHODS)
                        fail(compiler,name,"too many module singleton functions");
                    else {
                        method->is_private=true;method->is_protected=false;
                        DiamondMethod exported=*method;
                        exported.is_private=false;exported.is_protected=false;
                        exported.needs_receiver=true;
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
            method->has_variadic=function->has_variadic;
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
     * waste against the entry function's own register ceiling --
     * confirmed the hard way while porting the self-hosted parser
     * (docs/roadmap.md's Phase 3 follow-up File.open entry), which is
     * also why that ceiling was later widened (256 -> 4096). */
    const bool member_result_discarded =
        at_top_level && (compiler->current_class>=0||compiler->current_module>=0);
    const uint16_t result =
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
        emit_opcode(compiler,DIAMOND_OP_CLOSURE);emit_register(compiler,result);
        emit_function_index(compiler,function_index);emit_byte(compiler,(uint8_t)capture_count);
        for(size_t i=0;i<capture_count;i++)emit_register(compiler,captures[i]);
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
        (uint8_t)compiler->current_class:UINT8_MAX-1;
    function->arity=writer?2:1;function->required_arity=function->arity;
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<16;index++)function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;
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
            compiler->program->functions[source->function_index];
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
        source->is_private=true;source->is_protected=false;
        DiamondMethod exported=*source;exported.needs_receiver=true;
        exported.is_private=false;exported.is_protected=false;
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

/* `delegate name(params...), to: @ivar` -- compiles into an ordinary
 * forwarding method, as if the source had literally been
 * `def name(params...) @ivar.name(params...) end`: same method metadata,
 * visibility, inheritance, interface checks, dispatch caches, arity
 * validation, and respond_to? behavior as any other method, not a
 * parallel runtime dispatch mechanism (docs/roadmap.md's "Explicit-arity
 * method delegation"). Deliberately scoped: the target must be a bare
 * instance variable (no arbitrary expression, no `to: some_method()`);
 * parameters are bare names only (no type annotations or defaults). A final
 * `&block` parameter forwards the trailing block closure through the ordinary
 * last-Callable-argument convention. Splat and block forwarding cannot yet be
 * combined; the forwarded call always uses the same name
 * declared here (no renaming). Valid in both class and module bodies,
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
static void compile_delegate(Compiler *compiler) {
    const DiamondSpan keyword=compiler->current.span;
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
        fail(compiler,compiler->current.span,"expected method name after 'delegate'");
        return;
    }
    const DiamondSpan method_name=compiler->current.span;
    if(method_name.length>=DIAMOND_MAX_FUNCTION_NAME) {
        fail(compiler,method_name,"delegated method name is too long");return;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_PAREN) {
        fail(compiler,compiler->current.span,
             "expected '(' after delegated method name");return;
    }
    advance_token(compiler);
    skip_newlines(compiler);
    DiamondSpan parameter_names[16];size_t parameter_count=0;
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
                    "a delegate can only declare one variadic parameter");return;
            }
            variadic=true;advance_token(compiler);
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
            fail(compiler,compiler->current.span,"expected parameter name");return;
        }
        if(parameter_count==16) {
            fail(compiler,compiler->current.span,"too many parameters");return;
        }
        parameter_names[parameter_count++]=compiler->current.span;
        advance_token(compiler);
        skip_newlines(compiler);
        if(block_parameter&&compiler->current.kind==DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,
                "a block delegate parameter must be last");return;
        }
        if(variadic&&!block_parameter&&
           compiler->current.kind==DIAMOND_TOKEN_COMMA) {
            advance_token(compiler);skip_newlines(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_AMPERSAND) {
                fail(compiler,compiler->current.span,
                    "only a block parameter may follow a variadic delegate parameter");
                return;
            }
            continue;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
        advance_token(compiler);
        skip_newlines(compiler);
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_PAREN) {
        fail(compiler,compiler->current.span,
             "expected ')' after delegate parameters");return;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
        fail(compiler,compiler->current.span,
             "expected ', to: @ivar' after delegate parameters");return;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||
       !name_equals(compiler,"to",compiler->current.span,false)) {
        fail(compiler,compiler->current.span,"expected 'to:' in delegate");return;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
        fail(compiler,compiler->current.span,"expected ':' after 'to'");return;
    }
    advance_token(compiler);
    if(compiler->current.kind!=DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        fail(compiler,compiler->current.span,
             "delegate target must be an instance variable");return;
    }
    const DiamondSpan target=compiler->current.span;
    advance_token(compiler);

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
    DiamondMethod *methods=nullptr;size_t *method_count=nullptr;
    if(in_class) {
        DiamondClass *class=
            &compiler->program->classes[(size_t)compiler->current_class];
        methods=class->methods;method_count=&class->method_count;
    } else {
        DiamondModule *module=
            &compiler->program->modules[(size_t)compiler->current_module];
        methods=module->methods;method_count=&module->method_count;
    }
    for(size_t index=0;index<*method_count;index++)
        if(!methods[index].included&&strcmp(methods[index].name,stored_name)==0) {
            fail(compiler,method_name,"delegated method name is already defined");
            return;
        }
    if(*method_count==DIAMOND_MAX_METHODS) {
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
    const int outer_return_type=compiler->current_return_type;
    const DiamondSpan outer_return_type_span=compiler->current_return_type_span;
    const int outer_exception=compiler->current_exception;
    const size_t outer_retry_target=compiler->current_retry_target;
    LoopContext *outer_loop=compiler->current_loop;
    uint8_t inline_outer_known_types[256];int32_t inline_outer_known_type_sets[256];
    uint8_t *outer_known_types=inline_outer_known_types;
    int32_t *outer_known_type_sets=inline_outer_known_type_sets;
    uint8_t *heap_outer_known_types=nullptr;int32_t *heap_outer_known_type_sets=nullptr;
    if(outer_next_register>256) {
        heap_outer_known_types=malloc((size_t)outer_next_register*sizeof(uint8_t));
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
    function->owner_class=in_class?(uint8_t)compiler->current_class:UINT8_MAX-1;
    function->declaration_line=(uint32_t)keyword.line;
    function->declaration_column=(uint32_t)keyword.column;
    function->declaration_start=keyword.start;
    function->return_type_set=DIAMOND_NO_TYPE_SET;
    for(size_t index=0;index<16;index++)function->parameter_type_sets[index]=DIAMOND_NO_TYPE_SET;

    compiler->function=function;
    compiler->local_count=0;
    compiler->next_register=0;
    compiler->current_method=method_name;
    compiler->in_method=true;
    compiler->in_singleton_method=false;
    compiler->in_function=false;
    compiler->current_return_type=-1;
    compiler->current_return_type_span=(DiamondSpan){};
    compiler->current_exception=-1;
    compiler->current_retry_target=SIZE_MAX;
    compiler->current_loop=nullptr;

    (void)allocate_register(compiler); /* self */
    function->arity=1;function->required_arity=1;
    uint16_t parameter_registers[16];
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

    function->capture_count=0;
    function->register_count=compiler->next_register;
    if(!compiler->failed) {
        const size_t body_end=target.start+target.length;
        record_scope_locals(compiler,0,compiler->local_count,body_end);
        function->body_end=body_end;
    }

    compiler->function=outer_function;
    compiler->local_count=outer_local_count;
    for(size_t index=0;index<outer_local_count;index++)
        compiler->locals[index]=outer_locals[index];
    compiler->next_register=outer_next_register;
    compiler->current_method=outer_method;
    compiler->in_method=outer_in_method;
    compiler->in_singleton_method=outer_in_singleton_method;
    compiler->in_function=outer_in_function;
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
    DiamondMethod *method=&methods[(*method_count)++];
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

static uint16_t compile_class(Compiler *compiler) {
    advance_token(compiler);
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
        class=&compiler->program->classes[(size_t)index];
        if(class->declared_by_discovery) {
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
            memset(class->methods,0,sizeof class->methods);
            memset(class->singleton_methods,0,sizeof class->singleton_methods);
            memset(class->fields,0,sizeof class->fields);
            memset(class->field_type_status,0,sizeof class->field_type_status);
            memset(class->field_known_class,0,sizeof class->field_known_class);
            memset(class->class_variables,0,sizeof class->class_variables);
            class->method_count=0;
            class->singleton_method_count=0;
            class->field_count=0;
            class->class_variable_count=0;
            class->superclass=UINT8_MAX;
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
            fail(compiler, name, "expected valid class name"); return 0;
        }
        index=(int)compiler->program->class_count++;
        class=&compiler->program->classes[(size_t)index];
        class->declared_by_discovery=false;
        class->superclass=UINT8_MAX;
        superclass_decided=false;
    }
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
            class->superclass=(uint8_t)parent;
            const DiamondClass *parent_class=&compiler->program->classes[(size_t)parent];
            class->field_count=parent_class->field_count;
            for(size_t field=0;field<parent_class->field_count;field++) {
                for(size_t ch=0;ch<DIAMOND_MAX_FUNCTION_NAME;ch++)
                    class->fields[field][ch]=parent_class->fields[field][ch];
                class->field_type_status[field]=parent_class->field_type_status[field];
                class->field_known_class[field]=parent_class->field_known_class[field];
            }
        } else if(class->superclass!=(uint8_t)parent) {
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
    while(!compiler->failed && compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind==DIAMOND_TOKEN_PRIVATE||
           compiler->current.kind==DIAMOND_TOKEN_PROTECTED||
           compiler->current.kind==DIAMOND_TOKEN_PUBLIC) {
            const bool private_visibility=
                compiler->current.kind==DIAMOND_TOKEN_PRIVATE;
            const bool protected_visibility=
                compiler->current.kind==DIAMOND_TOKEN_PROTECTED;
            compile_visibility(compiler,private_visibility,protected_visibility);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            fail(compiler,compiler->current.span,
                 "module_function is only valid in modules");break;
        } else if(compiler->current.kind==DIAMOND_TOKEN_ALIAS_METHOD) {
            compile_alias_method(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_DELEGATE) {
            compile_delegate(compiler);
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
            (void)compile_definition(compiler,false);
        } else {
            fail(compiler,compiler->current.span,
                 "expected method definition or include in class");break;
        }
        if(compiler->current.kind==DIAMOND_TOKEN_NEWLINE) skip_newlines(compiler);
    }
    compiler->current_class=outer;
    compiler->methods_private=outer_private;
    compiler->methods_protected=outer_protected;
    if(compiler->current.kind==DIAMOND_TOKEN_END) advance_token(compiler);
    const uint16_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

static uint16_t compile_module(Compiler *compiler) {
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
    const int existing_module=find_module_name(compiler,stored_name);
    /* A module can always be reopened -- a second `module Foo ... end`
     * adds to the same module rather than erroring (see docs/syntax.md's
     * "Classes" section and this session's own module/class-reopening
     * design). Only a cross-*kind* collision (the same name already a
     * class or interface) stays a hard error. */
    if(find_class_name(compiler,stored_name)>=0||
       find_interface_name(compiler,stored_name)>=0) {
        fail(compiler,name,"module name is already defined");return 0;
    }
    int index;
    DiamondModule *module;
    if(existing_module>=0) {
        index=existing_module;
        module=&compiler->program->modules[(size_t)index];
        if(module->declared_by_discovery) {
            /* First time *this* compile pass touches a slot the *other*
             * (already-finished) pass populated -- see diamond_compile's
             * own comment on declared_by_discovery. Full zero, not just
             * the counts: compile_interface's own method->arity++ (and
             * anything else that accumulates into a slot's fields
             * trusting they start at zero) needs truly-empty arrays to
             * rebuild from, not whatever the other pass already left
             * there. */
            memset(module->methods,0,sizeof module->methods);
            memset(module->singleton_methods,0,sizeof module->singleton_methods);
            memset(module->fields,0,sizeof module->fields);
            module->method_count=0;
            module->singleton_method_count=0;
            module->field_count=0;
            module->declared_by_discovery=false;
        }
        /* else: already owned by this pass (freshly created earlier in
         * this same pass, or already reset just above) -- this is a
         * genuine reopen within the current pass. Merge new content on
         * top without resetting anything. */
    } else {
        if(compiler->program->module_count==DIAMOND_MAX_MODULES) {
            fail(compiler,name,"expected valid module name");return 0;
        }
        index=(int)compiler->program->module_count++;
        module=&compiler->program->modules[(size_t)index];
        module->declared_by_discovery=false;
    }
    (void)snprintf(module->name,sizeof module->name,"%s",stored_name);
    module->declaration_line=(uint32_t)name.line;
    module->declaration_column=(uint32_t)name.column;
    module->declaration_start=name.start;
    advance_token(compiler);
    if(!consume_block_start(compiler))return 0;
    const int outer=compiler->current_module;compiler->current_module=index;
    const bool outer_private=compiler->methods_private;
    const bool outer_protected=compiler->methods_protected;
    const bool outer_module_function=compiler->module_function_mode;
    compiler->methods_private=false;
    compiler->methods_protected=false;
    compiler->module_function_mode=false;
    while(!compiler->failed&&compiler->current.kind!=DIAMOND_TOKEN_END) {
        if(compiler->current.kind==DIAMOND_TOKEN_PRIVATE||
           compiler->current.kind==DIAMOND_TOKEN_PROTECTED||
           compiler->current.kind==DIAMOND_TOKEN_PUBLIC) {
            const bool private_visibility=
                compiler->current.kind==DIAMOND_TOKEN_PRIVATE;
            const bool protected_visibility=
                compiler->current.kind==DIAMOND_TOKEN_PROTECTED;
            compile_visibility(compiler,private_visibility,protected_visibility);
        } else if(compiler->current.kind==DIAMOND_TOKEN_MODULE_FUNCTION) {
            compile_module_function(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_ALIAS_METHOD) {
            compile_alias_method(compiler);
        } else if(compiler->current.kind==DIAMOND_TOKEN_DELEGATE) {
            compile_delegate(compiler);
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
            const uint16_t value=parse_expression(compiler);
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
            (void)compile_definition(compiler,false);
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
    compiler->methods_protected=outer_protected;
    compiler->module_function_mode=outer_module_function;
    if(compiler->current.kind==DIAMOND_TOKEN_END)advance_token(compiler);
    const uint16_t result=allocate_register(compiler);
    /* Sole writer; run_chunk's zero-init already covers this. */
    return result;
}

static uint16_t compile_interface(Compiler *compiler) {
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
        compiler->program->interfaces[(size_t)existing_interface].declared_by_discovery;
    if(!claiming&&(existing_interface>=0||
       find_class_name(compiler,stored_name)>=0||
       find_module_name(compiler,stored_name)>=0)) {
        fail(compiler,name,"type name is already defined");return 0;
    }
    if(!claiming&&compiler->program->interface_count==DIAMOND_MAX_INTERFACES) {
        fail(compiler,name,"expected valid interface name");return 0;
    }
    DiamondInterface *interface=claiming
        ? &compiler->program->interfaces[(size_t)existing_interface]
        : &compiler->program->interfaces[compiler->program->interface_count++];
    if(claiming) {
        /* See compile_class's own identical comment on why a full zero
         * of this array, not just the count, is needed here -- this is
         * in fact where the bug was actually found: compile_interface's
         * own method->arity++ below (parsing each `(param, ...)` list)
         * accumulates directly onto whatever was already sitting in
         * interface->methods[n], rather than assigning an absolute
         * value, so a claimed slot's stale discovery-pass arity was
         * silently being added to instead of replaced. */
        memset(interface->methods,0,sizeof interface->methods);
        interface->method_count=0;
        interface->declared_by_discovery=false;
    } else {
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
        for(size_t index=0;index<16;index++)
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
            if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER||method->arity==16) {
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
static uint16_t compile_assignment_store(Compiler *compiler, DiamondSpan name,
        bool instance_variable, bool class_variable, uint16_t value) {
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
                    &compiler->program->classes[(size_t)compiler->current_class];
                const uint8_t known=compiler->known_types[value];
                const bool concrete=known>=DIAMOND_TYPE_CLASS_BASE&&
                    known<DIAMOND_TYPE_VARIABLE_BASE&&
                    (size_t)(known-DIAMOND_TYPE_CLASS_BASE)<
                        compiler->program->class_count;
                const uint8_t class_index=concrete?
                    (uint8_t)(known-DIAMOND_TYPE_CLASS_BASE):0;
                if(class->field_type_status[(size_t)field]==0&&concrete) {
                    class->field_type_status[(size_t)field]=1;
                    class->field_known_class[(size_t)field]=class_index;
                } else if(!concrete||
                          class->field_known_class[(size_t)field]!=class_index) {
                    class->field_type_status[(size_t)field]=2;
                }
            }
        }
        return value;
    }
    if (class_variable) {
        const int slot = class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_SET_CVAR,
                         (uint8_t)compiler->current_class,(uint8_t)slot,value,3);
        return value;
    }
    int local = find_local(compiler, name);
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
                if(capture==16){fail(compiler,name,"too many captured variables");return 0;}
                compiler->capture_registers[compiler->capture_count++]=compiler->enclosing_locals[i-1].reg;
            }
            emit_instruction(compiler,DIAMOND_OP_SET_CAPTURE,(uint8_t)capture,value,0,2);
            return value;
        }
    }
    const uint16_t destination = local < 0
        ? define_local(compiler, name)
        : compiler->locals[(size_t)local].reg;
    emit_instruction(compiler, DIAMOND_OP_MOVE, destination, value, 0, 2);
    compiler->known_types[destination]=compiler->known_types[value];
    compiler->known_type_sets[destination]=compiler->known_type_sets[value];
    record_scope_type_fact(compiler,destination,compiler->current.span.start);
    return destination;
}

static uint16_t compile_assignment(Compiler *compiler) {
    const DiamondSpan name = compiler->current.span;
    const bool instance_variable =
        compiler->current.kind == DIAMOND_TOKEN_INSTANCE_VARIABLE;
    const bool class_variable =
        compiler->current.kind == DIAMOND_TOKEN_CLASS_VARIABLE;
    advance_token(compiler);
    advance_token(compiler);
    const uint16_t value = parse_expression(compiler);
    return compile_assignment_store(compiler, name, instance_variable, class_variable, value);
}

/* `x += y`/`x -= y`/.../`x ||= y`/`x &&= y` -- pure sugar, expanded here
 * into the same shape the equivalent spelled-out form would produce:
 * `x = x + y` for the arithmetic ones, `x = x || y`/`x = x && y`
 * (genuinely short-circuit -- `y` is never evaluated, and never
 * assigned, unless the short-circuit check requires it) for the other
 * two. Reads the target's current value via parse_prefix -- the exact
 * same prefix-dispatch parse_precedence itself uses, so a captured
 * local/an ivar/a cvar all read correctly with no special-casing here,
 * the same way compile_assignment_store's write side already handles
 * all three uniformly. Only ever called once compound_assignment_ahead
 * has confirmed the next token really is one of these -- indexed
 * targets (`arr[i] += 1`) aren't recognized at all (see that function's
 * own comment), so this never needs to guard against one. */
static uint16_t compile_compound_assignment(Compiler *compiler) {
    const DiamondSpan name = compiler->current.span;
    const bool instance_variable =
        compiler->current.kind == DIAMOND_TOKEN_INSTANCE_VARIABLE;
    const bool class_variable =
        compiler->current.kind == DIAMOND_TOKEN_CLASS_VARIABLE;
    const uint16_t left = parse_prefix(compiler);
    const DiamondTokenKind op_kind = compiler->current.kind;
    advance_token(compiler);
    if(op_kind==DIAMOND_TOKEN_OR_OR_EQUAL||op_kind==DIAMOND_TOKEN_AND_AND_EQUAL) {
        const bool is_and = op_kind==DIAMOND_TOKEN_AND_AND_EQUAL;
        const uint16_t destination = allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,left,0,2);
        const size_t end_jump = emit_jump(compiler,
            is_and?DIAMOND_OP_JUMP_IF_FALSE:DIAMOND_OP_JUMP_IF_TRUE,left);
        const uint16_t right = parse_expression(compiler);
        emit_instruction(compiler,DIAMOND_OP_MOVE,destination,right,0,2);
        patch_jump(compiler,end_jump,compiler->function->code_count);
        return compile_assignment_store(compiler,name,instance_variable,
            class_variable,destination);
    }
    const DiamondTokenKind plain_op =
        op_kind==DIAMOND_TOKEN_PLUS_EQUAL?DIAMOND_TOKEN_PLUS:
        op_kind==DIAMOND_TOKEN_MINUS_EQUAL?DIAMOND_TOKEN_MINUS:
        op_kind==DIAMOND_TOKEN_STAR_EQUAL?DIAMOND_TOKEN_STAR:
        op_kind==DIAMOND_TOKEN_SLASH_EQUAL?DIAMOND_TOKEN_SLASH:
        DIAMOND_TOKEN_PERCENT;
    const uint16_t right = parse_expression(compiler);
    const uint16_t destination = compile_binary_op(compiler, plain_op, left, right);
    return compile_assignment_store(compiler, name, instance_variable,
        class_variable, destination);
}

/* Finds (or, the first time in this function, registers) a one-member
 * type set matching plain `Array` (no element-type argument) -- the
 * same DiamondTypeSet shape parse_type_annotation builds when it reads
 * a literal `Array` annotation from source, just constructed directly
 * here since there's no source text driving it. Reused across every
 * destructuring statement in the same function rather than burning a
 * fresh type-set slot per statement. */
static uint16_t array_type_set_index(Compiler *compiler) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_ARRAY&&
           set->members[0].argument_set==DIAMOND_NO_TYPE_SET)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;
    set->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_ARRAY,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t member_index=0;member_index<16;member_index++)
        set->members[0].callable_parameter_sets[member_index]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)set_index;
}

static uint16_t hash_type_set_index(Compiler *compiler) {
    for(size_t index=0;index<compiler->function->type_set_count;index++) {
        const DiamondTypeSet *set=&compiler->function->type_sets[index];
        if(set->count==1&&set->members[0].id==DIAMOND_TYPE_HASH&&
           set->members[0].argument_set==DIAMOND_NO_TYPE_SET)
            return (uint16_t)index;
    }
    if(!reserve_type_sets(compiler,1))return 0;
    const size_t set_index=compiler->function->type_set_count++;
    DiamondTypeSet *set=&compiler->function->type_sets[set_index];
    set->count=1;
    set->members[0]=(DiamondTypeMember){.id=DIAMOND_TYPE_HASH,
        .argument_set=DIAMOND_NO_TYPE_SET,.second_argument_set=DIAMOND_NO_TYPE_SET,
        .callable_arity=UINT8_MAX,.callable_return_set=DIAMOND_NO_TYPE_SET,
        .callable_parameters_typed=false};
    for(size_t member_index=0;member_index<16;member_index++)
        set->members[0].callable_parameter_sets[member_index]=DIAMOND_NO_TYPE_SET;
    return (uint16_t)set_index;
}

typedef struct DestructureNode {
    bool leaf;
    bool rest;
    bool hash;
    bool hash_rest;
    DiamondSpan name;
    bool instance_variable;
    bool class_variable;
    bool indexed;
    bool member;
    uint16_t target_receiver;
    uint16_t target_index;
    DiamondSpan target_member;
    uint8_t children[16];
    uint8_t child_count;
    uint16_t key_register;
} DestructureNode;

static uint16_t load_destructure_target(Compiler *compiler,DiamondSpan name,
        DiamondTokenKind kind) {
    if(kind==DIAMOND_TOKEN_INSTANCE_VARIABLE) {
        const uint16_t receiver=allocate_register(compiler);
        if(compiler->current_module>=0&&compiler->current_class<0) {
            const uint16_t field=module_field_name(compiler,name);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR_NAME,receiver,0,field,3);
        } else {
            const int field=field_index(compiler,name,true);
            emit_instruction(compiler,DIAMOND_OP_GET_IVAR,receiver,0,(uint8_t)field,3);
        }
        return receiver;
    }
    if(kind==DIAMOND_TOKEN_CLASS_VARIABLE) {
        const uint16_t receiver=allocate_register(compiler);
        const int slot=class_variable_index(compiler,name,true);
        emit_instruction(compiler,DIAMOND_OP_GET_CVAR,receiver,
            (uint8_t)compiler->current_class,(uint8_t)slot,3);
        return receiver;
    }
    const int local=find_local(compiler,name);
    if(local<0) {
        fail(compiler,name,"undefined local variable");return 0;
    }
    uint16_t receiver=compiler->locals[(size_t)local].reg;
    if(compiler->locals[(size_t)local].captured) {
        emit_instruction(compiler,DIAMOND_OP_BOX_LOCAL,receiver,0,0,1);
        const uint16_t loaded=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_GET_CELL,loaded,receiver,0,2);
        receiver=loaded;
    }
    return receiver;
}

static uint8_t parse_destructure_node(Compiler *compiler,DestructureNode *nodes,
        size_t *node_count,size_t depth) {
    if(*node_count==64||depth>8) {
        fail(compiler,compiler->current.span,"destructuring pattern is too complex");
        return 0;
    }
    const uint8_t node_index=(uint8_t)(*node_count);
    DestructureNode *node=&nodes[(*node_count)++];
    *node=(DestructureNode){};
    if(compiler->current.kind==DIAMOND_TOKEN_STAR) {
        node->rest=true;advance_token(compiler);
        if(!destructuring_target_kind(compiler->current.kind)) {
            fail(compiler,compiler->current.span,"expected target after '*' in destructuring pattern");
            return node_index;
        }
    }
    if(destructuring_target_kind(compiler->current.kind)) {
        const DiamondTokenKind target_kind=compiler->current.kind;
        node->leaf=true;node->name=compiler->current.span;
        node->instance_variable=target_kind==DIAMOND_TOKEN_INSTANCE_VARIABLE;
        node->class_variable=target_kind==DIAMOND_TOKEN_CLASS_VARIABLE;
        advance_token(compiler);
        if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
           compiler->current.kind==DIAMOND_TOKEN_DOT) {
            uint16_t receiver=load_destructure_target(
                compiler,node->name,target_kind);
            while(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                  compiler->current.kind==DIAMOND_TOKEN_DOT) {
                if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET) {
                    advance_token(compiler);
                    const uint16_t index=parse_expression(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET) {
                        fail(compiler,compiler->current.span,
                            "expected ']' after destructuring target index");
                        return node_index;
                    }
                    advance_token(compiler);
                    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                       compiler->current.kind==DIAMOND_TOKEN_DOT) {
                        const uint16_t loaded=allocate_register(compiler);
                        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,
                            loaded,receiver,index,3);
                        receiver=loaded;
                    } else {
                        node->indexed=true;
                        node->target_receiver=receiver;
                        node->target_index=index;
                    }
                } else {
                    advance_token(compiler);
                    if(compiler->current.kind!=DIAMOND_TOKEN_IDENTIFIER) {
                        fail(compiler,compiler->current.span,
                            "expected member after '.' in destructuring target");
                        return node_index;
                    }
                    const DiamondSpan member=compiler->current.span;
                    advance_token(compiler);
                    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
                       compiler->current.kind==DIAMOND_TOKEN_DOT)
                        receiver=emit_invoke_call(compiler,receiver,member,false,
                            nullptr,0,nullptr,0);
                    else {
                        node->member=true;
                        node->target_receiver=receiver;
                        node->target_member=member;
                    }
                }
            }
        }
        return node_index;
    }
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        node->hash=true;advance_token(compiler);skip_newlines(compiler);
        while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE&&!compiler->failed) {
            if(node->child_count==16) {
                fail(compiler,compiler->current.span,"too many Hash destructuring targets");
                return node_index;
            }
            DiamondLexer rest_lookahead=compiler->lexer;
            const DiamondToken second_star=diamond_lexer_next(&rest_lookahead);
            if(compiler->current.kind==DIAMOND_TOKEN_STAR&&
               second_star.kind==DIAMOND_TOKEN_STAR) {
                advance_token(compiler);advance_token(compiler);
                const uint8_t child=parse_destructure_node(
                    compiler,nodes,node_count,depth+1);
                if(!nodes[child].leaf) {
                    fail(compiler,compiler->previous.span,
                        "Hash rest target must be a variable");return node_index;
                }
                nodes[child].hash_rest=true;
                node->children[node->child_count++]=child;
                skip_newlines(compiler);
                if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
                    fail(compiler,compiler->current.span,
                        "Hash rest target must be last");return node_index;
                }
                break;
            }
            const uint16_t key=parse_expression(compiler);
            if(compiler->current.kind!=DIAMOND_TOKEN_COLON) {
                fail(compiler,compiler->current.span,
                    "expected ':' after Hash destructuring key");return node_index;
            }
            advance_token(compiler);
            const uint8_t child=parse_destructure_node(
                compiler,nodes,node_count,depth+1);
            if(nodes[child].rest||nodes[child].hash_rest) {
                fail(compiler,nodes[child].name,
                    "rest target is not valid as a Hash entry target");
                return node_index;
            }
            nodes[child].key_register=key;
            node->children[node->child_count++]=child;
            skip_newlines(compiler);
            if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACE)break;
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
                fail(compiler,compiler->current.span,
                    "expected ',' in Hash destructuring pattern");return node_index;
            }
            advance_token(compiler);skip_newlines(compiler);
        }
        if(node->child_count==0) {
            fail(compiler,compiler->current.span,
                "Hash destructuring pattern cannot be empty");return node_index;
        }
        if(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACE) {
            fail(compiler,compiler->current.span,
                "expected '}' after Hash destructuring pattern");return node_index;
        }
        advance_token(compiler);return node_index;
    }
    if(compiler->current.kind!=DIAMOND_TOKEN_LEFT_BRACKET) {
        fail(compiler,compiler->current.span,"expected destructuring target");return 0;
    }
    advance_token(compiler);
    bool rest_seen=false;
    while(compiler->current.kind!=DIAMOND_TOKEN_RIGHT_BRACKET&&!compiler->failed) {
        if(node->child_count==16) {
            fail(compiler,compiler->current.span,"too many destructuring targets");
            return node_index;
        }
        node->children[node->child_count++]=
            parse_destructure_node(compiler,nodes,node_count,depth+1);
        if(nodes[node->children[node->child_count-1]].rest) {
            if(rest_seen) {
                fail(compiler,nodes[node->children[node->child_count-1]].name,
                    "destructuring pattern may contain only one rest target");
                return node_index;
            }
            rest_seen=true;
        }
        if(compiler->current.kind==DIAMOND_TOKEN_RIGHT_BRACKET)break;
        if(compiler->current.kind!=DIAMOND_TOKEN_COMMA) {
            fail(compiler,compiler->current.span,"expected ',' in destructuring pattern");
            return node_index;
        }
        advance_token(compiler);
    }
    if(node->child_count==0) {
        fail(compiler,compiler->current.span,"destructuring pattern cannot be empty");
        return node_index;
    }
    advance_token(compiler);return node_index;
}

static void emit_destructure_extract(Compiler *compiler,
        const DestructureNode *nodes,uint8_t node_index,uint16_t value,
        uint16_t array_set,uint16_t hash_set,uint16_t *node_values) {
    const DestructureNode *node=&nodes[node_index];
    node_values[node_index]=value;
    if(node->leaf)return;
    if(node->hash) {
        emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,value,hash_set,0,2);
        size_t fixed_count=0;
        for(size_t child=0;child<node->child_count;child++)
            if(!nodes[node->children[child]].hash_rest)fixed_count++;
        for(size_t child=0;child<node->child_count;child++) {
            const DestructureNode *child_node=&nodes[node->children[child]];
            if(child_node->hash_rest) {
                const uint16_t key_base=allocate_register(compiler);
                for(size_t key=1;key<fixed_count;key++)(void)allocate_register(compiler);
                size_t key_index=0;
                for(size_t fixed=0;fixed<node->child_count;fixed++) {
                    const DestructureNode *fixed_node=&nodes[node->children[fixed]];
                    if(fixed_node->hash_rest)continue;
                    emit_instruction(compiler,DIAMOND_OP_MOVE,
                        (uint16_t)(key_base+key_index++),fixed_node->key_register,0,2);
                }
                const uint16_t excluded=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_ARRAY,excluded,key_base,
                    (uint16_t)fixed_count,3);
                compiler->known_types[excluded]=DIAMOND_TYPE_ARRAY;
                const uint16_t rest=allocate_register(compiler);
                emit_instruction(compiler,DIAMOND_OP_HASH_REST,rest,value,excluded,3);
                compiler->known_types[rest]=DIAMOND_TYPE_HASH;
                node_values[node->children[child]]=rest;
                continue;
            }
            emit_instruction(compiler,DIAMOND_OP_CHECK_HASH_KEY,value,
                child_node->key_register,0,2);
            const uint16_t element=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_INDEX_GET,element,value,
                child_node->key_register,3);
            emit_destructure_extract(compiler,nodes,node->children[child],element,
                array_set,hash_set,node_values);
        }
        return;
    }
    emit_instruction(compiler,DIAMOND_OP_CHECK_TYPE,value,array_set,0,2);
    size_t rest_index=SIZE_MAX;
    for(size_t child=0;child<node->child_count;child++)
        if(nodes[node->children[child]].rest)rest_index=child;
    const bool has_rest=rest_index!=SIZE_MAX;
    const uint16_t fixed_count=(uint16_t)(node->child_count-(has_rest?1u:0u));
    emit_instruction(compiler,DIAMOND_OP_CHECK_DESTRUCTURE_COUNT,value,
        fixed_count|(has_rest?0x8000u:0u),0,2);
    for(size_t index=0;index<node->child_count;index++) {
        if(nodes[node->children[index]].rest) {
            const uint16_t rest=allocate_register(compiler);
            const uint16_t suffix=(uint16_t)(node->child_count-index-1);
            emit_instruction(compiler,DIAMOND_OP_ARRAY_MIDDLE,rest,value,
                (uint16_t)((index<<8)|suffix),3);
            compiler->known_types[rest]=DIAMOND_TYPE_ARRAY;
            emit_destructure_extract(compiler,nodes,node->children[index],rest,
                array_set,hash_set,node_values);continue;
        }
        if(has_rest&&index>rest_index) {
            const uint16_t element=allocate_register(compiler);
            emit_instruction(compiler,DIAMOND_OP_ARRAY_SUFFIX,element,value,
                (uint16_t)(node->child_count-index),3);
            emit_destructure_extract(compiler,nodes,node->children[index],element,
                array_set,hash_set,node_values);continue;
        }
        const uint16_t index_constant=add_constant(compiler,DIAMOND_INT((int64_t)index));
        const uint16_t index_register=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_CONSTANT,index_register,index_constant,0,2);
        const uint16_t element=allocate_register(compiler);
        emit_instruction(compiler,DIAMOND_OP_INDEX_GET,element,value,index_register,3);
        emit_destructure_extract(compiler,nodes,node->children[index],element,
            array_set,hash_set,node_values);
    }
}

static uint16_t emit_destructure_stores(Compiler *compiler,
        const DestructureNode *nodes,uint8_t node_index,const uint16_t *node_values) {
    const DestructureNode *node=&nodes[node_index];
    if(node->leaf) {
        if(node->indexed) {
            emit_instruction(compiler,DIAMOND_OP_INDEX_SET,node->target_receiver,
                node->target_index,node_values[node_index],3);
            return node_values[node_index];
        }
        if(node->member) {
            const uint16_t value=node_values[node_index];
            (void)emit_invoke_call(compiler,node->target_receiver,
                node->target_member,true,nullptr,0,&value,1);
            return value;
        }
        return compile_assignment_store(compiler,node->name,
            node->instance_variable,node->class_variable,node_values[node_index]);
    }
    uint16_t last=node_values[node_index];
    for(size_t index=0;index<node->child_count;index++)
        last=emit_destructure_stores(compiler,nodes,node->children[index],node_values);
    return last;
}

/* Existing `a, b = expr` plus bracketed/nested `[a, [b, c]] = expr`.
 * The RHS is evaluated once; every group independently enforces an exact
 * Array shape before its leaves use ordinary local/@ivar/@@cvar stores. */
static uint16_t compile_multi_assignment(Compiler *compiler) {
    DestructureNode nodes[64]={};size_t node_count=0;
    const uint8_t root=(uint8_t)node_count++;
    if(compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACKET||
       compiler->current.kind==DIAMOND_TOKEN_LEFT_BRACE) {
        node_count=0;
        (void)parse_destructure_node(compiler,nodes,&node_count,0);
    } else {
        bool rest_seen=false;
        while(!compiler->failed) {
            DestructureNode *root_node=&nodes[root];
            if(root_node->child_count==16) {
                fail(compiler,compiler->current.span,"too many destructuring targets");
                return 0;
            }
            root_node->children[root_node->child_count++]=
                parse_destructure_node(compiler,nodes,&node_count,0);
            if(nodes[root_node->children[root_node->child_count-1]].rest) {
                if(rest_seen) {
                    fail(compiler,
                        nodes[root_node->children[root_node->child_count-1]].name,
                        "destructuring pattern may contain only one rest target");
                    return 0;
                }
                rest_seen=true;
            }
            if(compiler->current.kind!=DIAMOND_TOKEN_COMMA)break;
            advance_token(compiler);
        }
    }
    if(compiler->failed)return 0;
    if(compiler->current.kind!=DIAMOND_TOKEN_EQUAL) {
        fail(compiler,compiler->current.span,"expected '=' after destructuring pattern");
        return 0;
    }
    advance_token(compiler);
    uint16_t rhs_values[16];size_t rhs_count=0;
    rhs_values[rhs_count++]=parse_expression(compiler);
    while(compiler->current.kind==DIAMOND_TOKEN_COMMA&&!compiler->failed) {
        if(rhs_count==16) {
            fail(compiler,compiler->current.span,
                "too many destructuring right-hand values");return 0;
        }
        advance_token(compiler);skip_newlines(compiler);
        rhs_values[rhs_count++]=parse_expression(compiler);
    }
    const uint16_t value=rhs_count==1?rhs_values[0]:
        emit_argument_array(compiler,rhs_values,rhs_count);
    const uint16_t array_set = array_type_set_index(compiler);
    const uint16_t hash_set = hash_type_set_index(compiler);
    uint16_t node_values[64]={};
    emit_destructure_extract(compiler,nodes,root,value,array_set,hash_set,node_values);
    return emit_destructure_stores(compiler,nodes,root,node_values);
}

static bool at_block_end(const Compiler *compiler) {
    return compiler->current.kind == DIAMOND_TOKEN_EOF ||
           compiler->current.kind == DIAMOND_TOKEN_ELSE ||
           compiler->current.kind == DIAMOND_TOKEN_ELSIF ||
           compiler->current.kind == DIAMOND_TOKEN_WHEN ||
           compiler->current.kind == DIAMOND_TOKEN_RESCUE ||
           compiler->current.kind == DIAMOND_TOKEN_ENSURE ||
           compiler->current.kind == DIAMOND_TOKEN_END;
}

static bool expression_finishes_block(const Compiler *compiler) {
    DiamondLexer lookahead=compiler->lexer;
    size_t nesting=0;
    DiamondToken token=compiler->current;
    for(;;) {
        if(token.kind==DIAMOND_TOKEN_EOF)return true;
        if(token.kind==DIAMOND_TOKEN_LEFT_PAREN||
           token.kind==DIAMOND_TOKEN_LEFT_BRACKET||
           token.kind==DIAMOND_TOKEN_LEFT_BRACE)nesting++;
        else if(token.kind==DIAMOND_TOKEN_RIGHT_PAREN||
                token.kind==DIAMOND_TOKEN_RIGHT_BRACKET||
                token.kind==DIAMOND_TOKEN_RIGHT_BRACE) {
            if(nesting>0)nesting--;
        } else if(token.kind==DIAMOND_TOKEN_NEWLINE&&nesting==0) {
            do token=diamond_lexer_next(&lookahead);
            while(token.kind==DIAMOND_TOKEN_NEWLINE);
            return token.kind==DIAMOND_TOKEN_EOF||
                   token.kind==DIAMOND_TOKEN_ELSE||
                   token.kind==DIAMOND_TOKEN_ELSIF||
                   token.kind==DIAMOND_TOKEN_WHEN||
                   token.kind==DIAMOND_TOKEN_RESCUE||
                   token.kind==DIAMOND_TOKEN_ENSURE||
                   token.kind==DIAMOND_TOKEN_END;
        }
        token=diamond_lexer_next(&lookahead);
        if(token.kind==DIAMOND_TOKEN_ERROR)return false;
    }
}

static uint16_t compile_sequence(Compiler *compiler) {
    skip_newlines(compiler);
    uint16_t result = allocate_register(compiler);
    /* No NIL here: `result` is a sole writer for an empty block (still
     * correctly nil via run_chunk's zero-init), and is superseded by
     * the first statement's own result register whenever the block is
     * non-empty -- the highest-frequency NIL-elision site, since this
     * fires once per compiled block. */
    bool last_statement_diverges = false;

    while (!compiler->failed && !at_block_end(compiler)) {
        bool statement_is_raise = false;
        const DiamondTokenKind postfix = postfix_modifier_ahead(compiler);
        const bool has_postfix = postfix == DIAMOND_TOKEN_IF ||
                                 postfix == DIAMOND_TOKEN_UNLESS;
        const uint16_t body_result_slot = result;
        const uint16_t postfix_result = has_postfix
            ? allocate_register(compiler)
            : body_result_slot;
        const size_t condition_jump = has_postfix
            ? emit_jump(compiler, DIAMOND_OP_JUMP, 0)
            : SIZE_MAX;
        const size_t body_start = compiler->function->code_count;
        const bool declaration_statement =
            compiler->current.kind == DIAMOND_TOKEN_DEF ||
            compiler->current.kind == DIAMOND_TOKEN_CLOSURE ||
            compiler->current.kind == DIAMOND_TOKEN_CLASS ||
            compiler->current.kind == DIAMOND_TOKEN_INTERFACE ||
            compiler->current.kind == DIAMOND_TOKEN_MODULE;
        if (compiler->current.kind == DIAMOND_TOKEN_DEF) {
            result = compile_definition(compiler,false);
        } else if (compiler->current.kind == DIAMOND_TOKEN_CLOSURE) {
            result = compile_definition(compiler,true);
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
            statement_is_raise = true;
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
            else if(index_compound_assignment_ahead(compiler))
                result=compile_index_compound_assignment(compiler);
            else if(multi_assignment_ahead(compiler))
                result=compile_multi_assignment(compiler);
            else if(compound_assignment_ahead(compiler))
                result=compile_compound_assignment(compiler);
            else if(assignment_ahead(compiler))
                result=compile_assignment(compiler);
            else if(!has_postfix&&
                    compiler->function->return_type_set!=DIAMOND_NO_TYPE_SET&&
                    expression_finishes_block(compiler))
                result=parse_with_expected_set(compiler,
                    compiler->function->return_type_set);
            else result=parse_expression(compiler);
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
            const uint16_t condition = parse_expression(compiler);
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
        last_statement_diverges = statement_is_raise && !has_postfix;
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
    compiler->sequence_diverges = last_statement_diverges;
    return result;
}

DiamondFunction *diamond_program_add_function(DiamondProgram *program) {
    if(program->function_count>=DIAMOND_MAX_FUNCTIONS)return nullptr;
    if(program->function_count==program->function_capacity) {
        size_t capacity=program->function_capacity==0?64:
            program->function_capacity*2;
        if(capacity>DIAMOND_MAX_FUNCTIONS)capacity=DIAMOND_MAX_FUNCTIONS;
        DiamondFunction **functions=realloc(program->functions,
            capacity*sizeof *functions);
        if(functions==nullptr)return nullptr;
        program->functions=functions;
        program->function_capacity=capacity;
    }
    DiamondFunction *function=calloc(1,sizeof *function);
    if(function==nullptr)return nullptr;
    program->functions[program->function_count++]=function;
    return function;
}

bool diamond_function_reserve_code(DiamondFunction *function,size_t capacity) {
    if(capacity<=function->code_capacity)return true;
    if(capacity>DIAMOND_MAX_CODE)return false;
    uint8_t *code=malloc(capacity*sizeof *code);
    uint32_t *lines=calloc(capacity,sizeof *lines);
    uint32_t *columns=calloc(capacity,sizeof *columns);
    if(code==nullptr||lines==nullptr||columns==nullptr) {
        free(code);free(lines);free(columns);return false;
    }
    if(function->code_count>0) {
        memcpy(code,function->code,function->code_count*sizeof *code);
        memcpy(lines,function->lines,function->code_count*sizeof *lines);
        memcpy(columns,function->columns,function->code_count*sizeof *columns);
    }
    free(function->code);free(function->lines);free(function->columns);
    function->code=code;function->lines=lines;function->columns=columns;
    function->code_capacity=capacity;
    return true;
}

bool diamond_function_reserve_constants(DiamondFunction *function,
                                         size_t capacity) {
    if(capacity<=function->constant_capacity)return true;
    if(capacity>DIAMOND_MAX_CONSTANTS)return false;
    DiamondValue *constants=realloc(function->constants,
        capacity*sizeof *constants);
    if(constants==nullptr)return false;
    function->constants=constants;function->constant_capacity=capacity;
    return true;
}

bool diamond_function_reserve_strings(DiamondFunction *function,size_t capacity) {
    if(capacity<=function->string_capacity)return true;
    if(capacity>DIAMOND_MAX_STRING_CONSTANTS)return false;
    const size_t previous_capacity=function->string_capacity;
    DiamondStringConstant *strings=realloc(function->strings,
        capacity*sizeof *strings);
    if(strings==nullptr)return false;
    memset(strings+previous_capacity,0,
        (capacity-previous_capacity)*sizeof *strings);
    function->strings=strings;function->string_capacity=capacity;
    return true;
}

bool diamond_function_reserve_type_sets(DiamondFunction *function,size_t capacity) {
    if(capacity<=function->type_set_capacity)return true;
    if(capacity>DIAMOND_MAX_TYPE_SETS)return false;
    const size_t previous_capacity=function->type_set_capacity;
    DiamondTypeSet *type_sets=realloc(function->type_sets,
        capacity*sizeof *type_sets);
    if(type_sets==nullptr)return false;
    memset(type_sets+previous_capacity,0,
        (capacity-previous_capacity)*sizeof *type_sets);
    function->type_sets=type_sets;function->type_set_capacity=capacity;
    return true;
}

bool diamond_function_copy(DiamondFunction *destination,
                           const DiamondFunction *source) {
    *destination=*source;
    destination->code=nullptr;destination->lines=nullptr;
    destination->columns=nullptr;destination->code_capacity=0;
    destination->constants=nullptr;destination->constant_capacity=0;
    destination->strings=nullptr;destination->string_capacity=0;
    destination->type_sets=nullptr;destination->type_set_capacity=0;
    if(source->code_count>0) {
        destination->code=malloc(source->code_count*sizeof *destination->code);
        destination->lines=malloc(source->code_count*sizeof *destination->lines);
        destination->columns=malloc(source->code_count*sizeof *destination->columns);
    }
    if(source->constant_count>0)
        destination->constants=malloc(
            source->constant_count*sizeof *destination->constants);
    if(source->string_count>0)
        destination->strings=malloc(
            source->string_count*sizeof *destination->strings);
    if(source->type_set_count>0)
        destination->type_sets=malloc(
            source->type_set_count*sizeof *destination->type_sets);
    if((source->code_count>0&&(destination->code==nullptr||
       destination->lines==nullptr||destination->columns==nullptr))||
       (source->constant_count>0&&destination->constants==nullptr)||
       (source->string_count>0&&destination->strings==nullptr)||
       (source->type_set_count>0&&destination->type_sets==nullptr)) {
        free(destination->code);free(destination->lines);
        free(destination->columns);free(destination->constants);
        free(destination->strings);
        free(destination->type_sets);
        destination->code=nullptr;destination->lines=nullptr;
        destination->columns=nullptr;destination->constants=nullptr;
        destination->strings=nullptr;destination->code_count=0;
        destination->type_sets=nullptr;
        destination->constant_count=0;destination->string_count=0;
        return false;
    }
    if(source->code_count>0) {
        memcpy(destination->code,source->code,
            source->code_count*sizeof *destination->code);
        memcpy(destination->lines,source->lines,
            source->code_count*sizeof *destination->lines);
        memcpy(destination->columns,source->columns,
            source->code_count*sizeof *destination->columns);
    }
    if(source->constant_count>0)
        memcpy(destination->constants,source->constants,
            source->constant_count*sizeof *destination->constants);
    if(source->string_count>0)
        memcpy(destination->strings,source->strings,
            source->string_count*sizeof *destination->strings);
    if(source->type_set_count>0)
        memcpy(destination->type_sets,source->type_sets,
            source->type_set_count*sizeof *destination->type_sets);
    destination->code_capacity=source->code_count;
    destination->constant_capacity=source->constant_count;
    destination->string_capacity=source->string_count;
    destination->type_set_capacity=source->type_set_count;
    return true;
}

void diamond_program_free(DiamondProgram *program) {
    if(program==nullptr)return;
    free(program->entry.code);free(program->entry.lines);
    free(program->entry.columns);free(program->entry.constants);
    free(program->entry.strings);
    free(program->entry.type_sets);
    program->entry.code=nullptr;program->entry.lines=nullptr;
    program->entry.columns=nullptr;program->entry.code_count=0;
    program->entry.code_capacity=0;
    program->entry.constants=nullptr;program->entry.constant_count=0;
    program->entry.constant_capacity=0;
    program->entry.strings=nullptr;program->entry.string_count=0;
    program->entry.string_capacity=0;
    program->entry.type_sets=nullptr;program->entry.type_set_count=0;
    program->entry.type_set_capacity=0;
    for(size_t index=0;index<program->function_count;index++) {
        free(program->functions[index]->code);
        free(program->functions[index]->lines);
        free(program->functions[index]->columns);
        free(program->functions[index]->constants);
        free(program->functions[index]->strings);
        free(program->functions[index]->type_sets);
        free(program->functions[index]);
    }
    free(program->functions);
    program->functions=nullptr;
    program->function_count=0;
    program->function_capacity=0;
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
        [DIAMOND_CLASS_THREAD_ERROR]={"ThreadError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_SQLITE3_ERROR]={"SQLite3Error",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_POSTGRES_ERROR]={"PostgreSQLError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_MYSQL_ERROR]={"MySQLError",DIAMOND_CLASS_STANDARD_ERROR},
        [DIAMOND_CLASS_NO_METHOD_ERROR]={"NoMethodError",DIAMOND_CLASS_STANDARD_ERROR},
    };
    program->range_class_index=UINT8_MAX;
    program->class_count=DIAMOND_BUILTIN_CLASS_COUNT;
    for(size_t index=0;index<DIAMOND_BUILTIN_CLASS_COUNT;index++) {
        DiamondClass *class=&program->classes[index];
        (void)snprintf(class->name,sizeof class->name,"%s",builtins[index].name);
        class->superclass=builtins[index].superclass;
        class->field_count=3;
        (void)snprintf(class->fields[0],DIAMOND_MAX_FUNCTION_NAME,"message");
        (void)snprintf(class->fields[1],DIAMOND_MAX_FUNCTION_NAME,"cause");
        (void)snprintf(class->fields[2],DIAMOND_MAX_FUNCTION_NAME,"backtrace");
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

/* The actual compile, run twice by diamond_compile below -- once
 * (discovery_pass=true) into a throwaway DiamondProgram purely to
 * register every class/module/interface's name/fields/methods and every
 * compiler-created function slot
 * regardless of textual order, then again (discovery_pass=false) into
 * the real, caller-supplied program, now with every declaration already
 * known. `program` must already be freshly diamond_program_init'd (with
 * allow_top_level_redefinition already restored onto it) by the caller --
 * both passes need that same prologue against their own separate
 * program, so it stays there rather than duplicated in here. */
static bool run_compile_pass(const char *source, DiamondProgram *program,
                             DiamondDiagnostic *diagnostic, bool discovery_pass) {
    *diagnostic = (DiamondDiagnostic){};
    Compiler compiler = {
        .source = source,
        .program = program,
        .function = &program->entry,
        .current_class = -1,
        .current_module = -1,
        .contextual_block_return_set = -1,
        .expected_expression_type_set = -1,
        .current_return_type = -1,
        .current_exception = -1,
        .current_retry_target = SIZE_MAX,
        .diagnostic = diagnostic,
        .discovery_pass = discovery_pass,
    };
    diamond_lexer_init(&compiler.lexer, source);
    compiler.current = diamond_lexer_next(&compiler.lexer);
    if(compiler.current.kind==DIAMOND_TOKEN_ERROR)
        fail(&compiler,compiler.current.span,"unexpected character");

    const uint16_t result = compile_sequence(&compiler);
    if (!compiler.failed && compiler.current.kind != DIAMOND_TOKEN_EOF) {
        fail(&compiler, compiler.current.span, "unexpected block terminator");
    }
    program->entry.register_count = compiler.next_register;
    if (!compiler.failed) {
        record_scope_locals(&compiler,0,compiler.local_count,strlen(source));
        program->entry.body_end=strlen(source);
        emit_instruction(&compiler, DIAMOND_OP_RETURN, result, 0, 0, 1);
        for(size_t class_index=0;class_index<program->class_count;class_index++) {
            DiamondClass *class=&program->classes[class_index];
            for(size_t field_count=0;field_count<=class->field_count;field_count++) {
                class->shapes[field_count]=(DiamondShape){
                    .class=class,.field_count=(uint8_t)field_count};
            }
            /* Resolved by name, once, here -- never hardcoded, since a
             * conservative prelude that skips optional modules can shift
             * which index a *later*-defined class lands at (confirmed
             * directly; Range itself is safe today since lib/core.di
             * defines it before any conditionally-included module, but
             * nothing should assume that stays true). See DiamondChunk's
             * own comment (src/vm.h) for what this is for. */
            if(strcmp(class->name,"Range")==0)
                program->range_class_index=(uint8_t)class_index;
        }
    }
    return !compiler.failed;
}

/* Compiles `source` twice. The first pass (see run_compile_pass's own
 * comment) is a throwaway declaration-discovery compile: it runs into a
 * separate, temporary DiamondProgram whose bytecode is discarded in
 * full, but whose class/module/interface tables (names, fields,
 * methods -- fully populated regardless of textual order, since a real
 * error there is the only thing that can stop this pass) are copied
 * into the second, real pass's own program before it starts. This is
 * what lets `SomeClass.new(...)`/`SomeClass.someMethod(...)`/a type
 * annotation reference a class/module/interface declared *later* in the
 * same source -- see docs/roadmap.md and the forward-declarations plan.
 * A real syntax/semantic error unrelated to a forward reference still
 * fails identically in the first pass, and the second pass never runs --
 * the caller sees exactly one clean error, same as a single-pass compile
 * always has. Superclass/base-interface resolution is deliberately left
 * exactly as strict as before in both passes (see compile_class's own
 * comment on `claiming`): that needs the referenced class already fully
 * compiled, not just known by name, which this doesn't attempt to fix. */
bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic) {
    const bool allow_top_level_redefinition = program->allow_top_level_redefinition;
    diamond_program_free(program);

    DiamondProgram *discovery = calloc(1, sizeof *discovery);
    diamond_program_init(discovery);
    discovery->allow_top_level_redefinition = allow_top_level_redefinition;
    DiamondDiagnostic discovery_diagnostic = {0};
    const bool discovered = run_compile_pass(
        source, discovery, &discovery_diagnostic, /*discovery_pass=*/true);

    diamond_program_init(program);
    program->allow_top_level_redefinition = allow_top_level_redefinition;
    if (!discovered) {
        *diagnostic = discovery_diagnostic;
        diamond_program_free(discovery);
        free(discovery);
        return false;
    }

    /* Carry discovery's fully-populated class/module/interface tables
     * (names, fields, methods -- everything but real bytecode) into the
     * second, real pass's own program, *before* that pass starts, so
     * every declaration is already known regardless of textual order.
     * `classes`/`interfaces`/`modules` are plain, pointer-free embedded
     * arrays (src/compiler.h) -- a byte copy is enough, no separate
     * ownership to transfer. compile_class/compile_module/
     * compile_interface's own claiming logic (see their comments) is
     * what makes the second pass treat every copied entry as its own
     * pre-reserved slot instead of a duplicate declaration. */
    memcpy(program->classes, discovery->classes, sizeof program->classes);
    program->class_count = discovery->class_count;
    memcpy(program->interfaces, discovery->interfaces, sizeof program->interfaces);
    program->interface_count = discovery->interface_count;
    memcpy(program->modules, discovery->modules, sizeof program->modules);
    program->module_count = discovery->module_count;

    /* Reserve every compiler-created function at its discovery-pass index.
     * The real pass claims the slots in the same source order, preserving
     * both early top-level calls and copied class/module method indices. */
    if(!allow_top_level_redefinition) {
        for(size_t index=0;index<discovery->function_count;index++) {
            const DiamondFunction *discovered_function=discovery->functions[index];
            DiamondFunction *reserved=diamond_program_add_function(program);
            if(reserved==nullptr) {
                diamond_program_free(discovery);free(discovery);
                *diagnostic=(DiamondDiagnostic){.message="out of memory"};
                return false;
            }
            if(!diamond_function_copy(reserved,discovered_function)) {
                diamond_program_free(discovery);free(discovery);
                *diagnostic=(DiamondDiagnostic){.message="out of memory"};
                return false;
            }
            reserved->declared_by_discovery=true;
        }
    }

    /* Mark every *user* class/module entry just copied as "populated by
     * a pass other than the one about to run" -- compile_class/
     * compile_module only reset declared_by_discovery to false the
     * instant *they themselves* touch a slot (needed so a genuine reopen
     * within one pass's own walk merges instead of resetting), so by the
     * time discovery finishes, every entry it created already reads
     * false again. Without this explicit re-marking here, the real pass
     * would treat discovery's leftover (bytecode-discarded) method/field
     * tables as "already mine" and merge its own real methods on top
     * instead of resetting first -- silently duplicating every method
     * table entry. Built-in classes (Exception and friends,
     * DIAMOND_BUILTIN_CLASS_COUNT of them, registered identically by
     * both programs' own diamond_program_init and never re-declared by
     * user code) are skipped -- nothing ever "reopens" them through this
     * path, so there's nothing to mark stale. */
    for(size_t index=DIAMOND_BUILTIN_CLASS_COUNT;index<program->class_count;index++)
        program->classes[index].declared_by_discovery=true;
    for(size_t index=0;index<program->module_count;index++)
        program->modules[index].declared_by_discovery=true;

    diamond_program_free(discovery);
    free(discovery);

    const bool compiled=run_compile_pass(
        source,program,diagnostic,/*discovery_pass=*/false);
    if(compiled)
        for(size_t index=0;index<program->interface_count;index++)
            program->interfaces[index].type_sets=program->entry.type_sets;
    return compiled;
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
        .modules=program->modules,
        .module_count=program->module_count,
        .register_count=program->entry.register_count,
        .range_class_index=program->range_class_index,
    };
}
