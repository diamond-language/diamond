db = SQLite3.open(":memory:")

guarded = false
begin
  db.execute("CREATE TABLE t(x); INSERT INTO t VALUES (1)")
rescue error: SQLite3Error
  guarded = true
end

[guarded]
