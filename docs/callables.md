# Functions, closures, and calls

[Language reference](syntax.md) · Previous: [Core syntax and values](core-syntax.md) · Next: [Classes, modules, and methods](classes-and-modules.md)

```ruby
def make_adder(amount)
  def add(value)
    amount + value
  end
  add
end
add_forty = make_adder(40)
add_forty(2)
```

`def` always requires parentheses, even for a zero-argument function
(`def foo()`). Only *nested* `def`s produce a referenceable closure value —
a top-level `def`'s name is not itself a value. Closures are invoked
directly as `f(x)`; there is no `.call` method.

### Blocks

```ruby
total = 0
[1, 2, 3, 4].each() do |x|
  total = total + x
end

doubled = [1, 2, 3].map() do |x|
  x * 2
end
```

`recv.method(args) do |params| ... end` attaches an anonymous closure as
the call's own last argument — the same mechanism a named nested `def`
uses (same capture behavior, same underlying `CLOSURE` value), just
without a name. It works the same way on a direct call to a top-level
function: `apply(5) do |n| n * 2 end`.

Delimiters are `do ... end` only — there is no `{ ... }` block form, and
none is planned. Every other Diamond control construct (`if`, `while`,
`def`, `class`, `case`) already uses `end`, and `{` already means a Hash
literal, so `arr.each { |x| ... }` would collide with a bare Hash
argument the way it does in Ruby.

Block parameters are bare identifiers only — no `: Type` annotations, no
`= default`. `do |x, y| ... end`, or `do ... end` with no parameters at
all for a zero-arity block. A block captures every local visible at the
point it's written, the same eager, unconditional capture nested `def`s
use — not just the ones its body actually references.

**`self` is not one of those captured locals.** Inside a `do ... end`
block written in an instance method, `self` does not refer to that
method's own receiver — unlike a named nested `closure` (see "Classes
and modules"'s own `closure` section), which does capture `self`
correctly even when passed elsewhere as a Callable. `[1].each() do |x|
self.foo() end` inside an instance method is a bug, not a shorter
`closure`: capture `self` into an ordinary local first (`instance =
self` before the block, `instance.foo()` inside it — a real,
established pattern, not a workaround invented for this) or use a
named `closure` instead of an anonymous block when the body needs
`self`.

A function or method can bind its optional trailing block with `&block`.
Inside that lexical body, `yield(args...)` invokes the bound Callable and
`block_given?()` reports whether a block was supplied. `yield` accepts zero or
more arguments. Calling it without guarding an absent optional block is a
normal non-Callable type error.
The parameter may carry a Callable annotation, such as
`&block: Callable[2]`. Absence remains valid; a supplied block must satisfy the
declared Callable shape.
If the Callable annotation includes a return type, that type becomes the
compile-time type of `yield`. Anonymous blocks with a statically known final
expression also advertise that return type to Callable validation.
At statically resolved function, singleton, instance-method, and constructor
calls, concrete parameter types inside `Callable[[...], Return]` are applied to
corresponding `do |...|` locals. Callable values with typed nested Callable
parameters provide the same context. Explicit generic bindings are substituted
before the block body is compiled. For statically resolved top-level,
singleton, and instance-method calls, omitted bindings are inferred when a
concrete positional argument maps directly to a generic parameter. Array and
Hash literals also provide recursive bindings for nested collection parameters.
Mixed distinguishable members form unions. Differently parameterized members
with the same outer type remain distinct alternatives, such as
`Array[Int] | Array[String]`; exact duplicates are invalid. Fixed constructor
calls infer `initialize` type variables
from their positional arguments in the same way.
Spreading a homogeneously typed Array supplies its element contract to generic
parameters before a trailing block is compiled. This applies to functions,
singleton methods, instance methods, and constructors.
Keyword values likewise infer generic bindings from their named parameter slot
for statically resolved functions, singleton methods, instance methods, and
constructors. Explicit generic arguments take precedence.
For a union receiver, independently overridden methods still provide block
context when their complete parameter contracts are structurally identical.
Any incompatible override keeps the block untyped.

A statically resolved call to a non-generic top-level, singleton, or instance
method retains its declared return type at the call site. This includes nested
type graphs: a result declared as `Array[Array[Int]]` can be indexed twice and
used as an `Int` without losing the element contract. Fixed, spread, keyword,
and trailing-block calls behave consistently. Generic return graphs are
instantiated at compile time when explicit or inferred bindings resolve every
type variable. Positional, keyword, and homogeneous spread arguments contribute
bindings. Unresolved or conflicting bindings leave the result dynamically
typed. Calling a value with a typed Callable return contract and indexing a
typed collection carry their result facts into the following expression too.
Inference recurses through typed Callable parameter and return contracts, even
when the Callable is nested in an Array or Hash. A union-receiver call retains
its generic result only when all candidate implementations declare matching
signatures. Structurally identical returns reuse one graph; divergent declared
returns form a safe union. Indexing a union of typed Arrays joins its element
graphs, while indexing a typed Hash union joins its value graphs with `Nil`.
Native collection relays preserve these facts too. Array `first` and `last`
produce the joined element type; `reverse`, `uniq`, `compact`, `sort`,
`sort_by`, `select`, `reject`, `take`, and `drop` preserve it inside an Array.
Hash `keys` and `values` produce Arrays containing the joined key and value
types respectively.
Fallback reads, `concat`, and `merge` also join typed argument graphs for fixed
and named-keyword calls. Typed callback returns determine `map`, `flat_map`,
`group_by`, and `map_values` results. `partition`, `zip`, `each_slice`,
`each_cons`, and `tally` retain their nested Array or Hash structure.
`min`, `max`, `min_by`, and `max_by` produce the joined element type. `find`,
`delete_at`, and `pop` add `Nil`. A zipped pair includes `Nil` in the other
Array's position because shorter inputs are padded at runtime. Callable-union
returns are joined before constructing mapped collection types.
Collection facts widen after mutation. `push`, indexed assignment, and compound
indexed assignment join statically known values into Array element graphs or
Hash key/value graphs. Empty literals acquire a nested graph from their first
known write, and subsequent local expressions and LSP hover observe that state.
If a write is dynamically typed or would exceed the eight-member union ceiling,
the nested fact is discarded while the outer Array/Hash kind remains known.
The same widening applies when a directly referenced local has been boxed by a
nested closure; later reads of that captured local retain the updated graph.
Mutations written inside anonymous blocks or nested definitions also widen the
enclosing local conservatively, whether or not later control flow invokes the
closure.
Direct lexical aliases of an Array or Hash share mutation facts. Reassigning
one name detaches it from aliases that still reference the previous object.
A name bound to different objects on different `if`/`case`/loop branches
detaches after the branches join, so a mutation through it afterward does not
retroactively widen either branch's object -- unless every branch agreed on
the same object, in which case the shared identity (and mutations through it)
carry across the join.
A name assigned inside an `if`/`elsif`/`else` branch, rather than to the
`if`'s own result, is also inferred as a union across the branches: `if flag
then x = 1 end; x` is `Int | Nil` (the branch that skips the assignment
reads back `nil`, same as at runtime), and `if flag then x = 1 else x = "s"
end; x` is `Int | String`. `case`/`when` and loop bodies join a branch-local
name the same way: `Nil` folds in for every `when` clause or loop exit that
doesn't independently bind the name, but not when every arm does --
`case mode when 1 then picked = 1 when 2 then picked = "s" else picked = 2.5
end; picked` is `Int | String | Float`, no `Nil`.
A mutation through one level of indexing off a plain local (`matrix[0].push(v)`,
`grid[0][1] = v`) widens that local's own element graph, not just the
indexed value's -- `matrix[0]` is one element of `matrix`, and `Array[T]`
describes every element the same way, so the editor sees `matrix` itself as
`Array[Array[T | ...]]` afterward. A second level of indexing
(`cube[0][0].push(v)`) is out of scope: only `cube`'s own type stays
tracked, not `cube[0]`'s.
Native collection blocks infer their parameter graphs from the receiver:
Array element callbacks, `each_with_index` element/index callbacks, and Hash
key/value callbacks expose those types to the body and editor hover.
`Array#sum` retains an `Int` result for Int elements and `Int | Float` for
numeric graphs containing Float. Other element graphs remain dynamic because
their repeated `+` dispatch may be overloaded.

A fixed-arity bound method reference such as `object.convert` retains its
declared parameter and return contract. Explicit generic references such as
`object.wrap[Int]` and `Factory.wrap[Int]` retain the substituted contract, so
their results compose with indexing, Callable arguments, and later calls.
When a bare generic singleton or bound instance reference is passed directly to
a statically resolved Callable parameter, Diamond can infer those bindings from
the parameter's expected Callable graph. This works for positional and keyword
arguments on functions, singleton methods, instance methods, and constructors.
A declared function return supplies the same context to an endless body, an
explicit `return` value, or the final expression of a multi-line body. Earlier
statements do not inherit the final return expectation. A standalone reference
without either call-site or return context still requires explicit bindings.
The same context recurses through elements of an expected `Array[T]` and the
keys and values of an expected `Hash[K, V]`. A literal such as
`[Tools.identity]` can therefore satisfy `Array[Callable[[Int], Int]]` without
spelling `[Int]` on the reference. A spread Array receives this context when all
remaining positional parameters declare the same type. A direct spread literal
instead maps each element to its exact positional parameter, so heterogeneous
Callable contracts can resolve independently. A dynamically sized
heterogeneous spread expression does not guess element positions.
Those positions also participate in generic inference. For example,
`second[T, U](*["ignored", 42]) -> U` resolves `T` as `String` and `U` as
`Int`, so the result remains `Int` at the call site.
Fixed arguments before and after a spread bind their corresponding generic
parameters as well. A homogeneous non-literal spread contributes only to the
middle parameter range left between those fixed edges.
Keywords may follow a spread. Positional spread elements and fixed edges infer
against their positions, while each keyword infers against its named slot. A
trailing block remains the final declared parameter rather than filling a gap
left by a keyword.
Only dynamically unresolved bound methods remain conservatively untyped.
Callable unions retain a return fact when all arms agree structurally.

Resolved variadic bound methods retain their fixed argument prefix and declared
return type. Bound Callable values also accept keyword arguments using the
original method parameter names. A trailing block passed to a Callable union is
contextually typed when every arm declares the same nested Callable contract;
otherwise its parameters remain dynamically typed.
Compatible variance is accepted: one nested Callable contract may supply block
context when it can satisfy every arm through contravariant inputs and covariant
returns. Bound references preserve omitted optional arguments, so defaults are
still evaluated by the original method rather than replaced with `nil`.
This also applies when optional fixed arguments precede `*rest`. If no complete
Callable arm supplies union-block context, Diamond may synthesize parameter
context from existing declared input types that safely accept every arm. If no
such input graph exists, it constructs a union of the arms' input graphs.
Return context uses an existing declared subtype only when it satisfies every
arm; synthetic return intersections are not inferred. Otherwise the block's
return type is determined by its body.
The complete parameter graph is visible inside the anonymous block. Calling a
method shared by every member of a synthesized union therefore retains a
structurally identical declared return graph for chained expressions.

Callable values accept the same trailing `do ... end` block as named function
and method calls, including calls with a spread Array. The block is appended as
the final positional argument.

Constructor calls accept the same trailing block for fixed and spread
arguments. The block is the final argument to `initialize`, which must declare
an `&block` parameter to bind it. Keyword constructor calls reconcile named
parameters first and then place the block in `initialize`'s final slot.

### Keyword arguments

```ruby
def move(x, y, speed = 1)
  # ...
end
move(3, 4)                    # positional
move(x: 3, y: 4)               # keyword, any order
move(3, y: 4, speed: 2)        # positional then keyword
```

A call to a top-level `def` may name its arguments instead of (or in
addition to) supplying them positionally. Positional arguments must come
first; once a keyword argument appears, every argument after it must also
be a keyword. A keyword argument can fill any parameter regardless of the
order it's written at the call site — the compiler resolves each name to
its declared position. Ordinary calls reorder entirely at compile time.
When a spread precedes keywords (`move(*coordinates, speed: 2)`), dedicated
spread bytecode merges the Array's positional values with those declared
slots at runtime, when the Array's length is known.

Keyword arguments can't skip over an earlier defaulted parameter to reach
a later one: `def f(a, b = 2, c = 3) ... end` called as `f(1, c: 5)` is a
compile error (`missing argument`) — you'd need to also pass `b`. Default
values are compiled inline into the function's own body, conditioned on
how many arguments were actually supplied counting from the start, not on
which specific ones were; keyword arguments let you name and reorder the
arguments you *do* supply, not skip an arbitrary one in the middle.

Keyword arguments work for top-level functions, user-defined instance
methods, Callable values, constructors with a Diamond-defined `initialize`,
and class/module singleton methods. Dynamic calls retain keyword names until
runtime target selection. Native C-backed receiver methods use the central
signature registry documented in `docs/internal/design.md`.

### Variadic parameters

```ruby
def sum(*nums)
  total = 0
  nums.each() do |n| total += n end
  total
end
sum()           # => 0, nums == []
sum(1, 2, 3)    # => 6, nums == [1, 2, 3]

def announce(name, *titles)
  "#{titles.join(" ")} #{name}".strip()
end
announce("Ahab")                 # => "Ahab"
announce("Ahab", "Captain")      # => "Captain Ahab"
```

A trailing `*name` parameter collects every argument beyond the ordinary
(non-variadic) ones into a plain `Array` — zero extra arguments collects
an empty one. Works on a top-level `def`, an instance method, a `self.`
singleton method, and a `closure name() ... end` alike. Deliberately
scoped narrow, matching this language's usual first-slice shape for a
feature like this (see `delegate`'s own "bare parameter names only"):

- must be the *last* parameter, and at most one per parameter list;
- bare name only — no `: Type` annotation, no `= default` (neither means
  anything for a collected `Array`);
- a *literal* call site can still only ever supply at most 255 argument
  expressions in total, variadic or not — a pre-existing limit on every
  call form (`"too many call arguments"`), not something specific to
  this feature. Call-site spread (below) isn't subject to it, since a
  spread argument's length is a runtime value, not one argument
  expression per element. A function/method declaration is separately
  capped at 32 parameters — see docs/internal/design.md's "No artificial
  call-argument/parameter ceiling" for why the two limits differ.

A variadic parameter widens `Callable[N]` matching too: a variadic
closure/function satisfies `Callable[N]` for any `N` at or above its own
required-argument count, not just an exact match — see docs/internal/design.md's
"Splat/variadic parameters" section for the full mechanism.

### Call-site spread

```ruby
def sum3(a, b, c)
  a + b + c
end
args = [1, 2, 3]
sum3(*args)          # => 6

def sum(*nums)
  total = 0
  nums.each() do |n| total += n end
  total
end
sum(*(1..50).to_a())  # => 1275, no argument-expression limit here
```

`foo(*array)`, `receiver.method(*array)`, `callable(*array)`,
`ClassName.new(*array)`, and `Namespace.method(*array)` expand an Array's elements
into positional arguments at the call site — the caller-side counterpart
to a variadic *parameter* above. The supported slice is deliberately narrow:

- direct functions, user-defined instance methods, Callable values,
  constructors, module/class singleton methods, and native receiver methods
  are supported;
- one spread argument can appear before, between, or after fixed positional
  arguments — `foo(1, *middle, 4)` preserves left-to-right order;
- all Diamond-defined targets may place keywords after the spread and any
  fixed positional suffix — `foo(1, *middle, last: 4)`; collisions and gaps
  are diagnosed at runtime because the spread length is dynamic;
- native C-backed receiver methods use the same keyword-after-spread form;
- explicit generic bindings compose with spread for functions and user-defined
  instance/class/module methods (`identity[Int](*values)`);
- arity and method visibility are checked against the Array's actual length at *runtime*
  (unlike an ordinary call, which the compiler validates against a
  statically-known callee's arity where it can) — too few or too many
  elements for a non-variadic target still raises `ArgumentError`, same
  message as any other arity mismatch; a non-`Array` argument raises
  `TypeError`.
- native receiver spreads share ordinary native invocation's 255-argument bound;

See docs/internal/design.md's "Call-site spread" section for the full mechanism.
