# Range

[API reference](index.md) · Previous: [Float](float.md) · Next: [Enumerable](enumerable.md)

A `Range` is a run of Ints. `1..5` includes its end, `1...5` stops before it,
and `1..` has no end. Ranges are `Enumerable`, so they also answer every
method on the [Enumerable](enumerable.md) page, such as `map`, `select` and
`sum`. Arrays and Strings take a Range as an index (`values[1..-1]`).

```ruby
(1..5).to_a()                         # => [1, 2, 3, 4, 5]
(1...5).to_a()                        # => [1, 2, 3, 4]
(1..3).map() do |i| i * i end         # => [1, 4, 9]
```

## Methods

### `Range.new(start, end_value, exclusive) -> Range`

The Range from `start` to `end_value`, leaving out `end_value` when
`exclusive` is true. `end_value` may be `nil` for an endless Range. `a..b` is
`Range.new(a, b, false)`.

```ruby
Range.new(1, 3, false).to_a()   # => [1, 2, 3]
```

### `each() do |i| ... end -> Range`

Calls the block with each number in order and returns the Range. On an
endless Range it runs until the block breaks out.

```ruby
seen = []
(1..3).each() do |i| seen.push(i) end
seen   # => [1, 2, 3]
```

### `each_until() do |i| ... end -> Range`

Calls the block with each number until it returns `false`. This is the
protocol `lazy()` and the short-circuiting Enumerable methods use.

### `endless?() -> Bool`

Whether the Range has no end.

```ruby
(1..).endless?()   # => true
```

### `exclusive?() -> Bool`

Whether the end is left out.

```ruby
(1...5).exclusive?()   # => true
```

### `first() -> Int`

### `first(n) -> Array[Int]`

The start, or the first `n` numbers. `first(n)` works on an endless Range.

```ruby
(1..5).first()     # => 1
(1..).first(3)     # => [1, 2, 3]
```

### `include?(value) -> Bool`

Whether `value` lies in the Range.

```ruby
(1..5).include?(5)    # => true
(1...5).include?(5)   # => false
```

### `last() -> Int`

The end value, even when the Range leaves it out, as in Ruby.

```ruby
(1..5).last()    # => 5
(1...5).last()   # => 5
```

### `length() -> Int`

How many numbers the Range holds.

```ruby
(1...5).length()   # => 4
```

### `reverse_each() do |i| ... end`

Calls the block with each number from the last down to the first.

```ruby
seen = []
(1..3).reverse_each() do |i| seen.push(i) end
seen   # => [3, 2, 1]
```

### `step(size) -> Array[Int]`

Every `size`-th number, starting with the first.

```ruby
(1..5).step(2)   # => [1, 3, 5]
```

### `to_a() -> Array[Int]`

The numbers as an Array.

```ruby
(1..5).to_a()   # => [1, 2, 3, 4, 5]
```

### `to_s() -> String`

The Range as it is written.

```ruby
(1..5).to_s()   # => "1..5"
```
