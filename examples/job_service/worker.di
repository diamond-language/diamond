# One worker owns its DB handle. The HTTP thread has a separate connection.
def job_service_run(db, row, shutdown)
  args = JSON.parse(row["payload"])
  source = Cancellation::Source.new(shutdown, args["timeout_ms"] / 1000.0)
  token = source.token()
  begin
    token.checkpoint()
    if row["attempts"] < args["fail_until"]
      raise RuntimeError.new("demonstration retry")
    end
    completed = 0
    while completed < args["work_ms"]
      token.checkpoint()
      cancelled = db.query("SELECT cancel_requested FROM jobs WHERE id = ?", [row["id"]])[0]["cancel_requested"]
      if cancelled == 1 then source.cancel() end
      token.sleep(0.01)
      completed += 10
    end
    token.checkpoint()
    # The predicate makes a cancellation accepted before this write win.
    db.execute("UPDATE jobs SET status = CASE WHEN cancel_requested = 1 THEN 'cancelled' ELSE 'succeeded' END, result = CASE WHEN cancel_requested = 1 THEN NULL ELSE ? END, finished_at = ?, locked_at = NULL WHERE id = ?", [completed, Time.now().to_i(), row["id"]])
  rescue error: Cancellation::DeadlineExceeded
    JobStore.finish(db, row["id"], "timed_out", error.message())
  rescue error: Cancellation::Cancelled
    cancelled = db.query("SELECT cancel_requested FROM jobs WHERE id = ?", [row["id"]])[0]["cancel_requested"]
    if cancelled == 1
      JobStore.finish(db, row["id"], "cancelled", error.message())
    else
      # Shutdown is not a failed attempt. Retain the job for the next boot.
      db.execute("UPDATE jobs SET status = 'pending', locked_at = NULL WHERE id = ?", [row["id"]])
    end
  rescue error: StandardError
    Jobs::Worker.retry_or_fail!(db, row, error.message())
    db.execute("UPDATE jobs SET status = 'cancelled' WHERE id = ? AND cancel_requested = 1", [row["id"]])
  end
end

def job_service_worker(path, shutdown)
  db = JobStore.open(path)
  begin
    # This service deliberately has one worker/one process per database.
    # A restart reclaims its abandoned rows; handlers must be idempotent.
    db.execute("UPDATE jobs SET status = CASE WHEN cancel_requested = 1 THEN 'cancelled' ELSE 'pending' END, locked_at = NULL WHERE status = 'running'")
    loop do
      shutdown.checkpoint()
      row = Jobs::Worker.claim_next(db, nil, Time.now().to_i())
      if row == nil
        shutdown.sleep(0.02)
      else
        job_service_run(db, row, shutdown)
      end
    end
  rescue error: Cancellation::Cancelled
    nil
  ensure
    db.close()
  end
end
