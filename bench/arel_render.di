# Isolated Arel render-path microbenchmark -- measures Query#to_a's own
# to_sql-rendering cost directly, in a tight loop, with no DB I/O, HTTP,
# or template rendering in the way. Built to sanity-check a controlled
# A/B on skindicate.dia's own "/" route (git stash isolating the
# SQLiteVisitor-reuse fix in packages/arel/lib/arel/query.di) that came
# back within noise (~27.0ms -> ~26.7ms across 40 real HTTP requests) --
# that round trip has enough DB/template/network noise to plausibly mask
# a real few-percent difference in the render path alone. This isolates
# just the render path to get a more sensitive read before deciding
# whether further Arel-level allocation/dispatch reduction is worth it.
#
# Query shape mirrors skindicate.dia's own Skin.uploaders_for (skin.di):
# two chained .where() calls, one plain Hash-shorthand predicate and one
# real Attribute#in_list -- not a synthetic toy shape.
#
# Usage (compare by hand, no DIAMOND_JIT involved -- render()'s own
# parameters/locals don't satisfy Phase 9/10's JIT eligibility rules
# either way, confirmed by DIAMOND_TRACE_JIT showing 0 compiled either
# run, so this measures pure interpreter dispatch/allocation cost):
#   ./build/diamond bench/arel_render.di
require "../packages/arel/lib/arel"

t = Arel.table("entries")
ids = []
i = 0
while i < 100
  ids.push(i)
  i += 1
end

base_query = Arel::Query.for_table(t)
filtered_query = base_query.where({"entryable_type": "Skin"})
query = filtered_query.where(t.column("entryable_id").in_list(ids))

iterations = 20000
j = 0
while j < iterations
  sql, params = query.to_sql()
  j += 1
end
puts(sql)
puts(params.length())
