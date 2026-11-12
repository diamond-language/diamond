# rc3 publication and deployment record

Completed 2026-11-11. The release tag `v1.0.0-rc3` points to
`fdf6ab487f03fde5ece43290714a8f48b0922a16`, the exact head of
[PR #546](https://github.com/diamond-language/diamond/pull/546). All 37 checks
passed before merge; the tag's tree was checked against the green merge tree.
The tag intentionally identifies that tested source commit rather than its
later merge commit.

## Published packages

[Release workflow 43121499272](https://github.com/diamond-language/diamond/actions/runs/43121499272)
built, installed and smoke-tested all four native packages before publishing
[d6 1.0.0-rc3](https://github.com/diamond-language/diamond/releases/tag/v1.0.0-rc3).
Downloaded sizes and SHA-256 values matched GitHub's release-asset metadata:

| Asset | SHA-256 |
|---|---|
| `d6-lang_1.0.0-rc3-1_amd64.deb` | `98c337f258cf3f3ef8efc607873934a6b1ab74a5c61653ad7d9e4cf8dc760994` |
| `d6-lang_1.0.0-rc3-1_arm64.deb` | `7b5c3c55013d59f10417aa88e7db7e2a8eff0da2745f253235805c9641f0c064` |
| `d6-lang-1.0.0-rc3-1.fc44.x86_64.rpm` | `ccb636857b0b8e51dc38fc44d0895d568894617c72b08aaca33f01ace118f4a5` |
| `d6-lang-1.0.0-rc3-1.fc44.aarch64.rpm` | `71dce21b08b7ba912ad522f172ebf9b347599c3f92b0428c4c4dee028b2e8e9b` |

No bundled cuts changed since rc2. Existing immutable releases were verified,
not republished; no publishing credential was issued.

## Production

All native production builds were made in the local Ubuntu 26.04 x86-64 VM.
Service runtimes use `-O3`/LTO and preserve the existing JIT settings. Nothing
was compiled on the droplet.

| Target | Deployed source | Verification |
|---|---|---|
| Registry | Runtime/tag `fdf6ab487f03fde5ece43290714a8f48b0922a16` | Restore-verified off-host backup; health; all 20 public cuts resolved, digest-checked, loaded and reinstalled from unchanged locks |
| SkindicateV3 | App `880472598607dfae3fa195654554d791be0a8e78`, runtime rc3 | Source and compiled/JIT suites; bundle checksum; running executable digest; public homepage/login |
| Pheint | App `2a4212fa24fe817e3a79b22804f22416bffe7e84`, runtime rc3 | Interpreter/JIT suites; fresh server and both web builds; bundle/running digest; API, player/developer deep links and security headers; nested public GraphQL returned three games and three posts |
| Marketing/docs | Marketing `16119f115290a7e6e59037c1869ce5ec2c9cc920`, docs/runtime rc3 | Fresh WASM tests; nine homepage/REPL tests; 37 docs pages; all four download URLs; live revision and WASM digest |

The app builds identify the reviewed branch-head commits, which are ancestors
of their merged main branches. Their tracked runtime pins identify the exact
rc3 tag commit. Pheint records its runtime in `/opt/pheint/DIAMOND_REVISION`;
Skindicate records it in `/opt/diamond/skindicate-v3/diamond.revision`.

Verified executable/module SHA-256 values:

- SkindicateV3: `970e0ac24e3e748d6c501aacb6759f94ce94d739c0451e3763d08a1fb7046416`.
- Pheint: `b9cdcabe754dd1b11e03c0758a894e496ad8f0b3bb635024e51bc6ee51dca083`.
- Browser WASM: `71209e1085b311edc513509df6452b36ca0d2ea10d70858774abe52a0e45b897`.
- Registry deployment archive: `c402ec50aeecd0b701070000cb7dc02a36b4edfc43139ba28fc83e455bc520b9`.

Private rollback binaries, sites, settings and integrity/FK-checked application
database snapshots were retained on the host under
`/root/diamond-rc3-rollback-20261111`. The registry's retained off-host snapshot
is recorded in its [production record](../applications/registry/deploy/PRODUCTION.md).
No app schema migration was introduced. Production worker counts were preserved
for the single-core host. The dedicated build VM was shut down after the builds.

No browser surface was available for visual interaction QA. Functional WASM,
homepage, build and HTTP checks passed; this record does not claim manual visual
verification.
