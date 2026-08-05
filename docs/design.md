# Diamond design

Diamond is a personal research language. These notes describe the current
implementation, followed by decisions that are intentionally still open.

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
invoking C undefined behavior.

## Compilation

Before lexing, the source loader expands line-form `require "path"`
dependencies at their declaration sites. Paths are canonicalized, `.dia` is
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

The C23 executable embeds `lib/core.dia` with `#embed` and prefixes it to each
file or `-e` program before compilation. An internal line reset keeps user
diagnostics and stack traces at their original coordinates. Core helpers are
therefore ordinary Diamond functions using the same bytecode, typing, exception,
and GC paths as application code; C only loads and composes the source.

Dynamic invocation recognizes one native collection primitive: zero-argument
`length()` on arrays, hashes, and strings (string length counts stored bytes).
Bounds decisions, fallback behavior, and empty predicates are implemented in
the Diamond prelude using that primitive.

Arrays use a growable separately allocated value buffer whose capacity is part
of GC accounting. Native `push` grows that buffer and enforces every persistent
element contract; `pop` returns `nil` when empty. Membership, callback iteration,
and mapping are Diamond prelude loops built from these primitives.

Hashes expose insertion-ordered `key_at(index)` and `value_at(index)` primitives;
invalid positions raise `IndexError`. The prelude builds key/value extraction,
membership, callback traversal, and value mapping on them. `hash_each` snapshots
the initial length, so callback insertions are not visited during that traversal.

## Object model

Classes are immutable module metadata rather than heap objects. Instances point
to their class and contain a fixed field array. Instance-variable names are
assigned stable class-owned offsets during compilation; subclasses copy their
parent's field-slot prefix.

Methods are bytecode functions with `self` in register zero. Dynamic method
lookup walks the receiver's class and superclass chain. `super(arguments)` is
anchored to the class that lexically defined the calling method.

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

A definition treats `=` as part of its method name only when immediately
followed by `(`, preserving endless `def name = expression`. Invocation appends
the same suffix before method-name interning, so user-defined writers traverse
ordinary visibility, lookup, typing, and module-field paths.

The lexer retains a terminal `?` or `!` in identifiers, making predicate and
bang names ordinary functions and methods throughout definition, dispatch,
typing, and aliasing.

`alias_method` copies an existing local method descriptor under a new name. The
alias points at the same function index and therefore retains its executable
body, arity range, visibility, receiver layout, and type metadata without
duplicating code or creating a forwarding frame.
Writer suffixes are resolved and copied as part of both names.
Alias declarations accept either bare comma-separated names or one
parenthesized pair.

For a direct `value == nil` or `value != nil` condition, the compiler splits a
union type-set into nil and non-nil branch facts. Facts for locals that existed
before the branch are merged at the join; disagreement becomes unknown, so
branch-local assignment cannot leak an invalid narrowing proof.

`unless` shares the `if` expression compiler with inverted branch selection;
its type narrowing facts are correspondingly exchanged between the body and
optional `else` branch.

`until` likewise shares loop compilation with `while`, negating only the
condition register before the existing exit jump; `break` and `next` targets
therefore retain identical semantics.

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
