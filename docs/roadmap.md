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

## Next priorities

1. Generic type variables for reusable typed collection transforms.

## Later experiments

- Modules/mixins and singleton methods.
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
