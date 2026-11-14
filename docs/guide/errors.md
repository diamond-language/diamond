# Errors

d6 uses exceptions: `raise` stops the current code and unwinds the call
stack until a `rescue` handles it. An exception nobody rescues ends the
program with a message and exit status 70.

## Raising

Raise an instance of an exception class with a message:

```ruby
def withdraw(balance: Int, amount: Int) -> Int
  raise ArgumentError.new("amount must be positive") if amount <= 0
  balance - amount
end

puts(withdraw(100, 30))   # => 70
```

The built-in classes include `ArgumentError`, `TypeError`, `IndexError`,
`ZeroDivisionError`, `RuntimeError` and `IOError`, all under
`StandardError`. Define your own by subclassing one:

```ruby
class NotFound < StandardError
end

def find_user(id: Int) -> String
  raise NotFound.new("no user #{id}") unless id == 1
  "Ada"
end
```

## Rescuing

`begin ... rescue ... end` catches exceptions raised inside it. Write
`rescue name: Type` to catch one class (and its subclasses) and bind the
exception to `name`:

```ruby
class NotFound < StandardError
end

def find_user(id: Int) -> String
  raise NotFound.new("no user #{id}") unless id == 1
  "Ada"
end

begin
  find_user(7)
rescue e: NotFound
  puts("lookup failed: #{e.message()}")
end
# => lookup failed: no user 7
```

A union catches several classes at once, and a `begin` block is an
expression, so it can produce a fallback value:

```ruby
def parse_ratio(text: String) -> Int
  parts = text.split("/")
  parts[0].to_i() / parts[1].to_i()
end

ratio = begin
  parse_ratio("10/0")
rescue e: ZeroDivisionError | IndexError
  0
end
puts(ratio)   # => 0
```

Several `rescue` clauses are tried in order. `rescue name` with no type
catches anything.

A `def` body, and a block body, can take `rescue` directly without a `begin`:

```ruby
def safe_divide(a: Int, b: Int) -> Int
  a / b
rescue e: ZeroDivisionError
  0
end

puts(safe_divide(10, 2))   # => 5
puts(safe_divide(1, 0))    # => 0
```

## ensure and retry

`ensure` runs on the way out no matter what: after a normal finish, after an
exception, and after a `return`. It is the place to release resources.

```ruby
def with_cleanup()
  puts("working")
  return "result"
ensure
  puts("cleaning up")
end

puts(with_cleanup())
# => working
```

That prints `working`, `cleaning up`, then `result`.

`retry` inside a `rescue` runs the `begin` block again from the top, for
operations worth another attempt.

Local values persist across attempts, including locals shared with blocks. A
retry does not undo earlier assignments. With implicit handlers on a method or
block body, it restarts that body instead.

```ruby
attempts = 0
result = begin
  attempts += 1
  raise RuntimeError.new("flaky") if attempts < 3
  "succeeded"
rescue e: RuntimeError
  retry if attempts < 3
  "gave up"
end
puts("#{result} after #{attempts} attempts")   # => succeeded after 3 attempts
```

## Raise exception objects, not strings

`raise` accepts any value, and that includes a plain string. Unlike Ruby,
`raise "boom"` raises the *string itself*, not a `RuntimeError`, so a
`rescue e: StandardError` does not catch it:

```ruby
caught = begin
  raise "boom"
rescue e: StandardError
  "caught by StandardError"
rescue other
  "caught a #{other.class()}"
end
puts(caught)   # => caught a String
```

Raise `RuntimeError.new("boom")`, or your own class, whenever callers
should be able to rescue by type.

## Errors from the language

The runtime raises the same exceptions for its own failures, so you rescue
them the same way: `ZeroDivisionError` for integer division by zero,
`IndexError` for an array index out of range, and `TypeError` when a value
fails a type annotation.

```ruby
begin
  [1, 2][5]
rescue e: IndexError
  puts(e.message())   # => index 5 out of bounds for Array of length 2
end
```

## Coming from another language

- **Ruby**: the syntax is `rescue e: Type`, not `rescue Type => e`, and
  `raise "message"` raises a String rather than a `RuntimeError`.
- **Python**: `try`/`except`/`finally` are `begin`/`rescue`/`ensure`, and
  `except ValueError as e` is `rescue e: ArgumentError`.
- **JavaScript**: `try`/`catch`/`finally` are `begin`/`rescue`/`ensure`,
  and `catch` can filter by class directly instead of with `instanceof`.

The [exceptions reference](../types-and-errors.md#exceptions) has the full
list of built-in exception classes.

Next: [Concurrency](concurrency.md).
