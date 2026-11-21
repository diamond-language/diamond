# 1.3.0 and 1.3.1 publication and deployment record

Completed 2026-11-21 UTC.

`v1.3.0` identifies `96329780`, the merge of
[PR #626](https://github.com/diamond-language/d6/pull/626). Its head passed all 43 checks and
was current with `main`, so the merge commit has the same tree as the tested head. The site
and docs went out for 1.3.0. Then the compile fuzzer, running on the dev-bump PR
([#627](https://github.com/diamond-language/d6/pull/627)), found a stack-use-after-return in
`parse_heredoc`: a syntax error after a heredoc opener on the same line (`x = <<~A $`) indexed
`known_types` with `UINT16_MAX`. No production application had been moved to 1.3.0 yet.

`v1.3.1` identifies `5892351a`, the merge of
[PR #628](https://github.com/diamond-language/d6/pull/628): the fix (return a fresh register,
as `parse_string` does), the regression case `tests/cases/heredoc_after_error`, and the 1.3.1
version. Its head passed all 43 checks with the same tree as the merge. Before the PR, the fix was
checked with a local ASan/UBSan build, and a 5-minute, 4-worker compile-fuzz run seeded with
heredocs (about a million executions) found nothing. `main` then moved to 1.4.0-dev (#627,
`f5ac934f`).

Like 1.1.0 and 1.2.0, both were cut from `main`; the `1.0` branch stays at 1.0.1. See the
release notes for [1.3.0](release-1.3.0.md) and [1.3.1](release-1.3.1.md).

## Published assets

Both release workflows built, installed and smoke-tested all eight packages before publishing.
GitHub marks [d6 1.3.1](https://github.com/diamond-language/d6/releases/tag/v1.3.1) Latest
(release run 44861924085; 1.3.0 was run 44858649879).

| Asset (1.3.1) | SHA-256 |
|---|---|
| `d6-1.3.1-linux-aarch64.tar.gz` | `600090d7a14dbaad5a714c90b944390fdccdd23b4523d38c25eec832b366273b` |
| `d6-1.3.1-linux-x86_64.tar.gz` | `831f860475e3732a21d2596c8de7c0a34544ef735f4ebaba64e1d2d4feaf20ad` |
| `d6-lang-1.3.1-1.fc44.aarch64.rpm` | `5b277308e277fabd4d2482a4737f0aef0d57fa8c6f6868b9684e8c2c2d2faae0` |
| `d6-lang-1.3.1-1.fc44.x86_64.rpm` | `345cf8cf7f584a7cb2a6c0d8f323fb9cb0bcf62273b3bfc698cfd4f834d03db8` |
| `d6-lang-1.3.1-1.fc45.aarch64.rpm` | `790f8ee7c201f63dcb72a71e6ae94ce8990bccfa053be74de97e0cc407e9ceff` |
| `d6-lang-1.3.1-1.fc45.x86_64.rpm` | `51d65e3c60c4e60f2b0946f70e23873d308ba63942bc510f393af6696c84de64` |
| `d6-lang_1.3.1-1_amd64.deb` | `3ef3758a390e97c8aeb19b87a71d5b638dd6e8915eefd761ecc804b1f938d078` |
| `d6-lang_1.3.1-1_arm64.deb` | `9687615a881fe761fb4eb6cd6fd5b803b5b1e52dd3413b5030bc5447ac87b2be` |
| `d6-vscode.vsix` | `bc7141308d274cbda2edd62df998565200845129123dfbf2900eb7725f5cb32e` |

## Cuts

http 0.2.7 and html 0.1.0 were published on 2026-11-20, before the release (see
`applications/registry/deploy/PRODUCTION.md`). `tools/verify_registry_launch.d6` passed for all 23
public cuts against cuts.rolld6.codes after the 1.3.0 tag and again after the registry upgrade, so
nothing more was published.

## Site and docs

| Target | Deployed source | Verification |
|---|---|---|
| Marketing | Marketing `7f49d50` (green PR #53; 1.3.0 was #52) | Fresh WASM from `v1.3.1` (`9df5126bd074293368cce2a5918846d24f76de90f1b076d6259216bd1b7ad489`), `make test-wasm` and the site REPL test; live: 1.3.1 eyebrow, all six download links return 200, WASM digest with `application/wasm` and `no-cache` |
| Docs | `v1.3.1` | 97 pages, 13,117 links checked, none broken. The release-candidate pages left the sidebar (summarized in [1.0.0](release-1.0.0.md)), and the docs sync now uses `--delete`, which removed their five live copies |

`RELEASE`, `REVISION` and `RUNTIME_REVISION` in the document root say 1.3.1, `7f49d50` and
`5892351a`. The document root from before 1.3.0 is in
`/root/d6-1.3.0-rollback-20261121/site.tar.gz`. nginx and TLS were not touched.

## Production applications

| Target | Deployed source | Verification | Rollback |
|---|---|---|---|
| Pheint | App `d7bd3f7` (pheint #56: runtime pin to `v1.3.1`), built on the local VM | `build_vm.sh` source suite, prebuilt suite with and without the JIT, standalone smoke; running binary SHA-256 `2d26a613...` matched the build; the deploy script's eight outside checks passed (55 MB); an anonymous `me` query got `UNAUTHENTICATED` | `/var/backups/pheint/20261121T035702Z` |
| Registry | `tools/deploy_registry.sh --revision=v1.3.1` after its dry run (upgrade check: REQUIRED, the http and facet code it uses changed) | Health ok; `DIAMOND_JIT=1` kept; all 23 public cuts resolved, digest-checked, loaded and reinstalled | Verified snapshot `~/diamond-registry-backups/cutbackup-2026-11-20-195928-pre-5892351a` (off-host) |
| SkindicateV3 | App `2ce1390` (skindicate-v3 #30: runtime pin to `v1.3.1`), main CI run 44862945145 bundle | Running binary SHA-256 `3fa0bf3b...` matched the bundle; `DIAMOND_JIT=1` kept; homepage, browse, login, skin, download and profile pages; no errors or 5xx logged (46 MB) | `/root/skindicate-1.3.1-rollback` |
