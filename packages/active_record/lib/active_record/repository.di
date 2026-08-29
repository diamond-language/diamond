require "../../../arel/lib/arel"

module ActiveRecord

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
  # `lock_column`, when given, turns on optimistic locking for #update:
  # the caller passes the version value it loaded as expected_lock_version,
  # #update matches it in the WHERE clause alongside id_column and bumps
  # it by one in the same statement, and a StaleObjectError is raised
  # (rather than silently updating 0 rows, or a different row's version)
  # when nothing matched -- someone else updated (or deleted) this row
  # first. No naming convention (there's no assumed "lock_version" column
  # name) and no automatic version-loading -- the caller already has the
  # row it loaded, expected_lock_version is just that row's own current
  # lock_column value, passed explicitly like everything else in this
  # package.
  #
  # Read-only access to this repository's own configuration -- not object
  # introspection (that principle is about not reflecting on an opaque
  # mapped domain object by naming convention; these are the repository's
  # own explicitly-supplied fields). HasManyThrough uses these to build
  # its own join query against the target repository's table and map
  # results through its mapper, since Repository's own #all/#find/#where
  # only ever filter on @table itself, not a second joined table.
  attr_reader table: Arel::Table
  attr_reader mapper: Callable[1]
  attr_reader visitor
  attr_reader id_column: String
  attr_reader lock_column
  attr_reader column_names: Array
  attr_reader inheritance_column
  attr_reader association_reflections: Array

  def initialize(table: Arel::Table, mapper: Callable[1], id_column: String = "id", visitor = nil,
                 validator = nil, before_save = nil, after_save = nil, lock_column = nil,
                 column_names: Array = [], inheritance_column = nil,
                 association_reflections: Array = [])
    @table = table
    @mapper = mapper
    @id_column = id_column
    @visitor = visitor
    @validator = validator
    @before_save = before_save
    @after_save = after_save
    @lock_column = lock_column
    @column_names = column_names
    @inheritance_column = inheritance_column
    @association_reflections = association_reflections
  end

  def primary_key() = @id_column
  def has_column?(name) -> Bool = @column_names.include?("#{name}")
  def reflect_on_association(name)
    string_name = "#{name}"
    index = 0
    while index < @association_reflections.length()
      if @association_reflections[index].name() == string_name
        return @association_reflections[index]
      end
      index += 1
    end
    nil
  end

  # `exclude_id` is nil on #create, and the record's own id on #update
  # -- threaded through to the validator (see validators.di's own
  # comment) so a uniqueness check can exclude the record's own
  # current row instead of always flagging it as conflicting with
  # itself the moment any field on it is saved again unchanged.
  def validate!(attributes: Hash, exclude_id = nil)
    if @validator == nil
      return
    end
    errors = @validator(attributes, exclude_id)
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

  # A lazy, chainable starting point over this repository's own table --
  # see Relation below. Unlike #all/#where above (which hit the database
  # immediately), nothing here runs until a terminal call
  # (#to_a/#first/#count) on the Relation it returns.
  def relation() = Relation.new(Arel.from(@table), @mapper, @visitor, self)

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

  def update(db, id, attributes: Hash, expected_lock_version = nil)
    self.validate!(attributes, id)
    final_attributes = attributes
    unless @before_save == nil
      final_attributes = @before_save(db, attributes, :update)
    end
    predicate = @table.column(@id_column).eq(id)
    unless @lock_column == nil
      if expected_lock_version == nil
        raise ArgumentError.new(
          "expected_lock_version is required when lock_column is configured")
      end
      bump = {}
      bump[@lock_column] = expected_lock_version + 1
      final_attributes = final_attributes.merge(bump)
      predicate = predicate.and_also(@table.column(@lock_column).eq(expected_lock_version))
    end
    statement = Arel.update(@table).set(final_attributes)
    result = statement.where(predicate).execute(db, @visitor)
    if @lock_column != nil && result == 0
      raise StaleObjectError.new(id)
    end
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

  # Batch iteration over the *entire* table, one batch of at most
  # batch_size mapped records per callback call -- unlike #all, which
  # loads everything into memory in one query. Pages by id_column
  # (`WHERE id_column > last_seen_id ORDER BY id_column ASC LIMIT
  # batch_size`) rather than SQL OFFSET, the same keyset-pagination
  # strategy Rails' own find_in_batches uses: an OFFSET-based page N
  # re-scans and discards the first (N-1)*batch_size rows on the
  # server for every page, and silently skips or repeats rows if the
  # table is written to while iterating (a row deleted from an
  # earlier page shifts every later page's OFFSET by one); this
  # doesn't have either problem, at the cost of requiring id_column to
  # be usable in an ORDER BY/> comparison (an ordinary integer primary
  # key always is).
  def find_in_batches(db, callback: Callable[1], batch_size = 1000)
    if batch_size < 1
      raise ArgumentError.new("batch_size must be at least 1")
    end
    id_column = @table.column(@id_column)
    last_id = nil
    while true
      query = Arel.from(@table).order(id_column.asc()).take(batch_size)
      unless last_id == nil
        query = query.where(id_column.gt(last_id))
      end
      rows = query.to_a(db, @visitor)
      if rows.empty?()
        break
      end
      mapped = []
      index = 0
      while index < rows.length()
        mapped.push(@mapper(rows[index]))
        index += 1
      end
      callback(mapped)
      last_id = rows[rows.length() - 1][@id_column]
      if rows.length() < batch_size
        break
      end
    end
  end

  # #find_in_batches, but callback receives one mapped record at a time
  # instead of a whole batch -- the same batch-size/query-shape
  # tradeoff, just a per-record calling convention. Its own callback is
  # a nested closure capturing find_each's own callback argument
  # (Diamond has no anonymous closure literal, so this is the ordinary
  # named-nested-function shape everywhere else in this package already
  # uses to pass a Callable).
  def find_each(db, callback: Callable[1], batch_size = 1000)
    def call_each_in_batch(batch)
      index = 0
      while index < batch.length()
        callback(batch[index])
        index += 1
      end
    end
    self.find_in_batches(db, call_each_in_batch, batch_size)
  end
end

end
