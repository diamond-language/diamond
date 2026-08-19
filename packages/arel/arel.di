# Immutable SQL AST and SQLite renderer. Query nodes describe intent; only
# ArelSQLiteVisitor knows how that intent becomes SQL.

def arel_array(value)
  if value is Array
    value
  else
    [value]
  end
end

def arel_quote_identifier(name: String) -> String
  pieces = ["\""]
  def append_character(character)
    if character == "\""
      pieces.push("\"")
    end
    pieces.push(character)
  end
  name.chars().each(append_character)
  pieces.push("\"")
  pieces.join()
end

class ArelNot
  def initialize(expression)
    @expression = expression
  end
  def expression() = @expression
end

class ArelLogical
  def initialize(left, operator: String, right)
    @left = left
    @operator = operator
    @right = right
  end
  def left() = @left
  def operator() = @operator
  def right() = @right
  def and_also(other) = ArelLogical.new(self, "AND", other)
  def or_else(other) = ArelLogical.new(self, "OR", other)
  def not_() = ArelNot.new(self)
end

class ArelPredicate
  def initialize(left, operator: String, right)
    @left = left
    @operator = operator
    @right = right
  end
  def left() = @left
  def operator() = @operator
  def right() = @right
  def and_also(other) = ArelLogical.new(self, "AND", other)
  def or_else(other) = ArelLogical.new(self, "OR", other)
  def not_() = ArelNot.new(self)
end

class ArelMembership
  def initialize(left, values, negated: Bool)
    @left = left
    @values = values
    @negated = negated
  end
  def left() = @left
  def values() = @values
  def negated?() = @negated
  def and_also(other) = ArelLogical.new(self, "AND", other)
  def or_else(other) = ArelLogical.new(self, "OR", other)
  def not_() = ArelNot.new(self)
end

class ArelBetween
  def initialize(left, lower, upper, negated: Bool)
    @left = left
    @lower = lower
    @upper = upper
    @negated = negated
  end
  def left() = @left
  def lower() = @lower
  def upper() = @upper
  def negated?() = @negated
  def and_also(other) = ArelLogical.new(self, "AND", other)
  def or_else(other) = ArelLogical.new(self, "OR", other)
  def not_() = ArelNot.new(self)
end

class ArelCollation
  def initialize(expression, name: String)
    @expression = expression
    @name = name
  end
  def expression() = @expression
  def name() = @name
end

class ArelFunction
  def initialize(name: String, arguments: Array, distinct = false)
    @name = name
    @arguments = arguments
    @distinct = distinct
  end
  def name() = @name
  def arguments() = @arguments
  def distinct?() = @distinct
  def eq(value) = ArelPredicate.new(self, "=", value)
  def not_eq(value) = ArelPredicate.new(self, "!=", value)
  def lt(value) = ArelPredicate.new(self, "<", value)
  def lteq(value) = ArelPredicate.new(self, "<=", value)
  def gt(value) = ArelPredicate.new(self, ">", value)
  def gteq(value) = ArelPredicate.new(self, ">=", value)
end

class ArelOrdering
  def initialize(expression, direction: String, nulls = nil)
    @expression = expression
    @direction = direction
    @nulls = nulls
  end
  def expression() = @expression
  def direction() = @direction
  def nulls() = @nulls
  def nulls_first() = ArelOrdering.new(@expression, @direction, "FIRST")
  def nulls_last() = ArelOrdering.new(@expression, @direction, "LAST")
end

class ArelAlias
  def initialize(expression, name: String)
    @expression = expression
    @name = name
  end
  def expression() = @expression
  def name() = @name
end

class ArelAttribute
  def initialize(table, name: String)
    @table = table
    @name = name
  end
  def table() = @table
  def name() = @name
  def eq(value) = ArelPredicate.new(self, "=", value)
  def not_eq(value) = ArelPredicate.new(self, "!=", value)
  def lt(value) = ArelPredicate.new(self, "<", value)
  def lteq(value) = ArelPredicate.new(self, "<=", value)
  def gt(value) = ArelPredicate.new(self, ">", value)
  def gteq(value) = ArelPredicate.new(self, ">=", value)
  def like(pattern: String) = ArelPredicate.new(self, "LIKE", pattern)
  def not_like(pattern: String) = ArelPredicate.new(self, "NOT LIKE", pattern)
  def in_list(values: Array) = ArelMembership.new(self, values, false)
  def not_in(values: Array) = ArelMembership.new(self, values, true)
  def in_subquery(query) = ArelMembership.new(self, query, false)
  def not_in_subquery(query) = ArelMembership.new(self, query, true)
  def between(lower, upper) = ArelBetween.new(self, lower, upper, false)
  def not_between(lower, upper) = ArelBetween.new(self, lower, upper, true)
  def asc() = ArelOrdering.new(self, "ASC")
  def desc() = ArelOrdering.new(self, "DESC")
  def as(name: String) = ArelAlias.new(self, name)
  def collate(name: String) = ArelCollation.new(self, name)
end

class ArelQualifiedStar
  def initialize(table)
    @table = table
  end
  def table() = @table
end

class ArelTable
  def initialize(name: String, table_alias = nil)
    @name = name
    @table_alias = table_alias
  end
  def name() = @name
  def table_alias() = @table_alias
  def reference_name()
    if @table_alias == nil
      @name
    else
      @table_alias
    end
  end
  def as(name: String) = ArelTable.new(@name, name)
  def column(name: String) = ArelAttribute.new(self, name)
  def star() = ArelQualifiedStar.new(self)
end

class ArelRawSql
  def initialize(sql: String, params: Array)
    @sql = sql
    @params = params
  end
  def sql() = @sql
  def params() = @params
  def and_also(other) = ArelLogical.new(self, "AND", other)
  def or_else(other) = ArelLogical.new(self, "OR", other)
  def not_() = ArelNot.new(self)
end

class ArelJoin
  def initialize(table: ArelTable, predicate, kind: String)
    @table = table
    @predicate = predicate
    @kind = kind
  end
  def table() = @table
  def predicate() = @predicate
  def kind() = @kind
end

class ArelExists
  def initialize(query, negated: Bool)
    @query = query
    @negated = negated
  end
  def query() = @query
  def negated?() = @negated
  def and_also(other) = ArelLogical.new(self, "AND", other)
  def or_else(other) = ArelLogical.new(self, "OR", other)
  def not_() = ArelExists.new(@query, !@negated)
end

class ArelScalarSubquery
  def initialize(query)
    @query = query
  end
  def query() = @query
  def eq(value) = ArelPredicate.new(self, "=", value)
  def not_eq(value) = ArelPredicate.new(self, "!=", value)
  def lt(value) = ArelPredicate.new(self, "<", value)
  def lteq(value) = ArelPredicate.new(self, "<=", value)
  def gt(value) = ArelPredicate.new(self, ">", value)
  def gteq(value) = ArelPredicate.new(self, ">=", value)
end

class ArelCte
  def initialize(name: String, query)
    @name = name
    @query = query
  end
  def name() = @name
  def query() = @query
end

class ArelSQLiteVisitor
  def attribute_allowed?(attribute: ArelAttribute) -> Bool
    if @query == nil
      true
    elsif attribute.table().reference_name() == @query.base_reference_name()
      true
    else
      found = false
      reference_name = attribute.table().reference_name()
      def check_join(join)
        if join.table().reference_name() == reference_name
          found = true
        end
      end
      @query.joins().each(check_join)
      def check_correlation(table)
        if table.reference_name() == reference_name
          found = true
        end
      end
      @query.correlations().each(check_correlation)
      found
    end
  end

  def render_attribute(attribute: ArelAttribute) -> String
    if !self.attribute_allowed?(attribute)
      raise ArgumentError.new("attribute belongs to a relation outside this query")
    end
    arel_quote_identifier(attribute.table().reference_name()) + "." + arel_quote_identifier(attribute.name())
  end

  def render_table(table: ArelTable) -> String
    sql = arel_quote_identifier(table.name())
    if table.table_alias() != nil
      sql = sql + " AS " + arel_quote_identifier(table.table_alias())
    end
    sql
  end

  def render_source(query, params: Array) -> String
    if query.source_query() != nil
      source_sql, source_params = query.source_query().to_sql()
      def append_source_param(value)
        params.push(value)
      end
      source_params.each(append_source_param)
      "(#{source_sql}) AS #{arel_quote_identifier(query.base_reference_name())}"
    else
      sql = query.table_name()
      if query.quoted_identifiers()
        sql = arel_quote_identifier(sql)
        if query.table_alias() != nil
          sql = sql + " AS " + arel_quote_identifier(query.table_alias())
        end
      end
      sql
    end
  end

  def render_ctes(query, params: Array) -> String
    entries = []
    def render_cte(cte)
      sql, bound = cte.query().to_sql()
      def append_cte_param(value)
        params.push(value)
      end
      bound.each(append_cte_param)
      entries.push("#{arel_quote_identifier(cte.name())} AS (#{sql})")
    end
    query.ctes().each(render_cte)
    if entries.length() == 0
      ""
    else
      "WITH #{entries.join(", ")} "
    end
  end

  def render_expression(expression, params: Array) -> String
    if expression is ArelAttribute
      self.render_attribute(expression)
    elsif expression is ArelPredicate
      left = self.render_expression(expression.left(), params)
      value = expression.right()
      if value == nil
        if expression.operator() == "="
          "#{left} IS NULL"
        elsif expression.operator() == "!="
          "#{left} IS NOT NULL"
        else
          raise ArgumentError.new("nil only supports eq/not_eq predicates")
        end
      elsif value is ArelAttribute
        "#{left} #{expression.operator()} #{self.render_attribute(value)}"
      else
        params.push(value)
        "#{left} #{expression.operator()} ?"
      end
    elsif expression is ArelLogical
      left = self.render_expression(expression.left(), params)
      right = self.render_expression(expression.right(), params)
      "(#{left} #{expression.operator()} #{right})"
    elsif expression is ArelMembership
      if !(expression.values() is Array)
        subquery_sql, subquery_params = expression.values().to_sql()
        def append_membership_param(value)
          params.push(value)
        end
        subquery_params.each(append_membership_param)
        operator = "IN"
        if expression.negated?()
          operator = "NOT IN"
        end
        "#{self.render_attribute(expression.left())} #{operator} (#{subquery_sql})"
      elsif expression.values().length() == 0
        if expression.negated?()
          "1 = 1"
        else
          "1 = 0"
        end
      else
        placeholders = []
        def bind_member(value)
          params.push(value)
          placeholders.push("?")
        end
        expression.values().each(bind_member)
        operator = "IN"
        if expression.negated?()
          operator = "NOT IN"
        end
        "#{self.render_attribute(expression.left())} #{operator} (#{placeholders.join(", ")})"
      end
    elsif expression is ArelBetween
      params.push(expression.lower())
      params.push(expression.upper())
      operator = "BETWEEN"
      if expression.negated?()
        operator = "NOT BETWEEN"
      end
      "#{self.render_attribute(expression.left())} #{operator} ? AND ?"
    elsif expression is ArelCollation
      inner = self.render_expression(expression.expression(), params)
      "#{inner} COLLATE #{arel_quote_identifier(expression.name())}"
    elsif expression is ArelExists
      sql, bound = expression.query().to_sql()
      def append_exists_param(value)
        params.push(value)
      end
      bound.each(append_exists_param)
      prefix = "EXISTS"
      if expression.negated?()
        prefix = "NOT EXISTS"
      end
      "#{prefix} (#{sql})"
    elsif expression is ArelScalarSubquery
      sql, bound = expression.query().to_sql()
      def append_scalar_param(value)
        params.push(value)
      end
      bound.each(append_scalar_param)
      "(#{sql})"
    else
      self.render_expression_tail(expression, params)
    end
  end

  # Keep the nominal narrowing chain in each method below Diamond's
  # eight-alternative union ceiling as the AST grows.
  def render_expression_tail(expression, params: Array) -> String
    if expression is ArelFunction
      arguments = []
      visitor = self
      def render_argument(argument)
        arguments.push(visitor.render_expression(argument, params))
      end
      expression.arguments().each(render_argument)
      prefix = ""
      if expression.distinct?()
        prefix = "DISTINCT "
      end
      "#{expression.name()}(#{prefix}#{arguments.join(", ")})"
    elsif expression is ArelQualifiedStar
      if !self.attribute_allowed?(ArelAttribute.new(expression.table(), "*"))
        raise ArgumentError.new("wildcard belongs to a relation outside this query")
      end
      arel_quote_identifier(expression.table().reference_name()) + ".*"
    elsif expression is ArelNot
      inner = self.render_expression(expression.expression(), params)
      "(NOT #{inner})"
    elsif expression is ArelOrdering
      sql = "#{self.render_expression(expression.expression(), params)} #{expression.direction()}"
      if expression.nulls() != nil
        sql = sql + " NULLS #{expression.nulls()}"
      end
      sql
    elsif expression is ArelAlias
      inner = self.render_expression(expression.expression(), params)
      "#{inner} AS #{arel_quote_identifier(expression.name())}"
    elsif expression is ArelRawSql
      def append_param(value)
        params.push(value)
      end
      expression.params().each(append_param)
      expression.sql()
    elsif expression is String
      expression
    else
      raise TypeError.new("unsupported Arel expression")
    end
  end

  def render(query) -> Array
    @query = query
    visitor = self
    params = []
    sql = self.render_ctes(query, params)
    projections = []
    def render_projection(projection)
      projections.push(visitor.render_expression(projection, params))
    end
    query.projections().each(render_projection)
    table_sql = self.render_source(query, params)
    def render_join(join)
      table_sql = table_sql + " #{join.kind()} JOIN #{visitor.render_table(join.table())}"
      if join.predicate() != nil
        table_sql = table_sql + " ON " + visitor.render_expression(join.predicate(), params)
      end
    end
    query.joins().each(render_join)
    select_keyword = "SELECT "
    if query.distinct_value()
      select_keyword = "SELECT DISTINCT "
    end
    sql = sql + select_keyword + projections.join(", ") + " FROM #{table_sql}"

    predicates = []
    def render_predicate(predicate)
      predicates.push(visitor.render_expression(predicate, params))
    end
    query.predicates().each(render_predicate)
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
    end

    groups = []
    def render_group(group)
      groups.push(visitor.render_expression(group, params))
    end
    query.groups().each(render_group)
    if groups.length() > 0
      sql = sql + " GROUP BY " + groups.join(", ")
    end

    havings = []
    def render_having(having)
      havings.push(visitor.render_expression(having, params))
    end
    query.havings().each(render_having)
    if havings.length() > 0
      sql = sql + " HAVING " + havings.join(" AND ")
    end

    orderings = []
    def render_ordering(ordering)
      orderings.push(visitor.render_expression(ordering, params))
    end
    query.orderings().each(render_ordering)
    if orderings.length() > 0
      sql = sql + " ORDER BY " + orderings.join(", ")
    end

    if query.limit_value() != nil
      if query.bind_limits()
        sql = sql + " LIMIT ?"
        params.push(query.limit_value())
      else
        sql = sql + " LIMIT #{query.limit_value()}"
      end
    end
    if query.offset_value() != nil
      if query.bind_limits()
        sql = sql + " OFFSET ?"
        params.push(query.offset_value())
      else
        sql = sql + " OFFSET #{query.offset_value()}"
      end
    end
    [sql, params]
  end
end

class ArelQuery
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

  def self.for_table(table: ArelTable)
    ArelQuery.new(table.name(), [], [], nil, nil, [ArelRawSql.new("*", [])], true, true,
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

  def copy(predicates, orderings, limit_value, offset_value, projections)
    ArelQuery.new(@table_name, predicates, orderings, limit_value, offset_value,
      projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations, @ctes)
  end

  def where(condition, params = nil)
    additions = []
    if condition is Hash
      table = ArelTable.new(@table_name)
      quoted_identifiers = @quoted_identifiers
      def add_equality(key, value)
        if quoted_identifiers
          additions.push(table.column(key).eq(value))
        else
          additions.push(ArelRawSql.new("#{key} = ?", [value]))
        end
      end
      condition.each(add_equality)
    elsif condition is String
      bound = params
      if bound == nil
        bound = []
      end
      additions.push(ArelRawSql.new(condition, bound))
    else
      additions.push(condition)
    end
    self.copy(array_concat(@predicates, additions), @orderings, @limit_value,
      @offset_value, @projections)
  end

  def project(columns)
    self.copy(@predicates, @orderings, @limit_value, @offset_value, arel_array(columns))
  end
  def select(columns) = self.project(columns)
  def distinct()
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, true, @groups,
      @havings, @joins, @source_query, @correlations, @ctes)
  end
  def group(expressions)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      array_concat(@groups, arel_array(expressions)), @havings, @joins, @source_query,
      @correlations, @ctes)
  end
  def having(predicate)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, array_concat(@havings, [predicate]), @joins, @source_query,
      @correlations, @ctes)
  end
  def ensure_join_alias_available(table: ArelTable)
    candidate = table.reference_name()
    if candidate == self.base_reference_name()
      raise ArgumentError.new("duplicate relation alias in query")
    end
    duplicate = false
    def check_existing_join(join)
      if join.table().reference_name() == candidate
        duplicate = true
      end
    end
    @joins.each(check_existing_join)
    if duplicate
      raise ArgumentError.new("duplicate relation alias in query")
    end
  end
  def join(table: ArelTable, predicate)
    self.ensure_join_alias_available(table)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, array_concat(@joins, [ArelJoin.new(table, predicate, "INNER")]),
      @source_query, @correlations, @ctes)
  end
  def left_join(table: ArelTable, predicate)
    self.ensure_join_alias_available(table)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, array_concat(@joins, [ArelJoin.new(table, predicate, "LEFT OUTER")]),
      @source_query, @correlations, @ctes)
  end
  def cross_join(table: ArelTable)
    self.ensure_join_alias_available(table)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, array_concat(@joins, [ArelJoin.new(table, nil, "CROSS")]),
      @source_query, @correlations, @ctes)
  end
  def correlate(table: ArelTable)
    candidate = table.reference_name()
    if candidate == self.base_reference_name()
      raise ArgumentError.new("correlation must reference an outer relation")
    end
    duplicate = false
    def check_correlation(table)
      if table.reference_name() == candidate
        duplicate = true
      end
    end
    @correlations.each(check_correlation)
    if duplicate
      raise ArgumentError.new("duplicate correlated relation")
    end
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query,
      array_concat(@correlations, [table]), @ctes)
  end
  def correlate_all(tables: Array)
    query = self
    index = 0
    while index < tables.length()
      query = query.correlate(tables[index])
      index = index + 1
    end
    query
  end
  def with(name: String, query)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations,
      array_concat(@ctes, [ArelCte.new(name, query)]))
  end
  def order(column_or_columns)
    self.copy(@predicates, array_concat(@orderings, arel_array(column_or_columns)),
      @limit_value, @offset_value, @projections)
  end
  def take(n: Int) = self.copy(@predicates, @orderings, n, @offset_value, @projections)
  def limit(n: Int) = self.take(n)
  def skip(n: Int) = self.copy(@predicates, @orderings, @limit_value, n, @projections)
  def offset(n: Int) = self.skip(n)
  def to_sql() = ArelSQLiteVisitor.new().render(self)

  def to_a(db)
    sql, params = self.to_sql()
    db.query(sql, params)
  end

  def count(db)
    sql, params = self.to_sql()
    rows = db.query("SELECT COUNT(*) AS count FROM (#{sql})", params)
    rows[0]["count"]
  end
end

class Arel
  def self.table(name: String) = ArelTable.new(name)
  def self.as(expression, name: String) = ArelAlias.new(expression, name)
  def self.asc(expression) = ArelOrdering.new(expression, "ASC")
  def self.desc(expression) = ArelOrdering.new(expression, "DESC")
  def self.sql(fragment: String, params = nil)
    bound = params
    if bound == nil
      bound = []
    end
    ArelRawSql.new(fragment, bound)
  end
  def self.count(expression) = ArelFunction.new("COUNT", [expression])
  def self.count_distinct(expression) = ArelFunction.new("COUNT", [expression], true)
  def self.sum(expression) = ArelFunction.new("SUM", [expression])
  def self.min(expression) = ArelFunction.new("MIN", [expression])
  def self.max(expression) = ArelFunction.new("MAX", [expression])
  def self.avg(expression) = ArelFunction.new("AVG", [expression])
  def self.lower(expression) = ArelFunction.new("LOWER", [expression])
  def self.upper(expression) = ArelFunction.new("UPPER", [expression])
  def self.exists(query) = ArelExists.new(query, false)
  def self.not_exists(query) = ArelExists.new(query, true)
  def self.scalar(query) = ArelScalarSubquery.new(query)
  def self.from_subquery(query, name: String)
    ArelQuery.new(name, [], [], nil, nil, [ArelRawSql.new("*", [])], true, true,
      name, false, [], [], [], query)
  end
  def self.from(table)
    if table is ArelTable
      ArelQuery.for_table(table)
    else
      ArelQuery.new(table, [], [], nil, nil, ["*"], false, false)
    end
  end
end
