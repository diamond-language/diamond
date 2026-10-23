path = "/tmp/diamond_sqlite3_open_flags_test.db"

setup = SQLite3.open(path, "rwc")
setup.execute("DROP TABLE IF EXISTS t")
setup.execute("CREATE TABLE t (id INTEGER)")
setup.execute("INSERT INTO t (id) VALUES (1)")
setup.close()

readonly = SQLite3.open(path, "r")
rows = readonly.query("SELECT * FROM t")

readonly_write_rejected = false
begin
  readonly.execute("INSERT INTO t (id) VALUES (2)")
rescue error: SQLite3Error
  readonly_write_rejected = true
end
readonly.close()

missing_readonly_rejected = false
begin
  SQLite3.open("/tmp/diamond_sqlite3_open_flags_missing.db", "r")
rescue error: SQLite3Error
  missing_readonly_rejected = true
end

bad_mode_rejected = false
begin
  SQLite3.open(path, "bogus")
rescue error: TypeError
  bad_mode_rejected = true
end

non_string_mode_rejected = false
begin
  SQLite3.open(path, 5)
rescue error: TypeError
  non_string_mode_rejected = true
end

[rows, readonly_write_rejected, missing_readonly_rejected, bad_mode_rejected, non_string_mode_rejected]
