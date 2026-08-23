require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

# The actual motivating case for ClassName.compile_method: a has_many
# :books-style association reader, synthesized at self.configure time
# rather than hand-written per model. Still not a macro in the Ruby
# sense (there's no has_many :books class-body declaration -- this is
# ordinary imperative code inside self.configure), and body_source can't
# name Book directly (see docs/design.md's "Runtime method synthesis"
# section) -- Book.repository() is evaluated here, in the *caller's* own
# chunk where "Book" actually resolves, and threaded in via
# compile_method's bound_values rather than referenced by name from
# inside the synthesized body.
class Author < ActiveRecord::Model
  attr_accessor name: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
  end

  def to_attributes() = {"name": @name}
  def repository() = @@repository
  def self.repository() = @@repository

  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
    callable = Author.compile_method("books", ["db"],
      "self.has_many(target_repo, \"author_id\").all(db, self.id())",
      {"target_repo": Book.repository()})
    Author.define_method("books", callable)
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
end

def build_book(row) = Book.new(row)

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")

  # Book must be configured first: Author.configure calls Book.repository()
  # directly (an ordinary, normal method call -- Book is a real class in
  # this same compile, resolved the usual way).
  Book.configure(ActiveRecord::Repository.new(Arel.table("books"), build_book, "id"))
  Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))

  Author.create(db, {"name": "Ada"})
  Book.create(db, {"title": "Sketch of the Analytical Engine", "author_id": 1})
  Book.create(db, {"title": "Notes on the Analytical Engine", "author_id": 1})

  ada = Author.find(db, 1)
  ada_books = ada.books(db)
  Minitest.assert_equal(2, ada_books.length())
  Minitest.assert_equal("Sketch of the Analytical Engine", ada_books[0].title())

  # A second author, created after the method was already installed --
  # target_repo (bound once, at compile_method time) still resolves
  # correctly for every instance, not just the one alive at install time.
  Author.create(db, {"name": "Grace"})
  grace = Author.find(db, 2)
  Minitest.assert_equal(0, grace.books(db).length())

  db.close()
end

run_tests()
puts("active_record dynamic has_many smoke ok")
