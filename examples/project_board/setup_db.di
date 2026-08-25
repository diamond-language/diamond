# Recreates and seeds the project board database.
require "./lib/config/environment"
db = SQLite3.open(AppEnvironment.database_path())

db.execute("PRAGMA foreign_keys = ON")
db.execute("DROP TABLE IF EXISTS sessions")
db.execute("DROP TABLE IF EXISTS tasks")
db.execute("DROP TABLE IF EXISTS projects")
db.execute("DROP TABLE IF EXISTS users")

db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, password_digest TEXT NOT NULL)")
db.execute("CREATE TABLE projects (id INTEGER PRIMARY KEY, name TEXT NOT NULL, description TEXT NOT NULL)")
db.execute("CREATE TABLE tasks (id INTEGER PRIMARY KEY, project_id INTEGER NOT NULL, title TEXT NOT NULL, done INTEGER NOT NULL DEFAULT 0, FOREIGN KEY(project_id) REFERENCES projects(id) ON DELETE CASCADE)")
db.execute("CREATE TABLE sessions (id INTEGER PRIMARY KEY, user_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, csrf_token TEXT NOT NULL, expires_at INTEGER NOT NULL, FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE)")

password_digest = BCrypt.hash("diamond123", 12)
db.execute("INSERT INTO users (email, password_digest) VALUES (?, ?)", ["admin@example.com", password_digest])
db.execute("INSERT INTO projects (name, description) VALUES (?, ?)", ["Diamond", "Build a small, expressive programming language."])
project_id = db.last_insert_row_id()
db.execute("INSERT INTO tasks (project_id, title, done) VALUES (?, ?, ?)", [project_id, "Document the example apps", 0])
db.execute("INSERT INTO tasks (project_id, title, done) VALUES (?, ?, ?)", [project_id, "Ship authenticated CRUD", 1])

puts(JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "project_board", "message": "database.seeded", "database": AppEnvironment.database_path(), "environment": AppEnvironment.name()}))
db.close()
JSON.stringify({"timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"), "level": "info", "tag": "project_board", "message": "seed.credentials_created", "email": "admin@example.com", "environment": AppEnvironment.name()})
