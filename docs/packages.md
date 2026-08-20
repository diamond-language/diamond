# Packages

This document covers Diamond's package resolution — a `diamond_packages/`
convention `require` falls back to when a bare package name doesn't
resolve as a relative file, plus the optional manifest a package can
declare its identity with — and `facet`, the standalone tool that
fetches and installs those packages. `facet` is a separate program (its
own binary, `build/facet`, built from `tools/facet.c`), not part of the
`diamond` runtime or its release — the same relationship RubyGems' `gem`
has to `ruby`. See `docs/roadmap.md` for what's still aspirational.

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
    package.di        # package metadata
    lib/
      greeter.di      # public entry point, same name as the package
      helpers.di       # implementation components
```

The public entry point is resolved from `lib/<name>.di`. Package code can
require sibling components with ordinary relative paths from that `lib/`
directory. The loader retains the former flat `<name>.di` location as a
compatibility fallback for packages that have not migrated yet.

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
language already has for any program it runs.

If no `package.di` exists at all, none of this applies — the package
resolves exactly as it would with no manifest support at all.

A manifest can be pretty-printed across multiple lines — a newline is
allowed right after `{`, right after each `,`, and right before `}`
(the natural positions a formatter would put one), matching the same
support every other bracket-delimited list in the language has (array/
hash literals, call arguments, parameter declarations, and more — see
`docs/syntax.md`). Newlines are not accepted *inside* an entry (between
a key and its `:`, or between `:` and the value) — an unusual style
no formatter would actually produce.

### Dependencies

```ruby
# package.di
{"name": "myapp", "version": "0.1.0", "dependencies": {"greeter": {"git": "https://example.com/user/greeter", "tag": "v1.0.0"}}}
```

An optional `dependencies` key, read only by `facet` (see below) — the
runtime's own manifest validation ignores it entirely, so adding it to
an existing manifest changes nothing about how `require` behaves. It's
a `Hash` from package name to a spec `Hash` with a required String
`git` key and **exactly one** of `tag`, `branch`, or `commit` (also
String) — ambiguous or missing ref keys are rejected before anything is
fetched.

Declaring a dependency here does **not** implicitly `require` it —
`require` and `dependencies` are separate mechanisms. A package that
uses a dependency's functions still needs its own explicit
`require "greeter"`, exactly as if that file were sitting locally;
`dependencies` only tells `facet` what to fetch and where to put it so
that `require` can find it.

## `facet`

`facet install` reads `package.di` in the current directory, resolves
every dependency (recursively — a dependency's own `dependencies` are
followed too), fetches each one via `git clone`/`checkout` (never a
shell — arguments go straight to `execvp`, so a URL or ref pulled from
a manifest can never be interpreted as shell syntax), and installs the
result into `diamond_packages/<name>/` with `.git/` stripped (the
lockfile below is the source of truth for "what commit," not a live
repository sitting inside `diamond_packages/`). It writes `facet.lock`
alongside `package.di` — a Diamond `Hash` literal, same as a manifest.
`facet` itself always writes it compact/single-line (it's
machine-generated, not something you're meant to hand-edit), but
reads one back the same way it reads any manifest, so a hand-edited
lockfile can be pretty-printed too if you ever want to:

```ruby
# facet.lock
{"greeter": {"git": "https://example.com/user/greeter", "commit": "a1b2c3d..."}}
```

a flat map of every resolved package (the whole transitive set, already
flattened) to its exact commit. Once `facet.lock` exists, `facet
install` installs exactly what it says without re-resolving anything —
the fast, reproducible path, unaffected by a tag or branch moving in
the meantime. `facet update` always re-resolves from `package.di`
(picking up anything a tracked branch has moved to) and rewrites the
lock.

Because Diamond has no registry to query dependency metadata from,
resolving *is* fetching: discovering a dependency's own transitive
dependencies requires a clone of it to read its `package.di`. There is
therefore no separate "resolve, then fetch" phase.

**Version conflicts are hard errors, not resolved.** If two different
requesters in the dependency graph want a different `git`/ref for the
same package name, `facet` reports both requesters by name and stops —
it can never pick one over the other, because of the same flat-namespace
constraint noted above: two versions of one package name could never
coexist in a single compiled Diamond program anyway, so there is no
"resolve to whichever" fallback to fall back to. A cycle in the
dependency graph (A depends on B depends on A) terminates safely with
no special handling — the second time the walk reaches an
already-resolved name it just stops, the same check that would catch a
genuine version conflict.

A cloned dependency's own `package.di` must declare a `name` matching
the dependency key it was fetched as — the same rule `require` already
enforces for hand-installed packages (see above) — so a misconfigured
or renamed dependency fails clearly at `facet install` time rather than
surprising `require` later.

No `facet init` or `facet add <dep>` — `package.di` stays a plain,
hand-edited Diamond `Hash` literal; only the fetch/install/lock steps
are automated.

### A real example: `packages/http`

Diamond's runtime has no HTTP support built in on purpose — `lib/http.di`
used to be bundled with the language and was pulled back out once
`facet` made "install it as a dependency" a real option. It briefly
lived as its own standalone sibling repo; it now lives at
`packages/http` in this same repo instead, kept there (rather than
folded back into `lib/`) for the same reason it was pulled out of
`lib/` in the first place: it's a real, ordinary package — its own
`package.di`, its own `README.md`, its own `test.sh` — not something
`require` finds automatically the way `lib/core.di`'s prelude is. It
just happens to be developed alongside the runtime instead of in a
separate git history, which buys nothing on its own once a package has
no actual dependents outside this repo to keep in sync with.

To use it from another project the same way any `facet` dependency
works, either copy `packages/http/` into that project's
`diamond_packages/http/` directly, or give it its own git remote and
point a `dependencies` entry at that:

```ruby
{"name": "myapp", "dependencies": {"http": {"git": "<url-of-a-remote-for-packages/http>", "tag": "v0.1.0"}}}
```

`facet` has no notion of "a subdirectory of a larger repo is the
package root" — a `git` dependency's whole repo becomes the installed
package, so the above only works against a remote whose root actually
is `packages/http`'s own contents, not this repo's.

## Precedence

A relative file always wins over a same-named package when both exist —
the fallback only fires once the primary relative-file `realpath()`
lookup has already failed. This is what makes the feature purely
additive: no program that already resolves via a relative file can have
its behavior changed by a `diamond_packages/` directory appearing
alongside it.

## What's deliberately out of scope so far

- **The `version` key**: still validated to be a String if present, but
  nothing compares, selects, or otherwise consumes it — `dependencies`
  pins an exact git ref instead, entirely independent of `version`.
  This isn't just unbuilt — it's architecturally constrained: Diamond
  compiles every `require`d file into *one* flat namespace with small,
  global, shared tables (`DIAMOND_MAX_FUNCTIONS`, `DIAMOND_MAX_CLASSES`,
  `DIAMOND_MAX_MODULES` in `src/vm.h`), so two versions of the same
  package could never coexist in one compiled program — same
  function/class names would collide. This is also why `facet` treats a
  ref conflict as a hard error rather than trying to resolve it (see
  above): there is no such thing as "both," so nothing could ever be
  resolved *to*.
- **A hosted registry/index**: `facet install greeter` by short name,
  search, or anything else that would need a service to query — package
  identity is a git URL, full stop.
- **`facet init`/`facet add`**: manifest-editing commands; `package.di`
  is hand-edited.

Each of these is a plausible next slice, sized independently rather than
attempted together — see `docs/roadmap.md`.
