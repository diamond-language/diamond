def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

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

def logging_middleware(request, context, forward)
  request["request_id"] = SecureRandom.hex(6)
  start = Time.monotonic()
  log_info(request, context, "request.started")
  response = forward(request, context)
  elapsed_ms = to_i((Time.monotonic() - start) * 1000)
  log_info(request, context, "request.completed", {"status": response[0], "duration_ms": elapsed_ms})
  response
end

def app(request, context)
  ensure_models_configured(context)
  chain = rack_compose([logging_middleware, load_current_user_middleware], route)
  rack_run_chain(chain, 0, request, context)
end
