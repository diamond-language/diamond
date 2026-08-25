class Book < ActiveRecord::Model
  attr_accessor title: String, author_id, year, available

  def initialize(attributes: Hash = {})
    super(attributes)
    @title = attributes["title"]
    @author_id = attributes["author_id"]
    @year = attributes["year"]
    @available = attributes["available"]
  end

  def to_attributes()
    {"title": @title, "author_id": @author_id, "year": @year, "available": @available}
  end
  def repository() = @@repository

  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def available?() -> Bool = @available == 1
  def author(db) = self.belongs_to(Author.repository()).get(db, @author_id)
end

def build_book(row) = Book.new(row)
