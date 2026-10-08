require "../../packages/mjolnir/lib/mjolnir"
require "../../lib/minitest"

struct MjolnirUser(id: Int, name: String, email: String, age: Int | Nil)
end

def build_mjolnir_user(row) = MjolnirUser.new(row["id"], row["name"], row["email"], row["age"])

def mjolnir_users()
  Mjolnir::Schema.new("users", {"id": :int, "name": :string, "email": :string, "age": :int},
    build_mjolnir_user)
end

def run_tests()
  users = mjolnir_users()
  suite = Minitest.new()

  suite.test("cast whitelists, coerces, and treats blank as nil") do
    changeset = Mjolnir::Changeset.cast(users, nil,
      {"name": "Ada", "age": "36", "email": "", "id": 9}, ["name", "age", "email"])
    Minitest.assert(changeset.valid?())
    Minitest.assert_equal("Ada", changeset.changes()["name"])
    Minitest.assert_equal(36, changeset.changes()["age"])
    Minitest.assert_equal(nil, changeset.changes()["email"])
    Minitest.assert(!changeset.changes().has_key?("id"))
  end

  suite.test("cast reports uncastable values as errors") do
    changeset = Mjolnir::Changeset.cast(users, nil, {"age": "old"}, ["age"])
    Minitest.assert(!changeset.valid?())
    Minitest.assert_equal(["is invalid"], changeset.errors()["age"])
  end

  suite.test("cast rejects fields the schema does not declare") do
    raised = false
    begin
      Mjolnir::Changeset.cast(users, nil, {"nope": 1}, ["nope"])
    rescue error: Mjolnir::UnknownFieldError
      raised = true
    end
    Minitest.assert(raised)
  end

  suite.test("only real differences against the entity are changes") do
    entity = MjolnirUser.new(1, "Ada", "ada@example.com", 36)
    changeset = Mjolnir::Changeset.cast(users, entity, {"name": "Ada", "age": 37}, ["name", "age"])
    Minitest.assert_equal({"age": 37}, changeset.changes())
    Minitest.assert_equal("Ada", changeset.get_field("name"))
    Minitest.assert_equal(37, changeset.apply_changes()["age"])
  end

  suite.test("validations accumulate errors without mutating the original") do
    base = Mjolnir::Changeset.cast(users, nil, {"name": "A", "email": "nope", "age": 200}, ["name", "email", "age"])
    checked = base
      .validate_required(["name", "email"])
      .validate_length("name", 2, 10)
      .validate_format("email", "@")
      .validate_number("age", 0, 150)
    Minitest.assert(base.valid?())
    Minitest.assert(!checked.valid?())
    Minitest.assert_equal(1, checked.errors()["name"].length())
    Minitest.assert_equal(["has invalid format"], checked.errors()["email"])
    Minitest.assert_equal(["must be at most 150"], checked.errors()["age"])
  end

  suite.test("validate_required flags a missing field") do
    changeset = Mjolnir::Changeset.cast(users, nil, {}, ["name"]).validate_required(["name"])
    Minitest.assert_equal(["can't be blank"], changeset.errors()["name"])
  end

  suite.run!()
end

run_tests()
