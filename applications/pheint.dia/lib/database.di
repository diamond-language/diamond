def build_pheint_query_logger(logger, context)
  def log_query(event)
    phase = event["phase"]
    correlation = context["log_context"]
    correlation = {} if correlation == nil
    fields = event.merge(correlation).merge({
      "orm": "active_record", "builder": "arel"
    })
    if phase == "failed"
      logger.error("database.query.failed", fields)
    else
      logger.debug("database.query.#{phase}", fields)
    end
  end
  log_query
end

class PheintDatabase
  def self.path() = PheintEnvironment.database_path()

  def self.get(context)
    db = context["db"]
    if db == nil
      connection = SQLite3.open(PheintDatabase.path())
      connection.execute("PRAGMA foreign_keys = ON")
      connection.execute("PRAGMA journal_mode = WAL")
      connection.execute("PRAGMA busy_timeout = 5000")
      db = ActiveRecord::InstrumentedConnection.new(
        connection, build_pheint_query_logger(PheintLogger.get(context), context))
      context["db"] = db
      PheintLogger.get(context).info("database.connection.opened", {
        "adapter": "sqlite3", "database": PheintDatabase.path(),
        "environment": PheintEnvironment.name(), "foreign_keys": true,
        "journal_mode": "wal", "busy_timeout_ms": 5000
      })
    end
    db
  end
end

def build_account_validator(db)
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("email"),
    ActiveRecord::Validators.format("email", Regexp.new("^[^@ ]+@[^@ ]+\\.[^@ ]+$")),
    ActiveRecord::Validators.uniqueness(db, Arel.table("accounts"), "email")
  ])
end

def build_player_validator(db)
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("handle"),
    ActiveRecord::Validators.length("handle", 3, 30),
    ActiveRecord::Validators.format("handle", Regexp.new("^[A-Za-z0-9_]+$")),
    ActiveRecord::Validators.uniqueness(db, Arel.table("players"), "handle")
  ])
end

def build_game_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("title"),
    ActiveRecord::Validators.length("title", 1, 120),
    ActiveRecord::Validators.presence("description"),
    ActiveRecord::Validators.length("description", 1, 2000)
  ])
end

def build_leaderboard_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("name"),
    ActiveRecord::Validators.length("name", 1, 80)
  ])
end

def build_score_validator()
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.numericality("value")
  ])
end

def ensure_pheint_models_configured(context)
  if context["models_configured"] == nil
    db = PheintDatabase.get(context)
    Player.configure(ActiveRecord::Repository.new(
      Arel.table("players"), build_player, "id", nil, build_player_validator(db),
      nil, nil, nil, ["id", "account_id", "handle"]))
    Session.configure(ActiveRecord::Repository.new(
      Arel.table("sessions"), build_session, "id", nil, nil, nil, nil, nil,
      ["id", "account_id", "token", "expires_at"]))
    Game.configure(ActiveRecord::Repository.new(
      Arel.table("games"), build_game, "id", nil, build_game_validator(),
      nil, nil, nil, ["id", "owner_id", "title", "description"]))
    Leaderboard.configure(ActiveRecord::Repository.new(
      Arel.table("leaderboards"), build_leaderboard, "id", nil,
      build_leaderboard_validator(), nil, nil, nil,
      ["id", "game_id", "name", "higher_is_better"]))
    Score.configure(ActiveRecord::Repository.new(
      Arel.table("scores"), build_score, "id", nil, build_score_validator(),
      nil, nil, nil, ["id", "leaderboard_id", "player_id", "value"]))
    Account.configure(ActiveRecord::Repository.new(
      Arel.table("accounts"), build_account, "id", nil, build_account_validator(db),
      nil, nil, nil, ["id", "email", "password_digest"], nil, [
        ActiveRecord::AssociationReflection.new(
          "player", "has_one", Player.repository(), "account_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "sessions", "has_many", Session.repository(), "account_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "games", "has_many", Game.repository(), "owner_id", "id")
      ]))
    Player.configure(ActiveRecord::Repository.new(
      Arel.table("players"), build_player, "id", nil, build_player_validator(db),
      nil, nil, nil, ["id", "account_id", "handle"], nil, [
        ActiveRecord::AssociationReflection.new(
          "account", "belongs_to", Account.repository(), "account_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "scores", "has_many", Score.repository(), "player_id", "id")
      ]))
    Game.configure(ActiveRecord::Repository.new(
      Arel.table("games"), build_game, "id", nil, build_game_validator(),
      nil, nil, nil, ["id", "owner_id", "title", "description"], nil, [
        ActiveRecord::AssociationReflection.new(
          "owner", "belongs_to", Account.repository(), "owner_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "leaderboards", "has_many", Leaderboard.repository(), "game_id", "id")
      ]))
    Leaderboard.configure(ActiveRecord::Repository.new(
      Arel.table("leaderboards"), build_leaderboard, "id", nil,
      build_leaderboard_validator(), nil, nil, nil,
      ["id", "game_id", "name", "higher_is_better"], nil, [
        ActiveRecord::AssociationReflection.new(
          "game", "belongs_to", Game.repository(), "game_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "scores", "has_many", Score.repository(), "leaderboard_id", "id")
      ]))
    Score.configure(ActiveRecord::Repository.new(
      Arel.table("scores"), build_score, "id", nil, build_score_validator(),
      nil, nil, nil, ["id", "leaderboard_id", "player_id", "value"], nil, [
        ActiveRecord::AssociationReflection.new(
          "leaderboard", "belongs_to", Leaderboard.repository(), "leaderboard_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "player", "belongs_to", Player.repository(), "player_id", "id")
      ]))
    Session.configure(ActiveRecord::Repository.new(
      Arel.table("sessions"), build_session, "id", nil, nil, nil, nil, nil,
      ["id", "account_id", "token", "expires_at"], nil, [
        ActiveRecord::AssociationReflection.new(
          "account", "belongs_to", Account.repository(), "account_id", "id")
      ]))
    context["models_configured"] = true
    PheintLogger.get(context).info("models.configured", {"model_count": 6})
  end
end
