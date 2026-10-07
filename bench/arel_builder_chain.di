# Arel builder-chain microbenchmark: a typed `Arel::Table` parameter feeds
# chained builder calls (`column().eq()`, `where().order().take()`), so every
# inner call's result is the receiver of the next one. The JIT only chains
# through a *declared* `-> Type` return, so this measures what the return
# annotations on Arel's builders buy (see docs/internal/jit-design.md).
#
# Usage (compare interpreter vs JIT; the work is identical):
#   ./build/diamond bench/arel_builder_chain.di
#   DIAMOND_JIT=1 DIAMOND_JIT_THRESHOLD=1 DIAMOND_TRACE_JIT=1 ./build/diamond bench/arel_builder_chain.di
require "../packages/arel/lib/arel"

def build(table: Arel::Table, id: Int, limit: Int)
  base = Arel::Query.for_table(table)
  filtered = base.where(table.column("id").eq(id))
  ordered = filtered.order(table.column("name").asc())
  ordered.take(limit).skip(id)
end

table = Arel.table("entries")
count = 0
i = 0
while i < 200000
  query = build(table, i, 10)
  count += query.projection_count()
  i += 1
end
puts(count)
