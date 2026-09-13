# Sandbox mode

Sandbox mode denies every native call that opens a real filesystem, network, or
subprocess resource -- for running untrusted or merely-not-yet-trusted Diamond code
without letting it touch anything outside the process itself. Ordinary computation,
`puts`/`print`, and everything else keep working normally; a sandboxed program still
runs to completion and can report a result.

```sh
diamond --sandbox untrusted.di
# or, equivalently:
DIAMOND_SANDBOX=1 diamond untrusted.di
```

Any value enables it -- `DIAMOND_SANDBOX=1`, matching `DIAMOND_STRESS_GC`'s own
convention. `diamond --sandbox` is a thin convenience that sets the same environment
variable before running normally; both forms, and `DIAMOND_SANDBOX=1 ./program` against
a `diamond build`-produced [standalone binary](deployment.md), behave identically.

## What's denied

Every one of these raises `SandboxError` (a rescuable `StandardError`) instead of doing
anything:

- `File.open`, `File.delete`, `File.directory?`, `File.expand_path`
- `Dir.entries`
- `TCPSocket.connect`, `TCPServer.listen`/`listen_nonblocking`
- `UDPSocket.bind`/`.open`
- `TLSSocket.connect`, `TLSServer.listen`
- `SQLite3.open`, `PostgreSQL.open`, `MySQL.open`
- `Process.run`, `Process.spawn`

`File.dirname`/`.basename`/`.extname`/`.absolute?`/`.join` are **not** denied -- they're
pure string manipulation with no real syscall, unlike `.directory?` (`stat`) and
`.expand_path` (path resolution against the real filesystem).

```ruby
begin
  File.open("secrets.txt")
rescue error: SandboxError
  puts(error.message())  # => "sandbox denies File.open"
end
```

## Resource limits

Independent of `--sandbox`/`DIAMOND_SANDBOX` -- these bound *how much* a program can
do, not *what kind* of thing it can do, so they're useful on their own (e.g. safely
executing an untrusted or fuzzer-mutated program that never touches the filesystem or
network at all) and combine naturally with `--sandbox` for defense in depth:

```sh
DIAMOND_MAX_INSTRUCTIONS=1000000 diamond untrusted.di
DIAMOND_MAX_WALL_MILLISECONDS=5000 diamond untrusted.di
DIAMOND_MAX_MEMORY_BYTES=100000000 diamond untrusted.di
```

- `DIAMOND_MAX_INSTRUCTIONS` -- opcode dispatch count.
- `DIAMOND_MAX_WALL_MILLISECONDS` -- wall-clock budget.
- `DIAMOND_MAX_MEMORY_BYTES` -- live (post-collection) memory budget.

All three raise a rescuable `ResourceLimitError` once exceeded -- the same way as any
other catchable error, including for the memory budget specifically: a genuine host
allocator failure is deliberately *not* catchable in Diamond (nothing can safely
recover from real memory exhaustion), but hitting your own *configured* cap, with
plenty of real memory still available, is a different, safe-to-catch situation.

```ruby
begin
  index = 0
  while true
    index = index + 1
  end
rescue error: ResourceLimitError
  puts(error.message())  # => "resource limit exceeded"
end
```

Each of the three is independently opt-in (unset = unlimited); all can be combined.
Once any one of them fires, it *stays* fired for the rest of that VM's run -- the
budget concept is "you get to find out once, and get one chance to react," not a
signal that keeps re-arming and re-interrupting the very `rescue`/cleanup code meant
to handle it.

**What's not covered**:

- **Not a shared, cross-thread budget.** Each `DiamondVm` -- the top-level program,
  and independently, each `Thread.new`/`Supervisor` child's own `child_vm` -- reads
  the same configured env var at its own init and enforces it against its own
  counters. A program that spawns many threads gets one independent budget *per
  thread*, not one shared total, so it could still multiply its aggregate resource use
  past a single configured number. A real fix needs a shared, atomic, cross-thread
  counter (real precedent exists for exactly this shape --
  `DIAMOND_MAX_THREADS`'s own process-wide atomic counter) but is real, separate,
  higher-effort work, not attempted here.
- **The instruction-count budget is exact; the wall-clock one is not.** Wall-clock is
  checked periodically (every few thousand instructions, not every single one, since
  reading the clock is comparatively expensive) -- the actual overshoot past a
  configured millisecond budget is bounded but nonzero.
- **A program that only grows already-allocated collections can bypass the memory
  budget.** The check lives where every *fresh* allocation already checks in before
  proceeding (same central place the generational collector's own thresholds are
  checked); a loop that only ever calls `Array#push`/`Hash#[]=` on one already-live
  collection, never allocating anything new, grows `Array`/`Hash` backing storage via
  a plain reallocation this check doesn't intercept. Most real memory-exhaustion
  shapes (allocating many discrete `String`/`Hash`/`Instance` objects) are covered;
  this narrower one isn't.

## What's not covered

Stated honestly, matching this project's own convention for every other feature's
known limitations:

- **No per-capability granularity.** It's all-or-nothing -- no "allow network but not
  filesystem," no allow-listing specific paths or hosts. Not attempted without a
  concrete use case asking for it.
- **`Thread.new`/`Supervisor.add_child` are not restricted.** A sandboxed program can
  still spawn OS threads (up to the existing 64-thread process-wide cap) -- this is a
  resource-exhaustion concern, a different category from "touching the outside world,"
  and each spawned thread's own code is still fully subject to every restriction above
  (see [Concurrency internals](internal/concurrency-internals.md) -- sandbox mode checks
  the real process environment directly at each gated opcode, so it applies identically
  to the top-level program, every `Thread`/`Supervisor` child, and anything run via
  `ProgramBuilder#run`, with nothing to propagate from parent to child).
- **`Signal.trap` is not restricted.** A process-wide side effect, adjacent to but
  distinct from resource-opening.
- **The compile-time `require` graph is not restricted.** Sandbox mode restricts
  *runtime* behavior; the entry script itself (and whatever it `require`s) is assumed
  to already be source the caller chose to run.

Revisit any of these only with a real driving need, not speculatively -- the same bar
[the roadmap](roadmap.md) already holds every other research direction to.
