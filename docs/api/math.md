# Math

[API reference](index.md) · Previous: [StringBuilder](string_builder.md) · Next: [JSON](json.md)

`Math` holds the usual floating-point functions. Each takes an `Int` or a
`Float` and returns a `Float`. Most are also plain functions: `sqrt(2)` is
`Math.sqrt(2)`.

```ruby
Math.sqrt(16)                 # => 4.0
sqrt(2)                       # => 1.4142135623730951
Math.hypot(3, 4)              # => 5.0
```

## Functions

### `Math.cbrt(x) -> Float`

The cube root.

```ruby
Math.cbrt(27)   # => 3.0
```

### `Math.cos(x) -> Float`

The cosine of `x` radians.

```ruby
Math.cos(0)   # => 1.0
```

### `Math.e() -> Float`

Euler's number.

```ruby
Math.e()   # => 2.718281828459045
```

### `Math.exp(x) -> Float`

`e` raised to `x`.

```ruby
Math.exp(1)   # => 2.718281828459045
```

### `Math.hypot(x, y) -> Float`

The length of the hypotenuse, `sqrt(x * x + y * y)`.

### `Math.log(x) -> Float`

The natural logarithm.

```ruby
Math.log(Math.e())   # => 1.0
```

### `Math.log10(x) -> Float`

The base-10 logarithm, exact for powers of ten.

```ruby
Math.log10(1000)   # => 3.0
```

### `Math.log2(x) -> Float`

The base-2 logarithm, exact for powers of two.

```ruby
Math.log2(8)   # => 3.0
```

### `Math.pi() -> Float`

π.

```ruby
Math.pi()   # => 3.141592653589793
```

### `Math.sin(x) -> Float`

The sine of `x` radians.

```ruby
Math.sin(0)   # => 0.0
```

### `Math.sqrt(x) -> Float`

The square root.

### `Math.tan(x) -> Float`

The tangent of `x` radians.

### `Math.tanh(x) -> Float`

The hyperbolic tangent.

```ruby
Math.tanh(0)   # => 0.0
```
