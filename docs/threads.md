# Threads

`Thread` runs Diamond code concurrently on native OS threads. Threads can use
multiple CPU cores; [`Fiber`](fibers.md) instead provides cooperative
concurrency within one OS thread.

## Creating and joining a thread

`Thread.new(callable, *args)` starts a thread immediately. The callable must
accept the supplied arguments and must not capture local state.

```ruby
def square(n)
  n * n
end

left = Thread.new(square, 6)
right = Thread.new(square, 7)

left.join()   # => 36
right.join()  # => 49
```

`join()` blocks until the thread finishes and returns its result. Joining the
same thread again returns the cached result. `alive?()` reports whether the
thread is still running without blocking.

Diamond does not support detached or fire-and-forget threads. If an unjoined
Thread becomes unreachable, garbage collection waits for it to finish before
reclaiming its resources.

## Isolated heaps

Every spawned thread has its own VM and heap. Threads do not share mutable
objects, class variables, globals, open files, sockets, or other runtime
resources. Arguments and return values cross the boundary as deep copies.

These values may cross a thread boundary:

- `nil`, booleans, integers, floats, strings, symbols, and bignums;
- arrays and hashes containing transferable values;
- instances whose fields contain transferable values; and
- zero-capture callables.

Capturing closures and native-resource values such as Fiber, File, Listener,
Socket, TLS socket, Regexp, ProgramBuilder, and Thread cannot cross the
boundary. Passing one raises `TypeError`.

The process permits at most 64 live spawned threads. Exceeding the limit
raises `ThreadError`.

## Per-thread initialization

Runtime mutations made before `Thread.new` are not copied into the new heap.
In particular, a class variable configured in the parent remains unset in the
child. Initialize repositories, clients, and other mutable services separately
in each worker; a lazy per-thread accessor is usually the simplest pattern.

This also applies to multi-threaded Gremlin servers: startup configuration in
the calling thread does not configure every request worker. Program
definitions are available in every thread, so top-level methods, classes, and
modules can be called normally. It is their mutable runtime state that remains
isolated.

## Results and exceptions

If a worker raises an uncaught Diamond exception, `join()` re-raises that
exception in the joining thread:

```ruby
def fail_work()
  raise ArgumentError.new("bad job")
end

worker = Thread.new(fail_work)

begin
  worker.join()
rescue error: ArgumentError
  puts(error.message())
end
```

An internal VM failure is reported as `ThreadError` at `join()`.

Standard input and output are process-wide native streams. Concurrent writes
are memory-safe but may interleave, so applications should serialize output
when ordering matters. `Signal.trap` is also process-wide; installing a
handler from multiple threads replaces the previous handler rather than
creating one handler per VM.

Implementation details, copy semantics, and lifecycle invariants are
documented in [Concurrency internals](internal/concurrency-internals.md).
