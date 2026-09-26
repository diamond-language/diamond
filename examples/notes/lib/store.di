# The notebook's SQLite database: schema migrations, notes and their tags,
# full-text search, and an all-or-nothing import.

struct Note(id: Int, title: String, body: String, created: String, tags: Array[String])
end

class NotesError < StandardError
end

# Each migration moves the schema up one version; PRAGMA user_version
# records how far a database has come, so opening an old file brings it
# up to date and opening a current one does nothing.
def migrations() -> Array[Array[String]]
  [
    [
      "CREATE TABLE notes (id INTEGER PRIMARY KEY, title TEXT NOT NULL, body TEXT NOT NULL, created TEXT NOT NULL)",
      "CREATE TABLE tags (note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE, name TEXT NOT NULL, PRIMARY KEY (note_id, name))"
    ],
    [
      "CREATE VIRTUAL TABLE notes_fts USING fts5(title, body, content='notes', content_rowid='id')",
      "CREATE TRIGGER notes_ai AFTER INSERT ON notes BEGIN INSERT INTO notes_fts (rowid, title, body) VALUES (new.id, new.title, new.body); END",
      "CREATE TRIGGER notes_ad AFTER DELETE ON notes BEGIN INSERT INTO notes_fts (notes_fts, rowid, title, body) VALUES ('delete', old.id, old.title, old.body); END",
      "INSERT INTO notes_fts (rowid, title, body) SELECT id, title, body FROM notes"
    ]
  ]
end

class NoteStore
  def initialize(path: String, now: String)
    @db = SQLite3.open(path)
    @now = now
    @in_transaction = false
    @db.execute("PRAGMA foreign_keys = ON")
    migrate()
  end

  def close()
    @db.close()
  end

  def version() -> Int = @db.query("PRAGMA user_version")[0]["user_version"]

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
    return body() if @in_transaction
    @db.execute("BEGIN")
    @in_transaction = true
    committed = false
    begin
      result = body()
      @db.execute("COMMIT")
      committed = true
      result
    ensure
      @in_transaction = false
      @db.execute("ROLLBACK") unless committed
    end
  end

  def add(title: String, body: String, tags: Array[String]) -> Int
    transaction() do
      @db.execute("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)", [title, body, @now])
      id = @db.last_insert_row_id()
      tag_all(id, tags)
      id
    end
  end

  def tag_all(id: Int, tags: Array[String])
    insert = @db.prepare("INSERT OR IGNORE INTO tags (note_id, name) VALUES (?, ?)")
    tags.each() do |tag| insert.execute([id, tag.downcase()]) end
    insert.close()
  end

  def untag(id: Int, tag: String) -> Bool
    @db.execute("DELETE FROM tags WHERE note_id = ? AND name = ?", [id, tag.downcase()]) > 0
  end

  def remove(id: Int) -> Bool
    @db.execute("DELETE FROM notes WHERE id = ?", [id]) > 0
  end

  def find(id: Int) -> Note | Nil
    rows = @db.query("SELECT * FROM notes WHERE id = ?", [id])
    return nil if rows.empty?()
    note_from(rows[0])
  end

  def list(tag: String | Nil) -> Array[Note]
    rows = if tag == nil
      @db.query("SELECT * FROM notes ORDER BY id")
    else
      @db.query("SELECT notes.* FROM notes JOIN tags ON tags.note_id = notes.id WHERE tags.name = ? ORDER BY notes.id", [tag.downcase()])
    end
    rows.map() do |row| note_from(row) end
  end

  # Best matches first (bm25), each with a snippet of the body around the
  # hit. A malformed query ("AND", unbalanced quotes) is FTS5's own error.
  def search(query: String) -> Array[Hash]
    begin
      @db.query("SELECT notes.id AS id, notes.title AS title, snippet(notes_fts, 1, '[', ']', '...', 8) AS snippet FROM notes_fts JOIN notes ON notes.id = notes_fts.rowid WHERE notes_fts MATCH ? ORDER BY bm25(notes_fts), notes.id", [query])
    rescue error: SQLite3Error
      raise NotesError.new("bad search: #{error.message()}")
    end
  end

  def tag_counts() -> Array[Hash]
    @db.query("SELECT name, COUNT(*) AS count FROM tags GROUP BY name ORDER BY count DESC, name")
  end

  def monthly_counts() -> Array[Hash]
    @db.query("SELECT substr(created, 1, 7) AS month, COUNT(*) AS count FROM notes GROUP BY month ORDER BY month")
  end

  def count() -> Int = @db.query("SELECT COUNT(*) AS n FROM notes")[0]["n"]

  # Every note in `entries` or none of them: the first invalid entry
  # raises, and the transaction rolls back whatever came before it.
  def import(entries: Array) -> Int
    transaction() do
      entries.each_with_index() do |entry, index|
        raise NotesError.new("entry #{index + 1}: not an object") unless entry is Hash
        title = entry.fetch("title", nil)
        body = entry.fetch("body", "")
        tags = entry.fetch("tags", [])
        unless title is String && !title.strip().empty?()
          raise NotesError.new("entry #{index + 1}: needs a title")
        end
        raise NotesError.new("entry #{index + 1}: body must be text") unless body is String
        raise NotesError.new("entry #{index + 1}: tags must be a list") unless tags is Array
        @db.execute("INSERT INTO notes (title, body, created) VALUES (?, ?, ?)",
          [title, body, entry.fetch("created", @now)])
        tag_all(@db.last_insert_row_id(), tags.map() do |tag| tag.to_s() end)
      end
      entries.length()
    end
  end

  def note_from(row: Hash) -> Note
    id = row["id"]
    tags = @db.query("SELECT name FROM tags WHERE note_id = ? ORDER BY name", [id]).map() do |tag| tag["name"] end
    Note.new(id, row["title"], row["body"], row["created"], tags)
  end
end
