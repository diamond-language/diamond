class SessionsController
  def self.session_lifetime_seconds() = 28800

  def self.start_session(context, user, redirect_to)
    token = SecureRandom.hex(32)
    csrf_token = SecureRandom.hex(32)
    lifetime = SessionsController.session_lifetime_seconds()
    expires_at = Time.now().to_i() + lifetime
    Session.create(Database.get(context), {"user_id": user.id(), "token": token, "csrf_token": csrf_token, "expires_at": expires_at})
    context["csrf_token"] = csrf_token
    cookie_value = SignedCookies.sign(token, SkindicateEnvironment.session_secret())
    [302, {"Location": redirect_to, "Set-Cookie": cookie_serialize("session_token", cookie_value, {"path": "/", "http_only": true, "same_site": "Lax", "max_age": lifetime})}, "signed in"]
  end

  def self.new_signup_form(request, context, params)
    log_debug(request, context, "signup.form_rendered")
    Div.html_response(200, layout_html("Sign up", signup_form_html([], "", ""), context["current_user"], context["csrf_token"]))
  end

  def self.create_signup(request, context, params)
    db = Database.get(context)
    # The very first account on a fresh Skindicate install becomes an
    # admin automatically -- there's no other way to reach the admin
    # panel otherwise (no separate seed/CLI flow), and this only ever
    # fires once (every subsequent signup finds a non-empty table).
    role = if User.all().count(db) == 0 then "admin" else "user" end
    attributes = {"email": params["email"], "username": params["username"], "password_digest": BCrypt.hash(if params["password"] == nil then "" else params["password"] end, 12), "role": role}
    user = User.new(attributes)
    begin
      user.save(db)
    rescue error: ActiveRecord::ValidationError
      log_warn(request, context, "signup.rejected", {"validation_errors": error.errors()})
      return Div.html_response(422, layout_html("Sign up", signup_form_html(error.errors(), params["email"], params["username"]), nil, nil))
    end
    log_info(request, context, "signup.succeeded", {"user_id": user.id(), "role": role})
    SessionsController.start_session(context, user, "/")
  end

  def self.new_form(request, context, params)
    log_debug(request, context, "login.form_rendered")
    Div.html_response(200, layout_html("Sign in", login_form_html(nil), context["current_user"], context["csrf_token"]))
  end

  def self.create(request, context, params)
    db = Database.get(context)
    user = User.where({"email": params["email"]}).first(db)
    if user == nil || !user.authenticate(params["password"])
      log_warn(request, context, "login.failed", {"email": params["email"]})
      return Div.html_response(401, layout_html("Sign in", login_form_html("Invalid email or password"), nil, nil))
    end
    log_info(request, context, "login.succeeded", {"user_id": user.id()})
    SessionsController.start_session(context, user, "/")
  end

  def self.destroy(request, context, params)
    session = context["current_session"]
    if session != nil
      session_id = session.id()
      session.destroy(Database.get(context))
      log_info(request, context, "logout.succeeded", {"session_id": session_id})
    else
      log_warn(request, context, "logout.session_not_found")
    end
    [302, {"Location": "/", "Set-Cookie": expired_session_cookie()}, "signed out"]
  end
end
