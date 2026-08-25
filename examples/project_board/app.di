require "../../packages/gremlin/lib/gremlin"
require "./boot"

puts("listening on http://127.0.0.1:18081 (Ctrl-C to stop)")
gremlin_serve(18081, app)
