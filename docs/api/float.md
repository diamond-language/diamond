# Float

[API reference](index.md) · Previous: [Int](int.md)

A `Float` is a 64-bit IEEE 754 floating-point number. Literals need a digit on
both sides of the point (`1.0`, `0.5`). Dividing by zero gives `Infinity` or
`NaN` rather than raising.

## Operators

| Operator | Result |
|---|---|
| `+`, `-`, `*`, `/` | a `Float`; an `Int` on the other side is converted |
| `a % b` | the remainder, with the sign of `b` |
| `a ** b` | a `Float` |
| `==`, `<`, `<=`, `>`, `>=` | numeric comparison, also against an `Int` |
| `a <=> b` | `-1`, `0` or `1` |

```ruby
7.0 / 2         # => 3.5
2.0 ** 0.5      # => 1.4142135623730951
1.0 / 0.0       # => Infinity
```

## Methods

### `abs() -> Float`

The absolute value.

```ruby
-2.5.abs()   # => 2.5
```

### `ago() -> Time`

The `Time` this many seconds before now.

### `between?(min, max) -> Bool`

Whether the receiver is between `min` and `max`, both included.

```ruby
1.5.between?(1, 2)   # => true
```

### `ceil() -> Int`

### `ceil(digits) -> Float`

Rounded up to a whole number, or to `digits` decimal places.

```ruby
2.1.ceil()      # => 3
2.123.ceil(1)   # => 2.2
```

### `clamp(min, max) -> Float`

The receiver limited to the range `min..max`.

```ruby
1.5.clamp(2.0, 3.0)   # => 2.0
```

### `day() -> Float`

### `days() -> Float`

The receiver as a number of days, in seconds.

```ruby
0.5.days()   # => 43200.0
```

### `deep_freeze() -> Float`

The receiver; numbers are always immutable.

### `divmod(other) -> Array`

`[quotient, remainder]`, with the quotient rounded down to a whole number.

```ruby
7.5.divmod(2)   # => [3, 1.5]
```

### `dup() -> Float`

The receiver.

### `equal?(other) -> Bool`

Whether `other` is a `Float` with the same value; `NaN` is `equal?` to itself.

### `fdiv(other) -> Float`

The same as `/`.

### `finite?() -> Bool`

Whether the receiver is neither infinite nor `NaN`.

```ruby
1.0.finite?()   # => true
```

### `floor() -> Int`

### `floor(digits) -> Float`

Rounded down to a whole number, or to `digits` decimal places.

```ruby
2.9.floor()       # => 2
2.987.floor(2)    # => 2.98
```

### `freeze() -> Float`

The receiver.

### `from_now() -> Time`

The `Time` this many seconds after now.

### `frozen?() -> Bool`

Always `true`.

### `hour() -> Float`

### `hours() -> Float`

The receiver as a number of hours, in seconds.

```ruby
1.5.hours()   # => 5400.0
```

### `infinite?() -> Bool`

Whether the receiver is positive or negative infinity.

```ruby
(1.0 / 0.0).infinite?()   # => true
```

### `inspect() -> String`

The same as `to_s()`.

### `minute() -> Float`

### `minutes() -> Float`

The receiver as a number of minutes, in seconds.

```ruby
1.5.minutes()   # => 90.0
```

### `nan?() -> Bool`

Whether the receiver is `NaN`.

```ruby
(0.0 / 0.0).nan?()   # => true
```

### `negative?() -> Bool`

Whether the receiver is below zero.

### `nil?() -> Bool`

Always `false` for a `Float`.

### `positive?() -> Bool`

Whether the receiver is above zero.

### `pow(exponent) -> Float`

The same as `**`.

### `round() -> Int`

### `round(digits) -> Float`

Rounded to the nearest whole number (halves away from zero), or to `digits`
decimal places.

```ruby
2.5.round()        # => 3
-2.5.round()       # => -3
3.14159.round(2)   # => 3.14
```

### `second() -> Float`

### `seconds() -> Float`

The receiver, read as seconds.

### `tap() do |value| ... end -> Float`

Calls the block with the receiver and returns the receiver.

### `to_f() -> Float`

The receiver.

### `to_i() -> Int`

The whole part, dropping the fraction.

```ruby
2.9.to_i()   # => 2
```

### `to_s() -> String`

The shortest text that reads back as the same `Float`.

```ruby
2.5.to_s()   # => "2.5"
1.0.to_s()   # => "1.0"
```

### `truncate() -> Int`

The whole part, rounding toward zero.

```ruby
-2.7.truncate()   # => -2
```

### `week() -> Float`

### `weeks() -> Float`

The receiver as a number of weeks, in seconds.

### `zero?() -> Bool`

Whether the receiver is zero.

```ruby
0.0.zero?()   # => true
```
