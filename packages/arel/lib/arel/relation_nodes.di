module Arel

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

end
