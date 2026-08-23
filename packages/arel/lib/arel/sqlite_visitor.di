module Arel

class SQLiteVisitor < Visitor
  def visitor_name() = "SQLite"
  def quote_identifier(name: String) -> String = arel_quote_identifier(name)
  # Every other capability is genuine SQLite syntax; these two are not --
  # verified directly (both raise a real SQLite syntax error) rather than
  # assumed: a bare DEFAULT in a VALUES row, and ON CONFLICT ON CONSTRAINT
  # naming a constraint directly. SQLite has no equivalent for either.
  def supports_extension?(name: String) -> Bool
    name != "per-column default values" && name != "named-constraint conflict targets"
  end
  def render_pagination(limit_value, offset_value, params: Array,
                        bind_values = true) -> String
    sql = ""
    if limit_value == nil && offset_value != nil
      sql = " LIMIT -1"
    elsif limit_value != nil
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
