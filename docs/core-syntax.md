# Core syntax and values

[Language reference](syntax.md) · Next: [Functions, closures, and calls](callables.md)

Ruby-like surface syntax, but expression-oriented and gradually typed — no
separate "typed" object model, just optional annotations on top of dynamic
dispatch. This document is a tour of the surface syntax; see
[Design and VM architecture](design.md) for how it compiles and executes,
[Object model](object-model.md), [Fibers](fibers.md), and
[I/O and native services](io.md) for focused guides.
Diamond's runtime intentionally has no HTTP support built in — see
[Cuts](packages.md) for `facet`, the package manager, and `packages/http`
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
values because Diamond's native types are not reified classes. A subject-less
boolean form (Ruby's `case` with no expression, where each `when`'s own value
is tested for truthiness instead of compared against a subject) is also
supported — see below.

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
(`arr[i] += 1`, `hash[k] ||= default`) are also supported — see "Indexed
compound assignment" under Ranges below.

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
