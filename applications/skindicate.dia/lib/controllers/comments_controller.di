class CommentsController
  def self.create(request, context, params)
    db = Database.get(context)
    skin = Skin.find(db, params["id"].to_i())
    if skin == nil then return Dials::Response.not_found(request["path"]) end
    skin_entry = skin.entry(db)
    parent_entry = if params["parent_id"] == nil || params["parent_id"] == "" then skin_entry else Entry.find(db, params["parent_id"].to_i()) end
    # Replies are capped at one level deep, matching the UI (which only
    # ever renders "top-level comment + its replies"): a top-level
    # comment's parent is the skin's own root entry (ancestry_depth 0),
    # a reply's parent is a top-level comment that must itself be a
    # direct child of this same skin's entry (not a comment on some
    # other skin, and not already a reply -- ancestry_depth 2 or
    # deeper can't take another reply).
    parent_belongs_to_this_skin = parent_entry != nil &&
      (parent_entry.id() == skin_entry.id() || parent_entry.parent_id() == skin_entry.id())
    if !parent_belongs_to_this_skin
      log_warn(request, context, "comment.create_rejected", {"skin_id": skin.id(), "reason": "invalid or too-deep parent"})
      return Dials::Response.redirect("/skins/#{skin.id()}", "comment rejected")
    end
    comment = Comment.new({"body": params["body"]})
    begin
      create_entry!(db, "Comment", comment, context["current_user"].id(), parent_entry)
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "comment.create_rejected", {"skin_id": skin.id(), "validation_errors": error.errors()})
      return Dials::Response.redirect("/skins/#{skin.id()}", "comment rejected")
    end
    log_info(request, context, "comment.created", {"comment_id": comment.id(), "skin_id": skin.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "commented")
  end

  def self.destroy(request, context, params)
    db = Database.get(context)
    comment = context["current_comment"]
    comment_entry = comment.entry(db)
    skin_entry = Entry.find(db, comment_entry.parent_id())
    skin_id = skin_entry.entryable_id()
    comment_entry.destroy_subtree!(db)
    log_info(request, context, "comment.deleted", {"comment_id": comment.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin_id}", "comment deleted")
  end
end
