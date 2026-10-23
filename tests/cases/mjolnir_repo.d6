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
  Mjolnir::Changeset.cast(users, entity, attrs, ["name", "email", "age"])
    .validate_required(["name", "email"])
    .unique_constraint("email")
end

def build_board_row(row) = row

struct Seen(game_id: Int, player_id: Int, hits: Int)
end

def build_seen(row) = Seen.new(row["game_id"], row["player_id"], row["hits"])

def board_changeset(boards, attrs) =
  Mjolnir::Changeset.cast(boards, nil, attrs, ["game_id", "slug", "label"]).unique_constraint("slug", columns: ["game_id", "slug"])

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

  suite.test("a composite unique constraint becomes a field error, and a different one still raises") do
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE boards (id INTEGER PRIMARY KEY, game_id INTEGER NOT NULL, slug TEXT NOT NULL, label TEXT UNIQUE, UNIQUE (game_id, slug))")
    boards = Mjolnir::Schema.new("boards", {"id": :int, "game_id": :int, "slug": :string, "label": :string}, build_board_row)
    repo = Mjolnir::Repo.new(db)
    attrs = {"game_id": 1, "slug": "a", "label": "x"}
    Minitest.assert(repo.insert(board_changeset(boards, attrs)).ok?())
    taken = repo.insert(board_changeset(boards, {"game_id": 1, "slug": "a", "label": "y"}))
    Minitest.assert(taken.err?())
    Minitest.assert_equal(["has already been taken"], taken.error().errors()["slug"])
    Minitest.assert(repo.insert(board_changeset(boards, {"game_id": 2, "slug": "a", "label": "z"})).ok?())
    raised = false
    begin
      repo.insert(board_changeset(boards, {"game_id": 3, "slug": "b", "label": "x"}))
    rescue error: StandardError
      raised = true
    end
    Minitest.assert(raised)
  end

  suite.test("a changeset can update an entity built as a plain Hash row") do
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE boards (id INTEGER PRIMARY KEY, game_id INTEGER NOT NULL, slug TEXT NOT NULL, label TEXT)")
    boards = Mjolnir::Schema.new("boards", {"id": :int, "game_id": :int, "slug": :string, "label": :string}, build_board_row)
    repo = Mjolnir::Repo.new(db)
    row = repo.insert!(Mjolnir::Changeset.cast(boards, nil, {"game_id": 1, "slug": "a", "label": "x"}, ["game_id", "slug", "label"]))
    updated = repo.update!(Mjolnir::Changeset.cast(boards, row, {"label": "y"}, ["label"]))
    Minitest.assert_equal("y", updated["label"])
    Minitest.assert_equal("a", repo.get(boards, row["id"])["slug"])
  end

  suite.test("a transaction takes the write lock up front") do
    path = "/tmp/mjolnir_tx_#{Time.now().to_i()}.db"
    first = SQLite3.open(path)
    first.execute("CREATE TABLE t (id INTEGER PRIMARY KEY)")
    second = SQLite3.open(path)
    first_repo = Mjolnir::Repo.new(first)
    second_repo = Mjolnir::Repo.new(second)
    blocked = false
    first_repo.transaction() do |tx|
      begin
        second_repo.transaction() do |other| 1 end
      rescue error: StandardError
        blocked = true
      end
      1
    end
    File.delete(path)
    Minitest.assert(blocked)
  end

  suite.test("a composite primary key finds, updates and deletes by its parts") do
    db = SQLite3.open(":memory:")
    db.execute("CREATE TABLE seen (game_id INTEGER NOT NULL, player_id INTEGER NOT NULL, hits INTEGER NOT NULL, PRIMARY KEY (game_id, player_id))")
    seen = Mjolnir::Schema.new("seen", {"game_id": :int, "player_id": :int, "hits": :int}, build_seen, primary_key: ["game_id", "player_id"])
    repo = Mjolnir::Repo.new(db)
    Minitest.assert(seen.composite?())
    Minitest.assert_equal(["game_id", "player_id"], seen.primary_keys())
    repo.insert!(Mjolnir::Changeset.cast(seen, nil, {"game_id": 1, "player_id": 2, "hits": 1}, ["game_id", "player_id", "hits"]))
    repo.insert!(Mjolnir::Changeset.cast(seen, nil, {"game_id": 1, "player_id": 3, "hits": 5}, ["game_id", "player_id", "hits"]))
    row = repo.get(seen, [1, 2])
    Minitest.assert_equal(1, row.hits())
    Minitest.assert_equal(nil, repo.get(seen, [2, 2]))
    repo.update!(Mjolnir::Changeset.cast(seen, row, {"hits": 9}, ["hits"]))
    Minitest.assert_equal(9, repo.get(seen, [1, 2]).hits())
    Minitest.assert_equal(5, repo.get(seen, [1, 3]).hits())
    Minitest.assert_equal(1, repo.delete(seen, [1, 2]))
    Minitest.assert_equal(1, repo.count(seen.query()))
    raised = false
    begin
      repo.get(seen, 1)
    rescue error: ArgumentError
      raised = true
    end
    Minitest.assert(raised)
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
