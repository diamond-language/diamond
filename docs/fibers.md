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
