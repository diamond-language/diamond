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

The initial queue primitive is FIFO, rejects non-runnable fibers, compacts
consumed storage, and transitions dequeued fibers to `RUNNING`.
Scheduler helpers now support one-step dequeue and requeue of yielded fibers,
preserving FIFO ordering.

Fiber frames currently carry the owning chunk, instruction checkpoint, and
call depth. Push/pop plus checkpoint updates are implemented independently of
the VM interpreter; queued-fiber enumeration provides the future GC root hook.

The current execution bridge binds a prepared fiber to a `DiamondVm` and runs
the complete chunk once it reaches `RUNNING`:

```c
diamond_fiber_bind_vm(fiber, &vm);
diamond_fiber_prepare(fiber);
diamond_fiber_resume(fiber);
diamond_fiber_run(fiber);
```

`diamond_fiber_run` records either the VM result and `COMPLETED`, or the VM
status and `FAILED`. This is deliberately a one-shot boundary: the VM still
executes a chunk to completion, so instruction checkpoints are metadata rather
than true mid-instruction continuations. Splitting the interpreter into
resumable steps is the next fiber implementation milestone.

The current C boundary is:

```c
DiamondFiber *diamond_fiber_new(const DiamondChunk *);
DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *, DiamondVm *);
DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *);
DiamondFiberStatus diamond_fiber_run(DiamondFiber *);
DiamondFiberStatus diamond_fiber_resume(DiamondFiber *);
DiamondFiberStatus diamond_fiber_yield(DiamondFiber *);
DiamondValue diamond_fiber_result(const DiamondFiber *);
DiamondVmStatus diamond_fiber_status(const DiamondFiber *);
void diamond_fiber_free(DiamondFiber *);
```

The API remains provisional until frame ownership, continuation points, and
exception propagation are integrated with the interpreter.
