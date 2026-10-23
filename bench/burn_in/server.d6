# Long-running gremlin_serve burn-in target -- see bench/burn_in/README.md.
#
# This is the workload docs/roadmap.md's "Generational or incremental GC"
# entry and docs/gc-generational-design.md both point at: a real,
# sustained-load, multi-threaded gremlin server, standing in for the
# fiber-per-connection HTTP server shape the roadmap flags as the actual
# motivating case for a generational collector. `threads: N` (see
# packages/gremlin/README.md) gives each worker its own independent
# DiamondVm/heap on its own OS thread, so this exercises N collectors
# running under load concurrently, not just one.
#
# Two allocation shapes run on every request:
#   - request-scoped churn (build_payload): a Hash/Array built and
#     discarded per request -- exactly what a nursery is meant to
#     reclaim cheaply.
#   - a persistent, bounded session cache (see touch_session/`context`
#     below): a large, mostly-stable working set that keeps getting
#     mutated in place -- the shape the roadmap entry is actually
#     worried about, and the one the first version of this file
#     couldn't produce at all (see "Per-worker session cache").

require "../../packages/gremlin/gremlin"

def build_payload(seed)
  items = []
  index = 0
  while index < 20
    items.push({"id": seed + index, "value": (seed + index) * 3})
    index = index + 1
  end
  items
end

# ## Per-worker session cache
#
# `context` (gremlin_worker's per-worker Hash, see packages/gremlin's own
# "Per-worker context" section) holds `sessions`, a Hash of up to
# MAX_SESSIONS entries that lives for this worker's entire lifetime --
# the large, mostly-stable live set docs/gc-generational-design.md's
# minor-collection section is written against, as opposed to
# build_payload's own pure request-scoped churn above.
#
# `next_id` cycles through 0..MAX_SESSIONS-1 (no modulo operator in
# Diamond yet, hence the manual wraparound) rather than growing without
# bound, modeling a real session store's fixed concurrent-user ceiling
# instead of an unbounded leak: live-set *size* stays flat once warmed
# up, but every touch still replaces that session's entire Hash entry
# wholesale -- so `sessions` (an old-generation object for the entire
# run after its first few requests) keeps having fresh young Hash/Array
# values written into it on every single request. That's exactly the
# old-to-young write a generational collector's write barrier has to
# catch (see gc-generational-design.md's "Write barrier" section) --
# this workload doesn't just grow the live set once at startup and sit
# idle, it mutates it continuously under load.
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

def handler(request, context)
  if context["sessions"] == nil
    context["sessions"] = {}
    context["next_id"] = 0
  end
  sessions = context["sessions"]

  session_id = context["next_id"]
  next_id = session_id + 1
  max_sessions = 20000
  if next_id >= max_sessions
    next_id = 0
  end
  context["next_id"] = next_id

  path = request["path"]
  hits = touch_session(sessions, session_id, path)

  body = "path=#{path} session=#{session_id} hits=#{hits} sessions_live=#{sessions.length()}"
  [200, {"Content-Type": "text/plain"}, body]
end

def run()
  port = 19420
  threads = 4
  gremlin_serve(port, handler, threads: threads)
end
run()
