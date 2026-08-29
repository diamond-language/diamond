# Same workload as uncached_query.di, but through
# Arel::PreparedStatements.for instead of a raw SQLite3#query call --
# the one statement shape gets prepared exactly once (on the first
# iteration) and every later call reuses that same compiled Statement,
# just binding the new parameter and stepping it.
#
# Usage: ./build/diamond bench/prepared_statements/cached_query.di ITERATIONS
require "../../packages/arel/lib/arel"
require "./setup"

iterations = ARGV[0].to_i()
db = build_bench_db()
sql = "SELECT * FROM items WHERE id = ?"

start = Time.monotonic()
index = 0
while index < iterations
  Arel::PreparedStatements.for(db, sql).query([mod(index, 200) + 1])
  index += 1
end
elapsed = Time.monotonic() - start

puts("cached iterations=#{iterations} elapsed=#{elapsed}s")
