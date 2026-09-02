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

    # Lets this connection participate in Arel::PreparedStatements'
    # caching (packages/arel/lib/arel/prepared_statements.di) instead of
    # being silently opted out of it -- without this, PreparedStatements.for
    # would hit a NoMethodError trying to call #prepare on this Instance
    # (this class predates that cache and never forwarded it), permanently
    # remember this connection as unsupported, and fall back to the
    # original always-uncached path for every query for the rest of the
    # connection's lifetime. Wraps the real Statement in
    # InstrumentedStatement below so a cached call stays just as
    # observable as an uncached #query/#execute call above -- caching only
    # skips the repeat *prepare*, never the per-call event.
    def prepare(sql: String)
      InstrumentedStatement.new(@connection.prepare(sql), @subscriber, sql)
    end

    delegate last_insert_row_id(), to: @connection
    delegate close(), to: @connection
  end

  # The #prepare counterpart to InstrumentedConnection above. `sql` is
  # captured once here (a real Statement's own #query/#execute don't take
  # it, since it's already baked in) so every event this fires still
  # carries the real SQL text, identical in shape to
  # InstrumentedConnection#query/#execute's own events.
  class InstrumentedStatement
    def initialize(statement, subscriber: Callable[1], sql: String)
      @statement = statement
      @subscriber = subscriber
      @sql = sql
    end

    def elapsed_ms(start) = (Time.monotonic() - start) * 1000

    def query(params = nil)
      query_id = SecureRandom.hex(4)
      bind_count = if params == nil then 0 else params.length() end
      @subscriber({"phase": "started", "query_id": query_id, "operation": "query", "sql": @sql, "bind_count": bind_count})
      start = Time.monotonic()
      begin
        rows = @statement.query(params)
      rescue error: StandardError
        @subscriber({"phase": "failed", "query_id": query_id, "operation": "query", "sql": @sql,
                     "bind_count": bind_count, "duration_ms": self.elapsed_ms(start), "error": error.message()})
        raise error
      end
      @subscriber({"phase": "completed", "query_id": query_id, "operation": "query", "sql": @sql,
                   "bind_count": bind_count, "duration_ms": self.elapsed_ms(start), "rows": rows.length()})
      rows
    end

    def execute(params = nil)
      query_id = SecureRandom.hex(4)
      bind_count = if params == nil then 0 else params.length() end
      @subscriber({"phase": "started", "query_id": query_id, "operation": "execute", "sql": @sql, "bind_count": bind_count})
      start = Time.monotonic()
      begin
        affected = @statement.execute(params)
      rescue error: StandardError
        @subscriber({"phase": "failed", "query_id": query_id, "operation": "execute", "sql": @sql,
                     "bind_count": bind_count, "duration_ms": self.elapsed_ms(start), "error": error.message()})
        raise error
      end
      @subscriber({"phase": "completed", "query_id": query_id, "operation": "execute", "sql": @sql,
                   "bind_count": bind_count, "duration_ms": self.elapsed_ms(start), "affected": affected})
      affected
    end

    delegate close(), to: @statement
  end

end
