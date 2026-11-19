# Fiber

[API reference](index.md) · Next: [Thread](thread.md)

A `Fiber` runs a callable cooperatively on the current thread: it runs until it
yields or finishes, and never in parallel. Use fibers for generators and event
loops, and [Thread](thread.md) for parallel work. Fibers are not available in
the WebAssembly build. [Fibers](../fibers.md) covers scheduling and I/O.

```ruby
def numbers()
  Fiber.yield(1)
  Fiber.yield(2)
  3
end

f = Fiber.new(numbers)
f.resume()   # => 1
f.resume()   # => 2
f.resume()   # => 3
f.status()   # => "completed"
```

## Functions

### `Fiber.new(callable) -> Fiber`

A fiber that will run `callable`, a zero-argument callable that may capture
local variables. Nothing runs until the first `resume`.

### `Fiber.yield(value = nil) -> Value`

Suspends the running fiber, making `resume` return `value`. When the fiber is
resumed again, `Fiber.yield` returns the value passed to that `resume`.
Outside a fiber it raises `FiberError`. Inside a method without `&block`,
plain `yield(value)` does the same.

## Methods

### `resume(value = nil) -> Value`

Runs the fiber until it yields or finishes, and returns the yielded value or
the callable's result. An exception the fiber does not rescue is raised here.
Resuming a finished fiber raises `FiberError`.

### `status() -> String`

`"runnable"` before the first resume, `"suspended"` after a yield,
`"completed"` after returning, or `"failed"` after an exception.

### `alive?() -> Bool`

Whether the fiber can still be resumed.
