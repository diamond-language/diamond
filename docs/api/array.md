# Array

[API reference](index.md) · Previous: [String](string.md) · Next: [Hash](hash.md)

An `Array` is an ordered, growable list of values, written `[1, 2, 3]` or
`Array[Int]` when typed. `Array.new(n)` makes `n` nils, `Array.new(n, fill)`
`n` references to one `fill`, and `Array.new(n) do |i| ... end` fills each slot
from the block.

Most methods return a new `Array`. The ones that change the receiver say so:
`push`, `pop`, `shift`, `unshift`, `insert`, `delete`, `delete_at`,
`delete_if`, `keep_if`, `fill`, `clear`, `replace`, and assignment through
`a[i] = x`. A block-taking method also accepts a callable in place of the
block (`values.select(is_even)`).

## Operators

| Operator | Result |
|---|---|
| `a + b` | `a`'s elements followed by `b`'s, the same as `concat` |
| `a - b` | `a`'s elements that are not in `b` |
| `a & b` | elements in both, without duplicates, in `a`'s order |
| `a \| b` | elements in either, without duplicates |
| `a * n` | `a` repeated `n` times |
| `a * sep` | `join(sep)` when `sep` is a `String` |
| `a == b` | same length and equal elements in order |
| `a[i]` | the element at `i`; a negative `i` counts from the end; `IndexError` outside |
| `a[start, length]` | `slice(start, length)` |
| `a[a..b]`, `a[a...b]` | the elements in the Range |
| `a[i] = x` | stores `x` at `i` |

```ruby
[1, 2] + [3]          # => [1, 2, 3]
[1, 2, 2, 3] - [2]    # => [1, 3]
[1, 2] & [2, 3]       # => [2]
[1, 2] | [2, 3]       # => [1, 2, 3]
[1, 2] * ", "         # => "1, 2"
[1, 2, 3][-1]         # => 3
[1, 2, 3][1..-1]      # => [2, 3]
```

## Methods

### `all?() do |x| ... end -> Bool`

Whether the block is true for every element. Without a block, whether no
element is `nil` or `false`.

```ruby
[1, 2].all?() do |x| x > 0 end   # => true
[1, nil].all?()                  # => false
```

### `any?() do |x| ... end -> Bool`

Whether the block is true for some element. Without a block, whether some
element is neither `nil` nor `false`.

```ruby
[1, 2].any?() do |x| x > 1 end   # => true
```

### `chunk_while() do |a, b| ... end -> Array[Array]`

Splits the elements into runs, starting a new run wherever the block is false
for a neighbouring pair.

```ruby
[1, 2, 4, 5, 7].chunk_while() do |a, b| b == a + 1 end   # => [[1, 2], [4, 5], [7]]
```

### `clear() -> Array`

Removes every element from the receiver and returns it.

### `combination(k) -> Array[Array]`

Every combination of `k` elements, in order.

```ruby
[1, 2, 3].combination(2)   # => [[1, 2], [1, 3], [2, 3]]
```

### `compact() -> Array`

The elements that are not `nil`.

```ruby
[1, nil, 2].compact()   # => [1, 2]
```

### `concat(other) -> Array`

A new `Array` with `other`'s elements after the receiver's; the receiver does
not change.

```ruby
[1].concat([2, 3])   # => [1, 2, 3]
```

### `count() -> Int`

### `count(value) -> Int`

### `count() do |x| ... end -> Int`

The number of elements, of elements equal to `value`, or of elements for which
the block is true.

```ruby
[1, 2, 2].count(2)                     # => 2
[1, 2, 3].count() do |x| x > 1 end     # => 2
```

### `deep_freeze() -> Array`

Freezes the receiver and every Array, Hash and object inside it.

### `delete(value) -> Value | Nil`

Removes every element equal to `value` from the receiver. Returns `value`, or
`nil` when nothing was removed.

```ruby
a = [1, 2, 1]
a.delete(1)   # => 1
a             # => [2]
```

### `delete_at(index) -> Value`

Removes the element at `index` from the receiver and returns it.

```ruby
a = [1, 2, 3]
a.delete_at(1)   # => 2
a                # => [1, 3]
```

### `delete_if() do |x| ... end -> Array`

Removes the elements for which the block is true, in place, and returns the
receiver.

```ruby
[1, 2, 3].delete_if() do |x| x > 1 end   # => [1]
```

### `dig(index, ...) -> Value | Nil`

Follows the indexes (or Hash keys) through nested collections, returning `nil`
when a step is missing. Up to six steps.

```ruby
[[1, [2, 3]]].dig(0, 1, 0)   # => 2
```

### `drop(n) -> Array`

The elements after the first `n`.

```ruby
[1, 2, 3].drop(1)   # => [2, 3]
```

### `drop_while() do |x| ... end -> Array`

The elements from the first one for which the block is false.

```ruby
[1, 2, 3, 1].drop_while() do |x| x < 2 end   # => [2, 3, 1]
```

### `dup() -> Array`

A shallow, unfrozen copy.

### `each() do |x| ... end -> Array`

Calls the block with each element and returns the receiver.

```ruby
total = 0
[1, 2, 3].each() do |x| total += x end
total   # => 6
```

### `each_cons(n) -> Array[Array]`

Every run of `n` consecutive elements. With a block, each run is also passed
to it.

```ruby
[1, 2, 3].each_cons(2)   # => [[1, 2], [2, 3]]
```

### `each_slice(n) -> Array[Array]`

The elements in groups of `n`; the last group may be shorter. With a block,
each group is also passed to it.

```ruby
[1, 2, 3].each_slice(2)   # => [[1, 2], [3]]
```

### `each_with_index() do |x, i| ... end -> Array`

Calls the block with each element and its index and returns the receiver.

```ruby
seen = []
["a", "b"].each_with_index() do |x, i| seen.push("#{i}#{x}") end
seen   # => ["0a", "1b"]
```

### `each_with_object(memo) do |x, memo| ... end -> Value`

Calls the block with each element and `memo`, then returns `memo`.

```ruby
[1, 2].each_with_object([]) do |x, memo| memo.push(x * 2) end   # => [2, 4]
```

### `empty?() -> Bool`

Whether there are no elements.

```ruby
[].empty?()   # => true
```

### `fill(value) -> Array`

Sets every element of the receiver to `value` and returns it.

```ruby
[1, 2].fill(0)   # => [0, 0]
```

### `filter_map() do |x| ... end -> Array`

The block's values that are not `nil` or `false`.

```ruby
[1, 2, 3].filter_map() do |x| x > 1 ? x * 10 : nil end   # => [20, 30]
```

### `find() do |x| ... end -> Value | Nil`

The first element for which the block is true, or `nil`.

```ruby
[1, 2, 3].find() do |x| x > 1 end   # => 2
```

### `find_index(value) -> Int | Nil`

### `find_index() do |x| ... end -> Int | Nil`

The index of the first element equal to `value`, or for which the block is
true; `nil` when there is none.

```ruby
[1, 2, 3].find_index() do |x| x > 1 end   # => 1
```

### `first() -> Value`

### `first(n) -> Array`

The first element (an empty Array raises `IndexError`), or the first `n`.

```ruby
[1, 2, 3].first()    # => 1
[1, 2, 3].first(2)   # => [1, 2]
```

### `first_or(fallback) -> Value`

The first element, or `fallback` when the Array is empty.

```ruby
[].first_or(0)   # => 0
```

### `flat_map() do |x| ... end -> Array`

The block's Arrays joined into one.

```ruby
[1, 2].flat_map() do |x| [x, x] end   # => [1, 1, 2, 2]
```

### `flatten(depth = -1) -> Array`

Nested Arrays spliced in, to `depth` levels or all of them.

```ruby
[1, [2, [3]]].flatten()    # => [1, 2, 3]
[1, [2, [3]]].flatten(1)   # => [1, 2, [3]]
```

### `freeze() -> Array`

Makes the receiver read-only (changing it raises `FrozenError`) and returns it.

### `frozen?() -> Bool`

Whether the receiver is frozen.

### `group_by() do |x| ... end -> Hash`

A `Hash` from each block value to the elements that produced it.

```ruby
[1, 2, 3].group_by() do |x| x.odd?() end   # => {true: [1, 3], false: [2]}
```

### `include?(value) -> Bool`

Whether some element is equal to `value`.

```ruby
[1, 2].include?(2)   # => true
```

### `index(value) -> Int | Nil`

### `index() do |x| ... end -> Int | Nil`

The same as `find_index`.

### `index_of(value) -> Int | Nil`

The index of the first element equal to `value`, or `nil`.

```ruby
[1, 2].index_of(3)   # => nil
```

### `inject(initial) do |memo, x| ... end -> Value`

The same as `reduce`.

### `insert(index, value) -> Array`

Inserts `value` before `index`, in place, and returns the receiver.

```ruby
[1, 3].insert(1, 2)   # => [1, 2, 3]
```

### `inspect() -> String`

The Array as a literal, each element shown with its own `inspect`.

```ruby
puts([1, "a"].inspect())   # => [1, "a"]
```

### `intersect?(other) -> Bool`

Whether the two Arrays share an element.

```ruby
[1, 2].intersect?([2, 3])   # => true
```

### `join(separator = "") -> String`

The elements as text (as interpolation shows them) with `separator` between.

```ruby
[1, 2, 3].join("-")   # => "1-2-3"
```

### `keep_if() do |x| ... end -> Array`

Keeps only the elements for which the block is true, in place, and returns the
receiver.

```ruby
[1, 2, 3].keep_if() do |x| x > 1 end   # => [2, 3]
```

### `last() -> Value`

### `last(n) -> Array`

The last element (an empty Array raises `IndexError`), or the last `n`.

```ruby
[1, 2, 3].last(2)   # => [2, 3]
```

### `last_or(fallback) -> Value`

The last element, or `fallback` when the Array is empty.

```ruby
[].last_or(9)   # => 9
```

### `lazy() -> LazyEnumerator`

A lazy view: `map`, `select` and `reject` on it are deferred until `to_a`,
`each`, `take`, `find`, `any?` or `all?` pulls values.

```ruby
[1, 2, 3].lazy().map() do |x| x * 2 end.take(2).to_a()   # => [2, 4]
```

### `length() -> Int`

The number of elements.

```ruby
[1, 2].length()   # => 2
```

### `map() do |x| ... end -> Array`

The block's value for each element.

```ruby
[1, 2].map() do |x| x * 2 end   # => [2, 4]
```

### `max() -> Value`

### `max(n) -> Array`

The largest element, or the `n` largest, largest first.

```ruby
[1, 3, 2].max()    # => 3
[1, 3, 2].max(2)   # => [3, 2]
```

### `max_by() do |x| ... end -> Value`

The element with the largest block value.

```ruby
["a", "ccc", "bb"].max_by() do |s| s.length() end   # => "ccc"
```

### `min() -> Value`

### `min(n) -> Array`

The smallest element, or the `n` smallest, smallest first.

```ruby
[3, 1, 2].min()   # => 1
```

### `min_by() do |x| ... end -> Value`

The element with the smallest block value.

```ruby
["ccc", "a"].min_by() do |s| s.length() end   # => "a"
```

### `minmax() -> Array`

`[min, max]`.

```ruby
[3, 1, 2].minmax()   # => [1, 3]
```

### `nil?() -> Bool`

Always `false` for an `Array`.

### `none?() do |x| ... end -> Bool`

Whether the block is false for every element. Without a block, whether every
element is `nil` or `false`.

```ruby
[1, 2].none?() do |x| x > 5 end   # => true
```

### `one?() do |x| ... end -> Bool`

Whether the block is true for exactly one element. Without a block, whether
exactly one element is neither `nil` nor `false`.

```ruby
[1, 2].one?() do |x| x > 1 end   # => true
```

### `pack(template) -> String`

Encodes the elements as a binary `String`; the directives are the ones
`String#unpack` reads.

```ruby
[65, 66].pack("C*")   # => "AB"
```

### `partition() do |x| ... end -> Array[Array]`

`[matching, not_matching]`.

```ruby
[1, 2, 3].partition() do |x| x.odd?() end   # => [[1, 3], [2]]
```

### `pop() -> Value`

Removes the last element from the receiver and returns it.

```ruby
a = [1, 2]
a.pop()   # => 2
a         # => [1]
```

### `product(other) -> Array[Array]`

Every pair of an element of the receiver and one of `other`.

```ruby
[1, 2].product([3, 4])   # => [[1, 3], [1, 4], [2, 3], [2, 4]]
```

### `push(value) -> Array`

Appends `value` to the receiver and returns it.

```ruby
a = [1]
a.push(2)   # => [1, 2]
```

### `reduce(initial) do |memo, x| ... end -> Value`

Starts with `initial`, replaces it with the block's value for each element,
and returns the last one.

```ruby
[1, 2, 3].reduce(0) do |sum, x| sum + x end   # => 6
```

### `reject() do |x| ... end -> Array`

The elements for which the block is false.

```ruby
[1, 2, 3].reject() do |x| x > 1 end   # => [1]
```

### `replace(other) -> Array`

Replaces the receiver's contents with `other`'s and returns the receiver.

```ruby
[1].replace([2, 3])   # => [2, 3]
```

### `reverse() -> Array`

The elements in reverse order.

```ruby
[1, 2].reverse()   # => [2, 1]
```

### `rindex(value) -> Int | Nil`

The index of the last element equal to `value`, or `nil`.

```ruby
[1, 2, 1].rindex(1)   # => 2
```

### `rotate(n = 1) -> Array`

The elements shifted left by `n` places, wrapping around; a negative `n`
shifts right.

```ruby
[1, 2, 3].rotate()     # => [2, 3, 1]
[1, 2, 3].rotate(-1)   # => [3, 1, 2]
```

### `sample() -> Value`

A random element.

### `select() do |x| ... end -> Array`

The elements for which the block is true.

```ruby
[1, 2, 3].select() do |x| x > 1 end   # => [2, 3]
```

### `shift() -> Value`

Removes the first element from the receiver and returns it.

```ruby
a = [1, 2]
a.shift()   # => 1
a           # => [2]
```

### `shuffle() -> Array`

The elements in random order.

### `size() -> Int`

The same as `length()`.

### `slice(start, length) -> Array`

`length` elements from `start`, clamped to the end.

```ruby
[1, 2, 3].slice(1, 5)   # => [2, 3]
```

### `slice_when() do |a, b| ... end -> Array[Array]`

Splits the elements into runs, starting a new run wherever the block is true
for a neighbouring pair.

```ruby
[1, 2, 4].slice_when() do |a, b| b != a + 1 end   # => [[1, 2], [4]]
```

### `sort() -> Array`

### `sort() do |a, b| ... end -> Array`

The elements in ascending order, or in the order given by a block that returns
a negative, zero or positive `Int`. The sort is stable. Arrays compare element
by element.

```ruby
[3, 1, 2].sort()                       # => [1, 2, 3]
[3, 1, 2].sort() do |a, b| b - a end   # => [3, 2, 1]
```

### `sort_by() do |x| ... end -> Array`

The elements ordered by their block values, calling the block once each.

```ruby
["bb", "a"].sort_by() do |s| s.length() end   # => ["a", "bb"]
```

### `sum(start = 0) -> Value`

### `sum(start = 0) do |x| ... end -> Value`

The elements, or their block values, added onto `start`.

```ruby
[1, 2].sum()                       # => 3
[1, 2].sum(0.5)                    # => 3.5
[1, 2].sum() do |x| x * 10 end     # => 30
```

### `sum_by() do |x| ... end -> Value`

The same as `sum` with a block.

### `take(n) -> Array`

The first `n` elements.

```ruby
[1, 2, 3].take(2)   # => [1, 2]
```

### `take_while() do |x| ... end -> Array`

The elements before the first one for which the block is false.

```ruby
[1, 2, 3].take_while() do |x| x < 3 end   # => [1, 2]
```

### `tally() -> Hash`

A `Hash` from each distinct element to how many times it occurs.

```ruby
["a", "b", "a"].tally()   # => {"a": 2, "b": 1}
```

### `tap() do |value| ... end -> Array`

Calls the block with the receiver and returns the receiver.

### `to_a() -> Array`

The receiver.

### `to_h() -> Hash`

A `Hash` from an Array of `[key, value]` pairs.

```ruby
[["a", 1]].to_h()   # => {"a": 1}
```

### `to_s() -> String`

The same as `inspect()`.

### `transpose() -> Array[Array]`

Rows turned into columns.

```ruby
[[1, 2], [3, 4]].transpose()   # => [[1, 3], [2, 4]]
```

### `uniq() -> Array`

### `uniq() do |x| ... end -> Array`

The first of each group of equal elements, or of elements with equal block
values, in order.

```ruby
[1, 1, 2].uniq()                                # => [1, 2]
["a", "A"].uniq() do |s| s.downcase() end       # => ["a"]
```

### `unshift(value) -> Array`

Inserts `value` at the front of the receiver and returns it.

```ruby
[2].unshift(1)   # => [1, 2]
```

### `values_at(index, ...) -> Array`

The elements at the given indexes, up to six.

```ruby
[1, 2, 3].values_at(0, 2)   # => [1, 3]
```

### `zip(other, ...) -> Array[Array]`

Pairs each element with the elements at the same index in up to six other
Arrays, padding with `nil` to the receiver's length.

```ruby
[1, 2, 3].zip([4, 5])   # => [[1, 4], [2, 5], [3, nil]]
```
