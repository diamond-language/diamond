# Diamond syntax overview

Ruby-like surface syntax, but expression-oriented and gradually typed — no
separate "typed" object model, just optional annotations on top of dynamic
dispatch. This document is a tour of the surface syntax; see `docs/design.md`
for how it compiles and executes, `docs/object-model.md` for the object
model in more depth, `docs/fibers.md` for fibers, and `docs/io.md` for I/O.
Diamond's runtime intentionally has no HTTP support built in — see
`docs/packages.md` for `facet`, the package manager, and `packages/http`
(a real package, structured to be `facet`-installable rather than
bundled into every program) for a minimal HTTP server and client built
entirely on top of the I/O primitives below.

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

## Multiple assignment

```ruby
def min_max(values)
  [values.min(), values.max()]
end

lowest, highest = min_max([3, 1, 4, 1, 5])
```

`t1, t2, ... = expr` unpacks a single `Array`-valued expression across
several targets in one statement — a natural fit for this codebase's own
convention of returning `[a, b, ...]` from a function that logically has
more than one result. Targets can be plain locals, `@ivar`s, or `@@cvar`s
(freely mixed), and behave exactly like their single-target counterparts
otherwise — a target that already exists is reassigned, not redeclared;
a new local is defined; a captured local is written through its cell.

The right-hand side must actually be an `Array` (a `Hash`, for instance,
raises `TypeError` rather than being silently — and almost certainly
wrongly — read by integer key) and its length must exactly match the
number of targets, or it raises `ArgumentError`. This is stricter than
Ruby's own lenient multiple assignment (which pads missing targets with
`nil` and silently drops extra values) — deliberately, to match this
language's existing preference for a clear, immediate error over quietly
doing something the call site probably didn't intend.

Not supported (yet): indexed (`arr[i]`) or chained (`obj.field`) targets,
a comma-separated *literal* right-hand side (`a, b = 1, 2` — write `a, b
= [1, 2]` instead), and nested destructuring.

## Numbers

```ruby
x = 3
y = 2.5
x + y        # => 5.5, Int auto-promotes to Float
x == 5       # => true
-y           # => -2.5
to_f(x)      # => 3.0
to_i(y)      # => 2
```

`Int` has no fixed size limit: arithmetic that overflows 64 bits
transparently promotes to an arbitrary-precision representation, the same
auto-promoting behavior as Ruby, Python, or Lisp. `Int` *literals* in source
still cap at 64-bit, though — only runtime arithmetic overflow promotes, not
literal syntax. `Float` is an IEEE-754 double. Float
literals need a digit on both sides of the `.` (`2.5`, not `.5` or `2.`),
so `5.abs()` still parses as a method call rather than a float literal.
Both accept `_` digit separators (`1_234.567_8`). Exponent notation
(`1e10`, `1.5e-3`, `2E+7`) is also accepted, with or without a `.`
fraction, and always produces a `Float`; an `e`/`E` not followed by a
valid exponent (no digits, e.g. `5e`) is left for the next token
rather than erroring, the same "peek before committing" rule the `.`
fraction already uses.

Mixing `Int` and `Float` in arithmetic or comparisons auto-promotes the
`Int` side to `Float` — `3 + 2.5` and `3 == 3.0` both behave as you'd
expect from Ruby or Python. Division by `0.0` follows IEEE-754 rather
than raising: `1.0 / 0.0` is `Infinity`, `-1.0 / 0.0` is `-Infinity`,
`0.0 / 0.0` is `NaN`, and `NaN` compares unequal to everything, including
itself. Integer division by zero still raises a rescuable
`ZeroDivisionError`. Integer overflow (including negating the smallest
representable 64-bit `Int`) no longer raises — it promotes to an
arbitrary-precision `Int` instead, transparently; `is Int` and arithmetic
both keep working the same way on a promoted value as on any other `Int`.

`to_f`/`to_i` convert explicitly between the two. `to_i` rejects `NaN` and
`Infinity` with a rescuable `RangeError` (there's no finite integer to
convert to), but a finite `Float` outside 64-bit range now promotes to an
arbitrary-precision `Int` rather than raising, consistent with arithmetic
overflow. Type annotations stay strict even though arithmetic
auto-promotes: a parameter declared `x: Float` rejects an `Int` argument
outright — pass it through `to_f` first.

Floats print with a forced `.0` when they'd otherwise look like an
integer (`3.0`, not `3`), so they stay visually distinct from `Int` in
`puts` output and string interpolation. Formatting always finds the
shortest decimal that round-trips back to the exact same value (an
iterative `%g`-precision search against `strtod`, not a fixed digit
count), so every `Float` prints exactly and unambiguously — including
values that need the full 17 significant digits a `double` can carry,
which a naive fixed-precision format can silently get wrong.

## Symbols

```ruby
status = :ok
status == :ok        # => true
status == "ok"        # => false, different kind entirely
to_sym("ok") == status # => true
```

`:name` is a `Symbol` literal — a small, immutable, name-like value distinct
from `String`, useful for enum-like constants and Hash keys. `Symbol`
equality and hashing are by byte content, exactly like `String` (`:ok ==
:ok` is `true` even across two separately-created `:ok` literals) — unlike
Ruby, where `Symbol` is interned and pointer-equal. Diamond deliberately
scoped Symbol this way: interning would mean every Symbol ever created lives
for the rest of the process (Ruby's own tradeoff), and this codebase defers
that kind of complexity until profiling shows it's actually worth it (see
`docs/design.md`'s note on why NaN-boxing is likewise deferred).

`to_sym(string)` converts a `String` to a `Symbol`; the reverse direction
goes through `puts`/string interpolation/a class's `to_s` method, all of
which print a Symbol as its bare name with **no** leading colon — matching
Ruby's `to_s`/`puts` convention (Ruby's colon only shows via `inspect`/`p`,
which Diamond has no equivalent of). This keeps `to_sym` and printing true
inverses of each other: `to_sym("#{:ok}") == :ok`. The practical tradeoff is
that a Symbol and a same-named String print identically — Ruby has this
same tradeoff for the same reason.

A colon starts a Symbol literal only when it isn't immediately glued (no
space) onto the end of a preceding identifier, digit, or closing
`)`/`]`/`}`/`"` — so a parameter type annotation (`x:Int`), a hash-literal
separator (`{"a":b}`), and a `rescue e:Type` binding all keep meaning
exactly what they meant before Symbols existed, even written with no space.

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
its declared position and reorders at compile time, so there's no runtime
cost or new bytecode involved.

Keyword arguments can't skip over an earlier defaulted parameter to reach
a later one: `def f(a, b = 2, c = 3) ... end` called as `f(1, c: 5)` is a
compile error (`missing argument`) — you'd need to also pass `b`. Default
values are compiled inline into the function's own body, conditioned on
how many arguments were actually supplied counting from the start, not on
which specific ones were; keyword arguments let you name and reorder the
arguments you *do* supply, not skip an arbitrary one in the middle.

This only works for direct calls to a top-level `def` — a name the
compiler can resolve to one specific function at the call site. Method
calls (`obj.foo(x: 1)`), calls through a closure value, module singleton
calls, and `ClassName.new(x: 1)` constructor calls all stay
positional-only for now, since none of those resolve to one fixed target
at compile time.

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

### Class variables

```ruby
class Counter
  def self.reset()
    @@count = 0
  end
  def initialize()
    @@count = @@count + 1
  end
  def self.count()
    @@count
  end
end

Counter.reset()
Counter.new()
Counter.new()
Counter.count()  # => 2
```

`@@cvar` is ordinary mutable storage shared by every instance method,
`def self.` method, and `initialize` call in a class — a plain per-class
slot, not a per-instance field. Reading one that's never been assigned
gives `nil`, the same default an instance field gets; there's no
separate declaration step. Two different classes' own `@@x` never
collide, even with the same name — each class gets its own slot. Unlike
instance fields, `@@cvar[index] = value` (indexed assignment into the
variable itself) isn't supported yet — read the value out, mutate the
local, and assign the whole thing back, the same workaround `@ivar`
needs today for the same reason.

`@@cvar` used anywhere outside a class body (a bare top-level `def`, a
`module`) is a compile error — there's no implicit global scope it could
fall back to. That's also why a `threads: N` server (see
`packages/gremlin/README.md`) can't use a class variable to share state
across `Thread.new`-spawned workers: each spawned thread gets its own
completely independent copy of every class variable, the same way it
gets its own heap and its own everything else (see `docs/threads.md`) —
consistent, not a special case, but worth knowing going in if the goal
is one counter shared across all workers rather than one counter per
worker.

## Operator overloading

```ruby
class Vector
  def initialize(x, y)
    @x = x
    @y = y
  end
  def +(other)
    Vector.new(@x + other.x(), @y + other.y())
  end
  def negate()
    Vector.new(-@x, -@y)
  end
  def ==(other)
    other is Vector && @x == other.x() && @y == other.y()
  end
  def x() = @x
  def y() = @y
end

Vector.new(1, 2) + Vector.new(3, 4)  # => Vector(4, 6)
-Vector.new(1, 2)                    # => Vector(-1, -2)
```

A class can define `+`, `-`, `*`, `/`, `==`, `<`, `<=`, `>`, `>=` as
ordinary instance methods, and Diamond's own operator syntax (`a + b`, `a ==
b`, ...) dispatches to them — same mechanism as any other method (inherited,
overridable, reachable through `super`, and satisfies an `interface` that
requires a method of the same name). Unary minus (`-x`) is a method named
`negate`, not Ruby's `-@` spelling — a plain identifier, since `-` alone
already names the binary form and Diamond doesn't need new lexer syntax to
tell them apart by name (they're already told apart by arity: `negate`
takes no extra arguments, `-` takes exactly one).

Dispatch is receiver-based only, same as every other method call in
Diamond: `a + b` checks whether `a` is an instance whose class defines `+`.
There's no coercion protocol — if `a` is a plain `Int`/`Float`/`String` and
`b` is an instance, `a + b` still raises `TypeError`; the instance has to be
on the left. `!=` isn't separately overloadable — it's always the negation
of `==`'s result (run through the same truthiness `if`/`while` use, not
required to be exactly `Bool`), matching Ruby's own default. Two instances
of a class that doesn't define `==` still compare by identity, exactly as
before this feature existed — defining `==` only changes behavior for
classes that opt in.

`[]`/`[]=` indexing, `<<`, and `<=>` (a single method deriving all four
comparisons, Ruby's `Comparable` convenience) aren't overloadable — define
`<`/`<=`/`>`/`>=` individually if a class needs ordering.

This mechanism (a user-defined class's own instance methods) is the
only way *user code* opts into operator support. `Time`
(`docs/io.md`) is the one **native**, non-`Instance` type with real
`+`/`-`/comparison support — implemented directly in the VM's own
arithmetic/comparison opcodes, not through this dispatch, since a
native type has no instance methods for it to reach. No other native
type (`Array`, `Hash`, `File`, `SQLite3`, ...) gets operators this
way; each would need its own dedicated VM-level support, same as
`Time` did.

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

## Collections

`[1, 2]` for arrays and `{"key": value}` for hashes, both with optional
type parameters, and both can be split across lines — a newline is
allowed right after the opening bracket, right after each `,`, and
right before the closing bracket, the same as every other
bracket-delimited, comma-separated list in the language (call
arguments, `def` parameter declarations, generic type-argument lists,
and more):

```ruby
config = {
  "name": "myapp",
  "values": [
    1,
    2,
  ],
}
```

`.push`, `.pop`, and `.length()` are native on both. Array additionally
has `.join(separator = "")`, native and O(n) total (a `StringBuilder`-
backed accumulator internally, not repeated string concatenation) —
stringifies each element (the same formatting string interpolation
uses, including calling a user-defined `to_s` override) and joins them
with `separator` between (not trailing).
Strings support `.length()`, `.index_of(needle)` (position or `nil`),
`.slice(start, length)`, `.to_i()`/`.to_f()` (lenient decimal parsing —
`.to_f()` additionally accepts exponent notation like `"1e3"` even
though Diamond's own float literals don't, and overflows to `Infinity`
rather than raising, unlike `.to_i()`; neither skips leading
whitespace, and `"nan"`/`"inf"` parse as `0.0`, matching Ruby's
`String#to_f`), and `.downcase()`/`.upcase()` (ASCII-only case
conversion), and
`.reverse()` (byte-level, not UTF-8-grapheme-aware — consistent with
the rest of the language having no other Unicode awareness), and
`.strip()` (trims leading/trailing ASCII whitespace). `.split(separator)`
returns an `Array` of every piece around non-overlapping occurrences of
`separator` (an empty `separator` splits into one-character strings);
unlike Ruby, it keeps every piece including empty ones from consecutive
or leading/trailing separators (no trailing-empty suppression) — a
deliberate simplification, not an attempt at Ruby compatibility.
`.ord()` returns the first byte's value as an `Int`; an empty String
raises a rescuable `IndexError`. `chr(code)` is its inverse — a global
function (not receiver syntax; `Int` has no per-value method dispatch)
returning a one-character `String`, recognized the same way `gets()`
is (shadowable by a local or top-level function). `code` outside
`0..255` raises a rescuable `RangeError`.

Strings also support `[]` with a single `Int` index, returning a new
one-character `String` (bounds-checked, `IndexError` outside the
string — the same as `.slice()`); unlike Array/Hash, `[]=` on a String
is rejected outright with a `TypeError` (strings are immutable).
`.repeat(n)` returns a new `String` with the receiver repeated `n`
times (`n == 0` → `""`); a negative `n` raises a rescuable `RangeError`.
This is deliberately a method, not `*` — `"x" * 3` isn't supported,
since making the `*` operator polymorphic over String would need a
new deoptimization mechanism for the compiler's Int-only fast path
(`MULTIPLY_INT`) that doesn't otherwise exist for it.

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
`array_join(values, separator = "")` is a thin wrapper around the
native `.join()` above, kept for existing callers that prefer the
free-function spelling; `array_delete_at(values, index)` mutates
`values` in place (like the native `.push`/`.pop`), removing and
returning the element at `index`, or `nil` without mutating if `index`
is out of bounds; `hash_merge(a, b)` returns a new `Hash` with `a`'s
pairs then `b`'s applied on top (`b` wins on key conflicts).

`Int`/`Float` have no per-value method dispatch (both are scalar
`DiamondValue`s, not heap objects), so numeric helpers are plain
functions: `abs(x)`/`min(a, b)`/`max(a, b)`/`mod(a, b)`, all accepting
`Int | Float` (mixed `Int`/`Float` arguments auto-promote, same as
arithmetic). `abs` inherits negation's overflow behavior, so `abs` of the
most negative 64-bit `Int` promotes to an arbitrary-precision `Int`
instead of raising or silently wrapping — there is no longer a most
negative `Int` that `abs` can't represent. `mod(a, b)` truncates `a / b` toward
zero before multiplying back (via `to_i`/`to_f` for the `Float` case,
since `/` between two `Float`s doesn't truncate the way `Int`
division does), so its result keeps the same C-style sign convention
for both types — `mod(-7, 3)` is `-1`, not `2` (not Euclidean/
Python-style mod), and `mod(-7.0, 3.0)` is likewise `-1.0`. `b == 0`
raises the same `ZeroDivisionError` integer division would; `b == 0.0`
raises a rescuable `RangeError` instead (from `to_i` rejecting the
resulting `Infinity`/`NaN` quotient — `Float` division by zero itself
never raises, only the truncation step does).

`sqrt(x)`, `sin(x)`, `cos(x)`, `tan(x)`, and `pow(base, exponent)` are
native functions (no bytecode primitive to build on, same reasoning as
`chr`/`to_f`/`to_i`) accepting `Int | Float` for every argument and
always returning `Float` — `pow(2, 10)` is `1024.0`, not `1024`, even
though both arguments are `Int`. No extra validation: results follow
IEEE-754 directly, so `sqrt(-1.0)` is `NaN` rather than an error, the
same philosophy `Float` arithmetic already uses throughout.

`Time.monotonic()` returns a `Float` number of seconds from
`CLOCK_MONOTONIC` — a duration-only clock: the value itself means
nothing (not a calendar timestamp, not comparable across processes),
only the difference between two readings does, e.g. `elapsed =
Time.monotonic() - start` for timing a request in a rack middleware.
Diamond has no wall-clock/calendar `Time` type yet — no `.year`/
`.to_s`/parsing — this is deliberately just enough to measure an
elapsed duration, not a step toward one.

`array_sort(values: Array[Int])` returns a new sorted array (input
untouched); `Int` is the only type with a native ordering comparison,
so this is Int-only, checked up front (`expected Array[Int], got
Array` on a non-Int element) rather than failing confusingly mid-sort.

`StringBuilder` (defined in the prelude, `lib/core.di`) is the named
escape hatch from `result = result + piece` in a loop — quadratic,
since it reallocates and copies the whole accumulated string on every
iteration:

```ruby
sb = StringBuilder.new()
sb.append("hello")
sb.append(", world")
sb.to_s()        # => "hello, world"
"#{sb}"           # same, via to_s
sb.length()       # => 12
```

`#append` returns `self`, so calls chain: `sb.append("a").append("b")`.
It's a plain Diamond class, not a native object — `#append` pushes
`"#{piece}"` onto an internal `Array` (already O(1) amortized) and
`#to_s` calls the native `.join("")` above once, so the total cost of
building a string this way is O(n), the same complexity `Array#join`
already has for a pre-collected array of pieces.

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
identically on both.

## Regexp

```ruby
re = Regexp.new("(\\d+)-(\\d+)")
m = re.match("id:42-99")   # => ["42-99", "42", "99"]
m[0]                        # full match
m[1]                        # first capture group

re.match?("id:42-99")       # => true, no captures allocated
re.match("no digits")       # => nil, no match

Regexp.new("foo", 1)        # 1 = case-insensitive (see options below)
```

Backed by `reginold`, a companion regex engine — not part of this repo,
built separately and linked in from a sibling checkout at `../reginold`
— compiled under Ruby regex syntax. `Regexp.new(pattern, options = 0)` —
the options argument is a plain `Int` bitmask: `1` = ignore case, `2` = `.`
matches newline, `4` = extended (whitespace and `#` comments ignored in
the pattern). Diamond has no bitwise-OR operator, so combine flags by
adding them (they're disjoint bits — addition and OR coincide): `3` for
case-insensitive *and* dot-matches-newline together.

`.match(string)` returns an `Array` — index `0` is the full match, indices
`1..` are capture groups in order, `nil` at any index for an unmatched
optional group (`(a)|(b)` matched against `"b"` gives
`["b", nil, "b"]`) — or `nil` if the pattern didn't match at all.
`.match?(string)` is the same search without allocating capture data, for
a plain yes/no check. An invalid pattern raises a rescuable `RegexpError`
at `Regexp.new` time.

This is deliberately a small first cut: no `/pattern/` literal syntax yet
(`/` already means division; telling a leading regex apart from division
needs the same kind of disambiguation Symbol's `:` got, not yet done for
`/`), no `String` integration (`"x".match(re)`, `=~`, `.split`/`.gsub`
taking a `Regexp`), and no richer `MatchData` object (`pre_match`, named
captures) — the plain-`Array` result covers the common case; `Regexp.new`
+ `.match`/`.match?` is the whole surface for now.

## No AST

The compiler is a single-pass Pratt parser that emits register bytecode
directly; there is no retained AST. Pass `--dump-bytecode` on the CLI to
see how any construct in this document actually lowers.
