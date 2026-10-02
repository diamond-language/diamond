# Entry point. All the wiring is in boot.di (so smoke_test.di can load the
# same app without opening a socket); this file only announces the server and
# starts it. Run `bash compile_views.sh` and `setup_db.di` first.
require_cut "gremlin"
require "./boot"

# One structured JSON log line saying where we are listening and which
# environment/database were selected.
Logger.new("project_board", AppEnvironment.log_level(), nil, "json").info("server.listening", {"host": "127.0.0.1", "port": 18081, "environment": AppEnvironment.name(), "database": AppEnvironment.database_path()})

# Blocks, handing every request to `app` (lib/middleware.di).
gremlin_serve(18081, app)
