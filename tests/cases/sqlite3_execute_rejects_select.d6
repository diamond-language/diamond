db = SQLite3.open(":memory:")
db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, x INTEGER)")
db.execute("INSERT INTO t (x) VALUES (1)")

# Set-pragmas that echo their new value back as a one-row result are
# exempt -- they're a legitimate #execute use even though they look
# query-shaped.
db.execute("PRAGMA journal_mode = WAL")
db.execute("PRAGMA busy_timeout = 5000")
db.execute("PRAGMA foreign_keys = ON")

select_via_execute_rejected = false
begin
  db.execute("SELECT * FROM t")
rescue error: TypeError
  select_via_execute_rejected = true
end

stmt = db.prepare("SELECT * FROM t")
select_via_statement_execute_rejected = false
begin
  stmt.execute()
rescue error: TypeError
  select_via_statement_execute_rejected = true
end
stmt.close()

[select_via_execute_rejected, select_via_statement_execute_rejected]
