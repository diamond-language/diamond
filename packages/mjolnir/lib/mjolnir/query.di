module Mjolnir

  # An immutable query over one Schema, wrapping an Arel::Query. Every
  # method returns a new Query; nothing touches the database until it is
  # handed to a Repo. `arel()` exposes the underlying Arel::Query for
  # anything this wrapper does not cover.
  class Query
    def initialize(schema: Schema, arel)
      @schema = schema
      @arel = arel
    end

    def schema() -> Schema = @schema
    def arel() = @arel

    # A Hash of field => value (ANDed equality; nil means IS NULL) or an
    # Arel predicate built from `schema.column(...)`.
    def where(condition) -> Query
      unless condition is Hash
        return Query.new(@schema, @arel.where(condition))
      end
      result = @arel
      condition.keys().each() do |name|
        result = result.where(@schema.column(name).eq(condition[name]))
      end
      Query.new(@schema, result)
    end

    def order_by(field, direction = :asc) -> Query
      column = @schema.column(field)
      ordering = if direction == :desc then column.desc() else column.asc() end
      Query.new(@schema, @arel.order(ordering))
    end

    def limit(n: Int) -> Query = Query.new(@schema, @arel.take(n))
    def offset(n: Int) -> Query = Query.new(@schema, @arel.skip(n))
    def to_sql(visitor = nil) -> Array = @arel.to_sql(visitor)
  end

end
