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

    # A Hash of field => value (ANDed; nil means IS NULL, an Array means IN)
    # or an Arel predicate built from `schema.column(...)`.
    def where(condition) -> Query
      unless condition is Hash
        return Query.new(@schema, @arel.where(condition))
      end
      result = @arel
      condition.keys().each() do |name|
        value = condition[name]
        column = @schema.column(name)
        result = result.where(if value is Array then column.in_list(value) else column.eq(value) end)
      end
      Query.new(@schema, result)
    end

    # A field name (with :asc or :desc), or an Arel ordering / raw
    # expression such as `Arel.sql("RANDOM()")`, passed through as is.
    def order_by(field, direction = :asc) -> Query
      unless field is String || field is Symbol
        return Query.new(@schema, @arel.order(field))
      end
      column = @schema.column(field)
      ordering = if direction == :desc then column.desc() else column.asc() end
      Query.new(@schema, @arel.order(ordering))
    end

    def limit(n: Int) -> Query = Query.new(@schema, @arel.take(n))
    def offset(n: Int) -> Query = Query.new(@schema, @arel.skip(n))
    def to_sql(visitor = nil) -> Array = @arel.to_sql(visitor)
  end

end
