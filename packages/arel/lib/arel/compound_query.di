module Arel

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

end
