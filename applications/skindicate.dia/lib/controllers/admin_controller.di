def user_roles() = ["user", "admin"]

class AdminController
  def self.index(request, context, params)
    db = Database.get(context)
    users = User.all().order(Arel.table("users").column("id").asc()).to_a(db)
    log_debug(request, context, "admin.users_listed", {"count": users.length()})
    content = admin_users_html(users, context["current_user"], context["csrf_token"])
    Div.html_response(200, layout_html("Admin", content, context["current_user"], context["csrf_token"]))
  end

  # A target can't change their own role -- avoids an admin
  # accidentally demoting themselves (or promoting themselves back)
  # with no one else around to undo it.
  def self.update_role(request, context, params)
    db = Database.get(context)
    target = User.find(db, params["id"].to_i())
    if target == nil then return Dials::Response.not_found(request["path"]) end
    if target.id() == context["current_user"].id()
      log_warn(request, context, "admin.role_change_rejected", {"reason": "self", "user_id": target.id()})
      return Dials::Response.redirect("/admin", "cannot change your own role")
    end
    unless user_roles().include?(params["role"])
      log_warn(request, context, "admin.role_change_rejected", {"reason": "invalid_role", "user_id": target.id(), "role": params["role"]})
      return Dials::Response.redirect("/admin", "invalid role")
    end
    target.role = params["role"]
    target.save(db)
    log_info(request, context, "admin.role_changed", {"user_id": target.id(), "role": target.role(), "changed_by": context["current_user"].id()})
    Dials::Response.redirect("/admin", "role updated")
  end
end
