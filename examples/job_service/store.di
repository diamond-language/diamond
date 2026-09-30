class JobStore
  def self.open(path)
    db = SQLite3.open(path)
    begin
      db.execute("PRAGMA busy_timeout = 1000")
      db.execute("PRAGMA journal_mode = WAL")
      db.execute("CREATE TABLE IF NOT EXISTS jobs (id INTEGER PRIMARY KEY, kind TEXT NOT NULL, payload TEXT NOT NULL, queue TEXT NOT NULL DEFAULT 'default', status TEXT NOT NULL DEFAULT 'pending', run_at INTEGER NOT NULL, attempts INTEGER NOT NULL DEFAULT 0, max_attempts INTEGER NOT NULL DEFAULT 5, last_error TEXT, locked_at INTEGER, created_at INTEGER NOT NULL, finished_at INTEGER, cancel_requested INTEGER NOT NULL DEFAULT 0, result INTEGER)")
      db.execute("CREATE INDEX IF NOT EXISTS jobs_poll_idx ON jobs(status, queue, run_at)")
    rescue error
      db.close()
      raise error
    end
    db
  end
  def self.finish(db, id, status, message)
    db.execute("UPDATE jobs SET status = ?, last_error = ?, finished_at = ?, locked_at = NULL WHERE id = ?", [status, message, Time.now().to_i(), id])
  end
end
