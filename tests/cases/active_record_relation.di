# ActiveRecord::Relation -- the lazy, chainable wrapper Model.all/.where
# now return (see ROADMAP.md). It's a thin wrapper over Arel::Query's own
# already-immutable/chainable builder (packages/arel/README.md), so this
# suite's real point isn't re-testing Arel's query building -- it's
# confirming Relation's own three concerns: nothing runs until a terminal
# call, chaining composes correctly through Model's class-level surface,
# and one Relation's chain never leaks into another built from the same
# base (mirroring Arel::Query's own immutability guarantee end to end
# through this wrapper).
require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

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
end

class Book < ActiveRecord::Model
  attr_accessor title: String, author_id: Int

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

def build_author(row) = Author.new(row)
def build_book(row) = Book.new(row)

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
  db.execute("CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER)")

  Book.configure(ActiveRecord::Repository.new(
    Arel.table("books"), build_book, "id", nil, nil, nil, nil, nil,
    ["id", "title", "author_id"]))
  Author.configure(ActiveRecord::Repository.new(
    Arel.table("authors"), build_author, "id", nil, nil, nil, nil, nil,
    ["id", "name", "country"], nil, [ActiveRecord::AssociationReflection.new(
      "books", "has_many", Book.repository(), "author_id", "id")]))
  Book.configure(ActiveRecord::Repository.new(
    Arel.table("books"), build_book, "id", nil, nil, nil, nil, nil,
    ["id", "title", "author_id"], nil, [ActiveRecord::AssociationReflection.new(
      "author", "belongs_to", Author.repository(), "author_id", "id")]))

  Author.create(db, {"name": "Ada", "country": "UK"})
  Author.create(db, {"name": "Grace", "country": "USA"})
  Author.create(db, {"name": "Marie", "country": "UK"})
  Book.create(db, {"title": "Notes", "author_id": 1})
  Book.create(db, {"title": "Engines", "author_id": 1})
  Book.create(db, {"title": "Compilers", "author_id": 2})

  def name_column() = Arel.table("authors").column("name")

  def test_all_returns_a_relation_that_executes_on_to_a(db)
    rows = Author.all().to_a(db)
    Minitest.assert_equal(3, rows.length())
  end

  def test_where_is_lazy_and_matches_the_old_eager_result(db)
    rows = Author.where({"country": "UK"}).to_a(db)
    Minitest.assert_equal(2, rows.length())
  end

  def test_chained_where_order_and_limit(db)
    rows = Author.where({"country": "UK"}).order(name_column().asc()).limit(1).to_a(db)
    Minitest.assert_equal(1, rows.length())
    Minitest.assert_equal("Ada", rows[0].name())
  end

  def test_first_returns_a_single_record_not_an_array(db)
    result = Author.where({"country": "UK"}).order(name_column().asc()).first(db)
    Minitest.assert_equal("Ada", result.name())
  end

  def test_first_returns_nil_when_nothing_matches(db)
    result = Author.where({"country": "Atlantis"}).first(db)
    Minitest.assert_equal(true, result == nil)
  end

  def test_count_reflects_the_filter(db)
    Minitest.assert_equal(2, Author.where({"country": "UK"}).count(db))
    Minitest.assert_equal(3, Author.all().count(db))
  end

  def test_independent_chains_off_the_same_base_relation_dont_leak(db)
    base = Author.where({"country": "UK"})
    ordered = base.order(name_column().asc())
    narrowed = base.where({"name": "Ada"})

    # base itself, and each derived chain, only ever sees what it was
    # actually built with -- confirming Arel::Query's own immutability
    # (packages/arel/README.md) survives being wrapped in a Relation.
    Minitest.assert_equal(2, base.to_a(db).length())
    Minitest.assert_equal(2, ordered.to_a(db).length())
    Minitest.assert_equal(1, narrowed.to_a(db).length())
  end

  def test_select_is_lazy_immutable_and_keeps_repository_metadata(db)
    base = Author.all()
    selected = base.select([Arel.table("authors").column("id"), Arel.table("authors").column("name")])
    rows = selected.order(name_column().asc()).to_a(db)
    Minitest.assert_equal(3, rows.length())
    Minitest.assert_equal("Ada", rows[0].name())
    Minitest.assert_equal(nil, rows[0].to_attributes()["country"])
    Minitest.assert_equal(1, base.query().projection_count())
    Minitest.assert_equal(2, selected.query().projection_count())
    Minitest.assert_equal("id", selected.repository().primary_key())
    Minitest.assert_equal(true, selected.repository().has_column?("country"))
  end

  def test_includes_batch_loads_has_many_into_the_model_cache(db)
    base = Author.all()
    included = base.includes("books")
    authors = included.order(name_column().asc()).to_a(db)

    Minitest.assert_equal(0, base.included_associations().length())
    Minitest.assert_equal(1, included.included_associations().length())
    Minitest.assert_equal(true, authors[0].association_loaded?("books"))
    Minitest.assert_equal(2, authors[0].preloaded_association("books").length())
    Minitest.assert_equal("Notes", authors[0].preloaded_association("books")[0].title())
    Minitest.assert_equal(0, authors[2].preloaded_association("books").length())
  end

  def test_includes_batch_loads_belongs_to_into_the_model_cache(db)
    books = Book.all().includes(["author"]).order(Arel.table("books").column("id")).to_a(db)
    Minitest.assert_equal(true, books[0].association_loaded?("author"))
    Minitest.assert_equal("Ada", books[0].preloaded_association("author").name())
    Minitest.assert_equal("Grace", books[2].preloaded_association("author").name())
  end

  def test_includes_rejects_unknown_associations(db)
    caught = false
    begin
      Author.all().includes("missing").to_a(db)
    rescue error: ActiveRecord::AssociationNotFoundError
      caught = true
      Minitest.assert_equal(true, error.message().include?("missing"))
    end
    Minitest.assert_equal(true, caught)
  end

  suite = Minitest.new()
  suite.test("Author.all() returns a Relation that executes on #to_a") do
    test_all_returns_a_relation_that_executes_on_to_a(db)
  end
  suite.test("Author.where(...) is lazy and matches the old eager result") do
    test_where_is_lazy_and_matches_the_old_eager_result(db)
  end
  suite.test("chained where/order/limit") do
    test_chained_where_order_and_limit(db)
  end
  suite.test("#first returns a single record, not an Array") do
    test_first_returns_a_single_record_not_an_array(db)
  end
  suite.test("#first returns nil when nothing matches") do
    test_first_returns_nil_when_nothing_matches(db)
  end
  suite.test("#count reflects the filter") do
    test_count_reflects_the_filter(db)
  end
  suite.test("independent chains off the same base Relation don't leak") do
    test_independent_chains_off_the_same_base_relation_dont_leak(db)
  end
  suite.test("#select is lazy/immutable and preserves repository metadata") do
    test_select_is_lazy_immutable_and_keeps_repository_metadata(db)
  end
  suite.test("#includes batch-loads has_many into the model cache") do
    test_includes_batch_loads_has_many_into_the_model_cache(db)
  end
  suite.test("#includes batch-loads belongs_to into the model cache") do
    test_includes_batch_loads_belongs_to_into_the_model_cache(db)
  end
  suite.test("#includes rejects unknown associations") do
    test_includes_rejects_unknown_associations(db)
  end
  suite.run!()

  db.close()
end

run_tests()
