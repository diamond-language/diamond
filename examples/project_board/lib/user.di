class User < ActiveRecord::Model
  attr_accessor email: String, password_digest

  def initialize(attributes: Hash = {})
    super(attributes)
    @email = attributes["email"]
    @password_digest = attributes["password_digest"]
  end

  def to_attributes() = {"email": @email, "password_digest": @password_digest}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def sessions(db) = self.has_many(Session.repository(), "user_id").all(db, self.id())
end

def build_user(row) = User.new(row)
