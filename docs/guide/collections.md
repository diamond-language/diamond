# Collections and blocks

D6 has three everyday collections: `Array`, `Hash` and `Range`. You will
mostly work with them through methods that take a *block*, a piece of code
written `do |...| ... end` that the method calls for you.

## Arrays

An array is an ordered list. Indexes start at 0, and a negative index counts
from the end.

```ruby
langs = ["d6", "ruby", "python"]
puts(langs[0])          # => d6
puts(langs[-1])         # => python
puts(langs.length())    # => 3
```

Reading past either end raises an `IndexError` rather than giving `nil`, so
check `length()` (or `empty?()`) when an index might be out of range.

Arrays grow and shrink in place:

```ruby
stack = [1, 2]
stack.push(3)
p(stack)          # => [1, 2, 3]
p(stack.pop())    # => 3
p(stack)          # => [1, 2]
```

A range inside the brackets gives a slice, and `first(n)` the first few:

```ruby
nums = [10, 20, 30, 40]
p(nums[1..2])       # => [20, 30]
p(nums.first(2))    # => [10, 20]
```

Assigning an array to a second variable does not copy it. Both names refer
to the same array, so a change through one is visible through the other:

```ruby
a = [1]
b = a
b.push(2)
p(a)   # => [1, 2]
```

Use `a.dup()` when you want an independent copy.

## Blocks

A block is the `do |params| ... end` after a call. The method decides when
to run it and what to pass in. `each` runs it once per element:

```ruby
["ada", "grace"].each() do |name|
  puts(name.upcase())
end
# => ADA
```

Many methods use the block's value to build a result. These are the ones you
will reach for most often:

```ruby
nums = [1, 2, 3, 4, 5, 6]

p(nums.map() do |n| n * n end)                # => [1, 4, 9, 16, 25, 36]
p(nums.select() do |n| n.even?() end)         # => [2, 4, 6]
p(nums.reject() do |n| n.even?() end)         # => [1, 3, 5]
p(nums.find() do |n| n > 3 end)               # => 4
p(nums.any?() do |n| n > 5 end)               # => true
p(nums.all?() do |n| n > 0 end)               # => true
p(nums.count() do |n| n.odd?() end)           # => 3
p(nums.sum())                                 # => 21
p(nums.reduce(1) do |product, n| product * n end)   # => 720
```

A block's value is its last line. Calls chain, so a pipeline reads top to
bottom:

```ruby
words = ["kiwi", "fig", "banana", "apple"]
long = words.select() do |w| w.length() > 3 end
p(long.map() do |w| w.upcase() end.sort())   # => ["APPLE", "BANANA", "KIWI"]
```

Some blocks take two parameters, like `each_with_index`:

```ruby
["a", "b"].each_with_index() do |letter, index|
  puts("#{index}: #{letter}")
end
# => 0: a
```

`sort_by` and `group_by` take a block that picks out what to sort or group
by:

```ruby
people = ["Grace", "Ada", "Linus"]
p(people.sort_by() do |name| name.length() end)   # => ["Ada", "Grace", "Linus"]
p(people.group_by() do |name| name.length() end)  # => {5: ["Grace", "Linus"], 3: ["Ada"]}
```

Blocks are always `do ... end`. There is no `{ |x| ... }` form, because
braces always mean a Hash.

Inside a block, `next value` ends this call of the block with `value`, and
`break value` stops the whole method call and makes `value` its result.

## Hashes

A hash maps keys to values. Its literal uses `key: value` with quoted string
keys:

```ruby
ages = {"ada": 36, "grace": 85}
puts(ages["ada"])              # => 36
p(ages["linus"])               # => nil
puts(ages.fetch("linus", 0))   # => 0
ages["linus"] = 54
puts(ages.length())            # => 3
```

A missing key gives `nil`; `fetch(key, default)` gives a default instead.
Hashes keep their keys in the order they were added. Blocks on a hash take
the key and the value:

```ruby
stock = {"apples": 3, "pears": 0, "plums": 7}

stock.each() do |fruit, count|
  puts("#{fruit}: #{count}")
end
# => apples: 3

in_stock = stock.select() do |fruit, count| count > 0 end
p(in_stock.keys())      # => ["apples", "plums"]
p(stock.include_key?("pears"))   # => true
```

Keys can be any value, including numbers and arrays. Write a variable or
expression as the key with `=>`: `{name => 1}` uses the value of `name`,
while `{"name": 1}` uses the string `"name"`.

## Ranges

`a..b` includes both ends and `a...b` stops before `b`. Ranges work with
`each`, `map` and the other block methods directly, and `to_a()` turns one
into an array:

```ruby
p((1..5).to_a())                     # => [1, 2, 3, 4, 5]
p((1...5).to_a())                    # => [1, 2, 3, 4]
p((1..4).map() do |n| n * 10 end)    # => [10, 20, 30, 40]
p((0..10).step(5).to_a())            # => [0, 5, 10]
```

## Unpacking

Several variables can be assigned at once, which also swaps values without a
temporary. A `*` name collects the rest:

```ruby
first, second = 1, 2
first, second = second, first
p([first, second])         # => [2, 1]

[head, *tail] = [1, 2, 3]
p(tail)                    # => [2, 3]
```

## Coming from another language

- **Python**: list comprehensions become `map` and `select` with blocks.
  `{"a": 1}` reads the same, but `len(x)` is `x.length()`, and slicing uses
  a range: `nums[1..2]`, not `nums[1:3]`.
- **JavaScript**: arrow functions passed to `map` and `filter` become
  blocks; `filter` is called `select`.
- **Ruby**: blocks are `do ... end` only, and a hash literal's `"key": value`
  makes a *string* key, not a symbol. `array[i]` past the end raises
  `IndexError` instead of returning `nil`. `inject` without a starting value and
  `take_while` are not available; use `reduce(start)` and a loop with
  `break`.

The [collections reference](../collections.md) lists every method on
`Array`, `Hash`, `String` and `Range`.

Next: [Functions](functions.md).
