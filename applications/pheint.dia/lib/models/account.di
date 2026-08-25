class Account < ActiveRecord::Model
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
  def player(db)
    if self.association_loaded?("player")
      self.preloaded_association("player")
    else
      self.has_one(Player.repository(), "account_id").get(db, self.id())
    end
  end
end

def build_account(row) = Account.new(row)
