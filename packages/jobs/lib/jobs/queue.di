# Enqueueing/scheduling only -- no polling, no execution, nothing here
# ever calls a handler. See Jobs::Worker (worker.di) for the half that
# does. Every function here takes `db` explicitly, the same convention
# ActiveRecord::Migrator and every app model already use -- this
# package owns no connection of its own.
module Jobs

  # Inserts one pending row into the app's own `jobs` table (see
  # README.md for the exact schema) -- `kind` names a handler the
  # app's own handler Hash maps to a Callable (see Jobs::Worker), never
  # interpreted by this package itself. `args` is any JSON-
  # serializable value (typically a Hash), round-tripped through
  # JSON.stringify/JSON.parse since SQLite has no native structured
  # column type. `run_at` defaults to now (run as soon as a worker
  # polls); pass an epoch-seconds Int to delay it, or use #enqueue_in/
  # #enqueue_at below instead of computing that yourself.
  def self.enqueue(db, kind: String, args: Hash = {}, queue: String = "default", run_at = nil, max_attempts = 5)
    scheduled_at = if run_at == nil then Time.now().to_i() else run_at end
    db.execute("INSERT INTO jobs (kind, payload, queue, status, run_at, attempts, max_attempts, created_at) VALUES (?, ?, ?, 'pending', ?, 0, ?, ?)",
      [kind, JSON.stringify(args), queue, scheduled_at, max_attempts, Time.now().to_i()])
    db.last_insert_row_id()
  end

  def self.enqueue_in(db, seconds, kind: String, args: Hash = {}, queue: String = "default", max_attempts = 5)
    Jobs.enqueue(db, kind: kind, args: args, queue: queue, run_at: Time.now().to_i() + seconds, max_attempts: max_attempts)
  end

  def self.enqueue_at(db, epoch_seconds, kind: String, args: Hash = {}, queue: String = "default", max_attempts = 5)
    Jobs.enqueue(db, kind: kind, args: args, queue: queue, run_at: epoch_seconds, max_attempts: max_attempts)
  end

  # Registers a fixed-interval recurring job by name -- idempotent, so
  # an app calls this unconditionally from its own boot/configure path
  # (the same "safe to call every time, only the first call does
  # anything" shape ActiveRecord::Migrator.run already has for
  # migrations) rather than needing its own "have I registered this
  # yet" bookkeeping. `every_seconds` is a fixed interval, not a cron
  # expression -- see README.md's own "Explicitly out of scope" on why.
  # A worker's own poll loop enqueues a fresh `jobs` row from this
  # whenever `next_run_at` comes due (Jobs::Worker.tick_recurring).
  def self.schedule_recurring(db, name: String, kind: String, every_seconds, args: Hash = {}, queue: String = "default")
    existing = db.query("SELECT id FROM recurring_jobs WHERE name = ?", [name])
    if existing.length() > 0 then return existing[0]["id"] end
    now = Time.now().to_i()
    db.execute("INSERT INTO recurring_jobs (name, kind, payload, queue, every_seconds, next_run_at, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)",
      [name, kind, JSON.stringify(args), queue, every_seconds, now + every_seconds, now])
    db.last_insert_row_id()
  end
end
