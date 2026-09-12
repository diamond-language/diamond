# Bytecode caching

`diamond script.di` caches the compiled program in a sibling `.dic` file next to the
source, so a second, unchanged run skips compilation entirely. No flag, no separate
command -- it's on by default for the ordinary CLI.

```sh
diamond app.di   # first run: compiles, writes app.dic
diamond app.di   # second run: reads app.dic, skips straight to running
```

On a real, substantial multi-file application (skindicate.dia's own `app.di`),
this took startup from ~1.35s to ~0.11s on a second run -- roughly 13x, with byte-
identical behavior.

## What gets cached, and how a change invalidates it

The cache key is a SHA-256 hash of the *fully `require`-expanded* source -- the entry
file's own text with every transitively `require`d file's content already spliced in,
the same flat string compilation itself operates on. `require` resolution reads real
files from disk on every run regardless of caching -- there's no separate cache for
*that* step, only for compiling the result. This means:

- editing the entry file invalidates the cache, obviously;
- editing a `require`d library file, with the entry file itself completely unchanged,
  *also* invalidates the cache -- the hash covers the whole expanded program, not just
  the one file named on the command line.

Caching only ever skips the **compile** step (lexing, parsing, bytecode generation).
`require` resolution and file reads still happen on every run, cache hit or not -- there
is no way around that without caching the load step separately, which this doesn't
attempt.

A `.dic` file that's missing, truncated, hand-edited, or was written by an
incompatible build of `diamond` (a build fingerprint covering the exact struct layouts
and opcode count the file format depends on is checked on every read) is treated
exactly like a cache miss: `diamond` falls back to a clean recompile and, on success,
overwrites the stale file. This never crashes and never affects a script's own
observable behavior -- caching is a pure optimization.

## Disabling it

`DIAMOND_NO_CACHE=1` (any value, matching `DIAMOND_STRESS_GC`'s own convention) turns
caching off entirely for that invocation -- no `.dic` file is read or written.
`DIAMOND_TRACE_CACHE=1` prints a `cache: hit`/`cache: miss`/`cache: wrote` line to
stderr, useful for confirming the feature is actually doing something.

## What's explicitly not covered

- **`-e`/the REPL.** Neither has a stable on-disk path a cache file could sit next to
  -- `-e`'s own display name is always the literal string `"-e"` regardless of what
  code was actually passed. A different, adjacent feature, not attempted here.
- **A step-debugger session (`DIAMOND_DEBUG_BREAKPOINTS` set).** Already forces a
  live, uncached compile unconditionally, so the DAP server's own assumptions about a
  freshly-compiled combined source buffer stay true.
- **`diamond build`.** Its own AOT-embed mechanism already means the *produced* binary
  never recompiles at all at run time; this cache is unrelated to that one-shot build
  command.
- **`tests/run_cases.c`'s own batch corpus runner** (and, by extension, every `make
  test*` target) never touches this cache at all, by construction -- it calls
  `diamond_run_source_with_template`, a distinct entry point from the CLI's own
  auto-dispatch that caching only ever hooks into. This is deliberate: a stale-but-
  fingerprint-compatible cache surviving between two test runs could otherwise mask a
  real compiler regression the test suite exists to catch.

See [docs/roadmap.md](roadmap.md)'s "Bytecode caching: what's next, if anything" for
what's deferred (a `diamond cache clear`-style command, moving the cache out from
next to the source for read-only deployments, ...).
