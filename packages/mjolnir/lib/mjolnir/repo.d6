module Mjolnir

  # The only object that touches the database. Stateless apart from the
  # connection it was handed: no identity map, no unit of work, no hidden
  # flush -- every method is one visible round trip (or a transaction).
  class Repo
    def initialize(db, visitor = nil)
      @db = db
      @visitor = visitor
    end

    def db() = @db

    def all(query: Query) -> Array
      schema = query.schema()
      rows = query.arel().to_a(@db, @visitor)
      entities = []
      rows.each() do |row|
        entities.push(schema.build(row))
      end
      entities
    end

    def one(query: Query)
      found = self.all(query.limit(1))
      if found.empty?() then nil else found[0] end
    end

    def get(schema: Schema, id)
      query = schema.query()
      schema.key_predicates(id).each() do |predicate|
        query = query.where(predicate)
      end
      self.one(query)
    end

    def count(query: Query) -> Int = query.arel().count(@db, @visitor)

    def exists?(query: Query) -> Bool = self.one(query) != nil

    # The first row whose fields equal `conditions` (a Hash, as for
    # Query#where), or nil.
    def get_by(schema: Schema, conditions: Hash) = self.one(schema.query().where(conditions))

    # Loads one association for many entities in a single query and returns
    # a Hash keyed by each entity's primary key: an Array of rows for
    # has_many (empty when none), a row or nil for belongs_to. `scope` is an
    # optional Query on the target schema (ordering, extra conditions)
    # applied before the lookup.
    #
    #   posts = repo.preload(users, users_list, "posts")
    #   posts[user.id()]   # Array of Post
    def preload(schema: Schema, entities: Array, name: String, scope = nil) -> Hash
      if schema.composite?()
        raise ArgumentError.new("preload does not support the composite key of #{schema.table_name()}")
      end
      found = schema.association(name)
      target = found["target"]
      key = found["key"]
      result = {}
      if entities.empty?()
        return result
      end
      base = if scope == nil then target.query() else scope end
      if found["kind"] == "has_many"
        owner_ids = entities.map() do |entity| entity.public_send(schema.primary_key()) end
        owner_ids.each() do |id| result[id] = [] end
        condition = {}
        condition[key] = owner_ids
        self.all(base.where(condition)).each() do |row|
          result[row.public_send(key)].push(row)
        end
      else
        wanted = {}
        entities.each() do |entity|
          value = entity.public_send(key)
          if value != nil then wanted[value] = true end
        end
        by_id = {}
        condition = {}
        condition[target.primary_key()] = wanted.keys()
        self.all(base.where(condition)).each() do |row|
          by_id[row.public_send(target.primary_key())] = row
        end
        entities.each() do |entity|
          result[entity.public_send(schema.primary_key())] = by_id[entity.public_send(key)]
        end
      end
      result
    end

    # Ok(entity) with the stored row, or Err(changeset) if the changeset
    # is invalid or a declared unique constraint failed.
    def insert(changeset: Changeset)
      unless changeset.valid?()
        return Err.new(changeset)
      end
      schema = changeset.schema()
      statement = Arel.insert_into(schema.table())
      values = self.stamped(changeset, true)
      if values.empty?()
        statement = statement.default_values()
      else
        statement = statement.values(values)
      end
      if self.supports_returning?(false)
        self.write(changeset, statement.returning(["*"]))
      else
        self.insert_then_read(changeset, statement, values)
      end
    end

    # Writes only the changed fields of a persisted entity. A changeset
    # with no changes is Ok(entity) without a query.
    def update(changeset: Changeset)
      if changeset.new_record?()
        raise ArgumentError.new("cannot update a changeset for a new record; use insert")
      end
      unless changeset.valid?()
        return Err.new(changeset)
      end
      if changeset.changes().empty?()
        return Ok.new(changeset.entity())
      end
      schema = changeset.schema()
      # The row is found by the loaded entity's key, not by a pending change: a permitted 'id' field would otherwise aim the UPDATE at whatever row
      # the submitted value names.
      parts = schema.primary_keys().map() do |key| changeset.original_field(key) end
      id = if schema.composite?() then parts else parts[0] end
      values = self.stamped(changeset, false)
      statement = Arel.update(schema.table()).set(values)
      schema.key_predicates(id).each() do |predicate|
        statement = statement.where(predicate)
      end
      result = if self.supports_returning?(true)
        self.write(changeset, statement.returning(["*"]))
      else
        self.update_then_read(changeset, statement, parts, values)
      end
      if result is Ok && result.value() == nil
        raise StaleEntryError.new(schema.table_name(), id)
      end
      result
    end

    # Like insert/update, but returns the entity and raises
    # InvalidChangesetError (or a constraint's own error) instead of Err.
    def insert!(changeset: Changeset) = self.unwrap(self.insert(changeset))
    def update!(changeset: Changeset) = self.unwrap(self.update(changeset))

    # Sets `changes` (a Hash) on every row `query` matches; returns the
    # number of rows changed. Stamps updated_at on a timestamped schema.
    # Refuses a query with no condition.
    def update_all(query: Query, changes: Hash)
      schema = query.schema()
      predicates = query.arel().predicates()
      if predicates.empty?()
        raise ArgumentError.new("update_all needs a condition on #{schema.table_name()}")
      end
      # Only declared fields: a column name from outside the schema is an error here, not a statement for the database to reject (or accept).
      changes.keys().each() do |name|
        unless schema.field?(name)
          raise UnknownFieldError.new(schema.table_name(), name)
        end
      end
      values = changes
      if schema.timestamps?() && !changes.has_key?("updated_at")
        stamp = {}
        stamp["updated_at"] = Time.now().to_i()
        values = changes.merge(stamp)
      end
      statement = Arel.update(schema.table()).set(values)
      predicates.each() do |predicate|
        statement = statement.where(predicate)
      end
      statement.execute(@db, @visitor)
    end

    # Removes every row `query` matches; returns the number removed.
    # Refuses a query with no condition.
    def delete_all(query: Query)
      schema = query.schema()
      predicates = query.arel().predicates()
      if predicates.empty?()
        raise ArgumentError.new("delete_all needs a condition on #{schema.table_name()}")
      end
      statement = Arel.delete_from(schema.table())
      predicates.each() do |predicate|
        statement = statement.where(predicate)
      end
      statement.execute(@db, @visitor)
    end

    # Rows removed.
    def delete(schema: Schema, id)
      statement = Arel.delete_from(schema.table())
      schema.key_predicates(id).each() do |predicate|
        statement = statement.where(predicate)
      end
      statement.execute(@db, @visitor)
    end

    # Runs `callback` (given this repo) in a transaction. An exception
    # rolls back and propagates; a returned Err rolls back and is returned;
    # anything else commits. On SQLite the write lock is taken up front
    # (BEGIN IMMEDIATE), so a read followed by a write cannot fail partway through
    # when another connection writes in between; a competing writer waits
    # for the connection's busy timeout instead.
    def transaction(callback: Callable[1])
      sqlite = @visitor == nil || @visitor.is_a?(Arel::SQLiteVisitor)
      @db.execute(if sqlite then "BEGIN IMMEDIATE" else "BEGIN" end)
      begin
        result = callback(self)
        if result is Err
          @db.execute("ROLLBACK")
        else
          @db.execute("COMMIT")
        end
        result
      rescue error: StandardError
        @db.execute("ROLLBACK")
        raise error
      end
    end

    def unwrap(result)
      if result is Err
        raise InvalidChangesetError.new(result.error())
      end
      result.value()
    end

    # The changeset's changes, plus created_at/updated_at on a timestamped
    # schema unless the changeset set them.
    def stamped(changeset: Changeset, creating: Bool) -> Hash
      changes = changeset.changes()
      unless changeset.schema().timestamps?()
        return changes
      end
      now = Time.now().to_i()
      stamps = {}
      if creating && !changes.has_key?("created_at")
        stamps["created_at"] = now
      end
      unless changes.has_key?("updated_at")
        stamps["updated_at"] = now
      end
      changes.merge(stamps)
    end

    # Whether the database can hand the written row back from the statement itself. SQLite and PostgreSQL can for INSERT and UPDATE, MariaDB
    # only for INSERT (its UPDATE ... RETURNING is a syntax error), MySQL for neither; the Arel visitor in use says which. No visitor means
    # SQLite.
    def supports_returning?(for_update: Bool) -> Bool
      return true if @visitor == nil
      return false unless @visitor.supports_extension?("returning clauses")
      if for_update then @visitor.supports_extension?("RETURNING on UPDATE") else true end
    end

    # INSERT without RETURNING: run it, work out the new row's key (the values it supplied, and the connection's last insert id for a single
    # generated key), and read the row back by that key.
    def insert_then_read(changeset: Changeset, statement, values: Hash)
      schema = changeset.schema()
      begin
        statement.execute(@db, @visitor)
        parts = schema.primary_keys().map() do |key|
          if values.has_key?(key) then values[key] else nil end
        end
        missing = parts.select() do |part| part == nil end.length()
        if missing > 1
          raise ArgumentError.new("cannot read back a row of #{schema.table_name()} whose key is generated in more than one column")
        end
        if missing == 1
          generated = @db.last_insert_row_id()
          parts = parts.map() do |part| if part == nil then generated else part end end
        end
        id = if schema.composite?() then parts else parts[0] end
        Ok.new(self.get(schema, id))
      rescue error: StandardError
        self.constraint_failure(changeset, error)
      end
    end

    # UPDATE without RETURNING: run it, then read the row back by its key after the update (the new key when the update changed it). An
    # update that matched nothing leaves Ok(nil), which #update reports as a stale entry. The affected-row count alone cannot say that:
    # MySQL counts rows changed, so setting a column to the value it already had reports 0 for a row that exists.
    def update_then_read(changeset: Changeset, statement, original_parts: Array, values: Hash)
      schema = changeset.schema()
      begin
        affected = statement.execute(@db, @visitor)
        keys = schema.primary_keys()
        parts = []
        index = 0
        while index < keys.length()
          parts.push(if values.has_key?(keys[index]) then values[keys[index]] else original_parts[index] end)
          index += 1
        end
        if affected == 0 && parts != original_parts
          return Ok.new(nil)
        end
        id = if schema.composite?() then parts else parts[0] end
        Ok.new(self.get(schema, id))
      rescue error: StandardError
        self.constraint_failure(changeset, error)
      end
    end

    # Runs an INSERT/UPDATE ... RETURNING and wraps the stored row, or
    # turns a failure of a declared constraint into an Err.
    def write(changeset: Changeset, statement)
      begin
        rows = statement.to_a(@db, @visitor)
        if rows.empty?() then Ok.new(nil) else Ok.new(changeset.schema().build(rows[0])) end
      rescue error: StandardError
        self.constraint_failure(changeset, error)
      end
    end

    # A failed write: Err(changeset with the field error) for a declared unique constraint, otherwise the error is raised again.
    def constraint_failure(changeset: Changeset, error)
      failed = self.violated_constraint(changeset, error.message())
      if failed == nil
        raise error
      end
      Err.new(changeset.add_error(failed["field"], failed["message"]))
    end

    def violated_constraint(changeset: Changeset, message: String)
      table = changeset.schema().table_name()
      changeset.constraints().find() do |declared|
        columns = declared["columns"]
        # A composite UNIQUE is reported as "t.a, t.b"; a single one as "t.a".
        reported = if columns.empty?() then "#{table}.#{declared["field"]}" else columns.map() do |column| "#{table}.#{column}" end.join(", ") end
        by_column = message.include?("UNIQUE constraint failed: #{reported}")
        named = declared["constraint"]
        by_name = named != nil && message.include?(named)
        # PostgreSQL names the columns in the error detail ("Key (a, b)=(1, 2) already exists"); MySQL and MariaDB name the index
        # ("Duplicate entry '1' for key 'name'", MySQL 8 qualifies it with the table), which defaults to the first column of the constraint.
        names = if columns.empty?() then [declared["field"]] else columns end
        by_postgres = message.include?("Key (#{names.join(", ")})=")
        first = names[0]
        by_mysql = message.include?("Duplicate entry") && (message.include?("for key '#{first}'") || message.include?("for key '#{table}.#{first}'"))
        by_column || by_name || by_postgres || by_mysql
      end
    end
  end

end
