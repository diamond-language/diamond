def pheint_bearer_token(request)
  headers = request["headers"]
  if headers == nil then return nil end
  authorization = headers["authorization"]
  if authorization == nil then authorization = headers["Authorization"] end
  if authorization == nil || !authorization.start_with?("Bearer ")
    return nil
  end
  token = authorization.slice(7, authorization.length() - 7).strip()
  if token == "" then nil else token end
end

def load_pheint_authentication_middleware(request, context, forward)
  context["current_account"] = nil
  context["current_session"] = nil
  token = pheint_bearer_token(request)
  if token != nil
    db = PheintDatabase.get(context)
    session = Session.where({"token": token}).first(db)
    if session == nil
      pheint_log_info(request, context, "authentication.rejected", {
        "reason": "session_not_found"})
    elsif session.expires_at() <= Time.now().to_i()
      session_id = session.id()
      session.destroy(db)
      pheint_log_info(request, context, "authentication.rejected", {
        "reason": "session_expired", "session_id": session_id})
    else
      account = Account.find(db, session.account_id())
      if account != nil
        context["current_session"] = session
        context["current_account"] = account
        pheint_log_info(request, context, "authentication.succeeded", {
          "account_id": account.id(), "session_id": session.id()})
      end
    end
  end
  forward(request, context)
end

def pheint_normalize_email(value) = value.strip().downcase()
def pheint_dummy_password_digest() = "$2b$12$sCm2ciqq5cfNls1GTYOiJO.j1AIdCd.2O9sX8EtdqpWhbqQ6hxFXW"

def pheint_normalize_handle(value)
  handle = value.strip().downcase()
  if handle.length() > 0 && handle[0] == "@"
    handle.slice(1, handle.length() - 1)
  else
    handle
  end
end

def pheint_issue_session(db, account)
  session = Session.new({
    "account_id": account.id(), "token": SecureRandom.hex(32),
    "expires_at": Time.now().to_i() + 2592000
  })
  session.save(db)
  session
end
