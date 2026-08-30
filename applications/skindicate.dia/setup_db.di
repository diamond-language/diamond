# Recreates and seeds the Skindicate database. Destructive -- rerun
# only when you want the selected environment's seed state back.
require "./lib/config/environment"
db = SQLite3.open(SkindicateEnvironment.database_path())

db.execute("PRAGMA foreign_keys = ON")
db.execute("DROP TABLE IF EXISTS follows")
db.execute("DROP TABLE IF EXISTS active_discussion_karma_votes")
db.execute("DROP TABLE IF EXISTS active_discussion_signals")
db.execute("DROP TABLE IF EXISTS active_discussion_comments")
db.execute("DROP TABLE IF EXISTS active_discussions")
db.execute("DROP TABLE IF EXISTS entries")
db.execute("DROP TABLE IF EXISTS comments")
db.execute("DROP TABLE IF EXISTS taggings")
db.execute("DROP TABLE IF EXISTS tags")
db.execute("DROP TABLE IF EXISTS skins")
db.execute("DROP TABLE IF EXISTS sessions")
db.execute("DROP TABLE IF EXISTS users")

db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, username TEXT NOT NULL UNIQUE, password_digest TEXT NOT NULL, role TEXT NOT NULL DEFAULT 'user')")
db.execute("CREATE TABLE sessions (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, csrf_token TEXT NOT NULL, expires_at INTEGER NOT NULL, FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)")
db.execute([
  "CREATE TABLE skins (id INTEGER PRIMARY KEY,",
  "title TEXT NOT NULL, description TEXT NOT NULL, platform TEXT NOT NULL,",
  "preview_image_path TEXT, file_path TEXT NOT NULL, original_filename TEXT NOT NULL)"
].join(" "))
db.execute("CREATE TABLE comments (id INTEGER PRIMARY KEY, body TEXT NOT NULL)")
db.execute([
  "CREATE TABLE entries (id INTEGER PRIMARY KEY,",
  "ancestry TEXT, ancestry_depth INTEGER NOT NULL DEFAULT 0,",
  "user_id INTEGER NOT NULL, entryable_type TEXT NOT NULL, entryable_id INTEGER NOT NULL,",
  "created_at INTEGER NOT NULL, updated_at INTEGER,",
  "FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)"
].join(" "))
db.execute("CREATE TABLE tags (id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE)")
db.execute([
  "CREATE TABLE taggings (id INTEGER PRIMARY KEY, skin_id INTEGER NOT NULL,",
  "tag_id INTEGER NOT NULL, UNIQUE(skin_id, tag_id),",
  "FOREIGN KEY(skin_id) REFERENCES skins(id) ON DELETE CASCADE,",
  "FOREIGN KEY(tag_id) REFERENCES tags(id) ON DELETE CASCADE)"
].join(" "))
db.execute([
  "CREATE TABLE follows (id INTEGER PRIMARY KEY, follower_id INTEGER NOT NULL,",
  "followed_id INTEGER NOT NULL, created_at INTEGER NOT NULL,",
  "FOREIGN KEY(follower_id) REFERENCES users(id) ON DELETE CASCADE,",
  "FOREIGN KEY(followed_id) REFERENCES users(id) ON DELETE CASCADE)"
].join(" "))
db.execute("CREATE INDEX sessions_user_id_idx ON sessions(user_id)")
db.execute("CREATE INDEX taggings_skin_id_idx ON taggings(skin_id)")
db.execute("CREATE INDEX taggings_tag_id_idx ON taggings(tag_id)")
db.execute("CREATE UNIQUE INDEX follows_follower_followed_idx ON follows(follower_id, followed_id)")
db.execute("CREATE INDEX follows_followed_id_idx ON follows(followed_id)")
db.execute("CREATE INDEX entries_ancestry_idx ON entries(ancestry)")
db.execute("CREATE INDEX entries_user_id_idx ON entries(user_id)")
db.execute("CREATE UNIQUE INDEX entries_entryable_idx ON entries(entryable_type, entryable_id)")

password_digest = BCrypt.hash("diamond123", 12)
db.execute("INSERT INTO users (email, username, password_digest, role) VALUES (?, ?, ?, 'admin')", ["admin@example.com", "admin", password_digest])
user_id = db.last_insert_row_id()
db.execute("INSERT INTO skins (title, description, platform, file_path, original_filename) VALUES (?, ?, ?, ?, ?)",
  ["Midnight Blue Taskbar", "A dark, minimal taskbar reskin for Windows 11.", "windows", "uploads/seed-placeholder.zip", "midnight-blue.zip"])
skin_id = db.last_insert_row_id()
db.execute("INSERT INTO entries (ancestry, ancestry_depth, user_id, entryable_type, entryable_id, created_at) VALUES (NULL, 0, ?, 'Skin', ?, ?)",
  [user_id, skin_id, Time.now().to_i()])
skin_entry_id = db.last_insert_row_id()
db.execute("INSERT INTO tags (name) VALUES (?)", ["dark"])
dark_tag_id = db.last_insert_row_id()
db.execute("INSERT INTO tags (name) VALUES (?)", ["minimal"])
minimal_tag_id = db.last_insert_row_id()
db.execute("INSERT INTO taggings (skin_id, tag_id) VALUES (?, ?)", [skin_id, dark_tag_id])
db.execute("INSERT INTO taggings (skin_id, tag_id) VALUES (?, ?)", [skin_id, minimal_tag_id])
db.execute("INSERT INTO comments (body) VALUES (?)", ["Love this one, using it right now."])
comment_id = db.last_insert_row_id()
db.execute("INSERT INTO entries (ancestry, ancestry_depth, user_id, entryable_type, entryable_id, created_at) VALUES (?, 1, ?, 'Comment', ?, ?)",
  ["#{skin_entry_id}", user_id, comment_id, Time.now().to_i()])

puts(JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "skindicate", "message": "database.seeded", "database": SkindicateEnvironment.database_path(), "environment": SkindicateEnvironment.name()}))
db.close()
JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "skindicate", "message": "seed.credentials_created", "email": "admin@example.com", "environment": SkindicateEnvironment.name()})
