# Publishable cut contract

Status: proposed contract, 2026-09-21. No registry or publish command exists
yet. Current behavior and compatibility details are in [packages.md](packages.md).

`facet check <cut-directory>` now performs the first local preflight. It checks
the canonical name and directory, strict version spelling, summary, nonempty
license declaration, data-only runtime dependency ranges, and regular
`diamond.cut`, `README.md`, `LICENSE`, and `lib/<name>.di` files. It rejects
unknown manifest keys. It does not yet verify SPDX expressions, inspect the
full artifact file list, find undeclared imports, or create an archive. Passing
this check is necessary for a future publish, but is not a publish operation.

## Identity and layout

A cut has one canonical, case-sensitive name and one public entry point.
Initially names use lowercase ASCII letters, digits, and underscores, start
with a letter, and are at most 63 bytes. Reject names that differ only by
ASCII case, reserved names, path separators, dots, percent encoding, Unicode
lookalikes, or trailing separators. A future namespace scheme must be designed
before accepting scoped names. This narrow rule makes paths and registry names
unambiguous on case-insensitive filesystems.

Bundled source lives at `packages/<name>/` inside Diamond's repo. An external
cut may live at the root of its own repo. In either case, the publishable root
has the same layout:

```text
<cut root>/
  diamond.cut             required for publication
  README.md               required for publication
  LICENSE                 required for publication
  lib/
    <name>.di             required public entry point
    <name>/...            optional implementation files
  test.sh                  optional development test runner
  tests/                  optional development tests
```

`facet pack <path>` should create an artifact from this root, not from its
parent repo. The archive contains paths relative to the cut root, including
`diamond.cut`, `README.md`, `LICENSE`, and runtime files. Tests, VCS files,
lockfiles, caches, build output, credentials, and installed `cuts/` are excluded
by default. The pack command prints the final file list and artifact digest.
Explicit includes may be added later, but cannot override safety exclusions.

An installed cut occupies `cuts/<name>/` within the consuming project, with
`cuts/<name>/lib/<name>.di` as its entry point. Installation never writes into
Diamond's own `packages/` tree. A version is selected per name for the whole
application because the compiler currently uses a flat program namespace.

## Manifest

The future publishable `diamond.cut` is **data**, using a restricted literal
grammar: one Hash literal with String keys and String, Hash, Array, Boolean,
or integer literal values as needed by the schema. No calls, interpolation,
variable references, `require`, or other executable expressions. Duplicate
keys and unknown top-level keys fail validation. Metadata is bounded in size.
The registry reads this data, never executes it. Before publishing, `facet`
must parse the same bytes with the same rules locally.

The first registry schema should require:

```ruby
{
  "name": "greeter",
  "version": "1.2.0",
  "summary": "Small greeting helpers",
  "license": "MIT",
  "dependencies": {"logger": "^0.4.0"}
}
```

`name` matches the root directory and entry point. `version` is strict SemVer
2.0.0, canonical without a leading `v`; build metadata needs an explicit
identity policy before publication is allowed. `summary` is single-line plain
text of 1-160 bytes.
`license` uses an SPDX expression or an explicit `LicenseRef` convention to be
specified. Runtime dependencies are name-to-range mappings; no floating Git
branch, arbitrary URL, or local path is allowed in a published cut's runtime
graph. Optional homepage, source, documentation, and issue links are display
metadata only and must never determine where `facet` fetches package bytes.
Compatibility fields for supported Diamond versions and external system
libraries need a schema before packages that require them are published.

The manifest declares runtime dependencies only. Development dependencies
belong in a project-local development manifest or test configuration, not in
the published runtime graph. A cut must explicitly `require_cut` the cuts it
uses; dependency declaration does not import them. The publisher validates
that the manifest and archive identity agree, and rejects undeclared runtime
imports where static inspection can find them.

Published library dependency ranges should cover versions actually tested,
without pinning every dependency to a single release. The consuming
application's `facet.lock` records the exact resolved graph. Published library
lockfiles are not used to resolve a consumer's graph.

## Release and install invariants

- The tuple (registry source, name, version) identifies exactly one immutable
  artifact and manifest. A failed or yanked release cannot be republished with
  different bytes under that tuple.
- `facet` verifies the artifact's cryptographic digest from the lockfile before
  extraction. Extraction rejects absolute paths, `..`, escaping symlinks and
  hardlinks, device files, duplicate entries, and case-folding collisions.
- Resolution uses registry metadata and never runs cut code. Installation does
  not run lifecycle scripts. Loading a cut executes its library code only when
  the application requests it.
- New resolution skips yanked releases. An already locked, verified release
  remains installable unless an exceptional security or legal takedown removes
  it. Such a removal must produce an explicit error, not silent substitution.
- Publishing requires ownership of the name and authenticated intent. Owner
  changes and release actions are audited. A publisher can deprecate or yank a
  bad release; ordinary deletion is not a package maintenance operation.
- The client pins the registry source for each name. If two requesters mean
  different sources or incompatible ranges, resolution fails with both paths
  through the dependency graph. It does not fall back to another registry.

## Compatibility with the current implementation

Current `facet` accepts Git dependencies shaped like `{"git": URL,
"version": RANGE}` and derives available versions from Git tags. Both `facet`
and `require_cut` parse metadata without executing it. Current bundled
manifests have only `name` and `version`; their runtime dependencies and
release metadata are incomplete.
These files are not yet publishable under this contract.

The migration now uses a dedicated data-only parser. Preserve reading existing
literal manifests and Git source specs. Add registry source specs and lockfile
digests without reinterpreting an old Git lock as a registry lock. The flat
`cuts/<name>/` compatibility fallback
can remain for manually installed legacy cuts; published artifacts use `lib/`.
