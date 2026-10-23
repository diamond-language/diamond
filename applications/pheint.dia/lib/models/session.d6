class Session < ActiveRecord::Model
  attr_accessor account_id: Int, token: String, expires_at: Int

  def initialize(attributes: Hash = {})
    super(attributes)
    @account_id = attributes["account_id"]
    @token = attributes["token"]
    @expires_at = attributes["expires_at"]
  end

  def to_attributes() = {
    "account_id": @account_id, "token": @token, "expires_at": @expires_at
  }
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

def build_session(row) = Session.new(row)
