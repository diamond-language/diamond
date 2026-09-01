def route(request, context) = Dials::RouterHolder.get(build_router).dispatch(request, context)

def ensure_configured(context)
  if context["configured"] == nil
    RequestLogging.configure(tag: "skindicate", level: SkindicateEnvironment.log_level(), format: "json")
    User.configure(ActiveRecord::Repository.new(Arel.table("users"), build_user, "id", nil, build_user_validator(Database.get(context))))
    Session.configure(ActiveRecord::Repository.new(Arel.table("sessions"), build_session, "id"))
    Skin.configure(ActiveRecord::Repository.new(Arel.table("skins"), build_skin, "id", nil, build_skin_validator()))
    Comment.configure(ActiveRecord::Repository.new(Arel.table("comments"), build_comment, "id", nil, build_comment_validator()))
    Entry.configure(ActiveRecord::Repository.new(Arel.table("entries"), build_entry, "id"))
    ActiveTagging::Tag.configure(ActiveRecord::Repository.new(Arel.table("tags"), build_active_tagging_tag, "id", nil, build_active_tagging_tag_validator()))
    ActiveTagging::Tagging.configure(ActiveRecord::Repository.new(Arel.table("taggings"), build_active_tagging_tagging, "id"))
    ActiveSocial::Follow.configure(ActiveRecord::Repository.new(Arel.table("follows"), build_follow, "id"))
    # One shared StaticFiles root for both the framework's own CSS/JS
    # and everything under public/uploads/ (see boot.di's own comment
    # on why this is one root rather than two separately-configured
    # instances).
    StaticFiles.configure({"root": "./public"})
    context["configured"] = true
    RequestLogging.get(context).info("app.configured", {"model_count": 7})
  end
end

def app(request, context)
  ensure_configured(context)
  chain = rack_compose([StaticFiles.call, RequestLogging.call, load_current_user_middleware], route)
  rack_run_chain(chain, 0, request, context)
end
