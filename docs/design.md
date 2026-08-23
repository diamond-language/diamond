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
booleans, signed 64-bit integers, and IEEE-754 double-precision floats.
Strings, arrays, hashes, instances, and arbitrary-precision integers are
managed heap objects with a common header. NaN boxing is deferred until
measurement shows that representation density is worth the complexity.

`Float` arithmetic and comparisons live entirely on the generic (non-`_INT`)
opcode paths — there is no quickened or compiler-specialized `_FLOAT` opcode
tier. Baseline benchmarking (see `bench/BASELINE.md` on the JIT-experiment
branch) found the existing `_INT` specialization/quickening tier gives no
measurable speedup even in code built specifically to exercise it, since
call/frame overhead dominates; building an analogous `_FLOAT` tier would have
meant replicating real complexity (several `_INT` opcodes have no
deopt-back-to-generic path) for a tier the project's own data says would not
pay for itself. The compiler's static `_INT` specialization already only
fires when both operands are provably `Int`, so anything else — a `Float`
literal, a `Float`-typed parameter, or fully dynamic code — already falls
through to the generic opcode with no changes needed; static-typed and
dynamic `Float` code are therefore identical at the bytecode level.
Mixed `Int`/`Float` arithmetic and comparisons auto-promote the `Int`
operand to `double`, matching Ruby/Python/JS rather than requiring explicit
conversion.

Integer arithmetic uses C23 checked arithmetic to detect overflow rather than
invoking C undefined behavior. Addition, subtraction, multiplication, and
negation all detect overflow through `ckd_add`/`ckd_sub`/`ckd_mul`; division
additionally special-cases `INT64_MIN / -1`, the classic two's-complement
overflow that checked-arithmetic division alone would not catch, since the
mathematically correct magnitude has no representable positive counterpart.
Rather than raising, every overflow auto-promotes to an arbitrary-precision
`DiamondBignum` (`src/bignum.c`), matching Ruby/Python/Lisp rather than
Java's separate `BigInteger` type — `Int` has no user-visible size limit,
only a representation that changes transparently once a value stops fitting
in 64 bits. A bignum stores sign and magnitude as base-10⁹ limbs (chosen so
decimal stringification — exercised on every `puts`/interpolation/error
message — is close to trivial, at a cost to raw arithmetic throughput that
doesn't matter for a cold path); every operation that produces one checks
whether the true result still fits `int64_t` and demotes back to a plain
inline `Int` if so, so a bignum object only ever exists to represent a value
that genuinely needs it. `Int` literals in source stay capped at 64-bit —
only runtime arithmetic overflow promotes. Bignum-producing arithmetic takes
operand "views" (`DiamondIntView`, either a small `int64_t` widened onto a
stack-local array or an existing bignum's limbs referenced directly) rather
than heap-allocated operands, so combining two values never allocates more
than once (the final result, if any) — allocating twice in a row to widen
both operands first would leave the first allocation unrooted from any GC
scan between the two calls, letting a collection triggered by the second
allocation sweep it away while it was still needed.

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
for an HTTP library (originally bundled as `lib/http.di`, later pulled
out into `packages/http` — see `docs/packages.md`) but are
general-purpose.

Arrays use a growable separately allocated value buffer whose capacity is part
of GC accounting. Native `push` grows that buffer and enforces every persistent
element contract; `pop` returns `nil` when empty. Membership, callback iteration,
and mapping are Diamond prelude loops built from these primitives.

Hashes expose insertion-ordered `key_at(index)` and `value_at(index)` primitives;
invalid positions raise `IndexError`. The prelude builds key/value extraction,
membership, callback traversal, and value mapping on them. `hash_each` snapshots
the initial length, so callback insertions are not visited during that traversal.

Internally, a `Hash` keeps its existing insertion-ordered entry array (what
`key_at`/`value_at` read directly) plus a separate open-addressing bucket
table mapping each key's hash to an index into that array, giving average
O(1) lookup/insert rather than scanning every entry. The bucket table is
rebuilt (not the entry array, which is never reordered) when its load
factor crosses 0.75; entry hashes are cached at insertion to avoid
recomputing a String key's hash on every rebuild. Key hashing matches
value equality exactly: `Int`/`Bool`/`Nil` by value, `String` by content
(FNV-1a), `Array`/`Hash`/instances by identity (pointer, via a MurmurHash3
finalizer for good bit distribution).

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

`sort`/`sort_by`/`min`/`max`/`min_by`/`max_by` and the rest of Array's
Enumerable-style surface (`reject`/`find`/`each_with_index`/`sum`/`take`/
`drop`/`flat_map`/`partition`/`group_by`/`zip`/`each_slice`/`each_cons`/
`tally`) are implemented once, over `Array` specifically, using indexed
access rather than `each()` — `enumerable_sort(values: Array)`, not
`values.each(...)`-driven like `enumerable_select` above. `module Enumerable`
still exposes all of them, but not by rewriting each one to be generic:
it adds a `to_a()` method (itself built on `self.each(...)`, the same way
`select`/`map`/etc. above are) that materializes the receiver into a real
`Array`, then delegates each of these methods to the matching existing
`array_*`/`enumerable_*` function on that materialized copy. This keeps
those already-tested Array-indexed implementations as the single source of
truth — an `include Enumerable` class (`Range`, or any other) pays one
`to_a()` copy for this group rather than duplicating index-based logic
generically, the same trade `vm.c`'s own Array-only fast path for
`sort`/`min`/etc. already makes (see "None of these are defined on `Hash`"
in `docs/syntax.md`'s Collections section) by not generalizing over `Hash`
either.

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

Classes are immutable module metadata rather than heap objects, with three
narrow, explicit exceptions (see below): `ClassName.redefine_method(name,
callable)` repoints an existing method's compiled body in place at runtime,
`ClassName.define_method(name, callable)` adds a new one, and `self` inside
a class-owned `def self.x` method evaluates to a lightweight
`DIAMOND_VALUE_CLASS` value (just a class index, not a heap object) so that
kind of method can dispatch virtually.
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
Supported types are `Int`, `Float`, `String`, `Bool`, `Nil`, `Array`, `Hash`, and
declared classes. An annotation is a reusable set of up to eight pipe-separated types;
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

`delegate name(params), to: @ivar` compiles a genuinely new forwarding
method rather than aliasing an existing one -- there is no existing
method to copy, since the point is calling out to a *different* object's
method of the same name. Unlike `attr_reader`/`attr_writer`, whose body is
always exactly one opcode (a field load/store) hand-written directly into
the generated `DiamondFunction`'s bytecode, `delegate`'s body is a real
dynamic-dispatch call with arbitrary declared arity, so it's compiled
through the same register-allocating/instruction-emitting machinery an
ordinary `def` uses -- `compile_delegate` temporarily redirects the
compiler at a fresh function exactly the way `compile_definition` does for
every other method, minus the closure-capture bookkeeping a class/module
member never needs. The target must be a bare instance variable (auto-
declared on first reference, the same as any other `@ivar`); parameters
are bare names with no type annotations, defaults, or splat/block
forwarding; the delegated call always reuses the declaring name (no
renaming). Because the result is an ordinary method registered in the
usual method table, it participates in inheritance, `super`,
`respond_to?`, and `redefine_method` exactly like a hand-written one --
deliberately, since a parallel dispatch mechanism was the one thing this
feature was scoped to avoid. Native-compiler only for now (not mirrored in
the self-hosted parser, which is in maintenance mode); see
`docs/roadmap.md`'s own entry for the reasoning.

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
on that class directly (no superclass walk); the callable must capture no
variables (a method slot only stores a raw function index, so captured state
would be silently discarded and then read as invalid bytecode the first time
the method dispatched); the callable's function must have been compiled with
`owner_class` equal to the target class (otherwise `self`-register and
`@field` offset assumptions baked in at compile time would be wrong for the
new receiver); and the callable's real arity must exactly match the existing
method's declared arity (avoiding a second metadata mutation axis). A
successful call returns `nil` and calls the same cache-invalidation path
`tests/api_invalidation.c` exercises directly, so already-warmed monomorphic
dispatch sites correctly reflect the change on their very next call.

`ClassName.define_method(name, callable)` is the same mechanism's
add-a-new-slot counterpart, recognized at the same call site and compiled to
its own dedicated opcode. `DiamondClass.methods` is a fixed-size array
(`DIAMOND_MAX_METHODS`, currently 256) that a class's compile-time method
count rarely fills, so "adding" a method at runtime is just writing a new
`DiamondMethod` entry into the next unused slot and incrementing
`method_count` — no reallocation, no new heap object, nothing for the GC to
track. It shares three of `redefine_method`'s four checks (String name,
capture-free callable, `owner_class` equal to the target class) but rejects
a name that *already* exists instead of requiring one, and drops the arity
check entirely — a brand-new method has no prior arity to match, so it just
takes the callable's own, the same as a `def` compiled directly into the
class would. Deliberately kept as a separate operation from
`redefine_method` rather than one function that branches on whether the name
exists, so each keeps a single, predictable contract. The new method
dispatches correctly for instances constructed before the call too, since
lookup is by class and name at call time, never snapshotted per instance.

### Runtime method synthesis

`ClassName.compile_method(name, params, body_source, bound_values)`
compiles a *new* method body from a source string at runtime and returns
a `Callable`, meant to be installed with the `define_method` above --
`define_method`'s existing "add a new slot" mechanism already handles
attaching it to a class, since ordinary dispatch was already runtime
name-lookup, not a compile-time-baked slot (`lookup_method`/
`lookup_method_cached`). The actual gap this closes is that
`define_method`'s callable previously always had to be an
already-compiled nested `def`, physically written in the source --
`compile_method` is the missing piece letting `has_many`-style
associative helpers (`packages/active_record`) synthesize a method body
from a runtime string instead, closer to Ruby's own dynamic
metaprogramming without a general `eval`.

```ruby
class Author < ActiveRecord::Model
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
    callable = Author.compile_method("books", ["db"],
      "self.has_many(target_repo, \"author_id\").all(db, self.id())",
      {"target_repo": Book.repository()})
    Author.define_method("books", callable)
  end
end
```

**Why this can't just append to the running program's own function
table.** `chunk->functions` is a fixed array, allocated once when a
`DiamondProgram` finishes compiling and never grown afterward
(`diamond_program_add_function` only ever runs *during* construction);
appending new bytecode to an already-running chunk isn't safe. Instead,
`compile_method` synthesizes a tiny, self-contained source string:

```
class <ClassName>
  attr_accessor <field1>, <field2>, ...
  def <name>(<params>, <bound_value_names>)
    <body_source>
  end
end
```

and compiles it, through the ordinary unmodified `diamond_compile()`,
into a **separate, throwaway `DiamondProgram`** -- kept alive forever via
the same `vm->adopted_programs` list `ProgramBuilder`'s own adopt mode
already uses (`adopt_program`), rather than any new lifetime mechanism.
The `attr_accessor` line re-declares the target class's *existing* field
names, in the same order, using the compiler's completely unmodified
`@ivar` auto-declare-on-first-reference machinery -- this is what seeds
an identically-shaped field table in the throwaway program without a new
compiler entry point, and `compile_method` then requires the resulting
class's `field_count` to match the original *exactly*: if `body_source`
referenced or auto-declared any field beyond that seeded list, the call
fails with `ArgumentError` rather than installing a method whose
`@field` offsets don't mean what the real class's instances expect.
Growing a class's *own* field count at runtime, by contrast, is never
attempted -- `allocate_instance` sizes an instance's `fields[]` from
`class->field_count` once, at allocation time, so already-existing
instances would be left with a too-small allocation.

**Why `body_source` can't name another class directly.** `class <Name>`
above declares a brand-new, unrelated class inside the throwaway
program -- Diamond's cross-program isolation (the same fact that makes
two independently-compiled `ProgramBuilder` programs safe to run
side-by-side, disambiguated by `DiamondInstance.owner`) means this can
never collide with a same-named class in the real, currently-running
program, but it also means a bare reference to some *other* class (like
`Book` in the example above) has nothing to resolve against: class
references compile to indices into whichever program compiled them, and
the throwaway program was never told `Book` exists. `bound_values` is
the answer -- a `Hash` of already-evaluated values (typically the result
of calling a method on a class `body_source` has no way to name), copied
onto the returned `DiamondClosure` and, once installed via
`define_method`, onto the class's own `DiamondMethod` entry
(`bound_values`/`bound_value_count`; capped at `DIAMOND_MAX_BOUND_VALUES`,
8). Every dispatch site appends them after the caller's own explicit
arguments before entering the function -- a caller of the installed
method never supplies them, so `arity`/`required_arity` on that
`DiamondMethod` already exclude them. This is the one place this feature
needed real, ongoing GC cooperation: `vm->adopted_programs`' own
`bound_values` arrays are walked and marked directly in
`diamond_vm_collect`, since `DiamondMethod`/`DiamondClass` entries are
otherwise pure program metadata that never reference GC heap values at
all.

**Why a method installed this way can safely call `self.foo()` against
the real receiver even though its *own* bytecode's ambient chunk is the
throwaway program.** Every instance method call inside the compiled body
still needs to resolve against whichever chunk the *receiver's actual
class* lives in, not whichever chunk happens to be executing --
previously the same thing, always, for every chunk in a single vm's
call stack, so nothing tracked them separately. `DiamondVm.root_chunk`
(set once, in `diamond_vm_run`, to the chunk the vm was first invoked
with) makes that distinction real: `instance->owner==nullptr` now means
"belongs to `root_chunk`," not "belongs to whatever chunk is ambient
right now." `DiamondMethod.source_chunk` (nullptr for every ordinary,
source-declared method) plays the same role for resolving the
*compiled-method's own* `function_index`, the two together letting a
`compile_method` result's bytecode correctly reference both itself
(its own chunk) and the instance it was called on (`root_chunk`) in the
same call.

**Deliberately out of scope for this first version:** `self.`
class-owned singleton methods (instance methods only -- `self.foo` has
its own separate `DIAMOND_VALUE_CLASS` dispatch, not touched here);
closures over the *calling* scope's own locals (only `bound_values`,
fixed at `compile_method` time, and the target class's existing fields
are visible); bare parameter names only, the same scope cut `delegate`
already uses (no types, defaults, splat, or block forwarding). Trust
model: `body_source` runs as real compiled bytecode with no sandboxing,
the same level Ruby's own `class_eval`/`define_method` already assume --
meant for programmer-authored macro implementations, not untrusted
end-user input.

### `method_missing`

`def method_missing(name, args)` on a class is consulted whenever ordinary
instance-method dispatch (`lookup_method`) finds no method by that name on
the receiver's class -- `name` is the attempted method as a `Symbol`,
`args` an `Array` of the call's own explicit arguments (the receiver
itself is not included, matching every other language's version of this
shape). A real method of that name always wins; dispatch never even
reaches the `method_missing` fallback for a method that exists, on any
ancestor, so it can't intercept calls to things a class actually defines.

```ruby
class Ghost
  def method_missing(name, args)
    "called #{name} with #{args.length()} args"
  end
end

g = Ghost.new()
puts(g.anything())        # called anything with 0 args
```

If a class has no `method_missing` of its own, a dispatch miss raises
`NoMethodError` (a new `StandardError` subclass) exactly as before this
feature existed -- `method_missing` is a fallback, not a replacement for
normal error behavior. If `method_missing` itself doesn't accept exactly
two required parameters, a miss raises `ArgumentError`, same as calling
any other method with the wrong arity.

Implementation-wise this reuses the exact dispatch-miss branch point
`compile_method`'s own `NoMethodError` work already introduced, and the
same `lookup_method`/`source_chunk` machinery documented above --
`method_missing` itself is found via an ordinary `lookup_method` call
against the receiver's class, so a `method_missing` defined on a
superclass is found for a subclass instance the same way any other
inherited method would be.

One correctness pitfall this surfaced, worth recording: the `Symbol` and
`Array` built for the `name`/`args` arguments are fresh allocations with
no register or container to hold them live until they're both folded
into the call's `args[]`. Under `DIAMOND_STRESS_GC=1` (which forces a
collection on *every* allocation), the second allocation could collect
the first, unrooted one out from under itself -- a real, confirmed
heap-use-after-free, not just theoretical. Fixed by `gc_protect`ing the
first value across the second allocation, the same "root it before the
next allocation can run" pattern already used elsewhere in this file
(e.g. `populate_default_argv_env`'s own `key_mark`) -- this is the
standard shape of bug to watch for whenever a dispatch helper needs more
than one *new* heap allocation before they're all reachable from a
single register or container.

**Deliberately scoped to this one dispatch site** (ordinary instance
method calls) for a first version -- not operator overloading
(`invoke_operator_method`), not `#to_s` (`stringify_value`), not `super`,
not `self.`-singleton dispatch. Each of those already has its own
sensible fallback (native operator semantics, a default object
representation, a real "no such superclass method" error, a separate
method table) that silently redirecting through `method_missing` would
be more likely to surprise than help; a class wanting custom behavior
there defines the specific method directly.

A `def self.x` method declared directly inside a class (not a module) also
now gets real virtual dispatch for `self.foo(...)` written in its own body,
via a new lightweight `DIAMOND_VALUE_CLASS` value kind: a 1-byte
`class_index` (into the ambient chunk's `classes[]`) carried directly in
`DiamondValue`'s existing union, alongside `bool`/`int64_t`/etc. -- no struct
growth, and no GC changes at all, since it owns no heap pointer for the
marker to trace (the same reason INT/FLOAT/BOOL/NIL already need none).
Deliberately narrow: a Class value only ever appears in register 0 (`self`)
inside a class-owned singleton method and as the receiver of `self.foo(...)`
dispatch there -- there's no other syntax to construct or pass one, though
nothing stops it flowing into an ordinary `Hash`/`Array`/return value once it
exists, so `values_equal`/`hash_value`/the three value-printing paths do
carry real cases for it (equal-by-index, hash-of-index, `#<Class:N>` --
no chunk/program context reaches those low-level paths to resolve a real
class name, so the index is what prints).

Mechanically, a class-owned singleton method now gets the same
implicit-self treatment an ordinary instance method already has:
`compile_definition` reserves register 0 and bumps `arity`/`required_arity`
by one, and (new) gives the function a real `owner_class` (previously
always `UINT8_MAX`, "not a method," for every singleton method regardless of
class or module). An external call with a literal class name
(`Author.find(db, 1)`) still resolves its *target function* exactly as
before -- walking the literal class's own `singleton_methods` then its
superclass chain, entirely at compile time -- but now also loads that
literal class (`Author`, via the new `DIAMOND_OP_LOAD_CLASS`) into the
callee's register 0, so `self` inside an *inherited* method's body reflects
the actual receiver rather than `owner_class` (whichever ancestor the method
happens to be lexically defined on). `self.foo(...)` inside such a method
is the one call form that can't resolve at compile time -- the same body is
compiled once, but `self` may hold a different class on each invocation --
so it's a new opcode, `DIAMOND_OP_INVOKE_SELF_METHOD`, whose runtime handler
is `lookup_method`'s exact walk-the-superclass-chain algorithm reused
verbatim over `singleton_methods[]` instead of `methods[]`
(`lookup_singleton_method`), finishing with the same runtime-indirect call
shape `DIAMOND_OP_CALL_CLOSURE` already uses (build a `DiamondChunk` view
from the resolved function, `run_chunk` it) rather than a bytecode-fixed
target.

Giving singleton methods a real `owner_class` has one deliberate side
effect: a **bare** call to a sibling singleton method (no explicit `self.`)
used to reach it by accident, since `find_function` (ordinary bare-call
resolution) matches any function with `owner_class==UINT8_MAX` regardless of
where it's declared, and singleton methods previously all had that value
regardless of class or module. Module namespace singletons never benefited
from this (they were already tagged `UINT8_MAX-1`, distinct from a plain
function, so a bare sibling call there was already an error); only
class-owned ones did, incidentally. Now that a class-owned singleton method
needs its class's real index for `self` to mean anything, it's excluded
from `find_function` the same way module ones always were -- closing an
inconsistency rather than introducing one. Nothing in the existing corpus
(including the self-hosted parser/lexer differential suite) relied on the
old accident; `self.foo(...)` is the one spelling that reaches a sibling
method now, uniformly, whether it needs to be virtual or not.

Everything above is scoped deliberately narrowly and stops well short of
making classes first-class runtime values in general: there is still no way
to pass a class as an ordinary argument, store one as an attribute, or name
one dynamically by a computed string -- only `self` inside the one context
above ever produces a Class value.

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

### `closure name() ... end`

A plain nested `def` closes over ordinary outer locals correctly (each
gets `DIAMOND_OP_BOX_LOCAL`'d in the enclosing function and read back via
`DIAMOND_OP_GET_CAPTURE`), but never over `self`/`@ivar` -- those compile
exactly like an ordinary method body, hardcoding register 0 as the
receiver, on the assumption that whatever eventually calls this function
will supply a real receiver there itself. That assumption holds for the
one thing a plain nested `def` is actually for -- a
`define_method`/`redefine_method` patch factory, given a genuine `self`
at *installation* time through ordinary method dispatch -- but is simply
false for a closure invoked immediately, in place, which never goes
through that call convention at all. `closure name() ... end` is a
second, separate nested-function form for exactly that case: it captures
`self` the same way it captures any other outer value, so `self`,
`@ivar`, and `self.foo(...)` all work correctly when called directly.

**Why self can't just be added to the ordinary capture list as-is.**
`self` isn't a named `Local` at all (register 0 is implicit, populated by
the call convention, never registered in `compiler->locals[]`), so
`compile_definition`'s existing by-name capture growth (`parse_identifier`
scanning `enclosing_locals` for a matching name) has nothing to match
against. And boxing register 0 *in place* the way an ordinary captured
local is boxed -- turning it into a `DiamondCell` right there in the
enclosing function -- would be actively wrong: every other `self`/`@ivar`
access in that same enclosing method (before or after the `closure`
statement, all still ordinary unmodified code) hardcodes register 0
unconditionally, with no `.captured`-flag check the way a named local's
own reads already have; boxing it would silently corrupt every one of
those into reading a `Cell` where an `Instance`/class value was expected.
The fix: copy self into a **freshly allocated register in the enclosing
function**, box *that copy*, and add its index to the capture list --
register 0 itself, and everything else in the enclosing method that reads
it, is never touched.

**Why the closure's own body doesn't need new self/ivar codegen at all.**
`GET_IVAR`/`SET_IVAR` already take a generic receiver register operand at
the VM level (confirmed directly, not assumed) -- the literal `0` every
ordinary method's compiled body passes is a compiler choice, not an
opcode constraint. But `DIAMOND_OP_INVOKE_SELF_METHOD` (the opcode
`self.foo(...)` compiles to inside a class-owned singleton method) *is*
hardcoded to register 0 at the VM level, unconditionally, with no
receiver operand at all. Rather than teach every one of these sites about
a per-closure capture index, `compile_definition` instead reserves
register 0 in the **closure's own** function (guaranteed available: its
`next_register` was just reset to 0 and nothing has claimed it yet) and
emits one `DIAMOND_OP_GET_CAPTURE` into it, immediately, before compiling
any of the closure's own body. From that point on, the closure's register
0 legitimately holds self, and every existing `self`/`@ivar`/
`self.foo(...)` code path -- `parse_prefix`'s `DIAMOND_TOKEN_SELF`/
`INSTANCE_VARIABLE` cases, `compile_assignment_store`, and
`DIAMOND_OP_INVOKE_SELF_METHOD`'s own VM handler -- runs completely
unmodified. `self.foo(...)` against a class-owned singleton method's
sibling works the same way: `compiler->in_singleton_method` is set true
for the closure too, whenever it's declared directly inside a `def
self.x` (the same depth-1-only condition `redefine_method`'s own
patch-factory idiom already uses,
`nested_in_singleton_method`), so `self.foo(...)` there resolves through
the ordinary `parse_self_class_method_call`/`INVOKE_SELF_METHOD` path,
reading the correctly materialized `DIAMOND_VALUE_CLASS` back out of
register 0.

One existing mechanism needed a matching fix, not just a new one added
alongside it: `nested_in_singleton_method` closures already got
`function->owner_class` set to mark them as "this is a method," purely so
`redefine_method` installation treats them correctly once patched in --
but that marking also implies an extra implicit-receiver parameter slot
everywhere a function's own arity/`parameter_offset` gets consulted. A
`closure` is never installed via `redefine_method` and gets self entirely
through capture, not a call-time argument, so it must never pick up
`owner_class` even when directly nested inside a singleton method --
confirmed the hard way: leaving this unexcluded produced a real "wrong
number of arguments" at the *call site of the closure itself*, not
anywhere near the actual `self.foo(...)` call inside it, since the
receiver-slot assumption was baked into the closure's own declared arity,
not its body.

No VM/opcode changes, no `DiamondClosure` struct field additions, no GC
changes. Self capture rides in the existing `captures[16]`/`capture_count`
array like any other captured value, which is also what makes two
existing invariants keep protecting a `closure` correctly for free: the
`Thread.new` zero-capture-only check and `copy_value_into_vm`'s cross-heap
gate both already reject any closure with `capture_count!=0` (a
`closure`'s is always `>=1`), and `define_method`/`redefine_method`
already require a capture-free callable -- neither needed to learn
anything new about this feature to keep rejecting it correctly.

**Explicitly not fixed by this feature**: a nested `def`/`closure`
redeclared a second time inside the same loop body raises a runtime
`TypeError` at the second declaration -- a separate, pre-existing bug
this feature's own work surfaced but did not investigate. See
`docs/roadmap.md`'s "Open design decisions" section.

## Deliberate constraints

- No Ruby compatibility guarantee.
- No singleton methods or visibility controls.
- No native-code generator or JIT.
- No stable bytecode, embedding API, or package format.
- `Thread` gives real parallel execution, but each thread runs against a
  fully independent, cloned heap rather than sharing GC/dispatch state with
  the spawning thread — trading per-thread memory for not having to make
  the rest of the interpreter thread-safe. See `docs/threads.md`.
- No portability target beyond the current development machine.
