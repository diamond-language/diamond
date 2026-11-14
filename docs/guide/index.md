# Welcome to d6

d6 is a typed, object-oriented language with a syntax that will look
familiar if you have written Ruby, Python or JavaScript. You add type
annotations where you want them, and the compiler checks them before your
program runs. A register VM runs the result, `d6 build` turns a program into
a single executable, and threads share nothing: they copy values instead.

This guide is for people who already program in another language. It does
not explain what a variable or a loop is. It shows how d6 spells the things
you already know, and spends its time on the places where d6 differs from
what you would guess.

## How to read it

The chapters build on each other, so read them in order the first time:

1. [Basics](basics.md): values, variables, strings and calling things.
2. [Control flow](control-flow.md): conditions, loops and `case`, all of
   which are expressions.
3. [Collections and blocks](collections.md): arrays, hashes, ranges, and
   the blocks you pass to `map` and `each`.
4. [Functions](functions.md): parameters, defaults, named arguments, and
   closures.
5. [Classes and structs](classes.md): objects, inheritance, modules, and
   value types.
6. [Types](types.md): what the compiler checks and when.
7. [Errors](errors.md): raising, rescuing and cleaning up.
8. [Concurrency](concurrency.md): threads, channels, supervisors and fibers.
9. [Your first project](../getting-started.md): installing d6, a project
   with tests and dependencies, and building an executable.

Most examples have a **Run** button that runs them in your browser, using the
same interpreter compiled to WebAssembly. A comment like `# => 42` shows what
a line evaluates to or prints. Every example in this guide is run by the
project's test suite, so what you read is what d6 does.

## A first taste

```ruby
# A function with typed parameters and return type.
def greet(name: String, times: Int = 1) -> String
  ("Hello, #{name}! " * times).strip()
end

puts(greet("Ada"))              # => Hello, Ada!
puts(greet("Grace", times: 2))  # => Hello, Grace! Hello, Grace!

# Blocks are written do ... end, and every call has parentheses.
lengths = ["d6", "ruby", "python"].map() do |word| word.length() end
puts(lengths)                   # => [2, 4, 6]
```

Calling `greet(42)` would not run at all. The compiler reports
`argument 'name' of greet: expected String, got Int` and stops before the
first line executes.

## If you are coming from...

Each chapter has notes like these where d6 breaks a habit:

- **Ruby**: the syntax is close, but every call needs parentheses, blocks are
  `do ... end` only, there are no single-quoted strings, and `yield` belongs
  to fibers rather than blocks.
- **Python**: `end` closes blocks instead of indentation, `None` is spelled `nil`,
  and named arguments work on any call: `area(width: 3, height: 4)`.
- **JavaScript**: there is no `undefined` and no implicit conversion.
  `"1" + 1` is an error, not `"11"`.

The [reference](../syntax.md) covers every feature in full detail. This guide
links to it where you might want more.
