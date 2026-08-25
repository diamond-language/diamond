class PheintGraphSQLMappings
  def self.games()
    if @@game_mapping == nil
      player = GraphSQL::Mapping.new(Player.repository(), "Player")
      player.column("id").column("handle")

      account = GraphSQL::Mapping.new(Account.repository(), "Account")
      account.column("id").column("email")
      account.association("player", "player", player)

      score = GraphSQL::Mapping.new(Score.repository(), "Score")
      score.column("id").column("value")
      score.association("player", "player", player)

      leaderboard = GraphSQL::Mapping.new(Leaderboard.repository(), "Leaderboard")
      leaderboard.column("id").column("name")
      leaderboard.association("scores", "scores", score)

      game = GraphSQL::Mapping.new(Game.repository(), "Game")
      game.column("id").column("title").column("description")
      game.association("owner", "owner", account)
      game.association("leaderboards", "leaderboards", leaderboard)
      @@game_mapping = game
    end
    @@game_mapping
  end
end
