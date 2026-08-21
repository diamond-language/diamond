require "../../arel/lib/arel"

module ActiveRecord

# Raised by Repository#create/#update when a configured
# validator reports at least one failure, before any SQL runs. `errors` is
# the Array of message Strings the validator returned; `message` joins
# them for the common case of just wanting one string to display or log.
class ValidationError < StandardError
  attr_reader message: String
  attr_reader errors: Array
  def initialize(errors: Array)
    @errors = errors
    @message = errors.join(", ")
  end
end

# Explicit persistence primitives over Arel. This package deliberately does
# not inspect schemas, infer columns, or dispatch through missing methods.
class Repository
  # `visitor` selects the Arel dialect this repository renders through --
  # nil (the default) means Arel's own default, Arel::SQLiteVisitor. Every
  # method below threads it through explicitly rather than leaving it
  # unstated: without this, a repository built over a PostgreSQL
  # connection would still render through Arel::SQLiteVisitor by default,
  # which happens to produce identical SQL for the simple queries this
  # repository builds today, but isn't guaranteed to stay that way (e.g.
  # SQLite's own offset-without-limit pagination sentinel, LIMIT -1, is
  # syntax PostgreSQL rejects outright -- see packages/arel/ROADMAP.md).
  #
  # `validator`, when given, is called with the caller's own attributes
  # Hash (never a mutated copy) and must return an Array of error message
  # Strings -- empty means valid. #create/#update run it before any SQL,
  # raising ValidationError on failure. There is no rule-object
  # DSL here; validator is an ordinary function, same as mapper.
  #
  # `before_save`/`after_save`, when given, are called around #create,
  # #update, and #delete alike, as `callback(db, attributes, on)` -- `on`
  # is a Symbol (`:create`/`:update`/`:destroy`) telling a single shared
  # hook which operation is running, rather than needing six separate
  # create/update/destroy-specific hook slots. For #delete, `attributes`
  # is a one-entry Hash ({id_column => id}), since a delete has no
  # attributes payload of its own -- just the row it targets.
  # before_save runs after validation and returns the attributes Hash to
  # actually write (letting it inject/transform values, e.g. a timestamp);
  # returning the same Hash unchanged is a no-op. Its return value is used
  # for #create/#update (what gets written) but ignored for #delete
  # (there is nothing to write). after_save runs once the operation
  # succeeds, with those same attributes, and its return value is always
  # ignored.
  def initialize(table: Arel::Table, mapper: Callable[1], id_column: String = "id", visitor = nil,
                 validator = nil, before_save = nil, after_save = nil)
    @table = table
    @mapper = mapper
    @id_column = id_column
    @visitor = visitor
    @validator = validator
    @before_save = before_save
    @after_save = after_save
  end

  def validate!(attributes: Hash)
    if @validator == nil
      return
    end
    errors = @validator(attributes)
    unless errors.empty?()
      raise ValidationError.new(errors)
    end
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
  # a repository caller actually needs (see HasMany#all,
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
    self.validate!(attributes)
    final_attributes = attributes
    unless @before_save == nil
      final_attributes = @before_save(db, attributes, :create)
    end
    result = Arel.insert_into(@table).values(final_attributes).execute(db, @visitor)
    unless @after_save == nil
      @after_save(db, final_attributes, :create)
    end
    result
  end

  def update(db, id, attributes: Hash)
    self.validate!(attributes)
    final_attributes = attributes
    unless @before_save == nil
      final_attributes = @before_save(db, attributes, :update)
    end
    statement = Arel.update(@table).set(final_attributes)
    result = statement.where(@table.column(@id_column).eq(id)).execute(db, @visitor)
    unless @after_save == nil
      @after_save(db, final_attributes, :update)
    end
    result
  end

  def delete(db, id)
    attributes = {}
    attributes[@id_column] = id
    unless @before_save == nil
      @before_save(db, attributes, :destroy)
    end
    statement = Arel.delete_from(@table)
    result = statement.where(@table.column(@id_column).eq(id)).execute(db, @visitor)
    unless @after_save == nil
      @after_save(db, attributes, :destroy)
    end
    result
  end
end

# Explicit change tracking for a plain attributes Hash -- there is no
# model base class here to instrument arbitrary setters on (mapper builds
# whatever class the caller wants, opaque to this package, the same
# reason validator/before_save/after_save are ordinary functions rather
# than a DSL). Wrap a loaded row (or any Hash of known attribute values),
# mutate it through #set, then ask #changed?/#changes before deciding to
# call Repository#update -- #changes returns exactly the Hash #update
# already expects, with no repository integration needed:
#
#   row = db.query("SELECT * FROM authors WHERE id = ?", [1])[0]
#   dirty = DirtyAttributes.new(row)
#   dirty.set("country", "England")
#   repository.update(db, row["id"], dirty.changes()) if dirty.changed?()
#
# Diamond doesn't support overloading `[]`/`[]=` on a user-defined class,
# hence #get/#set rather than bracket syntax. Comparison is `==`, so it's
# value equality for the ordinary Int/Float/String/Bool/Nil attribute
# values this is meant for, but identity equality if an attribute value
# is itself an Array/Hash -- the same distinction Diamond's own `==`
# already draws everywhere else.
class DirtyAttributes
  def initialize(original: Hash)
    @original = original
    @current = {}
    keys = original.keys()
    index = 0
    while index < keys.length()
      key = keys[index]
      @current[key] = original[key]
      index += 1
    end
  end

  def get(key) = @current[key]

  def set(key, value)
    @current[key] = value
  end

  def attribute_changed?(key) -> Bool = @original[key] != @current[key]

  def changed_keys() -> Array
    keys = @current.keys()
    result = []
    index = 0
    while index < keys.length()
      key = keys[index]
      if self.attribute_changed?(key)
        result.push(key)
      end
      index += 1
    end
    result
  end

  def changed?() -> Bool = self.changed_keys().length() > 0

  def changes() -> Hash
    result = {}
    changed = self.changed_keys()
    index = 0
    while index < changed.length()
      key = changed[index]
      result[key] = @current[key]
      index += 1
    end
    result
  end

  def to_h() -> Hash
    copy = {}
    keys = @current.keys()
    index = 0
    while index < keys.length()
      key = keys[index]
      copy[key] = @current[key]
      index += 1
    end
    copy
  end
end

class HasMany
  def initialize(repository: Repository, foreign_key: String,
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

class HasOne
  def initialize(repository: Repository, foreign_key: String,
                 owner_key: String = "id")
    @repository = repository
    @foreign_key = foreign_key
    @owner_key = owner_key
  end

  # Same shape as HasMany -- the owner value is passed explicitly, no
  # object introspection or naming convention resolves it -- but returns a
  # single record or nil, the same "not found" shape BelongsTo#get uses,
  # since the relationship is one-to-one on this side rather than
  # one-to-many.
  def get(db, owner_id)
    conditions = {}
    conditions[@foreign_key] = owner_id
    results = @repository.where(db, conditions)
    if results.length() == 0 then nil else results[0] end
  end
end

class BelongsTo
  def initialize(repository: Repository, owner_key: String = "id")
    @repository = repository
    @owner_key = owner_key
  end

  # The child's own foreign-key value is passed explicitly -- no object
  # introspection reads it off a child instance. Unlike
  # Repository#find (which always looks up @id_column), this
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
class Transaction
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

end
