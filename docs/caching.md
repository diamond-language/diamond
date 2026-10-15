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

A `.dic` file that's missing, truncated, damaged, hand-edited, or was written by a
different build of `diamond` is treated exactly like a cache miss: `diamond` falls back
to a clean recompile and, on success, overwrites the stale file. This never crashes and
never affects a script's own observable behavior -- caching is a pure optimization.
Every read checks, in order:

1. the magic bytes and a build fingerprint (the exact struct layouts, opcode count and
   byte order the format depends on, plus a checksum of the compiler's own sources and
   prelude);
2. the SHA-256 of the expanded source, so the file belongs to this program;
3. a checksum of the whole body, which catches truncation, bit rot and partial
   overwrites that leave the header intact;
4. bounds on every count read from the file, scalar-only constants, in-range method
   and superclass indices, and `diamond_verify_bytecode` over every function (register
   bounds, instruction alignment, jump targets) -- the same checks a hand-assembled
   program gets. This layer does not trust the checksum: it is not cryptographic, so a
   file deliberately re-signed after editing still has to pass it.

A `.dic` is still not a distribution format. A file that passes every check runs
whatever bytecode it contains, so anyone who can write one next to a script can change
what that script does without touching its source -- the same trust you already give the
directory's `.di` files. Set `DIAMOND_NO_CACHE=1` where that isn't true. Validation
guarantees memory safety, not that the bytecode matches the source. Cache format
version 4 added the body checksum; older files are ignored and rebuilt.

## Size

A `.dic` holds the program's own bytecode, not the prelude: the binary already embeds a
compiled copy, and a cache file leaves out the prelude's functions and rebuilds them from
that copy on load. A three-line script's cache is about 20 KB. Each function is stored as
its used bytes, not its full in-memory record (about 11 KB).

A program that mentions `JSON` is compiled against the same embedded prelude (it already
includes JSON), so its cache is just as small. Only a program that declares names of its own
that collide with the prelude's JSON classes falls back to compiling a live prelude, and its
cache then stores the whole prelude too. Large applications are large in their own code: a
program that requires ActiveRecord or GraphSQL has a cache of a few megabytes.

Format version 2 stored string constants as length-prefixed bytes rather than fixed 4 KB
records, version 6 stored the prelude once in the binary instead of in every file, and
version 7 added the byte order to the fingerprint (an image built for an opposite-endian
target is refused); older files are ignored and rebuilt.

## Removing caches

```sh
diamond cache clear app.di        # app.dic only
diamond cache clear src/ lib/     # every .dic under those directories
diamond cache clear               # every .dic under the current directory
diamond cache clear --dry-run     # list what would go, remove nothing
```

It removes only files that start with a Diamond cache's magic bytes, so an unrelated file
that happens to end in `.dic` is left alone, and it never follows symbolic links or enters
`.git`. A script whose cache was removed just recompiles and writes a new one on its next
run, so clearing is always safe.


## Warming caches

```sh
diamond cache warm app.di lib/worker.di
```

compiles each script and writes its cache without running it, which is what an image or
package build wants: run it once while the directory is still writable, and a read-only
deployment (a container image, a system-wide install) then reads the cache on every start.
It prints `cached PATH`, or `up to date PATH` when a valid cache is already there, and exits
65 if a script does not compile or 73 if the cache could not be written. A cache is only valid
for the `diamond` binary that wrote it, so warm with the binary the deployment runs.

## Disabling it

`DIAMOND_NO_CACHE=1` (any value, matching `DIAMOND_STRESS_GC`'s own convention) turns
caching off entirely for that invocation -- no `.dic` file is read or written.
`DIAMOND_TRACE_CACHE=1` prints a `cache: hit`/`cache: miss`/`cache: wrote` line to
stderr, useful for confirming the feature is actually doing something.

## What's explicitly not covered

- **`-e`/the REPL.** Neither has a stable on-disk path a cache file could sit next to
  -- `-e`'s own display name is always the literal string `"-e"` regardless of what
  code was actually passed. Not worth a content-addressed store either: a one-liner
  compiles in milliseconds against the embedded prelude, so there is little to save.
- **A step-debugger session (`DIAMOND_DEBUG_BREAKPOINTS` set).** Already forces a
  live, uncached compile unconditionally, so the DAP server's own assumptions about a
  freshly-compiled combined source buffer stay true.
- **`diamond build`.** Its own AOT-embed mechanism already means the *produced* binary
  never recompiles at all at run time; this cache is unrelated to that one-shot build
  command.
- **`tests/run_cases.c`'s own batch corpus runner** (the default for `make test`) never
  touches this cache at all, by construction -- it calls `diamond_run_source_with_template`,
  a distinct entry point from the CLI's own auto-dispatch that caching only ever hooks
  into. This is deliberate: a stale-but-fingerprint-compatible cache surviving between two
  test runs could otherwise mask a real compiler regression the test suite exists to
  catch. `DIAMOND_CASE_RUNNER=subprocess` runs every case through the real CLI instead,
  which does write caches next to the test sources (`diamond cache clear tests/cases`
  removes them).
