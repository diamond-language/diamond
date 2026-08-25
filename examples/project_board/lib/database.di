def build_query_logger(logger, context)
  def log_query(event)
    phase = event["phase"]
    correlation = context["log_context"]
    if correlation == nil
      correlation = {}
    end
    fields = event.merge(correlation).merge({"orm": "active_record", "builder": "arel"})
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
  def self.get(context)
    db = context["db"]
    if db == nil
      connection = SQLite3.open(Database.path())
      connection.execute("PRAGMA foreign_keys = ON")
      connection.execute("PRAGMA journal_mode = WAL")
      connection.execute("PRAGMA busy_timeout = 5000")
      db = ActiveRecord::InstrumentedConnection.new(connection, build_query_logger(AppLogger.get(context), context))
      context["db"] = db
      AppLogger.get(context).info("database.connection.opened", {"adapter": "sqlite3", "database": Database.path(), "environment": AppEnvironment.name(), "foreign_keys": true, "journal_mode": "wal", "busy_timeout_ms": 5000})
    end
    db
  end
end
