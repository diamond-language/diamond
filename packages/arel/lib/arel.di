# Immutable SQL AST and SQLite renderer. Query nodes describe intent; only
# SQLiteVisitor knows how that intent becomes SQL.

interface ArelTraversalNode
  def arel_children() -> Array
end

interface ArelInspectable
  def arel_inspect() -> String
end

interface ArelComparableNode
  def arel_same?(other) -> Bool
end

interface ArelReplaceableNode
  def arel_with_children(replacements: Array)
end

def arel_array(value)
  if value is Array
    value
  else
    [value]
  end
end

def arel_quote_identifier(name: String) -> String
  if name.length() == 0
    raise ArgumentError.new("SQL identifier cannot be empty")
  end
  pieces = ["\""]
  characters = name.chars()
  index = 0
  while index < characters.length()
    character = characters[index]
    if character == "\""
      pieces.push("\"")
    end
    pieces.push(character)
    index += 1
  end
  pieces.push("\"")
  pieces.join()
end

def arel_quote_identifier_backtick(name: String) -> String
  if name.length() == 0
    raise ArgumentError.new("SQL identifier cannot be empty")
  end
  pieces = ["`"]
  characters = name.chars()
  index = 0
  while index < characters.length()
    character = characters[index]
    if character == "`"
      pieces.push("`")
    end
    pieces.push(character)
    index += 1
  end
  pieces.push("`")
  pieces.join()
end

def arel_cte_name(value) -> String
  if value is String
    value
  else
    value.name()
  end
end

module Arel
class Not
  def initialize(expression)
    @expression = expression
  end
  def expression() = @expression
end

class Logical
  def initialize(left, operator: String, right)
    @left = left
    @operator = operator
    @right = right
  end
  def left() = @left
  def operator() = @operator
  def right() = @right
  def and_also(other) = Logical.new(self, "AND", other)
  def or_else(other) = Logical.new(self, "OR", other)
  def not_() = Not.new(self)
end

class Predicate
  def initialize(left, operator: String, right)
    @left = left
    @operator = operator
    @right = right
  end
  def left() = @left
  def operator() = @operator
  def right() = @right
  def and_also(other) = Logical.new(self, "AND", other)
  def or_else(other) = Logical.new(self, "OR", other)
  def not_() = Not.new(self)
end

class Membership
  def initialize(left, values, negated: Bool)
    @left = left
    @values = values
    @negated = negated
  end
  def left() = @left
  def values() = @values
  def negated?() = @negated
  def and_also(other) = Logical.new(self, "AND", other)
  def or_else(other) = Logical.new(self, "OR", other)
  def not_() = Not.new(self)
end

class Between
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
  def and_also(other) = Logical.new(self, "AND", other)
  def or_else(other) = Logical.new(self, "OR", other)
  def not_() = Not.new(self)
end

class Collation
  def initialize(expression, name: String)
    @expression = expression
    @name = name
  end
  def expression() = @expression
  def name() = @name
end

class Ordering
  def initialize(expression, direction: String, nulls = nil)
    @expression = expression
    @direction = direction
    @nulls = nulls
  end
  def expression() = @expression
  def direction() = @direction
  def nulls() = @nulls
  def nulls_first() = Ordering.new(@expression, @direction, "FIRST")
  def nulls_last() = Ordering.new(@expression, @direction, "LAST")
end

class Alias
  def initialize(expression, name: String)
    @expression = expression
    @name = name
  end
  def expression() = @expression
  def name() = @name
end

class Function
  def initialize(name: String, arguments: Array, distinct = false)
    unless Regexp.new("\\A[A-Za-z_][A-Za-z0-9_]*\\z").match?(name)
      raise ArgumentError.new("SQL function name must be an identifier")
    end
    @name = name
    @arguments = arguments
    @distinct = distinct
  end
  def name() = @name
  def arguments() = @arguments
  def distinct?() = @distinct
  def eq(value) = Predicate.new(self, "=", value)
  def not_eq(value) = Predicate.new(self, "!=", value)
  def lt(value) = Predicate.new(self, "<", value)
  def lteq(value) = Predicate.new(self, "<=", value)
  def gt(value) = Predicate.new(self, ">", value)
  def gteq(value) = Predicate.new(self, ">=", value)
  def like(pattern: String) = Predicate.new(self, "LIKE", pattern)
  def not_like(pattern: String) = Predicate.new(self, "NOT LIKE", pattern)
  def in_list(values: Array) = Membership.new(self, values, false)
  def not_in(values: Array) = Membership.new(self, values, true)
  def between(lower, upper) = Between.new(self, lower, upper, false)
  def not_between(lower, upper) = Between.new(self, lower, upper, true)
  def asc() = Ordering.new(self, "ASC")
  def desc() = Ordering.new(self, "DESC")
  def as(name: String) = Alias.new(self, name)
  def collate(name: String) = Collation.new(self, name)
end

class BinaryExpression
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
  def add(value) = BinaryExpression.new(self, "+", value)
  def subtract(value) = BinaryExpression.new(self, "-", value)
  def multiply(value) = BinaryExpression.new(self, "*", value)
  def divide(value) = BinaryExpression.new(self, "/", value)
  def eq(value) = Predicate.new(self, "=", value)
  def not_eq(value) = Predicate.new(self, "!=", value)
  def lt(value) = Predicate.new(self, "<", value)
  def lteq(value) = Predicate.new(self, "<=", value)
  def gt(value) = Predicate.new(self, ">", value)
  def gteq(value) = Predicate.new(self, ">=", value)
end

class Literal
  def initialize(value)
    if !(value is Int) && !(value is Bool)
      raise ArgumentError.new("SQL literals only support Int and Bool values")
    end
    @value = value
  end
  def value() = @value
end

class Cast
  # A bare identifier (INTEGER, TEXT) or an identifier with a numeric
  # parameter list (VARCHAR(255), NUMERIC(10,2), optional whitespace
  # after the comma) -- verified directly that both forms already work
  # identically on SQLite and PostgreSQL, so no visitor capability is
  # needed here, unlike the genuinely dialect-specific extensions below.
  # The pattern only ever admits letters/digits/underscore/comma/
  # whitespace/parens, so it stays injection-safe with the wider match.
  def initialize(expression, type_name: String)
    pattern = Regexp.new("\\A[A-Za-z_][A-Za-z0-9_]*(\\([0-9]+(,\\s*[0-9]+)?\\))?\\z")
    unless pattern.match?(type_name)
      raise ArgumentError.new(
        "SQL cast type must be an identifier, optionally with a numeric parameter list")
    end
    @expression = expression
    @type_name = type_name
  end
  def expression() = @expression
  def type_name() = @type_name
end

class Attribute
  def initialize(table, name: String)
    @table = table
    @name = name
  end
  def table() = @table
  def name() = @name
  def eq(value) = Predicate.new(self, "=", value)
  def not_eq(value) = Predicate.new(self, "!=", value)
  def lt(value) = Predicate.new(self, "<", value)
  def lteq(value) = Predicate.new(self, "<=", value)
  def gt(value) = Predicate.new(self, ">", value)
  def gteq(value) = Predicate.new(self, ">=", value)
  def like(pattern: String) = Predicate.new(self, "LIKE", pattern)
  def not_like(pattern: String) = Predicate.new(self, "NOT LIKE", pattern)
  def in_list(values: Array) = Membership.new(self, values, false)
  def not_in(values: Array) = Membership.new(self, values, true)
  def in_subquery(query) = Membership.new(self, query, false)
  def not_in_subquery(query) = Membership.new(self, query, true)
  def between(lower, upper) = Between.new(self, lower, upper, false)
  def not_between(lower, upper) = Between.new(self, lower, upper, true)
  def asc() = Ordering.new(self, "ASC")
  def desc() = Ordering.new(self, "DESC")
  def as(name: String) = Alias.new(self, name)
  def collate(name: String) = Collation.new(self, name)
  def add(value) = BinaryExpression.new(self, "+", value)
  def subtract(value) = BinaryExpression.new(self, "-", value)
  def multiply(value) = BinaryExpression.new(self, "*", value)
  def divide(value) = BinaryExpression.new(self, "/", value)
  def concat(value) = BinaryExpression.new(self, "||", value)
  def modulo(value) = BinaryExpression.new(self, "%", value)
  def add_expression(expression) = BinaryExpression.new(self, "+", expression, false)
  def subtract_expression(expression) = BinaryExpression.new(self, "-", expression, false)
  def multiply_expression(expression) = BinaryExpression.new(self, "*", expression, false)
  def divide_expression(expression) = BinaryExpression.new(self, "/", expression, false)
  def concat_expression(expression) = BinaryExpression.new(self, "||", expression, false)
  def modulo_expression(expression) = BinaryExpression.new(self, "%", expression, false)
end

class QualifiedStar
  def initialize(table)
    @table = table
  end
  def table() = @table
end

class Table
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
  def as(name: String) = Table.new(@name, name)
  def column(name: String) = Attribute.new(self, name)
  def star() = QualifiedStar.new(self)
end

class RawSql
  def initialize(sql: String, params: Array)
    @sql = sql
    @params = params
  end
  def sql() = @sql
  def params() = @params
  def and_also(other) = Logical.new(self, "AND", other)
  def or_else(other) = Logical.new(self, "OR", other)
  def not_() = Not.new(self)
end

class ExcludedAttribute
  def initialize(name: String)
    @name = name
  end
  def name() = @name
  def add(value) = BinaryExpression.new(self, "+", value)
end

class ConflictAttribute
  def initialize(name: String)
    @name = name
  end
  def name() = @name
  def eq(value) = Predicate.new(self, "=", value)
  def not_eq(value) = Predicate.new(self, "!=", value)
end

class Join
  def initialize(table: Arel::Table, predicate, kind: String)
    @table = table
    @predicate = predicate
    @kind = kind
  end
  def table() = @table
  def predicate() = @predicate
  def kind() = @kind
end

class Exists
  def initialize(query, negated: Bool)
    @query = query
    @negated = negated
  end
  def query() = @query
  def negated?() = @negated
  def and_also(other) = Logical.new(self, "AND", other)
  def or_else(other) = Logical.new(self, "OR", other)
  def not_() = Exists.new(@query, !@negated)
end

class ScalarSubquery
  def initialize(query)
    @query = query
  end
  def query() = @query
  def eq(value) = Predicate.new(self, "=", value)
  def not_eq(value) = Predicate.new(self, "!=", value)
  def lt(value) = Predicate.new(self, "<", value)
  def lteq(value) = Predicate.new(self, "<=", value)
  def gt(value) = Predicate.new(self, ">", value)
  def gteq(value) = Predicate.new(self, ">=", value)
end

class Cte
  def initialize(name: String, query, recursive = false)
    @name = name
    @query = query
    @recursive = recursive
  end
  def name() = @name
  def query() = @query
  def recursive?() = @recursive
end

class Visitor
  def require_extension(name: String)
    unless self.supports_extension?(name)
      raise ArgumentError.new("#{self.visitor_name()} visitor does not support #{name}")
    end
  end

  def attribute_allowed?(attribute: Arel::Attribute) -> Bool
    if @query == nil
      true
    elsif attribute.table().reference_name().downcase() == @query.base_reference_name().downcase()
      true
    else
      found = false
      reference_name = attribute.table().reference_name()
      index = 0
      while index < @query.joins().length()
        if @query.joins()[index].table().reference_name().downcase() == reference_name.downcase()
          found = true
        end
        index += 1
      end
      index = 0
      while index < @query.correlations().length()
        if @query.correlations()[index].reference_name().downcase() == reference_name.downcase()
          found = true
        end
        index += 1
      end
      found
    end
  end

  def render_attribute(attribute: Arel::Attribute) -> String
    unless self.attribute_allowed?(attribute)
      raise ArgumentError.new("attribute belongs to a relation outside this query")
    end
    self.quote_identifier(attribute.table().reference_name()) + "." + self.quote_identifier(attribute.name())
  end

  def render_table(table: Arel::Table) -> String
    sql = self.quote_identifier(table.name())
    unless table.table_alias() == nil
      sql = sql + " AS " + self.quote_identifier(table.table_alias())
    end
    sql
  end

  def render_source(query, params: Array) -> String
    if query.source_query() != nil
      source_sql, source_params = query.source_query().render_with(self)
      source_index = 0
      while source_index < source_params.length()
        params.push(source_params[source_index])
        source_index += 1
      end
      "(#{source_sql}) AS #{self.quote_identifier(query.base_reference_name())}"
    else
      sql = query.table_name()
      if query.quoted_identifiers()
        sql = self.quote_identifier(sql)
        unless query.table_alias() == nil
          sql = sql + " AS " + self.quote_identifier(query.table_alias())
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
        self.require_extension("recursive CTEs")
        recursive = true
      end
      sql, bound = cte.query().render_with(self)
      bound_index = 0
      while bound_index < bound.length()
        params.push(bound[bound_index])
        bound_index += 1
      end
      entries.push("#{self.quote_identifier(cte.name())} AS (#{sql})")
      index += 1
    end
    if entries.length() == 0
      ""
    else
      self.render_cte_prefix(entries, recursive)
    end
  end

  def render_cte_prefix(entries: Array, recursive: Bool) -> String
    prefix = "WITH "
    if recursive
      prefix = "WITH RECURSIVE "
    end
    "#{prefix}#{entries.join(", ")} "
  end

  def render_expression(expression, params: Array) -> String
    if expression is Attribute
      self.render_attribute(expression)
    elsif expression is Predicate
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
      elsif value is Attribute
        "#{left} #{expression.operator()} #{self.render_attribute(value)}"
      elsif value is Literal
        "#{left} #{expression.operator()} #{self.render_expression(value, params)}"
      else
        params.push(value)
        "#{left} #{expression.operator()} ?"
      end
    elsif expression is Logical
      left = self.render_expression(expression.left(), params)
      right = self.render_expression(expression.right(), params)
      "(#{left} #{expression.operator()} #{right})"
    elsif expression is Membership
      if !(expression.values() is Array)
        subquery_sql, subquery_params = expression.values().render_with(self)
        subquery_index = 0
        while subquery_index < subquery_params.length()
          params.push(subquery_params[subquery_index])
          subquery_index += 1
        end
        operator = "IN"
        if expression.negated?()
          operator = "NOT IN"
        end
        "#{self.render_expression(expression.left(), params)} #{operator} (#{subquery_sql})"
      elsif expression.values().length() == 0
        if expression.negated?()
          "1 = 1"
        else
          "1 = 0"
        end
      else
        placeholders = []
        value_index = 0
        while value_index < expression.values().length()
          params.push(expression.values()[value_index])
          placeholders.push("?")
          value_index += 1
        end
        operator = "IN"
        if expression.negated?()
          operator = "NOT IN"
        end
        "#{self.render_expression(expression.left(), params)} #{operator} (#{placeholders.join(", ")})"
      end
    elsif expression is Between
      params.push(expression.lower())
      params.push(expression.upper())
      operator = "BETWEEN"
      if expression.negated?()
        operator = "NOT BETWEEN"
      end
      "#{self.render_expression(expression.left(), params)} #{operator} ? AND ?"
    elsif expression is Collation
      inner = self.render_expression(expression.expression(), params)
      "#{inner} COLLATE #{self.quote_identifier(expression.name())}"
    elsif expression is Exists
      sql, bound = expression.query().render_with(self)
      bound_index = 0
      while bound_index < bound.length()
        params.push(bound[bound_index])
        bound_index += 1
      end
      prefix = "EXISTS"
      if expression.negated?()
        prefix = "NOT EXISTS"
      end
      "#{prefix} (#{sql})"
    elsif expression is ScalarSubquery
      sql, bound = expression.query().render_with(self)
      bound_index = 0
      while bound_index < bound.length()
        params.push(bound[bound_index])
        bound_index += 1
      end
      "(#{sql})"
    else
      self.render_expression_tail(expression, params)
    end
  end

  # Keep the nominal narrowing chain in each method below Diamond's
  # eight-alternative union ceiling as the AST grows.
  def render_expression_tail(expression, params: Array) -> String
    if expression is Function
      arguments = []
      argument_index = 0
      while argument_index < expression.arguments().length()
        arguments.push(self.render_expression(expression.arguments()[argument_index], params))
        argument_index += 1
      end
      prefix = ""
      if expression.distinct?()
        prefix = "DISTINCT "
      end
      "#{expression.name()}(#{prefix}#{arguments.join(", ")})"
    elsif expression is QualifiedStar
      unless self.attribute_allowed?(Attribute.new(expression.table(), "*"))
        raise ArgumentError.new("wildcard belongs to a relation outside this query")
      end
      self.quote_identifier(expression.table().reference_name()) + ".*"
    elsif expression is Not
      inner = self.render_expression(expression.expression(), params)
      "(NOT #{inner})"
    elsif expression is Ordering
      sql = "#{self.render_expression(expression.expression(), params)} #{expression.direction()}"
      unless expression.nulls() == nil
        self.require_extension("explicit NULL ordering")
        sql = sql + " NULLS #{expression.nulls()}"
      end
      sql
    elsif expression is Alias
      inner = self.render_expression(expression.expression(), params)
      "#{inner} AS #{self.quote_identifier(expression.name())}"
    elsif expression is RawSql
      param_index = 0
      while param_index < expression.params().length()
        params.push(expression.params()[param_index])
        param_index += 1
      end
      expression.sql()
    else
      self.render_expression_extension(expression, params)
    end
  end

  def render_expression_extension(expression, params: Array) -> String
    if expression is ExcludedAttribute
      self.require_extension("excluded-row attributes")
      "excluded.#{self.quote_identifier(expression.name())}"
    elsif expression is ConflictAttribute
      self.quote_identifier(expression.name())
    elsif expression is BinaryExpression
      if expression.operator() == "&" || expression.operator() == "|" ||
          expression.operator() == "<<" || expression.operator() == ">>"
        self.require_extension("integer bitwise operators")
      end
      left = self.render_expression(expression.left(), params)
      right = ""
      if expression.bind_right?()
        params.push(expression.right())
        right = "?"
      else
        right = self.render_expression(expression.right(), params)
      end
      "(#{left} #{expression.operator()} #{right})"
    elsif expression is Literal
      self.render_literal(expression.value())
    elsif expression is Cast
      inner = self.render_expression(expression.expression(), params)
      "CAST(#{inner} AS #{expression.type_name()})"
    elsif expression is String
      expression
    else
      raise TypeError.new("unsupported Arel expression")
    end
  end

  def render_literal(value) -> String
    if value is Bool
      if value
        "TRUE"
      else
        "FALSE"
      end
    else
      "#{value}"
    end
  end

  def render_join(join: Arel::Join, params: Array) -> String
    sql = "#{join.kind()} JOIN #{self.render_table(join.table())}"
    unless join.predicate() == nil
      sql = sql + " ON " + self.render_expression(join.predicate(), params)
    end
    sql
  end

  def render(query) -> Array
    previous_query = @query
    begin
    visitor = self
    params = []
    sql = self.render_ctes(query, params)
    @query = query
    projections = []
    index = 0
    while index < query.projections().length()
      projections.push(visitor.render_expression(query.projections()[index], params))
      index += 1
    end
    table_sql = self.render_source(query, params)
    index = 0
    while index < query.joins().length()
      join = query.joins()[index]
      table_sql += " " + visitor.render_join(join, params)
      index += 1
    end
    sql = sql + "SELECT "
    if query.distinct_value()
      sql = sql + "DISTINCT "
    end
    sql = sql + projections.join(", ") + " FROM #{table_sql}"

    predicates = []
    index = 0
    while index < query.predicates().length()
      predicates.push(visitor.render_expression(query.predicates()[index], params))
      index += 1
    end
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
    end

    groups = []
    index = 0
    while index < query.groups().length()
      groups.push(visitor.render_expression(query.groups()[index], params))
      index += 1
    end
    if groups.length() > 0
      sql = sql + " GROUP BY " + groups.join(", ")
    end

    havings = []
    index = 0
    while index < query.havings().length()
      havings.push(visitor.render_expression(query.havings()[index], params))
      index += 1
    end
    if havings.length() > 0
      sql = sql + " HAVING " + havings.join(" AND ")
    end

    orderings = []
    index = 0
    while index < query.orderings().length()
      orderings.push(visitor.render_expression(query.orderings()[index], params))
      index += 1
    end
    if orderings.length() > 0
      sql = sql + " ORDER BY " + orderings.join(", ")
    end

    sql = sql + self.render_pagination(query.limit_value(), query.offset_value(),
      params, query.bind_limits())
    [sql, params]
    ensure
      @query = previous_query
    end
  end

  def render_compound_branch(sql: String, grouped: Bool) -> String
    if grouped
      "SELECT * FROM (#{sql})"
    else
      sql
    end
  end

  def render_returning(expressions: Array, params: Array) -> String
    if expressions.length() > 0
      self.require_extension("returning clauses")
    end
    rendered = []
    index = 0
    while index < expressions.length()
      rendered.push(self.render_expression(expressions[index], params))
      index += 1
    end
    if rendered.length() == 0
      ""
    else
      " RETURNING #{rendered.join(", ")}"
    end
  end

  def render_compound(query) -> Array = query.render_default(self)
  def render_insert(statement) -> Array = statement.render_default(self)
  def render_update(statement) -> Array = statement.render_default(self)
  def render_delete(statement) -> Array = statement.render_default(self)

end

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

# PostgreSQL's own grammar accepts a bare OFFSET with no LIMIT clause at
# all, so unlike SQLiteVisitor's render_pagination above, no LIMIT -1
# sentinel is needed here. Most other capabilities this visitor claims
# below (identifier quoting, ON CONFLICT, DEFAULT VALUES, RETURNING, CTEs,
# NULLS FIRST/LAST, integer bitwise operators) use syntax identical to
# SQLite's own -- both were modeled on Postgres's own SQL to begin with --
# verified against a live PostgreSQL container in
# tests/cases/arel_postgres_dialect.di, not merely assumed from the
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

class CompoundQuery
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
  def orderings() = @orderings
  def limit_value() = @limit_value
  def offset_value() = @offset_value
  def projection_count() = @left.projection_count()
  def projection_count_known?() = true

  def order(ordering)
    CompoundQuery.new(@left, @operator, @right,
      @orderings.concat(arel_array(ordering)), @limit_value, @offset_value)
  end
  def take(n: Int)
    if n < 0
      raise ArgumentError.new("limit must be non-negative")
    end
    CompoundQuery.new(@left, @operator, @right, @orderings, n, @offset_value)
  end
  def limit(n: Int) = self.take(n)
  def skip(n: Int)
    if n < 0
      raise ArgumentError.new("offset must be non-negative")
    end
    CompoundQuery.new(@left, @operator, @right, @orderings, @limit_value, n)
  end
  def offset(n: Int) = self.skip(n)

  def render_default(visitor) -> Array
    left_sql, left_params = @left.render_with(visitor)
    right_sql, right_params = @right.render_with(visitor)
    left_sql = visitor.render_compound_branch(left_sql, @left is CompoundQuery)
    right_sql = visitor.render_compound_branch(right_sql, @right is CompoundQuery)
    params = left_params.concat(right_params)
    sql = "#{left_sql} #{@operator} #{right_sql}"
    rendered_orderings = []
    ordering_index = 0
    while ordering_index < @orderings.length()
      rendered_orderings.push(visitor.render_expression(@orderings[ordering_index], params))
      ordering_index += 1
    end
    if rendered_orderings.length() > 0
      sql = sql + " ORDER BY " + rendered_orderings.join(", ")
    end
    sql = sql + visitor.render_pagination(@limit_value, @offset_value, params)
    [sql, params]
  end

  def render_with(visitor) -> Array = visitor.render_compound(self)

  def to_sql(visitor = nil) -> Array
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
end

class CteRelation < Table
  def recursive_body(anchor, recursive_branch)
    if anchor.base_reference_name().downcase() == self.name().downcase()
      raise ArgumentError.new("recursive CTE anchor cannot reference itself")
    end
    if recursive_branch.base_reference_name().downcase() != self.name().downcase()
      raise ArgumentError.new("recursive branch must reference its CTE relation")
    end
    CompoundQuery.new(anchor, "UNION ALL", recursive_branch)
  end
end

class AssignmentValue
  def initialize(expression)
    @expression = expression
  end
  def expression() = @expression
end

class ConflictTarget
  def initialize(columns: Array, predicate = nil)
    @columns = columns
    @predicate = predicate
  end
  def columns() = @columns
  def predicate() = @predicate
  def where(predicate) = ConflictTarget.new(@columns, predicate)
  def column(name: String) = ConflictAttribute.new(name)
end

# `ON CONFLICT ON CONSTRAINT name DO ...` -- names the constraint directly
# instead of repeating its column list (or for a constraint ConflictTarget's
# column-list form can't express at all, like an exclusion constraint).
# Verified directly: PostgreSQL accepts this; SQLite has no equivalent
# (rejects it as a syntax error -- SQLite's own UPSERT grammar has no
# named-constraint form), so this is gated behind the "named-constraint
# conflict targets" capability.
class ConflictConstraintTarget
  def initialize(name: String)
    @name = name
  end
  def name() = @name
end

class DefaultValues
end

# A per-column DEFAULT within an ordinary VALUES row (`INSERT INTO t (a,
# b) VALUES (1, DEFAULT)`), as opposed to DefaultValues above (the whole
# row is DEFAULT VALUES, no column list at all). Verified directly against
# both dialects before adding this: PostgreSQL accepts a bare DEFAULT in a
# VALUES list (single- and multi-row); SQLite rejects it outright as a
# syntax error, so this is gated behind the "per-column default values"
# capability, unlike Cast above.
class ColumnDefault
end

# Namespace singleton methods rather than free top-level functions:
# Diamond's top-level function resolution is source-order (a call only
# sees functions already defined earlier in the file), and these need
# Cte/ConflictTarget/AssignmentValue already declared, but are also
# called from Insert/Update/Delete below -- nesting them here, after the
# classes they use and before their own call sites, and calling them
# self-referentially as Arel.append_cte(...)/Arel.render_insert_conflict(...)
# (verified this resolves the same way an external Arel.table(...) call
# does), satisfies both constraints at once.
def self.append_cte(ctes: Array, name: String, query, recursive = false) -> Array
  duplicate = false
  index = 0
  while index < ctes.length()
    if ctes[index].name().downcase() == name.downcase()
      duplicate = true
    end
    index += 1
  end
  if duplicate
    raise ArgumentError.new("duplicate CTE name")
  end
  ctes.concat([Cte.new(name, query, recursive)])
end

def self.render_insert_conflict(target, ignore: Bool, assignments, params: Array,
                                visitor) -> String
  if !ignore && assignments == nil
    return ""
  end
  target_sql = ""
  if target is ConflictConstraintTarget
    visitor.require_extension("named-constraint conflict targets")
    target_sql = " ON CONSTRAINT #{visitor.quote_identifier(target.name())}"
  else
    columns = target
    predicate = nil
    if target is ConflictTarget
      columns = target.columns()
      predicate = target.predicate()
    end
    targets = []
    target_index = 0
    while target_index < columns.length()
      targets.push(visitor.quote_identifier(columns[target_index]))
      target_index += 1
    end
    if targets.length() > 0
      target_sql = " (#{targets.join(", ")})"
    end
    unless predicate == nil
      visitor.require_extension("conflict-target predicates")
      target_sql += " WHERE " + visitor.render_expression(predicate, params)
    end
  end
  visitor.require_extension("upsert conflict actions")
  if ignore
    return " ON CONFLICT#{target_sql} DO NOTHING"
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
      rendered = visitor.render_expression(value.expression(), params)
      rendered_assignments.push("#{visitor.quote_identifier(name)} = #{rendered}")
    else
      rendered_assignments.push("#{visitor.quote_identifier(name)} = ?")
      params.push(value)
    end
    assignment_index += 1
  end
  " ON CONFLICT#{target_sql} DO UPDATE SET #{rendered_assignments.join(", ")}"
end

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

class Update
  def initialize(table: Arel::Table, assignments = nil, predicates = [], returning = [],
                 allow_all = false, ctes = [])
    @table = table
    @assignments = assignments
    @predicates = predicates
    @returning = returning
    @allow_all = allow_all
    @ctes = ctes
  end

  def ctes() = @ctes
  def structure() = [@table, @assignments, @predicates, @returning, @allow_all, @ctes]
  def set(assignments: Hash)
    Update.new(@table, assignments, @predicates, @returning, @allow_all, @ctes)
  end
  def where(predicate)
    Update.new(@table, @assignments, @predicates.concat([predicate]), @returning,
      @allow_all, @ctes)
  end
  def returning(expressions)
    Update.new(@table, @assignments, @predicates, arel_array(expressions), @allow_all,
      @ctes)
  end
  def all() = Update.new(@table, @assignments, @predicates, @returning, true, @ctes)
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    Update.new(@table, @assignments, @predicates, @returning, @allow_all,
      Arel.append_cte(@ctes, name, query))
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    Update.new(@table, @assignments, @predicates, @returning, @allow_all,
      Arel.append_cte(@ctes, name, query, true))
  end

  def render_default(visitor) -> Array
    if @ctes.length() > 0
      visitor.require_extension("write CTEs")
    end
    if @assignments == nil || @assignments.length() == 0
      raise ArgumentError.new("UPDATE requires at least one assignment")
    end
    if @predicates.length() == 0 && !@allow_all
      raise ArgumentError.new("UPDATE requires where() or explicit all()")
    end
    clauses = []
    params = []
    assignment_index = 0
    while assignment_index < @assignments.length()
      name = @assignments.key_at(assignment_index)
      value = @assignments[name]
      if value is AssignmentValue
        rendered = visitor.render_expression(value.expression(), params)
        clauses.push("#{visitor.quote_identifier(name)} = #{rendered}")
      else
        clauses.push("#{visitor.quote_identifier(name)} = ?")
        params.push(value)
      end
      assignment_index += 1
    end
    sql = "UPDATE #{visitor.quote_identifier(@table.name())} SET #{clauses.join(", ")}"
    predicates = []
    predicate_index = 0
    while predicate_index < @predicates.length()
      predicates.push(visitor.render_expression(@predicates[predicate_index], params))
      predicate_index += 1
    end
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
    end
    if @returning.length() > 0
      sql = sql + visitor.render_returning(@returning, params)
    end
    cte_params = []
    sql = visitor.render_ctes(self, cte_params) + sql
    params = cte_params.concat(params)
    [sql, params]
  end

  def render_with(visitor) -> Array = visitor.render_update(self)

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

class Delete
  def initialize(table: Arel::Table, predicates = [], returning = [], allow_all = false,
                 ctes = [])
    @table = table
    @predicates = predicates
    @returning = returning
    @allow_all = allow_all
    @ctes = ctes
  end

  def ctes() = @ctes
  def structure() = [@table, @predicates, @returning, @allow_all, @ctes]
  def where(predicate)
    Delete.new(@table, @predicates.concat([predicate]), @returning, @allow_all,
      @ctes)
  end
  def returning(expressions)
    Delete.new(@table, @predicates, arel_array(expressions), @allow_all, @ctes)
  end
  def all() = Delete.new(@table, @predicates, @returning, true, @ctes)
  def with(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    Delete.new(@table, @predicates, @returning, @allow_all,
      Arel.append_cte(@ctes, name, query))
  end
  def with_recursive(relation_or_name, query)
    name = arel_cte_name(relation_or_name)
    Delete.new(@table, @predicates, @returning, @allow_all,
      Arel.append_cte(@ctes, name, query, true))
  end

  def render_default(visitor) -> Array
    if @ctes.length() > 0
      visitor.require_extension("write CTEs")
    end
    if @predicates.length() == 0 && !@allow_all
      raise ArgumentError.new("DELETE requires where() or explicit all()")
    end
    params = []
    sql = "DELETE FROM #{visitor.quote_identifier(@table.name())}"
    predicates = []
    predicate_index = 0
    while predicate_index < @predicates.length()
      predicates.push(visitor.render_expression(@predicates[predicate_index], params))
      predicate_index += 1
    end
    if predicates.length() > 0
      sql = sql + " WHERE " + predicates.join(" AND ")
    end
    if @returning.length() > 0
      sql = sql + visitor.render_returning(@returning, params)
    end
    cte_params = []
    sql = visitor.render_ctes(self, cte_params) + sql
    params = cte_params.concat(params)
    [sql, params]
  end

  def render_with(visitor) -> Array = visitor.render_delete(self)

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

class Inspector
def with_children(node, replacements: Array)
  if node is Function
    Function.new(node.name(), replacements, node.distinct?())
  elsif node is Cast
    if replacements.length() != 1
      raise ArgumentError.new("Cast requires exactly one child")
    end
    Cast.new(replacements[0], node.type_name())
  elsif node is Collation
    if replacements.length() != 1
      raise ArgumentError.new("Collation requires exactly one child")
    end
    Collation.new(replacements[0], node.name())
  elsif node is Alias
    if replacements.length() != 1
      raise ArgumentError.new("Alias requires exactly one child")
    end
    Alias.new(replacements[0], node.name())
  elsif node is Ordering
    if replacements.length() != 1
      raise ArgumentError.new("Ordering requires exactly one child")
    end
    Ordering.new(replacements[0], node.direction(), node.nulls())
  elsif node is AssignmentValue
    if replacements.length() != 1
      raise ArgumentError.new("AssignmentValue requires exactly one child")
    end
    AssignmentValue.new(replacements[0])
  else
    self.with_children_tail(node, replacements)
  end
end

def with_children_tail(node, replacements: Array)
  if node is BinaryExpression
    expected = 1
    unless node.bind_right?()
      expected = 2
    end
    if replacements.length() != expected
      raise ArgumentError.new("BinaryExpression replacement child count mismatch")
    end
    right = node.right()
    unless node.bind_right?()
      right = replacements[1]
    end
    BinaryExpression.new(replacements[0], node.operator(), right, node.bind_right?())
  elsif node is QualifiedStar
    if replacements.length() != 1 || !(replacements[0] is Table)
      raise ArgumentError.new("QualifiedStar requires exactly one table child")
    end
    QualifiedStar.new(replacements[0])
  elsif node is Predicate
    expected = 1
    structural_right = node.right() is Attribute || node.right() is Literal
    if structural_right
      expected = 2
    end
    if replacements.length() != expected
      raise ArgumentError.new("Predicate replacement child count mismatch")
    end
    right = node.right()
    if structural_right
      right = replacements[1]
    end
    Predicate.new(replacements[0], node.operator(), right)
  elsif node is Logical
    if replacements.length() != 2
      raise ArgumentError.new("Logical requires exactly two children")
    end
    Logical.new(replacements[0], node.operator(), replacements[1])
  elsif node is Not
    if replacements.length() != 1
      raise ArgumentError.new("Not requires exactly one child")
    end
    Not.new(replacements[0])
  elsif node is Between
    if replacements.length() != 1
      raise ArgumentError.new("Between requires exactly one child")
    end
    Between.new(replacements[0], node.lower(), node.upper(), node.negated?())
  elsif node is Membership
    expected = 1
    unless node.values() is Array
      expected = 2
    end
    if replacements.length() != expected
      raise ArgumentError.new("Membership replacement child count mismatch")
    end
    values = node.values()
    unless values is Array
      values = replacements[1]
    end
    Membership.new(replacements[0], values, node.negated?())
  elsif node is Exists
    if replacements.length() != 1
      raise ArgumentError.new("Exists requires exactly one child")
    end
    Exists.new(replacements[0], node.negated?())
  elsif node is ScalarSubquery
    if replacements.length() != 1
      raise ArgumentError.new("ScalarSubquery requires exactly one child")
    end
    ScalarSubquery.new(replacements[0])
  elsif node is Join
    expected = 1
    unless node.predicate() == nil
      expected = 2
    end
    if replacements.length() != expected || !(replacements[0] is Table)
      raise ArgumentError.new("Join replacement children do not match its shape")
    end
    predicate = nil
    if expected == 2
      predicate = replacements[1]
    end
    Join.new(replacements[0], predicate, node.kind())
  elsif node is Cte
    if replacements.length() != 1
      raise ArgumentError.new("Cte requires exactly one child")
    end
    Cte.new(node.name(), replacements[0], node.recursive?())
  elsif node is ConflictTarget
    expected = 0
    unless node.predicate() == nil
      expected = 1
    end
    if replacements.length() != expected
      raise ArgumentError.new("ConflictTarget replacement child count mismatch")
    end
    predicate = nil
    if expected == 1
      predicate = replacements[0]
    end
    ConflictTarget.new(node.columns(), predicate)
  elsif node is CompoundQuery
    if replacements.length() != 2 + node.orderings().length()
      raise ArgumentError.new("CompoundQuery replacement child count mismatch")
    end
    orderings = []
    index = 2
    while index < replacements.length()
      orderings.push(replacements[index])
      index += 1
    end
    CompoundQuery.new(replacements[0], node.operator(), replacements[1], orderings,
      node.limit_value(), node.offset_value())
  elsif node is Query
    self.with_query_children(node, replacements)
  elsif node is Update || node is Delete || node is Insert
    self.with_write_children(node, replacements)
  elsif node is ArelReplaceableNode
    node.arel_with_children(replacements)
  else
    raise ArgumentError.new("Arel node does not support child replacement")
  end
end

def with_write_children(node, replacements: Array)
  if replacements.length() != self.children(node).length()
    raise ArgumentError.new("Arel write manager replacement child count mismatch")
  end
  if node is Update
    state = node.structure()
    index = state[5].length()
    table = replacements[index]
    index += 1
    assignments = nil
    unless state[1] == nil
      assignments = {}
      assignment_index = 0
      while assignment_index < state[1].length()
        key = state[1].key_at(assignment_index)
        value = state[1][key]
        if value is AssignmentValue
          value = replacements[index]
          index += 1
        end
        assignments[key] = value
        assignment_index += 1
      end
    end
    predicates = []
    predicate_index = 0
    while predicate_index < state[2].length()
      predicates.push(replacements[index])
      index += 1
      predicate_index += 1
    end
    returning = []
    returning_index = 0
    while returning_index < state[3].length()
      returning.push(replacements[index])
      index += 1
      returning_index += 1
    end
    ctes = []
    cte_index = 0
    while cte_index < state[5].length()
      ctes.push(replacements[cte_index])
      cte_index += 1
    end
    Update.new(table, assignments, predicates, returning, state[4], ctes)
  elsif node is Delete
    state = node.structure()
    index = state[4].length()
    table = replacements[index]
    index += 1
    predicates = []
    predicate_index = 0
    while predicate_index < state[1].length()
      predicates.push(replacements[index])
      index += 1
      predicate_index += 1
    end
    returning = []
    returning_index = 0
    while returning_index < state[2].length()
      returning.push(replacements[index])
      index += 1
      returning_index += 1
    end
    ctes = []
    cte_index = 0
    while cte_index < state[4].length()
      ctes.push(replacements[cte_index])
      cte_index += 1
    end
    Delete.new(table, predicates, returning, state[3], ctes)
  elsif node is Insert
    state = node.structure()
    index = state[8].length()
    table = replacements[index]
    index += 1
    source_query = state[4]
    unless source_query == nil
      source_query = replacements[index]
      index += 1
    end
    rows = []
    row_index = 0
    while row_index < state[1].length()
      original_row = state[1][row_index]
      if original_row is DefaultValues
        rows.push(original_row)
      else
        row = {}
        value_index = 0
        while value_index < original_row.length()
          key = original_row.key_at(value_index)
          value = original_row[key]
          if value is AssignmentValue
            value = replacements[index]
            index += 1
          end
          row[key] = value
          value_index += 1
        end
        rows.push(row)
      end
      row_index += 1
    end
    conflict_target = state[5]
    if conflict_target is ConflictTarget
      conflict_target = replacements[index]
      index += 1
    end
    conflict_assignments = nil
    unless state[7] == nil
      conflict_assignments = {}
      value_index = 0
      while value_index < state[7].length()
        key = state[7].key_at(value_index)
        value = state[7][key]
        if value is AssignmentValue
          value = replacements[index]
          index += 1
        end
        conflict_assignments[key] = value
        value_index += 1
      end
    end
    returning = []
    returning_index = 0
    while returning_index < state[2].length()
      returning.push(replacements[index])
      index += 1
      returning_index += 1
    end
    ctes = []
    cte_index = 0
    while cte_index < state[8].length()
      ctes.push(replacements[cte_index])
      cte_index += 1
    end
    Insert.new(table, rows, returning, state[3], source_query, conflict_target,
      state[6], conflict_assignments, ctes)
  else
    raise ArgumentError.new("Arel write manager does not support child replacement")
  end
end

def with_query_children(node: Arel::Query, replacements: Array)
  if replacements.length() != self.children(node).length()
    raise ArgumentError.new("Query replacement child count mismatch")
  end
  index = 0
  ctes = []
  part = 0
  while part < node.ctes().length()
    ctes.push(replacements[index + part])
    part += 1
  end
  index += node.ctes().length()
  source_query = nil
  unless node.source_query() == nil
    source_query = replacements[index]
    index += 1
  end
  projections = []
  part = 0
  while part < node.projections().length()
    projections.push(replacements[index + part])
    part += 1
  end
  index += node.projections().length()
  joins = []
  part = 0
  while part < node.joins().length()
    joins.push(replacements[index + part])
    part += 1
  end
  index += node.joins().length()
  predicates = []
  part = 0
  while part < node.predicates().length()
    predicates.push(replacements[index + part])
    part += 1
  end
  index += node.predicates().length()
  groups = []
  part = 0
  while part < node.groups().length()
    groups.push(replacements[index + part])
    part += 1
  end
  index += node.groups().length()
  havings = []
  part = 0
  while part < node.havings().length()
    havings.push(replacements[index + part])
    part += 1
  end
  index += node.havings().length()
  orderings = []
  part = 0
  while part < node.orderings().length()
    orderings.push(replacements[index + part])
    part += 1
  end
  index += node.orderings().length()
  correlations = []
  part = 0
  while part < node.correlations().length()
    correlations.push(replacements[index + part])
    part += 1
  end
  Query.new(node.table_name(), predicates, orderings, node.limit_value(),
    node.offset_value(), projections, node.quoted_identifiers(), node.bind_limits(),
    node.table_alias(), node.distinct_value(), groups, havings, joins, source_query,
    correlations, ctes)
end

def simplify(node, rules = [], report = false)
  original = node
  children = self.children(node)
  if children.length() > 0
    replacements = []
    index = 0
    while index < children.length()
      replacements.push(self.simplify(children[index], rules))
      index += 1
    end
    node = self.with_children(node, replacements)
  end
  if node is Not && node.expression() is Not
    node = node.expression().expression()
  elsif node is Membership && node.values() is Array && node.values().length() == 0
    if node.negated?()
      node = RawSql.new("1 = 1", [])
    else
      node = RawSql.new("1 = 0", [])
    end
  end
  rule_index = 0
  while rule_index < rules.length()
    rule = rules[rule_index]
    if !(rule is Array) || rule.length() != 2
      raise ArgumentError.new("Arel rewrite rule must be [pattern, replacement]")
    end
    if self.same?(node, rule[0])
      node = rule[1]
    end
    rule_index += 1
  end
  if report
    [node, !self.same?(original, node)]
  else
    node
  end
end

def walk(node, visitor = nil) -> Array
  visited = []
  pending = [node]
  while pending.length() > 0
    current = pending.pop()
    visited.push(current)
    unless visitor == nil
      visitor.visit(current)
    end
    children = self.children(current)
    index = children.length()
    while index > 0
      index -= 1
      pending.push(children[index])
    end
  end
  visited
end

def children(node) -> Array
  if node is Attribute || node is Literal || node is ExcludedAttribute ||
     node is RawSql || node is Table || node is ConflictAttribute ||
     node is DefaultValues
    []
  elsif node is BinaryExpression
    children = [node.left()]
    unless node.bind_right?()
      children.push(node.right())
    end
    children
  elsif node is Function
    node.arguments()
  elsif node is Cast || node is Collation || node is Alias ||
        node is Ordering || node is AssignmentValue
    [node.expression()]
  elsif node is QualifiedStar
    [node.table()]
  elsif node is Predicate
    children = [node.left()]
    if node.right() is Attribute || node.right() is Literal
      children.push(node.right())
    end
    children
  elsif node is Logical
    [node.left(), node.right()]
  elsif node is Not
    [node.expression()]
  elsif node is Between
    [node.left()]
  elsif node is Membership
    children = [node.left()]
    unless node.values() is Array
      children.push(node.values())
    end
    children
  elsif node is Exists || node is ScalarSubquery
    [node.query()]
  elsif node is Join
    children = [node.table()]
    unless node.predicate() == nil
      children.push(node.predicate())
    end
    children
  elsif node is ArelTraversalNode
    node.arel_children()
  else
    self.children_tail(node)
  end
end

def children_tail(node) -> Array
  if node is Query
    children = [].concat(node.ctes())
    unless node.source_query() == nil
      children.push(node.source_query())
    end
    children = children.concat(node.projections())
    children = children.concat(node.joins())
    children = children.concat(node.predicates())
    children = children.concat(node.groups())
    children = children.concat(node.havings())
    children = children.concat(node.orderings())
    children.concat(node.correlations())
  elsif node is CompoundQuery
    [node.left(), node.right()].concat(node.orderings())
  elsif node is Cte
    [node.query()]
  elsif node is ConflictTarget
    if node.predicate() == nil
      []
    else
      [node.predicate()]
    end
  elsif node is Insert || node is Update || node is Delete
    self.children_write(node)
  else
    []
  end
end

def children_write(node) -> Array
  if node is Insert
    state = node.structure()
    children = [].concat(state[8])
    children.push(state[0])
    unless state[4] == nil
      children.push(state[4])
    end
    row_index = 0
    while row_index < state[1].length()
      row = state[1][row_index]
      unless row is DefaultValues
        value_index = 0
        while value_index < row.length()
          value = row[row.key_at(value_index)]
          if value is AssignmentValue
            children.push(value)
          end
          value_index += 1
        end
      end
      row_index += 1
    end
    if state[5] is ConflictTarget
      children.push(state[5])
    end
    unless state[7] == nil
      value_index = 0
      while value_index < state[7].length()
        value = state[7][state[7].key_at(value_index)]
        if value is AssignmentValue
          children.push(value)
        end
        value_index += 1
      end
    end
    children.concat(state[2])
  elsif node is Update
    state = node.structure()
    children = [].concat(state[5])
    children.push(state[0])
    unless state[1] == nil
      value_index = 0
      while value_index < state[1].length()
        value = state[1][state[1].key_at(value_index)]
        if value is AssignmentValue
          children.push(value)
        end
        value_index += 1
      end
    end
    children = children.concat(state[2])
    children.concat(state[3])
  elsif node is Delete
    state = node.structure()
    children = [].concat(state[4])
    children.push(state[0])
    children = children.concat(state[1])
    children.concat(state[2])
  else
    []
  end
end

def inspect(node) -> String
  if node is Attribute
    "Attribute(#{node.table().reference_name()}.#{node.name()})"
  elsif node is BinaryExpression
    right = "Bind(#{node.right()})"
    unless node.bind_right?()
      right = self.inspect(node.right())
    end
    "Binary(#{node.operator()}, #{self.inspect(node.left())}, #{right})"
  elsif node is Literal
    "Literal(#{node.value()})"
  elsif node is Function
    arguments = []
    index = 0
    while index < node.arguments().length()
      arguments.push(self.inspect(node.arguments()[index]))
      index += 1
    end
    "Function(#{node.name()}, [#{arguments.join(", ")}])"
  elsif node is Cast
    "Cast(#{self.inspect(node.expression())}, #{node.type_name()})"
  elsif node is ExcludedAttribute
    "Excluded(#{node.name()})"
  elsif node is Table
    if node.table_alias() == nil
      "Table(#{node.name()})"
    else
      "Table(#{node.name()} AS #{node.table_alias()})"
    end
  elsif node is QualifiedStar
    "QualifiedStar(#{node.table().reference_name()})"
  elsif node is ConflictAttribute
    "ConflictAttribute(#{node.name()})"
  elsif node is Exists
    prefix = "Exists"
    if node.negated?()
      prefix = "NotExists"
    end
    "#{prefix}(#{self.inspect(node.query())})"
  elsif node is ScalarSubquery
    "Scalar(#{self.inspect(node.query())})"
  elsif node is AssignmentValue
    "Assignment(#{self.inspect(node.expression())})"
  elsif node is ConflictTarget
    columns = node.columns().join(", ")
    "ConflictTarget(#{columns}, predicate=#{node.predicate() != nil})"
  elsif node is DefaultValues
    "DefaultValues"
  elsif node is RawSql
    "RawSql(#{node.sql()}, #{node.params().length()} binds)"
  elsif node is Collation
    "Collation(#{node.name()}, #{self.inspect(node.expression())})"
  else
    self.inspect_tail(node)
  end
end

def inspect_tail(node) -> String
  if node is Predicate
    right = "Bind(#{node.right()})"
    if node.right() is Attribute || node.right() is Literal
      right = self.inspect(node.right())
    end
    "Predicate(#{node.operator()}, #{self.inspect(node.left())}, #{right})"
  elsif node is Logical
    "Logical(#{node.operator()}, #{self.inspect(node.left())}, #{self.inspect(node.right())})"
  elsif node is Not
    "Not(#{self.inspect(node.expression())})"
  elsif node is Between
    operator = "BETWEEN"
    if node.negated?()
      operator = "NOT BETWEEN"
    end
    "Between(#{operator}, #{self.inspect(node.left())}, #{node.lower()}, #{node.upper()})"
  elsif node is Membership
    operator = "IN"
    if node.negated?()
      operator = "NOT IN"
    end
    if node.values() is Array
      "Membership(#{operator}, #{self.inspect(node.left())}, #{node.values().length()} values)"
    else
      "Membership(#{operator}, #{self.inspect(node.left())}, subquery)"
    end
  elsif node is Ordering
    nulls = ""
    unless node.nulls() == nil
      nulls = ", NULLS #{node.nulls()}"
    end
    "Ordering(#{node.direction()}#{nulls}, #{self.inspect(node.expression())})"
  elsif node is Alias
    "Alias(#{node.name()}, #{self.inspect(node.expression())})"
  elsif node is Join
    predicate = "none"
    unless node.predicate() == nil
      predicate = self.inspect(node.predicate())
    end
    "Join(#{node.kind()}, #{self.inspect(node.table())}, #{predicate})"
  elsif node is Cte
    mode = "ordinary"
    if node.recursive?()
      mode = "recursive"
    end
    "Cte(#{node.name()}, #{mode}, #{self.inspect(node.query())})"
  elsif node is Query
    "Query(from=#{node.base_reference_name()}, projections=#{node.projections().length()}, predicates=#{node.predicates().length()}, joins=#{node.joins().length()}, ctes=#{node.ctes().length()})"
  elsif node is CompoundQuery
    "Compound(#{node.operator()}, #{self.inspect(node.left())}, #{self.inspect(node.right())})"
  elsif node is Insert
    state = node.structure()
    source = state[4] != nil
    "Insert(into=#{state[0].reference_name()}, rows=#{state[1].length()}, source=#{source}, returning=#{state[2].length()}, ctes=#{state[8].length()})"
  elsif node is Update
    state = node.structure()
    assignments = 0
    unless state[1] == nil
      assignments = state[1].length()
    end
    "Update(table=#{state[0].reference_name()}, assignments=#{assignments}, predicates=#{state[2].length()}, returning=#{state[3].length()}, all=#{state[4]}, ctes=#{state[5].length()})"
  elsif node is Delete
    state = node.structure()
    "Delete(from=#{state[0].reference_name()}, predicates=#{state[1].length()}, returning=#{state[2].length()}, all=#{state[3]}, ctes=#{state[4].length()})"
  elsif node is ArelInspectable
    node.arel_inspect()
  else
    "ArelNode(unknown)"
  end
end

def same?(left, right) -> Bool
  if left is Attribute
    right is Attribute && left.table().reference_name() == right.table().reference_name() &&
      left.name() == right.name()
  elsif left is BinaryExpression
    if !(right is BinaryExpression) || left.operator() != right.operator() ||
       left.bind_right?() != right.bind_right?() || !self.same?(left.left(), right.left())
      false
    elsif left.bind_right?()
      left.right() == right.right()
    else
      self.same?(left.right(), right.right())
    end
  elsif left is Literal
    right is Literal && left.value() == right.value()
  elsif left is Cast
    right is Cast && left.type_name() == right.type_name() &&
      self.same?(left.expression(), right.expression())
  elsif left is ExcludedAttribute
    right is ExcludedAttribute && left.name() == right.name()
  elsif left is Function
    if !(right is Function) || left.name() != right.name() ||
       left.distinct?() != right.distinct?() || left.arguments().length() != right.arguments().length()
      return false
    end
    index = 0
    while index < left.arguments().length()
      unless self.same?(left.arguments()[index], right.arguments()[index])
        return false
      end
      index += 1
    end
    true
  elsif left is RawSql
    if !(right is RawSql) || left.sql() != right.sql() ||
       left.params().length() != right.params().length()
      return false
    end
    index = 0
    while index < left.params().length()
      if left.params()[index] != right.params()[index]
        return false
      end
      index += 1
    end
    true
  elsif left is AssignmentValue
    right is AssignmentValue && self.same?(left.expression(), right.expression())
  elsif left is DefaultValues
    right is DefaultValues
  elsif left is QualifiedStar
    right is QualifiedStar && self.same?(left.table(), right.table())
  elsif left is ConflictAttribute
    right is ConflictAttribute && left.name() == right.name()
  elsif left is Exists
    right is Exists && left.negated?() == right.negated?() &&
      self.same?(left.query(), right.query())
  elsif left is ScalarSubquery
    right is ScalarSubquery && self.same?(left.query(), right.query())
  elsif left is ConflictTarget
    if !(right is ConflictTarget) || left.columns().length() != right.columns().length() ||
       (left.predicate() == nil) != (right.predicate() == nil)
      return false
    end
    index = 0
    while index < left.columns().length()
      if left.columns()[index] != right.columns()[index]
        return false
      end
      index += 1
    end
    left.predicate() == nil || self.same?(left.predicate(), right.predicate())
  elsif left is Predicate
    if !(right is Predicate) || left.operator() != right.operator() ||
       !self.same?(left.left(), right.left())
      false
    elsif left.right() is Attribute || left.right() is Literal
      self.same?(left.right(), right.right())
    else
      left.right() == right.right()
    end
  elsif left is Logical
    right is Logical && left.operator() == right.operator() &&
      self.same?(left.left(), right.left()) && self.same?(left.right(), right.right())
  elsif left is Not
    right is Not && self.same?(left.expression(), right.expression())
  else
    self.same_tail?(left, right)
  end
end

def same_nodes?(left, right) -> Bool
  if left == nil || right == nil
    return left == nil && right == nil
  end
  if left.length() != right.length()
    return false
  end
  if left is Hash
    unless right is Hash
      return false
    end
    index = 0
    while index < left.length()
      key = left.key_at(index)
      unless right.include_key?(key)
        return false
      end
      left_value = left[key]
      right_value = right[key]
      if left_value is AssignmentValue
        unless self.same?(left_value, right_value)
          return false
        end
      elsif left_value != right_value
        return false
      end
      index += 1
    end
    return true
  end
  index = 0
  while index < left.length()
    unless self.same?(left[index], right[index])
      return false
    end
    index += 1
  end
  true
end

def same_insert?(left: Arel::Insert, right: Arel::Insert) -> Bool
  left_state = left.structure()
  right_state = right.structure()
  if !self.same?(left_state[0], right_state[0]) ||
     left_state[1].length() != right_state[1].length() ||
     !self.same_nodes?(left_state[2], right_state[2]) ||
     left_state[3].length() != right_state[3].length() || left_state[6] != right_state[6] ||
     !self.same_nodes?(left_state[7], right_state[7]) ||
     !self.same_nodes?(left_state[8], right_state[8]) ||
     (left_state[4] == nil) != (right_state[4] == nil)
    return false
  end
  index = 0
  while index < left_state[3].length()
    if left_state[3][index] != right_state[3][index]
      return false
    end
    index += 1
  end
  if left_state[4] != nil && !self.same?(left_state[4], right_state[4])
    return false
  end
  if (left_state[5] is Array) != (right_state[5] is Array)
    return false
  end
  if left_state[5] is Array
    if left_state[5].length() != right_state[5].length()
      return false
    end
    index = 0
    while index < left_state[5].length()
      if left_state[5][index] != right_state[5][index]
        return false
      end
      index += 1
    end
  elsif !self.same?(left_state[5], right_state[5])
    return false
  end
  index = 0
  while index < left_state[1].length()
    left_row = left_state[1][index]
    right_row = right_state[1][index]
    if left_row is DefaultValues
      unless self.same?(left_row, right_row)
        return false
      end
    elsif !self.same_nodes?(left_row, right_row)
      return false
    end
    index += 1
  end
  true
end

def same_tail?(left, right) -> Bool
  if left is Between
    right is Between && left.negated?() == right.negated?() &&
      left.lower() == right.lower() && left.upper() == right.upper() &&
      self.same?(left.left(), right.left())
  elsif left is Membership
    if !(right is Membership) || left.negated?() != right.negated?() ||
       !self.same?(left.left(), right.left()) ||
       (left.values() is Array) != (right.values() is Array)
      return false
    end
    unless left.values() is Array
      return self.same?(left.values(), right.values())
    end
    if left.values().length() != right.values().length()
      return false
    end
    index = 0
    while index < left.values().length()
      if left.values()[index] != right.values()[index]
        return false
      end
      index += 1
    end
    true
  elsif left is Ordering
    right is Ordering && left.direction() == right.direction() &&
      left.nulls() == right.nulls() && self.same?(left.expression(), right.expression())
  elsif left is Alias
    right is Alias && left.name() == right.name() &&
      self.same?(left.expression(), right.expression())
  elsif left is Collation
    right is Collation && left.name() == right.name() &&
      self.same?(left.expression(), right.expression())
  elsif left is Join
    right is Join && left.kind() == right.kind() &&
      self.same?(left.table(), right.table()) &&
      ((left.predicate() == nil && right.predicate() == nil) ||
       (left.predicate() != nil && right.predicate() != nil &&
        self.same?(left.predicate(), right.predicate())))
  elsif left is Table
    right is Table && left.name() == right.name() &&
      left.table_alias() == right.table_alias()
  elsif left is Cte
    right is Cte && left.name() == right.name() &&
      left.recursive?() == right.recursive?() && self.same?(left.query(), right.query())
  elsif left is CompoundQuery
    if !(right is CompoundQuery) || left.operator() != right.operator() ||
       left.limit_value() != right.limit_value() || left.offset_value() != right.offset_value() ||
       !self.same?(left.left(), right.left()) || !self.same?(left.right(), right.right())
      return false
    end
    self.same_nodes?(left.orderings(), right.orderings())
  elsif left is Update
    unless right is Update
      return false
    end
    left_state = left.structure()
    right_state = right.structure()
    self.same?(left_state[0], right_state[0]) &&
      self.same_nodes?(left_state[1], right_state[1]) &&
      self.same_nodes?(left_state[2], right_state[2]) &&
      self.same_nodes?(left_state[3], right_state[3]) &&
      left_state[4] == right_state[4] && self.same_nodes?(left_state[5], right_state[5])
  elsif left is Insert
    right is Insert && self.same_insert?(left, right)
  elsif left is Delete
    unless right is Delete
      return false
    end
    left_state = left.structure()
    right_state = right.structure()
    self.same?(left_state[0], right_state[0]) &&
      self.same_nodes?(left_state[1], right_state[1]) &&
      self.same_nodes?(left_state[2], right_state[2]) &&
      left_state[3] == right_state[3] && self.same_nodes?(left_state[4], right_state[4])
  elsif left is Query
    if !(right is Query) || left.base_reference_name() != right.base_reference_name() ||
       left.distinct_value() != right.distinct_value() ||
       left.limit_value() != right.limit_value() || left.offset_value() != right.offset_value() ||
       !self.same_nodes?(left.projections(), right.projections()) ||
       !self.same_nodes?(left.predicates(), right.predicates()) ||
       !self.same_nodes?(left.orderings(), right.orderings()) ||
       !self.same_nodes?(left.groups(), right.groups()) ||
       !self.same_nodes?(left.havings(), right.havings()) ||
       !self.same_nodes?(left.correlations(), right.correlations()) ||
       !self.same_nodes?(left.joins(), right.joins()) ||
       !self.same_nodes?(left.ctes(), right.ctes())
      return false
    end
    if (left.source_query() == nil) != (right.source_query() == nil)
      return false
    end
    if left.source_query() != nil && !self.same?(left.source_query(), right.source_query())
      return false
    end
    true
  elsif left is ArelComparableNode
    left.arel_same?(right)
  else
    false
  end
end
end

  def self.table(name: String) = Table.new(name)
  def self.cte(name: String) = CteRelation.new(name)
  def self.as(expression, name: String) = Alias.new(expression, name)
  def self.asc(expression) = Ordering.new(expression, "ASC")
  def self.desc(expression) = Ordering.new(expression, "DESC")
  def self.sql(fragment: String, params = nil)
    bound = params
    if bound == nil
      bound = []
    end
    RawSql.new(fragment, bound)
  end
  def self.count(expression) = Function.new("COUNT", [expression])
  def self.count_distinct(expression) = Function.new("COUNT", [expression], true)
  def self.sum(expression) = Function.new("SUM", [expression])
  def self.min(expression) = Function.new("MIN", [expression])
  def self.max(expression) = Function.new("MAX", [expression])
  def self.avg(expression) = Function.new("AVG", [expression])
  def self.lower(expression) = Function.new("LOWER", [expression])
  def self.upper(expression) = Function.new("UPPER", [expression])
  def self.function(name: String, arguments: Array) = Function.new(name, arguments)
  def self.exists(query) = Exists.new(query, false)
  def self.not_exists(query) = Exists.new(query, true)
  def self.scalar(query) = ScalarSubquery.new(query)
  def self.expression(expression) = AssignmentValue.new(expression)
  def self.excluded(name: String) = ExcludedAttribute.new(name)
  def self.literal(value) = Literal.new(value)
  def self.cast(expression, type_name: String) = Cast.new(expression, type_name)
  def self.integer_operator(expression, operator: String, value)
    if operator != "&" && operator != "|" && operator != "<<" && operator != ">>"
      raise ArgumentError.new("unsupported SQL integer operator")
    end
    BinaryExpression.new(expression, operator, value)
  end
  def self.conflict_target(columns) = ConflictTarget.new(arel_array(columns))
  def self.conflict_target_on_constraint(name: String) = ConflictConstraintTarget.new(name)
  def self.column_default() = ColumnDefault.new()
  def self.render(statement, visitor = nil) = statement.to_sql(visitor)
  def self.inspect(node) = Inspector.new().inspect(node)
  def self.same?(left, right) = Inspector.new().same?(left, right)
  def self.children(node) = Inspector.new().children(node)
  def self.walk(node, visitor = nil) = Inspector.new().walk(node, visitor)
  def self.simplify(node, rules = [], report = false) = Inspector.new().simplify(node, rules, report)
  def self.with_children(node, replacements: Array) = Inspector.new().with_children(node, replacements)
  def self.union(left, right) = CompoundQuery.new(left, "UNION", right)
  def self.union_all(left, right) = CompoundQuery.new(left, "UNION ALL", right)
  def self.intersect(left, right) = CompoundQuery.new(left, "INTERSECT", right)
  def self.except(left, right) = CompoundQuery.new(left, "EXCEPT", right)
  def self.insert_into(table: Arel::Table) = Insert.new(table)
  def self.update(table: Arel::Table) = Update.new(table)
  def self.delete_from(table: Arel::Table) = Delete.new(table)
  def self.from_subquery(query, name: String)
    Query.new(name, [], [], nil, nil, [RawSql.new("*", [])], true, true,
      name, false, [], [], [], query)
  end
  def self.from(table)
    if table is Table
      Query.for_table(table)
    else
      Query.new(table, [], [], nil, nil, ["*"], false, false)
    end
  end
end
