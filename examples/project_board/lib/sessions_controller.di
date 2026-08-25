class SessionsController
  def self.session_lifetime_seconds() = 28800

  def self.new_form(request, context, params)
    log_debug(request, context, "login form rendered")
    Div.html_response(200, layout_html("Sign in", login_form_html(nil), context["current_user"], context["csrf_token"]))
  end

  def self.create(request, context, params)
    db = Database.get(context)
    user = User.where({"email": params["email"]}).first(db)
    if user == nil || !user.authenticate(params["password"])
      log_warn(request, context, "login failed email=#{params["email"]}")
      return Div.html_response(401, layout_html("Sign in", login_form_html("Invalid email or password"), nil, nil))
    end
    token = SecureRandom.hex(32)
    csrf_token = SecureRandom.hex(32)
    lifetime = SessionsController.session_lifetime_seconds()
    expires_at = Time.now().to_i() + lifetime
    Session.create(db, {"user_id": user.id(), "token": token, "csrf_token": csrf_token, "expires_at": expires_at})
    context["csrf_token"] = csrf_token
    log_info(request, context, "login succeeded user_id=#{user.id()} session=created lifetime_seconds=#{lifetime}")
    [302, {"Location": "/projects", "Set-Cookie": "session_token=#{token}; Path=/; Max-Age=#{lifetime}; HttpOnly; SameSite=Lax"}, "signed in"]
  end

  def self.destroy(request, context, params)
    token = cookie_value(request, "session_token")
    if token != nil
      session = Session.where({"token": token}).first(Database.get(context))
      if session != nil
        session_id = session.id()
        session.destroy(Database.get(context))
        log_info(request, context, "logout succeeded session_id=#{session_id}")
      else
        log_warn(request, context, "logout session=not_found")
      end
    end
    [302, {"Location": "/", "Set-Cookie": "session_token=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax"}, "signed out"]
  end
end
