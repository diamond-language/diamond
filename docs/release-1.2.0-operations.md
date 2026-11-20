# 1.2.0 publication and deployment record

Completed 2026-11-20 UTC. The release tag `v1.2.0` identifies `6372c574`, the merge of
[PR #621](https://github.com/diamond-language/d6/pull/621) into `main`. The PR head passed all 43
checks and was current with `main`, so the merge commit has the same tree as the tested head.
`main` then moved to 1.3.0-dev ([#622](https://github.com/diamond-language/d6/pull/622)). Like
1.1.0, it was cut from `main`; the `1.0` branch stays at 1.0.1.

1.2.0 adds the builtin API reference, `MatchData`, `Time.strptime`, `Dir.mktmpdir` and
`Dir.delete_tree`, `URI`, `STDOUT`/`STDERR`/`STDIN`, `facet new graphql` and the csv and toml cuts.
Two behaviors change: a closed output pipe ends a program quietly with status 141, and plain
`facet update` moves every cut to its newest compatible release. See
[the release notes](release-1.2.0.md).

## Published assets

[Release workflow 44776948588](https://github.com/diamond-language/d6/actions/runs/44776948588)
built, installed and smoke-tested all eight packages before publishing
[d6 1.2.0](https://github.com/diamond-language/d6/releases/tag/v1.2.0). GitHub marks it Latest.

| Asset | SHA-256 |
|---|---|
| `d6-1.2.0-linux-aarch64.tar.gz` | `5b089984e63bde3f92e51d3b4345ce51efc89e31d5d2cd5763c464c98b8869db` |
| `d6-1.2.0-linux-x86_64.tar.gz` | `fcfe1edf0de64235c0c09a513b3b26e0147be0fe88be9f70e088feae57af5e75` |
| `d6-lang-1.2.0-1.fc44.aarch64.rpm` | `07d6ebe28ce742205274d9734a04fc3515d0bbd16d565199bba7b894c010c981` |
| `d6-lang-1.2.0-1.fc44.x86_64.rpm` | `f8eb3c52b9c5f4a33084d0bb27537faf6fce6278d57c7ccd4d4b49a9f86793eb` |
| `d6-lang-1.2.0-1.fc45.aarch64.rpm` | `fc389ebbce050b8749390f50709bb87dedae92231308c5b5aef036b4896e8ded` |
| `d6-lang-1.2.0-1.fc45.x86_64.rpm` | `19833ee8eba55d98c15a43453bfae412e75be8516b59a8de651e8c045d758495` |
| `d6-lang_1.2.0-1_amd64.deb` | `967cf43718934265ab85f0a2d11e6acabccac66e0fc77ac835a881b91329d464` |
| `d6-lang_1.2.0-1_arm64.deb` | `a921a65c6b2ce3735c3e6dca775b49403f6a2bd9a2ef4405941c873d04a572c5` |
| `d6-vscode.vsix` | `35230ceb978b11d7687308d14d195233c7b27b29ae5d19c5e4c4930545b0b655` |

## mise and asdf

`mise ls-remote d6` through the asdf plugin lists 1.2.0. `mise latest github:diamond-language/d6`
gives 1.2.0 with `MISE_MINIMUM_RELEASE_AGE=0`, and bare `latest` will once the 24-hour hold passes.

## Cuts

csv 0.1.0 and toml 0.1.0 (credential 46), then active_record 0.21.4, arel 0.35.6, jobs 0.2.4 and
toml 0.1.1 (credential 47), were published on 2026-11-20, before the release (see
`applications/registry/deploy/PRODUCTION.md`). `tools/verify_registry_launch.d6` passed for all 22
public cuts against cuts.rolld6.codes after the tag, so nothing more was published.

## Site and docs

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `9592a7e` (green PR #51) | Fresh WASM from `v1.2.0` (`271bc2348de22eae29e25abfca614fbdfcd80d6be78b2515740ab669a3d4c9f4`), `make test-wasm`, the wasm REPL test and nine site REPL tests; live: 1.2.0 eyebrow, no 1.1.0 references, all six download links return 200, WASM digest with `application/wasm` and `no-cache` |
| Docs | `v1.2.0` | 99 pages; link check in PR #621's CI; live release page and the new `MatchData`, `URI`, csv and toml pages |

`RELEASE`, `REVISION` and `RUNTIME_REVISION` in the document root now say 1.2.0, `9592a7e` and
`6372c574`. The previous document root (with /docs) is in
`/root/d6-1.2.0-rollback-20261120/site.tar.gz`. nginx and TLS were not touched.

## Production applications

| Target | Deployed source | Verification | Rollback |
|---|---|---|---|
| Pheint | App `fb4b2df` (pheint #55: runtime pin to `v1.2.0`), built on the local VM | Source suite, prebuilt suite with and without the JIT, standalone smoke; running binary SHA-256 `509ae30d...` matched the build; the deploy script's eight outside checks passed (54 MB); an anonymous `me` query got `UNAUTHENTICATED` | `/var/backups/pheint/20261120T171514Z` |
| Registry | `tools/deploy_registry.sh --revision=v1.2.0` after its dry run, 2026-11-20 17:19 UTC (upgrade check: REQUIRED, the gremlin, http and mjolnir code it loads changed) | Health ok; `DIAMOND_JIT=1` kept; all 22 public cuts resolved, digest-checked, loaded and reinstalled | Verified snapshot `~/diamond-registry-backups/cutbackup-2026-11-20-091539-pre-6372c574` (off-host) |
| SkindicateV3 | App `93e8fc0` (skindicate-v3 #28: runtime pin to `v1.2.0`), main CI run 44780007560 bundle, 2026-11-20 17:16 UTC | Running binary SHA-256 `ae4ec907...` matched the bundle; `DIAMOND_JIT=1` kept; homepage, browse, login, skin, download and profile pages; no errors or 5xx logged (50 MB) | `/root/skindicate-1.2.0-rollback` |
