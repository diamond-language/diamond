require_cut "gremlin"
require "./boot"

# Entry point: serve the composed `rack_app` (built in boot.di) on a fixed
# local port. Everything interesting lives in boot.di so the smoke test can
# drive the app without opening a socket.
port = 18090

puts("guestbook listening on http://127.0.0.1:#{port}")
gremlin_serve(port, rack_app)
