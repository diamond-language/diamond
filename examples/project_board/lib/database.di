def build_query_logger(logger)
  def log_query(event)
    phase = event["phase"]
    message = "orm=active_record builder=arel query_id=#{event["query_id"]} operation=#{event["operation"]} #{phase} sql=#{event["sql"]} bind_count=#{event["bind_count"]}"
    if phase == "completed"
      count = if event["operation"] == "query" then "rows=#{event["rows"]}" else "affected=#{event["affected"]}" end
      logger.debug("#{message} #{count} duration_ms=#{event["duration_ms"]}")
    elsif phase == "failed"
      logger.error("#{message} duration_ms=#{event["duration_ms"]} error=#{event["error"]}")
    else
      logger.debug(message)
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
      AppLogger.get(context).info("database connection opened foreign_keys=on")
    end
    db
  end
end
