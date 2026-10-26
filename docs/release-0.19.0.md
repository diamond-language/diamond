# Diamond 0.19.0 release record

Release date: 2026-10-23. This document records verification and migration.
Production deployment and cut-publication outcomes are recorded in the
[registry production record](../applications/registry/deploy/PRODUCTION.md).
The [changelog](../CHANGELOG.md) contains the complete user-visible changes
since 0.18.0.

## Production outcome

- Diamond **v0.19.0** was published on 2026-10-23 at
  `961519af64c36a27a3efccbea5d71248e7678e28`, after all 29 final checks passed.
- The registry runs that revision and starts `app.d6`. Its 19-cut publication
  matches the frozen inventory, and legacy immutable artifacts remain usable.
  Credential 29 was revoked and its secret file removed.
- `getdiamond.codes` serves marketing revision
  `e1ff744d1dd5f39b98231b0d268343a431034a37`, including the `.d6` examples and
  Mjolnir `^0.3.1` installation. `/wasm/` redirects to `/#repl`;
  `dilang.tech` retains its path/query redirect. Live page, script, stylesheet,
  image, and WASM bytes match the merged source. All nine REPL checks passed
  against the downloaded live assets. The WASM SHA-256 remains
  `d7fa1e3f8c2f94522be70cab40fb1353c74bef08e2bac24ca32ddaa227fc0bc5`.
- SkindicateV3 runs app revision
  `158bf4138a4e060367541f9c050bdd69edc3565c`, from its verified Ubuntu build.
  Its executable SHA-256 is
  `3523098cf5fc47a37a4f52255d34af937f6fad88fe3981ea2ebafcd72c4d67cb`.
  The bundle pins runtime `02e8b7de`; its compiler, native runtime, prelude,
  and cut sources are identical to the released tag. The later tag changes
  release/operator tooling and documentation. The running executable identity
  and public HTTPS home/login/signup pages were verified after restart.
  There was no schema migration or V2 cutover.

Rollback bundles and both database snapshots were verified on the server and,
with operator approval, copied to private local storage for restore checking.
The publication credential and temporary payload were removed; the build VM
was shut down. The registry production record contains the detailed audit.
No browser surface was available: HTTP, exact asset identities, and live WASM
execution were checked; visual layout and interactive keyboard behavior were
not independently exercised in a browser.

## Release highlights

- `.d6` is the default and preferred source extension; new caches use `.d6c`.
  Existing `.di` sources, their `.dic` caches, and published legacy cuts remain
  supported. This avoids the collision with D interface files in editors.
- `Channel.new(0)` transfers a value directly to a receiver and waits for it
  to be taken. Send-side channel selection still requires positive capacity.
- Supervised stdin reads can be interrupted. Later REPL submissions retain
  printed output. Exhaustiveness checks handle object patterns on high class IDs.
- Language-server receiver inference and UTF-16 positions are improved.
  Runtime, middleware, database, and package fixes are detailed in the changelog.

## Migration

Start new programs as `program.d6`; rename older source files when convenient.
An extensionless import tries `.d6` before `.di`. For example:

```ruby
# helper.d6
def answer() = 42
```

```ruby
# helper.di
def answer() = 99
```

<!-- doc-check: skip -->
```ruby
# main.d6, with both helpers beside it
require "helper"
answer()  # => 42
```

An explicit `require "helper.di"` selects the legacy file. If `helper.d6` is
absent, `require "helper"` falls back to `helper.di`. See
[source files](core-syntax.md#source-files-and-imports) for the full behavior.
The VS Code extension associates `.d6` automatically; legacy files can be
opened in Diamond language mode manually.

The 26 source cuts start new minor lines because older runtimes do not discover
`.d6` entry points. Update the runtime before adopting those cuts and coordinate
dependent ranges. Repository versions are not evidence that an artifact has
been published. Existing immutable registry releases remain installable.
The 18-cut seed has its own [reviewed inventory](registry-launch-inventory.json).
The six withdrawn cuts remain excluded. The 0.19 publication cohort updates
the 19 already-public cut names: the 18-cut selection plus Mjolnir 0.3.1.
Its exact artifact identities are frozen in the
[publication inventory](registry-0.19-inventory.json). Cancellation remains
source-only; this release does not introduce another public cut name.

## Validation and remaining release steps

Local verification passed on 2026-10-23:

- `make test`: 1,910 tests, including the 0.19.0 version assertion.
- `make test-integration`: exit 0, including Gremlin with the stderr fix,
  the reviewed 18-cut inventory, package/application tests, editor/cache tests,
  and self-hosted smoke checks.
- A fresh wasi-sdk 34 build and `node tests/wasm_repl_test.mjs`: passed.
- The website's exact module/wrapper and six chips: passed, including the
  intentional Types error, repeated output, and receiver-aware completion.
  Native examples and published Mjolnir 0.2.4/Arel 0.34.6 installation and
  examples passed. The marketing PR's nine REPL tests also passed in CI.
- Documentation/site links and anchors: 254 local documentation targets and
  45 website links checked without a broken target; JavaScript syntax checks passed.

The original preparation passed all 29 CI checks and merged in PR #300 at
`02e8b7de`. Marketing preparation merged at `331b24e`. SkindicateV3's bundle
at `158bf41` pins the verified 0.19 runtime at `02e8b7de` and passed both
interpreted and compiled test suites on Ubuntu 26.04. The finalization PR #301 added
the release date and the registry's operator-script/systemd transition, passed
all 29 checks, and merged at `3679c49e` before publication.

The documentation and website review covers links and anchors, source/cache
suffixes, installation commands, registry availability, package commands,
runtime claims, keyboard accessibility, and REPL examples. Live HTTP checks
found `/wasm/` returned 403; the marketing nginx candidate redirects it to
`/#repl`. The canonical root and WASM asset were available, and `dilang.tech`
preserved paths and queries when redirecting to `getdiamond.codes`.
No browser surface was available in this session, so visual and interactive
browser verification remains an explicit pre-deployment check.

The isolated Gremlin shutdown failure from CI run `39928947356`, job
`120332146315`, is accepted for this release by the owner. Its cause is unknown;
the diagnostic rerun and subsequent full CI passed. Keep its investigation
separate from the release gate and preserve the original evidence rather than
weakening assertions. The [investigation memo](internal/gremlin-shutdown-flake.md)
records the matching image/package telemetry, unsuccessful reproduction probes,
and the confirmed stderr redirection bug. That diagnostic bug is fixed; the
underlying flake remains unexplained.

Release order: verify backups, pass the final CI matrix, tag/publish the verified
commit, upgrade the registry runtime, publish and verify the frozen 19-cut
cohort, and deploy marketing and SkindicateV3 with their recorded rollback
bundles. Restore the previous registry service unit as well as its release
symlink on a failed upgrade: its legacy command names `app.di`, while 0.19
uses `app.d6`. The backup downloader supports the installed operator script
before this transition. Temporary publication credentials must be revoked and
their local files removed after publication.
