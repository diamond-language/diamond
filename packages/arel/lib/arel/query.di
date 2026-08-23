module Arel

class Query
  def initialize(table_name, predicates, orderings, limit_value, offset_value,
                 projections, quoted_identifiers, bind_limits, table_alias = nil,
                 distinct_value = false, groups = [], havings = [], joins = [],
                 source_query = nil, correlations = [], ctes = [])
    @table_name = table_name
    @predicates = predicates
    @orderings = orderings
    @limit_value = limit_value
    @offset_value = offset_value
    @projections = projections
    @quoted_identifiers = quoted_identifiers
    @bind_limits = bind_limits
    @table_alias = table_alias
    @distinct_value = distinct_value
    @groups = groups
    @havings = havings
    @joins = joins
    @source_query = source_query
    @correlations = correlations
    @ctes = ctes
  end

  def self.for_table(table: Arel::Table)
    Query.new(table.name(), [], [], nil, nil, [RawSql.new("*", [])], true, true,
      table.table_alias())
  end

  def table_name() = @table_name
  def predicates() = @predicates
  def orderings() = @orderings
  def limit_value() = @limit_value
  def offset_value() = @offset_value
  def projections() = @projections
  def quoted_identifiers() = @quoted_identifiers
  def bind_limits() = @bind_limits
  def table_alias() = @table_alias
  def base_reference_name()
    if @table_alias == nil
      @table_name
    else
      @table_alias
    end
  end
  def distinct_value() = @distinct_value
  def groups() = @groups
  def havings() = @havings
  def joins() = @joins
  def source_query() = @source_query
  def correlations() = @correlations
  def ctes() = @ctes
  def projection_count() = @projections.length()
  def projection_count_known?() -> Bool
    if @projections.length() != 1
      return true
    end
    projection = @projections[0]
    if projection is RawSql
      projection.sql() != "*"
    elsif projection is String
      projection != "*"
    else
      true
    end
  end

  def copy(predicates, orderings, limit_value, offset_value, projections)
    Query.new(@table_name, predicates, orderings, limit_value, offset_value,
      projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations, @ctes)
  end

  def where(condition, params = nil)
    additions = []
    if condition is Hash
      table = Table.new(@table_name)
      index = 0
      while index < condition.length()
        key = condition.key_at(index)
        value = condition[key]
        if @quoted_identifiers
          additions.push(table.column(key).eq(value))
        else
          additions.push(RawSql.new("#{key} = ?", [value]))
        end
        index += 1
      end
    elsif condition is String
      bound = params
      if bound == nil
        bound = []
      end
      additions.push(RawSql.new(condition, bound))
    else
      additions.push(condition)
    end
    self.copy(@predicates.concat(additions), @orderings, @limit_value,
      @offset_value, @projections)
  end

  def project(columns)
    self.copy(@predicates, @orderings, @limit_value, @offset_value, arel_array(columns))
  end
  def select(columns) = self.project(columns)
  def distinct()
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, true, @groups,
      @havings, @joins, @source_query, @correlations, @ctes)
  end
  def group(expressions)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups.concat(arel_array(expressions)), @havings, @joins, @source_query,
      @correlations, @ctes)
  end
  def having(predicate)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings.concat([predicate]), @joins, @source_query,
      @correlations, @ctes)
  end
  def ensure_join_alias_available(table: Arel::Table)
    candidate = table.reference_name()
    if candidate.downcase() == self.base_reference_name().downcase()
      raise ArgumentError.new("duplicate relation alias in query")
    end
    duplicate = false
    index = 0
    while index < @joins.length()
      if @joins[index].table().reference_name().downcase() == candidate.downcase()
        duplicate = true
      end
      index += 1
    end
    if duplicate
      raise ArgumentError.new("duplicate relation alias in query")
    end
  end
  def join(table: Arel::Table, predicate)
    self.ensure_join_alias_available(table)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins.concat([Join.new(table, predicate, "INNER")]),
      @source_query, @correlations, @ctes)
  end
  def left_join(table: Arel::Table, predicate)
    self.ensure_join_alias_available(table)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins.concat([Join.new(table, predicate, "LEFT OUTER")]),
      @source_query, @correlations, @ctes)
  end
  def cross_join(table: Arel::Table)
    self.ensure_join_alias_available(table)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins.concat([Join.new(table, nil, "CROSS")]),
      @source_query, @correlations, @ctes)
  end
  def correlate(table: Arel::Table)
    candidate = table.reference_name()
    if candidate.downcase() == self.base_reference_name().downcase()
      raise ArgumentError.new("correlation must reference an outer relation")
    end
    duplicate = false
    index = 0
    while index < @correlations.length()
      if @correlations[index].reference_name().downcase() == candidate.downcase()
        duplicate = true
      end
      index += 1
    end
    if duplicate
      raise ArgumentError.new("duplicate correlated relation")
    end
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query,
      @correlations.concat([table]), @ctes)
  end
  def correlate_all(tables: Array)
    query = self
    index = 0
    while index < tables.length()
      query = query.correlate(tables[index])
      index += 1
    end
    query
  end
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    self.ensure_cte_name_available(name)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations,
      @ctes.concat([Cte.new(name, query)]))
  end
  def ensure_cte_name_available(name: String)
    duplicate = false
    index = 0
    while index < @ctes.length()
      if @ctes[index].name().downcase() == name.downcase()
        duplicate = true
      end
      index += 1
    end
    if duplicate
      raise ArgumentError.new("duplicate CTE name")
    end
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    self.ensure_cte_name_available(name)
    Query.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations,
      @ctes.concat([Cte.new(name, query, true)]))
  end
  def order(column_or_columns)
    self.copy(@predicates, @orderings.concat(arel_array(column_or_columns)),
      @limit_value, @offset_value, @projections)
  end
  def take(n: Int)
    if n < 0
      raise ArgumentError.new("limit must be non-negative")
    end
    self.copy(@predicates, @orderings, n, @offset_value, @projections)
  end
  def limit(n: Int) = self.take(n)
  def skip(n: Int)
    if n < 0
      raise ArgumentError.new("offset must be non-negative")
    end
    self.copy(@predicates, @orderings, @limit_value, n, @projections)
  end
  def offset(n: Int) = self.skip(n)
  def render_with(visitor) = visitor.render(self)
  def to_sql(visitor = nil)
    renderer = visitor
    if renderer == nil
      renderer = SQLiteVisitor.new()
    end
    self.render_with(renderer)
  end

  def to_a(db, visitor = nil)
    sql, params = self.to_sql(visitor)
    db.query(sql, params)
  end

  def count(db, visitor = nil)
    sql, params = self.to_sql(visitor)
    rows = db.query("SELECT COUNT(*) AS count FROM (#{sql})", params)
    rows[0]["count"]
  end
end

end
