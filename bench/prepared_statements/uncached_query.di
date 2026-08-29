# Baseline: the pre-Arel::PreparedStatements behavior still available
# via SQLite3#query directly -- prepares, binds, steps, and finalizes
# the same SQL statement fresh on every single call (sqlite3_prepare_v2's
# full parse/plan cost paid every time), even though it's the exact same
# statement shape every time. See cached_query.di for the same workload
# through Arel::PreparedStatements.for instead.
#
# Usage: ./build/diamond bench/prepared_statements/uncached_query.di ITERATIONS
require "./setup"

iterations = ARGV[0].to_i()
db = build_bench_db()
sql = "SELECT * FROM items WHERE id = ?"

start = Time.monotonic()
index = 0
while index < iterations
  db.query(sql, [mod(index, 200) + 1])
  index += 1
end
elapsed = Time.monotonic() - start

puts("uncached iterations=#{iterations} elapsed=#{elapsed}s")
