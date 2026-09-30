# CI operation

The `test` workflow runs once per pull-request update and on pushes to `main`.
Its concurrency group combines the workflow name with the PR number or branch
ref. Separate PRs run independently; a newer main push or manual main run
cancels earlier main work. Keep `cancel-in-progress: true` and avoid `always()`
on test jobs, which can keep cancelled work alive.

Cancellation is asynchronous. In run 309, superseded by run 310, sixteen jobs
finished cancellation within seconds, while FreeBSD remained until roughly
five minutes after cancellation began. Run 310 was superseded by manual run
311 before starting any jobs. This supports working concurrency cancellation;
it does not establish why the FreeBSD runner was slow to stop.

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

Budgets use maximum job durations from successful runs 305–308, including
setup and teardown, with extra time for runner and package-mirror variation:

| Job | Observed maximum | Timeout |
| --- | ---: | ---: |
| Matrix builds | 24.0 min | 40 min |
| Matrix integration | 11.1 min | 20 min |
| Matrix fuzz | 21.2 min | 35 min |
| ARM64 | 4.3 min | 20 min |
| ARM64 sanitizers | 32.3 min | 45 min (unchanged) |
| musl | 5.2 min | 20 min |
| FreeBSD | 9.2 min | 20 min |
| macOS | 6.9 min | 20 min |

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
