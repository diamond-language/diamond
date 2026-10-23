# A long SQLite query is cancelled with sqlite3_interrupt.
def crunch()
  db = SQLite3.open(":memory:")
  db.query("WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 4000000000) SELECT count(*) FROM c")
end

sup = Supervisor.new()
sup.add_child(crunch)
started = Time.monotonic()
puts(sup.stop(100))
puts(sup.alive?(0))
puts(Time.monotonic() - started < 10)
