# Cooperative cancellation: first implementation

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

`token.sleep`, `token.receive(channel)`, and `token.send(channel, value)` check
at intervals of at most 10 ms of requested sleep. Scheduling, GC, and native
work can add latency; this is not a real-time bound. Channel wrappers preserve
close/EOF and type errors. A checkpoint precedes each operation; cancellation
can race with a successful operation, so callers must account for side effects.

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

## Boundaries and next native-runtime milestone

This package deliberately proves the semantics before extending the VM.
Existing `Channel.receive`, `Thread.join`, sockets, SQL calls, and arbitrary
CPU loops do not gain cancellation implicitly. A scoped child must checkpoint
and use bounded/nonblocking I/O; an uncooperative child can still hold up joining.
Supervisor.stop still waits: cancel the shared source before calling it, and
catch expected cancellation at the supervised child's boundary so it returns
normally instead of restarting. Process crashes do not execute ensure.

The next native milestone is a shared cancellation state with registered wakeups
for channel waits and pollable I/O. That requires a lock-order and lifetime
protocol: register while holding the wait lock, recheck cancellation before
sleeping, unregister before destroying the waiter, and never run user code under
those locks. Socket/SQL interruption and implicit VM checkpoints require separate
contracts; polling wrappers do not claim to solve those problems.

## Reference service

[Job service](../examples/job_service/README.md) uses SQLite persistence, HTTP,
and a supervised worker. Jobs have checkpoints, independent monotonic deadlines,
and a durable cancellation request. Service shutdown cancels the root token,
requeues interrupted work, closes the worker database in ensure, and joins the
supervisor. Normal job failures retain the jobs package's retry/backoff policy;
cancellation and timeout are separate terminal outcomes.
