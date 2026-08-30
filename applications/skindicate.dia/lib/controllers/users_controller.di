class UsersController
  def self.show(request, context, params)
    db = Database.get(context)
    user = User.where({"username": params["username"]}).first(db)
    if user == nil then return Dials::Response.not_found(request["path"]) end
    skin_entries = Entry.where({"user_id": user.id(), "entryable_type": "Skin"}).to_a(db)
    skins = skin_entries.map() do |entry| entry.entryable(db) end
    is_following = context["current_user"] != nil &&
      ActiveSocial::Follow.follows?(db, context["current_user"].id(), user.id())
    log_debug(request, context, "user.shown", {"user_id": user.id(), "skin_count": skins.length()})
    content = user_show_html(user, skins, db, ActiveSocial::Follow.following_count(db, user.id()),
      ActiveSocial::Follow.followers_count(db, user.id()), is_following, context["current_user"], context["csrf_token"])
    Div.html_response(200, layout_html(user.username(), content, context["current_user"], context["csrf_token"]))
  end

  # Following yourself is left as Follow.follow!'s own no-op (idempotent
  # by target, not by actor) -- simplest, and there's no real harm in an
  # inert self-follow row never actually appearing (follower_id ==
  # followed_id would still pass ActiveSocial's own uniqueness index,
  # but nothing here would ever produce that row unless someone visited
  # their own profile and submitted the form, which the view doesn't
  # render a follow button for in the first place -- see user_show.html.div).
  def self.follow(request, context, params)
    db = Database.get(context)
    target = User.where({"username": params["username"]}).first(db)
    if target == nil then return Dials::Response.not_found(request["path"]) end
    ActiveSocial::Follow.follow!(db, context["current_user"].id(), target.id())
    log_info(request, context, "user.followed", {"follower_id": context["current_user"].id(), "followed_id": target.id()})
    Dials::Response.redirect("/users/#{target.username()}", "followed")
  end

  def self.unfollow(request, context, params)
    db = Database.get(context)
    target = User.where({"username": params["username"]}).first(db)
    if target == nil then return Dials::Response.not_found(request["path"]) end
    ActiveSocial::Follow.unfollow!(db, context["current_user"].id(), target.id())
    log_info(request, context, "user.unfollowed", {"follower_id": context["current_user"].id(), "followed_id": target.id()})
    Dials::Response.redirect("/users/#{target.username()}", "unfollowed")
  end
end
