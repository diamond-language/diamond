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
db.execute("DROP TABLE IF EXISTS comments")
db.execute("DROP TABLE IF EXISTS taggings")
db.execute("DROP TABLE IF EXISTS tags")
db.execute("DROP TABLE IF EXISTS skins")
db.execute("DROP TABLE IF EXISTS sessions")
db.execute("DROP TABLE IF EXISTS users")

db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, username TEXT NOT NULL UNIQUE, password_digest TEXT NOT NULL)")
db.execute("CREATE TABLE sessions (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, csrf_token TEXT NOT NULL, expires_at INTEGER NOT NULL, FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)")
db.execute([
  "CREATE TABLE skins (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL,",
  "title TEXT NOT NULL, description TEXT NOT NULL, platform TEXT NOT NULL,",
  "preview_image_path TEXT, file_path TEXT NOT NULL, original_filename TEXT NOT NULL,",
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
db.execute([
  "CREATE TABLE active_discussions (id INTEGER PRIMARY KEY,",
  "recording_id TEXT NOT NULL UNIQUE, persona_handle TEXT, title TEXT, body TEXT,",
  "karma INTEGER NOT NULL, max_depth INTEGER, karma_floor TEXT,",
  "locked INTEGER NOT NULL DEFAULT 0, locked_at INTEGER, locked_reason TEXT,",
  "cooldown_until INTEGER)"
].join(" "))
db.execute([
  "CREATE TABLE active_discussion_comments (id INTEGER PRIMARY KEY,",
  "discussion_id INTEGER NOT NULL, parent_id INTEGER,",
  "persona_handle TEXT NOT NULL, body TEXT NOT NULL,",
  "karma INTEGER NOT NULL, depth INTEGER NOT NULL,",
  "edited_at INTEGER, created_at INTEGER NOT NULL, updated_at INTEGER)"
].join(" "))
db.execute([
  "CREATE TABLE active_discussion_signals (id INTEGER PRIMARY KEY,",
  "discussion_id INTEGER NOT NULL, persona_handle TEXT NOT NULL,",
  "signal_type TEXT NOT NULL, flagged_by TEXT, created_at INTEGER NOT NULL)"
].join(" "))
db.execute([
  "CREATE TABLE active_discussion_karma_votes (id INTEGER PRIMARY KEY,",
  "target_type TEXT NOT NULL, target_id INTEGER NOT NULL, voter_handle TEXT NOT NULL,",
  "value INTEGER NOT NULL, applied_delta INTEGER NOT NULL)"
].join(" "))
db.execute("CREATE INDEX sessions_user_id_idx ON sessions(user_id)")
db.execute("CREATE INDEX skins_user_id_idx ON skins(user_id)")
db.execute("CREATE INDEX taggings_skin_id_idx ON taggings(skin_id)")
db.execute("CREATE INDEX taggings_tag_id_idx ON taggings(tag_id)")
db.execute("CREATE UNIQUE INDEX follows_follower_followed_idx ON follows(follower_id, followed_id)")
db.execute("CREATE INDEX follows_followed_id_idx ON follows(followed_id)")
db.execute("CREATE INDEX active_discussion_comments_discussion_id_idx ON active_discussion_comments(discussion_id)")
db.execute("CREATE INDEX active_discussion_comments_parent_id_idx ON active_discussion_comments(parent_id)")
db.execute("CREATE INDEX active_discussion_signals_discussion_id_idx ON active_discussion_signals(discussion_id)")
db.execute("CREATE UNIQUE INDEX active_discussion_karma_votes_target_voter_idx ON active_discussion_karma_votes(target_type, target_id, voter_handle)")

password_digest = BCrypt.hash("diamond123", 12)
db.execute("INSERT INTO users (email, username, password_digest) VALUES (?, ?, ?)", ["admin@example.com", "admin", password_digest])
user_id = db.last_insert_row_id()
db.execute("INSERT INTO skins (user_id, title, description, platform, file_path, original_filename) VALUES (?, ?, ?, ?, ?, ?)",
  [user_id, "Midnight Blue Taskbar", "A dark, minimal taskbar reskin for Windows 11.", "windows", "uploads/seed-placeholder.zip", "midnight-blue.zip"])
skin_id = db.last_insert_row_id()
db.execute("INSERT INTO tags (name) VALUES (?)", ["dark"])
dark_tag_id = db.last_insert_row_id()
db.execute("INSERT INTO tags (name) VALUES (?)", ["minimal"])
minimal_tag_id = db.last_insert_row_id()
db.execute("INSERT INTO taggings (skin_id, tag_id) VALUES (?, ?)", [skin_id, dark_tag_id])
db.execute("INSERT INTO taggings (skin_id, tag_id) VALUES (?, ?)", [skin_id, minimal_tag_id])
db.execute("INSERT INTO active_discussions (recording_id, karma) VALUES (?, 0)", ["#{skin_id}"])
discussion_id = db.last_insert_row_id()
db.execute([
  "INSERT INTO active_discussion_comments",
  "(discussion_id, parent_id, persona_handle, body, karma, depth, created_at)",
  "VALUES (?, NULL, ?, ?, 0, 0, ?)"
].join(" "),
  [discussion_id, "admin", "Love this one, using it right now.", Time.now().to_i()])

puts(JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "skindicate", "message": "database.seeded", "database": SkindicateEnvironment.database_path(), "environment": SkindicateEnvironment.name()}))
db.close()
JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "skindicate", "message": "seed.credentials_created", "email": "admin@example.com", "environment": SkindicateEnvironment.name()})
