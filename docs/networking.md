# Networking and signals

[I/O and native services](io.md) · Previous: [Local I/O](local-io.md) · Next: [Databases](databases.md)

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

`TCPSocket.connect(host, port, options)` also takes an optional third
`options` Hash (defaulting to `nil`, meaning none of this applies —
existing calls are unaffected):

```ruby
client = TCPSocket.connect("example.com", 8080,
    {"connect_timeout_ms": 5000, "read_timeout_ms": 10000})
```

- **`connect_timeout_ms`** — bounds only the `connect(2)` step. Applies
  per candidate address, not as one aggregate deadline across every
  address `getaddrinfo` returns (an ordinary connection refusal already
  moves on to the next candidate; a timeout does the same). Implemented
  by making the socket non-blocking before connecting and `poll`ing for
  writability, then restoring blocking mode for every operation after —
  everything past this point behaves exactly as if the socket had always
  been an ordinary blocking one.
- **`read_timeout_ms`**/**`write_timeout_ms`** — `SO_RCVTIMEO`/
  `SO_SNDTIMEO` on the connected socket, applying to every subsequent
  `.read()`/`.gets()`/`.write()` on it. A timed-out read/write raises the
  same rescuable `IOError` an ordinary failed read/write already does
  (`strerror(EAGAIN)`, i.e. "Resource temporarily unavailable") — no new
  error path, just a new way for the existing one to trigger.

Omitting a key leaves the corresponding OS default (block forever)
untouched. An unrecognized key, or a value of the wrong type, raises a
`TypeError` rather than being silently ignored — the same "an
unrecognized name is a mistake, not a no-op" stance `Signal.trap` already
takes.

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
  listeners/`Socket`s, or a `Process.spawn` handle's `Process::Stream`
  (see the Process section below) — an ordinary blocking listener/File
  is rejected, since polling a blocking-mode fd is meaningless: nothing
  in this VM ever puts one in non-blocking mode, so it would always
  appear either always-ready or never-ready depending on kernel
  buffering, never the genuine signal a caller needs) and a millisecond
  timeout (`-1` blocks
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

When testing `Signal.trap` from a shell script, remember that bash starts
asynchronous commands with `SIGINT` and `SIGQUIT` ignored. That disposition
survives `exec`, so `kill -INT` will not reach a Diamond handler in a process
started with a plain `... &`. Reset the signal before executing the program:

```sh
(trap - INT; exec ./build/diamond signal_test.di) &
```

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

- **`TLSSocket.connect(host, port, options)`** — resolves and connects
  like `TCPSocket.connect` (reuses the same `getaddrinfo`/try-each-
  candidate helper, and accepts the identical `connect_timeout_ms`/
  `read_timeout_ms`/`write_timeout_ms` in `options`), then performs a
  client-side TLS handshake. **Certificate verification is on
  unconditionally** — the peer's certificate must chain to a trusted CA
  *and* be valid for `host` (`SSL_set1_host`, SNI via
  `SSL_set_tlsext_host_name`). There is no `verify: false`-style escape
  hatch in this API — a connection to an untrusted or misnamed
  certificate raises a rescuable `IOError` rather than silently
  succeeding.

  The trust anchor itself is a real option, though:

  ```ruby
  conn = TLSSocket.connect("internal.example.com", 443,
      {"ca_file": "internal-ca.pem"})

  conn = TLSSocket.connect("example.com", 443,
      {"cert": "client-cert.pem", "key": "client-key.pem"})
  ```

  - **`ca_file`**/**`ca_path`** — load a caller-supplied CA bundle file
    and/or directory (`SSL_CTX_load_verify_locations`) as the trust
    anchor instead of the system trust store, for an internal CA or a
    hermetic test server. Neither given keeps the original default
    (`SSL_CTX_set_default_verify_paths`, honoring `$SSL_CERT_FILE`/
    `$SSL_CERT_DIR` — how `tests/run.sh`'s own TLS tests point this at a
    hermetic test CA instead of the real system store). Verification
    itself is never optional either way — this only changes *which*
    trust store is consulted, not whether one is.
  - **`cert`**/**`key`** — a client certificate chain and private key
    (both PEM files) for mutual TLS, presented only if the server
    actually requests one during the handshake (ordinary TLS
    negotiation — this never forces client-cert auth on a server that
    doesn't ask). Must be given together; a lone `cert` or `key` is a
    `TypeError`, checked before any network activity. A missing or
    unreadable file, or a key that doesn't match the certificate, raises
    a rescuable `IOError`.

  - **`alpn`** — an Array of protocol name Strings (RFC 7301, 1-255 bytes
    each), offered to the server in preference order for Application-
    Layer Protocol Negotiation:

    ```ruby
    conn = TLSSocket.connect("example.com", 443, {"alpn": ["h2", "http/1.1"]})
    conn.alpn_protocol()   # => "h2", or nil if nothing was negotiated
    ```

    Validated (non-empty, every element a 1-255-byte String) before any
    network activity, same as the `cert`/`key` pairing check. Which
    protocol actually gets picked is the *server's* call
    (`SSL_select_next_proto`, the standard algorithm both this client and
    `TLSServer.listen` below use): the first entry in the *server's* own
    list that the client also offered, not the client's own preference
    order. A client and server with no protocol in common is a fatal
    handshake failure (`IOError`), not a silent "no protocol negotiated"
    — there's nothing left to fall back to once neither side will accept
    what the other proposed.
  - **`session`** — a previously-serialized session blob (see `#session`
    below) to attempt resumption with. Always best-effort: a blob that
    fails to parse (corrupt, or from an incompatible OpenSSL build or a
    since-rotated server ticket key) is silently ignored rather than
    raising — TLS itself transparently falls back to an ordinary full
    handshake when resumption doesn't happen, exactly the same outcome as
    if this option had never been given. `#session_reused?` (below) tells
    the caller which actually happened.

  As with `TCPSocket.connect`, an unrecognized `options` key or a value
  of the wrong type is a `TypeError`, and `options` itself may be `nil`
  (the default) for the original, unchanged behavior.
- **`TLSSocket#alpn_protocol()`** — the negotiated ALPN protocol name as
  a `String`, or `nil` if ALPN wasn't offered or nothing was negotiated.
  Works the same way on both a client connection and a
  `TLSServer.listen` listener's accepted connection.
- **`TLSSocket#session()`** — serializes the connection's current session
  (DER-encoded via OpenSSL's `i2d_SSL_SESSION`, an opaque `String` blob)
  for later resumption via a future `TLSSocket.connect`'s own `session`
  option, or `nil` if no session is available yet. **Call this only
  after the connection has done at least one read** (`.gets()`/`.read()`)
  — a TLS 1.3 session ticket normally arrives as a post-handshake message
  that OpenSSL only actually processes (via a callback registered
  internally) during a later read, not synchronously inside
  `SSL_connect`/`SSL_accept` itself, so `#session` called immediately
  after connecting, with no read in between, can legitimately still
  return `nil` even though a ticket is about to arrive.
- **`TLSSocket#session_reused?()`** — `Bool`, whether this connection
  actually resumed a previous session (`SSL_session_reused`) rather than
  performing a full handshake. The one reliable way to confirm resumption
  worked, since a `session` option that fails to resume (an expired
  ticket, say) doesn't raise — the connection just silently falls back to
  a full handshake instead.
- **`TLSServer.listen(port, cert_path, key_path, options)`** — binds and
  listens like `TCPServer.listen`, then loads a certificate chain and
  private key (both PEM files) into one `SSL_CTX` reused across every
  `.accept()` on this listener, so a broken cert/key pair fails loudly at
  `.listen()` time rather than on whichever connection happens to arrive
  first, and the (potentially expensive) cert/key parsing happens once,
  not per connection. `options` (optional, a `Hash` or `nil`) supports
  one key so far: **`alpn`** — an Array of protocol name Strings this
  server is willing to negotiate, in *this server's own* preference
  order (see `#alpn_protocol` above for how a mismatch is handled).
  Session resumption needs no server-side option at all: reusing one
  `SSL_CTX`/session-ticket key across every `.accept()` on a listener,
  which `TLSServer.listen` already does, is the entire server-side
  requirement — a returning client's `TLSSocket.connect(..., {"session":
  ...})` just works against it.
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
