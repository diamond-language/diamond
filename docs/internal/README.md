# Internal docs

Implementation rationale, design history, and maintainer-only tooling for
Diamond's own runtime and toolchain -- none of this is needed to write or
run Diamond programs. If you're looking for language/API documentation,
start at [`docs/syntax.md`](../syntax.md) or [`docs/io.md`](../io.md) instead.

- [Design and VM architecture](design.md) -- implementation rationale, VM
  internals, and open design questions. Referenced throughout the public
  docs as background for *why*; not required reading to use the language.
- [Concurrency internals](concurrency-internals.md) -- Fiber/Thread
  mechanics at the VM level (ucontext, stack allocation). See
  [`docs/fibers.md`](../fibers.md)/[`docs/threads.md`](../threads.md) for
  the language-level API instead.
- [Generational GC design](gc-generational-design.md) -- write barriers,
  card marking, object header layout.
- [Fuzzing](fuzzing.md) -- building and running the fuzzing harness.
- [Diamond-modernization audit](di-modernization-audit.md) -- a one-time
  historical audit of a past refactor; kept for context, not maintained
  as ongoing reference.
