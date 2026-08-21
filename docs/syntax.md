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

## Case/when

```ruby
def classify(n)
  case n
  when 0
    "zero"
  when 1, 2, 3
    "small"
  else
    "big"
  end
end
classify(2)   # => "small"
```

`case SUBJECT` tests `SUBJECT == value` against each `when`'s value(s) in
turn, an expression like `if` — the matched branch's last line (or the
`else` branch, or `nil` if nothing matches and there's no `else`) is the
whole expression's value. A `when` can list several comma-separated
values (`when 1, 2, 3`); matching short-circuits left to right, so a
later value in the list is never even evaluated once an earlier one in
the same `when` already matched. `when VALUE then BODY` works the same
as `if COND then BODY`, for a one-line branch.

Plain `==` only — not Ruby's `===`, so `when 1..5`/`when String`/`when
/regex/` all just compare the subject against that Range/Class/Regexp
*value* with `==` rather than testing membership, which is almost never
what's wanted. A `===`-based dispatch (so `case`/`when` can pattern-match
against a `Range`, class, or `Regexp`) is a deliberate v1 scope cut,
worth its own follow-up once there's more than one type that would use
it. There's also no subject-less boolean form (Ruby's `case` with no
expression, where each `when`'s own value is tested for truthiness
instead of compared against a subject) — `case` always requires a
subject in Diamond today.

## Ternary

```ruby
x = 5
x > 3 ? "big" : "small"                   # => "big"
x == 1 ? "one" : x == 2 ? "two" : "other" # => "other", right-associative
```

`cond ? a : b` binds looser than every binary operator (including
`..`/`&&`/`||`) but tighter than assignment, matching Ruby's own
precedence — `x = a ? b : c` reads as `x = (a ? b : c)`, and `true ||
false ? "t" : "f"` reads as `(true || false) ? "t" : "f"`. Nested
ternaries in the false branch (as above) right-associate the way Ruby's
own does. Both branches are plain expressions, not statement blocks —
same as everywhere else Diamond takes an expression, this rules out
`x = 5` as a branch, but not another `?:`, a method call, or a literal.

Line continuation follows the same "trailing operator" convention every
other multi-line expression in Diamond uses — put the `?`/`:` at the end
of the line, not the start:
```ruby
x > 3 ?
  "big" :
  "small"
```

## Compound assignment

```ruby
x = 10
x += 5    # 15
x -= 3    # 12
x *= 2    # 24
x /= 4    # 6
x %= 5    # 1

count = nil
count ||= 0    # count was nil, so this assigns: count is now 0
count ||= 99   # count is already 0 (truthy-or-falsy check, not nil check
               # -- see below), so this is a no-op: count stays 0

flag = true
flag &&= check_something()  # only evaluates/assigns if flag is truthy
```

`+=`/`-=`/`*=`/`/=`/`%=` are pure sugar for `x = x <op> y` — same
semantics, same runtime errors (`x /= 0` raises `ZeroDivisionError` exactly
like plain `/` does), same operator-overload dispatch for an `Instance`
target. `||=`/`&&=` are genuinely short-circuit, not sugar for `x = x ||
y`/`x = x && y` evaluated unconditionally: the right-hand side is only
evaluated (and the assignment only happens) when the existing value
doesn't already decide the outcome — `x ||= y` skips `y` entirely when `x`
is already truthy, `x &&= y` skips `y` entirely when `x` is already falsy.
Truthy/falsy here means Diamond's ordinary truthiness (only `nil` and
`false` are falsy), not specifically "is nil" — `count ||= 99` above is a
no-op once `count` is `0`, since `0` is truthy, the same way plain `if
count` would treat it.

The target can be a plain local, an `@ivar`, or a `@@cvar` — same three
targets plain `=` and multiple assignment accept. Indexed targets
(`arr[i] += 1`, `hash[k] ||= default`) aren't supported yet; write the
indexed read and assignment out separately.

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

`%` is *floored* modulo, matching Ruby (not C's truncating `%`): the
result always takes the divisor's sign, not the dividend's — `-7 % 3` is
`2`, not `-1`. Works on `Int`/`Float` and any mix of the two (same
promotion rule as the other arithmetic operators); `Int % 0` raises
`ZeroDivisionError`, `Float % 0.0` is `NaN` like IEEE-754 division above.
User-overloadable like `+`/`-`/`*`/`/` (see "Operator overloading"
below) — unlike bignums, which `%` doesn't support at all yet, a
deliberate v1 scope cut.

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

`Int` has three block-consuming iteration methods, dispatched natively
straight to ordinary prelude functions rather than through any class —
`Int` isn't a class and can't be reopened:

```ruby
5.times() do |i|
  puts(i)          # 0, 1, 2, 3, 4
end
1.upto(5) do |i|
  puts(i)          # 1, 2, 3, 4, 5
end
5.downto(1) do |i|
  puts(i)          # 5, 4, 3, 2, 1
end
```

Each returns the receiver (matching Ruby), not the block's own result or
a collected array — the loop itself is the point.

## Ranges

```ruby
(1..5).first()         # => 1
(1..5).last()          # => 5
(1..5).include?(5)     # => true
(1...5).include?(5)    # => false, exclusive of the end
(1..5).length()        # => 5
(1...5).length()       # => 4
(5..1).length()        # => 0, reversed ranges are just empty

sum = 0
def add(x)
  sum = sum + x
end
(1..5).each(add)       # sum == 15
```

`1..5` (inclusive of `5`) and `1...5` (exclusive of `5`) both desugar
directly at parse time into `Range.new(1, 5, false)` /
`Range.new(1, 5, true)` — `Range` is a plain class in `lib/core.di`, the
same "not a native object" precedent `StringBuilder` already establishes,
not a new VM value kind. No new bytecode opcode exists for it either:
the desugaring reuses the exact same `NEW` instruction sequence
`ClassName.new(...)` already produces.

`..`/`...` bind looser than every other binary operator, including
`&&`/`||`, matching Ruby's own precedence table: `1..n+1` reads as
`1..(n+1)`, and `a > 0 .. b < 10` reads as `(a>0)..(b<10)`.

`Range` includes `Enumerable`, so `.select`/`.count`/`.any?`/`.all?`/
`.map`/`.reduce` and the rest of `Enumerable`'s methods all work on a range
the same way they do on any other `Enumerable`-including class (see
"Collections" below).

Scope, deliberately: `Range` is `Int`-only for v1 (`start`/`end` must
both be `Int`) — constructing one with `Float` or any other type raises
a `TypeError`, the same cut `array_sort` already makes elsewhere.
Range-based indexing/slicing (`arr[1..3]`, `hash[range]`) isn't
supported yet either — indexing still expects a plain `Int`, so passing
a `Range` there raises a `TypeError` rather than slicing; that's a
separate, larger change to `DIAMOND_OP_INDEX_GET`'s own dispatch, not
part of `Range` itself.

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

Not supported (both explicitly out of scope, not just unimplemented):
calling a closure *value* with a trailing block
(`some_callable(args) do ... end`) and `ClassName.new(args) do ... end`
constructor blocks. Ruby's `Thing.new { |t| ... }` idiom usually relies
on `initialize` yielding `self`, which Diamond has no equivalent of — a
block passed to `.new` would just be one more constructor argument, not
obviously useful without a declared parameter to bind it to.

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
collide, even with the same name — each class gets its own slot.
`@@cvar[index] = value` and `@ivar[index] = value` (indexed assignment
into the variable itself, as opposed to reassigning the whole variable)
both work: `@current[key] = value` loads `@current`'s existing Hash/Array
into a register and mutates it in place through `DIAMOND_OP_INDEX_SET`,
the same way indexing a plain local already did — there's no extra
"write the mutated value back to the ivar/cvar" step needed, since
Hash/Array are heap-allocated reference values in the first place.

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

### tap / dup / respond_to?

```ruby
class Point
  def initialize(x, y)
    @x = x
    @y = y
  end
  def x() = @x
  def y() = @y
end

p1 = Point.new(1, 2)
p2 = p1.dup()          # a distinct instance, same field values
p1.respond_to?(:x)     # => true
p1.respond_to?(:zoom)  # => false

[1, 2, 3].tap() do |arr|
  puts(arr.length())   # side effect, doesn't change the chain
end.push(4)             # => [1, 2, 3, 4]
```

`tap` yields the receiver to a block and returns the receiver itself
(not the block's own result) — works on any receiver, native or
user-defined, primitives included (`5.tap() do |x| ... end` is valid).

`dup` returns a shallow copy: a distinct `Array`/`Hash`/`Instance` (same
elements/fields, independently mutable afterward), or the same value
back unchanged for anything already immutable (`Int`, `String`, `Symbol`,
and every other primitive). Only defined for `Array`, `Hash`, `Instance`,
and primitives — native resource-backed types (`Regexp`, `Time`, `File`,
`Socket`, ...) don't support it, since "shallow copy" isn't a
well-defined operation for those. A class that defines its own `dup`
always wins over this default.

`respond_to?(name)` takes a `Symbol` and checks whether the receiver's
class defines a method by that name — `false` for a private method, same
as Ruby's own default (no `include_private` second argument yet). Only
defined for `Instance` receivers; calling it on a native type
(`Array`/`Int`/`String`/...) is currently an `undefined method` error
rather than an approximate answer — accurately enumerating every method a
native type actually supports isn't tracked anywhere as one real list.

Like `tap`, a class's own method of the same name always takes priority
over `dup`/`respond_to?`'s own built-in behavior, checked first.

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

A class can define `+`, `-`, `*`, `/`, `%`, `==`, `<`, `<=`, `>`, `>=` as
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

`[]`/`[]=` indexing and `<<` aren't overloadable. `<=>` **is** — see its
own section below.

This mechanism (a user-defined class's own instance methods) is the
only way *user code* opts into operator support. `Time`
(`docs/io.md`) is the one **native**, non-`Instance` type with real
`+`/`-`/comparison support — implemented directly in the VM's own
arithmetic/comparison opcodes, not through this dispatch, since a
native type has no instance methods for it to reach. `<<` is native the
same way, on two types: `Int << Int` is a bitwise left shift (shift
amount must be `0..63`, else `RangeError`); `Array << value` pushes
`value` and returns the array itself, the same as `Array#push`, letting
pushes chain (`arr << 1 << 2 << 3`). `<<` binds tighter than comparisons
but looser than `+`/`-` (`1 + 2 << 3` is `(1 + 2) << 3`, matching Ruby).
Any other left operand (a `String`, an `Instance`, ...) is a `TypeError`
— no other native type (`Hash`, `File`, `SQLite3`, ...) gets operators
this way; each would need its own dedicated VM-level support, same as
`Time` and `<<` did.

### `<=>` and `Comparable`

```ruby
class Box
  include Comparable
  def initialize(size)
    @size = size
  end
  def <=>(other) = @size - other.size()
  def size() = @size
end

Box.new(1) < Box.new(2)                    # => true
Box.new(5).between?(Box.new(1), Box.new(10))  # => true
Box.new(15).clamp(Box.new(1), Box.new(10)).size()  # => 10
```

`a <=> b` returns `-1`/`0`/`1` (an `Int`), or `Nil` for a pair with no
defined ordering — never a raised error on its own, unlike every other
comparison operator. Built in for `Int`/`Float` (including
arbitrary-precision `Int`s and mixed `Int`/`Float` operands; `NaN` on
either side is `Nil`, matching `Float::NAN <=> 1` in Ruby) and for any
`Instance` whose class defines its own `<=>` method — anything else
(`String`, `Time`, an `Instance` with no `<=>`, ...) is `Nil` too, not
`TypeError`.

`include Comparable` derives `<`, `<=`, `>`, `>=`, `==`, `between?`, and
`clamp` from that one `<=>` method — the same "several methods derived
from one" relationship `Enumerable` has with `each`. A genuinely
incomparable pair still surfaces as an error eventually (`(self <=>
other) < 0` becomes `nil < 0`), just one level removed from `<=>`
itself.

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

Array's Enumerable-style methods: `.each`/`.select`/`.map`/`.reduce`/
`.count`/`.any?`/`.all?` (shared with Hash, driven by `.each`),
`.sort`/`.sort_by`/`.min`/`.max`/`.min_by`/`.max_by`/`.reject`/`.find`/
`.each_with_index`/`.sum`, and `.take(n)`/`.drop(n)`/`.flat_map`/
`.partition`/`.group_by`/`.zip(other)`/`.each_slice(n)`/`.each_cons(n)`/
`.tally`. The last group returns its result directly rather than
through a block or an Enumerator (Diamond has neither) —
`.each_slice`/`.each_cons` collect every slice/window into an
`Array[Array]` up front, `.partition` returns `[matching,
non_matching]`, `.group_by` a `Hash` keyed on the block's own result,
`.tally` a `Hash` of element to occurrence count, and `.zip` pads the
shorter array with `Nil` out to the *receiver's* own length (`[1, 2,
3].zip([4, 5])` => `[[1, 4], [2, 5], [3, nil]]`), matching Ruby. None of
these are defined on `Hash` — a deliberate, narrower scope than Ruby's
own Enumerable, matching the existing asymmetry `.min`/`.max`/`.sort`
already have (Array-only, not Hash).
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

`.format(values)` is a `sprintf`-style formatter — `values` is either a
single value or an `Array` of them (matching Ruby's `String#%`, without
needing variadic/splat call support Diamond doesn't have):

```ruby
"Name: %s, Age: %d".format(["Alice", 30])  # => "Name: Alice, Age: 30"
"%05d".format(42)                          # => "00042"
"%-10s|".format("hi")                      # => "hi        |"
"%.2f".format(3.14159)                     # => "3.14"
```

Directives: `%d`/`%i` (`Int` or `Float`, truncated), `%f` (`Float` or
`Int`, default 6 decimal places, `%.Nf` for `N`), `%x`/`%X`/`%o`/`%b`
(`Int`, hex/octal/binary — `%b` has no C `printf` equivalent, hand-
rolled), `%s` (any value, via the same formatting string interpolation
uses, including a user-defined `to_s` override), and `%%` for a literal
`%`. `-` left-justifies and `0` zero-pads within a numeric width prefix
(`%-10s`, `%05d`); `.N` sets `%f`'s precision. Every directive's
argument type is checked against what that directive actually needs
(`TypeError` on a mismatch); too few arguments raises `ArgumentError`,
extra arguments are silently ignored. Each directive is handled by
building a small, internally-chosen conversion string from the parsed
flags/width/precision and handing it to a real `snprintf` alongside
exactly one correctly-typed value — never your format string forwarded
into a C varargs call directly, which would be a real format-string
vulnerability given a Diamond value's runtime type has no fixed
relationship to what a positionally-matched C conversion expects.

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

Any user-defined class gets those same six methods, plus `to_a`, `sort`,
`sort_by`, `min`, `max`, `min_by`, `max_by`, `reject`, `find`,
`each_with_index`, `sum`, `take`, `drop`, `flat_map`, `partition`,
`group_by`, `zip`, `each_slice`, `each_cons`, and `tally`, for free by
implementing its own `each(callback)` and `include`-ing `Enumerable`. The
first six forward straight to prelude functions driven by `self.each(...)`,
so they're generic by construction; the rest reuse Array's own existing,
already-tested index-based implementations by materializing the receiver
into an Array via `to_a` first (itself built on `each`) and delegating to
those, rather than re-deriving each one generically over `each()`.

A handful of further Array/Hash conveniences work as receiver syntax too,
forwarding the same way the Enumerable set above does:
`values.reverse()` returns a new array in reverse order;
`values.concat(other)` returns a new array with `other`'s elements
appended; `values.compact()` returns a new array with any `nil` elements
dropped; `values.uniq()` returns a new array with only the first
occurrence of each distinct (`==`) element, order preserved;
`values.flatten()` returns a new array with nested arrays fully
flattened (recursively, matching Ruby's default);
`values.delete_at(index)` mutates `values` in place (like the native
`.push`/`.pop`), removing and returning the element at `index`, or `nil`
without mutating if `index` is out of bounds; `hash.merge(other)` returns
a new `Hash` with the receiver's pairs then `other`'s applied on top
(`other` wins on key conflicts). Like the rest of the Enumerable set,
`vm.c`'s native dispatch resolves each of these receiver calls to a
same-named top-level prelude function (`array_reverse`, `array_concat`,
...) at runtime, so that free-function spelling still exists underneath
and can't be removed without a native-dispatch rework -- receiver syntax
is simply the only spelling documented and used going forward.
`array_join(values, separator = "")` was the one exception: a thin
wrapper *around* the already-native `.join()` above rather than
`.join()`'s own implementation, with no dispatch dependency on its name,
so it has been removed now that `values.join(separator)` is the only
spelling.

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

`ARGV` and `ENV` are plain global values, not calls — `ARGV` is an
`Array` of `String`s, the script's own trailing command-line arguments
(`diamond script.di one two` → `ARGV == ["one", "two"]`; `[]` for `-e`/a
file run with no trailing args, or from the REPL). `ENV` is a `Hash` of
`String` to `String`, a snapshot of the process environment taken at
startup — `ENV["PATH"]`, `ENV["HOME"]`, etc.; a missing key is `nil`,
same as any other `Hash`. Mutating the `ENV` `Hash` only changes that
in-memory snapshot, not the real environment (no `setenv` round-trip) —
read-only in effect, even though nothing stops the write syntax itself.
Like every other built-in name, a local variable or user-defined
function named `ARGV`/`ENV` shadows it.

## Debugging

```ruby
def compute(x)
  y = x * 2
  debugger()   # breakpoint() is the same thing, either name works
  y + 1
end
compute(5)
```

```
--- paused at compute:3:3 ---
locals:
  x = 5
  y = 10
(press Enter to continue)
```

`debugger()`/`breakpoint()` pauses execution, prints where it was called
from and every currently-live local (name and value — parameters count
as locals too), then waits for one line of input on stdin before
resuming normally. **Read-only inspection, not a live REPL**: there is no
way to evaluate a new expression or reassign a local from the pause — it
prints what's already there and continues, deliberately scoped short of
a Ruby `binding.pry`/`debug`-style interactive session. Stdin at EOF
(closed, redirected from `/dev/null` — the ordinary case under a non-
interactive script or test run) continues immediately rather than
hanging, so it's always safe to leave a `debugger()` call in code that
might run non-interactively.

Like `puts`/`gets`/`Time`/every other built-in name, a local variable or
a user-defined function named `debugger`/`breakpoint` shadows it —
`def debugger(); ...; end` makes `debugger()` call that instead, never
the built-in.

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

Backed by `reginold`, a companion regex engine vendored in-repo under
`reginold/` and built as a static archive — compiled under Ruby regex
syntax. `Regexp.new(pattern, options = 0)` —
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
