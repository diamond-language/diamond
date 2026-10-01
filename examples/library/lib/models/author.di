# One row of the `authors` table. ActiveRecord::Model supplies the generic
# find/create/save/destroy machinery; this class says which columns exist
# and how to move them between a Hash (a database row, or form input) and
# instance variables.
class Author < ActiveRecord::Model
  attr_accessor name: String, country: String

  # Build from a row-shaped Hash. `super` makes the base class keep its own
  # copy of the whole Hash (including the id column); the columns are then
  # copied into instance variables for the accessors.
  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @country = attributes["country"]
  end

  # The inverse of `initialize`: the columns to write on save (never the id).
  def to_attributes() = {"name": @name, "country": @country}

  # The Repository (table + row builder) lives in a class variable, set by
  # `configure` once PER WORKER THREAD; see ensure_models_configured in
  # lib/middleware.di for why it cannot be set once at startup. The
  # instance-level `repository` is what the base class calls on a loaded row.
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  # An author's books: every `books` row whose author_id is this author's id.
  def books(db) = self.has_many(Book.repository(), "author_id").all(db, self.id())
end

# Row-to-object builder handed to the Repository in middleware.di.
def build_author(row) = Author.new(row)
