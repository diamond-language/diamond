require "../../packages/gremlin/lib/gremlin"
require "./boot"

port = 18090
puts("guestbook listening on http://127.0.0.1:#{port}")
gremlin_serve(port, rack_app)
