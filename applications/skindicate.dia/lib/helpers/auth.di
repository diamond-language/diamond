# DB-backed sessions (real revocation -- session rows are deleted on
# logout/expiry, unlike packages/cookies' own CookieSession, which has
# no server-side record to revoke at all) wrapped in a *signed* cookie
# (packages/cookies' SignedCookies), not a bare opaque token: the
# cookie value is base64url(token) + "." + HMAC-SHA256(secret, token),
# so a tampered/forged cookie is rejected by the signature check below
# *before* ever reaching the database, not just via a failed SELECT.
# Built on packages/cookies' own cookie_parse/cookie_serialize/
# constant_time_equal throughout, rather than hand-rolling them.

def expired_session_cookie() = cookie_serialize("session_token", "", {"path": "/", "http_only": true, "same_site": "Lax", "max_age": 0})

def current_user(request, context)
  cookie_value = cookie_parse(request["headers"]["cookie"])["session_token"]
  if cookie_value == nil
    log_debug(request, context, "authentication.cookie_absent")
    return nil
  end
  token = SignedCookies.verify(cookie_value, SkindicateEnvironment.session_secret())
  if token == nil
    log_warn(request, context, "authentication.signature_invalid")
    return nil
  end
  db = Database.get(context)
  session = Session.where({"token": token}).first(db)
  if session == nil
    context["clear_session_cookie"] = true
    log_warn(request, context, "authentication.session_not_found")
    nil
  elsif session.expires_at() <= Time.now().to_i()
    session_id = session.id()
    session.destroy(db)
    context["clear_session_cookie"] = true
    log_warn(request, context, "authentication.session_expired", {"session_id": session_id})
    nil
  else
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

def load_current_user_middleware(request, context, forward)
  context["current_session"] = nil
  context["csrf_token"] = nil
  context["clear_session_cookie"] = false
  # multipart_upload's own cache must be cleared here every request --
  # context is reused across the whole worker's lifetime (or, as here,
  # across an entire smoke test run), not recreated per request.
  context["multipart_upload_cached"] = false
  user = current_user(request, context)
  context["current_user"] = user
  log_debug(request, context, "authentication.loaded", {"authenticated": user != nil})
  response = forward(request, context)
  if context["clear_session_cookie"]
    [status, headers, body] = response
    headers["Set-Cookie"] = expired_session_cookie()
    response[1] = headers
    log_debug(request, context, "authentication.cookie_expired")
  end
  response
end

# Dials::Router unconditionally runs every request body through
# Dials::Params (application/x-www-form-urlencoded only -- see
# packages/dials/lib/dials/params.di), so `params` never carries a
# multipart field, even though it parses "successfully" (silently
# empty/garbage) on a multipart body instead of raising. Cached on
# `context` so a multipart route's own controller action (which needs
# the same parse for its file fields) doesn't pay for parsing the body
# twice.
def multipart_upload(request, context)
  if !context["multipart_upload_cached"]
    context["multipart_upload"] = multipart_parse(request)
    context["multipart_upload_cached"] = true
  end
  context["multipart_upload"]
end

def csrf_token_from_request(request, context, params)
  content_type = request["headers"]["content-type"]
  if content_type != nil && content_type.index_of("multipart/form-data") != nil
    upload = multipart_upload(request, context)
    if upload == nil then nil else upload["fields"]["csrf_token"] end
  else
    params["csrf_token"]
  end
end

def require_csrf(request, context, params)
  expected = context["csrf_token"]
  supplied = csrf_token_from_request(request, context, params)
  if expected == nil || supplied == nil || !constant_time_equal(expected, supplied)
    log_warn(request, context, "authorization.denied", {"reason": "invalid_csrf_token"})
    return Dials::Response.text(403, "invalid CSRF token")
  end
  log_debug(request, context, "authorization.csrf_accepted")
  nil
end

def require_authentication(request, context, params)
  user = context["current_user"]
  if user == nil
    log_warn(request, context, "authorization.denied", {"reason": "authentication_required"})
    return [302, {"Location": "/login"}, "authentication required"]
  end
  log_debug(request, context, "authorization.allowed", {"user_id": user.id()})
  nil
end

# 404s (not 403) on someone else's skin -- avoids confirming to an
# unauthorized visitor that a given id even exists. Stashes the loaded
# Skin into context["current_skin"] on success so the controller action
# doesn't have to look it up a second time.
def require_ownership(request, context, params)
  skin = Skin.find(Database.get(context), params["id"].to_i())
  if skin == nil
    log_warn(request, context, "authorization.denied", {"reason": "not_found", "skin_id": params["id"]})
    return Dials::Response.not_found(request["path"])
  end
  if skin.user_id() != context["current_user"].id()
    log_warn(request, context, "authorization.denied", {"reason": "not_owner", "skin_id": skin.id()})
    return Dials::Response.not_found(request["path"])
  end
  context["current_skin"] = skin
  log_debug(request, context, "authorization.allowed", {"skin_id": skin.id()})
  nil
end

# Same 404-not-403 shape as require_ownership above, for a comment
# rather than a skin -- params["id"] here is the comment's own id (see
# routes.di's "/comments/:id/delete"). A comment can be deleted by its
# own author *or* by the skin's owner (a discussion's recording_id is
# always "#{skin_id}", see comments_controller.di's own comment on this
# mapping) -- there's no separate discussion-moderator role here.
def require_comment_ownership(request, context, params)
  db = Database.get(context)
  comment = ActiveDiscussion::Comment.find(db, params["id"].to_i())
  if comment == nil
    log_warn(request, context, "authorization.denied", {"reason": "not_found", "comment_id": params["id"]})
    return Dials::Response.not_found(request["path"])
  end
  discussion = comment.discussion(db)
  skin = Skin.find(db, discussion.recording_id().to_i())
  is_author = comment.persona_handle() == context["current_user"].username()
  is_skin_owner = skin != nil && skin.user_id() == context["current_user"].id()
  unless is_author || is_skin_owner
    log_warn(request, context, "authorization.denied", {"reason": "not_owner", "comment_id": comment.id()})
    return Dials::Response.not_found(request["path"])
  end
  context["current_comment"] = comment
  log_debug(request, context, "authorization.allowed", {"comment_id": comment.id()})
  nil
end
