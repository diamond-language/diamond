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
class Session < ActiveRecord::Model
  attr_accessor account_id, token: String, user_agent, ip_address, expires_at

  def self.duration_seconds() = 2592000 # 30 days
  def self.token_prefix() = "sha256:"

  def initialize(attributes: Hash = {})
    super(attributes)
    @account_id = attributes["account_id"]
    @token = attributes["token"]
    @user_agent = attributes["user_agent"]
    @ip_address = attributes["ip_address"]
    @expires_at = attributes["expires_at"]
  end

  def to_attributes() = {"account_id": @account_id, "token": @token, "user_agent": @user_agent,
    "ip_address": @ip_address, "expires_at": @expires_at}
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
      "user_agent": user_agent, "ip_address": ip_address,
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
end

end

# A genuine top-level function, not nested inside `module ActiveAuth`
# above -- see account.di's own comment for why it's placed after the
# module (a namespaced class reference needs the class already
# declared earlier in the file; the reverse call direction, a method
# above calling this function defined below it, is fine).
def build_session(row) = ActiveAuth::Session.new(row)
