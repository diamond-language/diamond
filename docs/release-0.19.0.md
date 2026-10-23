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

Record final local and CI results here before tagging. The candidate must pass
the native and integration suites and the full CI matrix on its final head.
The browser module must pass `make test-wasm` with wasi-sdk 34, and the website's
example chips must run through the exact wrapper and module staged for deployment.
The intentionally failing Types chip must report the type error.

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
