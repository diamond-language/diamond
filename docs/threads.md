# Threads: real OS-level parallelism

`Thread` gives Diamond genuine parallel execution — actual pthreads, actually
running concurrently on separate cores — as opposed to `Fiber`'s single-native-
thread cooperative scheduling (see `docs/fibers.md`). The two are unrelated
mechanisms solving different problems: Fiber suspends and resumes without
preemption on one OS thread; Thread runs code preemptively on another one
entirely.

## Isolated-heap design

Every other piece of concurrency-adjacent machinery already in this VM — the
GC, inline caches, `REDEFINE_METHOD`, the class/function tables a running
program dispatches against — was designed assuming a single native thread
ever touches a given `DiamondVm`. Making all of that thread-safe would have
meant auditing and synchronizing the entire interpreter. Instead, each
spawned `Thread` gets its own **fully independent heap**: its own
`DiamondVm` (registers, GC, allocation, exception state) and its own
`DiamondProgram` (functions/classes/interfaces tables) with no shared
mutable state with the spawning thread or any other thread. Two threads
never touch the same GC, the same inline cache, or the same class table, so
none of that machinery needs to become thread-safe at all.

The cost is memory: `Thread.new` clones the *entire* ambient `DiamondProgram`
via a byte-for-byte `memcpy` of its `functions[]`/`classes[]`/`interfaces[]`
tables (~83MB per clone; see the self-hosting history in `CHANGELOG.md`). This
is safe only because `DiamondFunction`/`DiamondClass`/
`DiamondInterface` are themselves pointer-free — every field is a fixed-size
inline array or scalar, constants are restricted to Int/Float/Bool/Nil at
compile time, and string constants live in an inline array rather than as
heap objects — so a raw copy needs no pointer fixup. `clone_program_from_chunk`
(`src/vm.c`) builds the clone via `diamond_program_init` (correct defaults
for the fields a `DiamondChunk` view never exposes — `modules[]`,
`namespace_constants[]`, `entry`, `entry_path` — all purely compile-time
bookkeeping `run_chunk` never reads) plus a targeted `memcpy` of just the
three live tables.

A process-wide `DIAMOND_MAX_THREADS` cap (64) bounds worst-case memory to
roughly 5GB and turns a runaway recursive `Thread.new` bug into a prompt
`ThreadError` instead of exhausting the host. The cap is tracked via a
single `atomic_size_t diamond_active_thread_count`, incremented once a
spawn's `DiamondThread` struct becomes valid enough for `free_thread` to
clean up, decremented exactly once — inside `free_thread` itself — whether
the thread was reaped by an explicit `.join()` or by the GC.

## Usage

```
def worker(n)
  n * n
end

t1 = Thread.new(worker, 6)
t2 = Thread.new(worker, 7)
puts(t1.join())   # => 36
puts(t2.join())   # => 49
```

`Thread.new(callable, *args)` takes any zero-capture `Callable` (an ordinary
top-level `def`, or a closure literal that captures nothing) plus however
many arguments its target function requires. Two constraints are enforced
at the call site, both rescuable as `TypeError`:

- **The callable must not capture any local state.** A captured
  `DiamondCell` is a live GC object belonging to the *spawning* heap;
  sharing it across an independent-heap thread would defeat the whole
  isolation design. Plain `def`s (first-class as of an earlier session) and
  literal closures that reference nothing outer both qualify.
- **Argument count must match the target function's arity**, the same
  `required_arity`/`arity` check `Thread.new`'s bytecode case performs
  before ever cloning anything.

`.join()` blocks until the thread finishes and returns its result (or
re-raises whatever exception the thread raised, uncaught). It is
idempotent — pthread's own `pthread_join` may only be called once per real
thread, so `.join()` is guarded by a `pthread_mutex_t`: the first call does
the real `pthread_join` plus the copy-back-into-this-VM's-heap work below,
and caches both the joined flag and the (now GC-rooted) result; every
subsequent call just re-reads the cache.

`.alive?()` polls an `atomic_bool finished` the thread sets right before it
returns — a wait-free check, no lock, no blocking.

There is no `.detach()` — every spawned thread is either explicitly
`.join()`ed or block-joined by the GC (see below). Fire-and-forget threads
are not supported.

## Crossing the heap boundary: `copy_value_into_vm`

Arguments passed to `Thread.new` and the value `.join()` returns both have
to move between two entirely separate heaps. Both directions reuse
`copy_value_into_vm` (`src/vm.c`), the same recursive deep-copy helper
built for `ProgramBuilder#run`'s own cross-heap `Instance` handling. It
handles String/Symbol/Array/Hash/Bignum identically for both callers; only
an `Instance` value's `->class` pointer fixup differs, selected by which of
two mutually exclusive modes the caller passes:

- **Adopt mode** (`ProgramBuilder#run`): the value's *entire source program*
  is genuinely foreign, so it's kept alive in the destination VM's
  `adopted_programs` list and the copied instance's `owner` points at it.
- **Rebase mode** (`Thread`): the destination program is already a full
  clone of the source, with byte-identical table layout — so an instance's
  class pointer is just rebased by array-offset arithmetic:
  `&rebase_dest_classes[source->class - rebase_source_classes]`. No
  adoption, no keep-alive list, `owner` stays `nullptr` — the destination's
  own ambient chunk is already built from the same cloned tables, so the
  ordinary "resolve against whatever chunk is ambient" dispatch fallback
  already resolves correctly.

`Thread.new` copies each argument in rebase mode from the parent's classes
into the freshly cloned `child_program`'s classes; `.join()` copies the
result back in the opposite direction, from `child_program`'s classes into
the joining VM's own ambient `chunk->classes`. Copying an instance's fields
directly (as this helper does, rather than going through the real
`SET_IVAR` opcode) also has to manually advance the copy's `shape` pointer
to match the source's — `allocate_instance` always starts a fresh instance
at `shapes[0]` ("nothing materialized"), and `GET_IVAR`'s inline cache gates
a field read on `field < shape->field_count`, so skipping this step makes
every copied field silently read back as a cached `nil` despite genuinely
holding a value.

The same restricted-kind rejection both callers already relied on still
applies: Fiber, File, Listener, Socket, UDP socket, TLS socket, Regexp,
ProgramBuilder, and Thread values can't cross a heap boundary at all
(rescuable `TypeError`) — each holds either live GC state tied to one
specific heap, or a native OS resource that doesn't make sense to
duplicate.

**One exception: a zero-capture Closure may cross, in rebase mode only.**
A *capturing* closure is still categorically forbidden — it holds a live
`DiamondCell`/GC state belonging to the source heap, exactly the same
hazard `Thread.new`'s own primary-callable check already rejects. But a
zero-capture closure is just a `function_index` into the shared
`functions[]` table, and in rebase mode the destination program is a
byte-for-byte clone of that same table (see "Isolated-heap design" above)
— so the index means the same function in both, with no arithmetic needed
at all (unlike the `Instance`/class-pointer case, which rebases a pointer).
This lets a `Callable` argument — such as a request handler — reach a
spawned thread like any other value. Adopt mode (`ProgramBuilder#run`)
still rejects every Closure unconditionally: its source and destination
programs are genuinely different, so a bare `function_index` wouldn't mean
the same function in both. `packages/gremlin`'s multi-threaded mode
(`gremlin_serve(port, handler, threads: N)`) is the motivating use case —
see its own source comments.

## `.join()`'s three outcomes

`thread_entry_trampoline` (the real `pthread_create` entry point) populates
exactly one of three mutually exclusive outcomes before setting `finished`:

1. **Plain return** — `result` holds the returned value.
2. **Uncaught `raise`** — `raised` is set; `result` holds the actual
   exception instance (still living in the child VM's own heap until
   `.join()` copies it back, exactly like an ordinary return value).
3. **Internal VM failure** — a stack overflow, invalid bytecode, or similar
   failure that was never a clean Diamond-level `raise`. `internal_failure`
   is set; `.join()` reads `child_vm->error` (still alive at that point,
   since the child VM isn't freed until `free_thread`) to build a fresh
   `ThreadError` and raise it.

`.join()` re-raises case (2) and (3) via `catch_exception` directly against
the caller's own enclosing rescue handlers — the same pattern
`DIAMOND_OP_RAISE` itself uses — rather than `VM_RETURN`'s
`catch_runtime_error`, which only knows how to synthesize a fresh exception
from a native status code and has no path for re-raising an
already-constructed exception *value*.

## GC and lifecycle

`Thread` follows Fiber's split-handle pattern: a thin `DiamondThreadHandle`
(`DIAMOND_OBJECT_THREAD`) wraps a pointer to the native `DiamondThread`
struct, giving it an ordinary GC-reachable lifetime — a `Thread` value with
nothing referencing it becomes garbage exactly like any other heap value,
with no explicit free from Diamond source.

**The GC block-join guarantee**: `free_thread` — called from both
`diamond_vm_collect`'s sweep and `diamond_vm_free`'s whole-VM teardown —
blocks on `pthread_join` if a thread was spawned but never explicitly
`.join()`ed, before reclaiming its child VM, cloned program, and mutex. This
guarantees no real OS thread ever outlives its `DiamondThreadHandle`'s GC
lifetime, so an abandoned unjoined thread doesn't leak an OS thread or leave
one running past the point its owning value became unreachable.

`mark_object`'s `DIAMOND_OBJECT_THREAD` branch only marks `thread->result`
once `.join()` has actually copied it into *this* VM's own heap
(`thread->joined && !thread->internal_failure`) — before that, the result
lives entirely in the child VM's own separate heap, which this VM's GC has
no business scanning.

## `pthread_join`'s happens-before guarantee

The child thread's writes to `result`/`raised`/`internal_failure` are plain,
non-atomic fields — no additional locking is needed for them specifically,
because `.join()` (and the GC's block-join path) always calls
`pthread_join` before reading any of them, and POSIX guarantees a full
happens-before edge across that call: everything the child thread wrote
before returning is visible to whoever successfully joins it. Only two
things need their own synchronization: `finished` (a lock-free
`atomic_bool`, for `.alive?()`'s non-blocking poll) and `join_lock` (a
`pthread_mutex_t`, making `.join()` itself safely re-callable — real
`pthread_join` is not).

## Explicitly out of scope

- **`Signal.trap`/`sigaction`**: already effectively single-VM-assuming
  before `Thread` existed (a second `sigaction` call for the same signal
  name silently overwrites the first, regardless of threads). `Thread`
  doesn't make this categorically worse, just extends an existing
  single-owner assumption to more callers. Not synchronized.
- **Bare global `stdout`/`stdin`**: memory-safe (glibc locks each `FILE*`
  internally) but genuinely shared/interleaved across concurrently-printing
  threads — standard behavior for real threads in any language, not a
  defect. No per-VM output redirection.
- **The REPL's global `stdout` reassignment**: a real race only if the REPL
  runs concurrently with a still-alive spawned thread's own output —
  bounded by the GC block-join guarantee to "an unfinished thread's output
  landing in the wrong place at REPL redraw time if abandoned mid-candidate,"
  a cosmetic edge case, not memory-unsafe.
- **Detached/fire-and-forget threads**: not supported, as above.

## Prerequisite fix: `diamond_fiber_entering`

A spawned `Thread`'s own Diamond code may legally use `Fiber` within its own
isolated VM. `diamond_fiber_entering` — the file-scope pointer
`diamond_fiber_trampoline` reads and `diamond_fiber_run` writes — was a
plain `static` before `Thread` existed, which would race across OS threads
the moment two threads' Diamond code both used a Fiber concurrently. Fixed
by making it `thread_local` (a native C23 keyword, no new dependency): each
OS thread now gets its own independent copy.

## Testing

The shared-process corpus (`tests/cases/thread_*.di`, run by `make test`
alongside everything else) covers: joining a scalar/Array/Instance result
(exercising the class-pointer rebase path specifically), an uncaught
exception re-raised and rescued at `.join()`, rejecting a capturing closure
and an arity mismatch, idempotent double-`.join()`, several threads summed
for a correctness (not timing) check, nested `Thread.new` from within a
thread's own callable, `.alive?()` after `.join()`, and an unjoined thread
correctly reaped at process exit via the GC block-join guarantee.

`tests/run.sh` adds a real-parallelism proof: two threads each doing
genuine CPU-bound work (not `sleep`, which could pass even under a
cooperative scheduler that yields during the sleep) finish in wall-clock
time well under twice one thread's own solo time — timed from bash around
the whole `diamond` subprocess, predating `Time.monotonic()` (see
`docs/syntax.md`) and not worth rewriting to use it: bash's own timing
is already the simpler, more obviously-correct way to time a whole
subprocess from outside.

`make test-tsan` runs the Thread-specific cases (plus a couple of
representative Fiber cases, since the `thread_local` fix above touches
shared machinery) under ThreadSanitizer — a separate build variant
(`-fsanitize=thread`) from `make test-sanitize`'s ASan+UBSan, since TSan
cannot combine with either. Deliberately narrower than the full
`tests/cases` corpus: TSan's overhead makes re-running the entire (already
ASan/UBSan-covered, largely single-threaded) suite under it needlessly slow
for a target whose only job is proving the new concurrency-specific code
paths are race-free.

```sh
make test              # includes tests/cases/thread_*.di
make test-tsan          # ThreadSanitizer-specific pass
```
</content>
