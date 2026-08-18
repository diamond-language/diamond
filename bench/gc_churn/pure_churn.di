# Control case for session_churn.di in the same directory: identical
# per-iteration allocation shape (build_payload, copied verbatim from
# bench/burn_in/server.di) but with no persistent session cache at all --
# every iteration's payload is immediately garbage, nothing survives past
# the next collection. Contrasts against session_churn.di's large,
# mostly-stable live set to isolate whether GC cost is driven by *live*
# data size (the generational-GC hypothesis) or just raw allocation
# volume, which this file holds comparable while zeroing out the live set.
#
# Usage: DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/pure_churn.di ITERATIONS

def build_payload(seed)
  items = []
  index = 0
  while index < 20
    items.push({"id": seed + index, "value": (seed + index) * 3})
    index = index + 1
  end
  items
end

iterations = ARGV[0].to_i()

i = 0
while i < iterations
  build_payload(i)
  i = i + 1
end

puts("iterations=#{iterations}")
