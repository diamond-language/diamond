# rc6 publication and deployment record

Completed 2026-11-15 UTC (2026-11-14 Pacific). The release tag `v1.0.0-rc6` identifies
`3e4554d9`, the merge of [PR #583](https://github.com/diamond-language/d6/pull/583) into
`1.0`, which passed every check before merge. The matching preparation landed on `main` in
[PR #584](https://github.com/diamond-language/d6/pull/584).

The site and registry addresses changed between the tag and the deployment: the former
site domains were removed and GitHub links now point at `diamond-language/d6`
([#585](https://github.com/diamond-language/d6/pull/585), backport
[#586](https://github.com/diamond-language/d6/pull/586)), and cuts.rolld6.codes became
the registry's canonical address and facet's default
([#587](https://github.com/diamond-language/d6/pull/587), backport
[#588](https://github.com/diamond-language/d6/pull/588)). The browser runtime, docs and
registry pages were therefore built from `1.0` at `0b310af1`, which is the tag plus those
four merges.

## Published packages

[Release workflow 43675896933](https://github.com/diamond-language/d6/actions/runs/43675896933)
built, installed and smoke-tested all four packages before publishing
[d6 1.0.0-rc6](https://github.com/diamond-language/d6/releases/tag/v1.0.0-rc6):

| Asset | SHA-256 |
|---|---|
| `d6-lang-1.0.0-rc6-1.fc44.aarch64.rpm` | `836b2c6442dd32c93652f1057bbdeb77cd896d362204aa31edcd1b52030f6469` |
| `d6-lang-1.0.0-rc6-1.fc44.x86_64.rpm` | `c6b616cb42bda30dc09e34db9a7a0857b80a61b958c30649c092f6bdfa4b4247` |
| `d6-lang_1.0.0-rc6-1_amd64.deb` | `18fab2e79ad22a8e69f9cabbb73abc58ca560be43dc080d3f4b9de4294cbae24` |
| `d6-lang_1.0.0-rc6-1_arm64.deb` | `b27d545fef58cf9eab7468882c309fba625fe05cd34c44d877b32829ed39fc98` |

## Cuts

All 20 public cuts were published with their `##` doc-comment patch releases (see
`applications/registry/deploy/PRODUCTION.md` for versions and digests). One 30-minute
credential (id 42, a `publish:` scope for each of the 20) was revoked afterwards, and
`tools/verify_registry_launch.d6` passed for all 20. A verified snapshot was taken first; the registry
deploy below pruned it in favour of its own, newer one.

## Site, docs and registry pages

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `9dcbe9a` (green PR #44) | Fresh WASM from `1.0` `0b310af1` (`1450f80beead47ad85d67d83b6dd9cfe3179a47c3948b845c491b173ec62ed47`); `make test-wasm`; nine REPL checks; live WASM digest, type and `no-cache`; rc6 on the homepage |
| Docs | `1.0` `0b310af1` | 62 pages; 5,435 link checks; live index, rc6 release page and cut pages |
| Registry pages | `catalog.html`, `cut.html` from `1.0` `0b310af1` | Live on cuts.rolld6.codes and the former address; no old domain or repository links |

The previous document root (including /docs) is in
`/root/d6-rc6-rollback-20261115/site.tar.gz`; the previous registry pages are in
`/root/registry-pages-rolld6-20261115/`. nginx and TLS were not touched by this deployment.

## Production applications

On 2026-11-15 UTC all three moved from main `ef4cb8f0` to main
`3d8eb74cab0f35accf5fbe3ac76475034b1da24e`: the rc6 runtime plus facet's cuts.rolld6.codes
default (main and `1.0` differ only in `docs/roadmap.md`).

| Target | Deployed source | Verification | Rollback |
|---|---|---|---|
| Registry | `tools/deploy_registry.sh --revision=3d8eb74c...` after its dry run (upgrade check: REQUIRED) | Health ok; `DIAMOND_JIT=1` kept; all 20 public cuts resolved, digest-checked, loaded and reinstalled | Verified snapshot `~/diamond-registry-backups/cutbackup-2026-11-15-141503-pre-3d8eb74c` (off-host) |
| SkindicateV3 | App `549e6d9` (PR #21), main CI run 43769152192 bundle | Running binary SHA-256 `be5b81b4...` matched the bundle; homepage, skin, download, login and profile pages; no errors logged | `/root/skindicate-rc6-rollback` |
| Pheint | App `45f7b29` (PR #49), built on the local VM | Running binary SHA-256 `725faa60...` matched the install; deploy script's eight outside checks passed (64 MB) | `/var/backups/pheint/20261115T224242Z` |
