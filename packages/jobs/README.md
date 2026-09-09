# jobs

Durable, database-backed background and scheduled work: enqueue a unit
of work now or for later, and run a worker process that claims and
executes it, with retries and a fixed-interval recurring schedule.
Grown from a real gap surfaced by an application (`skindicate.dia`'s
own ingest scripts and moderation-point allocator, both of which
needed exactly this and had nothing to use) rather than written
speculatively -- see `docs/roadmap.md`'s "Grow packages from
application needs."

## Why a durable queue, not an in-memory one

Diamond's concurrency primitives can't back an in-memory job queue
shared across workers: `Thread`s have fully isolated heaps (no shared
mutable objects, deep-copied arguments/results, at most 64 live
threads, no fire-and-forget -- see `docs/threads.md`), and separate
worker *processes* obviously share nothing at all. So the only
coordination point that actually works is the one thing every worker
already has independent access to: the application's own database. A
`jobs` row **is** the queue entry; claiming one is a single atomic
`UPDATE ... WHERE id = (SELECT ...) RETURNING *`, which SQLite
serializes correctly across concurrent writers with no extra locking
needed. This also means a worker process can die and restart (or run
under systemd exactly like a Gremlin server already does) without
losing anything -- the state was never anywhere but the database.

## What this package does not own

Like `ActiveRecord::Migrator` (deliberately raw SQL, no schema
ownership of its own -- see its own module comment), this package
creates no tables. The consuming app's own migrations own this exact
schema:

```sql
CREATE TABLE jobs (
  id INTEGER PRIMARY KEY,
  kind TEXT NOT NULL,
  payload TEXT NOT NULL,
  queue TEXT NOT NULL DEFAULT 'default',
  status TEXT NOT NULL DEFAULT 'pending',
  run_at INTEGER NOT NULL,
  attempts INTEGER NOT NULL DEFAULT 0,
  max_attempts INTEGER NOT NULL DEFAULT 5,
  last_error TEXT,
  locked_at INTEGER,
  created_at INTEGER NOT NULL,
  finished_at INTEGER
);
CREATE INDEX jobs_poll_idx ON jobs(status, queue, run_at);

CREATE TABLE recurring_jobs (
  id INTEGER PRIMARY KEY,
  name TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL,
  payload TEXT NOT NULL,
  queue TEXT NOT NULL DEFAULT 'default',
  every_seconds INTEGER NOT NULL,
  next_run_at INTEGER NOT NULL,
  created_at INTEGER NOT NULL
);
```

`recurring_jobs` is only needed if the app actually uses
`Jobs.schedule_recurring`/`Jobs::Worker.tick_recurring`.

## Usage

```ruby
require "../../jobs/lib/jobs"

# Enqueue -- kind names a handler the app registers below; args is any
# JSON-serializable value.
Jobs.enqueue(db, kind: "send_welcome_email", args: {"user_id": user.id()})
Jobs.enqueue_in(db, 3600, kind: "expire_reservation", args: {"id": reservation.id()})
Jobs.enqueue_at(db, some_epoch_seconds, kind: "publish_post", args: {"id": post.id()})

# A fixed-interval recurring job -- call this unconditionally from the
# app's own boot/configure path; it's a no-op after the first call.
Jobs.schedule_recurring(db, name: "grant_moderation_points", kind: "grant_moderation_points", every_seconds: 3600)

# The app owns its own kind -> Callable[1] handler table -- this
# package never knows what any kind actually does.
def send_welcome_email_handler(args)
  # args["user_id"], deliver an email, whatever the app needs
end

HANDLERS = {"send_welcome_email": send_welcome_email_handler, ...}

# Run one due job (or return false if nothing's due) -- useful from a
# request handler or a script that wants a single pass.
Jobs::Worker.run_once(db, handlers: HANDLERS)

# Run forever -- the actual worker process, typically its own systemd
# unit (`diamond bin/jobs_worker.di`), same shape as skindicate's own
# gremlin server unit. Exits gracefully on SIGTERM/SIGINT, finishing
# whatever job is already in flight first.
Jobs::Worker.run_forever(db, handlers: HANDLERS, poll_interval_seconds: 2)

# Restrict a worker to specific queues (nil/omitted means any queue) --
# useful for giving a slow job kind its own dedicated worker so it
# can't starve the rest.
Jobs::Worker.run_forever(db, handlers: HANDLERS, queues: ["mailers"])
```

## Design notes

- **Claiming is one atomic statement, not select-then-update.** Two
  workers racing for the same row would otherwise both see it pending
  before either claims it; `UPDATE ... WHERE id = (SELECT ... LIMIT 1)
  RETURNING *` closes that window entirely at the database level.
- **Retries are a capped exponential backoff** (5s, 10s, 20s, ... up
  to 300s) tracked via `attempts`/`max_attempts` on the row itself, not
  a separate table. A job that exhausts `max_attempts` becomes
  `status: 'failed'` — a dead letter the app can query
  (`SELECT * FROM jobs WHERE status = 'failed'`), not a UI this package
  provides.
- **An unregistered `kind` fails immediately**, not after retrying —
  no handler ever appearing is a deploy/config mistake, not a
  transient condition retries would fix.
- **Recurring jobs advance `next_run_at` from *now*, not from the old
  `next_run_at`.** A worker that was down for an hour does not fire a
  burst of catch-up jobs the moment it comes back — it just resumes
  the interval from whenever it actually ticks next.
- **Graceful shutdown is `Signal.trap` + `IO.poll([], [], timeout_ms)`
  used as an interruptible sleep** (no dedicated sleep primitive exists
  in Diamond -- `poll(2)` with zero fds and a timeout is the portable
  POSIX idiom for exactly this, and it's already exposed). Shutdown
  latency is bounded by `poll_interval_seconds`: a signal arriving
  mid-sleep is handled, but `IO.poll`'s own EINTR-retry re-polls for
  the *same* timeout again (see `docs/networking.md`'s own
  `IO.poll`/`Signal.trap` interaction section) since there's no fd
  whose state changed to make the retry return early the way closing a
  listener socket would for a server's own accept loop. A short
  interval (1-2s) keeps this bound small; it is not, and does not need
  to be, zero.

## Explicitly out of scope

- **Cron expression syntax.** `every_seconds` is a fixed interval, not
  a schedule language — nothing here currently needs more than that,
  and a real cron parser is its own scope.
- **Priority within a queue.** Jobs run oldest-`run_at`-first within
  whatever queue(s) a worker polls; `queue` itself is the only
  partitioning tool (a slow job kind gets its own queue and worker
  rather than competing for priority against a fast one).
- **Verified multi-worker-process horizontal scaling.** The atomic
  claim is correct under concurrent writers by construction (SQLite's
  own guarantee), but running more than one worker process against the
  same database hasn't been load-tested here — nothing stops it, it
  just isn't the shape this was built and verified against.
- **A dead-letter UI, job history/observability dashboard, or
  argument-schema validation.** `status: 'failed'` rows are plain
  queryable data; building anything on top of them is an application
  concern.

## Testing

`bash test.sh` (or `make test-jobs-package` from the repo root) runs a
self-contained suite against `SQLite3.open(":memory:")` — claim/
succeed, delayed jobs, retry/backoff/permanent-failure, unregistered-
kind failure, queue filtering, and recurring-job scheduling/
idempotency/no-catch-up-burst.
