module Arel

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

end
