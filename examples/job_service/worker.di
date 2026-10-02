# Runs one claimed job to completion. One worker owns its DB handle; the
# HTTP thread has a separate connection.
#
# A job can stop for four different reasons, and each must leave the row in
# the right state (see the `rescue` clauses at the bottom):
#   - it finishes            -> 'succeeded' (or 'cancelled' if asked at the wire)
#   - its own deadline hits  -> 'timed_out'
#   - the user cancels it    -> 'cancelled'
#   - the whole service stops-> back to 'pending' for the next boot
#   - it raises              -> retry, or fail after max_attempts
def job_service_run(db, row, shutdown)
  args = JSON.parse(row["payload"])

  # This job's own token. It is a CHILD of the service-wide `shutdown`
  # token with its own deadline, so it fires on whichever comes first:
  # the job's timeout_ms, an explicit source.cancel(), or service
  # shutdown.
  source = Cancellation::Source.new(shutdown, args["timeout_ms"] / 1000.0)
  token = source.token()

  begin
    token.checkpoint()

    # Demo failure injection: fail the first `fail_until` attempts so the
    # retry path can be seen working.
    if row["attempts"] < args["fail_until"]
      raise RuntimeError.new("demonstration retry")
    end

    # Simulate work in 10ms slices. Each slice is a cancellation point,
    # and also polls the database for a cancel request made over HTTP by
    # the OTHER connection (the only way that thread can reach this one).
    completed = 0
    while completed < args["work_ms"]
      token.checkpoint()
      cancelled = db.query("SELECT cancel_requested FROM jobs WHERE id = ?", [row["id"]])[0]["cancel_requested"]
      if cancelled == 1 then source.cancel() end
      token.sleep(0.01)
      completed += 10
    end

    # Commit the result. One last checkpoint, then a single UPDATE that
    # re-checks `cancel_requested` itself, so a cancel that landed between
    # the final poll above and this write still wins.
    token.checkpoint()
    # The predicate makes a cancellation accepted before this write win.
    db.execute("UPDATE jobs SET status = CASE WHEN cancel_requested = 1 THEN 'cancelled' ELSE 'succeeded' END, result = CASE WHEN cancel_requested = 1 THEN NULL ELSE ? END, finished_at = ?, locked_at = NULL WHERE id = ?", [completed, Time.now().to_i(), row["id"]])

  # The job's own timeout_ms expired. (Listed before `Cancelled`, since a
  # deadline error is also a kind of cancellation.)
  rescue error: Cancellation::DeadlineExceeded
    JobStore.finish(db, row["id"], "timed_out", error.message())

  # Cancelled by something. Ask the database WHY: a user cancel, or the
  # whole service shutting down?
  rescue error: Cancellation::Cancelled
    cancelled = db.query("SELECT cancel_requested FROM jobs WHERE id = ?", [row["id"]])[0]["cancel_requested"]
    if cancelled == 1
      JobStore.finish(db, row["id"], "cancelled", error.message())
    else
      # Shutdown is not a failed attempt. Retain the job for the next boot.
      db.execute("UPDATE jobs SET status = 'pending', locked_at = NULL WHERE id = ?", [row["id"]])
    end

  # The job itself failed. Let the jobs package schedule a retry or mark it
  # failed for good, then honor a cancel that arrived meanwhile (it would
  # otherwise be scheduled to run again).
  rescue error: StandardError
    Jobs::Worker.retry_or_fail!(db, row, error.message())
    db.execute("UPDATE jobs SET status = 'cancelled' WHERE id = ? AND cancel_requested = 1", [row["id"]])
  end
end

# The background worker: runs for the life of the service, claiming and
# running one job at a time. Started under a Supervisor by `main`.
def job_service_worker(path, shutdown)
  db = JobStore.open(path)

  begin
    # This service deliberately has one worker/one process per database.
    # A restart reclaims its abandoned rows; handlers must be idempotent.
    db.execute("UPDATE jobs SET status = CASE WHEN cancel_requested = 1 THEN 'cancelled' ELSE 'pending' END, locked_at = NULL WHERE status = 'running'")

    # Main loop. `shutdown.checkpoint()` raises Cancelled when the service
    # stops, which is the only way out of this `loop`.
    loop do
      shutdown.checkpoint()
      row = Jobs::Worker.claim_next(db, nil, Time.now().to_i())
      if row == nil
        shutdown.sleep(0.02)
      else
        job_service_run(db, row, shutdown)
      end
    end
  # Normal shutdown, not an error.
  rescue error: Cancellation::Cancelled
    nil
  ensure
    db.close()
  end
end
