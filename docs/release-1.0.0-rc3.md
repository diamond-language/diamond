# d6 1.0.0-rc3

Release date: 2026-11-11. The third release candidate for d6 1.0 focuses on
correctness, smaller deployment artifacts and broader runtime verification.

## Smaller bytecode caches and executables

Type sets now store only their count, flags and used members, rather than
fixed 354-byte records. In Pheint, the bytecode cache shrank from 71 MB to
7.3 MB and the compiled executable from 76 MB to 11 MB. Cache format 21
invalidates old caches automatically; source and language behavior are unchanged.

## Correct JIT dispatch for captured instance variables

Methods on arrays, hashes and strings loaded through captured instance
variables in compiled blocks now use the appropriate native dispatch.
Previously, a block such as `size.times() do |i| @inboxes.push(i) end` could
raise `undefined method 'push' for Array`. The fix preserves compiled execution
and correct dispatch instead of relying on a whole-function fallback.

## Broader runtime verification

Ten cross-feature workflows cover JSON, collections, numeric boundaries,
control flow, regular expressions, module/subclass dispatch, SQLite recovery,
subprocess pipes, files and channel payloads. They run across interpreter/JIT,
major/minor GC stress, cold/hot bytecode caches, and standalone executables.
`make test-runtime-workflows` runs the full 66-execution matrix; ordinary and
release unit targets include its 60-execution subset.

## Deployment and documentation

Registry deployments build at `-O3`, enable the opt-in JIT, and accept
`--revision=REF` to select a merged release tag. The documentation site fixes
compatibility-link rendering. Installation examples and marketing downloads
point to rc3; the browser REPL is rebuilt from the release source.

No bundled cuts changed. Existing published cut versions remain immutable and
are verified against the public registry rather than republished. See the
[changelog](../CHANGELOG.md) for the complete changes, the
[runtime coverage inventory](../tests/RUNTIME_WORKFLOWS.md) for the new tests,
and the [production record](../applications/registry/deploy/PRODUCTION.md) for
actual registry deployments and backups.
