# Types

d6 is *gradually typed*. You can write a whole program without a single type
annotation, and it runs like a dynamic language. Each annotation you add is a
promise the language enforces: the compiler rejects code it can prove breaks
the promise, and the runtime checks the rest the moment a value arrives.

## Where types go

Types are written on function parameters, function results, attributes and
struct fields. Local variables never carry an annotation; their type is
whatever was assigned.

```ruby
def average(values: Array[Int]) -> Float
  values.sum().to_f() / values.length()
end

puts(average([3, 4, 5]))   # => 4.0
```

The names you will use most:

| Type | Values |
|---|---|
| `Int`, `Float`, `String`, `Bool`, `Symbol` | the built-in values |
| `Nil` | only `nil` |
| `Array[Int]`, `Hash[String, Int]` | collections with typed elements |
| `Int \| String` | either type (a *union*) |
| `String \| Nil` | a String that may be missing |
| `Callable[[Int], String]` | a function from `Int` to `String` |
| `Account`, `Shape` | instances of your own classes |

## What the compiler catches

When the compiler can see a value's type, a mismatch is an error and the
program does not start. Here the first `puts` never runs:

```ruby
def double(n: Int) -> Int = n * 2
puts("starting")
puts(double("x"))
```

```text
error: argument 'n' of double: expected Int, got String
```

The compiler also checks a function's result against its `-> Type`, a `nil`
passed where `nil` is not allowed, and an object passed where an interface is
expected. It always rejects undefined local variables and syntax errors,
typed or not.

## What the runtime catches

Some values are only known when the program runs: an element read from an
untyped array, a value from an untyped function, input you parsed. When one
of those reaches a typed parameter, attribute or result, d6 checks it right
there and raises an error that names both types:

```ruby
def double(n: Int) -> Int = n * 2

values = [21, "x"]
puts(double(values[0]))   # => 42

begin
  double(values[1])
rescue e: TypeError
  puts(e.message())       # => expected Int, got String
end
```

Untyped code is never checked, so the error surfaces wherever the bad value
is finally used, as in any dynamic language:

```ruby
def twice(n) = n * 2
puts(twice(21))     # => 42
puts(twice("ab"))   # => abab
```

The practical rule: annotate the edges of your code (public functions,
attributes, struct fields) and bad values are stopped where they enter,
with an error that says what was expected.

## Nil

A type does not include `nil` unless you say so. Passing `nil` to a `String`
parameter is a compile error. To accept a missing value, write the union
`String | Nil` and handle the `nil` case:

```ruby
def greet(name: String | Nil) -> String
  return "Hello, stranger" if name == nil
  "Hello, #{name}"
end

puts(greet(nil))     # => Hello, stranger
puts(greet("Ada"))   # => Hello, Ada
```

After a check such as `name == nil`, `if name`, or `name != nil`, d6 treats
the variable as the non-`nil` type for the rest of that branch.

d6 does not yet *force* you to check: calling `name.upcase()` on a
`String | Nil` without a check compiles, and raises
`undefined method 'upcase' for Nil` at run time if the value is `nil`. The
annotation still documents the possibility and rejects `nil` everywhere it
was not declared.

When `nil` should simply pass through, use `&.` instead of `.`. It calls the
method when the value is there and gives `nil` without calling it when the
value is `nil`:

```ruby
ages = {"ada": 36}
puts(ages["ada"]&.to_s())             # => 36
puts(ages["bob"]&.to_s().inspect())   # => nil
```

Only `nil` is skipped (`false&.to_s()` is `"false"`), and only that one call:
in `a&.b().c()`, `c` is still called on the `nil`, so write `a&.b()&.c()` when
each step may be missing. The result is `T | Nil`, so a check narrows it as
above.

## Unions and is

A union accepts a value of any of its member types. `value is Type` tests
which one you have, and narrows the variable inside the branch:

```ruby
def describe(value: Int | String) -> String
  if value is Int
    "the number #{value + 1}"
  else
    "the text #{value.upcase()}"
  end
end

puts(describe(41))     # => the number 42
puts(describe("hi"))   # => the text HI
```

## Generics

Functions and classes can take type parameters in square brackets. d6 infers
them from the arguments, so you rarely write them at the call site:

```ruby
def first_of[T](items: Array[T]) -> T = items[0]
puts(first_of([3, 4]) + 1)         # => 4
puts(first_of(["a", "b"]).upcase())   # => A

class Box[T]
  attr_reader value: T
  def initialize(value: T)
    @value = value
  end
end

puts(Box.new(5).value() + 1)          # => 6
puts(Box[String].new("x").value())    # => x
```

## Interfaces

An interface lists methods. Any class that has them satisfies it, without
declaring so:

```ruby
interface Shape
  def area() -> Float
end

class Square
  def initialize(side: Float)
    @side = side
  end
  def area() -> Float = @side * @side
end

def report(shape: Shape) -> String = "area #{shape.area()}"
puts(report(Square.new(2.0)))   # => area 4.0
```

Passing an object whose class lacks `area` is a compile error:
`expected Shape, got Rock`.

## Coming from another language

- **TypeScript**: types are checked by the language itself, at compile time
  *and* at run time; a wrong value cannot slip past an annotation through an
  untyped path. Interfaces are structural, as in TypeScript.
- **Python with type hints**: annotations are not just hints. They are
  enforced without a separate checker.
- **Ruby**: there are no separate signature files; types sit in the code,
  and leaving them out is always allowed.

The [types and errors reference](../types-and-errors.md) covers the full
type grammar, inference, and narrowing rules.

Next: [Errors](errors.md).
