module Arel

# Verified directly against a live MariaDB 11 server (see
# test_mariadb_dialect.di/.sh), not assumed from similarly-named syntax --
# named "MariaDB" rather than "MySQL" because real divergence was found
# here, and some of what this visitor supports (RETURNING) is a
# MariaDB-only feature real MySQL 8.x doesn't have at all. Unlike
# PostgreSQLVisitor (where nearly everything matched SQLite's own grammar,
# pagination being the one seam), this dialect diverges enough that several
# capabilities needed real per-statement rendering differences rather than
# a plain accept/reject flag -- backtick identifier quoting, a big-sentinel
# LIMIT for offset-only pagination (MariaDB has no bare-OFFSET grammar),
# and its own INSERT rendering entirely (`render_insert`/`render_update`/
# `render_delete` override the shared `render_default` fallback, the
# extension point `Visitor#render_insert` etc. exist for -- see
# VISITORS.md's own "Compound and all three write managers enter the
# selected visitor first" paragraph):
#
# - upsert has no ON CONFLICT syntax at all: `INSERT IGNORE` (do-nothing)
#   and `... ON DUPLICATE KEY UPDATE col = VALUES(col)` (do-update, the
#   `excluded.col` equivalent) instead, and neither takes an explicit
#   conflict target -- MariaDB always resolves against whatever unique/
#   primary key it hits, so ConflictTarget's column list is accepted but
#   unused, while a real target predicate or ConflictConstraintTarget
#   (both requesting something more specific than MariaDB can express) are
#   rejected. `INSERT IGNORE` is also honestly a broader mechanism than
#   `ON CONFLICT ... DO NOTHING`: it suppresses errors for any constraint
#   violation on the statement, not just ones matching a specific target,
#   so this is a deliberately accepted semantic gap, not a hidden one;
# - `RETURNING` genuinely only works on INSERT/DELETE, not UPDATE (a real
#   MariaDB syntax error) -- gated by its own "RETURNING on UPDATE"
#   capability, checked in `render_update` before ever calling the shared
#   `render_returning` (which only knows the single "returning clauses"
#   capability, true here for INSERT/DELETE's sake);
# - bare `INSERT ... DEFAULT VALUES` isn't valid MariaDB syntax --
#   `INSERT INTO t () VALUES ()` is the equivalent this renders instead,
#   under the same "insert default values" capability name so callers
#   don't need to know the two dialects spell it differently.
#
# `conflict-target predicates` (partial-index-style targets) and
# `named-constraint conflict targets` are rejected outright -- MariaDB has
# no equivalent for either. `explicit NULL ordering` (`NULLS FIRST`/`LAST`)
# has no MariaDB syntax at all, unlike both SQLite and PostgreSQL. `write
# CTEs` (`WITH ... INSERT/UPDATE/DELETE`) aren't supported either -- only
# `WITH ... SELECT`, read-side recursive CTEs included, which is why
# `recursive CTEs` stays supported (write statements are already blocked
# earlier by the `write CTEs` rejection, so that combination never reaches
# the recursive check). Quoting, per-column `DEFAULT` in a multi-row
# `VALUES` list, and integer bitwise operators all match SQLite/PostgreSQL
# exactly.
class MariaDBVisitor < Visitor
  def visitor_name() = "MariaDB"
  def quote_identifier(name: String) -> String = arel_quote_identifier_backtick(name)
  def supports_extension?(name: String) -> Bool
    name != "conflict-target predicates" && name != "named-constraint conflict targets" &&
      name != "explicit NULL ordering" && name != "write CTEs" &&
      name != "RETURNING on UPDATE"
  end
  def render_pagination(limit_value, offset_value, params: Array,
                        bind_values = true) -> String
    sql = ""
    if limit_value == nil && offset_value != nil
      # MariaDB's own documented idiom for "no limit" -- unlike SQLite's
      # `LIMIT -1` or PostgreSQL's bare `OFFSET`, MariaDB has no negative-
      # limit or offset-without-limit grammar at all, verified directly
      # (a bare `OFFSET n` is a syntax error). 18446744073709551615 is
      # 2^64-1, an unsigned BIGINT's max value and MariaDB/MySQL's own
      # standard "effectively unlimited" sentinel.
      sql = " LIMIT 18446744073709551615"
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

  def render_expression_extension(expression, params: Array) -> String
    if expression is ExcludedAttribute
      self.require_extension("excluded-row attributes")
      "VALUES(#{self.quote_identifier(expression.name())})"
    else
      super(expression, params)
    end
  end

  # Shared by render_insert's VALUES and INSERT SELECT branches, mirroring
  # Arel.render_insert_conflict's own structure closely (see its comment
  # for the shape this parallels) but rendering MariaDB's own upsert
  # grammar: `ignore` needs no suffix at all here (the `INSERT IGNORE`
  # keyword prefix render_insert already chose does the whole job), so
  # this only ever contributes the `ON DUPLICATE KEY UPDATE` suffix for
  # the do-update case -- but validates the target shape for both, since
  # neither can express a predicate or a named constraint.
  def render_mariadb_upsert(target, ignore: Bool, assignments, params: Array) -> String
    if !ignore && assignments == nil
      return ""
    end
    if target is ConflictConstraintTarget
      self.require_extension("named-constraint conflict targets")
    elsif target is ConflictTarget && target.predicate() != nil
      self.require_extension("conflict-target predicates")
    end
    self.require_extension("upsert conflict actions")
    if ignore
      return ""
    end
    if assignments.length() == 0
      raise ArgumentError.new("conflict update requires at least one assignment")
    end
    rendered_assignments = []
    assignment_index = 0
    while assignment_index < assignments.length()
      name = assignments.key_at(assignment_index)
      value = assignments[name]
      if value is AssignmentValue
        rendered = self.render_expression(value.expression(), params)
        rendered_assignments.push("#{self.quote_identifier(name)} = #{rendered}")
      else
        rendered_assignments.push("#{self.quote_identifier(name)} = ?")
        params.push(value)
      end
      assignment_index += 1
    end
    " ON DUPLICATE KEY UPDATE #{rendered_assignments.join(", ")}"
  end

  def render_insert(statement) -> Array
    table, rows, returning, source_columns, source_query, conflict_target, conflict_ignore, conflict_assignments, ctes = statement.structure()
    if ctes.length() > 0
      self.require_extension("write CTEs")
    end
    if rows.length() == 1 && rows[0] is DefaultValues
      self.require_extension("insert default values")
      params = []
      sql = "INSERT INTO #{self.quote_identifier(table.name())} () VALUES ()"
      sql = sql + self.render_returning(returning, params)
      return [sql, params]
    end
    unless source_query == nil
      if source_columns.length() == 0
        raise ArgumentError.new("INSERT SELECT requires at least one column")
      end
      unless source_query.projection_count_known?()
        raise ArgumentError.new("INSERT SELECT requires explicit projections")
      end
      if source_columns.length() != source_query.projection_count()
        raise ArgumentError.new("INSERT SELECT columns must match query projections")
      end
      columns = []
      column_index = 0
      while column_index < source_columns.length()
        columns.push(self.quote_identifier(source_columns[column_index]))
        column_index += 1
      end
      source_sql, params = source_query.render_with(self)
      keyword = "INSERT INTO"
      if conflict_ignore
        keyword = "INSERT IGNORE INTO"
      end
      sql = "#{keyword} #{self.quote_identifier(table.name())} " +
        "(#{columns.join(", ")}) #{source_sql}"
      sql = sql + self.render_mariadb_upsert(conflict_target, conflict_ignore,
        conflict_assignments, params)
      sql = sql + self.render_returning(returning, params)
      return [sql, params]
    end
    if rows.length() == 0 || rows[0].length() == 0
      raise ArgumentError.new("INSERT requires at least one value")
    end
    columns = []
    params = []
    first = rows[0]
    column_index = 0
    while column_index < first.length()
      columns.push(self.quote_identifier(first.key_at(column_index)))
      column_index += 1
    end
    value_groups = []
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      if row.length() != first.length()
        raise ArgumentError.new("INSERT rows must have identical columns")
      end
      placeholders = []
      index = 0
      while index < first.length()
        key = first.key_at(index)
        unless row.include_key?(key)
          raise ArgumentError.new("INSERT rows must have identical columns")
        end
        value = row[key]
        if value is AssignmentValue
          placeholders.push(self.render_expression(value.expression(), params))
        elsif value is ColumnDefault
          self.require_extension("per-column default values")
          placeholders.push("DEFAULT")
        else
          placeholders.push("?")
          params.push(value)
        end
        index += 1
      end
      value_groups.push("(#{placeholders.join(", ")})")
      row_index += 1
    end
    keyword = "INSERT INTO"
    if conflict_ignore
      keyword = "INSERT IGNORE INTO"
    end
    sql = "#{keyword} #{self.quote_identifier(table.name())} " +
      "(#{columns.join(", ")}) VALUES #{value_groups.join(", ")}"
    sql = sql + self.render_mariadb_upsert(conflict_target, conflict_ignore,
      conflict_assignments, params)
    sql = sql + self.render_returning(returning, params)
    [sql, params]
  end

  def render_update(statement) -> Array
    table, assignments, predicates, returning, allow_all, ctes = statement.structure()
    if ctes.length() > 0
      self.require_extension("write CTEs")
    end
    if assignments == nil || assignments.length() == 0
      raise ArgumentError.new("UPDATE requires at least one assignment")
    end
    if predicates.length() == 0 && !allow_all
      raise ArgumentError.new("UPDATE requires where() or explicit all()")
    end
    clauses = []
    params = []
    assignment_index = 0
    while assignment_index < assignments.length()
      name = assignments.key_at(assignment_index)
      value = assignments[name]
      if value is AssignmentValue
        rendered = self.render_expression(value.expression(), params)
        clauses.push("#{self.quote_identifier(name)} = #{rendered}")
      else
        clauses.push("#{self.quote_identifier(name)} = ?")
        params.push(value)
      end
      assignment_index += 1
    end
    sql = "UPDATE #{self.quote_identifier(table.name())} SET #{clauses.join(", ")}"
    rendered_predicates = []
    predicate_index = 0
    while predicate_index < predicates.length()
      rendered_predicates.push(self.render_expression(predicates[predicate_index], params))
      predicate_index += 1
    end
    if rendered_predicates.length() > 0
      sql = sql + " WHERE " + rendered_predicates.join(" AND ")
    end
    if returning.length() > 0
      self.require_extension("RETURNING on UPDATE")
      sql = sql + self.render_returning(returning, params)
    end
    [sql, params]
  end

  def render_delete(statement) -> Array
    table, predicates, returning, allow_all, ctes = statement.structure()
    if ctes.length() > 0
      self.require_extension("write CTEs")
    end
    if predicates.length() == 0 && !allow_all
      raise ArgumentError.new("DELETE requires where() or explicit all()")
    end
    params = []
    sql = "DELETE FROM #{self.quote_identifier(table.name())}"
    rendered_predicates = []
    predicate_index = 0
    while predicate_index < predicates.length()
      rendered_predicates.push(self.render_expression(predicates[predicate_index], params))
      predicate_index += 1
    end
    if rendered_predicates.length() > 0
      sql = sql + " WHERE " + rendered_predicates.join(" AND ")
    end
    sql = sql + self.render_returning(returning, params)
    [sql, params]
  end
end

end
