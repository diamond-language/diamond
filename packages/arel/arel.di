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

class ArelOrdering
  def initialize(expression, direction: String)
    @expression = expression
    @direction = direction
  end
  def expression() = @expression
  def direction() = @direction
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
  def asc() = ArelOrdering.new(self, "ASC")
  def desc() = ArelOrdering.new(self, "DESC")
  def as(name: String) = ArelAlias.new(self, name)
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
end

class ArelRawSql
  def initialize(sql: String, params: Array)
    @sql = sql
    @params = params
  end
  def sql() = @sql
  def params() = @params
end

class ArelSQLiteVisitor
  def render_attribute(attribute: ArelAttribute) -> String
    arel_quote_identifier(attribute.table().reference_name()) + "." + arel_quote_identifier(attribute.name())
  end

  def render_expression(expression, params: Array) -> String
    if expression is ArelAttribute
      self.render_attribute(expression)
    elsif expression is ArelPredicate
      left = self.render_attribute(expression.left())
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
    elsif expression is ArelNot
      inner = self.render_expression(expression.expression(), params)
      "(NOT #{inner})"
    elsif expression is ArelOrdering
      "#{self.render_attribute(expression.expression())} #{expression.direction()}"
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
    visitor = self
    params = []
    projections = []
    def render_projection(projection)
      projections.push(visitor.render_expression(projection, params))
    end
    query.projections().each(render_projection)
    table_sql = query.table_name()
    if query.quoted_identifiers()
      table_sql = arel_quote_identifier(table_sql)
      if query.table_alias() != nil
        table_sql = table_sql + " AS " + arel_quote_identifier(query.table_alias())
      end
    end
    select_keyword = "SELECT "
    if query.distinct_value()
      select_keyword = "SELECT DISTINCT "
    end
    sql = select_keyword + projections.join(", ") + " FROM #{table_sql}"

    predicates = []
    def render_predicate(predicate)
      predicates.push(visitor.render_expression(predicate, params))
    end
    query.predicates().each(render_predicate)
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
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
                 distinct_value = false)
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
  def distinct_value() = @distinct_value

  def copy(predicates, orderings, limit_value, offset_value, projections)
    ArelQuery.new(@table_name, predicates, orderings, limit_value, offset_value,
      projections, @quoted_identifiers, @bind_limits, @table_alias, @distinct_value)
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
      @projections, @quoted_identifiers, @bind_limits, @table_alias, true)
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
  def self.from(table)
    if table is ArelTable
      ArelQuery.for_table(table)
    else
      ArelQuery.new(table, [], [], nil, nil, ["*"], false, false)
    end
  end
end
