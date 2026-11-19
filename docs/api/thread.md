# Thread

[API reference](index.md) · Previous: [Fiber](fiber.md) · Next: [Channel](channel.md)

A `Thread` runs a callable on its own operating-system thread, in parallel.
Each thread has its own heap: arguments and the result are copied, and a
callable that captures local variables is refused. Threads talk through
[Channels](channel.md). [Threads](../threads.md) covers the copying rules and
signal handling.

```ruby
def square(n) = n * n

left = Thread.new(square, 6)
right = Thread.new(square, 7)
left.join() + right.join()   # => 85
```

There are no detached threads: a program waits for every thread before it
exits normally, and an unjoined thread's uncaught exception is printed on
standard error.

## Functions

### `Thread.new(callable, args...) -> Thread`

Starts `callable` with copies of `args` and returns at once. A value that
cannot be copied to another thread, such as an open file or a capturing
closure, raises `TypeError`.

## Methods

### `join() -> Value`

Waits for the thread and returns a copy of its result. An exception the
thread did not rescue is raised again here. Joining again returns the same
result.

### `alive?() -> Bool`

Whether the thread is still running.
