# Kernel functions

[API reference](index.md) · Next: [Object](object.md)

These functions are called by name, without a receiver. A local variable or a
function of the same name takes precedence, so a program can shadow any of
them.

`raise`, `require`, `yield` and `return` are keywords, described in the
[language reference](../core-syntax.md). There is no `sleep`: wait with
`Channel.new(1).wait_readable([], Time.monotonic() + seconds)`.

## Output and input

### `print(value)`

Writes `value` to standard output, as interpolation shows it, with no newline.

### `puts(value)`

Writes `value` and a newline to standard output. It takes one argument; an
Array prints as its `to_s` on one line, and `nil` prints as `nil`.

```ruby
puts("hello")
puts([1, 2])   # prints [1, 2]
```

### `p(value) -> Value`

Writes `value.inspect()` and a newline to standard output, then returns
`value`, so it can wrap an expression while debugging.

```ruby
total = p(1 + 2)   # prints 3
total              # => 3
```

### `warn(value)`

Writes `value` and a newline to standard error.

### `gets() -> String | Nil`

Reads one line from standard input without its line ending (`\n` or `\r\n`),
or returns `nil` at end of input.

## Program control

### `exit(status = 0)`

Ends the whole process at once with `status`, an `Int` from 0 to 255. Unlike
Ruby's `exit` it raises nothing: no `ensure` block runs and every other thread
stops. Another status raises `ArgumentError`, and a non-`Int` raises
`TypeError`.

### `block_given?() -> Bool`

Inside a method that declares `&block`, whether the caller passed a block.

```ruby
def greet(&block)
  block_given?() ? yield("Ada") : "hello"
end

greet()                          # => "hello"
greet() do |name| "hi #{name}" end   # => "hi Ada"
```

### `debugger()`

### `breakpoint()`

Pauses here when the program runs under the step debugger, and does nothing
otherwise. See [debugging](../debugging.md).

## Conversions

### `chr(code) -> String`

The one-byte `String` for `code`, the same as `code.chr()`.

```ruby
chr(65)   # => "A"
```

### `to_f(int) -> Float`

An `Int` as a `Float`. Any other argument raises `TypeError`; use
`String#to_f` to parse text.

```ruby
to_f(3)   # => 3.0
```

### `to_i(float) -> Int`

A `Float` without its fraction, rounding toward zero. Any other argument
raises `TypeError`; use `String#to_i` to parse text.

```ruby
to_i(-2.9)   # => -2
```

### `to_sym(string) -> Symbol`

The `Symbol` with this name.

```ruby
to_sym("ok") == :ok   # => true
```

## Numbers

### `abs(x) -> Int | Float`

The absolute value of an `Int` or `Float`.

```ruby
abs(-3)   # => 3
```

### `max(a, b) -> Int | Float`

The larger of two numbers.

```ruby
max(1, 2.5)   # => 2.5
```

### `min(a, b) -> Int | Float`

The smaller of two numbers.

```ruby
min(1, 2.5)   # => 1
```

### `mod(a, b) -> Int | Float`

The remainder of `a / b` with the sign of `a`. The `%` operator instead takes
the sign of `b`.

```ruby
mod(-7, 3)   # => -1
-7 % 3       # => 2
```

### `pow(base, exponent) -> Float`

`base` raised to `exponent`, always as a `Float`. `Int#pow` and `**` keep an
`Int` result.

```ruby
pow(2, 3)   # => 8.0
```

### `sqrt(x)`, `sin(x)`, `cos(x)`, `tan(x)`, `exp(x)`, `log(x)`, `tanh(x)`

The same as the [Math](math.md) functions of those names.

```ruby
sqrt(9)   # => 3.0
```
