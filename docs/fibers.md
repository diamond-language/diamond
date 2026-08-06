# Fibers and cooperative scheduling

Fibers will suspend and resume Diamond execution without preempting native C
code. A fiber owns a resumable VM frame chain, register storage, instruction
locations, rescue/ensure state, and a completion value or exception.

The initial scheduler will be a single-threaded FIFO run queue. `yield` saves
the current fiber state and returns control to the scheduler; `resume` places a
suspended fiber at the back of the queue. Native calls must remain non-yielding
until an explicit continuation boundary exists.

The first implementation target is deterministic round-robin behavior with
clear errors for resuming a running or completed fiber. Garbage collection
must treat queued and suspended frame chains as roots.

## State machine

`NEW -> RUNNABLE -> RUNNING -> SUSPENDED -> RUNNABLE` is the normal cycle.
`RUNNING -> COMPLETED` records a value, while an uncaught exception records
`FAILED`. `SUSPENDED`, `COMPLETED`, and `FAILED` fibers retain their terminal
or resumable result until explicitly released.

The scheduler owns runnable fibers; the VM owns the currently running fiber.
Only the scheduler may transition `RUNNABLE` to `RUNNING`, and only an
explicit yield or completion may return control to the scheduler. A suspended
fiber's frame chain, captured cells, pending handlers, and result are GC roots.

The proposed C boundary is:

```c
DiamondFiber *diamond_fiber_new(DiamondVm *, const DiamondChunk *);
DiamondFiberStatus diamond_fiber_resume(DiamondFiber *);
DiamondFiberStatus diamond_fiber_yield(DiamondFiber *);
void diamond_fiber_free(DiamondFiber *);
```

The API remains provisional until frame ownership and exception propagation
are implemented.
