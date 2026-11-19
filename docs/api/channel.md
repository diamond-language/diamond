# Channel

[API reference](index.md) · Previous: [Thread](thread.md) · Next: [Supervisor](supervisor.md)

A `Channel` is a bounded, thread-safe queue. Values sent through it are
copied, while the Channel itself is shared: every thread holding it holds the
same queue. [Threads](../threads.md#channels) covers rendezvous channels and
`select` in more depth.

```ruby
ch = Channel.new(2)
ch.send("first")
ch.send("second")
ch.close()
ch.receive()   # => "first"
ch.receive()   # => "second"
ch.receive()   # => nil
```

`deadline` arguments are absolute `Time.monotonic()` readings, a Float, or
`nil` for no limit. There is no `sleep`; wait with
`Channel.new(1).wait_readable([], Time.monotonic() + seconds)`.

## Functions

### `Channel.new(capacity) -> Channel`

A channel holding up to `capacity` values, from `0` to `1_000_000`. With `0`,
each `send` waits until a receiver takes the value.

### `Channel.select(channels, deadline = nil) -> Array | Nil`

Receives from the first channel in `channels` that has a value, waiting until
one does. Returns `[index, value]`, or `nil` at the deadline or once every
channel is closed and empty. Earlier channels win when several are ready.

```ruby
a = Channel.new(1)
b = Channel.new(1)
b.send(:x)
Channel.select([a, b])   # => [1, :x]
```

### `Channel.select_send(channels, value, deadline = nil) -> Int | Nil`

Sends `value` to the first channel with room, waiting until one has it, and
returns that channel's index. Closed channels are skipped. Returns `nil` at the
deadline or when every channel is closed.

## Methods

### `send(value)`

Adds `value`, waiting while the channel is full. Raises `IOError` if the
channel is closed.

### `receive() -> Value | Nil`

Takes the oldest value, waiting while the channel is empty. Returns `nil` once
the channel is closed and empty.

### `try_send(value)`

### `try_receive() -> Value`

The same without waiting: a full or empty channel raises `WouldBlockError`.

### `wait_readable(cancellations, deadline) -> Nil`

### `wait_writable(cancellations, deadline) -> Nil`

Wait until the channel probably has a value or room, the deadline passes, or
one of the `cancellations` channels closes. They return `nil` either way;
check with `try_receive` or `try_send` afterwards.

### `close()`

Closes the channel. Waiting senders raise `IOError`, and receivers get the
remaining values, then `nil`. Closing again does nothing.

### `closed?() -> Bool`

Whether the channel is closed.

### `size() -> Int`

How many values are queued.
