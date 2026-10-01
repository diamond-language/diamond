# Authentication and CSRF protection. Login creates a server-side Session row
# and gives the browser only its random token in a cookie; every request
# looks that token up (current_user), so logging out or expiry is just
# deleting the row.

# A Set-Cookie value that makes the browser delete the cookie (Max-Age=0).
def expired_session_cookie() = "session_token=; Path=/; Max-Age=0; HttpOnly; SameSite=Lax"

# Finds one cookie's value in the request's `Cookie` header, or nil.
# The header looks like "a=1; session_token=abc; b=2".
def cookie_value(request, name)
  cookie = request["headers"]["cookie"]
  if cookie == nil
    return nil
  end

  # Check each `name=value` pair. Split only at the FIRST `=`, so a value
  # that itself contains `=` stays intact.
  parts = cookie.split(";")
  index = 0
  while index < parts.length()
    part = parts[index]
    stripped = part.strip()
    equals = stripped.index_of("=")
    if equals != nil && stripped.slice(0, equals) == name
      return stripped.slice(equals + 1, stripped.length())
    end
    index += 1
  end
  nil
end

# Resolves the logged-in User for this request, or nil. Three ways to end up
# anonymous: no cookie; a cookie whose token matches no session; an expired
# session. In the last two the browser holds a useless cookie, so
# `clear_session_cookie` is flagged and the middleware below removes it.
def current_user(request, context)
  token = cookie_value(request, "session_token")
  if token == nil
    log_debug(request, context, "authentication.cookie_absent")
    return nil
  end

  db = Database.get(context)
  session = Session.where({"token": token}).first(db)

  if session == nil
    # Unknown token (e.g. already logged out elsewhere).
    context["clear_session_cookie"] = true
    log_warn(request, context, "authentication.session_not_found")
    nil
  elsif session.expires_at() <= Time.now().to_i()
    # Expired: delete the stale row as well as the cookie.
    session_id = session.id()
    session.destroy(db)
    context["clear_session_cookie"] = true
    log_warn(request, context, "authentication.session_expired", {"session_id": session_id})
    nil
  else
    # Valid session. A session whose user row vanished still counts as
    # anonymous. Otherwise remember the session and expose its CSRF token
    # to the views (forms embed it) and `require_csrf`.
    user = session.user(db)
    if user == nil
      log_warn(request, context, "authentication.user_not_found", {"session_id": session.id()})
    else
      context["current_session"] = session
      context["csrf_token"] = session.csrf_token()
      log_info(request, context, "authentication.succeeded", {"session_id": session.id(), "user_id": user.id()})
    end
    user
  end
end

# Middleware: authenticate before routing and put the result on `context` for
# the controllers and views to use.
def load_current_user_middleware(request, context, forward)
  # `context` is reused across requests on a worker, so reset anything a
  # previous request left behind first.
  context["current_session"] = nil
  context["csrf_token"] = nil
  context["clear_session_cookie"] = false

  user = current_user(request, context)
  context["current_user"] = user
  log_debug(request, context, "authentication.loaded", {"authenticated": user != nil})

  # Run the rest of the app, then (if the cookie turned out to be bad) add a
  # header deleting it to whatever response came back.
  response = forward(request, context)
  if context["clear_session_cookie"]
    [status, headers, body] = response
    headers["Set-Cookie"] = expired_session_cookie()
    response[1] = headers
    log_debug(request, context, "authentication.cookie_expired")
  end
  response
end

# Compares two secret tokens without stopping at the first difference. A
# plain `==` can return sooner the earlier the strings differ, which lets an
# attacker guess a token one character at a time by timing responses. Here
# the loop always examines every character. Different lengths return early,
# which is fine: the length is not a secret.
def secure_token_equal(left, right)
  if left == nil || right == nil || left.length() != right.length()
    return false
  end

  matches = true
  index = 0
  while index < left.length()
    if left[index] != right[index]
      matches = false
    end
    index += 1
  end

  matches
end

# Route guard for state-changing POSTs: the form's hidden `csrf_token` must
# equal the one stored in the logged-in user's session. A page on another
# site can make the browser send our cookie, but cannot read this token, so
# it cannot forge a valid request. Returns a response to stop the request,
# or nil to let it continue.
def require_csrf(request, context, params)
  expected = context["csrf_token"]
  supplied = params["csrf_token"]
  if !secure_token_equal(expected, supplied)
    log_warn(request, context, "authorization.denied", {"reason": "invalid_csrf_token"})
    return Dials::Response.text(403, "invalid CSRF token")
  end
  log_debug(request, context, "authorization.csrf_accepted")
  nil
end

# Route guard: send anonymous users to the login page. Same contract as
# require_csrf: a response stops the request, nil continues.
def require_authentication(request, context, params)
  user = context["current_user"]
  if user == nil
    log_warn(request, context, "authorization.denied", {"reason": "authentication_required"})
    return [302, {"Location": "/login"}, "authentication required"]
  end
  log_debug(request, context, "authorization.allowed", {"user_id": user.id()})
  nil
end
