# jobs

Queue and run durable background jobs using the application database.

## Installation

Install the cut at `cuts/jobs/` and load it with `require_cut "jobs"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

## SQL schema

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

## Usage

```ruby
require_cut "jobs"

Jobs.enqueue(db, kind: "send_welcome_email", args: {"user_id": user.id()})
Jobs.enqueue_in(db, 3600, kind: "expire_reservation", args: {"id": reservation.id()})
Jobs.enqueue_at(db, some_epoch_seconds, kind: "publish_post", args: {"id": post.id()})

Jobs.schedule_recurring(db, name: "grant_moderation_points", kind: "grant_moderation_points", every_seconds: 3600)

def send_welcome_email_handler(args)
  puts(args["user_id"]) # Replace with the job work.
end

HANDLERS = {"send_welcome_email": send_welcome_email_handler}

Jobs::Worker.run_once(db, handlers: HANDLERS)

Jobs::Worker.run_forever(db, handlers: HANDLERS, poll_interval_seconds: 2)
```

## Notes

Create the tables before starting workers. Register a handler for each job kind. `run_once` handles one due job; `run_forever` polls continuously. `recurring_jobs` is needed only for recurring work.
