module ActiveAuth

# Ported from ActiveAuth's own EmailVerificationToken -- same
# fingerprint-only-storage/issue/from_token shape as Session (see that
# file's own comment for why `self.issue` exists instead of a
# Ruby-style before_create hook). 24-hour expiry.
class EmailVerificationToken < ActiveRecord::Model
  attr_accessor account_id, token: String, expires_at

  def self.duration_seconds() = 86400 # 24 hours
  def self.token_prefix() = "sha256:"

  def initialize(attributes: Hash = {})
    super(attributes)
    @account_id = attributes["account_id"]
    @token = attributes["token"]
    @expires_at = attributes["expires_at"]
  end

  def to_attributes() = {"account_id": @account_id, "token": @token, "expires_at": @expires_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def self.fingerprint(raw: String) -> String = "#{EmailVerificationToken.token_prefix()}#{Digest.sha256(raw)}"

  def expired?() -> Bool = @expires_at <= Time.now().to_i()

  def account(db) = self.belongs_to(Account.repository()).get(db, @account_id)

  def self.issue(db, account_id)
    raw = SecureRandom.hex(32)
    token = EmailVerificationToken.new({"account_id": account_id,
      "token": EmailVerificationToken.fingerprint(raw),
      "expires_at": Time.now().to_i() + EmailVerificationToken.duration_seconds()})
    token.save(db)
    [token, raw]
  end

  def self.from_token(db, raw)
    if raw == nil || raw == "" || raw.length() >= 7 && raw.slice(0, 7) == EmailVerificationToken.token_prefix()
      return nil
    end
    table = Arel.table("email_verification_tokens")
    predicate = table.column("token").in_list([EmailVerificationToken.fingerprint(raw), raw]).and_also(
      table.column("expires_at").gt(Time.now().to_i()))
    rows = Arel.from(table).where(predicate).take(1).to_a(db)
    if rows.length() == 0 then nil else build_email_verification_token(rows[0]) end
  end
end

end

# A genuine top-level function, not nested inside `module ActiveAuth`
# above -- see account.di's own comment for why it's placed after the
# module.
def build_email_verification_token(row) = ActiveAuth::EmailVerificationToken.new(row)
