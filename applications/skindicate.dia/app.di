require "../../packages/gremlin/lib/gremlin"
require "./boot"

Logger.new("skindicate", SkindicateEnvironment.log_level(), nil, "json").info("server.listening", {"host": "127.0.0.1", "port": 18110, "environment": SkindicateEnvironment.name(), "database": SkindicateEnvironment.database_path()})
gremlin_serve(18110, app)
