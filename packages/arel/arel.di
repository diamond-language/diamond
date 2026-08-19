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

class ArelBinaryExpression
  def initialize(left, operator: String, right, bind_right = true)
    @left = left
    @operator = operator
    @right = right
    @bind_right = bind_right
  end
  def left() = @left
  def operator() = @operator
  def right() = @right
  def bind_right?() = @bind_right
  def add(value) = ArelBinaryExpression.new(self, "+", value)
  def subtract(value) = ArelBinaryExpression.new(self, "-", value)
  def multiply(value) = ArelBinaryExpression.new(self, "*", value)
  def divide(value) = ArelBinaryExpression.new(self, "/", value)
  def add_expression(expression) = ArelBinaryExpression.new(self, "+", expression, false)
  def subtract_expression(expression) = ArelBinaryExpression.new(self, "-", expression, false)
  def multiply_expression(expression) = ArelBinaryExpression.new(self, "*", expression, false)
  def divide_expression(expression) = ArelBinaryExpression.new(self, "/", expression, false)
end

class ArelLiteral
  def initialize(value)
    if !(value is Int) && !(value is Bool)
      raise ArgumentError.new("SQL literals only support Int and Bool values")
    end
    @value = value
  end
  def value() = @value
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
  def add(value) = ArelBinaryExpression.new(self, "+", value)
  def subtract(value) = ArelBinaryExpression.new(self, "-", value)
  def multiply(value) = ArelBinaryExpression.new(self, "*", value)
  def divide(value) = ArelBinaryExpression.new(self, "/", value)
  def add_expression(expression) = ArelBinaryExpression.new(self, "+", expression, false)
  def subtract_expression(expression) = ArelBinaryExpression.new(self, "-", expression, false)
  def multiply_expression(expression) = ArelBinaryExpression.new(self, "*", expression, false)
  def divide_expression(expression) = ArelBinaryExpression.new(self, "/", expression, false)
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

def arel_cte_name(value) -> String
  if value is String
    value
  else
    value.name()
  end
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

class ArelExcludedAttribute
  def initialize(name: String)
    @name = name
  end
  def name() = @name
  def add(value) = ArelBinaryExpression.new(self, "+", value)
  def subtract(value) = ArelBinaryExpression.new(self, "-", value)
  def multiply(value) = ArelBinaryExpression.new(self, "*", value)
  def divide(value) = ArelBinaryExpression.new(self, "/", value)
  def add_expression(expression) = ArelBinaryExpression.new(self, "+", expression, false)
  def subtract_expression(expression) = ArelBinaryExpression.new(self, "-", expression, false)
  def multiply_expression(expression) = ArelBinaryExpression.new(self, "*", expression, false)
  def divide_expression(expression) = ArelBinaryExpression.new(self, "/", expression, false)
end

class ArelConflictAttribute
  def initialize(name: String)
    @name = name
  end
  def name() = @name
  def eq(value) = ArelPredicate.new(self, "=", value)
  def not_eq(value) = ArelPredicate.new(self, "!=", value)
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
  def initialize(name: String, query, recursive = false)
    @name = name
    @query = query
    @recursive = recursive
  end
  def name() = @name
  def query() = @query
  def recursive?() = @recursive
end

def arel_append_cte(ctes: Array, name: String, query, recursive = false) -> Array
  duplicate = false
  def check_cte(cte)
    if cte.name() == name
      duplicate = true
    end
  end
  ctes.each(check_cte)
  if duplicate
    raise ArgumentError.new("duplicate CTE name")
  end
  array_concat(ctes, [ArelCte.new(name, query, recursive)])
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
      source_sql, source_params = query.source_query().render_with(self)
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
    recursive = false
    index = 0
    while index < query.ctes().length()
      cte = query.ctes()[index]
      if cte.recursive?()
        recursive = true
      end
      sql, bound = cte.query().render_with(self)
      bound_index = 0
      while bound_index < bound.length()
        params.push(bound[bound_index])
        bound_index = bound_index + 1
      end
      entries.push("#{arel_quote_identifier(cte.name())} AS (#{sql})")
      index = index + 1
    end
    if entries.length() == 0
      ""
    else
      prefix = "WITH "
      if recursive
        prefix = "WITH RECURSIVE "
      end
      "#{prefix}#{entries.join(", ")} "
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
      elsif value is ArelLiteral
        "#{left} #{expression.operator()} #{self.render_expression(value, params)}"
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
        subquery_sql, subquery_params = expression.values().render_with(self)
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
      sql, bound = expression.query().render_with(self)
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
      sql, bound = expression.query().render_with(self)
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
    else
      self.render_expression_extension(expression, params)
    end
  end

  def render_expression_extension(expression, params: Array) -> String
    if expression is ArelExcludedAttribute
      "excluded.#{arel_quote_identifier(expression.name())}"
    elsif expression is ArelConflictAttribute
      arel_quote_identifier(expression.name())
    elsif expression is ArelBinaryExpression
      left = self.render_expression(expression.left(), params)
      right = ""
      if expression.bind_right?()
        params.push(expression.right())
        right = "?"
      else
        right = self.render_expression(expression.right(), params)
      end
      "(#{left} #{expression.operator()} #{right})"
    elsif expression is ArelLiteral
      if expression.value() is Bool
        if expression.value()
          "TRUE"
        else
          "FALSE"
        end
      else
        "#{expression.value()}"
      end
    elsif expression is String
      expression
    else
      raise TypeError.new("unsupported Arel expression")
    end
  end

  def render(query) -> Array
    previous_query = @query
    visitor = self
    params = []
    sql = self.render_ctes(query, params)
    @query = query
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
    sql = sql + "SELECT "
    if query.distinct_value()
      sql = sql + "DISTINCT "
    end
    sql = sql + projections.join(", ") + " FROM #{table_sql}"

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
    @query = previous_query
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
  def projection_count() = @projections.length()
  def projection_count_known?() -> Bool
    if @projections.length() != 1
      return true
    end
    projection = @projections[0]
    if projection is ArelRawSql
      projection.sql() != "*"
    elsif projection is String
      projection != "*"
    else
      true
    end
  end

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
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    self.ensure_cte_name_available(name)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations,
      array_concat(@ctes, [ArelCte.new(name, query)]))
  end
  def ensure_cte_name_available(name: String)
    duplicate = false
    def check_cte(cte)
      if cte.name() == name
        duplicate = true
      end
    end
    @ctes.each(check_cte)
    if duplicate
      raise ArgumentError.new("duplicate CTE name")
    end
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    self.ensure_cte_name_available(name)
    ArelQuery.new(@table_name, @predicates, @orderings, @limit_value, @offset_value,
      @projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value,
      @groups, @havings, @joins, @source_query, @correlations,
      array_concat(@ctes, [ArelCte.new(name, query, true)]))
  end
  def order(column_or_columns)
    self.copy(@predicates, array_concat(@orderings, arel_array(column_or_columns)),
      @limit_value, @offset_value, @projections)
  end
  def take(n: Int) = self.copy(@predicates, @orderings, n, @offset_value, @projections)
  def limit(n: Int) = self.take(n)
  def skip(n: Int) = self.copy(@predicates, @orderings, @limit_value, n, @projections)
  def offset(n: Int) = self.skip(n)
  def render_with(visitor) = visitor.render(self)
  def to_sql(visitor = nil)
    renderer = visitor
    if renderer == nil
      renderer = ArelSQLiteVisitor.new()
    end
    self.render_with(renderer)
  end

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

class ArelCompoundQuery
  def initialize(left, operator: String, right, orderings = [], limit_value = nil,
                 offset_value = nil)
    if !left.projection_count_known?() || !right.projection_count_known?()
      raise ArgumentError.new("compound queries require explicit projections")
    end
    if left.projection_count() != right.projection_count()
      raise ArgumentError.new("compound queries require equal projection counts")
    end
    @left = left
    @operator = operator
    @right = right
    @orderings = orderings
    @limit_value = limit_value
    @offset_value = offset_value
  end
  def left() = @left
  def operator() = @operator
  def right() = @right
  def projection_count() = @left.projection_count()
  def projection_count_known?() = true

  def order(ordering)
    ArelCompoundQuery.new(@left, @operator, @right,
      array_concat(@orderings, arel_array(ordering)), @limit_value, @offset_value)
  end
  def take(n: Int)
    ArelCompoundQuery.new(@left, @operator, @right, @orderings, n, @offset_value)
  end
  def limit(n: Int) = self.take(n)
  def skip(n: Int)
    ArelCompoundQuery.new(@left, @operator, @right, @orderings, @limit_value, n)
  end
  def offset(n: Int) = self.skip(n)

  def render_with(visitor) -> Array
    left_sql, left_params = @left.render_with(visitor)
    right_sql, right_params = @right.render_with(visitor)
    if @left is ArelCompoundQuery
      left_sql = "SELECT * FROM (#{left_sql})"
    end
    if @right is ArelCompoundQuery
      right_sql = "SELECT * FROM (#{right_sql})"
    end
    params = array_concat(left_params, right_params)
    sql = "#{left_sql} #{@operator} #{right_sql}"
    rendered_orderings = []
    def render_ordering(ordering)
      rendered_orderings.push(visitor.render_expression(ordering, params))
    end
    @orderings.each(render_ordering)
    if rendered_orderings.length() > 0
      sql = sql + " ORDER BY " + rendered_orderings.join(", ")
    end
    if @limit_value != nil
      sql = sql + " LIMIT ?"
      params.push(@limit_value)
    end
    if @offset_value != nil
      sql = sql + " OFFSET ?"
      params.push(@offset_value)
    end
    [sql, params]
  end

  def to_sql(visitor = nil) -> Array
    renderer = visitor
    if renderer == nil
      renderer = ArelSQLiteVisitor.new()
    end
    self.render_with(renderer)
  end

  def to_a(db)
    sql, params = self.to_sql()
    db.query(sql, params)
  end
end

class ArelCteRelation < ArelTable
  def initialize(name: String)
    super(name)
  end
  def recursive_body(anchor, recursive_branch)
    if anchor.base_reference_name() == self.name()
      raise ArgumentError.new("recursive CTE anchor cannot reference itself")
    end
    if recursive_branch.base_reference_name() != self.name()
      raise ArgumentError.new("recursive branch must reference its CTE relation")
    end
    ArelCompoundQuery.new(anchor, "UNION ALL", recursive_branch)
  end
end

class ArelAssignmentValue
  def initialize(expression)
    @expression = expression
  end
  def expression() = @expression
end

class ArelConflictTarget
  def initialize(columns: Array, predicate = nil)
    @columns = columns
    @predicate = predicate
  end
  def columns() = @columns
  def predicate() = @predicate
  def where(predicate) = ArelConflictTarget.new(@columns, predicate)
  def column(name: String) = ArelConflictAttribute.new(name)
end

class ArelDefaultValues
end

def arel_render_returning_clause(expressions: Array, params: Array, visitor) -> String
  rendered = []
  def render_expression(expression)
    rendered.push(visitor.render_expression(expression, params))
  end
  expressions.each(render_expression)
  if rendered.length() == 0
    ""
  else
    " RETURNING #{rendered.join(", ")}"
  end
end

def arel_render_insert_conflict(target, ignore: Bool, assignments, params: Array,
                                visitor) -> String
  if !ignore && assignments == nil
    return ""
  end
  columns = target
  predicate = nil
  if target is ArelConflictTarget
    columns = target.columns()
    predicate = target.predicate()
  end
  targets = []
  def quote_target(name)
    targets.push(arel_quote_identifier(name))
  end
  columns.each(quote_target)
  target_sql = ""
  if targets.length() > 0
    target_sql = " (#{targets.join(", ")})"
  end
  if predicate != nil
    target_sql = target_sql + " WHERE " + visitor.render_expression(predicate, params)
  end
  if ignore
    return " ON CONFLICT#{target_sql} DO NOTHING"
  end
  if assignments.length() == 0
    raise ArgumentError.new("conflict update requires at least one assignment")
  end
  rendered_assignments = []
  def render_assignment(name, value)
    if value is ArelAssignmentValue
      rendered = visitor.render_expression(value.expression(), params)
      rendered_assignments.push("#{arel_quote_identifier(name)} = #{rendered}")
    else
      rendered_assignments.push("#{arel_quote_identifier(name)} = ?")
      params.push(value)
    end
  end
  assignments.each(render_assignment)
  " ON CONFLICT#{target_sql} DO UPDATE SET #{rendered_assignments.join(", ")}"
end

class ArelInsert
  def initialize(table: ArelTable, rows = [], returning = [], source_columns = [],
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
  def values(attributes: Hash)
    ArelInsert.new(@table, [attributes], @returning, [], nil, @conflict_target,
      @conflict_ignore, @conflict_assignments, @ctes)
  end
  def values_many(rows: Array)
    ArelInsert.new(@table, rows, @returning, [], nil, @conflict_target,
      @conflict_ignore, @conflict_assignments, @ctes)
  end
  def default_values()
    ArelInsert.new(@table, [ArelDefaultValues.new()], @returning, [], nil,
      @conflict_target, @conflict_ignore, @conflict_assignments, @ctes)
  end
  def from_query(columns: Array, query)
    ArelInsert.new(@table, [], @returning, columns, query, @conflict_target,
      @conflict_ignore, @conflict_assignments, @ctes)
  end
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    ArelInsert.new(@table, @rows, @returning, @source_columns, @source_query,
      @conflict_target, @conflict_ignore, @conflict_assignments,
      arel_append_cte(@ctes, name, query))
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    ArelInsert.new(@table, @rows, @returning, @source_columns, @source_query,
      @conflict_target, @conflict_ignore, @conflict_assignments,
      arel_append_cte(@ctes, name, query, true))
  end
  def on_conflict_do_nothing(columns = [])
    target = columns
    if !(columns is ArelConflictTarget)
      target = arel_array(columns)
    end
    ArelInsert.new(@table, @rows, @returning, @source_columns, @source_query,
      target, true, nil, @ctes)
  end
  def on_conflict_do_update(columns, assignments: Hash)
    target = columns
    if !(columns is ArelConflictTarget)
      target = arel_array(columns)
    end
    ArelInsert.new(@table, @rows, @returning, @source_columns, @source_query,
      target, false, assignments, @ctes)
  end
  def returning(expressions)
    ArelInsert.new(@table, @rows, arel_array(expressions), @source_columns, @source_query,
      @conflict_target, @conflict_ignore, @conflict_assignments, @ctes)
  end

  def render_with(visitor) -> Array
    if @rows.length() == 1 && @rows[0] is ArelDefaultValues
      params = []
      sql = "INSERT INTO #{arel_quote_identifier(@table.name())} DEFAULT VALUES"
      sql = sql + arel_render_returning_clause(@returning, params, visitor)
      cte_params = []
      sql = visitor.render_ctes(self, cte_params) + sql
      return [sql, array_concat(cte_params, params)]
    end
    if @source_query != nil
      if @source_columns.length() == 0
        raise ArgumentError.new("INSERT SELECT requires at least one column")
      end
      if !@source_query.projection_count_known?()
        raise ArgumentError.new("INSERT SELECT requires explicit projections")
      end
      if @source_columns.length() != @source_query.projection_count()
        raise ArgumentError.new("INSERT SELECT columns must match query projections")
      end
      columns = []
      def quote_source_column(name)
        columns.push(arel_quote_identifier(name))
      end
      @source_columns.each(quote_source_column)
      source_sql, params = @source_query.render_with(visitor)
      sql = "INSERT INTO #{arel_quote_identifier(@table.name())} " +
        "(#{columns.join(", ")}) #{source_sql}"
      sql = sql + arel_render_insert_conflict(@conflict_target, @conflict_ignore,
        @conflict_assignments, params, visitor)
      sql = sql + arel_render_returning_clause(@returning, params, visitor)
      cte_params = []
      sql = visitor.render_ctes(self, cte_params) + sql
      params = array_concat(cte_params, params)
      return [sql, params]
    end
    if @rows.length() == 0 || @rows[0].length() == 0
      raise ArgumentError.new("INSERT requires at least one value")
    end
    columns = []
    params = []
    first = @rows[0]
    def collect_column(name, value)
      columns.push(arel_quote_identifier(name))
    end
    first.each(collect_column)
    value_groups = []
    def collect_row(row)
      if row.length() != first.length()
        raise ArgumentError.new("INSERT rows must have identical columns")
      end
      placeholders = []
      index = 0
      while index < first.length()
        key = first.key_at(index)
        if !hash_include_key(row, key)
          raise ArgumentError.new("INSERT rows must have identical columns")
        end
        value = row[key]
        if value is ArelAssignmentValue
          placeholders.push(visitor.render_expression(value.expression(), params))
        else
          placeholders.push("?")
          params.push(value)
        end
        index = index + 1
      end
      value_groups.push("(#{placeholders.join(", ")})")
    end
    @rows.each(collect_row)
    sql = "INSERT INTO #{arel_quote_identifier(@table.name())} " +
      "(#{columns.join(", ")}) VALUES #{value_groups.join(", ")}"
    sql = sql + arel_render_insert_conflict(@conflict_target, @conflict_ignore,
      @conflict_assignments, params, visitor)
    sql = sql + arel_render_returning_clause(@returning, params, visitor)
    cte_params = []
    sql = visitor.render_ctes(self, cte_params) + sql
    params = array_concat(cte_params, params)
    [sql, params]
  end

  def to_sql(visitor = nil) -> Array
    renderer = visitor
    if renderer == nil
      renderer = ArelSQLiteVisitor.new()
    end
    self.render_with(renderer)
  end

  def execute(db)
    sql, params = self.to_sql()
    db.execute(sql, params)
  end
  def to_a(db)
    sql, params = self.to_sql()
    db.query(sql, params)
  end
end

class ArelUpdate
  def initialize(table: ArelTable, assignments = nil, predicates = [], returning = [],
                 allow_all = false, ctes = [])
    @table = table
    @assignments = assignments
    @predicates = predicates
    @returning = returning
    @allow_all = allow_all
    @ctes = ctes
  end

  def ctes() = @ctes
  def set(assignments: Hash)
    ArelUpdate.new(@table, assignments, @predicates, @returning, @allow_all, @ctes)
  end
  def where(predicate)
    ArelUpdate.new(@table, @assignments, array_concat(@predicates, [predicate]), @returning,
      @allow_all, @ctes)
  end
  def returning(expressions)
    ArelUpdate.new(@table, @assignments, @predicates, arel_array(expressions), @allow_all,
      @ctes)
  end
  def all() = ArelUpdate.new(@table, @assignments, @predicates, @returning, true, @ctes)
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    ArelUpdate.new(@table, @assignments, @predicates, @returning, @allow_all,
      arel_append_cte(@ctes, name, query))
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    ArelUpdate.new(@table, @assignments, @predicates, @returning, @allow_all,
      arel_append_cte(@ctes, name, query, true))
  end

  def to_sql() -> Array
    if @assignments == nil || @assignments.length() == 0
      raise ArgumentError.new("UPDATE requires at least one assignment")
    end
    if @predicates.length() == 0 && !@allow_all
      raise ArgumentError.new("UPDATE requires where() or explicit all()")
    end
    clauses = []
    params = []
    visitor = ArelSQLiteVisitor.new()
    def collect_assignment(name, value)
      if value is ArelAssignmentValue
        rendered = visitor.render_expression(value.expression(), params)
        clauses.push("#{arel_quote_identifier(name)} = #{rendered}")
      else
        clauses.push("#{arel_quote_identifier(name)} = ?")
        params.push(value)
      end
    end
    @assignments.each(collect_assignment)
    sql = "UPDATE #{arel_quote_identifier(@table.name())} SET #{clauses.join(", ")}"
    predicates = []
    def render_predicate(predicate)
      predicates.push(visitor.render_expression(predicate, params))
    end
    @predicates.each(render_predicate)
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
    end
    rendered = []
    def render_returning(expression)
      rendered.push(visitor.render_expression(expression, params))
    end
    @returning.each(render_returning)
    if rendered.length() > 0
      sql = sql + " RETURNING " + rendered.join(", ")
    end
    cte_params = []
    sql = visitor.render_ctes(self, cte_params) + sql
    params = array_concat(cte_params, params)
    [sql, params]
  end

  def execute(db)
    sql, params = self.to_sql()
    db.execute(sql, params)
  end
  def to_a(db)
    sql, params = self.to_sql()
    db.query(sql, params)
  end
end

class ArelDelete
  def initialize(table: ArelTable, predicates = [], returning = [], allow_all = false,
                 ctes = [])
    @table = table
    @predicates = predicates
    @returning = returning
    @allow_all = allow_all
    @ctes = ctes
  end

  def ctes() = @ctes
  def where(predicate)
    ArelDelete.new(@table, array_concat(@predicates, [predicate]), @returning, @allow_all,
      @ctes)
  end
  def returning(expressions)
    ArelDelete.new(@table, @predicates, arel_array(expressions), @allow_all, @ctes)
  end
  def all() = ArelDelete.new(@table, @predicates, @returning, true, @ctes)
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    ArelDelete.new(@table, @predicates, @returning, @allow_all,
      arel_append_cte(@ctes, name, query))
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    ArelDelete.new(@table, @predicates, @returning, @allow_all,
      arel_append_cte(@ctes, name, query, true))
  end

  def to_sql() -> Array
    if @predicates.length() == 0 && !@allow_all
      raise ArgumentError.new("DELETE requires where() or explicit all()")
    end
    params = []
    sql = "DELETE FROM #{arel_quote_identifier(@table.name())}"
    predicates = []
    visitor = ArelSQLiteVisitor.new()
    def render_predicate(predicate)
      predicates.push(visitor.render_expression(predicate, params))
    end
    @predicates.each(render_predicate)
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
    end
    rendered = []
    def render_returning(expression)
      rendered.push(visitor.render_expression(expression, params))
    end
    @returning.each(render_returning)
    if rendered.length() > 0
      sql = sql + " RETURNING " + rendered.join(", ")
    end
    cte_params = []
    sql = visitor.render_ctes(self, cte_params) + sql
    params = array_concat(cte_params, params)
    [sql, params]
  end

  def execute(db)
    sql, params = self.to_sql()
    db.execute(sql, params)
  end
  def to_a(db)
    sql, params = self.to_sql()
    db.query(sql, params)
  end
end

class Arel
  def self.table(name: String) = ArelTable.new(name)
  def self.cte(name: String) = ArelCteRelation.new(name)
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
  def self.expression(expression) = ArelAssignmentValue.new(expression)
  def self.excluded(name: String) = ArelExcludedAttribute.new(name)
  def self.literal(value) = ArelLiteral.new(value)
  def self.conflict_target(columns) = ArelConflictTarget.new(arel_array(columns))
  def self.union(left, right) = ArelCompoundQuery.new(left, "UNION", right)
  def self.union_all(left, right) = ArelCompoundQuery.new(left, "UNION ALL", right)
  def self.intersect(left, right) = ArelCompoundQuery.new(left, "INTERSECT", right)
  def self.except(left, right) = ArelCompoundQuery.new(left, "EXCEPT", right)
  def self.insert_into(table: ArelTable) = ArelInsert.new(table)
  def self.update(table: ArelTable) = ArelUpdate.new(table)
  def self.delete_from(table: ArelTable) = ArelDelete.new(table)
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
