# Fibers and cooperative scheduling

Fibers suspend and resume Diamond execution without preempting native C code.
Each `DiamondFiber` owns its own native OS stack (allocated via `mmap`, with a
guard page) and a POSIX `ucontext_t`. `yield` and `resume` are implemented as
`swapcontext` calls between a fiber's own stack and whichever stack resumed
it — the interpreter (`run_chunk`) itself requires no special support for
suspension: nested Diamond calls, active rescue/ensure handlers, and generic
type-variable bindings are all ordinary C-stack state, and the OS preserves
all of it automatically across a suspend, exactly as it would across any
blocking function call.

The scheduler is a single-threaded FIFO run queue. `yield` parks the current
fiber's native stack and returns control to whoever resumed it; `resume`
places a suspended fiber back at the back of the queue. Garbage collection
treats queued and suspended fibers' frame chains as roots (see below).

Round-robin scheduling is deterministic, and every lifecycle transition
rejects a fiber outside its required source state with a clear error rather
than corrupting state (see the state machine section).

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
Scheduler helpers support one-step dequeue and requeue of yielded fibers,
preserving FIFO ordering.

Every lifecycle transition rejects a fiber outside its required source state
and returns `DIAMOND_FIBER_INVALID_STATE` rather than corrupting state:
`diamond_fiber_begin` requires `RUNNABLE`; `diamond_fiber_resume` requires
`RUNNABLE` or `SUSPENDED`; `diamond_fiber_run` requires `RUNNING` with a bound
VM and chunk; `diamond_fiber_suspend`, `diamond_fiber_complete`, and
`diamond_fiber_fail` all require `RUNNING`. `diamond_fiber_resumable` reports
whether `resume` would currently succeed (true only for `RUNNABLE` or
`SUSPENDED`) without attempting the transition. Every rejected transition has
regression coverage.

## Execution model

`diamond_fiber_prepare` allocates the fiber's native stack (`mmap` with a
`PROT_NONE` guard page below it, so a genuine stack overflow inside the
fiber faults deterministically instead of corrupting adjacent memory) and
establishes its `ucontext_t` via `getcontext`/`makecontext`, targeting an
internal trampoline that will run the fiber's chunk to completion or first
suspension.

```c
diamond_fiber_bind_vm(fiber, &vm);
diamond_fiber_prepare(fiber);
diamond_fiber_resume(fiber);
diamond_fiber_run(fiber);
```

`diamond_fiber_run` performs the context switch: it saves the VM's current
native-call-chain root and currently-running-fiber marker, installs the
fiber's own, and calls `swapcontext` into the fiber's `ucontext_t`. Control
returns to `diamond_fiber_run` only when the fiber yields or finishes
(completes or fails); the VM state is then restored and the fiber's status
maps to `COMPLETED`, `SUSPENDED`, or `FAILED` exactly as before.

`DIAMOND_OP_YIELD` swaps back to whoever resumed the fiber directly, in
place, at any call depth — including inside a called function, inside an
active `begin`/`rescue`/`ensure` block, or inside generic dispatch. It reads
two register operands, `dest` and `source`: `source` is the value yielded
*out* to whoever resumes, and on the next resume, `dest` is filled with
whatever value that resume delivered. This works because `registers[]` is
an ordinary C-stack local inside the fiber's own `run_chunk` activation,
which lives on the fiber's own never-unwound native stack — resuming a
`swapcontext` call is exactly like returning from any blocking function
call, with locals (and thus pending register writes) intact. This closes
two bugs from the interpreter's earlier single-frame-checkpoint design,
both confirmed by direct repro before the fix:

- **Nested yield used to silently corrupt on resume.** `def inner()\n
  yield\n 5\nend\ninner() + 100` would suspend "successfully," but resuming
  discarded the inner call's progress entirely, producing a misleading
  `DIAMOND_VM_TYPE_ERROR` from a garbage register instead of ever completing.
  It now resumes and completes correctly, to `105`.
- **Yield inside an active rescue used to lose the handler.** The rescue
  handler stack was C-local to each interpreter activation, not part of any
  checkpoint; resuming re-entered the interpreter with a fresh, empty handler
  stack, so an exception raised after the yield point was not caught by the
  still-conceptually-active rescue clause. It is now preserved correctly,
  since the entire native call stack — handlers included — stays parked
  rather than being unwound and re-entered.

If `yield` executes with no fiber currently running (plain `diamond_vm_run`,
not `diamond_fiber_run`), there is no scheduler to suspend into:
`DIAMOND_VM_YIELD_WITHOUT_FIBER` is returned instead, failing immediately and
clearly rather than doing nothing meaningful.

`diamond_fiber_scheduler_run_once` dequeues one runnable fiber, executes it,
requeues suspended fibers at the FIFO tail, and removes completed or failed
fibers from the queue.

`diamond_fiber_scheduler_run_all` loops `diamond_fiber_scheduler_run_once`
until the queue empties, draining every queued fiber to a terminal
`COMPLETED` or `FAILED` state. An individual fiber failure does not stop the
run; remaining queued fibers still execute to completion.

`diamond_vm_bind_fiber_queue` attaches a `DiamondFiberQueue` to a `DiamondVm`
as an additional garbage-collection root source. Collection walks every
fiber currently enumerable in the bound queue and marks that fiber's own
native call-frame chain (the same `DiamondFrame` linked-list walk used for
the VM's currently-running chain, applied to each suspended fiber's parked
stack instead). This closes the gap where a fiber sharing a VM with other
scheduled fibers could suspend holding heap references reachable only
through its own parked stack, while another fiber's turn triggers
collection. Binding is optional and additive; a VM with no bound queue
collects exactly as before.

Diamond source emits this boundary with `yield` or `yield(value)` — now a
primary expression, usable anywhere a value is expected (`x = yield(1) +
1`), not only as a standalone statement. Bare `yield` yields `nil` out
(unchanged from before); `yield(value)` yields `value` out. The expression's
own result — what a later resume delivers back in — is not yet reachable
from Diamond source, since there is no `Fiber.new`/`.resume(value)` syntax
yet; the C-level `diamond_fiber_resume(fiber, value)` API already threads a
value all the way through, exercised by `tests/fiber_run.c`, ahead of the
language-level construct landing.

The current C boundary is:

```c
DiamondFiber *diamond_fiber_new(const DiamondChunk *);
DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *, DiamondVm *);
DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *);
DiamondFiberStatus diamond_fiber_run(DiamondFiber *);
DiamondFiberStatus diamond_fiber_resume(DiamondFiber *);
DiamondFiberStatus diamond_fiber_yield(DiamondFiber *);
bool diamond_fiber_resumable(const DiamondFiber *);
DiamondValue diamond_fiber_result(const DiamondFiber *);
DiamondVmStatus diamond_fiber_status(const DiamondFiber *);
void diamond_fiber_free(DiamondFiber *);
void diamond_vm_bind_fiber_queue(DiamondVm *, const DiamondFiberQueue *);
```

There is no more instruction/register checkpoint API (`diamond_vm_run_context`,
`diamond_fiber_checkpoint`, `diamond_fiber_get_register`/`set_register`, and
similar) — a fiber's execution state is its own native stack, not a value the
caller can inspect or reconstruct. Values must be observed through
`diamond_fiber_result` after resuming to completion.

The focused regression harness is available with:

```sh
make test-fibers
make test-fiber-guards
make test-fiber-run
make test-vm-context
make test-scheduler
make test-scheduler-run-all
make test-fiber-gc-roots
make test-nested-yield-guard
```
