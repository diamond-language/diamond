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
