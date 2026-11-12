# Application and package maintenance review — 2026-11-12

This was a focused source cleanup with existing suite coverage, not a security
audit or a release. The runtime stays on rc4; nothing in `src/` changed.

## Outcome (shipped 2026-11-12)

- Package cleanup merged as #550 (`b83d3882`, 38 checks green).
- database_config 0.2.3, logger 0.5.3, network_safety 0.2.3 and rack 0.2.3
  published with a scoped, since-revoked credential; all 20 public cuts verified.
- App cleanups merged and deployed: Pheint #46 (`a7bbcee`), SkindicateV3 #17
  (`5e8f5de`). Both passed the external checks.
- Registry redeployed at `b83d3882` (needed for logger 0.5.3).
- Verified pre-deploy backup: `/root/diamond-maintenance-20261112` on the host,
  with an off-host copy in the operations workspace.

Production records: `applications/registry/deploy/PRODUCTION.md`, Pheint
`docs/deployment.md`, SkindicateV3 `DEPLOYMENT.md`.

## Application cleanup

Changes are committed on `review/app-cleanup` in each application repository:

| Repository | Commit | Change |
| --- | --- | --- |
| Pheint | `d87111c` | Filter, count and paginate notifications in SQL instead of loading and filtering rows in the application. Regression checks cover unread pagination and an empty mark-read ID list. |
| SkindicateV3 | `d036498` | Derive follower/following query columns from one argument; correct the README's runtime version to rc4. |
| example-apps | `2b271ce` | Use `File.write` for Tasks and KVD temporary-file saves, retaining rename-based replacement. |

Verification completed:

- Pheint: `./test.sh`, with the JIT off and with `DIAMOND_JIT=1`.
- Pheint frontend: `npm run build`, `npm test` (47 passed, 9 skipped), and
  `npm run test:player` (the remaining 9 passed). Browser end-to-end tests were
  not run; frontend source was unchanged.
- SkindicateV3: `test/run_all.sh`, including HTTP/WebSocket, worker concurrency,
  ingestion, migration, backup and deployment-preflight checks. A newly built
  standalone app was not tested; the cleanup is not a deployment bundle.
- Separate example-apps: Tasks (9 tests), KVD (10), Stash (9), and the resource
  monitor's collector tests.
- Diamond bundled examples: `make test-examples` and `make test-example-apps`.

The pre-existing Pheint README benchmark edit was left uncommitted and untouched.
Skindicate V2 (`skindicate.dia`) remains the legacy reference and was not changed.

An initial Stash run could not bind a local socket in the sandbox; it passed
with localhost access. One Pheint run was interrupted by a concurrent rebuild
of its shared local interpreter; both complete engine runs were repeated
successfully after the rebuild. Package builds use a separate worktree.

## Package cleanup

- Logger 0.5.3 formats the JSON timestamp only for JSON output; text logging
  no longer computes and discards that extra timestamp.
- database_config 0.2.3 delegates its file-read helper to `File.read`.
- Rack 0.2.3 uses `File.read` for static responses, keeping the existing
  fallback behaviour and closing the native file on read failure.
- network_safety 0.2.3 removes obsolete comments claiming the runtime has no
  DNS resolution, bitwise operators or usable constants. URL checks remain
  string-only; DNS rebinding protection has not been added.

The four patch versions and deterministic candidate registry inventory are
updated together. Existing caret dependency ranges accept these patch releases.
The candidate archives are local only; the live registry remains unchanged.
The focused package suites and registry reproducibility/inventory checks pass.
`make test` passed (2,210 tests and 60 runtime workflow executions), and the
complete `make test-integration` run exited 0, including package suites,
standalone runtime workflows (66 executions), bundled examples, language-server,
debugger, REPL, cache and self-hosted parser smoke checks. No sanitizer or
external database-dialect matrix was run for these Diamond-only cleanups.

The resource dashboard remains undeployed on its existing development branch.
Its README records remaining deployment/overhead checks and the requested
nginx 5xx monitoring scope.
