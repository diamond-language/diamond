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

end
