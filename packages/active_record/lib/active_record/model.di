module ActiveRecord

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

  # has_secure_password-style helpers, deliberately not a macro -- there
  # is no `has_secure_password :password` conjuring a real method into
  # existence from a symbol, for the same reason `has_many`/`has_one`/
  # `belongs_to` above aren't macros either (see this file's own class
  # comment and README.md). A model wires this up the same explicit way:
  # an ordinary `attr_accessor password_digest` (deliberately untyped, not
  # `: String` -- a model that never called #secure_password= has a nil
  # digest, and a typed attr_accessor's generated getter enforces its
  # return type at runtime, raising on a nil read rather than just
  # returning nil; #authenticate below relies on getting nil back), read
  # in `#initialize`/written into `#to_attributes`, same as any other
  # column. Backed by BCrypt (native, see docs/syntax.md), bcrypt cost 12
  # (matching Rails' own BCrypt::Engine::DEFAULT_COST) -- not
  # configurable per call here; a model wanting a different cost calls
  # `BCrypt.hash(password, cost)` directly instead of this helper.
  #
  # Deliberately calls self.password_digest()/self.password_digest=(...)
  # rather than touching @password_digest directly -- the same virtual
  # self.foo() dispatch #save/#destroy/#id already rely on above to reach
  # whatever a subclass's own attr_accessor generated, not any new or
  # untested behavior around inherited @ivar auto-declaration.
  def secure_password=(password: String)
    self.password_digest=(BCrypt.hash(password, 12))
  end

  def authenticate(password: String) -> Bool
    digest = self.password_digest()
    digest != nil && BCrypt.verify(password, digest)
  end

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
