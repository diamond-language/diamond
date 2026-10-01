# The notebook's SQLite database: schema migrations, notes and their tags,
# full-text search, and an all-or-nothing import.

# A note as read back from the database, with its tags already collected.
struct Note(id: Int, title: String, body: String, created: String, tags: Array[String])
end

# Any problem with the user's request (unknown note, bad input), as opposed to
# a failure of the program itself.
class NotesError < StandardError
end

# Each migration moves the schema up one version; PRAGMA user_version
# records how far a database has come, so opening an old file brings it
# up to date and opening a current one does nothing. Never edit an entry that
# has shipped; add a new one at the end instead.
def migrations() -> Array[Array[String]]
  [
    [
      "CREATE TABLE notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL, created TEXT NOT NULL)",
      "CREATE TABLE tags (note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, name TEXT NOT NULL, PRIMARY KEY (note_id, name))"
    ],
    # Version 2: full-text search. The `notes_fts` index stores no text of its
    # own (content='notes'), so two triggers keep it in step with inserts and
    # deletes, and the last statement indexes notes that already existed.
    [
      "CREATE VIRTUAL TABLE notes_fts USING fts5(title, body, content='notes', content_rowid='id')",
      "CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN INSERT INTO notes_fts (rowid, title, body) VALUES (new.id, new.title, new.body); END",
      "CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN INSERT INTO notes_fts (notes_fts, rowid, title, body) VALUES ('delete', old.id, old.title, old.body); END",
      "INSERT INTO notes_fts (rowid, title, body) SELECT id, title, body FROM notes"
    ]
  ]
end

# All database access. `now` is injected (a "YYYY-MM-DD" string) instead of
# read from the clock, so tests can make creation dates deterministic.
class NoteStore
  def initialize(path: String, now: String)
    @db = SQLite3.open(path)
    @now = now
    @in_transaction = false

    # SQLite only enforces foreign keys (and so ON DELETE CASCADE on tags)
    # when asked, per connection.
    @db.execute("PRAGMA foreign_keys = ON")

    # Bring a new or old file up to the current schema.
    migrate()
  end

  def close()
    @db.close()
  end

  # How many migrations this database has had applied.
  def version() -> Int = @db.query("PRAGMA user_version")[0]["user_version"]

  # Applies each missing migration, one transaction apiece, so a failure
  # leaves the file at the last good version instead of half-migrated. The
  # version number is written in the same transaction as the schema change.
  def migrate()
    steps = migrations()

    while version() < steps.length()
      target = version() + 1

      transaction() do
        steps[target - 1].each() do |sql| @db.execute(sql) end
        @db.execute("PRAGMA user_version = #{target}")
      end
    end
  end

  # Runs the block inside BEGIN/COMMIT. Anything that leaves the block
  # early -- an exception, or a `return` or `break` from inside it --
  # rolls back instead, which is why this is an ensure, not a rescue. A
  # transaction started inside another one is part of the outer one.
  def transaction(&body)
    # Nested call: SQLite cannot nest BEGIN, so just run inside the outer one.
    return body() if @in_transaction

    @db.execute("BEGIN")
    @in_transaction = true
    committed = false

    begin
      result = body()
      @db.execute("COMMIT")
      committed = true
      result
    # `ensure` runs however the block was left. Only a finished COMMIT sets
    # `committed`, so every other exit (including `return` and `break` out of
    # the block, see testdata/transaction_exits.di) rolls back.
    ensure
      @in_transaction = false
      @db.execute("ROLLBACK") unless committed
    end
  end

  # Inserts a note and its tags together; returns the new id.
  def add(title: String, body: String, tags: Array[String]) -> Int
    transaction() do
      @db.execute("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)", [title, body, @now])
      id = @db.last_insert_row_id()
      tag_all(id, tags)
      id
    end
  end

  # Adds tags to a note, lowercased so "Work" and "work" are one tag. One
  # prepared statement is reused for every tag; INSERT OR IGNORE makes
  # re-adding a tag the note already has a no-op.
  def tag_all(id: Int, tags: Array[String])
    insert = @db.prepare("INSERT OR IGNORE INTO tags (note_id, name) VALUES (?, ?)")
    tags.each() do |tag| insert.execute([id, tag.downcase()]) end
    insert.close()
  end

  # The next three return whether a row was actually affected (execute returns
  # the number of rows changed), which is how callers tell "done" from
  # "there was nothing to do".
  def untag(id: Int, tag: String) -> Bool
    @db.execute("DELETE FROM tags WHERE note_id = ? AND name = ?", [id, tag.downcase()]) > 0
  end

  # Deleting a note also deletes its tags (ON DELETE CASCADE) and its search
  # index entry (the notes_ad trigger).
  def remove(id: Int) -> Bool
    @db.execute("DELETE FROM notes WHERE id = ?", [id]) > 0
  end

  # One note, or nil if there is no such id.
  def find(id: Int) -> Note | Nil
    rows = @db.query("SELECT * FROM notes WHERE id = ?", [id])
    return nil if rows.empty?()
    note_from(rows[0])
  end

  # Every note in id order, or only those carrying `tag` (nil means all).
  def list(tag: String | Nil) -> Array[Note]
    rows = if tag == nil
      @db.query("SELECT * FROM notes ORDER BY id")
    else
      @db.query("SELECT notes.* FROM notes JOIN tags ON tags.note_id = notes.id WHERE tags.name = ? ORDER BY notes.id", [tag.downcase()])
    end
    rows.map() do |row| note_from(row) end
  end

  # Best matches first (bm25), each with a snippet of the body around the
  # hit. A malformed query ("AND", unbalanced quotes) is FTS5's own error,
  # which is caught and re-raised as a NotesError so it is reported as bad
  # input. In the snippet() call, 1 is the body column, '[' and ']' wrap each
  # match, '...' marks elisions, and 8 is the approximate length in words.
  def search(query: String) -> Array[Hash]
    begin
      @db.query("SELECT notes.id AS id, notes.title AS title, snippet(notes_fts, 1, '[', ']', '...', 8) AS snippet FROM notes_fts JOIN notes ON notes.id = notes_fts.rowid WHERE notes_fts MATCH ? ORDER BY bm25(notes_fts), notes.id", [query])
    rescue error: SQLite3Error
      raise NotesError.new("bad search: #{error.message()}")
    end
  end

  # Reports: tags by popularity, and notes per month (the first 7 characters
  # of "YYYY-MM-DD" are "YYYY-MM").
  def tag_counts() -> Array[Hash]
    @db.query("SELECT name, COUNT(*) AS count FROM tags GROUP BY name ORDER BY count DESC, name")
  end

  def monthly_counts() -> Array[Hash]
    @db.query("SELECT substr(created, 1, 7) AS month, COUNT(*) AS count FROM notes GROUP BY month ORDER BY month")
  end

  # Total number of notes.
  def count() -> Int = @db.query("SELECT COUNT(*) AS n FROM notes")[0]["n"]

  # Every note in `entries` or none of them: the first invalid entry
  # raises, and the transaction rolls back whatever came before it.
  def import(entries: Array) -> Int
    transaction() do
      entries.each_with_index() do |entry, index|
        # Validate the entry's shape BEFORE inserting it. Messages use the
        # 1-based position so the user can find the entry in their file.
        raise NotesError.new("entry #{index + 1}: not an object") unless entry is Hash
        title = entry.fetch("title", nil)
        body = entry.fetch("body", "")
        tags = entry.fetch("tags", [])
        unless title is String && !title.strip().empty?()
          raise NotesError.new("entry #{index + 1}: needs a title")
        end
        raise NotesError.new("entry #{index + 1}: body must be text") unless body is String
        raise NotesError.new("entry #{index + 1}: tags must be a list") unless tags is Array

        # An entry may carry its own creation date; otherwise it is "now".
        @db.execute("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)",
          [title, body, entry.fetch("created", @now)])
        tag_all(@db.last_insert_row_id(), tags.map() do |tag| tag.to_s() end)
      end
      entries.length()
    end
  end

  # Builds a Note from a `notes` row, looking up its tags.
  def note_from(row: Hash) -> Note
    id = row["id"]
    tags = @db.query("SELECT name FROM tags WHERE note_id = ? ORDER BY name", [id]).map() do |tag| tag["name"] end
    Note.new(id, row["title"], row["body"], row["created"], tags)
  end
end
