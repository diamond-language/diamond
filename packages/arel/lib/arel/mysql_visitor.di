module Arel

  # Real MySQL (not MariaDB), verified against a live MySQL 8 server, not
  # assumed from similarly-named syntax -- see packages/arel/ROADMAP.md for
  # the full comparison. Backed by the same native `MySQL` class
  # MariaDBVisitor's own tests already use (src/object.h, docs/io.md):
  # Diamond's "MariaDB" native support was never MariaDB-branded at the
  # native layer at all, it already speaks the real MySQL wire protocol via
  # MariaDB Connector/C, so this dialect needed no new native code, only a
  # new visitor.
  #
  # Structurally identical to MariaDBVisitor for everything actually shared
  # (backtick quoting, the 2^64-1 LIMIT sentinel for offset-without-limit,
  # `INSERT IGNORE`, bare `INSERT INTO t () VALUES ()`, the same four
  # rejected extensions below) -- confirmed live rather than assumed, since
  # MariaDB and MySQL share the same SQL dialect ancestry almost everywhere
  # except two things:
  #
  # - `RETURNING` doesn't exist in MySQL 8 at all (MariaDB has had it since
  #   10.5) -- rejected unconditionally here via the shared "returning
  #   clauses" gate (Visitor#render_returning), simpler than MariaDB's own
  #   two-part gate (which only needed a narrower "RETURNING on UPDATE"
  #   rejection, since MariaDB *does* support it on INSERT/DELETE).
  # - `ON DUPLICATE KEY UPDATE col = VALUES(col)` still works but is
  #   deprecated since MySQL 8.0.20 (warning 1287, verified live) in favor
  #   of a row-alias form, `INSERT ... VALUES (...) AS new_row ON DUPLICATE
  #   KEY UPDATE col = new_row.col`, for a VALUES(...)-list insert --
  #   confirmed live that the alias can always be present (no warning/error
  #   even when the update clause never references it), so
  #   render_mysql_upsert always includes it for that shape rather than
  #   conditionally deciding whether any assignment actually needs it.
  #   INSERT...SELECT...ON DUPLICATE KEY UPDATE has no row-alias equivalent
  #   at all though (every placement tried against a live MySQL 8 server is
  #   a syntax error, or silently aliases the wrong table) -- that shape
  #   keeps using the older `VALUES(col)` form, the only thing that actually
  #   works there. See @mysql_insert_uses_row_alias's own comment below for
  #   how the two forms are chosen per render.
  class MySQLVisitor < Visitor
    def visitor_name() = "MySQL"
    def quote_identifier(name: String) -> String = arel_quote_identifier_backtick(name)
    def supports_extension?(name: String) -> Bool
      name != "conflict-target predicates" && name != "named-constraint conflict targets" &&
        name != "explicit NULL ordering" && name != "write CTEs" &&
        name != "returning clauses"
    end
    def render_pagination(limit_value, offset_value, params: Array,
                          bind_values = true) -> String
      sql = ""
      if limit_value == nil && offset_value != nil
        # Same 2^64-1 "effectively unlimited" sentinel MariaDBVisitor uses
        # -- shared MariaDB/MySQL grammar, verified live against MySQL 8 too
        # (a bare `OFFSET n` is a syntax error there as well).
        sql = " LIMIT 18446744073709551615"
      elsif limit_value != nil
        if bind_values
          sql = " LIMIT ?"
          params.push(limit_value)
        else
          sql = " LIMIT #{limit_value}"
        end
      end
      unless offset_value == nil
        if bind_values
          sql = sql + " OFFSET ?"
          params.push(offset_value)
        else
          sql = sql + " OFFSET #{offset_value}"
        end
      end
      sql
    end

    # Which "excluded row" reference form to emit for the upsert currently
    # being rendered -- verified live that MySQL 8's row-alias syntax
    # (`new_row.col`) only exists for the VALUES(...) list form of INSERT.
    # INSERT...SELECT...ON DUPLICATE KEY UPDATE has no alias equivalent at
    # all: every placement of `AS new_row` tried against a real MySQL 8
    # server for that shape (after the table name, after the column list,
    # after the SELECT) is a syntax error there, or silently aliases the
    # wrong thing (the FROM-clause table, not the inserted row). The older,
    # still-functional-though-deprecated `VALUES(col)` function form is the
    # only thing that actually works for INSERT...SELECT, so this must stay
    # a per-render decision, not a fixed choice. Set by render_insert
    # immediately before calling render_mysql_upsert, read here while
    # rendering that same upsert's assignment expressions -- the same
    # mutable-instance-state pattern the base Visitor class already uses
    # for @query (see attribute_allowed?), needed because
    # render_expression_extension's signature is shared across every
    # visitor and can't take an extra parameter just for this.
    def render_expression_extension(expression, params: Array) -> String
      if expression is ExcludedAttribute
        self.require_extension("excluded-row attributes")
        if @mysql_insert_uses_row_alias
          "new_row.#{self.quote_identifier(expression.name())}"
        else
          "VALUES(#{self.quote_identifier(expression.name())})"
        end
      else
        super(expression, params)
      end
    end

    # Parallel to MariaDBVisitor#render_mariadb_upsert (same target-shape
    # validation, same do-nothing/do-update split), but rendering MySQL's
    # modern row-alias upsert grammar (`... AS new_row ON DUPLICATE KEY
    # UPDATE col = new_row.col`) for a VALUES(...)-list insert, or the
    # older `VALUES(col)` form for an INSERT...SELECT one -- see
    # @mysql_insert_uses_row_alias's own comment above for why both forms
    # are still needed. Caller (render_insert) sets that instance field
    # before calling this.
    def render_mysql_upsert(target, ignore: Bool, assignments, params: Array) -> String
      if !ignore && assignments == nil
        return ""
      end
      if target is ConflictConstraintTarget
        self.require_extension("named-constraint conflict targets")
      elsif target is ConflictTarget && target.predicate() != nil
        self.require_extension("conflict-target predicates")
      end
      self.require_extension("upsert conflict actions")
      if ignore
        return ""
      end
      if assignments.length() == 0
        raise ArgumentError.new("conflict update requires at least one assignment")
      end
      rendered_assignments = []
      assignment_index = 0
      while assignment_index < assignments.length()
        name = assignments.key_at(assignment_index)
        value = assignments[name]
        if value is AssignmentValue
          rendered = self.render_expression(value.expression(), params)
          rendered_assignments.push("#{self.quote_identifier(name)} = #{rendered}")
        else
          rendered_assignments.push("#{self.quote_identifier(name)} = ?")
          params.push(value)
        end
        assignment_index += 1
      end
      if @mysql_insert_uses_row_alias
        " AS new_row ON DUPLICATE KEY UPDATE #{rendered_assignments.join(", ")}"
      else
        " ON DUPLICATE KEY UPDATE #{rendered_assignments.join(", ")}"
      end
    end

    def render_insert(statement) -> Array
      table, rows, returning, source_columns, source_query, conflict_target, conflict_ignore, conflict_assignments, ctes = statement.structure()
      if ctes.length() > 0
        self.require_extension("write CTEs")
      end
      if rows.length() == 1 && rows[0] is DefaultValues
        self.require_extension("insert default values")
        params = []
        sql = "INSERT INTO #{self.quote_identifier(table.name())} () VALUES ()"
        sql = sql + self.render_returning(returning, params)
        return [sql, params]
      end
      unless source_query == nil
        if source_columns.length() == 0
          raise ArgumentError.new("INSERT SELECT requires at least one column")
        end
        unless source_query.projection_count_known?()
          raise ArgumentError.new("INSERT SELECT requires explicit projections")
        end
        if source_columns.length() != source_query.projection_count()
          raise ArgumentError.new("INSERT SELECT columns must match query projections")
        end
        columns = []
        column_index = 0
        while column_index < source_columns.length()
          columns.push(self.quote_identifier(source_columns[column_index]))
          column_index += 1
        end
        source_sql, params = source_query.render_with(self)
        keyword = "INSERT INTO"
        if conflict_ignore
          keyword = "INSERT IGNORE INTO"
        end
        sql = "#{keyword} #{self.quote_identifier(table.name())} " +
          "(#{columns.join(", ")}) #{source_sql}"
        @mysql_insert_uses_row_alias = false
        sql = sql + self.render_mysql_upsert(conflict_target, conflict_ignore,
          conflict_assignments, params)
        sql = sql + self.render_returning(returning, params)
        return [sql, params]
      end
      if rows.length() == 0 || rows[0].length() == 0
        raise ArgumentError.new("INSERT requires at least one value")
      end
      columns = []
      params = []
      first = rows[0]
      column_index = 0
      while column_index < first.length()
        columns.push(self.quote_identifier(first.key_at(column_index)))
        column_index += 1
      end
      value_groups = []
      row_index = 0
      while row_index < rows.length()
        row = rows[row_index]
        if row.length() != first.length()
          raise ArgumentError.new("INSERT rows must have identical columns")
        end
        placeholders = []
        index = 0
        while index < first.length()
          key = first.key_at(index)
          unless row.include_key?(key)
            raise ArgumentError.new("INSERT rows must have identical columns")
          end
          value = row[key]
          if value is AssignmentValue
            placeholders.push(self.render_expression(value.expression(), params))
          elsif value is ColumnDefault
            self.require_extension("per-column default values")
            placeholders.push("DEFAULT")
          else
            placeholders.push("?")
            params.push(value)
          end
          index += 1
        end
        value_groups.push("(#{placeholders.join(", ")})")
        row_index += 1
      end
      keyword = "INSERT INTO"
      if conflict_ignore
        keyword = "INSERT IGNORE INTO"
      end
      sql = "#{keyword} #{self.quote_identifier(table.name())} " +
        "(#{columns.join(", ")}) VALUES #{value_groups.join(", ")}"
      @mysql_insert_uses_row_alias = true
      sql = sql + self.render_mysql_upsert(conflict_target, conflict_ignore,
        conflict_assignments, params)
      sql = sql + self.render_returning(returning, params)
      [sql, params]
    end

    def render_update(statement) -> Array
      table, assignments, predicates, returning, allow_all, ctes = statement.structure()
      if ctes.length() > 0
        self.require_extension("write CTEs")
      end
      if assignments == nil || assignments.length() == 0
        raise ArgumentError.new("UPDATE requires at least one assignment")
      end
      if predicates.length() == 0 && !allow_all
        raise ArgumentError.new("UPDATE requires where() or explicit all()")
      end
      clauses = []
      params = []
      assignment_index = 0
      while assignment_index < assignments.length()
        name = assignments.key_at(assignment_index)
        value = assignments[name]
        if value is AssignmentValue
          rendered = self.render_expression(value.expression(), params)
          clauses.push("#{self.quote_identifier(name)} = #{rendered}")
        else
          clauses.push("#{self.quote_identifier(name)} = ?")
          params.push(value)
        end
        assignment_index += 1
      end
      sql = "UPDATE #{self.quote_identifier(table.name())} SET #{clauses.join(", ")}"
      rendered_predicates = []
      predicate_index = 0
      while predicate_index < predicates.length()
        rendered_predicates.push(self.render_expression(predicates[predicate_index], params))
        predicate_index += 1
      end
      if rendered_predicates.length() > 0
        sql = sql + " WHERE " + rendered_predicates.join(" AND ")
      end
      sql = sql + self.render_returning(returning, params)
      [sql, params]
    end

    def render_delete(statement) -> Array
      table, predicates, returning, allow_all, ctes = statement.structure()
      if ctes.length() > 0
        self.require_extension("write CTEs")
      end
      if predicates.length() == 0 && !allow_all
        raise ArgumentError.new("DELETE requires where() or explicit all()")
      end
      params = []
      sql = "DELETE FROM #{self.quote_identifier(table.name())}"
      rendered_predicates = []
      predicate_index = 0
      while predicate_index < predicates.length()
        rendered_predicates.push(self.render_expression(predicates[predicate_index], params))
        predicate_index += 1
      end
      if rendered_predicates.length() > 0
        sql = sql + " WHERE " + rendered_predicates.join(" AND ")
      end
      sql = sql + self.render_returning(returning, params)
      [sql, params]
    end
  end

end
