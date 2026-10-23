# Diamond 0.19.0 release preparation

Status: candidate preparation, 2026-10-23. No `v0.19.0` tag, public release,
site deployment, registry deployment, or cut publication is recorded here.
The [changelog](../CHANGELOG.md) contains the complete user-visible changes
since 0.18.0.

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
The six withdrawn cuts remain excluded; preparation does not authorize publishing them.

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

The marketing preparation is merged at `331b24e`, without a deployment.
The runtime release PR must pass the full CI matrix on its final head before
merging; record that result in the PR before tagging.

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

After the release PR is green and merged: finalize the release date, tag the
verified commit, publish the release notes, and deploy the matching marketing
revision and WASM. Verify canonical/legacy URLs and every example in a real
browser. Registry runtime upgrades and migrated cut publication use their own
backup, staging, and publication procedures; they are separate operations.
