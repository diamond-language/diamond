module Arel

class Insert
  def initialize(table: Arel::Table, rows = [], returning = [], source_columns = [],
                 source_query = nil, conflict_target = [], conflict_ignore = false,
                 conflict_assignments = nil, ctes = [])
    @table = table
    @rows = rows
    @returning = returning
    @source_columns = source_columns
    @source_query = source_query
    @conflict_target = conflict_target
    @conflict_ignore = conflict_ignore
    @conflict_assignments = conflict_assignments
    @ctes = ctes
  end

  def ctes() = @ctes
  def structure() = [@table, @rows, @returning, @source_columns, @source_query,
    @conflict_target, @conflict_ignore, @conflict_assignments, @ctes]
  def values(attributes: Hash)
    Insert.new(@table, [attributes], @returning, [], nil, @conflict_target,
      @conflict_ignore, @conflict_assignments, @ctes)
  end
  def values_many(rows: Array)
    Insert.new(@table, rows, @returning, [], nil, @conflict_target,
      @conflict_ignore, @conflict_assignments, @ctes)
  end
  def default_values()
    Insert.new(@table, [DefaultValues.new()], @returning, [], nil,
      @conflict_target, @conflict_ignore, @conflict_assignments, @ctes)
  end
  def from_query(columns: Array, query)
    Insert.new(@table, [], @returning, columns, query, @conflict_target,
      @conflict_ignore, @conflict_assignments, @ctes)
  end
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    Insert.new(@table, @rows, @returning, @source_columns, @source_query,
      @conflict_target, @conflict_ignore, @conflict_assignments,
      Arel.append_cte(@ctes, name, query))
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    Insert.new(@table, @rows, @returning, @source_columns, @source_query,
      @conflict_target, @conflict_ignore, @conflict_assignments,
      Arel.append_cte(@ctes, name, query, true))
  end
  def on_conflict_do_nothing(columns = [])
    target = columns
    unless columns is ConflictTarget || columns is ConflictConstraintTarget
      target = arel_array(columns)
    end
    Insert.new(@table, @rows, @returning, @source_columns, @source_query,
      target, true, nil, @ctes)
  end
  def on_conflict_do_update(columns, assignments: Hash)
    target = columns
    unless columns is ConflictTarget || columns is ConflictConstraintTarget
      target = arel_array(columns)
    end
    Insert.new(@table, @rows, @returning, @source_columns, @source_query,
      target, false, assignments, @ctes)
  end
  def returning(expressions)
    Insert.new(@table, @rows, arel_array(expressions), @source_columns, @source_query,
      @conflict_target, @conflict_ignore, @conflict_assignments, @ctes)
  end

  def render_default(visitor) -> Array
    if @ctes.length() > 0
      visitor.require_extension("write CTEs")
    end
    if @rows.length() == 1 && @rows[0] is DefaultValues
      visitor.require_extension("insert default values")
      params = []
      sql = "INSERT INTO #{visitor.quote_identifier(@table.name())} DEFAULT VALUES"
      sql = sql + visitor.render_returning(@returning, params)
      cte_params = []
      sql = visitor.render_ctes(self, cte_params) + sql
      return [sql, cte_params.concat(params)]
    end
    unless @source_query == nil
      if @source_columns.length() == 0
        raise ArgumentError.new("INSERT SELECT requires at least one column")
      end
      unless @source_query.projection_count_known?()
        raise ArgumentError.new("INSERT SELECT requires explicit projections")
      end
      if @source_columns.length() != @source_query.projection_count()
        raise ArgumentError.new("INSERT SELECT columns must match query projections")
      end
      columns = []
      column_index = 0
      while column_index < @source_columns.length()
        columns.push(visitor.quote_identifier(@source_columns[column_index]))
        column_index += 1
      end
      source_sql, params = @source_query.render_with(visitor)
      sql = "INSERT INTO #{visitor.quote_identifier(@table.name())} " +
        "(#{columns.join(", ")}) #{source_sql}"
      sql = sql + Arel.render_insert_conflict(@conflict_target, @conflict_ignore,
        @conflict_assignments, params, visitor)
      sql = sql + visitor.render_returning(@returning, params)
      cte_params = []
      sql = visitor.render_ctes(self, cte_params) + sql
      params = cte_params.concat(params)
      return [sql, params]
    end
    if @rows.length() == 0 || @rows[0].length() == 0
      raise ArgumentError.new("INSERT requires at least one value")
    end
    columns = []
    params = []
    first = @rows[0]
    column_index = 0
    while column_index < first.length()
      columns.push(visitor.quote_identifier(first.key_at(column_index)))
      column_index += 1
    end
    value_groups = []
    row_index = 0
    while row_index < @rows.length()
      row = @rows[row_index]
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
          placeholders.push(visitor.render_expression(value.expression(), params))
        elsif value is ColumnDefault
          visitor.require_extension("per-column default values")
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
    sql = "INSERT INTO #{visitor.quote_identifier(@table.name())} " +
      "(#{columns.join(", ")}) VALUES #{value_groups.join(", ")}"
    sql = sql + Arel.render_insert_conflict(@conflict_target, @conflict_ignore,
      @conflict_assignments, params, visitor)
    sql = sql + visitor.render_returning(@returning, params)
    cte_params = []
    sql = visitor.render_ctes(self, cte_params) + sql
    params = cte_params.concat(params)
    [sql, params]
  end

  def render_with(visitor) -> Array = visitor.render_insert(self)

  def to_sql(visitor = nil) -> Array
    renderer = visitor
    if renderer == nil
      renderer = SQLiteVisitor.new()
    end
    self.render_with(renderer)
  end

  def execute(db, visitor = nil)
    sql, params = self.to_sql(visitor)
    db.execute(sql, params)
  end
  def to_a(db, visitor = nil)
    sql, params = self.to_sql(visitor)
    db.query(sql, params)
  end
end

end
