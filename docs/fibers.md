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
(unchanged from before); `yield(value)` yields `value` out. `Fiber.new(callable)`
now constructs a fiber from Diamond source (see below), but there is not yet
any way to resume one from Diamond source — no `.resume(value)` dispatch
exists yet — so the expression's own result, what a later resume delivers
back in, is only reachable via the C-level `diamond_fiber_resume(fiber,
value)` API today, exercised by `tests/fiber_run.c`, ahead of the
`.resume(value)` dispatch landing.

The current C boundary is:

```c
DiamondFiber *diamond_fiber_new(const DiamondChunk *);
DiamondFiberStatus diamond_fiber_bind_vm(DiamondFiber *, DiamondVm *);
DiamondFiberStatus diamond_fiber_prepare(DiamondFiber *);
DiamondFiberStatus diamond_fiber_run(DiamondFiber *);
DiamondFiberStatus diamond_fiber_resume(DiamondFiber *, DiamondValue);
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

## Fiber object model and GC design

A fiber can now be wrapped in a `DiamondFiberHandle` — a thin
`DiamondObject`-headed struct holding a pointer to the underlying
`DiamondFiber` — giving it a new heap object kind, `DIAMOND_OBJECT_FIBER`.
The wrapper is deliberately thin rather than folding `DiamondObject` directly
into `DiamondFiber`: the existing C-level scheduler tests construct fibers
with no VM at all (`diamond_fiber_new(nullptr)`, multi-VM scheduler tests),
which folding would break.

`Fiber.new(callable)` is now Diamond-language syntax, recognized in the
compiler wherever an identifier named `Fiber` is followed by `.new(...)` and
is not shadowed by a local variable of that name (same precedent as
`redefine_method`: `Fiber = 5; Fiber.new(1)` compiles to ordinary dynamic
`INVOKE` dispatch on the local, not the special form). It compiles to a new
`DIAMOND_OP_FIBER_NEW dest, callable` instruction. The callable must be a
zero-argument `Callable` value; both constraints are enforced at the
`Fiber.new` call site with a rescuable `TypeError` or `ArgumentError`, since
classes are not first-class runtime values and neither the argument's kind
nor its target function's arity can be known until then. Captures are
allowed, unlike `redefine_method`'s restriction: the fiber trampoline invokes
its entry closure exactly the way `DIAMOND_OP_CALL_CLOSURE` already does, so
`GET_CAPTURE`/`SET_CAPTURE` need no special handling.

`DIAMOND_OP_FIBER_NEW` builds a fiber via `diamond_fiber_new_for_closure`,
binds it to the running VM, prepares its native stack, and wraps it in a
`DiamondFiberHandle` — closing the loop on the object kind, GC marking, and
sweep-time cleanup that landed in the prior phase ahead of this surface.
Reaching this opcode also surfaced an unrelated, previously-latent bug:
`diamond_fiber_prepare`/`diamond_fiber_run` both unconditionally rejected any
fiber with a null `chunk`, but closure-invoking fibers deliberately leave
`chunk` null (they run via `entry_closure` + `program_tables` instead) —
since nothing had exercised that path end-to-end before, this went unnoticed
until it would have rejected every fiber `Fiber.new` ever constructed. Both
guards now accept either `chunk` or `entry_closure` being set.

There is not yet any way to resume, inspect, or free a `Fiber.new`-constructed
fiber from Diamond source — that is the next phase (`.resume`/`.status`/
`.alive?` native dispatch). A `Fiber` value with nothing else referencing it
is ordinary GC-reachable garbage in the meantime.

`mark_object`'s `DIAMOND_OBJECT_FIBER` branch marks a fiber's own parked
frame chain (`native_frames`), its `result` and `resume_value`, and —
load-bearing once `Fiber.new(callable)` lands — its `entry_closure`: once
that call returns, the fiber wrapper is the only remaining path to the
closure and its captured cells. Sweeping an unreached fiber handle calls
`diamond_fiber_free` on the underlying fiber, releasing its `mmap`'d native
stack, rather than leaking it.

A second, independent GC-root gap was fixed in the same phase: while a
child fiber runs, `diamond_fiber_run` temporarily overwrites
`vm->frames`/`vm->running_fiber` with the child's own, for the duration of
the `swapcontext` call. Before doing so, it now records the *previous*
values onto the child fiber itself, as `resumer_frames`/`resumer_fiber`,
clearing both once the child returns control. `diamond_vm_collect` walks
`vm->running_fiber->resumer_fiber` transitively, marking each ancestor's
`resumer_frames` as additional roots. Without this, a value reachable only
through the resumer's own live registers — most importantly, the very Fiber
value the resumer is holding, if it lives nowhere else — could be collected
out from under it the instant the child fiber's own allocations trigger a
collection. This is a real hazard only once something can call
`diamond_fiber_run` on a nested fiber from inside another fiber's own
execution — today only exercisable by a C-level test that manually mirrors
what the eventual `.resume(value)` dispatch will do — but the fix and its
regression coverage (`tests/fiber_run.c`, verified against a deliberately
reverted fix under ASan first) are in place ahead of that dispatch landing.

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
