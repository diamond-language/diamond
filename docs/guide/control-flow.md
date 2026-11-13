# Control flow

D6's control structures will look familiar. Every one of them ends with
`end`, and every one of them is an *expression*: it has a value you can
assign or return.

## if, elsif, else

```ruby
temperature = 31

if temperature > 30
  puts("hot")
elsif temperature > 15
  puts("mild")
else
  puts("cold")
end
# => hot
```

Because `if` is an expression, you can assign its result. The value is the
last line of whichever branch ran, or `nil` if no branch ran.

```ruby
temperature = 12
feeling = if temperature > 15 then "warm" else "chilly" end
puts(feeling)   # => chilly
```

`then` lets a branch sit on the same line as its condition. For a short
choice there is also the ternary operator:

```ruby
count = 1
puts(count == 1 ? "1 item" : "#{count} items")   # => 1 item
```

`unless` is `if` with the condition inverted. A single statement can take
`if` or `unless` *after* it, which reads well for guards:

```ruby
balance = 40
warning = "low balance" if balance < 50
puts(warning)                          # => low balance
puts("overdrawn") unless balance >= 0  # prints nothing
```

Combine conditions with `&&`, `||` and `!`. Comparison uses `==`, `!=`, `<`,
`<=`, `>`, `>=`. Equality works on any values, including whole arrays and
hashes; ordering works on numbers and on strings.

## case and when

`case` compares one value against each `when` in turn. A `when` can list
several values, a range, or a class:

```ruby
def describe(score)
  case score
  when 100
    "perfect"
  when 90..99
    "excellent"
  when 50, 60, 70
    "a round number"
  else
    "something else"
  end
end

puts(describe(100))   # => perfect
puts(describe(93))    # => excellent
puts(describe(60))    # => a round number
```

`when` also takes patterns that check the shape of arrays and hashes and pull
values out of them:

```ruby
def handle(event)
  case event
  when ["move", x, y]
    "move to #{x},#{y}"
  when {"type": "say", "text": text}
    "say #{text}"
  else
    "unknown"
  end
end

puts(handle(["move", 3, 4]))                       # => move to 3,4
puts(handle({"type": "say", "text": "hello"}))     # => say hello
puts(handle(42))                                   # => unknown
```

A lowercase name inside a pattern binds that part of the value for the
branch. Patterns can nest, take an `if` guard, and match objects by class;
[case/when in the reference](../core-syntax.md#casewhen) has the details,
including how the compiler checks that a `case` over a closed set of classes
covers every one of them.

## Loops

`while` and `until` loop on a condition:

```ruby
n = 1
while n < 100
  n *= 2
end
puts(n)   # => 128
```

Most of the time you will not write a loop counter at all. You will call a
method that takes a block, which the [next chapter](collections.md) covers:

```ruby
3.times() do |i|
  puts("round #{i}")
end
# => round 0
```

`loop do ... end` repeats until a `break`. `break` can carry a value, which
becomes the value of the whole loop. `next` skips to the next iteration.

```ruby
attempts = 0
result = loop do
  attempts += 1
  next if attempts < 3
  break "succeeded after #{attempts} attempts"
end
puts(result)   # => succeeded after 3 attempts
```

A one-line loop can put the condition after the statement:

```ruby
countdown = 3
countdown -= 1 while countdown > 0
puts(countdown)   # => 0
```

There is no `for` loop. Iterate a range or a collection with `each`.

## Coming from another language

- **Python**: `elif` is `elsif`, and there is no `for x in y`: write
  `y.each() do |x| ... end`. `case` with patterns plays the role of `match`.
- **JavaScript**: `switch` is `case`, and branches do not fall through, so
  there is no `break` between `when`s.
- **Ruby**: this is very close to Ruby. The differences are that `case`
  patterns are written inline in `when` (there is no `in`), and the
  modifier form of `while` only works on a single statement, not on a
  `begin ... end` block.

Next: [Collections and blocks](collections.md).
