# One row of the `books` table; see Author for the shared structure.
class Book < ActiveRecord::Model
  attr_accessor title: String, author_id, year, available

  def initialize(attributes: Hash = {})
    super(attributes)
    @title = attributes["title"]
    @author_id = attributes["author_id"]
    @year = attributes["year"]
    @available = attributes["available"]
  end

  # Columns to write on save.
  def to_attributes()
    {"title": @title, "author_id": @author_id, "year": @year, "available": @available}
  end
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end


  # Availability is stored as 0/1 (SQLite has no boolean type).
  def available?() -> Bool = @available == 1

  # The owning author, or nil if that author was deleted (nothing cascades).
  def author(db) = self.belongs_to(Author.repository()).get(db, @author_id)
end

# Row-to-object builder handed to the Repository in middleware.di.
def build_book(row) = Book.new(row)
