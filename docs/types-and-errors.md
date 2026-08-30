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

`x is Foo && y is Bar` narrows both `x` and `y` inside the branch where
the whole condition is true (and `unless ... || ...`'s branch narrows
both operands where the whole condition is false) — composed the same
way chained/nested `&&`/`||` naturally compose, including through a mix
of both. The reverse direction of each (the `else` of an `&&`, the
`then` of an `||`) isn't narrowed: `!(A && B)` doesn't reduce to a
simple fact about either operand in general, so nothing is narrowed
there rather than guessing.

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

`raise value` accepts any value, not only `Exception` instances. `rescue
name` binds the raised value unconditionally; `rescue error: Type1 | Type2`
matches nominally (up to eight types, with subclass matching) before
binding. `ensure` provides cleanup that runs on normal completion,
exceptions, and explicit `return` alike. Built-in exception classes:
`Exception`, `StandardError`, `RuntimeError`, `TypeError`, `ArgumentError`,
`IndexError`, `ZeroDivisionError`, `RangeError`, `SystemStackError`,
`FiberError`, and `IOError`.

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
instance is ever raised.
