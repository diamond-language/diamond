# CI operation

The `test` workflow runs once per pull-request update and on pushes to `main`.
Its concurrency group combines the workflow name with the PR number or branch
ref. Separate PRs run independently; a newer main push or manual main run
cancels earlier main work. Keep `cancel-in-progress: true` and avoid `always()`
on test jobs, which can keep cancelled work alive.

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
