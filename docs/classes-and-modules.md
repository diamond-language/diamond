# Classes, modules, and methods

[Language reference](syntax.md) · Previous: [Functions, closures, and calls](callables.md) · Next: [Gradual typing and exceptions](types-and-errors.md)

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

### `compile_method`

`ClassName.compile_method(name, params, body_source, bound_values)` closes
the one gap `define_method` leaves: its callable normally has to be an
already-compiled nested `def`, physically written in the source, so there's
no way to build a method body from a runtime string. `compile_method` does
that -- it compiles `body_source` as if it were a method on `ClassName` and
returns a `Callable` ready to hand to `define_method`, the same as the
factory idiom above:

```ruby
class Greeter
  attr_accessor name: String

  def initialize(name: String)
    @name = name
  end
end

callable = Greeter.compile_method("greeting", ["prefix"], "prefix + self.name()", {})
Greeter.define_method("greeting", callable)

Greeter.new("Ada").greeting("Hello, ")  # => "Hello, Ada"
```

`params` is a plain list of parameter names (`String`s) -- no types,
defaults, splats, or block parameters. `body_source` can reference `self`,
call other methods on it, and read/write the class's *existing* `@fields`,
but it can't grow the class's field layout: referencing a field the class
doesn't already have fails with `ArgumentError` before anything is
installed, as does a syntax error in `body_source`. It also can't name
another class directly (`body_source` compiles in its own isolated
program, which doesn't know any class but `ClassName` exists) or close over
the calling scope's locals -- `bound_values`, a `Hash` of already-evaluated
values (capped at 8 entries), is how you thread those in instead; they're
spliced on as trailing parameters the installed method's own callers never
supply:

```ruby
callable = Other.compile_method("boxed_label", [], "formatter.wrap(self.label())",
  {"formatter": formatter})
```

Out of scope for now: `self.`-owned singleton methods (instance methods
only) and any sandboxing -- `body_source` runs as ordinary compiled
bytecode with full language access, the same trust level Ruby's own
`class_eval`/`define_method` assume. Meant for programmer-authored
metaprogramming (see `packages/active_record`'s `has_many`), not for
compiling untrusted input.

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
declaration.

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
its own. `self` inside a class-owned singleton method is one way to
obtain one; a bare class name as a `case`/`when` pattern (`when Dog`,
[core syntax](core-syntax.md)) is the other. Neither is a general
expression -- there is still no way to store a class in a variable
outside those two positions, pass one as an ordinary argument, or name
one dynamically by a computed string (see docs/internal/design.md's
`DIAMOND_VALUE_CLASS` section, and docs/roadmap.md's "Explicitly
deferred" section for why that stays out of scope).

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
collect and forward through singleton spread. Generic references accept
explicit bindings at the reference site (`Tools.identity[Int]`); the wrapper
bakes those bindings into its forwarded call. A direct, statically resolved
Callable argument may instead provide those bindings contextually. Without
either source, a bare unbound generic reference remains a compile error.

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

### tap / dup / respond_to? / public_send

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
p1.public_send(:x)     # => 1

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
over `dup`/`respond_to?`/`public_send`'s own built-in behavior, checked first.

`public_send(name, *arguments)` invokes the method named by a `Symbol` or
`String`. It follows ordinary dynamic dispatch for native and user-defined
receivers, including inheritance, overrides, variadic methods, and
`method_missing`. Only public targets are callable: private and protected
methods are rejected even when `public_send` itself is called from within the
target's class hierarchy. Diamond intentionally provides no visibility-
bypassing `send` counterpart. A user class may define its own `public_send`;
that method takes priority over the universal behavior.

### `method_missing`

```ruby
class Ghost
  def method_missing(name, args)
    "called #{name} with #{args.length()} args"
  end
end

g = Ghost.new()
g.anything()        # => "called anything with 0 args"
```

`def method_missing(name, args)` on a class is consulted whenever ordinary
instance-method dispatch finds no method by that name anywhere on the
receiver's class or its ancestors -- `name` is the attempted method as a
`Symbol`, `args` an `Array` of the call's own arguments (the receiver
itself isn't included). A real method of that name always wins first, on
any ancestor, so `method_missing` can never intercept a call to something
the class actually defines. Without one, a dispatch miss raises
`NoMethodError`, same as before this feature existed; if `method_missing`
itself doesn't take exactly two required parameters, a miss raises
`ArgumentError` instead. It's found via ordinary inherited lookup, so one
defined on a superclass covers every subclass too. Scoped to this one
dispatch site only -- not operator overloading, `to_s`, `super`, or
`self.`-singleton calls, each of which already has its own fallback.

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
only way *user code* opts into operator support. [`Time`](time.md) is the one
**native**, non-`Instance` type with real
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
