# The endpoint sweep.di measures. SWEEP_PORT and SWEEP_THREADS say where to listen and with
# how many workers; SWEEP_WORKLOAD picks the handler:
#
#   mixed  a little work per request, deliberately not a pure busy loop (integer
#          arithmetic, string building, hash construction and JSON encoding: the shape of a
#          small API handler; roughly a few hundred microseconds). The default.
#   hello  a constant response, so the server's own overhead is all there is.
#   cpu    a ten times longer integer loop and almost no I/O.
require_cut "gremlin"

def mixed(request, context)
  sum = 0
  i = 0
  while i < 2000
    sum = sum + (i * i) % 7
    i = i + 1
  end
  items = []
  j = 0
  while j < 8
    items.push({"id": j, "label": "item-#{j}", "score": sum + j})
    j = j + 1
  end
  body = JSON.stringify({"path": request["path"], "total": sum, "items": items})
  [200, {"Content-Type": "application/json"}, body]
end

def hello(request, context) = [200, {"Content-Type": "text/plain"}, "hello"]

def cpu(request, context)
  sum = 0
  i = 0
  while i < 20000
    sum = sum + (i * i) % 7
    i = i + 1
  end
  [200, {"Content-Type": "text/plain"}, "#{sum}"]
end

workload = ENV["SWEEP_WORKLOAD"] || "mixed"
port = ENV["SWEEP_PORT"].to_i()
threads = ENV["SWEEP_THREADS"].to_i()
if workload == "hello"
  gremlin_serve(port, hello, threads: threads)
elsif workload == "cpu"
  gremlin_serve(port, cpu, threads: threads)
else
  gremlin_serve(port, mixed, threads: threads)
end
