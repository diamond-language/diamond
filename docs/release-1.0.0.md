# d6 1.0.0

d6 1.0 is the language, runtime and tools of the seven release candidates, now with a
compatibility promise. Nothing changed since 1.0.0-rc7.

## What 1.0 promises

From 1.0.0, d6 follows semantic versioning:

- **Patch releases** (1.0.1, 1.0.2, ...) carry only bug fixes and performance work.
- **Minor releases** (1.1, 1.2, ...) may also add features.
- **Nothing in 1.x breaks compatibility.** A program, project or cut that works on one
  1.x release keeps working on every later 1.x release. Anything that has to break waits
  for 2.0.

[Compatibility](compatibility.md) spells out what that covers: the language and standard
library, Cutfiles and lockfiles, and published cuts. It also lists what it doesn't cover,
such as error-message wording, performance, the bytecode cache, and interfaces marked
experimental.

## What's in 1.0

- **The language:** classes, modules, structs, sealed hierarchies and exhaustive
  `case`/`when`; gradual static typing, checked at compile time where types are known and
  at run time where they aren't; generics; blocks, closures and safe navigation (`&.`).
- **The runtime:** a register-bytecode VM with an optional x86-64 JIT (`DIAMOND_JIT=1`),
  threads with channels and supervisors, a sandbox mode with resource limits,
  networking with TLS, subprocesses, and SQLite, PostgreSQL and MySQL drivers.
- **The tools:** `d6 build` for standalone executables, `facet` and the
  [cuts.rolld6.codes](https://cuts.rolld6.codes) registry for packages, a language server,
  a step debugger, and the
  [VS Code extension](https://marketplace.visualstudio.com/items?itemName=rolld6.d6).

## Installing

[Getting started](getting-started.md) covers the `.deb` and `.rpm` packages, the tarballs
for mise (`mise use -g github:diamond-language/d6`) or unpacking anywhere, and building
from source.
