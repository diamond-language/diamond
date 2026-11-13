# Functions

Functions are defined with `def` and called with parentheses. The last
expression in the body is the return value, so most functions never write
`return`.

## Defining and calling

```ruby
def greeting(name)
  "Hello, #{name}!"
end

puts(greeting("Ada"))   # => Hello, Ada!
```

A function whose body is a single expression can be written on one line
with `=`:

```ruby
def square(n) = n * n
puts(square(7))   # => 49
```

`return` still exists for leaving early:

```ruby
def sign(n)
  return "zero" if n == 0
  n > 0 ? "positive" : "negative"
end

puts(sign(-4))   # => negative
```

## Types on parameters and results

Parameters and results can carry types: `name: Type` on a parameter and
`-> Type` after the parameter list. They are optional, and you can annotate
some parameters and not others.

```ruby
def area(width: Int, height: Int) -> Int = width * height
puts(area(3, 4))   # => 12
```

With types written down, the compiler rejects a call it can see is wrong
before the program starts, and the runtime checks values whose type it
could not know in advance. [Types](types.md) explains exactly what is
checked when.

## Defaults and named arguments

A parameter can have a default, and a default can use the parameters before
it:

```ruby
def box(width, height = width, label = "box")
  "#{label}: #{width}x#{height}"
end

puts(box(3))                      # => box: 3x3
puts(box(3, 4))                   # => box: 3x4
```

Any argument can be passed by name. Named arguments can come in any order
after the positional ones, and can skip over defaults:

```ruby
def box(width, height = width, label = "box")
  "#{label}: #{width}x#{height}"
end

puts(box(2, label: "tile"))           # => tile: 2x2
puts(box(height: 5, width: 1))        # => box: 1x5
```

There is no separate keyword-parameter syntax: every parameter can be named
at the call site. (Inside a parameter list, `name: Type` is a type
annotation, not a keyword parameter.)

A `*` parameter collects any number of arguments into an array, and `*` at
a call site spreads an array into arguments:

```ruby
def total(*amounts) = amounts.sum()
puts(total(1, 2, 3))     # => 6

prices = [10, 20]
puts(total(*prices))     # => 30
```

## Functions are values

A function's name without parentheses is the function itself, which you can
pass to another function and call later. A method on an object works the
same way:

```ruby
def double(n) = n * 2

def apply_twice(f, value) = f(f(value))
puts(apply_twice(double, 5))     # => 20

shout = "quiet".upcase
puts(shout())                    # => QUIET
```

A function can also stand in for a block, which is handy when the block
would only call it:

```ruby
def double(n) = n * 2
p([1, 2, 3].map(double))         # => [2, 4, 6]
```

To say a parameter is a function, use `Callable`. `Callable[[Int], Int]`
takes one `Int` and returns an `Int`:

```ruby
def apply(f: Callable[[Int], Int], value: Int) -> Int = f(value)
def increment(n: Int) -> Int = n + 1
puts(apply(increment, 41))       # => 42
```

## Closures

A `def` inside another `def` is a closure: it can see the outer function's
local variables, and it keeps them alive after the outer function returns.
This is how D6 makes a function with private state:

```ruby
def make_counter()
  count = 0
  def next_value()
    count += 1
  end
  next_value
end

counter = make_counter()
counter()
counter()
puts(counter())     # => 3
```

Blocks are closures too: a block can read and assign the variables around it.

```ruby
total = 0
[5, 10, 15].each() do |n| total += n end
puts(total)         # => 30
```

## Writing a function that takes a block

Name the block with a `&` parameter and call it like any function:

```ruby
def repeat(times, &action)
  times.times() do |i| action(i) end
end

repeat(2) do |i|
  puts("call #{i}")
end
# => call 0
```

`return` inside a block returns from the *function the block was written
in*, not just from the block, which makes early exits from a loop
natural:

```ruby
def first_negative(nums)
  nums.each() do |n|
    return n if n < 0
  end
  nil
end

p(first_negative([3, -1, -5]))   # => -1
```

## Coming from another language

- **Ruby**: there is no `yield` for blocks (in D6, `yield` suspends a
  [fiber](concurrency.md#fibers)). Take the block as `&block` and call
  `block(x)`. There is no `.call`: a callable is called with parentheses.
  There are no `lambda`/`proc` literals; a nested `def` is the closure.
- **Python**: there are no `lambda` expressions. Use a nested `def`, which,
  unlike Python's, can assign to the variables it captures without
  `nonlocal`.
- **JavaScript**: there are no arrow functions. Pass a block, or a named
  function, where you would pass a callback.

Next: [Classes and structs](classes.md).
