class Session < ActiveRecord::Model
  attr_accessor user_id, token: String, csrf_token: String, expires_at

  def initialize(attributes: Hash = {})
    super(attributes)
    @user_id = attributes["user_id"]
    @token = attributes["token"]
    @csrf_token = attributes["csrf_token"]
    @expires_at = attributes["expires_at"]
  end

  def to_attributes() = {"user_id": @user_id, "token": @token, "csrf_token": @csrf_token, "expires_at": @expires_at}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def user(db) = self.belongs_to(User.repository()).get(db, @user_id)
end

def build_session(row) = Session.new(row)
