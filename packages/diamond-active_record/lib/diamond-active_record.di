require "../../arel/lib/arel"

# Explicit persistence primitives over Arel. This package deliberately does
# not inspect schemas, infer columns, or dispatch through missing methods.
class ActiveRecordRepository
  # `visitor` selects the Arel dialect this repository renders through --
  # nil (the default) means Arel's own default, ArelSQLiteVisitor. Every
  # method below threads it through explicitly rather than leaving it
  # unstated: without this, a repository built over a PostgreSQL
  # connection would still render through ArelSQLiteVisitor by default,
  # which happens to produce identical SQL for the simple queries this
  # repository builds today, but isn't guaranteed to stay that way (e.g.
  # SQLite's own offset-without-limit pagination sentinel, LIMIT -1, is
  # syntax PostgreSQL rejects outright -- see packages/arel/ROADMAP.md).
  def initialize(table: ArelTable, mapper: Callable[1], id_column: String = "id", visitor = nil)
    @table = table
    @mapper = mapper
    @id_column = id_column
    @visitor = visitor
  end

  def all(db)
    rows = Arel.from(@table).to_a(db, @visitor)
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(@mapper(rows[index]))
      index += 1
    end
    mapped
  end

  def find(db, id)
    rows = Arel.from(@table).where(@table.column(@id_column).eq(id)).take(1).to_a(db, @visitor)
    if rows.length() == 0 then nil else @mapper(rows[0]) end
  end

  # `conditions` is a Hash of column name to value, ANDed together via
  # `eq` -- still no query-builder DSL exposed here, just the one shape
  # a repository caller actually needs (see ActiveRecordHasMany#all,
  # which now builds one of these instead of taking a bare column/value
  # pair).
  def where(db, conditions: Hash)
    keys = conditions.keys()
    predicate = nil
    index = 0
    while index < keys.length()
      key = keys[index]
      column_predicate = @table.column(key).eq(conditions[key])
      predicate = if predicate == nil then column_predicate else predicate.and_also(column_predicate) end
      index += 1
    end
    query = Arel.from(@table)
    rows = if predicate == nil then query.to_a(db, @visitor) else query.where(predicate).to_a(db, @visitor) end
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(@mapper(rows[index]))
      index += 1
    end
    mapped
  end

  def create(db, attributes: Hash)
    Arel.insert_into(@table).values(attributes).execute(db, @visitor)
  end

  def update(db, id, attributes: Hash)
    statement = Arel.update(@table).set(attributes)
    statement.where(@table.column(@id_column).eq(id)).execute(db, @visitor)
  end

  def delete(db, id)
    statement = Arel.delete_from(@table)
    statement.where(@table.column(@id_column).eq(id)).execute(db, @visitor)
  end
end

class ActiveRecordHasMany
  def initialize(repository: ActiveRecordRepository, foreign_key: String,
                 owner_key: String = "id")
    @repository = repository
    @foreign_key = foreign_key
    @owner_key = owner_key
  end

  # The owner value is passed explicitly. No object introspection or naming
  # convention is involved in resolving the association.
  def all(db, owner_id)
    conditions = {}
    conditions[@foreign_key] = owner_id
    @repository.where(db, conditions)
  end
end

class ActiveRecordBelongsTo
  def initialize(repository: ActiveRecordRepository, owner_key: String = "id")
    @repository = repository
    @owner_key = owner_key
  end

  # The child's own foreign-key value is passed explicitly -- no object
  # introspection reads it off a child instance. Unlike
  # ActiveRecordRepository#find (which always looks up @id_column), this
  # goes through #where so an @owner_key other than the owner repository's
  # primary key still works; nil (not an empty Array) when nothing
  # matches, same "not found" shape #find already uses.
  def get(db, foreign_key_value)
    conditions = {}
    conditions[@owner_key] = foreign_key_value
    results = @repository.where(db, conditions)
    if results.length() == 0 then nil else results[0] end
  end
end

# Neither Arel nor the database drivers expose a transaction API
# themselves (BEGIN/COMMIT/ROLLBACK are ordinary SQL statements a caller
# runs through the same #execute(sql) every write in this package already
# uses -- see docs/io.md). This is that one missing piece: run `callback`,
# commit on a normal return, roll back and re-raise on any exception.
class ActiveRecordTransaction
  def self.run(db, callback: Callable[0])
    db.execute("BEGIN")
    begin
      result = callback()
      db.execute("COMMIT")
      result
    rescue error: StandardError
      db.execute("ROLLBACK")
      raise error
    end
  end
end
