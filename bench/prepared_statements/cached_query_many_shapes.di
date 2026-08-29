# Same cached path as cached_query.di, but rotating through
# SHAPE_COUNT distinct SQL strings (a trailing SQL comment makes each
# one textually unique, so each gets its own cache entry/prepared
# Statement) instead of just one -- checks that Arel::PreparedStatements
# still wins once a real app's own small-but-nonzero set of distinct
# query shapes is in play, not just the single-statement best case
# cached_query.di measures.
#
# Usage: ./build/diamond bench/prepared_statements/cached_query_many_shapes.di ITERATIONS SHAPE_COUNT
require "../../packages/arel/lib/arel"
require "./setup"

iterations = ARGV[0].to_i()
shape_count = ARGV[1].to_i()
db = build_bench_db()

shapes = []
shape_index = 0
while shape_index < shape_count
  shapes.push("SELECT * FROM items WHERE id = ? -- shape #{shape_index}")
  shape_index += 1
end

start = Time.monotonic()
index = 0
while index < iterations
  sql = shapes[mod(index, shape_count)]
  Arel::PreparedStatements.for(db, sql).query([mod(index, 200) + 1])
  index += 1
end
elapsed = Time.monotonic() - start

puts("cached_many_shapes iterations=#{iterations} shape_count=#{shape_count} elapsed=#{elapsed}s")
