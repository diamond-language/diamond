# 1.0.1 publication and deployment record

Completed 2026-11-17 UTC. The release tag `v1.0.1` identifies `2f84c11b`, the merge of
[PR #604](https://github.com/diamond-language/d6/pull/604) into `1.0`. Every check passed before
the merge; one arm64 database-dialect job was rerun after the Docker port-setup flake
([#600](https://github.com/diamond-language/d6/issues/600)). The same change landed on `main` in
[PR #605](https://github.com/diamond-language/d6/pull/605), which stays at 1.1.0-dev.

1.0.1 makes d6 install on distributions that have moved to OpenSSL 4 (Fedora 45, Arch). It adds
a Fedora 45 rpm, links OpenSSL statically into the tarballs and their `d6 build` kit, and adds a
CA-bundle fallback to `TLSSocket.connect` for the static OpenSSL. See
[the release notes](release-1.0.1.md).

## Published assets

[Release workflow 44165401036](https://github.com/diamond-language/d6/actions/runs/44165401036)
built, installed and smoke-tested all eight packages before publishing
[d6 1.0.1](https://github.com/diamond-language/d6/releases/tag/v1.0.1). GitHub marks it Latest.

| Asset | SHA-256 |
|---|---|
| `d6-1.0.1-linux-aarch64.tar.gz` | `581c5018824b89cdced2b6cd85aefb4cac97a28e34f70f08a23a775d37c1b795` |
| `d6-1.0.1-linux-x86_64.tar.gz` | `304bda36b8b53345ac808685c4230bb9e74b31cc63acdfa513408c09d1441d63` |
| `d6-lang-1.0.1-1.fc44.aarch64.rpm` | `d15ffe9cf73f98eda3a3df5b2be5d20cd28b61dc973368183a61bfeac0a831b1` |
| `d6-lang-1.0.1-1.fc44.x86_64.rpm` | `4d9388d62de637c72f1c0266d7c87590adea12fa773c836ad3250c9de5b2bb42` |
| `d6-lang-1.0.1-1.fc45.aarch64.rpm` | `26fa77d759e7973d9cfe22a96b0fce45ad0ab0b091b890a2989cf3dab6dac531` |
| `d6-lang-1.0.1-1.fc45.x86_64.rpm` | `c55a4236ccc391eaf3acfb5700a9be6ab9cc04027eae5fc8aea5fd60bd6e1e95` |
| `d6-lang_1.0.1-1_amd64.deb` | `5e67673226e3e7b0af8ad969fdebe2af6419c716289e060e29eb6098aea4b2ba` |
| `d6-lang_1.0.1-1_arm64.deb` | `6cb0ab3aa0ec7c3582fab88a7d673ea55c86851210919f29b143624518b6b9f4` |
| `d6-vscode.vsix` | `729f2db6a2fd1da3684c9c4e1370f51cc930b5439a3fc5f95c4192409289e5a4` |

The VS Code extension stayed at 0.8.0, so the Marketplace step skipped it.

## Verification on Fedora 45

On a Fedora 45 desktop (OpenSSL 4, no `openssl3-libs`), `mise install
github:diamond-language/d6@1.0.1` installed the x86-64 tarball. `d6 --version` printed
`d6 1.0.1`, a `TLSSocket.connect` to cuts.rolld6.codes verified the certificate, and `d6 build`
produced a standalone binary that did the same. Bare `latest` on mise's github backend gives
1.0.1 once its 24-hour `minimum_release_age` hold has passed (about 2026-11-18 20:00 UTC).

## Cuts

None changed between `v1.0.0` and `v1.0.1`. `tools/verify_registry_launch.d6` passed for all 20
public cuts against cuts.rolld6.codes, so nothing was published and no credential was issued.

## Site and docs

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `7d8cf56` (green PR #48) | Fresh WASM from `v1.0.1` (`32be387110b7ff50a0ce56b00521c56645a86c1cbc03b7b1fdffaad5b4b4b1b1`), `make test-wasm`, nine REPL tests; live: 1.0.1 eyebrow, the mise tab, no 1.0.0 links, all six download links return 200, WASM digest with `application/wasm` and `no-cache` |
| Docs | `v1.0.1` | 65 pages; 5,855 link checks; live release page, getting started (Fedora 45 packages, install comparison) and compatibility |

`RELEASE`, `REVISION` and `RUNTIME_REVISION` in the document root now say 1.0.1, `7d8cf56` and
`2f84c11b`. The previous document root (with /docs) is in
`/root/d6-1.0.1-rollback-20261117/site.tar.gz`. nginx and TLS were not touched.

## Production applications

Not redeployed. The registry, SkindicateV3 and Pheint still run the v1.0.0 tag. The 1.0.1
runtime change only matters where d6's OpenSSL directory doesn't exist on the host, which isn't
the case for those builds.
