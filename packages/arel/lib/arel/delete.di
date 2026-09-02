module Arel

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
      PreparedStatements.for(db, sql).execute(params)
    end
    def to_a(db, visitor = nil)
      sql, params = self.to_sql(visitor)
      PreparedStatements.for(db, sql).query(params)
    end
  end

end
