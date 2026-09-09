# Claiming, running, and retrying jobs -- the only half of this
# package that ever calls into application code. See Jobs (queue.di)
# for enqueueing.
module Jobs

  # A long-running worker process has no shared-heap way to coordinate
  # with other workers (see README.md's own "Why a durable queue, not
  # an in-memory one" -- Diamond threads/processes can't share mutable
  # objects), so the `jobs` row itself is the only coordination point:
  # claiming a job is one atomic UPDATE ... WHERE id = (SELECT ...
  # LIMIT 1) RETURNING *, not a separate SELECT-then-UPDATE (which
  # would let two workers both see the same pending row before either
  # claims it). SQLite serializes concurrent writers on its own; this
  # needs no extra locking beyond that.
  class Worker

    def self.claim_next(db, queues, now)
      queue_filter = ""
      extra_params = []
      if queues != nil && queues.length() > 0
        placeholders = queues.map() do |name| "?" end.join(", ")
        queue_filter = " AND queue IN (#{placeholders})"
        extra_params = queues
      end
      sql = "UPDATE jobs SET status = 'running', locked_at = ? WHERE id = (SELECT id FROM jobs WHERE status = 'pending' AND run_at <= ?#{queue_filter} ORDER BY run_at ASC, id ASC LIMIT 1) RETURNING *"
      params = [now, now].concat(extra_params)
      rows = db.query(sql, params)
      if rows.length() == 0 then nil else rows[0] end
    end

    def self.succeed!(db, row)
      db.execute("UPDATE jobs SET status = 'succeeded', finished_at = ? WHERE id = ?", [Time.now().to_i(), row["id"]])
    end

    # A capped exponential backoff (5s, 10s, 20s, ... capped at 300s) --
    # deliberately not configurable per job in this first slice; see
    # README.md's own "Explicitly out of scope."
    def self.backoff_seconds(attempts)
      seconds = 5
      step = 1
      while step < attempts
        seconds = seconds * 2
        step += 1
      end
      if seconds > 300 then 300 else seconds end
    end

    def self.retry_or_fail!(db, row, error_message)
      attempts = row["attempts"] + 1
      if attempts >= row["max_attempts"]
        db.execute("UPDATE jobs SET status = 'failed', attempts = ?, last_error = ?, finished_at = ? WHERE id = ?",
          [attempts, error_message, Time.now().to_i(), row["id"]])
      else
        next_run_at = Time.now().to_i() + Worker.backoff_seconds(attempts)
        db.execute("UPDATE jobs SET status = 'pending', attempts = ?, last_error = ?, run_at = ? WHERE id = ?",
          [attempts, error_message, next_run_at, row["id"]])
      end
    end

    # No handler registered for a job's own `kind` is a configuration
    # mistake, not a transient failure -- fails immediately (one
    # attempt recorded, for visibility) rather than retrying against
    # a handler that will never appear.
    def self.fail_unhandled!(db, row)
      db.execute("UPDATE jobs SET status = 'failed', attempts = attempts + 1, last_error = ?, finished_at = ? WHERE id = ?",
        ["no handler registered for kind '#{row["kind"]}'", Time.now().to_i(), row["id"]])
    end

    # Claims and runs at most one job. Returns false when there was
    # nothing due to claim (the caller's own signal to back off before
    # polling again -- see #run_forever), true otherwise (whether the
    # job succeeded, failed, or was retried). `handlers` is a Hash of
    # `kind => Callable[1]` the calling app owns -- this package never
    # knows what any `kind` actually does.
    def self.run_once(db, handlers: Hash, queues = nil)
      row = Worker.claim_next(db, queues, Time.now().to_i())
      if row == nil then return false end
      handler = handlers[row["kind"]]
      if handler == nil
        Worker.fail_unhandled!(db, row)
        return true
      end
      args = JSON.parse(row["payload"])
      begin
        handler(args)
        Worker.succeed!(db, row)
      rescue error: StandardError
        Worker.retry_or_fail!(db, row, error.message())
      end
      true
    end

    # A recurring job's own `next_run_at` coming due enqueues one
    # ordinary `jobs` row (so it goes through the exact same claim/
    # run/retry path as anything else) and advances the schedule --
    # advancing from `now`, not from the old `next_run_at`, so a
    # worker that was down for a while doesn't fire a burst of
    # catch-up jobs once it comes back.
    def self.tick_recurring(db, now)
      due = db.query("SELECT * FROM recurring_jobs WHERE next_run_at <= ?", [now])
      due.each() do |row|
        Jobs.enqueue(db, kind: row["kind"], args: JSON.parse(row["payload"]), queue: row["queue"], run_at: now)
        db.execute("UPDATE recurring_jobs SET next_run_at = ? WHERE id = ?", [now + row["every_seconds"], row["id"]])
      end
    end

    # Runs until SIGTERM/SIGINT, matching the graceful-shutdown
    # contract packages/gremlin's own server already has. Shutdown
    # latency is bounded by `poll_interval_seconds` (the IO.poll call
    # below only re-checks the stop flag once its own timeout elapses
    # or a signal wakes it -- see README.md's own note on why a short
    # interval, 1-2s, is worth keeping even though it means more idle
    # polling).
    def self.request_stop()
      @@stop_requested = true
    end

    def self.run_forever(db, handlers: Hash, queues = nil, poll_interval_seconds = 2)
      @@stop_requested = false
      def jobs_worker_stop_handler()
        Worker.request_stop()
      end
      Signal.trap("TERM", jobs_worker_stop_handler)
      Signal.trap("INT", jobs_worker_stop_handler)
      while !@@stop_requested
        worked = Worker.run_once(db, handlers: handlers, queues: queues)
        Worker.tick_recurring(db, Time.now().to_i())
        if !worked
          IO.poll([], [], poll_interval_seconds * 1000)
        end
      end
    end
  end
end
