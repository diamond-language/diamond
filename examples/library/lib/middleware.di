# Plain top-level functions, not classes -- a real constraint, not a style
# choice: rack middleware and gremlin_serve's handler must be zero-capture
# Callables, and Diamond classes are compile-time metadata, not first-class
# runtime values (docs/roadmap.md's "Classes as ordinary runtime objects"
# section) -- there is no way to hand rack_compose a class or an instance
# in place of a bare, zero-capture `def` reference. `route`'s own
# Dials::RouterHolder.get(build_router) is exactly RackChain's own
# per-worker-memoized-singleton pattern, reused for the same reason
# here -- see packages/dials/README.md's "Wiring into rack/gremlin".
def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

def logging_middleware(request, context, forward)
  response = forward(request, context)
  puts("#{request["method"]} #{request["path"]} -> #{response[0]}")
  response
end

# Outermost in the chain (see app() below) so its timing covers every
# other middleware's own work too, not just route()'s.
def timing_middleware(request, context, forward)
  start = Time.monotonic()
  response = forward(request, context)
  elapsed_ms = (Time.monotonic() - start) * 1000
  rounded_ms = to_f(to_i(elapsed_ms * 100)) / 100.0
  puts("#{request["method"]} #{request["path"]} took #{rounded_ms}ms")
  response
end

# `Author.configure`/`Book.configure` set @@repository -- a class
# variable, and (like every other piece of per-worker state here --
# Database.get's own connection, Dials::RouterHolder's router)
# `gremlin_serve(threads: N)`'s N-1 spawned workers each get their own
# independent VM/heap (see docs/threads.md), so a class variable set
# *before* gremlin_serve is called only actually lands on whichever one
# worker runs inline -- the spawned ones start with @@repository still
# nil. Confirmed directly, not assumed: benchmarking this app at
# threads > 1 reproducibly hit "type error" on ~(N-1)/N of requests,
# traced to Author.repository() reading nil on 3 of 4 worker threads.
# Guarded on `context` (per-worker, freshly {} per gremlin_worker call)
# so this runs exactly once per worker, the same shape Database.get
# already uses for its own per-worker resource.
def ensure_models_configured(context)
  if context["models_configured"] == nil
    Author.configure(ActiveRecord::Repository.new(Arel.table("authors"), build_author, "id"))
    Book.configure(ActiveRecord::Repository.new(Arel.table("books"), build_book, "id"))
    context["models_configured"] = true
  end
end

def app(request, context)
  ensure_models_configured(context)
  chain = rack_compose([timing_middleware, logging_middleware], route)
  rack_run_chain(chain, 0, request, context)
end
