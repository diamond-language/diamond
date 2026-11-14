# Concurrency

D6 has two tools for doing several things at once:

- **Threads** run on separate CPU cores. Each one has its own memory, so
  threads never share objects: values are *copied* between them, and they
  talk through channels.
- **Fibers** take turns on one thread. They are for generators and
  coroutines, not for using more cores.

Threads and fibers are not available in the browser build, so the examples
in this chapter have no Run button. Run them with `d6` on your machine.

## Threads

`Thread.new(function, args...)` starts a thread that calls the function with
the arguments. `join()` waits for it to finish and returns its result:

<!-- docsite: no-run -->
```ruby
def square(n) = n * n

left = Thread.new(square, 6)
right = Thread.new(square, 7)
puts(left.join() + right.join())   # => 85
```

The function you start a thread with must be a top-level function (or
another function that captures no local variables), because the thread
cannot see the variables of the code that started it.

`join()` re-raises an exception the thread did not rescue. A thread that
fails and is never joined prints its exception and backtrace on stderr, so
the error is not lost.

## Values are copied

Everything you pass to a thread, and everything it returns, is copied into
the receiving thread's memory. A thread that changes an array it was given
changes its own copy:

<!-- docsite: no-run -->
```ruby
def add_item(items)
  items.push("added by the thread")
  items.length()
end

mine = ["a"]
worker = Thread.new(add_item, mine)
puts(worker.join())     # => 2
puts(mine.length())     # => 1
```

This is the central idea of D6's concurrency. Because no two threads ever
hold the same object, there are no data races and no locks to get wrong.
When threads need to cooperate, they send each other values.

## Channels

A `Channel` is a queue that threads share. `send` adds a value, waiting
while the channel is full; `receive` takes one, waiting while it is empty.
After `close()`, `receive` returns `nil` once the queued values run out,
which is how a consumer knows to stop.

<!-- docsite: no-run -->
```ruby
def square_all(input, output)
  loop do
    n = input.receive()
    break if n == nil
    output.send(n * n)
  end
  output.close()
end

jobs = Channel.new(10)      # holds up to 10 values
results = Channel.new(10)
worker = Thread.new(square_all, jobs, results)

[1, 2, 3].each() do |n| jobs.send(n) end
jobs.close()

squares = []
loop do
  value = results.receive()
  break if value == nil
  squares.push(value)
end
worker.join()
p(squares)   # => [1, 4, 9]
```

A channel is the one value that is shared rather than copied: every thread
that receives it gets a handle to the same queue.

## Supervisors

A `Supervisor` runs workers and restarts one when it crashes with an
uncaught exception, the way Erlang supervisors do. Each restart starts from
the original arguments in fresh memory, so a crash cannot leave a worker
half-updated.

<!-- docsite: no-run -->
```ruby
def flaky_worker(attempts, results)
  attempts.send(1)
  raise RuntimeError.new("first try fails") if attempts.size() == 1
  results.send("finished on attempt #{attempts.size()}")
end

attempts = Channel.new(10)
results = Channel.new(10)
supervisor = Supervisor.new()
child = supervisor.add_child(flaky_worker, attempts, results)

puts(results.receive())                  # => finished on attempt 2
puts(supervisor.restart_count(child))    # => 1
```

A worker that returns normally is finished and is not restarted. The
[threads reference](../threads.md#supervisors) covers restart limits,
back-off, and the strategies that restart a group of workers together.

## Fibers

A fiber is a function you can pause. `Fiber.yield(value)` pauses it and
hands `value` to whoever called `resume`; the next `resume` continues from
that point. Fibers never run at the same time as each other, so they need no
channels or copying: they are ordinary closures.

<!-- docsite: no-run -->
```ruby
def counter()
  n = 0
  loop do
    Fiber.yield(n)
    n += 1
  end
end

numbers = Fiber.new(counter)
puts(numbers.resume())   # => 0
puts(numbers.resume())   # => 1
puts(numbers.resume())   # => 2
```

## Coming from another language

- **Go**: channels work like Go's buffered channels, and `Thread.new` is
  like `go`. Unlike goroutines, threads are OS threads (up to 64 at a time)
  and share no memory at all.
- **Python**: threads really run in parallel; there is no global
  interpreter lock. Passing data works like `multiprocessing`, by copying.
- **JavaScript**: a thread is closer to a Web Worker than to `async`:
  separate memory, communication by messages.
- **Ruby**: `Thread.new` takes a function and arguments rather than a block,
  and threads cannot see each other's objects. Fibers work like Ruby's,
  except that `Fiber.new` also takes a function rather than a block.

The [threads reference](../threads.md), [fibers reference](../fibers.md)
and [cancellation](../cancellation.md) cover timeouts, `Channel.select`,
and stopping work cleanly.

Next: [Your first project](../getting-started.md).
