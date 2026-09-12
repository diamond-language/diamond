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
- instances whose fields contain transferable values;
- zero-capture callables; and
- `Channel` (see [Channels](#channels) below) -- the one value that crosses
  by *reference*, not by deep copy: every thread that receives one gets its
  own handle onto the same underlying channel, not an independent copy.

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

## Channels

`Channel` is a bounded, thread-safe mailbox: one thread `send`s, another
`receive`s, both blocking as needed. Unlike a plain return value, a channel
lets threads exchange values throughout their whole lifetime, not just once
at `join()`.

```ruby
def producer(ch)
  ["first", "second", "third"].each() do |item|
    ch.send(item)
  end
  ch.close()
end

ch = Channel.new(4)  # capacity: 1..1_000_000
worker = Thread.new(producer, ch)

loop
  item = ch.receive()
  break if item == nil
  puts(item)
end

worker.join()
```

`send(value)` blocks while the channel is full; `receive()` blocks while it's
empty. Both unblock the moment the channel is `close()`d: a blocked `send`
raises `IOError`, and a blocked `receive` returns `nil` once every already-
queued value has been drained -- the same "nothing left, ever" signal
`File#read`'s own EOF-as-`nil` convention already uses, distinct from "wait
a bit and it might arrive" (see `try_send`/`try_receive` below).

`try_send(value)`/`try_receive()` are the non-blocking pair: instead of
waiting, a full `send`/empty `receive` raises `WouldBlockError` immediately
-- the same convention non-blocking socket reads/writes already use. Poll a
channel with `try_receive` in a loop for timeout- or signal-responsive
waiting; a plain blocking `receive` (like `Thread#join`) does not service
`Signal.trap` handlers while parked.

`close()` is idempotent -- closing an already-closed channel is a no-op, not
an error. `closed?()` and `size()` report status without blocking.

A `Channel` itself can be sent through another channel, passed as a
`Thread.new` argument, or stored in an array/hash/instance that crosses a
thread boundary -- every recipient shares the *same* channel (see the note
in [Isolated heaps](#isolated-heaps) above); only the values passed through
it are ever deep-copied. A value unsupported for cross-thread transfer
(a capturing closure, another native-resource handle) raises `TypeError` on
`send`/`try_send`, exactly as passing one to `Thread.new` already does.

Implementation details, copy semantics, and lifecycle invariants are
documented in [Concurrency internals](internal/concurrency-internals.md).
