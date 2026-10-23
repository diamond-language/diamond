db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY)")
db.close()
db.close()

closed_query_failed = false
begin
  db.query("SELECT 1")
rescue error: SQLite3Error
  closed_query_failed = true
end

[closed_query_failed]
