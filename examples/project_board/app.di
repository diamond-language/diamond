require "../../packages/gremlin/lib/gremlin"
require "./boot"

Logger.new("project_board", "info", nil, "json").info("server.listening", {"host": "127.0.0.1", "port": 18081})
gremlin_serve(18081, app)
