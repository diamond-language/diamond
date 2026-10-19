# CI operation

The `test` workflow runs once per pull-request update, and by hand
(`workflow_dispatch`) on any branch. It does not run on pushes to `main`: every
change arrives through a PR that was rebased onto `main` and passed on that exact
tree, so a run on the merge commit would repeat it. If `main` moved between a
PR's last run and its merge, rebase and let the PR run again before merging; use a
manual run to validate `main` itself after anything unusual.

Its concurrency group combines the workflow name with the PR number or branch
ref. Separate PRs run independently; a newer manual run on the same ref cancels
the earlier one. Keep `cancel-in-progress: true` and avoid `always()` on test
jobs, which can keep cancelled work alive.

Cancellation is asynchronous: most jobs stop within seconds of a newer push, but a
job on a slow runner (FreeBSD has been the usual one) can take up to the five minutes
GitHub allows.

GitHub documents a five-minute cancellation deadline in its
[workflow cancellation reference](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-cancellation).
If the UI is stuck, find the API run ID (different from the displayed run
number), inspect it, and force-cancel that specific run:

```sh
gh run list --workflow ci.yml
gh run view RUN_ID
gh api --method POST repos/diamond-language/diamond/actions/runs/RUN_ID/force-cancel
gh run view RUN_ID
```

A successful request does not mean cancellation has finished; verify the
status becomes completed and the conclusion cancelled. Deleting the run is
not required to stop it.

## Timeout budgets

Budgets are the longest successful job durations seen across recent `main` runs, including
setup and teardown, with room for runner and package-mirror variation:

| Job | Observed maximum | Timeout |
| --- | ---: | ---: |
| Matrix builds | 15.2 min | 40 min |
| Matrix integration | 7.6 min | 20 min |
| Matrix fuzz | 9.2 min | 35 min |
| ARM64 | 3.6 min | 20 min |
| ARM64 sanitizers | 10.5 min | 45 min |
| Stress, threaded cases on two CPUs | 10.5 min | 40 min |
| Stress under TSan | 14.8 min | 45 min |
| Stress on ARM64 | 8.4 min | 40 min |
| musl | 3.9 min | 20 min |
| FreeBSD | 5.5 min | 20 min |
| macOS | 5.5 min | 20 min |

The stress jobs pin the threaded test cases to two CPUs, which is slower and orders
threads differently than the other jobs; a timing-dependent test that passes elsewhere can
fail only there.

Job timeouts bound execution after a runner starts; they do not bound queue
wait or guarantee immediate cancellation. Revisit these budgets if successful
runs approach the limits rather than removing coverage to meet a deadline.

## Signal regression test

`tests/run.sh` invokes `tests/signal_interrupt.sh`. The child prints `ready`
after installing the handler and listening. The harness must see that line
before sending INT, then see `caught INT` before connecting the client.
Both waits are bounded and report captured output on failure. Run it alone:

```sh
DIAMOND_BIN=./build/diamond bash tests/signal_interrupt.sh
```

## Pinned actions

The workflows name each action by commit SHA with the tag in a comment (`actions/checkout@<sha> # v7`), so a retagged or compromised release cannot change what runs with the job token. To update one, resolve the new tag (`gh api repos/actions/checkout/git/ref/tags/v7 --jq .object.sha`; for an annotated tag, follow it to its commit) and replace the SHA and the comment together. The workflow token itself is read-only (`permissions: contents: read`).

## The sanitizer job and its one retry

`make test-sanitize` builds the ASan/UBSan variant and runs `tests/run.sh` through `tools/test_sanitize.sh`. A failed first attempt is retried once, because shared runners occasionally trip a timing assumption in the test harness. A sanitizer report in the first attempt is never retried: the run fails on the spot, so a real memory error that appears on one run in two cannot be buried by a green second attempt. A retry that does happen prints a line beginning `test-sanitize: attempt 1 failed` with the first failing test line, and a GitHub warning annotation.

What the history shows (CI logs of every run from 2026-08-20 to 2026-10-19, one sanitizer job per run): the step used to fail silently on about half of runs in early September; the retry was added on 2026-09-05, and after 2026-09-07 the first attempt failed only twice in about 540 jobs. Neither was a sanitizer finding: a 10 s timeout around a GC-stress client (2026-09-07), and a duplicated SIGINT from `timeout(1)` in the signal test (2026-10-10, since made tolerant). No log in the period contains a sanitizer report. To look for retries yourself, grep a job log for `test-sanitize: attempt 1 failed` at the start of a line (the Makefile echo of the old recipe contained the same words, so match the line start). If the retry fires more often than that, find the failing test before widening it. The environment facts step that once printed `mmap_rnd_bits` and friends is gone: the ASLR-entropy explanation it was built to test was never supported by the data.
