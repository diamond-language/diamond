class Leaderboard < ActiveRecord::Model
  attr_accessor game_id: Int, name: String

  def initialize(attributes: Hash = {})
    super(attributes)
    @game_id = attributes["game_id"]
    @name = attributes["name"]
  end

  def to_attributes() = {"game_id": @game_id, "name": @name}
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def game(db) = self.belongs_to(Game.repository()).get(db, @game_id)
  def scores(db) = self.has_many(Score.repository(), "leaderboard_id").all(db, self.id())
end

def build_leaderboard(row) = Leaderboard.new(row)
