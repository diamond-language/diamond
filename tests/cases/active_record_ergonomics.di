# ActiveRecord::Model's smaller ergonomic additions (see ROADMAP.md):
# self.find_by, the #save!/#destroy!/self.create! bang aliases, and
# #to_json/#as_json. None of these add a new mechanism -- find_by is
# where(...).first, the bang methods are aliases (this package's plain
# forms already always raise on failure, so there's no quiet-failure
# mode for a bang form to distinguish from), and to_json/as_json build on
# #to_attributes and the JSON module already in the prelude -- so this
# suite is mostly about confirming the one-line wiring is correct, not
# exercising new persistence machinery.
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

  # The "scope" convention callout from README.md -- an ordinary class
  # method reaching the same result a `scope :uk, -> { ... }` macro
  # would, since that macro isn't reachable (no define_method/
  # method_missing).
  def self.uk() = self.where({"country": "UK"})
end

def build_author(row) = Author.new(row)

def run_tests()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
  Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))

  Author.create(db, {"name": "Ada", "country": "UK"})
  Author.create(db, {"name": "Grace", "country": "USA"})

  def test_find_by_returns_a_single_matching_instance(db)
    found = Author.find_by(db, {"name": "Ada"})
    Minitest.assert_equal("Ada", found.name())
  end

  def test_find_by_returns_nil_when_nothing_matches(db)
    Minitest.assert_equal(true, Author.find_by(db, {"name": "Nobody"}) == nil)
  end

  def test_bang_methods_behave_like_their_plain_counterparts(db)
    ada = Author.find_by(db, {"name": "Ada"})
    ada.name = "Ada Lovelace"
    ada.save!(db)
    Minitest.assert_equal("Ada Lovelace", Author.find(db, ada.id()).name())

    created = Author.create!(db, {"name": "Marie", "country": "France"})
    Minitest.assert_equal(3, Author.all().count(db))

    marie = Author.find_by(db, {"name": "Marie"})
    marie.destroy!(db)
    Minitest.assert_equal(2, Author.all().count(db))
  end

  def test_as_json_is_the_plain_attributes_hash(db)
    ada = Author.find_by(db, {"name": "Ada Lovelace"})
    json_hash = ada.as_json()
    Minitest.assert_equal("Ada Lovelace", json_hash["name"])
    Minitest.assert_equal("UK", json_hash["country"])
  end

  def test_to_json_round_trips_through_the_json_module(db)
    ada = Author.find_by(db, {"name": "Ada Lovelace"})
    parsed = JSON.parse(ada.to_json())
    Minitest.assert_equal("Ada Lovelace", parsed["name"])
    Minitest.assert_equal("UK", parsed["country"])
  end

  def test_scope_style_class_method(db)
    Minitest.assert_equal(1, Author.uk().to_a(db).length())
    Minitest.assert_equal("Ada Lovelace", Author.uk().to_a(db)[0].name())
  end

  suite = Minitest.new()
  suite.test("self.find_by returns a single matching instance") do
    test_find_by_returns_a_single_matching_instance(db)
  end
  suite.test("self.find_by returns nil when nothing matches") do
    test_find_by_returns_nil_when_nothing_matches(db)
  end
  suite.test("bang methods behave like their plain counterparts") do
    test_bang_methods_behave_like_their_plain_counterparts(db)
  end
  suite.test("#as_json is the plain attributes Hash") do
    test_as_json_is_the_plain_attributes_hash(db)
  end
  suite.test("#to_json round-trips through the JSON module") do
    test_to_json_round_trips_through_the_json_module(db)
  end
  suite.test("scope-style class method (self.uk)") do
    test_scope_style_class_method(db)
  end
  suite.run!()

  db.close()
end

run_tests()
