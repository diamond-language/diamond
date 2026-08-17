db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (x INTEGER)")

count_mismatch = false
begin
  db.execute("INSERT INTO t VALUES (?)", [1, 2])
rescue error: ArgumentError
  count_mismatch = true
end

bad_type = false
begin
  db.execute("INSERT INTO t VALUES (?)", [[1, 2]])
rescue error: TypeError
  bad_type = true
end

[count_mismatch, bad_type]
