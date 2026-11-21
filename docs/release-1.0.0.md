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

## The release candidates

Seven release candidates, 2026-11-09 to 2026-11-16, led to 1.0. What they changed:

- **Smaller programs and lower memory use.** Type sets and string constants store only
  what they use. Pheint's bytecode cache shrank from 71 MB to 7.3 MB and its executable
  from 76 MB to 11 MB. Under load with four workers, Pheint dropped from 651 MiB to 245 MiB
  and Skindicate from 357 MiB to 152 MiB ([soak report](internal/rc4-application-soak.md)).
- **A faster JIT.** Values are stored as whole 8-byte words, and `select`,
  `each_with_index`, `reduce` and `times` run their blocks as native loops. fib takes
  half the cycles it did before, and with the JIT d6 is at least even with Ruby's YJIT on
  all 21 local benchmark pairs.
- **Everyday conveniences and Ruby's answers.** More argument shapes on Array, Hash, String
  and numbers (`each_slice`, `sum(0.0)`, `min(2)`, `merge` with a block, `chomp(".csv")`,
  `floor(2)` and others: see [collections](collections.md#prelude-methods)), `Hash.new(0)`,
  `Array.new(3) do |i| ... end`, and `-2.5.floor()` giving -3 as in Ruby.
- **Correctness.** Trapped signals now run while a server is blocked in `IO.poll`, `accept`
  or a socket read. A thread that fails without being joined prints its exception. JIT
  blocks dispatch methods correctly on captured instance variables. Cross-feature workflow
  tests run across the interpreter and JIT, GC stress, bytecode caches and standalone
  executables.
- **Distribution.** Tarballs for mise or unpacking anywhere, the VS Code extension in the
  Marketplace, a docs page for every published cut, and the move to
  [rolld6.codes](https://rolld6.codes/) and [cuts.rolld6.codes](https://cuts.rolld6.codes).

The [changelog](../CHANGELOG.md) lists every change in each candidate.

## Installing

[Getting started](getting-started.md) covers the `.deb` and `.rpm` packages, the tarballs
for mise (`mise use -g github:diamond-language/d6`) or unpacking anywhere, and building
from source.
