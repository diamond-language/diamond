# Concurrency internals

This document records the architectural constraints behind Diamond's Fiber
and Thread implementations. For language-level usage, see [Fibers](fibers.md)
and [Threads](threads.md).

## Fibers

Each `DiamondFiber` owns a native stack allocated with `mmap`, including a
guard page, and a POSIX `ucontext_t`. `swapcontext` suspends the complete
interpreter activation, so nested calls, rescue handlers, generic bindings,
and registers remain intact without a separate bytecode checkpoint format.

The lifecycle is:

```text
NEW -> RUNNABLE -> RUNNING -> SUSPENDED -> RUNNABLE
                          \-> COMPLETED
                          \-> FAILED
```

The C scheduler is a single-threaded FIFO queue. It can run one fiber or drain
the queue, requeueing suspended fibers and continuing after individual fiber
failures. This scheduler is an embedding API; Diamond programs build scheduling
policy from `Fiber#resume`, non-blocking I/O, and `IO.poll`.

Fiber handles are managed heap objects. Collection marks the entry closure,
resume and result values, parked frame chain, and every resumer frame chain.
Queued fibers are roots when their queue is bound to a VM. Sweeping a handle
releases its native stack.

## Threads

Each spawned `Thread` owns an independent `DiamondVm` and a clone of the
program's function, class, and interface tables. This avoids sharing the GC,
inline caches, object shapes, or runtime class tables between pthreads. The
tradeoff is higher memory use and copy-only communication.

Arguments and results use `copy_value_into_vm` in rebase mode. Composite
values are copied recursively, instance class pointers are rebased to the
corresponding class in the destination program, and zero-capture closures keep
their function-table index. Values tied to a heap or native resource are
rejected. Class-variable storage is heap state and therefore is not inherited
from the spawning VM.

`Thread#join` has three outcomes: copy a normal result into the caller, copy
and re-raise an uncaught Diamond exception, or synthesize `ThreadError` for an
internal VM failure. Joining is mutex-protected and idempotent. The child sets
an atomic finished flag for `alive?`; successful `pthread_join` supplies the
happens-before edge for reading all other result fields.

A thread handle owns its pthread, child VM, cloned program, and join mutex.
Garbage collection block-joins an abandoned thread before freeing those
resources. A process-wide atomic count enforces the 64-thread limit.

Fiber setup uses thread-local native state, allowing fibers to run inside
multiple isolated Diamond VMs concurrently.

## Channels

Every other native handle in this codebase (Socket, File, Thread, ...) is
owned by exactly one VM/heap. `DiamondChannel` is the first that is
genuinely *shared*: `DiamondChannelHandle` is a thin, refcounted reference to
it, and the same channel can be reachable from several independent VM heaps
at once (passed as a `Thread.new` argument, sent through another channel, or
copied inside an array/hash/instance). `copy_value_into_vm`'s own
`DIAMOND_OBJECT_CHANNEL` case bumps the refcount and hands the destination
VM a fresh handle onto the same channel -- never a copy of its contents --
in either of that function's two modes (Thread's rebase mode or
`ProgramBuilder#run`'s adopt mode; a channel needs neither, since nothing
about it points into a program's own tables the way an `Instance` does).

**Where a queued value lives.** A queued value can't sit in the sender's
heap (nothing keeps rooting it once `send` returns) or the receiver's heap
(doesn't exist yet) or unmanaged raw memory (would need reimplementing
deep-free for arbitrary nested values by hand). Instead, every
`Channel.new(capacity)` gets its own private `DiamondVm` plus a
`clone_program_from_chunk` clone of the constructing thread's class/
function/interface tables -- the identical clone `Thread.new` already makes
for a spawned thread's own use -- purely as GC-managed storage. Nothing ever
runs bytecode against this private VM. `send` rebases the value from the
sender's own ambient classes *into* the channel's private program (the same
call shape as `Thread.new`'s own argument copy); `receive` rebases the
dequeued value *out* into the receiver's own ambient classes (the same call
shape as `Thread#join`'s own result copy) -- so every value is deep-copied
on the way in and again on the way out, and only the channel itself is ever
shared.

**GC safety for the private VM.** `DiamondVm.extra_roots`/`extra_root_count`
(two fields added for exactly this) let the private VM's own collections
find its queued values -- nothing else on `DiamondVm` reaches an external
buffer like this. They point at the channel's own flat `queue` array and
stay current with `count`, updated under the channel's own mutex immediately
around any mutation. A `DiamondVm` is normally driven by exactly one OS
thread for its whole lifetime (a spawned Thread's `child_vm`); a channel's
private VM relaxes that to the strictly necessary invariant instead -- only
one OS thread touches it *at a time* -- which holds because every access
(`send`/`receive`, including every allocation they trigger on it) happens
while holding the channel's own mutex.

**Locking.** One `pthread_mutex_t` plus two `pthread_cond_t` (`not_empty`,
`not_full`) -- this codebase's first condition variables; every other mutex
here (`DiamondThread.join_lock`) only ever guards a one-shot idempotency
flag, never a repeated producer/consumer wait. `close()` broadcasts both
condvars so every blocked `send`/`receive` wakes to re-check the closed
condition. Blocking `send`/`receive` do not service `Signal.trap` handlers
while parked, matching `Thread#join`'s own `pthread_join` call -- a known,
already-accepted asymmetry versus `IO.poll`/`accept`'s own EINTR+signal-
dispatch retry loop, not a new inconsistency; `try_send`/`try_receive` are
the escape hatch for anyone who wants to poll instead.

**Lifecycle.** Freeing (in both `sweep_list` and `free_object_list`, the
same duplicated-cleanup shape `DiamondThread` itself uses) decrements the
refcount and only actually tears anything down -- destroying the mutex/
condvars, freeing the private VM/program/queue -- once it hits zero. Safe by
construction: reachability is exactly what the GC just proved false for
every referencing handle, so no thread can be mid-call against a channel
whose last reference is about to disappear. Any values still queued at that
point are reclaimed automatically along with the private VM itself -- no
separate walk of `queue[]` is needed.

## Supervisors

`DiamondSupervisor` collects up to `DIAMOND_MAX_SUPERVISOR_CHILDREN` (32)
worker slots (`DiamondSupervisorChild`), each backed by exactly **one**
`pthread_create` call for its entire supervised lifetime -- unlike `Thread`
(one OS thread per spawn), a supervised child's own OS thread loops
internally across every restart rather than being torn down and recreated
per attempt. Like `DiamondChannel`, a `DiamondSupervisor` is refcounted
(`DiamondSupervisorHandle`), but it is deliberately *not* one of
`copy_value_into_vm`'s handled kinds -- it can never cross a `Thread.new`/
`Channel` boundary at all, so in practice at most one real OS thread (the
one that created it) ever calls its own methods; the mutex below exists to
protect against that `Supervisor`'s *own* child retry-loop threads, not
against concurrent callers.

**Where a restarted attempt's arguments live.** `add_child`'s arguments
need to survive arbitrarily many restarts, each with its own fresh,
isolated heap -- so each child gets a private `args_vm`, the identical
"private `DiamondVm` used purely as GC-managed storage, rooted via
`extra_roots`" trick `Channel`'s own `private_vm` already established,
just write-once rather than a growing/shrinking queue. `program_template`
(a `clone_program_from_chunk` clone, the same call `Thread.new`/`Channel.new`
already make) is cloned exactly **once**, at `add_child` time, and reused
unmodified across every restart -- a program's own function/class/interface
tables never change once compiled, so there is no need to re-clone per
attempt the way `Thread.new` clones fresh per spawn.

**The retry loop.** `supervisor_child_entry_trampoline` is `thread_entry_
trampoline`'s shape (build a synthetic `DiamondChunk`, call `run_chunk`
directly rather than through `diamond_vm_run`) wrapped in a `for(;;)`: each
iteration allocates a fresh `DiamondVm`, re-copies the child's stored
arguments into it from `args_vm` (`copy_value_into_vm` using
`program_template`'s own classes on both sides -- `args_vm` and every
attempt's run `DiamondVm` are structurally identical clones of the same
template, so this is always a same-layout rebase, never a cross-program
adopt), runs it, and frees it -- whether that attempt crashed or not, so
nothing survives from one attempt to the next except `args_vm`'s own
untouched originals. A clean `DIAMOND_VM_OK` return ends the loop for good
(v1 restarts on crash only); anything else records the reason as plain
text (`format_uncaught_exception_message` for an uncaught exception,
`run_vm->error`/`diamond_vm_status_name` otherwise -- deliberately a
`char[]`, not a rehydrated exception value, since nothing needs to survive
a restart boundary as a live `DiamondValue` and a plain string sidesteps
having to give crash records their own GC-rooted storage) into
`last_error`, bumps `restart_count`, and loops again after a fixed 20ms
`nanosleep` unless `stop_requested` -- a safety valve bounding CPU use from
a child that crashes immediately every time, not a configurable backoff
policy.

**A `(DiamondSupervisor){}`/`(DiamondSupervisorChild){}` compound literal
must never appear inside `run_chunk`.** `DIAMOND_OP_SUPERVISOR_NEW`
allocates via `calloc`, never a compound literal, and every child-slot
cleanup path uses `memset` instead of `*new_child=(DiamondSupervisorChild)
{}` -- this was a real, shipped-and-caught regression during development,
not a hypothetical: `children[DIAMOND_MAX_SUPERVISOR_CHILDREN]` embeds
`DIAMOND_MAX_ARGUMENTS`-sized argument arrays per child, making
`DiamondSupervisor` itself well over 100KB. An unoptimized (`-O0`) build
gives a compound literal automatic storage duration inside whichever
function lexically contains it, unconditionally, regardless of which
`case` branch actually runs at that call -- so a literal of that size
anywhere inside `run_chunk`'s own `switch` inflated *every* recursive
`run_chunk` call's stack frame by 100KB+, blowing the C stack at a
call depth far below `DIAMOND_MAX_CALL_DEPTH`'s own guard (95) and
segfaulting instead of cleanly raising `SystemStackError`. `calloc`/
`memset` write directly into already-allocated heap memory and need no
such temporary -- the same reason `DiamondProgram` (a ~14MB struct) is
always `calloc`'d and never zero-initialized via a compound literal
anywhere in this codebase.

**Stopping and joining.** `supervisor_join_all_children` is shared by
`Supervisor#stop`, `Supervisor#join`, and `free_supervisor_reference` --
all three "join every child" call sites, any combination of which may run
against the same `Supervisor` over its lifetime. Each child's own `joined`
flag (checked and set under `lock` immediately before the real, unlocked
`pthread_join` call) makes that `pthread_join` idempotent regardless of
how many times or in which combination the three callers reach a given
child -- `pthread_join` on an already-joined thread is undefined behavior
per POSIX, the identical hazard `DiamondThread.joined`/`join_lock` already
guards against for a plain `Thread`. `stop()` additionally sets
`stop_requested` (checked, lock-free, once per retry-loop iteration --
mirroring `DiamondThread.finished`'s own lock-free hot-path read for
`alive?()`) and `stopped` (guarded by `lock`, blocking further
`add_child`) before joining; `join()` touches neither, so it only unblocks
once every child has finished on its own. Neither ever cancels a child
mid-attempt -- there is no cancellation anywhere in Diamond's concurrency
model, the same limitation `Thread` already has.

## Focused verification

```sh
make test-fibers
make test-fiber-guards
make test-fiber-run
make test-vm-context
make test-scheduler
make test-scheduler-run-all
make test-fiber-gc-roots
make test-nested-yield-guard
make test-tsan
```
