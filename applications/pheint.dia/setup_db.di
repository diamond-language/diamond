require "./lib/config/environment"

db = SQLite3.open(PheintEnvironment.database_path())
db.execute("PRAGMA foreign_keys = ON")
db.execute("DROP TABLE IF EXISTS scores")
db.execute("DROP TABLE IF EXISTS leaderboards")
db.execute("DROP TABLE IF EXISTS games")
db.execute("DROP TABLE IF EXISTS sessions")
db.execute("DROP TABLE IF EXISTS players")
db.execute("DROP TABLE IF EXISTS accounts")
db.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, password_digest TEXT NOT NULL)")
db.execute("CREATE TABLE players (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL UNIQUE, handle TEXT NOT NULL UNIQUE, FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE)")
db.execute("CREATE TABLE sessions (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, expires_at INTEGER NOT NULL, FOREIGN KEY(account_id) REFERENCES accounts(id) ON DELETE CASCADE)")
db.execute("CREATE TABLE games (id INTEGER PRIMARY KEY, owner_id INTEGER NOT NULL, title TEXT NOT NULL, description TEXT NOT NULL, FOREIGN KEY(owner_id) REFERENCES accounts(id) ON DELETE CASCADE)")
db.execute("CREATE TABLE leaderboards (id INTEGER PRIMARY KEY, game_id INTEGER NOT NULL, name TEXT NOT NULL, higher_is_better INTEGER NOT NULL, FOREIGN KEY(game_id) REFERENCES games(id) ON DELETE CASCADE)")
db.execute([
  "CREATE TABLE scores (id INTEGER PRIMARY KEY,",
  "leaderboard_id INTEGER NOT NULL, player_id INTEGER NOT NULL,",
  "value INTEGER NOT NULL,",
  "FOREIGN KEY(leaderboard_id) REFERENCES leaderboards(id) ON DELETE CASCADE,",
  "FOREIGN KEY(player_id) REFERENCES players(id) ON DELETE CASCADE,",
  "UNIQUE(leaderboard_id, player_id))"
].join(" "))
db.execute("CREATE INDEX sessions_account_id_idx ON sessions(account_id)")
db.execute("CREATE INDEX sessions_expires_at_idx ON sessions(expires_at)")
db.execute("CREATE INDEX games_owner_id_idx ON games(owner_id)")
db.execute("CREATE INDEX leaderboards_game_id_idx ON leaderboards(game_id)")
db.execute("CREATE INDEX scores_player_id_idx ON scores(player_id)")

password_cost = if PheintEnvironment.name() == "test" then 4 else 12 end
password_digest = BCrypt.hash("diamond123", password_cost)
db.execute("INSERT INTO accounts (email, password_digest) VALUES (?, ?)",
  ["demo@pheint.dia", password_digest])
account_id = db.last_insert_row_id()
db.execute("INSERT INTO players (account_id, handle) VALUES (?, ?)",
  [account_id, "demo"])
player_id = db.last_insert_row_id()
db.execute("INSERT INTO games (owner_id, title, description) VALUES (?, ?, ?)",
  [account_id, "Asteroid Run", "Pilot through an increasingly dense asteroid field."])
asteroid_game_id = db.last_insert_row_id()
db.execute("INSERT INTO leaderboards (game_id, name, higher_is_better) VALUES (?, ?, ?)",
  [asteroid_game_id, "All-time high score", true])
asteroid_board_id = db.last_insert_row_id()
db.execute("INSERT INTO scores (leaderboard_id, player_id, value) VALUES (?, ?, ?)",
  [asteroid_board_id, player_id, 128400])
db.execute("INSERT INTO games (owner_id, title, description) VALUES (?, ?, ?)",
  [account_id, "Cipher Sprint", "Solve a sequence of ciphers against the clock."])
cipher_game_id = db.last_insert_row_id()
db.execute("INSERT INTO leaderboards (game_id, name, higher_is_better) VALUES (?, ?, ?)",
  [cipher_game_id, "Fastest solvers", false])
puts(JSON.stringify({
  "timestamp": Time.now().strftime("%Y-%m-%dT%H:%M:%S%z"),
  "level": "info", "tag": "pheint", "message": "database.initialized",
  "database": PheintEnvironment.database_path(),
  "environment": PheintEnvironment.name()
}))
db.close()
