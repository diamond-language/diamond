module ActiveRecord

# Transparent database-connection decorator for ActiveRecord/Arel execution.
# The subscriber receives Hash events; bind values are deliberately omitted.
class InstrumentedConnection
  def initialize(connection, subscriber: Callable[1])
    @connection = connection
    @subscriber = subscriber
  end

  def elapsed_ms(start) = (Time.monotonic() - start) * 1000

  def query(sql: String, params: Array = [])
    query_id = SecureRandom.hex(4)
    @subscriber({"phase": "started", "query_id": query_id, "operation": "query", "sql": sql, "bind_count": params.length()})
    start = Time.monotonic()
    begin
      rows = @connection.query(sql, params)
    rescue error: StandardError
      @subscriber({"phase": "failed", "query_id": query_id, "operation": "query", "sql": sql,
                   "bind_count": params.length(), "duration_ms": self.elapsed_ms(start), "error": error.message()})
      raise error
    end
    @subscriber({"phase": "completed", "query_id": query_id, "operation": "query", "sql": sql,
                 "bind_count": params.length(), "duration_ms": self.elapsed_ms(start), "rows": rows.length()})
    rows
  end

  def execute(sql: String, params: Array = [])
    query_id = SecureRandom.hex(4)
    @subscriber({"phase": "started", "query_id": query_id, "operation": "execute", "sql": sql, "bind_count": params.length()})
    start = Time.monotonic()
    begin
      affected = @connection.execute(sql, params)
    rescue error: StandardError
      @subscriber({"phase": "failed", "query_id": query_id, "operation": "execute", "sql": sql,
                   "bind_count": params.length(), "duration_ms": self.elapsed_ms(start), "error": error.message()})
      raise error
    end
    @subscriber({"phase": "completed", "query_id": query_id, "operation": "execute", "sql": sql,
                 "bind_count": params.length(), "duration_ms": self.elapsed_ms(start), "affected": affected})
    affected
  end

  def last_insert_row_id() = @connection.last_insert_row_id()
  def close() = @connection.close()
end

end
