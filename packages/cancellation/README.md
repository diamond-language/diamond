# cancellation

Cooperative cancellation, monotonic deadlines, and scopes that join their tasks.
Install this cut and `require_cut "cancellation"`. Version 0.4.0 requires a
Diamond runtime with channel readiness waits, cancellation options for `IO.poll`,
and `TCPSocket.connect_nonblocking`.

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

## Outbound TCP connections

Version 0.4.0 adds `token.connect(address, port)`, returning a connected
nonblocking `Socket`. Numeric IPv4/IPv6 addresses only: hostnames are rejected
rather than performing a blocking DNS lookup. Parent cancellation and deadlines
apply throughout connection establishment. Failures and cancellation close the
pending descriptor; after success, the caller closes the socket in `ensure`.
Use the existing token `read` and `write` helpers with the returned socket.
See [outbound TCP](../../docs/networking.md#nonblocking-outbound-tcp) for an example
and the lower-level `TCPSocket.connect_nonblocking` / `Socket.finish_connect` API.
