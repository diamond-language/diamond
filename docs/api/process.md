# Process, Signal, streams and IO

[API reference](index.md) · Previous: [MatchData](match_data.md)

`Process` runs other programs, `Signal` handles a request to stop, and `IO`
asks about terminals and waits on sockets and pipes. Running programs needs
the `subprocess` sandbox capability. [Processes](../processes.md) describes
the rules in more depth.

A command is always an Array: the program, then its arguments, looked up on
`PATH`. No shell reads it, so arguments need no quoting; run
`["sh", "-c", "..."]` when you want a shell.

```ruby
result = Process.run(["sh", "-c", "echo out; exit 3"])
result.stdout()      # => "out\n"
result.exit_code()   # => 3
```

## Process

### `Process.run(argv, options = nil) -> ProcessResult`

Runs the command to completion and captures its output. Standard input is
empty unless `options` gives it. A program that cannot be started raises
`IOError`.

`options` is a Hash with String keys:

- `"env"`: a Hash of variables added to the current environment.
- `"cwd"`: the working directory.
- `"stdin"`: a String written to the program's standard input.

```ruby
Process.run(["wc", "-c"], {"stdin": "abc"}).stdout().strip()   # => "3"
```

### `Process.spawn(argv, options = nil) -> ProcessHandle`

Starts the command and returns at once. It takes `run`'s `"env"` and `"cwd"`,
plus:

- `"stdin"`: `true` for a writable pipe at `handle.stdin()`.
- `"stdout"`, `"stderr"`: a file path to append that output to instead of a
  pipe.
- `"stderr_to_stdout"`: `true` to send standard error wherever standard
  output goes.

### `Process.pid() -> Int`

This process's id, for a pid file or to tell workers apart in logs.

```ruby
Process.pid() > 0   # => true
```

## ProcessResult

### `stdout() -> String`

### `stderr() -> String`

What the program wrote.

### `exit_code() -> Int`

The exit status, or `128` plus the signal number if a signal ended it.

### `success?() -> Bool`

Whether the exit status was `0`.

## ProcessHandle

### `pid() -> Int`

The child's process ID.

### `stdout() -> ProcessStream | Nil`

### `stderr() -> ProcessStream | Nil`

The read end of the child's output, or `nil` when it goes to a file.

### `stdin() -> ProcessStream | Nil`

The child's standard input when spawned with `{"stdin": true}`.

### `wait() -> Int`

Waits for the child to exit and returns its exit code, as `exit_code()` gives
it. It does not read the output pipes, so drain them first if the child writes
a lot. Calling it again returns the same code.

### `running?() -> Bool`

Whether the child is still running, without waiting.

### `terminate()`

### `kill()`

Sends `SIGTERM` or `SIGKILL`. Raises `IOError` once the handle has seen the
child exit.

```ruby
child = Process.spawn(["sleep", "30"])
child.terminate()
child.wait()   # => 143
```

## ProcessStream

### `read(n) -> String | Nil`

Up to `n` bytes that are ready now; `nil` at the end of output. With nothing
ready it raises `WouldBlockError`, so wait with `IO.poll` first.

### `gets() -> String | Nil`

The next line without its line ending, waiting for it; `nil` at the end.

### `write(text) -> Int`

On `stdin()`: writes all of `text` and returns its length.

### `close()`

Closes the stream; closing `stdin()` sends end of input.

```ruby
child = Process.spawn(["sh", "-c", "read x; echo got $x"], {"stdin": true})
child.stdin().write("hi\n")
child.stdin().close()
child.stdout().gets()   # => "got hi"
child.wait()            # => 0
```

## Signal

### `Signal.trap(name, handler)`

Runs `handler`, a zero-argument callable, when the process receives `"INT"`,
`"TERM"` or `"HUP"`. Other names raise `TypeError`. The handler runs
between instructions, including during `IO.poll`, but not while a blocking
call such as `Process.run` waits.

<!-- doc-check: skip -->
```ruby
def shutdown()
  puts("shutting down")
end
Signal.trap("INT", shutdown)
```

## STDOUT, STDERR and STDIN

The standard streams as values, so a function can take where to write as an argument.
`STDOUT.puts(x)` is `puts(x)` and `STDERR.puts(x)` is `warn(x)`. A program that assigns
its own `STDOUT` (or `STDERR`, `STDIN`) uses that instead.

<!-- doc-check: skip -->
```ruby
def report(out, message) = out.puts("[report] #{message}")
report(STDERR, "3 rows skipped")
STDOUT.print("Name: ")
STDOUT.flush()
name = STDIN.gets()
```

### `write(value) -> Int`

Writes `value` as a String, with no newline, and returns the number of bytes. STDERR is
flushed at once; STDOUT when a newline is written or on `flush`.

### `print(value) -> Nil`

Writes `value` with no newline.

### `puts(value = "") -> Nil`

Writes `value` and a newline, then flushes.

### `flush() -> StandardStream`

Sends anything buffered out, for example a prompt written with `print`, and returns the
stream.

### `gets() -> String | Nil`

`STDIN` only: the next line without its line ending, or `nil` at the end, like `gets`.

### `read() -> String`

`STDIN` only: the rest of the input, each line ending in `\n`.

### `tty?() -> Bool`

Whether the stream is a terminal, as `IO.tty?`.

### `name() -> Symbol`

`:stdout`, `:stderr` or `:stdin`. Two values for the same stream are `==`.

Writing to `STDIN`, or reading from `STDOUT` or `STDERR`, raises `IOError`.

## IO

### `IO.tty?(stream) -> Bool`

Whether `:stdin`, `:stdout` or `:stderr` is a terminal.

<!-- doc-check: skip -->
```ruby
color = IO.tty?(:stdout) && ENV["NO_COLOR"] == nil
```

### `IO.poll(readables, writables, timeout_ms) -> Hash`

Waits until one of the non-blocking sockets, listeners, UDP sockets or
process streams in the two Arrays is ready, or `timeout_ms` passes (`-1`
waits forever, `0` checks once). Returns
`{"readable": [Bool, ...], "writable": [Bool, ...]}`, one entry per input in
the same order.
