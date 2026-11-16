# rc7 publication record

Completed 2026-11-16 UTC. The release tag `v1.0.0-rc7` identifies `7f179950`, the merge of
[PR #594](https://github.com/diamond-language/d6/pull/594) into `1.0`, which passed every
check before merge. The matching preparation landed on `main` in
[PR #595](https://github.com/diamond-language/d6/pull/595). Marketplace publishing by
trusted publishing came just before, in [#592](https://github.com/diamond-language/d6/pull/592)
(`1.0`) and [#593](https://github.com/diamond-language/d6/pull/593) (`main`).

rc7 has no language or runtime change since rc6. It was tagged to run the full release
workflow, with its new tarball and extension jobs, before 1.0.0. No cuts were published,
and the site, docs, registry pages and production apps were not redeployed.

## Published assets

[Release workflow 43951815261](https://github.com/diamond-language/d6/actions/runs/43951815261)
built, installed and smoke-tested the packages and tarballs before publishing
[d6 1.0.0-rc7](https://github.com/diamond-language/d6/releases/tag/v1.0.0-rc7):

| Asset | SHA-256 |
|---|---|
| `d6-1.0.0-rc7-linux-aarch64.tar.gz` | `dbea2016a62dd43a1773d9566dca6abeef15fb58e732a4f9e30f43cb08892446` |
| `d6-1.0.0-rc7-linux-x86_64.tar.gz` | `3ff91009d0d597af2bb984190812e0e14a1749f0e342e54eb98bb8d3e669f97d` |
| `d6-lang-1.0.0-rc7-1.fc44.aarch64.rpm` | `db956695c59cbfb74604bf69c5ec8004a12ae6a16b9474c1ada2f44c70552b6c` |
| `d6-lang-1.0.0-rc7-1.fc44.x86_64.rpm` | `641ee0fcdb5ad8be1ad80c0a7f0dcd36dd79bf84dcb4c2ebe7b5f8c2ddd4889b` |
| `d6-lang_1.0.0-rc7-1_amd64.deb` | `5acff02fc0a823c2537404d34c1f09943634142448e68b64112572d25e304ca5` |
| `d6-lang_1.0.0-rc7-1_arm64.deb` | `3c8055077d0df117b3113cdd3328ff6e1edbf33cda6bca2b97988a2785f423ec` |
| `d6-vscode.vsix` | `6a1830f54cd01ed69c1fe71608ec2544925ad84a06556e1b4b321d26c056b405` |

## VS Code extension

The workflow's `vscode` job published `rolld6.d6` 0.8.0 to the Visual Studio Marketplace by
GitHub OIDC trusted publishing (no stored token); the Marketplace query API lists it. The
Open VSX step was skipped, as intended, because no `OVSX_PAT` secret exists (see the
roadmap). The extension is versioned separately, so a later release publishes it again only
after `editors/vscode/package.json` is bumped.

## mise

`mise use -g github:diamond-language/d6@1.0.0-rc7`, run in an empty mise home on Fedora 45,
installed `bin/d6`, `facet`, `d6-lsp` and `d6-dap`; `d6 --version` printed `d6 1.0.0-rc7`
and a one-line program ran.

Without a version, mise skips pre-releases and picks the highest stable release that has a
matching tarball: 0.29.0, whose static musl tarball shares the naming pattern. 1.0.0 will be
the highest stable version and becomes the default once tagged; check
`mise use -g github:diamond-language/d6` (no version) then.
