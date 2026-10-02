# A row of the `users` table (see setup_db.di for the schema). Columns are
# copied from the row-shaped Hash into instance variables on construction and
# back out by `to_attributes` on save.
class User < ActiveRecord::Model
  # Only a BCrypt digest is stored, never the password itself.
  attr_accessor email: String, password_digest

  def initialize(attributes: Hash = {})
    super(attributes)
    @email = attributes["email"]
    @password_digest = attributes["password_digest"]
  end

  def to_attributes() = {"email": @email, "password_digest": @password_digest}
  # The Repository (table + row builder + validator) is a class variable set
  # per worker by ensure_models_configured (lib/middleware.di).
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  # All of this user's login sessions.
  def sessions(db) = self.has_many(Session.repository(), "user_id").all(db, self.id())
end

# Builds a User from a database row; given to the Repository.
def build_user(row) = User.new(row)
