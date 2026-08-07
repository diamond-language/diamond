# Packages

This document covers Diamond's package resolution — a `diamond_packages/`
convention `require` falls back to when a bare package name doesn't
resolve as a relative file, plus the optional manifest a package can
declare its identity with. See `docs/roadmap.md` for what's still
aspirational (versions actually being used for anything, a lockfile, an
installer).

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
nothing else to configure — a manifest (see below) is entirely optional,
and a package with none is still fully resolvable.

## Manifests

```ruby
# diamond_packages/greeter/package.di
{"name": "greeter", "version": "0.1.0"}
```

A package may optionally include `diamond_packages/<name>/package.di`.
If present, its last-expression value — the same "last line is the
result" convention every Diamond program already has — must be a `Hash`
with a String `name` key matching the package's own directory exactly; a
package whose manifest disagrees with its own directory name fails the
whole `require` with a clear error rather than silently loading anyway
(this is the manifest's actual payoff today: catching a package that was
copied or renamed incorrectly during a manual install). An optional
`version` key, if present, must be a String — validated for type only;
nothing reads its value yet, since there's no dependency resolution to
consult it (see `docs/roadmap.md`).

A manifest is compiled and run **standalone**: it does not go through
`require`'s own loader pipeline, so `require` inside a manifest is not
supported. A manifest is metadata, not a program — this keeps it to a
single self-contained expression and avoids the loader recursing into
itself to resolve a manifest's own dependencies. There's no sandboxing
around manifest execution beyond that — same trust model the rest of the
language already has for any program it runs, and packages are
hand-installed locally, not fetched from anywhere untrusted.

If no `package.di` exists at all, none of this applies — the package
resolves exactly as it would with no manifest support at all.

## Precedence

A relative file always wins over a same-named package when both exist —
the fallback only fires once the primary relative-file `realpath()`
lookup has already failed. This is what makes the feature purely
additive: no program that already resolves via a relative file can have
its behavior changed by a `diamond_packages/` directory appearing
alongside it.

## What's deliberately out of scope so far

- **Dependencies in the manifest**: only `name` and `version` are read;
  there is no `dependencies` key or anything that consults one.
- **Versions actually meaning anything**: `version` is validated to be a
  String if present, but nothing compares, selects, or otherwise consumes
  it. This isn't just unbuilt — it's architecturally constrained:
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
