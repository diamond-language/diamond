class User < ActiveRecord::Model
  attr_accessor email: String, username: String, password_digest, role: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @email = attributes["email"]
    @username = attributes["username"]
    @password_digest = attributes["password_digest"]
    @role = if attributes["role"] == nil then "user" else attributes["role"] end
  end

  def to_attributes() = {"email": @email, "username": @username, "password_digest": @password_digest, "role": @role}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def sessions(db) = self.has_many(Session.repository(), "user_id").all(db, self.id())

  def admin?() -> Bool = @role == "admin"

  # A user's own uploaded skins -- ownership lives on each skin's own
  # root Entry, not a direct FK on Skin (see entry.di).
  def skins(db) -> Array
    Entry.where({"user_id": self.id(), "entryable_type": "Skin"}).to_a(db).map() do |entry| entry.entryable(db) end
  end
end

def build_user(row) = User.new(row)

def build_user_validator(db)
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("email"),
    ActiveRecord::Validators.length("email", 3, 200),
    ActiveRecord::Validators.format("email", Regexp.new("^[^@\\s]+@[^@\\s]+\\.[^@\\s]+$")),
    ActiveRecord::Validators.uniqueness(db, Arel.table("users"), "email"),
    ActiveRecord::Validators.presence("username"),
    ActiveRecord::Validators.length("username", 3, 32),
    ActiveRecord::Validators.format("username", Regexp.new("^[A-Za-z0-9_]+$")),
    ActiveRecord::Validators.uniqueness(db, Arel.table("users"), "username"),
    ActiveRecord::Validators.presence("password_digest"),
    ActiveRecord::Validators.inclusion("role", ["user", "admin"]),
  ])
end
