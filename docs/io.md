# I/O

This document covers Diamond's I/O surface (stdout, stdin, files, and
TCP/UDP sockets) and will grow as later slices land (see
`docs/roadmap.md`).

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

`puts` (`newline=1`) flushes stdout after writing; `print` (`newline=0`)
does not. stdout is fully buffered, not line-buffered, once it isn't a
terminal — redirected to a file, a pipe, whatever a test harness or
`> log` capture uses — so without this, a `puts("ready")` written right
before a program blocks in a native call (`.accept()`, `IO.poll`, a
`UDPSocket#receive` loop) could sit in the buffer indefinitely: nothing
forces a flush until the buffer fills or the process exits, and a
process blocked waiting for someone to *see* its own "ready" line is
exactly the case that never reaches either. Found this the hard way
while building the signals test below (see its own section) — a
"server prints ready, test harness polls the captured output for that
line" pattern already used throughout `tests/run.sh` and the
`packages/*` test scripts, which happened to keep working anyway
wherever the thing being waited on (`bind`, mostly) completes fast
enough that the *lack* of an early flush didn't matter, until it did.
`print` stays fully buffered — matching ordinary line-buffered-on-a-
terminal behavior unconditionally (rather than only when `isatty()`)
only for the newline-terminated case, so building up a line
incrementally via repeated `print` calls doesn't pay a flush cost per
fragment.

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

## UDP sockets: `UDPSocket.bind`/`UDPSocket.open`, `.send`/`.receive`

```ruby
server = UDPSocket.bind(9999)
result = server.receive(1024)   # blocks until a datagram arrives
puts(result["data"])            # the bytes, as a String
puts(result["host"])            # sender's address, e.g. "127.0.0.1"
server.send("ack", result["host"], result["port"])
server.close()

client = UDPSocket.open()
client.send("hello", "example.com", 9999)
reply = client.receive(1024)
client.close()
```

UDP is connectionless — one socket sends and receives datagrams to/from
whatever address each individual call names, with no handshake and no
ordering/delivery guarantee, so the API shape is necessarily different
from the stream-oriented `TCPSocket`/`TCPServer` above:

- **`UDPSocket.bind(port)`** — resolves and binds to a specific local
  port (the same `getaddrinfo`/`AI_PASSIVE`/try-each-candidate dance
  `TCPServer.listen` uses, `SOCK_DGRAM` instead of `SOCK_STREAM`, no
  `listen(2)` call — UDP has no equivalent, there's nothing to listen
  *for*, just a socket ready to send and receive immediately). The
  "server" side: binds to a port callers already know to reach it at.
- **`UDPSocket.open()`** — opens a socket with no local address, letting
  the OS assign an ephemeral port on first use. The "client" side: don't
  care what port this sends from, just need to reach someone else's. A
  real scope cut, not an oversight: `open()` creates a plain `AF_INET`
  socket rather than resolving anything (there's no destination yet to
  resolve `AF_UNSPEC`/`getaddrinfo` against), so it can only reach IPv4
  destinations later — `UDPSocket.bind` has no such limit, since
  `AI_PASSIVE` resolution naturally produces whichever families the local
  machine actually supports.
- Both return the same new object kind, `DIAMOND_OBJECT_UDP_SOCKET`
  (`DiamondUdpSocketHandle`) — a raw fd, like the non-blocking `Socket`
  above, not `File`'s buffered `FILE*`: `sendto(2)`/`recvfrom(2)` need
  the peer address on every call, which buffered stdio read/write has no
  way to carry.
- **`.send(data, host, port)`** — resolves `host`/`port` via
  `getaddrinfo` (`AF_UNSPEC`, so both IPv4 and IPv6 destinations resolve)
  and tries `sendto` against each candidate in turn on this *same*
  existing socket until one succeeds — the send-time equivalent of
  `TCPSocket.connect`'s own multi-candidate resilience, needed here
  specifically because a socket opened via `UDPSocket.open()` might not
  share a family with whatever `host` resolves to first. Returns the
  `Int` byte count actually sent.
- **`.receive(n)`** — blocks until a datagram arrives, then returns a
  `Hash`: `{"data": ..., "host": ..., "port": ...}` — the payload (up to
  `n` bytes; a UDP datagram larger than the buffer is silently truncated
  by the kernel, standard `recvfrom` behavior, not something Diamond adds
  handling for) plus the sender's own address, extracted via
  `getnameinfo(..., NI_NUMERICHOST|NI_NUMERICSERV)` so `host` is always a
  plain address string, never a reverse-DNS lookup that could block or
  fail independently of the receive itself.
- **`.close()`** — idempotent, same as every other native handle here.

Building `.receive`'s three-key `Hash` result needed the exact same care
`IO.poll`'s own result did (see above): the `Hash` is rooted in
`registers[dest]` immediately after allocating it, and each `String`-
valued entry (`data`, `host`) roots its key with a `DIAMOND_NIL`
placeholder before allocating the real value — `port` is a scalar `Int`
with no allocation of its own, so its key and value go in with one
`hash_set` call, no placeholder needed, since nothing can trigger a
collection between allocating the key and calling `hash_set` on the very
next line.

`UDPSocket.bind`/`UDPSocket.open` are recognized in the compiler the same
way, one parser handling both forms (`bind` takes the 1-argument port,
`open` takes none) and erroring on anything else.

## Signals: `Signal.trap`

```ruby
def run()
  def handler()
    puts("shutting down")
  end
  Signal.trap("INT", handler)
  # ... normal program, including blocking calls ...
end
run()
```

`Signal.trap(name, handler)` registers a `Callable[0]` to run when the
named signal arrives — deliberately a small, fixed set of names rather
than every signal POSIX knows about: `"INT"` (Ctrl+C), `"TERM"` (`kill`),
`"HUP"` (terminal/controlling-process hangup). These three cover "someone
asked this process to stop," the motivating use case (a server closing
its listening socket and finishing in-flight work before exiting, rather
than just dying mid-request). `SIGKILL`/`SIGSTOP` can't be caught at the
OS level regardless; everything else (`SIGSEGV`, `SIGCHLD`, real-time
signals, ...) is out of scope for this first slice. `name` must be a
`String`, `handler` a `Callable` (a nested `def`, per the usual
top-level-`def`-isn't-a-value rule), or a rescuable `TypeError`; an
unrecognized name is also a `TypeError` rather than silently doing
nothing.

**Why a genuinely blocked native call needs separate handling from
CPU-bound code.** The actual OS signal handler installed by `Signal.trap`
only does what POSIX guarantees is async-signal-safe: set a
`volatile sig_atomic_t` and return (`diamond_signal_handler`, `src/vm.c`)
— resolving which Diamond closure to call and actually calling it happens
later, synchronously. For CPU-bound Diamond code, "later" means the very
next bytecode instruction: `run_chunk`'s own dispatch loop checks a
single cheap flag once per instruction (almost always false, negligible
steady-state cost) and, if set, invokes the pending handler(s) via the
same nested-`run_chunk` mechanism `DIAMOND_OP_CALL_CLOSURE` itself uses.
But a program genuinely blocked in a native call — `TCPServer#accept`,
`IO.poll`, `UDPSocket#receive` — might not reach "the next instruction"
for an arbitrarily long time (an idle server with nothing connecting,
by design, waits forever). So `Signal.trap`'s own `sigaction` call
deliberately omits `SA_RESTART`: the signal actually interrupts the
blocking syscall (`EINTR`) instead of the kernel silently resuming it as
if nothing happened, and each of those three call sites has its own
small retry loop that dispatches any pending signal(s) — running the
Diamond handler — before transparently retrying the syscall. From the
caller's own perspective, `.accept()` still either blocks until a real
connection or raises a real error; it's just no longer *unresponsive*
while doing so. `TCPSocket.connect` and buffered `File`/stdin reads are
not hardened this way — a real, documented scope cut for this first
slice, not an oversight; a signal arriving while blocked in one of those
won't be handled until the call completes on its own.

A trapped handler that raises an uncaught exception propagates exactly
like any other mid-dispatch failure (through the same `catch_exception`/
`VM_RETURN` machinery every other opcode uses) — rescuable at whatever
enclosing `rescue` was active when the interrupted instruction was about
to run, same as an ordinary exception from that point in the program
would be.

One non-obvious testing gotcha, worth recording since it cost real time
to track down: a non-interactive shell (`bash script.sh`, exactly what
every `tests/run.sh`/`packages/*/test.sh` invocation is) sets `SIGINT`
and `SIGQUIT` to be *ignored* for an asynchronous (backgrounded, `&`)
command — well-known bash/POSIX behavior, meant to keep a background job
alive when the terminal's own Ctrl+C targets the whole foreground
process group. Since `SIG_IGN` survives `exec(2)`, a `diamond` process
started as a plain `... &` from inside a script inherits `SIGINT`
already ignored, and a `kill -INT` sent to it visibly does nothing —
confirmed by identical code working every time run as a direct ad hoc
command and reliably failing every time run from inside a script file,
before the actual cause was traced to bash's own job-control behavior,
not a bug in `Signal.trap` itself. The fix, now used in
`tests/run.sh`'s own signal test: wrap the backgrounded command in a
subshell that does `trap - INT` (reset to default) before `exec`-ing the
real command, so the child never sees `SIG_IGN` in the first place.

## What's deliberately out of scope so far

- **TLS**: every socket above is plain text, TCP or UDP.
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
