# Processes

[I/O and native services](io.md) · Previous: [Time](time.md)

## `Process.run` and `Process.spawn`

```ruby
result = Process.run(["ls", "-la", dir])
puts(result.stdout())
puts(result.exit_code())
result.success?()          # => exit_code() == 0

result = Process.run(["sh", "-c", "echo out; echo err 1>&2; exit 3"])
result.stdout()            # => "out\n"
result.stderr()            # => "err\n"
result.exit_code()         # => 3
```

One constructor, one native result type (`DIAMOND_OBJECT_PROCESS_RESULT`),
compiling to a dedicated opcode the same way `Time.at`/`SQLite3.open` do:

- `Process.run(argv)` — `argv` is an `Array` of `String`s, the command
  followed by its arguments (`argv[0]` is resolved against `PATH`, same
  as `execvp`). Blocks until the child exits, with its full stdout and
  stderr captured. **Argv-array-only, deliberately** — there is no shell-
  string form (`Process.run("ls -la")`) at all, so there is no shell-
  injection surface to guard against; a `String` element is passed to the
  child exactly as written, never interpreted by a shell. Run a real
  shell explicitly (`Process.run(["sh", "-c", "..."])`) if that's what's
  needed, same as Ruby's own `Process.spawn(argv)` array form.

Instance methods on the result, all ordinary `.method()` calls:

- `.stdout()` / `.stderr()` → `String`, the child's captured output
- `.exit_code()` → `Int` — the child's real exit status if it exited
  normally, or `128 + signal number` if it was killed by a signal
  (matching the shell's own convention)
- `.success?()` → `Bool`, `exit_code() == 0`

**`Process.run`'s own v1 scope, deliberately minimal**:

- The child's stdin is always `/dev/null` — there is no way to feed it
  data, on `Process.run` or `Process.spawn` alike. A command that tries
  to read from stdin sees immediate EOF (e.g. `Process.run(["cat"])`
  returns empty stdout and exit code `0` immediately, rather than
  hanging). A writable stdin is a real, separate future slice.
- Fully blocking/synchronous — no way to run a child in the background,
  poll it, or kill it early (`Process.spawn`, below, is exactly that).
  A long-running or hung child blocks the calling Diamond program for as
  long as it runs.
- A trapped `Signal` (see `Signal.trap` above) does not get to run while
  a `Process.run` call is blocked waiting on the child — it runs once the
  child exits and `Process.run` returns, not immediately. (Unlike
  `IO.poll`, which does handle this — see that section above. This
  applies to `Process.spawn`'s own `#wait` too, for the same reason: it's
  a plain blocking `waitpid`, not built on `IO.poll`.)
- Command-not-found and other spawn failures (a bad path, no exec
  permission, ...) raise `IOError` synchronously, the same call that
  fails, rather than exit code `127` the way a real shell reports it.
  Same for `Process.spawn`.

### `Process.spawn`: a live, non-blocking handle

```ruby
handle = Process.spawn(["sh", "-c", "sleep 1; echo done"])
puts(handle.pid())

ready = IO.poll([handle.stdout()], [], -1)   # blocks until output arrives
puts(ready["readable"][0])                    # => true
puts(handle.stdout().read(100))               # => "done\n"

puts(handle.wait())                           # blocks until the child exits, => 0
puts(handle.running?())                       # => false
```

```ruby
handle = Process.spawn(["sleep", "30"])
handle.terminate()   # SIGTERM
handle.wait()         # => 143 (128 + SIGTERM)

handle.kill()          # raises IOError -- process has already exited
```

Unlike `Process.run`, `Process.spawn(argv)` (same `argv`-Array-only
argument, same `PATH` resolution, same `/dev/null` stdin) returns
immediately with a live `Process::Handle` — no output capture, no
waiting for exit:

- **`.pid()`** → `Int`, the child's process ID.
- **`.stdout()`** / **`.stderr()`** → a `Process::Stream` each (a new
  object kind, `DIAMOND_OBJECT_PROCESS_STREAM`) — the *read end* of a
  pipe to the child, always the same object across repeated calls. Set
  `O_NONBLOCK` right after spawning, the same reasoning
  `TCPServer.listen_nonblocking`'s accepted `Socket`s already have: a
  poll-driven caller needs a real `EAGAIN`, not libc stdio buffering
  silently swallowing "nothing yet". `.read(n)` returns a `String` of
  however many bytes were actually available (up to `n`), `nil` at true
  EOF, or raises `WouldBlockError` when nothing's ready — exactly
  `Socket#read`'s own contract (see the non-blocking-sockets section
  above), and for the same reason: it *is* that contract, on a pipe fd
  instead of a network one. `.close()` is idempotent. No `.write()` —
  this is a read-only pipe end (the child's stdin is `/dev/null`).
  Poll it directly: `IO.poll([handle.stdout(), handle.stderr()], [], timeout_ms)`
  accepts a `Process::Stream` exactly like a `Socket`.
- **`.wait()`** → `Int`, the same exit-code convention `Process.run`'s
  own `.exit_code()` uses (real exit status, or `128 + signal number`).
  Blocks until the child exits, `waitpid`-style — **does not drain
  `.stdout()`/`.stderr()` for you first**. A child that writes enough to
  fill the OS pipe buffer while nobody's reading it can deadlock right
  here, waiting for a read that will never come while the child itself
  is blocked writing — the same well-documented gotcha every language's
  "wait without draining" API has (e.g. Python's own
  `subprocess.Popen.wait()`). A caller that cares about output drains
  the streams itself (optionally via `IO.poll`) while the child runs, or
  accepts that `.wait()` can hang for a sufficiently chatty child.
  Idempotent — a second `.wait()` call just returns the same cached exit
  code instead of re-`waitpid`ing (which would either block forever on
  an already-reaped pid, or, worse, risk observing an unrelated process
  that has since reused it).
- **`.running?()`** → `Bool`. A non-blocking check (`waitpid` with
  `WNOHANG`) — reaps and caches the exit code if the child has already
  exited, same as `.wait()` would, but never blocks either way.
- **`.terminate()`** / **`.kill()`** → sends `SIGTERM`/`SIGKILL` to the
  child. Raises `IOError` if the handle has already observed the child
  exit (via `.wait()` or a `.running?()` that returned `false`) — once a
  pid is actually reaped, the OS is free to recycle it for an unrelated
  process, and signaling a stale pid at that point would hit whatever
  that pid means *now*. A child that has exited but hasn't been reaped
  yet (a zombie) is still safe to signal — POSIX guarantees a zombie's
  pid stays reserved until reaped — so this guard triggers only once
  this handle has actually reaped it, never preemptively.
- A handle that's simply dropped, never `.wait()`ed on: GC sweep reaps
  it with a non-blocking `waitpid` if it has already exited (so it
  doesn't sit around as a zombie forever), but never blocks the sweep
  waiting for a still-running child — that child is left running,
  detached from any Diamond-level handle from that point on, the same
  way a shell backgrounding a job and then exiting leaves it running.

## What's deliberately out of scope so far

- **Multiple `print`/`puts` arguments**: `puts(a, b)` (Ruby-style, one
  line per argument) is not supported — exactly one argument, matching
  the narrowest useful slice.
- **File mode validation**: `File.open` passes `mode` straight through
  to `fopen` with no Diamond-level checking.
- **Transactions as a dedicated *native* API**: no `.transaction { ... }`-
  style block helper built into `SQLite3`/`PostgreSQL`/`MySQL` themselves
  — `db.execute("BEGIN")`/`"COMMIT"`/`"ROLLBACK"` already work today
  through plain SQL, and `packages/active_record`'s
  `ActiveRecord::Transaction.run`/`.run_nested` already builds exactly
  this convenience layer in pure Diamond code on top of it (including
  `SAVEPOINT`-based nesting), so there's no native gap here to close.
- **Named timezones and a separate `Date`-only type**: no named IANA timezone
  selection (`Time.parse` and `.localtime` support explicit fixed UTC offsets;
  Ruby itself needs the `tzinfo` gem for named zones), and no date-without-time
  type distinct from `Time`.

Each of these is a plausible next slice, sized independently rather than
attempted together.
