require "../../packages/graphsql/lib/graphsql"
require "../../lib/minitest"

class GraphLookahead
  def initialize(children: Hash = {})
    @children = children
  end
  def selections() = @children.keys()
  def selection(name)
    child = @children["#{name}"]
    if child == nil then GraphLookahead.new() else child end
  end
end

class GraphAuthor < ActiveRecord::Model
  attr_accessor name
  def initialize(attributes: Hash = {})
    super(attributes)
    @name = attributes["name"]
  end
  def to_attributes() = {"name": @name}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

class GraphBook < ActiveRecord::Model
  attr_accessor title, author_id
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

def map_graph_author(row) = GraphAuthor.new(row)
def map_graph_book(row) = GraphBook.new(row)

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE graph_authors (id INTEGER PRIMARY KEY, name TEXT, secret TEXT)")
  db.execute("CREATE TABLE graph_books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")
  GraphBook.configure(ActiveRecord::Repository.new(
    Arel.table("graph_books"), map_graph_book, "id", nil, nil, nil, nil, nil,
    ["id", "title", "author_id"]))
  GraphAuthor.configure(ActiveRecord::Repository.new(
    Arel.table("graph_authors"), map_graph_author, "id", nil, nil, nil, nil, nil,
    ["id", "name", "secret"], nil, [ActiveRecord::AssociationReflection.new(
      "books", "has_many", GraphBook.repository(), "author_id", "id")]))
  GraphBook.configure(ActiveRecord::Repository.new(
    Arel.table("graph_books"), map_graph_book, "id", nil, nil, nil, nil, nil,
    ["id", "title", "author_id"], nil, [ActiveRecord::AssociationReflection.new(
      "author", "belongs_to", GraphAuthor.repository(), "author_id", "id")]))

  GraphAuthor.create(db, {"name": "Ada", "secret": "hidden"})
  GraphAuthor.create(db, {"name": "Grace", "secret": "classified"})
  GraphBook.create(db, {"title": "Notes", "author_id": 1})
  GraphBook.create(db, {"title": "Compilers", "author_id": 2})

  author_mapping = GraphSQL::Mapping.new(GraphAuthor.repository(), "Author").column("name")
  book_mapping = GraphSQL::Mapping.new(GraphBook.repository(), "Book").column("title")
  book_mapping.association("author", "author", author_mapping)
  author_mapping.association("books", "books", book_mapping)

  def test_flat_projection_stays_lazy_and_plain_associations_include(db)
    flat_mapping = GraphSQL::Mapping.new(GraphAuthor.repository(), "FlatAuthor").column("name")
    flat_mapping.association("books")
    lookahead = GraphLookahead.new({"name": GraphLookahead.new(), "books": GraphLookahead.new()})
    resolved = GraphSQL.resolve(GraphAuthor.all(), db, lookahead, flat_mapping)
    Minitest.assert_equal(true, resolved is ActiveRecord::Relation)
    Minitest.assert_equal(2, resolved.query().projection_count())
    authors = resolved.to_a(db)
    Minitest.assert_equal(true, authors[0].association_loaded?("books"))
  end

  def test_nested_mappings_project_and_preload_recursively(db, author_mapping)
    author_fields = GraphLookahead.new({"name": GraphLookahead.new()})
    book_fields = GraphLookahead.new({
      "title": GraphLookahead.new(), "author": author_fields})
    lookahead = GraphLookahead.new({"name": GraphLookahead.new(), "books": book_fields})
    authors = GraphSQL::Resolver.new(
      GraphAuthor.all(), db, lookahead, author_mapping, [], []).resolve()
    Minitest.assert_equal(true, authors is Array)
    books = authors[0].preloaded_association("books")
    Minitest.assert_equal("Notes", books[0].title())
    Minitest.assert_equal(true, books[0].association_loaded?("author"))
    Minitest.assert_equal("Ada", books[0].preloaded_association("author").name())
  end

  def test_unknown_mapped_column_fails_before_sql(db)
    bad = GraphSQL::Mapping.new(GraphAuthor.repository(), "BadAuthor").column("typo")
    caught = false
    begin
      GraphSQL::Resolver.new(GraphAuthor.all(), db,
        GraphLookahead.new({"typo": GraphLookahead.new()}), bad, [], []).resolve()
    rescue error: GraphSQL::UnknownColumnError
      caught = true
      Minitest.assert_equal(true, error.message().include?("typo"))
    end
    Minitest.assert_equal(true, caught)
  end

  def test_required_belongs_to_keeps_foreign_key_and_preloads(db, book_mapping)
    resolved = GraphSQL::Resolver.new(GraphBook.all(), db,
      GraphLookahead.new({"title": GraphLookahead.new()}),
      book_mapping, ["author"], []).resolve()
    Minitest.assert_equal(3, resolved.query().projection_count())
    books = resolved.to_a(db)
    Minitest.assert_equal(true, books[0].association_loaded?("author"))
    Minitest.assert_equal("Ada", books[0].preloaded_association("author").name())
  end

  def test_duplicate_aliases_fail_loudly(db, author_mapping, book_mapping)
    aliased = GraphSQL::Mapping.new(GraphAuthor.repository(), "AliasedAuthor").column("name")
    aliased.association("books", "books", book_mapping)
    aliased.association("books", "publications", book_mapping)
    caught = false
    begin
      GraphSQL::Resolver.new(GraphAuthor.all(), db, GraphLookahead.new({
        "books": GraphLookahead.new({"title": GraphLookahead.new()}),
        "publications": GraphLookahead.new({"title": GraphLookahead.new()})
      }), aliased, [], []).resolve()
    rescue error: GraphSQL::AliasedAssociationError
      caught = true
    end
    Minitest.assert_equal(true, caught)
  end

  suite = Minitest.new()
  suite.test("flat projection remains lazy and plain associations include") do
    test_flat_projection_stays_lazy_and_plain_associations_include(db)
  end
  suite.test("nested mappings project and preload recursively") do
    test_nested_mappings_project_and_preload_recursively(db, author_mapping)
  end
  suite.test("unknown mapped columns fail before SQL") do
    test_unknown_mapped_column_fails_before_sql(db)
  end
  suite.test("required belongs_to keeps its foreign key and preloads") do
    test_required_belongs_to_keeps_foreign_key_and_preloads(db, book_mapping)
  end
  suite.test("duplicate association aliases fail loudly") do
    test_duplicate_aliases_fail_loudly(db, author_mapping, book_mapping)
  end
  suite.run!()
  db.close()
end

run_tests()
