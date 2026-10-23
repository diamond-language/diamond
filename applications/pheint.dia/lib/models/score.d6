class Score < ActiveRecord::Model
  attr_accessor leaderboard_id: Int, player_id: Int, value: Int

  def initialize(attributes: Hash = {})
    super(attributes)
    @leaderboard_id = attributes["leaderboard_id"]
    @player_id = attributes["player_id"]
    @value = attributes["value"]
  end

  def to_attributes() = {
    "leaderboard_id": @leaderboard_id, "player_id": @player_id, "value": @value
  }
  def repository() = @@repository
  def self.repository() = @@repository
  def self.configure(repository: ActiveRecord::Repository)
    @@repository = repository
  end
  def leaderboard(db) = self.belongs_to(Leaderboard.repository()).get(db, @leaderboard_id)
  def player(db) = self.belongs_to(Player.repository()).get(db, @player_id)
end

def build_score(row) = Score.new(row)
