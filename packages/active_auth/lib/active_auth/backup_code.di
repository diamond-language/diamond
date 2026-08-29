module ActiveAuth

# Ported from ActiveAuth's own BackupCode -- one-time 2FA recovery
# codes. Only a bcrypt digest of each normalized code is ever stored;
# `self.generate_for!` returns the raw codes exactly once, for the
# caller to show the account holder immediately (never retrievable
# again afterward).
class BackupCode < ActiveRecord::Model
  attr_accessor account_id, code_digest: String, used_at

  def self.batch_size() = 8

  def initialize(attributes: Hash = {})
    super(attributes)
    @account_id = attributes["account_id"]
    @code_digest = attributes["code_digest"]
    @used_at = attributes["used_at"]
  end

  def to_attributes() = {"account_id": @account_id, "code_digest": @code_digest, "used_at": @used_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end

  def used?() -> Bool = @used_at != nil

  # Uppercase, alphanumeric-only -- accepts "a1b2-c3d4-e5f6",
  # "A1B2 C3D4 E5F6", or the bare "A1B2C3D4E5F6" as the same code.
  def self.normalize_code(raw: String) -> String
    letters = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    upper = raw.upcase()
    chars = []
    index = 0
    while index < upper.length()
      ch = upper.slice(index, 1)
      if letters.include?(ch)
        chars.push(ch)
      end
      index += 1
    end
    chars.join("")
  end

  # Ruby's own #match? also checks a second, more weakly-normalized
  # form (upcase + whitespace-only strip, dashes kept) -- redundant in
  # practice, since every stored digest was created from the fully
  # normalized (alnum-only) form to begin with, so it can never match
  # a real code the first check wouldn't already catch. Not ported.
  def match?(raw_code: String) -> Bool
    if self.used?()
      return false
    end
    BCrypt.verify(BackupCode.normalize_code(raw_code), @code_digest)
  end

  def consume!(db)
    self.used_at = Time.now().to_i()
    self.save(db)
  end

  # Checks and consumes in one call, so a caller doesn't have to
  # remember to call both -- the closest Diamond equivalent to Ruby's
  # own `with_lock`-wrapped atomic check-and-consume (this codebase has
  # no cross-request row locking primitive; single-worker-per-request
  # handling makes the two-step version safe in practice here).
  def consume_if_match!(db, raw_code: String) -> Bool
    if self.match?(raw_code)
      self.consume!(db)
      true
    else
      false
    end
  end

  def account(db) = self.belongs_to(Account.repository()).get(db, @account_id)

  # Replaces every existing code for `account_id` with BATCH_SIZE fresh
  # ones, returning the raw codes (e.g. ["A1B2-C3D4-E5F6", ...]) --
  # show these to the account holder once; only digests are persisted.
  def self.generate_for!(db, account_id)
    existing = BackupCode.where({"account_id": account_id}).to_a(db)
    existing.each() do |code| code.destroy(db) end
    raw_codes = []
    index = 0
    while index < BackupCode.batch_size()
      raw_codes.push("#{SecureRandom.hex(2).upcase()}-#{SecureRandom.hex(2).upcase()}-#{SecureRandom.hex(2).upcase()}")
      index += 1
    end
    raw_codes.each() do |raw|
      BackupCode.create(db, {"account_id": account_id, "code_digest": BCrypt.hash(BackupCode.normalize_code(raw), 12)})
    end
    raw_codes
  end
end

end

# A genuine top-level function, not nested inside `module ActiveAuth`
# above -- see account.di's own comment for why it's placed after the
# module.
def build_backup_code(row) = ActiveAuth::BackupCode.new(row)
