# Cut registry plan

Status: design plan, 2026-09-21. This document describes intended behavior;
`docs/packages.md` describes the current `facet` and `require_cut` behavior.

Diamond calls a reusable package a **cut**. Keep `facet` as the package manager,
`diamond.cut` as the manifest, `require_cut` as the import form, and `cuts/` as
the project-local installation directory. A registry may have its own brand
later without renaming these interfaces. A rename now would affect source code,
manifests, paths, documentation, and existing projects.

## Delivery order

1. Specify the publishable cut contract in [cut-contract.md](cut-contract.md).
   Make manifest parsing data-only and validate package contents before any
   hosted release. Keep local Git dependencies working during migration.
   The parser, `facet check`, and deterministic `facet pack` are implemented.
2. Add immutable archives, content digests, registry source identity, and
   checksums to the lockfile. `facet pack` and `facet verify` cover local
   artifacts; new Git locks record `source: git` and old Git locks still load.
   Registry lock records now preserve and validate source, version, digest, and
   artifact size. A locked install now downloads the digest-addressed archive,
   enforces its size and digest, verifies its contents, and stages extraction
   before replacing the installed cut without consulting mutable indexes or
   tags.
3. Resolve registry versions and transitive dependency metadata without
   downloading or executing package code. Use one version per cut name for a
   program. Preserve useful conflict explanations and deterministic selection;
   make `facet update` the explicit route to newer versions.
   Registry metadata resolution is implemented. Prefer retaining a compatible
   version from an existing lock during a future targeted-update refinement.
4. Build a minimal registry with name ownership, authenticated publishing,
   immutable version records, artifact storage, yanking, audit logs, and a
   read-only index API. The initial SQLite schema and content-addressed blob
   store and authenticated publish transaction are implemented in the `registry`
   cut, including archive verification, orphan recovery, and atomic audit records.
   The HTTP read/publish service now lives in `applications/registry`, with a
   real HTTPS facet integration test. Audited yank/unyank/takedown endpoints and local credential lifecycle commands
   are implemented. Owner management and production operations remain. Publish from a selected `packages/<name>/` directory,
   not from the monorepo root.
5. Add a web interface for search, package pages, documentation, owners,
   versions, and release status. Keep the API usable without the web interface.
6. Migrate bundled cuts in small dependency-connected groups. Declare their
   runtime dependencies, test installation from artifacts in CI, then publish.
   All 23 bundled cuts now have release metadata, license files, and declared
   dependencies. `tools/install_local_cuts.sh` stages verified archives for
   local projects, including Skindicate. Registry publishing remains.

## Decisions to carry through implementation

- `packages/<name>/` is the source location for bundled cuts. Its contents
  become the artifact root. Consumers install into `cuts/<name>/`. The registry
  and cache use content-addressed storage; they are not alternate source roots.
- A library declares compatible runtime dependency ranges; an application
  commits `facet.lock` with exact versions and digests. A library's own lockfile
  may help its tests but must not constrain its consumers.
- Registry identity is source plus cut name. A name cannot silently switch
  from a configured private source to a public source, or between Git and the
  registry. The lock records source identity for every dependency.
- Published `name` and `version` are validated against the upload request and
  cannot be overwritten. Yank hides a version from new resolution but does not
  change its bytes or break an existing verified lock. Exceptional takedown is
  an explicit operator action with an audit trail.
- Publishing uses narrowly scoped credentials. Plan for short-lived trusted
  publishing and provenance rather than requiring long-lived CI secrets.
- Avoid install-time scripts and implicit native build steps in the first
  artifact format. A package can declare external requirements, but `facet`
  must not run arbitrary package code during discovery, resolution, or install.
- Keep a manual path to inspect the exact artifact before publish. Publishing
  should show the included file list and reject secrets, symlinks escaping the
  root, unsafe archive paths, duplicate paths, and unexpected generated files.

## Work still to design

The [version 1 registry protocol](registry-protocol.md) now specifies the read API,
archive identity, lockfile shape, publishing boundary, and resolver rules.
Read and publish endpoints are implemented and tested through HTTPS. Operations
still need an owner, backups, incident response, abuse handling, and retention
rules before a public service is launched. Owner-management endpoints remain to be implemented.

## Lessons informing the contract

The split between a library manifest and an application's lockfile follows
[RubyGems' guidance](https://guides.rubygems.org/gemfile-and-gemspec/). Exact
artifact digests follow [Bundler's checksum guidance](https://guides.rubygems.org/security/)
and [npm's lockfile integrity field](https://docs.npmjs.com/files/package-lock.json/).
Immutable version identifiers and restrained removal follow
[npm's unpublish policy](https://docs.npmjs.com/policies/unpublish/). Short-lived
publishing credentials and provenance follow
[npm's trusted publishing model](https://docs.npmjs.com/trusted-publishers/)
and [RubyGems' security guidance](https://guides.rubygems.org/security/).
These are design inputs, not claims that Diamond already implements them.
