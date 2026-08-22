require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

class LibraryAuthor
  def initialize(id, name, country)
    @id = id
    @name = name
    @country = country
  end
  def name() = @name
  def country() = @country
end

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"], row["country"])
  end

  def test_tracks_changes_without_mutating_the_original()
    original = {"name": "Ada", "country": "UK"}
    dirty = ActiveRecord::DirtyAttributes.new(original)
    Minitest.assert_equal(false, dirty.changed?())

    dirty.set("country", "England")
    Minitest.assert_equal(true, dirty.changed?())
    Minitest.assert_equal(true, dirty.attribute_changed?("country"))
    Minitest.assert_equal(false, dirty.attribute_changed?("name"))
    Minitest.assert_equal("England", dirty.get("country"))
    Minitest.assert_equal("Ada", dirty.get("name"))

    changes = dirty.changes()
    Minitest.assert_equal(1, changes.keys().length())
    Minitest.assert_equal("England", changes["country"])

    whole = dirty.to_h()
    Minitest.assert_equal("Ada", whole["name"])
    Minitest.assert_equal("England", whole["country"])

    # The original Hash passed in is never mutated -- DirtyAttributes
    # keeps its own copy to diff against.
    Minitest.assert_equal("UK", original["country"])
  end

  def test_setting_back_to_the_original_value_is_not_a_change()
    dirty = ActiveRecord::DirtyAttributes.new({"country": "UK"})
    dirty.set("country", "England")
    dirty.set("country", "UK")
    Minitest.assert_equal(false, dirty.changed?())
    Minitest.assert_equal(0, dirty.changes().keys().length())
  end

  def test_feeds_directly_into_repository_update()
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")
    db.execute("INSERT INTO authors (name, country) VALUES (?, ?)", ["Ada", "UK"])
    repository = ActiveRecord::Repository.new(Arel.table("authors"), map_author)

    row = db.query("SELECT * FROM authors WHERE id = ?", [1])[0]
    dirty = ActiveRecord::DirtyAttributes.new(row)
    dirty.set("country", "England")
    if dirty.changed?()
      repository.update(db, row["id"], dirty.changes())
    end
    Minitest.assert_equal("England", repository.find(db, 1).country())

    # A no-op DirtyAttributes never even needs to call update: #changes
    # is empty and #changed? says so up front.
    row2 = db.query("SELECT * FROM authors WHERE id = ?", [1])[0]
    dirty2 = ActiveRecord::DirtyAttributes.new(row2)
    Minitest.assert_equal(false, dirty2.changed?())

    db.close()
  end

  suite = Minitest.new()
  suite.test("tracks changes without mutating the original",
             test_tracks_changes_without_mutating_the_original)
  suite.test("setting back to the original value is not a change",
             test_setting_back_to_the_original_value_is_not_a_change)
  suite.test("feeds directly into repository update",
             test_feeds_directly_into_repository_update)
  suite.run!()
end

run_tests()
