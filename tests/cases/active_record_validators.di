# ActiveRecord::Validators: reusable validator building blocks matching
# Repository's existing validator(attributes, exclude_id) -> Array[String]
# shape exactly (see packages/active_record/README.md's "Validators"
# section). `exclude_id` is the record's own id on #update (nil on
# #create), so `uniqueness` can exclude a record's own row from its own
# check; every other validator ignores it.
#
# Array equality in Diamond is identity, not value, comparison (confirmed
# directly: `[] == []` is false) -- every assertion below checks
# #length() plus individual elements by index, the same convention
# active_record_validations_and_callbacks.di already uses for
# ValidationError#errors(), rather than comparing whole Arrays.
require "../../packages/active_record/lib/active_record"
require "../../lib/minitest"

def run_tests()
  def test_presence()
    check = ActiveRecord::Validators.presence("name")
    Minitest.assert_equal(0, check({"name": "Ada"}, nil).length())
    errors = check({"name": ""}, nil)
    Minitest.assert_equal(1, errors.length())
    Minitest.assert_equal("name is required", errors[0])
    Minitest.assert_equal(1, check({}, nil).length())
  end

  def test_presence_custom_message()
    check = ActiveRecord::Validators.presence("name", "can't be blank")
    errors = check({"name": nil}, nil)
    Minitest.assert_equal(1, errors.length())
    Minitest.assert_equal("can't be blank", errors[0])
  end

  def test_length()
    check = ActiveRecord::Validators.length("name", 2, 5)
    Minitest.assert_equal(0, check({"name": "Ada"}, nil).length())
    too_short = check({"name": "A"}, nil)
    Minitest.assert_equal(1, too_short.length())
    Minitest.assert_equal("name is too short (minimum 2)", too_short[0])
    too_long = check({"name": "Abcdefgh"}, nil)
    Minitest.assert_equal(1, too_long.length())
    Minitest.assert_equal("name is too long (maximum 5)", too_long[0])
    Minitest.assert_equal(0, check({"name": nil}, nil).length())
  end

  def test_numericality()
    check = ActiveRecord::Validators.numericality("age")
    Minitest.assert_equal(0, check({"age": 30}, nil).length())
    Minitest.assert_equal(0, check({"age": 3.5}, nil).length())
    not_numeric = check({"age": "thirty"}, nil)
    Minitest.assert_equal(1, not_numeric.length())
    Minitest.assert_equal("age must be numeric", not_numeric[0])
    Minitest.assert_equal(1, check({"age": nil}, nil).length())
  end

  def test_format()
    check = ActiveRecord::Validators.format("email", Regexp.new("^[^@]+@[^@]+$"))
    Minitest.assert_equal(0, check({"email": "ada@example.com"}, nil).length())
    invalid = check({"email": "not-an-email"}, nil)
    Minitest.assert_equal(1, invalid.length())
    Minitest.assert_equal("email is invalid", invalid[0])
    Minitest.assert_equal(1, check({"email": nil}, nil).length())
  end

  def test_inclusion()
    check = ActiveRecord::Validators.inclusion("status", ["active", "inactive"])
    Minitest.assert_equal(0, check({"status": "active"}, nil).length())
    not_included = check({"status": "bogus"}, nil)
    Minitest.assert_equal(1, not_included.length())
    Minitest.assert_equal("status is not included in the list", not_included[0])
  end

  def test_combine_concatenates_every_failure()
    check = ActiveRecord::Validators.combine([
      ActiveRecord::Validators.presence("name"),
      ActiveRecord::Validators.length("name", 2, 5),
    ])
    Minitest.assert_equal(0, check({"name": "Ada"}, nil).length())
    both = check({"name": ""}, nil)
    Minitest.assert_equal(2, both.length())
    Minitest.assert_equal("name is required", both[0])
    Minitest.assert_equal("name is too short (minimum 2)", both[1])
  end

  def test_uniqueness(db, ada_id)
    table = Arel.table("authors")
    check = ActiveRecord::Validators.uniqueness(db, table, "email")
    taken = check({"email": "ada@example.com"}, nil)
    Minitest.assert_equal(1, taken.length())
    Minitest.assert_equal("email has already been taken", taken[0])
    Minitest.assert_equal(0, check({"email": "new@example.com"}, nil).length())
    # exclude_id lets a record's own unchanged row pass its own
    # uniqueness check (the #update fix -- see repository.di) without
    # excluding it from flagging a genuine conflict with someone else's row.
    Minitest.assert_equal(0, check({"email": "ada@example.com"}, ada_id).length())
    Minitest.assert_equal(1, check({"email": "ada@example.com"}, ada_id + 999).length())
  end

  # A real Repository wired up with a combined validator -- the shape
  # README.md's own worked example uses, proving these plug into
  # Repository unmodified.
  def test_repository_integration(db)
    def build_author(row) = row
    validator = ActiveRecord::Validators.combine([
      ActiveRecord::Validators.presence("name"),
      ActiveRecord::Validators.format("email", Regexp.new("^[^@]+@[^@]+$")),
    ])
    repository = ActiveRecord::Repository.new(
      Arel.table("authors"), build_author, "id", nil, validator)
    caught = false
    begin
      repository.create(db, {"name": "", "email": "bad"})
    rescue error: ActiveRecord::ValidationError
      caught = true
      Minitest.assert_equal(2, error.errors().length())
    end
    Minitest.assert_equal(true, caught)
    repository.create(db, {"name": "Grace", "email": "grace@example.com"})
    Minitest.assert_equal(2, repository.all(db).length())
  end

  # Regression test for the exclude_id fix: before it, #update always
  # failed uniqueness validation against the record's own unchanged
  # row, since the check had no way to know which row was "self."
  def test_update_does_not_conflict_with_its_own_unique_value(db)
    def build_author(row) = row
    validator = ActiveRecord::Validators.combine([
      ActiveRecord::Validators.uniqueness(db, Arel.table("authors"), "email"),
    ])
    repository = ActiveRecord::Repository.new(
      Arel.table("authors"), build_author, "id", nil, validator)
    repository.create(db, {"name": "Hedy", "email": "hedy@example.com"})
    id = db.last_insert_row_id()
    # Updating an unrelated field, email unchanged, must not conflict
    # with the record's own existing row.
    repository.update(db, id, {"name": "Hedy Lamarr", "email": "hedy@example.com"})
    updated = repository.find(db, id)
    Minitest.assert_equal("Hedy Lamarr", updated["name"])
    # A real conflict with a *different* row must still be caught.
    repository.create(db, {"name": "Other", "email": "other@example.com"})
    caught = false
    begin
      repository.update(db, id, {"name": "Hedy Lamarr", "email": "other@example.com"})
    rescue error: ActiveRecord::ValidationError
      caught = true
    end
    Minitest.assert_equal(true, caught)
  end

  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT, email TEXT)")
  db.execute("INSERT INTO authors (name, email) VALUES (?, ?)", ["Ada", "ada@example.com"])
  ada_id = db.last_insert_row_id()

  suite = Minitest.new()
  suite.test("presence") do
    test_presence()
  end
  suite.test("presence with a custom message") do
    test_presence_custom_message()
  end
  suite.test("length") do
    test_length()
  end
  suite.test("numericality") do
    test_numericality()
  end
  suite.test("format") do
    test_format()
  end
  suite.test("inclusion") do
    test_inclusion()
  end
  suite.test("combine concatenates every failure") do
    test_combine_concatenates_every_failure()
  end
  suite.test("uniqueness") do
    test_uniqueness(db, ada_id)
  end
  suite.test("plugs into Repository unmodified") do
    test_repository_integration(db)
  end
  suite.test("update does not conflict with its own unique value") do
    test_update_does_not_conflict_with_its_own_unique_value(db)
  end
  suite.run!()

  db.close()
end

run_tests()
