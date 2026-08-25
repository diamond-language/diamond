class Game < ActiveRecord::Model
  attr_accessor owner_id: Int, title: String, description: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @owner_id = attributes["owner_id"]
    @title = attributes["title"]
    @description = attributes["description"]
  end

  def to_attributes() = {
    "owner_id": @owner_id, "title": @title, "description": @description
  }
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def owner(db) = self.belongs_to(Account.repository()).get(db, @owner_id)
  def leaderboards(db) = self.has_many(Leaderboard.repository(), "game_id").all(db, self.id())
end

def build_game(row) = Game.new(row)
