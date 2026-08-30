require "../lib/database_config"

config = DatabaseConfig.load("test/fixtures.json", "test")
if DatabaseConfig.adapter(config) != "sqlite3" then raise "wrong adapter" end

db = DatabaseConfig.open(config)
db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT)")
db.execute("INSERT INTO items (id, name) VALUES (?, ?)", [1, "configured"])
if db.query("SELECT name FROM items WHERE id = ?", [1])[0]["name"] != "configured"
  raise "configured SQLite connection failed"
end
db.close()

pg = DatabaseConfig.load("test/fixtures.json", "postgres")
connection = DatabaseConfig.postgresql_connection(pg)
if !connection.include?("port=55432") then raise "PostgreSQL port missing" end
if !connection.include?("dbname='diamond test'") then raise "PostgreSQL database missing" end

missing = nil
begin
  DatabaseConfig.load("test/fixtures.json", "missing")
rescue error: ArgumentError
  missing = error.message()
end
if missing == nil then raise "missing environment did not fail" end

puts("database_config tests passed")
exit(0)
