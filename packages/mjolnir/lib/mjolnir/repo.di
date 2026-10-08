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
      condition = {}
      condition[schema.primary_key()] = id
      self.one(schema.query().where(condition))
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
      self.write(changeset, statement.returning(["*"]))
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
      id = changeset.get_field(schema.primary_key())
      statement = Arel.update(schema.table())
        .set(self.stamped(changeset, false))
        .where(schema.column(schema.primary_key()).eq(id))
        .returning(["*"])
      result = self.write(changeset, statement)
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
      statement = Arel.delete_from(schema.table()).where(schema.column(schema.primary_key()).eq(id))
      statement.execute(@db, @visitor)
    end

    # Runs `callback` (given this repo) in a transaction. An exception
    # rolls back and propagates; a returned Err rolls back and is returned;
    # anything else commits.
    def transaction(callback: Callable[1])
      @db.execute("BEGIN")
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

    # Runs an INSERT/UPDATE ... RETURNING and wraps the stored row, or
    # turns a failure of a declared constraint into an Err.
    def write(changeset: Changeset, statement)
      begin
        rows = statement.to_a(@db, @visitor)
        if rows.empty?() then Ok.new(nil) else Ok.new(changeset.schema().build(rows[0])) end
      rescue error: StandardError
        failed = self.violated_constraint(changeset, error.message())
        if failed == nil
          raise error
        end
        Err.new(changeset.add_error(failed["field"], failed["message"]))
      end
    end

    def violated_constraint(changeset: Changeset, message: String)
      table = changeset.schema().table_name()
      changeset.constraints().find() do |declared|
        by_column = message.include?("UNIQUE constraint failed: #{table}.#{declared["field"]}")
        named = declared["constraint"]
        by_name = named != nil && message.include?(named)
        by_column || by_name
      end
    end
  end

end
