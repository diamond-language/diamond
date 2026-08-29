class CommentsController
  def self.create(request, context, params)
    db = Database.get(context)
    skin = Skin.find(db, params["id"].to_i())
    if skin == nil then return Dials::Response.not_found(request["path"]) end
    comment = Comment.new({"skin_id": skin.id(), "user_id": context["current_user"].id(), "body": params["body"], "created_at": Time.now().to_i()})
    begin
      comment.save(db)
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "comment.create_rejected", {"skin_id": skin.id(), "validation_errors": error.errors()})
      return Dials::Response.redirect("/skins/#{skin.id()}", "comment rejected")
    end
    log_info(request, context, "comment.created", {"comment_id": comment.id(), "skin_id": skin.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin.id()}", "commented")
  end

  def self.destroy(request, context, params)
    comment = context["current_comment"]
    skin_id = comment.skin_id()
    comment.destroy(Database.get(context))
    log_info(request, context, "comment.deleted", {"comment_id": comment.id(), "user_id": context["current_user"].id()})
    Dials::Response.redirect("/skins/#{skin_id}", "comment deleted")
  end
end
