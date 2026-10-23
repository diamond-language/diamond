require_cut "gremlin"
require "./boot"

PheintLogger.build().info("server.listening", {
  "host": "127.0.0.1",
  "port": PheintEnvironment.port(),
  "environment": PheintEnvironment.name()
})
gremlin_serve(PheintEnvironment.port(), app)
