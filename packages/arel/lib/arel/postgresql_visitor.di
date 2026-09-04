module Arel

  # PostgreSQL's own grammar accepts a bare OFFSET with no LIMIT clause at
  # all, so unlike SQLiteVisitor's render_pagination above, no LIMIT -1
  # sentinel is needed here. Most other capabilities this visitor claims
  # below (identifier quoting, ON CONFLICT, DEFAULT VALUES, RETURNING, CTEs,
  # NULLS FIRST/LAST, integer bitwise operators) use syntax identical to
  # SQLite's own -- both were modeled on Postgres's own SQL to begin with --
  # verified against a live PostgreSQL container in
  # packages/arel/test_postgres_dialect.di, not merely assumed from the
  # similarly-named syntax (see this project's own stated quality bar in
  # ROADMAP.md for why that distinction matters). Two capabilities really
  # are Postgres-only, unlike everything else here: per-column default
  # values in a VALUES row, and named-constraint conflict targets --
  # SQLiteVisitor's own supports_extension? explicitly excludes both.
  class PostgreSQLVisitor < Visitor
    def visitor_name() = "PostgreSQL"
    def quote_identifier(name: String) -> String = arel_quote_identifier(name)
    def supports_extension?(name: String) = true
    def render_pagination(limit_value, offset_value, params: Array,
                          bind_values = true) -> String
      sql = ""
      unless limit_value == nil
        if bind_values
          sql = " LIMIT ?"
          params.push(limit_value)
        else
          sql = " LIMIT #{limit_value}"
        end
      end
      unless offset_value == nil
        if bind_values
          sql = sql + " OFFSET ?"
          params.push(offset_value)
        else
          sql = sql + " OFFSET #{offset_value}"
        end
      end
      sql
    end
  end

end
