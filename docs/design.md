# Diamond design

Diamond is a personal research language. These notes describe the current
implementation, followed by decisions that are intentionally still open.

Ruby familiarity is a surface and object-model influence only; behavioral or
library compatibility with Ruby is explicitly out of scope.

## Goals

- Ruby-like, expression-oriented syntax and object semantics.
- Gradual typing without a separate typed object model.
- A custom VM understandable from lexer through garbage collector.
- Fast experimentation over compatibility, portability, or ecosystem breadth.

## Implementation

Diamond is implemented in C23 and currently targets Linux with GCC 16. It uses
a register bytecode VM with 256 registers per call frame. Bytecode instructions
have one-byte opcodes and explicitly decoded operands; jumps contain absolute
16-bit bytecode offsets.

Runtime values use an explicit tagged union. Immediate values are `nil`,
booleans, and signed 64-bit integers. Strings, arrays, hashes, and instances are
managed heap objects with a common header. NaN boxing is deferred until
measurement shows that representation density is worth the complexity.

Integer arithmetic uses C23 checked arithmetic and reports overflow rather than
invoking C undefined behavior. Addition, subtraction, multiplication, and
negation all detect overflow through `ckd_add`/`ckd_sub`/`ckd_mul`; division
additionally special-cases `INT64_MIN / -1`, the classic two's-complement
overflow that checked-arithmetic division alone would not catch, since the
mathematically correct magnitude has no representable positive counterpart.
Every overflow raises the rescuable `RangeError` class rather than wrapping
or invoking undefined behavior, on both the generic and quickened arithmetic
opcode paths.

Nested Diamond calls recurse through the C call stack, one native activation
per call depth, and a call-depth counter enforces a hard ceiling
(`DIAMOND_MAX_CALL_DEPTH`) so uncontrolled recursion fails as a rescuable
`SystemStackError` instead of exhausting the process's real native stack. The
ceiling is calibrated to native stack safety, not chosen as a language-level
recursion limit: each call activation unconditionally allocates several
kilobytes of fixed-size C locals regardless of the called function's actual
complexity, and the ceiling must stay safely below the point where the real
stack would overflow first, across every build configuration this project
tests (debug, release, and under AddressSanitizer's redzone-inflated frames,
the tightest of the three). A ceiling calibrated only against an optimized
release build previously allowed recursion deep enough to segfault the
process outright in debug and sanitizer builds, well before the counter ever
got a chance to trip.

## Compilation

Before lexing, the source loader expands line-form `require "path"`
dependencies at their declaration sites. Paths are canonicalized, `.di` is
inferred, and nested paths are resolved relative to the requiring file. Active
and completed file sets provide cycle detection and load-once semantics. Source
segments map offsets in the expanded program back to dependency paths and
original lines, preserving useful diagnostics while the compiler retains one
ordered top-level namespace and bytecode module.

The lexer records byte offsets, lines, and columns. A Pratt parser compiles
expressions directly to register bytecode; there is no retained AST. Locals and
temporary values currently receive monotonically increasing registers within a
function, capped at 256.

Each opcode carries the line and column of the source token that produced it.
The disassembler displays these coordinates, and the VM uses them while
unwinding failed calls to build Diamond-level stack traces without exposing C
implementation frames.

`raise value` emits a dedicated exception opcode. The VM roots the raised
Diamond value and propagates a distinct exception status through functions,
methods, and closures, appending the same source-mapped frames used by runtime
errors until a handler catches the value.

`begin`/`rescue` compiles to a frame-local handler stack. A handler records a
bytecode target and destination register for the raised value. Normal execution
pops and skips the handler; an exception crossing any number of calls resumes at
the nearest target, clears the provisional uncaught trace, and evaluates the
rescue body. Raising again inside a rescue naturally targets the next enclosing
handler.

Rescue bindings may include up to eight pipe-separated primitive or nominal
types. During unwinding, a handler matches any member; mismatched handlers are
discarded and lookup continues outward. Nominal filters accept subclasses;
unannotated handlers match every raised Diamond value.

Every program includes `Exception`, `StandardError`, `RuntimeError`, `TypeError`,
`ArgumentError`, `IndexError`, `ZeroDivisionError`, `RangeError`, and
`SystemStackError`. Rescuable VM failures are materialized as instances of these
classes, so nominal matching and the ordinary exception unwind path handle both
runtime failures and explicitly raised values.

An `ensure` clause installs a second kind of frame-local unwind handler. Normal
completion, exceptions, and explicit returns enter the cleanup body with their
control state held in a GC-traced pending-unwind record. `END_ENSURE` resumes that
state through any enclosing cleanup or rescue. A `return` or exception from the
cleanup body supersedes the pending state, matching Ruby's cleanup semantics.

The compiler tracks exact types for locally obvious temporary values. It removes
provably redundant type guards, rejects provable mismatches, and leaves runtime
guards at dynamic boundaries. Mutable and uncertain flows are treated
conservatively.

## Core prelude

The C23 executable embeds `lib/core.di` with `#embed` and prefixes it to each
file or `-e` program before compilation. An internal line reset keeps user
diagnostics and stack traces at their original coordinates. Core helpers are
therefore ordinary Diamond functions using the same bytecode, typing, exception,
and GC paths as application code; C only loads and composes the source.

Dynamic invocation recognizes one native collection primitive: zero-argument
`length()` on arrays, hashes, and strings (string length counts stored bytes).
Bounds decisions, fallback behavior, and empty predicates are implemented in
the Diamond prelude using that primitive.

Strings also natively support `index_of(needle)` (first match position as
an `Int`, or `nil` if not found), `slice(start, length)` (a substring,
bounds-checking `start` but clamping `length` to what's available), and
`to_i()` (lenient leading-digit decimal parsing, using checked arithmetic
and raising `RangeError` on overflow). These were added as prerequisites
for `lib/http.di` (see `docs/http.md`) but are general-purpose.

Arrays use a growable separately allocated value buffer whose capacity is part
of GC accounting. Native `push` grows that buffer and enforces every persistent
element contract; `pop` returns `nil` when empty. Membership, callback iteration,
and mapping are Diamond prelude loops built from these primitives.

Hashes expose insertion-ordered `key_at(index)` and `value_at(index)` primitives;
invalid positions raise `IndexError`. The prelude builds key/value extraction,
membership, callback traversal, and value mapping on them. `hash_each` snapshots
the initial length, so callback insertions are not visited during that traversal.

### Enumerable

Arrays and hashes are native VM object kinds, not classes — they have no
method table and cannot `include` a module. So `values.each(cb)`,
`values.select(cb)`, and friends work through a different mechanism than
ordinary method dispatch: `DIAMOND_OP_INVOKE`'s existing native dispatch for
array/hash receivers recognizes a small fixed set of method names
(`each`/`select`/`count`/`any?`/`all?`/`reduce`/`map`) and, on a match, resolves
an ordinary *top-level* prelude function by name at runtime
(`find_top_level_function` in `src/vm.c`, applying the same two filters the
compiler's own `find_function` uses — excluding class/module methods and
nested `def`s, so a same-named local closure can never shadow the real
prelude function) and calls it with the receiver prepended as the first
argument. `each` forwards to the pre-existing `array_each`/`hash_each`;
every other name forwards to one shared `enumerable_*` function regardless
of array vs. hash, since that function only needs `values.each(...)` to work
generically once `each` itself dispatches correctly per receiver kind.

The same `enumerable_*` functions back a `module Enumerable` with one-line
delegating methods (`def select(callback) = enumerable_select(self, callback)`,
etc.) — any user-defined class that implements its own `each(callback)` and
`include`s `Enumerable` gets `select`/`count`/`any?`/`all?`/`reduce`/`map` for
free through the *ordinary* method-dispatch path, with zero duplicated logic
between the two routes.

`array_each` requires a 1-arity callback; `hash_each` requires a 2-arity
callback (key, value). A single `enumerable_select`-shaped function needs one
fixed-arity glue closure to pass to `values.each(...)`, so each branches once
on `values is Hash`, using a 2-arity glue closure that discards the key and
forwards only the value in the `Hash` branch. This means Enumerable operates
over **values only** for a hash receiver, discarding keys — consistent with
the pre-existing `hash_map_values`/`hash_values`/`hash_include_key`
convention, and a deliberate divergence from Ruby (where `Hash#select` yields
`[key, value]` pairs and returns a `Hash`), consistent with this project's
stance that Ruby compatibility is not a goal.

Each glue closure is declared directly inside the `if`/`else` arm that uses
it — the natural way to write it, and now safe: building this feature
surfaced (and this project then fixed) a real compiler bug where a nested
`def` inside one branch, when a *sibling* branch also declares its own
nested `def` capturing the same outer local, could capture a stale
unboxed value instead of the shared `Cell` (confirmed via disassembly — a
`CLOSURE` instruction capturing a raw register that the dominating branch's
own boxing never actually ran on, on the path that skipped it). Root
cause: capture boxing (`DIAMOND_OP_BOX_LOCAL`) was only ever emitted once
per local, at the first capture site encountered during compilation, on
the assumption that a single compile-time "already boxed" flag reliably
predicts runtime state — which breaks when that first site doesn't
dominate a later one. Fixed by making `BOX_LOCAL` idempotent (a no-op if
the register already holds a `Cell`) and always emitting it at every
capture site rather than skipping already-flagged locals, so boxing is
correct regardless of which branch actually ran.

## Object model

Classes are immutable module metadata rather than heap objects, with one
narrow, explicit exception: `ClassName.redefine_method(name, callable)` (see
below) repoints an existing method's compiled body in place at runtime.
Instances point to their class and contain a fixed field array.
Instance-variable names are assigned stable class-owned offsets during
compilation; subclasses copy their parent's field-slot prefix.

Methods are bytecode functions with `self` in register zero. Dynamic method
lookup walks the receiver's class and superclass chain. `super(arguments)` is
anchored to the class that lexically defined the calling method.

Direct calls to statically known functions have their arity checked at
compile time. Dynamic dispatch cannot: the compiler does not know which
method a receiver's class will resolve to until the call actually runs, so
every dynamic invoke path (`INVOKE`, the rewritten `INVOKE_MONO` fast path,
and constructor dispatch) validates the resolved method's declared arity at
runtime and raises a rescuable `ArgumentError` on mismatch. This check is
per-dispatch, not cached alongside the monomorphic or polymorphic call-site
caches, so a call site that has warmed on one receiver class still correctly
validates arity against whichever class actually shows up next.

Each class owns a pointer-stable chain of shapes representing materialized field
prefixes. Fresh instances begin at shape zero; writing a field advances to the
shape that includes its slot, while an unmaterialized read produces `nil`.
Method dispatch is independent of field state, so each dynamic invoke bytecode
site uses a VM-owned four-entry polymorphic cache guarded by class pointer. A hit
bypasses lookup; a miss performs normal lookup and fills or replaces an entry.
Field bytecode sites use parallel four-entry polymorphic caches guarded by shape
pointer. Cached reads remember whether their slot is materialized; cached writes
remember the resulting shape, avoiding repeated shape-chain decisions.

Nested functions compile to heap-allocated closure objects. A closure identifies
its bytecode function and traces captured values through the garbage collector;
dynamic closure calls use a separate opcode from statically resolved top-level
calls. Locals are boxed only when captured; sibling closures share the resulting
GC-traced mutable cell, while ordinary locals remain direct registers. Nested
closures forward cells through intermediate environments, preserving identity
and mutation across arbitrary lexical depth.

See [object-model.md](object-model.md) for layouts and current limitations.

## Memory management

Each VM owns a linked list of heap objects. Collection is stop-the-world
mark/sweep. Active register frames are explicit roots; instances trace fields,
arrays trace elements, and hashes trace keys and values. Strings are leaf
objects. `DIAMOND_STRESS_GC=1` collects before every eligible allocation to test
rooting paths.

The collector is non-generational and non-moving, so mutations do not require a
write barrier. Object finalizers and weak references do not exist.

## Gradual types

Annotations describe acceptable runtime values and do not alter value layout.
Supported types are `Int`, `String`, `Bool`, `Nil`, `Array`, `Hash`, and declared
classes. An annotation is a reusable set of up to eight pipe-separated types;
nominal members accept subclasses. Type-set indexes are bytecode operands, so
arbitrary unions do not consume opcode bits or alter runtime value layout.

`Array[Element]` members recursively reference another type set. Crossing a
typed boundary validates every existing element and attaches the element
contract to the array object. Indexed mutations subsequently check every
attached contract, preserving the guarantee through aliases; nested array
checks attach nested contracts as well. Plain `Array` remains fully dynamic.

`Hash[Key, Value]` applies the same boundary-and-persistent-contract design to
both halves of every entry. Existing entries are checked before attachment, and
indexed insertion or replacement must satisfy every contract already attached
to the hash. Generic arrays and hashes may nest recursively. Plain `Hash` remains
dynamic, and a missing-key read still returns `nil`.

Functions and methods may declare scoped type variables after their names, as
in `def pair[K, V](key: K, value: V) -> Hash[K, V]`. Each compiled function
retains up to eight variable names, and type members reserve stable variable IDs
that survive recursive Array, Hash, and Callable annotations. The initial layer
initial metadata layer used runtime erasure. Calls now infer primitive, nominal,
and union bindings by walking annotated arguments and declared callback returns.
The execution chunk exposes those bindings to parameter and return guards.
When a checked Array or Hash escapes, its constraint copies the binding IDs and
counts instead of retaining frame-local pointers, preserving generic mutation
safety. Bindings use bounded graphs of union nodes with Array/Hash child links,
so variables may represent recursively parameterized structures. Graphs inferred
from declared callback returns retain structure even when the mapped input is
empty; an empty value with no other evidence naturally leaves its child node
unbound.

Annotations are optional. Parameters are checked on function entry and return
values on every implicit or explicit exit unless the compiler proves the guard
redundant.

After its optional return annotation, a definition may use `= expression`
instead of a newline-delimited body and closing `end`. The compiler feeds this
expression through the same register allocation, capture discovery, return
guard, method installation, and interface-signature metadata paths as a normal
definition.

Functions retain a minimum and maximum arity. Trailing default expressions are
compiled into the start of the callee and guarded by `ARGUMENT_PROVIDED`, which
tests the original argument count rather than the register's value. Parameter
registers are reserved before any fallback bytecode is emitted, preserving
later supplied arguments. Defaults run left-to-right before parameter type
guards, and explicit `nil` never selects a fallback.

The lexer retains an interpolated double-quoted string as one source token while
skipping balanced embedded braces and quoted strings. The compiler temporarily
lexes each `#{expression}` range with the ordinary Pratt parser, emits
`TO_STRING`, and folds literal and evaluated segments through normal string
addition. `\#{` is decoded as a literal interpolation marker. Scalar values use
built-in conversion, while instances and collections follow the object
stringification protocol below.

`TO_STRING` dispatches a zero-argument `to_s` method on instances when one is
available and requires its result to be `String`; method lookup is inherited,
and exceptions propagate through the interpolation site. The default instance
form remains `#<Class>`. Arrays and hashes use a growable native formatter that
recurses through nested collections and tracks active object identities to emit
finite `[...]` or `{...}` markers for cycles.

`Sized` is Diamond's first structural interface. Its contract is a zero-arity
`length` method. `String`, `Array`, and `Hash` satisfy it natively; user classes
satisfy it by defining or inheriting a method with that name and arity. The same
structural test drives annotation checks, subtype reasoning, union narrowing,
and the non-throwing `is Sized` predicate.

User declarations generalize that model with `interface Name ... end`. An
interface body contains bodyless `def` signatures. Conformance is implicit and
compares every required method name and arity against native methods or the
receiver class's inherited method table. Annotated parameters are checked
contravariantly and annotated returns covariantly, including unions, nested
collection types, nominal subclasses, and callable result contracts. An
unannotated implementation parameter accepts every interface input; an
unannotated implementation return cannot satisfy a typed result requirement.

Closure objects satisfy `Callable`. An optional integer argument, as in
`Callable[2]`, checks the referenced bytecode function's arity at the typed
boundary. Core collection callbacks use these contracts, so invalid callbacks
are rejected even when an empty collection would perform no invocation.
`Callable[2, String | Nil]` additionally requires the closure to declare a
structurally compatible return type-set. Nominal returns are covariant, so a
declared subclass return satisfies a superclass result contract. Unannotated
closures do not satisfy a requested result contract.

Full callback signatures use `Callable[[Input, ...], Return]`, with `[]` for
zero inputs. Parameter contracts are contravariant: a closure accepting a
wider input set can satisfy a narrower callback requirement. Return contracts
remain covariant. Untyped closure parameters accept any contracted input, and
generic variables appearing in callback inputs or returns contribute to call
inference.

Registers may also carry a type-set fact in addition to an exact primitive or
class fact. Indexing `Array[T]` produces `T`; indexing `Hash[K, V]` produces
`V | Nil`. These facts survive ordinary local assignment and participate in
recursive union/subtype checks, eliminating redundant return guards while
rejecting a hash lookup used where a non-nil value is required.

Runtime Array and Hash contracts are also inference sources. When a collection
has no elements or entries to inspect, a later generic call imports the
contract's element, key, and value graphs—including substitutions captured by
an earlier generic frame. This preserves nested types across chains of generic
calls without requiring a sentinel value in an otherwise empty collection.

Generic functions and methods may be specialized explicitly with syntax such
as `empty[Int]()` or `factory.empty[String]()`. The call instruction carries
the caller's type-set graphs into the callee, resolving outer generic variables
when necessary. Explicit bindings disable inference for that call and therefore
act as authoritative parameter and return contracts.

Modules are reusable method sets rather than classes. `include Name` copies a
module's method descriptors into the receiving class while retaining ordinary
bytecode functions, typed signatures, generics, and the receiver slot. Reverse
declaration lookup gives direct class methods precedence over included methods
and later includes precedence over earlier ones; superclass lookup begins only
after the receiving class is exhausted. Modules have no instances,
superclasses, fields, or nominal type identity. A module may include an earlier
module; its flattened descriptors remain marked as imported so methods declared
directly by the composing module override them. Source-order resolution and the
absence of reopening prevent indirect cycles, while self-inclusion is diagnosed.

Module metadata also establishes a lexical constant namespace. Nested modules
and classes store their fully qualified `Outer::Name`, while source inside the
module may resolve a sibling by its local name. The lexer distinguishes `::`
from hash/rescue/type colons, and qualified class constants participate in
construction, nominal annotations, diagnostics, and object rendering without a
runtime namespace object.

Stateful module methods use symbolic instance-variable bytecode because a fixed
offset from one class cannot safely apply to another. Inclusion merges required
names into each class's ordered field table; runtime resolution against the
actual receiver produces an offset consumed by the existing shape and field
cache machinery. This keeps instances compact and makes field-name collisions
explicitly shared state rather than layout corruption.

Interfaces share the same lexical namespace and may be referenced through a
qualified type name. Namespace constant declarations evaluate ordinary Diamond
expressions during entry execution and store the resulting value in a VM root
table. `GET_NAMESPACE_CONSTANT` makes those bindings available from methods and
other function chunks without capturing the entry frame. Each binding is
write-once; object values remain mutable according to their own APIs.

`def self.name` inside a module declares a namespace singleton function rather
than an includable method. Its bytecode function has no receiver slot and is
called directly through the module's singleton descriptor table, retaining the
normal arity, defaults, type metadata, generic inference, and explicit generic
call encoding. Singleton descriptors are deliberately excluded from module
inclusion.

`module_function name` installs a second descriptor for an existing module
method and marks the instance descriptor private. Qualified calls insert a
hidden `nil` receiver solely to preserve the original function's register and
parameter layout; exported stateless code and namespace constants therefore
need no cloned bytecode or module heap object. Instance-field access still
requires a real mixed-in receiver and fails normally from the exported form.
The argument-less directive enables the same transformation as definitions are
registered for the remainder of that module body; the compiler saves and
restores the mode across nested module declarations.

Classes maintain a parallel singleton descriptor table. `Class.name()` resolves
that table from the named class through its superclass chain, while `Class.new`
continues to use constructor allocation and `initialize`. Singleton overrides
do not alter instance dispatch, and their functions have no receiver slot.

Method descriptors carry private visibility through inheritance and module
inclusion. Runtime invocation accepts a private descriptor only when executing a
method chunk and invoking register zero (`self`); calls through ordinary
external receiver registers fail through the normal rescuable type-error path.

Attribute declarations synthesize minimal six-byte getter/setter functions and
ordinary method descriptors. Class attributes embed numeric offsets; module
attributes embed symbolic names. Consequently generated accessors require no
special runtime dispatch and inherit visibility, mixing, caching, and shape
semantics from the existing method and field machinery.

Typed declarations such as `attr_accessor value: Int` copy the enclosing type
graph into each generated function. A writer checks its argument before the
field store; a reader checks the loaded value as its return contract.
`attr_predicate` selects a `?` descriptor name while retaining the unsuffixed
field name, so it uses exactly the same field bytecode and optional contract.

A definition treats `=` as part of its method name only when immediately
followed by `(`, preserving endless `def name = expression`. Invocation appends
the same suffix before method-name interning, so user-defined writers traverse
ordinary visibility, lookup, typing, and module-field paths.

The lexer retains a terminal `?` or `!` in identifiers, making predicate and
bang names ordinary functions and methods throughout definition, dispatch,
typing, and aliasing.
Descriptor-oriented directives consume the same identifier tokens, so suffixed
names require no parallel visibility, aliasing, or module-export machinery.

`alias_method` copies an existing local method descriptor under a new name. The
alias points at the same function index and therefore retains its executable
body, arity range, visibility, receiver layout, and type metadata without
duplicating code or creating a forwarding frame.
Writer suffixes are resolved and copied as part of both names.
Alias declarations accept either bare comma-separated names or one
parenthesized pair.

`ClassName.redefine_method(name, callable)` is `alias_method`'s runtime,
value-taking counterpart: it repoints an *existing* method slot to a
different already-compiled function, rather than resolving names at compile
time. `name` is any expression evaluating to a `String`; `callable` is any
expression evaluating to a `Callable` value, which in practice means a
nested, named function referenced by its bare name (Diamond has no anonymous
closure literal). Because classes are not first-class runtime values,
`redefine_method` is recognized contextually at the same call site that
already special-cases `ClassName.new(...)`, and compiles to a dedicated
opcode carrying the target class as a compile-time operand rather than an
ordinary dispatched call. Four structural checks reject unsafe replacements
before the method table is touched: the name must match an existing method
on that class directly (no superclass walk, and no defining a new method —
`DiamondClass.methods` is a fixed-size, compile-time-populated array); the
callable must capture no variables (a method slot only stores a raw function
index, so captured state would be silently discarded and then read as
invalid bytecode the first time the method dispatched); the callable's
function must have been compiled with `owner_class` equal to the target
class (otherwise `self`-register and `@field` offset assumptions baked in at
compile time would be wrong for the new receiver); and the callable's real
arity must exactly match the existing method's declared arity (avoiding a
second metadata mutation axis). A successful call returns `nil` and calls
the same cache-invalidation path `tests/api_invalidation.c` exercises
directly, so already-warmed monomorphic dispatch sites correctly reflect the
change on their very next call.

For a direct `value == nil` or `value != nil` condition, the compiler splits a
union type-set into nil and non-nil branch facts. Facts for locals that existed
before the branch are merged at the join; disagreement becomes unknown, so
branch-local assignment cannot leak an invalid narrowing proof.

`unless` shares the `if` expression compiler with inverted branch selection;
its type narrowing facts are correspondingly exchanged between the body and
optional `else` branch.

An `elsif` branch compiles as the false-side value of its preceding `if`,
sharing the final `end` while preserving expression results and type joins.
Conditional bodies accept either a newline or the `then` delimiter; after the
delimiter the existing sequence compiler handles inline or multiline bodies.
Loop bodies similarly accept a newline or `do`, allowing compact loops without
altering their condition, redo, next, or value-bearing break targets.
The `not` keyword shares prefix precedence and the `NOT` bytecode instruction
with `!`, producing a strict Boolean from Diamond truthiness.
`and` and `or` share precedence and short-circuit jump generation with `&&` and
`||`; like those operators, they return one of their operand values.

`until` likewise shares loop compilation with `while`, negating only the
condition register before the existing exit jump; `break` and `next` targets
therefore retain identical semantics.
Each loop also records the first body instruction separately from its condition;
`redo` targets that offset, whereas `next` targets condition reevaluation.
Loop expressions initialize a result register to `nil`; `break value` moves its
operand into that register before jumping to the shared exit, while bare break
retains the initialized value.

A `begin` expression may place `else` after its rescue clause. Normal execution
jumps over the handler into that branch, while rescued execution jumps past it;
both paths subsequently pass through `ensure`.

While compiling a rescue body, the compiler records its exception register;
bare `raise` emits the ordinary raise opcode against that register. The context
is scoped across nested rescue blocks and cleared for nested function bodies.

`retry` jumps to the rescue-handler installation immediately before the
protected body. The surrounding ensure frame remains installed, so repeated
attempts do not duplicate cleanup and final completion runs it exactly once.

The colon introducing rescue filters is independent of the optional local
binding. Both `rescue error: TypeError` and `rescue : TypeError` therefore emit
the same handler type table; only the former adds a lexical local.

Multiple rescue clauses share one catch-all VM handler and compile ordered
`IS_TYPE` dispatch at its target. An exception unmatched by every typed clause
is re-raised; a catch-all must therefore be last.

`value is Type` emits a non-throwing runtime predicate and has comparison
precedence. For a direct conditional test, union members accepted by `Type`
(including nominal subclasses) flow into the true branch and the complement
flows into the false branch. Compound boolean conditions remain conservative.

## Deliberate constraints

- No Ruby compatibility guarantee.
- No singleton methods or visibility controls.
- No native-code generator or JIT.
- No stable bytecode, embedding API, or package format.
- No parallel execution.
- No portability target beyond the current development machine.
