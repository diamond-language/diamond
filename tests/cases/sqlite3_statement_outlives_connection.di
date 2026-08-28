# A Statement's lifetime is independent of its owning connection's own
# Diamond-level reachability -- #close on the connection while a Statement
# prepared from it is still open must defer the real close (sqlite3_close_v2)
# rather than silently leaking the connection or breaking the still-open
# Statement.
db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (id INTEGER)")
db.execute("INSERT INTO t (id) VALUES (1)")
db.execute("INSERT INTO t (id) VALUES (2)")

stmt = db.prepare("SELECT * FROM t WHERE id = ?")
db.close()  # connection is Diamond-level closed, but stmt keeps it alive

first = stmt.query([1])
second = stmt.query([2])
stmt.close()  # only now does the real connection actually close

closed_connection_rejected = false
begin
  db.query("SELECT 1")
rescue error: SQLite3Error
  closed_connection_rejected = true
end

[first, second, closed_connection_rejected]
