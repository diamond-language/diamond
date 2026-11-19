# 1.1.0 publication and deployment record

Completed 2026-11-19 UTC (2026-11-18 Pacific). The release tag `v1.1.0` identifies `8fc5a2d5`,
the merge of [PR #610](https://github.com/diamond-language/d6/pull/610) into `main`. The PR head
passed all 43 checks and was current with `main`, so the merge commit has the same tree as the
tested head. `main` then moved to 1.2.0-dev
([#611](https://github.com/diamond-language/d6/pull/611)). 1.1.0 is the first minor release of
1.x and was cut from `main`; the `1.0` branch stays at 1.0.1 for any further 1.0.x patches.

1.1.0 adds `File.embed`, schema-first GraphQL apps (graphql 0.3.6), `--version` for built binaries
and `respond_to?` on modules and classes. See [the release notes](release-1.1.0.md).

## Published assets

[Release workflow 44425137598](https://github.com/diamond-language/d6/actions/runs/44425137598)
built, installed and smoke-tested all eight packages before publishing
[d6 1.1.0](https://github.com/diamond-language/d6/releases/tag/v1.1.0). GitHub marks it Latest.

| Asset | SHA-256 |
|---|---|
| `d6-1.1.0-linux-aarch64.tar.gz` | `848428386b5b014b5a09a21ff9c75f9dd1f42304f7b1703c1ffa79b74f0ad71e` |
| `d6-1.1.0-linux-x86_64.tar.gz` | `6389dff7144e45dac7d9e0f2e6e5345b7c0efa7b9bf1150855db9607f2597fc4` |
| `d6-lang-1.1.0-1.fc44.aarch64.rpm` | `e51ccbfc50a4d7e5e95ee20eeece7f02d4c5b66dd4904f20557d891cde75b0d9` |
| `d6-lang-1.1.0-1.fc44.x86_64.rpm` | `2d45906454c693ef48c7791d4111ac41ba30c34fa773cbff3a9ef1289baaf65c` |
| `d6-lang-1.1.0-1.fc45.aarch64.rpm` | `667f32f0b28fee11955c68b71d69bcda47e2ed2e803b5a56f636ea7b1d225571` |
| `d6-lang-1.1.0-1.fc45.x86_64.rpm` | `bf4f2ab8d22f181cecc9497b18903624f31e64993c20c2f722eb43ea610cd9bc` |
| `d6-lang_1.1.0-1_amd64.deb` | `f86ee076ee4b07f03b3d991510de5ac93b500353bcf30d79d3cb617e1a22bbd2` |
| `d6-lang_1.1.0-1_arm64.deb` | `719b5395417e94b5fe37fe89c0935728e71b3f88f99c5a57f517ee68395f5adb` |
| `d6-vscode.vsix` | `ba1591646ca54321cceb2824f59ed9970d48efd120167520da44e7228c4c54f1` |

## mise and asdf

`mise ls-remote d6` through the asdf plugin lists 1.1.0. `mise latest github:diamond-language/d6`
gives 1.1.0 with `MISE_MINIMUM_RELEASE_AGE=0`, and bare `latest` will once the 24-hour hold passes.

## Cuts

graphql 0.3.6 was published on 2026-11-18, before the release (see
`applications/registry/deploy/PRODUCTION.md`). `tools/verify_registry_launch.d6` passed for all 20
public cuts against cuts.rolld6.codes, so nothing was published and no credential was issued.

## Site and docs

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `89738b7` (green PR #50) | Fresh WASM from `v1.1.0` (`a5a64e87196f998e3bff62978fd895a50c39b383f314515db06acd0e1d673668`), `make test-wasm`, nine REPL tests; live: 1.1.0 eyebrow, no 1.0.1 references, all six download links return 200, WASM digest with `application/wasm` and `no-cache` |
| Docs | `v1.1.0` | 66 pages; link check in PR #610's CI; live release page, getting started and local I/O |

`RELEASE`, `REVISION` and `RUNTIME_REVISION` in the document root now say 1.1.0, `89738b7` and
`8fc5a2d5`. The previous document root (with /docs) is in
`/root/d6-1.1.0-rollback-20261119/site.tar.gz`. nginx and TLS were not touched.

## Production applications

| Target | Deployed source | Verification | Rollback |
|---|---|---|---|
| Pheint | App `7d00901` (pheint #54: the API on `GraphQL::App`, schema compiled in with `File.embed`), built on the local VM against `v1.1.0` | Source suite, prebuilt suite with and without the JIT; running binary SHA-256 `fcc05ff7...` matched the build; the deploy script's eight outside checks passed (53 MB); an anonymous `me` query got `UNAUTHENTICATED` from `@auth` | `/var/backups/pheint/20261119T002058Z` |

The registry and SkindicateV3 stay on the v1.0.0 tag. Nothing they use changed in 1.1.0.
