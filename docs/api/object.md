# Object

[API reference](index.md) · Previous: [Kernel functions](kernel.md) · Next: [String](string.md)

Every value answers the methods on this page, whatever its type: Strings,
numbers, collections, `nil`, Booleans, Symbols and instances of your own
classes. A class can define its own version of any of them.

```ruby
class Point
  def initialize(x: Int, y: Int)
    @x = x
    @y = y
  end
end

pt = Point.new(1, 2)
pt.class()          # => "Point"
pt.is_a?(Point)     # => true
pt.inspect()        # => "#<Point x=1, y=2>"
```

## Methods

### `class() -> String`

The name of the value's class.

```ruby
1.class()        # => "Int"
nil.class()      # => "Nil"
:ok.class()      # => "Symbol"
```

### `deep_freeze() -> Value`

Freezes the value and everything inside it.

### `dup() -> Value`

A shallow, unfrozen copy. Numbers, `nil`, Booleans and Symbols return
themselves.

### `freeze() -> Value`

Makes the value read-only and returns it. Changing a frozen Array, Hash or
object raises `FrozenError`.

### `frozen?() -> Bool`

Whether the value is frozen. Numbers, `nil`, Booleans and Symbols always are.

### `inspect() -> String`

The value as it would be written in source: Strings quoted and escaped,
Symbols with their colon, instances with their fields.

```ruby
"line\n".inspect()   # => "\"line\\n\""
:ok.inspect()        # => ":ok"
```

### `is_a?(Type) -> Bool`

Whether the value is an instance of `Type` or a subclass of it. `Type` is
written as a type name.

```ruby
1.is_a?(Int)     # => true
1.is_a?(Float)   # => false
```

### `nil?() -> Bool`

Whether the value is `nil`.

```ruby
nil.nil?()   # => true
0.nil?()     # => false
```

### `respond_to?(name) -> Bool`

Whether the value has a method called `name`, a `String` or a `Symbol`.

```ruby
"a".respond_to?("upcase")   # => true
"a".respond_to?(:zzz)       # => false
```

### `tap() do |value| ... end -> Value`

Calls the block with the value and returns the value.

### `to_s() -> String`

The value as text, as interpolation and `puts` show it. An instance without
its own `to_s` shows as `#<Name>`.

```ruby
:ok.to_s()    # => "ok"
nil.to_s()    # => "nil"
```

## Nil, Bool and Symbol

`nil` is the only `Nil` value. `true` and `false` are `Bool`; only `nil` and
`false` count as false in a condition. Booleans combine with `&&`, `||` and
`!` (there is no `&` or `|` on Bool).

A `Symbol` such as `:ok` is a small name-like value, compared by content.
`to_sym(string)` makes one, and it answers only the methods above.

```ruby
:ok == :ok        # => true
:ok == "ok"       # => false
"#{:ok}"          # => "ok"
```
