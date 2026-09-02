module ActiveAuth

  # Ported from ActiveAuth's own Session -- token-based, 30-day expiry.
  # The stored `token` column is always a SHA-256 fingerprint
  # ("sha256:<hex>"), never the raw presentable value -- matching the
  # Ruby original's own real security property (the DB never holds a
  # usable session token, only something that can verify one). The raw
  # token only ever exists in memory, returned once by `self.issue`
  # alongside the created record, for the caller to put in a cookie.
  #
  # Ruby's own version generates the token inside a `before_create` hook
  # and stashes the raw value in a non-persisted `@presented_token`
  # ivar, readable back off the same in-memory object after `.create!`
  # returns. Diamond's Repository callbacks (`before_save`) only see a
  # plain attributes Hash, not the calling Model instance, so there's
  # nothing to stash an ivar onto -- `self.issue` does the generate-then-
  # save-then-return-both explicitly instead, matching this codebase's
  # own "no implicit magic" convention (see e.g. Skindicate's
  # SessionsController.start_session, which already does exactly this
  # shape for its own hand-rolled session token).
  #
  # `csrf_token` and the `.issue_cookie`/`.from_cookie`/`.expired_cookie`
  # helpers below aren't in the Ruby original -- they're generalized from
  # that same Skindicate `SessionsController`, which additionally (a)
  # mints one CSRF token per session, stored on the row and echoed back
  # for the caller to embed in forms, and (b) wraps the raw token in a
  # `packages/cookies` `SignedCookies` envelope before handing it to the
  # browser, so a tampered/forged cookie is rejected by an HMAC check
  # *before* it ever reaches `.from_token`'s DB lookup, not just via a
  # failed fingerprint match. Both are optional: plain `.issue`/
  # `.from_token` above still work standalone for a caller that wants to
  # manage its own cookie envelope or skip CSRF protection entirely.
  class Session < ActiveRecord::Model
    attr_accessor account_id, token: String, csrf_token: String, user_agent, ip_address, expires_at

    def self.duration_seconds() = 2592000 # 30 days
    def self.token_prefix() = "sha256:"
    def self.default_cookie_name() = "session_token"

    def initialize(attributes: Hash = {})
      super(attributes)
      @account_id = attributes["account_id"]
      @token = attributes["token"]
      @csrf_token = attributes["csrf_token"]
      @user_agent = attributes["user_agent"]
      @ip_address = attributes["ip_address"]
      @expires_at = attributes["expires_at"]
    end

    def to_attributes() = {"account_id": @account_id, "token": @token, "csrf_token": @csrf_token,
      "user_agent": @user_agent, "ip_address": @ip_address, "expires_at": @expires_at}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end

    def self.fingerprint(raw: String) -> String = "#{Session.token_prefix()}#{Digest.sha256(raw)}"

    def expired?() -> Bool = @expires_at <= Time.now().to_i()

    def account(db) = self.belongs_to(Account.repository()).get(db, @account_id)

    # Creates and persists a new Session for `account_id`, returning
    # [session, raw_token] -- store `raw_token` in the caller's cookie;
    # nothing else ever sees it again.
    def self.issue(db, account_id, user_agent = nil, ip_address = nil)
      raw = SecureRandom.hex(32)
      session = Session.new({"account_id": account_id, "token": Session.fingerprint(raw),
        "csrf_token": SecureRandom.hex(32), "user_agent": user_agent, "ip_address": ip_address,
        "expires_at": Time.now().to_i() + Session.duration_seconds()})
      session.save(db)
      [session, raw]
    end

    # nil for a blank token, a token that's already a fingerprint (never
    # a legitimately-presented value), or one that doesn't match any
    # non-expired session -- matching the Ruby original's own three
    # guard conditions in `self.from_token`.
    def self.from_token(db, raw)
      if raw == nil || raw == "" || raw.length() >= 7 && raw.slice(0, 7) == Session.token_prefix()
        return nil
      end
      table = Arel.table("sessions")
      predicate = table.column("token").in_list([Session.fingerprint(raw), raw]).and_also(
        table.column("expires_at").gt(Time.now().to_i()))
      rows = Arel.from(table).where(predicate).take(1).to_a(db)
      if rows.length() == 0 then nil else build_session(rows[0]) end
    end

    # Same as .issue, but returns [session, set_cookie_header_value]
    # instead of the raw token -- the value is already signed
    # (SignedCookies.sign) and ready to hand straight to a response's
    # Set-Cookie header:
    #
    #   session, set_cookie = ActiveAuth::Session.issue_cookie(db, account.id(), secret: ENV["SESSION_SECRET"])
    #   [302, {"Location": "/", "Set-Cookie": set_cookie}, "signed in"]
    def self.issue_cookie(db, account_id, secret: String, user_agent = nil, ip_address = nil, cookie_name: String = Session.default_cookie_name())
      session, raw = Session.issue(db, account_id, user_agent, ip_address)
      cookie_value = SignedCookies.sign(raw, secret)
      set_cookie = cookie_serialize(cookie_name, cookie_value, {"path": "/", "http_only": true, "same_site": "Lax", "max_age": Session.duration_seconds()})
      [session, set_cookie]
    end

    # The `.issue_cookie` counterpart for reading a request back: verifies
    # the named cookie's signature (rejecting a tampered/forged value
    # without ever touching the database) and looks up the resulting raw
    # token via `.from_token`. Returns nil -- never raises -- for a
    # missing cookie, a bad signature, or a token that's expired/unknown,
    # matching `.from_token`'s own no-exceptions contract.
    def self.from_cookie(request, db, secret: String, cookie_name: String = Session.default_cookie_name())
      cookie_value = cookie_parse(request["headers"]["cookie"])[cookie_name]
      if cookie_value == nil
        return nil
      end
      raw = SignedCookies.verify(cookie_value, secret)
      if raw == nil
        return nil
      end
      Session.from_token(db, raw)
    end

    # A Set-Cookie value that immediately expires the named cookie
    # (Max-Age=0) -- hand this back on logout, matching
    # `.issue_cookie`'s own path/http_only/same_site attributes so the
    # browser actually recognizes it as clearing the same cookie.
    def self.expired_cookie(cookie_name: String = Session.default_cookie_name()) -> String
      cookie_serialize(cookie_name, "", {"path": "/", "http_only": true, "same_site": "Lax", "max_age": 0})
    end
  end

end

# A genuine top-level function, not nested inside `module ActiveAuth`
# above -- see account.di's own comment for why it's placed after the
# module (a namespaced class reference needs the class already
# declared earlier in the file; the reverse call direction, a method
# above calling this function defined below it, is fine).
def build_session(row) = ActiveAuth::Session.new(row)
