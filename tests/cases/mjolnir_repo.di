require "../../packages/mjolnir/lib/mjolnir"
require "../../lib/minitest"

struct RepoUser(id: Int, name: String, email: String, age: Int | Nil)
end

def build_repo_user(row) = RepoUser.new(row["id"], row["name"], row["email"], row["age"])

def repo_users()
  Mjolnir::Schema.new("users", {"id": :int, "name": :string, "email": :string, "age": :int},
    build_repo_user)
end

def user_changeset(users, entity, attrs)
  changeset = Mjolnir::Changeset.cast(users, entity, attrs, ["name", "email", "age"])
  changeset = changeset.validate_required(["name", "email"])
  changeset.unique_constraint("email")
end

def fresh_repo()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT NOT NULL UNIQUE, age INTEGER)")
  Mjolnir::Repo.new(db)
end

def run_tests()
  users = repo_users()
  suite = Minitest.new()

  suite.test("insert returns Ok with a frozen entity carrying the generated id") do
    repo = fresh_repo()
    result = repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "ada@example.com", "age": 36}))
    Minitest.assert(result.ok?())
    Minitest.assert_equal(1, result.value().id())
    Minitest.assert_equal("Ada", result.value().name())
    Minitest.assert(result.value().frozen?())
  end

  suite.test("insert of an invalid changeset returns Err and writes nothing") do
    repo = fresh_repo()
    result = repo.insert(user_changeset(users, nil, {"name": "Ada"}))
    Minitest.assert(result.err?())
    Minitest.assert_equal(["can't be blank"], result.error().errors()["email"])
    Minitest.assert_equal(0, repo.count(users.query()))
  end

  suite.test("a declared unique constraint becomes a field error") do
    repo = fresh_repo()
    repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com"}))
    result = repo.insert(user_changeset(users, nil, {"name": "Other", "email": "a@example.com"}))
    Minitest.assert(result.err?())
    Minitest.assert_equal(["has already been taken"], result.error().errors()["email"])
  end

  suite.test("an undeclared constraint failure raises") do
    repo = fresh_repo()
    repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com"}))
    plain = Mjolnir::Changeset.cast(users, nil, {"name": "X", "email": "a@example.com"}, ["name", "email"])
    raised = false
    begin
      repo.insert(plain)
    rescue error: StandardError
      raised = true
    end
    Minitest.assert(raised)
  end

  suite.test("queries filter, order, limit, and offset") do
    repo = fresh_repo()
    ["Cy", "Ada", "Bo"].each() do |name|
      repo.insert(user_changeset(users, nil, {"name": name, "email": "#{name}@example.com", "age": name.length()}))
    end
    names = repo.all(users.query().order_by("name")).map() do |user| user.name() end
    Minitest.assert_equal(["Ada", "Bo", "Cy"], names)
    newest = repo.all(users.query().order_by("name", :desc).limit(2).offset(1)).map() do |user| user.name() end
    Minitest.assert_equal(["Bo", "Ada"], newest)
    Minitest.assert_equal("Ada", repo.one(users.query().where({"age": 3})).name())
    Minitest.assert_equal(2, repo.count(users.query().where(users.column("age").lt(3))))
    Minitest.assert_equal(nil, repo.one(users.query().where({"name": "Zed"})))
  end

  suite.test("get finds by primary key") do
    repo = fresh_repo()
    saved = repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com"})).value()
    Minitest.assert_equal(saved, repo.get(users, saved.id()))
    Minitest.assert_equal(nil, repo.get(users, 99))
  end

  suite.test("update writes only changed fields and returns the new entity") do
    repo = fresh_repo()
    saved = repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com", "age": 36})).value()
    result = repo.update(user_changeset(users, saved, {"age": 37}))
    Minitest.assert(result.ok?())
    Minitest.assert_equal(37, result.value().age())
    Minitest.assert_equal("Ada", result.value().name())
    unchanged = repo.update(user_changeset(users, result.value(), {"age": 37}))
    Minitest.assert_equal(result.value(), unchanged.value())
  end

  suite.test("update of a deleted row raises StaleEntryError") do
    repo = fresh_repo()
    saved = repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com"})).value()
    repo.delete(users, saved.id())
    raised = false
    begin
      repo.update(user_changeset(users, saved, {"name": "Grace"}))
    rescue error: Mjolnir::StaleEntryError
      raised = true
    end
    Minitest.assert(raised)
  end

  suite.test("transaction commits, and rolls back on Err or exception") do
    repo = fresh_repo()
    repo.transaction() do |tx|
      tx.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com"}))
    end
    Minitest.assert_equal(1, repo.count(users.query()))
    failed = repo.transaction() do |tx|
      tx.insert(user_changeset(users, nil, {"name": "Bo", "email": "b@example.com"}))
      tx.insert(user_changeset(users, nil, {"name": "Dup", "email": "a@example.com"}))
    end
    Minitest.assert(failed.err?())
    Minitest.assert_equal(1, repo.count(users.query()))
    raised = false
    begin
      repo.transaction() do |tx|
        tx.insert(user_changeset(users, nil, {"name": "Cy", "email": "c@example.com"}))
        raise RuntimeError.new("boom")
      end
    rescue error: RuntimeError
      raised = true
    end
    Minitest.assert(raised)
    Minitest.assert_equal(1, repo.count(users.query()))
  end

  suite.test("results are exhaustively matchable") do
    repo = fresh_repo()
    result = repo.insert(user_changeset(users, nil, {"name": "Ada", "email": "a@example.com"}))
    label = case result
            when Mjolnir::Ok then "ok"
            when Mjolnir::Err then "err"
            end
    Minitest.assert_equal("ok", label)
  end

  suite.run!()
end

run_tests()
