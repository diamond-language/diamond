module ActiveAuth

  # ported from ActiveAuth's own Account -- the core identity model:
  # email/username/password (bcrypt via ActiveRecord::Model's own
  # #secure_password=/#authenticate, inherited for free the same way
  # every other model in this codebase already uses it), a site-wide
  # `role`, and email verification state. Multi-persona support
  # (Persona/PersonaMembership -- one account owning/co-managing several
  # public-facing identities) is deliberately not ported yet; see
  # README.md for why. Two-factor state (`two_factor_enabled?`/
  # `active_totp`) reads from TotpCredential, not ported in this same
  # pass either -- see README.md's TOTP section.
  class Account < ActiveRecord::Model
    attr_accessor email: String, username: String, password_digest, email_verified_at, role: String

    def self.roles() = ["member", "moderator", "admin", "dev"]
    def self.elevated_roles() = ["moderator", "admin", "dev"]
    def self.normalize_username(raw: String) -> String = raw.strip().downcase().gsub(Regexp.new("\\s+"), "-")

    # Normalizes email/username right here, not via a Repository
    # before_save callback the way Ruby's own before_validation hooks
    # would suggest -- Repository#create/#update run validation *before*
    # before_save (see repository.di), so a before_save-based normalizer
    # would validate the raw, unnormalized input and never see its own
    # output. Normalizing at construction time instead means every
    # Account, from the moment it exists, already holds normalized
    # values -- simpler, and validation always sees the same thing
    # #save will persist. The one gap this doesn't cover: reassigning
    # `account.email = "Mixed@Case.com"` after construction bypasses
    # normalization (attr_accessor's plain setter has no hook of its
    # own) -- construct a new Account with updated attributes instead of
    # mutating email/username directly, or normalize explicitly first.
    def initialize(attributes: Hash = {})
      super(attributes)
      @email = if attributes["email"] == nil then nil else attributes["email"].strip().downcase() end
      @username = if attributes["username"] == nil then nil else Account.normalize_username(attributes["username"]) end
      @password_digest = attributes["password_digest"]
      @email_verified_at = attributes["email_verified_at"]
      @role = if attributes["role"] == nil then "member" else attributes["role"] end
    end

    def to_attributes() = {"email": @email, "username": @username, "password_digest": @password_digest,
      "email_verified_at": @email_verified_at, "role": @role}
    def repository() = @@repository
    def self.repository() = @@repository
    def self.configure(repository: ActiveRecord::Repository)
      @@repository = repository
    end

    def email_verified?() -> Bool = @email_verified_at != nil
    def verify_email!(db)
      self.email_verified_at = Time.now().to_i()
      self.save(db)
    end

    def elevated?() -> Bool = Account.elevated_roles().include?(@role)

    def sessions(db) = self.has_many(Session.repository(), "account_id").all(db, self.id())
    def backup_codes(db) = self.has_many(BackupCode.repository(), "account_id").all(db, self.id())
    def unused_backup_codes(db) = self.backup_codes(db).reject() do |code| code.used?() end
  end

  end

  # Every one of these three helpers is a genuine top-level function, not
  # nested inside `module ActiveAuth` above -- a plain `def` (no `self.`)
  # declared directly inside a bare `module ... end` block isn't callable
  # at all (confirmed directly porting packages/active_karma; see that
  # package's own README). Placed *after* the module, not before: a
  # top-level function referencing a namespaced class (`ActiveAuth::
  # Account`) needs that class already declared earlier in the file --
  # unlike a plain top-level class, a `Module::Class` reference doesn't
  # get the declaration-discovery pass's forward-reference treatment
  # (confirmed directly). The reverse direction is fine: `Account.
  # initialize`/`self.x` methods above calling a top-level function
  # defined below them in the file works, since forward-reference from a
  # method to a not-yet-declared function is allowed -- it's only a
  # peer top-level function calling another one that needs strict source
  # order (see packages/arel/lib/arel.di's own comment on that rule).
  def build_account(row) = ActiveAuth::Account.new(row)

  def build_account_validator(db)
    ActiveRecord::Validators.combine([
      ActiveRecord::Validators.presence("email"),
      ActiveRecord::Validators.format("email", Regexp.new("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$")),
      ActiveRecord::Validators.uniqueness(db, Arel.table("accounts"), "email"),
      ActiveRecord::Validators.presence("username"),
      ActiveRecord::Validators.format("username", Regexp.new("^[a-zA-Z0-9_-]{3,30}$")),
      ActiveRecord::Validators.uniqueness(db, Arel.table("accounts"), "username"),
      ActiveRecord::Validators.inclusion("role", ActiveAuth::Account.roles()),
    ])
end
