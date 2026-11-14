# Main ef4cb8f0 deployment

On 2026-11-14 UTC every production target moved to Diamond main
`ef4cb8f02e9c0e2dae5d0f16eb4b397eaae024ba`: 1.0.0-rc5 plus the negative-literal
binding (#567) and the IO.poll trapped-signal fix (#569, also #570 on `1.0`). The
signal fix is why the applications moved: a trapped SIGTERM that landed just before
`IO.poll` began waiting went unhandled in JIT-compiled code, which showed up as an
occasional 10-second stop timeout in Pheint's test suite.

No cut changed, so nothing was published and no publishing credential was issued.

| Target | Deployed source | Verification | Rollback |
|---|---|---|---|
| Registry | `tools/deploy_registry.sh --force --revision=ef4cb8f0...` after its dry run | Upgraded from `b83d3882`; health ok; all 20 public cuts resolved, digest-checked, loaded and reinstalled from an unchanged lock | Verified snapshot `~/diamond-registry-backups/cutbackup-2026-11-13-230532-pre-ef4cb8f0` (off-host) |
| SkindicateV3 | App `79e6042` (PR #19), main CI run 43568805806 bundle | Running binary SHA-256 `bb95865c...` matched the bundle; homepage, browse, login, skin and profile pages 200 | `/root/skindicate-head-ef4cb8f0-rollback` |
| Pheint | App `e19f384` (PR #47, now built like SkindicateV3 on the local VM) | Running binary SHA-256 `ef054408...` matched the release; deploy script's health, app, schema, query and header checks passed (64 MB) | `/var/backups/pheint/20261114T070509Z` |
| Marketing | Marketing `fa9fe25` (PR #37) | WASM rebuilt from `ef4cb8f0`, `make test-wasm`; live digest `e2a19e48...` matched; only that file changed | `/root/diamond-head-rollback-20261113/marketing.tar.gz` |
| Docs | Main `ef4cb8f0` | A fresh build matched the live pages byte for byte, so nothing was uploaded | — |

Each application's own deployment notes carry the full digests: SkindicateV3's
`DEPLOYMENT.md` and Pheint's `docs/deployment.md`.
