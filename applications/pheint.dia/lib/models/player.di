class Player < ActiveRecord::Model
  attr_accessor account_id: Int, handle: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @account_id = attributes["account_id"]
    @handle = attributes["handle"]
  end

  def to_attributes() = {"account_id": @account_id, "handle": @handle}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
end

def build_player(row) = Player.new(row)
