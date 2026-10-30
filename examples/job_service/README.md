# Persistent job service

A reference application for cooperative cancellation: bounded HTTP requests,
SQLite persistence, one supervised worker, per-job deadlines, cancellation,
retries, and graceful shutdown. The browser dashboard imports real contact CSV
files, shows progress and validation errors, and offers cancel/retry controls.

From the repository root:

```sh
make debug
bash tools/install_local_cuts.sh .
build/diamond examples/job_service/service.d6 18090 /tmp/diamond-jobs.sqlite
# Open http://127.0.0.1:18090
```

The demo listens on loopback and checks Host/Origin for a single-user local
session. All browser assets are served by Diamond. An optional third argument
selects the asset directory (default `examples/job_service/web`, relative to
the working directory).

Paste CSV or choose a file with header `name,email` and up to 1,000 contacts /
64 KiB. Quoted commas, doubled quotes, embedded newlines, and CRLF are supported.
Names must contain 1–160 characters; emails receive a basic format check, not
mailbox verification. Every row is validated before contacts are published.
One invalid row fails the import with row numbers and leaves contacts unchanged.
Successful imports upsert by email and commit contacts with their terminal job
state in one transaction. This makes retry/restart replay safe. The progress
bar counts validated rows; terminal state confirms publication. The dashboard
shows the last 50 jobs and latest 100 contacts and refreshes every second.

`POST /jobs` with `{"csv":"name,email\nAda,ada@example.com\n"}` queues an
import. `GET /jobs` lists recent jobs; `GET /contacts` lists contacts.
`POST /jobs/N/retry` requeues failed, cancelled, or timed-out jobs and resets
their attempt/deadline state. Retry uses the original payload; fix invalid CSV
by submitting a new import. Existing queue files migrate in place.

The original count job remains available for deterministic cancellation,
deadline, and retry demonstrations, as well as lifecycle regression tests:

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

Send SIGTERM or press Ctrl-C to cancel the shared root token. The worker stops
and requeues interrupted jobs without consuming an attempt while Gremlin stops
accepting clients and drains existing HTTP connections for up to 0.5 seconds.
Silent clients and blocked response writers are then closed. Gremlin returns to
the service owner, which joins the supervisor and closes its database in ensure;
the worker closes its own database separately. Repeated signals cancel the same
source idempotently and do not bypass cleanup. The grace period bounds socket
waiting; application code and native calls still need to cooperate.

Run only one service process per database. On worker/process restart, abandoned
running jobs are requeued. This is at-least-once execution, not exactly-once:
real handlers must make side effects idempotent. SQLite busy waits are bounded
to one second but are not token-interruptible. Cancellation checks happen between
work units, not inside arbitrary native calls. Deadlines start at each attempt,
not at enqueue time; they are not persisted across process restarts.

`bash examples/job_service/smoke_test.sh` runs the HTTP lifecycle checks against
interpreted and compiled servers using temporary databases. No external database
server is required.

`bash examples/web_apps_test.sh` additionally covers CSV parsing, validation,
atomic failure, cancellation, explicit retry, replay, and durable contacts.
Both scripts run on CI.
