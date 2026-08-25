require "../../packages/gremlin/lib/gremlin"
require "./boot"

Logger.new("project_board", AppEnvironment.log_level(), nil, "json").info("server.listening", {"host": "127.0.0.1", "port": 18081, "environment": AppEnvironment.name(), "database": AppEnvironment.database_path()})
gremlin_serve(18081, app)
