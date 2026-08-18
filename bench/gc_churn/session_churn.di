# Short, non-networked, non-daemon reproducer for the write-heavy
# "large, mostly-stable live set" workload docs/roadmap.md's "Generational
# or incremental GC" entry cares about -- the same shape as
# bench/burn_in/server.di's session cache (build_payload/touch_session are
# copied verbatim from there), but run as a single-process, single-threaded
# loop instead of a gremlin_serve(threads: N) target driven by `ab`. That
# swap is the whole point: burn_in's own live pushes (see docs/roadmap.md)
# gave inconclusive, noisy RSS numbers because request-timing, OS
# scheduling, and page-cache behavior all get mixed in with the collector's
# own cost when the workload is a networked, multi-threaded daemon. This
# runs to completion and exits normally, so DIAMOND_TRACE_GC=1's ordinary
# print-at-exit path (src/run_source.c) reports direct collection
# count/total-time evidence with none of that confound.
#
# Usage: DIAMOND_TRACE_GC=1 ./build/diamond bench/gc_churn/session_churn.di LIVE_SET_SIZE ITERATIONS
#
# LIVE_SET_SIZE is the number of distinct session keys in rotation (the
# size of the persistent, mostly-stable live set) -- ITERATIONS is how many
# touch_session calls run in total (each one replaces one session's entire
# Hash entry wholesale, the old-generation-object-getting-a-fresh-young-
# value write a generational collector's write barrier exists for). Held
# fixed across a sweep of LIVE_SET_SIZE values, ITERATIONS measures whether
# per-collection cost scales with how much *live* data there is to walk
# (the stop-the-world mark-sweep hypothesis this investigation is actually
# testing) independent of churn volume.

def build_payload(seed)
  items = []
  index = 0
  while index < 20
    items.push({"id": seed + index, "value": (seed + index) * 3})
    index = index + 1
  end
  items
end

def touch_session(sessions, session_id, path)
  key = "s#{session_id}"
  existing = sessions[key]
  hits = 1
  if existing != nil
    hits = existing["hits"] + 1
  end
  sessions[key] = {"hits": hits, "last_path": path, "payload": build_payload(session_id)}
  hits
end

live_set_size = ARGV[0].to_i()
iterations = ARGV[1].to_i()

sessions = {}
next_id = 0
i = 0
while i < iterations
  touch_session(sessions, next_id, "/item/#{i}")
  next_id = (next_id + 1) % live_set_size
  i = i + 1
end

puts("live_set_size=#{live_set_size} iterations=#{iterations} sessions.length()=#{sessions.length()}")
