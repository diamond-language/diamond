# Diamond roadmap

Milestones describe executable capabilities. Research priorities may reorder
future work.

## Completed foundation

### Frontend and bytecode

- Source spans and caret diagnostics.
- Pratt expression compiler with direct register-bytecode emission.
- Debug/release builds and bounds-safe bytecode disassembly.
- Inline (`-e`) and file execution.
- Comments, semicolon separators, digit separators, and trailing commas.

### Dynamic language core

- Integers with checked arithmetic, booleans, `nil`, and strings.
- Locals, assignment, comparisons, expression-valued `if` and `while`.
- Ruby-style truthiness and value-preserving `!`, `&&`, and `||`.
- Explicit `return`, `break`, and `next` with nested control-flow targeting.
- Named functions, positional parameters, recursion, arity checks, and isolated
  register frames.

### Objects

- Classes, constructors, instances, fields, methods, and explicit `self`.
- Single inheritance, overriding, inherited constructors/fields, and lexically
  anchored `super(arguments)`.
- Dynamic receiver dispatch through class metadata.

### Managed collections

- Strings with concatenation and value equality.
- Arrays with literals, printing, indexed reads and writes, and bounds errors.
- Hashes with literals, value-key lookup, missing-key `nil`, insertion,
  replacement, growth, and printing.
- Stop-the-world mark/sweep collection with explicit frame roots and recursive
  tracing of instances and collections.
- Stress collection before every eligible allocation.

### Gradual typing

- Optional parameter and return annotations.
- `Int`, `String`, `Bool`, `Nil`, `Array`, `Hash`, and nominal class types.
- Subtype-aware class checks and reusable unions of up to eight types.
- Guards on every implicit and explicit typed return path.
- Exact local type facts that remove proven guards and reject proven errors.
- Detailed expected/actual runtime type diagnostics.
- Per-instruction source coordinates in bytecode dumps.
- Source-mapped runtime stack traces across functions and methods.
- First-class nested closures with escaping, shared mutable capture cells across
  recursively nested lexical environments.
- VM-owned four-entry polymorphic caches for dynamic method call sites.
- Class-owned runtime shape chains with lazy instance field transitions.
- Value-carrying `raise` with source-mapped cross-call unwinding.
- Nested `begin`/`rescue` expressions with optional raised-value bindings.
- Primitive and subtype-aware nominal filters on rescue bindings.
- Pipe-separated rescue type sets with up to eight alternatives.
- Shape-guarded polymorphic inline caches for instance field reads and writes.
- Built-in exception hierarchy with rescuable VM runtime failures.
- `ensure` unwinding for normal completion, exceptions, and returns.
- General union annotations for parameters and returns.
- Persistent, recursively checked `Array[Element]` annotations.
- Persistent `Hash[Key, Value]` contracts for entries and mutations.
- C23-embedded Diamond core prelude with initial collection helpers.
- Native collection `length()` with Diamond-written last/fallback/predicate helpers.
- Flow-sensitive generic result facts for array and hash indexing.
- Growable arrays with guarded `push`/`pop` and Diamond iteration helpers.
- Branch-sensitive nil narrowing with conservative local-fact joins.
- Ordered hash iteration primitives with Diamond-written transforms.
- Non-throwing `is` predicates with primitive and nominal union narrowing.
- Runtime `Callable[n]` contracts for Diamond core callbacks.
- Declared `Callable[n, Return]` result contracts and typed array transforms.
- First structural interface, `Sized`, implemented by native containers and
  user classes that provide zero-arity `length`.
- User-declared structural interfaces with implicit name-and-arity conformance
  for native objects and inherited class methods.
- Function-safe structural signature checks with contravariant parameters and
  covariant returns.
- Endless expression-bodied function and method definitions.
- Trailing default arguments for functions, methods, constructors, and
  closures, with supplied-argument tracking in bytecode.
- Double-quoted expression interpolation with scalar and instance conversion.
- Compile-time multi-file `require` with relative paths, extension inference,
  load-once semantics, cycle detection, and dependency-aware diagnostics.
- Inherited `to_s` stringification for instances and cycle-safe recursive
  collection interpolation.
- Scoped generic function type-variable declarations with retained recursive
  metadata and explicit runtime erasure.
- Runtime generic binding from values and callable returns, with persistent
  primitive, nominal, and union substitutions on returned collections.
- Bounded recursive generic binding graphs for nested Array and Hash types.
- Typed Callable parameter signatures with contravariant inputs, covariant
  returns, zero-argument lists, generic inference, and diagnostic rendering.
- Generic inference from persistent Array and Hash contracts, including empty
  collections and recursively bound contracts from earlier generic calls.
- Explicit generic arguments for function and method calls, including nested
  types, outer generic forwarding, strict arity, and specialized bytecode.
- Reusable modules with class inclusion, receiver-aware generic methods,
  deterministic precedence, inheritance integration, and name diagnostics.
- Transitive module-to-module inclusion with imported/direct precedence and
  self-cycle diagnostics.
- Lexical module namespaces with nested modules/classes, `Outer::Name`
  resolution, qualified type annotations, and isolated local names.
- Symbolic module instance fields resolved through receiver-class field tables,
  shapes, and caches, including transitive state requirements.
- Nested interfaces and write-once namespace constants with lexical/qualified
  lookup, arbitrary expression values, cross-frame access, and GC rooting.
- Module singleton functions with qualified dispatch, defaults, typing,
  generics, nested namespaces, and strict separation from included methods.
- Class singleton methods with generic specialization, defaults, independent
  overrides, and superclass-chain lookup alongside dedicated constructors.
- Private instance methods across classes, inheritance, and modules, restricted
  to explicit `self` calls from method frames.
- Public/private declaration toggling with named private-call diagnostics.
- Generated `attr_reader`/`attr_writer` methods for classes and modules, using
  numeric or symbolic fields while retaining visibility and inheritance.
- User-defined writer methods with typed/default parameters, module state, and
  private self-dispatch, while preserving endless-method parsing.
- Combined `attr_accessor` generation for class and module fields.
- Optional gradual type contracts on generated attribute readers and writers.
- Generated `attr_predicate` readers backed by ordinary instance fields.
- Comma-separated multi-name reader, writer, and accessor declarations.
- Targeted multi-name public/private changes for locally defined and generated
  methods without altering subsequent-definition defaults.
- `module_function` exports that preserve signatures and generics while making
  includable copies private and avoiding a fabricated module instance.
- Scoped standalone `module_function` mode for exporting subsequent methods.
- Duplicate generated attribute-method diagnostics for classes and modules.
- Compile-time rejection of stateful `module_function` exports.
- Class and module singleton writer definitions and qualified invocation.
- Targeted visibility changes for generated and handwritten writer methods.
- Parenthesized reader, writer, and accessor declaration syntax.
- Parenthesized targeted visibility syntax.
- Parenthesized multi-target `module_function` exports.
- Writer-method `module_function` targeting and qualified export calls.
- Reader-only `attr` shorthand with bare and parenthesized multi-name forms.
- Descriptor-level `alias_method` support for class and module instance methods.
- Writer-method sources and targets in `alias_method`.
- Parenthesized `alias_method(new_name, existing_name)` declarations.
- Predicate function and method names ending in `?`.
- Bang function and method names ending in `!`.
- Suffixed-name integration with visibility, aliasing, and module exports.
- `unless` conditional expressions with `else` and type narrowing.
- `until` loops with ordinary `break` and `next` control flow.
- Loop-body `redo` without condition reevaluation.
- Value-bearing `break` results for `while` and `until` expressions.
- Chained `elsif` branches in conditional expressions.
- Optional `then` delimiters for conditional expressions.
- Optional `do` delimiters for `while` and `until` loops.
- Unary `not` keyword alongside `!`.
- Short-circuit `and` and `or` keyword operators.
- Normal-completion `else` branches in `begin`/`rescue` expressions.
- Bare re-raise of the current rescued exception.
- Rescue-local `retry` with stable ensure-frame behavior.
- Binding-free typed rescue filters.
- Ordered multiple rescue clauses with unmatched re-raise.
- Duplicate rescue-filter type diagnostics.

## Next priorities

1. Strengthen structured exception control flow and diagnostics.

## Later experiments

- Visibility controls.
- Bytecode quickening and type-specialized instructions.
- Fibers and cooperative concurrency.
- Structural interfaces and more capable flow typing.
- Native-code generation or a tracing/method JIT.
- Self-hosting selected compiler and standard-library components.

## Explicitly deferred

- Ruby compatibility.
- Stable bytecode and embedding APIs.
- Package management.
- Multi-platform support.
- Parallel execution.
