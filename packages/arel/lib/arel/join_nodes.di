module Arel

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

end
