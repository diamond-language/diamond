# Shared fixture for every script in this directory: a small in-memory
# SQLite table, seeded once, that every benchmark queries repeatedly --
# the exact "same statement shape run many times" workload
# Arel::PreparedStatements (packages/arel/lib/arel/prepared_statements.di)
# targets. Kept as its own file (required, not copy-pasted) so every
# script here measures against an identical fixture.
def build_bench_db()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE items (id INTEGER PRIMARY KEY, name TEXT, price INTEGER)")
  index = 0
  while index < 200
    db.execute("INSERT INTO items (name, price) VALUES (?, ?)", ["item-#{index}", index * 3])
    index += 1
  end
  db
end
