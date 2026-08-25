def build_query_logger(logger)
  def log_query(event)
    phase = event["phase"]
    fields = event.merge({"orm": "active_record", "builder": "arel"})
    if phase == "failed"
      logger.error("database.query.failed", fields)
    else
      logger.debug("database.query.#{phase}", fields)
    end
  end
  log_query
end

class Database
  def self.get(context)
    db = context["db"]
    if db == nil
      connection = SQLite3.open("project_board.db")
      connection.execute("PRAGMA foreign_keys = ON")
      db = ActiveRecord::InstrumentedConnection.new(connection, build_query_logger(AppLogger.get(context)))
      context["db"] = db
      AppLogger.get(context).info("database.connection.opened", {"adapter": "sqlite3", "foreign_keys": true})
    end
    db
  end
end
