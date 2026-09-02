module Arel

  class Update
    def initialize(table: Arel::Table, assignments = nil, predicates = [], returning = [],
                   allow_all = false, ctes = [])
      @table = table
      @assignments = assignments
      @predicates = predicates
      @returning = returning
      @allow_all = allow_all
      @ctes = ctes
    end

    def ctes() = @ctes
    def structure() = [@table, @assignments, @predicates, @returning, @allow_all, @ctes]
    def set(assignments: Hash)
      Update.new(@table, assignments, @predicates, @returning, @allow_all, @ctes)
    end
    def where(predicate)
      Update.new(@table, @assignments, @predicates.concat([predicate]), @returning,
        @allow_all, @ctes)
    end
    def returning(expressions)
      Update.new(@table, @assignments, @predicates, arel_array(expressions), @allow_all,
        @ctes)
    end
    def all() = Update.new(@table, @assignments, @predicates, @returning, true, @ctes)
    def with(relation_or_name, query)
      name = arel_cte_name(relation_or_name)
      Update.new(@table, @assignments, @predicates, @returning, @allow_all,
        Arel.append_cte(@ctes, name, query))
    end
    def with_recursive(relation_or_name, query)
      name = arel_cte_name(relation_or_name)
      Update.new(@table, @assignments, @predicates, @returning, @allow_all,
        Arel.append_cte(@ctes, name, query, true))
    end

    def render_default(visitor) -> Array
      if @ctes.length() > 0
        visitor.require_extension("write CTEs")
      end
      if @assignments == nil || @assignments.length() == 0
        raise ArgumentError.new("UPDATE requires at least one assignment")
      end
      if @predicates.length() == 0 && !@allow_all
        raise ArgumentError.new("UPDATE requires where() or explicit all()")
      end
      clauses = []
      params = []
      assignment_index = 0
      while assignment_index < @assignments.length()
        name = @assignments.key_at(assignment_index)
        value = @assignments[name]
        if value is AssignmentValue
          rendered = visitor.render_expression(value.expression(), params)
          clauses.push("#{visitor.quote_identifier(name)} = #{rendered}")
        else
          clauses.push("#{visitor.quote_identifier(name)} = ?")
          params.push(value)
        end
        assignment_index += 1
      end
      sql = "UPDATE #{visitor.quote_identifier(@table.name())} SET #{clauses.join(", ")}"
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

    def render_with(visitor) -> Array = visitor.render_update(self)

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
