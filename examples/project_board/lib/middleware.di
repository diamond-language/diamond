# The same per-worker setup as examples/library/lib/middleware.di (see there
# for why models must be configured per worker thread). Here it also logs.

def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

# Gives each model its Repository on this worker's first request: table, row
# builder, id column, and (for Project and Task) a validator. Task's validator
# gets this worker's connection because it queries the projects table.
def ensure_models_configured(context)
  if context["models_configured"] == nil
    User.configure(ActiveRecord::Repository.new(Arel.table("users"), build_user, "id"))
    Session.configure(ActiveRecord::Repository.new(Arel.table("sessions"), build_session, "id"))
    Project.configure(ActiveRecord::Repository.new(Arel.table("projects"), build_project, "id", nil, build_project_validator()))
    Task.configure(ActiveRecord::Repository.new(Arel.table("tasks"), build_task, "id", nil, build_task_validator(Database.get(context))))
    context["models_configured"] = true
    AppLogger.get(context).info("models.configured", {"model_count": 4})
  end
end

# Outermost middleware: gives the request an id, logs its start and end, and
# publishes the id/method/path as `log_context` so that logs emitted deeper
# down (including every SQL statement) carry it too.
def logging_middleware(request, context, forward)
  request["request_id"] = SecureRandom.hex(6)
  context["log_context"] = request_log_fields(request)
  start = Time.monotonic()
  log_info(request, context, "request.started")

  # Everything else (authentication, routing, the action) runs inside here.
  response = forward(request, context)

  elapsed_ms = (Time.monotonic() - start) * 1000
  log_info(request, context, "request.completed", {"status": response[0], "duration_ms": elapsed_ms})

  # Clear it so a stray log call between requests is not mislabeled.
  context["log_context"] = nil
  response
end

# The handler given to gremlin_serve: configure this worker's models if
# needed, then run the chain logging -> authentication -> router (outermost
# first).
def app(request, context)
  ensure_models_configured(context)
  chain = rack_compose([logging_middleware, load_current_user_middleware], route)
  rack_run_chain(chain, 0, request, context)
end
