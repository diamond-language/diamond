# I/O

This document covers Diamond's I/O and native-service surface. Future
directions are tracked in `docs/roadmap.md`.

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

Both `TCPServer.listen`/`listen_nonblocking` also accept an optional
trailing `reuse_port: <expr>` keyword, defaulting to `false` — sets
`SO_REUSEPORT` on the listening socket, letting more than one independent
listener bind the *same* port (the kernel load-balances new connections
across them), instead of the default exclusive-ownership behavior where a
second `listen` on an already-bound port fails with `IOError`. Off by
default so every existing single-listener server keeps today's stricter
behavior unless it explicitly opts in — see `docs/threads.md`'s
"Crossing the heap boundary" section and `packages/gremlin`'s
`gremlin_worker` for why a caller would want this: each thread in a
multi-threaded server opens its own listener (a `Listener` can never cross
a `Thread` boundary), all bound to the same port via this flag.

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

## TLS: `TLSSocket.connect`/`TLSServer.listen`/`.accept`

```ruby
listener = TLSServer.listen(8443, "cert.pem", "key.pem")
conn = listener.accept()
puts(conn.gets())
conn.write("hello over TLS\n")
conn.close()
listener.close()
```

```ruby
conn = TLSSocket.connect("example.com", 443)
conn.write("GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n")
puts(conn.read())
conn.close()
```

Built on OpenSSL (the project's one non-vendored external dependency —
see the README's build prerequisites) rather than hand-rolled, unlike
most of the rest of this codebase: TLS is exactly the kind of thing where
"implemented from scratch" is a real security liability, not just extra
work, so this is a deliberate exception to the project's usual
zero-external-dependencies stance.

- **`TLSSocket.connect(host, port)`** — resolves and connects like
  `TCPSocket.connect` (reuses the same `getaddrinfo`/try-each-candidate
  helper), then performs a client-side TLS handshake. **Certificate
  verification is on unconditionally** — the peer's certificate must
  chain to a CA in the system trust store (`SSL_CTX_set_default_
  verify_paths`, which also honors `$SSL_CERT_FILE`/`$SSL_CERT_DIR` —
  how `tests/run.sh`'s own TLS tests point this at a hermetic test CA
  instead of the real system store) *and* be valid for `host`
  (`SSL_set1_host`, SNI via `SSL_set_tlsext_host_name`). There is no
  `verify: false`-style escape hatch in this API — a connection to an
  untrusted or misnamed certificate raises a rescuable `IOError` rather
  than silently succeeding. A custom/pinned trust store (rather than the
  system default) is a real, deliberate scope cut, not an oversight —
  see "out of scope" below.
- **`TLSServer.listen(port, cert_path, key_path)`** — binds and listens
  like `TCPServer.listen`, then loads a certificate chain and private key
  (both PEM files) into one `SSL_CTX` reused across every `.accept()` on
  this listener, so a broken cert/key pair fails loudly at `.listen()`
  time rather than on whichever connection happens to arrive first, and
  the (potentially expensive) cert/key parsing happens once, not per
  connection.
- **`.accept()`** on a `TLSServer.listen` listener does the same blocking
  `accept()` a plain `TCPServer.listen` listener does (including the
  signal-interruptible retry loop from the Signals section above — a
  server genuinely idle with nothing connecting still responds promptly
  to a trapped signal), then a server-side TLS handshake. No
  `TLSServer.listen_nonblocking` — non-blocking sockets and TLS are not
  combined in this first slice.
- Both return the same new object kind, `DIAMOND_OBJECT_TLS_SOCKET` —
  `.read(n)`/`.read()`/`.gets()`/`.write(value)`/`.close()`, the exact
  same method surface and semantics as `File` (bounded/unbounded read,
  line read, EOF-as-nil), not `Socket`'s raw-fd/non-blocking shape — a
  TLS connection here is always blocking, so `packages/http`'s own
  `conn.gets()`/`conn.read()`/`conn.write()` calls work unchanged against
  either kind of connection. Internally a raw fd plus an OpenSSL `SSL *`
  rather than a buffered `FILE *`: `SSL_read`/`SSL_write` need to own the
  fd's I/O directly, not share it with libc stdio buffering underneath.
  `.close()` calls `SSL_shutdown` (best-effort, one call, sends this
  side's own `close_notify` without blocking on the peer's) then
  `SSL_free` then `close(fd)` — both steps are required; `SSL_free` alone
  neither sends a `close_notify` nor closes the fd on its own.
- A write to a connection whose peer has already reset it (not just
  cleanly closed) raises `SIGPIPE` by default, which would otherwise kill
  the whole process outright — this is what actually surfaced needing a
  fix here, not something specific to TLS: OpenSSL servers send a
  post-handshake `NewSessionTicket` message automatically for TLS 1.3,
  and a peer that disconnects without ever reading it (any short-lived
  connection that errors out or drops right after the handshake — the
  first realistic case in this codebase's own tests, from a TLS error
  test that deliberately raises immediately post-handshake) leaves that
  message unread in the kernel receive buffer at close time, which is
  Linux's own trigger for sending `RST` instead of a plain `FIN`.
  `diamond_vm_init` now ignores `SIGPIPE` process-wide (once, idempotent)
  so a write like this returns a plain `EPIPE`/`IOError` instead — every
  write-capable I/O path already turns a failed write into a catchable
  error via `errno`/`SSL_get_error`, so this was a straightforward latent
  bug fix, not a new behavior choice; it applies to the plain TCP/UDP
  write paths too, which had the same exposure but no test that happened
  to trigger it before now.

`TLSSocket`/`TLSServer` are recognized in the compiler the same way
`TCPSocket`/`TCPServer`/`UDPSocket` already are (gated on the identifier
not already being a local/function, so a variable or function actually
named `TLSSocket` shadows the builtin entirely). `TLSSocket.connect`
compiles to `DIAMOND_OP_TLS_CONNECT`; `TLSServer.listen` to
`DIAMOND_OP_TLS_LISTEN`.

## SQLite3: `SQLite3.open`/`.execute`/`.query`/`.last_insert_row_id`/`.close`

```ruby
db = SQLite3.open("data.db")
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 30])
id = db.last_insert_row_id()
db.query("SELECT * FROM people WHERE age >= ?", [18])
# => [{id: 1, name: Ada, age: 30}]
db.close()
```

`SQLite3.open(path)` opens (creating if missing, sqlite3's own default)
via `sqlite3_open`, the system `libsqlite3` — a genuinely external C
dependency, unlike `Regexp`'s in-repo `reginold`, linked as a plain
`-lsqlite3` rather than a bundled static archive. A failed open raises a
rescuable `SQLite3Error` — a dedicated exception class, not `IOError`,
since a corrupt or unopenable database file is a sqlite-specific
condition and every other failure this type can raise (a bad statement,
a bind/step error) is `SQLite3Error` too, giving callers one class to
`rescue` against for anything this type raises.

A `SQLite3` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_SQLITE3`), a thin wrapper around a `sqlite3 *` — the
same shape as `File`'s `DiamondFileHandle` around a `FILE *`, including
the same closed/open sentinel (`db` nulled by `#close()`, checked before
any other operation) and the same "sweeping an unreached-but-still-open
handle closes it as a safety net" GC behavior.

`SQLite3.open` is recognized in the compiler the same way `File.open`
is, compiling to a single `DIAMOND_OP_SQLITE3_OPEN dest, path`
instruction. `.execute`/`.query`/`.last_insert_row_id`/`.close` are
native `DIAMOND_OP_INVOKE` dispatch on a `DIAMOND_OBJECT_SQLITE3`
receiver, the same mechanism `File`'s own methods use:

- `.execute(sql)` / `.execute(sql, params)` prepares and runs one
  statement, discarding any rows it produces, and returns the number of
  rows it changed (`sqlite3_changes`) as an `Int` — the useful return
  value for `INSERT`/`UPDATE`/`DELETE`/DDL.
- `.query(sql)` / `.query(sql, params)` prepares and runs one statement,
  collecting every row into `Array[Hash]` (column name → typed value).
- `.last_insert_row_id()` returns the `Int` rowid of the most recent
  successful `INSERT` on this connection (`sqlite3_last_insert_rowid`).
- `.close()` is idempotent, exactly like `File#close`.

`params`, when given, is an `Array` bound *positionally* to a
statement's `?` placeholders (1-indexed, sqlite3's own convention) —
this is the injection-safe way to include a value in a query; never
interpolate a value directly into the SQL string. A parameter-count
mismatch raises `ArgumentError`; an unsupported Diamond value in
`params` (anything but `Int`/`Float`/`String`/`Bool`/`Nil`) raises
`TypeError`. Bind/column type mapping:

| Diamond → sqlite3 (`params`) | sqlite3 → Diamond (`query` results) |
|---|---|
| `Int` → `sqlite3_bind_int64` | `INTEGER` → `Int` |
| `Float` → `sqlite3_bind_double` | `FLOAT` → `Float` |
| `String` → `sqlite3_bind_text` | `TEXT`/`BLOB` → `String` (a Diamond `String` is already a raw byte buffer, so a blob's raw bytes need no separate representation) |
| `Bool` → bound as `Int` 0/1 | `NULL` → `Nil` |
| `Nil` → `sqlite3_bind_null` | |

`sqlite3_prepare_v2` only compiles the first statement up to a `;` and
leaves the rest unexecuted — silently dropping a second statement
chained after the first would be a real correctness trap, so
`.execute`/`.query` reject anything left over besides trailing
whitespace with a clear `SQLite3Error` rather than ignoring it. Each
call does its own prepare→bind→step→finalize; there is no persistent
prepared-`Statement` object to explicitly reuse across calls (see "out
of scope" below).

## PostgreSQL: `PostgreSQL.open`/`.execute`/`.query`/`.last_insert_row_id`/`.close`

```ruby
db = PostgreSQL.open("host=localhost dbname=myapp user=myuser password=secret")
db.execute("CREATE TABLE people (id SERIAL PRIMARY KEY, name TEXT, age INT)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 30])
id = db.last_insert_row_id()
db.query("SELECT * FROM people WHERE age >= ?", [18])
# => [{id: 1, name: Ada, age: 30}]
db.close()
```

`PostgreSQL.open(conninfo)` connects via `PQconnectdb`, the system `libpq`
(via `-lpq`) — a genuinely external C dependency exactly like `SQLite3`'s
`libsqlite3`. `conninfo` is passed through untouched to libpq itself, so
either its keyword/value form (`"host=... port=... dbname=... user=...
password=..."`) or a `postgresql://user:pass@host:port/dbname` URI works,
whatever libpq's own parser accepts. A failed connection raises a
rescuable `PostgreSQLError` — a dedicated exception class, not `IOError`,
for the same reason `SQLite3Error` exists: every failure this type can
raise (a bad conninfo, a malformed statement, a bind/exec error) is
`PostgreSQLError`, giving callers one class to `rescue` against.

A `PostgreSQL` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_POSTGRES`), a thin wrapper around a `PGconn *` — the
same shape as `SQLite3`'s `DiamondSqlite3Handle` around a `sqlite3 *`,
including the same closed/open sentinel (`conn` nulled by `#close()`,
checked before any other operation) and the same "sweeping an
unreached-but-still-open handle closes it as a safety net" GC behavior.

`PostgreSQL.open` is recognized in the compiler the same way `SQLite3.open`
is, compiling to a single `DIAMOND_OP_POSTGRES_OPEN dest, conninfo`
instruction. `.execute`/`.query`/`.last_insert_row_id`/`.close` are native
`DIAMOND_OP_INVOKE` dispatch on a `DIAMOND_OBJECT_POSTGRES` receiver, the
same mechanism `SQLite3`'s own methods use:

- `.execute(sql)` / `.execute(sql, params)` runs one statement via
  `PQexecParams` and returns the number of rows it affected (`PQcmdTuples`,
  parsed as an `Int`; `""` — e.g. from `CREATE TABLE` — means `0`) as an
  `Int`, the useful return value for `INSERT`/`UPDATE`/`DELETE`/DDL.
- `.query(sql)` / `.query(sql, params)` runs one statement and collects
  every row into `Array[Hash]` (column name → typed value), same shape
  `SQLite3#query` produces.
- `.last_insert_row_id()` runs `SELECT lastval()` and returns its `Int`
  result — Postgres has no direct equivalent of `sqlite3_last_insert_
  rowid`, so this approximates it for a `serial`/`GENERATED ALWAYS AS
  IDENTITY` column. It raises `PostgreSQLError` (`lastval` itself failing
  with "lastval is not yet defined in this session") if no sequence has
  been used yet on this connection — the more idiomatic Postgres pattern
  for a specific insert's id is `INSERT ... RETURNING id` via `.query()`
  directly, not this method.
- `.close()` is idempotent, exactly like `SQLite3#close`.

`params`, when given, is an `Array` bound *positionally* — but unlike
`SQLite3`, which binds directly to sqlite3's own native `?` placeholders,
Postgres's C API (`PQexecParams`) requires numbered `$1`/`$2`/...
placeholders. `SQLite3`'s `?` spelling is kept at the Diamond level anyway,
for API consistency between the two drivers and so either is a drop-in
target for the same `#query(sql, params)` contract (see
[`packages/arel/README.md`](../packages/arel/README.md)): the driver
translates `?` to `$1`/`$2`/... internally before calling `PQexecParams`,
skipping any `?` inside a single-quoted string literal (`''` is the
standard SQL escaped quote). A `?` used outside a string literal always
counts as a placeholder — Postgres's own JSONB "key exists" `?` operator,
for instance, isn't distinguishable from one here and isn't usable through
the params-array call form. A parameter-count mismatch raises
`ArgumentError`; an unsupported Diamond value in `params` (anything but
`Int`/`Float`/`String`/`Bool`/`Nil` — notably including a bignum-promoted
`Int`, matching `SQLite3`'s own same restriction) raises `TypeError`. Bind/
column type mapping:

| Diamond → Postgres (`params`, text format) | Postgres → Diamond (`query` results, by OID) |
|---|---|
| `Int` → decimal text | `int2`/`int4`/`int8` → `Int` |
| `Float` → `%.17g`, or `Infinity`/`-Infinity`/`NaN` | `float4`/`float8`/`numeric` → `Float` (lossy for a `numeric` outside `Float` precision) |
| `String` → passed through unchanged | `text`/`varchar`/`bpchar` → `String` |
| `Bool` → `"true"`/`"false"` | `bool` → `Bool` |
| `Nil` → SQL `NULL` (a null `paramValues` entry) | `NULL` → `Nil` |
| | any other type (`date`/`timestamp`/`json`/`jsonb`/`uuid`/`bytea`/arrays/...) → the raw `String` libpq's text format already returns |

The last row is a deliberate, documented scope cut, not silent data loss:
`bytea` in particular stays Postgres's default hex-text spelling
(`\x...`), not decoded to raw bytes. `PQexecParams` itself refuses more
than one SQL command per call regardless of parameter count, so unlike
`SQLite3` (which needs an explicit tail-content check after
`sqlite3_prepare_v2`), no separate multi-statement guard is needed here —
Postgres's own rejection surfaces as an ordinary `PostgreSQLError`.

Out of scope for this driver, deliberately: an Arel dialect visitor for
Postgres (a separate project once there's a real second dialect to
validate Arel's grammar seams against — see
[`packages/arel/ROADMAP.md`](../packages/arel/ROADMAP.md)), prepared/named
statements, asynchronous/non-blocking connections, connection pooling, and
binary-format result decoding.

## MySQL: `MySQL.open`/`.execute`/`.query`/`.last_insert_row_id`/`.close`

```ruby
db = MySQL.open("localhost", "myuser", "secret", "myapp", 3306)
db.execute("CREATE TABLE people (id INTEGER PRIMARY KEY AUTO_INCREMENT, name TEXT, age INT)")
db.execute("INSERT INTO people (name, age) VALUES (?, ?)", ["Ada", 30])
id = db.last_insert_row_id()
db.query("SELECT * FROM people WHERE age >= ?", [18])
# => [{id: 1, name: Ada, age: 30}]
db.close()
```

`MySQL.open(host, user, password, database, port)` connects via
`mysql_real_connect`, MariaDB Connector/C (via `-lmariadb`, libmysqlclient-API-
compatible) — a genuinely external C dependency exactly like `SQLite3`'s
`libsqlite3` and `PostgreSQL`'s `libpq`. Unlike `PostgreSQL.open`'s single
conninfo `String` (libpq parses that key=value format itself), these are five
required, explicit positional arguments — MariaDB Connector/C's
`mysql_real_connect` takes discrete fields with no such conninfo string to
parse, so this driver takes them the same explicit way rather than inventing
a DSN mini-language it would then own the parsing/escaping/documentation of.
`port` has no default; a bind port default would hide a real, easy-to-get-
wrong choice (`3306` vs. a nonstandard port) rather than a rarely-needed
knob. A failed connection raises a rescuable `MySQLError` — a dedicated
exception class for the same reason `SQLite3Error`/`PostgreSQLError` exist:
every failure this type can raise (unreachable host, bad credentials, a
malformed statement, a bind/exec error) is `MySQLError`, giving callers one
class to `rescue` against.

A `MySQL` value is a new GC-managed heap object kind
(`DIAMOND_OBJECT_MYSQL`), a thin wrapper around a `MYSQL *` — the same shape
as `SQLite3`/`PostgreSQL`'s own handles, including the same closed/open
sentinel (`conn` nulled by `#close()`, checked before any other operation)
and the same "sweeping an unreached-but-still-open handle closes it as a
safety net" GC behavior.

`MySQL.open` is recognized in the compiler the same way `SQLite3.open`/
`PostgreSQL.open` are, compiling to a single
`DIAMOND_OP_MYSQL_OPEN dest, host, user, password, database, port`
instruction. `.execute`/`.query`/`.last_insert_row_id`/`.close` are native
`DIAMOND_OP_INVOKE` dispatch on a `DIAMOND_OBJECT_MYSQL` receiver, the same
mechanism `SQLite3`/`PostgreSQL`'s own methods use:

- `.execute(sql)` / `.execute(sql, params)` prepares and executes one
  statement (via `mysql_stmt_*`, MariaDB Connector/C's real out-of-band
  binary parameter binding — not string interpolation/escaping) and returns
  `mysql_stmt_affected_rows` as an `Int`, the useful return value for
  `INSERT`/`UPDATE`/`DELETE`/DDL.
- `.query(sql)` / `.query(sql, params)` runs one statement and collects
  every row into `Array[Hash]` (column name → typed value), same shape
  `SQLite3#query`/`PostgreSQL#query` produce.
- `.last_insert_row_id()` is a direct `mysql_insert_id(conn)` call —
  simpler than `PostgreSQL#last_insert_row_id`'s own `SELECT lastval()`
  round-trip, since MySQL's client library tracks the connection's last
  `AUTO_INCREMENT` value itself. Unlike `lastval()`, it has no "not yet
  defined this session" failure mode: an unused connection just reads
  back `0`.
- `.close()` is idempotent, exactly like `SQLite3#close`/`PostgreSQL#close`.

`params`, when given, is an `Array` bound *positionally* through real
prepared-statement parameter binding — MySQL's own placeholder spelling is
already `?`, the same as `SQLite3`'s, so (unlike `PostgreSQL`) no
translation step is needed. A parameter-count mismatch (checked against
`mysql_stmt_param_count` after preparing) raises `ArgumentError`; an
unsupported Diamond value in `params` (anything but
`Int`/`Float`/`String`/`Bool`/`Nil`, matching both other drivers' same
restriction) raises `TypeError`. Bind/column type mapping:

| Diamond → MySQL (`params`, prepared-statement binary protocol) | MySQL → Diamond (`query` results, by field type) |
|---|---|
| `Int` → `MYSQL_TYPE_LONGLONG` | `TINY`/`SHORT`/`LONG`/`LONGLONG`/`INT24`/`YEAR` → `Int` |
| `Float` → `MYSQL_TYPE_DOUBLE` | `FLOAT`/`DOUBLE`/`DECIMAL`/`NEWDECIMAL` → `Float` |
| `String` → `MYSQL_TYPE_STRING` | `STRING`/`VAR_STRING`/`BLOB`/anything else → the raw `String` its text form decodes to |
| `Bool` → `MYSQL_TYPE_TINY` (`0`/`1`) | (no native boolean type — see below) |
| `Nil` → `MYSQL_TYPE_NULL` | `NULL` → `Nil` |

The last result row is a deliberate, documented scope cut, not silent data
loss: dates/times/JSON/bit fields all stay MySQL's own text spelling. MySQL
has no native boolean type — `TINYINT(1)` is only a convention, indistinguishable
at the protocol level from any other `TINYINT` column — so every integer
type decodes as `Int` here, the same tradeoff `SQLite3` (also boolean-less)
already makes, unlike `PostgreSQL`'s real `bool` OID. This connection is
never opened with `CLIENT_MULTI_STATEMENTS`, so unlike `SQLite3` (which
needs an explicit tail-content check) or `PostgreSQL` (whose `PQexecParams`
refuses multiple commands itself), a semicolon-separated second statement
here is simply a syntax error `mysql_stmt_prepare` itself raises as an
ordinary `MySQLError`.

Out of scope for this driver, deliberately, for the same reasons
`PostgreSQL`'s own scope cuts are: an Arel dialect visitor for MySQL,
connection pooling, and `unix_socket`/`CLIENT_MULTI_STATEMENTS` connection
options.

## Time: `Time.now`/`.utc_now`/`.at`/`.strftime`/`+`/`-`/comparisons

```ruby
t = Time.at(0).utc()
t.year()            # => 1970
t.strftime("%Y-%m-%d %H:%M:%S")  # => "1970-01-01 00:00:00"

start = Time.now()
elapsed = Time.now() - start     # => Float seconds
deadline = Time.now() + 30       # => Time, 30s from now
Time.now() < deadline            # => true
```

A real GC-managed heap object (`DIAMOND_OBJECT_TIME`), wrapping a
fractional Unix-epoch `Float` plus a `utc`/local flag that only
controls which of `gmtime_r`/`localtime_r` component accessors and
`.strftime` use — both are real, DST-aware, system-tzdata-backed libc
calls, so "supporting timezones" here is just calling the right one,
not hand-rolled timezone logic.

Three constructors, compiling to dedicated opcodes the same way
`File.open`/`SQLite3.open` do:

- `Time.now()` — current wall-clock time, local.
- `Time.utc_now()` — current wall-clock time, UTC.
- `Time.at(epoch)` — from a given `Int`/`Float` epoch, local.

(`Time.monotonic()`, documented in `docs/syntax.md`'s "Numbers"
section, is unrelated — a bare duration-only `Float`, not a `Time`.
Mixing the two would be actively misleading, since one is a calendar
instant and the other means nothing outside "difference between two
readings.")

Instance methods, all ordinary `.method()` calls through the same
generic dispatch every other native type uses:

- `.year()`/`.month()`/`.day()`/`.hour()`/`.min()`/`.sec()` → `Int`
- `.wday()` → `Int`, `0`=Sunday..`6`=Saturday; `.yday()` → `Int`,
  `1`-`366`
- `.to_i()` → `Int` (truncated epoch); `.to_f()` → `Float` (full
  epoch)
- `.strftime(format)` → `String`, a thin wrapper over libc `strftime`
  — the format string passes straight through, so supported directives
  are whatever the system's `strftime(3)` supports, not a
  Diamond-specific subset
- `.to_s()` → `String`, a fixed default format (matches what
  `puts`/string interpolation print for a `Time` too — one formatting
  implementation, not two)
- `.utc()` / `.localtime()` → a new `Time`, same epoch, `utc` flag
  flipped (immutable — the receiver is never modified)
- `.utc?()` → `Bool`

`+`, `-`, and comparisons (`<`/`<=`/`>`/`>=`/`==`/`!=`) work directly,
matching Ruby — `Time` is the **one** native (non-`Instance`) type
with real operator support; see `docs/syntax.md`'s "Operator
overloading" section for why every other native type doesn't get this
for free:

- `t + n` (`Int`/`Float` seconds) → `Time`, offset forward, same `utc`
  flag as `t`. `t + t2` is a `TypeError` (Ruby doesn't support adding
  two `Time`s either).
- `t - n` → `Time`, offset backward. `t1 - t2` → `Float` seconds
  between them (Ruby's own dual-purpose `-`).
- `t1 < t2` / `<=` / `>` / `>=` — `Time` vs `Time` only, no
  `Time`-vs-numeric ordering.
- `t1 == t2` / `!=` — compares the underlying epoch, **not** identity
  and **not** the `utc`/local flag: two separately constructed `Time`s
  at the same instant are `==` regardless of which one is `.utc`,
  exactly like Ruby.

## Process: `Process.run`

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

**v1 scope, deliberately minimal** (settled with the user before
building this — a non-blocking `Process.spawn` with a live handle,
`.wait()`/`.kill()`/streaming output, is a real possible future
extension, not this one):

- The child's stdin is always `/dev/null` — there is no way to feed it
  data. A command that tries to read from stdin sees immediate EOF (e.g.
  `Process.run(["cat"])` returns empty stdout and exit code `0`
  immediately, rather than hanging).
- Fully blocking/synchronous — no way to run a child in the background,
  poll it, or kill it early. A long-running or hung child blocks the
  calling Diamond program for as long as it runs.
- A trapped `Signal` (see `Signal.trap` above) does not get to run while
  a `Process.run` call is blocked waiting on the child — it runs once the
  child exits and `Process.run` returns, not immediately. (Unlike
  `IO.poll`, which does handle this — see that section above.)
- Command-not-found and other spawn failures (a bad path, no exec
  permission, ...) raise `IOError` synchronously, the same call that
  fails, rather than exit code `127` the way a real shell reports it.

## What's deliberately out of scope so far

- **Multiple `print`/`puts` arguments**: `puts(a, b)` (Ruby-style, one
  line per argument) is not supported — exactly one argument, matching
  the narrowest useful slice.
- **Error handling for stdout write failures**: a failed `fwrite`/`fputc`
  to stdout (e.g. a broken pipe) is not currently surfaced as a
  rescuable exception, unlike `File#write`'s own `ferror` check; this
  mirrors most languages' baseline `print`, but is a known
  simplification, not a deliberate design stance. It no longer *kills
  the process* via `SIGPIPE` (see the TLS section's note on
  `diamond_vm_init` ignoring it process-wide) — a broken stdout pipe now
  fails each write silently and the program continues, rather than
  either dying outright or raising.
- **A custom/pinned TLS trust store**: `TLSSocket.connect` always
  verifies against the system trust store; there's no way from Diamond
  code itself to trust an additional/different CA (only the
  `$SSL_CERT_FILE`/`$SSL_CERT_DIR` environment-variable override
  OpenSSL's own default-paths lookup already respects). No call site in
  this codebase needed one yet.
- **TLS session resumption, client certificates, ALPN**: none of
  OpenSSL's more advanced connection-negotiation features are exposed —
  every connection is a fresh, full handshake with no protocol
  negotiated beyond default TLS.
- **File mode validation**: `File.open` passes `mode` straight through
  to `fopen` with no Diamond-level checking.
- **Reusable prepared `Statement` objects**: `.execute`/`.query` each do
  their own one-shot prepare→bind→step→finalize; there's no way to
  prepare a statement once and bind/step it repeatedly across calls.
- **Transactions as a dedicated API**: no `.transaction { ... }`-style
  block helper — `db.execute("BEGIN")`/`"COMMIT"`/`"ROLLBACK"` already
  work today through plain SQL, so this is a convenience layer to add
  later, not missing functionality.
- **Named (`:name`) bind placeholders, connection-open flags/mode**:
  `SQLite3.open` takes only a path (sqlite3's own create-if-missing
  default, no read-only/flags argument); `params` binds positionally
  (`?`) only.
- **`Time.parse`, named timezones, a separate `Date`-only type**: no
  parsing a `Time` from a `String`, no picking a timezone other than
  the process's own local zone or UTC (Ruby itself needs the `tzinfo`
  gem for that), no date-without-time type distinct from `Time`.

Each of these is a plausible next slice, sized independently rather than
attempted together.
