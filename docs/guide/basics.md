# Basics

This chapter covers the pieces every program uses: values, variables,
strings, and how to call things and print results.

## Running code

Save code in a file ending in `.d6` and run it with `d6`:

```sh
d6 hello.d6
```

Run `d6` with no arguments for a REPL that prints each line's value, or
`d6 -e 'CODE'` to run one line from the shell. A script prints only what it
writes with `puts`, `print` or `p`; the REPL also shows each value for you.

## Values and variables

A variable comes into existence when you first assign to it. There is no
`let`, `var` or declaration keyword, and a local variable has no type
annotation: its type is whatever you assign.

```ruby
count = 3
price = 2.5
name = "Ada"
ready = true
missing = nil

puts(count * price)   # => 7.5
puts(name)            # => Ada
```

The built-in value types are `Int`, `Float`, `String`, `Bool` (`true` and
`false`), `Nil` (the single value `nil`), `Symbol`, and the collections in
the [next chapters](collections.md). `.class()` tells you which one you have:

```ruby
puts(42.class())        # => Int
puts(4.2.class())       # => Float
puts("hi".class())      # => String
puts(:ready.class())    # => Symbol
```

A name that starts with a capital letter is a constant. Assigning to it a
second time is a compile error, not a warning.

```ruby
MAX_RETRIES = 3
puts(MAX_RETRIES)     # => 3
```

## Numbers

`Int` has no fixed size: arithmetic that outgrows 64 bits keeps going as a
big integer, with the same type.

```ruby
puts(2 ** 10)          # => 1024
puts(2 ** 100)         # => 1267650600228229401496703205376
puts((2 ** 100).class())   # => Int
puts(1_000_000 + 1)    # => 1000001
```

Dividing two `Int`s gives an `Int`. Use a `Float` on either side, or
`fdiv`, for a fractional answer. Mixing `Int` and `Float` gives a `Float`.

```ruby
puts(7 / 2)        # => 3
puts(7.0 / 2)      # => 3.5
puts(7.fdiv(2))    # => 3.5
puts(7 % 3)        # => 1
puts(3 + 0.5)      # => 3.5
```

Integer division by zero raises `ZeroDivisionError`; `Float` division by zero
follows IEEE-754 and gives `Infinity` or `NaN`. Converting between the two
is always explicit: `3.7.to_i()` is `3`, `3.to_f()` is `3.0`.

## Strings

Strings are written in double quotes, and `#{...}` inside one interpolates
any expression. There are no single-quoted strings.

```ruby
name = "Ada"
born = 1815
puts("#{name} was born in #{born}.")   # => Ada was born in 1815.
puts("tab:\tafter")                    # => tab:	after
```

Strings are immutable. Methods like `upcase` return a new string, and
`+` builds a new one:

```ruby
word = "hello"
loud = word.upcase()
puts(loud)          # => HELLO
puts(word)          # => hello
puts(word + "!")    # => hello!
```

`+` only joins a String to another String. Convert other values with
`to_s()`, or use interpolation, which converts for you:

```ruby
age = 36
puts("age " + age.to_s())   # => age 36
puts("age #{age}")          # => age 36
```

A few common methods:

```ruby
line = "  d6, ruby, python  "
puts(line.strip())                  # => d6, ruby, python
puts(line.strip().split(", "))      # => [d6, ruby, python]
puts("ab" * 3)                      # => ababab
puts("d6".include?("6"))            # => true
puts("hello".start_with?("he"))     # => true
puts("42".to_i() + 1)               # => 43
```

`length()` counts bytes, so a string with non-ASCII characters is longer
than the number of characters you see. The
[collections reference](../collections.md) lists every String method.

## Calling things

Every call has parentheses, even with no arguments:

```ruby
puts("hello".upcase())    # a call
puts([3, 1, 2].sort())    # => [1, 2, 3]
```

This is not just style. `"hello".upcase` without parentheses is a
*reference* to the method, a value you can pass around and call later. You
will meet method references in [Functions](functions.md).

## Printing

- `puts(value)` prints a value and a newline. It takes exactly one argument.
- `print(value)` prints without the newline.
- `p(value)` prints the value's `inspect()` form, which shows strings in
  quotes. Use it when you are debugging.

```ruby
puts("ada")         # => ada
p("ada")            # => "ada"
p([1, "two", nil])  # => [1, "two", nil]
```

`puts(nil)` prints `nil`, not an empty line.

## Comments

`#` starts a comment that runs to the end of the line. There is no block
comment syntax.

## Truthiness

Only `false` and `nil` are false. Everything else is true, including `0`,
`""` and `[]`:

```ruby
puts(0 ? "true" : "false")     # => true
puts("" ? "true" : "false")    # => true
puts(nil ? "true" : "false")   # => false
```

`a || b` gives `a` unless it is `false` or `nil`, which makes it the usual
way to supply a default. `x ||= value` assigns only when `x` is `nil` or
`false`.

```ruby
nickname = nil
puts(nickname || "anonymous")   # => anonymous
nickname ||= "ada"
puts(nickname)                  # => ada
```

## Coming from another language

- **Python**: `nil` is `None`, `true`/`false` are lowercase, and `0`, `""`
  and empty collections are *true*. Blocks end with `end`, not a dedent.
- **JavaScript**: there is no `undefined`, no `===`, and no implicit
  conversion: `"1" + 1` raises an error instead of producing `"11"`.
- **Ruby**: calls need parentheses, strings need double quotes, and strings
  are immutable, so there is no `<<` or `gsub!`.

Next: [Control flow](control-flow.md).
