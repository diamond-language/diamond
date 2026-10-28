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

Classes can declare one type parameter, inferred at construction or supplied explicitly:

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
also preserve these facts. Broader input types, user-class values, and structural
collection inference provide editor hints where representable, while keeping
runtime checks for the invariant binding. Unknown contributing arguments,
conflicting inferred shapes, and dynamic spreads stay conservative.

`Box[Int].new(42)` supplies the binding before `initialize` runs. It works
with positional, spread, keyword, defaulted, and trailing-block arguments.
An explicit binding also permits construction when there is no value to infer
from, such as `Box[Array[Int]].new([])` or a generic class with no initializer.
Without an explicit argument, the supplied constructor arguments must provide
enough information: for example, an empty `Array[T]` parameter cannot infer its
element type.

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
`Box[Box[Int]].new(Box.new(42))`; inference from a value of a user class still
retains that class's identity rather than its own type argument. Explicit method
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
eight variables: one class parameter and up to seven method parameters. Bound
method references support concrete explicit arguments; references that would
capture an enclosing type variable are rejected instead of losing that binding.

This native-compiler slice supports one class parameter in directly declared
instance methods and generated struct fields. Generic inheritance, reopening,
structural-interface conformance, and referring to the class
parameter from nested definitions remain future work. Native objects and
Callable types are not yet supported as explicit class type arguments. Unions
combining a type variable with other members, repeated outer types with different
arguments, and contracts beyond the runtime binding graph's capacity are also
rejected rather than weakened. Calls retain runtime checks. On a receiver with a
known class argument, declared method returns substitute that argument and the
method's own explicit or inferred arguments: `Box[Int].new(42).map[String](...)` carries
`Box[String]` through assignments and chained calls, and record projections retain
`Array[Model]` or `Array[String]`. The editor follows representable class and Array
graphs and primitive bound returns; missing or conflicting bindings remain
conservative. Inferred receiver facts remain editor-only and never remove runtime
checks.

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
