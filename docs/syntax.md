# Diamond syntax overview

Ruby-like surface syntax, but expression-oriented and gradually typed — no
separate "typed" object model, just optional annotations on top of dynamic
dispatch. This document is a tour of the surface syntax; see `docs/design.md`
for how it compiles and executes, `docs/object-model.md` for the object
model in more depth, `docs/fibers.md` for fibers, `docs/io.md` for I/O, and
`docs/http.md` for the HTTP library.

## Basics

```ruby
x = 0
sum = 0
while x < 10
  x = x + 1
  if x > 5
    sum = sum + x
  end
end
```

`if`/`while`/`begin` and friends are all expressions — the last line of a
block is its value. Blocks are `end`-delimited throughout; there are no
braces. `next`/`break` work inside loops.

## Functions and closures

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

## Classes

```ruby
class Named
  def initialize(name)
    @name = name
  end
  def render(prefix)
    prefix + @name
  end
end

class LoudNamed < Named
  def render(prefix)
    self.name() + prefix
  end
end
```

`@ivar` instance fields, `initialize` as the constructor, single inheritance
via `<`, and `super(...)`. Classes are compile-time metadata, not
first-class heap values, with one narrow exception:
`ClassName.redefine_method(name, callable)` repoints an existing method's
compiled body at runtime.

## Modules

`module Name ... end` declares a module; `include Name` copies its method
descriptors into the receiving class (mixin-style composition, not
inheritance). `def self.name` inside a module declares a namespace
singleton function rather than an instance method.

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

## Collections

`[1, 2]` for arrays and `{"key": value}` for hashes, both with optional
type parameters. `.push`, `.pop`, and `.length()` are native on both.
Strings support `.length()`, `.index_of(needle)` (position or `nil`),
`.slice(start, length)`, `.to_i()` (lenient decimal parsing), and
`.downcase()`/`.upcase()` (ASCII-only case conversion), and
`.reverse()` (byte-level, not UTF-8-grapheme-aware — consistent with
the rest of the language having no other Unicode awareness), and
`.strip()` (trims leading/trailing ASCII whitespace). `.split(separator)`
returns an `Array` of every piece around non-overlapping occurrences of
`separator` (an empty `separator` splits into one-character strings);
unlike Ruby, it keeps every piece including empty ones from consecutive
or leading/trailing separators (no trailing-empty suppression) — a
deliberate simplification, not an attempt at Ruby compatibility.
`.ord()` returns the first byte's value as an `Int`; an empty String
raises a rescuable `IndexError`.

`.each(callback)`, and the `Enumerable` methods derived from it —
`.select`/`.count`/`.any?`/`.all?`/`.reduce`/`.map` — work as receiver
syntax on both arrays and hashes (a hash's Enumerable operates over
*values*, discarding keys), forwarding at runtime to ordinary Diamond
functions in the prelude:

```ruby
[1, 2, 3, 4].select(is_even)
{"a": 1, "b": 2}.count(is_positive)
[1, 2, 3].reduce(0, add)
```

Any user-defined class gets the same six methods for free by implementing
its own `each(callback)` and `include`-ing `Enumerable`.

A handful of further Array/Hash conveniences live in the prelude as
plain functions (`array_reverse(values)`, not `values.reverse()` —
receiver syntax only exists for the natives and the Enumerable/forwarding
set above): `array_reverse(values)` returns a new array in reverse
order; `array_concat(values, other)` returns a new array with `other`'s
elements appended; `array_compact(values)` returns a new array with any
`nil` elements dropped; `array_uniq(values)` returns a new array with
only the first occurrence of each distinct (`==`) element, order
preserved; `array_flatten(values)` returns a new array with nested
arrays fully flattened (recursively, matching Ruby's default);
`array_join(values, separator = "")` stringifies each element (the same
formatting string interpolation uses) and joins them with `separator`
between (not trailing); `array_delete_at(values, index)` mutates
`values` in place (like the native `.push`/`.pop`), removing and
returning the element at `index`, or `nil` without mutating if `index`
is out of bounds; `hash_merge(a, b)` returns a new `Hash` with `a`'s
pairs then `b`'s applied on top (`b` wins on key conflicts).

`Int` has no per-value method dispatch (it's a scalar `DiamondValue`,
not a heap object), so numeric helpers are plain functions:
`abs(x)`/`min(a, b)`/`max(a, b)`. `abs` inherits the overflow check
already on negation, so `abs` of the most negative `Int` raises a
rescuable `RangeError` rather than silently wrapping. `mod(a, b)`
computes `a - (a / b) * b`; since `/` is C-style truncating division,
`mod`'s result takes the sign of `a`, not always non-negative (not
Euclidean/Python-style mod) — `mod(-7, 3)` is `-1`, not `2`. `b == 0`
raises the same `ZeroDivisionError` the division inside it already
would.

`array_sort(values: Array[Int])` returns a new sorted array (input
untouched); `Int` is the only type with a native ordering comparison,
so this is Int-only, checked up front (`expected Array[Int], got
Array` on a non-Int element) rather than failing confusingly mid-sort.

## Fibers

```ruby
f = Fiber.new(callable)
f.resume(0)      # runs/resumes; returns the yielded or completed value
f.status()       # "runnable" / "suspended" / "completed" / "failed"
f.alive?()
```

Inside the callable's body, `yield(value)` suspends and sends `value` out
to whoever resumes; the next `.resume(v)` delivers `v` back in as
`yield`'s own expression result. `Fiber.new`'s argument must be a
zero-argument callable (captures are fine — only nested `def`s produce a
referenceable one, per the closures section above).

## I/O

```ruby
puts("hello, #{name}")     # print(value) has no trailing newline
line = gets()               # one line from stdin, nil at EOF

f = File.open("data.txt", "w")
f.write("some text")
f.close()

server = TCPServer.listen(8080)
conn = server.accept()      # blocks until a client connects
conn.gets()
conn.write("response\n")
conn.close()

client = TCPSocket.connect("example.com", 8080)
```

A connected socket (from `.connect` or `.accept()`) is a `File` under the
hood, so `.read()`/`.read(n)`/`.gets()`/`.write(value)`/`.close()` work
identically on both. `require "lib/http"` adds a minimal Rack-style
`http_serve(port, handler)` on top — see `docs/http.md`.

## No AST

The compiler is a single-pass Pratt parser that emits register bytecode
directly; there is no retained AST. Pass `--dump-bytecode` on the CLI to
see how any construct in this document actually lowers.
