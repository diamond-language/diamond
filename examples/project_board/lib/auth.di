def cookie_value(request, name)
  cookie = request["headers"]["cookie"]
  if cookie == nil
    return nil
  end
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

def current_user(request, context)
  token = cookie_value(request, "session_token")
  if token == nil
    log_debug(request, context, "authentication cookie=absent")
    return nil
  end
  db = Database.get(context)
  session = Session.where({"token": token}).first(db)
  if session == nil
    log_warn(request, context, "authentication session=not_found")
    nil
  elsif session.expires_at() <= Time.now().to_i()
    session_id = session.id()
    session.destroy(db)
    log_warn(request, context, "authentication session_id=#{session_id} session=expired")
    nil
  else
    user = session.user(db)
    if user == nil
      log_warn(request, context, "authentication session_id=#{session.id()} user=not_found")
    else
      context["current_session"] = session
      context["csrf_token"] = session.csrf_token()
      log_info(request, context, "authentication session_id=#{session.id()} user_id=#{user.id()}")
    end
    user
  end
end

def load_current_user_middleware(request, context, forward)
  context["current_session"] = nil
  context["csrf_token"] = nil
  user = current_user(request, context)
  context["current_user"] = user
  log_debug(request, context, "authentication loaded authenticated=#{user != nil}")
  forward(request, context)
end

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

def require_csrf(request, context, params)
  expected = context["csrf_token"]
  supplied = params["csrf_token"]
  if !secure_token_equal(expected, supplied)
    log_warn(request, context, "authorization denied reason=invalid_csrf_token")
    return Dials::Response.text(403, "invalid CSRF token")
  end
  log_debug(request, context, "csrf token accepted")
  nil
end

def require_authentication(request, context, params)
  user = context["current_user"]
  if user == nil
    log_warn(request, context, "authorization denied reason=authentication_required")
    return [302, {"Location": "/login"}, "authentication required"]
  end
  log_debug(request, context, "authorization allowed user_id=#{user.id()}")
  nil
end
