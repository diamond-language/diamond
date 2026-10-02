# examples/resolver

A package version resolver: given an index of published releases and some
root requirements, it picks one version of every package they transitively
need, or reports exactly why it cannot. It is a small, real program built to
exercise Diamond's type system: generics, union types, sealed hierarchies with
exhaustive `case`, `Comparable`, and `freeze`.

```text
$ diamond resolver.di testdata/index.txt web:">=1.0.0"
web 2.0.0
rack 2.0.0
json 2.0.0

$ diamond resolver.di testdata/index.txt web:"=1.1.0"
web 1.1.0
rack 1.2.0
json 1.1.0
logger 0.5.0

$ diamond resolver.di testdata/index.txt admin:"*"
conflict: json must be exactly 1.0.0 (wanted by admin 1.0.0)
  but 2.0.0 is already chosen
```

## Usage

```text
resolver.di INDEX NAME:CONSTRAINT...
resolver.di --check INDEX
```

`INDEX` has one release per line: `name version [dep:constraint ...]`, with
blank lines and `#` comments skipped (see `testdata/index.txt`). A constraint
is `*` (anything), `=1.2.0` (exactly), `>=1.2.0` (that or newer) or
`1.0.0..2.0.0` (from the first, up to but not including the second).

Resolution picks the newest version that satisfies each constraint, walking
dependencies depth-first. Exit status is 0 when everything resolves and 1 for
a conflict or a missing package. `--check` lists dependencies on packages the
index never publishes (exit 1 if any). Other statuses: 64 usage, 65 bad data
(a malformed version, constraint or index line), 66 unreadable index.

The resolver does not backtrack: the first version chosen for a package is
final. That is why `admin:*` above conflicts (`web` 2.0.0 is chosen first and
pulls in `json` 2.0.0, but `admin` needs `json` exactly 1.0.0) while
`json:=1.0.0 web:=1.0.0 admin:*` succeeds: pinning the versions first steers
the same walk to a solution.

## What it shows

- **Generics on functions and methods.** `unique[T](items: Array[T]) ->
  Array[T]` (`lib/set.di`) ties its argument and result together, and
  `Set#add[T]` binds `T` per call. In this version type parameters exist on
  functions and methods only; `class Set[T]` is a parse error, so the `Set`
  class stores untyped members. The compiler checks calls it can prove wrong;
  anything else is checked as the call runs: `unique(5)` raises a `TypeError`
  (`expected Array[TypeVariable], got Int`).
- **Hand-written `Set`.** Diamond has no built-in `Set`, so `lib/set.di` builds
  one over a Hash. `--check` uses `unique` to report each missing dependency
  once, however many releases mention it.
- **Union types.** `resolve` returns `Resolved | Conflict | Missing`, three
  unrelated classes; `Conflict#chosen` is `Version | Nil`, and
  `Index#newest_matching` returns `Release | Nil` so callers must handle the
  empty case.
- **Exhaustive `case` over a union.** `report` in `resolver.di` has a `case`
  with no `else` that names `Resolved`, `Conflict` and `Missing`. Delete one
  arm and the file stops compiling: `case is not exhaustive over its
  subject's known closed type; missing: Missing`.
- **Sealed hierarchy.** `Constraint` is `sealed` with four subclasses
  (`lib/constraint.di`). `satisfies?` and `describe` both `case` over it with
  no `else`, using object patterns (`Between{low: low, high: high}`) to bind
  the readers; remove the `AtLeast` arm and compilation fails with `missing:
  AtLeast`. Adding a fifth constraint kind would break each of them the same
  way.
- **`Comparable`.** `Version` defines `<=>` once (`lib/version.di`) and gets
  `<`, `>=`, `==`, `between?`, `sort`, `sort_by` and `max` from
  `include Comparable`. The index keeps each package's releases sorted this
  way, so "newest matching" is `candidates.last()`.
- **`freeze`, and that it is shallow.** `Version#initialize` ends with
  `self.freeze()`, so a version can never change once it is a Hash key or a
  stored value. The loaded `Index` is frozen too, but `freeze()` only blocks
  assigning instance variables; a frozen object's Hash is still editable. So
  `Index#seal` freezes `@releases` as well, and a later `index.add(...)` raises
  `FrozenError` (`testdata/frozen.di`).
- **Plain `Struct`-style data.** `struct Release(name:, version:, needs:)`
  generates the constructor and readers.

## Things worth knowing

- Every call needs parentheses. `release.name` without `()` is not an error:
  it is a bound-method reference, so it prints as `#<Closure>` and quietly
  breaks Hash lookups. The readers are always called `release.name()`.
- A one-line guard such as `return problem if problem != nil` does not narrow
  `problem` for the return type check, but the block form does:
  `if problem != nil` / `return problem` / `end`. `pin` and `resolve` use the
  block form so their `Conflict | Missing | Nil` result narrows to the declared
  return type.
- Diamond has no `catch`/`throw`, so non-local exits here are either exceptions
  (`VersionError`, `IndexError`) or a result union like `Conflict | Missing | Nil`.
- Destructuring (`[a, b] = text.split(":")`) requires exactly that many parts,
  or it raises `ArgumentError`, so the parsers check `parts.length()` first.

## Test

```sh
bash smoke_test.sh
```

Runs the program interpreted and as a `diamond build` binary over eleven
resolutions (including the conflict, the pinned workaround, a missing package,
a cycle and `--check`), comparing both to `testdata/session.expected`; checks
every error status and message; and runs `testdata/frozen.di` for the freezing
and generic-argument behavior.
