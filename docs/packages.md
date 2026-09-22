# Cuts

For the proposed publishable package contract and hosted registry roadmap, see
[cut-contract.md](cut-contract.md) and
[package-registry-plan.md](package-registry-plan.md). This page documents the
currently implemented behavior.

`facet check <cut-directory> [--files]` validates the first publishable layout,
manifest, and runtime file inventory without installing dependencies or
creating an archive. `--files` prints the sorted candidate artifact file list.
`facet pack <cut-directory> <output.tar>` packages that exact list as a
deterministic ustar archive and prints its SHA-256 digest. Neither command
publishes to a registry yet.
`facet verify <archive.tar> [--sha256 <digest>]` checks a packed artifact
without extracting it. The expected digest is required when verifying bytes
against a trusted lockfile or registry record.
See [cut-contract.md](cut-contract.md) for its current scope.

For a local checkout before registry installation is available,
`tools/install_local_cuts.sh <project-directory>` installs all bundled cuts as
verified artifacts from `packages/` into that project's `cuts/` directory.
This development bootstrap does not
resolve registry versions or write a registry lockfile.

This document covers Diamond's package resolution — packages are called
**cuts**, resolved from a `cuts/` directory via `require_cut`, a require
form separate from and never competing with ordinary `require` — plus the
optional manifest a cut can declare its identity with, and `facet`, the
standalone tool that fetches and installs cuts. `facet` is a separate
program (its own binary, `build/facet`, built from `tools/facet.c`), not
part of the `diamond` runtime or its release — the same relationship
RubyGems' `gem` has to `ruby`. See `docs/roadmap.md` for what's still
aspirational.

## `require_cut "name"`

```ruby
require_cut "greeter"
greet("world")
```

`require_cut` is a require form of its own, distinct from plain `require`
— parsed the same textual way (`src/loader.c`'s `expand()`), but resolved
completely differently: `require_cut "name"` always resolves to
`cuts/name/lib/name.di`, anchored to the *process's own current working
directory* (not the requiring file's own directory — cuts are
project-level, so they're anchored the same way the top-level entry file
itself is). If `cuts/name/lib/name.di` doesn't exist, the loader falls
back to the flat `cuts/name/name.di` location before giving up. If neither
resolves, `require_cut` fails with a clear "cannot require_cut" error
rather than silently doing nothing.

`name` must be a bare cut name — a `require_cut "sub/greeter"` containing
a `/` is rejected outright, before any filesystem lookup, with a clear
error. There is no path-like form of `require_cut`; a cut is always
addressed purely by its own name.

**`require` and `require_cut` never compete.** Plain `require "name"`
means exactly what it always has — resolve `name` relative to the
requiring file (or as an absolute path) — full stop, with no fallback of
any kind. It never looks inside `cuts/`, even for a bare name with no
`/`. A `cuts/greeter/` directory sitting next to a file that does
`require "greeter"` changes nothing about what that `require` resolves
to; the two are simply unrelated. This is deliberate: earlier, a bare
`require "name"` first tried a relative file and silently fell back to a
package lookup if that failed — that ambiguity (was this a local file or
a dependency?) is exactly what `require_cut` as a *separate, explicit*
form removes. Every existing `require "name"` in every existing program
keeps meaning exactly what it always meant.

## Layout convention

```
cuts/
  greeter/
    diamond.cut           # manifest (optional)
    lib/
      greeter.di       # public entry point, same name as the cut
      helpers.di        # implementation components
```

The public entry point is resolved from `lib/<name>.di`. A cut's own code
can require sibling components with ordinary relative paths from that
`lib/` directory. The loader retains the flat `<name>.di` location (no
`lib/` subdirectory) as a compatibility fallback for cuts that have not
migrated yet.

## Manifests

```ruby
# cuts/greeter/diamond.cut
{"name": "greeter", "version": "0.1.0"}
```

A cut may optionally include `cuts/<name>/diamond.cut` — a fixed filename,
not parameterized by the cut's own name, since the same file also has to
work unmodified as the manifest sitting at a dependency's own repository
root before `facet` ever installs it anywhere (see "`facet`" below). If
present, it must contain a data-only `Hash` with a String `name` key matching
the cut's own directory exactly; a cut whose
manifest disagrees with its own directory name fails the whole
`require_cut` with a clear error rather than silently loading anyway
(this is the manifest's actual payoff today: catching a cut that was
copied or renamed incorrectly during a manual install). An optional
`version` key, if present, must be a String — validated for type only;
nothing reads its value yet, since there's no dependency resolution to
consult it (see `docs/roadmap.md`).

A manifest must contain a single data-only Hash literal. The loader and
`facet` parse it without compiling or running Diamond code. Expressions,
calls, interpolation, duplicate keys, and trailing code are rejected.
`require` and `require_cut` are not supported inside metadata.

If no `diamond.cut` exists at all, none of this applies — the cut resolves
exactly as it would with no manifest support at all.

A manifest can be pretty-printed across multiple lines. Whitespace is allowed
around keys, colons, values, commas, and braces. A trailing comma is accepted
for compatibility with existing `facet.lock` files.

### Dependencies

```ruby
# diamond.cut, at your project's own root
{"name": "myapp", "version": "0.1.0", "dependencies": {"greeter": {"git": "https://example.com/user/greeter", "tag": "v1.0.0"}}}
```

An optional `dependencies` key, read only by `facet` (see below) — the
runtime's own manifest validation ignores it entirely, so adding it to
an existing manifest changes nothing about how `require_cut` behaves. It's
a `Hash` from cut name to a spec `Hash` with a required String
`git` key and **exactly one** of `tag`, `branch`, `commit`, or `version`
(also String) — ambiguous or missing ref/version keys are rejected
before anything is fetched.

`version` is a semver range/constraint instead of a pinned ref —
`^1.2.3` (compatible-with, semver's own "don't change the left-most
non-zero digit" rule), `~1.2.3` (patch-level only), `>=1.0.0 <2.0.0`
(one or two whitespace-separated comparator terms), or an exact
`1.2.3`. There's still no registry: a dependency's available versions
come from `git ls-remote --tags --refs` against its own repository,
filtered to tags that parse as semver (with or without a leading `v`).
When two different requesters in the dependency graph constrain the
same cut, `facet` intersects both ranges and picks the *highest*
available tag satisfying the result, deterministically. If a requester
imposing a tighter range is only discovered *after* the cut already
resolved to a tag that range excludes, `facet` retries the whole
resolution rather than treating that as unrecoverable — see "real
backtracking" below. An empty intersection (no version satisfies both,
independent of resolution order) is still an immediate hard error
naming both requesters and their own ranges; mixing an exact ref and a
`version` constraint for the same cut is also a hard error, except when
whichever side resolved first happens to already satisfy the other's
own constraint too — that one is never resolved by backtracking (see
below). See `tools/semver.h` for the exact range grammar and
[`tools/facet.c`](../tools/facet.c) for the resolver implementation.

Declaring a dependency here does **not** implicitly `require_cut` it —
`require_cut` and `dependencies` are separate mechanisms. A project that
uses a dependency's functions still needs its own explicit
`require_cut "greeter"`, exactly as if that cut were installed by hand;
`dependencies` only tells `facet` what to fetch and where to put it so
that `require_cut` can find it.

## `facet`

`facet install` reads `diamond.cut` in the current directory, resolves
every dependency (recursively — a dependency's own `dependencies` are
followed too), fetches each one via `git clone`/`checkout` (never a
shell — arguments go straight to `execvp`, so a URL or ref pulled from
a manifest can never be interpreted as shell syntax), and installs the
result into `cuts/<name>/` with `.git/` stripped (the lockfile below is
the source of truth for "what commit," not a live repository sitting
inside `cuts/`). Because a resolved dependency's own cloned checkout is
moved into place as-is (`rename`, not a re-copy that could rewrite
anything), its manifest keeps the same fixed `diamond.cut` filename it had at
its own repository root — this is exactly why that filename isn't
parameterized by the cut's own name. It writes `facet.lock` alongside
`diamond.cut` — a Diamond `Hash` literal, same as a manifest. `facet` itself
always writes it compact/single-line (it's machine-generated, not
something you're meant to hand-edit), but reads one back the same way it
reads any manifest, so a hand-edited lockfile can be pretty-printed too
if you ever want to:

```ruby
# facet.lock
{"greeter": {"source": "git", "git": "https://example.com/user/greeter", "commit": "a1b2c3d..."}}
```

a flat map of every resolved cut (the whole transitive set, already
flattened) to its exact commit. Once `facet.lock` exists, `facet
install` installs exactly what it says without re-resolving anything —
the fast, reproducible path, unaffected by a tag or branch moving in
the meantime. `facet update` always re-resolves from `diamond.cut`
(picking up anything a tracked branch has moved to) and rewrites the
lock.

New locks mark each entry with `"source": "git"`. Older Git locks without
that key remain readable. Unknown source types and lockfile fields fail
explicitly; registry entries will have a separate schema when registry
installation is implemented.

Because Diamond has no registry to query dependency metadata from,
resolving *is* fetching: discovering a dependency's own transitive
dependencies requires a clone of it to read its `diamond.cut`. There is
therefore no separate "resolve, then fetch" phase.

**Exact-ref conflicts are hard errors, never resolved.** If two
different requesters in the dependency graph want a different `git`/
exact ref (`tag`/`branch`/`commit`) for the same cut name, `facet`
reports both requesters by name and stops — it can never pick one over
the other, because of the same flat-namespace constraint noted above:
two versions of one cut name could never coexist in a single compiled
Diamond program anyway, so there is no "resolve to whichever" fallback
to fall back to. Two `version`-constrained requesters, by contrast,
intersect (see "Dependencies" above) rather than conflicting outright,
as long as some version satisfies both ranges. A cycle in the
dependency graph (A depends on B depends on A) terminates safely with
no special handling — the second time the walk reaches an
already-resolved (or already-pending) name it just stops, the same
check that would catch a genuine conflict.

Because a version-constrained dependency's own transitive dependencies
can't be discovered without first picking (and cloning) one concrete
version of it, and that choice depends on every requester's constraint
— some of which may not be discovered until later in the walk — `facet`
resolves exact-ref dependencies immediately (as always) but defers each
version-constrained name until the exact-ref-reachable part of the
graph runs dry, then resolves one pending name at a time, folding
whatever new work that uncovers back in, until nothing pending remains.

**Real backtracking**: it's still possible for a name to resolve (clone
a concrete tag) before every requester's own constraint on it is known
— a requester reached only through a *different* pending name, resolved
later, can turn out to want a tighter range the already-chosen tag
doesn't satisfy. Rather than treating that as unrecoverable, `facet`
wipes its ephemeral scratch clones and re-resolves the whole graph from
scratch, carrying forward the full intersected constraint history for
every version-constrained name across attempts — so a later attempt
already knows everything an earlier one discovered about that name,
however late, and resolves it correctly the first time that attempt
reaches it. This converges quickly in practice (at most one retry per
name that's ever affected) and is bounded (a resolver-internal error,
not a manifest problem, if it somehow doesn't). A genuinely disjoint
pair of constraints is unaffected by any of this — that's still an
immediate hard error, not a reason to retry.

A cloned dependency's own `diamond.cut` must declare a `name` matching
the dependency key it was fetched as — the same rule `require_cut` already
enforces for hand-installed cuts (see above) — so a misconfigured
or renamed dependency fails clearly at `facet install` time rather than
surprising `require_cut` later.

### `facet init` and `facet add`

```sh
facet init [name]     # writes a fresh diamond.cut; defaults name to the
                       # current directory's own basename if omitted
facet add <name> --git <url> (--tag <ref> | --branch <ref> | --commit <ref> | --version <constraint>)
```

`facet init` refuses to run if `diamond.cut` already exists — it never
overwrites one. `facet add` requires one to already exist (`facet init`
first, or hand-write one), parses it, appends the new dependency (a hard
error if that name is already present — edit the existing entry by hand,
or remove it first, rather than guessing you meant to replace it), and
rewrites the file. Both validate what they write the same way `facet
install` would reject it later: exactly one of `--tag`/`--branch`/
`--commit`/`--version`, and a `--version` value that actually parses as a
constraint (`tools/semver.h`'s own grammar) — so a mistake is caught
immediately, not at the next `facet install`.

`facet add` rewrites the **entire** `diamond.cut`, not just the one entry
being added — there's no Hash-literal-aware text editor here, only a
parse-the-whole-thing-and-regenerate-it round trip (mirroring how `facet
install`/`update` already regenerate `facet.lock` from scratch every
time). Any hand-added comment or unusual formatting in an existing
`diamond.cut` does not survive a `facet add`; the regenerated file uses
the same pretty-printed, multi-line style described above. It does
round-trip every existing dependency's own ref kind exactly (a `branch`
dependency stays a `branch` dependency after adding an unrelated one) —
this is the one thing worth getting right in a full-file rewrite, since
guessing wrong here would silently change *what a dependency actually
tracks*, not just how the file looks.

### A real example: `packages/http`

HTTP support is an ordinary cut rather than part of the runtime. The bundled
example at `packages/http` has its own `diamond.cut`, README, and tests, and is
loaded with `require_cut` rather than discovered by the core `require` path.

To use it from another project the same way any `facet` dependency
works, either copy `packages/http/` into that project's
`cuts/http/` directly, or give it its own git remote and point a
`dependencies` entry at that:

```ruby
{"name": "myapp", "dependencies": {"http": {"git": "<url-of-a-remote-for-packages/http>", "tag": "v0.1.0"}}}
```

`facet` has no notion of "a subdirectory of a larger repo is the
cut root" — a `git` dependency's whole repo becomes the installed
cut, so the above only works against a remote whose root actually
is `packages/http`'s own contents, not this repo's.

## `require` and `require_cut` never shadow each other

A relative file and a same-named cut can sit side by side without either
one affecting the other — `require "greeter"` only ever considers a
relative file (or absolute path), and `require_cut "greeter"` only ever
considers `cuts/greeter/`. There is no precedence question to resolve
between them, because there is no path along which they could compete in
the first place.

## What's deliberately out of scope so far

- **A manifest's own top-level `version` key** (`{"name": "greeter",
  "version": "0.1.0"}` — a cut declaring its own version) is still
  validated to be a String and nothing more; nothing compares, selects,
  or otherwise consumes it. Don't confuse this with a `dependencies`
  entry's own `version` key (a *requester's* constraint on some other
  cut, "Dependencies" above) — that one is real now. A cut's own
  top-level `version` staying inert isn't an oversight sharing the same
  gap: `facet` learns what versions a dependency actually has from its
  git tags, never from a `diamond.cut` string the requester would have
  to trust regardless.
- **Two versions of the same cut coexisting** is architecturally
  constrained, not just unbuilt: Diamond compiles every `require`/
  `require_cut`d file into *one* flat namespace with small, global,
  shared tables (`DIAMOND_MAX_FUNCTIONS`, `DIAMOND_MAX_CLASSES`,
  `DIAMOND_MAX_MODULES` in `src/vm.h`), so two versions of the same cut
  could never coexist in one compiled program — same function/class
  names would collide. This is why an exact-ref conflict is still a hard
  error rather than something to resolve (two *version*-constrained
  requesters intersect instead, since a range genuinely can pick one
  version satisfying both — see "Dependencies" above): there is no such
  thing as "both," so nothing could ever be resolved *to* when the
  refs themselves disagree.
- **A hosted registry/index**: `facet install greeter` by short name,
  search, or anything else that would need a service to query — cut
  identity is a git URL, full stop.

Each of these is a plausible next slice, sized independently rather than
attempted together — see `docs/roadmap.md`.
