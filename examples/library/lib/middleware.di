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

def app(request, context)
  chain = rack_compose([timing_middleware, logging_middleware], route)
  rack_run_chain(chain, 0, request, context)
end
