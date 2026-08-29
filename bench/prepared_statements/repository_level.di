# Context, not a before/after comparison: the full ActiveRecord/Arel
# stack's own per-call cost (Query construction, SQLiteVisitor
# rendering, Hash-per-row mapping) around the same repeated
# Repository#find(db, id) call, which already goes through
# Arel::PreparedStatements automatically (see packages/arel/lib/arel/
# query.di's own Query#to_a). There's no "uncached Repository" variant
# left to compare against post-fix, so this exists to show how much of
# a real call's total cost is ORM overhead sitting on top of the raw
# SQL-layer numbers cached_query.di/uncached_query.di measure --
# reading the caching win off *this* number would understate it.
#
# Usage: ./build/diamond bench/prepared_statements/repository_level.di ITERATIONS
require "../../packages/active_record/lib/active_record"
require "./setup"

def build_item(row) = row

iterations = ARGV[0].to_i()
db = build_bench_db()
repo = ActiveRecord::Repository.new(Arel.table("items"), build_item, "id")

start = Time.monotonic()
index = 0
while index < iterations
  repo.find(db, mod(index, 200) + 1)
  index += 1
end
elapsed = Time.monotonic() - start

puts("repository_level iterations=#{iterations} elapsed=#{elapsed}s")
