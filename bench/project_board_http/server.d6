require "../../packages/gremlin/lib/gremlin"
require "../../examples/project_board/boot"

# Each Gremlin worker owns an isolated context. Set its log level before the
# app can construct that worker's Logger; instrumentation and calls remain in
# place, and Logger rejects each event before JSON serialization or output.
def benchmark_app(request, context)
  context["log_level"] = "off"
  app(request, context)
end

server_threads = if ARGV.length() > 0 then ARGV[0].to_i() else 6 end
server_port = if ARGV.length() > 1 then ARGV[1].to_i() else 19620 end
gremlin_serve(server_port, benchmark_app, threads: server_threads)
