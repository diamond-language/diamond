# Compatibility in 1.x

D6 follows semantic versioning from 1.0.0:

- **Patch releases** (1.0.1, 1.0.2, ...) contain only bug fixes and performance
  improvements.
- **Minor releases** (1.1, 1.2, ...) may also add features.
- **Nothing in 1.x breaks compatibility.** A program, project or cut that works
  on one 1.x release keeps working on every later 1.x release. Anything that has
  to break waits for 2.0.

## Language and standard library

Source that compiles and runs on 1.x compiles and runs, with the same results,
on later 1.x releases. Later releases may add syntax, core methods, classes,
modules, cuts and command-line options.

These are not covered:

- **Bugs.** A fix may change behavior that contradicted this documentation, or
  that crashed, hung or corrupted data.
- **Security fixes,** which may tighten what the sandbox, TLS, the HTTP parser or
  the package installer accept.
- **Message text.** Exception classes and the conditions that raise them are
  stable. The wording of error messages, warnings, backtraces and `inspect` output is
  not.
- **Performance and memory use,** which may change in either direction.
- **Interfaces marked experimental,** below.

## Compiled programs

The bytecode cache and the programs `d6 build` produces belong to the exact
build of `d6` that made them; see [caching](caching.md). Another build
treats a cache file as a miss and recompiles the source, so upgrading needs no
action. A standalone executable keeps running as built. To move it to a new
release, run `d6 build` again with that release.

There is no bytecode format to target. Distribute source or executables, not
cache files.

## Projects and packages

Every 1.x `facet` reads `Cutfile`, `Cutfile.dev` and `Cutfile.lock` files
written by earlier 1.x releases, and installs from them the same versions they
lock. Later releases may add manifest keys. A project that uses a new key needs
a `facet` that knows it, and older releases reject it rather than ignoring it.

Published cut releases are immutable; see the [cut contract](cut-contract.md).
A cut that installs and loads on 1.x keeps doing so on later 1.x releases.

## Experimental interfaces

These may change in any release:

- **The C embedding API.** The headers under `src/`, `ProgramBuilder` and the
  VM's native entry points are the implementation, not a library interface.
  No release installs a header or a shared library to embed.
- **Tuning and diagnostic variables,** such as `DIAMOND_JIT_THRESHOLD`,
  `DIAMOND_STRESS_GC` and the `DIAMOND_TRACE_*` family. `DIAMOND_JIT=1`, which
  turns the JIT on, is stable.
- **The self-hosted compiler** under `selfhost/`. It is a test oracle for the
  native compiler, not a supported frontend.
- **The debugger and language server protocols** beyond what their editors use:
  [debugging](debugging.md) and [lsp](lsp.md) describe the supported clients.

## Platforms

Supported platforms are listed in [portability](portability.md). A 1.x release
may add platforms. It drops one only when its toolchain or vendor support ends,
and the changelog announces the change a release ahead.
