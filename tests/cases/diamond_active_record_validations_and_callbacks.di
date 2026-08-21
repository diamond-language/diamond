require "../../packages/diamond-active_record/lib/diamond-active_record"
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

class SaveLog
  def initialize()
    @entries = []
  end
  def push(entry)
    @entries.push(entry)
  end
  def entries() = @entries
end

def run_tests()
  def map_author(row)
    LibraryAuthor.new(row["id"], row["name"], row["country"])
  end

  def validate_author(attributes)
    errors = []
    if attributes["name"] == nil || attributes["name"] == ""
      errors.push("name is required")
    end
    if attributes["country"] == nil
      errors.push("country is required")
    end
    errors
  end

  def make_before_save(log)
    def hook(db, attributes, on)
      log.push("before_save:#{on}")
      if on == :destroy
        return attributes
      end
      updated = {}
      keys = attributes.keys()
      index = 0
      while index < keys.length()
        updated[keys[index]] = attributes[keys[index]]
        index += 1
      end
      updated["country"] = attributes["country"].upcase()
      updated
    end
    hook
  end

  def make_after_save(log)
    def hook(db, attributes, on)
      log.push("after_save:#{on}:#{attributes["country"]}")
    end
    hook
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, country TEXT)")

  log = SaveLog.new()
  repository = ActiveRecordRepository.new(
    Arel.table("authors"), map_author, "id", nil,
    validate_author, make_before_save(log), make_after_save(log))

  # An invalid create is rejected before any SQL runs.
  validation_failed = false
  begin
    repository.create(db, {"name": "", "country": "UK"})
  rescue e: ActiveRecordValidationError
    validation_failed = true
    Minitest.assert_equal(1, e.errors().length())
    Minitest.assert_equal("name is required", e.errors()[0])
  end
  Minitest.assert_equal(true, validation_failed)
  Minitest.assert_equal(0, repository.all(db).length())

  # A valid create runs before_save (transforming attributes) then
  # after_save (observing the transformed result).
  repository.create(db, {"name": "Ada", "country": "uk"})
  Minitest.assert_equal("UK", repository.find(db, 1).country())
  Minitest.assert_equal(2, log.entries().length())
  Minitest.assert_equal("before_save:create", log.entries()[0])
  Minitest.assert_equal("after_save:create:UK", log.entries()[1])

  # update goes through the same validate -> before_save -> write ->
  # after_save pipeline.
  repository.update(db, 1, {"name": "Ada", "country": "us"})
  Minitest.assert_equal("US", repository.find(db, 1).country())

  # An invalid update is rejected before any SQL runs; the row is
  # unchanged.
  update_failed = false
  begin
    repository.update(db, 1, {"name": "", "country": "us"})
  rescue e: ActiveRecordValidationError
    update_failed = true
  end
  Minitest.assert_equal(true, update_failed)
  Minitest.assert_equal("US", repository.find(db, 1).country())

  # delete runs before_save/after_save with on == :destroy and a
  # single-entry {id_column => id} attributes Hash, not the full row --
  # there is no attributes payload of its own for a delete.
  Minitest.assert_equal(1, repository.delete(db, 1))
  Minitest.assert_equal(nil, repository.find(db, 1))
  Minitest.assert_equal(6, log.entries().length())
  Minitest.assert_equal("before_save:destroy", log.entries()[4])
  Minitest.assert_equal("after_save:destroy:nil", log.entries()[5])

  db.close()
end

run_tests()
puts("diamond_active_record validations/callbacks smoke ok")
