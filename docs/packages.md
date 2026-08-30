# Cuts

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
present, its last-expression value — the same "last line is the result"
convention every Diamond program already has — must be a `Hash` with a
String `name` key matching the cut's own directory exactly; a cut whose
manifest disagrees with its own directory name fails the whole
`require_cut` with a clear error rather than silently loading anyway
(this is the manifest's actual payoff today: catching a cut that was
copied or renamed incorrectly during a manual install). An optional
`version` key, if present, must be a String — validated for type only;
nothing reads its value yet, since there's no dependency resolution to
consult it (see `docs/roadmap.md`).

A manifest is compiled and run **standalone**: it does not go through
`require`/`require_cut`'s own loader pipeline, so neither is supported
inside a manifest. A manifest is metadata, not a program — this keeps it
to a single self-contained expression and avoids the loader recursing
into itself to resolve a manifest's own dependencies. There's no
sandboxing around manifest execution beyond that — same trust model the
rest of the language already has for any program it runs.

If no `diamond.cut` exists at all, none of this applies — the cut resolves
exactly as it would with no manifest support at all.

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
# diamond.cut, at your project's own root
{"name": "myapp", "version": "0.1.0", "dependencies": {"greeter": {"git": "https://example.com/user/greeter", "tag": "v1.0.0"}}}
```

An optional `dependencies` key, read only by `facet` (see below) — the
runtime's own manifest validation ignores it entirely, so adding it to
an existing manifest changes nothing about how `require_cut` behaves. It's
a `Hash` from cut name to a spec `Hash` with a required String
`git` key and **exactly one** of `tag`, `branch`, or `commit` (also
String) — ambiguous or missing ref keys are rejected before anything is
fetched.

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
{"greeter": {"git": "https://example.com/user/greeter", "commit": "a1b2c3d..."}}
```

a flat map of every resolved cut (the whole transitive set, already
flattened) to its exact commit. Once `facet.lock` exists, `facet
install` installs exactly what it says without re-resolving anything —
the fast, reproducible path, unaffected by a tag or branch moving in
the meantime. `facet update` always re-resolves from `diamond.cut`
(picking up anything a tracked branch has moved to) and rewrites the
lock.

Because Diamond has no registry to query dependency metadata from,
resolving *is* fetching: discovering a dependency's own transitive
dependencies requires a clone of it to read its `diamond.cut`. There is
therefore no separate "resolve, then fetch" phase.

**Version conflicts are hard errors, not resolved.** If two different
requesters in the dependency graph want a different `git`/ref for the
same cut name, `facet` reports both requesters by name and stops —
it can never pick one over the other, because of the same flat-namespace
constraint noted above: two versions of one cut name could never
coexist in a single compiled Diamond program anyway, so there is no
"resolve to whichever" fallback to fall back to. A cycle in the
dependency graph (A depends on B depends on A) terminates safely with
no special handling — the second time the walk reaches an
already-resolved name it just stops, the same check that would catch a
genuine version conflict.

A cloned dependency's own `diamond.cut` must declare a `name` matching
the dependency key it was fetched as — the same rule `require_cut` already
enforces for hand-installed cuts (see above) — so a misconfigured
or renamed dependency fails clearly at `facet install` time rather than
surprising `require_cut` later.

No `facet init` or `facet add <dep>` — `diamond.cut` stays a plain,
hand-edited Diamond `Hash` literal; only the fetch/install/lock steps
are automated.

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

- **The `version` key**: still validated to be a String if present, but
  nothing compares, selects, or otherwise consumes it — `dependencies`
  pins an exact git ref instead, entirely independent of `version`.
  This isn't just unbuilt — it's architecturally constrained: Diamond
  compiles every `require`/`require_cut`d file into *one* flat namespace
  with small, global, shared tables (`DIAMOND_MAX_FUNCTIONS`,
  `DIAMOND_MAX_CLASSES`, `DIAMOND_MAX_MODULES` in `src/vm.h`), so two
  versions of the same cut could never coexist in one compiled program —
  same function/class names would collide. This is also why `facet`
  treats a ref conflict as a hard error rather than trying to resolve it
  (see above): there is no such thing as "both," so nothing could ever be
  resolved *to*.
- **A hosted registry/index**: `facet install greeter` by short name,
  search, or anything else that would need a service to query — cut
  identity is a git URL, full stop.
- **`facet init`/`facet add`**: manifest-editing commands; `diamond.cut`
  is hand-edited.

Each of these is a plausible next slice, sized independently rather than
attempted together — see `docs/roadmap.md`.
