# Cooperative cancellation

## Contract

A `Cancellation::Source` owns an idempotent `cancel()` capability. Its token
can be passed through a Thread, Channel, or Supervisor argument. Sources and
tokens are ordinary Diamond objects; their cancellation state is a shared
Channel whose closure is permanent and visible in every heap. Cancelling a
source does not consume a message or cancel its parent. A child source retains
its parent token and optionally adds a deadline. Checkpoints consult both.

Deadlines use `Time.monotonic()` in seconds, calculated when the source is
created, and stay absolute when copied into another thread. A zero timeout
expires immediately; negative durations fail. Explicit cancellation is checked
before ancestor cancellation/deadlines, then the local deadline. Cancellation
is terminal; racing cancellation and completion may result in either outcome.

`checkpoint()` raises `Cancellation::Cancelled`; deadline expiry raises its
subclass `DeadlineExceeded`. These are ordinary exceptions, so normal `ensure`
cleanup runs. Broad `StandardError` rescues must re-raise cancellation or handle
it explicitly before applying retry policies. No asynchronous exception is
injected into another thread and no thread is forcibly killed.

`token.sleep`, `token.receive(channel)`, and `token.send(channel, value)` use
native channel wakeups. Each wait registers with the target channel and every
ancestor cancellation source, and uses the earliest absolute deadline. Closing
any source wakes its waiters directly. Ordinary worker waits have no periodic
polling timer. Scheduling and GC can still add latency; this is not a real-time
guarantee. Channel wrappers preserve close/EOF and type errors. A checkpoint
precedes each operation; cancellation can race with a successful operation,
so callers must account for side effects.

A VM with installed `Signal.trap` handlers returns from a native wait at least
every 10 ms of requested waiting so the VM can dispatch pending handlers. An
interrupted system call also returns to the VM. The token wrapper retries
spurious wakeups after a checkpoint; signal handlers can safely cancel sources.

`Cancellation.scope(callback, timeout)` owns every child started with
`scope.spawn(callable, *args)`. The callable receives `(token, *args)` and must
obey ordinary Thread transfer rules. The body may capture local state; child
callables may not. Normal exit joins children; body failure cancels and joins
children before re-raising. A child failure cancels siblings, joins all children,
and propagates the first failure observed in spawn order. The body's exception
wins if both fail. Expected child cancellation ends that child normally. The
scope's token is checked after successful joining so deadline expiry is not
reported as success. Explicit `Scope.close()` cancels then joins, and can raise
a child error. Scope objects are owned by their creating thread.

## Boundaries

Existing `Channel.receive`, `Thread.join`, sockets, SQL calls, and arbitrary
CPU loops do not gain cancellation implicitly. A scoped child must checkpoint
and use bounded/nonblocking I/O; an uncooperative child can still hold up joining.
Supervisor.stop still waits: cancel the shared source before calling it, and
catch expected cancellation at the supervised child's boundary so it returns
normally instead of restarting. Process crashes do not execute ensure.

## Native wait protocol

`Channel.wait_readable(cancellations, deadline)` and `wait_writable` are
readiness hints, not reservations. They return `nil` on readiness, target
closure, cancellation-source closure, deadline expiry, or a spurious wakeup.
Callers recheck cancellation and retry their nonblocking operation.

Each wait owns a nonblocking pipe and registers a notification link on every
channel. Registration and the readiness/closed check hold that channel's lock,
so a transition either precedes the check or signals the registered pipe.
Send, receive, and close notify registered waiters under the same lock. Cleanup
unregisters every link before closing the pipe or freeing its storage. Only one
channel lock is held at a time; no user code runs under it. Live receiver and
argument handles keep the channels alive until the wait returns. Duplicate
cancellation channels are allowed.

Deadlines are absolute monotonic timestamps. The native wait recomputes its
relative `poll` timeout from that timestamp, including after long timeout
segments, without extending the deadline. Each active wait uses two file
descriptors; allocation or descriptor exhaustion raises an ordinary runtime
error and leaves no registered waiter behind.

`IO.poll` accepts cancellation options and uses the same registration protocol,
adding the notification pipe alongside the caller's descriptors. Its original
integer timeout form remains available. The options form uses an absolute
deadline, so retries do not extend it. Spurious returns have no ready entries.

`token.poll(readables, writables)` checks cancellation before and after waiting;
a token cancelled alongside socket readiness raises instead of reporting ready.
`token.read(socket, count)` retries until bytes or EOF, and
`token.write(socket, string)` retries partial writes until all bytes are sent.
These helpers accept only nonblocking TCP `Socket`s. They neither close the
socket nor roll back a partial write: use `ensure` for ownership cleanup, and
account for a prefix already sent if a write is cancelled. Sockets stay in their
owning VM; pass a cancellation source across threads instead of the socket.

Blocking TLS/socket connection setup, SQL interruption, and implicit VM
checkpoints still require separate contracts. Existing HTTP servers must opt
into token-aware waits; installing the package does not alter their I/O loops.

## Reference service

[Job service](../examples/job_service/README.md) uses SQLite persistence, HTTP,
and a supervised worker. Jobs have checkpoints, independent monotonic deadlines,
and a durable cancellation request. Service shutdown cancels the root token,
requeues interrupted work, closes the worker database in ensure, and joins the
supervisor. Normal job failures retain the jobs package's retry/backoff policy;
cancellation and timeout are separate terminal outcomes.
