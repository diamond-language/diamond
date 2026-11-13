# rc4 publication and deployment record

Completed 2026-11-12 UTC (2026-11-11 Pacific). The release tag `v1.0.0-rc4` identifies
`0e482f26c56854c4c41c6c82d093ad04f6569a21`, the exact reviewed head of
[PR #549](https://github.com/diamond-language/diamond/pull/549). All 37 checks
passed before merge; the tagged tree equals the green merge tree. This promotes
the already tested `1.0` preparation without changing its source tree.

## Published packages

[Release workflow 43162263306](https://github.com/diamond-language/diamond/actions/runs/43162263306)
built, installed and smoke-tested all four packages before publishing
[D6 1.0.0-rc4](https://github.com/diamond-language/diamond/releases/tag/v1.0.0-rc4).
Downloaded sizes and SHA-256 values matched GitHub's release metadata:

| Asset | SHA-256 |
|---|---|
| `d6-lang-1.0.0-rc4-1.fc44.aarch64.rpm` | `d42966d6d301d228764bdb4cd9b7d7a77f1519c1ee65b2f6cc07755153b55626` |
| `d6-lang-1.0.0-rc4-1.fc44.x86_64.rpm` | `11ca577cfd1ed5358fa00671d460a178580083bd9827e7a481f01b882bcf6be0` |
| `d6-lang_1.0.0-rc4-1_amd64.deb` | `2a7ad6323de852cb6006fc2effc801834eda3f76511081c1c06e4afecbc1e9f5` |
| `d6-lang_1.0.0-rc4-1_arm64.deb` | `1e2659a4cfe63e832fefa3cc5113940d3fe4dfcfe28acda5e6cb37982bca9782` |

No bundled cuts changed. All 20 existing immutable public cuts were verified;
no cut was republished and no publishing credential was issued.

## Production

All native production builds used the local Ubuntu 26.04 x86-64 VM, with
`-O3`/LTO and the existing JIT settings. Nothing was compiled on the droplet.

| Target | Deployed source | Verification |
|---|---|---|
| Registry | Runtime/tag `0e482f26c56854c4c41c6c82d093ad04f6569a21` | Restore-verified off-host snapshot; health; all 20 public cuts resolved, digest-checked, loaded and reinstalled from unchanged locks |
| SkindicateV3 | App `8a5d5cf4813b40ac3839087448dbf6026dc370b1`, runtime rc4 | Source and compiled/JIT suites; bundle checksum; running executable digest; public homepage, browse, login and populated skin page |
| Pheint | App `0d21d7cea7b51306e088b8d3eb1e9eef94026cc5`, runtime rc4 | Source/JIT suites; Ubuntu standalone smoke and both web builds; bundle/running digest; player/developer deep links, security headers and nested public GraphQL returning three games and three posts |
| Marketing/docs | Marketing `241c3414a93e467b41fb1609f610585f38cbbb3f`, docs/runtime rc4 | Fresh WASM test; nine homepage/REPL checks; 38 docs pages and 2,330 link checks; all four download URLs; live revision and WASM digest |

The app and marketing revisions are the reviewed branch-head commits, now
ancestors of their merged main branches. Pheint records its runtime in
`/opt/pheint/DIAMOND_REVISION`; Skindicate records it in
`/opt/diamond/skindicate-v3/diamond.revision`. The registry records the exact
tagged commit in its deployment `REVISION`.

Verified executable/module SHA-256 values:

- SkindicateV3: `1a3a402e6d7079f1c149d414ac118636dc6f2a551938377bf408b3f05b1e9cf3`.
- Pheint: `a6b4a7500b08c7b446fe742e658de5fa51001117ec61f39f2fa22745385845c5`.
- Registry runtime: `fcb3abbdb932d955c398995a9e7b2adecbcdfd605b4eea8ef7083277d5f379a2`.
- Registry archive: `5afa2bfe3331e73986696b38d1bfc742505986fc80b35b886b39f6e1fe36d08a`.
- Browser WASM: `be08de3253267b7771af848e34b8c5288521f69176aae60081c19a2074a342f2`.

Private rollback binaries, sites, settings and integrity/FK-checked database
snapshots are retained under `/root/diamond-rc4-rollback-20261111`. The registry's fresh
off-host snapshot is
`~/diamond-registry-backups/cutbackup-2026-11-11-213807-pre-0e482f26/snapshot`.
Existing worker counts and JIT settings were preserved; comparisons confirmed
the Pheint/registry env files and Skindicate unit were unchanged. No app schema
migration was introduced. nginx syntax, all three service identities and public
health/pages passed final verification. The build VM was powered off afterward.

The [application soak report](internal/rc4-application-soak.md) records the
bounded memory comparison against rc3. No browser surface was available for
manual visual/keyboard QA; the functional WASM, homepage, build, link and HTTP
checks above passed.
