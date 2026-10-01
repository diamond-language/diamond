# Opens (creating if needed) the SQLite file backing the queue. Called once
# by the HTTP thread and once by the worker, so there are two connections
# to one file.
class JobStore
  def self.open(path)
    db = SQLite3.open(path)

    begin
      # Two connections write concurrently, so tell SQLite how to behave:
      # wait up to 1s for the other side's lock instead of failing with
      # "database is locked", and use write-ahead logging so readers do
      # not block the writer.
      db.execute("PRAGMA busy_timeout = 1000")
      db.execute("PRAGMA journal_mode = WAL")

      # The jobs table. `run_at`/`locked_at`/`attempts`/`last_error` are the
      # retry bookkeeping the `jobs` package uses; `cancel_requested` is
      # this example's own addition (set by POST /jobs/N/cancel and polled
      # by the worker).
      db.execute("CREATE TABLE IF NOT EXISTS jobs (id INTEGER PRIMARY KEY, kind TEXT NOT NULL, payload TEXT NOT NULL, queue TEXT NOT NULL DEFAULT 'default', status TEXT NOT NULL DEFAULT 'pending', run_at INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL DEFAULT 5, last_error TEXT, locked_at INTEGER, created_at INTEGER NOT NULL, finished_at INTEGER, cancel_requested INTEGER NOT NULL DEFAULT 0, result INTEGER)")

      # Lets the worker's "next due pending job" poll avoid a table scan.
      db.execute("CREATE INDEX IF NOT EXISTS jobs_poll_idx ON jobs(status, queue, run_at)")
    rescue error
      # Do not leak the connection if setup fails; surface the original
      # error.
      db.close()
      raise error
    end

    db
  end

  # Moves a job to a terminal `status` (succeeded, cancelled, timed_out,
  # ...), records why, stamps the finish time, and releases the lock.
  def self.finish(db, id, status, message)
    db.execute("UPDATE jobs SET status = ?, last_error = ?, finished_at = ?, locked_at = NULL WHERE id = ?", [status, message, Time.now().to_i(), id])
  end
end
