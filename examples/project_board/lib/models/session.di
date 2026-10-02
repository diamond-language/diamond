# A row of the `sessions` table (see setup_db.di for the schema). Columns are
# copied from the row-shaped Hash into instance variables on construction and
# back out by `to_attributes` on save.
class Session < ActiveRecord::Model
  # `token` is the secret in the cookie; `csrf_token` is the form-forgery
  # token (see helpers/auth.di); `expires_at` is a Unix timestamp.
  attr_accessor user_id, token: String, csrf_token: String, expires_at

  def initialize(attributes: Hash = {})
    super(attributes)
    @user_id = attributes["user_id"]
    @token = attributes["token"]
    @csrf_token = attributes["csrf_token"]
    @expires_at = attributes["expires_at"]
  end

  def to_attributes() = {"user_id": @user_id, "token": @token, "csrf_token": @csrf_token, "expires_at": @expires_at}
  # The Repository (table + row builder + validator) is a class variable set
  # per worker by ensure_models_configured (lib/middleware.di).
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  # The session's owner (nil if that user was deleted).
  def user(db) = self.belongs_to(User.repository()).get(db, @user_id)
end

# Builds a Session from a database row; given to the Repository.
def build_session(row) = Session.new(row)
