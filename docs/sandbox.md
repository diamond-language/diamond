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

## What's not covered

Stated honestly, matching this project's own convention for every other feature's
known limitations:

- **No per-capability granularity.** It's all-or-nothing -- no "allow network but not
  filesystem," no allow-listing specific paths or hosts. Not attempted without a
  concrete use case asking for it.
- **No resource limits.** No CPU, wall-clock, or memory budget. A sandboxed program
  denied every I/O bridge can still loop forever or allocate without bound.
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
