# Persistent job service

A reference application for cooperative cancellation: bounded HTTP requests,
SQLite persistence, one supervised worker, per-job deadlines, cancellation,
retries, and graceful shutdown. The built-in job simulates small work units;
replace it with checkpointed, idempotent application work.

From the repository root:

```sh
make debug
bash tools/install_local_cuts.sh .
build/diamond examples/job_service/service.di 18090 /tmp/diamond-jobs.sqlite
```

The demo has no authentication. Gremlin's listener is network-accessible; run it
on a development machine with appropriate network restrictions. It is not a
public deployment configuration.

```sh
curl -H 'Content-Type: application/json' -d '{"work_ms":1000,"timeout_ms":5000}' http://localhost:18090/jobs
curl http://localhost:18090/jobs/1
curl -X POST http://localhost:18090/jobs/1/cancel
```

`POST /jobs` accepts integer `work_ms` (0–60000, default 100), `timeout_ms`
(0–60000, default 5000), `max_attempts` (1–10, default 3), and `fail_until`
(0–10, default 0). `fail_until` injects ordinary failures for the first N failed
attempts so retries can be exercised. Successful output is the simulated work
count, rounded up to 10 ms units. `GET /health` reports HTTP readiness.

States are `pending`, `running`, `succeeded`, `failed`, `cancelled`, and
`timed_out`. Normal failures use the existing jobs cut's 5/10/20-second capped
backoff. Cancellation and deadline expiry do not retry. A cancellation arriving
before the success write wins; cancellation and timeout may race. Cancelling an
already terminal job returns its existing state unchanged. Queued cancellation
is durable and immediately terminal.

Send SIGTERM or press Ctrl-C once to stop. Gremlin drains active HTTP connections
(up to its existing grace period), then the service cancels its root token and
joins the worker. Interrupted jobs return to pending without consuming an
attempt. Each worker closes its own database in ensure. A second signal uses
Gremlin's existing forced-exit behavior and cannot promise cleanup.

Run only one service process per database. On worker/process restart, abandoned
running jobs are requeued. This is at-least-once execution, not exactly-once:
real handlers must make side effects idempotent. SQLite busy waits are bounded
to one second but are not token-interruptible. Cancellation checks happen between
work units, not inside arbitrary native calls. Deadlines start at each attempt,
not at enqueue time; they are not persisted across process restarts.

`bash examples/job_service/smoke_test.sh` runs the HTTP lifecycle checks against
interpreted and compiled servers using temporary databases. No external database
server is required.
