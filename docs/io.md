# I/O

This document covers Diamond's I/O surface (stdout, stdin, files, and
TCP sockets) and will grow as later slices land (see `docs/roadmap.md`).

## stdout: `print`/`puts`

```ruby
print("hello, ")
puts("world")
```

`print(value)` writes `value`'s stringified form to stdout with no
trailing newline; `puts(value)` writes it with one. Both take exactly one
argument and both return `nil`.

Stringification reuses the exact mechanism string interpolation (`#{}`)
and the `DIAMOND_OP_TO_STRING` opcode already use, via a shared
`stringify_value` helper in `src/vm.c`: a `String` value passes through
unchanged; an instance with a `to_s()` method has it called (and its
return value must be a `String`, or a rescuable `TypeError`); everything
else falls back to the same formatter used by string interpolation
(`nil`, `true`/`false`, integers, and `[...]`/`{...}` for arrays/hashes).
A `to_s` with the wrong arity raises a rescuable `ArgumentError`, exactly
as calling it any other way would.

`print`/`puts` are recognized in the compiler's `parse_name`, the same
place `Fiber.new` and `redefine_method` are — gated on the identifier not
already being a local or a user-defined function, so `def print(x) ...
end` shadows the builtin entirely (no reserved keyword). They compile to
a single `DIAMOND_OP_PRINT dest, source, newline` instruction; `newline`
is a compile-time-constant byte (`0` for `print`, `1` for `puts`), not a
runtime value.

## stdin: `gets()`

```ruby
name = gets()
puts("hi #{name}")
```

`gets()` takes no arguments and reads one line from stdin, returning it
as a `String` with the trailing line ending stripped — both `\n` and
`\r\n` are handled, and stripping happens on the accumulated line rather
than per underlying read, so it's correct regardless of where an
internal buffer boundary happens to fall relative to the ending. Returns
`nil` only when zero bytes were read before EOF; a final line with no
trailing newline still returns its content, matching Ruby's `gets`.
Compiles to a single `DIAMOND_OP_GETS dest` instruction. Recognized with
the same shadowing precedent as `print`/`puts`.

Line length is not capped — reading grows a buffer across as many
underlying `fgets` calls as a line needs, the same growable-buffer
pattern (`StringBuilder`) already used for value formatting elsewhere
in `src/vm.c`.

## Files: `File.open`/`.read`/`.gets`/`.write`/`.close`

```ruby
f = File.open("data.txt", "w")
f.write("hello, ")
f.write("world")
f.close()

g = File.open("data.txt", "r")
g.read()   # => "hello, world"
g.close()
```

`File.open(path, mode)` opens a file via the C `fopen(path, mode)`
convention directly — `mode` is passed through unvalidated (`"r"`,
`"w"`, `"a"`, `"r+"`, and so on all work exactly as they would in C; an
invalid mode fails the same way a missing path does). A failed open
raises a rescuable `IOError` with `strerror(errno)` in the message, the
same phrasing the CLI's own `require`/file-loading errors already use
(`cannot open '<path>': <reason>`).

A `File` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_FILE`), a thin wrapper around a `FILE *` — the same
shape as `Fiber`'s `DiamondFiberHandle` around a `DiamondFiber *`.
Unlike `Fiber`, nothing inside a `DiamondFileHandle` references another
Diamond value, so `mark_object` needs no dedicated branch for it; sweeping
an unreached handle whose stream is still open calls `fclose` on it as a
safety net, the same role sweep-time cleanup plays for an unclosed
`Fiber`'s native stack.

`File.open` is recognized in the compiler the same way `Fiber.new` is
(shadowable by a local or a top-level function of the same name),
compiling to a single
`DIAMOND_OP_FILE_OPEN dest, path, mode` instruction. `.read()`/`.gets()`/
`.write(value)`/`.close()` are native `DIAMOND_OP_INVOKE` dispatch on a
`DIAMOND_OBJECT_FILE` receiver, the same mechanism `Fiber`'s `.resume`/
`.status`/`.alive?` use:

- `.read()` reads all remaining bytes from the current position to EOF
  as one `String`; `.read(n)` reads up to `n` bytes and stops instead —
  needed for reading a fixed-size chunk (e.g. an HTTP request body of
  known `Content-Length`, as the `packages/http` package does) without
  also blocking on or consuming whatever the other side sends next on a
  still-open connection.
- `.gets()` reads one line, sharing the exact `read_line` helper stdin's
  global `gets()` uses (same EOF/`nil`, CRLF-stripping, and no-line-
  length-cap behavior).
- `.write(value)` stringifies `value` via `stringify_value` (same as
  `print`) and writes it.
- `.close()` is idempotent — closing an already-closed handle is a no-op,
  not an error.

Any operation other than `.close()` on an already-closed handle, or a
genuine read/write failure (checked via `ferror`, not just a short
return value), raises a rescuable `IOError`.

## TCP sockets: `TCPSocket.connect`/`TCPServer.listen`/`.accept`

```ruby
server = TCPServer.listen(8080)
conn = server.accept()   # blocks until a client connects
line = conn.gets()
conn.write("echo: #{line}\n")
conn.close()

client = TCPSocket.connect("example.com", 8080)
client.write("hello\n")
client.gets()
client.close()
```

`TCPSocket.connect(host, port)` and `TCPServer.listen(port)` both resolve
addresses via `getaddrinfo` (protocol-agnostic — IPv4 and IPv6 both work,
nothing hardcodes `sockaddr_in`), trying each candidate address in turn
until one succeeds. `port` is an `Int` in both cases (converted to the
string `getaddrinfo` expects internally); `host` is a `String` hostname
or address. A connect/bind/listen failure raises a rescuable `IOError`
with `strerror(errno)` from the actual failing attempt, even when
multiple addresses were tried.

The key design decision: a connected socket — whether from
`TCPSocket.connect` or from `.accept()` — is `fdopen()`'d and wrapped in
the *exact same* `DiamondFileHandle` a `File` uses. This means
`.read()`/`.gets()`/`.write(value)`/`.close()` on a connected socket are
the identical `File` dispatch code already described above; no new
read/write logic exists for sockets at all. Only connection
*establishment* is new:

- `TCPServer.listen(port)` produces a different, new heap object kind,
  `DIAMOND_OBJECT_LISTENER` (`DiamondListenerHandle`, wrapping a raw
  listening-socket file descriptor rather than a `FILE *`, since a
  listening socket is never read from or written to). Like `File`,
  nothing inside it references another Diamond value, so it needs no
  `mark_object` branch; sweeping an unreached handle whose fd is still
  open closes it, same safety-net role as `File`/`Fiber`.
- `.accept()` is native `DIAMOND_OP_INVOKE` dispatch on a
  `DIAMOND_OBJECT_LISTENER` receiver (alongside `.close()`) — the only
  genuinely new I/O *operation* in this slice. It blocks in `accept(2)`,
  then hands the resulting connection to the same `fdopen`-and-wrap path
  `TCPSocket.connect` uses.

`TCPSocket.connect`/`TCPServer.listen` are recognized in the compiler the
same way `File.open`/`Fiber.new` are.

## Non-blocking sockets: `TCPServer.listen_nonblocking`, `Socket`, `IO.poll`

```ruby
listener = TCPServer.listen_nonblocking(8080)
conn = listener.accept()   # nil immediately if nothing's pending -- never blocks
if conn != nil
  begin
    data = conn.read(4096)   # nil at EOF, or the bytes actually available right now
  rescue error: WouldBlockError
    # nothing to read yet -- not an error, just not ready
  end
  conn.write("hi")           # returns the byte count actually written (may be partial)
  conn.close()
end

ready = IO.poll([listener], [], -1)   # -1 blocks until something's ready, 0 polls once
ready["readable"][0]   # true/false, one entry per readables input, same order
```

Exists specifically to make genuine fiber-driven concurrency possible
(see `packages/gremlin`, `docs/fibers.md`) — a blocking `.accept()`/
`.read()`/`.write()` on the ordinary `TCPServer`/`TCPSocket`/`File`
primitives above blocks the *entire process*, not just one logical
connection, so a `Fiber`-per-connection design gets no real concurrency
out of them no matter how many fibers exist. These are the primitives
that let a fiber's own I/O yield back to a scheduler instead:

- **`TCPServer.listen_nonblocking(port)`** — identical to
  `TCPServer.listen(port)` (same `getaddrinfo`/`socket`/`bind`/`listen`
  dance, factored into one shared `tcp_listen_helper` in `src/vm.c`) but
  the resulting fd is also set `O_NONBLOCK` via `fcntl`, and the
  `DiamondListenerHandle` returned is flagged accordingly. `.accept()` on
  such a listener returns `nil` instead of blocking when nothing is
  pending (an ordinary `TCPServer.listen` listener's `.accept()` is
  unaffected either way — still blocks, same as always).
- **`Socket`** — a new object kind (`DIAMOND_OBJECT_SOCKET`,
  `DiamondSocketHandle`), returned only by `.accept()` on a
  `TCPServer.listen_nonblocking` listener. Deliberately *not* the
  buffered `FILE*` `File`/blocking-socket path reuses — libc stdio
  buffering and `EAGAIN` don't mix cleanly, since a short buffered read
  can silently swallow the "nothing available yet" signal a poll-driven
  caller needs to see on every call, not just the first. `.read(n)`/
  `.write(value)` are raw `read(2)`/`write(2)` against the fd directly:
  `.read(n)` returns a `String` of however many bytes were actually
  available (up to `n`), `nil` at true EOF (the peer closed), or raises
  `WouldBlockError` (a new `StandardError` subclass) when nothing's
  ready right now — a real, meaningful distinction `File#read` never
  needed, since a blocking read can't tell "not yet" from "never." Also
  accepted on Linux fd never inherits `O_NONBLOCK` from the listener it
  came from, so `.accept()` sets it explicitly on each accepted fd too.
  `.write(value)` returns the actual byte count written rather than
  either succeeding fully or raising — a partial write is a normal,
  expected outcome on a non-blocking socket whose send buffer filled up
  mid-write, and the caller (`packages/gremlin`'s
  `NonblockingConnection#write`) is expected to retry the remainder.
- **`IO.poll(readables, writables, timeout_ms)`** — a `poll(2)` wrapper
  accepting two Arrays (of `TCPServer.listen_nonblocking`
  listeners/`Socket`s — an ordinary blocking listener/File is rejected,
  since polling a blocking-mode fd is meaningless: nothing in this VM
  ever puts one in non-blocking mode, so it would always appear either
  always-ready or never-ready depending on kernel buffering, never the
  genuine signal a caller needs) and a millisecond timeout (`-1` blocks
  indefinitely, `0` returns immediately). Returns a `Hash`:
  `{"readable": [...], "writable": [...]}`, each an Array of `Bool`s the
  same length and order as the corresponding input — `readable[i]`
  answers "is `readables[i]` ready," not "which are ready" — deliberately
  avoiding a design needing `Array#include?`/object-identity comparison
  on the Diamond side, neither of which exists. The same fd can appear in
  both lists (or twice within one, from two different Diamond objects
  wrapping the same underlying connection) — internally deduplicated by
  fd and OR'd together into one `pollfd` entry, since `poll(2)` itself
  keys purely by fd. `POLLHUP`/`POLLERR`/`POLLNVAL` count toward *both*
  readiness directions: a peer that closed its connection is exactly the
  condition a caller's next `.read()`/`.write()` needs to be woken up to
  observe, not a state that would otherwise never produce a `POLLIN`/
  `POLLOUT` again.

Building this Hash result safely took real care: every `allocate_*` call
in `src/vm.c` can trigger a GC collection, and this VM's collector only
marks reachable values from *rooted* locations (registers, the exception
slot, fiber frame chains) — a freshly allocated object sitting in a
plain C local between two further `allocate_*` calls is invisible to it.
An earlier version of `DIAMOND_OP_IO_POLL`'s result-construction code
allocated all four pieces (two result Arrays, two `String` keys) before
ever touching `registers[dest]`, and a real heap-use-after-free in
`hash_set`/`hash_find` — caught by `make test-sanitize` under
`DIAMOND_STRESS_GC=1`, not by inspection — was the result. The fix: root
the `Hash` in `registers[dest]` immediately after allocating it, then for
each entry, root its key first with a `DIAMOND_NIL` placeholder value
(nil needs no protection, so this step is always safe) before allocating
the real value and overwriting the placeholder — `hash_set` already
updates an existing key's value in place. Nothing is ever more than one
allocation away from reachability through `registers[dest]`.

`TCPServer.listen_nonblocking`/`IO.poll` are recognized in the compiler
the same way `TCPServer.listen`/`TCPSocket.connect` are (`IO.poll`
mirroring `TCPSocket.connect`'s 3-argument shape, since neither is a
class with real dispatch — see above).

## What's deliberately out of scope so far

- **UDP and TLS**: sockets are TCP only, and plain-text TCP at that.
- **Multiple `print`/`puts` arguments**: `puts(a, b)` (Ruby-style, one
  line per argument) is not supported — exactly one argument, matching
  the narrowest useful slice.
- **Error handling for stdout write failures**: a failed `fwrite`/`fputc`
  to stdout (e.g. a broken pipe) is not currently surfaced as a
  rescuable exception, unlike `File#write`'s own `ferror` check; this
  mirrors most languages' baseline `print`, but is a known
  simplification, not a deliberate design stance.
- **File mode validation**: `File.open` passes `mode` straight through
  to `fopen` with no Diamond-level checking.

Each of these is a plausible next slice, sized independently rather than
attempted together.
