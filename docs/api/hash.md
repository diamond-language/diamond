# Hash

[API reference](index.md) · Previous: [Array](array.md) · Next: [Int](int.md)

A `Hash` maps keys to values and keeps insertion order. It is written
`{"name": "Ada", "age": 36}` (or with `=>`), and `Hash[String, Int]` when
typed. The key before `:` is an expression, so `{"a": 1}` has the key `"a"`.
`Hash.new(default)` makes a Hash whose missing keys read as `default` instead
of `nil`.

Iteration passes `(key, value)` to a block that takes two parameters. Methods
that also accept a one-parameter block pass it the value, and say so below.
`each` needs two parameters.

Most methods return a new value. `delete`, `delete_if`, `keep_if`, `clear`,
`update` and `h[key] = value` change the receiver.

## Operators

| Operator | Result |
|---|---|
| `h[key]` | the value for `key`, or the default (`nil` unless `Hash.new(default)`) |
| `h[key] = value` | stores `value` under `key` |
| `a == b` | the same keys with equal values, in any order |

```ruby
h = {"a": 1}
h["b"] = 2
h                                       # => {"a": 1, "b": 2}
h["z"]                                  # => nil
{"a": 1, "b": 2} == {"b": 2, "a": 1}    # => true
```

## Creating Hashes

### `Hash.new(default = nil) -> Hash`

An empty Hash whose missing keys read as `default`. Every missing key returns
the same `default` object, and reading one does not store it.

```ruby
counts = Hash.new(0)
counts["a"] += 1
counts   # => {"a": 1}
```

## Methods

### `all?() do |key, value| ... end -> Bool`

Whether the block is true for every entry. Without a block it is always
`true`: each entry counts as its `[key, value]` pair, which is never false.

```ruby
{"a": 1, "b": 2}.all?() do |k, v| v > 0 end   # => true
{"a": nil}.all?()                              # => true
```

### `any?() do |key, value| ... end -> Bool`

Whether the block is true for some entry. Without a block, whether the Hash
has any entries.

```ruby
{"a": 1, "b": 2}.any?() do |k, v| v > 1 end   # => true
{}.any?()                                      # => false
```

### `clear() -> Hash`

Removes every entry from the receiver and returns it.

### `compact() -> Hash`

The entries whose value is not `nil`.

```ruby
{"a": 1, "b": nil}.compact()   # => {"a": 1}
```

### `count() -> Int`

### `count() do |key, value| ... end -> Int`

The number of entries, or of entries for which the block is true.

```ruby
{"a": 1, "b": 2}.count() do |k, v| v > 1 end   # => 1
```

### `deep_freeze() -> Hash`

Freezes the receiver and every Array, Hash and object inside it.

### `default() -> Value`

The value missing keys read as: the argument to `Hash.new`, or `nil`.

```ruby
Hash.new(0).default()   # => 0
```

### `delete(key) -> Value | Nil`

Removes `key` from the receiver and returns its value, or `nil` when it was
not there.

```ruby
h = {"a": 1, "b": 2}
h.delete("a")   # => 1
h               # => {"b": 2}
```

### `delete_if() do |key, value| ... end -> Hash`

Removes the entries for which the block is true, in place, and returns the
receiver.

```ruby
{"a": 1, "b": 2}.delete_if() do |k, v| v > 1 end   # => {"a": 1}
```

### `dig(key, ...) -> Value | Nil`

Follows the keys (or Array indexes) through nested collections, returning
`nil` when a step is missing. Up to six steps.

```ruby
{"a": {"b": [5]}}.dig("a", "b", 0)   # => 5
```

### `dup() -> Hash`

A shallow, unfrozen copy, with the same default.

### `each() do |key, value| ... end -> Hash`

Calls the block with each key and value and returns the receiver.

```ruby
keys = []
{"a": 1, "b": 2}.each() do |k, v| keys.push(k) end
keys   # => ["a", "b"]
```

### `each_pair() do |key, value| ... end -> Hash`

The same as `each`.

### `each_with_index() do |pair, index| ... end -> Hash`

Calls the block with each `[key, value]` pair and its index.

```ruby
seen = []
{"a": 1}.each_with_index() do |pair, i| seen.push([pair, i]) end
seen   # => [[["a", 1], 0]]
```

### `each_with_object(memo) do |key, value, memo| ... end -> Value`

Calls the block with each entry and `memo`, then returns `memo`.

```ruby
{"a": 1, "b": 2}.each_with_object([]) do |k, v, memo| memo.push(k) end   # => ["a", "b"]
```

### `empty?() -> Bool`

Whether there are no entries.

```ruby
{}.empty?()   # => true
```

### `equal?(other) -> Bool`

Whether `other` is this same Hash object; `==` compares the entries.

### `except(key, ...) -> Hash`

The entries without the given keys, up to six.

```ruby
{"a": 1, "b": 2}.except("a")   # => {"b": 2}
```

### `fetch(key) -> Value`

### `fetch(key, fallback) -> Value`

The value for `key`. A missing key returns `fallback`, or raises `IndexError`
when there is none.

```ruby
{"a": 1}.fetch("a")      # => 1
{"a": 1}.fetch("z", 0)   # => 0
```

### `fetch_values(key, ...) -> Array`

The values for the given keys, up to six; a missing key raises `IndexError`.

```ruby
{"a": 1, "b": 2}.fetch_values("a", "b")   # => [1, 2]
```

### `filter() do |key, value| ... end -> Hash`

The same as `select`.

### `filter_map() do |key, value| ... end -> Array`

The block's values that are not `nil` or `false`.

```ruby
{"a": 1, "b": 2}.filter_map() do |k, v| v > 1 ? k : nil end   # => ["b"]
```

### `find() do |key, value| ... end -> Array | Nil`

The first `[key, value]` for which the block is true, or `nil`.

```ruby
{"a": 1, "b": 2}.find() do |k, v| v > 1 end   # => ["b", 2]
```

### `first() -> Array`

### `first(n) -> Array[Array]`

The first `[key, value]`, or the first `n` of them.

```ruby
{"a": 1, "b": 2}.first()   # => ["a", 1]
```

### `freeze() -> Hash`

Makes the receiver read-only (changing it raises `FrozenError`) and returns it.

### `frozen?() -> Bool`

Whether the receiver is frozen.

### `group_by() do |key, value| ... end -> Hash`

A `Hash` from each block value to a `Hash` of the entries that produced it.

```ruby
{"a": 1, "b": 2}.group_by() do |k, v| v.odd?() end   # => {true: {"a": 1}, false: {"b": 2}}
```

### `has_key?(key) -> Bool`

The same as `include_key?`.

### `has_value?(value) -> Bool`

The same as `value?`.

### `include?(key) -> Bool`

The same as `include_key?`.

### `include_key?(key) -> Bool`

Whether `key` is in the Hash.

```ruby
{"a": 1}.include_key?("a")   # => true
```

### `inspect() -> String`

The Hash as a literal, keys and values shown with their own `inspect`.

```ruby
puts({"a": 1}.inspect())   # => {"a": 1}
```

### `invert() -> Hash`

Values become keys and keys values.

```ruby
{"a": 1, "b": 2}.invert()   # => {1: "a", 2: "b"}
```

### `keep_if() do |key, value| ... end -> Hash`

Keeps only the entries for which the block is true, in place, and returns the
receiver.

```ruby
{"a": 1, "b": 2}.keep_if() do |k, v| v > 1 end   # => {"b": 2}
```

### `key?(key) -> Bool`

The same as `include_key?`. Unlike Ruby's `Hash#key`, `h.key(x)` is this test
too, not a reverse lookup.

### `key_at(index) -> Value`

The key at position `index` in insertion order.

```ruby
{"a": 1, "b": 2}.key_at(1)   # => "b"
```

### `keys() -> Array`

The keys in insertion order.

```ruby
{"a": 1, "b": 2}.keys()   # => ["a", "b"]
```

### `lazy() -> LazyEnumerator`

A lazy view over the values.

```ruby
{"a": 1, "b": 2}.lazy().map() do |v| v * 10 end.to_a()   # => [10, 20]
```

### `length() -> Int`

The number of entries.

```ruby
{"a": 1}.length()   # => 1
```

### `map() do |key, value| ... end -> Array`

### `map() do |value| ... end -> Array`

The block's value for each entry.

```ruby
{"a": 1, "b": 2}.map() do |k, v| "#{k}#{v}" end   # => ["a1", "b2"]
```

### `map_values() do |value| ... end -> Hash`

The same keys with each value replaced by the block's value.

```ruby
{"a": 1, "b": 2}.map_values() do |v| v * 2 end   # => {"a": 2, "b": 4}
```

### `max_by() do |key, value| ... end -> Array`

The `[key, value]` with the largest block value.

```ruby
{"a": 1, "b": 2}.max_by() do |k, v| v end   # => ["b", 2]
```

### `member?(key) -> Bool`

The same as `include_key?`.

### `merge(other) -> Hash`

A new Hash with `other`'s entries added; `other` wins where both have a key.
The receiver does not change.

```ruby
{"a": 1, "b": 2}.merge({"b": 3, "c": 4})   # => {"a": 1, "b": 3, "c": 4}
```

### `min_by() do |key, value| ... end -> Array`

The `[key, value]` with the smallest block value.

```ruby
{"a": 1, "b": 2}.min_by() do |k, v| v end   # => ["a", 1]
```

### `nil?() -> Bool`

Always `false` for a `Hash`.

### `none?() do |key, value| ... end -> Bool`

Whether the block is false for every entry. Without a block, whether the Hash
is empty.

```ruby
{"a": 1}.none?() do |k, v| v > 5 end   # => true
```

### `partition() do |key, value| ... end -> Array[Hash]`

`[matching, not_matching]`, each a `Hash`.

```ruby
{"a": 1, "b": 2}.partition() do |k, v| v > 1 end   # => [{"b": 2}, {"a": 1}]
```

### `reduce(initial) do |memo, value| ... end -> Value`

Folds the values, starting with `initial`.

```ruby
{"a": 1, "b": 2}.reduce(0) do |sum, v| sum + v end   # => 3
```

### `reject() do |key, value| ... end -> Hash`

The entries for which the block is false.

```ruby
{"a": 1, "b": 2}.reject() do |k, v| v > 1 end   # => {"a": 1}
```

### `select() do |key, value| ... end -> Hash`

### `select() do |value| ... end -> Array`

With a two-parameter block, the entries for which it is true, as a `Hash`.
With a one-parameter block, the matching values, as an `Array`.

```ruby
{"a": 1, "b": 2}.select() do |k, v| v > 1 end   # => {"b": 2}
{"a": 1, "b": 2}.select() do |v| v > 1 end      # => [2]
```

### `size() -> Int`

The same as `length()`.

### `slice(key, ...) -> Hash`

Only the entries for the given keys, up to six.

```ruby
{"a": 1, "b": 2}.slice("a")   # => {"a": 1}
```

### `sort() -> Array[Array]`

The `[key, value]` pairs sorted by key.

```ruby
{"b": 1, "a": 2}.sort()   # => [["a", 2], ["b", 1]]
```

### `sort_by() do |key, value| ... end -> Array[Array]`

The `[key, value]` pairs ordered by block value.

```ruby
{"a": 1, "b": 2}.sort_by() do |k, v| -v end   # => [["b", 2], ["a", 1]]
```

### `sum() -> Value`

### `sum() do |key, value| ... end -> Value`

The sum of the values, or of the block's values.

```ruby
{"a": 1, "b": 2}.sum()   # => 3
```

### `tap() do |value| ... end -> Hash`

Calls the block with the receiver and returns the receiver.

### `to_a() -> Array[Array]`

The `[key, value]` pairs.

```ruby
{"a": 1, "b": 2}.to_a()   # => [["a", 1], ["b", 2]]
```

### `to_h() -> Hash`

The receiver.

### `to_s() -> String`

The entries as text, with keys and values shown as interpolation shows them.

```ruby
{"a": 1, "b": 2}.to_s()   # => "{a: 1, b: 2}"
```

### `transform_keys() do |key| ... end -> Hash`

Each key replaced by the block's value.

```ruby
{"a": 1}.transform_keys() do |k| k.upcase() end   # => {"A": 1}
```

### `transform_values() do |value| ... end -> Hash`

The same as `map_values`.

```ruby
{"a": 1}.transform_values() do |v| v + 1 end   # => {"a": 2}
```

### `update(other) -> Hash`

Adds `other`'s entries to the receiver, in place, and returns it.

```ruby
h = {"a": 1}
h.update({"b": 2})
h   # => {"a": 1, "b": 2}
```

### `value?(value) -> Bool`

Whether some entry has this value.

```ruby
{"a": 1, "b": 2}.value?(2)   # => true
```

### `value_at(index) -> Value`

The value at position `index` in insertion order.

```ruby
{"a": 1, "b": 2}.value_at(0)   # => 1
```

### `values() -> Array`

The values in insertion order.

```ruby
{"a": 1, "b": 2}.values()   # => [1, 2]
```

### `values_at(key, ...) -> Array`

The values for the given keys, up to six; a missing key gives `nil`.

```ruby
{"a": 1}.values_at("a", "z")   # => [1, nil]
```
