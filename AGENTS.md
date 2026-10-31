# Agent instructions

- Always commit completed changes in this repository. After verification, stage the task's files, create a descriptive commit, and confirm the working tree status before reporting completion. Do not leave finished work uncommitted.
- Add new functionality on a branch and land it through a pull request, never by committing to `main`. Before merging, rebase the branch onto the current `main` (no merge commits from `main` into the branch) and have a green CI run on the rebased head. Do not merge on a red or still-running run. Several open pull requests that each have a green run and do not conflict may be merged together without a re-run apiece. See "Branches and pull requests" in CONTRIBUTING.md for the steps.

## How the owner works with agents

- **Act on what is authorised; do not hand work back.** The owner runs agents in bypass-permissions mode and has said "stop depending on me for stuff YOU can handle". Merge your own pull requests once the rules above are met, delete merged branches, clean up worktrees, publish and deploy when they have asked for it. Ask only for a real design decision (a policy format, a public API shape) or something destructive and unrequested.
- **Budget is tight.** Do not build compatibility shims for an out-of-date dependency: bump it and relock. Do not over-engineer for scale: the registry is a tiny VM serving a mostly static repository of skins with a few social features, with no budget for more (karma races were waved off for that reason). Do not build features the roadmap says need "a concrete use case" without one.
- **Scripts go in files.** Never paste a long script, heredoc or chained command block into a shell call. Write it to a file (the session scratchpad, or `tools/` when it is worth keeping) and run it by path. Background commands are echoed to the owner when they finish, so keep them to one short line. Use `git commit -F file` for long messages. Report results in a few plain sentences, not pasted output.
- **Check before claiming.** Reviewers (human or model) over-claim: verify a finding against the code before fixing it. Report outcomes as they are, including a merge or step that went wrong.
- **Keep the changelog current.** Include user-visible language and tooling changes in `CHANGELOG.md` under `Unreleased` in the same pull request. Summarize capability milestones rather than every implementation step.
- Use they/them for people whose pronouns are not stated.

## Landing a change

1. Branch from current `main` (`git checkout -b name`). For parallel work use `git worktree add ../wt-name -b name origin/main`; the merge script below checks out `main` in the main checkout.
2. Run the focused tests, then `make test` (hermetic, about 2,100 tests). Run `make test-integration > log 2>&1; echo EXIT=$?` for anything wider and gate the push on `EXIT=0`, not on `tail` of the log. `make test-sanitize`, `make test-tsan` and `make test-stress-threads` are for threading or memory changes.
3. Commit, `git push -u origin name`, `gh pr create`.
4. A minute after the push, run `tools/merge_when_green.sh PR BRANCH` from the main checkout, in the background. It waits for all checks (31 today), merges only if every one is green and `main` has not moved, then updates `main` and deletes the branch. If `main` moved, `git fetch && git rebase origin/main && git push --force-with-lease`, wait a minute, run it again. Never merge on a partial set of checks; the first few registered checks passing is not a green run. Do not replace the script with a hand-written shell gate: one built on `grep -qv` let PR #414 merge with a failing check.
5. After several PRs merge in a row, rebase the remaining ones one at a time so they do not race.

CI is slow (about 15 minutes) because it runs two distros times two compilers times builds/integration/fuzz, plus arm64, musl, FreeBSD, macOS, wasm and stress jobs. Batch related fixes into one branch rather than many small ones.

## Building and testing

- `make` builds `build/diamond`; `make facet` builds the package manager. Tests: `tests/cases/NAME.d6` with `.expected`, `.expected_error`, `.expected_contains` or `.expected_lastline`, optional `.env`/`.flags`; `tests/run.sh` holds the shell-driven tests; most packages have `packages/NAME/test.sh` and a `make test-NAME-package` target (Arel is covered by `tests/cases/arel_*` and `make test-database-dialects`).
- Measure performance on a `make release` build with `perf stat -e instructions:u`, since wall time moves with the laptop's power mode. Run each program once first so a cold bytecode cache does not skew the count, check that every variant printed the same valid output, and remember the JIT is opt-in (`DIAMOND_JIT=1`).
- Do not add Python to build or test paths; port such checks to Diamond, shell or C.
- Kill a stale `./pheint-server` before `make test-integration` (it holds port 18200 and the TLS echo test then fails).
- Write tests so they do not pin source line numbers. A few do on purpose (backtrace and parse-error positions: `tests/run.sh`, `tests/parser_diff.sh`, `tests/multifile`, `tests/parser_*`, `jit_*backtrace*`); leave those files' line layout alone.
- `tools/test_sanitize.sh` retries the sanitizer suite once for harness flakes but fails at once on any sanitizer report. Do not weaken that.
- Reading CI logs: `gh api --allow-escape-sequences repos/diamond-language/diamond/actions/jobs/ID/logs`. `ls` may be aliased and print a header row.
- Language gotchas: calls need parentheses (a bare `obj.method` is a method reference, not a call); blocks on calls are written `.each() do |x| ... end`; there is no sleep builtin (wait with `Channel.new(1).wait_readable([], Time.monotonic() + seconds)`); a script prints only what it `puts` (the REPL and `-e` also print the last expression's value, and so does the test harness: `tests/cases` expectations include it). Methods whose whole body is one expression are written as endless defs (`def f(x) = expr`, or `def f(x) =` with the body indented below when the line would pass 120 columns). `packages/` has not been converted (see below).

## Packages (cuts) and the registry

- Every change to a package under `packages/` needs a version bump in its `diamond.cut` and a regenerated launch inventory (`build/diamond tools/prepare_registry_seed.d6 --expect`, checked by `make test-registry-inventory`). Caret ranges on 0.x versions do not cross a minor (`^0.1.0` excludes 0.2.x), so a dependent's range must be bumped with it.
- Published registry releases are immutable. Never publish the taken-down cuts (`active_auth`, `active_discussion`, `active_karma`, `active_social`, `active_tagging`, `dials`). Publishing changed cuts is part of every release, so do not wait to be told; outside a release, publish only on request. Either way, take a verified backup first, use a short-lived scoped credential, revoke it afterwards and destroy local credential files. `tools/publish_registry_seed.d6`, `tools/verify_registry_launch.d6` and `applications/registry/deploy/PRODUCTION.md` (the publication record, which you update after each publish or deploy) describe the procedure; `docs/registry-release-plan.md` has the runbook.
- Deploying the registry service: `tools/deploy_registry.sh` (`--dry-run` first). It needs the local build VM, which is off by default: start it with `qemu-system-x86_64 -enable-kvm -cpu host -smp 4 -m 4G -drive file=disk.qcow2,if=virtio,format=qcow2 -drive file=seed.iso,if=virtio,format=raw,media=cdrom,readonly=on -netdev user,id=n0,hostfwd=tcp:127.0.0.1:2222-:22 -device virtio-net-pci,netdev=n0 -display none -serial file:console.log` run from `../.vm-build`, wait about a minute for SSH on port 2222, and power it off afterwards (`ssh -p 2222 builder@127.0.0.1 sudo poweroff`, using `../.vm-build/known_hosts`). Then record the new runtime revision in `PRODUCTION.md`. Only deploy when asked.

## State at the last hand-off (2026-10-31)

The latest release is v0.20.2, and the live registry runs that version. CI runs the live database dialect tests (`make test-database-dialects`, through `tools/ci_database_dialects.sh`, in an `ubuntu:26.04` container beside the runner's docker) and the registry nginx drill with the full seed rehearsal (`test-registry-nginx` job). Open items:

1. **Release 0.21:** tag only when the owner says so. First re-run the SkindicateV3 and Pheint suites on final `main` and re-measure against v0.20.2. Cuts changed since the last publish include `http`, `mjolnir` and `arel`; compare `packages/` with `PRODUCTION.md` for the full list.
2. **Language server receiver precision across imported files** (docs/roadmap.md, "Improve receiver-aware tooling").
3. Needs an owner decision or a driving use case first: generic Mjolnir `Schema`/`Query`/`Repo` (held; docs/roadmap.md has the blockers), sandbox path and host allow-lists (`docs/sandbox.md` says why they were left out; a JSON policy of exact resolved-path prefixes and exact hostnames was proposed), supervisors that cross threads, the ORM return-annotation audit, and endless-def conversion of `packages/` (143 methods; do it with the next release of each package, not on its own).
4. Literate programming: investigated and piloted on the `lp` branch (PR #250, unmerged) and then put on hold by the owner; do not merge or delete it unasked. Memo: `docs/internal/literate-programming-investigation.md` on that branch.
5. Reminder: check that the TLS certificates renew (dilang.tech about 2026-11-17, cuts.dilang.tech about 2026-11-22; `docs/registry-release-plan.md`).
6. Cleanup: `tools/trim_seed_to.sh` still has inline Python, the example web UIs have only been tested over HTTP, and `make test-parser-diff` has not been run since the generics work.

CI lessons from adding jobs: the source needs gcc 15 (`#embed`, C23), so jobs run in `ubuntu:26.04`; the plain `make` target carries none of the `-std=c23` flags (use `make debug` first, as `test-integration` does); tests that need cuts must install them into a temporary project (`tools/install_local_cuts.sh`); a database container is not ready when `pg_isready` first answers, so wait for a real query.
