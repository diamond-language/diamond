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

`case SUBJECT` matches each `when` pattern against the subject in turn, an
expression like `if` — the matched branch's last line (or the
`else` branch, or `nil` if nothing matches and there's no `else`) is the
whole expression's value. A `when` can list several comma-separated
values (`when 1, 2, 3`); matching short-circuits left to right, so a
later value in the list is never even evaluated once an earlier one in
the same `when` already matched. `when VALUE then BODY` works the same
as `if COND then BODY`, for a one-line branch.

Matching follows `===`-style case semantics: a `Range` tests numeric
inclusion, a `Regexp` searches a String subject, and a user class name matches
instances of that class or its subclasses. Other patterns use equality, with
the pattern as receiver, so a user instance can customize matching through
`def ==(value)`. Native type names such as `String` are not class-pattern
values because Diamond's native types are not reified classes. There is still
no subject-less boolean form (Ruby's `case` with no
expression, where each `when`'s own value is tested for truthiness
instead of compared against a subject) — `case` always requires a
subject in Diamond today.

Array patterns can match a nested shape and bind lowercase names:

```ruby
case event
when ["score", [1..100, points], Player, _]
  record(points)
else
  reject(event)
end
```

Every bracketed level must be an Array of exactly the written length unless it
contains one rest binding. A trailing `[head, *tail]` captures the remaining
elements; a middle `[head, *middle, tail]` reserves the written suffix and
captures everything between prefix and suffix. The rest binding accepts zero
or more elements and receives them as a fresh Array; `*_` accepts and discards
the same span. Only one rest binding is allowed per Array level. Nested
entries use the same literal, Range, Regexp, class/subclass, and custom-equality
matching described above. A lowercase bare name binds that element for the
branch; `_` ignores it. Prefix an existing lowercase local with `^` to compare
the element against its current value instead of rebinding it, as in
`[^expected_kind, payload]`. Pins work at any nested level and may reference a
captured local. An undefined or non-lowercase pin is a compile error. Bindings
are written only after the complete pattern
matches, so a failed later element cannot partially overwrite locals. An Array
binding pattern must be the sole pattern in its `when` clause; comma-separated
alternatives remain available for non-binding patterns.

Hash patterns require each written key while allowing additional keys:

```ruby
case event
when {"kind": ^expected_kind, "payload": {"score": score}}
  record(score)
else
  reject(event)
end
```

Keys are ordinary expressions and use normal Hash key equality. Values support
the same literals, nested Array or Hash patterns, lowercase bindings, `_`
wildcards, and `^local` pins as Array patterns. Missing keys fail the pattern
even when the requested value pattern would match `nil`. Empty `{}` matches any
Hash. A trailing `**remaining` binding receives every unmatched entry in a
fresh Hash; `**_` accepts and discards those entries without allocating a Hash.
The rest binding must be last and works at nested levels. Bindings remain
failure-atomic.

Object patterns guard by class and extract values through public readers:

```ruby
case event
when ScoreEvent{player: Player{name: name}, points: ^expected_points}
  record(name)
else
  reject(event)
end
```

The subject must be an instance of the named class or a subclass. Each written
reader must exist on that class, accept zero arguments, and be public; otherwise
the pattern is rejected at compile time. Reader results support nested object,
Array, and Hash patterns plus bindings, `_`, and `^local`. Empty `Class{}` is a
class-only guard. Bindings commit only after every reader result matches.

Any `when` pattern can carry an `if` guard:

```ruby
case event
when {"score": score, **metadata} if score > minimum && metadata.length() > 0
  accept(score, metadata)
else
  reject(event)
end
```

Collection bindings are visible inside the guard, but remain provisional.
The guard runs only after the structural pattern matches. A false guard falls
through to the next `when` without overwriting existing locals or creating new
binding values. Bindings commit immediately before the selected branch body.

Comma-separated collection patterns may serve as alternatives when every
alternative binds exactly the same set of names:

```ruby
case message
when ["score", value], {"score": value} if value > minimum
  accept(value)
else
  reject(message)
end
```

Alternatives match from left to right and stop after the first structural
match. Binding order may differ, but missing or additional binding names are a
compile error. Values from the selected alternative flow through shared
provisional registers, so one guard and one atomic commit path serve the whole
clause. A false guard does not retry later alternatives.

Omitting the `case` subject creates a boolean case:

```ruby
case
when score >= 90
  "excellent"
when score >= 70, override?
  "passing"
else
  "retry"
end
```

Each `when` expression is tested directly for truthiness. Comma-separated
expressions short-circuit from left to right, and ordinary `if` guards remain
available. Array and Hash spellings in a subjectless clause are ordinary
literals, not binding patterns; pattern bindings require a case subject.

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

## Writer-call assignment sugar

```ruby
class Author
  attr_accessor name: String
end

ada = Author.new()
ada.name = "Ada Lovelace"   # sugar for ada.name=("Ada Lovelace")
puts(ada.name())            # => "Ada Lovelace"
```

`receiver.attr = value` is pure sugar for the writer-method call
`receiver.attr=(value)` — the same call either spelling compiles to, reaching
any writer method, hand-written or `attr_accessor`-synthesized. Because it's
just a re-spelling of an ordinary method call rather than new assignment
semantics, the expression's own value is whatever the writer method actually
returns (an `attr_accessor`-generated writer returns the value it was just
given, but a hand-written one is free to return anything). The receiver can
be any expression, not just a local — `Author.new().name = "Ada"` and
`book.author().name = "Ada"` both work — and the right-hand side is one
ordinary expression, parsed exactly like plain assignment's own
right-hand side (so `attr = [1, 2, 3]` is the array literal, not an
attempt at generic arguments).

## Multiple assignment

```ruby
def min_max(values)
  [values.min(), values.max()]
end

lowest, highest = min_max([3, 1, 4, 1, 5])
[name, [x, y]] = ["origin", [0, 0]]
[head, *tail] = [1, 2, 3] # tail is [2, 3]
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

Bracketed patterns can nest to unpack structured Array results:
`[head, [left, right]] = value`. Every bracketed level must independently
be an Array of exactly the written length. Brackets are optional for the
outermost legacy form, so `head, [left, right] = value` is equivalent.
Leaves retain the same local/`@ivar`/`@@cvar` behavior at any depth.
The complete nested shape is validated before any leaf is assigned, so a
rescued inner type/length failure cannot leave earlier targets half-updated.
Any bracketed level may contain one rest target (`[head, *middle, tail]`).
It requires the fixed prefix and suffix, then assigns a fresh Array containing
the intervening elements, including an empty Array when the two fixed regions
touch. The unbracketed outer form supports the same middle-rest placement.

Hash patterns use required keys and may capture unmatched entries:
`{"name": name, "meta": {"id": id}, **remaining} = value`. The right-hand
side must be a `Hash`; each written key must be present, while extra keys are
tolerated. `**remaining` receives a fresh Hash containing only extra entries.
Nested Array and Hash patterns validate before any leaf store, so failures are
failure-atomic.

Comma-separated literal right-hand sides are supported: `a, b = 1, 2` is
equivalent to destructuring `[1, 2]`, with each expression evaluated once from
left to right. Indexed leaves are supported at any depth, including chained
indices: `[head, matrix[row][column]] = value`. Receivers and indices are
evaluated left to right, while the final indexed writes remain delayed until
the complete pattern validates. Member-writer leaves are also supported:
`[record.name, grid[row].score] = value`. Intermediate members in a target path
are invoked as zero-argument readers; the final member is invoked as its writer.
All final writer calls remain delayed until validation succeeds.

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
Range-based indexing/slicing works for `Array` (not `Hash` — real Ruby
doesn't support `Hash#[]` with a Range either, `[]` there is always plain
key lookup): `arr[1..3]` returns a fresh `Array` of the selected elements,
inclusive or exclusive matching the range's own `..`/`...`. The *start*
is bounds-checked (`arr[10..20]` on a 3-element array raises `IndexError`,
even though `start==array.length()` is valid and simply yields an empty
`Array`), but the implied *length* is silently clamped to whatever's
actually available — `arr[1..100]` is not an error. Writing a slice
(`arr[1..3] = [a, b, c]`) requires the replacement to have *exactly* as
many elements as the range covers (after the same clamping) — no
Ruby-style grow/shrink splice in this first version; a length mismatch
raises `TypeError` and leaves the array completely untouched, checked
before writing any element back.

Indexed *compound* assignment against a plain `Int` index is supported,
though: `arr[i] += 1`, `h[k] -= 1`, and the rest of `+=`/`-=`/`*=`/`/=`/
`%=`/`||=`/`&&=` all work against an Array element or Hash value, the
same as plain `arr[i] = v` — the index expression is evaluated exactly
once either way.

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
Mixed distinguishable members form unions; conflicting parameterizations of
the same outer type remain unknown. Fixed constructor calls infer `initialize` type variables
from their positional arguments in the same way.
Spreading a homogeneously typed Array supplies its element contract to generic
parameters before a trailing block is compiled. This applies to functions,
singleton methods, instance methods, and constructors.
Keyword values likewise infer generic bindings from their named parameter slot
for statically resolved functions, singleton methods, instance methods, and
constructors. Explicit generic arguments take precedence.

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
signature registry documented in `docs/design.md`.

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
- a *literal* call site can still only ever supply at most 16 argument
  expressions in total, variadic or not — a pre-existing limit on every
  call form (`"too many call arguments"`), not something specific to
  this feature. Call-site spread (below) isn't subject to it, since a
  spread argument's length is a runtime value, not one argument
  expression per element.

A variadic parameter widens `Callable[N]` matching too: a variadic
closure/function satisfies `Callable[N]` for any `N` at or above its own
required-argument count, not just an exact match — see docs/design.md's
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
sum(*(1..50).to_a())  # => 1275, no 16-argument-expression limit here
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
- native receiver spreads share ordinary native invocation's 16-argument bound;

See docs/design.md's "Call-site spread" section for the full mechanism.

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
first-class heap values, with two narrow exceptions:
`ClassName.redefine_method(name, callable)` repoints an existing method's
compiled body at runtime, and `ClassName.define_method(name, callable)`
adds a brand-new method under a name the class didn't already have. Both
take the same "patch-factory" shaped `callable` -- a nested, named
function (Diamond has no anonymous closure literal) that captures no
variables and was compiled inside the same class, typically returned
from a `def self.x_factory()` that declares the nested `def` and then
evaluates to its bare name:

```ruby
class Greeter
  def initialize(name)
    @name = name
  end
  def self.greet_factory()
    def greet()
      "hello, #{@name}"
    end
    greet
  end
end

g = Greeter.new("Ada")
Greeter.define_method("greet", Greeter.greet_factory())
g.greet()  # => "hello, Ada"
```

`define_method` fails if the name already exists (use `redefine_method`
for that) or if the callable captures a variable, isn't a method of
`ClassName` itself, or the class already has the maximum number of
methods. Unlike `redefine_method`, there's no arity to match against a
prior definition -- the new method simply takes the callable's own arity,
the same as an ordinary `def` would. The new method is visible to every
instance immediately, including ones already constructed before the
call, since dispatch looks the method up by class and name at call time
rather than snapshotting anything at construction time.

### `closure name() ... end`

A plain nested `def`, as above, is built for exactly one job: a detached
patch, meant to be handed to `define_method`/`redefine_method` and given
a real `self` later, at installation. Called directly instead -- in
place, never installed anywhere -- its `self`/`@ivar` references don't
mean anything: nothing ever supplied a receiver for them. `closure name()
... end` is the other case: an ordinary closure, called immediately, that
also closes over `self` (and therefore `@ivar` and `self.foo(...)`) the
same way it already closes over an ordinary outer local:

```ruby
class Widget
  def initialize(x)
    @x = x
  end
  def helper(n) = n * 2

  def run(value)
    closure inner()
      self.helper(value) + @x
    end
    inner()
  end
end

Widget.new(10).run(5)  # => 20
```

Legal only where `self` already exists -- inside an instance method or a
class-owned `def self.x` -- a `closure` declared at top level, or inside
a plain top-level `def`, is a compile error rather than something that
silently compiles into nonsense. `self.foo(...)` inside one dispatches
correctly whether the captured `self` is an instance or (inside a
`def self.x`, directly -- not nested any deeper, the same depth-1 limit
`redefine_method`'s own patch-factory idiom already has) a class value.

`closure`'s value is an ordinary `Callable` like any other capturing
closure -- which means it still can't cross a `Thread.new` boundary
(`Thread.new`'s own capture-free requirement, see "Threads"), and it
still can't be handed to `define_method`/`redefine_method` (both require
a capture-free callable too) -- `closure` and plain nested `def` solve
two different problems and aren't interchangeable.

One real, separate limitation `closure` inherits from plain nested `def`,
not something this feature fixes: redeclaring either one a second time
inside the same loop body raises a runtime `TypeError` at the second
declaration. See `docs/roadmap.md`'s "Open design decisions" section.

### Reopening

A second `class Name ... end` (or `module Name ... end`) for a name that
already exists adds to it -- new methods, new `@ivar`-backed fields, new
nested classes -- rather than erroring, whether the second declaration
is later in the same file or (the more common reason to want this) in a
separate file pulled in by `require`:

```ruby
class Widget
  def initialize(name)
    @name = name
  end
  def name() = @name
end

class Widget
  def rename(new_name)
    @name = new_name
  end
end

w = Widget.new("ada")
w.rename("grace")
w.name()  # => "grace"
```

Redefining a method that already exists is still a compile error
(`duplicate or excessive method definition`) -- reopening only ever
*adds*, it doesn't loosen this. A reopen's own `< Super` clause (if any)
is checked against whatever superclass the class already has (from an
earlier declaration this compile), not re-applied: omit it to leave the
existing superclass alone, restate the same one for a harmless no-op, or
name a different one to get a compile error (`superclass mismatch for
reopened class`) rather than silently changing what the class inherits
from underneath already-written code. Interfaces don't support
reopening -- a second `interface Name` is still a hard error.

### `self` inside `def self.x`

A `def self.x` method declared directly inside a class (not a module --
see below) can use `self`, and `self.method_name(...)` dispatches
virtually: it looks `method_name` up against `self`'s *actual* class,
walking that class's own superclass chain, rather than resolving to
whatever's lexically visible where the calling method happens to be
defined. This is what lets one method shared on a base class reach each
subclass's own override:

```ruby
class Model
  def self.table_name()
    "model_default"
  end
  def self.describe()
    self.table_name()
  end
end

class Author < Model
  def self.table_name()
    "authors"
  end
end

Author.describe()  # => "authors" -- describe is inherited from Model,
                    # but self.table_name() still reaches Author's own
Model.describe()    # => "model_default"
```

`self` also works as an ordinary value with no following call (it holds
the class itself, comparable with `==` and usable anywhere a value is
expected) -- but a Class value has no general-purpose literal syntax of
its own; the only way to obtain one is `self` inside a class-owned
singleton method.

Only the **explicit** `self.foo(...)` form dispatches this way. A **bare**
call to a sibling `self.` method (`table_name()` instead of
`self.table_name()`) does not -- and, as of this feature, no longer even
resolves to a sibling method at all; it's an ordinary undefined-function
error. (Before this, a bare call happened to reach a sibling class method
by accident: singleton methods shared the same "not a class member"
compile-time tag as plain top-level functions, so top-level function
lookup found them incidentally. Giving `self` a real value required
giving singleton methods a real owner, which closes that accident --
class-owned singleton methods now behave exactly like module ones always
did, where a bare sibling call was already an error. Use `self.foo(...)`
explicitly in both cases now.)

Module namespace singletons (`def self.name` inside a `module` block) are
unaffected by any of this -- `self` still isn't accessible there, and
`ModuleName.name(...)` still resolves entirely at compile time. Modules
have no superclass chain and nothing to virtually dispatch against.

### Bare singleton method references

```ruby
class AuthorsController
  def self.show(request, context, params)
    # ...
  end
end

handler = AuthorsController.show   # no call -- a Callable[3] value
handler(request, context, params)  # invoked later, elsewhere
```

`ClassName.method`/`ModuleName.method`, with no `(...)` following, is a
`Callable` value referencing that singleton method -- a small, zero-capture
wrapper matching the method's required/optional and variadic arity. Works for
a class `self.` method and a module singleton function alike. Variadic wrappers
collect and forward through singleton spread. Generic references require
explicit bindings at the reference site (`Tools.identity[Int]`); the wrapper
bakes those bindings into its forwarded call. A bare reference to an unbound
generic method is a compile error.

### Bound instance-method references

`receiver.method`, with no `(...)`, captures the evaluated receiver once and
returns a variadic Callable. Calling it later performs ordinary dynamic method
lookup and forwards all positional arguments, so overrides, inheritance,
visibility, `method_missing`, variadic methods, and native receivers behave as
they do at a direct call site. Explicit bindings are retained:
`converter.identity[Int]` produces a bound generic-method Callable. Bound
method Callables are capturing closures and therefore retain the ordinary
capturing-Callable restrictions for `Thread.new`.

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

### Method visibility

`public`, `protected`, and `private` change the visibility of following
methods in a class or module. They also accept existing method names, such as
`protected compare, token`. Public methods are callable everywhere. Protected
methods may use an explicit receiver only while executing an instance method
inside the declaring class hierarchy. Private methods remain restricted to
the current implicit/self receiver. `respond_to?` reports public and protected
methods, but not private methods.

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

`<<` isn't overloadable (see below for why). `[]`/`[]=` and `<=>` **are** —
see their own sections below.

### `[]`/`[]=` indexing

```ruby
class Box
  def initialize()
    @data = {}
  end
  def [](key) = @data[key]
  def []=(key, value)
    @data[key] = value
  end
end

box = Box.new()
box["a"] = 1
box["a"]  # => 1
```

`x[i]` dispatches to `x`'s own `[]` method (one required parameter, the
index); `x[i] = v` dispatches to `[]=` (two required parameters, index
then value) — same mechanism as every other overloadable operator above,
so inheritance/`super`/interface-satisfaction all work the same way.
`x[i] += v` and chained `x[a][b] = v` need no special support: each is
already just ordinary reads (`[]`) and one final write (`[]=`) under the
hood, and both already dispatch through the receiver correctly. As with
every other operator here, there's no coercion — whatever sits between
the brackets (an `Int`, a `Range`, anything) is handed to `[]`/`[]=`
verbatim, with none of `Array`'s own `Range`-based slicing behavior
applied. `x[i] = v`'s own value as an expression is always `v` itself,
regardless of what `[]=` returns — matching Ruby's own `[]=` semantics,
and matching how `x[i] = v` already worked before this feature existed
for `Array`/`Hash`. A class that defines neither still raises `TypeError`
on `x[i]`/`x[i] = v`, exactly as before this feature existed.

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
singleton function rather than an instance method. Like a class (see
"Reopening" above), a module can be reopened -- a second `module Name`
adds more methods/nested classes to the same module instead of erroring,
the usual way to split a module's classes across several files while
keeping one `require`-able entry point.

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
`.tally`. Calling `.lazy()` on an Array, Range, or other Enumerable
returns a `LazyEnumerator`. Its `map`, `select`, and `reject` operations are
composable and deferred until a terminal runs; chained transforms do not
allocate intermediate Arrays. `each`, `to_a`, and `force` drain the pipeline.
`take`, `find`, `any?`, and `all?` short-circuit through the cooperative
`each_until` protocol; Arrays, Hash values, and Ranges stop pulling as soon as
the terminal has its answer. The remaining methods retain their eager
behavior. The last group returns its result directly —
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

Native collections are extensible from Diamond source without VM changes.
Defining `array_name(values, ...)`, `hash_name(values, ...)`, or the shared
fallback `enumerable_name(values, ...)` exposes `.name(...)` on the matching
receiver. A trailing predicate `?` is omitted from the bridge function name,
so `array_large(values, minimum)` implements `values.large?(minimum)`.
Built-in VM operations take precedence over extension bridges.

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

`BCrypt.hash(password: String, cost: Int)` and `BCrypt.verify(password:
String, digest: String) -> Bool` are native bcrypt password hashing,
backed by this system's own `libxcrypt` (`crypt_gensalt_rn`/`crypt_r`,
real `$2b$` bcrypt) rather than a vendored implementation — the same
"link a system library" pattern every other native dependency here
already follows (`sqlite3`, `libpq`, `mariadb`, OpenSSL). `.hash` always
takes both arguments explicitly (no default `cost` at this layer — see
`packages/active_record/README.md`'s `#secure_password=` for where a
default of 12 actually lives); `cost` outside `4..31` raises
`ArgumentError` before any hashing happens. `.verify` re-derives a digest
from `password` using `digest` itself as the salt/settings source and
compares with a constant-time comparison — a malformed or foreign
`digest` (not a real bcrypt hash) is an ordinary `false`, not an
exception, since checking a password against a bad hash is a normal
outcome here, not a programmer error:

```ruby
digest = BCrypt.hash("hunter2", 12)   # => "$2b$12$..."
BCrypt.verify("hunter2", digest)      # => true
BCrypt.verify("wrong", digest)        # => false
```

`libxcrypt`'s bcrypt support is a Linux-specific fact about this system's
`crypt(3)`, not something Diamond papers over — see `docs/roadmap.md`'s
"Explicitly deferred" section on multi-platform portability. A password
containing an embedded NUL byte is truncated at that point before
hashing, the same inherent limitation every C-`crypt`-backed bcrypt
binding has (`password` is passed to `crypt_r` as a NUL-terminated C
string).

`SecureRandom.bytes(n: Int) -> String` returns `n` cryptographically
random bytes (OpenSSL `RAND_bytes`, already linked for TLS) as a raw
Diamond `String` — Diamond strings are already raw byte buffers, so no
separate binary type is needed. `SecureRandom.hex(n: Int) -> String`
returns the same `n` random bytes hex-encoded, as a `2*n`-character
`String`. Both raise `ArgumentError` for a negative `n` (or one large
enough to overflow the underlying `int`-sized call into OpenSSL).
Suited to session/remember-me/password-reset tokens and similar —
`SecureRandom.hex(32)` for a 256-bit token as a 64-character hex string.

`Digest.sha256(data: String) -> String` returns the lowercase hexadecimal
SHA-256 digest of the string's raw bytes. `HMAC.sha256(key: String, data:
String) -> String` returns the corresponding keyed HMAC, also as 64 lowercase
hexadecimal characters. Both preserve embedded NUL bytes and are backed by
OpenSSL's `libcrypto`:

```ruby
Digest.sha256("abc")
HMAC.sha256("secret", "payload")
```

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

Inside a callable body that does not declare an `&block` parameter,
`yield(value)` suspends and sends `value` out
to whoever resumes; the next `.resume(v)` delivers `v` back in as
`yield`'s own expression result. `Fiber.new`'s argument must be a
zero-argument callable (captures are fine — only nested `def`s produce a
referenceable one, per the closures section above).
`Fiber.yield(value)` is the explicit equivalent and remains unambiguous inside
a callable that also declares `&block`; `Fiber.yield()` sends `nil`.

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

`exit(code = 0)` immediately terminates the whole process with the given
status (0–255; anything else raises `ArgumentError`, a non-`Int` raises
`TypeError`). This is a *hard* exit, not a raised/rescuable control-flow
value like Ruby's plain `exit` — no `ensure` block anywhere on the call
stack runs, and every other `Thread.new`-spawned OS thread stops too,
since they all share this one process. A validation failure (bad code)
is an ordinary catchable exception; only a valid code actually exits:

```ruby
begin
  exit(-1)
rescue error: ArgumentError
  puts("bad exit code: #{error.message()}")
end
exit(1)   # this one actually terminates the process
```

Like every other built-in name, a local variable or user-defined
function named `exit` shadows it.

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

The compiler is a Pratt parser that emits register bytecode directly;
there is no retained AST. `diamond_compile` actually runs this parser
twice per compile, not once: a throwaway first "discovery" pass walks the
whole source (tolerating a forward reference to a not-yet-declared
class/module/interface just long enough to keep going) purely to
register every declaration regardless of textual order, then a real
second pass emits the bytecode that actually runs, with every
declaration already known from the start -- see `docs/roadmap.md`'s
"Compiler representation" section. This is what lets a class construct
or call a singleton method on another class declared later in the same
file (`SomeClass.new(...)`, `OtherClass.someMethod(...)`), and lets a
type annotation name a class declared later too. A superclass still has
to be declared first (`class B < A` needs `A`'s complete, already-*fully-
compiled* field table, not just its name), and there's still no retained
AST for either pass to share — each is a full, independent walk of the
token stream. Pass `--dump-bytecode` on the CLI to see how any construct
in this document actually lowers (that dump reflects only the second,
real pass's output).
