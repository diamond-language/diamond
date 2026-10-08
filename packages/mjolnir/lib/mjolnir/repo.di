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

    # Ok(entity) with the stored row, or Err(changeset) if the changeset
    # is invalid or a declared unique constraint failed.
    def insert(changeset: Changeset)
      unless changeset.valid?()
        return Err.new(changeset)
      end
      schema = changeset.schema()
      statement = Arel.insert_into(schema.table())
      if changeset.changes().empty?()
        statement = statement.default_values()
      else
        statement = statement.values(changeset.changes())
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
      statement = Arel.update(schema.table()).set(changeset.changes())
      statement = statement.where(schema.column(schema.primary_key()).eq(id)).returning(["*"])
      result = self.write(changeset, statement)
      if result is Ok && result.value() == nil
        raise StaleEntryError.new(schema.table_name(), id)
      end
      result
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
