module Arel

  class Visitor
    def require_extension(name: String)
      unless self.supports_extension?(name)
        raise ArgumentError.new("#{self.visitor_name()} visitor does not support #{name}")
      end
    end

    def attribute_allowed?(attribute: Arel::Attribute) -> Bool
      if @query == nil
        true
      elsif attribute.table().reference_name().downcase() == @query.base_reference_name().downcase()
        true
      else
        found = false
        reference_name = attribute.table().reference_name()
        index = 0
        while index < @query.joins().length()
          if @query.joins()[index].table().reference_name().downcase() == reference_name.downcase()
            found = true
          end
          index += 1
        end
        index = 0
        while index < @query.correlations().length()
          if @query.correlations()[index].reference_name().downcase() == reference_name.downcase()
            found = true
          end
          index += 1
        end
        found
      end
    end

    def render_attribute(attribute: Arel::Attribute) -> String
      unless self.attribute_allowed?(attribute)
        raise ArgumentError.new("attribute belongs to a relation outside this query")
      end
      self.quote_identifier(attribute.table().reference_name()) + "." + self.quote_identifier(attribute.name())
    end

    def render_table(table: Arel::Table) -> String
      sql = self.quote_identifier(table.name())
      unless table.table_alias() == nil
        sql = sql + " AS " + self.quote_identifier(table.table_alias())
      end
      sql
    end

    def render_source(query, params: Array) -> String
      if query.source_query() != nil
        source_sql, source_params = query.source_query().render_with(self)
        source_index = 0
        while source_index < source_params.length()
          params.push(source_params[source_index])
          source_index += 1
        end
        "(#{source_sql}) AS #{self.quote_identifier(query.base_reference_name())}"
      else
        sql = query.table_name()
        if query.quoted_identifiers()
          sql = self.quote_identifier(sql)
          unless query.table_alias() == nil
            sql = sql + " AS " + self.quote_identifier(query.table_alias())
          end
        end
        sql
      end
    end

    def render_ctes(query, params: Array) -> String
      entries = []
      recursive = false
      index = 0
      while index < query.ctes().length()
        cte = query.ctes()[index]
        if cte.recursive?()
          self.require_extension("recursive CTEs")
          recursive = true
        end
        sql, bound = cte.query().render_with(self)
        bound_index = 0
        while bound_index < bound.length()
          params.push(bound[bound_index])
          bound_index += 1
        end
        entries.push("#{self.quote_identifier(cte.name())} AS (#{sql})")
        index += 1
      end
      if entries.length() == 0
        ""
      else
        self.render_cte_prefix(entries, recursive)
      end
    end

    def render_cte_prefix(entries: Array, recursive: Bool) -> String
      prefix = "WITH "
      if recursive
        prefix = "WITH RECURSIVE "
      end
      "#{prefix}#{entries.join(", ")} "
    end

    def render_expression(expression, params: Array) -> String
      if expression is Attribute
        self.render_attribute(expression)
      elsif expression is Predicate
        left = self.render_expression(expression.left(), params)
        value = expression.right()
        if value == nil
          if expression.operator() == "="
            "#{left} IS NULL"
          elsif expression.operator() == "!="
            "#{left} IS NOT NULL"
          else
            raise ArgumentError.new("nil only supports eq/not_eq predicates")
          end
        elsif value is Attribute
          "#{left} #{expression.operator()} #{self.render_attribute(value)}"
        elsif value is Literal
          "#{left} #{expression.operator()} #{self.render_expression(value, params)}"
        else
          params.push(value)
          "#{left} #{expression.operator()} ?"
        end
      elsif expression is Logical
        left = self.render_expression(expression.left(), params)
        right = self.render_expression(expression.right(), params)
        "(#{left} #{expression.operator()} #{right})"
      elsif expression is Membership
        if !(expression.values() is Array)
          subquery_sql, subquery_params = expression.values().render_with(self)
          subquery_index = 0
          while subquery_index < subquery_params.length()
            params.push(subquery_params[subquery_index])
            subquery_index += 1
          end
          operator = "IN"
          if expression.negated?()
            operator = "NOT IN"
          end
          "#{self.render_expression(expression.left(), params)} #{operator} (#{subquery_sql})"
        elsif expression.values().length() == 0
          if expression.negated?()
            "1 = 1"
          else
            "1 = 0"
          end
        else
          placeholders = []
          value_index = 0
          while value_index < expression.values().length()
            params.push(expression.values()[value_index])
            placeholders.push("?")
            value_index += 1
          end
          operator = "IN"
          if expression.negated?()
            operator = "NOT IN"
          end
          "#{self.render_expression(expression.left(), params)} #{operator} (#{placeholders.join(", ")})"
        end
      elsif expression is Between
        params.push(expression.lower())
        params.push(expression.upper())
        operator = "BETWEEN"
        if expression.negated?()
          operator = "NOT BETWEEN"
        end
        "#{self.render_expression(expression.left(), params)} #{operator} ? AND ?"
      elsif expression is Collation
        inner = self.render_expression(expression.expression(), params)
        "#{inner} COLLATE #{self.quote_identifier(expression.name())}"
      elsif expression is Exists
        sql, bound = expression.query().render_with(self)
        bound_index = 0
        while bound_index < bound.length()
          params.push(bound[bound_index])
          bound_index += 1
        end
        prefix = "EXISTS"
        if expression.negated?()
          prefix = "NOT EXISTS"
        end
        "#{prefix} (#{sql})"
      elsif expression is ScalarSubquery
        sql, bound = expression.query().render_with(self)
        bound_index = 0
        while bound_index < bound.length()
          params.push(bound[bound_index])
          bound_index += 1
        end
        "(#{sql})"
      else
        self.render_expression_tail(expression, params)
      end
    end

    # Keep the nominal narrowing chain in each method below Diamond's
    # eight-alternative union ceiling as the AST grows.
    def render_expression_tail(expression, params: Array) -> String
      if expression is Function
        arguments = []
        argument_index = 0
        while argument_index < expression.arguments().length()
          arguments.push(self.render_expression(expression.arguments()[argument_index], params))
          argument_index += 1
        end
        prefix = ""
        if expression.distinct?()
          prefix = "DISTINCT "
        end
        "#{expression.name()}(#{prefix}#{arguments.join(", ")})"
      elsif expression is QualifiedStar
        unless self.attribute_allowed?(Attribute.new(expression.table(), "*"))
          raise ArgumentError.new("wildcard belongs to a relation outside this query")
        end
        self.quote_identifier(expression.table().reference_name()) + ".*"
      elsif expression is Not
        inner = self.render_expression(expression.expression(), params)
        "(NOT #{inner})"
      elsif expression is Ordering
        sql = "#{self.render_expression(expression.expression(), params)} #{expression.direction()}"
        unless expression.nulls() == nil
          self.require_extension("explicit NULL ordering")
          sql = sql + " NULLS #{expression.nulls()}"
        end
        sql
      elsif expression is Alias
        inner = self.render_expression(expression.expression(), params)
        "#{inner} AS #{self.quote_identifier(expression.name())}"
      elsif expression is RawSql
        param_index = 0
        while param_index < expression.params().length()
          params.push(expression.params()[param_index])
          param_index += 1
        end
        expression.sql()
      else
        self.render_expression_extension(expression, params)
      end
    end

    def render_expression_extension(expression, params: Array) -> String
      if expression is ExcludedAttribute
        self.require_extension("excluded-row attributes")
        "excluded.#{self.quote_identifier(expression.name())}"
      elsif expression is ConflictAttribute
        self.quote_identifier(expression.name())
      elsif expression is BinaryExpression
        if expression.operator() == "&" || expression.operator() == "|" ||
            expression.operator() == "<<" || expression.operator() == ">>"
          self.require_extension("integer bitwise operators")
        end
        left = self.render_expression(expression.left(), params)
        right = ""
        if expression.bind_right?()
          params.push(expression.right())
          right = "?"
        else
          right = self.render_expression(expression.right(), params)
        end
        "(#{left} #{expression.operator()} #{right})"
      elsif expression is Literal
        self.render_literal(expression.value())
      elsif expression is Cast
        inner = self.render_expression(expression.expression(), params)
        "CAST(#{inner} AS #{expression.type_name()})"
      elsif expression is String
        expression
      else
        raise TypeError.new("unsupported Arel expression")
      end
    end

    def render_literal(value) -> String
      if value is Bool
        if value
          "TRUE"
        else
          "FALSE"
        end
      else
        "#{value}"
      end
    end

    def render_join(join: Arel::Join, params: Array) -> String
      sql = "#{join.kind()} JOIN #{self.render_table(join.table())}"
      unless join.predicate() == nil
        sql = sql + " ON " + self.render_expression(join.predicate(), params)
      end
      sql
    end

    def render(query) -> Array
      previous_query = @query
      begin
      visitor = self
      params = []
      sql = self.render_ctes(query, params)
      @query = query
      projections = query.projections().map() do |projection|
        visitor.render_expression(projection, params)
      end
      table_sql = self.render_source(query, params)
      query.joins().each() do |join|
        table_sql += " " + visitor.render_join(join, params)
      end
      sql = sql + "SELECT "
      if query.distinct_value()
        sql = sql + "DISTINCT "
      end
      sql = sql + projections.join(", ") + " FROM #{table_sql}"

      predicates = query.predicates().map() do |predicate|
        visitor.render_expression(predicate, params)
      end
      if predicates.length() > 0
        sql = sql + " WHERE " + predicates.join(" AND ")
      end

      groups = query.groups().map() do |group|
        visitor.render_expression(group, params)
      end
      if groups.length() > 0
        sql = sql + " GROUP BY " + groups.join(", ")
      end

      havings = query.havings().map() do |having|
        visitor.render_expression(having, params)
      end
      if havings.length() > 0
        sql = sql + " HAVING " + havings.join(" AND ")
      end

      orderings = query.orderings().map() do |ordering|
        visitor.render_expression(ordering, params)
      end
      if orderings.length() > 0
        sql = sql + " ORDER BY " + orderings.join(", ")
      end

      sql = sql + self.render_pagination(query.limit_value(), query.offset_value(),
        params, query.bind_limits())
      [sql, params]
      ensure
        @query = previous_query
      end
    end

    def render_compound_branch(sql: String, grouped: Bool) -> String
      if grouped
        "SELECT * FROM (#{sql})"
      else
        sql
      end
    end

    def render_returning(expressions: Array, params: Array) -> String
      if expressions.length() > 0
        self.require_extension("returning clauses")
      end
      # `self` inside a `do...end` block doesn't resolve to this method's
      # own receiver (see `render`'s own `visitor = self` above, the
      # established workaround throughout this file) -- captured into a
      # local first.
      visitor = self
      rendered = expressions.map() do |expression|
        visitor.render_expression(expression, params)
      end
      if rendered.length() == 0
        ""
      else
        " RETURNING #{rendered.join(", ")}"
      end
    end

    def render_compound(query) -> Array = query.render_default(self)
    def render_insert(statement) -> Array = statement.render_default(self)
    def render_update(statement) -> Array = statement.render_default(self)
    def render_delete(statement) -> Array = statement.render_default(self)

  end

end
