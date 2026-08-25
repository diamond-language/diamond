class InstrumentedDatabase
  def initialize(connection, logger)
    @connection = connection
    @logger = logger
  end

  def elapsed_ms(start)
    milliseconds = (Time.monotonic() - start) * 1000
    to_f(to_i(milliseconds * 100)) / 100.0
  end

  def query(sql: String, params: Array = [])
    query_id = SecureRandom.hex(4)
    @logger.debug("orm=active_record builder=arel query_id=#{query_id} operation=query started sql=#{sql} bind_count=#{params.length()}")
    start = Time.monotonic()
    rows = @connection.query(sql, params)
    @logger.debug("orm=active_record builder=arel query_id=#{query_id} operation=query completed rows=#{rows.length()} duration_ms=#{self.elapsed_ms(start)}")
    rows
  end

  def execute(sql: String, params: Array = [])
    query_id = SecureRandom.hex(4)
    @logger.debug("orm=active_record builder=arel query_id=#{query_id} operation=execute started sql=#{sql} bind_count=#{params.length()}")
    start = Time.monotonic()
    affected = @connection.execute(sql, params)
    @logger.debug("orm=active_record builder=arel query_id=#{query_id} operation=execute completed affected=#{affected} duration_ms=#{self.elapsed_ms(start)}")
    affected
  end

  def last_insert_row_id() = @connection.last_insert_row_id()
  def close() = @connection.close()
end

class Database
  def self.get(context)
    db = context["db"]
    if db == nil
      connection = SQLite3.open("project_board.db")
      connection.execute("PRAGMA foreign_keys = ON")
      db = InstrumentedDatabase.new(connection, AppLogger.get(context))
      context["db"] = db
      AppLogger.get(context).info("database connection opened foreign_keys=on")
    end
    db
  end
end
