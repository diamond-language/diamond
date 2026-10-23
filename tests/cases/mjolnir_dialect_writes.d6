# Repo#insert and #update across dialects. A database without RETURNING for the statement (MySQL for both, MariaDB for UPDATE) is
# written and then read back by key; the rest use RETURNING. Runs on in-memory SQLite by default -- both with RETURNING and, through a
# visitor that reports no support for it, down the read-back path -- so the fallback is covered in every test run. packages/mjolnir/
# test_dialects.sh runs the same assertions against live MariaDB, MySQL and PostgreSQL servers (DIAMOND_MJOLNIR_DIALECT picks one).
require "../../packages/mjolnir/lib/mjolnir"
require "../../lib/minitest"

struct WAuthor(id: Int, name: String, email: String | Nil, created_at: Int | Nil, updated_at: Int | Nil)
end
struct WNote(id: Int, body: String)
end
struct WSeen(game_id: Int, player_id: Int, hits: Int)
end

def build_w_author(row) = WAuthor.new(row["id"], row["name"], row["email"], row["created_at"], row["updated_at"])
def build_w_note(row) = WNote.new(row["id"], row["body"])
def build_w_seen(row) = WSeen.new(row["game_id"], row["player_id"], row["hits"])

# SQLite, but reporting no RETURNING support, so Repo takes the write-then-read path.
class NoReturningVisitor < Arel::SQLiteVisitor
  def supports_extension?(name: String) -> Bool
    name != "returning clauses" && name != "RETURNING on UPDATE" &&
      name != "per-column default values" && name != "named-constraint conflict targets"
  end
end

def dialect_name()
  chosen = ENV["DIAMOND_MJOLNIR_DIALECT"]
  if chosen == nil then "sqlite" else chosen end
end

# A fresh connection with empty tables, and the visitor for the dialect (nil for plain SQLite).
def w_connect()
  dialect = dialect_name()
  if dialect == "sqlite" || dialect == "sqlite_no_returning"
    db = SQLite3.open(":memory:")
    key = "INTEGER PRIMARY KEY"
    text = "TEXT"
  elsif dialect == "postgres"
    db = PostgreSQL.open(ENV["DIAMOND_PG_TEST_CONNINFO"])
    key = "SERIAL PRIMARY KEY"
    text = "TEXT"
  else
    db = MySQL.open(ENV["DIAMOND_MYSQL_TEST_HOST"], ENV["DIAMOND_MYSQL_TEST_USER"], ENV["DIAMOND_MYSQL_TEST_PASSWORD"],
      ENV["DIAMOND_MYSQL_TEST_DATABASE"], ENV["DIAMOND_MYSQL_TEST_PORT"].to_i())
    key = "INT AUTO_INCREMENT PRIMARY KEY"
    text = "VARCHAR(100)"
  end
  ["authors", "notes", "seen"].each() do |table| db.execute("DROP TABLE IF EXISTS #{table}") end
  db.execute("CREATE TABLE authors (id #{key}, name #{text} NOT NULL, email #{text} UNIQUE, created_at BIGINT, updated_at BIGINT)")
  db.execute("CREATE TABLE notes (id #{key}, body #{text} NOT NULL)")
  db.execute("CREATE TABLE seen (game_id INT NOT NULL, player_id INT NOT NULL, hits INT NOT NULL, PRIMARY KEY (game_id, player_id))")
  visitor = nil
  visitor = NoReturningVisitor.new() if dialect == "sqlite_no_returning"
  visitor = Arel::MariaDBVisitor.new() if dialect == "mariadb"
  visitor = Arel::MySQLVisitor.new() if dialect == "mysql"
  visitor = Arel::PostgreSQLVisitor.new() if dialect == "postgres"
  [db, visitor]
end

def w_repo()
  connected = w_connect()
  Mjolnir::Repo.new(connected[0], connected[1])
end

def w_authors() = Mjolnir::Schema.new("authors", {"id": :int, "name": :string, "email": :string, "created_at": :int, "updated_at": :int}, build_w_author, "id", true)
def w_notes() = Mjolnir::Schema.new("notes", {"id": :int, "body": :string}, build_w_note)
def w_seen() = Mjolnir::Schema.new("seen", {"game_id": :int, "player_id": :int, "hits": :int}, build_w_seen, primary_key: ["game_id", "player_id"])

def w_author(repo, name, email = nil) =
  repo.insert!(Mjolnir::Changeset.cast(w_authors(), nil, {"name": name, "email": email}, ["name", "email"]))

def run_tests()
  suite = Minitest.new()

  suite.test("insert returns the stored row with its generated key and timestamps") do
    repo = w_repo()
    ada = w_author(repo, "Ada", "ada@example.com")
    Minitest.assert(ada.id() > 0)
    Minitest.assert_equal("Ada", ada.name())
    Minitest.assert_equal("ada@example.com", ada.email())
    Minitest.assert(ada.created_at() != nil)
    Minitest.assert_equal("Ada", repo.get(w_authors(), ada.id()).name())
    grace = w_author(repo, "Grace")
    Minitest.assert(grace.id() != ada.id())
  end

  suite.test("insert with an explicit key reads that row back") do
    repo = w_repo()
    note = repo.insert!(Mjolnir::Changeset.cast(w_notes(), nil, {"id": 77, "body": "pinned"}, ["id", "body"]))
    Minitest.assert_equal(77, note.id())
    Minitest.assert_equal("pinned", note.body())
  end

  suite.test("update returns the changed row and refreshes updated_at") do
    repo = w_repo()
    ada = w_author(repo, "Ada")
    changed = repo.update!(Mjolnir::Changeset.cast(w_authors(), ada, {"name": "Ada L"}, ["name"]).put_change("updated_at", 5))
    Minitest.assert_equal(ada.id(), changed.id())
    Minitest.assert_equal("Ada L", changed.name())
    Minitest.assert_equal(5, changed.updated_at())
    Minitest.assert_equal("Ada L", repo.get(w_authors(), ada.id()).name())
  end

  suite.test("an update that sets a value it already has is not a stale entry") do
    repo = w_repo()
    note = repo.insert!(Mjolnir::Changeset.cast(w_notes(), nil, {"body": "same"}, ["body"]))
    again = repo.update!(Mjolnir::Changeset.cast(w_notes(), note, {}, []).put_change("body", "same"))
    Minitest.assert_equal(note.id(), again.id())
    Minitest.assert_equal("same", again.body())
  end

  suite.test("updating a row that is gone raises StaleEntryError") do
    repo = w_repo()
    note = repo.insert!(Mjolnir::Changeset.cast(w_notes(), nil, {"body": "temporary"}, ["body"]))
    repo.delete_all(w_notes().query().where({"id": note.id()}))
    outcome = "no error"
    begin
      repo.update!(Mjolnir::Changeset.cast(w_notes(), note, {"body": "late"}, ["body"]))
    rescue error: Mjolnir::StaleEntryError
      outcome = "stale"
    end
    Minitest.assert_equal("stale", outcome)
  end

  suite.test("a declared unique constraint becomes a field error on insert and on update") do
    repo = w_repo()
    ada = w_author(repo, "Ada", "taken@example.com")
    grace = w_author(repo, "Grace", "grace@example.com")
    duplicate = Mjolnir::Changeset.cast(w_authors(), nil, {"name": "Other", "email": "taken@example.com"}, ["name", "email"]).unique_constraint("email")
    inserted = repo.insert(duplicate)
    Minitest.assert(inserted is Mjolnir::Err)
    Minitest.assert_equal(["has already been taken"], inserted.error().errors()["email"])
    clash = Mjolnir::Changeset.cast(w_authors(), grace, {"email": "taken@example.com"}, ["email"]).unique_constraint("email")
    updated = repo.update(clash)
    Minitest.assert(updated is Mjolnir::Err)
    Minitest.assert_equal(["has already been taken"], updated.error().errors()["email"])
    Minitest.assert_equal("grace@example.com", repo.get(w_authors(), grace.id()).email())
  end

  suite.test("changing the primary key updates the loaded row and returns it at the new key") do
    repo = w_repo()
    note = repo.insert!(Mjolnir::Changeset.cast(w_notes(), nil, {"body": "moving"}, ["body"]))
    moved = repo.update!(Mjolnir::Changeset.cast(w_notes(), note, {"id": 500}, ["id"]))
    Minitest.assert_equal(500, moved.id())
    Minitest.assert_equal("moving", moved.body())
    Minitest.assert_equal(nil, repo.get(w_notes(), note.id()))
  end

  suite.test("a composite key row is inserted and updated by its parts") do
    repo = w_repo()
    seen = repo.insert!(Mjolnir::Changeset.cast(w_seen(), nil, {"game_id": 1, "player_id": 2, "hits": 3}, ["game_id", "player_id", "hits"]))
    Minitest.assert_equal(3, seen.hits())
    more = repo.update!(Mjolnir::Changeset.cast(w_seen(), seen, {"hits": 9}, ["hits"]))
    Minitest.assert_equal(9, more.hits())
    Minitest.assert_equal(9, repo.get(w_seen(), [1, 2]).hits())
  end

  suite.run!()
end

run_tests()
