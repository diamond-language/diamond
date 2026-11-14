# rc5 publication and deployment record

Completed 2026-11-14 UTC (2026-11-13 Pacific). The release tag `v1.0.0-rc5` identifies
`e4864b4c8fa753be8e1e494fa643b51e4dab62e1`, the merge of
[PR #562](https://github.com/diamond-language/diamond/pull/562) into `1.0`, which
passed every check before merge. The matching preparation landed on `main` in
[PR #563](https://github.com/diamond-language/diamond/pull/563).

## Published packages

[Release workflow 43548488056](https://github.com/diamond-language/diamond/actions/runs/43548488056)
built, installed and smoke-tested all four packages before publishing
[D6 1.0.0-rc5](https://github.com/diamond-language/diamond/releases/tag/v1.0.0-rc5).
Downloaded SHA-256 values matched GitHub's release metadata:

| Asset | SHA-256 |
|---|---|
| `d6-lang-1.0.0-rc5-1.fc44.aarch64.rpm` | `f9d44cf50bd57b65dbe01ba595d8c89db4130eec571f69acc3e770ec7f4a8287` |
| `d6-lang-1.0.0-rc5-1.fc44.x86_64.rpm` | `1bc51843210558d8e9c0d27dee3a386e7406d50f7c48f75a8320e6d8df0bfc14` |
| `d6-lang_1.0.0-rc5-1_amd64.deb` | `d64fe2eedd16ea84aa6ce83cda1dd3c88b47b2f565f9c7160c66ccc69f4848c9` |
| `d6-lang_1.0.0-rc5-1_arm64.deb` | `c339942d6abdf3bc5298f63cca03b9fd1ca47a6e82be2466c72a44e5e2a530be` |

The cookies cut 0.2.4 was published on 2026-11-12 with these changes (see
`applications/registry/deploy/PRODUCTION.md`). No other bundled cut changed, so no
cut was published for this release and no publishing credential was issued.

## Site and docs

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `db6f0fd` (green PR #36) | Fresh WASM from the tag (`91bcc86a028af12c2f526db3e42a5c363168901f3f246d1142c07c1937b47181`); `make test-wasm`; nine REPL checks; live eyebrow, WASM digest and type, download URLs |
| Docs | Tag `v1.0.0-rc5` | 39 pages; 2,419 link checks; live index and rc5 release page |

The previous document root is retained in
`/root/diamond-rc5-rollback-20261113/marketing.tar.gz`. nginx, TLS and the domain
redirects were not touched.

## Production applications

The registry, SkindicateV3 and Pheint still run rc4 (see
`docs/release-1.0.0-rc4-operations.md`).
