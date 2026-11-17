# 1.0.0 publication and deployment record

Completed 2026-11-17 UTC (2026-11-16 Pacific). The release tag `v1.0.0` identifies `91fdb43a`,
the merge of [PR #598](https://github.com/diamond-language/d6/pull/598) into `1.0`, which passed
every check before merge (one database-dialect job was rerun after a Docker port-setup flake,
[#600](https://github.com/diamond-language/d6/issues/600)). The matching preparation landed on
`main` in [PR #599](https://github.com/diamond-language/d6/pull/599), and `main` then moved to
1.1.0-dev ([#601](https://github.com/diamond-language/d6/pull/601)).

The runtime is that of 1.0.0-rc6 and rc7: between `v1.0.0-rc6` and `v1.0.0` only the version
string in `src/main.c` changed.

## Published assets

[Release workflow 43991334091](https://github.com/diamond-language/d6/actions/runs/43991334091)
built, installed and smoke-tested the packages and tarballs before publishing
[d6 1.0.0](https://github.com/diamond-language/d6/releases/tag/v1.0.0). It is the first release
created with the pre-release rule from [#596](https://github.com/diamond-language/d6/pull/596): a
plain version, so GitHub marks it Latest and `releases/latest` names it.

| Asset | SHA-256 |
|---|---|
| `d6-1.0.0-linux-aarch64.tar.gz` | `3d2b7ebdc33919ed24cdbe2ea212af54c28a73dc3e8316007cd2ce753ff77c66` |
| `d6-1.0.0-linux-x86_64.tar.gz` | `a253f075343d99f87dda3a22beca4d67a4b078bf413bb8b93edf03bbd9d4d7f9` |
| `d6-lang-1.0.0-1.fc44.aarch64.rpm` | `5c85caff48de901f630a30fb737f52a505c4f410badf62a8f887b78a69192cf8` |
| `d6-lang-1.0.0-1.fc44.x86_64.rpm` | `a642a6f52f513e276b14719f372d0c1bc4377775fc46d4ed90c09bfe68938f7d` |
| `d6-lang_1.0.0-1_amd64.deb` | `a43b0f9ec0e078daf32da95cdc233995d037b510471b30cd52f9dcfa06285d05` |
| `d6-lang_1.0.0-1_arm64.deb` | `ebd99123400a3510b287e7af628acd12053405167afbfd2bfbc3042fa4f66c4a` |
| `d6-vscode.vsix` | `4bb439ce6670f2a53f9ecec03a33937826dd970d5ac31e64867d666d1cd88868` |

The VS Code extension stayed at 0.8.0 (published with rc7), so the Marketplace step skipped it.

## mise and asdf

`mise latest d6` through the asdf plugin (with its pre-release fix, asdf-diamond #2) gives 1.0.0.
`mise latest github:diamond-language/d6` gives 1.0.0 once mise's `minimum_release_age` hold has
passed for the release; with the hold off, `mise use -g github:diamond-language/d6@latest`
installed 1.0.0 and ran it.

## Cuts

None changed in 1.0.0. `tools/verify_registry_launch.d6` passed for all 20 public cuts against
the tag's inventory before and after the registry deploy, so nothing was published and no
credential was issued.

## Site, docs and registry pages

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `0156256` (green PR #46) | Fresh WASM from `v1.0.0` (`5b077ab7c764e83da4dc66b524f0e59cee5da5eb30df5c936e04404e12e82770`); `make test-wasm`; nine REPL checks; live WASM digest, type and `no-cache`; 1.0.0 and the Marketplace link on the homepage; all four download links |
| Docs | `v1.0.0` | 65 pages; 5,712 link checks; live 1.0.0 release page, getting started, compatibility and cut pages |
| Registry pages | Unchanged | `catalog.html` and `cut.html` at the tag were already byte-identical to the live pages |

The previous document root (including /docs) is in
`/root/d6-1.0.0-rollback-20261117/site.tar.gz`. The marketing repository's
`deploy/release-1.0.0.md` has the site details. nginx and TLS were not touched.

## Production applications

On 2026-11-17 UTC all three moved from main `3d8eb74c` (the rc6 runtime) to the v1.0.0 tag,
`91fdb43a2457d5c6d32947d7b271003d45a6a1bb`.

| Target | Deployed source | Verification | Rollback |
|---|---|---|---|
| Registry | `tools/deploy_registry.sh --force --revision=v1.0.0` after its dry run (upgrade check: OPTIONAL, version string only); needed [#602](https://github.com/diamond-language/d6/pull/602)/[#603](https://github.com/diamond-language/d6/pull/603) so `--revision` accepts the `1.0` branch | Health ok; `DIAMOND_JIT=1` kept; all 20 public cuts resolved, digest-checked, loaded and reinstalled | Verified snapshot `~/diamond-registry-backups/cutbackup-2026-11-16-174019-pre-91fdb43a` (off-host) |
| SkindicateV3 | App `df65d07` (PR #23), main CI run 43992624341 bundle | Running binary SHA-256 `44ca0a96...` matched the bundle; homepage, browse, skin, download, login and profile pages; no errors or 5xx logged | `/root/skindicate-1.0.0-rollback` |
| Pheint | App `ca504f6` (PR #51), built on the local VM | Running binary SHA-256 `81323b25...` matched the build; deploy script's eight outside checks passed (64 MB) | `/var/backups/pheint/20261117T011424Z` |
