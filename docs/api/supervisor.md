# Supervisor

[API reference](index.md) · Previous: [Channel](channel.md) · Next: [Networking](network.md)

A `Supervisor` runs workers on their own threads and restarts one when it
crashes with an uncaught exception. A worker that returns normally is not
restarted. Workers follow `Thread.new`'s rules: arguments are copied, fresh
for each attempt, and capturing closures are refused. [Threads](../threads.md#supervisors)
describes how stopping interrupts blocking calls.

<!-- doc-check: skip -->
```ruby
def fetch_loop(url)
  loop do
    fetch_and_process(url)
  end
end

sup = Supervisor.new()
sup.restart_policy({"max_restarts": 5, "within_ms": 10000})
sup.add_child(fetch_loop, "https://example.com/feed")
sup.join()
```

## Functions

### `Supervisor.new(strategy = :one_for_one) -> Supervisor`

A supervisor with no children. When a child crashes, `:one_for_one` restarts
only that child, `:one_for_all` restarts every running child, and
`:rest_for_one` restarts it and the children added after it.

## Methods

### `add_child(callable, args...) -> Int`

Starts a worker and returns its index, from `0`. Up to 32 children.

### `restart_policy(options)`

Limits restarts; call it before the first `add_child`. `options` is a Hash:

- `"max_restarts"` (0 to 100) and `"within_ms"` (default 5000): give up when
  more than `max_restarts` crashes happen within `within_ms`.
- `"backoff_ms"` (default 20) and `"max_backoff_ms"`: the pause before a
  restart, doubling for each consecutive crash up to `max_backoff_ms`.

A supervisor that gives up stops its other children, and `join()` raises
`SupervisorError`.

### `join()`

Waits until every child has finished on its own. Raises `SupervisorError` if
the supervisor gave up.

### `stop()`

### `stop(timeout_ms) -> Bool`

Stops restarting and waits for the current attempts to finish. With a
timeout, interrupts whatever is still running after `timeout_ms` and returns
whether everything finished in time. `ensure` blocks do not run in an
interrupted worker.

### `alive?(index) -> Bool`

Whether that child is still running or restarting.

### `restart_count(index) -> Int`

How many times that child has been restarted.

### `last_error(index) -> String | Nil`

The message from that child's most recent crash, or `nil`.

### `failed?() -> Bool`

Whether the supervisor gave up under its restart policy.
