def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

def ensure_configured(context)
  if context["configured"] == nil
    User.configure(ActiveRecord::Repository.new(Arel.table("users"), build_user, "id", nil, build_user_validator(Database.get(context))))
    Session.configure(ActiveRecord::Repository.new(Arel.table("sessions"), build_session, "id"))
    Skin.configure(ActiveRecord::Repository.new(Arel.table("skins"), build_skin, "id", nil, build_skin_validator()))
    Tag.configure(ActiveRecord::Repository.new(Arel.table("tags"), build_tag, "id", nil, build_tag_validator()))
    Tagging.configure(ActiveRecord::Repository.new(Arel.table("taggings"), build_tagging, "id"))
    Comment.configure(ActiveRecord::Repository.new(Arel.table("comments"), build_comment, "id", nil, build_comment_validator()))
    # One shared StaticFiles root for both the framework's own CSS/JS
    # and everything under public/uploads/ (see boot.di's own comment
    # on why this is one root rather than two separately-configured
    # instances).
    StaticFiles.configure({"root": "./public"})
    context["configured"] = true
    AppLogger.get(context).info("app.configured", {"model_count": 6})
  end
end

def logging_middleware(request, context, forward)
  request["request_id"] = SecureRandom.hex(6)
  context["log_context"] = request_log_fields(request)
  start = Time.monotonic()
  log_info(request, context, "request.started")
  response = forward(request, context)
  elapsed_ms = (Time.monotonic() - start) * 1000
  log_info(request, context, "request.completed", {"status": response[0], "duration_ms": elapsed_ms})
  context["log_context"] = nil
  response
end

def app(request, context)
  ensure_configured(context)
  chain = rack_compose([StaticFiles.call, logging_middleware, load_current_user_middleware], route)
  rack_run_chain(chain, 0, request, context)
end
