require "./lib/config/environment"

db = SQLite3.open(PheintEnvironment.database_path())
db.execute("PRAGMA foreign_keys = ON")
db.execute("DROP TABLE IF EXISTS sessions")
db.execute("DROP TABLE IF EXISTS profiles")
db.execute("DROP TABLE IF EXISTS accounts")
db.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, password_digest TEXT NOT NULL)")
db.execute("CREATE TABLE profiles (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL UNIQUE, handle TEXT NOT NULL UNIQUE, FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE)")
db.execute("CREATE TABLE sessions (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, expires_at INTEGER NOT NULL, FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE)")
db.execute("CREATE INDEX sessions_account_id_idx ON sessions(account_id)")
db.execute("CREATE INDEX sessions_expires_at_idx ON sessions(expires_at)")
puts(JSON.stringify({
  "timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"),
  "level": "info", "tag": "pheint", "message": "database.initialized",
  "database": PheintEnvironment.database_path(),
  "environment": PheintEnvironment.name()
}))
db.close()
