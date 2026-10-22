#ifndef DIAMOND_COMPILER_INTERNAL_H
#define DIAMOND_COMPILER_INTERNAL_H

/* Declarations shared between src/compiler.c and the compiler_*.c files split out of it.
 *
 * The compiler is one pass and its parts call each other freely: a call parser reaches the
 * expression parser, which reaches the definition compiler. What those files share is declared
 * here, along with the Compiler state they all thread through. This is not the public interface
 * (that is compiler.h). Nothing here is stable, and the symbols are hidden, so they do not widen
 * what a shared build of the runtime exports. */

#include "compiler.h"
#include "jit.h"
#include <stdint.h>

#define DIAMOND_INTERNAL __attribute__((visibility("hidden")))

enum { TYPE_UNKNOWN = DIAMOND_TYPE_NONE };

typedef struct ToolingDependencies {
    bool forward_calls;
    /* Indices in the immutable completed snapshot, including unknown returns:
     * an unknown callee becoming known must trigger another pass too. */
    bool *used;
    size_t count;
} ToolingDependencies;

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
    /* `**`: tighter than unary minus (`-2 ** 2` is -4) and
     * right-associative, as in Ruby. */
    PREC_POWER,
} Precedence;

typedef struct Local {
    DiamondSpan name;
    uint16_t reg;
    bool captured;
    uint32_t alias_identity;
} Local;

typedef struct LoopContext {
    struct LoopContext *previous;
    /* compiler->begin_depth where the loop started: a break/next/redo from deeper than this
     * is inside begin blocks that must be left (running their ensure bodies) on the way out. */
    size_t begin_depth;
    size_t continue_target;
    size_t redo_target;
    uint16_t result_register;
    size_t breaks[64];
    size_t break_count;
    size_t flow_reg_count;
    uint16_t *exit_types;
    int32_t *exit_sets;
    int32_t *exit_tooling_sets;
    bool exit_initialized;
    uint16_t result_type;
    int32_t result_set;
    int32_t result_tooling_set;
    /* Bounded by DIAMOND_MAX_LOCALS (64), unlike exit_types/exit_sets above,
     * so these travel inline with no malloc dance. */
    size_t flow_local_count;
    uint32_t exit_alias[DIAMOND_MAX_LOCALS];
    /* A local first bound inside the loop body (no binding before the loop
     * at all), joined across every exit point (the zero-iteration path
     * before the body if one exists, every `break`, and the natural
     * loop-back/condition-false path) the same seed-vs-merge way
     * merge_case_new_locals joins one across every `when`/`else` clause --
     * see that function's own comment; merge_loop_exit reuses it as-is. No
     * reset-before-this-exit step is needed here the way parse_case_
     * branches needs one before each `when`/`else`: a loop's body compiles
     * once, sequentially, so each exit point's own compile-time state is
     * already exactly what a mutually-exclusive branch's freshly-reset
     * state would be -- correct as found. */
    size_t new_local_count;
    uint16_t new_types[DIAMOND_MAX_LOCALS];
    int32_t new_sets[DIAMOND_MAX_LOCALS];
    uint32_t new_alias[DIAMOND_MAX_LOCALS];
} LoopContext;

enum { DIAMOND_MAX_NARROWING_FACTS = 8 };

typedef struct NarrowingFact {
    uint16_t reg;
    int32_t type_set;
    /* A companion editor graph shares the same proof slot as a checked fact.
     * Tooling-only proofs are never applied to checked compiler types. */
    int32_t tooling_type_set;
    bool tooling_only;
    bool has_tooling;
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

/* A plain CALL to a different function that sits in tail position, recorded
 * by maybe_rewrite_self_tail_call and rewritten to DIAMOND_OP_TAIL_CALL by
 * rewrite_cyclic_tail_calls only when the caller and callee can reach each
 * other through such calls. */
typedef struct TailCallCandidate {
    DiamondFunction *caller;
    size_t offset;
    uint16_t callee;
} TailCallCandidate;

typedef struct Compiler {
    const char *source;
    DiamondLexer lexer;
    DiamondToken current;
    DiamondToken previous;
    DiamondProgram *program;
    DiamondFunction *function;
    /* >0 anywhere lexically inside a begin/rescue/ensure block of the
     * function currently being compiled (compile_begin increments/
     * decrements around its own try/rescue/else/ensure bodies) --
     * disqualifies a self-tail-call from DIAMOND_OP_TAIL_CALL rewriting
     * (see compile_return's own comment): a live handler's jump target
     * is an offset into the *current* chunk's bytecode, which reusing
     * the same run_chunk invocation for "the next iteration" must never
     * invalidate. Saved to 0 and restored around compiling any nested
     * function's own body (every site that reassigns `function` above)
     * -- a def nested inside an outer begin block starts its own body
     * fresh, not already inside anything, regardless of its lexical
     * position in the outer function's own source. */
    size_t begin_depth;
    /* Records exactly where the most recently emitted plain (non-generic,
     * non-spread, non-method/singleton) DIAMOND_OP_CALL sits, for
     * compile_return's own tail-call peephole rewrite (see
     * maybe_rewrite_self_tail_call) -- set only at parse_call's own
     * plain-CALL emission site. Deliberately NOT scoped/saved-restored
     * around a nested function's own compilation the way begin_depth is:
     * code_count is per-DiamondFunction (each has its own separate code
     * buffer), so comparing last_plain_call_code_count against
     * compiler->function's *own current* code_count already self-
     * invalidates whenever anything else -- including an entire nested
     * function's own body -- was compiled in between, without needing
     * explicit scoping. This is deliberately NOT reconstructed by
     * decoding bytes backward from code_count (an earlier version of
     * this feature did exactly that, assuming the preceding instruction
     * was exactly 8 bytes without proving it actually was one real CALL
     * instruction boundary -- a real, found-by-testing bug: a coincidental
     * byte match on unrelated bytecode got misidentified as a self-call
     * and corrupted, causing an infinite loop in a completely unrelated
     * function that never actually called itself at all). Tracking the
     * exact offset at emission time instead makes this sound by
     * construction rather than by inference. */
    size_t last_plain_call_code_count;
    size_t last_plain_call_offset;
    /* A jump was patched to the byte right after that call: its result is not
     * the only value that can reach what follows. */
    bool last_plain_call_label_after;
    uint16_t last_plain_call_destination;
    uint16_t last_plain_call_function_index;
    DiamondFunction *last_plain_call_function;
    TailCallCandidate *tail_call_candidates;
    size_t tail_call_candidate_count;
    size_t tail_call_candidate_capacity;
    DiamondDiagnostic *diagnostic;
    Local locals[DIAMOND_MAX_LOCALS];
    size_t local_count;
    uint32_t next_alias_identity;
    uint16_t next_register;
    /* The byte offset (into compiler->function->code[]) of the most
     * recently emitted instruction, updated on every emit_opcode call --
     * see compile_assignment_store's own "reassigning an existing local"
     * comment (register recycling Stage 2) for the one thing that reads
     * this. Reset to SIZE_MAX (an always-out-of-bounds sentinel, checked
     * against code_count before ever being used to index code[]) at
     * every one of next_register's own resets, so a stale offset from an
     * outer/sibling function body being compiled can never be read
     * against a *different* function's own code array. */
    size_t last_instruction_offset;
    int current_class;
    int current_module;
    bool methods_private;
    bool methods_protected;
    /* While a struct's hand-written body compiles: the class whose
     * *generated* == / to_s a same-named `def` may replace (rather than
     * collide with). -1 outside a struct body; each flag clears once
     * replaced. */
    int struct_replaceable_class;
    bool struct_replaceable_eq;
    bool struct_replaceable_to_s;
    bool module_function_mode;
    DiamondSpan current_method;
    bool in_method;
    /* True while compiler->function is itself a class/module singleton
     * method (`def self.name`). Tracked separately from in_method (which
     * covers ordinary instance methods too) because it drives whether a
     * closure nested *directly* inside this function should get the same
     * self-register-reservation/owner_class treatment an instance method
     * gets, even though the closure itself isn't a class member -- see compile_definition's own use. */
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
     * survives -- confirmed directly, not assumed.
     * Sticky across nested loops (an inner loop reached while this is
     * already true skips its own scan/premark -- see parse_while/
     * parse_loop's own comments). */
    bool loop_captures_pending;
    uint16_t known_types[DIAMOND_REGISTER_COUNT];
    int32_t known_type_sets[DIAMOND_REGISTER_COUNT];
    /* LSP-only inferred return facts. Kept separate from known_type_sets so
     * an unannotated callee can improve receiver tooling after assignment
     * without changing opcode selection or compile-time type checks. */
    int32_t tooling_type_sets[DIAMOND_REGISTER_COUNT];
    /* Set when a register was loaded via one INDEX_GET directly off a
     * tracked local's own register (`x[i]`, one level only -- a chained
     * `x[a][b]` sees a non-local `receiver` on its second INDEX_GET and
     * simply doesn't set this again). Lets a mutation through that nested
     * receiver (`x[i].push(v)`, `x[i][k] = v`) write the widened element
     * fact back into the *root* local's own nested contract, the same
     * expression-local bridge a chained `.method()()` or `x[a][b]=v` call
     * already relies on `receiver`/`mutation_receiver` locals for -- no
     * cross-statement persistence, so none of the control-flow-join
     * machinery alias_identity needs applies here. */
    bool has_index_provenance[DIAMOND_REGISTER_COUNT];
    uint16_t index_provenance_root[DIAMOND_REGISTER_COUNT];
    bool in_function;
    /* True while compiling a `do ... end` block body (and not a def nested
     * in one). There `next` outside any loop ends the block's current call,
     * like `return`, instead of being an error. */
    bool in_block;
    /* The nested def whose body is being compiled (nullptr outside one),
     * and its name: during discovery, a call to or reference of that name
     * marks it calls_itself. See DiamondFunction.calls_itself. */
    DiamondFunction *current_definition;
    DiamondSpan current_definition_name;
    /* The def whose body (or a do-block within it) is being compiled, and
     * its declared return type (an index into that def's own type sets, or
     * -1): a `return` inside a block returns from this def, so it's checked
     * against this type and marks the def as a non-local landing site.
     * Unchanged by compile_block; nullptr outside any def. */
    DiamondFunction *method_function;
    int method_return_type;
    DiamondSpan method_return_type_span;
    /* A block inside method_function returns from it: its unannotated
     * return type can't be inferred from its own body and returns. */
    bool method_has_block_return;
    /* Set while compiling a do-block body when it contains a `break` that
     * leaves the block (see compile_loop_control). */
    bool block_has_break;
    /* A call whose block can `break` returns the break value instead of
     * its usual result; parse_precedence drops that call's type facts. */
    bool block_break_pending;
    /* A do-block with a `break`, compiled but not yet handed to its call:
     * the next call opcode emitted is that call (see emit_opcode). */
    DiamondFunction *pending_block_call;
    /* The explicit `&name` parameter for the function currently being
     * compiled. In that lexical context `yield(...)` invokes this Callable;
     * without one, yield retains its fiber-suspension meaning. */
    bool has_current_block;
    uint16_t current_block_register;
    uint16_t current_block_type_set;
    bool has_contextual_block_types;
    uint8_t contextual_block_arity;
    uint16_t contextual_block_types[16];
    int32_t contextual_block_type_sets[16];
    int32_t contextual_block_return_set;
    /* The contextual parameter sets come from an editor-only element graph:
     * publish them as tooling facts, never as checked ones. */
    bool contextual_block_tooling_only;
    int32_t expected_expression_type_set;
    bool positional_spread_literal;
    size_t positional_spread_first;
    size_t positional_spread_count;
    uint16_t positional_spread_elements[DIAMOND_MAX_ARGUMENTS];
    size_t positional_spread_fixed_count;
    size_t positional_spread_index;
    uint16_t positional_spread_fixed[DIAMOND_MAX_DECLARED_PARAMETERS];
    int current_return_type;
    DiamondSpan current_return_type_span;
    /* Accumulates the union of every explicit `return value`'s own known
     * type/type-set seen so far within the *current* function body being
     * compiled -- purely advisory input to compile_definition's own
     * inferred_return_type_set inference (lsp/receiver.c's call-chain
     * resolution), never return_type_set itself (see that field's own
     * comment, src/vm.h, for why the two stay separate). Exactly the
     * incremental-accumulate shape merge_loop_exit already uses for a
     * loop's own `break` values, minus everything about locals/alias-
     * identity a `return` doesn't need (it exits the function outright,
     * not just one control-flow region within it) -- see compile_return's
     * own use. Saved/restored around a nested function body the same way
     * every other per-function field here already is (compile_definition/
     * compile_block's own outer_* dance), and reset to "not seen" at the
     * start of each one, so an inner def's own returns never leak into an
     * outer one's inference or vice versa. */
    /* Editor-only refinement uses a previous completed compile's return
     * facts for forward callees, never its bytecode or checked contracts. */
    const DiamondProgram *tooling_returns;
    ToolingDependencies *tooling_forward_calls;
    bool return_flow_seen;
    uint16_t return_flow_type;
    int32_t return_flow_set;
    /* Parallel advisory return facts may include inferred call results.
     * Never feed these into a block's checked return_type_set. */
    uint16_t return_tooling_type;
    int32_t return_tooling_set;
    LoopContext *current_loop;
    int current_exception;
    size_t current_retry_target;
    bool failed;
    Local enclosing_locals[DIAMOND_MAX_LOCALS];
    size_t enclosing_local_count;
    uint16_t capture_registers[DIAMOND_MAX_CAPTURES];
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
    bool namespace_constant_defined[DIAMOND_MAX_NAMESPACE_CONSTANTS];
    /* True only for the real (discovery_pass=false) pass of a
     * diamond_compile_with_breakpoints call -- compile_sequence emits a
     * DIAMOND_OP_BREAKPOINT_CHECK at the start of *every* statement
     * (see its own comment, src/vm.h) rather than only at a fixed,
     * compile-time-selected set of lines the way v1's DIAMOND_OP_DEBUGGER
     * did: which lines are actually armed is now a runtime-mutable set
     * (DiamondVm.debug_active_lines) a live `setBreakpoints` command can
     * change with no recompile, so the compiler no longer needs to know
     * which lines matter in advance -- only whether this is a debug
     * session at all. Never set for the discovery pass: its bytecode is
     * discarded, so instrumenting it would be wasted work, not
     * incorrect. */
    bool debug_mode;
    /* Real-pass cursor through discovery's pre-reserved function slots. */
    size_t next_function_claim;
    /* Pending call-site patches for a module_function/`def self.x` method
     * whose signature a module's own pre-scan already found (letting an
     * earlier-compiled sibling call it) but whose real function_index
     * isn't known yet (the def itself hasn't been compiled). Mirrors
     * emit_jump/patch_jump's own "reserve two placeholder bytes now,
     * patch them once the real value is known" pattern exactly, just
     * patching a function_index instead of a jump target, and (unlike a
     * jump, always within the same function) across function boundaries
     * -- see DIAMOND_UNRESOLVED_SINGLETON_FUNCTION and
     * prescan_singleton_signatures's own comment for the full
     * design. Scoped to the whole compiler, not per-module: simpler, and
     * declaration identity keeps nested scopes with the same method name separate. */
    struct {
        DiamondFunction *function;
        size_t operand;
        const DiamondMethod *method;
    } pending_singleton_fixups[64];
    size_t pending_singleton_fixup_count;
} Compiler;

/* Sentinel DiamondMethod.function_index for a module_function/`def self.x`
 * signature a pre-scan found ahead of the real def -- never a real index
 * (DIAMOND_MAX_FUNCTIONS itself, one past the last valid slot 0..
 * DIAMOND_MAX_FUNCTIONS-1, matching how program->function_count is a
 * strictly-less-than bound on every real function_index everywhere else
 * in this file). */
enum { DIAMOND_UNRESOLVED_SINGLETON_FUNCTION = DIAMOND_MAX_FUNCTIONS };

/* What the compiler can prove about `reg` against the annotation
 * `set_index`: it certainly satisfies it, only a runtime check can tell,
 * or it certainly doesn't. Shared by emit_type_check (annotations checked
 * where the value is produced) and check_call_argument_types (arguments to
 * a statically known callee). */
typedef enum TypeCheckVerdict {
    TYPE_CHECK_SATISFIED,
    TYPE_CHECK_NEEDS_RUNTIME,
    TYPE_CHECK_VIOLATED,
} TypeCheckVerdict;

typedef enum CollectionRelay {
    COLLECTION_RELAY_NONE,
    COLLECTION_RELAY_ELEMENT,
    COLLECTION_RELAY_NULLABLE_ELEMENT,
    COLLECTION_RELAY_ARRAY_SAME,
    COLLECTION_RELAY_KEYS,
    COLLECTION_RELAY_VALUES,
    COLLECTION_RELAY_NULLABLE_VALUE,
    COLLECTION_RELAY_FALLBACK,
    COLLECTION_RELAY_CONCAT,
    COLLECTION_RELAY_MAP,
    COLLECTION_RELAY_FLAT_MAP,
    COLLECTION_RELAY_NESTED_ARRAY,
    COLLECTION_RELAY_GROUP_BY,
    COLLECTION_RELAY_ZIP,
    COLLECTION_RELAY_TALLY,
    COLLECTION_RELAY_FETCH,
    COLLECTION_RELAY_MERGE,
    COLLECTION_RELAY_MAP_VALUES,
    COLLECTION_RELAY_PUSH,
    COLLECTION_RELAY_SUM
} CollectionRelay;

typedef struct CollectionRelayContract {
    const char *name;
    CollectionRelay relay;
} CollectionRelayContract;

/* `body while cond` / `body until cond`: the loop is compiled exactly like the block form
 * (same flow bookkeeping, same break/next context), but the source order is body first.
 * The condition is therefore compiled first by moving the lexer to just past the modifier,
 * then the lexer returns to the body, and finally it skips past the condition. */
typedef struct {
    DiamondLexer after_modifier;
} PostfixLoop;

typedef struct CaseFlowJoin {
    uint16_t *types;
    int32_t *sets;
    int32_t *tooling_sets;
    bool *varied;
    bool initialized;
    uint16_t result_type;
    int32_t result_set;
    int32_t result_tooling_set;
    /* Bounded by DIAMOND_MAX_LOCALS (64), unlike types/sets/varied above,
     * so these travel inline with no malloc dance. */
    uint32_t alias[DIAMOND_MAX_LOCALS];
    /* A local first bound inside *some* `when`/`else` clause (no binding
     * before the `case` at all). Slots [0, new_local_count) have been
     * reached by at least one earlier-compiled branch already and merge
     * normally from here on; a slot reached for the first time either
     * seeds directly (this is the very first branch of the whole case
     * statement, so there's no earlier sibling to have implicitly skipped
     * it) or seeds as `merge(Nil, this branch's value)` (every other case:
     * some earlier-compiled branch already ran without this local existing
     * at all, so it implicitly contributed Nil down that path) -- see
     * merge_case_new_locals. */
    size_t new_local_count;
    uint16_t new_types[DIAMOND_MAX_LOCALS];
    int32_t new_sets[DIAMOND_MAX_LOCALS];
    uint32_t new_alias[DIAMOND_MAX_LOCALS];
} CaseFlowJoin;

/* Tracks whether every member of a `case` subject's known closed type is
 * covered by some unguarded `when` branch, so reaching `end` with no
 * `else` and an uncovered member is a compile error instead of silently
 * returning Nil (see parse_case). `required_ids` is populated from either
 * of two independent sources, never both at once:
 *
 * - an explicit multi-member union (compiler->known_type_sets[subject]
 *   >=0) whose every member is either Nil or a concrete user class -- a
 *   union containing any native scalar/container type, generic variable,
 *   or interface member has no current `when` syntax that can prove
 *   "this whole member is covered" (native type names aren't class-
 *   pattern values -- see docs/core-syntax.md's Case/when section), so
 *   such a case is left exactly as unchecked as it is today rather than
 *   either inventing new pattern syntax or producing false positives;
 * - a single (non-union) subject type naming a `sealed` class -- every
 *   *direct* subclass of it (found via a linear scan of compiler->
 *   program->classes[] for a matching `.superclass`), never recursed
 *   further, since `when Circle` already matches Circle-or-any-of-its-
 *   own-subclasses at runtime (CASE_MATCH) regardless of this feature.
 *   Only meaningful because a sealed class can't be instantiated
 *   directly (see the `.new`-dispatch site in src/compiler.c) -- with no
 *   possible direct-Shape instance, every runtime value is provably one
 *   of these direct subclasses (or deeper), so enumerating just them is
 *   sound. Not composed with the union case above: a union containing a
 *   sealed class as one of several members does not recursively expand
 *   into that member's own subclasses.
 *
 * Deliberately conservative in one more way, for both sources: only a
 * bare class-name/`nil` *scalar* `when` value marks a member covered,
 * never an Array/Hash/Object structural pattern (even an empty `Class{}`
 * class-only guard) and never a guarded `when ... if` clause -- see
 * parse_case_branches' own comment at the point this is populated. */
typedef struct CaseExhaustiveness {
    bool active;
    uint16_t required_ids[DIAMOND_MAX_UNION_TYPES];
    uint8_t required_count;
    bool covered[DIAMOND_MAX_UNION_TYPES];
    DiamondSpan case_span;
} CaseExhaustiveness;

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
    /* CASE_OBJECT_GROUP only: the resolved class index (find_class_
     * qualified_or_scoped's own result, cast down the same way
     * DIAMOND_OP_LOAD_CLASS's own emitted operand already is) --
     * value_register only holds a runtime Class *value*, not this raw
     * index, and exhaustiveness coverage (an empty `Class{}` guard
     * covering that class the same way a bare `when Class` does) needs
     * the index itself, not something to reconstruct from a register at
     * compile time. Left 0 (meaningless) for every other node kind. */
    uint16_t class_index;
} CaseArrayNode;

typedef struct CaseBinding {
    DiamondSpan name;
    uint16_t reg;
} CaseBinding;

/* Saved/restored around each of `struct`'s own three hand-emitted method
 * bodies (initialize/==/to_s) -- the same "swap compiler->function,
 * regenerate registers from 0, restore known_types/known_type_sets
 * afterward" dance compile_definition's own (much larger) save/restore
 * block performs whenever a nested `def` is compiled, narrowed to just
 * the state this generator's own straight-line emit_instruction/
 * allocate_register/emit_jump/patch_jump calls actually touch. Nothing
 * else compile_definition also saves (locals, current_method, begin_
 * depth, current_loop, etc.) is relevant here: this generator never
 * parses an identifier, never calls compile_return/compile_begin, and
 * never opens a block -- only compiler->function/next_register/
 * known_types/known_type_sets are ever read or written by the plain
 * emission helpers this uses. See compile_definition's own copy of the
 * inline-then-heap-fallback comment for why the 256 cap needs a fallback
 * rather than just being a fixed bound. */
typedef struct StructMethodState {
    DiamondFunction *outer_function;
    uint16_t outer_next_register;
    uint16_t inline_known_types[256];
    int32_t inline_known_type_sets[256];
    uint16_t *heap_known_types;
    int32_t *heap_known_type_sets;
} StructMethodState;

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


DIAMOND_INTERNAL void fail(Compiler *compiler, DiamondSpan span, const char *message);
DIAMOND_INTERNAL const char *closed_type_name(const Compiler *compiler,uint16_t id);
DIAMOND_INTERNAL void advance_token(Compiler *compiler);
DIAMOND_INTERNAL void skip_newlines(Compiler *compiler);
DIAMOND_INTERNAL bool emit_byte(Compiler *compiler, uint8_t byte);
DIAMOND_INTERNAL bool emit_opcode(Compiler *compiler, DiamondOpCode opcode);
DIAMOND_INTERNAL bool emit_function_index(Compiler *compiler, size_t function_index);
DIAMOND_INTERNAL bool emit_register(Compiler *compiler, uint16_t reg);
DIAMOND_INTERNAL bool emit_instruction(Compiler *compiler, DiamondOpCode opcode,
                             uint16_t a, uint16_t b, uint16_t c, size_t operands);
DIAMOND_INTERNAL uint16_t allocate_register(Compiler *compiler);
DIAMOND_INTERNAL bool known_type_satisfies_one(const Compiler *compiler, uint16_t known,
                                     uint16_t expected);
DIAMOND_INTERNAL void emit_type_check(Compiler *compiler, uint16_t reg, uint16_t set_index,
                            DiamondSpan span);
DIAMOND_INTERNAL uint16_t add_constant(Compiler *compiler, DiamondValue value);
DIAMOND_INTERNAL bool reserve_type_sets(Compiler *compiler,size_t additional);
DIAMOND_INTERNAL uint16_t concrete_type_set(Compiler *compiler,uint16_t type);
DIAMOND_INTERNAL int32_t join_type_set_indices(Compiler *compiler,int32_t left,
        int32_t right);
DIAMOND_INTERNAL int32_t joined_value_type_set(Compiler *compiler,
        const uint16_t *values,size_t count);
DIAMOND_INTERNAL int32_t collection_type_set(Compiler *compiler,
        uint16_t type,int32_t argument_set,int32_t second_argument_set);
DIAMOND_INTERNAL void record_collection_type_set(Compiler *compiler,uint16_t reg,
        uint16_t type,int32_t argument_set,int32_t second_argument_set);
DIAMOND_INTERNAL int32_t array_element_type_set(const Compiler *compiler,uint16_t reg);
DIAMOND_INTERNAL uint16_t clone_type_set_into_current(Compiler *compiler,
        const DiamondTypeSet *source_sets,size_t source_count,
        uint16_t source_index);
DIAMOND_INTERNAL uint16_t clone_substituted_type_set(Compiler *compiler,
        const DiamondFunction *target,uint16_t source_index,
        const uint16_t *bindings,size_t binding_count,bool *resolved);
DIAMOND_INTERNAL void publish_known_type_set(Compiler *compiler,uint16_t reg,
        uint16_t set_index);
DIAMOND_INTERNAL void publish_call_return_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target,const uint16_t *bindings,
        size_t binding_count);
DIAMOND_INTERNAL void publish_function_callable_type(Compiler *compiler,uint16_t reg,
        const DiamondFunction *target);
DIAMOND_INTERNAL uint16_t add_string_range(Compiler *compiler,size_t start,size_t length,
                                DiamondSpan span);
DIAMOND_INTERNAL uint16_t add_string(Compiler *compiler,DiamondSpan span);
DIAMOND_INTERNAL uint16_t add_name_string(Compiler *compiler, DiamondSpan span);
DIAMOND_INTERNAL size_t emit_jump(Compiler *compiler, DiamondOpCode opcode,
                        uint16_t condition);
DIAMOND_INTERNAL void patch_jump(Compiler *compiler, size_t operand, size_t target);
DIAMOND_INTERNAL void emit_pending_singleton_function_index(Compiler *compiler,
        DiamondSpan span, const DiamondMethod *method);
DIAMOND_INTERNAL void resolve_pending_singleton_fixups(Compiler *compiler,
        const DiamondMethod *method, size_t function_index);
DIAMOND_INTERNAL void emit_absolute_jump(Compiler *compiler, size_t target);
DIAMOND_INTERNAL bool spans_equal(const Compiler *compiler, DiamondSpan left,
                        DiamondSpan right);
DIAMOND_INTERNAL int find_local(const Compiler *compiler, DiamondSpan name);
DIAMOND_INTERNAL bool register_is_named(const Compiler *compiler, uint16_t reg);
DIAMOND_INTERNAL bool try_rewrite_producer_destination(Compiler *compiler, uint16_t value,
                                              uint16_t destination);
DIAMOND_INTERNAL uint16_t define_local_with_register(Compiler *compiler, DiamondSpan name,
                                            uint16_t preferred_register);
DIAMOND_INTERNAL uint16_t define_local(Compiler *compiler, DiamondSpan name);
DIAMOND_INTERNAL uint16_t parse_string(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_literal(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_identifier(Compiler *compiler);
DIAMOND_INTERNAL int find_function(const Compiler *compiler, DiamondSpan name);
DIAMOND_INTERNAL bool name_equals(const Compiler *compiler, const char *candidate,
                        DiamondSpan name, bool skip_at);
DIAMOND_INTERNAL DiamondFunction *compiler_add_function(Compiler *compiler,
                                               size_t *function_index);
DIAMOND_INTERNAL int find_class(const Compiler *compiler, DiamondSpan name);
DIAMOND_INTERNAL int find_interface(const Compiler *compiler,DiamondSpan name);
DIAMOND_INTERNAL int find_module(const Compiler *compiler,DiamondSpan name);
DIAMOND_INTERNAL int find_class_name(const Compiler *compiler,const char *name);
DIAMOND_INTERNAL int find_class_qualified_or_scoped(const Compiler *compiler,const char *name);
DIAMOND_INTERNAL int find_interface_name(const Compiler *compiler,const char *name);
DIAMOND_INTERNAL int find_module_name(const Compiler *compiler,const char *name);
DIAMOND_INTERNAL void record_included_module(uint8_t *list,uint8_t *count,uint8_t module_index);
DIAMOND_INTERNAL int find_lexical_constant_name(const Compiler *compiler,const char *name);
DIAMOND_INTERNAL int find_qualified_inherited_constant(const Compiler *compiler,
                                             const char *qualified);
DIAMOND_INTERNAL int find_namespace_constant(const Compiler *compiler,DiamondSpan name);
DIAMOND_INTERNAL uint16_t store_namespace_constant(Compiler *compiler,DiamondSpan name,
                                         uint16_t value);
DIAMOND_INTERNAL bool append_span_name(Compiler *compiler,char *buffer,size_t capacity,
                             DiamondSpan span);
DIAMOND_INTERNAL bool declaration_name(Compiler *compiler,char *buffer,size_t capacity,
                             DiamondSpan local);
DIAMOND_INTERNAL bool consume_qualified_name(Compiler *compiler,char *buffer,
                                   size_t capacity);
DIAMOND_INTERNAL int resolve_type_name(Compiler *compiler,const char *name,
                             DiamondSpan diagnostic);
DIAMOND_INTERNAL int parse_type_annotation(Compiler *compiler);
DIAMOND_INTERNAL int field_index(Compiler *compiler, DiamondSpan name, bool create);
DIAMOND_INTERNAL int class_variable_owner(Compiler *compiler, DiamondSpan name);
DIAMOND_INTERNAL int class_variable_index(Compiler *compiler, DiamondSpan name, bool create);
DIAMOND_INTERNAL uint16_t module_field_name(Compiler *compiler,DiamondSpan name);
DIAMOND_INTERNAL bool call_arguments_have_keyword(Compiler *compiler);
DIAMOND_INTERNAL uint16_t emit_argument_array(Compiler *compiler,const uint16_t *values,
        size_t count);
DIAMOND_INTERNAL uint16_t parse_dynamic_keyword_arguments(Compiler *compiler,
        DiamondSpan *keyword_names,uint16_t *keyword_values,
        size_t *keyword_count,const DiamondFunction *target);
DIAMOND_INTERNAL bool call_arguments_have_spread(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_spread_argument_array(Compiler *compiler,
        const DiamondFunction *keyword_function,uint8_t *keyword_slots,
        uint16_t *keyword_registers,size_t *keyword_count,
        const DiamondFunction *expected_function,size_t expected_count);
DIAMOND_INTERNAL uint16_t parse_closure_call_arguments(Compiler *compiler, uint16_t callable);
DIAMOND_INTERNAL uint16_t load_current_block(Compiler *compiler);
DIAMOND_INTERNAL uint16_t callable_type_set_index(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_contextual_typed_block(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_index,
        const uint16_t *type_arguments,size_t type_argument_count);
DIAMOND_INTERNAL uint16_t parse_expected_argument(Compiler *compiler,
        const DiamondFunction *target,size_t parameter);
DIAMOND_INTERNAL size_t infer_contextual_type_arguments(Compiler *compiler,
        const DiamondFunction *target,const uint16_t *arguments,
        size_t argument_count,uint16_t *bindings);
DIAMOND_INTERNAL size_t infer_contextual_spread_arguments(Compiler *compiler,
        const DiamondFunction *target,uint16_t spread,size_t parameter_count,
        uint16_t *bindings);
DIAMOND_INTERNAL void infer_contextual_keyword_arguments(Compiler *compiler,
        const DiamondFunction *target,const DiamondSpan *names,
        const uint16_t *values,size_t count,uint16_t *bindings);
DIAMOND_INTERNAL uint16_t parse_call(Compiler *compiler, DiamondSpan name);
DIAMOND_INTERNAL bool infer_reference_type_arguments(Compiler *compiler,
        const DiamondFunction *target,size_t parameter_offset,size_t arity,
        uint16_t *bindings);
DIAMOND_INTERNAL uint16_t parse_singleton_call(Compiler *compiler,
                                    const DiamondMethod *method,
                                    DiamondSpan namespace_name,
                                    int receiver_class_index);
DIAMOND_INTERNAL bool singleton_call_name_equals(const Compiler *compiler,
                                       const char *candidate,DiamondSpan name);
DIAMOND_INTERNAL uint16_t parse_redefine_method_call(Compiler *compiler, int class_index);
DIAMOND_INTERNAL uint16_t parse_define_method_call(Compiler *compiler, int class_index);
DIAMOND_INTERNAL uint16_t parse_compile_method_call(Compiler *compiler, int class_index);
DIAMOND_INTERNAL uint16_t parse_fiber_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_thread_new_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_channel_new_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_supervisor_new_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t plain_type_set_index(Compiler *compiler,uint16_t type_id);
DIAMOND_INTERNAL uint16_t parse_array_new_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_dir_entries_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_file_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_sqlite3_open_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_postgres_open_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_mysql_open_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_regexp_new_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_program_builder_new_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_tcp_connect_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_tcp_listen_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_tls_connect_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_tls_listen_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_io_poll_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_dns_resolve_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_udp_socket_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_signal_trap_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_chr_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_to_float_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_to_int_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_json_stringify_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_to_sym_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_math_unary_call(Compiler *compiler, DiamondMathFunction id);
DIAMOND_INTERNAL uint16_t parse_math_binary_call(Compiler *compiler, DiamondMathFunction id);
DIAMOND_INTERNAL uint16_t parse_print_call(Compiler *compiler, uint8_t flags);
DIAMOND_INTERNAL uint16_t parse_p_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_gets_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_exit_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t emit_debugger_pause(Compiler *compiler);
DIAMOND_INTERNAL uint16_t emit_breakpoint_check(Compiler *compiler,size_t source_line_offset);
DIAMOND_INTERNAL uint16_t parse_debugger_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_time_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_process_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_bcrypt_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_gzip_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_tensor_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_base64_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_cipher_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_secure_random_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_crypto_call(Compiler *compiler,bool keyed);
DIAMOND_INTERNAL const DiamondFunction *constructor_signature(const Compiler *compiler,
        int class_index);
DIAMOND_INTERNAL uint16_t parse_name(Compiler *compiler);
DIAMOND_INTERNAL uint16_t emit_invoke_call(Compiler *compiler, uint16_t receiver,
        DiamondSpan method_name, bool writer_name,
        const uint16_t *type_arguments, size_t type_argument_count,
        const uint16_t *args, size_t count);
DIAMOND_INTERNAL uint16_t emit_invoke_named(Compiler *compiler, uint16_t receiver,
        const char *name, const uint16_t *args, size_t count);
DIAMOND_INTERNAL uint16_t emit_invoke_spread(Compiler *compiler,uint16_t receiver,
        DiamondSpan method_name,uint16_t spread);
DIAMOND_INTERNAL uint16_t emit_build_spread_arguments(Compiler *compiler,
        const uint16_t *prefix,size_t prefix_count,uint16_t spread,
        const uint16_t *suffix,size_t suffix_count,bool optional_block);
DIAMOND_INTERNAL const DiamondFunction *class_instance_signature(
        const Compiler *compiler,uint16_t type,DiamondSpan name);
DIAMOND_INTERNAL bool type_members_structurally_equal(const DiamondTypeMember *left,
        const DiamondTypeSet *left_sets,size_t left_count,
        const DiamondTypeMember *right,const DiamondTypeSet *right_sets,
        size_t right_count,size_t depth);
DIAMOND_INTERNAL bool type_sets_structurally_equal(const DiamondTypeSet *left_sets,
        size_t left_count,uint16_t left_index,
        const DiamondTypeSet *right_sets,size_t right_count,
        uint16_t right_index,size_t depth);
DIAMOND_INTERNAL const DiamondFunction *instance_call_signature(
        const Compiler *compiler,uint16_t receiver,DiamondSpan name,
        bool require_matching_return);
DIAMOND_INTERNAL int32_t joined_collection_argument(Compiler *compiler,
        int32_t collection_set_index,uint16_t collection_type,bool second);
DIAMOND_INTERNAL void publish_block_tooling_callable(Compiler *compiler,uint16_t reg,
        const DiamondFunction *block,uint16_t return_set);
DIAMOND_INTERNAL uint32_t alias_identity_for_value(Compiler *compiler,uint16_t reg);
DIAMOND_INTERNAL void propagate_collection_alias_fact(Compiler *compiler,uint16_t reg);
DIAMOND_INTERNAL void propagate_nested_mutation_writeback(Compiler *compiler,
        uint16_t mutation_receiver);
DIAMOND_INTERNAL void update_collection_mutation_type(Compiler *compiler,uint16_t receiver,
        const uint16_t *keys,size_t key_count,const uint16_t *values,
        size_t value_count);
DIAMOND_INTERNAL void publish_instance_return_type(Compiler *compiler,uint16_t reg,
        uint16_t receiver,
        int32_t receiver_set_index,DiamondSpan name,
        const DiamondFunction *matching_target,const uint16_t *bindings,
        size_t binding_count);
DIAMOND_INTERNAL uint16_t parse_invoke(Compiler *compiler, uint16_t receiver);
DIAMOND_INTERNAL uint16_t parse_invoke_named(Compiler *compiler, uint16_t receiver,
        uint16_t mutation_receiver, int32_t receiver_set_index, DiamondSpan name);
DIAMOND_INTERNAL uint16_t parse_self_class_method_call(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_self_class_method_arguments(Compiler *compiler,
        DiamondSpan name,bool writer_name);
DIAMOND_INTERNAL uint16_t parse_super(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_with_expected_set(Compiler *compiler,
        uint16_t expected_set);
DIAMOND_INTERNAL int32_t type_set_with_nil(Compiler *compiler,uint16_t source_index);
DIAMOND_INTERNAL void apply_type_set_fact(Compiler *compiler,uint16_t reg,int32_t set_index);
DIAMOND_INTERNAL void apply_narrowing_facts(Compiler *compiler,const NarrowingFact *facts,
                                  size_t count);
DIAMOND_INTERNAL void apply_scope_narrowing_facts(Compiler *compiler,const NarrowingFact *facts,
        size_t count);
DIAMOND_INTERNAL void merge_flow_types(Compiler *compiler,uint16_t left_type,int32_t left_set,
        uint16_t right_type,int32_t right_set,uint16_t *result_type,int32_t *result_set);
DIAMOND_INTERNAL int32_t merge_tooling_type_sets(const Compiler *compiler,int32_t left,
        int32_t right);
DIAMOND_INTERNAL uint32_t merge_alias_identity(uint32_t left,uint32_t right);
DIAMOND_INTERNAL bool register_is_local(const Compiler *compiler,uint16_t reg);
DIAMOND_INTERNAL void merge_new_local_facts(Compiler *compiler,bool earlier_branch_exists,
        size_t flow_local_count,size_t *new_local_count,uint16_t *new_types,
        int32_t *new_sets,uint32_t *new_alias);
DIAMOND_INTERNAL void merge_loop_exit(Compiler *compiler,LoopContext *loop,
        uint16_t result_type,int32_t result_set,int32_t result_tooling_set);
DIAMOND_INTERNAL uint16_t parse_index(Compiler *compiler,uint16_t receiver);
DIAMOND_INTERNAL bool consume_block_start(Compiler *compiler);
DIAMOND_INTERNAL bool consume_conditional_start(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_if(Compiler *compiler,bool inverted);
DIAMOND_INTERNAL uint16_t parse_prefix(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_binary_op(Compiler *compiler, DiamondTokenKind operator,
                                   uint16_t left, uint16_t right);
DIAMOND_INTERNAL uint16_t parse_case_branches(Compiler *compiler, uint16_t subject,
        size_t flow_reg_count, const uint16_t *entry_types,
        const int32_t *entry_sets,const int32_t *entry_tooling,
        size_t flow_local_count,
        const uint32_t *entry_alias, uint16_t destination,CaseFlowJoin *join,
        bool subjectless,CaseExhaustiveness *exhaustiveness);
DIAMOND_INTERNAL uint16_t parse_case(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_expression(Compiler *compiler);
DIAMOND_INTERNAL bool assignment_ahead(const Compiler *compiler);
DIAMOND_INTERNAL bool index_assignment_ahead(const Compiler *compiler);
DIAMOND_INTERNAL DiamondTokenKind postfix_modifier_ahead(const Compiler *compiler,
        DiamondLexer *modifier_end);
DIAMOND_INTERNAL void maybe_rewrite_self_tail_call(Compiler *compiler,
        uint16_t return_value_register);
DIAMOND_INTERNAL void rewrite_cyclic_tail_calls(const DiamondProgram *program,
        const TailCallCandidate *candidates,size_t candidate_count);
DIAMOND_INTERNAL uint16_t compile_return(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_yield(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_raise(Compiler *compiler);
DIAMOND_INTERNAL void record_scope_locals(Compiler *compiler,size_t start_index,
        size_t end_index,size_t valid_end);
DIAMOND_INTERNAL void record_scope_type_fact(Compiler *compiler,uint16_t reg,
        size_t effective_start);
DIAMOND_INTERNAL uint16_t compile_begin(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_retry(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_loop_control(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_block(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_definition(Compiler *compiler, bool captures_self);
DIAMOND_INTERNAL void compile_delegate(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_class(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_struct(Compiler *compiler);
DIAMOND_INTERNAL void prescan_singleton_signatures(Compiler *compiler,
        DiamondMethod *methods,size_t *method_count,bool class_scope);
DIAMOND_INTERNAL uint16_t compile_module(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_interface(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_assignment_store(Compiler *compiler, DiamondSpan name,
        bool instance_variable, bool class_variable, uint16_t value,
        uint16_t rhs_start_register);
DIAMOND_INTERNAL uint16_t compile_assignment(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_multi_assignment(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_sequence(Compiler *compiler);
DIAMOND_INTERNAL DiamondFunction *diamond_program_add_function(DiamondProgram *program);
DIAMOND_INTERNAL bool diamond_function_reserve_code(DiamondFunction *function,size_t capacity);
DIAMOND_INTERNAL bool diamond_function_reserve_constants(DiamondFunction *function,
                                         size_t capacity);
DIAMOND_INTERNAL bool diamond_function_reserve_strings(DiamondFunction *function,size_t capacity);
DIAMOND_INTERNAL bool diamond_function_reserve_type_sets(DiamondFunction *function,size_t capacity);
DIAMOND_INTERNAL bool diamond_function_copy(DiamondFunction *destination,
                           const DiamondFunction *source);
DIAMOND_INTERNAL void diamond_function_free_arrays(DiamondFunction *function);
DIAMOND_INTERNAL DiamondClass *diamond_program_class_slot(DiamondProgram *program,size_t index);
DIAMOND_INTERNAL void diamond_program_free(DiamondProgram *program);
DIAMOND_INTERNAL void diamond_program_init(DiamondProgram *program);
DIAMOND_INTERNAL bool diamond_compile_impl(const char *source, DiamondProgram *program,
                                 const DiamondProgram *template,
                                 bool debug_mode,
                                 DiamondDiagnostic *diagnostic,
                                 const DiamondProgram *tooling_returns,
                                 ToolingDependencies *tooling_forward_calls);
DIAMOND_INTERNAL bool diamond_compile(const char *source, DiamondProgram *program,
                     DiamondDiagnostic *diagnostic);

DIAMOND_INTERNAL bool type_members_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,DiamondTypeMember known,
    const DiamondTypeSet *expected_sets,DiamondTypeMember expected);
DIAMOND_INTERNAL bool type_sets_satisfy_across(const Compiler *compiler,
    const DiamondTypeSet *known_sets,uint16_t known_index,
    const DiamondTypeSet *expected_sets,uint16_t expected_index);
DIAMOND_INTERNAL void check_call_argument_types(Compiler *compiler,const DiamondFunction *callee,
        const uint16_t *arguments,size_t count,size_t parameter_offset,
        DiamondSpan span);
DIAMOND_INTERNAL bool grow_type_sets(DiamondFunction *function,size_t additional);
DIAMOND_INTERNAL void publish_array_literal_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *values,size_t count);
DIAMOND_INTERNAL void publish_hash_literal_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *keys,const uint16_t *values,size_t count);
DIAMOND_INTERNAL void publish_array_spread_tooling_type(Compiler *compiler,uint16_t reg,
        const uint16_t *prefix,size_t prefix_count,uint16_t spread,
        const uint16_t *suffix,size_t suffix_count);
DIAMOND_INTERNAL void publish_callable_return_type(Compiler *compiler,uint16_t reg,
        int32_t callable_set_index);
DIAMOND_INTERNAL uint16_t add_string_range_mode(Compiler *compiler,size_t start,size_t length,
                                DiamondSpan span,bool raw);
DIAMOND_INTERNAL Precedence token_precedence(DiamondTokenKind kind);
DIAMOND_INTERNAL bool power_operator_ahead(const Compiler *compiler);
DIAMOND_INTERNAL Precedence infix_precedence(const Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_integer(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_float(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_symbol(Compiler *compiler);
DIAMOND_INTERNAL int load_constant_receiver(Compiler *compiler,DiamondSpan name);
DIAMOND_INTERNAL void note_self_reference(Compiler *compiler,DiamondSpan name);
DIAMOND_INTERNAL uint16_t parse_array_literal(Compiler *compiler,
        const DiamondFunction *expected_function,size_t first_parameter,
        size_t parameter_count);
DIAMOND_INTERNAL bool type_members_equal(DiamondTypeMember left,DiamondTypeMember right);
DIAMOND_INTERNAL bool type_sets_equal_unordered(DiamondTypeSet left,DiamondTypeSet right);
DIAMOND_INTERNAL void set_index_provenance(Compiler *compiler,uint16_t destination,
        uint16_t source_receiver);
DIAMOND_INTERNAL void finish_loop_flow(Compiler *compiler,LoopContext *loop,
        size_t effective_start);
DIAMOND_INTERNAL void publish_indexed_result_type(Compiler *compiler,uint16_t destination,
        uint16_t receiver);
DIAMOND_INTERNAL void mark_locals_captured(Compiler *compiler);
DIAMOND_INTERNAL bool statement_may_capture(const Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_while(Compiler *compiler,bool inverted);
DIAMOND_INTERNAL uint16_t parse_postfix_while(Compiler *compiler,bool inverted,DiamondLexer after_modifier);
DIAMOND_INTERNAL uint16_t parse_loop(Compiler *compiler);
DIAMOND_INTERNAL uint16_t parse_precedence(Compiler *compiler, Precedence precedence);
DIAMOND_INTERNAL bool compound_assignment_ahead(const Compiler *compiler);
DIAMOND_INTERNAL bool index_compound_assignment_ahead(const Compiler *compiler);
DIAMOND_INTERNAL bool multi_assignment_ahead(const Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_index_assignment(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_index_compound_assignment(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_compound_assignment(Compiler *compiler);
DIAMOND_INTERNAL uint16_t compile_expression_statement(Compiler *compiler,bool has_postfix);

#endif
