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
# The handler is deliberately request-scoped only (builds and discards a
# Hash/Array/String per request, nothing persisted across requests) --
# threads: N requires a zero-capture Callable, so there's no closing over
# mutable state here even if this wanted to simulate a longer-lived
# working set. What this does exercise well: steady young-object churn
# (exactly what a nursery is meant to reclaim cheaply) against a live set
# that grows as the process runs.

require "../../packages/gremlin/gremlin"

def build_items(seed)
  items = []
  index = 0
  while index < 20
    items.push({"id": seed + index, "value": (seed + index) * 3})
    index = index + 1
  end
  items
end

def handler(request)
  path = request["path"]
  method = request["method"]
  seed = path.length() * 7919
  items = build_items(seed)
  total = 0
  index = 0
  while index < items.length()
    total = total + items[index]["value"]
    index = index + 1
  end
  body = "method=#{method} path=#{path} items=#{items.length()} total=#{total}"
  [200, {"Content-Type": "text/plain"}, body]
end

def run()
  port = 19420
  threads = 4
  gremlin_serve(port, handler, threads: threads)
end
run()
