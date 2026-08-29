class CommentsController
  def self.create(request, context, params)
    db = Database.get(context)
    skin = Skin.find(db, params["id"].to_i())
    if skin == nil then return Dials::Response.not_found(request["path"]) end
    discussion = discussion_for_skin(db, skin.id())
    parent_id = if params["parent_id"] == nil || params["parent_id"] == "" then nil else params["parent_id"].to_i() end
    begin
      comment = discussion.create_comment!(db, context["current_user"].username(), params["body"], parent_id)
    rescue error: ActiveRecord::ValidationError | ActiveDiscussion::DiscussionLocked | ActiveDiscussion::CoolingDown | ActiveDiscussion::DepthExceeded | ActiveDiscussion::KarmaInsufficient
      log_warn(request, context, "comment.create_rejected", {"skin_id": skin.id(), "reason": error.message()})
      return Dials::Response.redirect("/skins/#{skin.id()}", "comment rejected")
    end
    log_info(request, context, "comment.created", {"comment_id": comment.id(), "skin_id": skin.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "commented")
  end

  def self.destroy(request, context, params)
    db = Database.get(context)
    comment = context["current_comment"]
    skin_id = comment.discussion(db).recording_id()
    comment.delete!(db)
    log_info(request, context, "comment.deleted", {"comment_id": comment.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin_id}", "comment deleted")
  end

  def self.report(request, context, params)
    db = Database.get(context)
    comment = ActiveDiscussion::Comment.find(db, params["id"].to_i())
    if comment == nil then return Dials::Response.not_found(request["path"]) end
    discussion = comment.discussion(db)
    discussion.signal!(db, comment.persona_handle(), type: ActiveDiscussion::Signal::REPORT, flagged_by: context["current_user"].username())
    log_info(request, context, "comment.reported", {"comment_id": comment.id(), "reported_by": context["current_user"].username()})
    Dials::Response.redirect("/skins/#{discussion.recording_id()}", "reported")
  end

  def self.lock(request, context, params)
    db = Database.get(context)
    skin = context["current_skin"]
    discussion = discussion_for_skin(db, skin.id())
    discussion.lock!(db, "locked by owner")
    log_info(request, context, "discussion.locked", {"skin_id": skin.id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "locked")
  end

  def self.unlock(request, context, params)
    db = Database.get(context)
    skin = context["current_skin"]
    discussion = discussion_for_skin(db, skin.id())
    discussion.unlock!(db)
    log_info(request, context, "discussion.unlocked", {"skin_id": skin.id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "unlocked")
  end
end
