module Arel

  class Join
    def initialize(table: Arel::Table, predicate, kind: String)
      @table = table
      @predicate = predicate
      @kind = kind
    end
    def table() -> Arel::Table = @table
    def predicate() = @predicate
    def kind() -> String = @kind
  end

  class Exists
    def initialize(query, negated: Bool)
      @query = query
      @negated = negated
    end
    def query() = @query
    def negated?() -> Bool = @negated
    def and_also(other) -> Logical = Logical.new(self, "AND", other)
    def or_else(other) -> Logical = Logical.new(self, "OR", other)
    def not_() -> Exists = Exists.new(@query, !@negated)
  end

  class ScalarSubquery
    def initialize(query)
      @query = query
    end
    def query() = @query
    def eq(value) -> Predicate = Predicate.new(self, "=", value)
    def not_eq(value) -> Predicate = Predicate.new(self, "!=", value)
    def lt(value) -> Predicate = Predicate.new(self, "<", value)
    def lteq(value) -> Predicate = Predicate.new(self, "<=", value)
    def gt(value) -> Predicate = Predicate.new(self, ">", value)
    def gteq(value) -> Predicate = Predicate.new(self, ">=", value)
  end

  class Cte
    def initialize(name: String, query, recursive = false)
      @name = name
      @query = query
      @recursive = recursive
    end
    def name() -> String = @name
    def query() = @query
    def recursive?() -> Bool = @recursive
  end

end
