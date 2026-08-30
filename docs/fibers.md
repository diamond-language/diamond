# Fibers

Fibers provide cooperative concurrency within one OS thread. A fiber runs
until it finishes or explicitly yields; it is never preempted between those
points. Use fibers for generators, coroutines, and event loops. Use
[`Thread`](threads.md) when work should execute in parallel on multiple CPU
cores.

## Creating and resuming a fiber

`Fiber.new(callable)` creates a fiber from a zero-argument callable. The
callable may capture surrounding local variables.

```ruby
def make_counter()
  def counter()
    i = 0
    loop do
      increment = Fiber.yield(i)
      i = i + increment
    end
  end
  counter
end

counter = Fiber.new(make_counter())
counter.resume(0)   # => 0
counter.resume(10)  # => 10
counter.resume(5)   # => 15
```

The first `resume(value)` starts the callable. Its argument becomes the result
of the first `Fiber.yield` expression. Each later resume continues immediately
after the previous yield.

`resume` returns the value passed to `Fiber.yield`, or the callable's return
value when the fiber completes. Its argument is optional and defaults to
`nil`.

## Yielding

Use `Fiber.yield()` or `Fiber.yield(value)` in any lexical context. Legacy
`yield` and `yield(value)` have the same fiber behavior in a body that does not
accept a named block with `&block`.

Yielding preserves the complete execution context, including nested method
calls and active `rescue` and `ensure` clauses. Calling `Fiber.yield` outside
a running fiber raises `FiberError`.

## Status and lifecycle

```ruby
fiber.status()  # => "runnable", "suspended", "completed", or "failed"
fiber.alive?()  # => true until completion or failure
```

A new fiber is runnable. Yielding suspends it; resuming makes it run again.
Returning completes it, while an uncaught exception marks it failed. Resuming
a completed or failed fiber raises `FiberError`.

An uncaught exception inside a fiber is re-raised at the `resume` call and may
be rescued there:

```ruby
fiber = Fiber.new(def()
  raise ArgumentError.new("bad input")
end)

begin
  fiber.resume()
rescue ArgumentError => error
  puts(error.message())
end
```

Fibers are garbage collected like other values; no explicit close operation
is required.

## Scheduling and I/O

Diamond does not expose a built-in fiber scheduler. An application can keep
fibers in an Array and decide which one to resume. The Gremlin HTTP server uses
this approach with non-blocking sockets and `IO.poll`; see
[`packages/gremlin`](../packages/gremlin/README.md) and the
[non-blocking I/O guide](io.md#non-blocking-sockets-tcpserverlisten_nonblocking-socket-iopoll).

Because scheduling is cooperative, blocking I/O or CPU-heavy work inside one
fiber blocks every fiber on that OS thread. Use non-blocking I/O with explicit
yields, or move CPU work to a Thread.

Implementation details and GC invariants are documented in
[Concurrency internals](concurrency-internals.md).
