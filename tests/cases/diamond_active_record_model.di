require "../../packages/diamond-active_record/lib/diamond-active_record"
require "../../lib/minitest"

# Author and Book associate in both directions (has_many/belongs_to), so
# neither class body can name the other directly -- Diamond resolves a
# class name inside a method body at compile time, and the two classes
# can't both come first. Each association reader instead reads a
# class-variable slot filled in later, once both classes exist, via its
# own self.wire_* call -- the same one-time "configure after everything
# exists" shape Model.configure itself already requires.
class Author < ActiveRecord::Model
  attr_accessor name: String, country: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
    @country = attributes["country"]
  end

  def to_attributes() = {"name": @name, "country": @country}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def books(db) = self.has_many(@@books_repository, "author_id").all(db, self.id())
  def self.wire_books(repository: ActiveRecord::Repository)
    @@books_repository = repository
  end
end

def build_author(row) = Author.new(row)

class Book < ActiveRecord::Model
  attr_accessor title: String, author_id

  def initialize(attributes: Hash = {})
    super(attributes)
    @title = attributes["title"]
    @author_id = attributes["author_id"]
  end

  def to_attributes() = {"title": @title, "author_id": @author_id}
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def author(db) = self.belongs_to(@@author_repository).get(db, @author_id)
  def self.wire_author(repository: ActiveRecord::Repository)
    @@author_repository = repository
  end
end

def build_book(row) = Book.new(row)

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")

  Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))
  Book.configure(ActiveRecord::Repository.new(Arel.table("books"), build_book, "id"))
  Author.wire_books(Book.repository())
  Book.wire_author(Author.repository())

  # Class-level, Rails-shaped surface.
  Author.create(db, {"name": "Ada", "country": "UK"})
  Author.create(db, {"name": "Grace", "country": "USA"})
  Minitest.assert_equal(2, Author.all().to_a(db).length())

  ada = Author.find(db, 1)
  Minitest.assert_equal("Ada", ada.name())
  Minitest.assert_equal(true, ada.persisted?())

  uk_authors = Author.where({"country": "UK"}).to_a(db)
  Minitest.assert_equal(1, uk_authors.length())
  Minitest.assert_equal("Ada", uk_authors[0].name())

  # Instance-level mutate-then-save, Rails style.
  ada.name=("Ada Lovelace")
  ada.save(db)
  reloaded = Author.find(db, 1)
  Minitest.assert_equal("Ada Lovelace", reloaded.name())

  # A brand-new (unpersisted) instance saves via #create instead of
  # #update.
  marie = Author.new({"name": "Marie", "country": "France"})
  Minitest.assert_equal(false, marie.persisted?())
  marie.save(db)
  Minitest.assert_equal(3, Author.all().to_a(db).length())

  # Association readers, one line each on the model itself, no macro.
  Book.create(db, {"title": "Sketch of the Analytical Engine", "author_id": 1})
  Book.create(db, {"title": "Notes on the Analytical Engine", "author_id": 1})
  ada_books = ada.books(db)
  Minitest.assert_equal(2, ada_books.length())
  Minitest.assert_equal("Sketch of the Analytical Engine", ada_books[0].title())

  first_book = Book.find(db, 1)
  book_author = first_book.author(db)
  Minitest.assert_equal("Ada Lovelace", book_author.name())

  # #destroy.
  Minitest.assert_equal(3, Author.all().to_a(db).length())
  marie.destroy(db)
  Minitest.assert_equal(2, Author.all().to_a(db).length())

  db.close()
end

run_tests()
puts("diamond_active_record model smoke ok")
