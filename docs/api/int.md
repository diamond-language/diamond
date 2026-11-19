# Int

[API reference](index.md) · Previous: [Hash](hash.md) · Next: [Float](float.md)

An `Int` is a whole number. Ints have no fixed size: a result past 64 bits
becomes a big integer instead of overflowing, so `2 ** 100` is exact.
Literals are written `42`, `1_000_000`, `0xff`, `0o17` or `0b101`.

## Operators

| Operator | Result |
|---|---|
| `+`, `-`, `*` | an `Int`, or a `Float` when the other side is one |
| `a / b` | an `Int` rounded toward negative infinity; a `Float` if either side is one. Dividing by `0` raises `ZeroDivisionError` |
| `a % b` | the remainder, with the sign of `b` |
| `a ** b` | exact for a non-negative `Int` exponent, a `Float` otherwise |
| `&`, `\|`, `^`, `<<`, `>>` | bitwise and, or, exclusive or, and shifts |
| `==`, `<`, `<=`, `>`, `>=` | numeric comparison, also against a `Float` |
| `a <=> b` | `-1`, `0` or `1` |

```ruby
7 / 2         # => 3
-7 / 2        # => -4
-7 % 3        # => 2
7 / 2.0       # => 3.5
2 ** 10       # => 1024
2 ** 100      # => 1267650600228229401496703205376
6 & 3         # => 2
1 << 4        # => 16
```

## Methods

### `abs() -> Int`

The absolute value.

```ruby
-5.abs()   # => 5
```

### `ago() -> Time`

The `Time` this many seconds before now: `5.minutes().ago()`.

### `between?(min, max) -> Bool`

Whether the receiver is between `min` and `max`, both included.

```ruby
5.between?(1, 10)   # => true
```

### `chr() -> String`

The one-byte `String` with this code, from `0` to `255`; anything else raises
`RangeError`. `String#ord` is the inverse.

```ruby
65.chr()   # => "A"
```

### `clamp(min, max) -> Int`

The receiver limited to the range `min..max`.

```ruby
15.clamp(1, 10)   # => 10
```

### `day() -> Int`

### `days() -> Int`

The receiver as a number of days, in seconds.

```ruby
2.days()   # => 172800
```

### `deep_freeze() -> Int`

The receiver; numbers are always immutable.

### `digits() -> Array[Int]`

The decimal digits, least significant first.

```ruby
123.digits()   # => [3, 2, 1]
```

### `divmod(other) -> Array`

`[quotient, remainder]`, as `/` and `%` give them.

```ruby
7.divmod(2)   # => [3, 1]
```

### `downto(limit) do |i| ... end -> Int`

Calls the block with each number from the receiver down to `limit`.

```ruby
seen = []
3.downto(1) do |i| seen.push(i) end
seen   # => [3, 2, 1]
```

### `dup() -> Int`

The receiver.

### `even?() -> Bool`

Whether the receiver is divisible by 2.

```ruby
4.even?()   # => true
```

### `fdiv(other) -> Float`

Division as Floats.

```ruby
7.fdiv(2)   # => 3.5
```

### `freeze() -> Int`

The receiver.

### `from_now() -> Time`

The `Time` this many seconds after now: `1.hour().from_now()`.

### `frozen?() -> Bool`

Always `true`.

### `gcd(other) -> Int`

The greatest common divisor.

```ruby
12.gcd(18)   # => 6
```

### `hour() -> Int`

### `hours() -> Int`

The receiver as a number of hours, in seconds.

```ruby
2.hours()   # => 7200
```

### `inspect() -> String`

The same as `to_s()`.

### `lcm(other) -> Int`

The least common multiple.

```ruby
4.lcm(6)   # => 12
```

### `minute() -> Int`

### `minutes() -> Int`

The receiver as a number of minutes, in seconds.

```ruby
5.minutes()   # => 300
```

### `negative?() -> Bool`

Whether the receiver is below zero.

```ruby
-1.negative?()   # => true
```

### `nil?() -> Bool`

Always `false` for an `Int`.

### `odd?() -> Bool`

Whether the receiver is not divisible by 2.

```ruby
3.odd?()   # => true
```

### `positive?() -> Bool`

Whether the receiver is above zero.

```ruby
1.positive?()   # => true
```

### `pow(exponent) -> Int | Float`

The same as `**`.

```ruby
2.pow(10)   # => 1024
2.pow(-1)   # => 0.5
```

### `pred() -> Int`

The receiver minus one.

```ruby
5.pred()   # => 4
```

### `second() -> Int`

### `seconds() -> Int`

The receiver, read as seconds; for symmetry with `minutes()` and the rest.

```ruby
30.seconds()   # => 30
```

### `step(limit, by) do |i| ... end -> Int`

Calls the block with the receiver, then every `by` after it, up to `limit`.
A negative `by` counts down.

```ruby
seen = []
1.step(10, 3) do |i| seen.push(i) end
seen   # => [1, 4, 7, 10]
```

### `succ() -> Int`

The receiver plus one.

```ruby
1.succ()   # => 2
```

### `tap() do |value| ... end -> Int`

Calls the block with the receiver and returns the receiver.

### `times() do |i| ... end -> Int`

Calls the block with each number from `0` below the receiver, and returns the
receiver.

```ruby
seen = []
3.times() do |i| seen.push(i) end
seen   # => [0, 1, 2]
```

### `to_f() -> Float`

The receiver as a `Float`.

```ruby
3.to_f()   # => 3.0
```

### `to_i() -> Int`

The receiver.

### `to_s(base = 10) -> String`

The receiver in `base`.

```ruby
255.to_s()     # => "255"
255.to_s(16)   # => "ff"
5.to_s(2)      # => "101"
```

### `upto(limit) do |i| ... end -> Int`

Calls the block with each number from the receiver up to `limit`.

```ruby
seen = []
1.upto(3) do |i| seen.push(i) end
seen   # => [1, 2, 3]
```

### `week() -> Int`

### `weeks() -> Int`

The receiver as a number of weeks, in seconds.

```ruby
1.week()   # => 604800
```

### `zero?() -> Bool`

Whether the receiver is zero.

```ruby
0.zero?()   # => true
```
