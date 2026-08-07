# Packages

This document covers Diamond's package resolution — a `diamond_packages/`
convention `require` falls back to when a bare package name doesn't
resolve as a relative file. See `docs/roadmap.md` for what's still
aspirational (manifests, versions, a lockfile, an installer).

## `require "name"` resolving a package

```ruby
require "greeter"
greet("world")
```

If `greeter.di` exists next to the requiring file, `require` behaves
exactly as it always has — pure relative-file resolution, unchanged. Only
when that lookup fails does `require` try one more candidate:
`diamond_packages/greeter/greeter.di`, resolved against the *process's
current working directory* (not the requiring file's own directory —
packages are project-level, so they're anchored the same way the
top-level entry file itself is). If that resolves, its source is spliced
in exactly as any other required file would be; if it doesn't, the
original "cannot require" error is reported, unchanged.

This is a **fallback, not new syntax**: `require "name"` means exactly
what it always meant (try the relative file first), so every existing
`require` call in every existing program is unaffected. The fallback only
ever activates for a *bare* name — one containing no `/` — so
`require "sub/helper"` is never treated as a package lookup, only ever
resolved relative to the requiring file (matching the pre-existing
behavior for any path that already looks like a path).

## Layout convention

```
diamond_packages/
  greeter/
    greeter.di       # entry point, same name as the directory
    helpers.di        # anything else the package needs
```

The package's own name is used twice — as the directory and as its entry
file's basename — so a package's internal files can `require` each other
by ordinary relative path (`require "helpers"` from inside
`diamond_packages/greeter/greeter.di` resolves relative to
`diamond_packages/greeter/`, the same relative-file resolution any
required file already gets) without needing any awareness of the
package convention that got it loaded in the first place. There is
nothing else to configure — no manifest file is read or required for a
package to be resolvable.

## Precedence

A relative file always wins over a same-named package when both exist —
the fallback only fires once the primary relative-file `realpath()`
lookup has already failed. This is what makes the feature purely
additive: no program that already resolves via a relative file can have
its behavior changed by a `diamond_packages/` directory appearing
alongside it.

## What's deliberately out of scope so far

- **Manifests / package metadata**: no `name`/`version`/`dependencies`
  file is read. `require` is pure compile-time text-splicing (see
  `src/loader.c`'s `expand()`) — reading structured metadata mid-resolution
  would mean the loader compiling and running a small Diamond program to
  get a `Hash` back, a real mechanism of its own, not bundled into this
  round.
- **Versions and dependency resolution**: there is no version concept at
  all yet. This isn't just unbuilt — it's architecturally constrained:
  Diamond compiles every `require`d file into *one* flat namespace with
  small, global, shared tables (`DIAMOND_MAX_FUNCTIONS`,
  `DIAMOND_MAX_CLASSES`, `DIAMOND_MAX_MODULES` in `src/vm.h`), so two
  versions of the same package could never coexist in one compiled
  program — same function/class names would collide. Any future
  version-resolution logic can only ever mean "resolve to exactly one
  version, or error," never real multi-version support.
- **A lockfile**: meaningless without versions to lock.
- **Fetching/installing**: there is no HTTP client in Diamond (only
  outbound `TCPSocket.connect` and `lib/http.di`'s *server*-side pieces —
  see `docs/io.md`/`docs/http.md`) and no process-spawning capability, so
  there's no way to build fetch tooling yet even if there were a registry
  to fetch from, which there also isn't. Populating `diamond_packages/` is
  a manual, hand-managed step for now.

Each of these is a plausible next slice, sized independently rather than
attempted together — see `docs/roadmap.md`.
