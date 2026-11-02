# Gradual typing and exceptions

[Language reference](syntax.md) · Previous: [Classes, modules, and methods](classes-and-modules.md) · Next: [Collections and Enumerable](collections.md)

## Gradual typing

```ruby
def choose(flag: Bool, value: Record | Nil) -> Record | Nil
  if flag
    value
  else
    nil
  end
end

def checked(values: Hash[String, Int | Nil]) -> Hash[String, Int | Nil]
  values
end
```

Optional `name: Type` parameter annotations and `-> Type` return
annotations, pipe-separated unions, generics (`Array[T]`, `Hash[K, V]`),
`is` for runtime type tests, and structural interfaces (`Sized`,
`Callable[n]`-style).

Classes can declare one or two type parameters, inferred at construction or supplied explicitly:

```ruby
class Box[T]
  def initialize(value: T)
    @value = value
  end
  def value() -> T = @value
  def replace(value: T)
    @value = value
  end
  def map[U](transform: Callable[[T], U]) -> Box[U] = Box[U].new(transform(self.value()))
end

box = Box.new(42)
box.replace(43)
puts(box.value())
box.replace("wrong")  # TypeError: expected Int, got String
```

Two parameters bind independently, including in generated struct readers:

```ruby
struct Result[T, E](value: T, error: E)
end
result = Result[Int, String | Nil].new(42, nil)
```

Both arguments are required when specializing a two-parameter class or struct.
Each stored binding is invariant. Method parameters follow both receiver slots.

Construction infers `T` from the supplied, annotated constructor arguments,
including nested `Array[T]` and `Hash[String, T]` parameters. Multiple values
contributing to `T` form a union, as for generic functions. The resulting binding
belongs to that instance: every annotated instance-method argument and return
uses it, including through method references, `dup()`, and thread copies.
Another instance can bind a different type. Unannotated fields and methods keep
their usual dynamic behavior.

The compiler retains an exact scalar constructor binding: `Box.new(42)` carries
`Box[Int]` through assignments and chained calls, so `box.map(as_string)` retains
`Box[String]` when the callback is typed. Keyword arguments and literal spreads
also preserve these facts. Broader input types, instances of nongeneric classes,
and structural collection inference provide editor hints where representable, while keeping
runtime checks for the invariant binding. Unknown contributing arguments,
conflicting inferred shapes, and dynamic spreads stay conservative.

Generic instances retain their stored arguments during inference:
`Box.new(Box.new(42))` binds `Box[Box[Int]]`. This also works through generic
functions, collections, both parameters of a generic struct, `dup()`, and thread
copies. A known invariant instance binding can preserve compiler return facts;
broader or unknown inputs still use conservative editor hints and runtime checks.
Contributions with the same generic class must agree on its stored arguments.
Conflicting bindings and graphs beyond the runtime's capacity raise `TypeError`
instead of discarding an inner contract.

`Box[Int].new(42)` supplies the binding before `initialize` runs. It works
with positional, spread, keyword, defaulted, and trailing-block arguments.
An explicit binding also permits construction when there is no value to infer
from, such as `Box[Array[Int]].new([])` or a generic class with no initializer.
Without an explicit argument, the supplied constructor arguments must provide
enough information: for example, an empty `Array[T]` parameter cannot infer its
element type.

The exception is a variable that only a callable with no declared return could
bind, which leaves it *open*. Such an instance accepts any result, and shows as
`Schema[?]`. Generic classes built from it stay open too, but it does not satisfy a closed
annotation like `Schema[Item]`, whose callers rely on results nothing checked. Declare the
callable's return or write `Schema[Item].new(...)` to close it. A subclass that transforms its
parent's arguments (`Sub[T] < Base[Array[T]]`) cannot be left open and raises instead.

```ruby
class Schema[T]
  def initialize(builder: Callable[[Hash], T])
    @builder = builder
  end
  def build(row: Hash) -> T = @builder(row)
end

def build_open(row) = Item.new(row)
def build_item(row: Hash) -> Item = Item.new(row)
def first(schema: Schema[Item]) -> Item = schema.build({})

first(Schema.new(build_item))        # closed: T is Item, and each result is checked
first(Schema[Item].new(build_open))  # closed explicitly
first(Schema.new(build_open))        # TypeError: expected Schema[Item], got Schema[?]
Schema.new(build_open).build({})     # open: works, and promises nothing
```

```ruby
def read(box: Box[Int]) -> Int = box.value()
def identity[T](box: Box[T]) -> Box[T] = box

read(Box[Int].new(42))
read(identity(Box.new(43)))
```

Parameterized class annotations check the stored binding, including in return
annotations and nested collection contracts. They are invariant: a mutable
`Box[Int]` is not a `Box[Int | String]`, or vice versa. Plain `Box` only checks
class identity. Nested bindings can be supplied explicitly, for example
`Box[Box[Int]].new(Box.new(42))`; inferred construction preserves the same nested
instance arguments. Explicit method
type arguments cannot override an instance's binding. `is Box` likewise tests
the class identity; parameterized `is` tests are not supported.

Instance methods can introduce their own parameters after the class parameter:
`map[U]` uses the receiver's fixed `T` and binds a new `U` for each call. For
example, `Box[Int].new(42).map[String](as_string)` returns a `Box[String]` when
`as_string` has signature `(Int) -> String`; the original box remains a
`Box[Int]`. Omit `[String]` when a typed argument or callback signature can infer
`U`. An unannotated trailing block can use an explicit method argument:
`box.map[String]() do |value| value.to_s() end`. Explicitly specialized receivers
also supply their class argument to contextual block typing.

Explicit method arguments bind only method parameters. They cannot replace `T`,
and a method cannot redeclare its class's parameter name. The total budget is
eight variables shared between the class parameters and method parameters. Bound
method references support concrete arguments and enclosing type variables.
For example, `def reference[U](box) = box.echo[U]` returns a Callable that
retains that invocation's `U` after `reference` returns. References made inside
generic instance methods also retain receiver-owned and method-owned slots
independently. Their typed Callable contracts use these saved bindings rather
than the later caller's type variables. Capturing bound references retain the
existing thread-transfer restriction on captured closures.
Module-function and class-singleton references also retain enclosing bindings:
`def reference[T]() = Helpers.identity[T]` returns a Callable specialized to that
invocation's `T`. These references have no captured value cells, so threads can
copy their immutable type graphs when used as workers, arguments, or results.
Supervised workers retain the same saved type environment across attempts.
Top-level generic functions can also be specialized as values: `identity[Int]`
is a typed Callable, while `identity[Int](42)` calls the function immediately.
`def reference[T]() = identity[T]` saves the enclosing invocation's binding.
References may precede their function declaration, and retain defaults,
variadic parameters, and names for keyword calls. These wrappers have no
captured value cells and support the same thread transfers as singleton references.

This native-compiler slice supports up to two class parameters in directly declared
instance methods and generated struct fields. A concrete subclass can fix its
generic parent's arguments: `class StringBox < Box[String]`. It inherits the
parent's fields and methods, and `StringBox.new("hello")` retains `String`
through inherited getters, overrides, `super`, duplication, and thread copies.
Both parent arguments may be fixed, including nested types, native kinds, and
Callable signatures. Further concrete subclasses inherit the same bindings.
The concrete subclass cannot supply new type arguments or infer replacements
from its constructor values. Fixed parent arguments must be concrete and fit
the bounded binding graph. A parameterized subclass may instead forward its
parameters: `class Child[A] < Box[A]`, `class Result[A, B] < Pair[A, B]`, or
`class Swapped[A, B] < Pair[B, A]`. Parameters may have different names from the
parent's. Child parameters may also be used only by the child's own methods.
Explicit or inferred constructor arguments then bind the inherited slots;
inherited methods, `super`, method references, and further subclasses retain
those bindings in the correct ancestor view. Constructor inference maps the
parent's slots back to the subclass's declaration order, and permutations compose
through multiple inheritance levels. Parent arguments may also wrap parameters,
as in `class Collection[T] < Box[Array[T]]`. An inferred
`Collection.new(["hello"])` binds the child's `T` to `String` and the inherited
parent's `T` to `Array[String]`. Nested containers, class arguments, native types,
and Callable signatures retain their metadata through this substitution.
Transformations compose with forwarding and reordered parents; inherited
constructors infer child parameters by matching the parent argument structure.
Repeated occurrences share one binding: `class Point[T] < Pair[T, T]`
joins constructor witnesses for both parent slots. `Point.new(1, "two")` binds
`T` to `Int | String` and exposes `Pair[Int | String, Int | String]` to inherited
methods. Explicit specialization fixes that shared contract instead. Empty
collection contributions can be completed by other occurrences; empty-only
inference still requires an explicit argument. Repetitions can occur inside
containers or Callable signatures, and contextual constructor blocks use the
same coherent ancestor view. Conflicting invariant instance or Callable graphs
and bindings beyond the graph budget are rejected rather than weakened.
A parameter or return annotation may combine a type variable with other members,
as in `-> T | Nil`, but a class type argument may not: `Box[T | Nil]`. Parent and child may have different parameter
counts: `class StringResult[T] < Result[T, String]` fixes the parent's second
slot while inferring the child's `T` from the first. `StringResult.new(42, "error")`
and `StringResult[Int].new(42, "error")` both retain the parent view
`Result[Int, String]`. Fixed slots use their declared contracts, including for
omitted defaults and empty collections, and cannot be rebound by constructor
arguments. Reordered and transformed arguments compose with these fixed slots.
Two child parameters may also be combined into one parent argument, as in
`class Entries[A, B] < Box[Hash[A, B]]`.
Structs also support these parent mappings. Their generated constructors
specialize inherited field annotations and can infer additional child parameters
from new fields; those parameters need not occur in the parent arguments.
See [generic struct inheritance](classes-and-modules.md#struct-declarations).
Ordinary classes may also introduce child-only parameters. For example,
`class Tagged[T, U] < Box[T]` can define `initialize(value: T, tag: U)`, call
`super(value)`, and infer both bindings from `Tagged.new(42, "label")`.
An inherited constructor cannot infer a child parameter absent from its parent
view; supply explicit arguments such as `Tagged[Int, String].new(42)` if the
child inherits `Box`'s constructor instead. Parameterized classes can inherit
nongeneric or fixed parents as well: `class Labelled[T] < StringBox` retains
`StringBox`'s fixed bindings while its own constructor infers `T`. Missing or
empty-only witnesses still fail rather than dropping a child binding.
Reopening these classes and referring to class parameters from nested
definitions remain future work. Native types such as
`Time` and typed Callable signatures work as explicit or inferred class
arguments. Bindings retain native kinds, Callable arity, parameter types, and
return types, including through duplication and thread copies. Class type
arguments combining a type variable with other members, repeated outer types with different
arguments, and contracts beyond the runtime binding graph's capacity are also
rejected rather than weakened. Calls retain runtime checks. On a receiver with a
known class argument, declared method returns substitute that argument and the
method's own explicit or inferred arguments: `Box[Int].new(42).map[String](...)` carries
`Box[String]` through assignments and chained calls, and record projections retain
`Array[Model]` or `Array[String]`. The editor follows representable class and Array
graphs and primitive bound returns; missing or conflicting bindings remain
conservative. Inferred receiver facts remain editor-only and never remove runtime
checks.

`-> Self` is an instance of the class a method was called on, which lets a method
shared through inheritance describe what each subclass gets back:

```ruby
class Model
  def self.find(id: Int) -> Self | Nil = self.load(id)
  def touch() -> Self
    @touched = true
    self
  end
end

class Author < Model
  def self.load(id: Int) = Author.new()
  def greet() -> String = "hello"
end

puts(Author.find(1).touch().greet())
```

In a `def self.x` method the receiver is the class named at the call, and in an instance
method it is the instance's own class. The result is checked when the method returns:
a subclass instance is accepted, and a sibling class or the class value itself raises
`TypeError`. Returning `self` from an instance method needs no check. `Self` can stand
alone or join a union, but it can't appear in a parameter, inside `Array[Self]` or in an
interface, and a program's own type named `Self` takes priority. The check covers the class
and not a generic class's type arguments, so a call on a generic class publishes no fact.
A method declaring `Self` counts as unannotated for interface conformance, has no
return fact when the receiver's class isn't known, and always runs in the interpreter
when it has to check.

```ruby
interface Named
  def name() -> String
end

interface Greeter < Named
  def greet() -> String
end
```

`interface Name ... end` declares a bodyless list of method
signatures; any class or native `String`/`Array`/`Hash` value that
structurally implements them satisfies it — no `implements`
declaration needed, and a value can satisfy any number of unrelated
interfaces at once. `interface Sub < Base1, Base2` builds one
interface out of others: their method signatures are flattened into
`Sub` at compile time (no runtime interface hierarchy), so it's an
error to redeclare a name a base already provides, or for two bases to
share a method name — both are just "duplicate interface method",
resolved by not repeating the signature. Native `String`/`Array`/`Hash`
values satisfy an interface method against the VM's real method
surface (e.g. `strip`/`slice`/`to_i`/`split` on `String`, `push`/`pop`
on `Array`, `key_at`/`value_at` on `Hash`, `length` on all three), not
just the handful of collection primitives structural interfaces
originally recognized; a method whose native return type isn't one
fixed scalar (`pop`, `key_at`, `value_at`, `String#index_of`) can still
satisfy an interface method with no return annotation, just never one
that requires a specific return type.

Satisfying a `-> Type` interface method needs the implementing method's
own explicit `-> Type` return annotation (an endless `def name() -> String
= "Ada"` works; a bare `def name() = "Ada"` does not, even though its
return type is otherwise correctly inferred as `String`) — interface
conformance is checked against `return_type_set`, which only an explicit
annotation populates, not the separate, tooling-only return-type inference
compile_definition also computes for every function (see docs/roadmap.md's
"Improve receiver-aware tooling").

Generic classes and structs satisfy interfaces using each instance's fixed type
arguments. `Box[String]` can satisfy `StringSource` below; `Box[Int]` cannot:

```ruby
interface StringSource
  def value() -> String
end
struct Box[T](value: T)
end
def read(source: StringSource) -> String = source.value()
puts(read(Box.new("hello")))
```

Conformance substitutes both receiver parameters through nested class,
collection, and Callable signatures. Parameters remain contravariant and returns
covariant. Known incompatible specializations are compile errors; unknown or
symbolic receiver arguments retain runtime checks. `is Interface` checks the
actual instance bindings, including after duplication or a thread copy.
Unrelated generic methods do not prevent conformance. A required method may
introduce its own type parameters: `identity[U](value: U) -> U` satisfies a
method taking and returning `Int`, for example. Each method variable joins the
interface's supplied input types into a union, including occurrences inside
collection and Callable signatures. For example, `same[U](left: U, right: U) -> U`
satisfies a method taking `Int` and `String` and returning `Int | String`.
Structurally equal inputs are deduplicated, including nested types. The complete specialized signature must
then pass parameter and return variance checks. Receiver variables stay fixed;
an unrelated caller's variables cannot supply a witness. Return-only variables
cannot establish a concrete return contract. Witnesses
must fit the bounded type graph, including native kinds and typed Callable
values. Callable parameters remain contravariant and returns covariant.
Incompatible Callable alternatives sharing a binding are rejected rather than
merged into a broader signature. Foreign dynamically compiled method bodies are not specialized
by this check. Recursive checks that cannot establish a
finite proof are rejected conservatively. An unconstrained collection binding
cannot promise a typed element contract.

A call to a known top-level function is checked at compile time where
the compiler can prove an argument is the wrong type -- `f("s")` for
`def f(x: Int)` is a compile error naming the parameter
(`argument 'x' of f: expected Int, got String`). Anything short of proof
(a value whose type depends on a branch, an untyped function's result, a
generic or variadic callee) is still checked when the call runs, as a
rescuable `TypeError`. That includes a bare `Array` or `Hash`, which says
nothing about its elements: returning one where `Array[String]` is
expected checks the elements at run time, while returning a `Hash` there
is a compile error.

A one-line guard that exits narrows everything after it:
`return "none" if x == nil` leaves `x` non-nil below it, and
`raise ... unless x is String` leaves it a `String`. The same holds for
`break`, `next`, `redo`, and `retry` guards, and for the block form of
`if`.

Native values can be named too: `Time`, `Fiber`, `Channel`, `Thread`,
`Supervisor`, `File`, `Socket`, `Listener`, `UDPSocket`, `TLSSocket`,
`Regexp`, `Tensor`, `SQLite3`, `Statement`, `PostgreSQL`, `MySQL`,
`ProcessResult`, `ProcessHandle`, `ProcessStream`, and `ProgramBuilder`
work in annotations, struct fields, unions (`Time | Nil`), `is`, and
`is_a?`. A program's own class of the same name takes precedence.

`x is Foo && y is Bar` narrows both `x` and `y` inside the branch where
the whole condition is true (and `unless ... || ...`'s branch narrows
both operands where the whole condition is false) — composed the same
way chained/nested `&&`/`||` naturally compose, including through a mix
of both. The reverse direction of each (the `else` of an `&&`, the
`then` of an `||`) isn't narrowed: `!(A && B)` doesn't reduce to a
simple fact about either operand in general, so nothing is narrowed
there rather than guessing.

`value.class()` returns a `String` naming `value`'s own runtime type —
`"Int"`, `"String"`, `"RuntimeError"` for a user class instance, and so
on, the exact same bare name already used in every "expected X, got Y"
type-error message. It works uniformly on every value, including native
kinds (`Int`, `Array`, ...) that have no user-defined class of their
own. `value.is_a?(Type)` is a runtime boolean test against the same rich
type grammar `is`/`rescue error: Type` already accept — a class name
(with subclass matching), an interface (structural, same rules as
above), or a built-in structural type like `Sized`:

```ruby
class Dog
end

interface Named
  def name() -> String
end

dog = Dog.new()
puts(dog.class())            # "Dog"
puts(dog.is_a?(Dog))         # true
puts(1.is_a?(Sized))         # false
puts("x".is_a?(Sized))       # true
```

Neither is a general expression: `Type` in `is_a?(Type)` is a type name
resolved entirely at compile time, the same way a `rescue error: Type`
clause's type is — there is no way to obtain a class as an ordinary
runtime value to pass around, store, or compute `is_a?`'s argument from
(see docs/internal/design.md's `DIAMOND_VALUE_CLASS` section, and docs/
roadmap.md's "Explicitly deferred" section for why that stays out of
scope).

## Exceptions

```ruby
result = begin
  raise "boom"
rescue error: TypeError
  0
rescue final
  final + "!"
end
```

`raise value` accepts any value, not only `Exception` instances. Unlike Ruby,
`raise "boom"` raises the String itself, not a `RuntimeError`, so `rescue error:
StandardError` does not catch it; raise `RuntimeError.new("boom")` when callers
should be able to rescue by class. `rescue
name` binds the raised value unconditionally; `rescue error: Type1 | Type2`
matches nominally (up to eight types, with subclass matching) before
binding. `ensure` provides cleanup that runs on normal completion,
exceptions, and explicit `return` alike. Built-in exception classes, all
under `StandardError` except `SystemStackError` (direct `Exception`
subclass): `RuntimeError`, `TypeError`, `ArgumentError`, `IndexError`,
`ZeroDivisionError`, `RangeError`, `FiberError`, `IOError`, `RegexpError`,
`WouldBlockError`, `ThreadError`, `NoMethodError`, `JSONError`, and the
native database drivers' `SQLite3Error`, `PostgreSQLError`, and
`MySQLError`.

A `def` or `do` block body can take the same `rescue`/`else`/`ensure` clauses
without a `begin`, as in Ruby. The clauses cover the whole body, and `retry`
reruns it:

```ruby
def parse_port(text: String) -> Int
  port = text.to_i()
  raise ArgumentError.new("not a port: #{text}") unless port > 0 && port < 65536
  port
rescue error: ArgumentError
  warn(error.message())
  7000
end

parse_port("8080")   # => 8080

ratios = [1, 0, 2].map() do |n|
  10 / n
rescue error: ZeroDivisionError
  0
end
ratios               # => [10, 0, 5]
```

A body without such clauses compiles exactly as before; the handler is only
installed for one that has them.

`Exception.new(message, cause)` takes up to two positional arguments
(both optional), readable back via `.message()`/`.cause()`. A subclass
that overrides `initialize` to take its own extra arguments reaches the
built-in constructor the normal way, with `super(message)` (or
`super(message, cause)`):

```ruby
class ValidationError < StandardError
  def initialize(message, field)
    super(message)
    @field = field
  end
  def field()
    @field
  end
end
```

`.backtrace()` returns an Array of `"chunk:line:column"` Strings, one per
still-live call frame, captured at the moment `raise` runs (not lazily
when `.backtrace()` is called) — it reflects where the exception was
raised even after the frames that were active then have long since
returned by the time a `rescue` clause inspects it. `nil` before an
instance is ever raised. Only the first raise records it: raising the
same exception again (`raise error` in a rescue clause, or a `rescue`
whose types don't match) keeps the original, and an uncaught re-raised
exception's report adds `(raised at chunk:line:column)`.

Errors the runtime raises itself (`ZeroDivisionError`, `TypeError`,
`IOError`, ...) behave the same way: `.message()` is just the message, and
`.backtrace()` lists where the error happened and every frame it passed
through.
