# Login and logout. The browser's only credential is a random `token` cookie
# that names a row in the `sessions` table (see helpers/auth.di).
class SessionsController
  # How long a login lasts: 8 hours.
  def self.session_lifetime_seconds() = 28800

  # GET /login.
  def self.new_form(request, context, params)
    log_debug(request, context, "login.form_rendered")
    Div.html_response(200, layout_html("Sign in", login_form_html(nil), context["current_user"], context["csrf_token"]))
  end

  # POST /login.
  def self.create(request, context, params)
    db = Database.get(context)
    user = User.where({"email": params["email"]}).first(db)

    # Unknown email and wrong password produce the SAME message, so the form
    # cannot be used to discover which emails have accounts. 401 (not a
    # redirect) so the page re-renders with the message.
    if user == nil || !user.authenticate(params["password"])
      log_warn(request, context, "login.failed", {"email": params["email"]})
      return Div.html_response(401, layout_html("Sign in", login_form_html("Invalid email or password"), nil, nil))
    end

    # Mint two independent random secrets: `token` is the login cookie,
    # `csrf_token` is embedded in forms (see require_csrf).
    token = SecureRandom.hex(32)
    csrf_token = SecureRandom.hex(32)
    lifetime = SessionsController.session_lifetime_seconds()
    expires_at = Time.now().to_i() + lifetime

    # Persist the session server-side, so it can be revoked by deleting it.
    Session.create(db, {"user_id": user.id(), "token": token, "csrf_token": csrf_token, "expires_at": expires_at})
    context["csrf_token"] = csrf_token
    log_info(request, context, "login.succeeded", {"user_id": user.id(), "lifetime_seconds": lifetime})

    # Redirect to the project list and set the cookie. HttpOnly hides it from
    # page scripts; SameSite=Lax stops it being sent on cross-site POSTs;
    # Max-Age matches the server-side expiry.
    [302, {"Location": "/projects", "Set-Cookie": "session_token=#{token}; Path=/; Max-Age=#{lifetime}; HttpOnly; SameSite=Lax"}, "signed in"]
  end

  # POST /logout. Delete the session row (so the cookie is dead even if
  # someone kept a copy), then tell the browser to drop the cookie. A
  # missing cookie or session is not an error; logging out is idempotent.
  def self.destroy(request, context, params)
    token = cookie_value(request, "session_token")

    if token != nil
      session = Session.where({"token": token}).first(Database.get(context))
      if session != nil
        session_id = session.id()
        session.destroy(Database.get(context))
        log_info(request, context, "logout.succeeded", {"session_id": session_id})
      else
        log_warn(request, context, "logout.session_not_found")
      end
    end

    [302, {"Location": "/", "Set-Cookie": expired_session_cookie()}, "signed out"]
  end
end
