# cancellation

Cooperative cancellation, monotonic deadlines, and scopes that join their tasks.
Install this cut and `require_cut "cancellation"`. Version 0.5.0 requires a
Diamond runtime with channel readiness waits, cancellation options for `IO.poll`,
`TCPSocket.connect_nonblocking`, and `DNS.resolve`.

```ruby
require_cut "cancellation"

def worker(token, jobs)
  loop do
    job = token.receive(jobs)
    break if job == nil
    token.checkpoint()
    puts(job)
  end
end

def run(scope)
  jobs = Channel.new(2)
  scope.spawn(worker, jobs)
  scope.token().send(jobs, "hello")
  jobs.close()
end
Cancellation.scope(run, 5)
```

`Source.new(parent_token = nil, timeout_seconds = nil)` creates a source;
`source.token()` exposes checkpoints and `source.cancel()` broadcasts to all
copies. A child source observes its parent without cancelling it. Tokens and
sources can cross thread/channel boundaries; scopes stay in their owning thread.

- `token.checkpoint()` raises `Cancellation::Cancelled` or its subclass
  `Cancellation::DeadlineExceeded`.
- `token.sleep(seconds)`, `token.receive(channel)`, and
  `token.send(channel, value)` are cancellation-aware waits.
- `token.poll(readables, writables)` waits on pollable I/O with cancellation and
  the earliest inherited deadline. Readiness is a hint and can be all false.
- `token.read(socket, count)` returns up to `count` bytes or `nil` at EOF;
  `token.write(socket, string)` writes the entire string and returns its byte
  count. Both retry nonblocking TCP `Socket` operations. A cancelled write may
  have sent a prefix already; close owned sockets in `ensure`. Blocking File/TLS
  handles are rejected by these helpers.
- `Cancellation.scope(body, timeout_seconds = nil)` joins all spawned children.
  Body failure cancels them; child failure cancels siblings. Cleanup runs before
  propagating errors, including non-Exception raised values.
- Explicit `Scope.new(parent, timeout)`, `spawn`, `cancel`, `join`, and `close`
  support owners such as services; ensure `close()` runs when leaving the owner.

Waits use native channel notifications and the earliest inherited monotonic
deadline. Ordinary workers do not poll periodically. A VM with `Signal.trap`
handlers returns at most every 10 ms of requested waiting to dispatch signals;
this is not a hard response-latency guarantee. CPU loops need checkpoints.
Existing blocking native calls
are not interrupted; uncooperative tasks can prevent scope exit. Cancellation
can race with successful operations. Expected child cancellation is absorbed at
the task boundary, while scope deadline expiry still propagates from `scope`.

Use `ensure` for resource cleanup and handle cancellation before generic retry
rescues. Scope bodies may capture locals; spawned callables must satisfy normal
Thread transfer rules. The [design](../../docs/cancellation.md) defines error
precedence, deadline behavior, limitations, and the native wait lifetime protocol.

Run `DIAMOND_BIN=/absolute/path/to/diamond bash test.sh` for interpreted and
compiled tests. This package is experimental and is not added to the public
registry inventory by this change.

## DNS and outbound TCP connections

Version 0.5.0 adds `token.resolve(host)`, returning unique IPv4/IPv6 address
strings in system resolver order. `token.connect(host, port)` accepts either a
hostname or a numeric IP literal, tries addresses in order after connection
failures, and returns a connected nonblocking `Socket`. DNS and every connection
attempt share the token's existing deadline, including inherited deadlines.
There is no per-address timeout reset or parallel Happy Eyeballs racing.

Cancellation promptly stops waiting for DNS. A libc lookup already in progress
may continue in a native worker, but it retains no VM objects and cannot delay
VM cleanup or process exit. At most eight native lookups can be outstanding
process-wide, including abandoned lookups. If all are busy, another hostname
lookup raises `IOError` immediately. Slots and native buffers are reclaimed when
those lookups finish. Numeric IP literals bypass the worker limit. Resolution
uses the system resolver, including local hosts configuration; this cut adds no
DNS cache.

Failed and cancelled connection attempts close their descriptors. After
success, the caller owns the socket and closes it in `ensure`; token `read` and
`write` helpers work as before. For numeric-only callers, `token.connect_address`
retains the direct connection path. TLS handshakes remain separate blocking work.
See [outbound TCP](../../docs/networking.md#nonblocking-outbound-tcp) for an example.
