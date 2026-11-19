# Enumerable

[API reference](index.md) · Previous: [Range](range.md) · Next: [Comparable](comparable.md)

`Enumerable` is a module that gives a class the collection methods once it
defines `each(callback)` and includes the module. [Range](range.md) uses it;
Array and Hash have their own versions of these methods, listed on their pages.

```ruby
class Countdown
  include Enumerable

  def initialize(from: Int)
    @from = from
  end

  def each(callback: Callable[1])
    i = @from
    while i > 0
      callback(i)
      i -= 1
    end
  end
end

Countdown.new(3).to_a()                       # => [3, 2, 1]
Countdown.new(4).select() do |i| i.even?() end   # => [4, 2]
Countdown.new(3).sum()                        # => 6
```

Each method calls `each` again, so a collection whose `each` has side effects
repeats them. Methods from `sort` down in this list first collect the elements
with `to_a`.

## Methods

### `all?() do |x| ... end -> Bool`

Whether the block is true for every element.

```ruby
(1..3).all?() do |i| i > 0 end   # => true
```

### `any?() do |x| ... end -> Bool`

Whether the block is true for some element.

```ruby
(1..3).any?() do |i| i > 2 end   # => true
```

### `count() do |x| ... end -> Int`

How many elements the block is true for.

```ruby
(1..4).count() do |i| i > 2 end   # => 2
```

### `drop(n) -> Array`

The elements after the first `n`.

```ruby
(1..4).drop(2)   # => [3, 4]
```

### `each_cons(size) -> Array[Array]`

Every run of `size` consecutive elements.

```ruby
(1..3).each_cons(2)   # => [[1, 2], [2, 3]]
```

### `each_slice(size) -> Array[Array]`

The elements in groups of `size`.

```ruby
(1..5).each_slice(2)   # => [[1, 2], [3, 4], [5]]
```

### `each_until() do |x| ... end`

Calls the block with each element until it returns `false`, then stops
calling it. `lazy()` and the short-circuiting methods pull elements this way.

### `each_with_index() do |x, i| ... end -> Array`

Calls the block with each element and its index; returns the elements.

### `find() do |x| ... end -> Value | Nil`

The first element the block is true for, or `nil`.

```ruby
(1..9).find() do |i| i * i > 10 end   # => 4
```

### `flat_map() do |x| ... end -> Array`

The block's Arrays joined into one.

```ruby
(1..2).flat_map() do |i| [i, i] end   # => [1, 1, 2, 2]
```

### `group_by() do |x| ... end -> Hash`

A `Hash` from each block value to the elements that produced it.

```ruby
(1..4).group_by() do |i| i.odd?() end   # => {true: [1, 3], false: [2, 4]}
```

### `lazy() -> LazyEnumerator`

A [LazyEnumerator](lazy_enumerator.md) over the elements.

### `map() do |x| ... end -> Array`

The block's value for each element.

```ruby
(1..3).map() do |i| i * 2 end   # => [2, 4, 6]
```

### `max() -> Value`

The largest element.

```ruby
(3..7).max()   # => 7
```

### `max_by() do |x| ... end -> Value`

The element with the largest block value.

### `min() -> Value`

The smallest element.

### `min_by() do |x| ... end -> Value`

The element with the smallest block value.

```ruby
(1..5).min_by() do |i| (i - 3).abs() end   # => 3
```

### `partition() do |x| ... end -> Array[Array]`

`[matching, not_matching]`.

```ruby
(1..4).partition() do |i| i.even?() end   # => [[2, 4], [1, 3]]
```

### `reduce(initial) do |memo, x| ... end -> Value`

Starts with `initial` and replaces it with the block's value for each element.

```ruby
(1..4).reduce(1) do |product, i| product * i end   # => 24
```

### `reject() do |x| ... end -> Array`

The elements the block is false for.

### `select() do |x| ... end -> Array`

The elements the block is true for.

```ruby
(1..6).select() do |i| i.even?() end   # => [2, 4, 6]
```

### `sort() -> Array`

### `sort() do |a, b| ... end -> Array`

The elements in ascending order, or in the block's order.

```ruby
(1..3).sort() do |a, b| b - a end   # => [3, 2, 1]
```

### `sort_by() do |x| ... end -> Array`

The elements ordered by block value.

### `sum() -> Value`

The elements added together, starting from `0`.

```ruby
(1..4).sum()   # => 10
```

### `take(n) -> Array`

The first `n` elements.

### `tally() -> Hash`

A `Hash` from each distinct element to its count.

### `to_a() -> Array`

The elements, collected by calling `each`.

### `zip(other) -> Array[Array]`

Each element paired with the element at the same index of `other`.

```ruby
(1..2).zip(["a", "b"])   # => [[1, "a"], [2, "b"]]
```
