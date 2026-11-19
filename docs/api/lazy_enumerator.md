# LazyEnumerator

[API reference](index.md) · Previous: [Comparable](comparable.md) · Next: [StringBuilder](string_builder.md)

A `LazyEnumerator` comes from `lazy()` on an Array, Hash (its values), Range
or other `Enumerable`. `map`, `select` and `reject` on it record a step instead
of building an Array; nothing runs until a method that needs values pulls
them. `take`, `find`, `any?` and `all?` stop pulling as soon as they have their
answer, so a lazy chain works on an endless Range.

```ruby
(1..).lazy().map() do |i| i * 2 end.select() do |i| i % 3 == 0 end.take(2)   # => [6, 12]
```

## Methods

### `all?() do |x| ... end -> Bool`

Whether the block is true for every value; stops at the first false one.

```ruby
(1..5).lazy().all?() do |i| i > 0 end   # => true
```

### `any?() do |x| ... end -> Bool`

Whether the block is true for some value; stops at the first true one.

```ruby
(1..).lazy().any?() do |i| i > 10 end   # => true
```

### `each() do |x| ... end`

Runs the steps and calls the block with each resulting value.

```ruby
seen = []
(1..3).lazy().map() do |i| i * 10 end.each() do |i| seen.push(i) end
seen   # => [10, 20, 30]
```

### `each_until() do |x| ... end`

Runs the steps until the block returns `false`.

### `find() do |x| ... end -> Value | Nil`

The first value the block is true for, or `nil`.

```ruby
(1..).lazy().find() do |i| i > 10 end   # => 11
```

### `force() -> Array`

The same as `to_a()`.

### `map() do |x| ... end -> LazyEnumerator`

Adds a step that replaces each value with the block's value.

### `reject() do |x| ... end -> LazyEnumerator`

Adds a step that drops the values the block is true for.

### `select() do |x| ... end -> LazyEnumerator`

Adds a step that keeps the values the block is true for.

### `take(count) -> Array`

The first `count` values.

```ruby
(1..).lazy().map() do |i| i * i end.take(3)   # => [1, 4, 9]
```

### `to_a() -> Array`

Every value. Do not call it on an endless source.

```ruby
(1..5).lazy().reject() do |i| i.odd?() end.to_a()   # => [2, 4]
```
