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

# Raised by Repository#update when optimistic locking is configured
# (see Repository's lock_column) and the row's lock column no longer
# matches the caller's expected_lock_version -- someone else updated (or
# deleted) this row first. `id` is the row this update targeted.
class StaleObjectError < StandardError
  attr_reader message: String
  attr_reader id
  def initialize(id)
    @id = id
    @message = "attempted to update a stale object (id=#{id})"
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

  def initialize(table: Arel::Table, mapper: Callable[1], id_column: String = "id", visitor = nil,
                 validator = nil, before_save = nil, after_save = nil, lock_column = nil)
    @table = table
    @mapper = mapper
    @id_column = id_column
    @visitor = visitor
    @validator = validator
    @before_save = before_save
    @after_save = after_save
    @lock_column = lock_column
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

  # A lazy, chainable starting point over this repository's own table --
  # see Relation below. Unlike #all/#where above (which hit the database
  # immediately), nothing here runs until a terminal call
  # (#to_a/#first/#count) on the Relation it returns.
  def relation() = Relation.new(Arel.from(@table), @mapper, @visitor)

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
    self.validate!(attributes)
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

# A thin, lazy wrapper over an Arel::Query -- built by Repository#relation
# above. Not constructed by hand against an arbitrary query/mapper/visitor
# the way HasMany/HasOne are meant to be, though nothing stops that. Every
# chain method here (#where/#order/#take/#skip) just forwards to the same
# method Arel::Query already has (packages/arel/README.md's own
# "immutable, chainable" query builder) and wraps the new Query it
# returns in a new Relation -- no query-building logic lives here at all.
# Nothing hits the database until #to_a/#first/#count, at which point
# rows are mapped through the same @mapper Repository#all/#where already
# use.
class Relation
  def initialize(query, mapper: Callable[1], visitor = nil)
    @query = query
    @mapper = mapper
    @visitor = visitor
  end

  # Query#where already accepts a plain Hash and ANDs its keys together
  # via `eq` (see packages/arel/README.md's "Compatibility where"), so
  # there is no predicate-building of this Relation's own -- unlike
  # Repository#where above, which still hand-rolls that loop for its own
  # eager, non-Relation callers.
  def where(conditions) = Relation.new(@query.where(conditions), @mapper, @visitor)
  def order(column_or_columns) = Relation.new(@query.order(column_or_columns), @mapper, @visitor)
  def take(n: Int) = Relation.new(@query.take(n), @mapper, @visitor)
  def limit(n: Int) = self.take(n)
  def skip(n: Int) = Relation.new(@query.skip(n), @mapper, @visitor)
  def offset(n: Int) = self.skip(n)

  def to_a(db)
    rows = @query.to_a(db, @visitor)
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(@mapper(rows[index]))
      index += 1
    end
    mapped
  end

  # Deliberately does not add an implicit `ORDER BY` (e.g. by primary
  # key) the way real ActiveRecord's #first does -- this package has no
  # schema inspection or naming convention to build one from (see
  # README.md), so #first without a preceding #order returns whatever row
  # the database happens to return first. Chain #order(...) first for a
  # deterministic result.
  def first(db)
    rows = self.take(1).to_a(db)
    if rows.length() == 0 then nil else rows[0] end
  end

  def count(db) = @query.count(db, @visitor)
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

  # Batch form of #all -- one query for every owner_id instead of one
  # query per owner, avoiding the N+1 pattern a naive loop over #all
  # would produce. Returns a Hash of owner_id -> Array of mapped records;
  # every owner_id passed in gets a key (an empty Array if it has none),
  # so a caller never needs its own fallback for a missing key.
  def preload(db, owner_ids: Array)
    table = @repository.table()
    rows = Arel.from(table).where(
      table.column(@foreign_key).in_list(owner_ids)).to_a(db, @repository.visitor())
    mapper = @repository.mapper()
    grouped = {}
    index = 0
    while index < owner_ids.length()
      grouped[owner_ids[index]] = []
      index += 1
    end
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      grouped[row[@foreign_key]].push(mapper(row))
      row_index += 1
    end
    grouped
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

  # Batch form of #get -- one query for every owner_id. Returns a Hash of
  # owner_id -> mapped record or nil, the same "not found" shape #get
  # uses, for every owner_id passed in. If more than one row matches a
  # given owner_id (a data-integrity assumption this association doesn't
  # enforce), the last one wins -- same as #get looking at only the
  # first result, just the opposite end of an unordered result set.
  def preload(db, owner_ids: Array)
    table = @repository.table()
    rows = Arel.from(table).where(
      table.column(@foreign_key).in_list(owner_ids)).to_a(db, @repository.visitor())
    mapper = @repository.mapper()
    grouped = {}
    index = 0
    while index < owner_ids.length()
      grouped[owner_ids[index]] = nil
      index += 1
    end
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      grouped[row[@foreign_key]] = mapper(row)
      row_index += 1
    end
    grouped
  end
end

# Many-to-many via an explicit join table -- no naming convention
# resolves it (the join table, its two foreign-key column names, and the
# target repository are all supplied directly, the same explicit shape
# HasMany/HasOne/BelongsTo already use). Unlike those three, this can't
# just call the target repository's own #where (which only ever filters
# on the target's own table), since resolving the association means
# joining the join table to the target table and filtering on the join
# table's owner-key column -- so this builds that Arel query itself,
# using the target repository's own table/mapper/visitor (see its three
# read-only accessors above) to render and map results consistently with
# how the rest of that repository already works.
class HasManyThrough
  def initialize(target_repository: Repository, join_table: Arel::Table,
                 join_owner_key: String, join_target_key: String)
    @target_repository = target_repository
    @join_table = join_table
    @join_owner_key = join_owner_key
    @join_target_key = join_target_key
  end

  def all(db, owner_id)
    target_table = @target_repository.table()
    join_predicate = @join_table.column(@join_target_key).eq(
      target_table.column(@target_repository.id_column()))
    query = Arel.from(@join_table).join(target_table, join_predicate)
    query = query.where(@join_table.column(@join_owner_key).eq(owner_id))
    query = query.project([target_table.star()])
    rows = query.to_a(db, @target_repository.visitor())
    mapper = @target_repository.mapper()
    mapped = []
    index = 0
    while index < rows.length()
      mapped.push(mapper(rows[index]))
      index += 1
    end
    mapped
  end

  # Batch form of #all -- one join query for every owner_id instead of
  # one join query per owner. Returns a Hash of owner_id -> Array of
  # mapped target records, an empty Array for an owner_id with none.
  # Projects the join table's own owner-key column alongside the target
  # table's star purely to know which owner each returned row belongs to
  # when grouping -- a narrow, deliberate addition to the star projection
  # #all already uses, safe as long as the join table's owner-key column
  # name doesn't collide with one of the target table's own column names.
  def preload(db, owner_ids: Array)
    target_table = @target_repository.table()
    join_predicate = @join_table.column(@join_target_key).eq(
      target_table.column(@target_repository.id_column()))
    query = Arel.from(@join_table).join(target_table, join_predicate)
    query = query.where(@join_table.column(@join_owner_key).in_list(owner_ids))
    query = query.project([@join_table.column(@join_owner_key), target_table.star()])
    rows = query.to_a(db, @target_repository.visitor())
    mapper = @target_repository.mapper()
    grouped = {}
    index = 0
    while index < owner_ids.length()
      grouped[owner_ids[index]] = []
      index += 1
    end
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      grouped[row[@join_owner_key]].push(mapper(row))
      row_index += 1
    end
    grouped
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

  # Batch form of #get -- one query for every foreign_key_value instead
  # of one per child. Returns a Hash of foreign_key_value -> mapped owner
  # record or nil, the same "not found" shape #get uses, for every value
  # passed in.
  def preload(db, foreign_key_values: Array)
    table = @repository.table()
    rows = Arel.from(table).where(
      table.column(@owner_key).in_list(foreign_key_values)).to_a(db, @repository.visitor())
    mapper = @repository.mapper()
    grouped = {}
    index = 0
    while index < foreign_key_values.length()
      grouped[foreign_key_values[index]] = nil
      index += 1
    end
    row_index = 0
    while row_index < rows.length()
      row = rows[row_index]
      grouped[row[@owner_key]] = mapper(row)
      row_index += 1
    end
    grouped
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

  # For use *inside* an already-running #run (or any transaction the
  # caller already opened itself) -- databases don't support nesting a
  # real BEGIN/COMMIT, so this runs `callback` inside a named SAVEPOINT
  # instead, releasing it on a normal return or rolling back to it (not
  # the whole outer transaction) on any exception. Verified directly
  # that SAVEPOINT/RELEASE SAVEPOINT/ROLLBACK TO SAVEPOINT are identical
  # syntax and semantics across all three supported dialects (SQLite,
  # PostgreSQL, MariaDB), so this needs no visitor/dialect parameter the
  # way Repository does.
  #
  # Deliberately a separate, explicitly-called method rather than #run
  # auto-detecting nesting -- there is no ambient "am I already inside a
  # transaction on this connection" state exposed to Diamond code to
  # detect that with, and guessing from some other signal would be
  # exactly the kind of implicit magic this package avoids everywhere
  # else. The caller already knows whether it's nested; say so.
  #
  #   ActiveRecord::Transaction.run(db) do
  #     repository.create(db, {"name": "Ada"})
  #     ActiveRecord::Transaction.run_nested(db, "before_grace") do
  #       repository.create(db, {"name": "Grace"})
  #       raise RuntimeError.new("oops")
  #     end
  #   end
  #   # => "Ada" is committed, "Grace" is not -- the outer transaction
  #   # itself is untouched by the inner rollback.
  def self.run_nested(db, savepoint_name: String, callback: Callable[0])
    self.validate_savepoint_name(savepoint_name)
    db.execute("SAVEPOINT #{savepoint_name}")
    begin
      result = callback()
      db.execute("RELEASE SAVEPOINT #{savepoint_name}")
      result
    rescue error: StandardError
      db.execute("ROLLBACK TO SAVEPOINT #{savepoint_name}")
      raise error
    end
  end

  # SAVEPOINT/RELEASE/ROLLBACK TO take a bare identifier, not a bind
  # parameter -- there is no `?` placeholder form for a savepoint name in
  # any of the three dialects this checked directly, the same reason a
  # table or column name can't be bound either. Restricting it to
  # ASCII letters/digits/underscore, not starting with a digit, before
  # ever interpolating it into SQL text closes off that injection
  # surface entirely, the same spirit as Arel's own identifier quoting
  # (though a plain reject-anything-else check here, not quote-and-escape,
  # since a savepoint name has no legitimate reason to contain anything
  # else in the first place).
  def self.validate_savepoint_name(name: String)
    if name.length() == 0
      raise ArgumentError.new("savepoint name cannot be empty")
    end
    # Diamond's `>=`/`<=` aren't defined for String -- compared by
    # ordinal codepoint (#ord) instead, not by the character itself.
    characters = name.chars()
    first_code = characters[0].ord()
    if first_code >= 48 && first_code <= 57
      raise ArgumentError.new("savepoint name cannot start with a digit: #{name}")
    end
    index = 0
    while index < characters.length()
      code = characters[index].ord()
      valid = (code >= 97 && code <= 122) || (code >= 65 && code <= 90) ||
        (code >= 48 && code <= 57) || code == 95
      unless valid
        raise ArgumentError.new(
          "savepoint name must contain only letters, digits, and underscores: #{name}")
      end
      index += 1
    end
  end
end

# An optional, deliberately thin Rails-ActiveRecord-flavored layer over
# everything above -- not a replacement for Repository/HasMany/HasOne/
# BelongsTo/HasManyThrough (Model is built entirely out of them), just a
# more familiar surface for anyone used to that shape. Two real Diamond
# constraints shaped it, verified directly rather than assumed, and worth
# understanding before extending it:
#
# - Diamond classes have fixed, compile-time method tables -- there is no
#   `define_method`/`method_missing`, and the one runtime mechanism that
#   exists (`ClassName.redefine_method`) can only repoint an *existing*
#   method slot, never add a new one (see docs/design.md). So there is no
#   `has_many :books`-style macro that conjures a real `books` method out
#   of thin air -- every method a model exposes, including association
#   readers, is written as an ordinary `def` in that model, same as any
#   other Diamond class.
# - Instance methods dispatch virtually (`self.foo()` called from a
#   shared method correctly reaches a subclass's override -- confirmed
#   directly), but `def self.x` class methods do not: `self` isn't even
#   accessible inside one, and a bare call from inside one resolves to
#   whichever same-named method is lexically visible at compile time, not
#   the receiver's actual runtime class. Every instance method below
#   (#save, #destroy, #persisted?, #id) is written so a subclass's own
#   overrides of #repository/#to_attributes are what actually run,
#   because that dispatch is real. The class-level surface
#   (`Author.find`/`.all`/`.where`/`.create`) can't be inherited the same
#   way -- each model writes its own short one-line forwarders (see the
#   worked example in README.md), the one real per-model boilerplate
#   this layer couldn't eliminate.
class Model
  def initialize(attributes: Hash = {})
    # Copied rather than aliased, so #save's create path (which sets
    # id_column once the id is known) never mutates a Hash the caller
    # still holds its own reference to.
    copy = {}
    keys = attributes.keys()
    index = 0
    while index < keys.length()
      copy[keys[index]] = attributes[keys[index]]
      index += 1
    end
    @attributes = copy
  end

  # Every subclass must override both of these as instance methods (not
  # `self.` methods -- see the class comment above for why that matters).
  # #repository returns this model's own configured Repository (built
  # once via `.configure`, see README.md); #to_attributes is the reverse
  # of a Repository's own mapper function, returning this instance's
  # current field values as the same plain Hash shape
  # Repository#create/#update already write.
  def repository()
    raise RuntimeError.new("Model subclass must override #repository")
  end
  def to_attributes()
    raise RuntimeError.new("Model subclass must override #to_attributes")
  end

  def id() = @attributes[self.repository().id_column()]
  def persisted?() -> Bool = @attributes.include_key?(self.repository().id_column())

  # #as_json is the same plain Hash #to_attributes already builds -- the
  # override point for a subclass that wants a different JSON shape than
  # its raw attributes (dropping a column, renaming a key, embedding an
  # association) without touching #to_json itself. #to_json is just
  # JSON.stringify(#as_json()) -- the JSON module already in the prelude,
  # no new plumbing.
  def as_json() = self.to_attributes()
  def to_json() -> String = JSON.stringify(self.as_json())

  # Small, non-magic conveniences for writing a one-line association
  # reader on a subclass (see README.md) -- these just construct the
  # association object; #all/#get/#preload on it work exactly as
  # documented above.
  def has_many(repository: Repository, foreign_key: String) = HasMany.new(repository, foreign_key)
  def has_one(repository: Repository, foreign_key: String) = HasOne.new(repository, foreign_key)
  def belongs_to(repository: Repository) = BelongsTo.new(repository)

  # Threads optimistic locking through automatically when this model's
  # repository has a lock_column configured -- the current value already
  # loaded into @attributes is what #update expects as
  # expected_lock_version, so there is nothing further for a caller to
  # pass. Raises StaleObjectError exactly as Repository#update itself
  # does, on the same condition.
  def save(db)
    if self.persisted?()
      lock_column = self.repository().lock_column()
      if lock_column == nil
        self.repository().update(db, self.id(), self.to_attributes())
      else
        self.repository().update(
          db, self.id(), self.to_attributes(), @attributes[lock_column])
      end
    else
      self.repository().create(db, self.to_attributes())
      # Without this, @attributes never gains an id_column key, so
      # #persisted?/#id (and therefore a later #save or #destroy) would
      # keep treating this instance as brand new forever after its very
      # first, successful #save.
      @attributes[self.repository().id_column()] = db.last_insert_row_id()
    end
  end

  def destroy(db) = self.repository().delete(db, self.id())

  # `!`-suffixed aliases for #save/#destroy, provided purely for
  # Rails-naming familiarity -- unlike real ActiveRecord, where plain
  # #save/#destroy swallow a validation failure and return false while
  # #save!/#destroy! raise, this package's #save/#destroy already always
  # raise on failure (ValidationError, StaleObjectError -- see Repository
  # above), so there is no quiet failure mode to distinguish from. Both
  # spellings behave identically; use whichever reads better at the call
  # site.
  def save!(db) = self.save(db)
  def destroy!(db) = self.destroy(db)

  # Class-level finders, shared here and inherited by every subclass --
  # made possible by Diamond's virtual self.foo(...) dispatch inside a
  # class-owned singleton method (self, here, is whichever subclass the
  # original call actually named, not Model, even though these four
  # methods are only ever compiled once). Each subclass still has to
  # write its own self.repository() (and a self.configure(repository) to
  # set it) rather than inheriting one -- @@repository is a class
  # variable, and Diamond scopes @@cvar storage to whichever class the
  # *code that reads/writes it* is defined in, not the receiver a call
  # was made through, so an inherited self.repository() reading
  # Model's own @@repository would give every subclass the same shared
  # slot instead of its own. See README.md for the full worked example.
  #
  # self.all/self.where are lazy: they return a Relation (see above)
  # rather than rows, and take no `db` -- nothing hits the database until
  # a terminal call (#to_a(db)/#first(db)/#count(db)) on the Relation
  # they hand back, so `.order(...)`/`.limit(...)`/further `.where(...)`
  # can be chained on first, the same way real ActiveRecord's do:
  # `Author.where({"country": "UK"}).order(...).limit(10).to_a(db)`.
  def self.repository()
    raise RuntimeError.new("Model subclass must override self.repository")
  end
  def self.find(db, id) = self.repository().find(db, id)
  def self.all() = self.repository().relation()
  def self.where(conditions: Hash) = self.repository().relation().where(conditions)
  # `where(...).first`, one line -- a single matching instance, or nil,
  # with the same "no implicit ORDER BY" caveat #first(db) already has.
  def self.find_by(db, conditions: Hash) = self.repository().relation().where(conditions).first(db)
  def self.create(db, attributes: Hash) = self.repository().create(db, attributes)
  # See #save!/#destroy! above on why this behaves identically to
  # self.create -- self.create already raises on a validation failure.
  def self.create!(db, attributes: Hash) = self.create(db, attributes)
  def self.find_each(db, callback: Callable[1], batch_size = 1000)
    self.repository().find_each(db, callback, batch_size)
  end
  def self.find_in_batches(db, callback: Callable[1], batch_size = 1000)
    self.repository().find_in_batches(db, callback, batch_size)
  end
end

end
