# Returns the callback the instrumented connection calls around every SQL
# statement. It is a nested `def` that captures `logger` and `context`, and the
# bare name on the last line hands it back as a value.
def build_query_logger(logger, context)
  # `event["phase"]` says what happened to the statement (started,
  # completed or failed); the phase becomes part of the log event's name.
  def log_query(event)
    phase = event["phase"]

    # Attach the current request's id/method/path, so each SQL line can be
    # tied back to the request that issued it. `log_context` is only set
    # while a request is in flight (see logging_middleware).
    correlation = context["log_context"]
    if correlation == nil
      correlation = {}
    end
    fields = event.merge(correlation).merge({"orm": "active_record", "builder": "arel"})

    # Failures are errors; everything else is debug noise.
    if phase == "failed"
      logger.error("database.query.failed", fields)
    else
      logger.debug("database.query.#{phase}", fields)
    end
  end
  log_query
end

class Database
  def self.path() = AppEnvironment.database_path()

  # One connection per worker thread, opened on first use and cached in
  # `context` (see examples/library/lib/database.di).
  def self.get(context)
    db = context["db"]

    if db == nil
      connection = SQLite3.open(Database.path())

      # SQLite ignores foreign keys unless asked per connection; the schema's
      # ON DELETE CASCADE depends on this. WAL plus a 5s busy timeout lets
      # several workers share the file without "database is locked".
      connection.execute("PRAGMA foreign_keys = ON")
      connection.execute("PRAGMA journal_mode = WAL")
      connection.execute("PRAGMA busy_timeout = 5000")

      # Wrap the raw connection so every statement goes through the logger
      # above.
      db = ActiveRecord::InstrumentedConnection.new(connection, build_query_logger(AppLogger.get(context), context))
      context["db"] = db
      AppLogger.get(context).info("database.connection.opened", {"adapter": "sqlite3", "database": Database.path(), "environment": AppEnvironment.name(), "foreign_keys": true, "journal_mode": "wal", "busy_timeout_ms": 5000})
    end
    db
  end
end
