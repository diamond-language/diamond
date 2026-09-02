module ActiveAuth

  # Ported from ActiveAuth's own PasswordResetToken -- same fingerprint-
  # only-storage shape as Session/EmailVerificationToken, plus a
  # used_at/consume! single-use guard. 1-hour expiry.
  class PasswordResetToken < ActiveRecord::Model
    attr_accessor account_id, token: String, expires_at, used_at

    def self.token_prefix() = "sha256:"
    def self.duration_seconds() = 3600 # 1 hour

    def initialize(attributes: Hash = {})
      super(attributes)
      @account_id = attributes["account_id"]
      @token = attributes["token"]
      @expires_at = attributes["expires_at"]
      @used_at = attributes["used_at"]
    end

    def to_attributes() = {"account_id": @account_id, "token": @token, "expires_at": @expires_at, "used_at": @used_at}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end

    def self.fingerprint(raw: String) -> String = "#{PasswordResetToken.token_prefix()}#{Digest.sha256(raw)}"

    def used?() -> Bool = @used_at != nil

    def account(db) = self.belongs_to(Account.repository()).get(db, @account_id)

    def self.issue(db, account_id)
      raw = SecureRandom.hex(32)
      token = PasswordResetToken.new({"account_id": account_id,
        "token": PasswordResetToken.fingerprint(raw),
        "expires_at": Time.now().to_i() + PasswordResetToken.duration_seconds()})
      token.save(db)
      [token, raw]
    end

    # Unlike Session/EmailVerificationToken, an already-used token must
    # never match again even if it hasn't expired yet -- excluded via
    # `used_at IS NULL` in the same query, not a separate check after
    # the fact.
    def self.from_token(db, raw)
      if raw == nil || raw == "" || raw.length() >= 7 && raw.slice(0, 7) == PasswordResetToken.token_prefix()
        return nil
      end
      table = Arel.table("password_reset_tokens")
      predicate = table.column("token").in_list([PasswordResetToken.fingerprint(raw), raw]).and_also(
        table.column("expires_at").gt(Time.now().to_i())).and_also(
        table.column("used_at").eq(nil))
      rows = Arel.from(table).where(predicate).take(1).to_a(db)
      if rows.length() == 0 then nil else build_password_reset_token(rows[0]) end
    end

    # Raises, matching the Ruby original's own `raise ... if used_at` --
    # a caller trying to consume an already-used token is a programmer
    # error (from_token above already excludes used tokens from ever
    # being looked up again), not a normal, expected outcome to model as
    # a false return.
    def consume!(db)
      if self.used?()
        raise RuntimeError.new("token already used")
      end
      self.used_at = Time.now().to_i()
      self.save(db)
    end
  end

end

# A genuine top-level function, not nested inside `module ActiveAuth`
# above -- see account.di's own comment for why it's placed after the
# module.
def build_password_reset_token(row) = ActiveAuth::PasswordResetToken.new(row)
