module Arel

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

end
